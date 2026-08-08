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

	if (lim->cpuset_cpus != NULL) {
		if (write_cgroup_file(dir, "cpuset.cpus", lim->cpuset_cpus) != 0)
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
 * before any container's cgroup leaf can exist): enables every cgroup
 * v2 controller this project's own container/host-stats code needs,
 * in the root's own subtree_control -- io/cpuset (this function's own
 * original scope) plus memory/pids/cpu, added after a real, confirmed
 * gap: a genuinely fresh cgroup v2 hierarchy (kanxeod running as real
 * PID 1, no systemd ever pre-delegating anything to its own default
 * slices, unlike every dev/test environment this project had
 * exercised so far) starts with a completely EMPTY root
 * subtree_control -- the kernel itself never auto-populates it,
 * regardless of what cgroup.controllers lists as merely *available*.
 * Confirmed live on 192.168.15.95 (this project's first genuine
 * bare-metal/real-PID-1 install to actually try a resource-limited
 * container): a bare `run` with no cgroup_limits succeeded outright
 * (creating a child cgroup directory needs no delegated controller at
 * all), while `--memory-max=`/`--pids-max=` both failed with a bare,
 * undiagnosed 500 -- cgroup_create()'s own write_cgroup_file() calls
 * for memory.max/pids.max simply found no such file to open, since
 * those controllers were never delegated down from the root in the
 * first place. Every dev/test environment up to this point ran under
 * a real distro's own systemd, which pre-delegates memory/pids/cpu to
 * its own hierarchy by default -- masking this gap entirely until now.
 *
 * A first version of this fix wrote a single fixed
 * "+io +cpuset +memory +pids +cpu" unconditionally -- wrong, confirmed
 * live on the same box (ADR-0080's own diagnostics investigation): a
 * cgroup v2 subtree_control write is atomic across every token it
 * contains, so if even one of the five was ever unavailable on this
 * kernel, the WHOLE write failed with EINVAL, silently regressing
 * io/cpuset back to undelegated too -- they'd worked fine moments
 * earlier under the OLD code's two separate single-controller writes.
 * Reading cgroup.controllers first and only requesting tokens actually
 * listed there removes this failure mode entirely: every genuinely
 * available controller gets delegated in one write regardless of what
 * this particular kernel happens to be missing, matching the "degrade
 * per-resource, never fail the whole thing" posture the doc comment
 * below already promises but the first version didn't actually keep.
 *
 * Not fatal on failure to read/write either file (already enabled from
 * a prior daemon instance, or open()/read() itself failing) --
 * containers requesting an unavailable limit simply run unrestricted
 * for that one resource, never blocked from being created at all over
 * a controller this daemon couldn't delegate.
 */
void cgroup_enable_controllers(void)
{
	static const char *const wanted[] = { "io", "cpuset", "memory", "pids", "cpu" };
	char path[PATH_MAX];
	char avail[256];
	char request[64];
	size_t request_len = 0;
	size_t i;
	int fd;
	ssize_t n;

	if (snprintf(path, sizeof(path), "%s/cgroup.controllers", CGROUP_ROOT) >= (int)sizeof(path))
		return;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("cgroup_enable_controllers: open cgroup.controllers");
		return;
	}
	n = read(fd, avail, sizeof(avail) - 1);
	close(fd);
	if (n < 0) {
		perror("cgroup_enable_controllers: read cgroup.controllers");
		return;
	}
	avail[n] = '\0';

	request[0] = '\0';
	for (i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
		char *saveptr, *tok, avail_copy[sizeof(avail)];
		int found = 0;

		snprintf(avail_copy, sizeof(avail_copy), "%s", avail);
		for (tok = strtok_r(avail_copy, " \n", &saveptr); tok != NULL;
		     tok = strtok_r(NULL, " \n", &saveptr)) {
			if (strcmp(tok, wanted[i]) == 0) {
				found = 1;
				break;
			}
		}
		if (!found)
			continue;
		if (request_len > 0 && request_len + 1 < sizeof(request))
			request[request_len++] = ' ';
		request_len += (size_t)snprintf(request + request_len, sizeof(request) - request_len,
		                                 "+%s", wanted[i]);
	}

	if (request[0] == '\0')
		return;

	if (write_cgroup_file(CGROUP_ROOT, "cgroup.subtree_control", request) != 0)
		perror("cgroup_enable_controllers: write to cgroup.subtree_control");
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
 * cgroup_enable_controllers()) -- zero lines is not an error, all
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

void cgroup_read_pressure(int cgroup_fd, const char *filename, struct cgroup_pressure *out)
{
	char buf[512];
	char *line, *saveptr;

	memset(out, 0, sizeof(*out));
	if (read_whole_file_at(cgroup_fd, filename, buf, sizeof(buf)) < 0)
		return;

	for (line = strtok_r(buf, "\n", &saveptr); line != NULL; line = strtok_r(NULL, "\n", &saveptr)) {
		char *tok, *tsave;
		int is_full;
		double avg10 = 0, avg60 = 0, avg300 = 0;
		long long total = 0;

		tok = strtok_r(line, " ", &tsave);
		if (tok == NULL)
			continue;
		is_full = (strcmp(tok, "full") == 0);
		if (!is_full && strcmp(tok, "some") != 0)
			continue;

		for (tok = strtok_r(NULL, " ", &tsave); tok != NULL; tok = strtok_r(NULL, " ", &tsave)) {
			char *eq = strchr(tok, '=');

			if (eq == NULL)
				continue;
			*eq = '\0';
			if (strcmp(tok, "avg10") == 0)
				avg10 = strtod(eq + 1, NULL);
			else if (strcmp(tok, "avg60") == 0)
				avg60 = strtod(eq + 1, NULL);
			else if (strcmp(tok, "avg300") == 0)
				avg300 = strtod(eq + 1, NULL);
			else if (strcmp(tok, "total") == 0)
				total = strtoll(eq + 1, NULL, 10);
		}

		if (is_full) {
			out->full_avg10 = avg10;
			out->full_avg60 = avg60;
			out->full_avg300 = avg300;
			out->full_total = total;
		} else {
			out->some_avg10 = avg10;
			out->some_avg60 = avg60;
			out->some_avg300 = avg300;
			out->some_total = total;
		}
	}
}
