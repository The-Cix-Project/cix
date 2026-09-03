#include "container.h"

#include <errno.h>
#include <dirent.h>
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

/*
 * Every failure path below calls both perror() (real stderr, useful only
 * with direct console/journal access -- this project's real production
 * box, 192.168.15.95, has neither) and container_set_last_error_step()
 * (container.c), which is what actually reaches an operator: read via
 * container_create_last_error_step() by whichever daemon-layer caller
 * has real log-store access (main.c links logstore.c; this file and
 * container.c don't and shouldn't). Confirmed the hard way, chasing a
 * real ADR-0165 production regression: logstore_write() is a one-way
 * "also print to stderr for boot visibility" call -- nothing mirrors
 * real stderr back into the queryable log store, so perror() alone
 * here would silently go nowhere useful once boot has finished.
 */
int cgroup_delegate(const struct cgroup_limits *lim, long long uid, long long gid)
{
	/* The set the kernel's delegation guidance names, and no more --
	 * see container.h for why the limit files are excluded. */
	static const char *const delegated[] = { "cgroup.procs", "cgroup.subtree_control",
		                                     "cgroup.threads" };
	char dir[PATH_MAX];
	char path[PATH_MAX];
	size_t i;

	if (snprintf(dir, sizeof(dir), "%s/%s", CGROUP_ROOT, lim->name) >= (int)sizeof(dir)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (chown(dir, (uid_t)uid, (gid_t)gid) != 0) {
		perror("cgroup_delegate: chown dir");
		container_set_last_error_step("cgroup_delegate: chown dir");
		return -1;
	}
	for (i = 0; i < sizeof(delegated) / sizeof(delegated[0]); i++) {
		if (snprintf(path, sizeof(path), "%s/%s", dir, delegated[i]) >= (int)sizeof(path)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		/* cgroup.threads is absent on some kernels; a missing file is
		 * not a failure to delegate what does exist. */
		if (chown(path, (uid_t)uid, (gid_t)gid) != 0 && errno != ENOENT) {
			perror("cgroup_delegate: chown file");
			container_set_last_error_step("cgroup_delegate: chown file");
			return -1;
		}
	}
	return 0;
}

/*
 * Returns a cgroup to the state a freshly created one would be in:
 * no delegated controllers, and no leftover children (#258).
 *
 * Children first, then the delegation -- a controller cannot be
 * withdrawn from a subtree that still has cgroups using it, so the
 * other order fails on exactly the case that matters.
 */
static void reset_cgroup_to_leaf(const char *dir)
{
	char buf[256];
	DIR *d;
	struct dirent *e;
	int fd;
	ssize_t n;

	d = opendir(dir);
	if (d != NULL) {
		while ((e = readdir(d)) != NULL) {
			char child[PATH_MAX];

			if (e->d_type != DT_DIR || e->d_name[0] == '.')
				continue;
			if (snprintf(child, sizeof(child), "%s/%s", dir, e->d_name) >= (int)sizeof(child))
				continue;
			/* Only an empty cgroup can be removed, which is the
			 * right constraint: anything still holding processes is
			 * not ours to tear down. */
			rmdir(child);
		}
		closedir(d);
	}

	if (snprintf(buf, sizeof(buf), "%s/cgroup.subtree_control", dir) >= (int)sizeof(buf))
		return;
	fd = open(buf, O_RDONLY);
	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return;
	buf[n] = '\0';
	{
		char *saveptr, *tok;
		char off[256];
		size_t len = 0;

		off[0] = '\0';
		for (tok = strtok_r(buf, " \n", &saveptr); tok != NULL;
		     tok = strtok_r(NULL, " \n", &saveptr)) {
			if (tok[0] == '\0')
				continue;
			len += (size_t)snprintf(off + len, sizeof(off) - len, "%s-%s",
			                         len > 0 ? " " : "", tok);
			if (len >= sizeof(off))
				return;
		}
		if (off[0] != '\0')
			write_cgroup_file(dir, "cgroup.subtree_control", off);
	}
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

	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		perror("cgroup_create: mkdir");
		container_set_last_error_step("cgroup_create: mkdir");
		return -1;
	}
	/*
	 * A reused cgroup must be handed back as a clean LEAF (#258).
	 *
	 * Container cgroup names are reused constantly -- build slots are
	 * literally named after the slot index -- and CLONE_INTO_CGROUP
	 * refuses with EBUSY if the target has controllers delegated to its
	 * subtree, because a cgroup that delegates may not also hold
	 * processes. So whatever the previous occupant left behind decides
	 * whether the next container can start at all.
	 *
	 * This is not hypothetical and it is self-inflicted: a nested cixd
	 * enables subtree_control on its own namespace root, which IS this
	 * cgroup, and that setting outlives the container. The next build
	 * in the same slot then failed with
	 * "container_create: ns_clone3: Device or resource busy" --
	 * every package build on the host, broken by having once run a
	 * container that ran containers.
	 *
	 * Best-effort on purpose: a fresh cgroup has nothing to reset, and
	 * a failure to tidy is not a reason to refuse a container that may
	 * well start anyway. The mkdir above already established the
	 * directory; this only puts it back in the state a new one would
	 * have been in.
	 */
	reset_cgroup_to_leaf(dir);

	if (lim->memory_max > 0) {
		snprintf(value, sizeof(value), "%lld", lim->memory_max);
		if (write_cgroup_file(dir, "memory.max", value) != 0) {
			perror("cgroup_create: write memory.max");
			container_set_last_error_step("cgroup_create: write memory.max");
			return -1;
		}
	}

	/*
	 * >= 0, not > 0: zero is a real setting here -- no swap at all for
	 * this container. See struct cgroup_limits.
	 */
	if (lim->memory_swap_max >= 0) {
		snprintf(value, sizeof(value), "%lld", lim->memory_swap_max);
		if (write_cgroup_file(dir, "memory.swap.max", value) != 0) {
			perror("cgroup_create: write memory.swap.max");
			container_set_last_error_step("cgroup_create: write memory.swap.max");
			return -1;
		}
	}

	if (lim->pids_max > 0) {
		snprintf(value, sizeof(value), "%lld", lim->pids_max);
		if (write_cgroup_file(dir, "pids.max", value) != 0) {
			perror("cgroup_create: write pids.max");
			container_set_last_error_step("cgroup_create: write pids.max");
			return -1;
		}
	}

	if (lim->cpu_max != NULL) {
		if (write_cgroup_file(dir, "cpu.max", lim->cpu_max) != 0) {
			perror("cgroup_create: write cpu.max");
			container_set_last_error_step("cgroup_create: write cpu.max");
			return -1;
		}
	}

	if (lim->cpuset_cpus != NULL) {
		if (write_cgroup_file(dir, "cpuset.cpus", lim->cpuset_cpus) != 0) {
			perror("cgroup_create: write cpuset.cpus");
			container_set_last_error_step("cgroup_create: write cpuset.cpus");
			return -1;
		}
	}

	/*
	 * O_CLOEXEC: this fd is only needed by the kernel at clone3() time
	 * (CLONE_INTO_CGROUP reads it during the syscall itself); the
	 * daemon keeps its own copy for bookkeeping, but the fd table this
	 * clone3() inherits into shouldn't carry a lingering duplicate
	 * past that child's own execve().
	 */
	fd = open(dir, O_PATH | O_CLOEXEC);
	if (fd < 0) {
		perror("cgroup_create: open(O_PATH)");
		container_set_last_error_step("cgroup_create: open(O_PATH)");
		return -1;
	}

	*out_fd = fd;
	return 0;
}

/*
 * Best-effort, called once at daemon startup (main()'s init sequence,
 * before any container's cgroup leaf can exist): enables every cgroup
 * v2 controller this project's own container/host-stats code needs,
 * in the root's own subtree_control -- io/cpuset (this function's own
 * original scope) plus memory/pids/cpu, added after a real, confirmed
 * gap: a genuinely fresh cgroup v2 hierarchy (cixd running as real
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
/*
 * Delegates each controller to dir's subtree INDIVIDUALLY, not as one
 * "+cpu +memory +pids +cpuset" write. A single write is all-or-nothing:
 * one controller the kernel does not offer at that level fails the
 * whole line, and every other controller silently never gets delegated.
 * That is how containers requesting a cpuset started failing with a
 * bare 500 the moment they moved under a parent (issue #86) -- the
 * parent had cpu and memory and nothing else, so the child's own
 * cpuset.cpus write had no controller to write to.
 */
static void delegate_controllers(const char *dir)
{
	static const char *const wanted[] = { "+cpu", "+memory", "+pids", "+cpuset", "+io" };
	size_t i;

	for (i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++)
		write_cgroup_file(dir, "cgroup.subtree_control", wanted[i]);
}

/*
 * Issue #85: creates (or updates) a PARENT cgroup that a group of
 * containers will live under, carrying the AGGREGATE limit for all of
 * them, and delegates the controllers its children need.
 *
 * This exists because a per-container limit is not a budget. Cix's own
 * package builder applied its configured cpu_max/memory_max to each
 * build container individually while allowing up to max_concurrent_jobs
 * of them, so the real ceiling was (limit x concurrency) -- on a real
 * 2-CPU box that starved cixd itself off the run queue and, since a
 * shell-less host has no other way in, took the whole machine out of
 * reach until it was reset. A parent cgroup makes the configured number
 * mean what it says: N children can never collectively exceed it,
 * whatever N is.
 *
 * Children are created by passing "<parent>/<child>" as a cgroup_limits
 * name -- cgroup_create()'s own single mkdir() then only has to create
 * the leaf, exactly as it always did.
 */
int cgroup_create_parent(const struct cgroup_limits *lim)
{
	char dir[PATH_MAX];
	char value[32];

	if (snprintf(dir, sizeof(dir), "%s/%s", CGROUP_ROOT, lim->name) >= (int)sizeof(dir)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	/*
	 * mkdir every component, not just the last: a nested parent
	 * ("cix-workload/cix-pkgbuild") is how the build budget is kept
	 * INSIDE the workload budget rather than beside it (issue #86).
	 * Beside it would mean two ceilings that add up to more than the
	 * machine, which is the same class of mistake #85 was.
	 */
	{
		char *slash = dir + strlen(CGROUP_ROOT) + 1;

		for (; *slash != '\0'; slash++) {
			if (*slash != '/')
				continue;
			*slash = '\0';
			if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
				perror("cgroup_create_parent: mkdir (intermediate)");
				*slash = '/';
				return -1;
			}
			/* An intermediate level must delegate too, or the next one
			 * down cannot be created with any controller at all. */
			delegate_controllers(dir);
			*slash = '/';
		}
	}
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		perror("cgroup_create_parent: mkdir");
		return -1;
	}
	/*
	 * Re-applied on every call, not just at creation: the aggregate
	 * budget is operator-configurable at runtime, and a parent that was
	 * created under an older value must pick up the new one rather than
	 * silently keeping the stale ceiling.
	 */
	if (lim->memory_max > 0) {
		snprintf(value, sizeof(value), "%lld", lim->memory_max);
		if (write_cgroup_file(dir, "memory.max", value) != 0)
			perror("cgroup_create_parent: write memory.max");
	}
	if (lim->pids_max > 0) {
		snprintf(value, sizeof(value), "%lld", lim->pids_max);
		if (write_cgroup_file(dir, "pids.max", value) != 0)
			perror("cgroup_create_parent: write pids.max");
	}
	if (lim->cpu_max != NULL) {
		if (write_cgroup_file(dir, "cpu.max", lim->cpu_max) != 0)
			perror("cgroup_create_parent: write cpu.max");
	}
	/*
	 * A cgroup v2 parent must explicitly delegate controllers to its
	 * subtree, or its children cannot set their own limits at all.
	 * Best-effort per controller: a kernel without one of these simply
	 * doesn't get that child-level knob, which is not a reason to fail
	 * the whole build subsystem.
	 */
	delegate_controllers(dir);
	return 0;
}

/*
 * Moves every process in the cgroup-namespace root into a leaf below it
 * (#258), so the root can delegate controllers to its children.
 *
 * Only ever reached inside a container, where the namespace root is an
 * ordinary cgroup subject to the no-internal-process rule. On a host
 * the root cgroup is exempt and the caller's first write succeeds.
 *
 * The leaf is named with a dot so it can never collide with a
 * container: container names are validated as simple names and cannot
 * contain one, so a container called "cixd" would still be a different
 * directory.
 *
 * Reads and re-reads cgroup.procs rather than trusting one pass: moving
 * a process is a write per PID, and the set can change underneath. A
 * bounded number of passes, because an unbounded loop here would hang
 * daemon startup, and the honest outcome of not converging is to say so
 * and carry on without nested controllers.
 */
static int relocate_self_to_leaf(void)
{
	char leaf[PATH_MAX];
	int pass;

	if (snprintf(leaf, sizeof(leaf), "%s/cix.daemon", CGROUP_ROOT) >= (int)sizeof(leaf)) {
		errno = ENAMETOOLONG;
		perror("cgroup_enable_controllers: leaf path");
		return -1;
	}
	if (mkdir(leaf, 0755) != 0 && errno != EEXIST) {
		perror("cgroup_enable_controllers: mkdir leaf");
		return -1;
	}

	for (pass = 0; pass < 8; pass++) {
		char path[PATH_MAX];
		char buf[4096];
		char *saveptr, *tok;
		int fd, moved = 0;
		ssize_t n;

		snprintf(path, sizeof(path), "%s/cgroup.procs", CGROUP_ROOT);
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			perror("cgroup_enable_controllers: open cgroup.procs");
			return -1;
		}
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n < 0) {
			perror("cgroup_enable_controllers: read cgroup.procs");
			return -1;
		}
		buf[n] = '\0';
		if (buf[0] == '\0')
			return 0; /* nothing left in the root -- delegation can proceed */

		for (tok = strtok_r(buf, "\n", &saveptr); tok != NULL;
		     tok = strtok_r(NULL, "\n", &saveptr)) {
			if (tok[0] == '\0')
				continue;
			/* One PID per write: cgroup.procs accepts exactly one,
			 * and a failure to move one process is not a reason to
			 * stop trying the rest. */
			if (write_cgroup_file(leaf, "cgroup.procs", tok) == 0)
				moved++;
		}
		if (moved == 0)
			break; /* made no progress; another pass will not either */
	}

	fprintf(stderr, "cgroup_enable_controllers: could not empty the cgroup namespace root -- "
	                "nested containers will have no controllers (#258)\n");
	errno = EBUSY;
	return -1;
}

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

	if (write_cgroup_file(CGROUP_ROOT, "cgroup.subtree_control", request) == 0)
		return;

	/*
	 * EBUSY here is the cgroup v2 "no internal process" rule, and it is
	 * the difference between running on a host and running inside a
	 * container (#258).
	 *
	 * A cgroup that holds processes may not enable controllers for its
	 * children. The real root cgroup is exempt, which is why this has
	 * always worked on a host: cixd sits in the root and writes this
	 * happily. Inside a container the root of our cgroup NAMESPACE is
	 * an ordinary cgroup, we are in it, and the rule applies -- so the
	 * write failed, no controller was ever delegated, and every
	 * container a nested cixd tried to create found no cpu.max or
	 * memory.max to write. Measured as 19 of 41 remaining failures the
	 * second time the full suite ran (#224).
	 *
	 * The fix is to stop being an internal process: move ourselves into
	 * a leaf below the root and try again. Nothing else needs to
	 * change -- containers are still created as siblings of that leaf,
	 * one level under the namespace root, and they inherit the
	 * controllers this write is enabling.
	 *
	 * Everything above us is untouched, so the parent budgets keep
	 * applying exactly as before (ADR-0165): we are moving DOWN inside
	 * our own cgroup, not out of it.
	 */
	if (errno != EBUSY) {
		perror("cgroup_enable_controllers: write to cgroup.subtree_control");
		return;
	}
	if (relocate_self_to_leaf() != 0)
		return; /* already reported, and the retry below cannot help */
	if (write_cgroup_file(CGROUP_ROOT, "cgroup.subtree_control", request) != 0)
		perror("cgroup_enable_controllers: write to cgroup.subtree_control after relocating");
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
 * Reads cpu.max via cgroup_fd into out (raw "QUOTA PERIOD" text,
 * trailing newline stripped) -- the exact same string shape
 * struct cgroup_limits.cpu_max is written from, so a caller wanting
 * to show "what's actually configured right now" gets it back
 * verbatim, no reinterpretation either direction. The kernel's own
 * unlimited spelling ("max 100000") is returned as-is, not translated
 * to NULL/empty -- that translation is a caller-side display choice,
 * not this function's job. Returns -1 only on a real I/O error (the
 * file not existing at all, e.g. the cpu controller isn't delegated
 * here) -- distinct from "no limit set," which reads back as literal
 * "max <period>" text, not a read failure.
 */
int cgroup_read_cpu_max(int cgroup_fd, char *out, size_t out_size)
{
	char buf[64];
	size_t len;

	out[0] = '\0';
	if (read_whole_file_at(cgroup_fd, "cpu.max", buf, sizeof(buf)) < 0)
		return -1;

	len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	snprintf(out, out_size, "%s", buf);
	return 0;
}

/*
 * Issue #49: the read-back counterpart for cpuset.cpus, same shape as
 * cgroup_read_cpu_max() above. An empty result is the kernel's own
 * "no restriction configured" state (cpuset.cpus starts empty and
 * inherits the parent's effective set) -- callers treat "" as unset,
 * exactly like "max ..." means unset for cpu.max.
 */
int cgroup_read_cpuset(int cgroup_fd, char *out, size_t out_size)
{
	char buf[256];
	size_t len;

	out[0] = '\0';
	if (read_whole_file_at(cgroup_fd, "cpuset.cpus", buf, sizeof(buf)) < 0)
		return -1;

	len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	snprintf(out, out_size, "%s", buf);
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
