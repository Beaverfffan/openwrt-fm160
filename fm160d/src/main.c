/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * main.c - fm160d entry point.
 *
 * Responsibilities:
 *   - connect to ubus and register the "fm160" object;
 *   - subscribe to the transport's ubus events ("qmodem.at.urc" and
 *     "qmodem.at.line") so unsolicited modem output is handled by event
 *     instead of by polling;
 *   - load configuration, start the scheduler, run the uloop.
 */

#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "fm160d.h"

struct fm160_state g_state;
struct ubus_context *g_ubus;

static struct ubus_event_handler at_event_handler;
static const char *log_name = "fm160d";
static int log_level = LOG_INFO;
/* UCI ships log_level '6'.  Without this flag a `-d` on the command line was
 * silently overridden by the config file, so the most natural way to debug the
 * daemon produced no DEBUG output at all -- which cost a full diagnose cycle. */
static bool log_level_forced;

/* ------------------------------------------------------------------ */
/* logging                                                              */
/* ------------------------------------------------------------------ */

/*
 * The in-process log ring.  See fm160d.h for why it exists; the mechanics are
 * here.
 *
 * Single-threaded by construction: every caller of fm160_log() runs on the
 * uloop, including the signal handlers (uloop_signal_add() delivers through the
 * event loop, not from the handler context), so no lock is needed and a reader
 * that walks the ring cannot be preempted in the middle of it.  The one rule
 * that follows is that a reader must not log while walking - fm160_log_ring_at()
 * says the same thing from the other side.
 */
static struct fm160_log_rec log_ring[FM160_LOG_RING_LINES];
static size_t log_ring_head;      /* next slot to write */
static size_t log_ring_count;     /* slots filled, saturating at the capacity */
static uint64_t log_ring_total;   /* lines ever logged */

/*
 * When the daemon started, for the uptime the bundle reports.  Stamped at the
 * top of main(), before anything that can log, so "uptime" and "the age of the
 * oldest remembered line" are measured from the same instant.
 */
static uint64_t start_ms;

void fm160_log(int priority, const char *fmt, ...)
{
	struct fm160_log_rec *r;
	va_list ap;
	int n;

	if (priority > log_level)
		return;

	va_start(ap, fmt);
	vsyslog(priority, fmt, ap);
	va_end(ap);

	/*
	 * Remember it as well.  The format is run a second time rather than the
	 * rendered bytes being captured from vsyslog, which cannot be done: the
	 * two calls therefore cannot drift, because a change to the arguments
	 * changes both.  What they CAN differ in is truncation - syslog takes
	 * the whole line, this record is FM160_LOG_LINE_MAX - so a line that
	 * does not fit is marked rather than shortened quietly.
	 */
	r = &log_ring[log_ring_head];
	r->at_ms = fm160_now_ms();
	r->prio = priority;
	va_start(ap, fmt);
	n = vsnprintf(r->msg, sizeof(r->msg), fmt, ap);
	va_end(ap);
	if (n < 0) {
		r->msg[0] = '\0';
		r->truncated = true;
	} else {
		r->truncated = ((size_t)n >= sizeof(r->msg));
	}

	log_ring_head = (log_ring_head + 1) % FM160_LOG_RING_LINES;
	if (log_ring_count < FM160_LOG_RING_LINES)
		log_ring_count++;
	log_ring_total++;
}

size_t fm160_log_ring_count(void)
{
	return log_ring_count;
}

uint64_t fm160_log_ring_total(void)
{
	return log_ring_total;
}

const struct fm160_log_rec *fm160_log_ring_at(size_t i)
{
	size_t oldest;

	if (i >= log_ring_count)
		return NULL;

	/* Once the ring has wrapped, the oldest line is the one about to be
	 * overwritten - i.e. at `head`.  Before that the array is still being
	 * filled from 0 and head is simply the end of it. */
	oldest = (log_ring_count == FM160_LOG_RING_LINES) ? log_ring_head : 0;

	return &log_ring[(oldest + i) % FM160_LOG_RING_LINES];
}

/*
 * The two scalars the diagnostics bundle needs from this file.
 *
 * Both are file-static here, and both would be wrong if guessed at by the
 * caller: the log level is decided from uci and from the command line, and the
 * uptime has to come from a stamp taken before anything else ran, not from the
 * first log line (which is emitted after the argument parsing that can restart
 * the clock of a reader's expectations).
 */
int fm160_log_level(void)
{
	return log_level;
}

uint64_t fm160_uptime_ms(void)
{
	uint64_t now = fm160_now_ms();

	return (now > start_ms) ? now - start_ms : 0;
}

/* ------------------------------------------------------------------ */
/* config                                                               */
/* ------------------------------------------------------------------ */

static int uci_get_section(const char *section, const char *option,
			   char *out, size_t outlen)
{
	char cmd[192];
	FILE *p;
	size_t n;

	snprintf(cmd, sizeof(cmd),
		 "uci -q get fm160.%s.%s 2>/dev/null", section, option);
	p = popen(cmd, "r");
	if (!p)
		return -1;
	if (!fgets(out, outlen, p)) {
		pclose(p);
		return -1;
	}
	pclose(p);
	n = strlen(out);
	while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		out[--n] = '\0';
	return out[0] ? 0 : -1;
}

int fm160_uci_get_section(const char *section, const char *option,
			  char *out, size_t outlen)
{
	return uci_get_section(section, option, out, outlen);
}

int fm160_uci_get(const char *option, char *out, size_t outlen)
{
	return uci_get_section("main", option, out, outlen);
}

/*
 * The only write path into uci.  See fm160d.h for what is written and why;
 * what matters here is the guard.
 *
 * The value reaches a shell, so a conservative character set is checked first
 * and anything else refused rather than escaped: the set covers every value
 * this daemon writes (mode numbers, epoch seconds, comma-joined numbers,
 * "-1" for "nothing"), so a refusal can only mean a bug and never a legitimate
 * value that needed quoting.  Two uci calls because `uci set` alone only edits
 * the in-memory delta - without the commit, a restart loses exactly the state
 * that had to survive it.
 */
int fm160_uci_set(const char *section, const char *option, const char *value)
{
	char cmd[256];
	const char *p;

	if (!section || !option || !value)
		return -1;
	for (p = value; *p; p++) {
		int c = (unsigned char)*p;

		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
		    (c >= 'A' && c <= 'Z') || c == '_' || c == '-' ||
		    c == '.' || c == ',' || c == ':')
			continue;
		fm160_log(LOG_ERR, "refusing to write %s.%s: value has an "
			  "unexpected character (%d)", section, option, c);
		return -1;
	}

	snprintf(cmd, sizeof(cmd),
		 "uci -q set fm160.%s.%s='%s' && uci -q commit fm160",
		 section, option, value);
	if (system(cmd))
		return -1;
	return 0;
}

int fm160_uci_set_int(const char *section, const char *option, long long v)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%lld", v);
	return fm160_uci_set(section, option, buf);
}

/*
 * NOTE the sense of the test below.  uci_get_section() follows the C
 * convention of the rest of this file -- 0 for success, -1 for failure --
 * and every other caller writes `if (!fm160_uci_get(...))` to mean "use the
 * value".  This one is the mirror of that: it returns the fallback when the
 * READ FAILED, so the test must be positive.  Written the other way round it
 * returns the fallback when the read SUCCEEDED, which makes every boolean in
 * the section silently equal to its default no matter what uci says -- and
 * because three of the four defaults happen to match the shipped config, the
 * only visible symptom is the one option a user ever changes by hand.
 */
static bool uci_bool(const char *option, bool fallback)
{
	char v[32];

	if (uci_get_section("main", option, v, sizeof(v)))
		return fallback;
	if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes") ||
	    !strcmp(v, "on"))
		return true;
	if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no") ||
	    !strcmp(v, "off"))
		return false;
	return fallback;
}

void fm160_config_load(void)
{
	char v[FM160_STR_MAX];
	int n;

	g_state.enabled = uci_bool("enabled", true);

	/* GNSS is the one subsystem whose switch the MODULE does not store, so
	 * "have the engine on" has to be re-applied by us at every boot.  Default
	 * off: turning a radio on is the user's decision, and a tracker that
	 * starts transmitting a position nobody asked for is not a default. */
	g_state.gnss.autostart = uci_bool("gnss_autostart", false);

	/* --- M2: the data plane --------------------------------------- */
	{
		char apn[FM160_NET_APN_MAX] = "";
		char pdp[16] = "";
		int cid = 1;

		if (!fm160_uci_get("dial_apn", v, sizeof(v)))
			snprintf(apn, sizeof(apn), "%s", v);
		if (!fm160_uci_get("dial_pdp", v, sizeof(v)))
			snprintf(pdp, sizeof(pdp), "%s", v);
		if (!fm160_uci_get("dial_cid", v, sizeof(v)))
			cid = atoi(v);

		/* Both default to off, and both defaults are deliberate:
		 *
		 *   autostart - this device has no SIM, so an autostart would
		 *   climb the whole ladder at every boot and finish by leaving a
		 *   reset in the ledger, for a link that could never come up.
		 *
		 *   allow_reset - AT+CFUN=1,1 is the only thing in this daemon
		 *   that reboots the module.  DESIGN 4.3 allows it as the last
		 *   rung; being off by default means the ladder stops and says
		 *   why instead, which on a modem we cannot power-cycle by hand
		 *   is the better failure.
		 */
		fm160_dial_configure(apn, pdp, cid,
				     uci_bool("dial_allow_reset", false),
				     uci_bool("dial_autostart", false));
	}

	if (!fm160_uci_get("tier_scale", v, sizeof(v))) {
		n = atoi(v);
		if (n >= 1 && n <= 6)
			g_state.tier_scale = n;
	}

	if (!fm160_uci_get("netdev", v, sizeof(v))) {
		if (snprintf(g_state.netdev, sizeof(g_state.netdev), "%s", v) >=
		    (int)sizeof(g_state.netdev)) {
			g_state.netdev[0] = '\0';
			fm160_log(LOG_WARN,
				  "config netdev '%s' too long, ignored", v);
		}
	}

	/* A manually pinned AT port skips discovery (useful while debugging). */
	if (!fm160_uci_get("port", v, sizeof(v))) {
		if (snprintf(g_state.cand[0], FM160_PORT_MAX, "%s", v) >=
		    FM160_PORT_MAX) {
			fm160_log(LOG_WARN,
				  "config port '%s' too long, ignored", v);
		} else {
			g_state.cand_count = 1;
			fm160_log(LOG_INFO, "AT port pinned to %s by config", v);
		}
	}

	if (!log_level_forced && !fm160_uci_get("log_level", v, sizeof(v))) {
		n = atoi(v);
		if (n >= LOG_ERR && n <= LOG_DEBUG)
			log_level = n;
	}
}

/* ------------------------------------------------------------------ */
/* transport events                                                     */
/* ------------------------------------------------------------------ */

enum {
	EV_PORT,
	EV_OWNER,
	EV_URC_ID,
	EV_RAW_LINE,
	EV_CORRELATION,
	EV_SEQUENCE,
	EV_DROP_COUNT,
	__EV_MAX
};

static const struct blobmsg_policy ev_policy[] = {
	[EV_PORT]        = { .name = "port",        .type = BLOBMSG_TYPE_STRING },
	[EV_OWNER]       = { .name = "owner",       .type = BLOBMSG_TYPE_STRING },
	[EV_URC_ID]      = { .name = "urc_id",      .type = BLOBMSG_TYPE_STRING },
	[EV_RAW_LINE]    = { .name = "raw_line",    .type = BLOBMSG_TYPE_STRING },
	[EV_CORRELATION] = { .name = "correlation", .type = BLOBMSG_TYPE_STRING },
	[EV_SEQUENCE]    = { .name = "sequence",    .type = BLOBMSG_TYPE_INT64  },
	[EV_DROP_COUNT]  = { .name = "drop_count",  .type = BLOBMSG_TYPE_INT64  },
};

/* Handle a line that the transport classified as AT_CORRELATION_IDLE, i.e. it
 * belongs to no command we issued - a genuine unsolicited result code. */
static void handle_urc_line(const char *line)
{
	if (!strncmp(line, "+CEREG", 6)) {
		int stat = FM160_REG_UNKNOWN;
		char tmp[FM160_STR_MAX];

		snprintf(tmp, sizeof(tmp), "%s", line);
		fm160_parse_greg(tmp, "CEREG", &stat);
		if (stat != g_state.cereg) {
			g_state.cereg = stat;
			fm160_state_mark_dirty();
			fm160_log(LOG_INFO, "registration (LTE) -> %d", stat);
		}
		return;
	}
	if (!strncmp(line, "+C5GREG", 7)) {
		int stat = FM160_REG_UNKNOWN;
		char tmp[FM160_STR_MAX];

		snprintf(tmp, sizeof(tmp), "%s", line);
		fm160_parse_greg(tmp, "C5GREG", &stat);
		if (stat != g_state.c5greg) {
			g_state.c5greg = stat;
			fm160_state_mark_dirty();
			fm160_log(LOG_INFO, "registration (NR) -> %d", stat);
		}
		return;
	}
	if (!strncmp(line, "+CMTI", 5) || !strncmp(line, "+CMT", 4) ||
	    !strncmp(line, "+CDS", 4) || !strncmp(line, "+CMS ERROR", 10) ||
	    !strncmp(line, "+CME ERROR", 10)) {
		/* SMS owns its own unsolicited results.  It gets first refusal
		 * here so that the fetch a +CMTI triggers is scheduled by the
		 * event itself, rather than by a note taken here plus a hook
		 * elsewhere that has to agree with it. */
		if (fm160_sms_handle_urc(line))
			return;
	}
	fm160_log(LOG_DEBUG, "URC: %s", line);
}

static void at_event_cb(struct ubus_context *ctx, struct ubus_event_handler *ev,
			const char *type, struct blob_attr *msg)
{
	struct blob_attr *tb[__EV_MAX];
	const char *line, *corr, *port, *urc_id;
	bool is_urc_event = type && !strcmp(type, "qmodem.at.urc");

	blobmsg_parse(ev_policy, __EV_MAX, tb,
		      msg ? blob_data(msg) : NULL, msg ? blob_len(msg) : 0);
	if (!tb[EV_RAW_LINE])
		return;

	line = blobmsg_get_string(tb[EV_RAW_LINE]);
	corr = tb[EV_CORRELATION] ? blobmsg_get_string(tb[EV_CORRELATION]) : "";
	port = tb[EV_PORT] ? blobmsg_get_string(tb[EV_PORT]) : "";
	urc_id = tb[EV_URC_ID] ? blobmsg_get_string(tb[EV_URC_ID]) : "";

	if (g_state.port_found && port[0] && strcmp(port, g_state.port))
		return;         /* some other port (should not happen) */

	if (tb[EV_DROP_COUNT] && blobmsg_get_u64(tb[EV_DROP_COUNT])) {
		fm160_log(LOG_WARN, "transport dropped %llu line event(s)",
			  (unsigned long long)blobmsg_get_u64(tb[EV_DROP_COUNT]));
	}

	if (is_urc_event)
		fm160_log(LOG_DEBUG, "urc[%s]: %s", urc_id, line);

	/* Only lines that belong to no command are unsolicited.  Lines tagged
	 * "response" or "terminal" are our own answers and have already been
	 * parsed by the command callback. */
	if (corr[0] && strcmp(corr, "idle"))
		return;

	handle_urc_line(line);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                            */
/* ------------------------------------------------------------------ */

/*
 * libubox quirk: the handler receives only the signal object - the number is a
 * member of struct uloop_signal, and uloop_signal_add() takes just the object,
 * so .signo has to be filled in at initialisation time.
 */
static void signal_cb(struct uloop_signal *s)
{
	fm160_log(LOG_INFO, "signal %d, exiting", s->signo);
	uloop_end();
}

static struct uloop_signal sigterm = { .cb = signal_cb, .signo = SIGTERM };
static struct uloop_signal sigint  = { .cb = signal_cb, .signo = SIGINT };

static void ubus_connect_cb(struct ubus_context *ctx)
{
	at_daemon_hint_reconnect();
}

static int setup_ubus(void)
{
	int ret;

	g_ubus = ubus_connect(NULL);
	if (!g_ubus) {
		fm160_log(LOG_ERR, "cannot connect to ubus");
		return -1;
	}
	ubus_add_uloop(g_ubus);
	g_ubus->connection_lost = ubus_connect_cb;

	fm160_ubus_methods_init();

	at_event_handler.cb = at_event_cb;
	ret = ubus_register_event_handler(g_ubus, &at_event_handler,
					  "qmodem.at.*");
	if (ret)
		fm160_log(LOG_WARN,
			  "cannot subscribe to qmodem.at.* events (%s): URC handling disabled",
			  ubus_strerror(ret));

	return 0;
}

int main(int argc, char **argv)
{
	bool once = false;
	int i;

	/* First statement in the process, so "uptime" in a diagnostic bundle and
	 * the age of the oldest remembered log line are the same measurement. */
	start_ms = fm160_now_ms();

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--debug")) {
			log_level = LOG_DEBUG;
			log_level_forced = true;
		}
		else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--once"))
			once = true;
	}

	openlog(log_name, LOG_PID, LOG_DAEMON);
	fm160_log(LOG_INFO, "fm160d starting");

	uloop_init();
	uloop_signal_add(&sigterm);
	uloop_signal_add(&sigint);

	fm160_state_init();
	/* Before the config load, so these two can wipe their tables: the APN,
	 * the context id, the profile the switch came from and the reset ledger
	 * all arrive from uci afterwards. */
	fm160_dial_init();
	fm160_modesw_init();
	fm160_config_load();
	fm160_sms_init();

	if (setup_ubus()) {
		uloop_done();
		return 1;
	}

	atq_init();
	fm160_sched_init();

	if (once) {
		/* Run the loop briefly then exit - handy for CI and for probing
		 * a device without leaving a daemon behind. */
		uloop_run_timeout(1500);
		fm160_state_publish();
		uloop_done();
		return 0;
	}

	uloop_run();
	uloop_done();
	closelog();
	return 0;
}
