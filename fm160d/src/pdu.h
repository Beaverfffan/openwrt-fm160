/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * pdu.h - SMS PDU encoding and decoding (3GPP TS 23.040 / TS 23.038).
 *
 * This file deliberately includes NOTHING but the C library.  Everything in it
 * is pure transformation of bytes, with no modem, no ubus and no clock, which
 * is what lets the host-side test (23-sms-pdu-test.sh) compile this exact
 * source and exercise it against published vectors.
 *
 * Scope: SMS-SUBMIT, SMS-DELIVER and SMS-STATUS-REPORT PDUs, GSM 7-bit and
 * UCS2 user data, concatenation via the user data header.
 *
 * Two conventions worth stating once, because getting either backwards
 * produces answers that look right and are wrong:
 *
 *   - Every semi-octet field (addresses, the time stamp) is stored with the
 *     FIRST digit in the LOW nibble.  "27" is the octet 0x72, not 0x27.
 *
 *   - The <length> argument of AT+CMGS is the TPDU length in octets and
 *     EXCLUDES the SMSC field, while the PDU string handed to the modem in-
 *     cludes it.  We emit an SMSC field of a single 0x00 octet, which means
 *     "use the SMSC already configured on the card", so the two numbers differ
 *     by exactly one.
 */

#ifndef FM160_PDU_H
#define FM160_PDU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Longest body we accept, in UTF-8 bytes.  4 segments of 67 UCS2 characters is
 * 268 characters; 1024 bytes of UTF-8 is far more than that and costs nothing
 * on a device that has megabytes of RAM. */
#define SMS_PDU_TEXT_MAX   1024
/* One ASCII-hex PDU.  A 140-octet TPDU is 280 hex characters, plus the SMSC
 * octet and a NUL; 512 leaves room for the worst case without being sloppy. */
#define SMS_PDU_HEX_MAX    512
/* Digits, '+' stripped.  The longest thing a real SIM will accept. */
#define SMS_PDU_ADDR_MAX   24
#define SMS_PDU_SEG_MAX    4
/* Octets of TP-UD in one segment.  140 is the hard limit for both schemes:
 * 160 septets is 140 octets, and 70 UCS2 characters is 140 octets too. */
#define SMS_PDU_UD_MAX     140

enum sms_enc {
	SMS_ENC_GSM7 = 0,
	SMS_ENC_UCS2 = 1,
	SMS_ENC_8BIT = 2,
};

/* One TPDU, in the form AT+CMGS wants it. */
struct sms_segment {
	char hex[SMS_PDU_HEX_MAX];  /* SMSC octet + TPDU, ASCII hex        */
	int  tpdu_len;              /* the <length> for AT+CMGS            */
	int  udl;                   /* TP-UDL exactly as written           */
	int  ud_octets;             /* octets of TP-UD, UDH included       */
};

struct sms_pdu_out {
	struct sms_segment seg[SMS_PDU_SEG_MAX];
	int  segments;
	int  encoding;              /* enum sms_enc                        */
	int  ref;                   /* concatenation reference, 0 if unused */
	bool ref16;
	int  chars;                 /* characters in the source text       */
	int  septets;               /* GSM7 only: septets before splitting */
};

struct sms_pdu_in {
	bool valid;
	/* Only SMS-DELIVER carries a sender we can reply to; for a SUBMIT this
	 * holds the destination, which is still the right thing to show. */
	char number[SMS_PDU_ADDR_MAX + 1];
	bool number_international;
	char text[SMS_PDU_TEXT_MAX];
	int  text_len;              /* characters, not bytes               */
	int  encoding;              /* enum sms_enc                        */
	int  dcs;
	int  pid;
	int  mti;                   /* 0 deliver, 1 submit, 2 report       */

	/* SCTS / TP-SCTS.  Local time as stamped by the SMSC, plus the zone it
	 * was stamped in - we do not convert, because a message that says 09:31
	 * should keep saying 09:31 on the page. */
	int  year, month, day, hour, min, sec;
	int  tz_quarters;           /* units of 15 minutes                 */
	bool tz_negative;
	bool has_scts;

	/* Concatenation, when the header says so. */
	bool concat;
	int  ref, parts, part;
	int  udh_octets;

	/* Diagnostics: enough to tell "nothing to report" from "decoding gave
	 * up", which for a parser is the only distinction that matters. */
	int  udl;
	int  ud_octets;
	int  bad_octet;             /* -1, or where decoding stopped        */
	char err[64];
};

/* Encode `text` for `number`.  Splits into at most `max_segments` PDUs when the
 * body does not fit; a body that still does not fit is refused rather than
 * silently truncated.
 *
 * `ref` is the concatenation reference (1..255, or 1..65535 when ref16).  Pass
 * 0 to let the caller's choice of 1 be used; the reference actually used is
 * reported back in out->ref.
 *
 * Returns the number of segments, or a negative errno-ish code:
 *   -EINVAL  number missing or unusable, or text empty
 *   -E2BIG   text needs more than max_segments segments
 *   -ENOTSUP text contains characters that are in neither the GSM alphabet nor
 *            representable in UCS2 (astral plane)
 * `err` (may be NULL) receives a one-line explanation. */
int sms_pdu_encode(const char *number, const char *text,
		   int max_segments, int ref, bool ref16,
		   struct sms_pdu_out *out, char *err, size_t errlen);

/* Decode one PDU as returned by AT+CMGR / AT+CMGL.  Accepts the hex with or
 * without surrounding quotes and whitespace.  Returns 0 on success. */
int sms_pdu_decode(const char *hex, struct sms_pdu_in *out);

/* How the text would be encoded, and how many characters it has, without
 * building anything.  encoding is an enum sms_enc; returns the segment count,
 * or a negative code as above. */
int sms_pdu_plan(const char *text, int max_segments, int *encoding, int *chars);

/* Strip everything that is not a digit.  A leading '+' is recorded in
 * *international, then dropped.  Returns 0 on success, -EINVAL when the result
 * is empty or too long for SMS_PDU_ADDR_MAX. */
int sms_number_clean(const char *in, char *out, size_t outlen,
		     bool *international);

/* A number we are willing to put on the wire: 2..20 digits, no leading zero of
 * a kind that suggests the caller passed a formatted string by mistake.  This
 * is a sanity check, not a validity check - the network is the authority. */
bool sms_number_plausible(const char *digits);

/* The GSM 7-bit alphabet, exposed for the front end and the tests.  Maps a
 * Unicode code point to its septet value, or to 0x1B00|ext for an extension
 * table character (two septets), or -1. */
int  sms_gsm7_from_ucs(uint32_t cp);
uint32_t sms_gsm7_to_ucs(int septet, bool ext);

#endif /* FM160_PDU_H */
