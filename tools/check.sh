#!/bin/sh
# Every verification gate this checkout carries, in one command.
#
#   sh tools/check.sh                        # everything the host can run
#   LUCI=/path/to/luci sh tools/check.sh     # ... and the translation gates
#
# Why this exists
# ---------------
# Each failure it catches is one that no build step reports:
#
#   * a LuCI view with a missing bracket is a blank page in the browser and
#     nothing else -- there is no compile step for the front end;
#   * an init script with CRLF line endings installs perfectly and then never
#     starts, because the kernel reads the interpreter as "/bin/sh ...\r";
#   * a .po whose msgids are not whitespace-canonical compiles to a .lmo whose
#     keys nothing ever looks up, so the UI stays English and the file looks
#     fine;
#   * a fm160d function declared in the header and called from six modules but
#     defined in none of them compiles cleanly and fails only at link time;
#   * a diagnostics bundle whose buffer is one byte short is refused on the
#     device and looks, from the page, like a feature that does not work.
#
# The gates live under tools/ and are each meant to be runnable alone; this
# script only orchestrates them so that CI, and a human, have one command.
# tools/cccheck/check.sh already runs jscheck and eolcheck at the end, which is
# why they are not repeated here.
#
# What is NOT proven: that anything works on a device.  There is no substitute
# for the hardware; see docs/AT-FACTS.md for what was verified on the module.
#
# Environment:
#   LUCI=<dir>   a luci checkout (or the luci feed) -- enables the sections of
#                the translation check that need the real cbi.js and the
#                upstream catalogue.  Without it those sections report SKIPPED.
#   NODE=<bin>   node, for the front-end and hash checks
#   PY=<bin>     python 3, for the translation check
#   ZIG=<bin>    zig, for the C check
#   CC=<bin>     a C compiler for the host-side test under tools/hosttest;
#                searched for as cc/gcc/clang when unset
#   STRICT=1     treat a skipped gate as a failure.  Set this in CI, where
#                every toolchain is supposed to be present: a gate that could
#                not run has proved nothing, and "SKIPPED" scrolling past in a
#                green build is the one outcome worse than a red one.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)

# The interpreters this script spawns are native binaries, and an MSYS path
# handed to one resolves against the current drive (`/c/Users/x` -> `C:\c\...`),
# so every path that crosses that boundary goes through wpath().  wpath() is
# the identity on POSIX, which is exactly why it is shared rather than repeated.
# shellcheck source=lib/tooling.sh
. "$ROOT/tools/lib/tooling.sh"

fail=0
skipped=

note() { printf '\n=== %s\n' "$1"; }
warn_skip() {
	printf '  SKIPPED: %s\n' "$1"
	skipped="$skipped $2"
	[ -n "${STRICT:-}" ] && fail=1
	return 0
}

# A gate can also run and still leave something unproven: the translation check
# prints 66 assertions and then reports SKIPPED for the two sections that need
# an upstream luci tree.  That is not a gate that did not run, and naming it in
# the "gates not run" line beside a real omission (the host-side C test) reads
# as though the 66 assertions never happened -- the same disappearance the
# header warns about.  STRICT still fails: a section that could not run has
# proved nothing either, it is just reported as what it is.
note_skip() {
	printf '  SKIPPED: %s\n' "$1"
	[ -n "${STRICT:-}" ] && fail=1
	return 0
}

# --- the C, symbol, EOL and front-end gates ------------------------------
note "tools/cccheck/check.sh"
if sh "$HERE/cccheck/check.sh"; then
	:
else
	rc=$?
	if [ "$rc" = 2 ]; then
		# 2 is the script's own "no compiler found", which is a host limitation
		# and not a defect in the tree.
		warn_skip "no zig (run 'pip install ziglang' or set ZIG)" cccheck
	else
		fail=1
	fi
fi

# --- the host-side gate for the diagnostics bundle -----------------------
# It is a compile-and-run test of diag.c, which is the file that turns the
# daemon's state into the text somebody pastes into a bug report.  It needs a
# Linux host (see the header of the test) and is therefore the one gate that a
# Windows workstation cannot run - reported as SKIPPED rather than failed, and
# failed under STRICT, which is what CI sets.
note "tools/hosttest/diag-export-test.sh"
HOST_CC=
for cand in "${CC:-}" cc gcc clang; do
	[ -n "$cand" ] || continue
	if command -v "$cand" >/dev/null 2>&1; then
		HOST_CC=$cand
		break
	fi
done
if [ -z "$HOST_CC" ]; then
	warn_skip "no C compiler (set CC= or install gcc)" hosttest
else
	# PY is passed through when the caller pinned one, so the reply-key section
	# inside the test uses the same interpreter as everything else here.
	if [ -n "${PY:-}" ]; then
		CC="$HOST_CC" PY="$PY" sh "$HERE/hosttest/diag-export-test.sh" || fail=1
	else
		CC="$HOST_CC" sh "$HERE/hosttest/diag-export-test.sh" || fail=1
	fi
fi

# --- the translation gates ----------------------------------------------
note "tools/i18n/check.py"

LUCI_ARG=
PO2LMO=
if [ -z "${LUCI:-}" ]; then
	# The check still runs, and still reports SKIPPED for the two sections that
	# need an upstream tree; it just has nothing to compare against.
	note_skip "set LUCI=<luci checkout> to enable the cbi.js and collision checks"
else
	LUCI_ARG=$(wpath "$LUCI")
	# The end-to-end section needs a po2lmo, and the only trustworthy one is
	# built from the same luci source being compared against.
	SRC=
	for cand in "$LUCI/modules/luci-base/src" "$LUCI/feeds/luci/modules/luci-base/src"; do
		[ -d "$cand" ] && SRC=$cand && break
	done
	if [ -n "$SRC" ]; then
		if sh "$HERE/i18n/build-po2lmo.sh" "$SRC" >/dev/null 2>&1; then
			PO2LMO="$ROOT/_tmp/i18n/po2lmo"
			[ -n "${MSYSTEM:-}" ] && PO2LMO="$PO2LMO.exe"
		else
			note_skip "po2lmo did not build"
		fi
	else
		note_skip "no modules/luci-base/src under $LUCI"
	fi
fi

set -- "$(wpath "$HERE/i18n/check.py")"
[ -n "$LUCI_ARG" ] && set -- "$@" --luci "$LUCI_ARG"
[ -n "$PO2LMO" ] && set -- "$@" --po2lmo "$(wpath "$PO2LMO")"
[ -n "${NODE:-}" ] && set -- "$@" --node "$(wpath "$NODE")"

if [ -n "${PY:-}" ]; then
	"$PY" "$@"
else
	python3 "$@"
fi
[ $? = 0 ] || fail=1

# --- summary -------------------------------------------------------------
printf '\n=================================================\n'
if [ -n "$skipped" ]; then
	printf 'tools/check.sh: gates not run:%s\n' "$skipped"
fi
if [ "$fail" = 0 ]; then
	echo "tools/check.sh: clean"
else
	echo "tools/check.sh: FAILED"
fi
exit $fail
