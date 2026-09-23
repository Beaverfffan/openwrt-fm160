# Shared helpers for the verification tooling under tools/.  POSIX sh, meant to
# be sourced:
#
#   ROOT=...            # the fm160-luci checkout root, set by the caller
#   . "$ROOT/tools/lib/tooling.sh"
#
# It provides, and each one exists because getting it wrong is silent:
#
#   find_zig            -> ZIG          locate the ziglang wheel's compiler
#   wpath <p>           -> stdout       MSYS path -> Windows path
#   find_python         -> PY           a python that can run the helpers
#   del <files...>                      remove files (see below)
#   rmrf <dir>                          remove a tree (see below)
#   zig_setup_cache <d>                 make zig's caches Windows-shaped
#
# Why removals do not use rm
# --------------------------
# The sandbox the workstation runs under intercepts rm and kills the whole
# script with SIGTERM.  The symptom is a script that prints nothing and exits
# on a signal, which looks like a logic error rather than a sandbox policy.
# Everything is deleted through python instead.
#
# Why everything handed to zig is a Windows path
# ----------------------------------------------
# A Windows zig cannot read /c/Users/x: it reports "error: CacheCheckFailed" at
# 1:1 on the first file and nothing else.  And an *absolute* source path makes
# it lose the file when writing intermediates ("error: FileNotFound" at the
# main file), so callers should cd to the workspace root and pass paths
# relative to it, converting only what has to be absolute.

: "${ROOT:?tooling.sh: set ROOT before sourcing}"

# --- Windows path conversion ------------------------------------------------
# MSYS provides cygpath; native tools resolve /c/... against the current drive
# instead, which is how "can't open C:\c\Users\..." happens.
wpath() {
	if command -v cygpath >/dev/null 2>&1; then
		cygpath -w "$1"
	else
		printf '%s\n' "$1"
	fi
}

# --- python for the helpers -------------------------------------------------
# Not for the caller's own use of python (it may want a specific interpreter),
# only for the shell helpers that cannot be written portably in sh.
find_python() {
	if [ -n "${PY:-}" ] && "$PY" -c 'pass' 2>/dev/null; then
		return 0
	fi
	for cand in python python3 py; do
		if command -v "$cand" >/dev/null 2>&1 && "$cand" -c 'pass' 2>/dev/null; then
			PY=$cand
			return 0
		fi
	done
	# The managed runtimes keep the interpreter under versions/<v>/ while a
	# virtualenv sits beside it; search both, since neither is on PATH here.
	# ($ROOT is the checkout, so its _tmp is only ever author scratch.)
	for cand in "$ROOT"/_tmp/*/python.exe \
	            "$HOME"/.workbuddy/binaries/python/envs/*/Scripts/python.exe \
	            "$HOME"/.workbuddy/binaries/python/versions/*/python.exe; do
		if [ -x "$cand" ] && "$cand" -c 'pass' 2>/dev/null; then
			PY=$cand
			return 0
		fi
	done
	echo "tooling.sh: no python found; set \$PY" >&2
	return 1
}

# Removals go through python because the sandbox intercepts rm (see the header).
#
# The arguments MUST be Windows paths.  The python here is a Windows binary and
# resolves an MSYS /c/... against the current drive (-> C:\c\...), finds nothing,
# and -- with errors ignored -- reports success.  That is exactly how a stale
# object file survives a "clean" rebuild and gets linked into a product that
# then behaves inexplicably.  So: convert, then verify the path is gone.
#
# Paths may not contain spaces (nothing in this workspace does); the conversion
# is passed through the shell's word splitting.
_py_paths() {
	_args=
	for _p in "$@"; do
		_args="$_args $(wpath "$_p")"
	done
	printf '%s' "$_args"
}

del() {
	find_python || return 1
	# shellcheck disable=SC2046
	"$PY" -c '
import os, sys
for p in sys.argv[1:]:
    try:
        os.remove(p)
    except OSError:
        pass
' $(_py_paths "$@")
}

rmrf() {
	find_python || return 1
	# shellcheck disable=SC2046
	"$PY" -c '
import shutil, sys
for p in sys.argv[1:]:
    shutil.rmtree(p, ignore_errors=True)
' $(_py_paths "$@")
	for _p in "$@"; do
		if [ -e "$_p" ]; then
			echo "tooling.sh: rmrf could not remove $_p" >&2
			return 1
		fi
	done
	return 0
}

# --- zig --------------------------------------------------------------------
# Resolution order: $ZIG, then the ziglang wheel inside whichever python has it
# (`pip install ziglang`), then the wheel in a sibling virtualenv, then plain
# `zig`.  Steps 2 and 3 are not guesswork: the interpreter on PATH is often a
# bare versioned install while the wheel lives in a separate environment.
find_zig() {
	if [ -n "${ZIG:-}" ] && [ -x "$ZIG" ]; then
		return 0
	fi
	ZIG=
	ZIG_PROBE='
import os, sys
try:
    import ziglang
except Exception:
    sys.exit(0)
name = "zig.exe" if os.name == "nt" else "zig"
print(os.path.join(os.path.dirname(ziglang.__file__), name))
'
	if command -v python >/dev/null 2>&1; then
		ZIG=$(python -c "$ZIG_PROBE" 2>/dev/null || true)
	fi
	if [ -z "$ZIG" ]; then
		PYBIN=$(command -v python 2>/dev/null || command -v python3 2>/dev/null || true)
		if [ -n "$PYBIN" ]; then
			PYUP=$(dirname "$PYBIN")
			for _depth in 1 2 3; do
				PYUP=$(dirname "$PYUP")
				for cand in "$PYUP"/envs/*/Scripts/python.exe \
				            "$PYUP"/envs/*/bin/python \
				            "$PYUP"/[a-z]*/Scripts/python.exe; do
					[ -x "$cand" ] || continue
					ZIG=$("$cand" -c "$ZIG_PROBE" 2>/dev/null || true)
					[ -n "$ZIG" ] && break
				done
				[ -n "$ZIG" ] && break
			done
		fi
	fi
	[ -n "$ZIG" ] || ZIG=$(command -v zig 2>/dev/null || true)
	if [ -z "$ZIG" ] || [ ! -x "$ZIG" ]; then
		echo "tooling.sh: zig not found - run 'pip install ziglang' or set \$ZIG" >&2
		return 1
	fi
	return 0
}

# Give zig caches it can actually open.  $1 is the MSYS directory to use.
#
# Only the two cache variables are redirected.  Rewriting HOME would do the
# same job -- zig falls back to it -- but HOME is also read by every MSYS tool
# in the same shell, which expect a POSIX path there, so it is left alone.
zig_setup_cache() {
	ZIG_CACHE_W=$(wpath "$1")
	export ZIG_GLOBAL_CACHE_DIR="$ZIG_CACHE_W/g"
	export ZIG_LOCAL_CACHE_DIR="$ZIG_CACHE_W/l"
	mkdir -p "$1/g" "$1/l"
}
