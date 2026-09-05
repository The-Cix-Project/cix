#ifndef IOHELPERS_H
#define IOHELPERS_H

#include <errno.h>
#include <fcntl.h>
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

/*
 * Restores this process's OOM protection to the kernel default (0),
 * undoing the -1000 it inherited from the control plane.
 *
 * Called from inside a forked child, before it execs a workload.
 *
 * The daemon sets oom_score_adj -1000 on itself (protect_control_plane(),
 * daemon/src/main.c) because a box whose only management path has been
 * OOM-killed is a box nobody can reach. That is right, and it is also
 * inherited: oom_score_adj survives both fork() and execve(), and cixd
 * runs as pid 1, so without this EVERY process on the host is immune --
 * containers, package builds, exec sessions, the lot.
 *
 * The consequence is worse than it sounds and was measured on
 * 192.168.15.95 (#278): a cgroup that hits its memory ceiling with no
 * eligible victim does not fail, it LIVELOCKS. The kernel scans, finds
 * everything unkillable, gives up, the allocation retries, and it OOMs
 * again -- 1,180,732 failed allocations climbing to 1,194,664 in the
 * following minute, at ~1000 kernel log lines per second, until the box
 * was manually reset. The build neither completed nor failed. Every
 * memory limit this platform offers was unenforceable for the same
 * reason.
 *
 * The line drawn here is deliberate and narrow: the control plane and
 * the short-lived helpers it runs as part of its own work stay
 * protected, because under real global pressure the kernel should
 * reclaim from a workload rather than from the only way into the
 * machine. Anything running WORKLOAD code -- a container, a build, an
 * exec session -- must be an ordinary candidate, or its cgroup's ceiling
 * is a livelock trigger rather than a bound.
 *
 * Deliberately silent on failure. This runs in a forked child a moment
 * before execve with nowhere to report to, and the process must still
 * exec: an unwritable oom_score_adj leaves the inherited value, which is
 * exactly the behaviour that existed before this and is no worse.
 */
static inline void cix_oom_unprotect_self(void)
{
	int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return;
	n = write(fd, "0", 1);
	(void)n;
	close(fd);
}

#endif /* IOHELPERS_H */
