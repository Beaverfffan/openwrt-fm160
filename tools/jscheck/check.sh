#!/bin/sh
# Syntax-check the LuCI front end.
#
# LuCI's JavaScript is never compiled: a file with a broken bracket is accepted
# by every build step and only fails when the browser loads it, at which point
# the page is simply blank.  That is how overview.js shipped with a missing
# brace -- it was committed, packaged, and pushed before anything parsed it.
#
# The gate is node's own parser, because it is the real thing and it is right by
# construction.  When it reports a failure, balance.py is run as well: node's
# message points at a line near the damage, but a stray ')' four hundred lines
# below a missing '}' is reported at the stray, and balance.py names the line
# where the bracket was actually opened.
#
# Usage: sh tools/jscheck/check.sh
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
# `pwd -W` is an MSYS extension and cygpath does not exist on a POSIX host, so
# the fallback is the path we were given rather than a second conversion tool.
#
# The space after `$(` is load-bearing.  `$((` starts an arithmetic expansion in
# every POSIX shell, and the group below is not arithmetic: bash notices that
# and falls back to a subshell, dash does not and dies parsing the file --
# "Syntax error: Missing '))'" -- before a single .js has been looked at.  This
# is why jscheck never ran on a CI runner (whose /bin/sh is dash) while passing
# on the workstation (whose sh is bash).
HERE_W=$( (cd "$HERE" && pwd -W 2>/dev/null) || printf '%s' "$HERE")

VIEWS="$ROOT/luci-app-fm160/htdocs"
# Windows-style twin of VIEWS: the python and node in use are Windows
# binaries, and MSYS /c/... paths resolve against the current drive for them
# (-> C:\c\...).
VIEWS_W=$(cd "$VIEWS" 2>/dev/null && pwd -W 2>/dev/null || printf '%s' "$VIEWS")

# --- locate node ---------------------------------------------------------
# Resolution order: $NODE, then the node first on PATH, then the managed
# runtime under ~/.workbuddy, whose version directory name is not predictable
# enough to hard-code.
if [ -z "${NODE:-}" ]; then
	NODE=$(command -v node 2>/dev/null || true)
fi

if [ -z "${NODE:-}" ]; then
	for cand in "$HOME"/.workbuddy/binaries/node/versions/*/node.exe \
		    "$HOME"/.workbuddy/binaries/node/versions/*/bin/node; do
		[ -x "$cand" ] && NODE="$cand" && break
	done
fi

if [ -z "${NODE:-}" ] || [ ! -x "$NODE" ]; then
	echo "node not found - install node or set \$NODE" >&2
	exit 2
fi

# --- locate python (balance.py, and the ws-safe file listing) -------------
if [ -z "${PY:-}" ]; then
	PY=$(command -v python 2>/dev/null || command -v python3 2>/dev/null || true)
fi
if [ -z "${PY:-}" ]; then
	for cand in "$HOME"/.workbuddy/binaries/python/versions/*/python.exe \
		    "$HOME"/.workbuddy/binaries/python/versions/*/bin/python3; do
		[ -x "$cand" ] && PY="$cand" && break
	done
fi

# --- the file list -------------------------------------------------------
# Globs under a Windows path so node can open them: MSYS hands out /c/... paths
# that a Windows binary resolves against the current drive (-> C:\c\...).
#
# Use python to walk the tree rather than `find`: on this workstation
# C:\Windows\System32\find.exe shadows the MSYS one and answers with
# "invalid parameter" instead of a file list.
list_js() {
	"$PY" - "$VIEWS_W" <<'PYEOF'
import os, sys

# A Windows python translates '\n' to '\r\n' on stdout, and the shell keeps the
# CR, so every path in the list arrives as 'foo.js\r' and neither node nor
# python can open it.  Pin the newline at the source.
sys.stdout.reconfigure(newline='\n')

views = sys.argv[1]
found = []
for dirpath, dirnames, filenames in os.walk(views):
    dirnames.sort()
    for name in sorted(filenames):
        if name.endswith('.js'):
            found.append(os.path.join(dirpath, name))
# Windows-style paths: node.exe cannot resolve MSYS /c/... paths.
print('\n'.join(os.path.normpath(p) for p in found))
PYEOF
}

if [ ! -d "$VIEWS" ]; then
	echo "no front-end directory at $VIEWS" >&2
	exit 2
fi

FILES=$(list_js)
count=$(printf '%s\n' "$FILES" | grep -c . )
[ "$count" -gt 0 ] || { echo "no .js files found under $VIEWS" >&2; exit 2; }

fail=0
failed_files=""

# Word splitting on $FILES is intended: the list is one path per line and no
# path here contains whitespace.
for f in $FILES; do
	out=$("$NODE" --check "$f" 2>&1)
	if [ $? -ne 0 ]; then
		fail=1
		failed_files="$failed_files $f"
		printf '  FAIL  %s\n' "${f#$VIEWS_W/}"
		printf '%s\n' "$out" | sed 's/^/        /' | head -8
	fi
done

if [ "$fail" = 0 ]; then
	# Parsing proves the file is valid JavaScript; it says nothing about whether
	# it computes the right thing.  test_api.js loads api.js in node against stub
	# LuCI globals and checks the schema accessors and the USB mode policy --
	# including that every PID the modem can present is still recognised as an
	# FM160, since failing that would turn a switch into a one-way trip.
	"$NODE" "$HERE_W/test_api.js" || fail=1
fi

if [ "$fail" = 0 ]; then
	# test_api.js answers "does api.js compute the right thing".  It cannot
	# answer "is the thing it calls actually there": a method reaches a browser
	# through the daemon's table, api.js's rpc.declare and the rpcd grant, three
	# files in three languages, and nothing else compares them.  The failure
	# mode is a button that returns "Access denied" - for every user except the
	# administrator testing it, because root is not subject to ACLs.
	"$NODE" "$HERE_W/test_contract.js" || fail=1
fi

if [ "$fail" = 0 ]; then
	printf 'jscheck: %s files parsed by node, all clean\n' "$count"
	exit 0
fi

# Explain, don't just complain: name the line where the unbalanced bracket was
# opened.  No-op if the failure is something node found and the lexer did not
# (a real syntax error that still balances, e.g. a missing comma).
echo
echo "--- bracket balance (the line that needs editing) ---"
"$PY" "$HERE_W/balance.py" $failed_files || true
exit 1
