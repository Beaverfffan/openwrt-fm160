/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * usbmode.c - the profile table and the switch policy.  See usbmode.h for why
 * this is a separate, libc-only file.
 */

#include <stdlib.h>
#include <string.h>

#include "usbmode.h"

/*
 * The FM160 port table (dial-up document 2.1.1, table 1), merged with the two
 * other facts that decide safety:
 *
 *   documented    listed in AT manual 11.1.2.4
 *   kernel_entry  drivers/usb/serial/option.c carries a matching PID
 *
 * The last column is the vendor's own interface composition and is copied
 * verbatim, because it is what the UI shows a person who is deciding.  A row is
 * never removed from this table when it becomes unreachable: mode 24 is a
 * one-way door, and the page is a better place to learn that than an absence.
 *
 * kernel_entry comes from the 6.6.127 source that ships with the build tree
 * (AT-FACTS.md 3.2(2)); the zero-count verification in that section is what
 * establishes the four "false" rows that matter, since a missing entry is
 * invisible in every other way.
 */
static const struct fm160_usbmode_info modes[] = {
	{ 17, "0104", DIAL_KIND_QMI,  true,  true,  true,
	  "DIAG+MODEM+AT+PIPE+RMNET+ADB" },
	{ 18, "0105", DIAL_KIND_ECM,  true,  true,  true,
	  "DIAG+MODEM+AT+PIPE+ECM+ECM+ADB" },
	{ 19, "0106", DIAL_KIND_ECM,  true,  false, true,
	  "DIAG+MODEM+AT+ECM+ECM" },
	{ 20, "0107", DIAL_KIND_NONE, false, true,  false,
	  "MODEM (no AT, no data)" },
	{ 21, "0108", DIAL_KIND_NONE, true,  true,  false,
	  "MODEM+AT (no data)" },
	{ 22, "0109", DIAL_KIND_QMI,  true,  false, false,
	  "MODEM+AT+RMNET" },
	{ 23, "010a", DIAL_KIND_ECM,  true,  false, true,
	  "MODEM+AT+ECM+ECM" },
	{ 24, "010b", DIAL_KIND_NONE, false, true,  true,
	  "RNDIS+RNDIS+MODEM+DIAG+ADB (no AT)" },
	{ 28, "010f", DIAL_KIND_NONE, false, false, false,
	  "MBIM only (no AT)" },
	{ 29, "0110", DIAL_KIND_MBIM, true,  true,  false,
	  "MBIM+MBIM+AT+DIAG" },
	{ 30, "0111", DIAL_KIND_MBIM, true,  true,  true,
	  "MBIM+MBIM+MODEM+DIAG+AT" },
	{ 31, "",     DIAL_KIND_NONE, false, true,  false,
	  "DIAG+MODEM+RMNET+DPL+QDSS+ADB (no AT)" },
	{ 32, "0104", DIAL_KIND_QMI,  true,  true,  true,
	  "DIAG+MODEM+AT+PIPE+RMNET" },
	{ 33, "0105", DIAL_KIND_ECM,  true,  true,  true,
	  "DIAG+MODEM+AT+PIPE+ECM+ECM" },
};

#define MODE_COUNT ((int)(sizeof(modes) / sizeof(modes[0])))

/*
 * The hard blacklist.  These are the modes whose selection costs the AT channel
 * itself, which on this module cannot be undone from the host side.
 *
 * It is an explicit list rather than "everything with has_at == false" on
 * purpose: the two agree today, and the host-side test asserts that they do, so
 * a row added later with a missing AT interface shows up as a test failure
 * instead of silently inheriting a rule nobody wrote down.
 */
static const int mode_blacklist[] = { 20, 24, 28, 31 };
#define BLACKLIST_COUNT ((int)(sizeof(mode_blacklist) / sizeof(mode_blacklist[0])))

/* Every mode number in circulation is two digits.  Anything larger is a
 * misparse, and letting it through would truncate when narrowed to int. */
#define SANE_MAX 255

const struct fm160_usbmode_info *fm160_usbmode_info(int mode)
{
	int i;

	for (i = 0; i < MODE_COUNT; i++)
		if (modes[i].mode == mode)
			return &modes[i];
	return NULL;
}

int fm160_usbmode_table_size(void)
{
	return MODE_COUNT;
}

const struct fm160_usbmode_info *fm160_usbmode_at(int idx)
{
	return (idx >= 0 && idx < MODE_COUNT) ? &modes[idx] : NULL;
}

bool fm160_usbmode_known(int mode)
{
	return fm160_usbmode_info(mode) != NULL;
}

bool fm160_usbmode_has_at(int mode)
{
	const struct fm160_usbmode_info *m = fm160_usbmode_info(mode);

	return m ? m->has_at : false;
}

bool fm160_usbmode_documented(int mode)
{
	const struct fm160_usbmode_info *m = fm160_usbmode_info(mode);

	return m ? m->documented : false;
}

bool fm160_usbmode_kernel_entry(int mode)
{
	const struct fm160_usbmode_info *m = fm160_usbmode_info(mode);

	return m ? m->kernel_entry : false;
}

enum fm160_dial_kind fm160_usbmode_kind(int mode)
{
	const struct fm160_usbmode_info *m = fm160_usbmode_info(mode);

	return m ? m->kind : DIAL_KIND_NONE;
}

const char *fm160_usbmode_layout(int mode)
{
	const struct fm160_usbmode_info *m = fm160_usbmode_info(mode);

	return m ? m->layout : "";
}

const char *fm160_usbmode_pid(int mode)
{
	const struct fm160_usbmode_info *m = fm160_usbmode_info(mode);

	return m ? m->pid : "";
}

const char *fm160_dial_kind_name(enum fm160_dial_kind kind)
{
	switch (kind) {
	case DIAL_KIND_QMI:  return "qmi";
	case DIAL_KIND_MBIM: return "mbim";
	case DIAL_KIND_ECM:  return "ecm";
	case DIAL_KIND_NONE: return "none";
	}
	return "none";
}

bool fm160_usbmode_blacklisted(int mode)
{
	int i;

	for (i = 0; i < BLACKLIST_COUNT; i++)
		if (mode_blacklist[i] == mode)
			return true;
	return false;
}

int fm160_usbmode_preferred(enum fm160_dial_kind kind, int *out, int max)
{
	int n = 0;
	int i;

	if (!out || max <= 0)
		return 0;

	/*
	 * The order is a preference, not a whitelist: 33 before 18 because both
	 * are the same ECM layout with and without the ADB interface, and every
	 * interface removed is one less thing to go wrong when the device
	 * re-enumerates.  MBIM offers 30 alone - 29 has the nicer descriptor but
	 * its PID 0x0110 is exactly the kind of absent option.c entry that
	 * makes a switch unrecoverable.
	 */
	static const int qmi[]  = { 32, 17 };
	static const int mbim[] = { 30 };
	static const int ecm[]  = { 33, 18 };
	const int *list = NULL;
	int list_n = 0;

	switch (kind) {
	case DIAL_KIND_QMI:  list = qmi;  list_n = 2; break;
	case DIAL_KIND_MBIM: list = mbim; list_n = 1; break;
	case DIAL_KIND_ECM:  list = ecm;  list_n = 2; break;
	default: break;
	}

	for (i = 0; i < list_n && n < max; i++) {
		/* A preference that cannot survive the switch is not a
		 * preference.  Checked here rather than trusted, so that
		 * editing the table above cannot quietly strand a modem. */
		if (!fm160_usbmode_has_at(list[i]))
			continue;
		if (!fm160_usbmode_kernel_entry(list[i]))
			continue;
		out[n++] = list[i];
	}
	return n;
}

bool fm160_usbmode_offerable(enum fm160_dial_kind kind, int mode, bool advanced_ok)
{
	const struct fm160_usbmode_info *m = fm160_usbmode_info(mode);

	if (!m)
		return false;
	if (!m->has_at || fm160_usbmode_blacklisted(mode))
		return false;
	if (!m->kernel_entry)
		return false;
	if (!m->documented && !advanced_ok)
		return false;
	/* DIAL_KIND_NONE never reaches here with has_at true, but keep the
	 * intent explicit: a profile with no data interface has nothing to
	 * offer any dial kind. */
	return m->kind == kind;
}

enum fm160_usbmode_verdict
fm160_usbmode_decide(int current, int target,
		     const int *supported, int supported_n,
		     bool supported_known, bool advanced_ok)
{
	int i;

	if (!fm160_usbmode_known(target))
		return USB_MODE_DENY_UNKNOWN;
	if (fm160_usbmode_blacklisted(target) || !fm160_usbmode_has_at(target))
		return USB_MODE_DENY_NO_AT;
	if (!fm160_usbmode_kernel_entry(target))
		return USB_MODE_DENY_NO_DRIVER;
	if (!fm160_usbmode_documented(target) && !advanced_ok)
		return USB_MODE_DENY_UNDOCUMENTED;

	/* DESIGN 5.2 step 3: the whitelist is checked before the no-op test, so
	 * "you are already in mode 24" cannot be reported as success. */
	if (current == target)
		return USB_MODE_OK_SAME;

	/*
	 * The second gate.  An unanswered AT+GTUSBMODE=? means we do not know
	 * what this firmware offers, and the design forbids switching at all in
	 * that state - which is why the "not known" case is its own verdict
	 * rather than an empty supported list.
	 */
	if (!supported_known)
		return USB_MODE_DENY_LIST_UNKNOWN;
	for (i = 0; i < supported_n; i++)
		if (supported[i] == target)
			return USB_MODE_OK;
	return USB_MODE_DENY_NOT_SUPPORTED;
}

bool fm160_usbmode_verdict_is_ok(enum fm160_usbmode_verdict v)
{
	return v == USB_MODE_OK || v == USB_MODE_OK_SAME;
}

const char *fm160_usbmode_verdict_text(enum fm160_usbmode_verdict v)
{
	switch (v) {
	case USB_MODE_OK:
		return "allowed";
	case USB_MODE_OK_SAME:
		return "the modem is already in that profile";
	case USB_MODE_DENY_UNKNOWN:
		return "no such USB profile is known for this modem";
	case USB_MODE_DENY_NO_AT:
		return "that profile has no AT interface, so the modem could never be "
		       "switched back";
	case USB_MODE_DENY_NO_DRIVER:
		return "the kernel has no driver for that profile's USB ID, so no AT "
		       "port would appear";
	case USB_MODE_DENY_UNDOCUMENTED:
		return "that profile is not in the FM160 AT manual; it needs an "
		       "explicit opt-in";
	case USB_MODE_DENY_NOT_SUPPORTED:
		return "the modem does not offer that profile";
	case USB_MODE_DENY_LIST_UNKNOWN:
		return "the modem has not answered AT+GTUSBMODE=?, so no profile change "
		       "is allowed";
	}
	return "refused";
}

/* ------------------------------------------------------------------ */
/* parsing                                                             */
/* ------------------------------------------------------------------ */

static const char *ltrim(const char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

/*
 * If `p` opens with `tok`, return the text after it and after an optional ':'.
 * The trailing character test is the important half: the echo of the command we
 * sent ("AT+GTUSBMODE=?", "+GTUSBMODE=?") also begins with the token, and
 * treating that as the modem's answer would read '=?' as a value.
 */
static const char *after_token(const char *p, const char *tok)
{
	size_t n = strlen(tok);

	if (strncmp(p, tok, n) != 0)
		return NULL;
	p += n;
	if (*p == ':')
		p++;
	else if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '\0')
		return NULL;
	return ltrim(p);
}

/*
 * The field of the modem's +GTUSBMODE answer, or NULL.
 *
 * Accepts a whole sendat response (the usual case) and a bare field (what
 * fm160_resp_find() hands back, which is what the daemon will pass).  Lines are
 * walked rather than searched with strstr(), for the echo reason above.
 */
static const char *usbmode_field(const char *s)
{
	const char *p, *q;

	if (!s)
		return NULL;

	/* Already just the field. */
	if (*s == '(' || (*s >= '0' && *s <= '9'))
		return s;

	for (p = s; p && *p; p = q) {
		const char *eol = strchr(p, '\n');
		const char *f;

		q = eol ? eol + 1 : NULL;
		f = ltrim(p);
		f = after_token(f, "+GTUSBMODE");
		if (!f)
			f = after_token(ltrim(p), "GTUSBMODE");
		if (f)
			return f;
		if (!eol)
			break;
	}
	return NULL;
}

/* Skip whitespace and separators between list tokens. */
static const char *skip_seps(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t' || *p == ','))
		p++;
	return p;
}

int fm160_usbmode_parse_list(const char *resp, int *out, int max)
{
	const char *p = usbmode_field(resp);
	const char *end;
	int n = 0;

	if (!p || !out || max <= 0)
		return -1;

	p = ltrim(p);
	if (*p == '(') {
		const char *close = strchr(p, ')');

		if (!close)
			return -1;
		p++;
		end = close;
	} else {
		end = p + strcspn(p, "\r\n");
	}

	for (p = skip_seps(p, end); p < end; p = skip_seps(p, end)) {
		long lo, hi, v;
		char *stop;

		if (*p < '0' || *p > '9')
			return -1;

		lo = strtol(p, &stop, 10);
		p = stop;
		hi = lo;

		if (p < end && *p == '-') {
			const char *q = p + 1;

			if (q >= end || *q < '0' || *q > '9')
				return -1;
			hi = strtol(q, &stop, 10);
			p = stop;
		}

		if (lo < 0 || hi < lo || hi > SANE_MAX)
			return -1;
		/* Refuse rather than truncate: a shortened list would make the
		 * page claim a profile the modem supports is unsupported, and
		 * the daemon refuse a switch that is in fact legal.
		 * hi - lo cannot overflow here: both are bounded by SANE_MAX. */
		if (n + (int)(hi - lo) + 1 > max)
			return -1;

		for (v = lo; v <= hi; v++)
			out[n++] = (int)v;
	}

	return n;
}

int fm160_usbmode_parse_current(const char *resp)
{
	const char *p = usbmode_field(resp);
	char *stop;
	long v;

	if (!p)
		return -1;

	p = ltrim(p);
	if (*p < '0' || *p > '9')
		return -1;

	v = strtol(p, &stop, 10);
	if (v < 0 || v > SANE_MAX)
		return -1;

	/* Anything but whitespace after the number means this was not the bare
	 * "AT+GTUSBMODE: 32" form.  This is the guard that stops the capability
	 * answer being read as the current profile. */
	stop = (char *)ltrim((const char *)stop);
	if (*stop && *stop != '\r' && *stop != '\n')
		return -1;

	return (int)v;
}
