/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * fm160d - Fibocom FM160 dedicated modem manager daemon (policy layer).
 *
 * Layering:
 *   LuCI JS  --ubus-->  fm160d (this)  --ubus-->  at-daemon (transport)
 *                                                    |
 *                                                /dev/ttyUSBx
 *
 * fm160d is the ONLY component allowed to issue AT commands on the FM160's AT
 * port.  Everything else goes through the ubus object "fm160".
 */

#ifndef FM160D_H
#define FM160D_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <libubox/list.h>
#include <libubox/uloop.h>
#include <libubox/blobmsg.h>
#include <libubox/utils.h>
#include <libubus.h>

#include "pdu.h"
/*
 * M2's two pure layers are included here rather than forward-declared, because
 * struct fm160_state EMBEDS their enums and constants.  Both are libc-only
 * (see their headers), so this pulls in no libubox dependency, and the
 * direction is one-way: neither of them knows fm160d.h exists, which is what
 * lets the host-side test compile them on their own.
 */
#include "net.h"
#include "usbmode.h"
/*
 * diag.h pulls in nothing but <stdint.h>/<stddef.h> and forward-declares the
 * two structs it works on, so including it here costs no ordering constraint.
 * diag.c is the only file that needs the real definitions, and it gets them by
 * including this header.
 */
#include "diag.h"

/*
 * The daemon's own version, reported in the diagnostics bundle.
 *
 * It is duplicated from PKG_VERSION in ../Makefile because a -D on the
 * toolchain command line is the only other way to get it here, and that path
 * cannot be checked on the workstation at all (no make).  tools/check.sh
 * asserts that the two strings are equal, so the duplication is enforced
 * rather than trusted - a stale version here would make every diagnostic
 * report that a maintainer reads slightly wrong, in the one artefact they
 * consult precisely because they have lost track of what is running.
 */
#define FM160_VERSION       "0.1.0"

#define FM160_VENDOR_ID     "2cb7"   /* Fibocom */
#define FM160_PORT_MAX      64
#define FM160_STR_MAX       96
/* One response line.  FM160_STR_MAX is fine for scalar answers but AT+GTACT=?
 * answers with a single ~131-character line of nine groups, and
 * fm160_resp_find() truncates silently at outlen-1 - so the M4 parsers ask for
 * a buffer that fits the longest documented enumeration. */
#define FM160_RESP_LINE_MAX 512
#define FM160_AT_CMD_MAX    256
#define FM160_AT_END_MAX    32
/* Largest two-stage payload, in hex characters.  One SMS PDU is up to
 * SMS_PDU_HEX_MAX ASCII characters (pdu.h) and each is written as two hex
 * digits, plus the 0x1A that ends AT+CMGS. */
#define FM160_AT_PAYLOAD_MAX 1088
#define FM160_RAW_MAX       8192
#define FM160_CAND_MAX      12

/* ------------------------------------------------------------------ */
/* AT request queue                                                     */
/* ------------------------------------------------------------------ */

enum at_prio {
	AT_PRIO_INTERACTIVE = 0,   /* user pressed something            */
	AT_PRIO_STATE       = 1,   /* dialer / mode switch / sms        */
	AT_PRIO_POLL        = 2,   /* background polling                */
};

enum at_status {
	AT_STATUS_UNKNOWN = 0,
	AT_STATUS_OK      = 1,
	AT_STATUS_ERROR   = 2,     /* ERROR / +CME ERROR                */
	AT_STATUS_TIMEOUT = 3,     /* no terminal line                  */
	AT_STATUS_NOPORT  = 4,     /* AT port not (yet) available       */
	AT_STATUS_BUSY    = 5,     /* rejected, e.g. quiet window       */
};

struct at_req;

typedef void (*at_done_cb)(struct at_req *req, enum at_status status,
			   const char *response, void *arg);

struct at_req {
	struct list_head list;
	enum at_prio prio;
	uint64_t id;
	uint64_t submit_ms;
	char cmd[FM160_AT_CMD_MAX];
	char end_flag[FM160_AT_END_MAX];
	int timeout_ms;          /* sendat timeout, seconds internally */
	bool silent;             /* do not advance the circuit breaker */
	/*
	 * Two-stage transactions: AT+CMGS and AT+CMGW.
	 *
	 * "AT+CMGS=<n>" is answered with a prompt and NO line terminator, after
	 * which the modem sits waiting for the PDU.  A single sendat cannot
	 * express that, so when payload_hex is set the request is sent as two
	 * sendat calls - the first waiting for `prompt`, the second writing the
	 * bytes and waiting for the real terminal.
	 *
	 * Both calls happen inside ONE queue entry, and that is the whole point:
	 * the one-request-in-flight rule is what stops a background poll from
	 * being slipped in between the prompt and the payload.  A poll landing
	 * there would look to the modem like the answer to its prompt, and the
	 * message would be quietly garbled.
	 *
	 * No change to at-daemon is needed for this: its "raw_at_content" field
	 * already means "these hex bytes, written verbatim, no CR appended", and
	 * it still waits for the end flag (see HARDWARE-PROBE.md 10.1).
	 */
	char prompt[FM160_AT_END_MAX];       /* stage-1 terminator, normally ">" */
	char payload_hex[FM160_AT_PAYLOAD_MAX];   /* stage-2 bytes, hex encoded */
	/* Which port this request goes out on.  Empty means "the port the daemon
	 * has settled on", i.e. g_state.port.  A port probe fills it in instead:
	 * the probe is the thing that DISCOVERS g_state.port, so it cannot be
	 * routed by it.  Keep it here rather than reading g_state inside
	 * atq_issue() -- that read is exactly what made the probe send on an
	 * empty port and fail forever. */
	char port[FM160_PORT_MAX];
	at_done_cb cb;
	void *arg;
};

/* ------------------------------------------------------------------ */
/* State cache                                                          */
/* ------------------------------------------------------------------ */

#define FM160_REG_UNKNOWN  99
/* Sentinel for a decoded value the modem did not report.  Every "not known or
 * not detectable" code is 255 (or 99) and 255 is also a legal raw value, so the
 * raw number is always kept next to the decoded one. */
#define FM160_NONE         (-1000000)

/*
 * One cell as reported by AT+GTCCINFO? (AT manual 9.1.15).
 *
 * The column layout is NOT the same for every row - it depends on both <rat>
 * and <IsServiceCell>.  Verified against the manual:
 *
 *   LTE service (rat 4, IsServiceCell 1) - 14 columns
 *     <IsServiceCell>,<rat>,<mcc>,<mnc>,<tac>,<cellid>,<earfcn>,
 *     <physicalcellId>,<band>,<bandwidth>,<rssnr_value>,<rxlev>,<rsrp>,<rsrq>
 *       idx  0..7         8 band  9 bandwidth 10 sinr 11 rxlev 12 rsrp 13 rsrq
 *
 *   LTE neighbour (rat 4, IsServiceCell 2) - 12 columns
 *     ...,<physicalcellId>,<bandwidth>,<rxlev>,<rsrp>,<rsrq>
 *       idx  8 bandwidth 9 rxlev 10 rsrp 11 rsrq      (band NOT reported)
 *
 *   NR service (rat 9) - 14 columns, same indices as the LTE service row.
 *   NR neighbour (rat 9)  - 12 columns but 8 is <ss-sinr>, NOT <bandwidth>:
 *     ...,<physicalcellId>,<ss-sinr>,<rxlev>,<ss-rsrp>,<ss-rsrq>
 *       idx  8 sinr 9 rxlev 10 rsrp 11 rsrq
 */
struct fm160_cell {
	bool valid;
	bool is_service;
	int  rat;                /* <rat>: 0 invalid, 2 WCDMA, 4 LTE, 9 NR */
	int  mcc, mnc;
	long long tac;
	long long cellid;
	long long earfcn;
	int  pci;
	int  band;               /* 0 = not reported (all neighbour rows)   */
	int  bandwidth_raw;      /* LTE: RB count 6/15/25/50/75/100         */
	                         /* NR : code 0(=5MHz)/10/15/.../400        */
	int  bandwidth_mhz;      /* decoded MHz, FM160_NONE when unknown    */
	int  sinr_raw;           /* rssnr_value (LTE) / ss-sinr (NR)        */
	int  sinr_db10;          /* tenths of a dB; FM160_NONE when 255     */
	int  rsrp_raw;
	int  rsrp_dbm;
	int  rsrq_raw;
	int  rsrq_db10;          /* tenths of a dB                          */
	int  rxlev_raw;
};

/* ------------------------------------------------------------------ */
/* M4: band lock / cell lock / carrier aggregation                      */
/* ------------------------------------------------------------------ */

#define FM160_BAND_MAX   32   /* entries per RAT, both state and caps   */
#define FM160_CAP_INT_MAX 24  /* integers in one AT+GTACT=? group       */
#define FM160_CA_MAX      8   /* secondary cells we keep                */

/* M4 write path guardrails. */
#define FM160_GTACT_GROUPS  9  /* rat,pref1,pref2,gsm,umts,lte,cdma,evdo,nr */

/*
 * One band as it appears in AT+GTACT / AT+GTACT=? output.
 *
 * Both commands use a RAT-prefixed encoding rather than a uniform offset:
 * UMTS = band number (1..25), LTE = 100 + band (101..164), NR = 500 + band for
 * one/three-digit bands but 5000 + band and 50000 + band for the two- and
 * three-digit ones (5010 = n10, 50100 = n100).  The raw token is kept next to
 * the decoded band because the *raw* form is what has to be written back.
 */
struct fm160_band {
	int raw;
	int band;      /* decoded band number, 0 when undecodable */
	int rat;       /* 2 UMTS, 4 LTE, 9 NR */
};

/* AT+GTACT? - what the modem is currently restricted to. */
struct fm160_gtact_state {
	bool valid;
	int  rat, pref1, pref2;        /* leading scalars, same as AT+GTRAT? */
	struct fm160_band umts[FM160_BAND_MAX]; int umts_n;
	struct fm160_band lte[FM160_BAND_MAX];  int lte_n;
	struct fm160_band nr[FM160_BAND_MAX];   int nr_n;
	/* Tokens that fit no documented encoding.  Counted rather than guessed,
	 * so an unexpected firmware can be spotted instead of silently dropped. */
	int  unknown_n;
	/* A band value of 0 means "automatic band selection" - i.e. unrestricted
	 * for that RAT.  It is a marker, not a band. */
	bool auto_seen;
	uint64_t last_ok_ms;
};

/*
 * AT+GTACT=? - what the modem says it supports.  Nine groups in a fixed order.
 *
 * Never assume these exist: on FM160-CN the gsm, cdma and evdo groups come
 * back as "( )", i.e. empty.  group_nonempty[] records which ones the modem
 * actually filled in, so the UI can grey out what does not apply instead of
 * offering the manual's generic list.
 */
struct fm160_gtact_caps {
	bool valid;
	int  rat[FM160_CAP_INT_MAX];   int rat_n;
	int  pref1[FM160_CAP_INT_MAX]; int pref1_n;
	int  pref2[FM160_CAP_INT_MAX]; int pref2_n;
	struct fm160_band umts[FM160_BAND_MAX]; int umts_n;
	struct fm160_band lte[FM160_BAND_MAX];  int lte_n;
	struct fm160_band nr[FM160_BAND_MAX];   int nr_n;
	bool group_nonempty[FM160_GTACT_GROUPS];
	uint64_t last_ok_ms;
};

/*
 * AT+GTCELLLOCK? - persistent cell lock state.
 *
 * ⚠️ Persistent = Yes and it only takes effect after a UE reset, because the
 * value is written to an EFS file.  fm160d therefore never reboots the module
 * on its own: it applies the AT write and tells the user what remains to do.
 */
struct fm160_celllock_state {
	bool valid;
	bool enabled;                /* mode field: 0 disabled, 1 enabled */
	int  rat;                    /* 0 LTE, 1 NR, 2 UMTS   */
	int  type;                   /* 0 lock PCI/PSC, 1 lock frequency */
	unsigned long long earfcn;
	int  pci;
	int  scs;                    /* 0 = 15 kHz, 1 = 30 kHz */
	int  nrband;                 /* 501 + n form */
	bool has_pci, has_scs, has_nrband;   /* trailing fields are optional */
	uint64_t last_ok_ms;
};

/*
 * AT+GTCELLLOCK=? - the modem answers ranges, not enumerations, for the
 * numeric fields, so this is stored as bounds.
 *
 * ⚠️ The live modem reports mode list "(0,1,2)" while the manual only defines
 * 0 and 1.  mode 2 is undocumented, so fm160d only ever writes 0 or 1 and
 * records the third value here purely as evidence.
 */
struct fm160_celllock_caps {
	bool valid;
	int  mode[8];   int mode_n;
	int  rat_min, rat_max;
	int  type[4];   int type_n;
	unsigned int earfcn_max;
	int  pci_max;
	int  scs_min, scs_max;
	int  nrband_min, nrband_max;
	uint64_t last_ok_ms;
};

/*
 * AT+GTCAINFO? - carrier aggregation.
 *
 * Returns a bare "OK" with no data line when the modem is not aggregated
 * (measured with no SIM), so "parsed nothing" is a normal state and must not
 * be reported as a parse error.
 */
struct fm160_ca_cell {
	bool valid;
	bool is_pcc;
	int  state;            /* 1 configured+deactivated, 2 activated */
	int  ul_configured;
	int  band;
	int  pci;
	unsigned long long freq;    /* earfcn (LTE) or narfcn (NR) */
	int  dl_bw_mhz, ul_bw_mhz;
	int  dl_mimo, ul_mimo;
	int  dl_mod, ul_mod;        /* 0 BPSK .. 5 1024QAM, 6 unknown */
	int  rsrp_dbm;
};

struct fm160_ca_state {
	bool valid;            /* a PCC row was parsed */
	int  rat;              /* 4 LTE, 9 NR -- the first (headline) block */
	bool has_nr;           /* an NR block was present (EN-DC) */
	struct fm160_ca_cell pcc;
	struct fm160_ca_cell scc[FM160_CA_MAX];
	int  scc_n;
	uint64_t last_ok_ms;
};

/* ------------------------------------------------------------------ */
/* M5: GNSS                                                             */
/* ------------------------------------------------------------------ */

#define FM160_GNSS_CONST_MAX  8    /* NMEA talker prefixes kept          */
#define FM160_GNSS_SAT_MAX    32   /* satellites kept across all of them */
#define FM160_GNSS_USED_MAX   12   /* PRN slots in one GSA sentence      */
#define FM160_GNSS_RAW_MAX    512  /* verbatim NMEA kept for the UI      */
#define FM160_GNSS_CAP_MAX    32   /* values in one AT+GTGPSCFG=? answer */

/*
 * AT+GTGPSCFG= takes an "x" that names which field is being written, and the
 * capability answer gives each x its own value set.  The sets are NOT
 * interchangeable, so they are kept apart - one slot per x, plus one for a
 * group that arrived with no "GTGPSCFG: x" header in front of it at all.
 *
 * The unkeyed slot exists so that an answer this parser cannot attribute is
 * still visible in the log instead of vanishing, but it never licenses a write:
 * a value with no field attached has no meaning to check against.
 */
#define FM160_GNSS_CFG_X_MAX   4   /* the x values AT+GTGPSCFG can take       */
#define FM160_GNSS_CFG_X_NONE  4   /* slot for groups with no x in front      */
#define FM160_GNSS_CFG_SLOTS   5   /* four fields plus the unkeyed slot       */
#define FM160_GNSS_CFG_X_WRITE 2   /* the x that AT+GTGPSCFG=2,<v> writes     */

/*
 * NMEA talker prefixes - a LABEL, never a switch.
 *
 * The prefix identifies a constellation but nothing downstream may depend on
 * having seen it before: an unknown one gets a slot of its own with its two
 * characters recorded, exactly like an undecodable band token in M4.
 *
 * Measured frame (engine on, no fix - see AT-FACTS.md 10.2):
 *   $GPGSA $BDGSA $GAGSA $PQGSA / $GPGSV $BDGSV $GLGSV $GAGSV
 * Note that QZSS arrives as the proprietary "$PQ" talker, and that a
 * constellation can appear in GSA without ever appearing in GSV (PQ) or the
 * other way round (GL) - so neither list may be derived from the other.
 */
enum fm160_gnss_kind {
	GNSS_KIND_OTHER = 0,   /* prefix recorded, meaning not assumed        */
	GNSS_KIND_GPS,         /* GP                                          */
	GNSS_KIND_BDS,         /* BD on this firmware (GB is the older prefix) */
	GNSS_KIND_GLO,         /* GL                                          */
	GNSS_KIND_GAL,         /* GA                                          */
	GNSS_KIND_QZSS,        /* PQ - proprietary, as emitted by this firmware */
};

/* One satellite, from one GSV quad group. */
struct fm160_gnss_sat {
	int  const_idx;        /* index into fm160_gnss_reading.cons[]        */
	int  prn;
	int  elev_deg;         /* FM160_NONE when the sentence left it empty  */
	int  azim_deg;         /* (0 is a real elevation, so empty != 0)      */
	int  snr_db;           /* C/N0 in dB-Hz; FM160_NONE when not tracked  */
	bool in_fix;           /* this PRN appears in its own constellation's
				* GSA "used" list                            */
};

/*
 * One constellation, from its own GSV sentences.
 *
 * `visible` is the modem's own count (<sats in view>) and `parsed` is how many
 * quad groups actually came out of the sentences.  They are kept apart on
 * purpose: the count is what the modem claims, the parsed number is what it
 * showed.  A response truncated by the AT buffer then shows up as a difference
 * between the two instead of silently shrinking the satellite list.
 */
struct fm160_gnss_const {
	char talker[4];
	int  kind;             /* enum fm160_gnss_kind                        */
	int  visible;          /* FM160_NONE until a GSV says so              */
	int  parsed;
	int  in_fix;           /* non-empty PRN slots in its GSA sentence     */
	int  snr_best_db;      /* FM160_NONE when nothing was tracked         */
	/* The GSA PRN list is kept so a GSV quad - which may be parsed before or
	 * after its GSA - can be flagged as used in the fix.  Order within one
	 * response is not guaranteed by anything. */
	int  used_prn[FM160_GNSS_USED_MAX];
	int  used_n;
};

/*
 * One reading of AT+GTGPS?, i.e. one NMEA block.
 *
 * Deliberately separate from the engine and configuration state, for one
 * measured reason: the first read after AT+GTGPSPOWER=1 comes back with a
 * header and NO sentence at all (88 bytes).  "I have nothing for you yet" and
 * "there are no satellites" must not be confused, so an empty block leaves the
 * previous picture alone and only sets empty_frame.
 */
/*
 * The last NMEA block the module actually produced, plus the outcome of the
 * most recent AT+GTGPS? attempt.  The two are deliberately not the same thing:
 * an "OK, but no sentence yet" answer - the measured state right after the
 * engine is switched on - carries no picture at all, so it must NOT overwrite
 * one.  Everything down to raw_len describes the last block; the poll_* group
 * describes the last attempt.  read_ms belongs to the block, so its age stays
 * the age of the picture and an empty poll cannot make stale data look fresh.
 */
struct fm160_gnss_reading {
	uint64_t read_ms;      /* when the last block with sentences arrived   */
	int  resp_bytes;       /* that answer: whole response, header and OK   */
	int  sentences;        /* sentences accepted in it - `ignored` included */
	int  nmea_bytes;       /* their total length                           */
	int  bad_checksum;     /* sentences whose *hh did not match            */
	int  bad_shape;        /* lines starting with $ that are not sentences */
	int  ignored;          /* well-formed sentences of a formatter we skip */
	char raw[FM160_GNSS_RAW_MAX];  /* verbatim, truncated for the page     */
	int  raw_len;

	/* --- outcome of the most recent AT+GTGPS? attempt ------------------ */
	bool empty_frame;      /* OK, but not one sentence yet                 */
	int  empty_frames;     /* empties in a row since the last real block   */
	uint64_t empty_ms;     /* when the last of them arrived                */
	int  empty_bytes;      /* how big that empty answer was                */
	bool read_error;       /* the read answered ERROR - the engine is off  */

	bool gsv_trailing;     /* a GSV carried a field past its last quad      */
	int  gsv_trailing_value;

	/* fix, from GSA */
	int  fix_mode;         /* GSA field 1: 'A' automatic / 'M' manual, 0 none */
	/* fix_type carries FM160_NONE until a GSA arrives -- fm160_state_init()
	 * seeds it that way and only a real GSA clears it.  So it is in the
	 * sentinel domain as well as the 1/2/3 domain, and anything that renders
	 * it (diag.c) must go through w_int() or -1000000 reaches the reader.
	 * This comment used to name only 1/2/3, which is why it did not. */
	int  fix_type;         /* GSA field 2: 1 none, 2 2D, 3 3D, or FM160_NONE */
	int  sats_used;        /* non-empty PRN slots across every GSA         */
	int  pdop_x10, hdop_x10, vdop_x10;    /* FM160_NONE when empty        */

	/* position, from GGA / RMC - absent entirely until there is a fix */
	bool has_position;
	int  quality;          /* GGA field 6: 0 invalid, 1 GPS, 2 DGPS ...    */
	int  sats_in_use;      /* GGA field 7                                  */
	/* Degrees x 1e7, only meaningful while has_position is set.  FM160_NONE
	 * is NOT used here: -1000000 is a legal scaled coordinate (-0.1 deg), so
	 * a sentinel would collide with the Gulf of Guinea. */
	int  lat_1e7, lon_1e7;
	char lat_ns, lon_ew;
	int  alt_dm;           /* GGA altitude, decimetres                     */
	int  geoid_dm;         /* GGA geoid separation, decimetres             */
	int  speed_cmps;       /* RMC speed over ground, cm/s                  */
	int  course_d10;       /* RMC track made good, tenths of a degree      */
	char utc[16];          /* hhmmss.ss, from GGA or RMC                   */
	char date[16];         /* ddmmyy, RMC only                             */

	/* satellites */
	struct fm160_gnss_const cons[FM160_GNSS_CONST_MAX];
	int  cons_n;
	struct fm160_gnss_sat sats[FM160_GNSS_SAT_MAX];
	int  sats_n;
	int  visible_total;    /* sum of cons[].visible                        */
	int  snr_best_db;      /* best C/N0 anywhere                           */
};

/* AT+GTGPSPOWER? - the engine switch, which the module does NOT store. */
struct fm160_gnss_engine {
	bool known;
	bool on;
	uint64_t last_ok_ms;
};

/*
 * AT+GTGPSCFG? - the satellite combination, and the AGPS settings beside it.
 *
 * The live modem answers with a header and then one "x,value" line per group,
 * and it OMITS x=1 entirely (measured 0,2 / 2,14 / 3,0 against a manual that
 * documents four groups).  The lines are therefore matched by their x, never by
 * their position, and a missing line is a reported state rather than an error.
 */
struct fm160_gnss_cfg {
	bool valid;
	int  supl_version;     /* x=0; FM160_NONE when the line is absent      */
	int  constellation;    /* x=2; FM160_NONE when the line is absent      */
	int  cert;             /* x=3; FM160_NONE when the line is absent      */
	bool xtra_present;     /* x=1 - absent on this firmware                */
	int  xtra;
	int  unknown;          /* values of x this parser does not know        */
	uint64_t last_ok_ms;

	/*
	 * The licence for AT+GTGPSCFG=2,<v>.
	 *
	 * MEASURED on the FM160 (2026-09-19), AT+GTGPSCFG=? answers one line per
	 * field and each line carries its own value set:
	 *
	 *     +GTGPSCFG: 0,(0-2)     supl version  - three values
	 *     +GTGPSCFG: 2,(0-15)    constellation - sixteen
	 *     +GTGPSCFG: 3,(0,1)
	 *
	 * and x=1 is absent here too, exactly as in the "?" form.  The sets are not
	 * interchangeable, so they are stored per x and the write path asks only
	 * about FM160_GNSS_CFG_X_WRITE.
	 *
	 * The first version of this parser flattened every group into one list.  On
	 * this firmware that produced the right answer by luck - x=2's set happens
	 * to be a superset of the other two, so the union equalled it - while
	 * actually asking "is this value legal for ANY field?".  A firmware that
	 * made some other field the wider one would have licensed writes the modem
	 * rejects, and the check would have looked like it was working the whole
	 * time.  A coincidence is not a check.
	 *
	 * caps_valid means the x=2 set is known, which is exactly the licence the
	 * write needs.
	 */
	bool caps_valid;
	int  caps_n[FM160_GNSS_CFG_SLOTS];
	int  caps_values[FM160_GNSS_CFG_SLOTS][FM160_GNSS_CAP_MAX];
	int  caps_x_mask;              /* bit x: a line for that x did arrive    */
	int  caps_groups;              /* answers seen, for the log             */
};

/* AT+GTGPSEPO? / AT+GTAGPSSERV? - read-only in this milestone. */
struct fm160_gnss_agps {
	bool valid;
	int  epo;              /* 0 off, 1 MSB, 2 MSA                          */
	char server[FM160_STR_MAX];
	int  port;
	uint64_t last_ok_ms;
};

struct fm160_gnss_state {
	struct fm160_gnss_engine  engine;
	struct fm160_gnss_cfg     cfg;
	struct fm160_gnss_agps    agps;
	struct fm160_gnss_reading r;

	/* User intent, from uci.  AT+GTGPSPOWER is not stored by the module, so
	 * "the engine was on last time" has to live here and be re-applied once
	 * per ident run - see fm160_cmd_gnss_autostart(). */
	bool autostart;
	bool autostart_done;
};

/* ------------------------------------------------------------------ */
/* M3: SMS                                                             */
/* ------------------------------------------------------------------ */

/*
 * Everything SMS-related is event-driven or user-driven.  Nothing in this
 * subsystem is polled, and that is a measurement rather than a style choice:
 *
 *   AT+CPMS?  10 345 ms   ERROR          (no SIM fitted)
 *   AT+CMGF?   5 373 ms   +CME ERROR: 10
 *
 * on a serial port that answers AT+CSQ in 24 ms.  A poll tier containing either
 * of them would starve every other command queued behind it, so the SMS layer
 * is entered only when a page asks for something or when the modem announces a
 * message of its own accord (+CMTI).
 */

#define FM160_SMS_TEXT_MAX   SMS_PDU_TEXT_MAX
#define FM160_SMS_NUM_MAX    (SMS_PDU_ADDR_MAX + 1)
#define FM160_SMS_SEG_MAX    SMS_PDU_SEG_MAX
/* Messages kept in RAM, newest wins.  While a card is fitted the modem's own
 * store is the durable copy; this exists so the page has something to show
 * without a round trip, and so a message already read survives the modem
 * forgetting it (which it does when its store fills). */
#define FM160_SMS_KEEP       64

struct fm160_sms_msg {
	bool used;
	uint64_t id;                   /* local, monotonic, never reused */
	bool outgoing;
	/* Where it lives on the modem; -1 when we hold the only copy. */
	int  index;
	char storage[8];
	char number[FM160_SMS_NUM_MAX];
	bool international;
	char text[FM160_SMS_TEXT_MAX];
	int  text_len;                 /* characters, not bytes */
	int  encoding;                 /* enum sms_enc */
	bool has_time;                 /* an SMS-DELIVER is stamped by the SMSC */
	int  year, month, day, hour, min, sec, tz_quarters;
	bool tz_negative;
	bool concat;
	int  ref, parts, part;
	int  status;                   /* outgoing: 0 unknown, 2 sent, 3 failed */
	uint64_t local_ms;             /* when this copy was made */
};

/*
 * What the page needs before it offers to send anything.
 *
 * `usable` is the licence for the whole feature: AT+CMGF=0 was accepted, a
 * storage was chosen from what AT+CPMS=? reported, and the new-message
 * indication was armed.  Until all three hold the page greys out, rather than
 * offering a button whose command the modem has already refused.
 */
struct fm160_sms_status {
	bool probed;
	bool usable;
	char mem[8];                   /* the storage actually in use */
	int  used, total;              /* as reported by AT+CPMS? */
	int  kept;                     /* messages in our own list */
	int  unread;
	int  received;                 /* pulled from the modem since boot */
	int  duplicates;               /* dropped because we already had it */
	int  last_error;               /* +CMS ERROR code, 0 = none */
	char last_error_text[FM160_STR_MAX];
	uint64_t last_ok_ms;

	/* --- the evidence PLAN §4 asks for ---------------------------- */
	/* A PDU longer than one USB packet that times out is the symptom of a
	 * missing zero-length-packet workaround.  Counted rather than assumed:
	 * we do not patch a kernel on a hunch. */
	int  sent_ok, sent_fail, sent_timeout;
	int  long_sent, long_timeout;
};

struct fm160_sms_state {
	struct fm160_sms_status st;
	struct fm160_sms_msg msg[FM160_SMS_KEEP];
	int  head;                     /* next slot to overwrite */
	int  count;
	uint64_t next_id;

	int  cmgf;                     /* -1 unknown, 0 PDU mode, 1 text mode */
	bool setup_done;
	bool setup_running;
	uint64_t setup_next_ms;        /* backoff after a refused setup */

	int  sent_mr;                  /* the +CMGS reference, when reported */
	/* The exact bytes the last send put on the wire.  Kept for the page and
	 * the log: when a message does not arrive, this is the artefact worth
	 * looking at, and re-deriving it later would prove nothing. */
	char last_pdu[FM160_AT_PAYLOAD_MAX];
	char last_send_cmd[FM160_AT_CMD_MAX];
	int  last_segments;
	bool busy;                     /* a send or a fetch is in flight */
};

/* ------------------------------------------------------------------ */
/* M2: the data plane and the USB profile switch                        */
/* ------------------------------------------------------------------ */

/*
 * Why the data plane is a state machine and not a script
 *
 * The vendor's dial-up document gives a linear recipe, and the linear part is
 * reproduced exactly (see net.h).  What a recipe cannot express is that every
 * step can also say "not yet": registration takes tens of seconds, activation is
 * documented as taking up to 210 s, and the first four steps all fail
 * differently on a modem with no card.  So each rung has a deadline of its own,
 * a failure climbs a ladder rather than restarting instantly (DESIGN 4.3), and
 * what the modem actually said is kept verbatim for the page to show.
 *
 * Only ECM is dialled from here.  QMI and MBIM are dialled by the kernel stack
 * through uqmi/umbim, and duplicating that would mean two things racing to
 * activate the same PDP context; fm160_dial_start() refuses those profiles and
 * says which protocol to use instead.
 *
 * `step` is where the ladder IS, and never where it briefly was: the in-flight
 * marker is `running`, a separate field, because a snapshot that said "busy"
 * every time a transaction was out would hide the one thing the page is showing
 * - which rung is being waited on.  (NET_STEP_BUSY in net.h is the placeholder
 * that idea would have used; this implementation never enters it.)
 */
struct fm160_dial_state {
	bool wanted;                 /* the user asked for the link            */
	enum fm160_net_step step;
	enum fm160_dial_kind kind;   /* how the ACTIVE profile is dialled      */
	bool kind_known;

	int  cid;
	enum fm160_net_pdp pdp;
	char apn[FM160_NET_APN_MAX];

	bool up;
	char addr[FM160_NET_ADDR_MAX];
	char dns1[FM160_NET_ADDR_MAX];
	char dns2[FM160_NET_ADDR_MAX];
	char pdp_in_use[8];          /* as the modem spelled it, for evidence  */

	/* Which of AT+GTWWAN / AT+GTRNDIS this modem answers.  -1 until probed:
	 * the vendor's two documents contradict each other (AT-FACTS 2.1), so
	 * this is a fact about the unit, discovered at run time. */
	int  verb;

	int  attempt;                /* rung of the refresh ladder             */
	uint64_t attempt_ms;         /* when the current attempt began         */
	uint64_t next_try_ms;
	int  ip_polls;
	/* Which sub-step of the current rung is out.  The activation rung needs
	 * one (probe +GTWWAN=? / probe +GTRNDIS=? / deactivate first / activate)
	 * and the registration rung needs one (CREG / CGREG / CEREG), and both
	 * are the same kind of thing: a rung that is several transactions long
	 * and has to be resumable between them. */
	int  sub;
	bool running;                /* an AT transaction of the ladder is out */
	bool sim_ready;              /* AT+CPIN? said READY at least once      */
	/* The context has been up at least once during this attempt.  Recorded
	 * because re-activating a context that is still half-open is not the
	 * same operation as activating a fresh one: DESIGN 4.3's "light reset"
	 * is exactly the deactivate-then-activate pair, and it is only owed
	 * when there was something to deactivate. */
	bool was_up;

	/* uci, re-read on every config reload */
	bool allow_reset;            /* may the ladder end in AT+CFUN=1,1      */
	bool autostart;              /* dial as soon as the port is there      */
	bool autostart_done;         /* one attempt per ident run              */
	bool config_error;           /* bad APN or cid: retrying cannot help   */

	uint64_t last_ok_ms;
	char last_error[FM160_STR_MAX];

	/* The evidence DESIGN 4.3 asks for.  A daemon that silently resets a
	 * modem forever turns a bad card into a router that reboots its modem
	 * every few minutes, and hides the fault while doing it; the count is
	 * kept so that stopping is a decision with a number behind it. */
	int  resets_in_window;
	int  resets_total;
	bool healing_stopped;

	int  starts, failures;
};

/*
 * The USB profile switch (DESIGN 5).
 *
 * The decision itself lives in usbmode.c and is pure; this is the part that
 * talks to the modem, and it exists mainly to make the two irreversible mistakes
 * impossible:
 *
 *   - applying a target that was never checked against the device list
 *   - losing the ability to say what the profile was before the switch
 *
 * The rollback point is written to uci BEFORE the write, because after
 * AT+GTUSBMODE= the modem may re-enumerate and there is no second chance to
 * record where we came from.
 */
enum fm160_modesw_state {
	MODESW_IDLE = 0,      /* nothing pending                          */
	MODESW_CAPS,          /* reading AT+GTUSBMODE=?                    */
	MODESW_APPLY,         /* the write is in flight                    */
	MODESW_VERIFY,        /* waiting for the modem to come back        */
	MODESW_STUCK,         /* no AT port after a switch: stop, do not spin */
};

struct fm160_modesw {
	enum fm160_modesw_state st;
	int  current;                /* -1 until AT+GTUSBMODE? answered        */
	int  target;                 /* -1 when nothing was requested          */
	int  rollback_mode;          /* where we came from, for the one retry  */

	bool caps_valid;
	int  supported[FM160_USBMODE_SUP_MAX];
	int  supported_n;

	bool pending;                /* a switch was applied and not verified  */
	uint64_t pending_since_ms;
	uint64_t applied_ms;
	int  verify_result;          /* 0 unverified, 1 arrived, -1 rolled back */
	bool rolled_back;

	bool busy;
	uint64_t last_ok_ms;
	char last_error[FM160_STR_MAX];
};

struct fm160_state {
	/* port */
	char port[FM160_PORT_MAX];
	bool port_found;
	bool port_probing;
	uint64_t port_next_probe_ms;
	char cand[FM160_CAND_MAX][FM160_PORT_MAX];
	int  cand_count;
	int  cand_idx;

	/* identity */
	char manufacturer[FM160_STR_MAX];
	char model[FM160_STR_MAX];
	char revision[FM160_STR_MAX];
	char imei[FM160_STR_MAX];
	char sn[FM160_STR_MAX];
	char iccid[FM160_STR_MAX];
	int  usb_mode;                 /* -1 unknown */
	bool ident_done;

	/* sim / registration */
	char pin_status[32];
	int  creg, cgreg, cereg, c5greg;   /* FM160_REG_UNKNOWN = unknown */
	char oper[FM160_STR_MAX];
	int  act_rat;

	/* signal */
	int  csq_rssi, csq_ber;            /* 99 = unknown */
	/* AT+GTCSQNREN=1 replaces +CSQ's <rssi> with ss_rsrp (0..126) and makes
	 * the -113+2n formula meaningless.  We never enable it; this flag lets
	 * the UI hide the dBm number instead of showing a wrong one. */
	bool csq_is_ss_rsrp;
	/*
	 * AT+CESQ (AT manual 9.1.2) returns NINE fields:
	 *   <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>,<ss_rsrq>,<ss_rsrp>,<ss_sinr>
	 * The first four are GERAN/UTRAN, 4-5 are LTE, 6-8 are NR.  A field that
	 * does not apply to the current serving cell is 255 (99 for rxlev/ber).
	 * Raw values are kept as reported; the decoded ones are in tenths of a dB
	 * (or dBm) so that no float arithmetic is needed anywhere.
	 */
	int  cesq_rxlev, cesq_ber, cesq_rscp, cesq_ecno, cesq_rsrq, cesq_rsrp;
	int  cesq_ss_rsrq, cesq_ss_rsrp, cesq_ss_sinr;
	int  geran_rssi_dbm;               /* -110 + rxlev   (0..63)   */
	int  utra_rscp_dbm;                /* -120 + rscp    (0..96)   */
	int  utra_ecno_db10;               /* -240 + 5*ecno  (0..49)   */
	int  lte_rsrp_dbm;                 /* -140 + rsrp    (0..97)   */
	int  lte_rsrq_db10;                /* -195 + 5*rsrq  (0..34)   */
	int  nr_ss_rsrp_dbm;               /* -156 + ss_rsrp (0..126)  */
	int  nr_ss_rsrq_db10;              /* -430 + 5*ss_rsrq (0..126)*/
	int  nr_ss_sinr_db10;              /* -230 + 5*ss_sinr (0..127)*/

	struct fm160_cell serving;   /* headline cell = highest RAT seen      */
	struct fm160_cell serving2;  /* EN-DC: the LTE anchor, otherwise empty */
	struct fm160_cell neigh[10];
	int  neigh_count;
	uint64_t cell_last_ok_ms;

	/* M4: band lock / cell lock / carrier aggregation */
	struct fm160_gtact_state    gtact;
	struct fm160_gtact_caps     gtact_caps;
	struct fm160_celllock_state celllock;
	struct fm160_celllock_caps  celllock_caps;
	struct fm160_ca_state       ca;

	/* M5: GNSS.  Published as its own top-level table rather than folded in
	 * with the M4 settings: those are one screen's worth of loosely related
	 * locking options, while GNSS is a subsystem with its own switch, its own
	 * per-read picture and its own write gate. */
	struct fm160_gnss_state     gnss;

	/* M3: SMS.  Its own table for the same reason GNSS has one: it is a
	 * subsystem with its own storage, its own readiness gate and its own
	 * write path, not three settings on one screen. */
	struct fm160_sms_state      sms;

	/* M2: the data plane and the USB profile switch.  Two tables rather than
	 * one: "how the link is dialled" and "which physical profile the modem
	 * is in" are different questions with different failure modes, and the
	 * second can take the entire management channel away. */
	struct fm160_dial_state     dial;
	struct fm160_modesw         modesw;

	/* netdev counters (read from sysfs, costs zero AT) */
	char netdev[32];
	uint64_t rx_bytes, tx_bytes;
	uint64_t rx_bytes_prev, tx_bytes_prev;
	uint64_t rx_bps, tx_bps;

	/* health */
	int  at_state;                     /* 0 ok, 1 degraded, 2 dead   */
	int  consec_timeout;
	uint64_t last_ok_ms;
	uint64_t last_fail_ms;
	uint64_t at_busy_max_ms;           /* worst response time seen   */

	/* quiet window */
	int  quiet_kind;
	uint64_t quiet_until_ms;
	char quiet_reason[FM160_STR_MAX];

	/* foreground detection */
	uint64_t last_active_ms;
	bool foreground;

	/* accepted profile override from config */
	bool enabled;
	int  tier_scale;                   /* 1 = normal, 2 = relaxed    */
};

enum {
	QUIET_NONE = 0,
	QUIET_MANUAL,
	QUIET_CFUN,
	QUIET_MODE_SWITCH,
	QUIET_DIAL,
	QUIET_COPS,
	QUIET_GNSS,
	/* M4.  The manual forbids combining GTACT/GTRAT/COPS/GTCELLLOCK, and both
	 * of these are persistent writes, so they get their own reason and their
	 * own quiet window rather than sharing QUIET_MANUAL. */
	QUIET_BANDS,
	QUIET_CELLLOCK,
	/* M3.  The SMS setup (CMGF/CPMS/CNMI) and the fetch of a stored message
	 * both answer in seconds on a modem with no card, and AT+CPMS? was
	 * measured at 10.3 s.  A poll landing in the middle of the CMGS prompt
	 * transaction would be read by the modem as the message body, so the
	 * window covers the whole send. */
	QUIET_SMS,
};

extern struct fm160_state g_state;
extern struct ubus_context *g_ubus;

/* ------------------------------------------------------------------ */
/* time helpers                                                         */
/* ------------------------------------------------------------------ */

uint64_t fm160_now_ms(void);

/* ------------------------------------------------------------------ */
/* config (uci /etc/config/fm160)                                       */
/* ------------------------------------------------------------------ */

void fm160_config_load(void);

/* Read one option of the fm160.main section.  Returns 0 on success. */
int  fm160_uci_get(const char *option, char *out, size_t outlen);
/* The same, for one of the state sections (dial / switch). */
int  fm160_uci_get_section(const char *section, const char *option,
			   char *out, size_t outlen);

/*
 * Write one option of an arbitrary section and commit.  Returns 0 on success.
 *
 * Deliberately the ONLY write path into uci, and deliberately narrow: two
 * pieces of state have to outlive a restart, and both of them are the record of
 * something that cannot be observed afterwards.
 *
 *   - the mode-switch rollback point.  After AT+GTUSBMODE the modem may
 *     re-enumerate, so this is written BEFORE the command: once it has been
 *     sent there is no second chance to record where we came from.
 *   - the module-reset ledger (DESIGN 4.3).  "Three resets in 24 h and then
 *     stop" is only a limit if the count survives a daemon restart, and a
 *     permanent `while true; do reset; done` is exactly the failure this is
 *     meant to prevent.
 *
 * The value is checked against a conservative character set first: it reaches
 * a shell, and refusing a value is better than quoting one.  `section` and
 * `option` are callers' literals and are not checked.
 */
int  fm160_uci_set(const char *section, const char *option, const char *value);
int  fm160_uci_set_int(const char *section, const char *option, long long v);

/* ------------------------------------------------------------------ */
/* atq.c - AT queue over at-daemon                                      */
/* ------------------------------------------------------------------ */

int  atq_init(void);
/* Enqueue a command.  cb may be NULL (fire and forget).  Returns 0 if the
 * request was accepted, -EAGAIN when AT is not usable (no port yet, quiet
 * window, circuit open) and -ENOSPC when the queue is full. */
int  atq_submit(enum at_prio prio, const char *cmd, const char *end_flag,
		int timeout_ms, at_done_cb cb, void *arg);
/*
 * A two-stage request: send `cmd`, wait for `prompt`, then write the raw bytes
 * encoded in `payload_hex` (no line terminator - the caller includes the 0x1A)
 * and wait for `end_flag`.
 *
 * The callback receives the outcome of the exchange as a whole.  A request that
 * never reaches the prompt is reported as a timeout with the modem's partial
 * answer in the response, so the caller can tell "the modem refused" from "the
 * modem never answered".
 */
int  atq_submit_prompt(enum at_prio prio, const char *cmd, const char *prompt,
		       const char *payload_hex, const char *end_flag,
		       int timeout_ms, at_done_cb cb, void *arg);
int  atq_submit_silent(enum at_prio prio, const char *cmd, int timeout_ms);
void atq_set_quiet(int kind, const char *reason, int seconds);
void atq_clear_quiet(void);
bool atq_quiet_active(void);
int  atq_depth(void);
void atq_state_reset(void);
/* Called by the port prober while scanning candidates. */
int  atq_probe_port(const char *port, at_done_cb cb, void *arg);
/* Force a fresh lookup of the transport ubus object. */
void at_daemon_hint_reconnect(void);

/* Response helpers usable from callbacks. */
bool fm160_resp_ok(const char *resp);
bool fm160_resp_error(const char *resp);
/* Find a line starting with "prefix" (after stripping echo); copies the rest
 * (after the prefix, leading spaces trimmed) into out. Returns true on hit. */
bool fm160_resp_find(const char *resp, const char *prefix, char *out, size_t outlen);
int  fm160_resp_lines(const char *resp, const char *prefix,
		      char out[][FM160_STR_MAX], int max);

/* ------------------------------------------------------------------ */
/* sched.c - polling scheduler                                          */
/* ------------------------------------------------------------------ */

void fm160_sched_init(void);
void fm160_sched_tick(void);           /* called from the 1 Hz housekeeping timer */
void fm160_sched_note_result(bool ok, bool timeout);
void fm160_sched_report_foreground(void);
void fm160_sched_kick(void);           /* force an immediate refresh of tier 0/1 */
void fm160_sched_suspend(int seconds); /* park every tier for a while          */

/* ------------------------------------------------------------------ */
/* state.c - snapshot and push                                          */
/* ------------------------------------------------------------------ */

void fm160_state_init(void);
void fm160_state_touch_ok(void);
/* Publish the current snapshot to ubus subscribers as "fm160.state". */
void fm160_state_publish(void);
void fm160_state_mark_dirty(void);
/* Build (or reuse) the serialised snapshot; caller must not free it twice. */
struct blob_buf *fm160_state_blob(void);
/* netdev counters */
void fm160_netdev_poll(void);

/* ------------------------------------------------------------------ */
/* cmds.c - FM160 command layer                                         */
/* ------------------------------------------------------------------ */

void fm160_cmd_ident_start(void);
void fm160_ident_reset(void);
void fm160_cmd_poll_reg(void);
void fm160_cmd_poll_signal(void);
void fm160_cmd_poll_cell(void);
/* M4 polls: all three are cache refreshers for the snapshot. */
void fm160_cmd_poll_gtact(void);
void fm160_cmd_poll_celllock(void);
void fm160_cmd_poll_ca(void);

/* M4 write paths.  Both refuse to run when the capability enumeration failed,
 * both read the value back before reporting success, and neither ever resets
 * the module.  cb receives AT_STATUS_OK only after the read-back agrees. */
int  fm160_cmd_set_bands(const char *bands_csv, at_done_cb cb, void *arg);
int  fm160_cmd_set_celllock(int mode, int rat, int type, unsigned long long earfcn,
			    int pci, int scs, int nrband, at_done_cb cb, void *arg);
/* The command string the write path would send - exposed so the UI can show it
 * and so it can be checked without touching the modem. */
int  fm160_bands_command(char *out, size_t outlen, const char *bands_csv);
int  fm160_celllock_command(char *out, size_t outlen, int mode, int rat, int type,
			    unsigned long long earfcn, int pci, int scs, int nrband);

/* M5 polls.  The NMEA read is only ever submitted while the engine is on:
 * with the engine off AT+GTGPS? answers ERROR, so the read would be pure
 * waste.  The power read is always submitted, because it is the only way to
 * notice that the engine is (or is no longer) running. */
void fm160_cmd_poll_gnss(void);
void fm160_cmd_poll_gnss_power(void);
/* Turn the engine on once per ident run when uci asks for it.  Called at the
 * end of the identity chain, where the engine state has just been read. */
void fm160_cmd_gnss_autostart(void);

/* M5 write paths, on the same write-then-read-back contract as M4. */
int  fm160_cmd_set_gnss_power(int on, at_done_cb cb, void *arg);
int  fm160_cmd_set_gnss_cfg(int value, at_done_cb cb, void *arg);

/* The command strings the M5 write paths would send.  Exposed for the same
 * reason the M4 builders are: they can be printed and checked without a modem
 * anywhere near them. */
int  fm160_gnss_item_command(char *out, size_t outlen, const char *item);
int  fm160_gnss_power_command(char *out, size_t outlen, int on);
int  fm160_gnss_cfg_command(char *out, size_t outlen, int value);

/* Parsers (also used by the AT debug page / tests). */
void fm160_parse_csq(const char *resp);
void fm160_parse_cesq(const char *resp);
void fm160_parse_greg(const char *resp, const char *prefix, int *slot);
void fm160_parse_ccinfo(const char *resp);
void fm160_parse_usbmode(const char *resp);
void fm160_parse_gtact(const char *resp);
void fm160_parse_gtact_caps(const char *resp);
void fm160_parse_celllock(const char *resp);
void fm160_parse_celllock_caps(const char *resp);
void fm160_parse_cainfo(const char *resp);
void fm160_parse_gnss(const char *resp);
void fm160_parse_gnss_power(const char *resp);
void fm160_parse_gnss_cfg(const char *resp);
void fm160_parse_gnss_cfg_caps(const char *resp);
void fm160_parse_gnss_epo(const char *resp);
void fm160_parse_gnss_supl(const char *resp);

/* Band encoding shared by the parsers and the write path. */
int  fm160_band_decode(int rat, int raw);
int  fm160_band_rat_of(int raw);

/* ------------------------------------------------------------------ */
/* sms.c - the SMS subsystem (M3)                                       */
/* ------------------------------------------------------------------ */

/*
 * The whole subsystem is driven from two places: a page asking for something
 * ("send", "refresh", "delete"), and the modem announcing a message of its own
 * accord (+CMTI).  There is no timer behind any of it - see the note above
 * struct fm160_sms_state for the measurements that decided that.
 */

/* Reset the counters and load whatever we have stored locally. */
void fm160_sms_init(void);
/* Drive the one-time AT+CMGF / AT+CPMS / AT+CNMI setup.  Safe to call from
 * anywhere and as often as you like: it submits nothing once the setup has
 * succeeded, and backs off after a refusal instead of retrying into a modem
 * that has already said no. */
void fm160_sms_setup_tick(void);
/* Invalidate the SMS setup so it re-arms.  Must be called whenever the
 * module is (re)identified: a module reset - AT+CFUN, a flash, a power cycle
 * - silently puts CNMI back to 0,0,0,0,0 and CPMS back to "SM", and the
 * cached "setup_done" flag would otherwise keep the daemon from ever
 * re-arming the new module (+CMTI never arrives again). */
void fm160_sms_setup_reset(void);
bool fm160_sms_usable(void);

/* Pull the messages the modem is holding.  Priority 1, opens the SMS quiet
 * window, and refuses while another SMS operation is in flight. */
int  fm160_cmd_sms_sync(void);
/* Read one index and append it - the +CMTI path. */
int  fm160_cmd_sms_fetch(int index);
/* Remove one message from the modem (and from our list). */
int  fm160_cmd_sms_delete(int index, at_done_cb cb, void *arg);
/* Encode, split and send.  cb is called once, with the outcome of the whole
 * message - not once per segment. */
int  fm160_cmd_sms_send(const char *number, const char *text,
			at_done_cb cb, void *arg);

/* Consume an unsolicited line.  Returns true when it was one of ours. */
bool fm160_sms_handle_urc(const char *line);

/* Our own list, newest first.  nth 0 is the most recent. */
int  fm160_sms_count(void);
const struct fm160_sms_msg *fm160_sms_get(int nth);
/* One message by its local id, or NULL.  Used by the delete path, which needs
 * the modem-side index before it can decide whether the modem has a copy to
 * remove as well. */
const struct fm160_sms_msg *fm160_sms_find(uint64_t id);
/* Drop one message by its local id.  Returns 0, or -ENOENT. */
int  fm160_sms_delete_local(uint64_t id);
int  fm160_sms_mark_read(uint64_t id);
void fm160_sms_clear(void);

/* Parsers.  Exposed for the host-side test and the AT debug page. */
void fm160_parse_cmgr(const char *resp, int index, const char *storage);
void fm160_parse_cpms(const char *resp);
void fm160_parse_cpms_caps(const char *resp);
void fm160_parse_cmgl(const char *resp);
void fm160_parse_cmgs(const char *resp);

/* ------------------------------------------------------------------ */
/* dialer.c - the ECM data plane                                        */
/* ------------------------------------------------------------------ */

/* Reset the state and read the persistent reset ledger. */
void fm160_dial_init(void);
/* The state machine's only clock.  Driven from housekeeping, like the SMS
 * setup: there is no poll tier behind this, because a dial sequence is a
 * sequence and not a poll. */
void fm160_dial_tick(void);

/* Take the APN/pdp/cid from uci.  Safe to call on every config reload. */
void fm160_dial_configure(const char *apn, const char *pdp, int cid,
			  bool allow_reset, bool autostart);

/* Ask for the link.  Returns 0, -EINVAL when no APN is configured and
 * -ENOTSUP when the active profile is not one this daemon dials. */
int  fm160_dial_start(void);
/* Take the link down with AT+GTWWAN=0,<cid>.  Pulling the cable or powering
 * the module down is NOT a disconnect (the vendor is explicit about this). */
int  fm160_dial_stop(void);
/* True when the current profile is one this daemon dials. */
bool fm160_dial_usable(void);

/* ------------------------------------------------------------------ */
/* modesw.c - the USB profile switch                                    */
/* ------------------------------------------------------------------ */

void fm160_modesw_init(void);
void fm160_modesw_tick(void);
/* Ask to switch.  Returns 0 when the request was accepted for execution, or a
 * negative errno; the refusal's reason is in the snapshot either way. */
int  fm160_modesw_apply(int target, bool advanced_ok);
/* Record the capability list (from AT+GTUSBMODE=?) and the active profile
 * (from AT+GTUSBMODE?).  Called by the identity chain. */
void fm160_modesw_note_caps(const int *modes, int n);
void fm160_modesw_note_current(int mode);

/* ------------------------------------------------------------------ */
/* ubus_methods.c                                                       */
/* ------------------------------------------------------------------ */

void fm160_ubus_methods_init(void);

/* ------------------------------------------------------------------ */
/* logging                                                              */
/* ------------------------------------------------------------------ */

void fm160_log(int priority, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/*
 * The remembered log.
 *
 * fm160_log() has always done exactly one thing: hand the line to vsyslog().
 * The consequence is that fm160d's own log is only ever as durable as logd's
 * ring, which on this box is shared with every other daemon, is lost at the
 * first reboot, and cannot be read at all from inside the process.  That made
 * "send me the log" a request the daemon could not answer about itself, and a
 * bug report therefore arrived as "the page went blank" with nothing else.
 *
 * So a fixed-size ring is kept beside the syslog call.  Deliberately NOT a
 * file: a log file on the overlay needs rotation, a rotation that fails fills
 * the flash, and this is a router.  Deliberately not in /tmp either - tmpfs is
 * where the interesting minutes go when RAM is what ran out.
 *
 * The ring is filled AFTER the level gate, i.e. it contains exactly what syslog
 * received and nothing more.  That keeps the "log level" setting meaningful for
 * the export too: what you would have seen in logread is what you download.
 * The level in force is reported in the bundle, so a DEBUG line that is absent
 * is explained by the header rather than being a mystery.
 *
 * FM160_LOG_LINE_MAX is a variable-length field inside a fixed-size record, so
 * a line longer than it is truncated and marked; the export says "(truncated)"
 * on that line rather than silently shortening it.
 */
#define FM160_LOG_RING_LINES 128
#define FM160_LOG_LINE_MAX   192

/* Sizing for the diagnostics buffer.  See diag.h for the two halves. */
#define FM160_DIAG_BUF_MAX  (FM160_DIAG_HEAD_MAX + \
			     FM160_LOG_RING_LINES * FM160_DIAG_LOG_LINE_MAX)

struct fm160_log_rec {
	uint64_t at_ms;                  /* monotonic, when it was logged     */
	int      prio;                   /* LOG_ERR..LOG_DEBUG               */
	bool     truncated;              /* did not fit in msg[]              */
	char     msg[FM160_LOG_LINE_MAX];
};

/* How many lines the ring holds right now (0..FM160_LOG_RING_LINES). */
size_t fm160_log_ring_count(void);
/* How many lines have ever been logged, so the export can say how many were
 * dropped.  Counting them is the difference between "the log looks short" and
 * "the log IS short because 400 lines went past". */
uint64_t fm160_log_ring_total(void);
/* The i-th remembered line, oldest first, or NULL past the end.  The pointer
 * stays valid until the next fm160_log() call, so a reader must not log while
 * it walks the ring. */
const struct fm160_log_rec *fm160_log_ring_at(size_t i);

/* The effective log level, and how long the daemon has been up.  Both live in
 * main.c and are read by the diagnostics handler. */
int      fm160_log_level(void);
uint64_t fm160_uptime_ms(void);

/* <syslog.h> defines LOG_ERR/LOG_INFO/LOG_DEBUG with these exact values; the
 * guards keep this header usable both with and without it. */
#ifndef LOG_ERR
#define LOG_ERR   3
#endif
#ifndef LOG_WARN
#define LOG_WARN  4
#endif
#ifndef LOG_INFO
#define LOG_INFO  6
#endif
#ifndef LOG_DEBUG
#define LOG_DEBUG 7
#endif

#endif /* FM160D_H */
