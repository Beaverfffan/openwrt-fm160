/*
 * Host-build shim: the two POSIX headers the C library does not carry here.
 *
 * WHY THIS EXISTS
 * ---------------
 * po2lmo is worth building locally because it is the only thing that can tell
 * us whether our .po is a shape po2lmo actually accepts -- escapes, the header
 * entry, the "#:" comments, the msgid==msgstr skip.  Every static check in the
 * world passes on a .po that po2lmo then mis-parses.
 *
 * Building it needs lib/lmo.c, which is a POSIX file: it includes <sys/mman.h>
 * and <fnmatch.h> for the *reader* (lmo_open / lmo_load_catalog).  po2lmo is the
 * *writer* and never calls either -- verified, not assumed:
 *
 *     $ grep -n 'lmo_open\|mmap\|fnmatch' po2lmo.c
 *     (nothing)
 *
 * so the symbols only have to exist, never to work.  Rather than pretend, the
 * stubs below abort().  A future change that starts routing po2lmo through
 * lmo_open fails loudly on the first call instead of silently reading or
 * matching nothing.
 */
#ifndef FM160_SHIM_MMAN_FNMATCH_H
#define FM160_SHIM_MMAN_FNMATCH_H

#include <sys/types.h>

/* <sys/mman.h>, as much of it as lmo.c touches. */
#define PROT_READ   0x1
#define MAP_SHARED  0x1
#define MAP_FAILED  ((void *)-1)

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int   munmap(void *addr, size_t len);

/* <fnmatch.h>, as much of it as lmo.c touches. */
#define FNM_NOMATCH 1
#define FNM_PATHNAME 0x1

int fnmatch(const char *pattern, const char *string, int flags);

#endif /* FM160_SHIM_MMAN_FNMATCH_H */
