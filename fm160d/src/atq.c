/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * atq.c - the AT request queue.
 *
 * This is the single choke point through which every AT command issued by
 * fm160d passes.  Invariants:
 *   - at most ONE request is in flight on the wire at any time;
 *   - requests are ordered by (priority, submission time), so a user click
 *     always overtakes background polling;
 *   - duplicate background polls are coalesced;
 *   - a background (POLL) request starved for more than 30 s is promoted so
 *     that a user hammering the UI cannot stall status updates forever.
 *
 * The actual serial I/O belongs to at-daemon (ubus object "at-daemon",
 * method "sendat"); we only talk to it asynchronously.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "fm160d.h"

#define ATQ_MAX_DEPTH          24
#define ATQ_STARVE_MS          30000
#define AT_DAEMON_OBJ          "at-daemon"
#define AT_DAEMON_RETRY_MS     3000
/* How long to wait for the "> " that AT+CMGS answers with.  The modem emits it
 * immediately; the margin is for a busy modem, not for slowness of ours. */
#define ATQ_PROMPT_TIMEOUT_MS  10000

static struct list_head q_head = LIST_HEAD_INIT(q_head);
static struct at_req *inflight;
static uint64_t next_id = 1;
static uint32_t at_daemon_id;
static uint64_t at_daemon_retry_ms;

struct sendat_reply {
	const char *response;
	const char *status;
	const char *end_flag_matched;
	uint32_t response_time_ms;
};

static const struct blobmsg_policy sendat_policy[] = {
	[0] = { .name = "response",          .type = BLOBMSG_TYPE_STRING },
	[1] = { .name = "status",            .type = BLOBMSG_TYPE_STRING },
	[2] = { .name = "end_flag_matched",  .type = BLOBMSG_TYPE_STRING },
	[3] = { .name = "response_time_ms",  .type = BLOBMSG_TYPE_INT32  },
};

static bool at_daemon_ready(void)
{
	if (at_daemon_id)
		return true;
	if (!g_ubus)
		return false;
	if (fm160_now_ms() < at_daemon_retry_ms)
		return false;
	at_daemon_retry_ms = fm160_now_ms() + AT_DAEMON_RETRY_MS;
	if (ubus_lookup_id(g_ubus, AT_DAEMON_OBJ, &at_daemon_id)) {
		at_daemon_id = 0;
		return false;
	}
	fm160_log(LOG_INFO, "transport ready: %s", AT_DAEMON_OBJ);
	return true;
}

/*
 * Detach a request from the queue, at most once.
 *
 * This guard is not defensive style, it is the fix for a crash that only
 * appeared once port discovery started working.  libubox is NOT the kernel
 * list.h: its list_del() is
 *
 *     _list_del(entry);                    // entry->next->prev = entry->prev
 *     entry->next = entry->prev = NULL;    // NULL, not LIST_POISON1/2
 *
 * so a second list_del() on the same node dereferences NULL and writes through
 * it: instant SIGSEGV, before anything can be logged.
 *
 * The double delete came from the two halves of the request lifetime each
 * believing they owned it:
 *   - atq_dispatch() detaches the request before handing it to at-daemon;
 *   - sendat_cb() detached it again on the reply.
 * A request only ever REACHES sendat_cb() by succeeding, and before the port
 * probe could succeed no request ever completed -- so the second delete was
 * never executed.  The first successful AT probe killed the daemon, which is
 * why fm160d logged "reading module identity" and then died in procd's
 * respawn loop with exit_code 139 (128 + SIGSEGV).
 *
 * A live queued node always has non-NULL next/prev (list_add_tail fills them,
 * INIT_LIST_HEAD points at the head), and list_del() NULLs both, so this test
 * is exact rather than a guess about state.
 */
static void at_req_free(struct at_req *req)
{
	if (req->list.next || req->list.prev)
		list_del(&req->list);
	free(req);
}

static void atq_dispatch(void);

/*
 * Retire a request: report it, release the queue slot, start the next one.
 *
 * Every path that ends a request's life comes through here.  That is the point
 * of extracting it: the two-stage path can finish a request *before* anything
 * has been written to the modem (no prompt ever arrived), and a request retired
 * anywhere else leaves `inflight` set, at which point the queue stops dead and
 * every later command sits in it forever.
 */
static void atq_finish(struct at_req *req, enum at_status status, const char *resp)
{
	if (status == AT_STATUS_OK)
		fm160_state_touch_ok();

	if (!req->silent)
		fm160_sched_note_result(status == AT_STATUS_OK,
					status == AT_STATUS_TIMEOUT);
	/* Silent requests (port probes) deliberately do not feed the circuit
	 * breaker: a wrong candidate port must not be counted as a modem fault.
	 * The probe callback decides what to do with the result. */

	if (status != AT_STATUS_OK)
		fm160_log(LOG_DEBUG, "AT '%s' -> %s (%s)", req->cmd,
			  status == AT_STATUS_ERROR ? "error" :
			  status == AT_STATUS_TIMEOUT ? "timeout" : "?", resp ? resp : "");

	if (req->cb)
		req->cb(req, status, resp, req->arg);

	inflight = NULL;
	at_req_free(req);
	atq_dispatch();
}

static void sendat_cb(struct ubus_request *ureq, int type, struct blob_attr *msg)
{
	struct at_req *req = ureq->priv;
	struct blob_attr *tb[ARRAY_SIZE(sendat_policy)];
	struct sendat_reply rep = { .status = NULL, .response = NULL };
	enum at_status status;
	const char *resp = NULL;

	(void)type;

	if (msg) {
		blobmsg_parse(sendat_policy, ARRAY_SIZE(sendat_policy), tb,
			      blob_data(msg), blob_len(msg));
		if (tb[0])
			rep.response = blobmsg_get_string(tb[0]);
		if (tb[1])
			rep.status = blobmsg_get_string(tb[1]);
		if (tb[2])
			rep.end_flag_matched = blobmsg_get_string(tb[2]);
		if (tb[3])
			rep.response_time_ms = blobmsg_get_u32(tb[3]);
	}

	resp = rep.response;

	if (!msg) {
		status = AT_STATUS_TIMEOUT;
	} else if (resp && strstr(resp, "ERROR")) {
		/* Classify on the text BEFORE trusting the transport's status.
		 *
		 * at-daemon's default end-flag list contains "ERROR" AND both
		 * "+CME ERROR:" and "+CME ERROR:", so it treats an error reply
		 * as a matched terminal flag, returns 0, and reports
		 * status="success".  "success" therefore means "the exchange
		 * finished", not "the modem accepted the command".
		 *
		 * This was not theoretical: with no SIM, AT+ICCID answers
		 * "+CME ERROR: 13" and the ident chain stored that string as
		 * the card's ICCID, which the UI then displayed.
		 *
		 * A false positive would need a response whose payload contains
		 * the literal word ERROR; fm160_resp_error() already makes the
		 * same assumption everywhere else, so this only moves the rule
		 * to the choke point. */
		status = AT_STATUS_ERROR;
	} else if (rep.status && !strcmp(rep.status, "success")) {
		status = AT_STATUS_OK;
	} else {
		status = AT_STATUS_TIMEOUT;
	}

	if (status == AT_STATUS_OK && rep.response_time_ms > g_state.at_busy_max_ms)
		g_state.at_busy_max_ms = rep.response_time_ms;

	atq_finish(req, status, resp);
}

/* ------------------------------------------------------------------ */
/* two-stage transactions (AT+CMGS / AT+CMGW)                           */
/* ------------------------------------------------------------------ */

/*
 * Outcome of the first stage: did the modem ask for the payload?
 *
 * `partial` keeps whatever it said instead.  A refused message answers with a
 * +CMS ERROR naming the reason, and that line is the only diagnosis available -
 * dropping it would turn "message too long" into "timeout" on the page.
 */
struct stage1 {
	struct at_req *req;
	bool seen;
	enum at_status status;
	char partial[FM160_RESP_LINE_MAX];
};

static void prompt_cb(struct ubus_request *ureq, int type, struct blob_attr *msg)
{
	struct stage1 *s = ureq->priv;
	struct blob_attr *tb[ARRAY_SIZE(sendat_policy)];
	const char *resp = NULL, *matched = NULL, *st = NULL;

	(void)type;
	s->status = AT_STATUS_TIMEOUT;
	s->partial[0] = '\0';

	if (msg) {
		blobmsg_parse(sendat_policy, ARRAY_SIZE(sendat_policy), tb,
			      blob_data(msg), blob_len(msg));
		if (tb[0])
			resp = blobmsg_get_string(tb[0]);
		if (tb[1])
			st = blobmsg_get_string(tb[1]);
		if (tb[2])
			matched = blobmsg_get_string(tb[2]);
	}
	if (resp && *resp)
		snprintf(s->partial, sizeof(s->partial), "%s", resp);

	if (resp && strstr(resp, "ERROR")) {
		s->status = AT_STATUS_ERROR;
		return;
	}
	if (matched && s->req->prompt[0] && !strcmp(matched, s->req->prompt)) {
		s->seen = true;
		s->status = AT_STATUS_OK;
		return;
	}
	/* "success" without our prompt means the modem terminated the command on
	 * its own terms - it answered instead of asking.  There is nothing to
	 * type into, so this is not a prompt. */
	if (st && !strcmp(st, "success"))
		s->status = AT_STATUS_ERROR;
}

/* One sendat round trip.  The reply, if any, reaches `handler` before this
 * returns, because ubus_invoke() is synchronous. */
static int sendat_call(const char *target, const char *field, const char *value,
		       int secs, const char *end_flag,
		       void (*handler)(struct ubus_request *, int,
				       struct blob_attr *),
		       void *priv, int wait_ms)
{
	struct blob_buf b = {};
	int ret;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "at_port", target);
	blobmsg_add_string(&b, field, value);
	blobmsg_add_u32(&b, "timeout", secs);
	if (end_flag && *end_flag)
		blobmsg_add_string(&b, "end_flag", end_flag);

	ret = ubus_invoke(g_ubus, at_daemon_id, "sendat", b.head, handler, priv,
			  wait_ms);
	blob_buf_free(&b);
	return ret;
}

static int atq_issue(struct at_req *req)
{
	const char *target;
	int secs, ret;

	if (!at_daemon_ready())
		return -EAGAIN;

	secs = (req->timeout_ms + 999) / 1000;
	if (secs < 1)
		secs = 1;

	/* A request that names its own port wins: that is how the port probe
	 * talks to a candidate that g_state.port does not describe yet.  Falling
	 * back to g_state.port is the normal path for everything else. */
	target = req->port[0] ? req->port : g_state.port;

	if (!req->payload_hex[0]) {
		ret = sendat_call(target, "at_cmd", req->cmd, secs,
				  req->end_flag, sendat_cb, req,
				  req->timeout_ms + 2000);
		if (ret) {
			/* Could not even hand it to ubus: treat as timeout. */
			fm160_log(LOG_WARN, "sendat invoke failed: %s",
				  ubus_strerror(ret));
			at_daemon_id = 0;
			return ret;
		}
		return 0;
	}

	/* --- stage 1: the command, up to the prompt ------------------- */
	{
		struct stage1 s;
		int psecs = (ATQ_PROMPT_TIMEOUT_MS + 999) / 1000;

		s.req = req;
		s.seen = false;
		s.status = AT_STATUS_TIMEOUT;
		s.partial[0] = '\0';

		ret = sendat_call(target, "at_cmd", req->cmd, psecs,
				  req->prompt[0] ? req->prompt : ">",
				  prompt_cb, &s, ATQ_PROMPT_TIMEOUT_MS + 2000);
		if (ret) {
			fm160_log(LOG_WARN, "sendat (prompt) invoke failed: %s",
				  ubus_strerror(ret));
			at_daemon_id = 0;
			atq_finish(req, AT_STATUS_TIMEOUT, NULL);
			return 0;
		}
		if (!s.seen) {
			fm160_log(LOG_DEBUG, "no prompt for '%s': %s", req->cmd,
				  s.partial[0] ? s.partial : "no answer");
			atq_finish(req, s.status,
				   s.partial[0] ? s.partial : NULL);
			return 0;
		}

		/* --- stage 2: the payload, then the real terminal -------
		 *
		 * Between these two calls the modem is holding the line open
		 * waiting for data.  Nothing else in fm160d can run in that
		 * gap: this function has not returned, so the queue still
		 * believes the request is in flight. */
		ret = sendat_call(target, "raw_at_content", req->payload_hex,
				  secs, req->end_flag, sendat_cb, req,
				  req->timeout_ms + 2000);
		if (ret) {
			fm160_log(LOG_WARN, "sendat (payload) invoke failed: %s",
				  ubus_strerror(ret));
			at_daemon_id = 0;
			atq_finish(req, AT_STATUS_TIMEOUT, NULL);
			return 0;
		}
		return 0;
	}
}

static void atq_dispatch(void)
{
	struct at_req *req, *best = NULL;

	if (inflight)
		return;

	list_for_each_entry(req, &q_head, list) {
		if (!best)
			best = req;
		else if (req->prio < best->prio)
			best = req;
	}
	if (!best)
		return;

	/* starvation guard: promote long-waiting background work  */
	if (best->prio == AT_PRIO_POLL &&
	    fm160_now_ms() - best->submit_ms > ATQ_STARVE_MS)
		best->prio = AT_PRIO_STATE;

	list_del(&best->list);

	/* Only drop work that has nowhere to go.  A request carrying its own port
	 * must be let through even while port_found is false -- the port probe is
	 * exactly that request, and it is the only thing that ever SETS
	 * port_found.  Rejecting it here (which is what this used to do
	 * unconditionally) meant every probe died with NOPORT before reaching
	 * at-daemon, so port discovery could never converge and the daemon sat in
	 * a 5 s rescan loop forever. */
	if (!best->port[0] && !g_state.port_found) {
		if (best->cb)
			best->cb(best, AT_STATUS_NOPORT, NULL, best->arg);
		free(best);
		atq_dispatch();
		return;
	}

	inflight = best;
	if (atq_issue(best))
		/* Could not send: fail it and try the next one. */
		atq_finish(best, AT_STATUS_TIMEOUT, NULL);
}

static bool atq_duplicate(const struct at_req *want)
{
	struct at_req *req;

	if (want->prio != AT_PRIO_POLL)
		return false;
	list_for_each_entry(req, &q_head, list) {
		if (req->prio == AT_PRIO_POLL && !strcmp(req->cmd, want->cmd))
			return true;
	}
	if (inflight && inflight->prio == AT_PRIO_POLL &&
	    !strcmp(inflight->cmd, want->cmd))
		return true;
	return false;
}

static int atq_submit_full(enum at_prio prio, const char *cmd,
			   const char *end_flag, int timeout_ms,
			   at_done_cb cb, void *arg, bool silent,
			   const char *override_port,
			   const char *prompt, const char *payload_hex)
{
	struct at_req *req;

	if (!g_state.enabled)
		return -EAGAIN;

	if (!override_port) {
		if (!g_state.port_found)
			return -EAGAIN;
		if (prio == AT_PRIO_POLL && atq_quiet_active())
			return -EAGAIN;
		if (prio == AT_PRIO_POLL && g_state.at_state == 2)
			return -EAGAIN;
	}

	req = calloc(1, sizeof(*req));
	if (!req)
		return -ENOMEM;

	snprintf(req->cmd, sizeof(req->cmd), "%s", cmd);
	if (end_flag)
		snprintf(req->end_flag, sizeof(req->end_flag), "%s", end_flag);
	if (payload_hex && *payload_hex) {
		/* Refuse rather than truncate: half a PDU is a message the modem
		 * would accept and send wrong. */
		if (strlen(payload_hex) >= sizeof(req->payload_hex)) {
			free(req);
			return -ENOSPC;
		}
		snprintf(req->payload_hex, sizeof(req->payload_hex), "%s",
			 payload_hex);
		snprintf(req->prompt, sizeof(req->prompt), "%s",
			 (prompt && *prompt) ? prompt : ">");
	}
	req->prio = prio;
	req->timeout_ms = timeout_ms > 0 ? timeout_ms : 3000;
	req->cb = cb;
	req->arg = arg;
	req->silent = silent;
	/* Recorded, not merely used as a gate.  The override used to be consulted
	 * only to decide whether to skip the port_found check, and then thrown
	 * away -- so the probe passed the gate and was still sent on
	 * g_state.port (empty), and failed. */
	if (override_port)
		snprintf(req->port, sizeof(req->port), "%s", override_port);
	req->id = next_id++;
	req->submit_ms = fm160_now_ms();

	if (atq_duplicate(req)) {
		free(req);
		return -EAGAIN;
	}

	if (atq_depth() >= ATQ_MAX_DEPTH) {
		/* Drop the youngest lowest-priority item to keep the queue bounded. */
		struct at_req *victim = NULL, *it;
		list_for_each_entry(it, &q_head, list)
			if (!victim || it->prio > victim->prio)
				victim = it;
		if (victim && victim->prio > req->prio) {
			list_del(&victim->list);
			free(victim);
		} else {
			free(req);
			return -ENOSPC;
		}
	}

	list_add_tail(&req->list, &q_head);
	atq_dispatch();
	return 0;
}

int atq_submit(enum at_prio prio, const char *cmd, const char *end_flag,
	       int timeout_ms, at_done_cb cb, void *arg)
{
	return atq_submit_full(prio, cmd, end_flag, timeout_ms, cb, arg,
			       false, NULL, NULL, NULL);
}

int atq_submit_prompt(enum at_prio prio, const char *cmd, const char *prompt,
		      const char *payload_hex, const char *end_flag,
		      int timeout_ms, at_done_cb cb, void *arg)
{
	/* With no payload this is an ordinary request; going through the
	 * two-stage path would wait for a prompt nobody is going to send. */
	if (!payload_hex || !*payload_hex)
		return atq_submit(prio, cmd, end_flag, timeout_ms, cb, arg);
	return atq_submit_full(prio, cmd, end_flag, timeout_ms, cb, arg,
			       false, NULL, prompt, payload_hex);
}

int atq_submit_silent(enum at_prio prio, const char *cmd, int timeout_ms)
{
	return atq_submit_full(prio, cmd, NULL, timeout_ms, NULL, NULL,
			       true, NULL, NULL, NULL);
}

int atq_probe_port(const char *port, at_done_cb cb, void *arg)
{
	return atq_submit_full(AT_PRIO_STATE, "AT", NULL, 2000, cb, arg,
			       true, port, NULL, NULL);
}

int atq_depth(void)
{
	struct at_req *req;
	int n = inflight ? 1 : 0;

	list_for_each_entry(req, &q_head, list)
		n++;
	return n;
}

/* Called when the ubus connection was lost: force a fresh object lookup. */
void at_daemon_hint_reconnect(void)
{
	at_daemon_id = 0;
	at_daemon_retry_ms = 0;
}

bool atq_quiet_active(void)
{
	return g_state.quiet_kind != QUIET_NONE &&
	       fm160_now_ms() < g_state.quiet_until_ms;
}

void atq_set_quiet(int kind, const char *reason, int seconds)
{
	uint64_t until = fm160_now_ms() + (uint64_t)seconds * 1000;

	if (atq_quiet_active() && g_state.quiet_kind == kind &&
	    g_state.quiet_until_ms > until)
		return;             /* keep the longest window of the same kind */

	g_state.quiet_kind = kind;
	g_state.quiet_until_ms = until;
	snprintf(g_state.quiet_reason, sizeof(g_state.quiet_reason), "%s",
		 reason ? reason : "");
	fm160_log(LOG_INFO, "quiet window: %s for %d s", g_state.quiet_reason,
		  seconds);
	fm160_state_mark_dirty();
}

void atq_clear_quiet(void)
{
	if (!g_state.quiet_kind)
		return;
	g_state.quiet_kind = QUIET_NONE;
	g_state.quiet_until_ms = 0;
	g_state.quiet_reason[0] = '\0';
	fm160_state_mark_dirty();
}

void atq_state_reset(void)
{
	struct at_req *req, *tmp;

	list_for_each_entry_safe(req, tmp, &q_head, list) {
		list_del(&req->list);
		if (req->cb)
			req->cb(req, AT_STATUS_NOPORT, NULL, req->arg);
		free(req);
	}
}

int atq_init(void)
{
	g_state.at_state = 0;
	g_state.consec_timeout = 0;
	fm160_log(LOG_DEBUG, "atq initialised");
	return 0;
}

/* ------------------------------------------------------------------ */
/* response helpers                                                     */
/* ------------------------------------------------------------------ */

bool fm160_resp_error(const char *resp)
{
	if (!resp)
		return true;
	return strstr(resp, "ERROR") != NULL;
}

bool fm160_resp_ok(const char *resp)
{
	if (!resp)
		return false;
	if (fm160_resp_error(resp))
		return false;
	/* The terminal "OK" must be present as a line of its own. */
	return strstr(resp, "OK") != NULL;
}

bool fm160_resp_find(const char *resp, const char *prefix,
		     char *out, size_t outlen)
{
	const char *p = resp;
	size_t plen = strlen(prefix);

	out[0] = '\0';
	if (!resp)
		return false;

	while ((p = strstr(p, prefix)) != NULL) {
		const char *v = p + plen;

		/* Must be at a line start (start of buffer or right after \n). */
		if (p != resp && p[-1] != '\n') {
			p++;
			continue;
		}
		/* And the next char must not continue the token. */
		if (*v != ':' && *v != ' ' && *v != '\t' && *v != '\r' &&
		    *v != '\n' && *v != '\0') {
			p++;
			continue;
		}
		while (*v == ':' || *v == ' ' || *v == '\t')
			v++;
		{
			size_t n = strcspn(v, "\r\n");
			if (n >= outlen)
				n = outlen - 1;
			memcpy(out, v, n);
			out[n] = '\0';
		}
		return true;
	}
	return false;
}

int fm160_resp_lines(const char *resp, const char *prefix,
		     char out[][FM160_STR_MAX], int max)
{
	const char *p = resp;
	int n = 0;
	size_t plen = strlen(prefix);

	if (!resp)
		return 0;

	while (n < max && (p = strstr(p, prefix)) != NULL) {
		const char *v;
		size_t len;

		if (p != resp && p[-1] != '\n') {
			p++;
			continue;
		}
		v = p + plen;
		if (*v != ':' && *v != ' ' && *v != '\t' && *v != '\r' &&
		    *v != '\n' && *v != '\0') {
			p++;
			continue;
		}
		while (*v == ':' || *v == ' ' || *v == '\t')
			v++;
		len = strcspn(v, "\r\n");
		if (len >= FM160_STR_MAX)
			len = FM160_STR_MAX - 1;
		memcpy(out[n], v, len);
		out[n][len] = '\0';
		n++;
		p = v + len;
	}
	return n;
}
