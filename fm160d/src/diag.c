/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * diag.c - the support bundle.  See diag.h for the contract.
 *
 * The whole file is one pass of snprintf into a walking buffer.  Two things are
 * worth knowing before changing it:
 *
 *   - every format is written with an explicit cast (%llu against
 *     (unsigned long long)).  uint64_t is `unsigned long` on this target, and
 *     -Wall -Wextra treats the mismatch as an error here because tools/cccheck
 *     fails on any output at all.  A "%llu" without the cast compiles on 64-bit
 *     and warns on nothing else until someone builds 32-bit.
 *
 *   - a counter that means "never happened" is printed by name, not as its
 *     sentinel.  A report that says "resets: 0" when the real answer is "no
 *     reset has ever been attempted" invites the reader to conclude the feature
 *     is working; `none` and `0` are different answers and are printed as such.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fm160d.h"

#define U64(x) ((unsigned long long)(x))

/* ------------------------------------------------------------------ */
/* a buffer that knows when it ran out                                  */
/* ------------------------------------------------------------------ */

struct w {
	char  *buf;
	size_t cap;     /* including the terminating NUL */
	size_t len;
	bool   over;    /* something did not fit */
};

static void w_put(struct w *w, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (w->len + 1 >= w->cap) {
		w->over = true;
		return;
	}

	va_start(ap, fmt);
	n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
	va_end(ap);

	if (n < 0) {
		w->over = true;
		return;
	}
	if ((size_t)n >= w->cap - w->len) {
		/* vsnprintf truncated it and returned what it would have needed.
		 * Treating that as success is how a bundle ends mid-sentence and
		 * still looks fine, so it is a hard "over" instead. */
		w->over = true;
		w->len = w->cap - 1;
		return;
	}
	w->len += (size_t)n;
}

/* ------------------------------------------------------------------ */
/* small formatters                                                     */
/* ------------------------------------------------------------------ */

static uint64_t age_of(uint64_t now, uint64_t then)
{
	/* A log line can carry a stamp from before the caller's now_ms was read
	 * (it is read after the snapshot is taken), and a monotonic clock can
	 * only go forward - but a zero would mean 1970 if it ever did not. */
	if (!then || then > now)
		return 0;
	return now - then;
}

static void w_ago(struct w *w, uint64_t now, uint64_t then)
{
	uint64_t a;

	if (!then) {
		w_put(w, "never");
		return;
	}
	a = age_of(now, then);
	w_put(w, "%llu.%03llu s ago", U64(a / 1000), U64(a % 1000));
}

/* An int whose value may be the "not reported" sentinel. */
static void w_int(struct w *w, int v)
{
	if (v == FM160_NONE) {
		w_put(w, "-");
		return;
	}
	w_put(w, "%d", v);
}

/* A string that may be empty because nothing has answered yet. */
static void w_str(struct w *w, const char *s)
{
	w_put(w, "%s", (s && s[0]) ? s : "-");
}

static const char *bool_str(bool b)
{
	return b ? "yes" : "no";
}

/*
 * The single-letter priority used in the log tail.  Also the reason this file
 * does not include <syslog.h>: fm160d.h defines the level values when syslog.h
 * is absent, which is what lets this compile both as part of the daemon and as
 * a host-side test binary.  LOG_NOTICE and LOG_WARNING are spelled out here for
 * the same reason.
 */
static char prio_char(int prio)
{
	switch (prio) {
	case 0: case 1: case 2:
	case LOG_ERR:     return 'E';
	case 4:           return 'W';   /* LOG_WARNING */
	case 5:           return 'N';   /* LOG_NOTICE  */
	case LOG_INFO:    return 'I';
	case LOG_DEBUG:   return 'D';
	default:          return '?';
	}
}

/* ------------------------------------------------------------------ */
/* the bundle                                                           */
/* ------------------------------------------------------------------ */

long fm160_diag_format(const struct fm160_diag_in *in, char *buf, size_t buflen)
{
	const struct fm160_state *st;
	struct w w;
	uint64_t dropped;
	size_t i;

	if (!in || !in->st || !buf || buflen == 0)
		return -1;

	st = in->st;
	w.buf = buf;
	w.cap = buflen;
	w.len = 0;
	w.over = false;

	/* --- header ---------------------------------------------------- */
	w_put(&w, "fm160d diagnostics\n==================\n");
	w_put(&w, "version      : %s\n",
	      (in->version && in->version[0]) ? in->version : "unknown");
	w_put(&w, "generated    : %s\n",
	      (in->wall && in->wall[0]) ? in->wall : "(clock not set)");
	w_put(&w, "uptime       : %llu.%03llu s\n",
	      U64(in->uptime_ms / 1000), U64(in->uptime_ms % 1000));
	w_put(&w, "log level    : %d\n", in->log_level);

	dropped = (in->log_total > in->log_n) ? in->log_total - in->log_n : 0;
	w_put(&w, "log ring     : %u of %d lines kept, %llu dropped\n",
	      (unsigned)in->log_n, FM160_LOG_RING_LINES, U64(dropped));
	w_put(&w, "management   : %s\n", st->enabled ? "enabled" : "disabled");
	w_put(&w, "foreground   : %s, last active ", bool_str(st->foreground));
	w_ago(&w, in->now_ms, st->last_active_ms);
	w_put(&w, "\n");

	/* --- port ------------------------------------------------------- */
	w_put(&w, "\n-- port --\n");
	w_put(&w, "port         : "); w_str(&w, st->port); w_put(&w, "\n");
	w_put(&w, "found        : %s\n", bool_str(st->port_found));
	w_put(&w, "probing      : %s\n", bool_str(st->port_probing));
	w_put(&w, "candidates   : %d\n", st->cand_count);

	/* --- identity --------------------------------------------------- */
	w_put(&w, "\n-- identity --\n");
	w_put(&w, "manufacturer : "); w_str(&w, st->manufacturer); w_put(&w, "\n");
	w_put(&w, "model        : "); w_str(&w, st->model); w_put(&w, "\n");
	w_put(&w, "revision     : "); w_str(&w, st->revision); w_put(&w, "\n");
	w_put(&w, "usb mode     : "); w_int(&w, st->usb_mode); w_put(&w, "\n");
	w_put(&w, "ident done   : %s\n", bool_str(st->ident_done));
	/*
	 * These three are personal data.  They are in the bundle because a
	 * support case for a modem is usually decided by them - an IMEI that
	 * the operator has blacklisted, an ICCID that identifies the SIM - and
	 * a bundle the user cannot hand over unedited is a bundle that does not
	 * get sent.  The page warns that the file contains them.
	 */
	w_put(&w, "imei         : "); w_str(&w, st->imei); w_put(&w, "\n");
	w_put(&w, "sn           : "); w_str(&w, st->sn); w_put(&w, "\n");
	w_put(&w, "iccid        : "); w_str(&w, st->iccid); w_put(&w, "\n");

	/* --- sim and registration --------------------------------------- */
	w_put(&w, "\n-- sim and registration --\n");
	w_put(&w, "pin          : "); w_str(&w, st->pin_status); w_put(&w, "\n");
	w_put(&w, "creg         : "); w_int(&w, st->creg); w_put(&w, "\n");
	w_put(&w, "cgreg        : "); w_int(&w, st->cgreg); w_put(&w, "\n");
	w_put(&w, "cereg        : "); w_int(&w, st->cereg); w_put(&w, "\n");
	w_put(&w, "c5greg       : "); w_int(&w, st->c5greg); w_put(&w, "\n");
	w_put(&w, "operator     : "); w_str(&w, st->oper); w_put(&w, "\n");
	w_put(&w, "access tech  : "); w_int(&w, st->act_rat); w_put(&w, "\n");

	/* --- signal ----------------------------------------------------- */
	w_put(&w, "\n-- signal --\n");
	w_put(&w, "csq          : rssi %d ber %d\n", st->csq_rssi, st->csq_ber);
	/*
	 * The flag matters more than the number: with AT+GTCSQNREN=1 the +CSQ
	 * <rssi> is ss_rsrp and the -113+2n formula below is meaningless, so a
	 * report that showed only the dBm would be wrong by a fixed offset and
	 * look entirely reasonable.
	 */
	w_put(&w, "csq is ss_rsrp: %s\n", bool_str(st->csq_is_ss_rsrp));
	w_put(&w, "csq rssi dbm : %d dBm\n", -113 + 2 * st->csq_rssi);
	w_put(&w, "geran rssi   : "); w_int(&w, st->geran_rssi_dbm); w_put(&w, " dBm\n");
	w_put(&w, "utra rscp    : "); w_int(&w, st->utra_rscp_dbm); w_put(&w, " dBm\n");
	w_put(&w, "lte rsrp     : "); w_int(&w, st->lte_rsrp_dbm); w_put(&w, " dBm\n");
	w_put(&w, "lte rsrq     : "); w_int(&w, st->lte_rsrq_db10); w_put(&w, " (0.1 dB)\n");
	w_put(&w, "nr ss rsrp   : "); w_int(&w, st->nr_ss_rsrp_dbm); w_put(&w, " dBm\n");
	w_put(&w, "nr ss rsrq   : "); w_int(&w, st->nr_ss_rsrq_db10); w_put(&w, " (0.1 dB)\n");
	w_put(&w, "nr ss sinr   : "); w_int(&w, st->nr_ss_sinr_db10); w_put(&w, " (0.1 dB)\n");
	w_put(&w, "serving      : ");
	if (st->serving.valid) {
		w_put(&w, "rat %d plmn %d-%d band %d pci %d earfcn %lld",
		      st->serving.rat, st->serving.mcc, st->serving.mnc,
		      st->serving.band, st->serving.pci,
		      (long long)st->serving.earfcn);
	} else {
		w_put(&w, "none");
	}
	w_put(&w, "\n");
	w_put(&w, "neighbours   : %d\n", st->neigh_count);
	w_put(&w, "cell ok      : "); w_ago(&w, in->now_ms, st->cell_last_ok_ms);
	w_put(&w, "\n");

	/* --- link ------------------------------------------------------- */
	w_put(&w, "\n-- link --\n");
	w_put(&w, "netdev       : "); w_str(&w, st->netdev); w_put(&w, "\n");
	w_put(&w, "rx bytes     : %llu\n", U64(st->rx_bytes));
	w_put(&w, "tx bytes     : %llu\n", U64(st->tx_bytes));
	w_put(&w, "rate         : %llu / %llu bit/s\n",
	      U64(st->rx_bps), U64(st->tx_bps));

	/* --- health ----------------------------------------------------- */
	w_put(&w, "\n-- health --\n");
	w_put(&w, "at state     : %d\n", st->at_state);
	w_put(&w, "timeouts     : %d in a row\n", st->consec_timeout);
	w_put(&w, "last ok      : "); w_ago(&w, in->now_ms, st->last_ok_ms);
	w_put(&w, "\n");
	w_put(&w, "last fail    : "); w_ago(&w, in->now_ms, st->last_fail_ms);
	w_put(&w, "\n");
	w_put(&w, "worst resp   : %llu ms\n", U64(st->at_busy_max_ms));
	w_put(&w, "quiet        : ");
	if (st->quiet_kind == QUIET_NONE) {
		w_put(&w, "none\n");
	} else {
		/* The reason is kept as text because the kind alone cannot say
		 * which write was in flight when everything else went quiet. */
		w_put(&w, "kind %d for ", st->quiet_kind);
		if (st->quiet_until_ms > in->now_ms)
			w_put(&w, "%llu ms more",
			      U64(st->quiet_until_ms - in->now_ms));
		else
			w_put(&w, "no time left (expired?)");
		w_put(&w, ", reason ");
		w_str(&w, st->quiet_reason);
		w_put(&w, "\n");
	}
	w_put(&w, "queue depth  : %d\n", in->atq_depth);

	/* --- M2: the data plane ----------------------------------------- */
	w_put(&w, "\n-- M2 dial --\n");
	w_put(&w, "wanted       : %s\n", bool_str(st->dial.wanted));
	w_put(&w, "step         : %d", (int)st->dial.step);
	{
		const char *n = fm160_net_step_name(st->dial.step);

		if (n)
			w_put(&w, " (%s)", n);
	}
	w_put(&w, "\n");
	w_put(&w, "kind         : %d, known %s\n",
	      (int)st->dial.kind, bool_str(st->dial.kind_known));
	w_put(&w, "context      : cid %d pdp %d, apn ", st->dial.cid,
	      (int)st->dial.pdp);
	w_str(&w, st->dial.apn);
	w_put(&w, "\n");
	w_put(&w, "up           : %s\n", bool_str(st->dial.up));
	w_put(&w, "address      : "); w_str(&w, st->dial.addr); w_put(&w, "\n");
	w_put(&w, "dns          : "); w_str(&w, st->dial.dns1); w_put(&w, " / ");
	w_str(&w, st->dial.dns2); w_put(&w, "\n");
	/*
	 * The verb is a fact about the unit, not a constant: the vendor's two
	 * documents disagree about which of AT+GTWWAN / AT+GTRNDIS an ECM
	 * interface answers (AT-FACTS 2.1), and the daemon probes it once.
	 * Whether it has been probed is part of the answer, so -1 is printed by
	 * name rather than as a number that looks like a verb.
	 */
	w_put(&w, "verb         : ");
	if (st->dial.verb < 0)
		w_put(&w, "not probed yet");
	else {
		const char *n = fm160_net_verb_name((enum fm160_net_verb)st->dial.verb);

		w_put(&w, "%d", st->dial.verb);
		if (n)
			w_put(&w, " (%s)", n);
	}
	w_put(&w, "\n");
	w_put(&w, "attempt      : %d, sub %d, ip polls %d\n",
	      st->dial.attempt, st->dial.sub, st->dial.ip_polls);
	w_put(&w, "sim ready    : %s, was up %s\n",
	      bool_str(st->dial.sim_ready), bool_str(st->dial.was_up));
	w_put(&w, "resets       : %d in window, %d total%s\n",
	      st->dial.resets_in_window, st->dial.resets_total,
	      st->dial.healing_stopped ? ", healing STOPPED" : "");
	w_put(&w, "last ok      : "); w_ago(&w, in->now_ms, st->dial.last_ok_ms);
	w_put(&w, "\n");
	w_put(&w, "last error   : "); w_str(&w, st->dial.last_error); w_put(&w, "\n");

	/* --- M2: the USB profile switch --------------------------------- */
	w_put(&w, "\n-- M2 usb profile --\n");
	w_put(&w, "state        : %d\n", (int)st->modesw.st);
	w_put(&w, "current      : "); w_int(&w, st->modesw.current); w_put(&w, "\n");
	w_put(&w, "target       : "); w_int(&w, st->modesw.target); w_put(&w, "\n");
	w_put(&w, "rollback     : "); w_int(&w, st->modesw.rollback_mode);
	w_put(&w, "\n");
	w_put(&w, "caps         : %s, %d modes\n",
	      st->modesw.caps_valid ? "valid" : "none", st->modesw.supported_n);
	/*
	 * A pending switch is the one state in this bundle that means the
	 * management channel may be about to disappear: the modem is
	 * re-enumerating and the AT port we are talking to may not come back.
	 * How long it has been pending is the whole diagnosis.
	 */
	w_put(&w, "pending      : ");
	if (!st->modesw.pending)
		w_put(&w, "no\n");
	else {
		w_put(&w, "yes for ");
		w_ago(&w, in->now_ms, st->modesw.pending_since_ms);
		w_put(&w, ", verify %d, rolled back %s\n",
		      st->modesw.verify_result,
		      bool_str(st->modesw.rolled_back));
	}
	w_put(&w, "last error   : "); w_str(&w, st->modesw.last_error); w_put(&w, "\n");

	/* --- M4 --------------------------------------------------------- */
	w_put(&w, "\n-- M4 bands and locking --\n");
	w_put(&w, "bands        : %s\n", st->gtact.valid ? "valid" : "none");
	w_put(&w, "bands umts/lte/nr : %d / %d / %d\n",
	      st->gtact.umts_n, st->gtact.lte_n, st->gtact.nr_n);
	w_put(&w, "bands auto seen   : %s\n", bool_str(st->gtact.auto_seen));
	/* Tokens that fit no documented encoding.  Counted, never guessed: a
	 * non-zero value here is the difference between "the modem does not
	 * support that band" and "we do not understand that band". */
	w_put(&w, "bands unknown     : %d\n", st->gtact.unknown_n);
	w_put(&w, "cell lock    : %s, %s, rat %d type %d\n",
	      st->celllock.valid ? "valid" : "none",
	      st->celllock.enabled ? "enabled" : "disabled",
	      st->celllock.rat, st->celllock.type);
	w_put(&w, "carrier agg  : %s, rat %d, nr %s, %d scell\n",
	      st->ca.valid ? "valid" : "none", st->ca.rat,
	      bool_str(st->ca.has_nr), st->ca.scc_n);

	/* --- M5 --------------------------------------------------------- */
	w_put(&w, "\n-- M5 gnss --\n");
	w_put(&w, "engine       : known %s, %s\n",
	      bool_str(st->gnss.engine.known), st->gnss.engine.on ? "on" : "off");
	w_put(&w, "autostart    : %s, done %s\n",
	      bool_str(st->gnss.autostart), bool_str(st->gnss.autostart_done));
	/*
	 * fix_type is in the sentinel domain even though its meaningful values
	 * are only 1/2/3: fm160_state_init() seeds it with FM160_NONE and it
	 * stays that way until the first GSA arrives, which on a bench board
	 * with the engine off is forever.  Printed with a plain %d it leaked
	 * "fix type     : -1000000" into the bundle -- the exact "reads as a
	 * measurement" failure this formatter exists to prevent, and the only
	 * such leak in the whole report.
	 *
	 * sats_used is only ever incremented, never seeded, so it cannot hold
	 * the sentinel and %d is right for it.
	 */
	w_put(&w, "fix type     : "); w_int(&w, st->gnss.r.fix_type);
	w_put(&w, ", sats used %d\n", st->gnss.r.sats_used);
	w_put(&w, "position     : %s\n", bool_str(st->gnss.r.has_position));
	/*
	 * These two are the reason a bench receiver reports no fix, and they
	 * look nothing alike in a bug report: "empty frames" climbing means the
	 * engine is on and has no sky, while "read error" means the engine is
	 * off and AT+GTGPS is answering ERROR.  Neither is visible from the
	 * page, which only ever shows the last successful block.
	 */
	w_put(&w, "empty frames : %d in a row\n", st->gnss.r.empty_frames);
	w_put(&w, "read error   : %s\n", bool_str(st->gnss.r.read_error));
	w_put(&w, "sentences    : %d, bad checksum %d, bad shape %d\n",
	      st->gnss.r.sentences, st->gnss.r.bad_checksum, st->gnss.r.bad_shape);
	w_put(&w, "last read    : "); w_ago(&w, in->now_ms, st->gnss.r.read_ms);
	w_put(&w, "\n");

	/* --- M3 --------------------------------------------------------- */
	w_put(&w, "\n-- M3 sms --\n");
	w_put(&w, "cgmf         : "); w_int(&w, st->sms.cmgf);
	w_put(&w, " (0 PDU, 1 text)\n");
	w_put(&w, "stored       : %d of %d kept\n", st->sms.count, FM160_SMS_KEEP);
	w_put(&w, "setup done   : %s, running %s\n",
	      bool_str(st->sms.setup_done), bool_str(st->sms.setup_running));
	w_put(&w, "busy         : %s\n", bool_str(st->sms.busy));
	/* The last send is kept verbatim in the daemon (sms.last_pdu) but only
	 * its length is reported here: the bundle is text a person pastes into
	 * a public issue, and an unambiguous PDU is a decodable phone number
	 * and message body.  The page shows the full thing to its owner. */
	w_put(&w, "last send    : %s", st->sms.last_send_cmd[0] ? "yes" : "none");
	if (st->sms.last_send_cmd[0])
		w_put(&w, ", %d segment(s), pdu %u chars",
		      st->sms.last_segments, (unsigned)strlen(st->sms.last_pdu));
	w_put(&w, "\n");

	/* --- the log ---------------------------------------------------- */
	w_put(&w, "\n-- log (%u lines, oldest first%s) --\n", (unsigned)in->log_n,
	      dropped ? ", older lines dropped" : "");
	if (in->log_n == 0) {
		w_put(&w, "(nothing logged at this level)\n");
	} else {
		for (i = 0; i < in->log_n; i++) {
			const struct fm160_log_rec *r = in->log[i];
			uint64_t a = age_of(in->now_ms, r->at_ms);

			w_put(&w, "%10llu.%03llu %c %s%s\n",
			      U64(a / 1000), U64(a % 1000), prio_char(r->prio),
			      r->msg, r->truncated ? " [TRUNCATED]" : "");
		}
	}

	if (w.over)
		return -1;

	buf[w.len] = '\0';
	return (long)w.len;
}
