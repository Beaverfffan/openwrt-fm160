/*
 * Host-build shim: <stdio.h> that forces binary mode on every fopen().
 *
 * WHY THIS EXISTS -- it is not cosmetic, it silently corrupts the output
 *
 * po2lmo writes the .lmo with fopen(argv[2], "w").  On a POSIX host "w" and
 * "wb" are the same thing, so upstream has never had a reason to write "wb".
 * On Windows they are not: text mode rewrites every 0x0A byte as 0x0D 0x0A.
 *
 * The .lmo is a binary container -- values, then a 16-byte-per-entry index of
 * big-endian uint32s, then the index offset.  A hash or a length field
 * containing the byte 0x0A therefore grows by one byte, and every record after
 * it is read shifted.  The damage is invisible from the outside: po2lmo exits
 * 0, the file looks plausible, and the only tell is that
 *
 *     (size - 4 - <trailing uint32>) % 16 != 0
 *
 * Observed here, which is how this was found: 13 newlines inside the index
 * inflated an 8392-byte .lmo to 8405 bytes, the first six records decoded
 * correctly and the remaining 130 did not, and the checker reported 135 index
 * entries where the po has 140 messages.
 *
 * The read side matters too: text mode also treats 0x1A as end-of-file, so a
 * .po containing that byte would be truncated at it.
 *
 * Both are fixed by the same thing -- a mode string that always ends in 'b'.
 * A macro rather than _set_fmode() on purpose: if the CRT ever stops providing
 * _set_fmode, that call fails to compile or silently does nothing, whereas a
 * redirected fopen either works or does not link.
 *
 * shim.c #undefs fopen before using it, or it would recurse into itself.
 */
#ifndef FM160_SHIM_STDIO_H
#define FM160_SHIM_STDIO_H

#include_next <stdio.h>

FILE *fm160_fopen_binary(const char *path, const char *mode);

#define fopen(path, mode) fm160_fopen_binary((path), (mode))

#endif /* FM160_SHIM_STDIO_H */
