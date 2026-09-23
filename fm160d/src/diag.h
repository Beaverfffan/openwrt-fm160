/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * diag - the support bundle: one plain-text report of what the daemon knows.
 *
 * Same contract as pdu.c, usbmode.c and net.c, and for the same reason: this is
 * a pure transformation with no I/O, no globals and no clock of its own, so the
 * host-side test can drive it.  Everything it needs arrives in the input struct
 * and everything it produces goes into a caller-supplied buffer.
 *
 * Why it is text and not a blob
 * -----------------------------
 * The bundle exists to be pasted into a bug report or attached to a forum post.
 * A blobmsg tree would be more convenient for the LuCI page and useless for
 * that, because the person reading it is usually not the person who has the
 * page open.  The ubus reply therefore carries this text, verbatim, as one
 * string, and the page offers it as a download and as a copyable block.
 *
 * Why the caller passes the clock
 * -------------------------------
 * Every timestamp in here - the wall clock, the uptime, the age of each log
 * line - is an input rather than a call to time().  That is what makes the
 * output byte-for-byte reproducible, and a report whose text cannot be
 * reproduced cannot be asserted on.  The formatter never calls fm160_now_ms()
 * either, so it links into a test binary that has no daemon behind it.
 *
 * Why it returns a length instead of writing as it goes
 * ----------------------------------------------------
 * The bundle is assembled into one buffer and returned whole, because half a
 * report is worse than none: a truncated bundle looks complete, and the reader
 * has no way to tell which sections are missing.  Formatted length is returned,
 * so the caller can prove the buffer was big enough (>= 0) or grow it (-1)
 * instead of shipping something that ends mid-sentence.
 */

#ifndef FM160_DIAG_H
#define FM160_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Defined in fm160d.h.  Forward-declared rather than included: fm160d.h
 * includes this header, so the dependency has to point the other way. */
struct fm160_state;
struct fm160_log_rec;

/*
 * Everything the bundle is made of, gathered by the caller.
 *
 * This is deliberately NOT struct fm160_state plus a few extras: the two
 * scalars that do not live in the state (the AT queue depth and the clock) are
 * cheap to pass, and keeping the formatter's input to a pointer means a caller
 * that forgets to fill something gets a compile error rather than a zero.
 */
struct fm160_diag_in {
	const struct fm160_state *st;

	/*
	 * The log ring, oldest first, as fm160_log_ring_at() walks it.
	 *
	 * Borrowed pointers rather than a copy of the records: the ring lives in
	 * main.c and a full copy would be 128 * sizeof(struct fm160_log_rec) of
	 * static RAM held permanently for a report that is produced when someone
	 * presses a button.  The array of pointers costs 1 KiB and is filled in
	 * the loop that already has to walk the ring.
	 *
	 * The pointers must stay valid for the duration of the call, which here
	 * means nothing may log while the bundle is being formatted.  Nothing
	 * does - the formatter has no I/O and no state - but it is why this is
	 * an explicit contract rather than an implementation detail.
	 */
	const struct fm160_log_rec *const *log;
	size_t log_n;
	/* Lines ever logged.  total > n means the ring wrapped, and the bundle
	 * says so: a report that quietly shows the last 128 of 4 000 lines
	 * invites the reader to conclude that nothing happened earlier. */
	uint64_t log_total;

	/* atq_depth(): how many AT requests are waiting.  A snapshot taken while
	 * the queue is deep is a snapshot of a busy daemon, and several of the
	 * "stale" fields below mean something different in that case. */
	int atq_depth;

	/* fm160_now_ms(), and the same clock read at startup.  uptime_ms is
	 * passed separately rather than assumed to be now_ms, because after a
	 * clock step (NTP, or the RTC being set once the modem has time) the
	 * difference is not uptime at all. */
	uint64_t now_ms;
	uint64_t uptime_ms;

	/* The wall clock the caller formatted (strftime, "%Y-%m-%d %H:%M:%S %z").
	 * May be NULL: a router that has never reached NTP has a 1970 clock and
	 * printing it as fact would be worse than printing nothing. */
	const char *wall;

	/* Effective log level, so an absent DEBUG line is explained. */
	int log_level;
	const char *version;
};

/*
 * Format the bundle into buf.  Returns the number of bytes written, excluding
 * the terminating NUL, or -1 when buf is too small.
 *
 * buf is always NUL-terminated on success.  buflen == 0 returns -1.
 */
long fm160_diag_format(const struct fm160_diag_in *in, char *buf, size_t buflen);

/*
 * Worst-case size of one bundle, for sizing the caller's buffer.
 *
 * The state sections above the log print a fixed set of keys, but not a fixed
 * number of bytes: every one of them may hold a string, and the longest of them
 * (an ICCID, an operator name, a dial error, an APN) is FM160_STR_MAX or larger.
 * MAXED_OUT is what the sum comes to when every one of those strings is at its
 * limit - which the host-side test constructs on purpose, because a bundle that
 * does not fit is refused rather than truncated and a refused bundle is a
 * support case with no evidence in it.
 *
 *   port 64, manufacturer/model/revision/imei/sn/iccid and oper at 96,
 *   pin_status 32, quiet_reason 96, netdev 32, apn 100, addr/dns1/dns2 at 64,
 *   dial.last_error and modesw.last_error at 96            ~ 1.3 KiB of strings
 *   the ~90 key lines and their labels                     ~ 2.2 KiB
 *   the log section's own header                           ~ 0.1 KiB
 *
 * MEASURED by the host-side test, with every string at its limit AND all 128 log
 * slots holding maximum-length truncated lines: 32 192 of the 34 816 bytes below.
 * The number is not left as an estimate because the cost of being wrong is
 * asymmetric - an oversized static buffer is 6 KiB of a 2 GiB machine, while an
 * undersized one turns the feature OFF exactly when the daemon is busiest, which
 * is when the strings are longest.  The test asserts the fit, so a section added
 * here without revisiting this number fails the gate rather than a device.
 *
 * The log section is one remembered line each, at most FM160_LOG_LINE_MAX bytes
 * of text plus a "[TRUNCATED]" marker and the age/priority prefix -
 * FM160_DIAG_LOG_LINE_MAX below.  FM160_DIAG_BUF_MAX itself is defined in
 * fm160d.h, because the ring constants it is built from live there.
 */
#define FM160_DIAG_HEAD_MAX     6144
#define FM160_DIAG_LOG_LINE_MAX (FM160_LOG_LINE_MAX + 32)

#endif /* FM160_DIAG_H */
