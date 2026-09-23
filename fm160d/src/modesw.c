/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * modesw.c - applying a USB profile change, and living to tell the tale.
 *
 * The decision itself is in usbmode.c and is pure; this file is the part that
 * talks to the modem.  It exists to make three specific mistakes impossible:
 *
 *   1. applying a target that was never checked against the modem's own list
 *      AND the hard whitelist (both gates, not one);
 *   2. losing the ability to say what the profile was before the switch.  The
 *      rollback point is written to uci BEFORE AT+GTUSBMODE, because after that
 *      command the module may re-enumerate and there is no second chance to
 *      write it down;
 *   3. retrying forever after a switch that worked but left no AT port.  That
 *      state has no automatic way out - it needs a person - so the daemon says
 *      so once and stops, rather than rebooting a module every 90 seconds.
 *
 * WHY THE VERIFICATION IS FREE
 *
 * The verify step is not an extra transaction.  AT+GTUSBMODE? is already part
 * of the boot identity chain, so a switch is confirmed by the same read that
 * fills in the snapshot - which also means verification happens on the FIRST
 * boot after the switch, including the case where fm160d itself was restarted
 * (or respawned by procd) at exactly the wrong moment.  The pending flag lives
 * in uci precisely so that this survives the process that set it.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "fm160d.h"
#include "usbmode.h"

#define MODESW_SECTION "switch"

/*
 * How long to wait for the AT port after a switch before declaring the profile
 * unreachable.  The design's own UI wording is "the module restarts, expect
 * 30-90 s offline", so this is that window plus the re-enumeration that follows
 * it.  Below it, "no port" is just the switch in progress.
 */
#define MODESW_STUCK_MS    120000

/* An APPLY the modem never acknowledged: it re-enumerated mid-transaction, so
 * the write may well have landed.  Move to VERIFY rather than declaring a
 * failure we cannot distinguish from a success. */
#define MODESW_APPLY_GRACE_MS 15000

/* Quiet window: the module is going to disappear and come back, and every poll
 * in between is a transaction against a device that is not there. */
#define MODESW_QUIET_S         60

static struct fm160_modesw *M(void)
{
	return &g_state.modesw;
}

static void modesw_error(const char *fmt, ...)
{
	struct fm160_modesw *m = M();
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(m->last_error, sizeof(m->last_error), fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------ */
/* uci - the only durable part of this file                             */
/* ------------------------------------------------------------------ */

static void uci_clear_pending(void)
{
	fm160_uci_set(MODESW_SECTION, "pending", "0");
}

static void uci_note_pending(int target, int rollback)
{
	fm160_uci_set_int(MODESW_SECTION, "rollback_mode", rollback);
	fm160_uci_set_int(MODESW_SECTION, "target", target);
	fm160_uci_set_int(MODESW_SECTION, "pending_since", (long long)time(NULL));
	fm160_uci_set(MODESW_SECTION, "pending", "1");
}

void fm160_modesw_init(void)
{
	struct fm160_modesw *m = M();
	struct fm160_dial_state *unused = NULL;
	char v[64];
	long long since = 0;
	time_t now = time(NULL);

	(void)unused;
	memset(m, 0, sizeof(*m));
	m->current = -1;
	m->target = -1;
	m->rollback_mode = -1;
	m->st = MODESW_IDLE;

	if (!fm160_uci_get_section(MODESW_SECTION, "rollback_mode", v, sizeof(v)))
		m->rollback_mode = atoi(v);
	if (!fm160_uci_get_section(MODESW_SECTION, "target", v, sizeof(v)))
		m->target = atoi(v);
	if (!fm160_uci_get_section(MODESW_SECTION, "pending_since", v, sizeof(v)))
		since = strtoll(v, NULL, 10);

	/* "pending" is 1 only between the write and the read-back that confirms
	 * it.  Anything else - including a missing section, i.e. every install
	 * that has never switched - leaves the daemon in IDLE. */
	if (fm160_uci_get_section(MODESW_SECTION, "pending", v, sizeof(v)) ||
	    strcmp(v, "1") != 0)
		return;

	m->pending = true;
	m->st = MODESW_VERIFY;
	m->pending_since_ms = fm160_now_ms();
	if (since > 0 && now >= (time_t)since) {
		/* Carry the clock across the restart instead of restarting it:
		 * a daemon respawned every few seconds would otherwise never
		 * reach the stuck verdict at all, which is the one case this
		 * timer exists for. */
		uint64_t age = (uint64_t)(now - (time_t)since) * 1000;

		m->pending_since_ms = age < m->pending_since_ms ?
				      m->pending_since_ms - age : 0;
	}
	fm160_log(LOG_WARNING,
		  "USB profile switch to %d was never confirmed (applied %lld s "
		  "ago); waiting for the AT port to verify it",
		  m->target, since > 0 ? (long long)(now - (time_t)since) : -1);
}

/* ------------------------------------------------------------------ */
/* the outcome of a past switch                                         */
/* ------------------------------------------------------------------ */

static void verify_settle(int result, bool rolled_back, const char *note)
{
	struct fm160_modesw *m = M();

	m->pending = false;
	m->st = MODESW_IDLE;
	m->verify_result = result;
	m->rolled_back = rolled_back;
	uci_clear_pending();
	/* last_error is what the page shows as "the last thing that happened",
	 * so a settled success clears it and a failure explains itself. */
	if (note)
		modesw_error("%s", note);
	else
		m->last_error[0] = '\0';
	fm160_state_mark_dirty();
}

static void cb_rollback(struct at_req *req, enum at_status status,
			const char *resp, void *arg)
{
	struct fm160_modesw *m = M();

	m->busy = false;
	if (status != AT_STATUS_OK) {
		fm160_log(LOG_ERR,
			  "USB profile rollback to %d was refused (status %d); the "
			  "module will not return on its own",
			  m->rollback_mode, status);
	} else {
		fm160_log(LOG_WARNING,
			  "USB profile rollback to %d accepted; the module is "
			  "re-enumerating", m->rollback_mode);
	}
}

static void verify_mismatch(int seen)
{
	struct fm160_modesw *m = M();
	char cmd[48];

	/*
	 * One rollback, then stop.  A rollback that is itself unverifiable must
	 * not turn into a two-mode oscillation: the module would re-enumerate
	 * forever and the daemon would be the reason.
	 */
	if (m->rolled_back || m->rollback_mode < 0 || m->rollback_mode == seen) {
		fm160_log(LOG_ERR,
			  "USB profile verification failed: wanted %d, the module "
			  "reports %d (rollback %s)", m->target, seen,
			  m->rolled_back ? "already attempted" :
			  m->rollback_mode < 0 ? "point unknown" :
						 "would be a no-op");
		verify_settle(-1, m->rolled_back,
			      "the module came back in a profile other than the "
			      "one requested, and rolling back is not possible");
		return;
	}

	snprintf(cmd, sizeof(cmd), "AT+GTUSBMODE=%d", m->rollback_mode);
	fm160_log(LOG_ERR,
		  "USB profile verification failed: wanted %d, the module "
		  "reports %d - rolling back to %d",
		  m->target, seen, m->rollback_mode);
	m->busy = true;
	if (atq_submit(AT_PRIO_INTERACTIVE, cmd, NULL, 10000, cb_rollback,
		       NULL)) {
		m->busy = false;
		verify_settle(-1, false,
			      "the module came back in the wrong profile and the "
			      "rollback could not be sent");
		return;
	}
	m->rolled_back = true;
	m->verify_result = -1;
	m->target = m->rollback_mode;
	m->st = MODESW_APPLY;
	m->applied_ms = fm160_now_ms();
	uci_note_pending(m->target, m->rollback_mode);
	fm160_state_mark_dirty();
}

static void modesw_stuck(void)
{
	struct fm160_modesw *m = M();

	m->st = MODESW_STUCK;
	modesw_error("no AT port %d s after switching to profile %d; the switch "
		     "may have left the module in a profile this host cannot "
		     "talk to.  fm160d will not switch again by itself - recover "
		     "the module over its DIAG/ADB interface, then clear "
		     "/etc/config/fm160 (switch.pending).",
		     MODESW_STUCK_MS / 1000, m->target);
	fm160_log(LOG_ERR, "USB profile switch: %s", m->last_error);
	fm160_state_mark_dirty();
}

/* Called by the identity chain, which is the only reader of AT+GTUSBMODE?. */
void fm160_modesw_note_current(int mode)
{
	struct fm160_modesw *m = M();

	m->current = mode;
	fm160_state_mark_dirty();

	if (!m->pending || m->target < 0)
		return;

	if (mode == m->target) {
		fm160_log(LOG_INFO, "USB profile switch to %d confirmed", mode);
		verify_settle(1, m->rolled_back, NULL);
		return;
	}
	verify_mismatch(mode);
}

void fm160_modesw_note_caps(const int *modes, int n)
{
	struct fm160_modesw *m = M();
	int i;

	m->supported_n = 0;
	if (n > 0 && modes) {
		for (i = 0; i < n && m->supported_n < FM160_USBMODE_SUP_MAX; i++)
			m->supported[m->supported_n++] = modes[i];
	}
	/* "Answered with nothing" is a licence to switch nowhere, and it is NOT
	 * the same as "never answered": the first refuses every target with
	 * DENY_NOT_SUPPORTED, the second with DENY_LIST_UNKNOWN.  Both refuse,
	 * but the page says which. */
	m->caps_valid = true;
	fm160_log(LOG_INFO, "USB profile switch: the module offers %d profile(s)",
		  m->supported_n);
	fm160_state_mark_dirty();
}

/* ------------------------------------------------------------------ */
/* applying                                                             */
/* ------------------------------------------------------------------ */

static void cb_apply(struct at_req *req, enum at_status status,
		     const char *resp, void *arg)
{
	struct fm160_modesw *m = M();

	m->busy = false;

	if (status == AT_STATUS_TIMEOUT) {
		/* The module re-enumerated before it could answer.  That is what
		 * a successful switch looks like from here as well, so the
		 * pending flag stays up and the read-back decides. */
		fm160_log(LOG_WARNING,
			  "AT+GTUSBMODE=%d did not answer; the module is probably "
			  "re-enumerating already", m->target);
		m->st = MODESW_VERIFY;
		m->applied_ms = fm160_now_ms();
		return;
	}

	if (status != AT_STATUS_OK) {
		/* Nothing was applied, so nothing is pending: clear the marker
		 * rather than leaving a boot to verify a switch that never
		 * happened. */
		fm160_log(LOG_ERR, "AT+GTUSBMODE=%d was refused (status %d)",
			  m->target, status);
		m->pending = false;
		m->st = MODESW_IDLE;
		m->verify_result = -1;
		m->rolled_back = false;
		uci_clear_pending();
		modesw_error("the modem refused the profile change to %d", m->target);
		fm160_state_mark_dirty();
		return;
	}

	fm160_log(LOG_INFO, "AT+GTUSBMODE=%d accepted; waiting for the module to "
		  "come back", m->target);
	m->st = MODESW_VERIFY;
	m->applied_ms = fm160_now_ms();
	fm160_state_mark_dirty();
}

int fm160_modesw_apply(int target, bool advanced_ok)
{
	struct fm160_modesw *m = M();
	enum fm160_usbmode_verdict verdict;
	char cmd[48];
	int cur;

	if (m->st == MODESW_STUCK) {
		modesw_error("a previous switch left the module unreachable; "
			     "recover it by hand before switching again");
		return -EPERM;
	}
	if (m->busy || m->pending) {
		modesw_error("a profile change is already in progress");
		return -EBUSY;
	}
	if (!g_state.enabled)
		return -EAGAIN;

	cur = m->current;
	verdict = fm160_usbmode_decide(cur, target, m->supported,
				       m->supported_n, m->caps_valid,
				       advanced_ok);

	if (verdict == USB_MODE_OK_SAME) {
		/* Not a refusal and not a write: asking for the profile the
		 * module is already in is a no-op, and issuing AT+GTUSBMODE for
		 * it would cost a re-enumeration for nothing. */
		m->last_ok_ms = fm160_now_ms();
		m->last_error[0] = '\0';
		fm160_log(LOG_INFO, "USB profile is already %d", target);
		fm160_state_mark_dirty();
		return 0;
	}

	if (!fm160_usbmode_verdict_is_ok(verdict)) {
		modesw_error("%s", fm160_usbmode_verdict_text(verdict));
		fm160_log(LOG_WARNING, "refusing to switch to USB profile %d: %s",
			  target, m->last_error);
		fm160_state_mark_dirty();
		return verdict == USB_MODE_DENY_LIST_UNKNOWN ? -EAGAIN : -EPERM;
	}

	/*
	 * Past here the decision has said yes, so the only thing left that can
	 * stop the switch is not knowing where we are coming from.  That is a
	 * refusal and not a warning: without a rollback point there is no way
	 * back, and DESIGN 5's whole structure assumes there is one.
	 */
	if (cur < 0) {
		modesw_error("the active profile is not known, so a change could "
			     "not be undone; refusing");
		fm160_log(LOG_WARNING, "refusing to switch to USB profile %d: %s",
			  target, m->last_error);
		return -EPERM;
	}
	if (!g_state.port_found)
		return -EAGAIN;

	snprintf(cmd, sizeof(cmd), "AT+GTUSBMODE=%d", target);

	/* The rollback point goes down FIRST.  Everything after this line can
	 * end with the module unreachable, and this is the only record of where
	 * it was. */
	uci_note_pending(target, cur);
	m->target = target;
	m->rollback_mode = cur;
	m->pending = true;
	m->rolled_back = false;
	m->verify_result = 0;
	m->pending_since_ms = fm160_now_ms();
	m->applied_ms = m->pending_since_ms;
	m->st = MODESW_APPLY;
	m->busy = true;
	atq_set_quiet(QUIET_MODE_SWITCH, "switching USB profile", MODESW_QUIET_S);

	fm160_log(LOG_WARNING, "switching USB profile %d -> %d (%s)", cur, target,
		  cmd);
	if (atq_submit(AT_PRIO_INTERACTIVE, cmd, NULL, 10000, cb_apply, NULL)) {
		/* Never sent, so nothing happened: drop the marker instead of
		 * leaving the next boot to verify a switch that did not occur. */
		m->busy = false;
		m->pending = false;
		m->st = MODESW_IDLE;
		uci_clear_pending();
		modesw_error("the profile change could not be sent to the modem");
		fm160_state_mark_dirty();
		return -EIO;
	}
	fm160_state_mark_dirty();
	return 0;
}

/* ------------------------------------------------------------------ */
/* the tick                                                             */
/* ------------------------------------------------------------------ */

void fm160_modesw_tick(void)
{
	struct fm160_modesw *m = M();

	if (m->st == MODESW_STUCK || !m->pending)
		return;

	/* An APPLY that never came back and never left either: the module
	 * re-enumerated mid-transaction.  Go look rather than guess. */
	if (m->st == MODESW_APPLY &&
	    fm160_now_ms() - m->applied_ms > MODESW_APPLY_GRACE_MS)
		m->st = MODESW_VERIFY;

	if (m->st != MODESW_VERIFY)
		return;

	/* While there is an AT port the identity chain does the verifying -
	 * AT+GTUSBMODE? is already one of its reads. */
	if (g_state.port_found)
		return;
	if (fm160_now_ms() - m->pending_since_ms < MODESW_STUCK_MS)
		return;

	modesw_stuck();
}
