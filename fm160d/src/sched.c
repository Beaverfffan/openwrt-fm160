/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sched.c - polling scheduler, quiet windows, circuit breaker, port discovery.
 *
 * The FM160 runs Qualcomm firmware whose AT parser is easily overwhelmed, so
 * every periodic exchange goes through one of the tiers defined below.  Rules
 * that are enforced here (and nowhere else):
 *
 *   - tiers have separate base intervals for the idle and foreground case;
 *   - every firing gets +-20% jitter so tiers never phase-lock;
 *   - a global "speed factor" derived from the circuit breaker slows ALL
 *     polling down when the modem starts timing out;
 *   - while a quiet window is open no POLL-priority command is submitted at
 *     all (state-machine commands own the window and are still allowed);
 *   - byte counters are read from sysfs and cost zero AT commands.
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "fm160d.h"

#define HOUSEKEEP_MS            1000
#define FOREGROUND_HOLD_MS      30000
#define PORT_PROBE_GAP_MS       5000
#define BREAKER_SOFT_LIMIT      3      /* consecutive timeouts -> degraded  */
#define BREAKER_HARD_LIMIT      10     /* -> stop automatic polling         */
#define BREAKER_SOFT_HOLD_S     60

struct poll_item {
	const char *name;
	bool active_only;      /* only fire while a UI page is open     */
	int  idle_ms;          /* 0 = never at idle                     */
	int  active_ms;        /* 0 = same as idle                      */
	uint64_t next_ms;
	uint64_t last_fire_ms;
	int  fires;
	int  skips;
	void (*fire)(void);
};

/*
 * Commands that must NEVER be added to this table.
 *
 * Measured on FM160-CN 89614.1000.00.04.01.02 with no SIM fitted, which is the
 * worst case for anything that waits on card state.  The three rows below all
 * answer in <= 36 ms; these do not, and because the port is serial a single
 * stalled row starves every other command:
 *
 *   AT+GTPKGVER?   12 901 ms   ERROR (and leaks a +GTDUALSIM URC)
 *   AT+CCID        10 345 ms   timeout
 *   AT+CPMS?       10 345 ms   ERROR
 *   AT+CMGF?        5 373 ms   +CME ERROR: 10
 *   AT+CIMI         5 221 ms   (no data, URC leak)
 *   AT+CGDCONT?     5 211 ms   (no data, URC leak)
 *
 * Identity (CGMI/CGMM/CGMR/CGSN/CFSN/ICCID), the USB mode and the M4 capability
 * lists (AT+GTACT=? / AT+GTCELLLOCK=?) are read once per boot by the ident
 * sequence, never here.  The capability lists in particular are the *permission*
 * for the M4 write path, so they must not be something a poll loop can lose.
 * Anything SIM- or SMS-related belongs inside the quiet window behind an
 * explicit user action.
 *
 * For contrast, the commands that ARE polled, measured the same way:
 *   AT+CSQ 24 ms | AT+CREG?/AT+CGREG?/AT+CEREG? 26-36 ms | AT+GTCCINFO? 25 ms
 *   AT+GTUSBMODE? 16 ms | AT+GTGPS? 15 ms
 */
static struct poll_item items[] = {
	/* registration + RSSI: the cheapest and most important tier */
	{ "reg",    false, 15000,  5000, 0, 0, 0, 0, fm160_cmd_poll_reg    },
	/* extended signal quality (RSRP/RSRQ/SINR) - needs a UI to be useful */
	{ "signal", true,      0, 10000, 0, 0, 0, 0, fm160_cmd_poll_signal },
	/* serving + neighbour cells (AT+GTCCINFO?, < 3 s on the modem) */
	{ "cell",   true,      0, 10000, 0, 0, 0, 0, fm160_cmd_poll_cell   },
	/* --- M4 -------------------------------------------------------- */
	/* Persistent band restriction, 26 ms measured.  These two are the one
	 * deliberate exception to "slow-changing commands stay out of the poll
	 * loop": they are not merely informational, they are the *evidence* that
	 * a restriction is in force.  An empty cache would let the UI say
	 * "unrestricted" when the truth is "we have not looked", which is a
	 * wrong answer rather than a missing one - so they are read at a very
	 * long idle interval instead of never. */
	{ "gtact",    false, 600000, 15000, 0, 0, 0, 0, fm160_cmd_poll_gtact    },
	/* Cell lock state, 16 ms.  Also the gate for the mutual exclusion
	 * between cell lock and band lock, so it is worth knowing at idle. */
	{ "celllock", false, 600000, 15000, 0, 0, 0, 0, fm160_cmd_poll_celllock },
	/* Carrier aggregation: only meaningful to a human reading the page. */
	{ "ca",       true,      0, 15000, 0, 0, 0, 0, fm160_cmd_poll_ca       },
	/* --- M5 -------------------------------------------------------- */
	/* The engine switch, 22 ms.  Always submitted, including at idle, because
	 * it is the only read that can notice the engine having been switched on
	 * from elsewhere or reset to 0 by a module power cycle - and with the
	 * engine off the NMEA tier below stays silent, so without this tier a
	 * module that came back would never be noticed at all. */
	{ "gpspower", false, 600000, 15000, 0, 0, 0, 0, fm160_cmd_poll_gnss_power },
	/* The NMEA block, 30 ms for eight sentences - cheaper than the cell query
	 * above it.  Foreground only, matching the design's "only while the GNSS
	 * page is open": the engine keeps tracking regardless of who is reading,
	 * so nobody pays for a screen that is not being looked at.
	 *
	 * The fire function returns without submitting anything while the engine
	 * is off, which is the normal state - with the engine off AT+GTGPS?
	 * answers ERROR, so every such poll would be a wasted transaction on the
	 * one parser this project is built to be gentle with. */
	{ "gnss",     true,      0,  5000, 0, 0, 0, 0, fm160_cmd_poll_gnss      },
	{ NULL, false, 0, 0, 0, 0, 0, 0, NULL },
};

static struct uloop_timeout housekeep_timer;
static int speed_factor = 1;
static uint64_t poll_suspend_until_ms;
static bool port_ever_found;

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static int read_sysfs(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return -1;
	n = fread(buf, 1, len - 1, f);
	fclose(f);
	if (!n)
		return -1;
	buf[n] = '\0';
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
		     buf[n - 1] == ' '))
		buf[--n] = '\0';
	return 0;
}

static double rand01(void)
{
	return (double)(rand() % 10000) / 10000.0;
}

static int item_base_ms(const struct poll_item *it)
{
	int base;

	if (g_state.foreground && it->active_ms)
		base = it->active_ms;
	else
		base = it->idle_ms;

	if (!base)
		return 0;
	base *= g_state.tier_scale > 0 ? g_state.tier_scale : 1;

	/* jitter, then the breaker-driven speed factor */
	base = (int)(base * (0.8 + 0.4 * rand01()));
	return base * speed_factor;
}

static void item_arm(struct poll_item *it)
{
	int base = item_base_ms(it);

	if (!base) {
		it->next_ms = UINT64_MAX;
		return;
	}
	it->next_ms = fm160_now_ms() + base;
}

/* ------------------------------------------------------------------ */
/* port discovery                                                       */
/* ------------------------------------------------------------------ */

/* Last candidate set we announced, so the rescan loop stays quiet. */
static char scan_sig[256];

/*
 * Resolve (idVendor, bInterfaceNumber) of the USB node a /dev/<name> tty hangs
 * off, in one upward walk.
 *
 * The sysfs chain is NOT uniform across drivers, which is what broke the old
 * hard-coded "/sys/class/tty/%s/device/../idVendor":
 *
 *   usb-serial / option:  .../ttyUSB0/device -> the serial PORT device
 *                                              (parent = usb_interface)
 *   cdc_acm:              .../ttyACM0/device -> the usb_interface itself
 *
 * idVendor lives in the usb_device, one level ABOVE the interface, while
 * bInterfaceNumber lives IN the interface.  On this hardware
 * /sys/class/tty/ttyUSBn/device/.. resolves to the interface directory -- it
 * has bInterfaceNumber but no idVendor -- so the old single-level read failed
 * for all four ports.  Walk upwards instead of guessing a depth.
 *
 * Returns 0 when idVendor was found (written to out), -1 otherwise.  *iface is
 * set to -1 when bInterfaceNumber could not be read.
 */
static int tty_usb_ids(const char *name, char *out, size_t outlen, int *iface)
{
	char link[256], dir[PATH_MAX];
	int have_vid = 0, up;

	*iface = -1;
	if (snprintf(link, sizeof(link), "/sys/class/tty/%s/device", name)
	    >= (int)sizeof(link))
		return -1;
	if (!realpath(link, dir))
		return -1;

	for (up = 0; up < 6; up++) {
		char path[PATH_MAX], buf[64];
		char *slash;

		if (!have_vid &&
		    snprintf(path, sizeof(path), "%s/idVendor", dir)
		    < (int)sizeof(path) && read_sysfs(path, out, outlen) == 0)
			have_vid = 1;
		if (*iface < 0 &&
		    snprintf(path, sizeof(path), "%s/bInterfaceNumber", dir)
		    < (int)sizeof(path) && read_sysfs(path, buf, sizeof(buf)) == 0)
			*iface = (int)strtol(buf, NULL, 16);  /* sysfs: %02x */
		if (have_vid && *iface >= 0)
			break;

		slash = strrchr(dir, '/');
		if (!slash || slash == dir)
			break;
		*slash = '\0';
		if (!strcmp(dir, "/sys/devices") || !strcmp(dir, "/sys"))
			break;
	}
	return have_vid ? 0 : -1;
}

/*
 * Probe order within one scan.  The vendor's serial layout is documented as
 * 0=DIAG 1=NMEA 2=AT 3=MODEM, and it is a useful starting point -- but it is
 * NOT what this module actually does.  Measured on FM160-CN 89614.1000.00.04.01.02
 * (mode 32, QMI) on 2026-09-18, four probes per interface, 2 s each:
 *
 *   iface 0  silent    -- not one byte back, so it can only ever cost a timeout
 *   iface 1  answers   -- "AT" -> OK in 20 ms, ATI returns the full 142-byte
 *                         identity.  It emits NO NMEA at all (an end flag that
 *                         can never match captured only the OK), so the
 *                         "1=NMEA" label is wrong here.
 *   iface 2  answers   -- the port the datasheet promises
 *   iface 3  echoes    -- partial_response on timeout is our own "AT\r\n\r\n":
 *                         echoed, but no AT interpreter behind it
 *
 * So there are TWO usable AT ports, and iface 1 is a real fallback rather than
 * dead weight.  Ordering iface 1 second means the common failure -- iface 2
 * momentarily busy after a re-enumeration -- costs one probe instead of a
 * wasted timeout on the near-dead iface 3.
 *
 * This is a HINT only.  The probe still has the final say, so a wrong guess
 * costs time and can never select a port that does not actually answer.
 */
enum cand_class {
	CAND_FIBOCOM_AT = 0,   /* Fibocom, interface 2 -- the designated AT port */
	CAND_FIBOCOM_AT2,      /* Fibocom, interface 1 -- second AT port, verified */
	CAND_FIBOCOM,          /* Fibocom, some other interface */
	CAND_UNKNOWN,          /* idVendor unreadable: take it, but say so */
	CAND_SILENT,           /* known not to answer AT: see cand_class_for() */
	CAND_CLASSES
	/* Foreign ports need no class: they are dropped outright below. */
};

/*
 * Rank a Fibocom interface by how likely it is to answer "AT".
 *
 * Returns CAND_SILENT for interfaces this module has been measured never to
 * answer on.  Those are held back rather than thrown away -- see the emission
 * loop in fm160_scan_candidates(): if they are the ONLY thing present, they are
 * still tried.  Dropping a real AT port means the daemon never comes up at all,
 * which is a far worse failure than spending one timeout, and a future USB
 * mode could move AT back to interface 0.
 */
static int cand_class_for(int iface)
{
	switch (iface) {
	case 2:
		return CAND_FIBOCOM_AT;
	case 1:
		return CAND_FIBOCOM_AT2;
	case 0:
		return CAND_SILENT;
	default:
		return CAND_FIBOCOM;
	}
}

static void fm160_scan_candidates(void)
{
	struct { char name[32]; int cls; int iface; } found[FM160_CAND_MAX];
	DIR *d;
	struct dirent *de;
	int nfound = 0, n_vid = 0, n_unknown = 0, n_foreign = 0, n_silent = 0;
	int cls, i;
	char sig[256] = "";
	size_t off = 0;
	bool changed;

	g_state.cand_count = 0;
	g_state.cand_idx = 0;

	d = opendir("/sys/class/tty");
	if (!d)
		return;

	while ((de = readdir(d)) != NULL && nfound < FM160_CAND_MAX) {
		char *name = de->d_name;
		char vid[64];
		int iface, n;

		if (strncmp(name, "ttyUSB", 6) && strncmp(name, "ttyACM", 6))
			continue;

		n = snprintf(found[nfound].name, sizeof(found[nfound].name),
			     "/dev/%s", name);
		if (n < 0 || n >= (int)sizeof(found[nfound].name))
			continue;   /* truncated path is worse than no path */

		if (tty_usb_ids(name, vid, sizeof(vid), &iface) != 0) {
			/* Could not establish whose port this is.  Keep it: a
			 * probe that fails is cheap and recoverable, whereas
			 * dropping the real AT port means the daemon never
			 * comes up at all.  But do NOT claim it is Fibocom --
			 * that lie is what hid this bug for a whole cycle. */
			found[nfound].cls = CAND_UNKNOWN;
			n_unknown++;
		} else if (strcasecmp(vid, FM160_VENDOR_ID)) {
			n_foreign++;
			continue;
		} else {
			found[nfound].cls = cand_class_for(iface);
			if (found[nfound].cls == CAND_SILENT)
				n_silent++;
			n_vid++;
		}
		found[nfound].iface = iface;
		nfound++;
	}
	closedir(d);

	/* Emit in class order; the array is stable, so ports keep readdir order
	 * inside a class.  CAND_SILENT is skipped here and considered only if
	 * the loop above produced nothing at all. */
	for (cls = 0; cls < CAND_CLASSES; cls++) {
		if (cls == CAND_SILENT)
			continue;
		for (i = 0; i < nfound; i++) {
			if (found[i].cls != cls)
				continue;
			snprintf(g_state.cand[g_state.cand_count],
				 FM160_PORT_MAX, "%s", found[i].name);
			g_state.cand_count++;
		}
	}

	if (g_state.cand_count == 0 && n_silent > 0) {
		fm160_log(LOG_WARNING,
			  "only interfaces that never answer are present; "
			  "probing them anyway (%d)", n_silent);
		for (i = 0; i < nfound; i++) {
			if (found[i].cls != CAND_SILENT)
				continue;
			snprintf(g_state.cand[g_state.cand_count],
				 FM160_PORT_MAX, "%s", found[i].name);
			g_state.cand_count++;
		}
	}

	/* Only speak when the picture actually changed.  This function is called
	 * once per failed sweep, and the old unconditional log made a 5 s rescan
	 * loop look like the daemon was doing something. */
	for (i = 0; i < g_state.cand_count; i++) {
		int w = snprintf(sig + off, sizeof(sig) - off, "%s,",
				 g_state.cand[i]);
		if (w < 0 || (size_t)w >= sizeof(sig) - off)
			break;
		off += (size_t)w;
	}
	changed = strcmp(sig, scan_sig) != 0;
	if (changed)
		snprintf(scan_sig, sizeof(scan_sig), "%s", sig);

	if (!changed)
		return;

	if (g_state.cand_count) {
		char order[FM160_CAND_MAX * 24];
		size_t o = 0;
		int j;

		/* Print g_state.cand[], not found[]: found[] is in readdir order,
		 * while cand[] is the order the probes will actually go out in.
		 * Printing the former made the log claim if3 was tried first when
		 * the daemon had in fact gone straight to if2. */
		order[0] = '\0';
		for (i = 0; i < g_state.cand_count && o < sizeof(order); i++) {
			int iface = -1, w;

			for (j = 0; j < nfound; j++) {
				if (!strcmp(found[j].name, g_state.cand[i])) {
					iface = found[j].iface;
					break;
				}
			}
			w = snprintf(order + o, sizeof(order) - o, "%s%s(if%d)",
				     i ? " " : "", g_state.cand[i], iface);
			if (w < 0 || (size_t)w >= sizeof(order) - o)
				break;
			o += (size_t)w;
		}

		fm160_log(LOG_INFO,
			  "AT candidates: %d Fibocom (%s:*), %d unclassified, "
			  "%d foreign, %d held back as silent | probe order: %s",
			  n_vid, FM160_VENDOR_ID, n_unknown, n_foreign, n_silent,
			  order);
	} else if (n_foreign || n_unknown) {
		fm160_log(LOG_INFO,
			  "no AT candidate: %d foreign tty(s), %d unclassified",
			  n_foreign, n_unknown);
	} else {
		fm160_log(LOG_INFO, "no serial port on /sys/class/tty");
	}
}

static void probe_cb(struct at_req *req, enum at_status status,
		     const char *response, void *arg)
{
	const char *probed = arg;

	if (status == AT_STATUS_OK && fm160_resp_ok(response)) {
		bool first = !g_state.port_found;

		snprintf(g_state.port, sizeof(g_state.port), "%s", probed);
		g_state.port_found = true;
		g_state.port_probing = false;
		g_state.at_state = 0;
		g_state.consec_timeout = 0;
		speed_factor = 1;
		port_ever_found = true;
		fm160_log(LOG_INFO, "AT port is %s", g_state.port);
		fm160_state_mark_dirty();
		fm160_sched_kick();
		if (first)
			fm160_cmd_ident_start();
		return;
	}

	g_state.port_probing = false;
	/* try the next candidate on the next housekeeping pass */
	g_state.port_next_probe_ms = 0;
}

static void port_discovery_tick(void)
{
	if (!g_state.port_found) {
		if (g_state.port_probing)
			return;
		if (fm160_now_ms() < g_state.port_next_probe_ms)
			return;
		if (g_state.cand_idx >= g_state.cand_count) {
			/* Either we have never scanned (cand_count == 0), or we
			 * just walked the whole list without an answer.  In the
			 * latter case wait before starting over: a modem that is
			 * absent (or still booting) must not be probed in a tight
			 * loop, and re-scanning immediately also re-ran the
			 * "found N ports" log on every single sweep. */
			if (g_state.cand_count > 0) {
				g_state.cand_count = 0;
				g_state.cand_idx = 0;
				g_state.port_next_probe_ms =
					fm160_now_ms() + PORT_PROBE_GAP_MS;
				return;
			}
			fm160_scan_candidates();
			if (!g_state.cand_count) {
				g_state.port_next_probe_ms =
					fm160_now_ms() + PORT_PROBE_GAP_MS;
				return;
			}
		}
		g_state.port_probing = true;
		if (atq_probe_port(g_state.cand[g_state.cand_idx], probe_cb,
				   g_state.cand[g_state.cand_idx]) != 0) {
			g_state.port_probing = false;
			g_state.port_next_probe_ms = fm160_now_ms() + 500;
			return;
		}
		g_state.cand_idx++;
		return;
	}

	/* Port was found before but the modem may have re-enumerated: if AT is
	 * degraded or dead for a while, drop the port and rescan. */
	if (g_state.at_state >= 1 &&
	    g_state.last_fail_ms &&
	    fm160_now_ms() - g_state.last_fail_ms > 20000) {
		fm160_log(LOG_WARN,
			  "AT unhealthy for >20 s, dropping %s and rescanning",
			  g_state.port);
		g_state.port_found = false;
		g_state.cand_idx = 0;
		g_state.cand_count = 0;
		g_state.port_next_probe_ms = fm160_now_ms() + PORT_PROBE_GAP_MS;
		fm160_state_mark_dirty();
	}
}

/* ------------------------------------------------------------------ */
/* circuit breaker                                                      */
/* ------------------------------------------------------------------ */

void fm160_sched_note_result(bool ok, bool timeout)
{
	if (ok) {
		g_state.consec_timeout = 0;
		if (g_state.at_state) {
			fm160_log(LOG_INFO, "AT recovered");
			g_state.at_state = 0;
			fm160_state_mark_dirty();
		}
		if (speed_factor != 1) {
			speed_factor = 1;
			fm160_sched_kick();
		}
		return;
	}

	g_state.last_fail_ms = fm160_now_ms();

	if (!timeout)
		return;         /* a plain ERROR is the modem answering - fine */

	g_state.consec_timeout++;

	if (g_state.consec_timeout == BREAKER_SOFT_LIMIT &&
	    g_state.at_state < 1) {
		g_state.at_state = 1;
		speed_factor = 2;
		fm160_log(LOG_WARN,
			  "AT degraded (%d timeouts): slowing polling and resting %d s",
			  g_state.consec_timeout, BREAKER_SOFT_HOLD_S);
		fm160_sched_suspend(BREAKER_SOFT_HOLD_S);
		fm160_state_mark_dirty();
	} else if (g_state.consec_timeout >= BREAKER_HARD_LIMIT &&
		   g_state.at_state < 2) {
		g_state.at_state = 2;
		speed_factor = 4;
		fm160_log(LOG_ERR,
			  "AT dead (%d timeouts): automatic polling stopped",
			  g_state.consec_timeout);
		fm160_state_mark_dirty();
	}
}

void fm160_sched_suspend(int seconds)
{
	struct poll_item *it;

	poll_suspend_until_ms = fm160_now_ms() + (uint64_t)seconds * 1000;
	for (it = items; it->name; it++)
		it->next_ms = poll_suspend_until_ms;
}

/* ------------------------------------------------------------------ */
/* housekeeping                                                         */
/* ------------------------------------------------------------------ */

void fm160_sched_report_foreground(void)
{
	g_state.last_active_ms = fm160_now_ms();
}

void fm160_sched_kick(void)
{
	struct poll_item *it;
	uint64_t now = fm160_now_ms();

	for (it = items; it->name; it++) {
		if (it->next_ms > now)
			it->next_ms = now;
	}
}

static bool item_enabled(const struct poll_item *it)
{
	if (!it->idle_ms && !it->active_ms)
		return false;
	if (it->active_only && !g_state.foreground)
		return false;
	return true;
}

static void housekeeping(struct uloop_timeout *t)
{
	struct poll_item *it;
	uint64_t now = fm160_now_ms();

	uloop_timeout_set(&housekeep_timer, HOUSEKEEP_MS);

	/* quiet window expiry */
	if (g_state.quiet_kind != QUIET_NONE && now >= g_state.quiet_until_ms) {
		atq_clear_quiet();
		fm160_sched_kick();
	}

	/* foreground decay */
	g_state.foreground = g_state.last_active_ms &&
			     (now - g_state.last_active_ms) < FOREGROUND_HOLD_MS;

	port_discovery_tick();
	fm160_netdev_poll();

	/*
	 * SMS has no poll tier - AT+CPMS? was measured at 10.3 s with no card on
	 * a port where AT+CSQ takes 24 ms, and one such row would starve every
	 * command behind it.  What it does have is a one-time setup, and this is
	 * the only clock it needs: it submits nothing once the setup has
	 * succeeded, and nothing at all after it has been refused its allowed
	 * number of times.
	 */
	fm160_sms_setup_tick();

	/*
	 * M2.  Both of these run BEFORE the "no port" and "suspended" returns
	 * below, and that placement is the whole point for both of them:
	 *
	 *   the dialer - a dial sequence is a sequence, not a poll, so it has
	 *   no tier; and while the link is coming up the circuit breaker may
	 *   well have parked the polls, which must not stop the dial.
	 *
	 *   the mode switch - the state it exists to detect is "there has been
	 *   no AT port for two minutes", so a guard that returned whenever the
	 *   port was missing would make it undetectable.  It is also the only
	 *   user of `pending_since_ms`, which is why it keeps its own clock
	 *   across a daemon restart.
	 */
	fm160_dial_tick();
	fm160_modesw_tick();

	if (now < poll_suspend_until_ms)
		return;
	if (g_state.at_state == 2)
		return;
	if (!g_state.port_found || !g_state.enabled)
		return;

	for (it = items; it->name; it++) {
		int base;

		if (!item_enabled(it))
			continue;
		if (now < it->next_ms)
			continue;

		base = item_base_ms(it);
		/* Do not pile up: if the queue is already busy this tick is
		 * skipped and simply re-armed (counted for diagnostics). */
		if (atq_depth() >= 3) {
			it->skips++;
			it->next_ms = now + (base ? base / 2 : 1000);
			continue;
		}

		it->fires++;
		it->last_fire_ms = now;
		it->fire();
		it->next_ms = now + (base ? base : 1000);
	}

	fm160_state_publish();
}

void fm160_sched_init(void)
{
	struct poll_item *it;

	srand((unsigned)fm160_now_ms());
	for (it = items; it->name; it++)
		item_arm(it);
	poll_suspend_until_ms = 0;

	housekeep_timer.cb = housekeeping;
	uloop_timeout_set(&housekeep_timer, HOUSEKEEP_MS);

	fm160_scan_candidates();
}
