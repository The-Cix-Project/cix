#include "container.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CGROUP_ROOT "/sys/fs/cgroup"
#define CGROUP_STAT_BUF_SIZE 8192

static int write_cgroup_file(const char *dir, const char *file, const char *value)
{
	char path[PATH_MAX];
	int fd;
	ssize_t written;
	size_t len;

	if (snprintf(path, sizeof(path), "%s/%s", dir, file) >= (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;

	len = strlen(value);
	written = write(fd, value, len);
	if (written < 0 || (size_t)written != len) {
		int saved_errno = (written < 0) ? errno : EIO;
		close(fd);
		errno = saved_errno;
		return -1;
	}

	if (close(fd) != 0)
		return -1;

	return 0;
}

int cgroup_create(const struct cgroup_limits *lim, int *out_fd)
{
	char dir[PATH_MAX];
	char value[32];
	int fd;

	if (snprintf(dir, sizeof(dir), "%s/%s", CGROUP_ROOT, lim->name) >= (int)sizeof(dir)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	if (mkdir(dir, 0755) != 0 && errno != EEXIST)
		return -1;

	if (lim->memory_max > 0) {
		snprintf(value, sizeof(value), "%lld", lim->memory_max);
		if (write_cgroup_file(dir, "memory.max", value) != 0)
			return -1;
	}

	if (lim->pids_max > 0) {
		snprintf(value, sizeof(value), "%lld", lim->pids_max);
		if (write_cgroup_file(dir, "pids.max", value) != 0)
			return -1;
	}

	if (lim->cpu_max != NULL) {
		if (write_cgroup_file(dir, "cpu.max", lim->cpu_max) != 0)
			return -1;
	}

	/*
	 * O_CLOEXEC: this fd is only needed by the kernel at clone3() time
	 * (CLONE_INTO_CGROUP reads it during the syscall itself); the
	 * daemon keeps its own copy for bookkeeping, but the fd table this
	 * clone3() inherits into shouldn't carry a lingering duplicate
	 * past that child's own execve().
	 */
	fd = open(dir, O_PATH | O_CLOEXEC);
	if (fd < 0)
		return -1;

	*out_fd = fd;
	return 0;
}

/*
 * Best-effort, called once at daemon startup (main()'s init sequence,
 * before any container's cgroup leaf can exist): enables the io
 * controller in the cgroup v2 root's own subtree_control, so every
 * container's leaf -- existing and future, cgroup v2 propagates a
 * newly-enabled controller to all descendants immediately, no
 * recreation needed -- gains a populated io.stat for GET .../stats.
 * Not fatal on failure (e.g. already enabled from a prior daemon
 * instance, or a restricted host with no io controller at all): CPU/
 * memory/network/disk-space stats must keep working regardless --
 * io.stat simply stays absent for every container in that case,
 * exactly the same "absence is zero" case cgroup_read_io_totals()
 * already has to handle for a container with no tracked block I/O
 * yet.
 */
void cgroup_enable_io_accounting(void)
{
	if (write_cgroup_file(CGROUP_ROOT, "cgroup.subtree_control", "+io") != 0)
		perror("cgroup_enable_io_accounting: write +io to cgroup.subtree_control");
}

/* Reads filename (relative to dir_fd, e.g. a container's own cgroup_fd)
 * whole into buf, NUL-terminated. Returns the byte count read (never
 * including the NUL), or -1 on any I/O error (including truncation --
 * every file this is used for is a small, bounded cgroup v2 pseudo-file,
 * so hitting bufsize is treated as a real error, not silently accepted
 * partial content). */
static ssize_t read_whole_file_at(int dir_fd, const char *filename, char *buf, size_t bufsize)
{
	int fd;
	ssize_t total = 0, n;

	fd = openat(dir_fd, filename, O_RDONLY);
	if (fd < 0)
		return -1;

	while (total < (ssize_t)bufsize - 1) {
		n = read(fd, buf + total, bufsize - 1 - (size_t)total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		if (n == 0) {
			close(fd);
			buf[total] = '\0';
			return total;
		}
		total += n;
	}
	close(fd);
	errno = EFBIG;
	return -1;
}

/*
 * Looks up a single "key value" line (cpu.stat's/memory.stat's own flat
 * format, one stat per line) inside filename via cgroup_fd. *out is 0
 * if key isn't present at all -- a stat that simply hasn't accumulated
 * yet (e.g. throttled_usec before any throttling ever occurred) is not
 * an error. Returns -1 only if the file itself couldn't be opened/read
 * (a real I/O error, not a missing key).
 */
int cgroup_read_stat_key(int cgroup_fd, const char *filename, const char *key, long long *out)
{
	char buf[CGROUP_STAT_BUF_SIZE];
	char *line, *saveptr;

	*out = 0;
	if (read_whole_file_at(cgroup_fd, filename, buf, sizeof(buf)) < 0)
		return -1;

	for (line = strtok_r(buf, "\n", &saveptr); line != NULL; line = strtok_r(NULL, "\n", &saveptr)) {
		char *sp = strchr(line, ' ');

		if (sp == NULL)
			continue;
		*sp = '\0';
		if (strcmp(line, key) == 0) {
			*out = strtoll(sp + 1, NULL, 10);
			return 0;
		}
	}
	return 0;
}

/*
 * Reads a cgroup v2 single-value file (memory.current, memory.peak,
 * memory.max) via cgroup_fd. memory.max may read the literal "max"
 * (unlimited) instead of a number -- *out_is_unlimited is set 1 in
 * that case and *out is left at 0.
 */
int cgroup_read_single_value(int cgroup_fd, const char *filename, long long *out, int *out_is_unlimited)
{
	char buf[64];
	size_t len;

	*out = 0;
	*out_is_unlimited = 0;
	if (read_whole_file_at(cgroup_fd, filename, buf, sizeof(buf)) < 0)
		return -1;

	len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	if (strcmp(buf, "max") == 0) {
		*out_is_unlimited = 1;
		return 0;
	}
	*out = strtoll(buf, NULL, 10);
	return 0;
}

/*
 * Sums rbytes/wbytes/rios/wios across every device line in io.stat via
 * cgroup_fd. A container with no tracked block I/O yet has a genuinely
 * empty io.stat (or the io controller might not be enabled at all, see
 * cgroup_enable_io_accounting()) -- zero lines is not an error, all
 * four outputs are simply 0. Returns -1 only if the file itself
 * couldn't be opened (a real I/O error).
 */
int cgroup_read_io_totals(int cgroup_fd, long long *out_rbytes, long long *out_wbytes,
                           long long *out_rios, long long *out_wios)
{
	char buf[CGROUP_STAT_BUF_SIZE];
	char *line, *saveptr;

	*out_rbytes = *out_wbytes = *out_rios = *out_wios = 0;
	if (read_whole_file_at(cgroup_fd, "io.stat", buf, sizeof(buf)) < 0)
		return -1;

	for (line = strtok_r(buf, "\n", &saveptr); line != NULL; line = strtok_r(NULL, "\n", &saveptr)) {
		char *tok, *tsave;

		/* First token is "<major>:<minor>" -- skip it, everything after
		 * is "key=value" pairs. */
		tok = strtok_r(line, " ", &tsave);
		if (tok == NULL)
			continue;
		for (tok = strtok_r(NULL, " ", &tsave); tok != NULL; tok = strtok_r(NULL, " ", &tsave)) {
			char *eq = strchr(tok, '=');
			long long val;

			if (eq == NULL)
				continue;
			*eq = '\0';
			val = strtoll(eq + 1, NULL, 10);
			if (strcmp(tok, "rbytes") == 0)
				*out_rbytes += val;
			else if (strcmp(tok, "wbytes") == 0)
				*out_wbytes += val;
			else if (strcmp(tok, "rios") == 0)
				*out_rios += val;
			else if (strcmp(tok, "wios") == 0)
				*out_wios += val;
		}
	}
	return 0;
}
