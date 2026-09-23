/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dialer.c - the ECM data plane.
 *
 * WHAT THIS FILE IS, AND WHAT IT IS NOT
 *
 * It is the sequence from the vendor's dial-up document 3.5/3.6, turned into a
 * state machine:
 *
 *   AT+CPIN? -> AT+CREG?/CGREG?/CEREG? -> AT+CGDCONT= -> (probe verb) ->
 *   AT+GTWWAN=1,<cid> -> AT+GTWWAN? until there is an address
 *
 * It is NOT a second implementation of what netifd already does.  QMI and MBIM
 * are dialled by uqmi/umbim through the stock `proto qmi` / `proto mbim`; only
 * ECM needs this, because on ECM the modem stays silent until something
 * activates the PDP context from inside, and netifd has no way to do that.
 * fm160_dial_start() therefore refuses a QMI or MBIM profile and names the
 * protocol to use instead, rather than racing the kernel stack for the same
 * context.
 *
 * WHY A STATE MACHINE AND NOT A SCRIPT
 *
 * Every rung can answer "not yet".  Registration takes tens of seconds,
 * activation is documented as taking up to 210 s, and the first four rungs all
 * fail differently on a modem with no card.  A linear recipe has nowhere to put
 * that, so each rung gets its own deadline, a failure climbs DESIGN 4.3's
 * ladder instead of restarting instantly, and what the modem actually said is
 * kept verbatim in last_error for the page to show.
 *
 * WHAT IS DELIBERATELY ABSENT
 *
 *   - No polling tier.  A dial sequence is a sequence, not a poll: it is driven
 *     from housekeeping (1 Hz) exactly like the SMS setup, and it submits
 *     nothing at all while `wanted` is false.
 *   - No unconditional reset.  The ladder can escalate to AT+CFUN=1,1 only
 *     after all five rungs are spent, only when uci opts in, and only while the
 *     persistent ledger says fewer than FM160_NET_RESET_LIMIT resets have
 *     happened in the last 24 h.  Otherwise it stops and says why - see
 *     dial_escalate().
 *
 * THE ONE THING THIS FILE CANNOT PROVE
 *
 * This device has no SIM.  AT+CPIN? answers ERROR, AT+CGDCONT? returns no data
 * after 5.2 s, and AT+GTWWAN? has never returned an address, so the SUCCESS
 * path below has never run on hardware.  The command builders and the parsers
 * it calls are the ones net.c proves on the host (25-usbmode-dial-test.sh,
 * 327 checks), and the rungs are ordered so that the first thing a live SIM
 * would change is the first thing that would show up in `step`.  Saying this
 * out loud is the point: the alternative is a green tick over an unexercised
 * path.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "fm160d.h"
#include "net.h"
#include "usbmode.h"

/* ------------------------------------------------------------------ */
/* deadlines                                                            */
/* ------------------------------------------------------------------ */

/*
 * Measured response times on this unit, used as the floor for each deadline:
 * AT+CREG?/CGREG?/CEREG? 26-36 ms, AT+GTCCINFO? 25 ms, AT+GTUSBMODE? 16 ms.
 * A rung's timeout is set for the worst case the documentation admits rather
 * than for the measured one, because the case that matters is the one where
 * something is wrong.
 */
#define DIAL_PIN_MS          8000
#define DIAL_REG_MS          5000
#define DIAL_APN_MS          8000
#define DIAL_IP_MS           5000
#define DIAL_READ_MS         5000
#define DIAL_DOWN_MS        30000

/*
 * Activation, and why this is 60 s and not the documented 210 s.
 *
 * AT+GTWWAN=1,<cid> may take up to 210 s to answer, and the transport is
 * synchronous: atq_issue() calls ubus_invoke() on at-daemon, and the daemon's
 * own uloop is parked until that returns.  A 210 s deadline would therefore
 * stop port discovery, the health lamps and the SMS setup for three and a half
 * minutes, which is a worse failure than the one it guards against.
 *
 * So the deadline is 60 s and a timeout is NOT treated as a failure: the rung
 * moves on to READING the context back, which is the only way to find out
 * whether an activation the modem never acknowledged actually happened.  The
 * failure that matters - "the activation was refused" - arrives immediately and
 * as `+GTWWAN: 0`, so it is not affected by this at all.
 *
 * ★ Updated 2026-09-21: a refusal is now treated the same way as a timeout, for
 * the same reason - see cb_activate().  On profile 33 the write IS refused and
 * the link IS up, so "it arrives immediately" was never the useful half of that
 * distinction; "the read-back is the verdict" is.
 */
#define DIAL_ACTIVATE_MS    60000
/* Quiet window covering an activation.  The modem may not accept AT while it
 * is bringing a context up, and the vendor is explicit that the activation is
 * the slow one; keeping the polls off it for a minute costs nothing. */
#define DIAL_QUIET_S           60

/* The address is not there the instant the activation is accepted. */
#define DIAL_IP_GAP_MS       2000
#define DIAL_IP_MAX_POLLS       5
/* How often a link that is already up is read back.  15 ms measured, and it is
 * the only thing in the daemon that can notice the PDP going away underneath
 * netifd. */
#define DIAL_SUPERVISE_MS   60000
/* After AT+CFUN=1,1 the module takes tens of seconds to re-enumerate. */
#define DIAL_POST_RESET_MS  30000
/* Between the deactivate and the activate of DESIGN 4.3's light reset. */
#define DIAL_REACTIVATE_MS   1000

#define DIAL_REG_SUBS           3   /* CREG, CGREG, CEREG                   */

/*
 * The ACTIVATE rung is several transactions long, and `sub` is what carries it
 * across ticks:
 *
 *   verb < 0, sub 0   probing AT+GTWWAN=?
 *   verb < 0, sub 1   probing AT+GTRNDIS=?
 *   verb >= 0         the probe is settled; `was_up` decides whether a
 *                     deactivate is owed first
 *
 * The probe index lives in `sub` rather than in a field of its own because the
 * rung is resumed between ticks and every rung needs the same thing; the
 * registration rung uses it for CREG/CGREG/CEREG in exactly the same way.
 */
#define DIAL_PROBE_SUBS         2

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

/*
 * Copy that reports truncation instead of hiding it.  Same reasoning as net.c's
 * set_str(), and here it also keeps -Wformat-truncation quiet: last_error is
 * FM160_STR_MAX while several of the sources are longer arrays, and a
 * snprintf("%s") between two known array sizes is exactly what that warning
 * exists for.
 */
static void copy_str(char *dst, size_t n, const char *src)
{
	size_t len;

	if (!n)
		return;
	if (!src)
		src = "";
	len = strlen(src);
	if (len >= n)
		len = n - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
}

/* Record why the current attempt failed, and say it in the log too.  The
 * message is what the page shows, so it names the modem's own answer where
 * there is one. */
static void dial_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_state.dial.last_error, sizeof(g_state.dial.last_error),
		  fmt, ap);
	va_end(ap);
	fm160_log(LOG_WARNING, "dial: %s", g_state.dial.last_error);
}

/* The modem's own words, for the error text: the first line that starts with
 * '+' (a "+CME ERROR: 10"), else the first non-empty line. */
static void first_answer_line(const char *resp, char *out, size_t outlen)
{
	const char *p;

	out[0] = '\0';
	if (!resp)
		return;
	p = resp;
	while (*p) {
		const char *e;
		size_t n;

		while (*p == '\r' || *p == '\n')
			p++;
		if (!*p)
			return;
		e = p;
		while (*e && *e != '\r' && *e != '\n')
			e++;
		n = (size_t)(e - p);
		if (n >= outlen)
			n = outlen - 1;
		if (*p == '+' || !out[0]) {
			memcpy(out, p, n);
			out[n] = '\0';
			if (*p == '+')
				return;
		}
		p = e;
	}
}

static const char *status_text(enum at_status s)
{
	switch (s) {
	case AT_STATUS_OK:      return "answered";
	case AT_STATUS_ERROR:   return "refused";
	case AT_STATUS_TIMEOUT: return "did not answer in time";
	case AT_STATUS_NOPORT:  return "had no port";
	case AT_STATUS_BUSY:    return "was rejected by the queue";
	case AT_STATUS_UNKNOWN:
	default:                return "ended without an answer";
	}
}

/* ------------------------------------------------------------------ */
/* the persistent reset ledger                                          */
/* ------------------------------------------------------------------ */

/*
 * "At most FM160_NET_RESET_LIMIT module resets in 24 h, then stop" only means
 * anything if the count outlives the daemon - and procd respawns it.  A daemon
 * that resets, crashes, is respawned and resets again is a boot loop with extra
 * AT traffic in it.
 *
 * Stored as wall-clock epoch seconds, not the monotonic milliseconds used
 * everywhere else, because the point is that it survives the process:
 * CLOCK_MONOTONIC restarts at boot.  That makes it vulnerable to a clock step,
 * which is acceptable here, because the failure it produces is a ledger that
 * forgets - and forgetting means "one more reset is allowed", never "reset
 * forever".
 */
#define LEDGER_MAX 8
#define LEDGER_SECTION "dial"

static void ledger_load(void)
{
	struct fm160_dial_state *st = &g_state.dial;
	char v[192];
	time_t now = time(NULL);
	int i;

	st->resets_in_window = 0;
	st->resets_total = 0;

	if (!fm160_uci_get_section(LEDGER_SECTION, "reset_total", v, sizeof(v)))
		st->resets_total = (int)strtol(v, NULL, 10);
	if (fm160_uci_get_section(LEDGER_SECTION, "reset_stamps", v, sizeof(v)))
		return;

	for (i = 0; i < LEDGER_MAX && v[0]; i++) {
		char *comma = strchr(v, ',');
		long long ts;

		if (comma)
			*comma = '\0';
		ts = strtoll(v, NULL, 10);
		/* Pruned while loading rather than on a timer: the window is
		 * only ever consulted at the moment of a decision. */
		if (ts > 0 && now >= (time_t)ts &&
		    (long long)(now - (time_t)ts) < FM160_NET_RESET_WINDOW_S)
			st->resets_in_window++;
		if (!comma)
			break;
		memmove(v, comma + 1, strlen(comma + 1) + 1);
	}
}

static void ledger_note_reset(void)
{
	struct fm160_dial_state *st = &g_state.dial;
	char stamps[192], v[192];
	time_t now = time(NULL);
	int i, w;
	size_t off;

	st->resets_total++;
	st->resets_in_window++;

	/* Newest first, pruned to the window, at most LEDGER_MAX entries.
	 * Written back as one comma-joined string because uci has no list type
	 * worth the ceremony for eight numbers. */
	w = snprintf(stamps, sizeof(stamps), "%lld", (long long)now);
	off = (w > 0) ? (size_t)w : 0;

	if (!fm160_uci_get_section(LEDGER_SECTION, "reset_stamps", v, sizeof(v))) {
		for (i = 0; i < LEDGER_MAX - 1 && v[0]; i++) {
			char *comma = strchr(v, ',');
			long long ts;

			if (comma)
				*comma = '\0';
			ts = strtoll(v, NULL, 10);
			if (ts <= 0 || (long long)(now - (time_t)ts) >=
				       FM160_NET_RESET_WINDOW_S)
				break;
			w = snprintf(stamps + off, sizeof(stamps) - off,
				     ",%lld", ts);
			if (w < 0 || (size_t)w >= sizeof(stamps) - off)
				break;
			off += (size_t)w;
			if (!comma)
				break;
			memmove(v, comma + 1, strlen(comma + 1) + 1);
		}
	}

	fm160_uci_set(LEDGER_SECTION, "reset_stamps", stamps);
	fm160_uci_set_int(LEDGER_SECTION, "reset_total", st->resets_total);
	fm160_log(LOG_ERR,
		  "dial: module reset %d in this 24 h window (limit %d), %d total",
		  st->resets_in_window, FM160_NET_RESET_LIMIT, st->resets_total);
}

/* ------------------------------------------------------------------ */
/* state transitions                                                    */
/* ------------------------------------------------------------------ */

static bool dial_kind_is_ours(const struct fm160_dial_state *st)
{
	return st->kind_known && st->kind == DIAL_KIND_ECM;
}

static void dial_to(enum fm160_net_step step)
{
	g_state.dial.step = step;
	g_state.dial.sub = 0;
	fm160_state_mark_dirty();
}

/* Stop healing, with the reason attached.  Reached from two places and both of
 * them are a deliberate decision rather than a timeout: the APN cannot be used
 * at all, or the ladder is spent and resetting is not allowed. */
static void dial_stop_healing(const char *reason)
{
	struct fm160_dial_state *st = &g_state.dial;

	st->healing_stopped = true;
	st->next_try_ms = UINT64_MAX;
	dial_to(NET_STEP_FAILED);
	dial_error("%s", reason);
}

static void dial_escalate(void)
{
	struct fm160_dial_state *st = &g_state.dial;

	if (!st->allow_reset) {
		dial_stop_healing("the reconnect ladder is spent and automatic "
				  "module resets are off (fm160.main.dial_allow_reset); "
				  "check the SIM and the APN, then dial again");
		return;
	}
	if (!fm160_net_reset_allowed(st->resets_in_window)) {
		dial_stop_healing(fm160_net_reset_refusal());
		return;
	}

	ledger_note_reset();
	fm160_log(LOG_ERR, "dial: resetting the module (AT+CFUN=1,1)");
	atq_submit(AT_PRIO_STATE, "AT+CFUN=1,1", NULL, 10000, NULL, NULL);
	dial_error("the ladder was spent; the module was reset (%d of %d resets "
		   "used in this 24 h window)", st->resets_in_window,
		   FM160_NET_RESET_LIMIT);

	/* The module re-enumerates.  Leave it alone for a while, then start
	 * again from the top with a cleared ladder.  `wanted` is untouched, so
	 * this only continues while the user still wants the link. */
	st->attempt = 0;
	st->attempt_ms = fm160_now_ms();
	st->next_try_ms = fm160_now_ms() + DIAL_POST_RESET_MS;
	dial_to(NET_STEP_PIN);
}

/* A rung failed.  Climb DESIGN 4.3's ladder, or stop. */
static void dial_fail(const char *reason)
{
	struct fm160_dial_state *st = &g_state.dial;
	int rung = st->attempt;

	st->failures++;
	st->running = false;
	st->up = false;
	st->ip_polls = 0;
	copy_str(st->last_error, sizeof(st->last_error), reason);
	fm160_log(LOG_WARNING, "dial: attempt %d failed: %s", rung, reason);

	if (rung >= fm160_net_ladder_len()) {
		/* All five rungs spent.  This is where a naive daemon loops
		 * forever; see ledger_note_reset() for what stopping buys. */
		dial_escalate();
		return;
	}

	st->next_try_ms = fm160_now_ms() +
			  (uint64_t)fm160_net_backoff_ms(rung);
	fm160_log(LOG_INFO, "dial: restarting at the first rung in %d s",
		  fm160_net_backoff_s(rung));
	st->attempt = rung + 1;
	st->attempt_ms = fm160_now_ms();
	dial_to(NET_STEP_PIN);
}

/* ------------------------------------------------------------------ */
/* the rungs                                                            */
/* ------------------------------------------------------------------ */

static void dial_submit(const char *cmd, int timeout_ms, at_done_cb cb,
			void *arg);

/* --- 1. the card ------------------------------------------------ */

static void cb_pin(struct at_req *req, enum at_status status,
		   const char *resp, void *arg)
{
	struct fm160_dial_state *st = &g_state.dial;
	char word[FM160_STR_MAX];
	int r;

	st->running = false;
	first_answer_line(resp, word, sizeof(word));

	if (status != AT_STATUS_OK) {
		dial_fail("AT+CPIN? could not be answered");
		dial_error("AT+CPIN? %s%s%s", status_text(status),
			   word[0] ? ": " : "", word);
		return;
	}

	r = fm160_net_parse_cpin(resp);
	if (r == 1) {
		st->sim_ready = true;
		st->next_try_ms = 0;
		fm160_log(LOG_INFO, "dial: the SIM reports READY");
		dial_to(NET_STEP_REG);
		return;
	}
	if (r == 0) {
		/* The card is present but not usable yet - "SIM PIN", "NOT
		 * INSERTED", "NOT READY".  The word is the whole diagnosis, so
		 * it goes into the message rather than being replaced by our
		 * own summary of it. */
		dial_fail("the SIM is not ready");
		dial_error("the SIM is not ready (%s)", word);
		return;
	}
	dial_fail("AT+CPIN? was answered but carried no +CPIN line");
}

/* --- 2. registration -------------------------------------------- */

static const char *reg_token(int sub)
{
	switch (sub) {
	case 0:  return "CREG";
	case 1:  return "CGREG";
	default: return "CEREG";
	}
}

static void cb_reg(struct at_req *req, enum at_status status,
		   const char *resp, void *arg)
{
	struct fm160_dial_state *st = &g_state.dial;
	int stat = FM160_NET_REG_UNKNOWN;

	st->running = false;

	/*
	 * A refused query is not a failed rung.  With no card fitted these can
	 * answer ERROR, and the answer that matters is the one from the domain
	 * that does work - so an ERROR from CREG must not stop CEREG from being
	 * asked.  Only "every domain said not registered" fails.
	 */
	if (status == AT_STATUS_OK)
		stat = fm160_net_parse_reg(resp, reg_token(st->sub));

	if (stat == 1 || stat == 5) {
		fm160_log(LOG_INFO, "dial: registered (+%s: %d)",
			  reg_token(st->sub), stat);
		st->next_try_ms = 0;
		dial_to(NET_STEP_APN);
		return;
	}

	st->sub++;
	if (st->sub < DIAL_REG_SUBS) {
		st->next_try_ms = 0;      /* next domain, same tick if free */
		return;
	}

	dial_fail("the modem is not registered on any domain "
		  "(CREG/CGREG/CEREG all say no)");
}

/* --- 3. the PDP context ----------------------------------------- */

static void cb_apn(struct at_req *req, enum at_status status,
		   const char *resp, void *arg)
{
	struct fm160_dial_state *st = &g_state.dial;
	char word[FM160_STR_MAX];

	st->running = false;
	first_answer_line(resp, word, sizeof(word));

	if (status != AT_STATUS_OK) {
		dial_fail("the modem refused the PDP context");
		dial_error("AT+CGDCONT= %s%s%s", status_text(status),
			   word[0] ? ": " : "", word);
		return;
	}
	st->next_try_ms = 0;
	dial_to(NET_STEP_ACTIVATE);
}

/* --- 4. activation (and the verb probe that has to come first) --- */

static void cb_verb_probe(struct at_req *req, enum at_status status,
			  const char *resp, void *arg)
{
	struct fm160_dial_state *st = &g_state.dial;
	enum fm160_net_verb tried = (enum fm160_net_verb)(intptr_t)arg;

	st->running = false;

	if (status == AT_STATUS_OK) {
		st->verb = (int)tried;
		fm160_log(LOG_INFO, "dial: this unit answers AT+%s=?",
			  fm160_net_verb_name(tried));
		st->sub = 0;               /* settled; the rung moves on */
		st->next_try_ms = 0;
		return;
	}

	if (tried == NET_VERB_GTWWAN) {
		/* The vendor contradicts itself about ECM (net.h), so both are
		 * tried and whichever answers is remembered. */
		st->sub = 1;
		st->next_try_ms = 0;
		return;
	}

	dial_fail("neither AT+GTWWAN=? nor AT+GTRNDIS=? was answered, so the "
		  "activation verb for this unit cannot be determined");
}

static void cb_activate(struct at_req *req, enum at_status status,
			const char *resp, void *arg)
{
	struct fm160_dial_state *st = &g_state.dial;
	struct fm160_net_wwan w;
	char word[FM160_STR_MAX];

	st->running = false;
	first_answer_line(resp, word, sizeof(word));

	if (status == AT_STATUS_TIMEOUT) {
		/*
		 * Not a failure.  The activation is documented as taking up to
		 * 210 s and our deadline is 60 s (see DIAL_ACTIVATE_MS), so the
		 * only honest conclusion is "unknown" - and the way to resolve
		 * an unknown is to read the context back, which is the next
		 * rung anyway.
		 */
		fm160_log(LOG_WARNING,
			  "dial: the activation did not answer within %d ms; "
			  "reading the context back to find out",
			  DIAL_ACTIVATE_MS);
		st->next_try_ms = 0;
		dial_to(NET_STEP_IP);
		return;
	}

	if (status != AT_STATUS_OK) {
		/*
		 * ★★ A REFUSAL IS NOT A FAILED RUNG, and that is measured on
		 * this unit rather than argued from the manual.
		 *
		 * net.h already records the shape of it - "profile 33 (ECM)
		 * +GTWWAN=1,1 REFUSED, +GTRNDIS=1,1 REFUSED too, ...and the
		 * ECM data plane was carrying traffic anyway" - and the same
		 * header draws the conclusion this branch was violating:
		 * "a refusal of the write is not proof that the context is
		 *  down: the module is allowed to have activated it on its
		 *  own, which is exactly what profile 33 does.  The read-back
		 *  is the verdict, never the write's status."
		 *
		 * Real capture, 2026-09-21, profile 33, this unit, with no host
		 * write of any kind beforehand (so the context was the module's
		 * own doing, and AT+GTAUTOCONNECT was 0):
		 *
		 *   AT+GTWWAN=1,1  -> ERROR                    <- what the page showed
		 *   AT+GTWWAN?     -> +GTWWAN: 1,1,"10.179.143.75,240e:400:...",
		 *                                "218.2.2.2,240e:5a::6666",
		 *                                "218.4.4.4,240e:5b::6666"
		 *
		 * and the interface went on to move 38.5 MB.  Failing here is
		 * what produced step 8 / "the activation was refused" over a
		 * link that had been up the whole time.
		 *
		 * So a refusal falls through to the read-back rung exactly like
		 * a timeout does.  A genuinely refused activation is still
		 * caught, one rung later and with a message that is true: the
		 * read-back answers "+GTWWAN: 0" and cb_ip() fails on that.  The
		 * cost is one rung of latency, and dial_fail() there still
		 * counts the attempt, so the ladder is unaffected.
		 */
		fm160_log(LOG_WARNING,
			  "dial: AT+%s=1,%d was refused (%s%s%s); reading the "
			  "context back, because on this profile a refusal is "
			  "not the verdict",
			  fm160_net_verb_name((enum fm160_net_verb)st->verb),
			  st->cid, status_text(status),
			  word[0] ? ": " : "", word);
		st->next_try_ms = 0;
		dial_to(NET_STEP_IP);
		return;
	}

	/* OK, but the answer may still carry the documented refusal. */
	if (fm160_net_parse_wwan(resp, &w) && w.valid && w.active == 0) {
		dial_fail("the modem refused to activate the context (+GTWWAN: 0)");
		return;
	}

	st->next_try_ms = 0;
	dial_to(NET_STEP_IP);
}

/* --- 5. the address (and the supervision of a link that is up) --- */

static void cb_ip(struct at_req *req, enum at_status status,
		  const char *resp, void *arg)
{
	struct fm160_dial_state *st = &g_state.dial;
	struct fm160_net_wwan w;
	char word[FM160_STR_MAX];
	bool was_up = st->up;

	st->running = false;

	if (status != AT_STATUS_OK || !fm160_net_parse_wwan(resp, &w) ||
	    !w.valid) {
		if (status != AT_STATUS_OK) {
			first_answer_line(resp, word, sizeof(word));
			fm160_log(LOG_DEBUG, "dial: the context read %s%s%s",
				  status_text(status), word[0] ? ": " : "",
				  word);
		}
		goto not_yet;
	}

	/* "+GTWWAN: 0" - the context is not active at all.  Reading it four
	 * more times would only be four more ways to see the same zero. */
	if (w.active == 0) {
		dial_fail("the context is not active (+GTWWAN: 0)");
		return;
	}

	if (!w.has_addr) {
		fm160_log(LOG_DEBUG,
			  "dial: the context is active but has no address yet "
			  "(%d field(s))", w.raw_n);
		goto not_yet;
	}

	/* The address is about to be handed to netifd, whose failure mode with
	 * an address it cannot parse is an interface that looks up and carries
	 * nothing.  Refuse it here instead. */
	if (!fm160_net_addr_ok(w.addr)) {
		dial_fail("the modem reported an address this host will not accept");
		return;
	}

	copy_str(st->addr, sizeof(st->addr), w.addr);
	copy_str(st->dns1, sizeof(st->dns1), w.dns1);
	copy_str(st->dns2, sizeof(st->dns2), w.dns2);
	copy_str(st->pdp_in_use, sizeof(st->pdp_in_use), w.pdp);
	st->up = true;
	st->was_up = true;
	st->last_ok_ms = fm160_now_ms();
	st->last_error[0] = '\0';
	st->attempt = 0;               /* the link works: the ladder resets */
	st->attempt_ms = fm160_now_ms();
	st->ip_polls = 0;
	st->step = NET_STEP_UP;
	st->sub = 0;
	st->next_try_ms = fm160_now_ms() + DIAL_SUPERVISE_MS;
	if (!was_up)
		fm160_log(LOG_INFO, "dial: context %d is up, address %s",
			  st->cid, st->addr);
	fm160_state_mark_dirty();
	return;

not_yet:
	st->ip_polls++;
	if (st->ip_polls < DIAL_IP_MAX_POLLS) {
		st->next_try_ms = fm160_now_ms() + DIAL_IP_GAP_MS;
		return;
	}
	dial_fail(was_up ? "the link stopped answering the context read"
			 : "no address appeared after five reads of the context");
}

/* --- the disconnect -------------------------------------------- */

static void cb_down(struct at_req *req, enum at_status status,
		    const char *resp, void *arg)
{
	struct fm160_dial_state *st = &g_state.dial;
	char word[FM160_STR_MAX];

	st->running = false;
	first_answer_line(resp, word, sizeof(word));
	if (status != AT_STATUS_OK) {
		/* The link is down as far as this host is concerned either way;
		 * what is recorded is that the modem did not confirm it, because
		 * the vendor is explicit that a modem left active keeps the
		 * session that nobody is using. */
		dial_error("the deactivation was not confirmed by the modem (%s%s%s)",
			   status_text(status), word[0] ? ": " : "", word);
	} else {
		st->last_error[0] = '\0';
		fm160_log(LOG_INFO, "dial: context %d deactivated", st->cid);
	}
	st->step = NET_STEP_DOWN;
	st->sub = 0;
	st->up = false;
	st->was_up = false;
	fm160_state_mark_dirty();
}

/* ------------------------------------------------------------------ */
/* submission                                                           */
/* ------------------------------------------------------------------ */

static void dial_submit(const char *cmd, int timeout_ms, at_done_cb cb,
			void *arg)
{
	int ret;

	g_state.dial.running = true;
	ret = atq_submit(AT_PRIO_STATE, cmd, NULL, timeout_ms, cb, arg);
	if (ret) {
		/* atq_submit() answers without calling back on these paths, so
		 * the in-flight flag has to be cleared here or the ladder stops
		 * dead with `running` stuck true. */
		g_state.dial.running = false;
		g_state.dial.next_try_ms = fm160_now_ms() + 2000;
		fm160_log(LOG_DEBUG, "dial: %s not submitted (rc=%d)", cmd, ret);
	}
}

/* ------------------------------------------------------------------ */
/* the tick                                                             */
/* ------------------------------------------------------------------ */

static void dial_sync_kind(void)
{
	struct fm160_dial_state *st = &g_state.dial;
	bool known = g_state.usb_mode > 0;
	enum fm160_dial_kind kind = known ?
		fm160_usbmode_kind(g_state.usb_mode) : DIAL_KIND_NONE;

	if (known == st->kind_known && kind == st->kind)
		return;
	st->kind_known = known;
	st->kind = kind;
	fm160_state_mark_dirty();
}

void fm160_dial_init(void)
{
	struct fm160_dial_state *st = &g_state.dial;

	/* Called before fm160_config_load(), so this can wipe the table: the
	 * APN, the context id and the two flags arrive from uci afterwards. */
	memset(st, 0, sizeof(*st));
	st->verb = -1;
	st->step = NET_STEP_IDLE;
	st->kind_known = false;
	st->cid = 1;
	st->pdp = NET_PDP_IP;
	ledger_load();
	fm160_log(LOG_INFO, "dial: reset ledger loaded (%d reset(s) in the last "
		  "24 h, %d total)", st->resets_in_window, st->resets_total);
}

void fm160_dial_configure(const char *apn, const char *pdp, int cid,
			  bool allow_reset, bool autostart)
{
	struct fm160_dial_state *st = &g_state.dial;
	enum fm160_net_pdp p;

	st->allow_reset = allow_reset;
	st->autostart = autostart;
	if (!autostart)
		st->autostart_done = false;

	if (apn && apn[0]) {
		if (fm160_net_apn_ok(apn)) {
			if (strcmp(st->apn, apn))
				copy_str(st->apn, sizeof(st->apn), apn);
		} else if (strcmp(st->apn, apn)) {
			/* Said once, when the value changes: a bad APN is the
			 * single most likely reason for a dial that never comes
			 * up, and it is invisible in every other way.  It is
			 * also not stored, so dial_start() refuses rather than
			 * sending a half-built command. */
			fm160_log(LOG_ERR,
				  "dial: APN '%s' is not usable (letters, digits, "
				  "'.', '-' and '_' only; no leading or trailing "
				  "dot); it will not be used", apn);
		}
	}
	if (pdp && fm160_net_pdp_parse(pdp, &p))
		st->pdp = p;
	if (fm160_net_cid_ok(cid))
		st->cid = cid;
	fm160_state_mark_dirty();
}

bool fm160_dial_usable(void)
{
	return dial_kind_is_ours(&g_state.dial);
}

int fm160_dial_start(void)
{
	struct fm160_dial_state *st = &g_state.dial;

	/* A second start on a link that is still coming up is not an error -
	 * the proto script may legitimately call this again after a reload -
	 * so it is idempotent rather than refused. */
	if (st->wanted && !st->healing_stopped)
		return 0;

	if (!fm160_net_apn_ok(st->apn)) {
		st->config_error = true;
		dial_error("no usable APN is configured (fm160.main.dial_apn)");
		return -EINVAL;
	}
	if (!dial_kind_is_ours(st)) {
		if (!st->kind_known)
			return -EAGAIN;         /* the profile is not read yet */
		fm160_log(LOG_WARNING,
			  "dial: USB profile %d is dialled by the kernel stack, "
			  "not by fm160d; use %s instead", g_state.usb_mode,
			  st->kind == DIAL_KIND_MBIM ? "proto mbim (umbim)" :
			  st->kind == DIAL_KIND_QMI  ? "proto qmi (uqmi)" :
						       "one of the data profiles");
		dial_error("USB profile %d is not an ECM profile, and this daemon "
			   "only dials ECM", g_state.usb_mode);
		return -ENOTSUP;
	}
	if (!g_state.port_found)
		return -EAGAIN;                 /* no AT port yet: try later */

	st->config_error = false;
	st->healing_stopped = false;
	st->wanted = true;
	st->attempt = 0;
	st->attempt_ms = fm160_now_ms();
	st->next_try_ms = 0;
	st->ip_polls = 0;
	st->was_up = false;
	st->up = false;
	st->last_error[0] = '\0';
	st->starts++;
	fm160_log(LOG_INFO, "dial: starting (cid %d, %s, APN %s)", st->cid,
		  fm160_net_pdp_name(st->pdp), st->apn);
	dial_to(NET_STEP_PIN);
	fm160_sched_kick();
	return 0;
}

int fm160_dial_stop(void)
{
	struct fm160_dial_state *st = &g_state.dial;
	char cmd[FM160_NET_CMD_MAX];

	/* Idempotent: the proto script's teardown may run when the link was
	 * never up, and that is not an error. */
	if (!st->wanted && st->step == NET_STEP_DOWN)
		return 0;

	st->wanted = false;
	st->up = false;
	st->config_error = false;
	st->ip_polls = 0;
	st->sub = 0;
	st->next_try_ms = 0;

	/*
	 * The verb may never have been probed.  In that case no activation was
	 * ever attempted either, so there is nothing to tear down - and
	 * inventing AT+GTWWAN=0 for a unit that might answer only +GTRNDIS is
	 * exactly the hardcoding net.h forbids.  Say so instead.
	 */
	if (st->verb < 0) {
		fm160_log(LOG_INFO, "dial: nothing to tear down (no activation was "
			  "ever attempted)");
		st->step = NET_STEP_DOWN;
		fm160_state_mark_dirty();
		return 0;
	}
	if (fm160_net_activate_command(cmd, sizeof(cmd),
				       (enum fm160_net_verb)st->verb,
				       st->cid, false) < 0) {
		st->step = NET_STEP_DOWN;
		return -EINVAL;
	}

	/* The vendor's red line: pulling the cable or powering the module down
	 * does NOT disconnect.  This command is the disconnect. */
	dial_submit(cmd, DIAL_DOWN_MS, cb_down, NULL);
	st->step = NET_STEP_DOWN;
	st->sub = 0;
	fm160_state_mark_dirty();
	return 0;
}

void fm160_dial_tick(void)
{
	struct fm160_dial_state *st = &g_state.dial;
	uint64_t now = fm160_now_ms();
	char cmd[FM160_NET_CMD_MAX];

	if (!g_state.enabled)
		return;

	dial_sync_kind();

	/* One attempt at the link per boot, if uci asked for it.  Held back
	 * until the profile is known, because starting on a QMI profile could
	 * only produce a refusal. */
	if (st->autostart && !st->autostart_done && st->kind_known &&
	    !st->wanted && !st->config_error) {
		st->autostart_done = true;
		if (fm160_dial_start())
			fm160_log(LOG_WARNING,
				  "dial: autostart declined (see the message above)");
	}

	if (!st->wanted || st->healing_stopped)
		return;
	if (!g_state.port_found || g_state.at_state >= 2)
		return;
	if (st->running)
		return;
	if (now < st->next_try_ms)
		return;
	if (atq_depth() >= 3)
		return;

	switch (st->step) {
	case NET_STEP_IDLE:
	case NET_STEP_DOWN:
	case NET_STEP_BUSY:
	case NET_STEP_FAILED:
		return;

	case NET_STEP_PIN:
		dial_submit("AT+CPIN?", DIAL_PIN_MS, cb_pin, NULL);
		return;

	case NET_STEP_REG:
		if (st->sub >= DIAL_REG_SUBS) {
			st->sub = 0;
			dial_fail("the modem is not registered on any domain");
			return;
		}
		snprintf(cmd, sizeof(cmd), "AT+%s?", reg_token(st->sub));
		dial_submit(cmd, DIAL_REG_MS, cb_reg, NULL);
		return;

	case NET_STEP_APN:
		if (fm160_net_apn_command(cmd, sizeof(cmd), st->cid, st->pdp,
					  st->apn) < 0) {
			/* A configuration fault, not a link fault.  The ladder
			 * exists for "try again later"; this cannot get better
			 * by being retried, so it stops with the reason
			 * attached instead of climbing. */
			st->config_error = true;
			dial_stop_healing("the APN or the context id is not "
					  "usable, so the PDP context cannot be "
					  "set (fm160.main.dial_apn)");
			return;
		}
		dial_submit(cmd, DIAL_APN_MS, cb_apn, NULL);
		return;

	case NET_STEP_ACTIVATE:
		if (st->verb < 0) {
			enum fm160_net_verb v = (st->sub == 0) ? NET_VERB_GTWWAN :
							        NET_VERB_GTRNDIS;

			if (st->sub >= DIAL_PROBE_SUBS) {
				st->sub = 0;
				dial_fail("neither AT+GTWWAN=? nor AT+GTRNDIS=? "
					  "was answered");
				return;
			}
			fm160_net_probe_command(cmd, sizeof(cmd), v, true);
			dial_submit(cmd, DIAL_READ_MS, cb_verb_probe,
				    (void *)(intptr_t)v);
			return;
		}
		if (st->was_up) {
			/* DESIGN 4.3's light reset: deactivate first, because
			 * activating a context the network still considers open
			 * does not work. */
			if (fm160_net_activate_command(cmd, sizeof(cmd),
						       (enum fm160_net_verb)st->verb,
						       st->cid, false) < 0) {
				st->was_up = false;
				return;
			}
			fm160_log(LOG_INFO, "dial: deactivating context %d before "
				  "reactivating it", st->cid);
			/* Fire and forget: the vendor's own sequence is
			 * deactivate-then-activate and the activate is what
			 * decides, so a refusal here is not worth a rung. */
			atq_submit(AT_PRIO_STATE, cmd, NULL, DIAL_DOWN_MS,
				   NULL, NULL);
			st->was_up = false;
			st->next_try_ms = now + DIAL_REACTIVATE_MS;
			return;
		}
		if (fm160_net_activate_command(cmd, sizeof(cmd),
					       (enum fm160_net_verb)st->verb,
					       st->cid, true) < 0) {
			dial_fail("the activation command cannot be built for "
				  "this context");
			return;
		}
		atq_set_quiet(QUIET_DIAL, "activating the data context",
			      DIAL_QUIET_S);
		dial_submit(cmd, DIAL_ACTIVATE_MS, cb_activate, NULL);
		return;

	case NET_STEP_IP:
		fm160_net_probe_command(cmd, sizeof(cmd),
					(enum fm160_net_verb)st->verb, false);
		dial_submit(cmd, DIAL_IP_MS, cb_ip, NULL);
		return;

	case NET_STEP_UP:
		/* The link is up and netifd owns the interface.  This read is
		 * the only thing in the daemon that can notice the PDP going
		 * away underneath it - 15 ms measured, once a minute. */
		fm160_net_probe_command(cmd, sizeof(cmd),
					(enum fm160_net_verb)st->verb, false);
		dial_submit(cmd, DIAL_READ_MS, cb_ip, NULL);
		return;
	}
}
