/*
 * Host-build shim: <unistd.h> plus fsync().
 *
 * Unlike mmap/fnmatch/fcntl, fsync is NOT dead code -- po2lmo calls it on the
 * .lmo it just wrote, before fclose():
 *
 *     print_uint32(offset, out);
 *     fsync(fileno(out));
 *     fclose(out);
 *
 * so it is implemented for real (see shim.c: it forwards to _commit) rather
 * than stubbed.  A no-op here would leave a truncated .lmo possible on the one
 * path that matters, and a truncated .lmo is exactly the failure this whole
 * exercise exists to rule out.
 */
#ifndef FM160_SHIM_UNISTD_H
#define FM160_SHIM_UNISTD_H

#include_next <unistd.h>

int fsync(int fd);

#endif /* FM160_SHIM_UNISTD_H */
