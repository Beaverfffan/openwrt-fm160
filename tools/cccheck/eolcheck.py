#!/usr/bin/env python3
"""
eolcheck.py - CRLF guard for files that get installed on the device.

Why this exists
---------------
Windows is the authoring host, so every file risks being written with CRLF.
For C sources and Makefiles that is merely untidy - GNU make and gcc both cope.
For a shell script it is fatal, and it fails quietly in the worst way:

    /etc/init.d/fm160d   starting with  "#!/bin/sh /etc/rc.common\r\n"

The kernel sees the interpreter path as "/bin/sh /etc/rc.common\\r", which does
not exist, and the service simply never starts.  Nothing in the build reports
it, and the .ipk looks perfectly fine.  Exactly this was found in the vendored
at-daemon init script, which had been silently CRLF while upstream is LF.

Scope: anything that is copied to the device verbatim - the files/ overlays,
luci-app-fm160/root/ and its htdocs/, plus any *.sh and any file with a
shebang.  C sources and .js are included too, since keeping the whole tree LF
is simpler to reason about than an allow-list.

Exit status: 0 if everything is LF, 1 if any file needs converting.
"""

import os
import sys

# Directories that are neither shipped nor worth scanning.
SKIP_DIRS = {'.git', 'out', 'probe', '.zigcache', 'verify', '_tmp'}

# Path fragments whose contents are installed on the device.
SHIPPED = ('files' + os.sep, 'root' + os.sep, 'htdocs' + os.sep)


def must_be_lf(path, data):
    if b'\r\n' not in data:
        return False
    if path.endswith('.sh'):
        return True
    if data.startswith(b'#!'):
        return True
    # files/ root/ htdocs/ overlays are copied verbatim into the image.
    normalised = path.replace('/', os.sep)
    return any(seg in normalised for seg in SHIPPED)


def main():
    roots = sys.argv[1:] or ['.']
    offenders, scanned = [], 0

    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for name in sorted(filenames):
                full = os.path.join(dirpath, name)
                try:
                    data = open(full, 'rb').read()
                except OSError:
                    continue
                scanned += 1
                if must_be_lf(full, data):
                    crlf = data.count(b'\r\n')
                    offenders.append((full, crlf, len(data)))

    print('eolcheck: scanned %d files under %s' % (scanned, ', '.join(roots)))

    if not offenders:
        print('  every shipped file is LF -> no CRLF will reach the device')
        return 0

    print()
    print('  CRLF IN SHIPPED FILES (%d) - broken on the device:' % len(offenders))
    for path, crlf, size in offenders:
        print('    %-58s %d CRLF endings, %d bytes' % (path, crlf, size))
    print()
    print('Fix with, from the workspace root:')
    print('    python -c "import sys;[open(p,\'wb\').write('
          'open(p,\'rb\').read().replace(b\'\\r\\n\',b\'\\n\'))'
          ' for p in sys.argv[1:]]" <files...>')
    return 1


if __name__ == '__main__':
    sys.exit(main())
