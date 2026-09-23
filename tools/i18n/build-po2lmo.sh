#!/bin/sh
# Build luci's po2lmo outside an OpenWrt tree, so that a .po can be compiled to
# a .lmo -- and therefore inspected -- before anything is built or flashed.
#
#   sh tools/i18n/build-po2lmo.sh <luci-base/src> [<out>]
#
# <luci-base/src> is the modules/luci-base/src directory of a luci checkout, in
# the tree or in a feed (feeds/luci/modules/luci-base/src).  <out> defaults to
# _tmp/i18n/po2lmo[.exe].
#
# Why bother
# ----------
# po2lmo is the only thing that can answer "will this .po actually produce a
# working .lmo?".  It is also one of the three implementations that make up the
# lookup path, and the three disagree in one specific way that no static check
# can see (see tools/i18n/check.py, section "writer/reader"): po2lmo hashes the
# msgid verbatim while both readers -- lmo.c's lmo_canon_hash() and cbi.js's
# sfh(trimws(s)) -- collapse whitespace first.  A .po whose msgids are not
# already whitespace-canonical therefore translates nothing, silently, and
# building the .lmo and comparing key hashes is the only way to see it.
#
# Two build paths
# ---------------
# POSIX host (the build machine): plain cc, no shim, exactly what OpenWrt does.
#
# Windows host: lib/lmo.c is POSIX -- it includes <sys/mman.h>, <fnmatch.h> and
# <arpa/inet.h>, none of which exist here -- but all three are used *only* by
# lmo_open(), the catalogue reader, and po2lmo is the writer and never calls it.
# po2lmo-host/shim supplies the missing headers, aborting stubs for the dead
# calls, and one real implementation (fsync -> _commit) for the call that is
# not dead.  See po2lmo-host/shim/fm160_shim.h.
#
# The one flag that is not optional
# ---------------------------------
# lmo.c:50 is `hash ^= (signed char)data[sizeof(uint16_t)] << 18;` -- a left
# shift of a negative value, undefined behaviour in C, reached on any msgstr
# containing a non-ASCII (i.e. any Chinese) byte in that tail position.  zig's
# runtime checks trap on it:
#
#     thread panic: left shift of negative value -126
#     lmo.c:50: in sfh_hash, called from po2lmo.c:176
#
# gcc/clang built without those checks do what the hardware does, which is what
# the JS reader's `s8(bytes, off+2) << 18` does too, so upstream is consistent
# in practice.  The sanitizer is disabled here to match the real build -- not
# because the code is fine.
set -e

LUCI_SRC=${1:-}
OUT=${2:-}

if [ -z "$LUCI_SRC" ]; then
	echo "usage: sh $0 <luci-base/src> [<out>]" >&2
	echo "  e.g. sh $0 /mnt/data4t/istoreos-h69k/feeds/luci/modules/luci-base/src" >&2
	exit 2
fi

for f in po2lmo.c lib/lmo.c lib/lmo.h lib/plural_formula.y contrib/lemon.c contrib/lempar.c; do
	[ -f "$LUCI_SRC/$f" ] || { echo "missing $LUCI_SRC/$f" >&2; exit 2; }
done

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
# shellcheck source=../lib/tooling.sh
. "$ROOT/tools/lib/tooling.sh"

SHIM=$HERE/po2lmo-host/shim
CACHE=$ROOT/_tmp/i18n/build

if [ -z "$OUT" ]; then
	OUT=$ROOT/_tmp/i18n/po2lmo
	[ -n "${MSYSTEM:-}" ] && OUT=$OUT.exe
fi

rmrf "$CACHE"
mkdir -p "$CACHE/obj" "$CACHE/lib" "$(dirname "$OUT")"
cp "$LUCI_SRC/po2lmo.c"              "$CACHE/"
cp "$LUCI_SRC/lib/lmo.c"             "$LUCI_SRC/lib/lmo.h" \
   "$LUCI_SRC/lib/plural_formula.y"  "$CACHE/lib/"
cp "$LUCI_SRC/contrib/lemon.c"       "$CACHE/lemon.c"
cp "$LUCI_SRC/contrib/lempar.c"      "$CACHE/lempar.c"

# --- pick a compiler -------------------------------------------------------
# A POSIX cc is preferred because it is what the real build uses; the shim only
# exists to make the Windows host possible.
NATIVE=0
if command -v cc >/dev/null 2>&1 &&
   printf '#include <sys/mman.h>\n#include <fnmatch.h>\nint main(void){return 0;}\n' > "$CACHE/probe.c" &&
   cc -o "$CACHE/probe" "$CACHE/probe.c" 2>/dev/null; then
	NATIVE=1
fi

if [ "$NATIVE" = 1 ]; then
	echo "build-po2lmo: native cc (POSIX host, no shim)"
	CC="cc"
	INC=
	EXTRA=
	OUT_ARG=$OUT
else
	find_zig || exit 2
	zig_setup_cache "$CACHE/.zigcache"
	echo "build-po2lmo: zig cc + POSIX shim"
	CC="$ZIG cc"
	INC="-I$(wpath "$SHIM")"
	EXTRA="-O2 -fno-sanitize=undefined -fno-sanitize=shift"
	OUT_ARG=$(wpath "$OUT")
fi

# --- lemon turns lib/plural_formula.y into the plural-rule parser -----------
# Everything runs from $CACHE with relative paths: an absolute source path
# makes a Windows zig lose the file when it writes intermediates.
cd "$CACHE"
# shellcheck disable=SC2086
$CC $EXTRA -std=gnu17 -o lemon lemon.c
./lemon -q lib/plural_formula.y
[ -f lib/plural_formula.c ] || { echo "lemon produced no plural_formula.c" >&2; exit 1; }

# --- compile and link ------------------------------------------------------
# shellcheck disable=SC2086
$CC -DNDEBUG $INC $EXTRA -c -o obj/lmo.o            lib/lmo.c
# shellcheck disable=SC2086
$CC -DNDEBUG $INC $EXTRA -c -o obj/plural_formula.o lib/plural_formula.c
# shellcheck disable=SC2086
$CC -DNDEBUG $INC $EXTRA -c -o obj/po2lmo.o         po2lmo.c

if [ "$NATIVE" = 1 ]; then
	# shellcheck disable=SC2086
	$CC $EXTRA -o "$OUT_ARG" obj/po2lmo.o obj/lmo.o obj/plural_formula.o
else
	cp "$SHIM/shim.c" "$CACHE/shim.c"
	# shellcheck disable=SC2086
	$CC -DNDEBUG $INC $EXTRA -c -o obj/shim.o shim.c
	# shellcheck disable=SC2086
	$CC $EXTRA -o "$OUT_ARG" obj/po2lmo.o obj/lmo.o obj/plural_formula.o obj/shim.o
fi

echo "build-po2lmo: $OUT"
ls -la "$OUT"

# --- self-test ---------------------------------------------------------------
# The compiler that was just used is the compiler that will build po2lmo, so
# prove it opens files in binary mode before trusting anything it produces.
# Not a hypothetical: a text-mode fopen silently rewrites every 0x0A byte in
# the .lmo index as 0x0D 0x0A, and po2lmo still exits 0.  Checking "the index
# is a whole number of 16-byte records" catches it downstream; this catches it
# here, deterministically, without depending on a real .po happening to hash to
# a value containing 0x0A.
mkdir -p "$CACHE/selftest"
printf '#include <stdio.h>\nint main(void){FILE*f=fopen("probe.out","w");int i;' > "$CACHE/selftest/probe.c"
printf 'if(!f)return 1;for(i=0;i<3;i++)fputc(0x0A,f);fclose(f);return 0;}\n' >> "$CACHE/selftest/probe.c"
cd "$CACHE/selftest"
if [ "$NATIVE" = 1 ]; then
	# shellcheck disable=SC2086
	$CC $EXTRA -o probe probe.c
else
	cp "$SHIM/shim.c" probe_shim.c
	# shellcheck disable=SC2086
	$CC -DNDEBUG $INC $EXTRA -o probe.exe probe.c probe_shim.c
fi
./probe* 2>/dev/null || ./probe.exe
SIZE=$(wc -c < probe.out)
if [ "$SIZE" != 3 ]; then
	echo "build-po2lmo: SELF-TEST FAILED - three 0x0A bytes came out as $SIZE bytes." >&2
	echo "  fopen() is in text mode, so every .lmo built here would have a corrupt index." >&2
	echo "  On Windows this means po2lmo-host/shim/stdio.h is not being picked up;" >&2
	echo "  check that -I points at it and that the shim was copied in." >&2
	exit 1
fi
echo "build-po2lmo: self-test ok (fopen is binary: 3 bytes out for 3 bytes in)"
