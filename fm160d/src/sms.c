/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sms.c - the SMS subsystem.
 *
 * Shape of the thing:
 *
 *   - a one-time setup (AT+CMGF=0, AT+CPMS, AT+CNMI) whose success is the
 *     licence for everything else;
 *   - an incoming path driven entirely by the +CMTI URC, which asks the modem
 *     for one index at a time;
 *   - an outgoing path that encodes, splits, and drives one AT+CMGS per
 *     segment through the two-stage queue entry;
 *   - a local list, appended to a file, so a message survives the modem
 *     forgetting it.
 *
 * Nothing here is polled.  See the note above struct fm160_sms_state in
 * fm160d.h for the measurements behind that.
 *
 * PDU mode (AT+CMGF=0) is not a preference.  Text mode cannot carry anything
 * outside the GSM alphabet - a Chinese message either fails or arrives as
 * question marks - and the delivery report the modem hands back has no place to
 * put the sender.  One codec, exercised by a host-side test, is worth more than
 * two paths of which one is lossy.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>

#include <libubox/blobmsg_json.h>

#include "fm160d.h"

#define SMS_STORE_DIR        "/etc/fm160/sms"
#define SMS_STORE_FILE       "/etc/fm160/sms/inbox.jsonl"

/* The setup runs once, a few seconds after the identity chain has finished.
 * On a modem with no card AT+CPMS? takes 10.3 s and answers ERROR, so the retry
 * is deliberately slow and deliberately finite: three tries, then it waits for
 * a human or for a message to actually arrive. */
#define SMS_SETUP_FIRST_MS   4000
#define SMS_SETUP_RETRY_MS   60000
#define SMS_SETUP_MAX_TRIES  3

/* A segment can legitimately take a while on a slow network. */
#define SMS_SEND_TIMEOUT_MS  60000
/* Covers a whole multi-segment send, so no background poll can be slipped
 * between the prompt and the payload of any segment. */
#define SMS_QUIET_S          180

#define SMS_PREFERRED_MEM    "ME"
#define SMS_FALLBACK_MEM     "SM"

static int  setup_step;
static int  setup_tries;
static uint64_t setup_next_ms;

/* ------------------------------------------------------------------ */
/* the local list                                                       */
/* ------------------------------------------------------------------ */

static struct fm160_sms_msg *sms_new_slot(void)
{
	struct fm160_sms_msg *m = &g_state.sms.msg[g_state.sms.head];

	g_state.sms.head = (g_state.sms.head + 1) % FM160_SMS_KEEP;
	if (g_state.sms.count < FM160_SMS_KEEP)
		g_state.sms.count++;
	memset(m, 0, sizeof(*m));
	m->used = true;
	m->index = -1;
	m->local_ms = fm160_now_ms();
	m->id = ++g_state.sms.next_id;
	return m;
}

int fm160_sms_count(void)
{
	return g_state.sms.count;
}

/*
 * nth 0 is the newest message, and "newest" means by id - not by slot.
 *
 * The slots are a ring, but a deletion makes a hole in the middle of it and the
 * next message is written into the hole rather than at the end (the ring only
 * ever overwrites its oldest slot, which is where head points).  A slot's
 * position therefore stops matching its age the first time anything is deleted.
 * Counting backwards from head would then skip a message, show a deleted one,
 * or reorder the list - all of which look like corruption to the reader.  The
 * id is handed out in increasing order and never reused, so ordering by it is
 * ordering by time regardless of where the ring happens to write.
 *
 * A select over 64 entries, repeated nth+1 times: the list is small enough that
 * clarity is worth more than the sort.
 */
const struct fm160_sms_msg *fm160_sms_get(int nth)
{
	const struct fm160_sms_msg *found = NULL;
	uint64_t ceiling = UINT64_MAX;
	int k, i;

	if (nth < 0 || nth >= g_state.sms.count)
		return NULL;
	for (k = 0; k <= nth; k++) {
		const struct fm160_sms_msg *best = NULL;

		for (i = 0; i < FM160_SMS_KEEP; i++) {
			const struct fm160_sms_msg *c = &g_state.sms.msg[i];

			if (!c->used || c->id >= ceiling)
				continue;
			if (!best || c->id > best->id)
				best = c;
		}
		if (!best)
			return NULL;
		found = best;
		ceiling = best->id;
	}
	return found;
}

static struct fm160_sms_msg *sms_by_id(uint64_t id)
{
	int i;

	for (i = 0; i < FM160_SMS_KEEP; i++)
		if (g_state.sms.msg[i].used && g_state.sms.msg[i].id == id)
			return &g_state.sms.msg[i];
	return NULL;
}

/* The same lookup the delete path uses, for callers outside this file that need
 * a message's modem-side index before deciding what to ask the modem to do. */
const struct fm160_sms_msg *fm160_sms_find(uint64_t id)
{
	return sms_by_id(id);
}

/*
 * Is this message already in the list?
 *
 * Keyed on the modem's own storage and index, because that is the only thing
 * that is genuinely unique while the message sits there.  The index is reused
 * once a message is deleted, which is why the body and the sender are compared
 * too: a fresh message that happens to land in a recycled slot must not be
 * mistaken for the old one and swallowed.
 */
static struct fm160_sms_msg *sms_find_duplicate(const char *storage, int index,
						const char *number, const char *text)
{
	int i;

	for (i = 0; i < FM160_SMS_KEEP; i++) {
		struct fm160_sms_msg *m = &g_state.sms.msg[i];

		if (!m->used || m->index != index)
			continue;
		if (strcmp(m->storage, storage))
			continue;
		if (strcmp(m->number, number))
			continue;
		if (strcmp(m->text, text))
			continue;
		return m;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* persistence                                                          */
/* ------------------------------------------------------------------ */

/*
 * A JSON object per line.  Appended, never rewritten: a message that has been
 * read is evidence, and rewriting the file to remove one would risk the rest.
 * A line that fails to parse is skipped rather than fatal - a truncated tail
 * (power cut mid-write) must not cost the whole history.
 */
static void sms_store_append(const struct fm160_sms_msg *m)
{
	struct blob_buf b = {};
	FILE *f;

	if (mkdir(SMS_STORE_DIR, 0755) && errno != EEXIST)
		return;

	f = fopen(SMS_STORE_FILE, "a");
	if (!f)
		return;

	blob_buf_init(&b, 0);
	blobmsg_add_u64(&b, "id", m->id);
	blobmsg_add_u8(&b, "out", m->outgoing);
	blobmsg_add_u32(&b, "idx", (uint32_t)m->index);
	blobmsg_add_string(&b, "store", m->storage);
	blobmsg_add_string(&b, "num", m->number);
	blobmsg_add_string(&b, "text", m->text);
	blobmsg_add_u32(&b, "enc", (uint32_t)m->encoding);
	if (m->has_time) {
		blobmsg_add_u32(&b, "y", (uint32_t)m->year);
		blobmsg_add_u32(&b, "mo", (uint32_t)m->month);
		blobmsg_add_u32(&b, "d", (uint32_t)m->day);
		blobmsg_add_u32(&b, "h", (uint32_t)m->hour);
		blobmsg_add_u32(&b, "mi", (uint32_t)m->min);
		blobmsg_add_u32(&b, "s", (uint32_t)m->sec);
		blobmsg_add_u32(&b, "tz", (uint32_t)(m->tz_negative ?
						     -m->tz_quarters : m->tz_quarters));
	}
	if (m->concat) {
		blobmsg_add_u8(&b, "cat", 1);
		blobmsg_add_u32(&b, "ref", (uint32_t)m->ref);
		blobmsg_add_u32(&b, "parts", (uint32_t)m->parts);
		blobmsg_add_u32(&b, "part", (uint32_t)m->part);
	}
	fprintf(f, "%s\n", blobmsg_format_json(b.head, true));
	fclose(f);
	blob_buf_free(&b);
}

enum {
	ST_ID, ST_OUT, ST_IDX, ST_STORE, ST_NUM, ST_TEXT, ST_ENC,
	ST_Y, ST_MO, ST_D, ST_H, ST_MI, ST_S, ST_TZ, ST_CAT, ST_REF,
	ST_PARTS, ST_PART, ST_DEL, __ST_MAX
};

static const struct blobmsg_policy store_policy[] = {
	[ST_ID]    = { .name = "id",    .type = BLOBMSG_TYPE_INT64  },
	[ST_OUT]   = { .name = "out",   .type = BLOBMSG_TYPE_INT8   },
	[ST_IDX]   = { .name = "idx",   .type = BLOBMSG_TYPE_INT32  },
	[ST_STORE] = { .name = "store", .type = BLOBMSG_TYPE_STRING },
	[ST_NUM]   = { .name = "num",   .type = BLOBMSG_TYPE_STRING },
	[ST_TEXT]  = { .name = "text",  .type = BLOBMSG_TYPE_STRING },
	[ST_ENC]   = { .name = "enc",   .type = BLOBMSG_TYPE_INT32  },
	[ST_Y]     = { .name = "y",     .type = BLOBMSG_TYPE_INT32  },
	[ST_MO]    = { .name = "mo",    .type = BLOBMSG_TYPE_INT32  },
	[ST_D]     = { .name = "d",     .type = BLOBMSG_TYPE_INT32  },
	[ST_H]     = { .name = "h",     .type = BLOBMSG_TYPE_INT32  },
	[ST_MI]    = { .name = "mi",    .type = BLOBMSG_TYPE_INT32  },
	[ST_S]     = { .name = "s",     .type = BLOBMSG_TYPE_INT32  },
	[ST_TZ]    = { .name = "tz",    .type = BLOBMSG_TYPE_INT32  },
	[ST_CAT]   = { .name = "cat",   .type = BLOBMSG_TYPE_INT8   },
	[ST_REF]   = { .name = "ref",   .type = BLOBMSG_TYPE_INT32  },
	[ST_PARTS] = { .name = "parts", .type = BLOBMSG_TYPE_INT32  },
	[ST_PART]  = { .name = "part",  .type = BLOBMSG_TYPE_INT32  },
	[ST_DEL]   = { .name = "del",   .type = BLOBMSG_TYPE_INT8   },
};

/*
 * A deletion is a line in the same file, and for the same reason the messages
 * are: nothing ever rewrites this file, so RAM is the only place a deletion
 * could otherwise live - and a message the user deleted would be back after the
 * next reboot.  The line carries the id and a flag; the message itself stays
 * where it is, byte for byte, because a half-rewritten history is worse than a
 * slightly longer one.
 */
static void sms_store_append_tombstone(uint64_t id)
{
	struct blob_buf b = {};
	FILE *f;

	if (mkdir(SMS_STORE_DIR, 0755) && errno != EEXIST)
		return;
	f = fopen(SMS_STORE_FILE, "a");
	if (!f)
		return;

	blob_buf_init(&b, 0);
	blobmsg_add_u64(&b, "id", id);
	blobmsg_add_u8(&b, "del", 1);
	fprintf(f, "%s\n", blobmsg_format_json(b.head, true));
	fclose(f);
	blob_buf_free(&b);
}

static void sms_store_load(void)
{
	FILE *f = fopen(SMS_STORE_FILE, "r");
	char *line;
	size_t cap = 0;
	int loaded = 0, skipped = 0;

	if (!f)
		return;
	line = malloc(8192);
	if (!line) {
		fclose(f);
		return;
	}
	cap = 8192;
	while (fgets(line, (int)cap, f)) {
		struct blob_buf b = {};
		struct blob_attr *tb[__ST_MAX];
		struct fm160_sms_msg *m;
		int tz;

		blob_buf_init(&b, 0);
		if (!blobmsg_add_json_from_string(&b, line)) {
			blob_buf_free(&b);
			skipped++;
			continue;
		}
		blobmsg_parse(store_policy, __ST_MAX, tb, blob_data(b.head),
			      blob_len(b.head));
		/*
		 * A deletion marker.  Applied in file order, and the file is
		 * append-only, so the message it refers to has always been read
		 * already by the time the marker arrives - no second pass, and
		 * no need to remember ids for later.
		 */
		if (tb[ST_DEL] && blobmsg_get_u8(tb[ST_DEL]) && tb[ST_ID]) {
			struct fm160_sms_msg *gone =
				sms_by_id(blobmsg_get_u64(tb[ST_ID]));

			if (gone) {
				gone->used = false;
				if (g_state.sms.count > 0)
					g_state.sms.count--;
			}
			blob_buf_free(&b);
			continue;
		}
		if (!tb[ST_TEXT] || !tb[ST_NUM]) {
			blob_buf_free(&b);
			skipped++;
			continue;
		}
		m = sms_new_slot();
		m->id = tb[ST_ID] ? blobmsg_get_u64(tb[ST_ID]) : 0;
		if (!m->id)
			m->id = g_state.sms.next_id;
		if (m->id > g_state.sms.next_id)
			g_state.sms.next_id = m->id;
		m->outgoing = tb[ST_OUT] ? blobmsg_get_u8(tb[ST_OUT]) : 0;
		m->index = tb[ST_IDX] ? (int)blobmsg_get_u32(tb[ST_IDX]) : -1;
		snprintf(m->storage, sizeof(m->storage), "%s",
			 tb[ST_STORE] ? blobmsg_get_string(tb[ST_STORE]) : "");
		snprintf(m->number, sizeof(m->number), "%s",
			 blobmsg_get_string(tb[ST_NUM]));
		snprintf(m->text, sizeof(m->text), "%s",
			 blobmsg_get_string(tb[ST_TEXT]));
		m->text_len = tb[ST_TEXT] ?
			(int)strlen(blobmsg_get_string(tb[ST_TEXT])) : 0;
		m->encoding = tb[ST_ENC] ? (int)blobmsg_get_u32(tb[ST_ENC]) : 0;
		if (tb[ST_Y]) {
			m->has_time = true;
			m->year = (int)blobmsg_get_u32(tb[ST_Y]);
			m->month = tb[ST_MO] ? (int)blobmsg_get_u32(tb[ST_MO]) : 0;
			m->day = tb[ST_D] ? (int)blobmsg_get_u32(tb[ST_D]) : 0;
			m->hour = tb[ST_H] ? (int)blobmsg_get_u32(tb[ST_H]) : 0;
			m->min = tb[ST_MI] ? (int)blobmsg_get_u32(tb[ST_MI]) : 0;
			m->sec = tb[ST_S] ? (int)blobmsg_get_u32(tb[ST_S]) : 0;
			tz = tb[ST_TZ] ? (int)blobmsg_get_u32(tb[ST_TZ]) : 0;
			m->tz_negative = tz < 0;
			m->tz_quarters = tz < 0 ? -tz : tz;
		}
		if (tb[ST_CAT]) {
			m->concat = true;
			m->ref = tb[ST_REF] ? (int)blobmsg_get_u32(tb[ST_REF]) : 0;
			m->parts = tb[ST_PARTS] ? (int)blobmsg_get_u32(tb[ST_PARTS]) : 0;
			m->part = tb[ST_PART] ? (int)blobmsg_get_u32(tb[ST_PART]) : 0;
		}
		blob_buf_free(&b);
		loaded++;
	}
	free(line);
	fclose(f);
	if (loaded || skipped)
		fm160_log(LOG_INFO, "sms store: %d message(s) loaded, %d line(s) skipped",
			  loaded, skipped);
}

int fm160_sms_delete_local(uint64_t id)
{
	struct fm160_sms_msg *m = sms_by_id(id);

	if (!m)
		return -ENOENT;
	/* The file is append-only, so "deleted" means "hidden from now on".  A
	 * rewrite would have to reproduce every remaining line byte for byte,
	 * and a crash in the middle of one costs the whole history to save a
	 * few kilobytes of flash.  The marker is written before the list is
	 * touched so that a crash between the two leaves the message deleted,
	 * which is what was asked for, rather than leaving it present, which
	 * would silently undo the request. */
	sms_store_append_tombstone(id);
	m->used = false;
	g_state.sms.count--;
	fm160_state_mark_dirty();
	return 0;
}

int fm160_sms_mark_read(uint64_t id)
{
	struct fm160_sms_msg *m = sms_by_id(id);

	if (!m)
		return -ENOENT;
	if (m->status == 0) {
		m->status = 1;
		if (g_state.sms.st.unread > 0)
			g_state.sms.st.unread--;
		fm160_state_mark_dirty();
	}
	return 0;
}

/*
 * Drop the in-memory list.  This does NOT touch the store, and deliberately so:
 * the file is the record of what arrived, and the only supported way to make a
 * message stop being shown is fm160_sms_delete_local(), which appends a marker.
 * Anything that used this to "empty the inbox" would find it full again after a
 * reboot.
 */
void fm160_sms_clear(void)
{
	memset(g_state.sms.msg, 0, sizeof(g_state.sms.msg));
	g_state.sms.head = 0;
	g_state.sms.count = 0;
	g_state.sms.st.kept = 0;
	g_state.sms.st.unread = 0;
	fm160_state_mark_dirty();
}

/* ------------------------------------------------------------------ */
/* parsers                                                              */
/* ------------------------------------------------------------------ */

/*
 * Put one decoded PDU into the list.
 *
 * Returns the message so the caller can report it.  A duplicate is refreshed
 * (the timestamp is the same, but the modem may have moved it) and counted.
 */
static struct fm160_sms_msg *sms_store_pdu(const char *hex, int index,
					   const char *storage)
{
	struct sms_pdu_in in;
	struct fm160_sms_msg *m;

	memset(&in, 0, sizeof(in));
	if (sms_pdu_decode(hex, &in)) {
		/* Not silent: a PDU the codec cannot read is the only way a
		 * message is ever "lost", so it goes in the log with the reason. */
		fm160_log(LOG_WARNING, "cannot decode a stored PDU at %s,%d: %s",
			  storage, index, in.err);
		return NULL;
	}
	if (!in.valid)
		return NULL;

	m = sms_find_duplicate(storage, index, in.number, in.text);
	if (m) {
		g_state.sms.st.duplicates++;
		return m;
	}

	m = sms_new_slot();
	snprintf(m->number, sizeof(m->number), "%s", in.number);
	m->international = in.number_international;
	snprintf(m->text, sizeof(m->text), "%s", in.text);
	m->text_len = in.text_len;
	m->encoding = in.encoding;
	snprintf(m->storage, sizeof(m->storage), "%s", storage);
	m->index = index;
	/* A status report carries a recipient and a stamp but no body; it is
	 * marked outgoing so the page does not show it as something to reply
	 * to. */
	m->outgoing = (in.mti == 1 || in.mti == 2);
	m->status = m->outgoing ? 2 : 0;
	m->has_time = in.has_scts;
	m->year = in.year;
	m->month = in.month;
	m->day = in.day;
	m->hour = in.hour;
	m->min = in.min;
	m->sec = in.sec;
	m->tz_quarters = in.tz_quarters;
	m->tz_negative = in.tz_negative;
	m->concat = in.concat;
	m->ref = in.ref;
	m->parts = in.parts;
	m->part = in.part;
	if (!m->outgoing)
		g_state.sms.st.unread++;
	g_state.sms.st.received++;
	g_state.sms.st.kept = g_state.sms.count;
	g_state.sms.st.last_ok_ms = fm160_now_ms();
	sms_store_append(m);
	fm160_state_mark_dirty();
	fm160_log(LOG_INFO, "sms from %s, %d character(s)%s",
		  m->number, m->text_len,
		  m->concat ? " (part of a concatenated message)" : "");
	return m;
}

void fm160_parse_cmgr(const char *resp, int index, const char *storage)
{
	char hdr[FM160_RESP_LINE_MAX];
	const char *p, *hex, *nl;
	char buf[SMS_PDU_HEX_MAX + 32];
	size_t n;

	if (!resp || !fm160_resp_find(resp, "+CMGR", hdr, sizeof(hdr))) {
		fm160_log(LOG_DEBUG, "no +CMGR header in the answer for index %d",
			  index);
		return;
	}
	/* Line-start search, like fm160_parse_cmgl: a bare strstr also hits
	 * the echoed command ("AT+CMGR=2" contains "+CMGR"), and the "PDU"
	 * cut from that line is the header itself - parse_hex then fails on
	 * the '+' and the message is lost with "not hexadecimal". */
	p = resp;
	while ((p = strstr(p, "+CMGR")) != NULL) {
		if (p == resp || p[-1] == '\n')
			break;
		p++;
	}
	nl = p ? strchr(p, '\n') : NULL;
	if (!nl) {
		fm160_log(LOG_DEBUG, "index %d: the header had no PDU line after it",
			  index);
		return;
	}
	hex = nl + 1;
	while (*hex == ' ' || *hex == '\t' || *hex == '\r')
		hex++;
	/* Stop at the line end: the "OK" that follows is not part of the PDU,
	 * and feeding it to the hex reader would fail the whole message. */
	n = strcspn(hex, "\r\n");
	if (n == 0 || n >= sizeof(buf)) {
		fm160_log(LOG_WARNING, "index %d: the PDU line is empty or absurdly long",
			  index);
		return;
	}
	memcpy(buf, hex, n);
	buf[n] = '\0';
	sms_store_pdu(buf, index, storage);
}

/*
 * AT+CPMS? answers three triples, one per storage role:
 *   +CPMS: "ME",2,50,"ME",2,50,"ME",2,50
 * The first is the one reads and deletes go to, which is the one worth showing.
 */
void fm160_parse_cpms(const char *resp)
{
	char v[FM160_RESP_LINE_MAX];
	const char *p;
	int used = 0, total = 0;
	char mem[8] = "";

	if (!fm160_resp_find(resp, "+CPMS", v, sizeof(v))) {
		fm160_log(LOG_DEBUG, "no +CPMS line in '%s'",
			  resp ? resp : "(null)");
		return;
	}
	p = v;
	while (*p == ' ')
		p++;
	if (*p == '"') {
		const char *e = strchr(p + 1, '"');
		size_t n = e ? (size_t)(e - p - 1) : 0;

		if (n >= sizeof(mem))
			n = sizeof(mem) - 1;
		memcpy(mem, p + 1, n);
		mem[n] = '\0';
		p = e ? e + 1 : p;
	}
	if (*p == ',')
		p++;
	used = atoi(p);
	p = strchr(p, ',');
	total = p ? atoi(p + 1) : 0;

	if (mem[0])
		snprintf(g_state.sms.st.mem, sizeof(g_state.sms.st.mem), "%s", mem);
	g_state.sms.st.used = used;
	g_state.sms.st.total = total;
	g_state.sms.st.probed = true;
	fm160_log(LOG_INFO, "sms storage: %s, %d of %d used",
		  g_state.sms.st.mem, used, total);
}

/*
 * AT+CPMS=? answers the set of storages this modem supports, one list per role:
 *   +CPMS: ("ME","SM"),("ME","SM"),("ME","SM")
 *
 * The preferred storage is chosen from THIS, never assumed: a modem that does
 * not have "ME" answers ERROR to being told to use it, and the error is
 * indistinguishable from "no card" unless we know what it offered.
 */
void fm160_parse_cpms_caps(const char *resp)
{
	char v[FM160_RESP_LINE_MAX];
	const char *p;

	if (!resp || !fm160_resp_find(resp, "+CPMS", v, sizeof(v)))
		return;
	p = strstr(v, SMS_PREFERRED_MEM);
	if (p) {
		memcpy(g_state.sms.st.mem, SMS_PREFERRED_MEM,
		       sizeof(SMS_PREFERRED_MEM));
		fm160_log(LOG_DEBUG, "modem offers %s for messages", SMS_PREFERRED_MEM);
		return;
	}
	if (strstr(v, SMS_FALLBACK_MEM)) {
		memcpy(g_state.sms.st.mem, SMS_FALLBACK_MEM,
		       sizeof(SMS_FALLBACK_MEM));
		fm160_log(LOG_INFO, "modem does not offer %s, falling back to %s",
			  SMS_PREFERRED_MEM, SMS_FALLBACK_MEM);
		return;
	}
	fm160_log(LOG_WARNING, "modem offers no storage this code understands: %s", v);
}

/*
 * PDU-mode AT+CMGL answers pairs of lines:
 *   +CMGL: 0,0,,27
 *   07917283010010F5040B...
 */
void fm160_parse_cmgl(const char *resp)
{
	const char *p = resp;
	int n = 0;

	if (!resp)
		return;
	while ((p = strstr(p, "+CMGL")) != NULL) {
		const char *hex, *nl;
		char buf[SMS_PDU_HEX_MAX + 32];
		size_t len;
		int index;

		if (p != resp && p[-1] != '\n') {
			p++;
			continue;
		}
		index = atoi(p + 5 + 1);   /* skip "+CMGL:" */
		nl = strchr(p, '\n');
		if (!nl)
			break;
		hex = nl + 1;
		while (*hex == ' ' || *hex == '\t' || *hex == '\r')
			hex++;
		len = strcspn(hex, "\r\n");
		if (len == 0 || len >= sizeof(buf)) {
			p = nl + 1;
			continue;
		}
		memcpy(buf, hex, len);
		buf[len] = '\0';
		if (sms_store_pdu(buf, index, g_state.sms.st.mem))
			n++;
		p = hex + len;
	}
	if (n)
		fm160_log(LOG_INFO, "pulled %d message(s) from %s", n,
			  g_state.sms.st.mem);
}

void fm160_parse_cmgs(const char *resp)
{
	char v[FM160_RESP_LINE_MAX];

	if (!resp || !fm160_resp_find(resp, "+CMGS", v, sizeof(v)))
		return;
	g_state.sms.sent_mr = atoi(v);
}

/* ------------------------------------------------------------------ */
/* setup: CMGF / CPMS / CNMI                                            */
/* ------------------------------------------------------------------ */

bool fm160_sms_usable(void)
{
	return g_state.sms.st.usable;
}

static void setup_note_error(enum at_status st, const char *resp)
{
	g_state.sms.st.usable = false;
	if (resp) {
		char v[FM160_RESP_LINE_MAX];
		const char *code = NULL;

		if (fm160_resp_find(resp, "+CMS ERROR", v, sizeof(v)) ||
		    fm160_resp_find(resp, "+CME ERROR", v, sizeof(v)))
			code = v;
		snprintf(g_state.sms.st.last_error_text,
			 sizeof(g_state.sms.st.last_error_text), "%s",
			 code ? code : resp);
		g_state.sms.st.last_error = code ? atoi(code) : 0;
	} else if (st == AT_STATUS_TIMEOUT) {
		snprintf(g_state.sms.st.last_error_text,
			 sizeof(g_state.sms.st.last_error_text),
			 "the modem did not answer");
	}
	fm160_log(LOG_WARNING, "sms setup refused: %s",
		  g_state.sms.st.last_error_text[0] ?
		  g_state.sms.st.last_error_text : "no answer");
	fm160_state_mark_dirty();
}

static void setup_step_cb(struct at_req *req, enum at_status st,
			  const char *resp, void *arg)
{
	(void)req;
	(void)arg;

	if (st != AT_STATUS_OK) {
		/* One failure is enough to stop the chain: every later step
		 * builds on the previous one, so carrying on would just queue
		 * more refusals on a modem that has already said no. */
		setup_note_error(st, resp);
		setup_step = 0;
		setup_next_ms = fm160_now_ms() + SMS_SETUP_RETRY_MS;
		if (++setup_tries >= SMS_SETUP_MAX_TRIES) {
			fm160_log(LOG_WARNING,
				  "sms setup gave up after %d attempts; it will be "
				  "retried when a message arrives or a page asks",
				  setup_tries);
			g_state.sms.setup_next_ms = 0;
		}
		return;
	}

	switch (setup_step) {
	case 1:
		/* AT+CMGF=0 accepted.  Read it back rather than trusting the OK:
		 * a modem that ignores the command also answers OK. */
		setup_step = 2;
		if (atq_submit(AT_PRIO_STATE, "AT+CMGF?", NULL, 5000,
			       setup_step_cb, NULL))
			setup_step = 1;
		return;
	case 2: {
		char v[FM160_RESP_LINE_MAX];

		g_state.sms.cmgf = fm160_resp_find(resp, "+CMGF", v, sizeof(v))
				   ? atoi(v) : -1;
		if (g_state.sms.cmgf != 0) {
			snprintf(g_state.sms.st.last_error_text,
				 sizeof(g_state.sms.st.last_error_text),
				 "modem is not in PDU mode (CMGF=%d)",
				 g_state.sms.cmgf);
			setup_note_error(AT_STATUS_ERROR, NULL);
			setup_step = 0;
			setup_next_ms = fm160_now_ms() + SMS_SETUP_RETRY_MS;
			setup_tries = SMS_SETUP_MAX_TRIES;
			return;
		}
		/* What storages does it offer?  Optional: a modem that will not
		 * enumerate them can still be asked for one. */
		setup_step = 3;
		if (atq_submit(AT_PRIO_STATE, "AT+CPMS=?", NULL, 15000,
			       setup_step_cb, NULL))
			setup_step = 2;
		return;
	}
	case 3: {
		char cmd[64];
		const char *mem = g_state.sms.st.mem[0] ? g_state.sms.st.mem
						       : SMS_PREFERRED_MEM;

		fm160_parse_cpms_caps(resp);
		mem = g_state.sms.st.mem[0] ? g_state.sms.st.mem : mem;
		/* All three roles to the same storage: one place to look when a
		 * message does not appear where it was expected. */
		snprintf(cmd, sizeof(cmd), "AT+CPMS=\"%s\",\"%s\",\"%s\"",
			 mem, mem, mem);
		fm160_log(LOG_INFO, "sms storage set to %s", mem);
		setup_step = 4;
		if (atq_submit(AT_PRIO_STATE, cmd, NULL, 20000,
			       setup_step_cb, NULL))
			setup_step = 3;
		return;
	}
	case 4:
		/* AT+CNMI=2,1,0,0,0 - announce new messages as an index
		 * (+CMTI) and do NOT deliver them inline.
		 *
		 * The second field is the whole reason: with 2,2 the modem
		 * splices a whole PDU into the AT stream the moment it arrives,
		 * which lands in the middle of whatever command is in flight and
		 * corrupts both.  An index costs one extra round trip and cannot
		 * collide with anything. */
		setup_step = 5;
		if (atq_submit(AT_PRIO_STATE, "AT+CNMI=2,1,0,0,0", NULL, 8000,
			       setup_step_cb, NULL))
			setup_step = 4;
		return;
	case 5:
		setup_step = 0;
		setup_tries = 0;
		g_state.sms.setup_done = true;
		g_state.sms.st.usable = true;
		g_state.sms.st.last_ok_ms = fm160_now_ms();
		g_state.sms.st.last_error = 0;
		g_state.sms.st.last_error_text[0] = '\0';
		fm160_log(LOG_INFO, "sms ready: PDU mode, storage %s, +CMTI armed",
			  g_state.sms.st.mem);
		fm160_state_mark_dirty();
		return;
	default:
		return;
	}
}

void fm160_sms_setup_reset(void)
{
	/* A (re)ident run means the module may be a different physical device -
	 * or the same one after AT+CFUN / a flash, which quietly puts CNMI back
	 * to 0,0,0,0,0 and CPMS back to "SM".  Every "armed once, armed forever"
	 * assumption dies here so the setup state machine re-runs and re-arms
	 * the new module.  In-flight setup transactions belonged to the old
	 * port and are already dead (the at-daemon lease was lost with it), so
	 * clearing the step counter cannot strand a live callback that would
	 * resurrect setup_done with only half the arming done. */
	g_state.sms.setup_done = false;
	g_state.sms.setup_running = false;
	g_state.sms.cmgf = -1;         /* the mode cache described the old module */
	setup_step = 0;
	setup_tries = 0;
	setup_next_ms = 0;
}

void fm160_sms_setup_tick(void)
{
	if (g_state.sms.setup_done || setup_step)
		return;
	if (!g_state.port_found || !g_state.ident_done)
		return;
	if (setup_tries >= SMS_SETUP_MAX_TRIES)
		return;                /* waiting for a human, or a message */
	if (fm160_now_ms() < setup_next_ms)
		return;

	setup_step = 1;
	setup_next_ms = fm160_now_ms() + SMS_SETUP_RETRY_MS;
	if (atq_submit(AT_PRIO_STATE, "AT+CMGF=0", NULL, 5000,
		       setup_step_cb, NULL))
		setup_step = 0;        /* queue busy; try again next tick */
	else
		fm160_log(LOG_DEBUG, "sms: asking the modem for PDU mode");
}

/* Called when something arrives that makes the setup worth retrying - a human
 * pressed refresh, or the modem announced a message. */
static void sms_setup_kick(void)
{
	if (g_state.sms.setup_done)
		return;
	if (setup_tries >= SMS_SETUP_MAX_TRIES) {
		setup_tries = 0;
		setup_next_ms = fm160_now_ms();
	}
}

/* ------------------------------------------------------------------ */
/* reading and deleting                                                 */
/* ------------------------------------------------------------------ */

static void fetch_cb(struct at_req *req, enum at_status st, const char *resp,
		     void *arg)
{
	int index = (int)(intptr_t)arg;

	g_state.sms.busy = false;
	atq_clear_quiet();
	if (st == AT_STATUS_OK)
		fm160_parse_cmgr(resp, index, g_state.sms.st.mem);
	else
		fm160_log(LOG_WARNING, "reading message %d from %s failed (%s)",
			  index, g_state.sms.st.mem,
			  resp ? resp : "no answer");
	fm160_state_mark_dirty();
}

int fm160_cmd_sms_fetch(int index)
{
	char cmd[32];

	if (!fm160_sms_usable())
		return -EAGAIN;
	if (g_state.sms.busy)
		return -EBUSY;
	if (index < 0)
		return -EINVAL;

	snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);
	g_state.sms.busy = true;
	atq_set_quiet(QUIET_SMS, "reading a stored message", 30);
	if (atq_submit(AT_PRIO_STATE, cmd, NULL, 30000, fetch_cb,
		       (void *)(intptr_t)index)) {
		g_state.sms.busy = false;
		atq_clear_quiet();
		return -EAGAIN;
	}
	return 0;
}

static void sync_cb(struct at_req *req, enum at_status st, const char *resp,
		    void *arg)
{
	(void)req;
	(void)arg;

	g_state.sms.busy = false;
	atq_clear_quiet();
	if (st != AT_STATUS_OK) {
		snprintf(g_state.sms.st.last_error_text,
			 sizeof(g_state.sms.st.last_error_text), "%s",
			 resp ? resp : "no answer");
		fm160_log(LOG_WARNING, "listing stored messages failed: %s",
			  g_state.sms.st.last_error_text);
		fm160_state_mark_dirty();
		return;
	}
	/* AT+CMGL=4 means "all messages, every state", which in PDU mode is the
	 * only listing that needs one command.  The alternative is four lists
	 * (unread / read / sent / unsent) and four chances to disagree. */
	fm160_parse_cmgl(resp);
	fm160_state_mark_dirty();
}

int fm160_cmd_sms_sync(void)
{
	if (!g_state.sms.setup_done) {
		/* A human asked for this, so it is worth another try even if the
		 * automatic attempts have been used up. */
		sms_setup_kick();
		fm160_sms_setup_tick();
		if (!g_state.sms.setup_done)
			return -EAGAIN;
	}
	if (g_state.sms.busy)
		return -EBUSY;

	g_state.sms.busy = true;
	atq_set_quiet(QUIET_SMS, "listing stored messages", 60);
	if (atq_submit(AT_PRIO_STATE, "AT+CMGL=4", NULL, 45000, sync_cb, NULL)) {
		g_state.sms.busy = false;
		atq_clear_quiet();
		return -EAGAIN;
	}
	return 0;
}

static void delete_cb(struct at_req *req, enum at_status st, const char *resp,
		      void *arg)
{
	at_done_cb outer = (at_done_cb)arg;

	atq_clear_quiet();
	if (st == AT_STATUS_OK)
		fm160_log(LOG_INFO, "message deleted from the modem");
	else
		fm160_log(LOG_WARNING, "deleting from the modem failed: %s",
			  resp ? resp : "no answer");
	if (outer)
		outer(req, st, resp, NULL);
}

int fm160_cmd_sms_delete(int index, at_done_cb cb, void *arg)
{
	char cmd[32];

	if (!fm160_sms_usable())
		return -EAGAIN;
	if (index < 0)
		return -EINVAL;

	snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
	atq_set_quiet(QUIET_SMS, "deleting a stored message", 30);
	if (atq_submit(AT_PRIO_STATE, cmd, NULL, 30000, delete_cb, cb)) {
		atq_clear_quiet();
		return -EAGAIN;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* sending                                                              */
/* ------------------------------------------------------------------ */

/*
 * One send spans as many AT+CMGS transactions as there are segments, so the
 * caller's callback cannot be the queue callback directly.  This job owns the
 * sequence and reports once, at the end, with the outcome of the whole message.
 */
struct sms_job {
	struct sms_pdu_out out;
	int  seg;                      /* the segment being sent, 0 based */
	int  done_segments;
	enum at_status status;
	char detail[FM160_RESP_LINE_MAX];
	at_done_cb cb;
	void *arg;
};

static void sms_job_finish(struct sms_job *job, enum at_status st,
			   const char *detail)
{
	struct at_req fake;

	/* The callback contract is (req, status, response, arg); the request it
	 * refers to is gone by now, so it is handed a zeroed one.  Callbacks
	 * written for this path read the status and the response only. */
	memset(&fake, 0, sizeof(fake));

	g_state.sms.busy = false;
	atq_clear_quiet();

	if (st == AT_STATUS_OK) {
		g_state.sms.st.sent_ok++;
		fm160_log(LOG_INFO, "sms sent in %d segment(s)", job->done_segments);
	} else if (st == AT_STATUS_TIMEOUT) {
		g_state.sms.st.sent_timeout++;
		fm160_log(LOG_WARNING, "sms send timed out after %d segment(s): %s",
			  job->done_segments, detail ? detail : "");
	} else {
		g_state.sms.st.sent_fail++;
		fm160_log(LOG_WARNING, "sms send failed after %d segment(s): %s",
			  job->done_segments,
			  (detail && *detail) ? detail : "refused");
	}
	if ((detail && *detail) || job->done_segments) {
		snprintf(g_state.sms.st.last_error_text,
			 sizeof(g_state.sms.st.last_error_text), "%s",
			 detail ? detail : "");
	}
	g_state.sms.st.last_ok_ms = fm160_now_ms();
	fm160_state_mark_dirty();

	if (job->cb)
		job->cb(&fake, st, detail, job->arg);
	free(job);
}

static void sms_seg_cb(struct at_req *req, enum at_status st, const char *resp,
		       void *arg)
{
	struct sms_job *job = arg;

	(void)req;

	if (st != AT_STATUS_OK) {
		sms_job_finish(job, st, resp);
		return;
	}

	fm160_parse_cmgs(resp);
	job->done_segments++;

	if (job->seg + 1 < job->out.segments) {
		char cmd[32];
		struct sms_segment *sg;
		int bytes;

		job->seg++;
		sg = &job->out.seg[job->seg];
		snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", sg->tpdu_len);

		/* The payload the modem wants is the PDU as ASCII, written
		 * verbatim, ended by Ctrl-Z.  at-daemon's raw_at_content takes
		 * hex, so each character becomes two digits - and the 0x1A goes
		 * on the end, NOT a carriage return. */
		bytes = (int)strlen(sg->hex);
		if (bytes * 2 + 2 >= (int)sizeof(g_state.sms.last_pdu)) {
			sms_job_finish(job, AT_STATUS_ERROR,
				       "internal: the PDU does not fit the send buffer");
			return;
		}
		{
			static const char hx[] = "0123456789ABCDEF";
			char *w = g_state.sms.last_pdu;
			int i;

			for (i = 0; i < bytes; i++) {
				unsigned char c = (unsigned char)sg->hex[i];

				*w++ = hx[c >> 4];
				*w++ = hx[c & 0x0F];
			}
			*w++ = '1';
			*w++ = 'A';
			*w = '\0';
		}
		snprintf(g_state.sms.last_send_cmd, sizeof(g_state.sms.last_send_cmd),
			 "%s", cmd);

		/* Counted for the zero-length-packet question: a PDU that needs
		 * more than one USB packet is the one that would stall if the
		 * workaround were missing (PLAN 4). */
		if (bytes + 1 > 64)
			g_state.sms.st.long_sent++;

		if (atq_submit_prompt(AT_PRIO_STATE, cmd, ">",
				      g_state.sms.last_pdu, NULL,
				      SMS_SEND_TIMEOUT_MS, sms_seg_cb, job)) {
			sms_job_finish(job, AT_STATUS_TIMEOUT,
				       "could not queue the next segment");
		}
		return;
	}

	sms_job_finish(job, AT_STATUS_OK, "OK");
}

int fm160_cmd_sms_send(const char *number, const char *text,
		       at_done_cb cb, void *arg)
{
	struct sms_job *job;
	char err[128] = "";
	int n, i;

	if (!fm160_sms_usable())
		return -EAGAIN;
	if (g_state.sms.busy)
		return -EBUSY;

	job = calloc(1, sizeof(*job));
	if (!job)
		return -ENOMEM;

	n = sms_pdu_encode(number, text, FM160_SMS_SEG_MAX, 0, false,
			   &job->out, err, sizeof(err));
	if (n <= 0) {
		/* Distinguish "the modem said no" from "we would not build it":
		 * only the second is something the user can fix by editing the
		 * text, and the message has to say which. */
		fm160_log(LOG_WARNING, "sms not sent: %s", err);
		snprintf(g_state.sms.st.last_error_text,
			 sizeof(g_state.sms.st.last_error_text), "%s", err);
		fm160_state_mark_dirty();
		free(job);
		return n < 0 ? n : -EINVAL;
	}

	job->cb = cb;
	job->arg = arg;
	job->out.segments = n;
	g_state.sms.last_segments = n;
	g_state.sms.busy = true;
	atq_set_quiet(QUIET_SMS, "sending a message", SMS_QUIET_S);

	fm160_log(LOG_INFO, "sms: %d character(s) -> %d segment(s), %s",
		  job->out.chars, n,
		  job->out.encoding == SMS_ENC_UCS2 ? "UCS2" : "GSM 7-bit");

	{
		char cmd[32];
		struct sms_segment *sg = &job->out.seg[0];
		static const char hx[] = "0123456789ABCDEF";
		char *w = g_state.sms.last_pdu;
		int bytes = (int)strlen(sg->hex);

		if (bytes * 2 + 2 >= (int)sizeof(g_state.sms.last_pdu)) {
			free(job);
			g_state.sms.busy = false;
			atq_clear_quiet();
			return -ENOSPC;
		}
		for (i = 0; i < bytes; i++) {
			unsigned char c = (unsigned char)sg->hex[i];

			*w++ = hx[c >> 4];
			*w++ = hx[c & 0x0F];
		}
		*w++ = '1';
		*w++ = 'A';
		*w = '\0';

		snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", sg->tpdu_len);
		snprintf(g_state.sms.last_send_cmd, sizeof(g_state.sms.last_send_cmd),
			 "%s", cmd);
		if (bytes + 1 > 64)
			g_state.sms.st.long_sent++;

		if (atq_submit_prompt(AT_PRIO_STATE, cmd, ">",
				      g_state.sms.last_pdu, NULL,
				      SMS_SEND_TIMEOUT_MS, sms_seg_cb, job)) {
			g_state.sms.busy = false;
			atq_clear_quiet();
			free(job);
			return -EAGAIN;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* unsolicited results                                                  */
/* ------------------------------------------------------------------ */

bool fm160_sms_handle_urc(const char *line)
{
	if (!line)
		return false;

	if (!strncmp(line, "+CMTI:", 6)) {
		/* +CMTI: "SM",12 */
		const char *q = strchr(line, '"');
		int index = -1;
		char storage[8] = "";

		if (q) {
			const char *e = strchr(q + 1, '"');

			if (e) {
				size_t n = (size_t)(e - q - 1);

				if (n >= sizeof(storage))
					n = sizeof(storage) - 1;
				memcpy(storage, q + 1, n);
				storage[n] = '\0';
				/* e points at the CLOSING quote, so e + 1 is ",12":
				 * atoi() stops at the comma and would silently
				 * return 0 - every +CMTI would fetch message 0
				 * and the real one would stay unread.  Skip the
				 * separator before converting. */
				index = atoi(e + 1 + strspn(e + 1, " ,\t"));
			}
		}
		fm160_log(LOG_INFO, "new message announced: %s", line);
		if (storage[0])
			snprintf(g_state.sms.st.mem, sizeof(g_state.sms.st.mem),
				 "%s", storage);
		/* The modem has just proved the feature works, so a setup that
		 * had given up is worth retrying now. */
		sms_setup_kick();
		if (index >= 0) {
			if (!fm160_sms_usable()) {
				fm160_sms_setup_tick();
				if (!fm160_sms_usable()) {
					fm160_log(LOG_INFO,
						  "message %d is waiting but the sms setup has not succeeded yet",
						  index);
					return true;
				}
			}
			fm160_cmd_sms_fetch(index);
		}
		/* No unread bump here: the count is kept by the fetch path
		 * when the PDU is actually stored, and bumping at announce
		 * time too double-counts every delivered message. */
		fm160_state_mark_dirty();
		return true;
	}

	if (!strncmp(line, "+CMT:", 5)) {
		/* Only ever seen if someone arms AT+CNMI=2,2.  We do not, and
		 * this note exists so that if it does turn up the log says what
		 * it means instead of looking like noise. */
		fm160_log(LOG_WARNING,
			  "modem delivered a message inline (+CMT); CNMI is not what we armed");
		return true;
	}

	if (!strncmp(line, "+CMS ERROR", 10) || !strncmp(line, "+CME ERROR", 10)) {
		char v[FM160_RESP_LINE_MAX];

		snprintf(v, sizeof(v), "%s", line);
		fm160_log(LOG_WARNING, "modem error during an sms operation: %s", v);
		return true;
	}

	if (!strncmp(line, "+CDS:", 5)) {
		/* A delivery report.  Parsed as an ordinary PDU by the fetch
		 * path; nothing to do at the URC level. */
		fm160_log(LOG_INFO, "delivery report received");
		return true;
	}

	return false;
}

/* ------------------------------------------------------------------ */

void fm160_sms_init(void)
{
	memset(&g_state.sms, 0, sizeof(g_state.sms));
	g_state.sms.cmgf = -1;
	g_state.sms.sent_mr = -1;
	setup_step = 0;
	setup_tries = 0;
	setup_next_ms = fm160_now_ms() + SMS_SETUP_FIRST_MS;
	sms_store_load();
	g_state.sms.st.kept = g_state.sms.count;
	fm160_log(LOG_INFO, "sms: %d stored message(s), setup in %d ms",
		  g_state.sms.count, SMS_SETUP_FIRST_MS);
}
