/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * pdu.c - SMS PDU encoding and decoding.
 *
 * Everything here is pure byte transformation: no modem, no ubus, no clock.
 * That is deliberate.  The parts of M3 that can be proven without a SIM are
 * exactly these, and they are the parts where a mistake is invisible - a
 * wrongly packed septet still decodes to *a* string, just not the one that was
 * sent.  The host-side test compiles this file whole.
 *
 * References: 3GPP TS 23.040 (PDU formats) and TS 23.038 (alphabet, DCS).
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdu.h"

/* ------------------------------------------------------------------ */
/* the GSM 7-bit alphabet                                               */
/* ------------------------------------------------------------------ */

/*
 * TS 23.038 §6.2.1, the default alphabet.  Index = septet value, value =
 * Unicode code point.  0xFFFF marks the escape slot (0x1B), which on its own
 * means nothing and only takes meaning with the next septet.
 *
 * Written out in full rather than derived: the table is the specification, and
 * a table computed by a rule would be a rule I invented.
 */
static const uint16_t gsm7_default[128] = {
	0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC, /* @ £ $ ¥ è é ù ì */
	0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5, /* ò Ç LF Ø ø CR Å å */
	0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8, /* Δ _ Φ Γ Λ Ω Π Ψ */
	0x03A3, 0x0398, 0x039E, 0xFFFF, 0x00C6, 0x00E6, 0x00DF, 0x00C9, /* Σ Θ Ξ ESC Æ æ ß É */
	0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027, /*   ! " # ¤ % & ' */
	0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F, /* ( ) * + , - . / */
	0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037, /* 0 1 2 3 4 5 6 7 */
	0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, /* 8 9 : ; < = > ? */
	0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047, /* ¡ A B C D E F G */
	0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F, /* H I J K L M N O */
	0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, /* P Q R S T U V W */
	0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7, /* X Y Z Ä Ö Ñ Ü § */
	0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, /* ¿ a b c d e f g */
	0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, /* h i j k l m n o */
	0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077, /* p q r s t u v w */
	0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0, /* x y z ä ö ñ ü à */
};

/*
 * TS 23.038 §6.2.1.1, the single-shift extensions.  Each of these costs TWO
 * septets (an ESC followed by the listed value), which is why the encoder
 * counts septets rather than characters.
 */
static const uint16_t gsm7_ext[128] = {
	[0x0A] = 0x000C,   /* form feed     */
	[0x14] = 0x005E,   /* ^             */
	[0x28] = 0x007B,   /* {             */
	[0x29] = 0x007D,   /* }             */
	[0x2F] = 0x005C,   /* backslash     */
	[0x3C] = 0x005B,   /* [             */
	[0x3D] = 0x007E,   /* ~             */
	[0x3E] = 0x005D,   /* ]             */
	[0x40] = 0x007C,   /* |             */
	[0x65] = 0x20AC,   /* euro sign     */
};

#define GSM7_ESC 0x1B
#define GSM7_EXT_FLAG 0x1B00

int sms_gsm7_from_ucs(uint32_t cp)
{
	int i;

	for (i = 0; i < 128; i++) {
		if (i == GSM7_ESC)
			continue;
		if (gsm7_default[i] == cp)
			return i;
	}
	for (i = 0; i < 128; i++) {
		if (gsm7_ext[i] && gsm7_ext[i] == cp)
			return GSM7_EXT_FLAG | i;
	}
	return -1;
}

uint32_t sms_gsm7_to_ucs(int septet, bool ext)
{
	septet &= 0x7F;
	if (ext)
		return gsm7_ext[septet] ? gsm7_ext[septet] : 0xFFFD;
	if (septet == GSM7_ESC)
		return 0xFFFD;   /* a lone escape is not a character */
	return gsm7_default[septet];
}

/* ------------------------------------------------------------------ */
/* small helpers                                                        */
/* ------------------------------------------------------------------ */

static void seterr(char *err, size_t errlen, const char *msg)
{
	if (err && errlen)
		snprintf(err, errlen, "%s", msg);
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * Decode ASCII hex into octets, ignoring spaces, quotes and the separators a
 * hand-typed PDU usually carries.  Returns the octet count or -EINVAL.
 */
static int parse_hex(const char *in, uint8_t *out, int outmax)
{
	int n = 0, hi = -1;

	if (!in)
		return -EINVAL;
	for (; *in; in++) {
		int v;

		if (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n' ||
		    *in == '"' || *in == ',' || *in == ':')
			continue;
		v = hexval((unsigned char)*in);
		if (v < 0)
			return -EINVAL;
		if (hi < 0) {
			hi = v;
			continue;
		}
		if (n >= outmax)
			return -EINVAL;
		out[n++] = (uint8_t)((hi << 4) | v);
		hi = -1;
	}
	if (hi >= 0)          /* an odd number of digits */
		return -EINVAL;
	return n;
}

static char *put_hex(char *dst, const uint8_t *src, int n)
{
	static const char d[] = "0123456789ABCDEF";
	int i;

	for (i = 0; i < n; i++) {
		*dst++ = d[src[i] >> 4];
		*dst++ = d[src[i] & 0x0F];
	}
	*dst = '\0';
	return dst;
}

/* UTF-8 → code point.  Returns bytes consumed, or 0 at the end, or -1 on a
 * malformed sequence.  Deliberately strict: a malformed sequence is reported
 * rather than replaced, because silently substituting U+FFFD in a message body
 * would hide a real encoding fault. */
static int utf8_next(const char *s, size_t *i, size_t len, uint32_t *cp)
{
	unsigned char c;
	uint32_t v;
	int extra, k;

	if (*i >= len)
		return 0;
	c = (unsigned char)s[*i];

	if (c < 0x80) {
		*cp = c;
		(*i)++;
		return 1;
	}
	if ((c & 0xE0) == 0xC0) {
		v = c & 0x1F;
		extra = 1;
	} else if ((c & 0xF0) == 0xE0) {
		v = c & 0x0F;
		extra = 2;
	} else if ((c & 0xF8) == 0xF0) {
		v = c & 0x07;
		extra = 3;
	} else {
		return -1;
	}
	if (*i + (size_t)extra >= len)
		return -1;
	for (k = 1; k <= extra; k++) {
		unsigned char cc = (unsigned char)s[*i + k];

		if ((cc & 0xC0) != 0x80)
			return -1;
		v = (v << 6) | (cc & 0x3F);
	}
	/* Reject overlong forms and surrogates: they would round-trip into
	 * something different from what was sent. */
	if ((extra == 1 && v < 0x80) || (extra == 2 && v < 0x800) ||
	    (extra == 3 && v < 0x10000) || v > 0x10FFFF ||
	    (v >= 0xD800 && v <= 0xDFFF))
		return -1;
	*cp = v;
	*i += (size_t)extra + 1;
	return extra + 1;
}

static int utf8_put(char *out, size_t outlen, size_t *pos, uint32_t cp)
{
	if (cp < 0x80) {
		if (*pos + 1 >= outlen)
			return -1;
		out[(*pos)++] = (char)cp;
	} else if (cp < 0x800) {
		if (*pos + 2 >= outlen)
			return -1;
		out[(*pos)++] = (char)(0xC0 | (cp >> 6));
		out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		if (*pos + 3 >= outlen)
			return -1;
		out[(*pos)++] = (char)(0xE0 | (cp >> 12));
		out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
	} else {
		if (*pos + 4 >= outlen)
			return -1;
		out[(*pos)++] = (char)(0xF0 | (cp >> 18));
		out[(*pos)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
	}
	out[*pos] = '\0';
	return 0;
}

/*
 * Step over exactly one character, whatever the text contains.
 *
 * utf8_next() is strict and refuses to move on a malformed byte; used directly
 * in a loop that is an infinite loop waiting for a bad character to arrive.
 * Every loop in this file goes through here instead, so the byte cursor always
 * advances and a corrupt body degrades into replacement characters rather than
 * hanging the daemon.
 *
 * Returns bytes consumed, or 0 at end of string.
 */
static int text_step(const char *text, size_t len, size_t *byte, uint32_t *cp)
{
	size_t before = *byte;
	int got;

	if (*byte >= len) {
		*cp = 0;
		return 0;
	}
	got = utf8_next(text, byte, len, cp);
	if (got > 0)
		return got;
	*byte = before + 1;
	*cp = 0xFFFD;
	return 1;
}

/* Skip `count` characters.  Returns the number actually skipped. */
static int text_skip(const char *text, size_t len, size_t *byte, int count)
{
	uint32_t cp;
	int i;

	for (i = 0; i < count; i++) {
		if (!text_step(text, len, byte, &cp))
			break;
	}
	return i;
}

/* Semi-octet BCD, first digit in the LOW nibble (TS 23.040 §9.1.2.3). */
static void bcd_encode(const char *digits, int ndigits, uint8_t *out)
{
	int i, octets = (ndigits + 1) / 2;

	for (i = 0; i < octets; i++) {
		int lo = digits[i * 2] - '0';
		int hi = (i * 2 + 1 < ndigits) ? digits[i * 2 + 1] - '0' : 0x0F;

		out[i] = (uint8_t)((hi << 4) | lo);
	}
}

static int bcd_decode(const uint8_t *in, int octets, int ndigits,
		      char *out, size_t outlen)
{
	int i, n = 0;

	for (i = 0; i < octets && n < ndigits; i++) {
		int lo = in[i] & 0x0F;
		int hi = (in[i] >> 4) & 0x0F;

		if (lo > 9 || (n + 1 < ndigits && hi > 9))
			return -EINVAL;
		if ((size_t)n + 1 >= outlen)
			return -EINVAL;
		out[n++] = (char)('0' + lo);
		if (n < ndigits) {
			if (hi > 9)
				return -EINVAL;
			out[n++] = (char)('0' + hi);
		}
	}
	out[n] = '\0';
	return n == ndigits ? 0 : -EINVAL;
}

int sms_number_clean(const char *in, char *out, size_t outlen,
		     bool *international)
{
	size_t n = 0;
	bool intl = false;
	const char *p;

	if (!in || !outlen)
		return -EINVAL;
	out[0] = '\0';
	for (p = in; *p; p++) {
		if (*p == '+') {
			/* Only a leading '+' means "international".  A stray one
			 * later is almost certainly formatting, so it is dropped
			 * too - but it does not promote the number. */
			if (!n && !intl) {
				intl = true;
				continue;
			}
			continue;
		}
		if (*p >= '0' && *p <= '9') {
			if (n + 2 > outlen)
				return -EINVAL;
			out[n++] = *p;
			continue;
		}
		if (*p == ' ' || *p == '-' || *p == '(' || *p == ')' ||
		    *p == '.' || *p == '/')
			continue;
		return -EINVAL;   /* not formatting, so not a phone number */
	}
	out[n] = '\0';
	if (international)
		*international = intl;
	return n ? 0 : -EINVAL;
}

bool sms_number_plausible(const char *d)
{
	size_t n;

	if (!d)
		return false;
	n = strlen(d);
	if (n < 2 || n > 20)
		return false;
	for (; *d; d++)
		if (*d < '0' || *d > '9')
			return false;
	return true;
}

/* ------------------------------------------------------------------ */
/* alphabet selection                                                   */
/* ------------------------------------------------------------------ */

struct text_scan {
	int  chars;
	int  septets;      /* GSM7 only */
	bool gsm7_ok;
	uint32_t first_bad;
};

static void scan_text(const char *text, struct text_scan *sc)
{
	size_t i = 0, len = strlen(text);

	memset(sc, 0, sizeof(*sc));
	sc->gsm7_ok = true;
	while (i < len) {
		uint32_t cp;
		int n = text_step(text, len, &i, &cp);

		(void)n;
		sc->chars++;
		if (sc->gsm7_ok) {
			int s = sms_gsm7_from_ucs(cp);

			if (s < 0) {
				sc->gsm7_ok = false;
				if (!sc->first_bad)
					sc->first_bad = cp;
			} else if ((s & GSM7_EXT_FLAG) == GSM7_EXT_FLAG) {
				sc->septets += 2;   /* ESC + the value */
			} else {
				sc->septets += 1;
			}
		}
	}
}

/*
 * TS 23.038 §4: the alphabet is selected by bits 3-2 of the DCS octet.  The
 * three higher groups (message-waiting, 0x40 compressed, 0xC0) reuse the same
 * two bits, which is why this reads the bits rather than matching whole
 * octets - 0xF0 (auto-delete, 7-bit) and 0xF8 (auto-delete, UCS2) are both
 * common and neither equals 0x00 or 0x08.
 */
static int dcs_encoding(int dcs)
{
	switch ((dcs >> 2) & 3) {
	case 0:
		return SMS_ENC_GSM7;
	case 1:
		return SMS_ENC_8BIT;
	case 2:
		return SMS_ENC_UCS2;
	default:
		return SMS_ENC_GSM7;   /* reserved; 7-bit is the safe read */
	}
}

static int dcs_encode_for(int enc)
{
	switch (enc) {
	case SMS_ENC_UCS2:
		return 0x08;
	case SMS_ENC_8BIT:
		return 0x04;
	default:
		return 0x00;
	}
}

/* ------------------------------------------------------------------ */
/* GSM 7-bit packing                                                    */
/* ------------------------------------------------------------------ */

/*
 * A bit writer, LSB-first within each octet, which is the order TS 23.040 uses
 * for a septet stream.  Going through a bit stream rather than the usual
 * "shift the septet into the current octet" loop is what makes the user data
 * header fall out for free: the header is eight whole octets at the front of
 * the same stream, and the fill bits that align it to a septet boundary are
 * simply a skip.
 */
struct bitw {
	uint8_t buf[SMS_PDU_UD_MAX + 16];
	int bitpos;
};

static void bw_init(struct bitw *w)
{
	memset(w->buf, 0, sizeof(w->buf));
	w->bitpos = 0;
}

static void bw_bits(struct bitw *w, uint32_t val, int nbits)
{
	int i;

	for (i = 0; i < nbits; i++) {
		if ((val >> i) & 1)
			w->buf[w->bitpos >> 3] |= (uint8_t)(1 << (w->bitpos & 7));
		w->bitpos++;
	}
}

static void bw_septets(struct bitw *w, const uint8_t *s, int n)
{
	int i;

	for (i = 0; i < n; i++)
		bw_bits(w, s[i], 7);
}

static int bw_octets(const struct bitw *w)
{
	return (w->bitpos + 7) / 8;
}

/* Fill bits that align a header of `octets` octets to a septet boundary. */
static int udh_fill_bits(int octets)
{
	int bits = octets * 8;

	return (7 - (bits % 7)) % 7;
}

static int udh_septets(int octets)
{
	return (octets * 8 + udh_fill_bits(octets)) / 7;
}

static int septets_to_octets(int septets)
{
	return (septets * 7 + 7) / 8;
}

/* ------------------------------------------------------------------ */
/* encoding                                                             */
/* ------------------------------------------------------------------ */

struct seg_plan {
	int offset;        /* character index where this segment starts */
	int chars;         /* characters in it                           */
};

/*
 * Split the text into at most max_segments pieces.  GSM7 splits on septet
 * boundaries and refuses to cut an escape pair in half; UCS2 splits on
 * character boundaries.
 */
static int plan_segments(const char *text, int enc, int septets, int chars,
			 int max_segments, struct seg_plan *plan, int *out_n)
{
	int i = 0, n = 0;
	size_t byte = 0, len = strlen(text);
	int per = (enc == SMS_ENC_GSM7) ? 153 : 67;

	if (enc == SMS_ENC_GSM7 ? (septets <= 160) : (chars <= 70)) {
		plan[0].offset = 0;
		plan[0].chars = chars;
		*out_n = 1;
		return 1;               /* one segment, no header */
	}

	while (byte < len) {
		int used_septets = 0, used_chars = 0;

		if (n >= max_segments)
			return -E2BIG;
		plan[n].offset = i;
		while (byte < len) {
			uint32_t cp;
			size_t before = byte;
			int cost;

			if (!text_step(text, len, &byte, &cp))
				break;
			if (enc == SMS_ENC_GSM7) {
				int s = sms_gsm7_from_ucs(cp);

				cost = (s >= 0 && (s & GSM7_EXT_FLAG) == GSM7_EXT_FLAG)
				       ? 2 : 1;
			} else {
				cost = 1;
			}
			if (used_septets + cost > per) {
				/* The character does not fit.  Put it back: the
				 * byte cursor has already moved past it, and
				 * leaving it moved means the next segment starts
				 * one character late - every part after the first
				 * silently loses its first character and the last
				 * part loses its last. */
				byte = before;
				break;
			}
			used_septets += cost;
			used_chars++;
			i++;
		}
		plan[n].chars = used_chars;
		n++;
		if (used_chars == 0)
			return -E2BIG;   /* something made no progress */
	}
	*out_n = n;
	return n;
}

int sms_pdu_plan(const char *text, int max_segments, int *encoding, int *chars)
{
	struct text_scan sc;

	if (!text || !*text)
		return -EINVAL;
	scan_text(text, &sc);
	if (encoding)
		*encoding = sc.gsm7_ok ? SMS_ENC_GSM7 : SMS_ENC_UCS2;
	if (chars)
		*chars = sc.chars;

	if (sc.gsm7_ok)
		return sc.septets <= 160 ? 1 :
		       (sc.septets + 152) / 153;
	return sc.chars <= 70 ? 1 : (sc.chars + 66) / 67;
}

int sms_pdu_encode(const char *number, const char *text,
		   int max_segments, int ref, bool ref16,
		   struct sms_pdu_out *out, char *err, size_t errlen)
{
	static const uint8_t smsc[1] = { 0x00 };   /* "use the card's SMSC" */
	char digits[SMS_PDU_ADDR_MAX + 1];
	bool intl = false;
	struct text_scan sc;
	struct seg_plan plan[SMS_PDU_SEG_MAX];
	int enc, nseg, i, j;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));

	if (!number || !text || !*text) {
		seterr(err, errlen, "number and text are both required");
		return -EINVAL;
	}
	if (sms_number_clean(number, digits, sizeof(digits), &intl)) {
		seterr(err, errlen, "not a usable phone number");
		return -EINVAL;
	}
	if (!sms_number_plausible(digits)) {
		seterr(err, errlen, "number must be 2..20 digits");
		return -EINVAL;
	}
	if (max_segments < 1)
		max_segments = 1;
	if (max_segments > SMS_PDU_SEG_MAX)
		max_segments = SMS_PDU_SEG_MAX;

	scan_text(text, &sc);
	enc = sc.gsm7_ok ? SMS_ENC_GSM7 : SMS_ENC_UCS2;
	if (!sc.gsm7_ok && !sc.first_bad) {
		seterr(err, errlen, "text is not valid UTF-8");
		return -EINVAL;
	}

	i = plan_segments(text, enc, sc.septets, sc.chars, max_segments,
			  plan, &nseg);
	if (i < 0) {
		seterr(err, errlen, "too long: would need more than the allowed segments");
		return i;
	}

	out->encoding = enc;
	out->chars = sc.chars;
	out->septets = sc.gsm7_ok ? sc.septets : 0;
	out->segments = nseg;

	for (j = 0; j < nseg; j++) {
		struct sms_segment *sg = &out->seg[j];
		uint8_t tpdu[SMS_PDU_UD_MAX + 32];
		struct bitw w;
		int p = 0, udh_oct = 0, udl = 0;
		bool concat = nseg > 1;

		if (concat) {
			/* Header octets INCLUDING the UDHL octet itself:
			 *   UDHL(1) + IEI(1) + IEDL(1) + ref + total + seq
			 * which is 6 with an 8-bit reference and 7 with a 16-bit
			 * one.  The IEI/IEDL octets are not optional. */
			udh_oct = ref16 ? 7 : 6;
		}

		/* --- TP-DA ------------------------------------------------- */
		tpdu[p++] = (uint8_t)(0x01 | (concat ? 0x40 : 0x00));
		tpdu[p++] = 0x00;              /* TP-MR: let the network sort it */
		tpdu[p++] = (uint8_t)strlen(digits);
		tpdu[p++] = (uint8_t)(intl ? 0x91 : 0x81);
		bcd_encode(digits, (int)strlen(digits), tpdu + p);
		p += ((int)strlen(digits) + 1) / 2;
		tpdu[p++] = 0x00;              /* TP-PID */
		tpdu[p++] = (uint8_t)dcs_encode_for(enc);

		/* --- TP-UD ------------------------------------------------- */
		bw_init(&w);
		if (concat) {
			uint8_t hdr[8];
			int hl = ref16 ? 6 : 5;

			hdr[0] = (uint8_t)hl;              /* UDHL = octets after it */
			hdr[1] = ref16 ? 0x08 : 0x00;      /* concat IEI */
			hdr[2] = ref16 ? 0x04 : 0x03;      /* IE data length */
			if (ref16) {
				int r = ref ? ref : 1;

				hdr[3] = (uint8_t)(r >> 8);
				hdr[4] = (uint8_t)(r & 0xFF);
				hdr[5] = (uint8_t)nseg;
				hdr[6] = (uint8_t)(j + 1);
			} else {
				int r = ref ? ref : 1;

				hdr[3] = (uint8_t)(r & 0xFF);
				hdr[4] = (uint8_t)nseg;
				hdr[5] = (uint8_t)(j + 1);
			}
			for (i = 0; i <= hl; i++)
				bw_bits(&w, hdr[i], 8);
			/*
			 * The fill bits exist for ONE reason: a 7-bit septet
			 * stream has no byte boundaries, so a header of whole
			 * octets does not end on a septet boundary and the
			 * first character would otherwise start mid-septet.
			 *
			 * In a byte-oriented alphabet there is nothing to
			 * align - the header is already a whole number of
			 * octets and the data follows immediately.  Adding the
			 * fill there shoves every UCS2 segment one bit out and
			 * makes it 141 octets instead of 140, which the 140
			 * check then rejects as "too long" for a message that
			 * fits exactly.
			 */
			if (enc == SMS_ENC_GSM7)
				bw_bits(&w, 0, udh_fill_bits(hl + 1));
			out->ref = ref ? ref : 1;
			out->ref16 = ref16;
		}

		if (enc == SMS_ENC_GSM7) {
			/* A full segment is 153 septets, or 160 for the
			 * unsplit single segment - so this has to hold more
			 * than SMS_PDU_UD_MAX. */
			uint8_t sept[168];
			int ns = 0;
			size_t byte = 0, len = strlen(text);

			text_skip(text, len, &byte, plan[j].offset);
			for (i = 0; i < plan[j].chars; i++) {
				uint32_t cp;
				int s;

				if (!text_step(text, len, &byte, &cp))
					break;
				s = sms_gsm7_from_ucs(cp);
				if (s < 0)
					s = sms_gsm7_from_ucs(0x003F);   /* '?' */
				if ((s & GSM7_EXT_FLAG) == GSM7_EXT_FLAG) {
					sept[ns++] = GSM7_ESC;
					sept[ns++] = (uint8_t)(s & 0x7F);
				} else {
					sept[ns++] = (uint8_t)s;
				}
			}
			bw_septets(&w, sept, ns);
			udl = udh_septets(udh_oct) + ns;
		} else {
			size_t byte = 0, len = strlen(text);

			text_skip(text, len, &byte, plan[j].offset);
			for (i = 0; i < plan[j].chars; i++) {
				uint32_t cp;

				if (!text_step(text, len, &byte, &cp))
					break;
				if (cp > 0xFFFF)
					cp = 0xFFFD;   /* UCS2 has no astral plane */
				bw_bits(&w, (cp >> 8) & 0xFF, 8);
				bw_bits(&w, cp & 0xFF, 8);
			}
			udl = udh_oct + plan[j].chars * 2;
		}

		{
			int ud_oct = bw_octets(&w);

			if (ud_oct > SMS_PDU_UD_MAX) {
				seterr(err, errlen, "internal: segment exceeds 140 octets");
				return -E2BIG;
			}
			tpdu[p++] = (uint8_t)udl;
			memcpy(tpdu + p, w.buf, (size_t)ud_oct);
			p += ud_oct;
			sg->udl = udl;
			sg->ud_octets = ud_oct;
		}

		/* The PDU handed to the modem carries the SMSC field; the
		 * <length> argument does not. */
		{
			uint8_t full[SMS_PDU_UD_MAX + 40];

			memcpy(full, smsc, sizeof(smsc));
			memcpy(full + sizeof(smsc), tpdu, (size_t)p);
			if (1 + p >= SMS_PDU_HEX_MAX / 2) {
				seterr(err, errlen, "internal: PDU too long for the buffer");
				return -E2BIG;
			}
			put_hex(sg->hex, full, p + 1);
			sg->tpdu_len = p;
		}
	}
	return nseg;
}

/* ------------------------------------------------------------------ */
/* decoding                                                             */
/* ------------------------------------------------------------------ */

static int decode_addr(const uint8_t *b, int n, int *p, char *out,
		       size_t outlen, bool *intl)
{
	int ndig, octets, toa;

	if (*p + 1 > n)
		return -EINVAL;
	ndig = b[(*p)++];
	if (ndig == 0) {
		out[0] = '\0';
		if (intl)
			*intl = false;
		return 0;
	}
	if (ndig > SMS_PDU_ADDR_MAX)
		return -EINVAL;
	octets = (ndig + 1) / 2;
	if (*p + 1 + octets > n)
		return -EINVAL;
	toa = b[(*p)++];
	if (intl)
		*intl = (toa & 0x70) == 0x10;      /* type-of-number = international */
	if (bcd_decode(b + *p, octets, ndig, out, outlen))
		return -EINVAL;
	*p += octets;
	return 0;
}

/* Find the concatenation IE, if any, in a user data header.
 *
 * `udh` points at the header INCLUDING its own length octet, so parsing starts
 * at index 1: the first octet is the UDHL, not an information element.  Reading
 * from index 0 was a real bug - it made every header look like it began with an
 * element of type "UDHL", which matched nothing, so a concatenated message was
 * silently reported as a standalone one and every part looked like part 0. */
static void parse_udh(const uint8_t *udh, int len, struct sms_pdu_in *out)
{
	int i = 1;

	out->concat = false;
	out->ref = out->parts = out->part = 0;
	while (i + 2 <= len) {
		int iei = udh[i];
		int iel = udh[i + 1];

		if (i + 2 + iel > len)
			break;
		if (iei == 0x00 && iel == 3) {
			out->concat = true;
			out->ref = udh[i + 2];
			out->parts = udh[i + 3];
			out->part = udh[i + 4];
		} else if (iei == 0x08 && iel == 4) {
			out->concat = true;
			out->ref = (udh[i + 2] << 8) | udh[i + 3];
			out->parts = udh[i + 4];
			out->part = udh[i + 5];
		}
		i += 2 + iel;
	}
}

/*
 * Decode one semi-octet pair stored as a swapped BCD octet.
 *
 * The FIRST digit is in the LOW nibble, so the value is lo*10 + hi - the
 * opposite of reading the octet as ordinary packed BCD.  Checked against the
 * worked example in the TS 23.040 commentary everyone quotes:
 *
 *     99 20 21 50 75 03 21  ->  1999-02-12 05:57:30 GMT+3
 *
 * Reading 0x21 as "21" instead of "12" is the failure this comment exists to
 * prevent: it is off by a factor of ten and only obvious on the day above 12.
 *
 * `use_sign` applies only to the time zone, where bit 3 of the first
 * semi-octet (i.e. of the tens digit) is the sign of the offset.
 */
static int scts_pair(uint8_t o, bool *negative, bool use_sign)
{
	int lo = o & 0x0F;
	int hi = (o >> 4) & 0x0F;
	bool neg = false;

	if (use_sign) {
		neg = (lo & 0x08) != 0;
		lo &= 0x07;
	}
	if (hi > 9 || lo > 9)
		return -EINVAL;
	if (negative)
		*negative = neg;
	return lo * 10 + hi;
}

int sms_pdu_decode(const char *hex, struct sms_pdu_in *out)
{
	uint8_t b[SMS_PDU_UD_MAX + 48];
	int n, p = 0, octet0, smsc_len, udh_len = 0, enc;
	bool udhi = false;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	out->bad_octet = -1;

	n = parse_hex(hex, b, (int)sizeof(b));
	if (n < 0) {
		seterr(out->err, sizeof(out->err), "not hexadecimal");
		return -EINVAL;
	}
	if (n < 2) {
		seterr(out->err, sizeof(out->err), "too short to be a PDU");
		return -EINVAL;
	}

	/* The length octet of the SMSC counts OCTETS, while the length octet of
	 * every other address counts DIGITS.  Getting this wrong shifts the
	 * whole PDU by a variable amount and is the classic way to parse a
	 * valid message into nonsense. */
	smsc_len = b[p++];
	if (p + smsc_len > n) {
		seterr(out->err, sizeof(out->err), "SMSC length runs past the end");
		return -EINVAL;
	}
	p += smsc_len;
	if (p >= n) {
		seterr(out->err, sizeof(out->err), "no TPDU after the SMSC field");
		return -EINVAL;
	}

	octet0 = b[p++];
	out->mti = octet0 & 0x03;
	udhi = (octet0 & 0x40) != 0;

	if (out->mti == 0) {
		/* SMS-DELIVER: sender, then the SMSC's own timestamp. */
		if (decode_addr(b, n, &p, out->number, sizeof(out->number),
				&out->number_international)) {
			seterr(out->err, sizeof(out->err), "sender address is malformed");
			return -EINVAL;
		}
		if (p + 2 > n)
			goto truncated;
		out->pid = b[p++];
		out->dcs = b[p++];
		if (p + 7 > n)
			goto truncated;
		{
			bool neg = false;
			int v[7], k;

			for (k = 0; k < 7; k++) {
				v[k] = scts_pair(b[p + k], &neg, k == 6);
				if (v[k] < 0)
					goto badtime;
			}
			p += 7;
			out->year = v[0];
			out->month = v[1];
			out->day = v[2];
			out->hour = v[3];
			out->min = v[4];
			out->sec = v[5];
			out->tz_quarters = v[6];
			out->tz_negative = neg;
			out->has_scts = true;
		}
	} else if (out->mti == 1) {
		/* SMS-SUBMIT: destination, optional validity period, no stamp. */
		int vpf = (octet0 >> 3) & 0x03;

		p++;   /* TP-MR */
		if (decode_addr(b, n, &p, out->number, sizeof(out->number),
				&out->number_international)) {
			seterr(out->err, sizeof(out->err), "destination address is malformed");
			return -EINVAL;
		}
		if (p + 2 > n)
			goto truncated;
		out->pid = b[p++];
		out->dcs = b[p++];
		if (vpf == 1)
			p += 1;
		else if (vpf == 2 || vpf == 3)
			p += 7;
	} else if (out->mti == 2) {
		/* SMS-STATUS-REPORT.  Worth parsing so a report is not mistaken
		 * for an empty message: it has a recipient, a stamp and a
		 * status, just no body. */
		p++;   /* TP-MR */
		if (decode_addr(b, n, &p, out->number, sizeof(out->number),
				&out->number_international)) {
			seterr(out->err, sizeof(out->err), "report address is malformed");
			return -EINVAL;
		}
		if (p + 7 + 7 + 3 > n)
			goto truncated;
		p += 7;   /* TP-SCTS */
		p += 7;   /* TP-DT (discharge time) */
		p += 1;   /* TP-ST (status) */
		out->pid = b[p++];
		out->dcs = b[p++];
	} else {
		seterr(out->err, sizeof(out->err), "unknown TP-MTI (reserved type)");
		return -EINVAL;
	}

	if (p >= n)
		goto truncated;
	out->udl = b[p++];
	if (out->mti == 2) {
		/* A report's user data is usually absent; report it as such
		 * rather than inventing an empty body. */
		out->valid = true;
		out->text[0] = '\0';
		out->text_len = 0;
		out->encoding = dcs_encoding(out->dcs);
		seterr(out->err, sizeof(out->err), "status report, no body");
		return 0;
	}

	enc = dcs_encoding(out->dcs);
	out->encoding = enc;

	{
		int avail = n - p;
		int ud_oct;

		if (enc == SMS_ENC_GSM7) {
			ud_oct = septets_to_octets(out->udl);
			if (ud_oct > avail)
				ud_oct = avail;
		} else {
			ud_oct = out->udl;
			if (ud_oct > avail)
				ud_oct = avail;
		}
		out->ud_octets = ud_oct;

		if (udhi) {
			int hl;

			if (ud_oct < 1) {
				seterr(out->err, sizeof(out->err),
				       "header bit set but no user data header");
				return -EINVAL;
			}
			hl = b[p] + 1;
			if (hl > ud_oct)
				goto truncated;
			udh_len = hl;
			out->udh_octets = hl;
			parse_udh(b + p, hl, out);
		}

		if (enc == SMS_ENC_GSM7) {
			/* UDL counts septets, so a single unsplit segment can
			 * carry 160 of them - more than SMS_PDU_UD_MAX, which
			 * counts octets. */
			uint8_t sept[168];
			int skip = udh_len ? udh_septets(udh_len) : 0;
			int nsept = out->udl - skip;
			size_t pos = 0;
			int k;
			bool esc = false;

			if (nsept < 0)
				nsept = 0;
			if (nsept > (int)sizeof(sept))
				nsept = (int)sizeof(sept);
			for (k = 0; k < nsept; k++) {
				int bit = k * 7 + (udh_len ? udh_septets(udh_len) * 7 : 0);
				int byte = bit >> 3, off = bit & 7;
				uint32_t v;

				if (byte >= ud_oct)
					break;
				v = b[p + byte];
				if (byte + 1 < ud_oct)
					v |= (uint32_t)b[p + byte + 1] << 8;
				sept[k] = (uint8_t)((v >> off) & 0x7F);
			}
			out->text[0] = '\0';
			for (k = 0; k < nsept; k++) {
				uint32_t cp;

				if (sept[k] == GSM7_ESC) {
					esc = true;
					continue;
				}
				cp = sms_gsm7_to_ucs(sept[k], esc);
				esc = false;
				if (!cp)
					continue;
				if (utf8_put(out->text, sizeof(out->text), &pos, cp))
					break;
				out->text_len++;
			}
		} else {
			size_t pos = 0;
			int off = udh_len;
			int k;

			out->text[0] = '\0';
			for (k = off; k + 1 < ud_oct; k += 2) {
				uint32_t cp = ((uint32_t)b[p + k] << 8) | b[p + k + 1];

				/* A surrogate pair is not legal UCS2, but
				 * modules that send emoji emit one anyway;
				 * recombining costs three lines and avoids
				 * printing two replacement characters. */
				if (cp >= 0xD800 && cp <= 0xDBFF && k + 3 < ud_oct) {
					uint32_t lo = ((uint32_t)b[p + k + 2] << 8) |
						      b[p + k + 3];

					if (lo >= 0xDC00 && lo <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) +
						     (lo - 0xDC00);
						k += 2;
					}
				}
				if (cp >= 0xD800 && cp <= 0xDFFF)
					cp = 0xFFFD;
				if (utf8_put(out->text, sizeof(out->text), &pos, cp))
					break;
				out->text_len++;
			}
		}
	}

	out->valid = true;
	return 0;

truncated:
	out->bad_octet = p;
	seterr(out->err, sizeof(out->err), "PDU ends in the middle of a field");
	return -EINVAL;
badtime:
	out->bad_octet = p;
	seterr(out->err, sizeof(out->err), "time stamp is not valid semi-octet BCD");
	return -EINVAL;
}
