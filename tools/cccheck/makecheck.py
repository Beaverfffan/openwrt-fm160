#!/usr/bin/env python3
"""Two facts that span two files each, and that nothing else checks.

Both of them fail the same way: the build succeeds, the artefact installs, and
the mistake is only visible much later and somewhere else.

  1. fm160/src/Makefile's OBJS versus the .c files on disk.

     A source added but not listed is compiled by ccheck (which globs *.c) and
     therefore type-checked, and then never linked - so a function it defines
     resolves by accident if the object is still around from an earlier build,
     and fails as an undefined symbol on a clean tree.  The reverse, a stale
     entry in OBJS, is a link error that names a file nobody edited.  Neither
     is visible from the source alone.

  2. FM160_VERSION in fm160d.h versus PKG_VERSION in fm160d/Makefile.

     The version is duplicated because the alternative - passing it on the
     toolchain command line with -D - cannot be exercised anywhere except the
     build machine, and a quoting mistake there breaks the build rather than
     the report.  A drift between the two is invisible until someone reads a
     support bundle and concludes the wrong daemon produced it, which is
     exactly the situation a support bundle exists to resolve.

     The comparison is deliberately textual: a version string is data, and a
     check that normalised it would be checking something else.

Exit status: 0 when both hold, 1 otherwise.
"""

import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

SRC = os.path.join(ROOT, 'fm160d', 'src')
SRC_MAKE = os.path.join(SRC, 'Makefile')
PKG_MAKE = os.path.join(ROOT, 'fm160d', 'Makefile')
HEADER = os.path.join(SRC, 'fm160d.h')

checks = 0
failures = 0


def ok(cond, what, detail=''):
    global checks, failures
    checks += 1
    if cond:
        print('  ok   ' + what)
    else:
        failures += 1
        print('  FAIL ' + what)
        if detail:
            print('       ' + detail)


def read(path):
    return io.open(path, encoding='utf-8').read()


# --- 1. OBJS versus the directory ----------------------------------------

make_text = read(SRC_MAKE)

# OBJS is a make variable assigned with `=` and continued with backslashes, so
# the value is everything up to the first line that does not end in one.
#
# The inner group is `[^\n]*\\\n` rather than `.*\\\n` on purpose: a greedy `.*`
# swallows the trailing backslash and the continuation is never matched, which
# silently reads only the first line.  That is how this check first shipped, and
# the planted case below is what caught it.
OBJS_RE = re.compile(r'(?m)^OBJS[ \t]*=[ \t]*((?:[^\n]*\\\n)*[^\n]*)$')


def objs_tokens(text):
    m = OBJS_RE.search(text)
    if not m:
        return []
    return [t for t in re.split(r'\s+', m.group(1).replace('\\\n', ' ')) if t]


listed = set(objs_tokens(make_text))

on_disk = set()
for f in sorted(os.listdir(SRC)):
    if f.endswith('.c'):
        on_disk.add(f[:-2] + '.o')

# A planted case first: the regex above is the whole check, and one that matched
# nothing would make both comparisons below pass on an empty set.
ok(objs_tokens('OBJS = a.o \\\n       b.o \\\n       c.o\nLIBS = -lx\n')
   == ['a.o', 'b.o', 'c.o'],
   'the OBJS pattern reads a continued assignment')

ok(len(listed) > 5, 'OBJS was read (%d objects)' % len(listed))
ok(len(on_disk) > 5, 'the source directory was read (%d sources)' % len(on_disk))

missing = sorted(on_disk - listed)
stale = sorted(listed - on_disk)

ok(not missing,
   'every .c in fm160d/src is linked' +
   (' -- not in OBJS: ' + ', '.join(missing) if missing else ''))
ok(not stale,
   'OBJS lists no object with no source' +
   (' -- stale: ' + ', '.join(stale) if stale else ''))

# --- 2. the two version strings ------------------------------------------

hdr_version = re.search(r'(?m)^#define[ \t]+FM160_VERSION[ \t]+"([^"]*)"',
                        read(HEADER))
pkg_version = re.search(r'(?m)^PKG_VERSION:?=[ \t]*(\S+)', read(PKG_MAKE))

ok(hdr_version is not None, 'FM160_VERSION is defined in fm160d.h')
ok(pkg_version is not None, 'PKG_VERSION is defined in fm160d/Makefile')

if hdr_version and pkg_version:
    ok(hdr_version.group(1) == pkg_version.group(1),
       'FM160_VERSION matches PKG_VERSION (%s)' % pkg_version.group(1),
       'header says "%s", the package says "%s"' %
       (hdr_version.group(1), pkg_version.group(1)))

print()
print('%d/%d assertions passed' % (checks - failures, checks))
if failures:
    print('makecheck: FAILED')
    sys.exit(1)
print('makecheck: clean')
