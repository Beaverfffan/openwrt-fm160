/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * usbmode - the USB profile table, and the policy for moving between profiles.
 *
 * WHY THIS FILE IS SEPARATE AND libc-ONLY
 *
 * Switching USB profile is the one operation in this project that can put the
 * modem permanently beyond reach.  The FM160 has no reset-to-default that can be
 * triggered from the host side, so a profile with no AT interface - or one whose
 * AT interface the kernel will not enumerate - is a one-way door that ends with
 * somebody walking over to the device and pulling its power.
 *
 * A safety decision of that weight must be provable without a modem present.
 * Everything here is therefore pure: a table, some predicates, and one decision
 * function with no side effects.  It includes nothing from libubox, which is
 * what lets 25-usbmode-dial-test.sh compile it on its own and exercise every
 * combination, including the ones no live modem would ever be talked into.
 *
 * The daemon is the authority, not the web page.  api.js carries the same table
 * so the UI can label and filter options, but the UI can only ever *propose*;
 * fm160_usbmode_decide() is what actually stands between a request and
 * AT+GTUSBMODE.  The host-side test parses api.js and asserts the two tables
 * still agree, because a table that drifts apart is how a mode that the daemon
 * refuses ends up being offered by the page anyway.
 */

#ifndef FM160_USBMODE_H
#define FM160_USBMODE_H

#include <stdbool.h>
#include <stddef.h>

/* How the host will dial once this profile is active. */
enum fm160_dial_kind {
	DIAL_KIND_NONE = 0,   /* no data interface at all                */
	DIAL_KIND_QMI,        /* qmi_wwan -> /dev/cdc-wdm0 + wwan0       */
	DIAL_KIND_MBIM,       /* cdc_mbim -> /dev/cdc-wdm0 + wwan0       */
	DIAL_KIND_ECM,        /* cdc_ether -> usb0, dialled from inside  */
};

#define FM160_DIAL_KIND_MAX 4

/* One row of the vendor's FM160 port table (dial-up document 2.1.1, table 1). */
struct fm160_usbmode_info {
	int  mode;
	/* PID as four lowercase hex digits, or "" when the vendor table gives
	 * none for this mode.  Informational only: 17 and 32 share 0x0104 and
	 * 18 and 33 share 0x0105, so VID:PID can never identify the active
	 * profile.  Only AT+GTUSBMODE? can. */
	const char *pid;
	enum fm160_dial_kind kind;
	/* The composite descriptor contains an "AT Device Application
	 * Interface".  False means AT+GTUSBMODE could never be sent again. */
	bool has_at;
	/* Listed in AT manual 11.1.2.4.  A mode known only from the port table
	 * is "device dependent": it may work, but nothing documents it. */
	bool documented;
	/* The kernel holds a drivers/usb/serial/option.c entry with a matching
	 * PID.  Without one option.ko never probes the device's interfaces at
	 * all - the descriptor can be perfectly healthy and there is still no
	 * /dev/ttyUSB*.  False is just as fatal as has_at == false. */
	bool kernel_entry;
	const char *layout;
};

/*
 * Why a switch was refused.  Every refusal names the specific fact that failed,
 * because the refusal is shown to a person who is about to conclude "this
 * firmware is broken" - "mode 29 would leave no AT port on this host" is
 * actionable; "invalid target" is not.
 */
enum fm160_usbmode_verdict {
	USB_MODE_OK = 0,
	USB_MODE_OK_SAME,            /* already there: successful no-op      */
	USB_MODE_DENY_UNKNOWN,       /* no such profile number               */
	USB_MODE_DENY_NO_AT,         /* the modem would keep no management channel */
	USB_MODE_DENY_NO_DRIVER,     /* the host would keep no management channel  */
	USB_MODE_DENY_UNDOCUMENTED,  /* device dependent; opt-in required    */
	USB_MODE_DENY_NOT_SUPPORTED, /* absent from the modem's own list     */
	USB_MODE_DENY_LIST_UNKNOWN,  /* AT+GTUSBMODE=? was never answered    */
};

/* Largest list the modem's capability answer may carry. */
#define FM160_USBMODE_SUP_MAX 32

/* The table, or NULL.  Never NULL for a mode this firmware has ever offered. */
const struct fm160_usbmode_info *fm160_usbmode_info(int mode);
int  fm160_usbmode_table_size(void);
/* Row `idx` of the table, in the vendor's own order, or NULL.  Provided so the
 * state snapshot and the page can enumerate profiles without duplicating the
 * table. */
const struct fm160_usbmode_info *fm160_usbmode_at(int idx);

bool fm160_usbmode_known(int mode);
bool fm160_usbmode_has_at(int mode);
bool fm160_usbmode_documented(int mode);
bool fm160_usbmode_kernel_entry(int mode);
enum fm160_dial_kind fm160_usbmode_kind(int mode);
const char *fm160_usbmode_layout(int mode);
const char *fm160_usbmode_pid(int mode);
const char *fm160_dial_kind_name(enum fm160_dial_kind kind);

/*
 * Modes that must never be a switch target, whatever the modem says it
 * supports.  Present here rather than filtered out of the table so that the UI
 * can still NAME them: a page that shows "mode 24 - RNDIS+MODEM+DIAG+ADB" and a
 * greyed-out reason is more useful than one that pretends 24 does not exist.
 *
 * 20, 24, 28 and 31 have no AT interface (20, 24 and 31 are in this unit's
 * AT+GTUSBMODE=? answer; 28 comes from the vendor table and is kept as a guard
 * for other firmware builds).
 */
bool fm160_usbmode_blacklisted(int mode);

/*
 * Preferred target per dial kind, best first, restricted to what is worth
 * offering.  Returns the count written to `out` (never more than `max`).
 *
 * ECM prefers 33 over 18 because both are the same ECM layout with and without
 * the ADB interface, and fewer interfaces means fewer things that can go wrong
 * on re-enumeration.  MBIM lists 30 only: 29 looks better on paper (two MBIM
 * interfaces, no extra Modem port) but its PID 0x0110 has no option.c entry, so
 * the host would lose the AT port.  Keep this in step with kernel_entry.
 */
int fm160_usbmode_preferred(enum fm160_dial_kind kind, int *out, int max);

/*
 * The decision.  `current` may be -1 when the active profile is not known yet.
 *
 * `supported`/`supported_n` are the modes the MODEM reported in
 * AT+GTUSBMODE=?; `supported_known` says whether the modem was ever asked.
 * The design's second gate is exactly this: a target has to be in both the
 * modem's own list and the hard whitelist.  A modem that has never answered
 * yields USB_MODE_DENY_LIST_UNKNOWN for everything - quitting while ahead is the
 * whole point of the exercise.
 *
 * `advanced_ok` licenses the "documented == false" modes (19/22/23).  It is the
 * caller's job to set it only after an explicit confirmation; this function
 * does not ask.
 */
enum fm160_usbmode_verdict
fm160_usbmode_decide(int current, int target,
		     const int *supported, int supported_n,
		     bool supported_known, bool advanced_ok);

/* A sentence naming the failed fact.  Never NULL, never longer than a line. */
const char *fm160_usbmode_verdict_text(enum fm160_usbmode_verdict v);
/* True for the two non-refusals. */
bool fm160_usbmode_verdict_is_ok(enum fm160_usbmode_verdict v);

/*
 * "AT+GTUSBMODE: (17-18,20-21,24,29-33)" -> { 17,18,20,21,24,29,30,31,32,33 }.
 *
 * The answer uses closed ranges, so a parser that only splits on commas finds
 * four of the ten modes this firmware offers - and the four it loses are
 * precisely the ones the user is most likely to want (32 is inside "29-33",
 * and 32 is the profile this device ships in).
 *
 * Returns the number of modes written, or -1 when nothing parseable was found.
 * An empty list "( )" is a legitimate answer and returns 0; the caller must
 * treat "answered, but with nothing" as different from "never answered", for
 * the same reason the registration parsers keep FM160_REG_UNKNOWN apart from 0.
 */
int fm160_usbmode_parse_list(const char *resp, int *out, int max);

/* "AT+GTUSBMODE: 32" -> 32, or -1.  Only a bare number is accepted: the
 * capability answer "AT+GTUSBMODE: (17-18,...)" must NOT be misread as the
 * current profile, and that is the bug this function exists to avoid. */
int fm160_usbmode_parse_current(const char *resp);

/*
 * True when `mode` may be listed as a switch target for this dial kind.
 *
 * This is the UI's filter and nothing more: it answers "is this worth putting in
 * the dropdown", where fm160_usbmode_decide() answers "may it be applied".  The
 * daemon calls decide() again on the way in, so a page that ignores this
 * function cannot switch anything it should not.
 *
 * `advanced_ok` includes the device-dependent modes (19/22/23), which is what a
 * "show advanced profiles" opt-in would set.
 */
bool fm160_usbmode_offerable(enum fm160_dial_kind kind, int mode, bool advanced_ok);

#endif /* FM160_USBMODE_H */
