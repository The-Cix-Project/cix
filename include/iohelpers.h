#ifndef IOHELPERS_H
#define IOHELPERS_H

#include <errno.h>
#include <stddef.h>
#include <unistd.h>

/*
 * Loops write() until n bytes are sent (EINTR retried) or a real error
 * occurs. Returns 0, or -1 on error. Needed identically by the daemon's
 * HTTP response writer, the new WebSocket frame writer, and the exec
 * helper's pid-handoff pipe (daemon/src/http.c, websocket.c, exec.c) --
 * and, since include/ is on both the daemon's and the CLI/client's own
 * build line, by the CLI's own console client too (Part 2). One real
 * implementation, reused rather than duplicated (No Parallel
 * Implementations), matching the precedent this header's own
 * cix_mkdir_p() and daemon/include/namecheck.h already set.
 */
static inline int cix_write_all(int fd, const void *buf, size_t n)
{
	const char *p = buf;
	size_t written = 0;
	ssize_t w;

	while (written < n) {
		w = write(fd, p + written, n - written);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		written += (size_t)w;
	}
	return 0;
}

#endif /* IOHELPERS_H */
