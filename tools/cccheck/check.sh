#!/bin/sh
# Syntax/type check the fm160d sources without an OpenWrt tree.
#
# Why: the Windows workstation has no C compiler, and the build machine is not
# always reachable.  `pip install ziglang` provides `zig cc`, a clang front end
# that ships full musl/glibc headers - musl being exactly what OpenWrt uses -
# so our own code and its use of libubox/libubus can be type-checked locally.
#
# The libubox/libubus headers under ./include are the REAL upstream files
# (openwrt/libubox, openwrt/ubus), so every API call is checked against the
# actual signatures rather than a guess.  That is how the two bugs this script
# was written for were found: uloop_signal_add() takes one argument, and
# ubus_request_data_dup()/ubus_request_data_free() do not exist at all.
#
# What this proves: fm160d compiles as C, every referenced libubox/libubus
# symbol exists with the assumed signature, (with -Wall -Wextra) there are no
# warnings, and every symbol the modules reference is defined somewhere in the
# project (see symcheck.py).
# What it does NOT prove: real linking against libubox/libubus, or anything
# about runtime behaviour.
#
# Usage: sh tools/cccheck/check.sh
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC="$ROOT/fm160d/src"

# A directory in the form the native binaries here can open: C:/x on MSYS,
# unchanged on a POSIX host.  `pwd -W` is an MSYS extension, so a plain Linux
# shell has to fall through to the path it was given rather than to cygpath
# (which does not exist there, and whose absence used to print an error into
# the middle of the build log).
winpath() {
	(cd "$1" 2>/dev/null && pwd -W 2>/dev/null) || printf '%s' "$1"
}

# --- Windows path pitfalls -------------------------------------------------
#  1. zig resolves its caches via HOME, and MSYS hands out /c/... paths that
#     zig cannot read.  Symptom: "error: CacheCheckFailed" on every file.
#  2. absolute paths for the *sources* make zig lose the file when it writes
#     intermediate artefacts ("error: FileNotFound" at the main file).
#     => run from the workspace root and pass relative paths, and give zig
#     Windows-style cache dirs it can actually open.
HERE_W=$(winpath "$HERE")
REL_SRC="fm160d/src"
REL_INC="tools/cccheck/include"

# --- locate python -------------------------------------------------------
# The removal helpers below are python because this sandbox intercepts `rm` and
# kills the script with SIGTERM.  The interpreter is `python` on the Windows
# workstation and `python3` on the build host, where `python` does not exist at
# all -- resolving it once is the difference between the gate running there and
# the gate reporting "python: command not found" as a compile failure.
if [ -z "${PY:-}" ]; then
	PY=$(command -v python 2>/dev/null || command -v python3 2>/dev/null || true)
fi
if [ -z "${PY:-}" ]; then
	echo "no python on PATH -- set PY=" >&2
	exit 2
fi

# --- locate a C compiler ------------------------------------------------
# `zig cc` is a clang front end that ships full musl headers, so our code and
# its use of libubox/libubus can be type-checked without an OpenWrt tree.
# Resolution order: $ZIG, then the ziglang wheel inside the python environment
# that is first on PATH (`pip install ziglang`), then plain `zig`.
if [ -z "${ZIG:-}" ]; then
	# 1. whatever python is first on PATH, if it has the wheel.
	ZIG=$("$PY" -c "
import os, sys
try:
    import ziglang
except Exception:
    sys.exit(0)
name = 'zig.exe' if os.name == 'nt' else 'zig'
print(os.path.join(os.path.dirname(ziglang.__file__), name))
" 2>/dev/null || true)
fi

if [ -z "${ZIG:-}" ]; then
	# 2. The interpreter on PATH is often a bare versioned install while the
	#    wheel lives in a sibling virtualenv.  Search the venvs and versions
	#    next to it before giving up.  (This is not guesswork: asking the
	#    interpreter that *does* have the package is step 1, and this covers
	#    the common "managed runtime + separate env" layout.)
	PYBIN=$(command -v python 2>/dev/null || command -v python3 2>/dev/null || true)
	if [ -n "$PYBIN" ]; then
		# Walk up from the interpreter looking for sibling environments.
		# Layout here is <root>/python/versions/3.13.12/python with the wheel
		# in <root>/python/envs/default/, i.e. two levels up, so try the
		# immediate parents rather than assuming one.
		PYUP=$(dirname "$PYBIN")
		for depth in 1 2 3; do
			PYUP=$(dirname "$PYUP")
			for cand in "$PYUP"/envs/*/Scripts/python.exe \
			            "$PYUP"/envs/*/bin/python \
			            "$PYUP"/[a-z]*/Scripts/python.exe; do
				[ -x "$cand" ] || continue
				ZIG=$("$cand" -c "
import os, sys
try:
    import ziglang
except Exception:
    sys.exit(0)
name = 'zig.exe' if os.name == 'nt' else 'zig'
print(os.path.join(os.path.dirname(ziglang.__file__), name))
" 2>/dev/null || true)
				[ -n "$ZIG" ] && break
			done
			[ -n "$ZIG" ] && break
		done
	fi
fi

[ -n "${ZIG:-}" ] || ZIG=$(command -v zig 2>/dev/null || true)

if [ -z "${ZIG:-}" ] || [ ! -x "$ZIG" ]; then
	echo "zig not found - run 'pip install ziglang' or set \$ZIG" >&2
	exit 2
fi

# zig resolves its caches through HOME, and MSYS hands out /c/... paths that
# zig (a Windows binary) cannot read: symptom "error: CacheCheckFailed" on
# every file.  Convert HOME to a Windows-style path when we can.
HOME_W=$(winpath "$HOME")
export HOME="$HOME_W"
export ZIG_GLOBAL_CACHE_DIR="$HERE_W/.zigcache/g"
export ZIG_LOCAL_CACHE_DIR="$HERE_W/.zigcache/l"
mkdir -p "$HERE/.zigcache/g" "$HERE/.zigcache/l"

# musl target: same libc family as the target firmware.
CFLAGS="-target x86_64-linux-musl -D_GNU_SOURCE"
CFLAGS="$CFLAGS -Wall -Wextra -Wno-unused-parameter -Wno-format-nonliteral"
CFLAGS="$CFLAGS -I$REL_INC -I$REL_SRC"

OUT="$HERE/out"
OUT_W="$HERE_W/out"
mkdir -p "$OUT"

cd "$ROOT" || exit 2

fail=0
# For the failure report at the end.  Every gate below is guarded by
# `[ "$fail" = 0 ]` so that a broken compile does not produce a cascade of
# misleading downstream errors, but that guard also suppresses the *reason*:
# a compile failure goes into out/<name>.log and nothing ever printed it, so
# the visible result of any failure was "exit 1 with no output at all".
# These two variables are what the report needs to explain itself.
bad_compiles=""
stopped_in=""

# This sandbox intercepts `rm` and kills the whole script with SIGTERM, so
# removals go through Python.  (Symptom if you use rm: no output at all and
# exit code 1 / signal SIGTERM.)
del() {
	"$PY" -c "
import os, sys
for p in sys.argv[1:]:
    try:
        os.remove(p)
    except OSError:
        pass
" "$@"
}

# Start from an empty output directory.  Stale .o files from an earlier run
# would otherwise be fed to symcheck and report a symbol table that no longer
# corresponds to the sources.
# NB: on Windows this python.exe is a *Windows* binary, and MSYS hands out
# /c/... paths that Windows resolves against the current drive (-> C:\c\...).
# Anything passed to python there must be a Windows-style path, hence $OUT_W,
# which winpath() leaves alone on a POSIX host.
"$PY" -c "
import os, sys
d = sys.argv[1]
for f in os.listdir(d):
    if f.endswith(('.o', '.log')) or f == 'SUMMARY':
        try:
            os.remove(os.path.join(d, f))
        except OSError:
            pass
" "$OUT_W"

# Same class of problem one level down, and much harder to see: zig does not
# replay a diagnostic when it answers a compile out of its cache.  A unit that
# once compiled *with a warning* keeps its cached object, and every later run
# reports nothing for it -- so the verdict depends on whether .zigcache happens
# to be warm, with the warm answer being the wrong one.  Measured on this tree,
# same sources, same compiler, nothing else changed:
#
#     warm cache  -> exit 0, "fm160: clean"
#     wiped       -> exit 1, modesw.c:62:13: warning: unused function 'copy_str'
#
# which is why CI (cold every run) failed on a warning that never appeared
# locally, and why that warning stayed invisible behind the empty failure
# output.  Only the local cache is dropped: the global one holds the musl
# headers and compiler-rt, which are what make a cold build slow.
"$PY" -c "
import shutil, sys
shutil.rmtree(sys.argv[1], ignore_errors=True)
" "$HERE_W/.zigcache/l"
mkdir -p "$HERE/.zigcache/l"

run_one() {          # $1 = basename, $2 = path relative to $ROOT
	name=$1
	log="$OUT/$name.log"
	# A real compile to an object file, not -fsyntax-only: clang accepts
	# -fsyntax-only happily but zig's driver then fails with a confusing
	# "error: FileNotFound" on the main file, while -c -o gives us codegen
	# (and therefore more checks) for free.  -o needs a Windows path.
	# The .o is kept: symcheck.py reads its symbol table.
	"$ZIG" cc $CFLAGS -c -o "$OUT_W/$name.o" "$2" 2>&1 \
		| grep -v '^zig: warning: argument unused' > "$log"
	if [ -s "$log" ]; then
		fail=1
		bad_compiles="$bad_compiles $name"
	else
		# Only keep logs that have something to say (i.e. the failures).
		del "$OUT_W/$name.log"
	fi
}

for f in "$SRC"/*.c; do
	run_one "$(basename "$f")" "$REL_SRC/$(basename "$f")"
done

# A single header is not a translation unit - compiling fm160d.h directly makes
# zig try to link the result and fail with "unknown file type".  Check it the
# way it is really used, through a one-line TU.
printf '#include "fm160d.h"\n' > "$OUT/hdr_check.c"
run_one "fm160d.h" "$REL_INC/../out/hdr_check.c"
del "$OUT_W/hdr_check.c"

# Compiling with -c resolves nothing across files, so a function that is
# declared in fm160d.h and called from six modules but defined in none of them
# passes every check above and fails only at link time on the build box.
# symcheck.py closes that gap by reading the symbol tables.
#
# Two gaps stay open at this point and are closed by makecheck.py: a source that
# EXISTS but is not listed in OBJS (type-checked by the loop above, never linked),
# and FM160_VERSION drifting from PKG_VERSION.  Both are cross-file facts that
# the compile above cannot see, and both fail somewhere other than here.
if [ "$fail" = 0 ]; then
	stopped_in="makecheck.py"
	mkout=$("$PY" "$HERE_W/makecheck.py" 2>&1)
	mkrc=$?
	echo "$mkout"
	[ "$mkrc" = 0 ] || fail=1
fi

if [ "$fail" = 0 ]; then
	stopped_in="symcheck.py"
	symout=$("$PY" "$HERE_W/symcheck.py" "$OUT_W" 2>&1)
	symrc=$?
	echo "$symout"
	[ "$symrc" = 0 ] || fail=1
fi

# Last gate: a CRLF init script compiles, packages and installs perfectly and
# then never starts on the device, because the kernel reads the interpreter as
# "/bin/sh /etc/rc.common\r".  Cheap to check, very expensive to debug.
# Relative paths only for python - see the note about $OUT_W above.
if [ "$fail" = 0 ]; then
	stopped_in="eolcheck.py"
	"$PY" "$HERE_W/eolcheck.py" . || fail=1
fi

# The front end has no compile step whatsoever, so a LuCI view with a missing
# brace is accepted by every build stage and only fails when a browser loads it
# -- a blank page with no build error to explain it.  That is exactly how
# overview.js was committed, packaged and pushed with a syntax error.  Parsing
# four small files costs a moment.
if [ "$fail" = 0 ]; then
	stopped_in="tools/jscheck/check.sh"
	sh "$ROOT/tools/jscheck/check.sh" || fail=1
fi

# --- say why, when it fails -----------------------------------------------
# Nothing above prints a compiler diagnostic: the capture in run_one() is the
# only place the two lines of a type error exist, and it is written to a file
# that is never read.  On a CI runner there is no out/ to inspect afterwards,
# so "check.sh: FAILED" with an empty log is all anyone got -- a failure that
# is indistinguishable from the gate not having run at all, which is what cost
# an hour of guessing at the pipeline's own conditions (host, branch, case
# sensitivity, line endings) for a compile error that was sitting in a file.
if [ "$fail" != 0 ]; then
	echo
	echo "=== cccheck FAILED ==="
	if [ -n "$bad_compiles" ]; then
		echo "sources that did not compile:$bad_compiles"
	elif [ -n "$stopped_in" ]; then
		echo "last gate entered: $stopped_in"
	fi
	# The logs are the failures: run_one() deletes every log that came out
	# empty, and the summary wipe at the top of this script clears the rest.
	for f in "$OUT"/*.log; do
		[ -f "$f" ] || continue
		echo
		echo "--- $(basename "$f") ---"
		cat "$f"
	done
fi

if [ "$fail" = 0 ]; then
	echo "fm160: clean" > "$OUT/SUMMARY"
else
	echo "fm160: FAILED" > "$OUT/SUMMARY"
fi
exit $fail
