#!/usr/bin/env python3
"""
symcheck.py - catch unresolved symbols in the fm160d object files.

Why this exists
---------------
check.sh compiles each translation unit with `-c`, which resolves nothing
across files.  A function that is *declared* in fm160d.h and called from six
modules but defined in none of them compiles perfectly and then fails at link
time - which only happens on the build box, ~20 minutes into a build cycle.
That is exactly how `fm160_now_ms` slipped through.

What it does
------------
Reads the ELF symbol table of every .o in the output directory and reports
every undefined reference that is not:
  * defined by another one of our own .o files (global or weak), or
  * a known external (libubox / libubus / libc).

The external set is *derived*, not guessed: libubox/libubus names come from the
real upstream headers in ./include, and the libc names come from LIBC below.
A new libc call therefore shows up as a finding rather than being silently
waved through - add it to LIBC when it is genuinely a libc function.

Exit status: 0 when every undefined symbol resolves, 1 otherwise.
"""

import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
INC = os.path.join(HERE, 'include')

# ---------------------------------------------------------------- ELF64

SHT_SYMTAB = 2
SHT_STRTAB = 3
SHN_UNDEF = 0
STB_GLOBAL = 1
STB_WEAK = 2


def elf_symbols(data):
    """Return (defined_globals, undefined) as sets of names, or None if the
    file is not a little-endian ELF64 object with a symbol table."""
    if len(data) < 64 or data[:4] != b'\x7fELF':
        return None
    if data[4] != 2 or data[5] != 1:          # 64-bit, little-endian
        return None

    e_shoff, = struct.unpack_from('<Q', data, 0x28)
    e_shentsize, = struct.unpack_from('<H', data, 0x3A)
    e_shnum, = struct.unpack_from('<H', data, 0x3C)
    if e_shoff == 0 or e_shnum == 0:
        return None

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type, = struct.unpack_from('<I', data, off + 4)
        sh_offset, = struct.unpack_from('<Q', data, off + 0x18)
        sh_size, = struct.unpack_from('<Q', data, off + 0x20)
        sh_link, = struct.unpack_from('<I', data, off + 0x28)
        sh_entsize, = struct.unpack_from('<Q', data, off + 0x38)
        sections.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))

    def strtab(index):
        t, off, size, _, _ = sections[index]
        if t != SHT_STRTAB:
            return b''
        return data[off:off + size]

    defined, undefined = set(), set()
    for t, off, size, link, entsize in sections:
        if t != SHT_SYMTAB or entsize == 0:
            continue
        names = strtab(link)
        for n in range(size // entsize):
            base = off + n * entsize
            st_name, = struct.unpack_from('<I', data, base)
            st_info = data[base + 4]
            st_shndx, = struct.unpack_from('<H', data, base + 6)
            end = names.find(b'\0', st_name)
            if st_name == 0 or end < 0:
                continue
            name = names[st_name:end].decode('utf-8', 'replace')
            bind = st_info >> 4
            if st_shndx == SHN_UNDEF:
                undefined.add(name)
            elif bind in (STB_GLOBAL, STB_WEAK):
                defined.add(name)
    return defined, undefined


# ------------------------------------------------------- external symbol set

# libc / libgcc.  Kept explicit on purpose: a miss here is a visible finding,
# not a silently accepted call.
LIBC = {
    'abort', 'access', 'atoi', 'calloc', 'clock_gettime', 'close',
    'closedir', 'closelog', 'exit', 'fclose', 'fgets', 'fileno', 'fopen',
    'fprintf', 'fread', 'free', 'fwrite', 'getpid', 'gettimeofday',
    'localtime', 'localtime_r', 'malloc', 'memcpy', 'memmove', 'memset',
    'mkdir', 'nanosleep', 'open', 'opendir', 'openlog', 'pclose', 'poll',
    'popen', 'printf', 'putchar', 'puts', 'rand', 'read', 'readdir',
    'realloc', 'realpath', 'snprintf', 'srand', 'sscanf', 'stpcpy',
    'strcasecmp', 'strchr', 'strcmp', 'strcpy', 'strcspn', 'strftime',
    'strlen', 'strncasecmp', 'strncmp', 'strncpy', 'strpbrk', 'strrchr',
    'strspn', 'strstr', 'strtod', 'strtol', 'strtoll', 'strtoul', 'strtoull',
    'syslog', 'system', 'time', 'unlink', 'usleep', 'vsnprintf', 'vsyslog',
    'write',
    '__assert_fail', '__errno_location', '__stack_chk_fail',
    '__stack_chk_guard', '__libc_start_main', '_exit', 'errno',
}

# zig cc defaults to its Debug mode, which instruments the code with UBSan
# even though the firmware build does not.  Those runtime hooks are supplied
# by zig at link time, so they are external by definition.
SANITIZER_PREFIXES = ('__ubsan_', '__asan_', '__tsan_', '__sanitizer_',
                      '__msan_', '__lsan_')

# A function declaration, possibly wrapped over several lines.  The earlier
# line-oriented version silently missed anything zigzagged like
#     int blobmsg_add_field(struct blob_buf *buf, int type, const char *name,
#                           const void *data, unsigned int len);
# which is most of libubox - so the object scan flagged real libubox calls as
# unresolved.  Matching a parenthesised list followed by ';' handles the
# wrapping; nested parens (function-pointer arguments) never occur in these
# headers, so excluding them is safe.
DECL = re.compile(r'\b([A-Za-z_]\w*)\s*\([^;(){}]*\)\s*;', re.S)

# Not everything libubox exports is a function: `extern bool uloop_cancelled;`
# is a variable, and a function-only scan flags it as unresolved.
EXTERN_VAR = re.compile(r'\bextern\b[^;()]*?\b([A-Za-z_]\w*)\s*;', re.S)


def header_externs():
    """Function and variable names declared by the real libubox/libubus
    headers."""
    names = set()
    for root, _, files in os.walk(INC):
        for f in files:
            if not f.endswith('.h'):
                continue
            text = open(os.path.join(root, f), encoding='utf-8',
                        errors='replace').read()
            # Drop comments so commented-out prototypes are not treated as
            # real ones.
            text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
            text = re.sub(r'//[^\n]*', ' ', text)
            names.update(DECL.findall(text))
            names.update(EXTERN_VAR.findall(text))
    names |= LIBC
    return names


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'out')
    objs = sorted(f for f in os.listdir(outdir) if f.endswith('.o'))
    if not objs:
        print('symcheck: no .o files in %s - run check.sh first' % outdir)
        return 1

    defined, undefined, broken = set(), {}, []
    for f in objs:
        r = elf_symbols(open(os.path.join(outdir, f), 'rb').read())
        if r is None:
            broken.append(f)
            continue
        d, u = r
        defined |= d
        for name in u:
            undefined.setdefault(name, set()).add(f)

    externs = header_externs()
    unresolved = {}
    for n, w in undefined.items():
        if n in defined or n in externs:
            continue
        if n.startswith(SANITIZER_PREFIXES):
            continue
        unresolved[n] = w

    print('symcheck: %d objects, %d defined globals, %d undefined references'
          % (len(objs), len(defined), len(undefined)))
    if broken:
        print('  [warn] no readable symtab: %s' % ', '.join(broken))

    if unresolved:
        print()
        print('  UNRESOLVED (%d) - declared and called somewhere, defined '
              'nowhere:' % len(unresolved))
        for n in sorted(unresolved):
            print('    %-28s referenced by %s' % (n, ', '.join(sorted(unresolved[n]))))
        print()
        print('THIS WOULD FAIL AT LINK TIME on the build box.  Either add the')
        print('definition, or - if it really is an external - add the name to')
        print('LIBC in this script so the next run stays quiet.')
        return 1

    print('  all references resolve -> symbol table consistent')
    return 0


if __name__ == '__main__':
    sys.exit(main())
