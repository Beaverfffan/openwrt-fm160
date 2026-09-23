/*
 * The aborting stubs for the two POSIX calls lmo.c references but po2lmo never
 * makes.  See shim/fm160_shim.h for why they abort rather than do something
 * plausible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include "fm160_shim.h"

/*
 * shim/stdio.h redirected fopen() to the helper below.  Undo that here or this
 * function would call itself for ever.
 */
#undef fopen
#include <unistd.h>

static void never(const char *what)
{
	fprintf(stderr,
	        "fm160 shim: %s() was called, but this is the po2lmo-only build\n"
	        "             (the mmap catalogue reader is dead code here).\n"
	        "             Build on a POSIX host if po2lmo now needs the reader.\n",
	        what);
	abort();
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	(void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
	never("mmap");
	return MAP_FAILED;
}

int munmap(void *addr, size_t len)
{
	(void)addr; (void)len;
	never("munmap");
	return -1;
}

int fnmatch(const char *pattern, const char *string, int flags)
{
	(void)pattern; (void)string; (void)flags;
	never("fnmatch");
	return FNM_NOMATCH;
}

int fcntl(int fd, int cmd, ...)
{
	(void)fd; (void)cmd;
	never("fcntl");
	return -1;
}

/*
 * The one shim that is not a stub: po2lmo flushes the .lmo it wrote with
 * fsync(fileno(out)).  _commit is the Windows equivalent, and it is called on
 * the same descriptor, so the flush genuinely happens.
 */
int fsync(int fd)
{
	return _commit(fd);
}

/*
 * fopen() with 'b' forced, so that a .lmo is not mangled by newline
 * translation.  See shim/stdio.h for what that costs when it is missed; in
 * short, po2lmo exits 0 and writes a file whose index every reader mis-parses.
 */
FILE *fm160_fopen_binary(const char *path, const char *mode)
{
	char m[8];
	size_t n = strlen(mode);

	if (n > sizeof(m) - 2)
		n = sizeof(m) - 2;

	memcpy(m, mode, n);
	m[n] = 'b';
	m[n + 1] = '\0';

	return fopen(path, m);
}
