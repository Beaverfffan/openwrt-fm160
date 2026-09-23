/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * net.c - data-plane command builders and parsers.  See net.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "net.h"

/* ------------------------------------------------------------------ */
/* small parsers shared by the rest                                     */
/* ------------------------------------------------------------------ */

static const char *ltrim(const char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

/*
 * Bounded copy that reports truncation rather than hiding it.
 *
 * snprintf() would do the job, but gcc cannot prove the source is short and
 * warns on every one of these (-Wformat-truncation), and a warning everyone
 * learns to scroll past is how a real one gets missed.  This also makes the
 * policy explicit: a value that does not fit is truncated AND reported, so a
 * caller that cares can treat it as "the modem said something we cannot hold"
 * rather than as a value.
 */
static bool set_str(char *dst, size_t n, const char *src)
{
	size_t len;

	if (n == 0)
		return false;
	len = strlen(src);
	if (len >= n) {
		memcpy(dst, src, n - 1);
		dst[n - 1] = '\0';
		return false;
	}
	memcpy(dst, src, len + 1);
	return true;
}

/*
 * Read one comma-separated field out of an AT response line.
 *
 * Handles the quoted form the modem uses for strings.  An unterminated quote is
 * a failure rather than "the rest of the line": the point of the quotes is that
 * a value may contain commas, so a field whose closing quote never arrives has
 * an unknown extent, and guessing it is how a parser starts inventing values.
 */
static bool csv_field(const char **p, char *out, size_t outlen)
{
	const char *s = *p;
	size_t n = 0;

	if (outlen == 0)
		return false;

	if (*s == '"') {
		s++;
		for (;;) {
			if (!*s || *s == '\r' || *s == '\n')
				return false;
			if (*s == '"')
				break;
			if (n + 1 < outlen)
				out[n++] = *s;
			s++;
		}
		s++;
	} else {
		while (*s && *s != ',' && *s != '\r' && *s != '\n') {
			if (n + 1 < outlen)
				out[n++] = *s;
			s++;
		}
	}
	out[n] = '\0';

	if (*s == ',')
		s++;
	*p = s;
	return true;
}

/* Split a field list into at most `max` fields.  A line with more fields than
 * that is truncated for storage but still reported as ITS full length, so a
 * caller can tell "five fields" from "five fields kept out of nine". */
static int split_csv(const char *line, char out[][FM160_NET_ADDR_MAX],
		     int max, int *seen)
{
	int n = 0;

	*seen = 0;
	line = ltrim(line);
	while (*line && *line != '\r' && *line != '\n') {
		char scratch[FM160_NET_ADDR_MAX];
		char *dst = (n < max) ? out[n] : scratch;

		if (!csv_field(&line, dst, FM160_NET_ADDR_MAX))
			return -1;
		(*seen)++;
		if (n < max)
			n++;
	}
	return n;
}

/*
 * The first address inside ONE field, because one field can hold TWO.
 *
 * ★★ Measured 2026-09-21 on this unit, profile 33 (ECM), context up:
 *
 *      AT+GTWWAN?
 *        -> +GTWWAN: 1,1,"10.179.143.75,240e:400:1638:3d:3897:ecce:bfca:6f88",
 *                           "218.2.2.2,240e:5a::6666",
 *                           "218.4.4.4,240e:5b::6666"
 *
 * The module packs an IPv4 and an IPv6 address into a single QUOTED field,
 * and csv_field() keeps commas that are quoted - so the field arrives here as
 * "10.179.143.75,240e:...", which fm160_net_addr_ok() rejects: it contains a
 * ':', so it is tested as IPv6, and the '.' of the leading IPv4 then fails the
 * hex test.  Validating the field as a whole therefore finds NO address in an
 * answer that plainly carries one, and every ECM dial ends in "no address
 * appeared after five reads of the context".
 *
 * Splitting on the comma and taking the first part that validates also picks
 * the IPv4 one, which is the half the ECM netdev actually uses: the address on
 * usb0 comes from the module's DHCP server, not from AT.
 */
static bool addr_from_field(const char *field, char *out, size_t outlen)
{
	const char *p = field;

	while (*p) {
		char part[FM160_NET_ADDR_MAX];

		if (!csv_field(&p, part, sizeof(part)))
			return false;
		if (fm160_net_addr_ok(part)) {
			set_str(out, outlen, part);
			return true;
		}
	}
	return false;
}

/*
 * The text after "TOKEN:" on a line that starts with it, or NULL.  The line
 * must OPEN with the token: the echo of the command we sent contains the same
 * text and must never be read as the answer.
 */
static const char *tag_line(const char *p, const char *tok)
{
	size_t n = strlen(tok);

	if (strncmp(p, tok, n) != 0)
		return NULL;
	p += n;
	if (*p == ':')
		p++;
	return ltrim(p);
}

/*
 * The body of the first line whose text opens with `tok`, or NULL.
 *
 * Written as a line walk rather than a strstr() search because AT responses are
 * full of near-misses: the echoed command carries the same token, and some
 * answers (AT+CGDCONT?) print the header on the first line only.
 */
static const char *find_tagged(const char *resp, const char *tok)
{
	const char *line;

	for (line = resp; line && *line; ) {
		const char *nl = strchr(line, '\n');
		const char *next = nl ? nl + 1 : NULL;
		const char *txt = tag_line(ltrim(line), tok);

		if (txt)
			return txt;
		line = next;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* the dial ladder                                                      */
/* ------------------------------------------------------------------ */

const char *fm160_net_step_name(enum fm160_net_step s)
{
	switch (s) {
	case NET_STEP_IDLE:     return "idle";
	case NET_STEP_PIN:      return "waiting for SIM";
	case NET_STEP_REG:      return "waiting for registration";
	case NET_STEP_APN:      return "setting the PDP context";
	case NET_STEP_ACTIVATE: return "activating";
	case NET_STEP_IP:       return "waiting for an address";
	case NET_STEP_UP:       return "connected";
	case NET_STEP_BUSY:     return "busy";
	case NET_STEP_FAILED:   return "failed";
	case NET_STEP_DOWN:     return "disconnected";
	}
	return "unknown";
}

/* ------------------------------------------------------------------ */
/* verbs                                                                */
/* ------------------------------------------------------------------ */

const char *fm160_net_verb_name(enum fm160_net_verb v)
{
	return v == NET_VERB_GTRNDIS ? "GTRNDIS" : "GTWWAN";
}

int fm160_net_probe_command(char *out, size_t outlen, enum fm160_net_verb v, bool caps)
{
	/*
	 * Two different command shapes, not one shape with an optional suffix:
	 * "AT+GTWWAN=?" asks what the command can do, "AT+GTWWAN?" asks what it
	 * currently is.  Building the read form as "AT+GTWWAN=" (which is what a
	 * single format string with an empty suffix produces) is a syntax error
	 * the modem answers with ERROR - i.e. it would look like "the verb is
	 * unsupported" and send the probe down the wrong branch.
	 */
	int n = snprintf(out, outlen, caps ? "AT+%s=?" : "AT+%s?",
			 fm160_net_verb_name(v));

	if (n < 0 || (size_t)n >= outlen)
		return -1;
	return n;
}

int fm160_net_activate_command(char *out, size_t outlen, enum fm160_net_verb v,
			       int cid, bool up)
{
	int n;

	if (!fm160_net_cid_ok(cid))
		return -1;
	/* The vendor's own wording for teardown is AT+GTWWAN=0,<cid>, and the
	 * dial-up document is explicit that pulling the cable or powering the
	 * module down does NOT count as disconnecting. */
	n = snprintf(out, outlen, "AT+%s=%d,%d", fm160_net_verb_name(v),
		     up ? 1 : 0, cid);
	if (n < 0 || (size_t)n >= outlen)
		return -1;
	return n;
}

/* ------------------------------------------------------------------ */
/* PDP context                                                          */
/* ------------------------------------------------------------------ */

bool fm160_net_pdp_parse(const char *s, enum fm160_net_pdp *out)
{
	static const struct {
		const char *name;
		enum fm160_net_pdp v;
	} tab[] = {
		{ "IP",     NET_PDP_IP },
		{ "IPV6",   NET_PDP_IPV6 },
		{ "IPV4V6", NET_PDP_IPV4V6 },
	};
	char buf[16];
	size_t n;
	int t;

	if (!s)
		return false;

	/*
	 * Trimmed on BOTH sides.  The first version only skipped leading spaces,
	 * which passed its test until the test tried " IPV6 " - and a parser that
	 * trims one side is the kind of thing that works until a firmware revision
	 * moves the space to the other side.
	 */
	s = ltrim(s);
	n = strcspn(s, " \t\r\n");
	if (n == 0 || n >= sizeof(buf))
		return false;
	memcpy(buf, s, n);
	buf[n] = '\0';

	for (t = 0; t < 3; t++)
		if (!strcasecmp(buf, tab[t].name)) {
			if (out)
				*out = tab[t].v;
			return true;
		}
	return false;
}

const char *fm160_net_pdp_name(enum fm160_net_pdp p)
{
	switch (p) {
	case NET_PDP_IPV6:   return "IPV6";
	case NET_PDP_IPV4V6: return "IPV4V6";
	case NET_PDP_IP:
	default:             return "IP";
	}
}

bool fm160_net_cid_ok(int cid)
{
	return cid >= 1 && cid <= FM160_NET_CID_LIMIT;
}

/*
 * Characters an APN may contain: the dot-separated label syntax of TS 23.003
 * plus the hyphen and underscore real carriers use.  Everything else is
 * refused - notably '"', ',' and ';', each of which would end the quoted
 * argument early and turn the rest of the APN into further AT+CGDCONT fields.
 */
bool fm160_net_apn_ok(const char *apn)
{
	size_t i;
	size_t n;

	if (!apn)
		return false;
	n = strlen(apn);
	if (n == 0 || n > FM160_NET_APN_MAX - 1)
		return false;
	/* A leading or trailing dot is not a valid label, and "." or ".." as a
	 * whole APN is a well-known way to make a modem dial a default context
	 * silently.  Refuse rather than pass it on. */
	if (apn[0] == '.' || apn[n - 1] == '.')
		return false;

	for (i = 0; i < n; i++) {
		int c = (unsigned char)apn[i];

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '.' || c == '-' ||
		    c == '_')
			continue;
		return false;
	}
	return true;
}

int fm160_net_apn_command(char *out, size_t outlen, int cid,
			  enum fm160_net_pdp pdp, const char *apn)
{
	int n;

	if (!fm160_net_cid_ok(cid))
		return -1;
	if (!fm160_net_apn_ok(apn))
		return -1;
	n = snprintf(out, outlen, "AT+CGDCONT=%d,\"%s\",\"%s\"",
		     cid, fm160_net_pdp_name(pdp), apn);
	if (n < 0 || (size_t)n >= outlen)
		return -1;
	return n;
}

/* ------------------------------------------------------------------ */
/* parsers                                                              */
/* ------------------------------------------------------------------ */

bool fm160_net_addr_ok(const char *s)
{
	const char *p;

	if (!s || !*s)
		return false;

	/*
	 * IPv6-shaped: hex groups and colons.  A full RFC 4291 validator is not
	 * the job here - the job is to reject "pdns", "IP" and "", which is what
	 * actually turns up in a +GTWWAN answer next to a real address.  So the
	 * rules are the cheap ones that separate an address from a label: at
	 * least two colons (a single colon needs at least one "::" to be legal),
	 * at most one "::" run, and no group longer than four hex digits.
	 */
	if (strchr(s, ':')) {
		int colons = 0, run = 0, best = 0, group = 0;

		for (p = s; *p; p++) {
			if (*p == ':') {
				colons++;
				run++;
				if (run > best)
					best = run;
				group = 0;
				continue;
			}
			run = 0;
			if (!((*p >= '0' && *p <= '9') ||
			      (*p >= 'a' && *p <= 'f') ||
			      (*p >= 'A' && *p <= 'F')))
				return false;
			if (++group > 4)
				return false;
		}
		return colons >= 2 && best <= 2;
	}

	/* IPv4: exactly four decimal octets, 0-255. */
	{
		int octet = 0, dots = 0, digits = 0, first = 0;

		for (p = s; *p; p++) {
			if (*p == '.') {
				if (digits == 0)
					return false;
				dots++;
				octet = 0;
				digits = 0;
				continue;
			}
			if (*p < '0' || *p > '9')
				return false;
			if (digits == 0)
				first = *p - '0';
			octet = octet * 10 + (*p - '0');
			digits++;
			/* "010" is octal to some readers and a typo to others;
			 * refuse the padded form rather than choose. */
			if (digits > 3 || octet > 255 || (digits > 1 && first == 0))
				return false;
		}
		return digits > 0 && dots == 3;
	}
}

bool fm160_net_parse_wwan(const char *resp, struct fm160_net_wwan *st)
{
	const char *body;
	int i;

	if (!st)
		return false;
	memset(st, 0, sizeof(*st));
	st->active = -1;
	st->cid = -1;

	/*
	 * ⚠️ BOTH verb families are accepted here, and that is a measurement
	 * rather than a precaution.  On the FM160 in this project, the ECM
	 * profile (33) answers "+GTWWAN: 1,1," after AT+GTWWAN=1,1 is REFUSED,
	 * while the QMI profile (32) refuses +GTWWAN outright and answers
	 * +GTRNDIS with a real address.  A parser that only knew the +GTWWAN
	 * tag could therefore never report a link as up in one of the two
	 * comps, whatever the dialer did - the read-back would look like an
	 * unparseable answer and the ladder would time out on a link that is
	 * actually carrying traffic.
	 *
	 * The two answers have the same shape (<cid>,<active>,["pdp",dnss...]),
	 * which is what makes one parser sufficient; the capability answer
	 * ("+GTRNDIS: (0,1),(1-23)") is rejected by the "must start with a
	 * digit" check below, so accepting the second tag adds no ambiguity.
	 */
	body = find_tagged(resp, "+GTWWAN");
	if (!body)
		body = find_tagged(resp, "+GTRNDIS");
	if (!body || !(*body == '0' || (*body >= '0' && *body <= '9')))
		return false;

	if (split_csv(body, st->raw, 6, &st->raw_n) < 0 || st->raw_n == 0) {
		memset(st, 0, sizeof(*st));
		st->active = -1;
		st->cid = -1;
		return false;
	}

	st->valid = true;

	/* "<verb>: 0" - one field - is the documented "not active" answer, and
	 * the FM160 gives it for either tag. */
	if (st->raw_n == 1) {
		st->active = atoi(st->raw[0]);
		return true;
	}

	/*
	 * See the header: the order of the first two fields is assumed.  The
	 * manual's ?-syntax is "<state>,<cid>,<ip>,<pdns>,<sdns>" while the write
	 * syntax is "<op>,<cid>", and everything measured so far has state == cid
	 * == 1, so nothing has told the two orders apart.  It is left as it is
	 * on purpose: flipping it would silently change st->cid for every context
	 * that is not 1, and no measurement covers that case yet.
	 *
	 * ⚠️ CORRECTED 2026-09-21.  This comment used to say that an ECM profile
	 * "answers with the two integers and nothing else", because the only ECM
	 * answer this project had seen was "+GTWWAN: 1,1," - three fields with the
	 * third empty.  That was a context that was NOT up.  With the context up,
	 * profile 33 reports the address and both resolvers like any other profile
	 * (full line in addr_from_field() above).  Two consequences:
	 *
	 *   - has_addr IS reachable on ECM, so cb_ip() may require it - which is
	 *     what it does, and what net.h's "callers must not require has_addr"
	 *     note would have contradicted;
	 *   - the address arrives as an IPv4/IPv6 PAIR inside one quoted field,
	 *     so the scan below must split before it validates (see
	 *     addr_from_field()).
	 */
	st->cid = atoi(st->raw[0]);
	st->active = atoi(st->raw[1]);
	/*
	 * The PDP type is stored in its CANONICAL spelling, not verbatim: it comes
	 * out of a 64-byte field and goes into an 8-byte one, and a copy that
	 * truncates would leave "IPV4V6" as "IPV4V6" but "IPV4V6X" as "IPV4V6X" -
	 * i.e. a value the rest of the daemon would compare against nothing.
	 * Passing it through fm160_net_pdp_parse() first makes the length a
	 * property of the set, not of the modem.
	 */
	if (st->raw_n > 2) {
		enum fm160_net_pdp p;

		if (fm160_net_pdp_parse(st->raw[2], &p))
			set_str(st->pdp, sizeof(st->pdp), fm160_net_pdp_name(p));
	}
	/*
	 * Same "v4,v6 in one quoted field" shape as the address, so the same
	 * split: "218.2.2.2,240e:5a::6666" is one field holding two resolvers.
	 * The ECM path takes its resolver from the module's DHCP server over the
	 * netdev (fm160.sh step 4), so these two are reported rather than used -
	 * but reporting a comma-joined pair as a single DNS server is precisely
	 * the kind of thing the AT page exists to show correctly.
	 */
	if (st->raw_n > 3)
		addr_from_field(st->raw[3], st->dns1, sizeof(st->dns1));
	if (st->raw_n > 4)
		addr_from_field(st->raw[4], st->dns2, sizeof(st->dns2));

	/*
	 * Where the address sits is not announced, so rather than guess a
	 * column, take the first field that CARRIES one.  Field 0 and 1 are the
	 * two integers, so the scan starts at 2.
	 *
	 * Each field is passed through addr_from_field() rather than straight
	 * into fm160_net_addr_ok(), because a field is allowed to hold the
	 * module's IPv4/IPv6 pair ("10.179.143.75,240e:400:...").  Validating
	 * the field as a whole finds no address in exactly the case the dialer
	 * needs one - see addr_from_field().
	 */
	for (i = 2; i < st->raw_n; i++) {
		if (addr_from_field(st->raw[i], st->addr, sizeof(st->addr))) {
			st->has_addr = true;
			break;
		}
	}
	return true;
}

int fm160_net_parse_cgdcont(const char *resp,
			    struct fm160_net_cgdcont *out, int max)
{
	const char *line;
	int n = 0;
	bool header_seen = false;
	bool done = false;

	if (!resp || !out || max <= 0)
		return 0;

	for (line = resp; line && *line && !done; ) {
		const char *nl = strchr(line, '\n');
		const char *next = nl ? nl + 1 : NULL;
		const char *txt = tag_line(ltrim(line), "+CGDCONT");

		if (!txt) {
			/*
			 * A continuation line: the modem prints the header once
			 * and then the remaining contexts bare.  Only accepted
			 * after a header, so an unrelated numeric line elsewhere
			 * in the response cannot be mistaken for a context.
			 */
			txt = ltrim(line);
			if (!header_seen || (*txt != '"' &&
					     !(*txt >= '0' && *txt <= '9')))
				txt = NULL;
		} else {
			header_seen = true;
		}

		if (txt) {
			char f[6][FM160_NET_ADDR_MAX];
			int seen = 0;
			int got = split_csv(txt, f, 6, &seen);

			if (got < 0) {
				/* A malformed line does not invalidate the ones
				 * already read; it just is not one. */
				line = next;
				continue;
			}
			if (seen >= 3 && n < max) {
				enum fm160_net_pdp p;

				out[n].cid = atoi(f[0]);
				/* Canonical spelling, not a verbatim copy: see the
				 * note in fm160_net_parse_wwan(). */
				out[n].pdp[0] = '\0';
				if (fm160_net_pdp_parse(f[1], &p))
					set_str(out[n].pdp, sizeof(out[n].pdp),
						fm160_net_pdp_name(p));
				set_str(out[n].apn, sizeof(out[n].apn), f[2]);
				n++;
				if (n >= max)
					done = true;
			}
		}
		line = next;
	}
	return n;
}

int fm160_net_parse_cpin(const char *resp)
{
	const char *txt = find_tagged(resp, "+CPIN");
	char f[2][FM160_NET_ADDR_MAX];
	int seen = 0;

	if (!txt)
		return -1;
	if (split_csv(txt, f, 2, &seen) < 0 || seen < 1)
		return 0;
	return !strcasecmp(f[0], "READY");
}

int fm160_net_parse_cgact(const char *resp, int cid)
{
	const char *line;

	if (!resp)
		return -1;
	for (line = resp; line && *line; ) {
		const char *nl = strchr(line, '\n');
		const char *next = nl ? nl + 1 : NULL;
		const char *txt = tag_line(ltrim(line), "+CGACT");

		if (txt) {
			char f[3][FM160_NET_ADDR_MAX];
			int seen = 0;

			if (split_csv(txt, f, 3, &seen) >= 2 && seen >= 2 &&
			    atoi(f[0]) == cid)
				return atoi(f[1]);
		}
		line = next;
	}
	return -1;
}

int fm160_net_parse_reg(const char *resp, const char *token)
{
	char pat[16];
	const char *txt;
	char f[4][FM160_NET_ADDR_MAX];
	int seen = 0;

	if (!token)
		return FM160_NET_REG_UNKNOWN;
	snprintf(pat, sizeof(pat), "+%s", token);

	txt = find_tagged(resp, pat);
	if (!txt)
		return FM160_NET_REG_UNKNOWN;
	if (split_csv(txt, f, 4, &seen) < 0 || seen == 0)
		return FM160_NET_REG_UNKNOWN;
	/* "+CREG: 1" is the compact form and means stat 1; "+CREG: 0,1"
	 * carries <n> first. */
	return atoi(seen >= 2 ? f[1] : f[0]);
}

/* ------------------------------------------------------------------ */
/* the reconnect ladder                                                 */
/* ------------------------------------------------------------------ */

static const int ladder_s[FM160_NET_LADDER_MAX] = { 0, 5, 15, 60, 300 };

int fm160_net_ladder_len(void)
{
	return FM160_NET_LADDER_MAX;
}

int fm160_net_backoff_s(int attempt)
{
	if (attempt < 0)
		attempt = 0;
	if (attempt >= FM160_NET_LADDER_MAX)
		attempt = FM160_NET_LADDER_MAX - 1;
	return ladder_s[attempt];
}

int fm160_net_backoff_ms(int attempt)
{
	return fm160_net_backoff_s(attempt) * 1000;
}

bool fm160_net_reset_allowed(int resets_in_window)
{
	return resets_in_window < FM160_NET_RESET_LIMIT;
}

const char *fm160_net_reset_refusal(void)
{
	return "too many module resets in the last 24 h - automatic recovery is "
	       "stopped so the fault stays visible; check the SIM and the antenna";
}
