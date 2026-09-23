/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * state.c - state cache, sysfs traffic counters and ubus push.
 *
 * Two things are deliberately kept out of the AT path here:
 *   - byte counters come from /sys/class/net/<if>/statistics (zero AT cost);
 *   - the UI only ever reads the cached snapshot; it can never trigger AT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "fm160d.h"

/*
 * Monotonic milliseconds - the single clock behind every deadline in this
 * daemon (poll jitter, backoff, quiet windows, cache ages).  It deliberately
 * is not the wall clock: an NTP step or a manual `date` would otherwise make a
 * timer fire instantly or hang for hours.  libubox exposes its timers but no
 * public clock helper, so this is a two-line wrapper over CLOCK_MONOTONIC.
 */
uint64_t fm160_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

static bool dirty = true;
static uint64_t netdev_last_ms;
static const char *netdev_candidates[] = { "wwan0", "usb0", "wwan1", NULL };

static int read_u64_file(const char *path, uint64_t *out)
{
	FILE *f = fopen(path, "r");
	char buf[64];

	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	*out = strtoull(buf, NULL, 10);
	return 0;
}

static bool netdev_exists(const char *name)
{
	char path[128];

	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes", name);
	return access(path, R_OK) == 0;
}

void fm160_netdev_poll(void)
{
	char path[160];
	uint64_t now = fm160_now_ms(), dt;
	uint64_t rx, tx;

	if (!g_state.netdev[0]) {
		const char **p;

		for (p = netdev_candidates; *p; p++) {
			if (netdev_exists(*p)) {
				snprintf(g_state.netdev, sizeof(g_state.netdev),
					 "%s", *p);
				fm160_log(LOG_INFO, "traffic counters from %s",
					  g_state.netdev);
				break;
			}
		}
		if (!g_state.netdev[0])
			return;
	}

	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes",
		 g_state.netdev);
	if (read_u64_file(path, &rx))
		return;
	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_bytes",
		 g_state.netdev);
	if (read_u64_file(path, &tx))
		return;

	if (!g_state.rx_bytes_prev && !g_state.tx_bytes_prev) {
		g_state.rx_bytes_prev = rx;
		g_state.tx_bytes_prev = tx;
		netdev_last_ms = now;
		return;
	}

	dt = now - netdev_last_ms;
	if (dt >= 500) {
		uint64_t drx = rx >= g_state.rx_bytes_prev ?
			       rx - g_state.rx_bytes_prev : 0;
		uint64_t dtx = tx >= g_state.tx_bytes_prev ?
			       tx - g_state.tx_bytes_prev : 0;

		g_state.rx_bps = drx * 1000 / dt;
		g_state.tx_bps = dtx * 1000 / dt;
		g_state.rx_bytes_prev = rx;
		g_state.tx_bytes_prev = tx;
		netdev_last_ms = now;
		dirty = true;
	}
	g_state.rx_bytes = rx;
	g_state.tx_bytes = tx;
}

void fm160_state_init(void)
{
	memset(&g_state, 0, sizeof(g_state));
	g_state.usb_mode = -1;
	g_state.creg = FM160_REG_UNKNOWN;
	g_state.cgreg = FM160_REG_UNKNOWN;
	g_state.cereg = FM160_REG_UNKNOWN;
	g_state.c5greg = FM160_REG_UNKNOWN;
	g_state.csq_rssi = 99;
	g_state.csq_ber = 99;
	g_state.cesq_rsrp = 255;
	g_state.cesq_rsrq = 255;
	g_state.tier_scale = 1;
	g_state.enabled = true;

	/* M5.  Every field the modem leaves EMPTY while there is no fix is seeded
	 * with FM160_NONE rather than 0, so that a page CAN tell "0 satellites
	 * used" apart from "the modem had nothing to report" -- see the notes
	 * above struct fm160_gnss_reading. */
	g_state.gnss.cfg.supl_version = FM160_NONE;
	g_state.gnss.cfg.constellation = FM160_NONE;
	g_state.gnss.cfg.cert = FM160_NONE;
	g_state.gnss.cfg.xtra = FM160_NONE;
	g_state.gnss.r.fix_type = FM160_NONE;
	g_state.gnss.r.pdop_x10 = FM160_NONE;
	g_state.gnss.r.hdop_x10 = FM160_NONE;
	g_state.gnss.r.vdop_x10 = FM160_NONE;
	g_state.gnss.r.quality = FM160_NONE;
	g_state.gnss.r.sats_in_use = FM160_NONE;
	g_state.gnss.r.alt_dm = FM160_NONE;
	g_state.gnss.r.geoid_dm = FM160_NONE;
	g_state.gnss.r.speed_cmps = FM160_NONE;
	g_state.gnss.r.course_d10 = FM160_NONE;
	g_state.gnss.r.snr_best_db = FM160_NONE;
	g_state.gnss.r.gsv_trailing_value = FM160_NONE;
	/* visible_total too.  It is a SUM of what the GSV lines claim, so 0 is a
	 * perfectly ordinary measurement indoors - and that is exactly why it must
	 * not also be the value of "no reading has been taken yet".  Left at 0 by
	 * the zero-initialisation, the page says "engine on, 0 satellites in view"
	 * in the window between switching the engine on and the first NMEA tier
	 * answer, which is a claim about the sky that nothing has looked at.  The
	 * fix table already renders the sentinel as '-'; this makes it arrive. */
	g_state.gnss.r.visible_total = FM160_NONE;
	dirty = true;
}

void fm160_state_touch_ok(void)
{
	g_state.last_ok_ms = fm160_now_ms();
}

void fm160_state_mark_dirty(void)
{
	dirty = true;
}

/* ------------------------------------------------------------------ */
/* serialisation                                                        */
/* ------------------------------------------------------------------ */

/*
 * One cell row.  Raw values and decoded values are both exported: the raw ones
 * are what the modem said (useful when a decode rule turns out to be wrong on
 * real hardware), the decoded ones are what the UI shows.
 *
 * Note that blobmsg stores INT32 as a signed 32 bit value on the wire - libubox
 * formats it with PRId32 - so negative dBm values survive the trip to LuCI.
 */
static void cell_to_blob(struct blob_buf *b, const char *name,
			 const struct fm160_cell *c)
{
	void *t = blobmsg_open_table(b, name);

	blobmsg_add_u8(b, "valid", c->valid);
	blobmsg_add_u8(b, "is_service", c->is_service);
	blobmsg_add_u32(b, "rat", c->rat);
	blobmsg_add_u32(b, "mcc", c->mcc);
	blobmsg_add_u32(b, "mnc", c->mnc);
	blobmsg_add_u64(b, "tac", c->tac);
	blobmsg_add_u64(b, "cellid", c->cellid);
	blobmsg_add_u64(b, "earfcn", c->earfcn);
	blobmsg_add_u32(b, "pci", c->pci);
	blobmsg_add_u32(b, "band", c->band);           /* 0 = not reported */
	blobmsg_add_u32(b, "bandwidth", c->bandwidth_mhz);
	blobmsg_add_u32(b, "sinr_db10", c->sinr_db10);
	blobmsg_add_u32(b, "rsrp_dbm", c->rsrp_dbm);
	blobmsg_add_u32(b, "rsrq_db10", c->rsrq_db10);
	blobmsg_add_u32(b, "rxlev_raw", c->rxlev_raw);
	/* raw, for diagnostics */
	blobmsg_add_u32(b, "raw_sinr", c->sinr_raw);
	blobmsg_add_u32(b, "raw_rsrp", c->rsrp_raw);
	blobmsg_add_u32(b, "raw_rsrq", c->rsrq_raw);
	blobmsg_add_u32(b, "raw_bandwidth", c->bandwidth_raw);

	blobmsg_close_table(b, t);
}

/*
 * --- M4 helpers ---------------------------------------------------------
 *
 * A band is three integers, so the band lists are rendered as arrays of
 * tables rather than as strings: LuCI then gets the decoded value without
 * having to re-implement the encoding, which differs per RAT and which
 * fm160_band_decode() already owns.  The raw token travels alongside because
 * the raw form - not the decoded one - is what has to be written back.
 */
static void band_to_blob(struct blob_buf *b, const struct fm160_band *bd)
{
	void *t = blobmsg_open_table(b, NULL);

	blobmsg_add_u32(b, "raw", bd->raw);
	blobmsg_add_u32(b, "band", bd->band);
	blobmsg_add_u32(b, "rat", bd->rat);
	blobmsg_close_table(b, t);
}

static void band_array_to_blob(struct blob_buf *b, const char *name,
			       const struct fm160_band *v, int n)
{
	void *a = blobmsg_open_array(b, name);
	int i;

	for (i = 0; i < n; i++)
		band_to_blob(b, &v[i]);
	blobmsg_close_array(b, a);
}

/* Plain integer list, used for the rat/pref1/pref2 capability groups. */
static void int_array_to_blob(struct blob_buf *b, const char *name,
			      const int *v, int n)
{
	void *a = blobmsg_open_array(b, name);
	int i;

	for (i = 0; i < n; i++)
		blobmsg_add_u32(b, NULL, (uint32_t)v[i]);
	blobmsg_close_array(b, a);
}

/*
 * One carrier-aggregation row.  dl_mod/ul_mod are exported as the raw code
 * (0 BPSK .. 5 1024QAM, 6 unknown) so that a UI which has not been updated
 * still shows something honest.
 */
static void ca_cell_to_blob(struct blob_buf *b, const struct fm160_ca_cell *c)
{
	void *t = blobmsg_open_table(b, NULL);

	blobmsg_add_u8(b, "valid", c->valid);
	blobmsg_add_u8(b, "is_pcc", c->is_pcc);
	blobmsg_add_u32(b, "state", c->state);
	blobmsg_add_u32(b, "band", c->band);
	blobmsg_add_u32(b, "pci", c->pci);
	blobmsg_add_u64(b, "freq", c->freq);
	blobmsg_add_u32(b, "dl_bw_mhz", c->dl_bw_mhz);
	blobmsg_add_u32(b, "ul_bw_mhz", c->ul_bw_mhz);
	blobmsg_add_u32(b, "dl_mimo", c->dl_mimo);
	blobmsg_add_u32(b, "ul_mimo", c->ul_mimo);
	blobmsg_add_u32(b, "dl_mod", c->dl_mod);
	blobmsg_add_u32(b, "ul_mod", c->ul_mod);
	blobmsg_add_u32(b, "rsrp_dbm", c->rsrp_dbm);
	blobmsg_close_table(b, t);
}

struct blob_buf *fm160_state_blob(void)
{
	static struct blob_buf b;
	struct blob_attr *cells;
	void *m4;
	int i, j;

	/* Reusable buffer: free the previous contents before refilling so that
	 * repeated calls do not leak.  blob_buf_free() resets head/buf/buflen,
	 * and blob_buf_init() only fills in .grow, so the two compose safely. */
	if (b.head)
		blob_buf_free(&b);
	blob_buf_init(&b, 0);

	/* --- port / health ------------------------------------------- */
	blobmsg_add_string(&b, "port", g_state.port_found ? g_state.port : "");
	blobmsg_add_u8(&b, "port_found", g_state.port_found);
	blobmsg_add_u8(&b, "enabled", g_state.enabled);
	blobmsg_add_u32(&b, "at_state", g_state.at_state);
	blobmsg_add_u32(&b, "consec_timeout", g_state.consec_timeout);
	blobmsg_add_u32(&b, "worst_response_ms", (uint32_t)g_state.at_busy_max_ms);
	blobmsg_add_u32(&b, "queue_depth", atq_depth());
	blobmsg_add_u8(&b, "foreground", g_state.foreground);
	blobmsg_add_u8(&b, "quiet", atq_quiet_active());
	if (atq_quiet_active()) {
		blobmsg_add_string(&b, "quiet_reason", g_state.quiet_reason);
		blobmsg_add_u32(&b, "quiet_left_ms",
				(uint32_t)(g_state.quiet_until_ms - fm160_now_ms()));
	}
	if (g_state.last_ok_ms)
		blobmsg_add_u32(&b, "last_ok_age_ms",
				(uint32_t)(fm160_now_ms() - g_state.last_ok_ms));

	/* --- identity ------------------------------------------------- */
	blobmsg_add_string(&b, "manufacturer", g_state.manufacturer);
	blobmsg_add_string(&b, "model", g_state.model);
	blobmsg_add_string(&b, "revision", g_state.revision);
	blobmsg_add_string(&b, "imei", g_state.imei);
	blobmsg_add_string(&b, "sn", g_state.sn);
	blobmsg_add_string(&b, "iccid", g_state.iccid);
	blobmsg_add_u8(&b, "ident_done", g_state.ident_done);
	if (g_state.usb_mode >= 0)
		blobmsg_add_u32(&b, "usb_mode", g_state.usb_mode);

	/* --- sim / registration --------------------------------------- */
	blobmsg_add_string(&b, "pin_status", g_state.pin_status);
	blobmsg_add_u32(&b, "cereg", g_state.cereg);
	blobmsg_add_u32(&b, "c5greg", g_state.c5greg);
	blobmsg_add_string(&b, "operator", g_state.oper);

	/* --- signal --------------------------------------------------- */
	{
		void *s = blobmsg_open_table(&b, "csq");

		blobmsg_add_u32(&b, "raw_rssi", g_state.csq_rssi);
		blobmsg_add_u32(&b, "raw_ber", g_state.csq_ber);
		blobmsg_add_u8(&b, "is_ss_rsrp", g_state.csq_is_ss_rsrp);
		/* Only meaningful while the field really is an RSSI. */
		if (!g_state.csq_is_ss_rsrp && g_state.csq_rssi <= 31)
			blobmsg_add_u32(&b, "rssi_dbm",
					(uint32_t)(-113 + 2 * g_state.csq_rssi));
		blobmsg_close_table(&b, s);
	}

	{
		void *c = blobmsg_open_table(&b, "cesq");

		/* decoded */
		blobmsg_add_u32(&b, "geran_rssi_dbm", (uint32_t)g_state.geran_rssi_dbm);
		blobmsg_add_u32(&b, "utra_rscp_dbm", (uint32_t)g_state.utra_rscp_dbm);
		blobmsg_add_u32(&b, "utra_ecno_db10", (uint32_t)g_state.utra_ecno_db10);
		blobmsg_add_u32(&b, "lte_rsrp_dbm", (uint32_t)g_state.lte_rsrp_dbm);
		blobmsg_add_u32(&b, "lte_rsrq_db10", (uint32_t)g_state.lte_rsrq_db10);
		blobmsg_add_u32(&b, "nr_ss_rsrp_dbm", (uint32_t)g_state.nr_ss_rsrp_dbm);
		blobmsg_add_u32(&b, "nr_ss_rsrq_db10", (uint32_t)g_state.nr_ss_rsrq_db10);
		blobmsg_add_u32(&b, "nr_ss_sinr_db10", (uint32_t)g_state.nr_ss_sinr_db10);
		/* raw, as reported by the modem (255 = not applicable) */
		blobmsg_add_u32(&b, "raw_rxlev", g_state.cesq_rxlev);
		blobmsg_add_u32(&b, "raw_ber", g_state.cesq_ber);
		blobmsg_add_u32(&b, "raw_rscp", g_state.cesq_rscp);
		blobmsg_add_u32(&b, "raw_ecno", g_state.cesq_ecno);
		blobmsg_add_u32(&b, "raw_rsrq", g_state.cesq_rsrq);
		blobmsg_add_u32(&b, "raw_rsrp", g_state.cesq_rsrp);
		blobmsg_add_u32(&b, "raw_ss_rsrq", g_state.cesq_ss_rsrq);
		blobmsg_add_u32(&b, "raw_ss_rsrp", g_state.cesq_ss_rsrp);
		blobmsg_add_u32(&b, "raw_ss_sinr", g_state.cesq_ss_sinr);
		blobmsg_close_table(&b, c);
	}

	/* --- cells ---------------------------------------------------- */
	/* serving is the headline cell (highest RAT).  In EN-DC the modem reports
	 * two service rows; the LTE anchor ends up in serving2. */
	blobmsg_add_u8(&b, "cell_valid", g_state.serving.valid);
	blobmsg_add_u8(&b, "cell2_valid", g_state.serving2.valid);
	cell_to_blob(&b, "serving", &g_state.serving);
	cell_to_blob(&b, "serving2", &g_state.serving2);
	if (g_state.cell_last_ok_ms)
		blobmsg_add_u32(&b, "cell_age_ms",
				(uint32_t)(fm160_now_ms() - g_state.cell_last_ok_ms));

	cells = blobmsg_open_array(&b, "neighbours");
	for (i = 0; i < g_state.neigh_count; i++)
		cell_to_blob(&b, NULL, &g_state.neigh[i]);
	blobmsg_close_array(&b, cells);

	/* --- M4: band lock / cell lock / carrier aggregation ----------- */
	/*
	 * CONVENTION, and it is not the obvious one: blobmsg_open_table() /
	 * blobmsg_open_array() return a HANDLE that is only ever handed back to
	 * blobmsg_close_*().  Every value and every nested open still goes into the
	 * same struct blob_buf - &b - because that is where libubox tracks the
	 * current head.  Passing the handle instead (blobmsg_add_u32(&t, ...))
	 * compiles perfectly and then dereferences a void* as a blob_buf, which on
	 * this device was a SIGSEGV inside fm160_state_publish() every time the
	 * identity chain finished.
	 */
	m4 = blobmsg_open_table(&b, "m4");

	/* AT+GTACT? - the restriction actually in force right now. */
	{
		void *g = blobmsg_open_table(&b, "bands");

		blobmsg_add_u8(&b, "valid", g_state.gtact.valid);
		blobmsg_add_u32(&b, "rat", (uint32_t)g_state.gtact.rat);
		blobmsg_add_u32(&b, "pref1", (uint32_t)g_state.gtact.pref1);
		blobmsg_add_u32(&b, "pref2", (uint32_t)g_state.gtact.pref2);
		/* Bands the modem listed that match no documented encoding.
		 * Counted, never guessed - a non-zero value means the firmware
		 * is not the one the parser was written against. */
		blobmsg_add_u32(&b, "unknown", (uint32_t)g_state.gtact.unknown_n);
		blobmsg_add_u8(&b, "auto_seen", g_state.gtact.auto_seen);
		band_array_to_blob(&b, "umts", g_state.gtact.umts, g_state.gtact.umts_n);
		band_array_to_blob(&b, "lte", g_state.gtact.lte, g_state.gtact.lte_n);
		band_array_to_blob(&b, "nr", g_state.gtact.nr, g_state.gtact.nr_n);
		if (g_state.gtact.last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - g_state.gtact.last_ok_ms);
		blobmsg_close_table(&b, g);
	}

	/* AT+GTACT=? - what the modem says it supports.  This is the licence
	 * for the write path: an invalid caps set means the UI must refuse to
	 * offer band editing rather than let the user guess. */
	{
		void *g = blobmsg_open_table(&b, "band_caps");
		void *gn;

		blobmsg_add_u8(&b, "valid", g_state.gtact_caps.valid);
		int_array_to_blob(&b, "rat", g_state.gtact_caps.rat,
				  g_state.gtact_caps.rat_n);
		int_array_to_blob(&b, "pref1", g_state.gtact_caps.pref1,
				  g_state.gtact_caps.pref1_n);
		int_array_to_blob(&b, "pref2", g_state.gtact_caps.pref2,
				  g_state.gtact_caps.pref2_n);
		band_array_to_blob(&b, "umts", g_state.gtact_caps.umts,
				   g_state.gtact_caps.umts_n);
		band_array_to_blob(&b, "lte", g_state.gtact_caps.lte,
				   g_state.gtact_caps.lte_n);
		band_array_to_blob(&b, "nr", g_state.gtact_caps.nr,
				   g_state.gtact_caps.nr_n);
		/* Which of the nine groups the modem actually filled in.  On
		 * FM160-CN the gsm, cdma and evdo groups come back empty. */
		gn = blobmsg_open_array(&b, "group_nonempty");
		for (i = 0; i < FM160_GTACT_GROUPS; i++)
			blobmsg_add_u8(&b, NULL, g_state.gtact_caps.group_nonempty[i]);
		blobmsg_close_array(&b, gn);
		if (g_state.gtact_caps.last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - g_state.gtact_caps.last_ok_ms);
		blobmsg_close_table(&b, g);
	}

	/* AT+GTCELLLOCK? - persistent, and only takes effect after a UE reset
	 * that fm160d deliberately never performs itself. */
	{
		void *c = blobmsg_open_table(&b, "celllock");

		blobmsg_add_u8(&b, "valid", g_state.celllock.valid);
		blobmsg_add_u8(&b, "enabled", g_state.celllock.enabled);
		blobmsg_add_u32(&b, "rat", (uint32_t)g_state.celllock.rat);
		blobmsg_add_u32(&b, "type", (uint32_t)g_state.celllock.type);
		blobmsg_add_u64(&b, "earfcn", g_state.celllock.earfcn);
		blobmsg_add_u32(&b, "pci", (uint32_t)g_state.celllock.pci);
		blobmsg_add_u32(&b, "scs", (uint32_t)g_state.celllock.scs);
		blobmsg_add_u32(&b, "nrband", (uint32_t)g_state.celllock.nrband);
		blobmsg_add_u8(&b, "has_pci", g_state.celllock.has_pci);
		blobmsg_add_u8(&b, "has_scs", g_state.celllock.has_scs);
		blobmsg_add_u8(&b, "has_nrband", g_state.celllock.has_nrband);
		if (g_state.celllock.last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - g_state.celllock.last_ok_ms);
		blobmsg_close_table(&b, c);
	}

	/* AT+GTCELLLOCK=? - ranges, not enumerations. */
	{
		void *c = blobmsg_open_table(&b, "celllock_caps");
		void *a;
		bool undocumented = false;

		blobmsg_add_u8(&b, "valid", g_state.celllock_caps.valid);
		a = blobmsg_open_array(&b, "mode");
		for (i = 0; i < g_state.celllock_caps.mode_n; i++) {
			blobmsg_add_u32(&b, NULL,
					(uint32_t)g_state.celllock_caps.mode[i]);
			/* The live modem offers a third mode value that the
			 * manual does not define.  Recorded as evidence only;
			 * fm160_celllock_command() refuses to write anything
			 * other than 0 or 1. */
			if (g_state.celllock_caps.mode[i] > 1)
				undocumented = true;
		}
		blobmsg_close_array(&b, a);
		blobmsg_add_u8(&b, "mode_undocumented", undocumented);
		blobmsg_add_u32(&b, "rat_min", (uint32_t)g_state.celllock_caps.rat_min);
		blobmsg_add_u32(&b, "rat_max", (uint32_t)g_state.celllock_caps.rat_max);
		a = blobmsg_open_array(&b, "type");
		for (i = 0; i < g_state.celllock_caps.type_n; i++)
			blobmsg_add_u32(&b, NULL,
					(uint32_t)g_state.celllock_caps.type[i]);
		blobmsg_close_array(&b, a);
		/* u64, not u32: blobmsg formats an INT32 as a SIGNED int32, so
		 * this value - the top of the range the modem advertises - comes
		 * back as -1 on the wire.  It really is 4294967295. */
		blobmsg_add_u64(&b, "earfcn_max", g_state.celllock_caps.earfcn_max);
		blobmsg_add_u32(&b, "pci_max", (uint32_t)g_state.celllock_caps.pci_max);
		blobmsg_add_u32(&b, "scs_min", (uint32_t)g_state.celllock_caps.scs_min);
		blobmsg_add_u32(&b, "scs_max", (uint32_t)g_state.celllock_caps.scs_max);
		blobmsg_add_u32(&b, "nrband_min", (uint32_t)g_state.celllock_caps.nrband_min);
		blobmsg_add_u32(&b, "nrband_max", (uint32_t)g_state.celllock_caps.nrband_max);
		if (g_state.celllock_caps.last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - g_state.celllock_caps.last_ok_ms);
		blobmsg_close_table(&b, c);
	}

	/* AT+GTCAINFO? - "valid" is false both when the modem is idle (a bare
	 * OK, measured with no SIM) and when the read failed; ca_age_ms only
	 * appears once a PCC row has actually been seen. */
	{
		void *c = blobmsg_open_table(&b, "ca");
		void *a;

		blobmsg_add_u8(&b, "valid", g_state.ca.valid);
		blobmsg_add_u32(&b, "rat", (uint32_t)g_state.ca.rat);
		blobmsg_add_u8(&b, "has_nr", g_state.ca.has_nr);
		if (g_state.ca.valid) {
			void *p = blobmsg_open_table(&b, "pcc");

			blobmsg_add_u32(&b, "state", g_state.ca.pcc.state);
			blobmsg_add_u32(&b, "band", g_state.ca.pcc.band);
			blobmsg_add_u32(&b, "pci", g_state.ca.pcc.pci);
			blobmsg_add_u64(&b, "freq", g_state.ca.pcc.freq);
			blobmsg_add_u32(&b, "dl_bw_mhz", g_state.ca.pcc.dl_bw_mhz);
			blobmsg_add_u32(&b, "ul_bw_mhz", g_state.ca.pcc.ul_bw_mhz);
			blobmsg_add_u32(&b, "dl_mimo", g_state.ca.pcc.dl_mimo);
			blobmsg_add_u32(&b, "ul_mimo", g_state.ca.pcc.ul_mimo);
			blobmsg_add_u32(&b, "dl_mod", g_state.ca.pcc.dl_mod);
			blobmsg_add_u32(&b, "ul_mod", g_state.ca.pcc.ul_mod);
			blobmsg_add_u32(&b, "rsrp_dbm", g_state.ca.pcc.rsrp_dbm);
			blobmsg_close_table(&b, p);
		}
		a = blobmsg_open_array(&b, "scc");
		for (j = 0; j < g_state.ca.scc_n; j++)
			ca_cell_to_blob(&b, &g_state.ca.scc[j]);
		blobmsg_close_array(&b, a);
		if (g_state.ca.last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - g_state.ca.last_ok_ms);
		blobmsg_close_table(&b, c);
	}

	blobmsg_close_table(&b, m4);

	/* --- M5: GNSS -------------------------------------------------- */
	/*
	 * Published as its own top-level table rather than grouped by milestone
	 * like "m4": GNSS is a subsystem with a switch, a per-read picture and its
	 * own write gate, not three loosely related settings on one screen.
	 *
	 * Two conventions matter to whoever reads this blob from JavaScript:
	 *
	 *   - FM160_NONE (-1000000) goes out through blobmsg_add_u32/u64, so it
	 *     arrives as 4293967296 / 18446744073708551616 rather than as a
	 *     negative number.  api.js's reported() exists for exactly that.  It is
	 *     deliberately NOT used for lat_1e7/lon_1e7: -0.1 degrees is a legal
	 *     coordinate, so has_position is the only valid gate there, and those
	 *     two fields are also signed - the same u32 round trip turns a western
	 *     longitude into a large positive number, which api.gnssCoord()
	 *     undoes.
	 *
	 *   - "empty" and "zero" are different answers everywhere below.  Zero
	 *     satellites in view is a measurement; an absent DOP or elevation is
	 *     the absence of one.  A page that renders both as "0" would be
	 *     inventing data.
	 */
	{
		void *g = blobmsg_open_table(&b, "gnss");
		const struct fm160_gnss_reading *gr = &g_state.gnss.r;
		char ns[2] = { gr->lat_ns, '\0' };
		char ew[2] = { gr->lon_ew, '\0' };
		void *t;
		int k;

		t = blobmsg_open_table(&b, "engine");
		blobmsg_add_u8(&b, "known", g_state.gnss.engine.known);
		blobmsg_add_u8(&b, "on", g_state.gnss.engine.on);
		blobmsg_add_u8(&b, "autostart", g_state.gnss.autostart);
		if (g_state.gnss.engine.last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - g_state.gnss.engine.last_ok_ms);
		blobmsg_close_table(&b, t);

		/* The last NMEA read, and how much of it made sense. */
		t = blobmsg_open_table(&b, "read");
		blobmsg_add_u32(&b, "sentences", (uint32_t)gr->sentences);
		blobmsg_add_u32(&b, "nmea_bytes", (uint32_t)gr->nmea_bytes);
		blobmsg_add_u32(&b, "resp_bytes", (uint32_t)gr->resp_bytes);
		blobmsg_add_u32(&b, "bad_checksum", (uint32_t)gr->bad_checksum);
		blobmsg_add_u32(&b, "bad_shape", (uint32_t)gr->bad_shape);
		blobmsg_add_u32(&b, "ignored", (uint32_t)gr->ignored);
		blobmsg_add_u8(&b, "empty_frame", gr->empty_frame);
		/* Empty answers in a row.  "Engine on" + a climbing count is a real
		 * fault; one or two right after the engine is switched on is the
		 * measured normal.  empty_ms is when the last of them arrived, and
		 * age_ms above is the age of the PICTURE - the two differ on
		 * purpose, see struct fm160_gnss_reading. */
		blobmsg_add_u32(&b, "empty_frames", (uint32_t)gr->empty_frames);
		blobmsg_add_u32(&b, "empty_bytes", (uint32_t)gr->empty_bytes);
		if (gr->empty_ms)
			blobmsg_add_u64(&b, "empty_age_ms",
					fm160_now_ms() - gr->empty_ms);
		blobmsg_add_u8(&b, "read_error", gr->read_error);
		/* The modem emits one field past the last GSV quad group.  Recorded
		 * as evidence, never interpreted - see fm160_parse_gnss(). */
		blobmsg_add_u8(&b, "gsv_trailing", gr->gsv_trailing);
		if (gr->gsv_trailing_value != FM160_NONE)
			blobmsg_add_u32(&b, "gsv_trailing_value",
					(uint32_t)gr->gsv_trailing_value);
		if (gr->read_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - gr->read_ms);
		blobmsg_add_u32(&b, "raw_len", (uint32_t)gr->raw_len);
		blobmsg_add_string(&b, "raw", gr->raw);
		blobmsg_close_table(&b, t);

		t = blobmsg_open_table(&b, "fix");
		blobmsg_add_u8(&b, "has_position", gr->has_position);
		/* 'A'/'M' as the raw character code, 0 when no GSA arrived. */
		blobmsg_add_u32(&b, "fix_mode", (uint32_t)gr->fix_mode);
		blobmsg_add_u32(&b, "fix_type", (uint32_t)gr->fix_type);
		blobmsg_add_u32(&b, "sats_used", (uint32_t)gr->sats_used);
		blobmsg_add_u32(&b, "sats_in_use", (uint32_t)gr->sats_in_use);
		blobmsg_add_u32(&b, "quality", (uint32_t)gr->quality);
		blobmsg_add_u32(&b, "visible", (uint32_t)gr->visible_total);
		blobmsg_add_u32(&b, "snr_best_db", (uint32_t)gr->snr_best_db);
		blobmsg_add_u32(&b, "pdop_x10", (uint32_t)gr->pdop_x10);
		blobmsg_add_u32(&b, "hdop_x10", (uint32_t)gr->hdop_x10);
		blobmsg_add_u32(&b, "vdop_x10", (uint32_t)gr->vdop_x10);
		blobmsg_add_u32(&b, "lat_1e7", (uint32_t)gr->lat_1e7);
		blobmsg_add_u32(&b, "lon_1e7", (uint32_t)gr->lon_1e7);
		if (gr->lat_ns)
			blobmsg_add_string(&b, "lat_ns", ns);
		if (gr->lon_ew)
			blobmsg_add_string(&b, "lon_ew", ew);
		blobmsg_add_u32(&b, "alt_dm", (uint32_t)gr->alt_dm);
		blobmsg_add_u32(&b, "geoid_dm", (uint32_t)gr->geoid_dm);
		blobmsg_add_u32(&b, "speed_cmps", (uint32_t)gr->speed_cmps);
		blobmsg_add_u32(&b, "course_d10", (uint32_t)gr->course_d10);
		blobmsg_add_string(&b, "utc", gr->utc);
		blobmsg_add_string(&b, "date", gr->date);
		blobmsg_close_table(&b, t);

		/* One row per constellation: what the modem CLAIMS (visible) next to
		 * what it actually showed (parsed), so a truncated response is
		 * visible as a difference instead of as a smaller list. */
		t = blobmsg_open_array(&b, "constellations");
		for (k = 0; k < g_state.gnss.r.cons_n; k++) {
			const struct fm160_gnss_const *c = &g_state.gnss.r.cons[k];
			void *row = blobmsg_open_table(&b, NULL);

			blobmsg_add_string(&b, "talker", c->talker);
			blobmsg_add_u32(&b, "kind", (uint32_t)c->kind);
			blobmsg_add_u32(&b, "visible", (uint32_t)c->visible);
			blobmsg_add_u32(&b, "parsed", (uint32_t)c->parsed);
			blobmsg_add_u32(&b, "in_fix", (uint32_t)c->in_fix);
			blobmsg_add_u32(&b, "snr_best_db", (uint32_t)c->snr_best_db);
			blobmsg_close_table(&b, row);
		}
		blobmsg_close_array(&b, t);

		t = blobmsg_open_array(&b, "satellites");
		for (k = 0; k < g_state.gnss.r.sats_n; k++) {
			const struct fm160_gnss_sat *s = &g_state.gnss.r.sats[k];
			const char *talker = "";
			void *row;

			if (s->const_idx >= 0 && s->const_idx < g_state.gnss.r.cons_n)
				talker = g_state.gnss.r.cons[s->const_idx].talker;
			row = blobmsg_open_table(&b, NULL);
			blobmsg_add_string(&b, "talker", talker);
			blobmsg_add_u32(&b, "prn", (uint32_t)s->prn);
			blobmsg_add_u32(&b, "elev_deg", (uint32_t)s->elev_deg);
			blobmsg_add_u32(&b, "azim_deg", (uint32_t)s->azim_deg);
			blobmsg_add_u32(&b, "snr_db", (uint32_t)s->snr_db);
			blobmsg_add_u8(&b, "in_fix", s->in_fix);
			blobmsg_close_table(&b, row);
		}
		blobmsg_close_array(&b, t);

		/* AT+GTGPSCFG? - the stored configuration, x by x. */
		t = blobmsg_open_table(&b, "config");
		blobmsg_add_u8(&b, "valid", g_state.gnss.cfg.valid);
		blobmsg_add_u32(&b, "constellation",
				(uint32_t)g_state.gnss.cfg.constellation);
		blobmsg_add_u32(&b, "supl_version",
				(uint32_t)g_state.gnss.cfg.supl_version);
		blobmsg_add_u32(&b, "cert", (uint32_t)g_state.gnss.cfg.cert);
		/* x=1 is MISSING on this firmware; the flag says which of "the
		 * modem did not report it" and "the modem reported 0" is true. */
		blobmsg_add_u8(&b, "xtra_present", g_state.gnss.cfg.xtra_present);
		blobmsg_add_u32(&b, "xtra", (uint32_t)g_state.gnss.cfg.xtra);
		blobmsg_add_u32(&b, "unknown", (uint32_t)g_state.gnss.cfg.unknown);
		if (g_state.gnss.cfg.last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - g_state.gnss.cfg.last_ok_ms);
		blobmsg_close_table(&b, t);

		/* AT+GTGPSCFG=? - the licence for the constellation write.  A page
		 * that ignores this will offer a button the daemon refuses. */
		t = blobmsg_open_table(&b, "config_caps");
		blobmsg_add_u8(&b, "valid", g_state.gnss.cfg.caps_valid);
		blobmsg_add_u32(&b, "groups", (uint32_t)g_state.gnss.cfg.caps_groups);
		blobmsg_add_u32(&b, "write_x", (uint32_t)FM160_GNSS_CFG_X_WRITE);
		blobmsg_add_u32(&b, "x_mask", (uint32_t)g_state.gnss.cfg.caps_x_mask);
		/* "values" is the x=2 set, which is the one a write is checked
		 * against.  The other fields' sets follow in "by_x" because they
		 * genuinely differ - x=0 is (0-2) on this modem - and a page that
		 * displayed their union as the constellation's list would be
		 * quoting some other field's answer. */
		{
			void *arr = blobmsg_open_array(&b, "values");

			for (k = 0;
			     k < g_state.gnss.cfg.caps_n[FM160_GNSS_CFG_X_WRITE];
			     k++)
				blobmsg_add_u32(&b, NULL, (uint32_t)
						g_state.gnss.cfg.caps_values
						[FM160_GNSS_CFG_X_WRITE][k]);
			blobmsg_close_array(&b, arr);
		}
		{
			void *arr = blobmsg_open_array(&b, "by_x");
			int x;

			for (x = 0; x < FM160_GNSS_CFG_SLOTS; x++) {
				void *row, *v;
				int n = g_state.gnss.cfg.caps_n[x];

				if (!n)
					continue;
				row = blobmsg_open_table(&b, NULL);
				blobmsg_add_u32(&b, "x", (uint32_t)x);
				v = blobmsg_open_array(&b, "values");
				for (k = 0; k < n; k++)
					blobmsg_add_u32(&b, NULL, (uint32_t)
							g_state.gnss.cfg
							.caps_values[x][k]);
				blobmsg_close_array(&b, v);
				blobmsg_close_table(&b, row);
			}
			blobmsg_close_array(&b, arr);
		}
		blobmsg_close_table(&b, t);

		/* AGPS, read-only in this milestone. */
		t = blobmsg_open_table(&b, "agps");
		blobmsg_add_u8(&b, "valid", g_state.gnss.agps.valid);
		blobmsg_add_u32(&b, "epo", (uint32_t)g_state.gnss.agps.epo);
		blobmsg_add_string(&b, "server", g_state.gnss.agps.server);
		blobmsg_add_u32(&b, "port", (uint32_t)g_state.gnss.agps.port);
		blobmsg_close_table(&b, t);

		blobmsg_close_table(&b, g);
	}

	/* --- M3: SMS -------------------------------------------------- */
	/*
	 * Only the status is published here, never the messages themselves.  A
	 * list of 64 messages each carrying up to a kilobyte of text would be
	 * re-serialised on every snapshot push to answer a question nobody is
	 * asking; the page that wants them calls "fm160.sms_list", which builds
	 * the list once, on demand, and can say how many it is sending.
	 */
	{
		const struct fm160_sms_status *s = &g_state.sms.st;
		void *t, *u;

		t = blobmsg_open_table(&b, "sms");
		blobmsg_add_u8(&b, "probed", s->probed);
		blobmsg_add_u8(&b, "usable", s->usable);
		blobmsg_add_u8(&b, "setup_done", g_state.sms.setup_done);
		/* -1 means "never read".  Sent as a flag as well, because
		 * blobmsg has no signed integer and 4294967295 on its own reads
		 * like a nonsense mode rather than a missing answer. */
		blobmsg_add_u8(&b, "cmgf_known", g_state.sms.cmgf >= 0);
		blobmsg_add_u32(&b, "cmgf", (uint32_t)g_state.sms.cmgf);
		blobmsg_add_string(&b, "storage", s->mem);
		blobmsg_add_u32(&b, "used", (uint32_t)s->used);
		blobmsg_add_u32(&b, "total", (uint32_t)s->total);
		blobmsg_add_u32(&b, "kept", (uint32_t)g_state.sms.count);
		blobmsg_add_u32(&b, "unread", (uint32_t)s->unread);
		blobmsg_add_u32(&b, "received", (uint32_t)s->received);
		blobmsg_add_u32(&b, "duplicates", (uint32_t)s->duplicates);
		blobmsg_add_u32(&b, "last_error", (uint32_t)s->last_error);
		blobmsg_add_string(&b, "last_error_text", s->last_error_text);
		blobmsg_add_u8(&b, "busy", g_state.sms.busy);
		if (s->last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - s->last_ok_ms);

		/* The zero-length-packet evidence (PLAN §4).  A long PDU that
		 * times out is the symptom; these are the counts that decide
		 * whether a kernel workaround is warranted. */
		u = blobmsg_open_table(&b, "counters");
		blobmsg_add_u32(&b, "sent_ok", (uint32_t)s->sent_ok);
		blobmsg_add_u32(&b, "sent_fail", (uint32_t)s->sent_fail);
		blobmsg_add_u32(&b, "sent_timeout", (uint32_t)s->sent_timeout);
		blobmsg_add_u32(&b, "long_sent", (uint32_t)s->long_sent);
		blobmsg_add_u32(&b, "long_timeout", (uint32_t)s->long_timeout);
		blobmsg_close_table(&b, u);

		u = blobmsg_open_table(&b, "last_send");
		blobmsg_add_string(&b, "command", g_state.sms.last_send_cmd);
		blobmsg_add_u32(&b, "segments",
				(uint32_t)g_state.sms.last_segments);
		blobmsg_add_u32(&b, "pdu_chars",
				(uint32_t)strlen(g_state.sms.last_pdu));
		blobmsg_add_u32(&b, "mr", (uint32_t)g_state.sms.sent_mr);
		blobmsg_close_table(&b, u);

		blobmsg_close_table(&b, t);
	}

	/* --- M2: the data plane and the USB profile switch -------------- */
	/*
	 * Two tables, and they are kept apart for the same reason the structs
	 * are: "how the link is dialled" and "which physical profile the modem
	 * is in" fail differently, and only the second can take the management
	 * channel away.
	 *
	 * What is NOT here: the list of profiles the modem offers.  That is
	 * static data (fourteen rows) and this snapshot is rebuilt whenever a
	 * sysfs counter moves - about twice a second.  The page asks for it
	 * once, through fm160.profiles, exactly as it does for the SMS list.
	 */
	{
		const struct fm160_dial_state *d = &g_state.dial;
		void *t = blobmsg_open_table(&b, "dial");

		blobmsg_add_u8(&b, "wanted", d->wanted);
		blobmsg_add_string(&b, "step", fm160_net_step_name(d->step));
		blobmsg_add_u32(&b, "step_raw", (uint32_t)d->step);
		blobmsg_add_string(&b, "kind", fm160_dial_kind_name(d->kind));
		blobmsg_add_u8(&b, "kind_known", d->kind_known);
		blobmsg_add_u8(&b, "usable", fm160_dial_usable());
		blobmsg_add_u32(&b, "cid", (uint32_t)d->cid);
		blobmsg_add_string(&b, "pdp", fm160_net_pdp_name(d->pdp));
		blobmsg_add_string(&b, "apn", d->apn);
		blobmsg_add_u8(&b, "up", d->up);
		blobmsg_add_string(&b, "address", d->addr);
		blobmsg_add_string(&b, "dns1", d->dns1);
		blobmsg_add_string(&b, "dns2", d->dns2);
		blobmsg_add_string(&b, "pdp_in_use", d->pdp_in_use);
		/* -1 means "not probed yet", which is a different statement from
		 * either verb being wrong - the vendor's two documents disagree
		 * (net.h), so this is a fact about the unit. */
		blobmsg_add_u8(&b, "verb_known", d->verb >= 0);
		blobmsg_add_string(&b, "verb", d->verb >= 0 ?
				   fm160_net_verb_name((enum fm160_net_verb)d->verb) : "");
		blobmsg_add_u32(&b, "attempt", (uint32_t)d->attempt);
		blobmsg_add_u32(&b, "ladder_len",
				(uint32_t)fm160_net_ladder_len());
		blobmsg_add_u32(&b, "ip_polls", (uint32_t)d->ip_polls);
		blobmsg_add_u8(&b, "running", d->running);
		blobmsg_add_u8(&b, "sim_ready", d->sim_ready);
		blobmsg_add_u8(&b, "was_up", d->was_up);
		blobmsg_add_u8(&b, "allow_reset", d->allow_reset);
		blobmsg_add_u8(&b, "autostart", d->autostart);
		blobmsg_add_u8(&b, "config_error", d->config_error);
		blobmsg_add_u8(&b, "healing_stopped", d->healing_stopped);
		if (d->next_try_ms != UINT64_MAX && d->next_try_ms > fm160_now_ms())
			blobmsg_add_u64(&b, "next_try_in_ms",
					d->next_try_ms - fm160_now_ms());
		blobmsg_add_u32(&b, "starts", (uint32_t)d->starts);
		blobmsg_add_u32(&b, "failures", (uint32_t)d->failures);
		blobmsg_add_string(&b, "last_error", d->last_error);
		if (d->last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - d->last_ok_ms);

		/* DESIGN 4.3's evidence, exported because the number is the
		 * difference between "we keep trying" and "we stopped on
		 * purpose" - and because a reset ledger the page cannot show
		 * is a reset ledger nobody can audit. */
		{
			void *r = blobmsg_open_table(&b, "resets");

			blobmsg_add_u32(&b, "in_window",
					(uint32_t)d->resets_in_window);
			blobmsg_add_u32(&b, "limit",
					(uint32_t)FM160_NET_RESET_LIMIT);
			blobmsg_add_u32(&b, "window_s",
					(uint32_t)FM160_NET_RESET_WINDOW_S);
			blobmsg_add_u32(&b, "total", (uint32_t)d->resets_total);
			blobmsg_close_table(&b, r);
		}
		blobmsg_close_table(&b, t);
	}

	{
		const struct fm160_modesw *m = &g_state.modesw;
		static const char *st_names[] = {
			"idle", "reading the profile list", "applying",
			"verifying", "stuck",
		};
		void *t = blobmsg_open_table(&b, "modesw");
		int i;

		blobmsg_add_string(&b, "state",
				   (m->st >= 0 && (size_t)m->st <
				    ARRAY_SIZE(st_names)) ? st_names[m->st] : "?");
		blobmsg_add_u32(&b, "state_raw", (uint32_t)m->st);
		blobmsg_add_u8(&b, "current_known", m->current >= 0);
		blobmsg_add_u32(&b, "current", (uint32_t)m->current);
		blobmsg_add_u32(&b, "target", (uint32_t)m->target);
		blobmsg_add_u32(&b, "rollback_mode", (uint32_t)m->rollback_mode);
		blobmsg_add_u8(&b, "caps_valid", m->caps_valid);
		{
			void *s = blobmsg_open_array(&b, "supported");

			for (i = 0; i < m->supported_n; i++)
				blobmsg_add_u32(&b, NULL, (uint32_t)m->supported[i]);
			blobmsg_close_array(&b, s);
		}
		blobmsg_add_u8(&b, "pending", m->pending);
		if (m->pending)
			blobmsg_add_u64(&b, "pending_for_ms",
					fm160_now_ms() - m->pending_since_ms);
		blobmsg_add_u32(&b, "verify_result", (uint32_t)m->verify_result);
		blobmsg_add_u8(&b, "rolled_back", m->rolled_back);
		blobmsg_add_u8(&b, "busy", m->busy);
		blobmsg_add_string(&b, "last_error", m->last_error);
		if (m->last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - m->last_ok_ms);
		blobmsg_close_table(&b, t);
	}

	/* --- traffic -------------------------------------------------- */
	{
		void *t = blobmsg_open_table(&b, "traffic");

		blobmsg_add_string(&b, "netdev", g_state.netdev);
		blobmsg_add_u64(&b, "rx_bytes", g_state.rx_bytes);
		blobmsg_add_u64(&b, "tx_bytes", g_state.tx_bytes);
		blobmsg_add_u64(&b, "rx_bps", g_state.rx_bps);
		blobmsg_add_u64(&b, "tx_bps", g_state.tx_bps);
		blobmsg_close_table(&b, t);
	}

	return &b;
}

void fm160_state_publish(void)
{
	struct blob_buf *b;

	if (!dirty || !g_ubus)
		return;
	b = fm160_state_blob();
	ubus_send_event(g_ubus, "fm160.state", b->head);
	blob_buf_free(b);
	dirty = false;
}
