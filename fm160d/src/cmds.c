/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmds.c - FM160 command layer: builders and response parsers.
 *
 * Command set and semantics come from the Fibocom documents; see
 * docs/AT-FACTS.md for the page references behind every choice here.
 *
 * Nothing in this file ever hardcodes a modem-reported capability: enumerations
 * that the manual calls "device dependent" (USB modes, bands) are always read
 * back with the =? / ? forms before being used.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "fm160d.h"
#include "usbmode.h"

/* ------------------------------------------------------------------ */
/* small CSV helpers                                                    */
/* ------------------------------------------------------------------ */

static int csv_int(const char *s, int idx, int fallback)
{
	const char *p = s;
	int i = 0;

	while (i < idx) {
		p = strchr(p, ',');
		if (!p)
			return fallback;
		p++;
		i++;
	}
	if (!*p)
		return fallback;
	return atoi(p);
}

/* Pointer to field <idx> of a CSV line, or "" when the line is shorter. */
static const char *csv_at(const char *s, int idx)
{
	const char *p = s;
	int i = 0;

	if (!s)
		return "";
	while (i < idx) {
		p = strchr(p, ',');
		if (!p)
			return "";
		p++;
		i++;
	}
	return p;
}

/* <tac>, <cell_id>, <earfcn> and <physicalcellId> are printed as hex with
 * no 0x prefix; everything else in the same row is decimal.
 *
 *   AT+GTCCINFO?  ->  1,4,460,01,4F50,52BF337,672,1E4,103,100,44,63,63,24
 *                     TAC 0x4F50 = 20304, cell 0x52BF337 = 86725431,
 *                     EARFCN 0x672 = 1650, PCI 0x1E4 = 484
 *
 * (measured on FM160-CN 89614.1000.00.04.01.02).  The manual never states the
 * radix, but its "range is 0-0xFFFFFFF / 0-0xFFFFFFFF" wording gives it away,
 * and a decimal parse of "1E4" yields 1, which is how this was caught. */
static long long csv_hex_ll(const char *s, int idx, long long fallback)
{
	const char *p = s;
	int i = 0;

	while (i < idx) {
		p = strchr(p, ',');
		if (!p)
			return fallback;
		p++;
		i++;
	}
	if (!*p)
		return fallback;
	return strtoll(p, NULL, 16);
}

/*
 * Band number decoding.
 *
 * AT+GTACT (AT manual 9.1.14) does NOT use a uniform offset scheme; the
 * <lte_band>/<nr_band> values are built by prefixing a RAT prefix to the band
 * number, which makes them look like decimal numbers:
 *
 *   101     BAND_LTE_1        100 + n                  -> 101 .. 164
 *   501     BAND_NR_1         501 .. 509   (n = 1 digit)
 *   5010    BAND_NR_10        the manual prints 5010, i.e. NOT 500 + 10
 *   5078    BAND_NR_78        (manual example: AT+GTACT=,,,103,5078)
 *   50512   BAND_NR_512       -> three-digit bands are 50100 .. 50512
 *
 * A naive `band - 500` therefore turns NR n78 into 4578.  AT+GTCCINFO? names
 * the same enum constants (BAND_LTE_1..BAND_LTE_64, BAND_NR_1..BAND_NR_512).
 *
 * Hardware confirms both commands use the prefixed form, so the encoder is
 * shared: `AT+GTACT?` reports 101,103,105,108,134,138,139,140,141 for LTE and
 * 501,5028,5041,5078,5079 for NR (= B1/B3/B5/B8/B34/B38/B39/B40/B41 and
 * n1/n28/n41/n78/n79), while `AT+GTCCINFO?` reports band 103 for the serving
 * cell it describes as B3.  fm160_band_decode() still also accepts a bare band
 * number, because <band> is documented as BAND_INVALID (0) whenever the modem
 * is not registered and the firmware could report either form.
 */
int fm160_band_decode(int rat, int raw)
{
	if (raw <= 0)
		return 0;

	switch (rat) {
	case 4:                                   /* LTE */
		if (raw >= 101 && raw <= 164)     /* BAND_LTE_1..64  */
			return raw - 100;
		if (raw <= 64)                    /* already bare    */
			return raw;
		return 0;
	case 9:                                   /* NR */
		if (raw >= 50100 && raw <= 50999) /* BAND_NR_100..999 */
			return raw - 50000;
		if (raw >= 5010 && raw <= 5099)   /* BAND_NR_10..99   */
			return raw - 5000;
		if (raw >= 501 && raw <= 509)     /* BAND_NR_1..9     */
			return raw - 500;
		if (raw <= 512)                   /* already bare     */
			return raw;
		return 0;
	case 2:                                   /* WCDMA / UMTS     */
		if (raw <= 25)                    /* BAND_UMTS_I..XXV */
			return raw;
		return 0;
	default:
		return 0;
	}
}

/*
 * Which RAT does a raw band token belong to?
 *
 * AT+GTACT? hands back ONE flat band list that mixes all RATs, e.g.
 *   20,6,3,1,8,101,103,105,108,134,138,139,140,141,501,5028,5041,5078,5079
 * where 1,8 are UMTS, the 101..141 run is LTE and the 501.. run is NR.  The
 * only way to tell them apart is the encoding itself, so this is the inverse of
 * fm160_band_decode() and the two must agree.
 *
 * The three NR ranges are disjoint from each other and from LTE, which is what
 * makes the classification unambiguous:
 *   501..509         n1..n9        (501 = 500 + 1)
 *   5010..5099       n10..n99      (5028 = 5000 + 28)
 *   50100..50999     n100..n999    (50100 = 50000 + 100)
 *   101..499         B1..B399      (103 = 100 + 3; GTCAINFO advertises up to 171)
 *   1..25            UMTS I..XXV
 *
 * A bare 0 is AT+GTACT's "automatic band selection" marker, not a band, and
 * anything else (e.g. 510..5009) is not in any documented scheme: it is
 * reported as unknown rather than guessed into a RAT.
 */
int fm160_band_rat_of(int raw)
{
	if (raw <= 0)
		return 0;                        /* 0 = auto / invalid */
	if (raw >= 501 && raw <= 509)
		return 9;
	if (raw >= 5010 && raw <= 5099)
		return 9;
	if (raw >= 50100 && raw <= 50999)
		return 9;
	if (raw >= 101 && raw <= 499)
		return 4;
	if (raw <= 25)
		return 2;
	return 0;
}

/* <bandwidth>: LTE is a resource-block count, NR is a plain MHz code whose 0
 * means 5 MHz - two different tables for the same column. */
static int decode_bandwidth_mhz(int rat, int raw)
{
	static const int lte_rb[6]   = { 6, 15, 25, 50, 75, 100 };
	static const int lte_mhz[6]  = { 1, 3,  5,  10, 15, 20  };
	static const int nr_code[13] = { 0, 10, 15, 20, 25, 30, 40, 50, 60, 80, 90, 100, 200 };
	static const int nr_mhz[13]  = { 5, 10, 15, 20, 25, 30, 40, 50, 60, 80, 90, 100, 200 };
	unsigned i;

	if (raw <= 0)
		return FM160_NONE;

	if (rat == 4) {
		for (i = 0; i < ARRAY_SIZE(lte_rb); i++)
			if (lte_rb[i] == raw)
				return lte_mhz[i];
		return FM160_NONE;
	}
	if (rat == 9) {
		for (i = 0; i < ARRAY_SIZE(nr_code); i++)
			if (nr_code[i] == raw)
				return nr_mhz[i];
		if (raw == 400)
			return 400;
		return FM160_NONE;
	}
	return FM160_NONE;
}

/* <rssnr_value> (LTE) is dB = raw/2; <ss-sinr> (NR) is dB = -23 + raw/2. */
static int decode_sinr_db10(int rat, int raw)
{
	if (raw == 255 || raw < -100 || raw > 127)
		return FM160_NONE;
	if (rat == 4)
		return raw * 5;
	if (rat == 9)
		return -230 + raw * 5;
	return FM160_NONE;
}

/* <rsrp> (LTE) is dBm = -140 + raw; <ss-rsrp> (NR) is dBm = -156 + raw. */
static int decode_rsrp_dbm(int rat, int raw)
{
	if (raw == 255)
		return FM160_NONE;
	if (rat == 4 && raw <= 97)
		return -140 + raw;
	if (rat == 9 && raw <= 126)
		return -156 + raw;
	return FM160_NONE;
}

/* <rsrq> (LTE) is dB = -19.5 + raw/2; <ss-rsrq> (NR) is dB = -43 + raw/2. */
static int decode_rsrq_db10(int rat, int raw)
{
	if (raw == 255)
		return FM160_NONE;
	if (rat == 4 && raw <= 34)
		return -195 + raw * 5;
	if (rat == 9 && raw <= 126)
		return -430 + raw * 5;
	return FM160_NONE;
}

/* ------------------------------------------------------------------ */
/* parsers                                                              */
/* ------------------------------------------------------------------ */

/*
 * AT+CSQ -> +CSQ: <rssi>,<ber>   (AT manual 9.1.1)
 *   rssi 0 = -113 dBm or less, 31 = -51 dBm or greater, 99 = unknown.
 *
 * CAUTION: the manual adds "When act=11 or 13, and set AT+GTCSQNREN=1, the rssi
 * replaced by ss_rsrp (0-126)".  In that configuration the first field is no
 * longer an RSSI and the -113+2n formula would report nonsense.
 *
 * AT+GTCSQNREN has no section of its own in the manual (only this one mention),
 * so rather than poking an undocumented command we detect the situation for
 * free: a value above 31 that is not the 99 "unknown" code can only be an
 * ss_rsrp reading.  No extra AT traffic, and the wrong number is never shown.
 */
void fm160_parse_csq(const char *resp)
{
	char line[FM160_STR_MAX];
	int rssi;

	if (!fm160_resp_find(resp, "+CSQ", line, sizeof(line)))
		return;

	rssi = csv_int(line, 0, 99);
	g_state.csq_rssi = rssi;
	g_state.csq_ber = csv_int(line, 1, 99);

	if (rssi > 31 && rssi != 99) {
		g_state.csq_is_ss_rsrp = true;
		g_state.nr_ss_rsrp_dbm = rssi <= 126 ? -156 + rssi : FM160_NONE;
	} else {
		g_state.csq_is_ss_rsrp = false;
	}
}

/*
 * AT+CESQ -> +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>,
 *                   <ss_rsrq>,<ss_rsrp>,<ss_sinr>          (AT manual 9.1.2)
 *
 * Nine fields, not six.  Fields that do not apply to the current serving cell
 * are reported as 255 (99 for <rxlev>/<ber>), which is why every decoder below
 * has an explicit validity check and the decoded value has a sentinel.
 */
void fm160_parse_cesq(const char *resp)
{
	char line[FM160_STR_MAX];

	if (!fm160_resp_find(resp, "+CESQ", line, sizeof(line)))
		return;

	g_state.cesq_rxlev   = csv_int(line, 0, 99);
	g_state.cesq_ber     = csv_int(line, 1, 99);
	g_state.cesq_rscp    = csv_int(line, 2, 255);
	g_state.cesq_ecno    = csv_int(line, 3, 255);
	g_state.cesq_rsrq    = csv_int(line, 4, 255);
	g_state.cesq_rsrp    = csv_int(line, 5, 255);
	g_state.cesq_ss_rsrq = csv_int(line, 6, 255);
	g_state.cesq_ss_rsrp = csv_int(line, 7, 255);
	g_state.cesq_ss_sinr = csv_int(line, 8, 255);

	g_state.geran_rssi_dbm = g_state.cesq_rxlev <= 63 ?
				 -110 + g_state.cesq_rxlev : FM160_NONE;
	g_state.utra_rscp_dbm  = g_state.cesq_rscp <= 96 ?
				 -120 + g_state.cesq_rscp : FM160_NONE;
	g_state.utra_ecno_db10 = g_state.cesq_ecno <= 49 ?
				 -240 + g_state.cesq_ecno * 5 : FM160_NONE;
	g_state.lte_rsrp_dbm   = g_state.cesq_rsrp <= 97 ?
				 -140 + g_state.cesq_rsrp : FM160_NONE;
	g_state.lte_rsrq_db10  = g_state.cesq_rsrq <= 34 ?
				 -195 + g_state.cesq_rsrq * 5 : FM160_NONE;
	g_state.nr_ss_rsrp_dbm = g_state.cesq_ss_rsrp <= 126 ?
				 -156 + g_state.cesq_ss_rsrp : FM160_NONE;
	g_state.nr_ss_rsrq_db10 = g_state.cesq_ss_rsrq <= 126 ?
				  -430 + g_state.cesq_ss_rsrq * 5 : FM160_NONE;
	g_state.nr_ss_sinr_db10 = g_state.cesq_ss_sinr <= 127 ?
				  -230 + g_state.cesq_ss_sinr * 5 : FM160_NONE;
}

void fm160_parse_greg(const char *resp, const char *prefix, int *slot)
{
	char line[FM160_STR_MAX];
	char pat[16];

	snprintf(pat, sizeof(pat), "+%s", prefix);
	if (!fm160_resp_find(resp, pat, line, sizeof(line)))
		return;
	/* +CxREG: <n>,<stat>[,...] -- stat is always field 1 */
	*slot = csv_int(line, 1, FM160_REG_UNKNOWN);
}

void fm160_parse_usbmode(const char *resp)
{
	int mode = fm160_usbmode_parse_current(resp);

	/* Only the bare "AT+GTUSBMODE: 32" form is accepted, and the strictness
	 * is the point: the capability answer to AT+GTUSBMODE=? begins with the
	 * same token and its first number is the first element of a RANGE
	 * ("(17-18,20-21,24,29-33)"), so a loose integer scrape reads the
	 * current profile as 17 on a modem that is in 32.  usbmode.c owns that
	 * rule; this function only routes the answer to it. */
	if (mode < 0)
		return;
	g_state.usb_mode = mode;
	/* The switch layer keeps the profile it may have to return to.  One
	 * reader, one writer: this is the only place either copy is set. */
	fm160_modesw_note_current(mode);
}

/* Column index map for one GTCCINFO row; -1 = the column does not exist in
 * that variant.  See the struct fm160_cell comment for the source tables. */
struct cc_cols {
	int band, bw, sinr, rxlev, rsrp, rsrq, rscp, ecno;
};

void fm160_parse_ccinfo(const char *resp)
{
	struct fm160_cell svc[2];
	int svc_n = 0;
	int neigh = 0;
	const char *p;

	memset(svc, 0, sizeof(svc));

	/* GTCCINFO answers with a labelled multi-line block:
	 *
	 *   +GTCCINFO:
	 *   LTE service cell:
	 *   1,4,460,01,4F50,52BF337,672,1E4,103,100,44,63,63,24
	 *   LTE neighbor cell:
	 *
	 * (captured on FM160-CN 89614.1000.00.04.01.02).  The row labels on real
	 * hardware are shorter than the manual's "LTE/eMTC/NB-IoT service cell:",
	 * and EN-DC puts two differently-shaped rows under one single
	 * "LTE-NR EN-DC service cell:" label, so the label is used only to skip
	 * lines: the layout comes from the row's own <IsServiceCell>,<rat>, which
	 * are identical in every variant.  A data row always starts with a digit;
	 * every label, blank line and the trailing OK do not.
	 *
	 * fm160_resp_lines() is unusable here -- it only returns text sitting on
	 * the same line as the prefix, and "+GTCCINFO:" occupies a line of its
	 * own, so on hardware it silently produced zero rows. */
	if (!resp)
		return;
	p = strstr(resp, "+GTCCINFO");
	if (!p) {
		fm160_log(LOG_DEBUG, "GTCCINFO: no data line");
		return;
	}
	p = strchr(p, '\n');
	if (!p)
		return;
	p++;

	while (*p) {
		char line[FM160_STR_MAX];
		const char *eol = p + strcspn(p, "\r\n");
		size_t len = (size_t)(eol - p);
		int is_service, rat;
		struct cc_cols c;
		struct fm160_cell *cell;

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = '\0';

		/* Advance first: several branches below use `continue`. */
		p = *eol ? eol + 1 : eol;

		if (line[0] < '0' || line[0] > '9')
			continue;         /* row label, blank line, OK, ERROR */

		is_service = csv_int(line, 0, 0);
		rat = csv_int(line, 1, 0);

		if (rat != 2 && rat != 4 && rat != 9)
			continue;

		if (is_service == 1) {
			switch (rat) {
			case 4: case 9:      /* LTE / NR service */
				c = (struct cc_cols){ 8, 9, 10, 11, 12, 13, -1, -1 };
				break;
			case 2:              /* WCDMA service  */
				c = (struct cc_cols){ 8, -1, -1, 12, -1, -1, 10, 9 };
				break;
			default:
				continue;
			}
			if (svc_n >= 2)
				continue;         /* more than 2 service cells: ignore */
			cell = &svc[svc_n++];
		} else {
			switch (rat) {
			case 4:              /* LTE neighbour  */
				c = (struct cc_cols){ -1, 8, -1, 9, 10, 11, -1, -1 };
				break;
			case 9:              /* NR neighbour: 8 is ss-sinr! */
				c = (struct cc_cols){ -1, -1, 8, 9, 10, 11, -1, -1 };
				break;
			case 2:              /* WCDMA neighbour */
				c = (struct cc_cols){ -1, -1, -1, 13, -1, -1, 14, 11 };
				break;
			default:
				continue;
			}
			if (neigh >= (int)ARRAY_SIZE(g_state.neigh))
				continue;
			cell = &g_state.neigh[neigh++];
		}

		memset(cell, 0, sizeof(*cell));
		cell->valid = true;
		cell->is_service = (is_service == 1);
		cell->rat = rat;
		cell->mcc = csv_int(line, 2, 0);
		cell->mnc = csv_int(line, 3, 0);
		/* <lac>/<tac>, <cell_id>, <arfcn>/<earfcn>/<narfcn> and
		 * <physicalcellId> are printed in hex; <mcc>/<mnc> are decimal.
		 * See csv_hex_ll() for the hardware evidence. */
		cell->tac = csv_hex_ll(line, 4, 0);
		cell->cellid = csv_hex_ll(line, 5, 0);
		cell->earfcn = csv_hex_ll(line, 6, 0);
		cell->pci = (int)csv_hex_ll(line, 7, 0);
		cell->band = c.band >= 0 ?
			fm160_band_decode(rat, csv_int(line, c.band, 0)) : 0;

		cell->bandwidth_raw = c.bw >= 0 ? csv_int(line, c.bw, -1) : -1;
		cell->bandwidth_mhz = decode_bandwidth_mhz(rat, cell->bandwidth_raw);

		cell->sinr_raw = c.sinr >= 0 ? csv_int(line, c.sinr, 255) : 255;
		cell->sinr_db10 = decode_sinr_db10(rat, cell->sinr_raw);

		cell->rxlev_raw = c.rxlev >= 0 ? csv_int(line, c.rxlev, 255) : 255;

		cell->rsrp_raw = c.rsrp >= 0 ? csv_int(line, c.rsrp, 255) : 255;
		cell->rsrp_dbm = decode_rsrp_dbm(rat, cell->rsrp_raw);

		cell->rsrq_raw = c.rsrq >= 0 ? csv_int(line, c.rsrq, 255) : 255;
		cell->rsrq_db10 = decode_rsrq_db10(rat, cell->rsrq_raw);

		/* WCDMA has no RSRP/RSRQ; keep rscp/ecno in their own fields. */
		if (rat == 2) {
			int rscp = c.rscp >= 0 ? csv_int(line, c.rscp, 255) : 255;

			cell->rsrp_raw = rscp;
			cell->rsrp_dbm = rscp <= 96 ? -120 + rscp : FM160_NONE;
		}
	}

	g_state.neigh_count = neigh;

	if (!svc_n) {
		/* No service cell in this response: keep old data, but do not claim
		 * a fresh reading. */
		fm160_log(LOG_DEBUG, "GTCCINFO: %d neighbour row(s), no service cell",
			  neigh);
		g_state.serving.valid = false;
		g_state.serving2.valid = false;
		return;
	}

	/* EN-DC reports two service rows (LTE anchor + NR).  Show the highest RAT
	 * as the headline and keep the other one next to it. */
	if (svc_n == 2 && svc[0].rat < svc[1].rat) {
		g_state.serving  = svc[1];
		g_state.serving2 = svc[0];
	} else {
		g_state.serving  = svc[0];
		g_state.serving2 = svc_n == 2 ? svc[1] : (struct fm160_cell){ 0 };
	}
	g_state.cell_last_ok_ms = fm160_now_ms();
}

/* ------------------------------------------------------------------ */
/* identity chain                                                       */
/* ------------------------------------------------------------------ */

static int ident_step;

static void ident_store(struct at_req *req, enum at_status status,
			const char *response, void *arg);

enum ident_kind {
	IDENT_STR = 0,        /* copy the first value line into dst          */
	IDENT_USBMODE,        /* +GTUSBMODE: <n>                             */
	IDENT_USBMODE_CAPS,   /* +GTUSBMODE: (17-18,20-21,24,29-33)          */
	IDENT_GTACT_CAPS,     /* +GTACT: (…),(…) x9                          */
	IDENT_CELLLOCK_CAPS,  /* +GTCELLLOCK: ranges x7                      */
	/* M5 */
	IDENT_GNSS_POWER,     /* +GTGPSPOWER: <n>                            */
	IDENT_GNSS_CFG,       /* +GTGPSCFG: then "<x>,<value>" lines          */
	IDENT_GNSS_CFG_CAPS,  /* +GTGPSCFG=? - the licence to write it        */
	IDENT_GNSS_EPO,       /* +GTGPSEPO: <n>                              */
	IDENT_GNSS_SUPL,      /* +GTAGPSSERV: "<host>",<port>                */
};

struct ident_item {
	const char *cmd;
	char *dst;
	size_t dstlen;
	enum ident_kind kind;
};

/*
 * Read once per boot, in order.  Nothing here is polled: these either cannot
 * change while the daemon runs (identity, capability lists) or are handled by
 * a dedicated tier (see sched.c).
 *
 * The "=?" capability reads belong here rather than in a poll tier for a
 * sharper reason than cost: they are the *permission* for the write paths.
 * fm160_cmd_set_bands()/fm160_cmd_set_celllock()/fm160_cmd_set_gnss_cfg()
 * refuse to send anything until the corresponding enumeration has succeeded, so
 * a module that will not tell us what it supports never gets written to.
 */
static struct ident_item ident_plan[] = {
	{ "AT+CGMI",       g_state.manufacturer, sizeof(g_state.manufacturer), IDENT_STR },
	{ "AT+CGMM",       g_state.model,        sizeof(g_state.model),        IDENT_STR },
	{ "AT+CGMR",       g_state.revision,     sizeof(g_state.revision),     IDENT_STR },
	{ "AT+CGSN",       g_state.imei,         sizeof(g_state.imei),         IDENT_STR },
	{ "AT+CFSN",       g_state.sn,           sizeof(g_state.sn),           IDENT_STR },
	/* AT+ICCID, not AT+CCID: on FM160-CN 89614.1000.00.04.01.02 a card-less
	 * AT+CCID blocks for the full 10 s timeout, while AT+ICCID answers in
	 * 18 ms (+CME ERROR: 13 without a card).  This is the same preference
	 * QModem's fibocom.sh encodes when it tries ICCID first and only falls
	 * back to CCID.
	 *
	 * Caveat, recorded deliberately: the AT manual lists AT+CCID (3.1.12)
	 * but NOT AT+ICCID, so ICCID is apparently an unlisted alias.  It is safe
	 * here because (a) hardware proves the command is understood - it answers
	 * +CME ERROR rather than bare ERROR - and (b) identity is read once per
	 * boot and never polled, so a missing alias costs a "-" in the UI rather
	 * than a stalled queue.  If it ever regresses, fall back to AT+CCID behind
	 * a long timeout inside the quiet window, never in the poll loop. */
	{ "AT+ICCID",      g_state.iccid,        sizeof(g_state.iccid),        IDENT_STR },
	{ "AT+GTUSBMODE?", NULL,                 0,                            IDENT_USBMODE },
	/* The second gate of DESIGN 5.1.  A target has to be in this list AND
	 * in the hard whitelist, and this read is the only source of the first
	 * half - which is why it is here, in the once-per-boot chain, and not
	 * in a poll tier: a permission a poll loop could lose is not a
	 * permission.  It is also read at boot for the same reason the M4 and
	 * M5 capability lists are: a module that will not say what it supports
	 * never gets written to. */
	{ "AT+GTUSBMODE=?", NULL,                0,                            IDENT_USBMODE_CAPS },
	/* M4.  Both answer in 21 ms / 16 ms on the live module. */
	{ "AT+GTACT=?",    NULL,                 0,                            IDENT_GTACT_CAPS },
	{ "AT+GTCELLLOCK=?", NULL,               0,                            IDENT_CELLLOCK_CAPS },
	/*
	 * M5.  Five reads, 104 ms in total measured, in this order because the
	 * first two are what fm160_cmd_gnss_autostart() needs at the end of the
	 * chain: the engine state to compare against, and the capability list
	 * that licenses the constellation write.  AT+GTGPSCFG=? in particular
	 * belongs here rather than in a poll tier for the same reason the M4
	 * capability reads do: it is a PERMISSION, and a permission that a poll
	 * loop could lose is not a permission.
	 *
	 * ⚠️ AT+GTGPSCFG=? is the one entry here whose answer has never been
	 * seen on hardware.  It is asked for precisely so that the write path
	 * can refuse when the answer is unusable - see
	 * fm160_parse_gnss_cfg_caps().
	 */
	{ "AT+GTGPSPOWER?", NULL,                0,                            IDENT_GNSS_POWER },
	{ "AT+GTGPSCFG=?", NULL,                 0,                            IDENT_GNSS_CFG_CAPS },
	{ "AT+GTGPSCFG?",  NULL,                 0,                            IDENT_GNSS_CFG },
	{ "AT+GTGPSEPO?",  NULL,                 0,                            IDENT_GNSS_EPO },
	{ "AT+GTAGPSSERV?", NULL,                0,                            IDENT_GNSS_SUPL },
};

static const char *first_value_line(const char *resp)
{
	static char buf[FM160_STR_MAX];
	const char *p, *e;

	buf[0] = '\0';
	if (!resp)
		return buf;

	/* Skip the echoed command and any blank lines; take the first line that
	 * is neither blank nor a bare result code. */
	p = resp;
	while (*p) {
		size_t len;

		while (*p == '\r' || *p == '\n')
			p++;
		if (!*p)
			break;
		e = p;
		while (*e && *e != '\r' && *e != '\n')
			e++;
		len = (size_t)(e - p);
		if (!(len == 2 && !strncmp(p, "OK", 2)) &&
		    !(len >= 5 && !strncmp(p, "ERROR", 5)) &&
		    !(len >= 3 && !strncmp(p, "AT+", 3)) &&
		    !(len >= 3 && !strncmp(p, "at+", 3))) {
			/* Strip a leading "+PREFIX:" when the command puts one
			 * there.  AT+CFSN answers `+CFSN: "FP62PE002F"` while
			 * AT+CGMI answers a bare string, and without this the UI
			 * showed the whole protocol line as the serial number --
			 * observed on the board as sn = "+CFSN: \"FP62PE002F\"".
			 * Applied generically rather than naming +CFSN, because
			 * AT+ICCID/+CCID are the same shape. */
			if (*p == '+') {
				const char *c = p;

				while (c < e && *c != ':')
					c++;
				if (c < e) {
					p = c + 1;
					while (p < e && (*p == ' ' || *p == '\t'))
						p++;
					len = (size_t)(e - p);
				}
			}
			/* ...then the quotes a few commands wrap the value in. */
			if (len >= 2 && *p == '"' && p[len - 1] == '"') {
				p++;
				len -= 2;
			}
			if (len >= FM160_STR_MAX)
				len = FM160_STR_MAX - 1;
			memcpy(buf, p, len);
			buf[len] = '\0';
			return buf;
		}
		p = e;
	}
	return buf;
}

static void ident_next(void)
{
	if (ident_step >= (int)ARRAY_SIZE(ident_plan)) {
		g_state.ident_done = true;
		fm160_log(LOG_INFO, "identity: %s %s fw=%s imei=%s mode=%d",
			  g_state.manufacturer, g_state.model, g_state.revision,
			  g_state.imei, g_state.usb_mode);
		fm160_log(LOG_INFO,
			  "boot read-outs: gtact caps=%s celllock caps=%s gnss cfg caps=%s",
			  g_state.gtact_caps.valid ? "ok" : "unavailable",
			  g_state.celllock_caps.valid ? "ok" : "unavailable",
			  g_state.gnss.cfg.caps_valid ? "ok" : "unavailable");
		fm160_state_mark_dirty();
		fm160_state_publish();
		/* The engine state and the write licence have both just been read,
		 * which is the only moment uci's gnss_autostart wish can be acted on
		 * without either guessing or fighting the user - see the notes on
		 * fm160_cmd_gnss_autostart(). */
		fm160_cmd_gnss_autostart();
		return;
	}

	atq_submit(AT_PRIO_STATE, ident_plan[ident_step].cmd, NULL, 3000,
		   ident_store, NULL);
}

static void ident_store(struct at_req *req, enum at_status status,
			const char *response, void *arg)
{
	struct ident_item *it = &ident_plan[ident_step];

	if (status == AT_STATUS_OK) {
		switch (it->kind) {
		case IDENT_USBMODE:
			fm160_parse_usbmode(response);
			break;
		case IDENT_USBMODE_CAPS: {
			int list[FM160_USBMODE_SUP_MAX];
			int n = fm160_usbmode_parse_list(response, list,
							 FM160_USBMODE_SUP_MAX);

			/* A list this parser cannot read is NOT the same as an
			 * empty one, and neither of them is "never answered":
			 * the first leaves caps_valid false (every switch is
			 * refused as LIST_UNKNOWN) rather than claiming the
			 * modem offers nothing.  Refusing harder, with a
			 * different reason, is the safe direction. */
			if (n < 0)
				fm160_log(LOG_WARNING,
					  "AT+GTUSBMODE=? answered in a form this "
					  "parser does not understand; USB profile "
					  "changes stay disabled");
			else
				fm160_modesw_note_caps(list, n);
			break;
		}
		case IDENT_GTACT_CAPS:
			fm160_parse_gtact_caps(response);
			break;
		case IDENT_CELLLOCK_CAPS:
			fm160_parse_celllock_caps(response);
			break;
		case IDENT_GNSS_POWER:
			fm160_parse_gnss_power(response);
			break;
		case IDENT_GNSS_CFG:
			fm160_parse_gnss_cfg(response);
			break;
		case IDENT_GNSS_CFG_CAPS:
			fm160_parse_gnss_cfg_caps(response);
			break;
		case IDENT_GNSS_EPO:
			fm160_parse_gnss_epo(response);
			break;
		case IDENT_GNSS_SUPL:
			fm160_parse_gnss_supl(response);
			break;
		case IDENT_STR:
		default:
			if (it->dst)
				snprintf(it->dst, it->dstlen, "%s",
					 first_value_line(response));
			break;
		}
	} else {
		fm160_log(LOG_DEBUG, "%s failed (status %d)", it->cmd, status);
	}

	ident_step++;
	ident_next();
}

void fm160_cmd_ident_start(void)
{
	ident_step = 0;
	fm160_ident_reset();
	/* The SMS arming (CMGF/CPMS/CNMI) belongs to the module we are about
	 * to (re)read, not to whichever module answered last: a reset or a
	 * reflash silently defaults CNMI/CPMS, and setup_done would otherwise
	 * freeze the arming forever. */
	fm160_sms_setup_reset();
	fm160_log(LOG_INFO, "reading module identity");
	ident_next();
}

void fm160_ident_reset(void)
{
	g_state.manufacturer[0] = '\0';
	g_state.model[0] = '\0';
	g_state.revision[0] = '\0';
	g_state.imei[0] = '\0';
	g_state.sn[0] = '\0';
	g_state.iccid[0] = '\0';
	g_state.usb_mode = -1;
	g_state.ident_done = false;

	/* M2.  The profile the switch may have to return to goes with the
	 * identity it was read from, and so does the list that licenses a
	 * switch at all.  A pending switch is deliberately NOT touched here:
	 * its target and rollback point live in uci and have to survive a
	 * restart, which is the only reason a switch applied just before the
	 * daemon died is still verified on the next boot. */
	g_state.modesw.current = -1;
	g_state.modesw.caps_valid = false;
	g_state.modesw.supported_n = 0;

	/* Capabilities are invalidated with the identity they belong to.  If the
	 * re-read fails the write paths are blocked, which is the safe
	 * direction: writing a persistent setting to a module that just failed
	 * to describe itself is worse than a temporarily greyed-out button. */
	g_state.gtact_caps.valid = false;
	g_state.celllock_caps.valid = false;
	g_state.gnss.cfg.caps_valid = false;
	/* The sets go too, not just the licence: the snapshot publishes them and
	 * the page draws a value list from them, so a licence reset that left the
	 * old lists behind would show the previous module's answer as current. */
	memset(g_state.gnss.cfg.caps_n, 0, sizeof(g_state.gnss.cfg.caps_n));
	g_state.gnss.cfg.caps_x_mask = 0;

	/* The autostart wish gets one attempt per ident run.  Resetting it here
	 * rather than never means a module that was power-cycled - the one case
	 * where the engine really does come back switched off - gets its engine
	 * switched on again, while a user who turned the engine off by hand is
	 * not fought by a poll that keeps seeing "off". */
	g_state.gnss.autostart_done = false;
}

/* ------------------------------------------------------------------ */
/* periodic polls                                                       */
/* ------------------------------------------------------------------ */

static void reg_cb_csq(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_csq(resp);
}

static void reg_cb_cereg(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_greg(resp, "CEREG", &g_state.cereg);
}

static void reg_cb_c5greg(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_greg(resp, "C5GREG", &g_state.c5greg);
}

void fm160_cmd_poll_reg(void)
{
	/* Three cheap queries per tier tick.  AT+CSQ carries RSSI, the two
	 * registration queries are the only reliable way to know whether the
	 * modem is on an LTE or an NR cell (FM160 is NR-capable). */
	atq_submit(AT_PRIO_POLL, "AT+CSQ",    NULL, 3000, reg_cb_csq,    NULL);
	atq_submit(AT_PRIO_POLL, "AT+CEREG?", NULL, 3000, reg_cb_cereg,  NULL);
	atq_submit(AT_PRIO_POLL, "AT+C5GREG?", NULL, 3000, reg_cb_c5greg, NULL);
}

static void sig_cb_cesq(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_cesq(resp);
}

void fm160_cmd_poll_signal(void)
{
	atq_submit(AT_PRIO_POLL, "AT+CESQ", NULL, 3000, sig_cb_cesq, NULL);
}

static void cell_cb(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_ccinfo(resp);
	else
		fm160_log(LOG_DEBUG, "GTCCINFO failed (status %d)", s);
}

void fm160_cmd_poll_cell(void)
{
	/* One command returns the serving cell plus up to ten neighbours with
	 * RSRP/RSRQ/SINR - by far the best value per AT transaction. */
	atq_submit(AT_PRIO_POLL, "AT+GTCCINFO?", NULL, 8000, cell_cb, NULL);
}

/* ================================================================== */
/* M4: band lock / cell lock / carrier aggregation                      */
/* ================================================================== */

/*
 * The three commands behind this milestone have one property that shapes every
 * decision below: they are all Persistent = Yes, i.e. they end up in an EFS
 * file on the module.  AT+GTCELLLOCK additionally does not take effect until
 * the UE is reset, and the manual forbids combining it with
 * GTFREQLOCK / COPS / GTACT / GTCELLLOCK / GTRAT.
 *
 * So the write path here is deliberately narrow:
 *   - nothing is written before the corresponding "=?" enumeration succeeded
 *     (a modem that will not tell us what it supports does not get written to);
 *   - the value is written, then read back, and only the read-back is reported
 *     as the new state;
 *   - the module is never reset by this daemon.  A cell lock needs a reset to
 *     take effect, and that is a decision for the user, not for a poll loop.
 */

/* Split "(1,2),(3),( )" into its groups, parens and padding removed. */
static int split_groups(const char *s, char out[][FM160_RESP_LINE_MAX], int max)
{
	int n = 0;
	const char *p = s;

	if (!s)
		return 0;

	while (*p && n < max) {
		const char *open, *close;
		size_t len, b, e;

		open = strchr(p, '(');
		if (!open)
			break;
		close = strchr(open, ')');
		if (!close)
			break;

		/* Trim the spaces the modem pads empty groups with: "( )". */
		b = 1;
		e = (size_t)(close - open);
		while (b < e && (open[b] == ' ' || open[b] == '\t'))
			b++;
		while (e > b && (open[e - 1] == ' ' || open[e - 1] == '\t'))
			e--;

		len = e - b;
		if (len >= FM160_RESP_LINE_MAX)
			len = FM160_RESP_LINE_MAX - 1;
		memcpy(out[n], open + b, len);
		out[n][len] = '\0';
		n++;
		p = close + 1;
	}
	return n;
}

/* "1,8,101" -> { 1, 8, 101 }; skips blank tokens, stops at max. */
static int parse_int_list(const char *csv, int *out, int max)
{
	int n = 0;
	const char *p = csv;

	if (!csv)
		return 0;
	while (*p && n < max) {
		const char *e = p + strcspn(p, ",");
		char tmp[32];
		size_t len = (size_t)(e - p);

		if (len >= sizeof(tmp))
			len = sizeof(tmp) - 1;
		memcpy(tmp, p, len);
		tmp[len] = '\0';
		if (tmp[0] && tmp[0] != ' ')
			out[n++] = atoi(tmp);

		if (!*e)
			break;
		p = e + 1;
	}
	return n;
}

/*
 * Parse a band list into an array.  forced_rat != 0 is used for the AT+GTACT=?
 * groups, where the group index already says which RAT it is; forced_rat == 0
 * is used for the flat AT+GTACT? list, where the encoding itself decides.
 * Tokens that fit no scheme are counted in *unknown, never guessed.
 */
static int parse_band_list(const char *csv, struct fm160_band *out, int max,
			   int forced_rat, int *unknown)
{
	int n = 0;
	const char *p = csv;

	if (!csv)
		return 0;
	while (*p && n < max) {
		const char *e = p + strcspn(p, ",");
		char tmp[32];
		size_t len = (size_t)(e - p);
		int raw, rat;

		if (len >= sizeof(tmp))
			len = sizeof(tmp) - 1;
		memcpy(tmp, p, len);
		tmp[len] = '\0';

		if (tmp[0] && tmp[0] != ' ') {
			raw = atoi(tmp);
			rat = forced_rat ? forced_rat : fm160_band_rat_of(raw);
			if (raw > 0 && !rat) {
				if (unknown)
					(*unknown)++;
			} else {
				out[n].raw = raw;
				out[n].band = fm160_band_decode(rat, raw);
				out[n].rat = rat;
				n++;
			}
		}
		if (!*e)
			break;
		p = e + 1;
	}
	return n;
}

/*
 * Walk the flat AT+GTACT? band list once, dispatching each token by its own
 * encoding.  Nothing is guessed: a token that matches no documented scheme is
 * counted in unknown_n, and a bare 0 (AT+GTACT's "automatic band selection")
 * sets auto_seen instead of being stored as a band.
 */
static void parse_flat_bands(const char *csv, struct fm160_gtact_state *st)
{
	const char *p = csv;

	if (!csv)
		return;
	while (*p) {
		const char *e = p + strcspn(p, ",");
		char tmp[32];
		size_t len = (size_t)(e - p);
		struct fm160_band *dst = NULL;
		int *n = NULL;
		int raw, rat;

		if (len >= sizeof(tmp))
			len = sizeof(tmp) - 1;
		memcpy(tmp, p, len);
		tmp[len] = '\0';

		if (tmp[0] && tmp[0] != ' ') {
			raw = atoi(tmp);
			rat = fm160_band_rat_of(raw);

			switch (rat) {
			case 2:  dst = &st->umts[st->umts_n]; n = &st->umts_n; break;
			case 4:  dst = &st->lte[st->lte_n];   n = &st->lte_n;  break;
			case 9:  dst = &st->nr[st->nr_n];     n = &st->nr_n;   break;
			default:
				if (raw > 0)
					st->unknown_n++;
				else if (raw == 0)
					st->auto_seen = true;
				break;
			}
			if (dst && n && *n < FM160_BAND_MAX) {
				dst->raw = raw;
				dst->rat = rat;
				dst->band = fm160_band_decode(rat, raw);
				(*n)++;
			}
		}
		if (!*e)
			break;
		p = e + 1;
	}
}

/* AT+GTACT? -> +GTACT: <rat>,<pref1>,<pref2>,[<band_1>,...<band_n>] */
void fm160_parse_gtact(const char *resp)
{
	char line[FM160_RESP_LINE_MAX];
	struct fm160_gtact_state st;
	const char *bands;

	if (!fm160_resp_find(resp, "+GTACT", line, sizeof(line)))
		return;
	/* "+GTACT: (…)" is the =? form; it must not be mistaken for state. */
	if (line[0] == '(')
		return;

	memset(&st, 0, sizeof(st));
	st.rat   = csv_int(line, 0, -1);
	st.pref1 = csv_int(line, 1, -1);
	st.pref2 = csv_int(line, 2, -1);

	/* The bands start at field 3: one flat list mixing all RATs. */
	bands = csv_at(line, 3);
	parse_flat_bands(bands, &st);

	st.valid = true;
	st.last_ok_ms = fm160_now_ms();
	g_state.gtact = st;

	fm160_log(LOG_INFO,
		  "GTACT: rat=%d pref=%d,%d bands umts=%d lte=%d nr=%d"
		  "%s%s",
		  st.rat, st.pref1, st.pref2, st.umts_n, st.lte_n, st.nr_n,
		  st.unknown_n ? " (undecodable tokens!)" : "",
		  st.auto_seen ? " (automatic band selection)" : "");
	fm160_state_mark_dirty();
}

/*
 * AT+GTACT=? -> nine groups:
 *   rat, pref1, pref2, gsm, umts, lte, cdma, evdo, nr
 * On FM160-CN the gsm/cdma/evdo groups are literally "( )".  Recording which
 * groups were empty is what lets the UI show the truth instead of the manual.
 */
void fm160_parse_gtact_caps(const char *resp)
{
	char line[FM160_RESP_LINE_MAX];
	char groups[FM160_GTACT_GROUPS][FM160_RESP_LINE_MAX];
	struct fm160_gtact_caps cp;
	int n, unknown = 0, i;

	if (!fm160_resp_find(resp, "+GTACT", line, sizeof(line)))
		return;
	if (line[0] != '(') {
		fm160_log(LOG_WARNING,
			  "GTACT=? did not answer with a capability list (%s)", line);
		return;
	}

	n = split_groups(line, groups, FM160_GTACT_GROUPS);
	memset(&cp, 0, sizeof(cp));
	for (i = 0; i < n && i < FM160_GTACT_GROUPS; i++)
		cp.group_nonempty[i] = groups[i][0] != '\0';

	if (n > 0) cp.rat_n   = parse_int_list(groups[0], cp.rat,   FM160_CAP_INT_MAX);
	if (n > 1) cp.pref1_n = parse_int_list(groups[1], cp.pref1, FM160_CAP_INT_MAX);
	if (n > 2) cp.pref2_n = parse_int_list(groups[2], cp.pref2, FM160_CAP_INT_MAX);
	if (n > 4) cp.umts_n  = parse_band_list(groups[4], cp.umts, FM160_BAND_MAX, 2, &unknown);
	if (n > 5) cp.lte_n   = parse_band_list(groups[5], cp.lte,  FM160_BAND_MAX, 4, &unknown);
	if (n > 8) cp.nr_n    = parse_band_list(groups[8], cp.nr,   FM160_BAND_MAX, 9, &unknown);

	/* A capability list with no usable RAT is worse than none: the write path
	 * treats "valid" as permission to send. */
	if (!cp.rat_n) {
		fm160_log(LOG_WARNING, "GTACT=? gave %d group(s) but no RAT list", n);
		return;
	}

	cp.valid = true;
	cp.last_ok_ms = fm160_now_ms();
	g_state.gtact_caps = cp;

	fm160_log(LOG_INFO,
		  "GTACT caps: %d rat, %d pref1, %d pref2, umts=%d lte=%d nr=%d "
		  "(groups gsm=%d cdma=%d evdo=%d)",
		  cp.rat_n, cp.pref1_n, cp.pref2_n, cp.umts_n, cp.lte_n, cp.nr_n,
		  cp.group_nonempty[3], cp.group_nonempty[6], cp.group_nonempty[7]);
}

/* AT+GTCELLLOCK? -> +GTCELLLOCK: <mode>[,<rat>,<type>,<earfcn>[,...]] */
void fm160_parse_celllock(const char *resp)
{
	char line[FM160_RESP_LINE_MAX];
	struct fm160_celllock_state cl;

	if (!fm160_resp_find(resp, "+GTCELLLOCK", line, sizeof(line)))
		return;
	if (line[0] == '(')
		return;                      /* the =? form */

	memset(&cl, 0, sizeof(cl));
	cl.enabled = csv_int(line, 0, 0) == 1;

	/* Everything after <mode> is present only while the function is on.
	 * earfcn can reach 4294967295, so it is read as unsigned long long
	 * straight from its field rather than through csv_int(). */
	cl.rat = csv_int(line, 1, -1);
	cl.type = csv_int(line, 2, -1);
	cl.earfcn = strtoull(csv_at(line, 3), NULL, 0);

	cl.pci = csv_at(line, 4)[0] ? csv_int(line, 4, 0) : -1;
	cl.scs = csv_at(line, 5)[0] ? csv_int(line, 5, 0) : -1;
	cl.nrband = csv_at(line, 6)[0] ? csv_int(line, 6, 0) : -1;

	/* The trailing fields are optional; -1 means "the modem did not say". */
	cl.has_pci    = cl.pci != -1;
	cl.has_scs    = cl.scs != -1;
	cl.has_nrband = cl.nrband != -1;
	if (!cl.has_pci)
		cl.pci = 0;
	if (!cl.has_scs)
		cl.scs = 0;
	if (!cl.has_nrband)
		cl.nrband = 0;

	cl.valid = true;
	cl.last_ok_ms = fm160_now_ms();
	g_state.celllock = cl;

	fm160_log(LOG_INFO, "celllock: %s rat=%d type=%d earfcn=%llu pci=%d",
		  cl.enabled ? "enabled" : "disabled", cl.rat, cl.type,
		  cl.earfcn, cl.pci);
	fm160_state_mark_dirty();
}

/*
 * AT+GTCELLLOCK=? -> ranges instead of lists:
 *   (0,1,2),(0-2),(0,1),(0-4294967295),(0-1007),(0-1),(501-50261)
 *
 * ⚠️ The live modem advertises mode "(0,1,2)".  The manual only defines 0
 * (disable) and 1 (enable).  mode 2 is therefore recorded but never written:
 * this is exactly the case where "the device supports it" and "we may use it"
 * are different questions.
 */
void fm160_parse_celllock_caps(const char *resp)
{
	char line[FM160_RESP_LINE_MAX];
	char groups[FM160_GTACT_GROUPS][FM160_RESP_LINE_MAX];
	struct fm160_celllock_caps cp;
	int n;

	if (!fm160_resp_find(resp, "+GTCELLLOCK", line, sizeof(line)))
		return;
	if (line[0] != '(') {
		fm160_log(LOG_WARNING,
			  "GTCELLLOCK=? did not answer with a capability list (%s)",
			  line);
		return;
	}

	n = split_groups(line, groups, FM160_GTACT_GROUPS);
	memset(&cp, 0, sizeof(cp));

	if (n > 0)
		cp.mode_n = parse_int_list(groups[0], cp.mode, (int)ARRAY_SIZE(cp.mode));
	/* Groups 1..6 are "lo-hi" ranges, and <earfcn> reaches 4294967295, so
	 * these are read with sscanf into explicitly typed locals rather than
	 * through the int-returning csv helpers. */
	if (n > 1) {
		int lo = 0, hi = 0;

		if (sscanf(groups[1], "%d-%d", &lo, &hi) == 2) {
			cp.rat_min = lo;
			cp.rat_max = hi;
		}
	}
	if (n > 2)
		cp.type_n = parse_int_list(groups[2], cp.type, (int)ARRAY_SIZE(cp.type));
	if (n > 3) {
		unsigned int lo = 0, hi = 0;

		if (sscanf(groups[3], "%u-%u", &lo, &hi) == 2)
			cp.earfcn_max = hi;
	}
	if (n > 4) {
		int lo = 0, hi = 0;

		if (sscanf(groups[4], "%d-%d", &lo, &hi) == 2)
			cp.pci_max = hi;
	}
	if (n > 5)
		sscanf(groups[5], "%d-%d", &cp.scs_min, &cp.scs_max);
	if (n > 6)
		sscanf(groups[6], "%d-%d", &cp.nrband_min, &cp.nrband_max);

	if (!cp.mode_n) {
		fm160_log(LOG_WARNING, "GTCELLLOCK=? gave no mode list");
		return;
	}

	cp.valid = true;
	cp.last_ok_ms = fm160_now_ms();
	g_state.celllock_caps = cp;

	fm160_log(LOG_INFO,
		  "GTCELLLOCK caps: mode=%d%s rat=%d-%d type=%d earfcn_max=%u "
		  "pci<=%d scs=%d-%d nrband=%d-%d",
		  cp.mode_n, cp.mode_n > 2 ? " (includes an undocumented value!)" : "",
		  cp.rat_min, cp.rat_max, cp.type_n, cp.earfcn_max, cp.pci_max,
		  cp.scs_min, cp.scs_max, cp.nrband_min, cp.nrband_max);
}

/* Bandwidth code -> MHz: the same two tables GTCCINFO uses. */
static int ca_bw_mhz(int rat, int raw)
{
	return decode_bandwidth_mhz(rat, raw);
}

/*
 * One "PCC: ..." / "SCCn: ..." row.  LTE and NR rows have the same column
 * count; what differs is the PCC/SCC offset, because an SCC row leads with
 * <scell_state>,<ul_configured> and the PCC row does not:
 *
 *   PCC: <band>,<pci>,<freq>,<dl_bw>,<dl_mimo>,<ul_mimo>,<dl_mod>,<ul_mod>,<rsrp>
 *   SCC: <state>,<ul_cfg>,<band>,<pci>,<freq>,<dl_bw>,<ul_bw>,<dl_mimo>,
 *        <ul_mimo>,<dl_mod>,<ul_mod>,<rsrp>
 *
 * ⚠️ <freq> is parsed as DECIMAL here, unlike GTCCINFO's <earfcn> which is hex.
 * The manual gives GTCCINFO's ranges as "0-0xFFFFFFF" but GTCAINFO's as
 * "0-65535" (earfcn) and "0-2229167" (narfcn) - no 0x anywhere - so the same
 * looking column has two different radices in two different responses.
 * Nothing in this milestone can verify that (CA needs a registered SIM), so the
 * decode stays as documented and the raw string is not re-derived from it.
 */
static bool ca_parse_row(const char *csv, int rat, bool is_pcc,
			 struct fm160_ca_cell *out)
{
	int off = is_pcc ? 0 : 2;      /* SCC rows lead with state,ul_configured */

	if (!csv || csv[0] < '0' || csv[0] > '9')
		return false;

	memset(out, 0, sizeof(*out));
	out->valid = true;
	out->is_pcc = is_pcc;

	if (!is_pcc) {
		out->state = csv_int(csv, 0, 0);
		out->ul_configured = csv_int(csv, 1, 0);
	}
	out->band = fm160_band_decode(rat, csv_int(csv, off + 0, 0));
	out->pci = csv_int(csv, off + 1, -1);
	out->freq = strtoull(csv_at(csv, off + 2), NULL, 10);
	out->dl_bw_mhz = ca_bw_mhz(rat, csv_int(csv, off + 3, 0));
	out->ul_bw_mhz = is_pcc ? FM160_NONE :
			 ca_bw_mhz(rat, csv_int(csv, off + 4, 0));
	out->dl_mimo = csv_int(csv, off + (is_pcc ? 4 : 5), 0);
	out->ul_mimo = csv_int(csv, off + (is_pcc ? 5 : 6), 0);
	out->dl_mod = csv_int(csv, off + (is_pcc ? 6 : 7), -1);
	out->ul_mod = csv_int(csv, off + (is_pcc ? 7 : 8), -1);
	out->rsrp_dbm = decode_rsrp_dbm(rat, csv_int(csv, off + (is_pcc ? 8 : 9), 255));
	return true;
}

/*
 * AT+GTCAINFO? -> a labelled block:
 *
 *   1. LTE
 *   PCC: 103,484,1650,100,2,1,4,4,47
 *   SCC1:2,0,141,123,3900,75,50,2,2,3,3,40
 *   2. NR
 *   PCC: ...
 *
 * Measured with no SIM it answers a bare "OK" - no data block at all - so
 * "nothing parsed" is a normal outcome and clears the state instead of being
 * logged as a failure.  Without that, the UI would keep showing a stale CA
 * table from before the module deregistered.
 */
void fm160_parse_cainfo(const char *resp)
{
	const char *p;
	struct fm160_ca_state ca;
	int cur_rat = 0;

	if (!resp)
		return;
	p = strstr(resp, "+GTCAINFO");
	if (!p) {
		/* No block: not aggregated (or not registered).  Clear, do not warn. */
		g_state.ca.valid = false;
		g_state.ca.scc_n = 0;
		g_state.ca.last_ok_ms = fm160_now_ms();
		g_state.ca.has_nr = false;
		return;
	}
	p = strchr(p, '\n');
	if (!p)
		return;
	p++;

	memset(&ca, 0, sizeof(ca));

	while (*p) {
		char line[FM160_RESP_LINE_MAX];
		const char *eol = p + strcspn(p, "\r\n");
		size_t len = (size_t)(eol - p);

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = '\0';
		p = *eol ? eol + 1 : eol;

		if (line[0] < '0' || line[0] > '9') {
			/* A header ("1. LTE" / "2. NR").  cur_rat tracks the block we
			 * are inside; ca.rat keeps the FIRST one, because that is the
			 * PCC the UI reports as the headline. */
			if (strstr(line, "LTE")) {
				cur_rat = 4;
			} else if (strstr(line, "NR")) {
				cur_rat = 9;
				ca.has_nr = true;
			}
			if (!ca.rat && cur_rat)
				ca.rat = cur_rat;
			continue;
		}
		if (!cur_rat || cur_rat != ca.rat)
			continue;      /* a second RAT block: see the note below */

		if (!strncmp(line, "PCC", 3)) {
			const char *colon = strchr(line, ':');

			if (colon)
				ca_parse_row(colon + 1, ca.rat, true, &ca.pcc);
			continue;
		}
		if (!strncmp(line, "SCC", 3)) {
			const char *colon = strchr(line, ':');

			if (colon && ca.scc_n < FM160_CA_MAX &&
			    ca_parse_row(colon + 1, ca.rat, false, &ca.scc[ca.scc_n]))
				ca.scc_n++;
		}
	}

	/* Only the first block (the one whose PCC we keep) is expanded into the
	 * SCC array.  EN-DC reports a second block for the NR leg; ca.has_nr
	 * records that it was there so the UI can say "EN-DC" without pretending
	 * it has two full CA tables. */
	if (!ca.pcc.valid) {
		ca.valid = false;
		ca.scc_n = 0;
	}

	ca.last_ok_ms = fm160_now_ms();
	g_state.ca = ca;

	if (ca.valid)
		fm160_log(LOG_INFO, "CA: %s PCC band %d + %d SCC%s",
			  ca.rat == 9 ? "NR" : "LTE", ca.pcc.band, ca.scc_n,
			  ca.has_nr && ca.rat != 9 ? " (EN-DC: NR leg present)" : "");
	else
		fm160_log(LOG_DEBUG, "CA: not aggregated");
	fm160_state_mark_dirty();
}

/* ------------------------------------------------------------------ */
/* M4 polls                                                             */
/* ------------------------------------------------------------------ */

static void gtact_cb(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_gtact(resp);
}

static void celllock_cb(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_celllock(resp);
}

static void ca_cb(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_cainfo(resp);
}

void fm160_cmd_poll_gtact(void)
{
	/* 26 ms measured.  This is the only cheap way to learn that a persistent
	 * band restriction is in place - the UI must not present "unlocked" just
	 * because nobody asked. */
	atq_submit(AT_PRIO_POLL, "AT+GTACT?", NULL, 3000, gtact_cb, NULL);
}

void fm160_cmd_poll_celllock(void)
{
	atq_submit(AT_PRIO_POLL, "AT+GTCELLLOCK?", NULL, 5000, celllock_cb, NULL);
}

void fm160_cmd_poll_ca(void)
{
	atq_submit(AT_PRIO_POLL, "AT+GTCAINFO?", NULL, 5000, ca_cb, NULL);
}

/* ------------------------------------------------------------------ */
/* M4 write path                                                        */
/* ------------------------------------------------------------------ */

/*
 * Build the AT+GTACT set command.
 *
 * The manual's rule is that leaving <rat> and both <PreferredAct*> blank keeps
 * the current RAT selection and only restricts bands, and that the flat band
 * list is what follows:
 *   AT+GTACT=,,,160,155      -> LTE B60 + B55 only
 *   AT+GTACT=,,,103,5078     -> LTE B3 + NR n78
 *
 * Both fields are omitted rather than defaulted, so this builder only ever
 * emits the "leave the RAT alone" form.  Changing the RAT is AT+GTRAT's job and
 * it is deliberately not implemented in this milestone: the manual forbids
 * combining GTACT/COPS/GTRAT/GTCELLLOCK in the first place.
 *
 * bands_csv is written verbatim after validation, because the RAT-prefixed
 * tokens (101.. / 501..) are the only form the modem accepts.
 */
int fm160_bands_command(char *out, size_t outlen, const char *bands_csv)
{
	int n = 0;
	const char *p;

	if (!bands_csv || !bands_csv[0])
		return -1;

	/* Validate before building: only digits and commas get into a command
	 * line.  A stray space or letter would change the meaning of the band
	 * list, so anything else is rejected rather than trimmed. */
	for (p = bands_csv; *p; p++) {
		if (*p >= '0' && *p <= '9') {
			n++;
			continue;
		}
		if (*p != ',')
			return -1;
	}
	/* One band per slot and no empty slot in the middle: "101,,103" would
	 * shift every following parameter, and a trailing comma would add one. */
	if (!n || strstr(bands_csv, ",,") ||
	    bands_csv[0] == ',' || bands_csv[strlen(bands_csv) - 1] == ',')
		return -1;

	n = snprintf(out, outlen, "AT+GTACT=,,,%s", bands_csv);
	if (n < 0 || (size_t)n >= outlen)
		return -1;
	return 0;
}

/*
 * Build the AT+GTCELLLOCK set command.
 *
 *   AT+GTCELLLOCK=<mode>[,<rat>,<type>,<earfcn>[,<PCI>][,<scs>[,<nrband>]]]
 *
 * The trailing fields nest, so they are appended strictly in order and each one
 * has an explicit "present" marker rather than a zero default:
 *   pci < 0          -> omit <PCI> (and therefore <scs>/<nrband> as well)
 *   rat != 1 (NR)    -> omit <scs>/<nrband>, which exist only for NR
 *   scs < 0          -> omit <scs>, even for NR
 *   nrband < 0       -> omit <nrband>
 * Writing a filler 0 for an absent <PCI> would silently claim "lock PCI 0", and
 * a filler 0 for <nrband> claims NR band 0, which does not exist.  -1 is the
 * "absent" marker for all three; 0 is a legal value for <scs>.
 *
 * mode is restricted to the two values the manual defines.  The live modem
 * additionally advertises mode 2 in its capability list; writing an
 * undocumented value into a persistent EFS setting is not something a
 * "make band locking stable" milestone should do.
 */
int fm160_celllock_command(char *out, size_t outlen, int mode, int rat, int type,
			   unsigned long long earfcn, int pci, int scs, int nrband)
{
	size_t used;
	int n;

	if (mode != 0 && mode != 1)
		return -1;

	if (mode == 0) {
		/* Disabling takes no other argument: AT+GTCELLLOCK=0 */
		n = snprintf(out, outlen, "AT+GTCELLLOCK=0");
		return (n < 0 || (size_t)n >= outlen) ? -1 : 0;
	}

	if (rat < 0 || rat > 2 || type < 0 || type > 1)
		return -1;

	n = snprintf(out, outlen, "AT+GTCELLLOCK=1,%d,%d,%llu", rat, type, earfcn);
	if (n < 0 || (size_t)n >= outlen)
		return -1;
	used = (size_t)n;

	if (pci >= 0) {
		n = snprintf(out + used, outlen - used, ",%d", pci);
		if (n < 0 || (size_t)n >= outlen - used)
			return -1;
		used += (size_t)n;

		if (rat == 1) {
			/* <scs> and <nrband> are both individually optional, so each
			 * one is only written when the caller actually supplied it. */
			if (scs >= 0) {
				n = snprintf(out + used, outlen - used, ",%d", scs);
				if (n < 0 || (size_t)n >= outlen - used)
					return -1;
				used += (size_t)n;

				if (nrband >= 0) {
					n = snprintf(out + used, outlen - used, ",%d",
						     nrband);
					if (n < 0 || (size_t)n >= outlen - used)
						return -1;
				}
			}
		}
	}
	return 0;
}

/* ================================================================== */
/* the shared write-then-read-back sequence                             */
/* ================================================================== */

/*
 * Every setting fm160d can write is stored in the module, and none of them
 * reports the outcome in its own answer: AT+GTACT=,,,103 answers OK whether or
 * not the modem clamped or ignored the value.  The only trustworthy
 * confirmation is reading the setting back and re-parsing it - which is also
 * what makes "the write landed" a statement about the modem rather than about
 * this queue.
 *
 * So the sequence is always: write -> read the setting back -> parse the
 * read-back -> answer the caller with the READ-BACK's status, not the write's.
 *
 * One flag serialises the whole class.  The manual forbids combining
 * GTACT / GTRAT / COPS / GTCELLLOCK with each other, and although the GNSS
 * writes are not named there they are configuration writes to the same flash:
 * queueing two of these back to back is never worth the risk, so the gate is
 * global to all four setters.
 *
 * This machinery used to live inside the two M4 setters only, with the
 * read-back's parser chosen by strstr() on the command string.  With four
 * setters the parser became a field (apply) instead of an inference, and the
 * GNSS writes added "after": a follow-up that runs once the sequence is done,
 * so the caller does not have to know the sequence exists.
 */
struct cfg_write {
	at_done_cb user_cb;
	void *user_arg;
	int  quiet_kind;
	void (*apply)(const char *resp);   /* parse the read-back; may be NULL */
	void (*after)(void);               /* optional follow-up; may be NULL  */
	char readback[FM160_AT_CMD_MAX];
};

static bool cfg_writing;

static void cfg_readback_cb(struct at_req *req, enum at_status status,
			    const char *response, void *arg)
{
	struct cfg_write *c = arg;

	/* The read-back, not the write, carries the new state: the module may
	 * clamp, reject or reinterpret a value, and only the read-back shows
	 * which of those happened. */
	if (status == AT_STATUS_OK && c->apply)
		c->apply(response);

	if (c->user_cb) {
		/* The caller is answered with the READ-BACK command, because that
		 * is the exchange the response actually belongs to. */
		struct at_req fake = { 0 };

		snprintf(fake.cmd, sizeof(fake.cmd), "%s", c->readback);
		c->user_cb(&fake, status, response, c->user_arg);
	}

	if (c->after)
		c->after();
	fm160_state_mark_dirty();
	cfg_writing = false;
	free(c);
}

static void cfg_write_cb(struct at_req *req, enum at_status status,
			 const char *response, void *arg)
{
	struct cfg_write *c = arg;
	int ret;

	if (status != AT_STATUS_OK) {
		if (c->user_cb)
			c->user_cb(req, status, response, c->user_arg);
		/* The write never landed, so whatever the quiet window was opened
		 * for is not happening: drop it rather than keeping the polls away
		 * for the rest of its duration. */
		if (g_state.quiet_kind == c->quiet_kind)
			atq_clear_quiet();
		cfg_writing = false;
		free(c);
		return;
	}

	/* The read-back is a fresh request; if the queue will not take it (no
	 * port, quiet window, full) the caller still has to be answered, and the
	 * context still has to be freed - otherwise the ubus request stays
	 * deferred forever and the caller hangs. */
	ret = atq_submit(AT_PRIO_STATE, c->readback, NULL, 5000, cfg_readback_cb, c);
	if (ret) {
		fm160_log(LOG_WARNING, "read-back %s rejected (rc=%d)",
			  c->readback, ret);
		if (c->user_cb)
			c->user_cb(req, AT_STATUS_BUSY, response, c->user_arg);
		cfg_writing = false;
		free(c);
	}
}

/*
 * Send one configuration write and then its read-back.
 *
 * readback is a WHOLE command line ("AT+GTACT?"), not a suffix, because a
 * write whose read-back is not the same setting cannot be confirmed - so the
 * pair is supplied together and neither is derived from the other.
 *
 * Nothing is logged here: the caller keeps its own log line so that each write
 * has its own name, which is also how the device verification script counts
 * them.
 */
static int cfg_write(const char *cmd, const char *readback, int timeout_ms,
		     int quiet_kind, const char *quiet_reason, int quiet_s,
		     void (*apply)(const char *), void (*after)(void),
		     at_done_cb cb, void *arg)
{
	struct cfg_write *c;
	int ret;

	if (cfg_writing)
		return -1;

	c = calloc(1, sizeof(*c));
	if (!c)
		return -1;
	c->user_cb = cb;
	c->user_arg = arg;
	c->quiet_kind = quiet_kind;
	c->apply = apply;
	c->after = after;
	snprintf(c->readback, sizeof(c->readback), "%s", readback);

	/* The flag goes up BEFORE the submit: atq_dispatch() answers NOPORT and
	 * "could not send" synchronously, so the callback may already have run
	 * (and cleared the flag) by the time atq_submit() returns.  Setting it
	 * afterwards would leave the flag stuck true and lock the write path
	 * out for the rest of the process's life. */
	cfg_writing = true;
	ret = atq_submit(AT_PRIO_INTERACTIVE, cmd, NULL, timeout_ms, cfg_write_cb, c);
	if (ret) {
		cfg_writing = false;
		free(c);
		return ret;
	}
	/* Only when the sequence is really in flight, and only when the caller
	 * asked for a window: a rejected write must not leave one behind with
	 * nothing to justify it, and a write with nothing to wait for (the GNSS
	 * engine switch) must not open one either - a QUIET_NONE/0 pair is how
	 * that is expressed. */
	if (cfg_writing && quiet_s > 0)
		atq_set_quiet(quiet_kind, quiet_reason, quiet_s);
	return 0;
}

int fm160_cmd_set_bands(const char *bands_csv, at_done_cb cb, void *arg)
{
	char cmd[FM160_AT_CMD_MAX];

	if (!g_state.gtact_caps.valid) {
		fm160_log(LOG_WARNING,
			  "refusing to write bands: AT+GTACT=? never enumerated");
		return -1;
	}
	if (fm160_bands_command(cmd, sizeof(cmd), bands_csv))
		return -1;

	fm160_log(LOG_WARNING, "band lock write: %s", cmd);
	/* A band change drops the current cell, so the quiet window keeps the
	 * polls off the module while it re-registers, and the derived CA view is
	 * refreshed once the read-back has been parsed. */
	if (cfg_write(cmd, "AT+GTACT?", 8000, QUIET_BANDS,
		      "band lock write, re-registering", 8,
		      fm160_parse_gtact, fm160_cmd_poll_ca, cb, arg))
		return -1;
	return 0;
}

int fm160_cmd_set_celllock(int mode, int rat, int type, unsigned long long earfcn,
			   int pci, int scs, int nrband, at_done_cb cb, void *arg)
{
	char cmd[FM160_AT_CMD_MAX];

	if (!g_state.celllock_caps.valid) {
		fm160_log(LOG_WARNING,
			  "refusing to write cell lock: AT+GTCELLLOCK=? never enumerated");
		return -1;
	}
	if (fm160_celllock_command(cmd, sizeof(cmd), mode, rat, type, earfcn,
				   pci, scs, nrband))
		return -1;

	fm160_log(LOG_WARNING, "cell lock write: %s", cmd);
	/* The write only takes effect after a UE reset, which fm160d never
	 * performs; the quiet window only covers the EFS commit. */
	if (cfg_write(cmd, "AT+GTCELLLOCK?", 10000, QUIET_CELLLOCK,
		      "cell lock write to EFS", 8, fm160_parse_celllock, NULL,
		      cb, arg))
		return -1;
	return 0;
}

/* ================================================================== */
/* M5: GNSS                                                             */
/* ================================================================== */

/*
 * The GNSS command set is not in the AT manual at all - it comes from the
 * vendor's separate Application Guide_GNSS_V1.0 - and where that guide and the
 * hardware disagree, the hardware is what this file follows.  The four measured
 * facts that shape everything below (docs/AT-FACTS.md section 10):
 *
 *   1. NMEA arrives as the RESPONSE to AT+GTGPS? on the AT port.  There is no
 *      GNSS interface in this USB mode, so nothing new is opened, leased or
 *      listened on: the whole milestone rides the ttyUSB2 channel that has been
 *      running since M1.
 *   2. The <item> argument MUST be quoted.  AT+GTGPS=RMC answers ERROR;
 *      AT+GTGPS="RMC" answers OK.  Both failures look identical to "the engine
 *      is off", so a wrong quote is expensive to diagnose - hence the builder
 *      below.
 *   3. With the engine off, AT+GTGPS? answers ERROR as well.  The read is
 *      therefore never attempted unless the power state says the engine is on.
 *   4. The sentences do NOT follow the header on the same line.  The response is
 *      a bare "+GTGPS: " line and then the sentences, so a parser keyed on the
 *      header would find nothing at all.
 */

/* ------------------------------------------------------------------ */
/* M5 command builders                                                  */
/* ------------------------------------------------------------------ */

/*
 * AT+GTGPS[=<item>] - read the NMEA buffer, or one item of it.
 *
 * The quotes are syntax, not decoration (see fact 2 above).  The item is
 * restricted to exactly three uppercase letters rather than escaped: that is
 * the whole alphabet the modem advertises ("RMC","GGA","GSA","GSV"), and
 * refusing anything else is the only way a space, comma or quote can never
 * reach the command line.
 *
 * item == NULL or "" means "every sentence", which is the form the poll uses:
 * one transaction returns the whole block, so asking for the four items
 * separately would quadruple the cost for the same data.
 */
int fm160_gnss_item_command(char *out, size_t outlen, const char *item)
{
	int n, i;

	if (!item || !item[0]) {
		n = snprintf(out, outlen, "AT+GTGPS?");
		return (n < 0 || (size_t)n >= outlen) ? -1 : 0;
	}
	for (i = 0; i < 3; i++)
		if (item[i] < 'A' || item[i] > 'Z')
			return -1;
	if (item[3])
		return -1;

	n = snprintf(out, outlen, "AT+GTGPS=\"%s\"", item);
	return (n < 0 || (size_t)n >= outlen) ? -1 : 0;
}

/* AT+GTGPSPOWER=<x> - 0 off, 1 on.  Not stored by the module. */
int fm160_gnss_power_command(char *out, size_t outlen, int on)
{
	int n;

	if (on != 0 && on != 1)
		return -1;
	n = snprintf(out, outlen, "AT+GTGPSPOWER=%d", on);
	return (n < 0 || (size_t)n >= outlen) ? -1 : 0;
}

/*
 * The satellite combinations the GNSS guide defines for AT+GTGPSCFG=2.
 *
 * This is the only value list in this file taken from documentation instead of
 * from the module, and it is deliberate: the field is a protocol enumeration of
 * the command itself, not a "device dependent" capability like the USB modes or
 * the band lists.  It is used as an UPPER bound only - the write path
 * additionally requires the value to appear in the module's own
 * AT+GTGPSCFG=? answer, so a firmware with a wider list still cannot be sent a
 * value this table does not understand.
 */
static const int gnss_constellations[] = {
	0,  /* GPS + GLONASS                                              */
	2,  /* GPS + Galileo                                              */
	3,  /* GPS + QZSS                                                 */
	4,  /* GPS + BeiDou + Galileo                                     */
	5,  /* GPS + BeiDou + GLONASS                                     */
	6,  /* GPS + BeiDou + QZSS                                        */
	7,  /* GPS + GLONASS + Galileo                                    */
	14, /* GPS + BeiDou + Galileo + GLONASS + QZSS (this module's value) */
	15, /* GPS only                                                   */
};

static bool gnss_constellation_documented(int v)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(gnss_constellations); i++)
		if (gnss_constellations[i] == v)
			return true;
	return false;
}

/*
 * AT+GTGPSCFG=2,<value> - the satellite combination.
 *
 * x is fixed to 2 in the builder.  x=0 (SUPL version) and x=3 (SUPL
 * certificate) belong to AGPS, which cannot do anything without a data
 * connection and was never verified on hardware; x=1 (xtra) is not even
 * reported by this firmware.  Taking x from the caller would offer three
 * unverified persistent writes for the price of one.
 */
int fm160_gnss_cfg_command(char *out, size_t outlen, int value)
{
	int n;

	if (!gnss_constellation_documented(value))
		return -1;
	n = snprintf(out, outlen, "AT+GTGPSCFG=2,%d", value);
	return (n < 0 || (size_t)n >= outlen) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* M5 parsers: the NMEA block                                           */
/* ------------------------------------------------------------------ */

#define NMEA_FIELD_MAX 32   /* a GSV is 4 header fields + 4 quad groups */

/*
 * One field of a sentence.  An EMPTY field means "the modem has no value here",
 * which is a different state from 0: elevation 0 degrees is a real elevation, so
 * it too would be ambiguous, and NMEA happens to use the empty field for
 * "unknown" throughout.
 */
static int nmea_int(const char *f)
{
	if (!f || !f[0])
		return FM160_NONE;
	return atoi(f);
}

/*
 * A field NMEA prints with one decimal, in tenths: "1.2" -> 12.
 *
 * Parsed from the string rather than through a double so that no rounding rule
 * has to be agreed with the modem, and only ONE fractional digit is taken - a
 * "1.25" (which no NMEA field produces, but a firmware might) then reads as 12
 * instead of drifting to 13.
 */
static int nmea_x10(const char *f)
{
	char buf[24];
	const char *dot;
	size_t len, i;
	int whole, frac = 0, neg = 0;
	bool digits = false;

	if (!f || !f[0])
		return FM160_NONE;
	len = strlen(f);
	if (len >= sizeof(buf))
		return FM160_NONE;
	memcpy(buf, f, len + 1);

	if (buf[0] == '-') {
		neg = 1;
		memmove(buf, buf + 1, strlen(buf + 1) + 1);
	}
	dot = strchr(buf, '.');
	if (dot) {
		if (dot[1] >= '0' && dot[1] <= '9')
			frac = dot[1] - '0';
		*(char *)dot = '\0';
	}
	for (i = 0; buf[i]; i++) {
		if (buf[i] < '0' || buf[i] > '9')
			return FM160_NONE;
		digits = true;
	}
	if (!digits)
		return FM160_NONE;
	whole = atoi(buf);
	return (neg ? -1 : 1) * (whole * 10 + frac);
}

/*
 * NMEA latitude/longitude - ddmm.mmmm and dddmm.mmmm - as degrees x 1e7.
 *
 * Computed as a double and rounded to an integer: a tenth of a centimetre is far
 * below anything a receiver reports, so no precision is lost, and the integer
 * keeps the rest of the daemon float-free.  Latitude fits an int32 at 90 degrees
 * x 1e7 with room to spare.
 *
 * A coordinate of exactly 0 is treated as absent.  The one false negative is a
 * position exactly on the equator or the prime meridian, and it is worth it: 0,0
 * is also what a receiver without a fix reports, and "0.000000, 0.000000" on a
 * map is a confidently wrong answer.
 */
static int nmea_degrees_1e7(const char *f)
{
	double v, deg;
	int d;

	if (!f || !f[0])
		return FM160_NONE;
	v = strtod(f, NULL);
	if (v <= 0)
		return FM160_NONE;
	d = (int)(v / 100.0);
	deg = (double)d + (v - (double)d * 100.0) / 60.0;
	return (int)(deg * 10000000.0 + 0.5);
}

/*
 * The NMEA checksum is the XOR of every character between '$' and '*'.
 *
 * It is computed for real rather than skipped, and that is the whole reason this
 * function exists: the sentence the modem emits with nothing in view is
 *
 *     $GPGSV,1,1,0,1*54
 *
 * which is NOT the shape the standard describes - it carries one field more than
 * it should.  Shape therefore cannot be used to tell a real sentence from a
 * mangled one; only the checksum can.
 */
static bool nmea_checksum_ok(const char *body, size_t len, unsigned want)
{
	unsigned c = 0;
	size_t i;

	for (i = 0; i < len; i++)
		c ^= (unsigned char)body[i];
	return c == (want & 0xff);
}

/* Split a comma-separated body in place.  A trailing comma yields a trailing
 * empty field, which is what the callers expect: an empty field is data. */
static int nmea_split(char *body, char *f[], int max)
{
	int n = 0;
	char *p = body;

	if (!*p || max < 1)
		return 0;
	f[n++] = p;
	for (; *p && n < max; p++) {
		if (*p == ',') {
			*p = '\0';
			f[n++] = p + 1;
		}
	}
	return n;
}

/*
 * Slot for one talker prefix, created on first sight.
 *
 * Keyed on the raw two characters, so a constellation this build has never seen
 * still gets its own row with its prefix recorded - the same rule the band
 * parser follows with tokens it cannot decode.  "GB" is mapped to BeiDou
 * alongside "BD" because that is the older prefix for the same system; the slot
 * stays keyed on the raw prefix either way, so a module that used both would show
 * two rows rather than losing one.
 */
static int gnss_slot(struct fm160_gnss_reading *r, const char *talker)
{
	int i, n;

	for (i = 0; i < r->cons_n; i++)
		if (!strncmp(r->cons[i].talker, talker, 2))
			return i;
	if (r->cons_n >= FM160_GNSS_CONST_MAX)
		return -1;

	n = r->cons_n++;
	memset(&r->cons[n], 0, sizeof(r->cons[n]));
	snprintf(r->cons[n].talker, sizeof(r->cons[n].talker), "%s", talker);
	r->cons[n].visible = FM160_NONE;
	r->cons[n].snr_best_db = FM160_NONE;
	if (!strcmp(talker, "GP"))
		r->cons[n].kind = GNSS_KIND_GPS;
	else if (!strcmp(talker, "BD") || !strcmp(talker, "GB"))
		r->cons[n].kind = GNSS_KIND_BDS;
	else if (!strcmp(talker, "GL"))
		r->cons[n].kind = GNSS_KIND_GLO;
	else if (!strcmp(talker, "GA"))
		r->cons[n].kind = GNSS_KIND_GAL;
	else if (!strcmp(talker, "PQ"))
		r->cons[n].kind = GNSS_KIND_QZSS;
	else
		r->cons[n].kind = GNSS_KIND_OTHER;
	return n;
}

/*
 * $--GSV,<messages>,<number>,<in view>[,<prn>,<elev>,<azim>,<snr>]...[,<extra>]
 *
 * Groups of four are emitted while four whole fields remain.  That tolerance is
 * not defensive padding, it is required: the measured zero-satellite sentence has
 * ONE field after <in view> where the standard has none.  It matches the NMEA
 * 4.10+ signalId convention (GPS L1 -> 1, Galileo E1 -> 7, and the measured
 * $GAGSV,1,1,0,7*43 agrees), so it is recorded as evidence and otherwise left
 * alone - a parser that demanded the standard shape would reject a perfectly
 * normal "nothing in view" answer.
 */
static void gnss_gsv(struct fm160_gnss_reading *r, const char *talker,
		     int nf, char *f[])
{
	int slot = gnss_slot(r, talker), quads, i, visible;
	struct fm160_gnss_const *c;

	if (slot < 0) {
		r->ignored++;
		return;
	}
	c = &r->cons[slot];

	/* <in view> is repeated in every message of a multi-message GSV.  The
	 * larger value wins, so a receiver that only fills it in on its first
	 * message is not read as "nothing visible" on the second. */
	visible = nf > 3 ? nmea_int(f[3]) : FM160_NONE;
	if (visible != FM160_NONE && (c->visible == FM160_NONE || visible > c->visible))
		c->visible = visible;

	quads = (nf - 4) / 4;
	for (i = 0; i < quads; i++) {
		char **q = &f[4 + i * 4];
		struct fm160_gnss_sat *s;

		if (r->sats_n >= FM160_GNSS_SAT_MAX)
			break;
		s = &r->sats[r->sats_n++];
		memset(s, 0, sizeof(*s));
		s->const_idx = slot;
		s->prn      = nmea_int(q[0]);
		s->elev_deg = nmea_int(q[1]);
		s->azim_deg = nmea_int(q[2]);
		s->snr_db   = nmea_int(q[3]);
		c->parsed++;
		if (s->snr_db != FM160_NONE &&
		    (c->snr_best_db == FM160_NONE || s->snr_db > c->snr_best_db))
			c->snr_best_db = s->snr_db;
	}

	if ((nf - 4) % 4) {
		r->gsv_trailing = true;
		r->gsv_trailing_value = nmea_int(f[4 + quads * 4]);
	}
}

/*
 * $--GSA,<mode>,<fix type>,<prn x12>,<pdop>,<hdop>,<vdop>
 *
 * The three dilution fields are addressed from the END of the sentence, whatever
 * the number of PRN slots in front of them.  Reading them at a fixed offset would
 * turn a sentence with fewer slots into a satellite list with three DOPs in it,
 * and a DOP of "1.2" would be reported as PRN 1.
 *
 * <fix type>: 1 no fix, 2 2D, 3 3D.  Measured no-fix frames carry
 * "A,1,,...,,," i.e. mode A, type 1 and every field empty.
 */
static void gnss_gsa(struct fm160_gnss_reading *r, const char *talker,
		     int nf, char *f[])
{
	int slot = gnss_slot(r, talker), dop_base, i;

	if (slot < 0) {
		r->ignored++;
		return;
	}
	if (nf < 5) {
		r->bad_shape++;
		return;
	}

	/* Kept as the raw letter ('A' = automatic, 'M' = manual) so nothing has
	 * to be decided about it here. */
	r->fix_mode = (int)(unsigned char)f[1][0];
	r->fix_type = nmea_int(f[2]);
	if (r->fix_type == FM160_NONE)
		r->fix_type = 0;

	dop_base = nf - 3;
	if (dop_base > 3) {
		for (i = 3; i < dop_base; i++) {
			int prn = nmea_int(f[i]);

			if (prn == FM160_NONE)
				continue;
			r->sats_used++;
			if (r->cons[slot].used_n < FM160_GNSS_USED_MAX)
				r->cons[slot].used_prn[r->cons[slot].used_n++] = prn;
		}
	}

	/*
	 * The three dilutions are taken as ONE unit from the first sentence that
	 * carries a complete triple, and nothing later may change them:
	 *   - a multi-constellation receiver emits one GSA per constellation
	 *     (measured: GPS, BeiDou, Galileo, QZSS all present), and the order is
	 *     fixed by the modem, GPS first.  Letting each one overwrite would make
	 *     the published PDOP/HDOP/VDOP depend on which constellation happens
	 *     to be emitted last.
	 *   - a sparse later sentence - one that lists no DOP at all, which is the
	 *     measured no-fix shape - would otherwise ERASE a good triple and
	 *     replace it with three sentinels.
	 * So: the first complete triple wins, and an incomplete one is never
	 * published.  Requiring all three together also keeps the unit intact -
	 * mixing one constellation's PDOP with another's VDOP would describe a
	 * solution that does not exist.
	 */
	{
		int p = nmea_x10(f[dop_base]);
		int h = nmea_x10(f[dop_base + 1]);
		int v = nmea_x10(f[dop_base + 2]);

		if (r->pdop_x10 == FM160_NONE &&
		    p != FM160_NONE && h != FM160_NONE && v != FM160_NONE) {
			r->pdop_x10 = p;
			r->hdop_x10 = h;
			r->vdop_x10 = v;
		}
	}
	r->cons[slot].in_fix = r->cons[slot].used_n;
}

/*
 * $--GGA,<utc>,<lat>,<N/S>,<lon>,<E/W>,<quality>,<in use>,<hdop>,<alt>,M,...
 *
 * Present only once there is a fix - the measured no-fix block contains no GGA
 * line at all - so nothing in here may be treated as required.
 *
 * GGA carries an HDOP of its own and it is deliberately IGNORED: the DOP triple
 * is taken as a unit from GSA so PDOP, HDOP and VDOP always describe the same
 * solution.  Taking GGA's here would also make the result depend on the order the
 * two sentences happen to arrive in.
 */
static void gnss_gga(struct fm160_gnss_reading *r, int nf, char *f[])
{
	int lat, lon;

	if (nf < 7)
		return;
	if (f[1][0])
		snprintf(r->utc, sizeof(r->utc), "%s", f[1]);

	lat = nmea_degrees_1e7(f[2]);
	lon = nmea_degrees_1e7(f[4]);
	if (lat != FM160_NONE && lon != FM160_NONE) {
		r->lat_ns = f[3][0];
		r->lon_ew = f[5][0];
		r->lat_1e7 = (r->lat_ns == 'S') ? -lat : lat;
		r->lon_1e7 = (r->lon_ew == 'W') ? -lon : lon;
		r->has_position = true;
	}

	r->quality = nmea_int(f[6]);
	if (r->quality == FM160_NONE)
		r->quality = 0;
	if (nf > 7)
		r->sats_in_use = nmea_int(f[7]);
	if (nf > 9)
		r->alt_dm = nmea_x10(f[9]);
	if (nf > 11)
		r->geoid_dm = nmea_x10(f[11]);
}

/*
 * $--RMC,<utc>,<status>,<lat>,<N/S>,<lon>,<E/W>,<speed knots>,<course>,<date>
 *
 * <status> is 'A' (valid) or 'V' (void).  A void sentence still carries fields,
 * usually the last position the receiver had, so the position is only taken when
 * the status is 'A' - otherwise a stale fix would be displayed as a live one.
 *
 * Speed arrives in knots and is stored in cm/s, the unit the rest of the
 * daemon's face-value integers use.  1 knot = 51.4444 cm/s.
 */
static void gnss_rmc(struct fm160_gnss_reading *r, int nf, char *f[])
{
	int lat, lon;

	if (nf < 10)
		return;
	if (f[1][0] && !r->utc[0])
		snprintf(r->utc, sizeof(r->utc), "%s", f[1]);
	if (f[9][0])
		snprintf(r->date, sizeof(r->date), "%s", f[9]);
	if (f[2][0] != 'A')
		return;

	lat = nmea_degrees_1e7(f[3]);
	lon = nmea_degrees_1e7(f[5]);
	if (lat != FM160_NONE && lon != FM160_NONE) {
		r->lat_ns = f[4][0];
		r->lon_ew = f[6][0];
		r->lat_1e7 = (r->lat_ns == 'S') ? -lat : lat;
		r->lon_1e7 = (r->lon_ew == 'W') ? -lon : lon;
		r->has_position = true;
	}
	if (f[7][0])
		r->speed_cmps = (int)(strtod(f[7], NULL) * 51.4444 + 0.5);
	r->course_d10 = nmea_x10(f[8]);
}

/* Keep the sentences verbatim for the page's raw-output box.  Truncation is
 * recorded through raw_len, so the page can say the text is incomplete instead of
 * showing a silently shortened block. */
static void gnss_remember_raw(struct fm160_gnss_reading *r, const char *line,
			      size_t len)
{
	if (r->raw_len + (int)len + 2 >= FM160_GNSS_RAW_MAX)
		return;
	memcpy(r->raw + r->raw_len, line, len);
	r->raw_len += (int)len;
	r->raw[r->raw_len++] = '\n';
	r->raw[r->raw_len] = '\0';
}

/* One line: verify it, then dispatch on its formatter.
 *
 * The checksum gate comes FIRST, before the line is looked at in any other way.
 * A sentence whose *hh does not match may have been corrupted anywhere,
 * including in its formatter, so nothing in it can be trusted - not even the
 * question "is this a well-formed sentence?".  The visible consequence is that
 * a short formatter with a bad checksum is counted as bad_checksum rather than
 * bad_shape; that is the honest classification, and it is why bad_shape should
 * be read as "the module emitted something structurally wrong" only among the
 * sentences that passed the checksum.
 */
static void gnss_sentence(struct fm160_gnss_reading *r, char *line, size_t line_len)
{
	char *star, *f[NMEA_FIELD_MAX], tk[4];
	const char *form;
	size_t blen;
	int want, nf;

	if (*line != '$') {
		r->bad_shape++;
		return;
	}
	star = strrchr(line, '*');
	if (!star || strlen(star + 1) < 2) {
		r->bad_shape++;
		return;
	}

	blen = (size_t)(star - (line + 1));
	want = (int)strtol(star + 1, NULL, 16);
	if (!nmea_checksum_ok(line + 1, blen, (unsigned)want)) {
		r->bad_checksum++;
		return;
	}
	*star = '\0';      /* the body is now a plain NUL-terminated string */

	nf = nmea_split(line + 1, f, NMEA_FIELD_MAX);
	if (nf < 1 || strlen(f[0]) < 5) {
		r->bad_shape++;
		return;
	}

	form = f[0] + 2;                                   /* "GSV", "GSA", ... */
	snprintf(tk, sizeof(tk), "%c%c", f[0][0], f[0][1]);

	r->sentences++;
	r->nmea_bytes += (int)line_len;

	if (!strcmp(form, "GSV"))
		gnss_gsv(r, tk, nf, f);
	else if (!strcmp(form, "GSA"))
		gnss_gsa(r, tk, nf, f);
	else if (!strcmp(form, "GGA"))
		gnss_gga(r, nf, f);
	else if (!strcmp(form, "RMC"))
		gnss_rmc(r, nf, f);
	else
		r->ignored++;    /* VTG, ZDA, GLL, GST, TXT ... counted, not parsed */
}

/*
 * Fold the two halves together once the whole block has been read.
 *
 * GSA carries the PRN lists and GSV carries the satellites, and nothing
 * guarantees which of the two comes first - so "is this satellite part of the
 * fix" can only be answered for the block as a whole.  The constellation totals
 * are summed here for the same reason: a GSV that arrived before its GSA has its
 * <in view> count already, and one that arrived after does too, so the sum must
 * happen at the end rather than as each sentence is parsed.
 */
static void gnss_link_fix(struct fm160_gnss_reading *r)
{
	int i, j, total = 0, best = FM160_NONE;

	for (i = 0; i < r->cons_n; i++) {
		if (r->cons[i].visible != FM160_NONE)
			total += r->cons[i].visible;
		if (r->cons[i].snr_best_db != FM160_NONE &&
		    (best == FM160_NONE || r->cons[i].snr_best_db > best))
			best = r->cons[i].snr_best_db;
	}
	for (j = 0; j < r->sats_n; j++) {
		struct fm160_gnss_sat *s = &r->sats[j];
		struct fm160_gnss_const *c;
		int k;

		if (s->const_idx < 0 || s->const_idx >= r->cons_n) {
			s->in_fix = false;
			continue;
		}
		c = &r->cons[s->const_idx];
		s->in_fix = false;
		for (k = 0; k < c->used_n; k++)
			if (c->used_prn[k] == s->prn) {
				s->in_fix = true;
				break;
			}
	}
	r->visible_total = total;
	r->snr_best_db = best;
}

/* The values a fresh reading starts from.  Every one of these is a field the
 * modem leaves EMPTY while there is no fix, so a default of 0 would be a wrong
 * value rather than an unfilled one. */
static void gnss_reading_defaults(struct fm160_gnss_reading *r)
{
	r->fix_type = FM160_NONE;
	r->pdop_x10 = r->hdop_x10 = r->vdop_x10 = FM160_NONE;
	r->quality = FM160_NONE;
	r->sats_in_use = FM160_NONE;
	r->alt_dm = r->geoid_dm = FM160_NONE;
	r->speed_cmps = r->course_d10 = FM160_NONE;
	r->snr_best_db = FM160_NONE;
	r->gsv_trailing_value = FM160_NONE;
}

/*
 * AT+GTGPS? - the whole NMEA block.
 *
 * Measured shape (engine on, no fix, 8 sentences):
 *
 *   \r\n+GTGPS: \r\n
 *   $GPGSA,A,1,,,,,,,,,,,,,,,,*32
 *   $BDGSA,... $GAGSA,... $PQGSA,...
 *   $GPGSV,1,1,0,1*54   $BDGSV,... $GLGSV,... $GAGSV,1,1,0,7*43
 *   \r\nOK\r\n
 *
 * Three consequences, all of them handled below:
 *   - the header carries no data, so every line is examined for '$';
 *   - only GSA and GSV are present, because position and time sentences are
 *     omitted while there is no fix.  "The block is healthy" therefore CANNOT be
 *     defined as "it contains an RMC" - it is defined as "it contains sentences
 *     whose checksums match";
 *   - an OK with no sentence at all is a normal, measured state (the first read
 *     after switching the engine on), so it must not be reported as a failure.
 */
void fm160_parse_gnss(const char *resp)
{
	struct fm160_gnss_state *g = &g_state.gnss;
	struct fm160_gnss_reading r;
	const char *p;
	uint64_t now = fm160_now_ms();

	if (!resp)
		return;

	memset(&r, 0, sizeof(r));
	gnss_reading_defaults(&r);
	r.resp_bytes = (int)strlen(resp);

	p = strstr(resp, "+GTGPS:");
	if (!p) {
		/* No header and no error: not a shape this parser has ever seen.
		 * Leave the previous picture alone rather than half-updating it. */
		fm160_log(LOG_DEBUG, "GNSS: no +GTGPS header in the answer");
		return;
	}

	while (*p) {
		const char *eol = p + strcspn(p, "\r\n");
		size_t len = (size_t)(eol - p);
		char line[FM160_RESP_LINE_MAX];

		if (len) {
			if (len >= sizeof(line)) {
				r.bad_shape++;
			} else {
				memcpy(line, p, len);
				line[len] = '\0';
				/* '$' is the only test needed to pick the sentences
				 * out of the response: the echoed command, the header,
				 * the blank line and the final OK all fail it. */
				if (line[0] == '$') {
					gnss_remember_raw(&r, line, len);
					gnss_sentence(&r, line, len);
				}
			}
		}
		p = *eol ? eol + 1 : eol;
	}

	if (!r.sentences && !r.bad_checksum && !r.bad_shape && !r.ignored) {
		/* OK, but not one sentence yet - measured right after
		 * AT+GTGPSPOWER=1, while the receiver is still coming up.
		 *
		 * That is neither an error nor "there are no satellites", so the
		 * picture is left exactly as it was, INCLUDING its timestamp: an
		 * empty poll is not a new reading, and moving read_ms here would
		 * make ten-minute-old satellites look like they arrived just now.
		 * Only the poll outcome is recorded.  `empty_frames` counts them in
		 * a row, which is what separates "the engine is still starting" from
		 * "the engine claims to be on but never produces a sentence". */
		g->r.empty_frame  = true;
		g->r.empty_frames++;
		g->r.empty_ms     = now;
		g->r.empty_bytes  = r.resp_bytes;
		g->r.read_error   = false;
		fm160_log(LOG_DEBUG, "GNSS: empty frame (%d bytes), no sentence yet",
			  r.resp_bytes);
		fm160_state_mark_dirty();
		return;
	}

	gnss_link_fix(&r);
	r.read_ms = now;
	r.empty_frame = false;
	r.read_error = false;

	{
		bool first_sentences = g->r.sentences == 0 && r.sentences > 0;
		bool fix_appeared = !g->r.has_position && r.has_position;

		g->r = r;

		if (fix_appeared)
			fm160_log(LOG_INFO,
				  "GNSS: position fix acquired (%d satellite(s) in use, %d visible)",
				  r.sats_used, r.visible_total);
		else if (first_sentences)
			fm160_log(LOG_INFO,
				  "GNSS: %d sentence(s), %d visible, fix type %d",
				  r.sentences, r.visible_total, r.fix_type);
		else
			fm160_log(LOG_DEBUG,
				  "GNSS: %d sentence(s), %d visible, fix type %d",
				  r.sentences, r.visible_total, r.fix_type);
	}
	fm160_state_mark_dirty();
}

/* ------------------------------------------------------------------ */
/* M5 parsers: engine, configuration, AGPS                              */
/* ------------------------------------------------------------------ */

/*
 * AT+GTGPSPOWER? -> "+GTGPSPOWER: 0|1"
 *
 * Asked for rather than remembered: the module does not store this setting, so
 * after a power cycle the engine is off again and a cached value would be a
 * confident lie until the next poll.
 */
void fm160_parse_gnss_power(const char *resp)
{
	char buf[FM160_STR_MAX];
	struct fm160_gnss_engine *e = &g_state.gnss.engine;
	bool was_on = e->on;

	if (!fm160_resp_find(resp, "+GTGPSPOWER:", buf, sizeof(buf)))
		return;

	e->known = true;
	e->on = (atoi(buf) == 1);
	e->last_ok_ms = fm160_now_ms();
	if (was_on != e->on)
		fm160_log(LOG_INFO, "GNSS engine is now %s", e->on ? "on" : "off");
	fm160_state_mark_dirty();
}

/*
 * AT+GTGPSCFG? -> a header and then one "<x>,<value>" line per group.
 *
 * Measured: "0,2" / "2,14" / "3,0" - three lines against a manual that documents
 * four groups, i.e. x=1 (xtra) is simply not reported by this firmware.  The
 * lines are therefore matched BY THEIR x and never by their position; a missing
 * line is a reported state (xtra_present) rather than a parse failure, and an x
 * this parser does not know is counted instead of being folded into its
 * neighbour.
 *
 * The fields are written into g_state.gnss.cfg one by one instead of assigning a
 * whole fresh struct, because the same struct also holds the capability list the
 * write path depends on - and a plain read of the current value has no business
 * invalidating the licence to change it.
 */
void fm160_parse_gnss_cfg(const char *resp)
{
	struct fm160_gnss_cfg *cfg = &g_state.gnss.cfg;
	const char *p;

	if (!resp)
		return;
	p = strstr(resp, "+GTGPSCFG");
	if (!p)
		return;
	p = strchr(p, '\n');
	if (!p)
		return;
	p++;

	cfg->supl_version = FM160_NONE;
	cfg->constellation = FM160_NONE;
	cfg->cert = FM160_NONE;
	cfg->xtra = FM160_NONE;
	cfg->xtra_present = false;
	cfg->unknown = 0;

	while (*p) {
		const char *eol = p + strcspn(p, "\r\n");
		size_t len = (size_t)(eol - p);

		if (len && len < FM160_STR_MAX) {
			char line[FM160_STR_MAX], *comma;

			memcpy(line, p, len);
			line[len] = '\0';
			comma = strchr(line, ',');
			if (comma && line[0] >= '0' && line[0] <= '9') {
				int x = atoi(line);
				int v = comma[1] ? atoi(comma + 1) : FM160_NONE;

				switch (x) {
				case 0:  cfg->supl_version = v; break;
				case 1:  cfg->xtra_present = true; cfg->xtra = v; break;
				case 2:  cfg->constellation = v; break;
				case 3:  cfg->cert = v; break;
				default: cfg->unknown++; break;
				}
			}
		}
		p = *eol ? eol + 1 : eol;
	}

	cfg->valid = true;
	cfg->last_ok_ms = fm160_now_ms();
	fm160_log(LOG_INFO,
		  "GNSS config: constellation=%d supl=%d xtra=%s cert=%d",
		  cfg->constellation, cfg->supl_version,
		  cfg->xtra_present ? "reported" : "absent", cfg->cert);
	fm160_state_mark_dirty();
}

/*
 * One parenthesised capability group, as a list of values.
 *
 * The manual writes value lists as "(0,1,2)" but the M4 capability answers on
 * this firmware use ranges for some fields, so both forms are accepted.  A MIXED
 * form ("0,1-3") is refused rather than expanded: expanding it can only produce a
 * superset, and a superset here weakens the gate that keeps unverified values out
 * of a persistent write.
 */
static int gnss_cap_expand(const char *group, int *out, int max)
{
	const char *comma = strchr(group, ',');
	const char *hyphen = strchr(group, '-');

	if (comma) {
		if (hyphen)
			return 0;
		return parse_int_list(group, out, max);
	}
	if (!hyphen)
		return parse_int_list(group, out, max);

	{
		int lo = atoi(group), hi = atoi(hyphen + 1), v, n = 0;

		if (hi < lo || hi - lo > 256)
			return 0;
		for (v = lo; v <= hi && n < max; v++)
			out[n++] = v;
		return n;
	}
}

/*
 * The x in "GTGPSCFG: 2,(0-15)" names the field the group belongs to.
 *
 * It is read from the header rather than from the line's position, because the
 * modem omits x=1 entirely: a positional reading would shift x=3's set onto
 * x=2, which is precisely the field the write path uses.  A line with no header
 * at all returns FM160_GNSS_CFG_X_NONE - the values are still kept and logged,
 * but they cannot license anything.
 */
static int gnss_cfg_slot_of(const char *line)
{
	const char *h = strstr(line, "GTGPSCFG");
	const char *c, *p;

	if (!h)
		return FM160_GNSS_CFG_X_NONE;
	c = strchr(h, ':');
	if (!c)
		return FM160_GNSS_CFG_X_NONE;

	p = c + 1;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p < '0' || *p > '9')
		return FM160_GNSS_CFG_X_NONE;

	{
		int x = atoi(p);

		return (x >= 0 && x < FM160_GNSS_CFG_X_MAX) ? x :
		       FM160_GNSS_CFG_X_NONE;
	}
}

/*
 * AT+GTGPSCFG=? -> the licence for the constellation write.
 *
 * Measured 2026-09-19 (AT-FACTS 10.6): the answer is one line per field, each
 * carrying its own value set, and the sets genuinely differ - x=0 is (0-2)
 * while x=2 is (0-15).  Each line is therefore attributed to its x and stored
 * on its own, and the write path checks fm160_gnss_cfg_x_write and nothing
 * else.  (fm160d.h records why the flattened, union-of-everything version that
 * came before was wrong even where it happened to give the right answer.)
 */
void fm160_parse_gnss_cfg_caps(const char *resp)
{
	struct fm160_gnss_cfg *cfg = &g_state.gnss.cfg;
	const char *p;
	int lines = 0, i, k;

	if (!resp)
		return;

	memset(cfg->caps_n, 0, sizeof(cfg->caps_n));
	memset(cfg->caps_values, 0, sizeof(cfg->caps_values));
	cfg->caps_x_mask = 0;
	cfg->caps_groups = 0;

	for (p = resp; *p; ) {
		const char *eol = strpbrk(p, "\r\n");
		char line[FM160_RESP_LINE_MAX];
		char groups[4][FM160_RESP_LINE_MAX];
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		int slot, ng;

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = '\0';
		p = eol ? eol + strspn(eol, "\r\n") : p + strlen(p);

		/* Only lines that carry a group are answers. */
		if (!strchr(line, '('))
			continue;

		lines++;
		slot = gnss_cfg_slot_of(line);
		if (slot < FM160_GNSS_CFG_X_MAX)
			cfg->caps_x_mask |= 1 << slot;

		ng = split_groups(line, groups, ARRAY_SIZE(groups));
		for (i = 0; i < ng; i++) {
			int vals[FM160_GNSS_CAP_MAX];
			int n = gnss_cap_expand(groups[i], vals, ARRAY_SIZE(vals));

			for (k = 0; k < n; k++) {
				int j;
				bool dup = false;

				for (j = 0; j < cfg->caps_n[slot]; j++)
					if (cfg->caps_values[slot][j] == vals[k])
						dup = true;
				if (!dup && cfg->caps_n[slot] < FM160_GNSS_CAP_MAX)
					cfg->caps_values[slot][cfg->caps_n[slot]++] = vals[k];
			}
		}
	}

	cfg->caps_groups = lines;
	/* The licence covers the field the write targets, and nothing else. */
	cfg->caps_valid = cfg->caps_n[FM160_GNSS_CFG_X_WRITE] > 0;

	fm160_log(LOG_INFO,
		  "GNSS config caps: x0=%d x1=%d x2=%d x3=%d unkeyed=%d (%d line(s)) - %s",
		  cfg->caps_n[0], cfg->caps_n[1], cfg->caps_n[2], cfg->caps_n[3],
		  cfg->caps_n[FM160_GNSS_CFG_X_NONE], lines,
		  cfg->caps_valid ? "writing is licensed" :
		  "no set for x=2, writing stays disabled");
	fm160_state_mark_dirty();
}

/* AT+GTGPSEPO? -> "+GTGPSEPO: <x>", 0 off / 1 MSB / 2 MSA.  The guide's own
 * example says "(0,2)" while the module answers "(0-2)"; read-only here, so the
 * difference only decides what the page can display. */
void fm160_parse_gnss_epo(const char *resp)
{
	char buf[FM160_STR_MAX];

	if (!fm160_resp_find(resp, "+GTGPSEPO:", buf, sizeof(buf)))
		return;
	g_state.gnss.agps.epo = atoi(buf);
	g_state.gnss.agps.valid = true;
	g_state.gnss.agps.last_ok_ms = fm160_now_ms();
	fm160_state_mark_dirty();
}

/* AT+GTAGPSSERV? -> '+GTAGPSSERV: "supl.qxwz.com",7276'
 *
 * The command name carries TWO S's (GT-AGPS-SERV).  Read-only, and the vendor's
 * default is a Chinese SUPL server, which is worth showing: AGPS is the one part
 * of this subsystem that cannot work without a data connection. */
void fm160_parse_gnss_supl(const char *resp)
{
	char buf[FM160_STR_MAX];
	char *open, *close, *comma;

	if (!fm160_resp_find(resp, "+GTAGPSSERV:", buf, sizeof(buf)))
		return;

	open = strchr(buf, '"');
	close = open ? strchr(open + 1, '"') : NULL;
	if (open && close) {
		*close = '\0';
		snprintf(g_state.gnss.agps.server,
			 sizeof(g_state.gnss.agps.server), "%s", open + 1);
		comma = strchr(close + 1, ',');
	} else {
		comma = strchr(buf, ',');
		if (comma) {
			*comma = '\0';
			snprintf(g_state.gnss.agps.server,
				 sizeof(g_state.gnss.agps.server), "%s", buf);
		}
	}
	if (comma && comma[1])
		g_state.gnss.agps.port = atoi(comma + 1);

	g_state.gnss.agps.valid = true;
	g_state.gnss.agps.last_ok_ms = fm160_now_ms();
	fm160_state_mark_dirty();
}

/* ------------------------------------------------------------------ */
/* M5 polls and write paths                                             */
/* ------------------------------------------------------------------ */

static void gnss_cb(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	bool first = !g_state.gnss.r.read_error;

	if (s == AT_STATUS_OK) {
		fm160_parse_gnss(resp);
		return;
	}

	/* ERROR here is not a broken AT link and not a parse failure: with the
	 * engine off, AT+GTGPS? answers ERROR (measured).  The poll only runs
	 * while the cached power state says the engine is on, so getting here
	 * means the engine was switched off behind us - record that rather than
	 * counting it against the circuit breaker. */
	g_state.gnss.r.read_error = true;
	g_state.gnss.r.read_ms = fm160_now_ms();
	fm160_log(LOG_DEBUG, "GNSS read answered status %d (engine off?)", s);
	fm160_state_mark_dirty();

	/* One power read to find out whether the engine is still on - guarded so
	 * that a module answering ERROR repeatedly is not asked twice every round,
	 * and so that the recovery costs one transaction rather than waiting for
	 * the power tier's idle interval (10 minutes with no page open). */
	if (first)
		fm160_cmd_poll_gnss_power();
}

/*
 * AT+GTGPS? - 30 ms measured for the whole eight-sentence block, i.e. cheaper
 * than the cell query next to it in the tier table.
 *
 * Never submitted while the engine is off: the answer would be ERROR every time,
 * and on a Qualcomm AT parser a wasted transaction is the thing to avoid rather
 * than the thing to tolerate.
 */
void fm160_cmd_poll_gnss(void)
{
	char cmd[FM160_AT_CMD_MAX];

	if (!g_state.gnss.engine.on)
		return;
	if (fm160_gnss_item_command(cmd, sizeof(cmd), NULL))
		return;
	atq_submit(AT_PRIO_POLL, cmd, NULL, 3000, gnss_cb, NULL);
}

static void gnss_power_cb(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_gnss_power(resp);
}

void fm160_cmd_poll_gnss_power(void)
{
	/* 22 ms.  Always submitted, even though the engine is usually off: this
	 * read is the only thing that can notice either an engine switched on
	 * from elsewhere or the module coming back from a power cycle with the
	 * switch reset to 0. */
	atq_submit(AT_PRIO_POLL, "AT+GTGPSPOWER?", NULL, 3000, gnss_power_cb, NULL);
}

/*
 * Switch the GNSS engine.
 *
 * No capability gate, and the reason is precisely that this setting is the one
 * GNSS command the module does NOT store: the guide says so (Require Data Store
 * at Power Down = No) and hardware confirmed it, since the engine reads back as 0
 * after a power cycle.  A wrong value therefore cannot survive anything, and both
 * values were exercised end to end (AT-FACTS 10.0/10.4).  Compare the
 * constellation write below, which is persistent and does need a licence.
 *
 * No quiet window either, and that one is deliberate in the other direction: the
 * window exists to keep polls off a module that has work to do (a band re-scan, an
 * EFS commit), while this is a 50 ms switch that changes neither registration nor
 * the data path.  Opening one would actively hurt, because a POLL-priority read
 * is BLOCKED by an open window - and the point of switching the engine on is to
 * see NMEA as soon as it exists.
 */
int fm160_cmd_set_gnss_power(int on, at_done_cb cb, void *arg)
{
	char cmd[FM160_AT_CMD_MAX];

	if (fm160_gnss_power_command(cmd, sizeof(cmd), on))
		return -1;

	fm160_log(LOG_WARNING, "GNSS power write: %s", cmd);
	/* The follow-up asks for the NMEA block straight away.  The engine needs a
	 * moment before it produces sentences - the first read after switching it
	 * on is empty - and an empty frame is a state the parser already reports,
	 * so asking is both free and the fastest way to get the page out of its
	 * "engine on, nothing yet" state. */
	if (cfg_write(cmd, "AT+GTGPSPOWER?", 8000, QUIET_NONE, NULL, 0,
		      fm160_parse_gnss_power, fm160_cmd_poll_gnss, cb, arg))
		return -1;
	return 0;
}

/*
 * Write the satellite combination.
 *
 * This one IS stored in the module, so it follows the M4 rule to the letter: the
 * write is refused unless the modem answered AT+GTGPSCFG=?, and the value must
 * also appear in that answer.  Two independent checks and neither of them is the
 * source of the values the page offers - the page offers the guide's set, so a
 * firmware with a wider list still cannot be sent something that means nothing.
 *
 * The caller is answered only after AT+GTGPSCFG? has been read back, so the page
 * can compare what it asked for with what the module now reports.  That
 * comparison is left to the page on purpose: the daemon cannot know whether a
 * reinterpreted value is wrong, but the person reading the page can.
 */
int fm160_cmd_set_gnss_cfg(int value, at_done_cb cb, void *arg)
{
	char cmd[FM160_AT_CMD_MAX];
	int i;
	bool listed = false;

	if (!g_state.gnss.cfg.caps_valid) {
		fm160_log(LOG_WARNING,
			  "refusing to write GNSS config: AT+GTGPSCFG=? gave no set for x=%d",
			  FM160_GNSS_CFG_X_WRITE);
		return -1;
	}
	if (fm160_gnss_cfg_command(cmd, sizeof(cmd), value))
		return -1;
	for (i = 0; i < g_state.gnss.cfg.caps_n[FM160_GNSS_CFG_X_WRITE]; i++)
		if (g_state.gnss.cfg.caps_values[FM160_GNSS_CFG_X_WRITE][i] == value)
			listed = true;
	if (!listed) {
		fm160_log(LOG_WARNING,
			  "refusing to write GNSS config %d: not in the x=%d set from AT+GTGPSCFG=?",
			  value, FM160_GNSS_CFG_X_WRITE);
		return -1;
	}

	fm160_log(LOG_WARNING, "GNSS configuration write: %s", cmd);
	if (cfg_write(cmd, "AT+GTGPSCFG?", 8000, QUIET_NONE, NULL, 0,
		      fm160_parse_gnss_cfg, fm160_cmd_poll_gnss, cb, arg))
		return -1;
	return 0;
}

/*
 * Apply uci's "switch the engine on at boot" wish, once per ident run.
 *
 * AT+GTGPSPOWER is not stored by the module, which is exactly why this exists:
 * without it the wish would silently revert at every router reboot, and the only
 * symptom would be a page saying "engine off" for no visible reason.
 *
 * Once per ident run, NOT once per observation.  The engine state is polled, and
 * re-applying whenever it is seen to be off would fight the user's own "switch it
 * off" click forever.  An ident run happens at boot and again if the module
 * disappears and comes back, which is the case this is meant to cover.
 */
void fm160_cmd_gnss_autostart(void)
{
	if (!g_state.gnss.autostart || g_state.gnss.autostart_done)
		return;
	if (!g_state.gnss.engine.known)
		return;      /* nothing to compare against yet - wait for the read */
	g_state.gnss.autostart_done = true;

	if (g_state.gnss.engine.on) {
		fm160_log(LOG_DEBUG, "GNSS autostart: the engine is already on");
		return;
	}
	fm160_log(LOG_INFO, "GNSS autostart: switching the engine on");
	if (fm160_cmd_set_gnss_power(1, NULL, NULL))
		fm160_log(LOG_WARNING, "GNSS autostart: the write was not accepted");
}
