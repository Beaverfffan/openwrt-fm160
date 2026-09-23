#!/bin/sh
# Host-side test for the M6 diagnostics bundle (diag.c), run ON A LINUX HOST.
#
#   sh tools/hosttest/diag-export-test.sh          # from the checkout root
#   CC=gcc sh tools/hosttest/diag-export-test.sh   # explicit compiler
#
# It is also runnable against a build tree, which is where it started:
#
#   SRCDIR=$TREE/package/fm160d/src WORK=/tmp/diag \
#     sh tools/hosttest/diag-export-test.sh
#
# It lives in the repository rather than on the bench, unlike 23-sms-pdu-test.sh
# and 25-usbmode-dial-test.sh, because everything it needs is IN the repository:
# the source it compiles, the vendored libubox headers under tools/cccheck, and
# a C compiler that every CI runner has.  A gate that cannot be run by whoever
# clones the tree is a gate that only its author benefits from.
#
# Why this one needs a Linux host and not the workstation
# -------------------------------------------------------
# 23-sms-pdu-test.sh and 25-usbmode-dial-test.sh compile pdu.c / usbmode.c /
# net.c, which are libc-only: `zig cc` on Windows can build and run them
# directly.  diag.c is a different case, and deliberately so - it prints struct
# fm160_state field by field, so it includes fm160d.h, which includes the real
# libubox headers.  Those do not compile for a Windows target (uloop.h wants
# struct sigaction, utils.h wants <machine/endian.h>), and the alternative -
# restating the state as a private struct in diag.c - would mean the formatter
# asserted against a copy of the fields rather than against the fields, which is
# the one thing this test exists to avoid.
#
# So: compile with a real cc, where the same headers are ordinary Linux headers.
# On the workstation that means a Linux host is needed; `zig cc -target
# x86_64-linux-musl` type-checks the same code but produces a binary the
# workstation cannot execute, which is why tools/check.sh reports this gate as
# SKIPPED there instead of failing it.
#
# Why it exists
# -------------
# The bundle is text that a person reads once, under stress, when something has
# already gone wrong.  Every failure mode here is quiet:
#
#   - a missing "never" turns "no reset has ever been attempted" into "0
#     resets", which reads as a working feature;
#   - a value that is FM160_NONE printed as -1000000 instead of "-" reads as a
#     measurement;
#   - a buffer that was one byte too small truncates the report mid-sentence,
#     and nothing in the file says so;
#   - a log tail in the wrong order makes a sequence of events read backwards.
#
# None of those is visible from the daemon, from the ubus reply or from the page,
# and all of them are decidable without a modem.
#
# What it does NOT prove: that the device produces this input.  The state has to
# be filled by the daemon before the formatter sees it, and that fill is checked
# by tools/cccheck (it compiles the real fields, so a renamed field is a compile
# error) and by the device-side call in 15-verify-device.sh - not here.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

TREE=${TREE:-/mnt/data4t/istoreos-h69k/src}
SRCDIR=${SRCDIR:-$ROOT/fm160d/src}
LIBUBOX=${LIBUBOX:-$ROOT/tools/cccheck/include}
WORK=${WORK:-$ROOT/_tmp/hosttest/diag}
CC=${CC:-cc}
PY=${PY:-python3}

if [ ! -f "$SRCDIR/diag.c" ] && [ -f "$TREE/package/fm160d/src/diag.c" ]; then
	SRCDIR=$TREE/package/fm160d/src
fi

for f in "$SRCDIR/diag.c" "$SRCDIR/diag.h" "$SRCDIR/fm160d.h" \
         "$SRCDIR/net.c" "$SRCDIR/net.h" "$SRCDIR/usbmode.h" "$SRCDIR/pdu.h"; do
	[ -f "$f" ] || { echo "FATAL: $f not found" >&2
			 echo "       pass SRCDIR=/path/to/fm160d/src" >&2; exit 1; }
done
if [ ! -f "$LIBUBOX/libubox/blobmsg.h" ]; then
	echo "FATAL: the libubox headers are not in $LIBUBOX" >&2
	echo "       pass LIBUBOX=/path/to/openwrt-fm160/tools/cccheck/include" >&2
	exit 1
fi

command -v "$CC" >/dev/null 2>&1 || { echo "FATAL: no $CC" >&2; exit 1; }

# The removals go through tools/lib/tooling.sh: this checkout is edited on a
# workstation whose sandbox intercepts `rm` and kills the whole script with
# SIGTERM, which reads as a logic error rather than as a policy.
# shellcheck source=../lib/tooling.sh
. "$ROOT/tools/lib/tooling.sh"

rmrf "$WORK" || exit 1
mkdir -p "$WORK"

# --- 1. the C test ---------------------------------------------------------
cat > "$WORK/test.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "fm160d.h"

static int checks, failures;

static void ok(int cond, const char *what)
{
	checks++;
	if (cond) {
		printf("  ok   %s\n", what);
	} else {
		failures++;
		printf("  FAIL %s\n", what);
	}
}

static void eqi(long got, long want, const char *what)
{
	checks++;
	if (got == want) {
		printf("  ok   %s (= %ld)\n", what, got);
	} else {
		failures++;
		printf("  FAIL %s: got %ld, want %ld\n", what, got, want);
	}
}

/* The line of the report whose key is `key`, or NULL.  The key field is padded
 * on the left, so "port         :" and "csq is ss_rsrp:" are both found by
 * looking for the key at the start of a line and reading to the colon. */
static const char *line_of(const char *text, const char *key)
{
	size_t klen = strlen(key);
	const char *p = text;

	if (!*p)
		return NULL;
	for (;;) {
		if (!strncmp(p, key, klen)) {
			const char *c = p + klen;

			while (*c == ' ')
				c++;
			if (*c == ':')
				return p;
		}
		p = strchr(p, '\n');
		if (!p)
			return NULL;
		p++;
	}
}

/* The value of `key`, i.e. everything after the ": ".  Static buffer: the
 * caller prints it immediately and never holds two. */
static const char *value_of(const char *text, const char *key)
{
	static char buf[512];
	const char *l = line_of(text, key);
	const char *c, *e;
	size_t n;

	if (!l)
		return NULL;
	c = strchr(l, ':');
	if (!c)
		return NULL;
	c += 1;
	while (*c == ' ')
		c++;
	e = strchr(c, '\n');
	if (!e)
		e = c + strlen(c);
	n = (size_t)(e - c);
	if (n >= sizeof(buf))
		n = sizeof(buf) - 1;
	memcpy(buf, c, n);
	buf[n] = '\0';
	return buf;
}

static void eqv(const char *text, const char *key, const char *want,
		const char *what)
{
	const char *got = value_of(text, key);

	checks++;
	if (got && !strcmp(got, want)) {
		printf("  ok   %s\n", what);
	} else {
		failures++;
		printf("  FAIL %s:\n         %s = '%s'\n         want   '%s'\n",
		       what, key, got ? got : "(no such line)",
		       want);
	}
}

static int has(const char *text, const char *needle)
{
	return strstr(text, needle) != NULL;
}

/* --- the state the bundle is built from ------------------------------ */

static struct fm160_state st;

/*
 * A state that is NOT the all-zero default.  A formatter that reads the wrong
 * field, or that prints a constant, passes against a zeroed struct; it does not
 * pass against this one, because every number here is distinguishable from
 * every other number in the file.
 */
static void fill_state(void)
{
	memset(&st, 0, sizeof(st));

	strcpy(st.port, "/dev/ttyUSB2");
	st.port_found = true;
	st.cand_count = 3;

	strcpy(st.manufacturer, "Fibocom");
	strcpy(st.model, "FM160-CN");
	strcpy(st.revision, "89614.1000.00.04.01.02");
	strcpy(st.imei, "111111111111111");
	strcpy(st.sn, "SNSNSNSN");
	strcpy(st.iccid, "89860111111111111111");
	st.usb_mode = 17;
	st.ident_done = true;

	strcpy(st.pin_status, "READY");
	st.creg = 1; st.cgreg = 1; st.cereg = 5; st.c5greg = FM160_NONE;
	strcpy(st.oper, "CHN-UNICOM");
	st.act_rat = 7;

	st.csq_rssi = 20; st.csq_ber = 99;
	st.csq_is_ss_rsrp = false;
	st.lte_rsrp_dbm = -95;
	st.lte_rsrq_db10 = -115;
	st.nr_ss_rsrp_dbm = FM160_NONE;
	st.serving.valid = true;
	st.serving.rat = 4;
	st.serving.mcc = 460; st.serving.mnc = 1;
	st.serving.band = 3; st.serving.pci = 321;
	st.serving.earfcn = 1650;
	st.neigh_count = 4;
	st.cell_last_ok_ms = 7000;

	strcpy(st.netdev, "wwan0");
	st.rx_bytes = 123456; st.tx_bytes = 654321;
	st.rx_bps = 4321; st.tx_bps = 1234;

	st.at_state = 0; st.consec_timeout = 0;
	st.last_ok_ms = 5000; st.last_fail_ms = 0;
	st.at_busy_max_ms = 12400;
	st.quiet_kind = 0;                   /* QUIET_NONE */
	st.foreground = false;
	st.last_active_ms = 4000;
	st.enabled = true;

	st.dial.wanted = true;
	st.dial.step = NET_STEP_IP;
	st.dial.kind = 2;
	st.dial.kind_known = true;
	st.dial.cid = 1;
	st.dial.pdp = NET_PDP_IPV4V6;
	strcpy(st.dial.apn, "cmnet");
	st.dial.up = false;
	st.dial.verb = NET_VERB_GTWWAN;
	st.dial.attempt = 2;
	st.dial.sub = 1;
	st.dial.ip_polls = 3;
	st.dial.sim_ready = true;
	st.dial.was_up = true;
	st.dial.resets_in_window = 1;
	st.dial.resets_total = 4;
	st.dial.healing_stopped = true;
	st.dial.last_ok_ms = 6000;
	strcpy(st.dial.last_error, "AT+GTWWAN=1,1 -> ERROR");

	st.modesw.st = 7;
	st.modesw.current = 17;
	st.modesw.target = -1;
	st.modesw.rollback_mode = 18;
	st.modesw.caps_valid = true;
	st.modesw.supported_n = 9;
	st.modesw.pending = true;
	st.modesw.pending_since_ms = 8000;
	st.modesw.verify_result = 0;

	st.gtact.valid = true;
	st.gtact.umts_n = 0; st.gtact.lte_n = 3; st.gtact.nr_n = 2;
	st.gtact.unknown_n = 1;
	st.gtact.auto_seen = true;

	st.celllock.valid = true;
	st.celllock.enabled = false;
	st.celllock.rat = 1;
	st.celllock.type = 1;

	st.ca.valid = true;
	st.ca.rat = 4;
	st.ca.has_nr = true;
	st.ca.scc_n = 2;

	st.gnss.engine.known = true;
	st.gnss.engine.on = false;
	st.gnss.autostart = true;
	st.gnss.autostart_done = true;
	st.gnss.r.fix_type = 1;
	st.gnss.r.sats_used = 0;
	st.gnss.r.has_position = false;
	st.gnss.r.empty_frames = 31;
	st.gnss.r.read_error = true;
	st.gnss.r.sentences = 0;
	st.gnss.r.bad_checksum = 2;
	st.gnss.r.bad_shape = 1;
	st.gnss.r.read_ms = 9000;

	st.sms.cmgf = -1;
	st.sms.count = 3;
	st.sms.setup_done = false;
	st.sms.setup_running = true;
	st.sms.busy = false;
	strcpy(st.sms.last_send_cmd, "AT+CMGS=34");
	st.sms.last_segments = 1;
	memset(st.sms.last_pdu, 'A', 68);
	st.sms.last_pdu[68] = '\0';
}

/*
 * Every string field at its maximum length.  The worst case for the buffer is
 * not "the longest number" - it is a daemon whose operator name, ICCID, APN and
 * last error are all at the limit at once, which is what a device in trouble
 * looks like.  An APN is user input and the errors are the modem's own words,
 * so none of these lengths is under our control.
 */
static void pad(char *dst, size_t n)
{
	if (n == 0)
		return;
	memset(dst, 'z', n - 1);
	dst[n - 1] = '\0';
}

static void fill_state_long(void)
{
	fill_state();
	pad(st.port, sizeof(st.port));
	pad(st.manufacturer, sizeof(st.manufacturer));
	pad(st.model, sizeof(st.model));
	pad(st.revision, sizeof(st.revision));
	pad(st.imei, sizeof(st.imei));
	pad(st.sn, sizeof(st.sn));
	pad(st.iccid, sizeof(st.iccid));
	pad(st.pin_status, sizeof(st.pin_status));
	pad(st.oper, sizeof(st.oper));
	pad(st.quiet_reason, sizeof(st.quiet_reason));
	pad(st.netdev, sizeof(st.netdev));
	pad(st.dial.apn, sizeof(st.dial.apn));
	pad(st.dial.addr, sizeof(st.dial.addr));
	pad(st.dial.dns1, sizeof(st.dial.dns1));
	pad(st.dial.dns2, sizeof(st.dial.dns2));
	pad(st.dial.last_error, sizeof(st.dial.last_error));
	pad(st.modesw.last_error, sizeof(st.modesw.last_error));
	st.quiet_kind = 6;   /* QUIET_GNSS, so the reason is printed */
}

/* --- the log ring the bundle is built from --------------------------- */

#define TEST_LOG_MAX 8
static struct fm160_log_rec recs[TEST_LOG_MAX];
static const struct fm160_log_rec *ptrs[TEST_LOG_MAX];

static void log_rec(int i, uint64_t at_ms, int prio, int truncated,
		    const char *msg)
{
	memset(&recs[i], 0, sizeof(recs[i]));
	recs[i].at_ms = at_ms;
	recs[i].prio = prio;
	recs[i].truncated = truncated != 0;
	snprintf(recs[i].msg, sizeof(recs[i].msg), "%s", msg);
	ptrs[i] = &recs[i];
}

static struct fm160_diag_in in;

static void base_input(void)
{
	memset(&in, 0, sizeof(in));
	in.st = &st;
	in.log = ptrs;
	in.log_n = 0;
	in.log_total = 0;
	in.atq_depth = 2;
	in.now_ms = 10000;
	in.uptime_ms = 10000;
	in.wall = "2026-09-19 06:40:12 +0800";
	in.log_level = 6;
	in.version = "0.1.0";
}

/*
 * The bundle buffer is sized by the header, so the test allocates exactly what
 * the daemon allocates.  Anything that fits here fits on the device.
 */
static char buf[FM160_DIAG_BUF_MAX];

int main(void)
{
	long len, len2;
	char *big;
	size_t i;
	const char *p;

	printf("== the shape of the report ==\n");

	fill_state();
	base_input();
	len = fm160_diag_format(&in, buf, sizeof(buf));
	ok(len > 0, "a filled state formats without asking for a bigger buffer");
	if (len <= 0)
		return 1;

	ok(!strncmp(buf, "fm160d diagnostics\n==================\n",
		    strlen("fm160d diagnostics\n")),
	   "the report opens with its own name and an underline");

	/* Every line is either a section header, a `key  : value` line or a log
	 * line.  This is what caught a section whose heading had drifted out of
	 * the form the rest of the file uses. */
	{
		int bad = 0, keys = 0, sections = 0, logs = 0, title = 0;

		p = buf;
		while (*p) {
			const char *e = strchr(p, '\n');
			size_t n = e ? (size_t)(e - p) : strlen(p);

			if (n == 0) {
				/* blank separator */
			} else if (!strncmp(p, "fm160d diagnostics", 18) ||
				   !strncmp(p, "==================", 18)) {
				title++;
			} else if (n > 4 && !strncmp(p, "-- ", 3) &&
				   !strncmp(p + n - 3, " --", 3)) {
				sections++;
			} else if (!strncmp(p, "(nothing logged at this level)", 29)) {
				/* The one line in the report that is prose: an empty
				 * log explains itself rather than printing nothing. */
			} else if (n > 5 && p[0] == ' ' &&
				   strchr("0123456789", p[1]) &&
				   strstr(p, " E ") != NULL) {
				logs++;
			} else {
				/* key : value -- the key is lowercase, may contain
				 * spaces or a slash, and is followed by padding and a
				 * colon.  Nothing else is allowed on a line. */
				const char *c = memchr(p, ':', n);
				size_t j;
				int okkey = 0;

				if (c) {
					okkey = 1;
					for (j = 0; j < (size_t)(c - p); j++) {
						char ch = p[j];

						if (ch >= 'a' && ch <= 'z')
							continue;
						if (ch >= '0' && ch <= '9')
							continue;
						if (ch == ' ' || ch == '_' ||
						    ch == '/')
							continue;
						okkey = 0;
						break;
					}
					if (okkey && (size_t)(c - p) + 2 > n)
						okkey = 0;
					if (okkey && (c[1] != ' '))
						okkey = 0;
				}
				if (okkey)
					keys++;
				else {
					if (bad < 3) {
						printf("       unclassified: '");
						fwrite(p, 1, n, stdout);
						printf("'\n");
					}
					bad++;
				}
			}
			if (!e)
				break;
			p = e + 1;
		}
		eqi(bad, 0, "every line is a key, a section header or a log line");
		ok(title == 2, "the two title lines are present");
		ok(sections >= 10, "the section headers are present");
		ok(keys >= 50, "the key lines are present");
		eqi(logs, 0, "no log lines when there is no log");
	}

	/* --- the header ------------------------------------------------- */
	printf("\n== the header ==\n");
	eqv(buf, "version", "0.1.0", "the caller's version is reported");
	eqv(buf, "generated", "2026-09-19 06:40:12 +0800",
	    "the caller's wall clock is reported verbatim");
	eqv(buf, "uptime", "10.000 s", "the uptime is the caller's, in seconds");
	eqv(buf, "log level", "6", "the effective log level is reported");
	eqv(buf, "management", "enabled", "the enable flag is reported");
	eqv(buf, "queue depth", "2", "the AT queue depth is reported");

	printf("\n== a value that is not the default ==\n");
	eqv(buf, "port", "/dev/ttyUSB2", "the port name");
	eqv(buf, "candidates", "3", "the candidate count");
	eqv(buf, "model", "FM160-CN", "the model");
	eqv(buf, "usb mode", "17", "the raw USB mode");
	eqv(buf, "imei", "111111111111111", "the IMEI");
	eqv(buf, "pin", "READY", "the PIN state");
	eqv(buf, "cereg", "5", "cereg is independent of creg");
	eqv(buf, "operator", "CHN-UNICOM", "the operator name");
	eqv(buf, "csq", "rssi 20 ber 99", "the raw CSQ pair");
	eqv(buf, "csq rssi dbm", "-73 dBm", "the decoded CSQ rssi");
	eqv(buf, "lte rsrp", "-95 dBm", "the decoded LTE RSRP");
	eqv(buf, "lte rsrq", "-115 (0.1 dB)", "the decoded LTE RSRQ");
	eqv(buf, "serving", "rat 4 plmn 460-1 band 3 pci 321 earfcn 1650",
	    "the serving cell identity");
	eqv(buf, "neighbours", "4", "the neighbour count");
	eqv(buf, "netdev", "wwan0", "the netdev name");
	eqv(buf, "rate", "4321 / 1234 bit/s", "the rate pair");
	eqv(buf, "worst resp", "12400 ms", "the worst response time");
	eqv(buf, "wanted", "yes", "the dial want flag");
	eqv(buf, "step", "5 (waiting for an address)",
	    "the dial step, named by net.c");
	eqv(buf, "context", "cid 1 pdp 2, apn cmnet", "the dial context");
	eqv(buf, "verb", "0 (GTWWAN)", "the probed verb, named by net.c");
	eqv(buf, "resets", "1 in window, 4 total, healing STOPPED",
	    "the reset ledger");
	eqv(buf, "last error", "AT+GTWWAN=1,1 -> ERROR", "the dial error text");
	eqv(buf, "state", "7", "the USB profile state");
	eqv(buf, "rollback", "18", "the rollback point");
	eqv(buf, "bands umts/lte/nr", "0 / 3 / 2", "the per-RAT band counts");
	eqv(buf, "bands unknown", "1", "the undecodable band count");
	eqv(buf, "cell lock", "valid, disabled, rat 1 type 1", "the cell lock");
	eqv(buf, "carrier agg", "valid, rat 4, nr yes, 2 scell",
	    "the carrier aggregation");
	eqv(buf, "engine", "known yes, off", "the GNSS engine");
	eqv(buf, "empty frames", "31 in a row", "the empty-frame run");
	eqv(buf, "read error", "yes", "the GNSS read error flag");
	eqv(buf, "sentences", "0, bad checksum 2, bad shape 1",
	    "the NMEA counters");
	eqv(buf, "stored", "3 of 64 kept", "the SMS store");
	eqv(buf, "last send", "yes, 1 segment(s), pdu 68 chars",
	    "the last send, described rather than quoted");

	printf("\n== what a counter means when it has never happened ==\n");
	eqv(buf, "c5greg", "-", "an unset registration state is a dash");
	eqv(buf, "last fail", "never", "a failure that never happened");
	eqv(buf, "nr ss rsrp", "- dBm",
	    "a signal field that does not apply keeps its unit");
	ok(!has(buf, "-1000000"), "the FM160_NONE sentinel never reaches the page");
	eqv(buf, "dns", "- / -", "an empty pair of strings is a dash each");

	/*
	 * --- the state before anything has been read ---------------------
	 *
	 * The assertion just above passes trivially.  fill_state() models a modem
	 * that has ANSWERED, so nothing it fills is in the sentinel domain and
	 * there is no sentinel for a formatter to mishandle.  The state that
	 * actually ships on a board with the engine off is the one
	 * fm160_state_init() builds, where a dozen GNSS fields are seeded with
	 * FM160_NONE and keep it until the first GSA/GSV arrives.
	 *
	 * That is not hypothetical.  fix_type was printed with a plain %d, so a
	 * real bundle carried "fix type     : -1000000, sats used 0" and the
	 * first thing to notice was a device-side grep, on the board, after a
	 * flash.  This fixture makes the same mistake land here instead.
	 *
	 * Every field fm160_state_init() seeds is set below, so the two
	 * assertions are general rather than a list of the leaks known today:
	 * whichever one of them forgets w_int() shows up, including a field not
	 * yet printed at all.
	 */
	printf("\n== the state before anything has been read ==\n");
	base_input();
	fill_state();
	st.gnss.r.fix_type = FM160_NONE;
	st.gnss.r.visible_total = FM160_NONE;
	st.gnss.r.snr_best_db = FM160_NONE;
	st.gnss.r.pdop_x10 = FM160_NONE;
	st.gnss.r.hdop_x10 = FM160_NONE;
	st.gnss.r.vdop_x10 = FM160_NONE;
	st.gnss.r.quality = FM160_NONE;
	st.gnss.r.sats_in_use = FM160_NONE;
	st.gnss.r.alt_dm = FM160_NONE;
	st.gnss.r.geoid_dm = FM160_NONE;
	st.gnss.r.speed_cmps = FM160_NONE;
	st.gnss.r.course_d10 = FM160_NONE;
	st.gnss.r.gsv_trailing_value = FM160_NONE;
	st.gnss.cfg.supl_version = FM160_NONE;
	st.gnss.cfg.constellation = FM160_NONE;
	st.gnss.cfg.cert = FM160_NONE;
	st.gnss.cfg.xtra = FM160_NONE;
	len2 = fm160_diag_format(&in, buf, sizeof(buf));
	ok(len2 > 0, "the pre-read state formats");
	ok(!has(buf, "-1000000"), "no seeded sentinel reaches the reader");
	eqv(buf, "fix type", "-, sats used 0",
	    "a fix type no GSA has set is a dash, not -1000000");
	/* And the zero it would have printed instead is still distinct: a GSA
	 * that really said "no fix" is 1, and 1 must not render as a dash. */
	st.gnss.r.fix_type = 1;
	len2 = fm160_diag_format(&in, buf, sizeof(buf));
	eqv(buf, "fix type", "1, sats used 0",
	    "a measured fix type is not mistaken for 'not reported'");

	/* Zero and "never" are different answers and must not print alike. */
	base_input();
	in.now_ms = 0;
	st.last_fail_ms = 0;
	st.last_ok_ms = 0;
	len2 = fm160_diag_format(&in, buf, sizeof(buf));
	ok(len2 > 0, "a state with no timestamps still formats");
	eqv(buf, "last ok", "never", "a zero timestamp is never, not 0.000 s ago");
	in.now_ms = 10000;
	st.last_ok_ms = 10000;
	len2 = fm160_diag_format(&in, buf, sizeof(buf));
	eqv(buf, "last ok", "0.000 s ago", "a timestamp of now is not never");

	/* --- the quiet window ------------------------------------------- */
	printf("\n== the quiet window ==\n");
	fill_state();
	base_input();
	st.quiet_kind = 0;
	fm160_diag_format(&in, buf, sizeof(buf));
	eqv(buf, "quiet", "none", "no quiet window");

	st.quiet_kind = 6;   /* QUIET_GNSS */
	strcpy(st.quiet_reason, "AT+GTGPS=");
	st.quiet_until_ms = 12500;   /* 2.5 s after now */
	fm160_diag_format(&in, buf, sizeof(buf));
	ok(has(buf, "kind 6 for 2500 ms more, reason AT+GTGPS="),
	   "an open quiet window reports its kind, remainder and reason");

	/* An expired window is a different bug from no window: it says the
	 * daemon believes it is still quiet and the clock says otherwise. */
	st.quiet_until_ms = 9000;
	fm160_diag_format(&in, buf, sizeof(buf));
	ok(has(buf, "no time left (expired?)"),
	   "a window that has run out says so rather than printing a negative");

	/* --- the pending profile switch --------------------------------- */
	printf("\n== a profile switch that has not come back ==\n");
	fill_state();
	base_input();
	fm160_diag_format(&in, buf, sizeof(buf));
	ok(has(buf, "pending      : yes for 2.000 s ago, verify 0, rolled back no"),
	   "a pending switch reports how long it has been pending");

	st.modesw.pending = false;
	fm160_diag_format(&in, buf, sizeof(buf));
	eqv(buf, "pending", "no", "no pending switch is one word");

	/* --- the log tail ----------------------------------------------- */
	printf("\n== the log tail ==\n");
	fill_state();
	base_input();
	log_rec(0, 1000, 6, 0, "fm160d starting");
	log_rec(1, 4000, 4, 0, "port disappeared");
	log_rec(2, 7000, 3, 0, "at-daemon is gone");
	log_rec(3, 8000, 7, 1, "a line that did not fit in the record");
	in.log_n = 4;
	in.log_total = 4;
	len2 = fm160_diag_format(&in, buf, sizeof(buf));
	ok(len2 > 0, "a report with a log tail formats");

	ok(has(buf, "-- log (4 lines, oldest first) --"),
	   "the log section names its size and order");
	ok(!has(buf, "older lines dropped"),
	   "nothing is called dropped when nothing was");

	/* Oldest first, and each line carries its age relative to now_ms.  The
	 * age field is right-aligned in ten columns, so the expected strings
	 * start at the number rather than reproducing the padding. */
	{
		static const char *want[] = {
			"9.000 I fm160d starting",
			"6.000 W port disappeared",
			"3.000 E at-daemon is gone",
			"2.000 D a line that did not fit in the record [TRUNCATED]"
		};
		for (i = 0; i < 4; i++)
			ok(has(buf, want[i]), want[i]);
	}

	/* The priority letter: one letter per syslog level, in order. */
	log_rec(0, 10000, 3, 0, "e");
	log_rec(1, 10000, 4, 0, "w");
	log_rec(2, 10000, 5, 0, "n");
	log_rec(3, 10000, 6, 0, "i");
	log_rec(4, 10000, 7, 0, "d");
	in.log_n = 5;
	in.log_total = 5;
	fm160_diag_format(&in, buf, sizeof(buf));
	ok(has(buf, "0.000 E e"), "LOG_ERR is E");
	ok(has(buf, "0.000 W w"), "LOG_WARNING is W");
	ok(has(buf, "0.000 N n"), "LOG_NOTICE is N");
	ok(has(buf, "0.000 I i"), "LOG_INFO is I");
	ok(has(buf, "0.000 D d"), "LOG_DEBUG is D");

	/* A line stamped after now_ms must not become a huge age. */
	log_rec(0, 999999, 6, 0, "from the future");
	in.log_n = 1;
	in.log_total = 1;
	fm160_diag_format(&in, buf, sizeof(buf));
	ok(has(buf, "0.000 I from the future"),
	   "a line stamped after the snapshot ages to 0, not to a huge number");

	/* A ring that has wrapped says so. */
	log_rec(0, 9000, 6, 0, "only the last line");
	in.log_n = 1;
	in.log_total = 4000;
	fm160_diag_format(&in, buf, sizeof(buf));
	ok(has(buf, "1 of 128 lines kept, 3999 dropped"),
	   "the header says how many lines the ring dropped");
	ok(has(buf, "-- log (1 lines, oldest first, older lines dropped) --"),
	   "the log section repeats that older lines were dropped");

	printf("\n== an empty log ==\n");
	base_input();
	in.log_n = 0;
	in.log_total = 0;
	fm160_diag_format(&in, buf, sizeof(buf));
	ok(has(buf, "(nothing logged at this level)"),
	   "an empty log explains itself instead of printing nothing");

	/* --- the buffer ------------------------------------------------- */
	printf("\n== the buffer ==\n");

	/* The worst case: every ring slot used, every line at the record limit,
	 * every line marked truncated.  If this does not fit, the daemon's own
	 * buffer does not either and every report from a busy device is a
	 * refusal. */
	{
		struct fm160_log_rec *worst = calloc(FM160_LOG_RING_LINES,
						     sizeof(*worst));
		const struct fm160_log_rec **wp =
			calloc(FM160_LOG_RING_LINES, sizeof(*wp));
		char *line = malloc(FM160_LOG_LINE_MAX + 1);

		ok(worst && wp && line, "the worst-case ring allocated");
		if (worst && wp && line) {
			memset(line, 'x', FM160_LOG_LINE_MAX);
			line[FM160_LOG_LINE_MAX] = '\0';

			for (i = 0; i < FM160_LOG_RING_LINES; i++) {
				memset(&worst[i], 0, sizeof(worst[i]));
				worst[i].at_ms = 10000 + i;
				worst[i].prio = 6;
				worst[i].truncated = true;
				memcpy(worst[i].msg, line,
				       sizeof(worst[i].msg) - 1);
				worst[i].msg[sizeof(worst[i].msg) - 1] = '\0';
				wp[i] = &worst[i];
			}

			fill_state();
			base_input();
			in.log = wp;
			in.log_n = FM160_LOG_RING_LINES;
			in.log_total = FM160_LOG_RING_LINES;

			len2 = fm160_diag_format(&in, buf, sizeof(buf));
			ok(len2 > 0,
			   "a full ring of maximum-length lines fits the "
			   "daemon's buffer");
			if (len2 > 0)
				printf("       %ld of %d bytes used\n", len2,
				       (int)sizeof(buf));
			else
				printf("       the buffer is %d bytes; the report"
				       " did not fit\n", (int)sizeof(buf));

			/* And again with every string at its limit.  This is the
			 * case the head allowance is actually sized for; without
			 * it the number above is measured on a state whose strings
			 * are all one character long. */
			fill_state_long();
			len2 = fm160_diag_format(&in, buf, sizeof(buf));
			ok(len2 > 0,
			   "a full ring AND maximum-length strings still fit");
			if (len2 > 0)
				printf("       %ld of %d bytes used\n", len2,
				       (int)sizeof(buf));
			else
				printf("       the buffer is %d bytes; the report"
				       " did not fit\n", (int)sizeof(buf));
		}
		free(line); free(wp); free(worst);
	}

	/* A buffer one byte short must be a refusal, not a truncated report.
	 * big is deliberately larger than the report so the canary is outside
	 * the range the formatter may touch. */
	fill_state();
	base_input();
	in.log_n = 0;
	len = fm160_diag_format(&in, buf, sizeof(buf));
	big = malloc((size_t)len + 64);
	ok(big != NULL, "the canary buffer allocated");
	if (big) {
		int dirty = 0;
		size_t j;

		memset(big, 0x7f, (size_t)len + 64);

		eqi(fm160_diag_format(&in, big, (size_t)len),
		    -1, "a buffer of exactly len bytes is refused");

		/* The refusal must be a refusal and not a quiet truncation: with a
		 * capacity of len the formatter may touch 0..len-1 and no more.
		 * Checked before the successful call below, because that one
		 * legitimately writes the terminator at index len. */
		for (j = (size_t)len; j < (size_t)len + 64; j++)
			if ((unsigned char)big[j] != 0x7f)
				dirty++;
		eqi(dirty, 0, "nothing is written past the declared capacity");

		eqi(fm160_diag_format(&in, big, (size_t)len + 1),
		    len, "a buffer of len + 1 bytes holds the whole report");
		eqi(fm160_diag_format(&in, big, 0), -1,
		    "a buffer of zero bytes is refused");

		ok(!memcmp(big, buf, (size_t)len),
		   "the report is the same in a different buffer");
		free(big);
	}

	eqi(fm160_diag_format(NULL, buf, sizeof(buf)), -1,
	    "a NULL input is refused");
	in.st = NULL;
	eqi(fm160_diag_format(&in, buf, sizeof(buf)), -1,
	    "a NULL state is refused");
	fill_state();
	base_input();
	eqi(fm160_diag_format(&in, NULL, sizeof(buf)), -1,
	    "a NULL buffer is refused");

	/* --- the same input twice --------------------------------------- */
	printf("\n== the report is reproducible ==\n");
	{
		char *b2 = malloc(sizeof(buf));
		long l2;

		ok(b2 != NULL, "the second buffer allocated");
		if (b2) {
			len = fm160_diag_format(&in, buf, sizeof(buf));
			l2 = fm160_diag_format(&in, b2, sizeof(buf));
			eqi(l2, len, "the same input produces the same length");
			ok(len > 0 && !memcmp(buf, b2, (size_t)len),
			   "the same input produces the same bytes");
			free(b2);
		}
	}

	printf("\n%d/%d assertions passed\n", checks - failures, checks);
	if (failures)
		printf("M6 DIAG-EXPORT TEST FAILED\n");
	else
		printf("M6 DIAG-EXPORT TEST OK\n");
	return failures ? 1 : 0;
}
EOF

# --- 2. build and run ------------------------------------------------------
# diag.c and net.c are compiled AS-IS.  net.c is here because the bundle prints
# the dial step and the AT verb through its name functions, i.e. the two
# implementations of those names are the ones that ship, not a copy.
if ! $CC -O1 -g -Wall -Wextra -Wno-unused-parameter \
	-I"$SRCDIR" -I"$LIBUBOX" -I"$WORK" -o "$WORK/t" \
	"$WORK/test.c" "$SRCDIR/diag.c" "$SRCDIR/net.c" > "$WORK/cc.log" 2>&1; then
	echo "FATAL: the test did not compile" >&2
	cat "$WORK/cc.log" >&2
	exit 1
fi
if [ -s "$WORK/cc.log" ]; then
	echo "--- compiler warnings (the test compiles the real sources) ---" >&2
	cat "$WORK/cc.log" >&2
	echo >&2
fi

if ! "$WORK/t"; then
	echo
	echo "diag-export: FAILED" >&2
	exit 1
fi

# --- 3. the reply keys match what the page reads --------------------------
# The daemon and the page are written in two languages and joined by a set of
# key names that no build step checks.  A key the daemon sends and the page
# never reads is dead weight; a key the page reads and the daemon never sends
# renders as "undefined" in a browser and nothing else.  That is exactly the
# class of failure the three-list contract check exists for on the method names,
# applied one level down to the reply.
UBUS_C=${UBUS_C:-$ROOT/fm160d/src/ubus_methods.c}
VIEW_JS=${VIEW_JS:-$ROOT/luci-app-fm160/htdocs/luci-static/resources/view/fm160/debug.js}

if [ -z "$UBUS_C" ] || [ -z "$VIEW_JS" ]; then
	echo "  SKIPPED: reply-key check -- pass UBUS_C= and VIEW_JS=" >&2
else
	echo
	"$PY" - "$UBUS_C" "$VIEW_JS" <<'PYEOF'
import io, re, sys

ubus, view = sys.argv[1], sys.argv[2]
src = io.open(ubus, encoding='utf-8').read()

# The handler body only: from its definition to the method table.
m = re.search(r'static int handle_diagnostics\(.*?\n\}\n', src, re.S)
if not m:
    sys.exit('the diagnostics handler is not in %s' % ubus)
body = m.group(0)

sent = set(re.findall(r'blobmsg_add_\w+\(&b,\s*"([^"]+)"', body))

# Only the methods that talk to THIS reply.  Taking every `res.X` in the file
# also picks up the AT console's own `res.status` / `res.response`, which belong
# to the "at" method - so the check would fail for a reason that has nothing to
# do with the bundle, and the natural fix would be to add two phantom keys to
# the handler.  The view's methods are one `name: function` per method, so the
# diagnostics ones are selected by name.
vs = io.open(view, encoding='utf-8').read()
methods = {}
for mm in re.finditer(r'(?m)^\t([A-Za-z_][A-Za-z0-9_]*): function', vs):
    methods[mm.group(1)] = mm.start()
starts = sorted(methods.values())
blocks = []
for name, pos in methods.items():
    if not name.startswith('diag'):
        continue
    nxt = [s for s in starts if s > pos]
    blocks.append(vs[pos:nxt[0] if nxt else len(vs)])
used = set()
for b in blocks:
    used |= set(re.findall(r'\bres\.([A-Za-z_][A-Za-z0-9_]*)', b))

checks = failures = 0


def ok(cond, what, detail=''):
    global checks, failures
    checks += 1
    if cond:
        print('  ok   ' + what)
    else:
        failures += 1
        print('  FAIL ' + what)
        if detail:
            print('       ' + detail)


ok(len(sent) >= 5, 'the handler sends its keys (%d)' % len(sent))
ok(len(blocks) >= 3, 'the page has diagnostics methods (%d)' % len(blocks))
ok(len(used) >= 3, 'the page reads its keys (%d)' % len(used))

missing = sorted(used - sent)
useless = sorted(sent - used)

ok(not missing,
   'every key the page reads is one the handler sends' +
   (' -- not sent: ' + ', '.join(missing) if missing else ''))
ok(not useless,
   'the handler sends no key the page ignores' +
   (' -- unused: ' + ', '.join(useless) if useless else ''))

# The design claim this whole feature rests on: the bundle is built from cached
# state, so asking for it cannot change what it describes.  The tempting
# alternative - re-read the modem so the report is fresh - is not merely
# unnecessary here, it is destructive: on this hardware a re-read opens a quiet
# window, resets the poll ladder and changes the timing a fault depends on, so
# the diagnostic makes its own subject disappear.  A regression would be one
# plausible-looking line added in that spirit, and no functional test would
# notice, so the absence is asserted rather than assumed.
#
# Checked as a list of name prefixes rather than by looking for what IS there:
# a new call that puts work on the wire must trip this, and the only way to
# guarantee that is to forbid the families of calls that do.
FORBIDDEN = [
    'atq_submit', 'atq_set_quiet', 'atq_clear_quiet', 'atq_state_reset',
    'atq_probe_port', 'fm160_cmd_', 'fm160_uci_set', 'fm160_modesw_apply',
    'fm160_dial_', 'fm160_sched_kick', 'fm160_state_mark_dirty',
    'sendat', 'popen', 'system(',
]
hit = [f for f in FORBIDDEN if f in body]
ok(not hit, 'the handler queues no AT work and changes no state',
   '-- found: ' + ', '.join(hit))

# And it must actually read the ring, or the log section would be empty on every
# device while every other assertion here still passed.
ok('fm160_log_ring_at' in body,
   'the handler walks the log ring')
ok('fm160_diag_format' in body,
   'the handler goes through the pure formatter')

print()
print('%d/%d assertions passed' % (checks - failures, checks))
sys.exit(1 if failures else 0)
PYEOF
	[ $? = 0 ] || exit 1
fi

echo
echo "diag-export: clean"
