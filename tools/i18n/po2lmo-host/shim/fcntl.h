/*
 * Host-build shim: <fcntl.h> with the four identifiers a POSIX <fcntl.h>
 * provides and this one does not.
 *
 * All four are used on a single line inside lmo_open():
 *
 *     fcntl(ar->fd, F_SETFD, fcntl(ar->fd, F_GETFD) | FD_CLOEXEC);
 *
 * which sets close-on-exec on the catalogue fd.  po2lmo never opens a
 * catalogue (see shim/fm160_shim.h), so fcntl() is an aborting stub -- but the
 * CONSTANTS still have to exist or the file will not compile, and their values
 * matter only in that they must not collide with a real fcntl command if this
 * ever starts being called for real.
 */
#ifndef FM160_SHIM_FCNTL_H
#define FM160_SHIM_FCNTL_H

#include_next <fcntl.h>

#ifndef F_GETFD
#define F_GETFD 1
#endif
#ifndef F_SETFD
#define F_SETFD 2
#endif
#ifndef FD_CLOEXEC
#define FD_CLOEXEC 1
#endif

int fcntl(int fd, int cmd, ...);

#endif /* FM160_SHIM_FCNTL_H */
