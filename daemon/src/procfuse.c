/*
 * ADR-0262 / issue #336 -- the host-side FUSE server behind a
 * container's own /proc/meminfo and friends. See daemon/include/procfuse.h
 * for why it is one server rather than one per container.
 *
 * The FUSE ABI comes from the kernel's own <linux/fuse.h>, not from
 * libfuse. That is not a compromise of ADR-0262's "raw protocol"
 * decision, it is what the decision meant: the argument against libfuse
 * was its ABI surface, its FUSE_USE_VERSION to track and a third-party
 * dependency for one consumer, none of which a uapi header brings. This
 * project already includes <linux/rtnetlink.h>, <linux/netlink.h> and
 * <linux/veth.h> the same way.
 *
 * Checked before relying on it, because this platform has been bitten
 * by a kernel header before: <linux/fuse.h> contains no
 * __attribute__((packed)) at all, so ADR-0008's TCC packing trap --
 * where TCC silently ignores the attribute and lays a struct out
 * differently from the kernel -- cannot apply here. Every field is a
 * naturally aligned u32/u64 with explicit padding.
 */
#include "procfuse.h"

#include "logstore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fuse.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CGROUP_ROOT "/sys/fs/cgroup"
#define FUSE_DEV "/dev/fuse"

/*
 * One request plus its payload. FUSE negotiates max_write in INIT; this
 * is a read-only filesystem that never receives a write, so the only
 * inbound payload of any size is a path component. The kernel will not
 * send more than this because INIT says so.
 */
#define PROCFUSE_BUF_MAX 65536

/* Longest file this synthesises. cpuinfo on a many-CPU host is the one
 * that grows; a stanza is a few hundred bytes and this bounds it well
 * above anything a stanza-per-CPU listing reaches on real hardware. */
#define PROCFUSE_CONTENT_MAX 65536

static const struct procfuse_file g_files[] = PROCFUSE_FILES;

static pid_t g_server_pid = -1;
static char g_mount_path[PATH_MAX];
static int g_mounted;

/* ------------------------------------------------------------------ */
/* cgroup resolution: the request's pid, not the server's own cgroup    */
/* ------------------------------------------------------------------ */

/*
 * /proc/<pid>/cgroup on a cgroup v2 host is one line, "0::/<path>".
 *
 * A container's own line is its path in the HOST's hierarchy, which is
 * what this needs -- the server reads /sys/fs/cgroup/<path>/... from
 * outside. Note the container itself cannot do this: a build container
 * has no cgroup tree mounted at all (measured, probe-cgroup-view/1),
 * which is exactly why the server is on this side.
 *
 * Returns 0 and fills out with an absolute directory under
 * /sys/fs/cgroup, or -1 when the pid is gone, unreadable, or at the
 * root -- in which case the caller reports the host's real numbers,
 * which is the correct answer for a process that is not confined.
 */
static int cgroup_dir_for_pid(uint32_t pid, char *out, size_t out_size)
{
	char path[64];
	char line[PATH_MAX];
	FILE *f;
	char *rel;
	size_t len;

	snprintf(path, sizeof(path), "/proc/%u/cgroup", (unsigned)pid);
	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	line[0] = '\0';
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "0::", 3) == 0)
			break;
		line[0] = '\0';
	}
	fclose(f);
	if (line[0] == '\0')
		return -1;

	rel = line + 3;
	len = strlen(rel);
	while (len > 0 && (rel[len - 1] == '\n' || rel[len - 1] == '\r'))
		rel[--len] = '\0';
	/* "/" means the root cgroup: not confined, so no limits to report. */
	if (len == 0 || (len == 1 && rel[0] == '/'))
		return -1;
	if (snprintf(out, out_size, CGROUP_ROOT "%s", rel) >= (int)out_size)
		return -1;
	return 0;
}

/*
 * One number out of one cgroup file. Returns `absent` for a missing
 * file, an unreadable one, or the literal "max" -- all three mean "no
 * limit here", and collapsing them is right because the caller's next
 * move is identical for each.
 */
static long long cgroup_ll(const char *dir, const char *file, long long absent)
{
	char path[PATH_MAX];
	char buf[64];
	FILE *f;
	long long v;

	if (dir == NULL)
		return absent;
	if (snprintf(path, sizeof(path), "%s/%s", dir, file) >= (int)sizeof(path))
		return absent;
	f = fopen(path, "r");
	if (f == NULL)
		return absent;
	if (fgets(buf, sizeof(buf), f) == NULL) {
		fclose(f);
		return absent;
	}
	fclose(f);
	if (strncmp(buf, "max", 3) == 0)
		return absent;
	errno = 0;
	v = strtoll(buf, NULL, 10);
	if (errno != 0)
		return absent;
	return v;
}

/* One key out of a flat "key value\n" cgroup file (memory.stat). */
static long long cgroup_stat_key(const char *dir, const char *file, const char *key,
                                  long long absent)
{
	char path[PATH_MAX];
	char line[256];
	FILE *f;
	size_t keylen = strlen(key);
	long long v = absent;

	if (dir == NULL)
		return absent;
	if (snprintf(path, sizeof(path), "%s/%s", dir, file) >= (int)sizeof(path))
		return absent;
	f = fopen(path, "r");
	if (f == NULL)
		return absent;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, key, keylen) == 0 && line[keylen] == ' ') {
			v = strtoll(line + keylen + 1, NULL, 10);
			break;
		}
	}
	fclose(f);
	return v;
}

/* The host's own value for a /proc/meminfo key, in kB. */
static long long host_meminfo_kb(const char *key)
{
	char line[256];
	FILE *f;
	size_t keylen = strlen(key);
	long long v = 0;

	f = fopen("/proc/meminfo", "r");
	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, key, keylen) == 0 && line[keylen] == ':') {
			v = strtoll(line + keylen + 1, NULL, 10);
			break;
		}
	}
	fclose(f);
	return v;
}

/* ------------------------------------------------------------------ */
/* content synthesis                                                    */
/* ------------------------------------------------------------------ */

/*
 * Every /proc/meminfo field cgroup v2 can actually answer, in one
 * table, because the alternative is a chain of strcmp()s in the
 * streaming loop and one of them silently not matching.
 *
 * `kb` is the value in kB. A key absent from this table is passed
 * through from the host unchanged.
 *
 * WHY NOT JUST PASS THE HOST'S THROUGH. A 1 GiB container would then
 * report the host's `Active: 3 GB` -- a field larger than its own
 * MemTotal, which is not merely imprecise but arithmetically impossible
 * and will confuse any tool that cross-checks them.
 *
 * WHY NOT JUST EMIT THE FEW WE KNOW. That was the first version: eight
 * fields where the host has fifty, so everything reading Active, Dirty,
 * Slab, SReclaimable, AnonPages, Mapped or PageTables got nothing at
 * all (#359).
 *
 * The fields deliberately left to the host are the ones no cgroup
 * accounts: HugePages_*, Vmalloc*, DirectMap*, CommitLimit,
 * HardwareCorrupted and friends. They describe the machine, not the
 * cgroup, and are the same for every reader.
 */
static int meminfo_override(const char *cg, const char *key, size_t keylen, long long *kb)
{
	long long limit = cgroup_ll(cg, "memory.max", -1);
	long long total_kb, cur_kb;
	long long swap_total_kb, swap_cur_kb;

	if (limit < 0) {
		total_kb = host_meminfo_kb("MemTotal");
		cur_kb = total_kb - host_meminfo_kb("MemFree");
	} else {
		total_kb = limit / 1024;
		cur_kb = cgroup_ll(cg, "memory.current", 0) / 1024;
	}
	if (cur_kb < 0)
		cur_kb = 0;
	if (cur_kb > total_kb)
		cur_kb = total_kb;

	swap_total_kb = cgroup_ll(cg, "memory.swap.max", -1);
	swap_total_kb = swap_total_kb < 0 ? 0 : swap_total_kb / 1024;
	swap_cur_kb = cgroup_ll(cg, "memory.swap.current", 0) / 1024;
	if (swap_cur_kb < 0)
		swap_cur_kb = 0;

#define MSTAT(k) (cgroup_stat_key(cg, "memory.stat", (k), 0) / 1024)
#define MATCH(name) (keylen == sizeof(name) - 1 && strncmp(key, (name), keylen) == 0)

	if (MATCH("MemTotal")) {
		*kb = total_kb;
	} else if (MATCH("MemFree")) {
		*kb = total_kb - cur_kb;
	} else if (MATCH("MemAvailable")) {
		/* Reclaimable page cache counts as available even though it is
		 * in use -- that is what MemAvailable means and why it is not
		 * MemFree. */
		long long avail = total_kb - cur_kb + MSTAT("inactive_file");

		*kb = avail < 0 ? 0 : (avail > total_kb ? total_kb : avail);
	} else if (MATCH("Buffers")) {
		/* cgroup v2 does not separate buffer cache from page cache;
		 * all of it is reported as Cached. Zero here is a real answer,
		 * not a placeholder. */
		*kb = 0;
	} else if (MATCH("Cached")) {
		*kb = MSTAT("file");
	} else if (MATCH("SwapCached")) {
		*kb = 0;
	} else if (MATCH("SwapTotal")) {
		*kb = swap_total_kb;
	} else if (MATCH("SwapFree")) {
		*kb = swap_total_kb - swap_cur_kb;
	} else if (MATCH("Active")) {
		*kb = MSTAT("active_anon") + MSTAT("active_file");
	} else if (MATCH("Inactive")) {
		*kb = MSTAT("inactive_anon") + MSTAT("inactive_file");
	} else if (MATCH("Active(anon)")) {
		*kb = MSTAT("active_anon");
	} else if (MATCH("Inactive(anon)")) {
		*kb = MSTAT("inactive_anon");
	} else if (MATCH("Active(file)")) {
		*kb = MSTAT("active_file");
	} else if (MATCH("Inactive(file)")) {
		*kb = MSTAT("inactive_file");
	} else if (MATCH("Unevictable")) {
		*kb = MSTAT("unevictable");
	} else if (MATCH("Dirty")) {
		*kb = MSTAT("file_dirty");
	} else if (MATCH("Writeback")) {
		*kb = MSTAT("file_writeback");
	} else if (MATCH("AnonPages")) {
		*kb = MSTAT("anon");
	} else if (MATCH("Mapped")) {
		*kb = MSTAT("file_mapped");
	} else if (MATCH("Shmem")) {
		*kb = MSTAT("shmem");
	} else if (MATCH("KReclaimable") || MATCH("SReclaimable")) {
		*kb = MSTAT("slab_reclaimable");
	} else if (MATCH("SUnreclaim")) {
		*kb = MSTAT("slab_unreclaimable");
	} else if (MATCH("Slab")) {
		*kb = MSTAT("slab");
	} else if (MATCH("KernelStack")) {
		*kb = MSTAT("kernel_stack");
	} else if (MATCH("PageTables")) {
		*kb = MSTAT("pagetables");
	} else {
		return 0;
	}
#undef MATCH
#undef MSTAT
	if (*kb < 0)
		*kb = 0;
	return 1;
}

/*
 * The host's /proc/meminfo with every field this cgroup can answer
 * written over it, and the rest passed through.
 *
 * ADR-0262 is explicit that these are never fractions of the limit. A
 * process deciding whether it can allocate reads exactly these fields,
 * and a fixed half is a lie in both directions -- it refuses work that
 * would fit and admits work that will not.
 */
static size_t render_meminfo(const char *cg, char *out, size_t cap)
{
	FILE *f;
	char line[512];
	size_t len = 0;

	f = fopen("/proc/meminfo", "r");
	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *colon = strchr(line, ':');
		long long kb;

		if (colon != NULL &&
		    meminfo_override(cg, line, (size_t)(colon - line), &kb)) {
			len += (size_t)snprintf(out + len, len < cap ? cap - len : 0,
			                         "%.*s:%*lld kB\n", (int)(colon - line), line,
			                         (int)(23 - (colon - line)), kb);
			continue;
		}
		if (len < cap)
			len += (size_t)snprintf(out + len, cap - len, "%s", line);
	}
	fclose(f);
	return len < cap ? len : cap;
}

/*
 * Counts the CPUs in a cpuset list ("0-1", "0,2-3", "" for none).
 * Returns -1 when the file is absent or empty, which means unconfined.
 */
static int cpuset_count(const char *cg)
{
	char path[PATH_MAX];
	char buf[512];
	FILE *f;
	const char *p;
	int n = 0;

	if (cg == NULL)
		return -1;
	if (snprintf(path, sizeof(path), "%s/cpuset.cpus.effective", cg) >= (int)sizeof(path))
		return -1;
	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	if (fgets(buf, sizeof(buf), f) == NULL) {
		fclose(f);
		return -1;
	}
	fclose(f);

	p = buf;
	while (*p != '\0' && *p != '\n') {
		long a, b;
		char *end;

		a = strtol(p, &end, 10);
		if (end == p)
			break;
		p = end;
		b = a;
		if (*p == '-') {
			p++;
			b = strtol(p, &end, 10);
			if (end == p)
				break;
			p = end;
		}
		if (b >= a)
			n += (int)(b - a + 1);
		if (*p == ',')
			p++;
	}
	return n > 0 ? n : -1;
}

/*
 * The host's own per-processor stanzas, renumbered 0..n-1, truncated to
 * the cpuset's count.
 *
 * It follows cpuset.cpus.effective and deliberately NOT cpu.max. That is
 * the measured decision (ADR-0262, probe-cgroup-view): a build
 * container's quota is exactly one CPU while nproc reports 2, and nproc
 * reads sched_getaffinity, which a cpuset constrains and a quota does
 * not. A cpuinfo derived from the quota would report 1 and turn every
 * `make -j$(nproc)` into -j1, with nothing in the build output saying
 * why.
 *
 * Real stanzas rather than a synthetic vendor and model, because things
 * read those fields.
 */
static size_t render_cpuinfo(const char *cg, char *out, size_t cap)
{
	FILE *f;
	char line[512];
	int want = cpuset_count(cg);
	int emitted = 0;
	size_t len = 0;
	int skipping = 0;

	f = fopen("/proc/cpuinfo", "r");
	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "processor", 9) == 0) {
			if (want > 0 && emitted >= want) {
				skipping = 1;
				continue;
			}
			skipping = 0;
			len += (size_t)snprintf(out + len, len < cap ? cap - len : 0,
			                         "processor\t: %d\n", emitted);
			emitted++;
			continue;
		}
		if (skipping)
			continue;
		/*
		 * Topology has to agree with the processor count, or a tool
		 * reads one CPU and two siblings and believes whichever it
		 * asked for second. Measured before this existed: a container
		 * pinned to `cpuset_cpus: "0"` emitted one processor block
		 * carrying the host's `siblings: 2` and `cpu cores: 2`.
		 */
		if (want > 0 && strncmp(line, "siblings", 8) == 0) {
			len += (size_t)snprintf(out + len, len < cap ? cap - len : 0,
			                         "siblings\t: %d\n", want);
			continue;
		}
		if (want > 0 && strncmp(line, "cpu cores", 9) == 0) {
			len += (size_t)snprintf(out + len, len < cap ? cap - len : 0,
			                         "cpu cores\t: %d\n", want);
			continue;
		}
		if (len < cap)
			len += (size_t)snprintf(out + len, cap - len, "%s", line);
	}
	fclose(f);
	return len < cap ? len : cap;
}

/*
 * ------------------------------------------------------------------
 * One time base for every renderer that needs one (#359)
 * ------------------------------------------------------------------
 *
 * /proc/stat's idle column and /proc/uptime's second field are the same
 * quantity in different units, so they are computed once, here. When
 * they were computed separately they were both simply zero; if they
 * were computed separately and correctly they would still drift apart,
 * and a tool that cross-checks them would see a machine whose numbers
 * do not add up.
 */

/*
 * Walks the cgroup's process list ONCE and answers everything that
 * needs it: when the oldest process in this container started, and how
 * many of its tasks are running or blocked right now.
 *
 * WHY THE OLDEST PROCESS AND NOT THE CGROUP DIRECTORY'S mtime. The
 * first version anchored container uptime on the cgroup directory's
 * mtime, which has two faults measured rather than supposed. A
 * directory's mtime moves whenever an entry is added, and this
 * platform's own src/cgroup.c records the case in as many words -- a
 * nested cixd enables subtree_control on its own cgroup, so a container
 * that runs containers gains child cgroups and its uptime would jump
 * backwards to zero. And mtime is realtime, so an NTP step moves it;
 * `starttime` is measured in ticks since boot and cannot be stepped.
 *
 * cix-init is pid 1 in every container for that container's whole life
 * (ADR-0260), so the oldest process is always there to ask.
 *
 * Field 22 of /proc/<pid>/stat is parsed from the LAST ')' rather than
 * by counting from the start: field 2 is the executable name, it is not
 * escaped, and a process really can be called "foo) 1 2 3 (bar".
 */
struct cgroup_procs_info {
	long long oldest_starttime_ticks; /* -1 when nothing was readable */
	long long running;
	long long blocked;
};

static void cgroup_procs_scan(const char *cg, struct cgroup_procs_info *out)
{
	char path[PATH_MAX];
	char line[64];
	FILE *f;

	out->oldest_starttime_ticks = -1;
	out->running = 0;
	out->blocked = 0;

	if (cg == NULL)
		return;
	if (snprintf(path, sizeof(path), "%s/cgroup.procs", cg) >= (int)sizeof(path))
		return;
	f = fopen(path, "r");
	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		char statpath[64];
		char buf[1024];
		FILE *sf;
		char *close_paren;
		char *p;
		size_t n;
		long pid = strtol(line, NULL, 10);
		int field;
		char state = '\0';

		if (pid <= 0)
			continue;
		snprintf(statpath, sizeof(statpath), "/proc/%ld/stat", pid);
		sf = fopen(statpath, "r");
		if (sf == NULL)
			continue; /* exited between the list and the read */
		n = fread(buf, 1, sizeof(buf) - 1, sf);
		fclose(sf);
		buf[n] = '\0';

		close_paren = strrchr(buf, ')');
		if (close_paren == NULL || close_paren[1] == '\0')
			continue;
		p = close_paren + 1;

		/*
		 * p now sits just before field 3. starttime is field 22, so it
		 * is the 20th whitespace-separated token from here.
		 */
		for (field = 3; field <= 22; field++) {
			while (*p == ' ')
				p++;
			if (*p == '\0')
				break;
			if (field == 3)
				state = *p;
			if (field == 22) {
				long long st = strtoll(p, NULL, 10);

				if (st >= 0 && (out->oldest_starttime_ticks < 0 ||
				                st < out->oldest_starttime_ticks))
					out->oldest_starttime_ticks = st;
				break;
			}
			while (*p != '\0' && *p != ' ')
				p++;
		}
		if (state == 'R')
			out->running++;
		else if (state == 'D')
			out->blocked++;
	}
	fclose(f);
}

/* The host's own uptime in seconds, which every fallback here needs. */
static double host_uptime_seconds(void)
{
	FILE *f = fopen("/proc/uptime", "r");
	double up = 0.0;

	if (f == NULL)
		return 0.0;
	if (fscanf(f, "%lf", &up) != 1)
		up = 0.0;
	fclose(f);
	return up;
}

/*
 * How long this container has been up, in seconds.
 *
 * Falls back to the cgroup directory's mtime when the process list
 * cannot be read, and to the host's uptime when there is no cgroup at
 * all -- an unconfined reader is genuinely looking at the host.
 */
static double container_uptime(const char *cg, const struct cgroup_procs_info *procs)
{
	long ticks = sysconf(_SC_CLK_TCK);
	double host_up = host_uptime_seconds();
	struct stat st;

	if (cg == NULL)
		return host_up;
	if (ticks <= 0)
		ticks = 100;
	if (procs->oldest_starttime_ticks >= 0) {
		double up = host_up - (double)procs->oldest_starttime_ticks / (double)ticks;

		return up > 0.0 ? up : 0.0;
	}
	if (stat(cg, &st) == 0) {
		struct timespec now;

		if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
			double up = (double)(now.tv_sec - st.st_mtim.tv_sec) +
			            ((double)now.tv_nsec - (double)st.st_mtim.tv_nsec) / 1e9;

			return up > 0.0 ? up : 0.0;
		}
	}
	return host_up;
}

/*
 * How many processors this container can run on AT ONCE. This is a
 * parallelism bound and it deliberately ignores the quota -- see
 * container_cpu_allowance() below for why the two are different
 * questions, and procfuse.h for why cpuinfo must answer this one.
 */
static int container_ncpu(const char *cg)
{
	int n = cpuset_count(cg);
	long host;

	if (n > 0)
		return n;
	host = sysconf(_SC_NPROCESSORS_ONLN);
	return host > 0 ? (int)host : 1;
}

/*
 * How much CPU this container may consume over time, in CPUs -- which
 * is a THROUGHPUT bound and can be fractional.
 *
 * TWO DIFFERENT LIMITS, AND ONLY ONE OF THEM IS THE CPU COUNT.
 * `cpuset.cpus.effective` says how many CPUs may run this container's
 * tasks simultaneously; `cpu.max` says how much CPU time it may use per
 * period. A container can have one CPU in its cpuset and a quota of
 * half a CPU, and its real ceiling is the smaller of the two.
 *
 * Using the cpuset alone here was a real bug and exactly the one this
 * whole feature exists to prevent. A container pinned to one CPU with
 * `cpu.max` of "50000 100000" cannot exceed half a CPU; running flat
 * out against that cap, a capacity of uptime x 1 CPU makes the busy
 * half of the total and every tool reports 50% -- "half idle" for a
 * workload with no headroom whatsoever, which is #278/#279's own shape:
 * a number that says there is room when there is none. With the quota
 * in the denominator the same container reads ~100%, which is true and
 * is what tells an operator it is throttled.
 *
 * NOT the same question as cpuinfo's processor count, and procfuse.h
 * records why that one follows the cpuset instead: a quota-derived
 * processor count would turn every `make -j$(nproc)` here into -j1.
 * Parallelism and throughput are separate limits and each file answers
 * the one its readers act on.
 */
static double container_cpu_allowance(const char *cg)
{
	double cpus = (double)container_ncpu(cg);
	char path[PATH_MAX];
	char buf[64];
	FILE *f;
	long long quota, period;
	char *end;

	if (cg == NULL)
		return cpus;
	if (snprintf(path, sizeof(path), "%s/cpu.max", cg) >= (int)sizeof(path))
		return cpus;
	f = fopen(path, "r");
	if (f == NULL)
		return cpus;
	if (fgets(buf, sizeof(buf), f) == NULL) {
		fclose(f);
		return cpus;
	}
	fclose(f);
	/* "max <period>" is the unlimited form: the cpuset alone decides. */
	if (strncmp(buf, "max", 3) == 0)
		return cpus;
	quota = strtoll(buf, &end, 10);
	period = strtoll(end, NULL, 10);
	if (quota <= 0 || period <= 0)
		return cpus;
	if ((double)quota / (double)period < cpus)
		cpus = (double)quota / (double)period;
	return cpus;
}

/*
 * The cgroup's CPU accounting in /proc/stat's own units, with the idle
 * column SYNTHESISED rather than left at zero.
 *
 * This is the defect that made #359 worth doing. cgroup v2 accounts
 * only busy time -- there is no idle counter -- and the first version
 * emitted 0 for it. Every CPU-percentage tool computes
 * 100 x (total - idle) / total over a delta, so an idle column pinned
 * at zero makes the denominator the busy time itself and the answer
 * 100% at any load above nothing. Measured on 192.168.15.95: a
 * container given one CPU and one busy loop for 15 s went from
 * `cpu 0 0 0 ...` to `cpu 1501 0 0 ...`, and a container using HALF a
 * CPU would read 100% by the same arithmetic while truly using 50%.
 * That is worse than reporting the host's figures, because it is
 * confidently wrong rather than obviously foreign.
 *
 * Idle is what the container could have used and did not:
 * uptime x ncpus x HZ, less what it did use. Clamped at zero because
 * the two clocks are sampled independently and a busy container can
 * momentarily account for more than the window suggests.
 */
/*
 * A /proc/stat counter MUST NOT go backwards, and a synthesised one can.
 *
 * Measured on 192.168.15.95 (v2.57.25), a container pegged at a
 * half-CPU cap over 20 s: `delta: [1001, 0, 1, -1, ...]` -- the idle
 * column fell by one tick between samples. The cause is real rather
 * than rounding: idle is capacity minus busy, and CFS bandwidth lets a
 * container overrun its nominal quota slightly within a period, so
 * measured busy (1002 ticks) can exceed capacity computed from the
 * nominal quota (1001). Clamping idle at zero stops it going negative;
 * it does not stop it FALLING from one to zero.
 *
 * That matters because every consumer of /proc/stat computes a delta,
 * and several compute it in unsigned arithmetic, where a one-tick
 * decrease becomes a number near 2^64 and the percentage derived from
 * it is nonsense. The kernel's own counters are monotonic and a reader
 * is entitled to rely on it.
 *
 * So the last value served for a cgroup is remembered and never
 * lowered. The table is small and fixed: there are at most
 * REGISTRY_MAX_CONTAINERS live cgroups and this process serves them
 * all.
 *
 * KEYED ON THE PATH, THE CONTAINER'S START TIME, AND ITS ALLOWANCE.
 *
 * The start time, because cgroup names are reused constantly -- build
 * slots are literally named after the slot index (src/cgroup.c says
 * so). Without it, a new container inheriting a recycled name would
 * inherit its predecessor's idle floor and report a pinned,
 * far-too-high idle for its whole life.
 *
 * The allowance, because idle is capacity-so-far less busy-so-far, and
 * changing a container's cpu.max or cpuset changes what capacity-so-far
 * even means. That is an operator action recomputing the basis, not the
 * jitter this floor exists to absorb, so the floor starts over. It is
 * the one moment the served counter may legitimately step down, and it
 * is deliberate: pinning idle to a floor computed under a limit that no
 * longer applies would be a permanently wrong number rather than a
 * momentarily surprising one.
 */
#define IDLE_CACHE_MAX 128

struct idle_cache_entry {
	char cg[PATH_MAX];
	long long starttime_ticks;
	double allowance;
	long long idle;
	unsigned long long used; /* for replacing the least recently seen */
};

static struct idle_cache_entry g_idle_cache[IDLE_CACHE_MAX];
static unsigned long long g_idle_clock;

static long long idle_monotonic(const char *cg, const struct cgroup_procs_info *procs,
                                 double allowance, long long idle)
{
	struct idle_cache_entry *slot = NULL;
	struct idle_cache_entry *oldest = &g_idle_cache[0];
	int i;

	if (cg == NULL)
		return idle;

	for (i = 0; i < IDLE_CACHE_MAX; i++) {
		struct idle_cache_entry *e = &g_idle_cache[i];

		if (e->cg[0] != '\0' && strcmp(e->cg, cg) == 0) {
			slot = e;
			break;
		}
		if (e->cg[0] == '\0') {
			slot = e;
			break;
		}
		if (e->used < oldest->used)
			oldest = e;
	}
	if (slot == NULL)
		slot = oldest;

	/* A recycled name, a different occupant, or a changed limit all
	 * mean the counter has a new basis and the floor starts over. */
	if (slot->cg[0] == '\0' || strcmp(slot->cg, cg) != 0 ||
	    slot->starttime_ticks != procs->oldest_starttime_ticks ||
	    slot->allowance != allowance) {
		snprintf(slot->cg, sizeof(slot->cg), "%s", cg);
		slot->starttime_ticks = procs->oldest_starttime_ticks;
		slot->allowance = allowance;
		slot->idle = idle;
	} else if (idle > slot->idle) {
		slot->idle = idle;
	}
	slot->used = ++g_idle_clock;
	return slot->idle;
}

struct cpu_times {
	long long user;   /* ticks */
	long long system; /* ticks */
	long long idle;   /* ticks */
	int ncpu;
	double uptime; /* seconds */
};

static void cgroup_cpu_times(const char *cg, const struct cgroup_procs_info *procs,
                              struct cpu_times *t)
{
	long long user_usec = cgroup_stat_key(cg, "cpu.stat", "user_usec", 0);
	long long system_usec = cgroup_stat_key(cg, "cpu.stat", "system_usec", 0);
	long ticks = sysconf(_SC_CLK_TCK);
	long long capacity;
	double allowance;

	if (ticks <= 0)
		ticks = 100;
	if (user_usec < 0)
		user_usec = 0;
	if (system_usec < 0)
		system_usec = 0;

	/*
	 * usec -> ticks as (usec * HZ) / 1000000, not usec / (1000000 / HZ):
	 * the latter truncates the divisor first and is wrong for any HZ
	 * that does not divide 1000000 exactly.
	 */
	t->user = user_usec * ticks / 1000000;
	t->system = system_usec * ticks / 1000000;
	t->ncpu = container_ncpu(cg);
	t->uptime = container_uptime(cg, procs);

	/*
	 * The denominator is the ALLOWANCE, not the processor count: a
	 * container held at half a CPU by cpu.max is at its ceiling, and
	 * dividing by a whole CPU would report it half idle.
	 */
	allowance = container_cpu_allowance(cg);
	capacity = (long long)(t->uptime * allowance * (double)ticks);
	t->idle = capacity - t->user - t->system;
	if (t->idle < 0)
		t->idle = 0;
	t->idle = idle_monotonic(cg, procs, allowance, t->idle);
}

/*
 * /proc/uptime: this container's age, and the idle time that goes with
 * it. Both from the one time base above, so the second field here and
 * /proc/stat's idle column are always the same quantity.
 */
static size_t render_uptime(const char *cg, char *out, size_t cap)
{
	struct cgroup_procs_info procs;
	struct cpu_times t;
	long ticks = sysconf(_SC_CLK_TCK);

	if (ticks <= 0)
		ticks = 100;
	if (cg == NULL)
		return (size_t)snprintf(out, cap, "%.2f %.2f\n", host_uptime_seconds(), 0.0);

	cgroup_procs_scan(cg, &procs);
	cgroup_cpu_times(cg, &procs, &t);
	return (size_t)snprintf(out, cap, "%.2f %.2f\n", t.uptime, (double)t.idle / (double)ticks);
}

/*
 * /proc/stat: the host's file with what this cgroup knows written over
 * it, rather than a short file of our own.
 *
 * PASSTHROUGH IS THE POINT, and it is the same shape render_cpuinfo
 * already uses. The first version emitted seven lines of its own and
 * every field it could not compute was a fabricated zero -- which is a
 * different and worse thing than a host value. Two of those zeros were
 * real breakage rather than mere absence:
 *
 *   btime 0    /proc/<pid>/stat's `starttime` is jiffies since HOST
 *              boot and is not namespaced, and ps/top compute a
 *              process's wall-clock start as btime + starttime/HZ. So
 *              btime MUST stay the host's: a container-relative one
 *              would be wrong by the host's whole uptime, and zero puts
 *              every process in 1970.
 *
 *   no cpuN    htop draws no CPU meters at all without per-CPU lines.
 *
 * intr, ctxt, processes and softirq are passed through for the same
 * reason: cgroup v2 does not account them, and the host's true number
 * is a better answer than an invented zero.
 *
 * The per-CPU lines split the aggregate evenly. cgroup v2 has no
 * per-CPU breakdown to draw on, and an even split is the one
 * distribution that is guaranteed to sum back to the total a tool also
 * reads on the `cpu` line.
 */
static size_t render_stat(const char *cg, char *out, size_t cap)
{
	struct cgroup_procs_info procs;
	struct cpu_times t;
	FILE *f;
	char line[4096];
	size_t len = 0;
	int i;

	cgroup_procs_scan(cg, &procs);
	cgroup_cpu_times(cg, &procs, &t);

	f = fopen("/proc/stat", "r");
	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "cpu", 3) == 0 && (line[3] == ' ' || (line[3] >= '0' && line[3] <= '9'))) {
			/*
			 * Both the aggregate and every host per-CPU line are
			 * dropped here; ours are emitted once, in their place,
			 * when the aggregate comes past.
			 */
			if (line[3] != ' ')
				continue;
			len += (size_t)snprintf(out + len, len < cap ? cap - len : 0,
			                         "cpu  %lld 0 %lld %lld 0 0 0 0 0 0\n", t.user, t.system,
			                         t.idle);
			for (i = 0; i < t.ncpu; i++) {
				long long u = t.user / t.ncpu;
				long long s = t.system / t.ncpu;
				long long d = t.idle / t.ncpu;

				/* The remainder lands on cpu0 so the parts sum to
				 * the whole rather than quietly losing up to
				 * ncpu-1 ticks per column. */
				if (i == 0) {
					u += t.user % t.ncpu;
					s += t.system % t.ncpu;
					d += t.idle % t.ncpu;
				}
				len += (size_t)snprintf(out + len, len < cap ? cap - len : 0,
				                         "cpu%d %lld 0 %lld %lld 0 0 0 0 0 0\n", i, u, s, d);
			}
			continue;
		}
		if (strncmp(line, "procs_running", 13) == 0) {
			len += (size_t)snprintf(out + len, len < cap ? cap - len : 0,
			                         "procs_running %lld\n",
			                         procs.running > 0 ? procs.running : 1);
			continue;
		}
		if (strncmp(line, "procs_blocked", 13) == 0) {
			len += (size_t)snprintf(out + len, len < cap ? cap - len : 0,
			                         "procs_blocked %lld\n", procs.blocked);
			continue;
		}
		if (len < cap)
			len += (size_t)snprintf(out + len, cap - len, "%s", line);
	}
	fclose(f);
	return len < cap ? len : cap;
}

/*
 * Load average is a host-wide kernel figure with no per-cgroup
 * equivalent, so this reports the host's own rather than inventing one.
 * Serving it at all is what keeps the mount uniform; lying about it
 * would be worse than the host's true number.
 */
static size_t render_loadavg(const char *cg, char *out, size_t cap)
{
	char buf[256];
	FILE *f;

	(void)cg;
	f = fopen("/proc/loadavg", "r");
	if (f == NULL)
		return (size_t)snprintf(out, cap, "0.00 0.00 0.00 1/1 1\n");
	if (fgets(buf, sizeof(buf), f) == NULL)
		buf[0] = '\0';
	fclose(f);
	return (size_t)snprintf(out, cap, "%s", buf);
}

/* The cgroup's own swap accounting, in /proc/swaps' shape. An empty
 * table (header only) is the honest answer when swap is not permitted
 * here, and is what a container with memory.swap.max of 0 should see. */
static size_t render_swaps(const char *cg, char *out, size_t cap)
{
	long long swap_max = cgroup_ll(cg, "memory.swap.max", -1);
	long long swap_cur = cgroup_ll(cg, "memory.swap.current", 0);
	size_t len = (size_t)snprintf(out, cap,
	                               "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");

	if (swap_max > 0)
		len += (size_t)snprintf(out + len, len < cap ? cap - len : 0,
		                         "/swap\t\t\t\t\tfile\t\t%lld\t\t%lld\t\t-2\n", swap_max / 1024,
		                         (swap_cur < 0 ? 0 : swap_cur) / 1024);
	return len < cap ? len : cap;
}

/*
 * /sys/devices/system/cpu/{online,present,possible} for this container.
 *
 * All three get the same answer -- the container's own cpuset -- because
 * from inside, a CPU it may never be scheduled on is not present in any
 * sense the reader can act on, and a container's set does not change
 * under it the way a host's can with real hotplug.
 *
 * cpuset.cpus.effective is already a kernel range list ("0", "0-1",
 * "0,2-3"), which is exactly this file's format, so it is emitted
 * verbatim rather than re-rendered from a parsed count -- re-rendering
 * would turn "0,2-3" into something that no longer names the same CPUs
 * as /proc/stat's filtered per-cpu lines.
 *
 * An unconfined container falls back to the host's own file, which is
 * the truthful answer for a container that really may use every CPU.
 */
static size_t render_cpu_range(const char *cg, char *out, size_t cap)
{
	char path[PATH_MAX];
	char buf[512];
	FILE *f;
	size_t n;

	buf[0] = '\0';
	if (cg != NULL && snprintf(path, sizeof(path), "%s/cpuset.cpus.effective", cg) <
	                       (int)sizeof(path)) {
		f = fopen(path, "r");
		if (f != NULL) {
			if (fgets(buf, sizeof(buf), f) == NULL)
				buf[0] = '\0';
			fclose(f);
		}
	}
	if (buf[0] == '\0' || buf[0] == '\n') {
		f = fopen("/sys/devices/system/cpu/online", "r");
		if (f != NULL) {
			if (fgets(buf, sizeof(buf), f) == NULL)
				buf[0] = '\0';
			fclose(f);
		}
	}
	if (buf[0] == '\0')
		snprintf(buf, sizeof(buf), "0\n");
	n = strlen(buf);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		n--;
	buf[n] = '\0';
	return (size_t)snprintf(out, cap, "%s\n", buf);
}

/*
 * The set of block-device NAMES (as /proc/partitions lists them:
 * "vdb5", not "/dev/vdb5") that back the mounts in a mountinfo file.
 * Small fixed bound: a container mounts a handful of real filesystems,
 * and one that somehow exceeds it has the overflow omitted rather than
 * a growing allocation in the read path.
 */
#define PARTITIONS_MAX_DEVS 32
#define PARTITIONS_NAME_MAX 64

struct dev_set {
	char name[PARTITIONS_MAX_DEVS][PARTITIONS_NAME_MAX];
	size_t count;
};

static void dev_set_add(struct dev_set *s, const char *name)
{
	size_t i;

	for (i = 0; i < s->count; i++)
		if (strcmp(s->name[i], name) == 0)
			return;
	if (s->count >= PARTITIONS_MAX_DEVS)
		return;
	snprintf(s->name[s->count], PARTITIONS_NAME_MAX, "%s", name);
	s->count++;
}

static int dev_set_has(const struct dev_set *s, const char *name)
{
	size_t i;

	for (i = 0; i < s->count; i++)
		if (strcmp(s->name[i], name) == 0)
			return 1;
	return 0;
}

/*
 * The mount SOURCE of one mountinfo line -- the field after the " - "
 * separator, past the filesystem type. Written into `src` (capacity
 * `cap`); returns 0 on success, -1 when the line has no source.
 *
 * mountinfo after the separator is `<fstype> <source> <superopts>`, so
 * the source is the token two past " - ".
 */
static int mountinfo_source(const char *line, char *src, size_t cap)
{
	const char *sep, *fstype, *source, *end;
	size_t len;

	sep = strstr(line, " - ");
	if (sep == NULL)
		return -1;
	fstype = sep + 3;
	while (*fstype == ' ')
		fstype++;
	source = fstype;
	while (*source != '\0' && *source != ' ')
		source++;
	while (*source == ' ')
		source++;
	if (*source == '\0')
		return -1;
	end = source;
	while (*end != '\0' && *end != ' ' && *end != '\n')
		end++;
	len = (size_t)(end - source);
	if (len == 0 || len >= cap)
		return -1;
	memcpy(src, source, len);
	src[len] = '\0';
	return 0;
}

size_t procfuse_render_partitions_from(const char *mountinfo_path,
                                       const char *partitions_path, char *out,
                                       size_t cap)
{
	struct dev_set devs;
	char line[PATH_MAX * 2];
	FILE *f;
	size_t used = 0;
	int header_written = 0;

	devs.count = 0;

	/*
	 * Gather the device name of every /dev/... source this container
	 * mounts. A source that is not a device path (proc, sysfs, tmpfs, a
	 * fuse tag) has no row in /proc/partitions and is skipped. The set
	 * is keyed on the device NAME, not the line's st_dev (mountinfo
	 * field 3), because a btrfs mount's st_dev is an ANONYMOUS device
	 * (measured inside jump, 2026-09-13: "0:22" for a /dev/vdb5 mount)
	 * that matches no /proc/partitions row -- whereas the source's
	 * basename ("vdb5") is exactly the name column of the row that
	 * describes it. This also sidesteps stat()ing a device node, which
	 * a build container and the dev sandbox cannot do at all.
	 */
	f = fopen(mountinfo_path, "r");
	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		char src[PATH_MAX];
		const char *base;

		if (mountinfo_source(line, src, sizeof(src)) != 0)
			continue;
		if (strncmp(src, "/dev/", 5) != 0)
			continue;
		base = strrchr(src, '/');
		base = (base != NULL) ? base + 1 : src;
		if (*base != '\0')
			dev_set_add(&devs, base);
	}
	fclose(f);

	/*
	 * Copy the header and blank line of the real /proc/partitions
	 * verbatim, then only the data rows whose device name this
	 * container actually backs. Rows are appended byte-for-byte from
	 * the source file; the sscanf is used only to decide keep-or-drop.
	 */
	f = fopen(partitions_path, "r");
	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		unsigned int maj, min;
		unsigned long long blocks;
		char name[PARTITIONS_NAME_MAX];
		size_t linelen = strlen(line);

		if (sscanf(line, "%u %u %llu %63s", &maj, &min, &blocks, name) != 4) {
			/* The header ("major minor  #blocks  name") and the blank
			 * line that follows it: kept once, so the file opens the
			 * same way the kernel's does. */
			if (!header_written && used + linelen < cap) {
				memcpy(out + used, line, linelen);
				used += linelen;
			}
			continue;
		}
		header_written = 1;
		if (!dev_set_has(&devs, name))
			continue;
		if (used + linelen >= cap)
			break;
		memcpy(out + used, line, linelen);
		used += linelen;
	}
	fclose(f);
	return used;
}

/*
 * The FUSE path for /proc/partitions: the reader's mount table lives at
 * /proc/<pid>/mountinfo, and the pid is the host-namespace pid the FUSE
 * header carries, so the host-side server reads the container's own
 * mount view directly.
 */
static size_t render_partitions(uint32_t pid, char *out, size_t cap)
{
	char mountinfo[64];

	snprintf(mountinfo, sizeof(mountinfo), "/proc/%u/mountinfo", (unsigned)pid);
	return procfuse_render_partitions_from(mountinfo, "/proc/partitions", out, cap);
}

/*
 * One entry point so the opcode handler never grows a second copy of
 * the file table's ordering.
 */
size_t procfuse_render_for_cgroup(int index, const char *cg, char *out, size_t cap)
{
	switch (index) {
	case 0:
		return render_meminfo(cg, out, cap);
	case 1:
		return render_cpuinfo(cg, out, cap);
	case 2:
		return render_stat(cg, out, cap);
	case 3:
		return render_uptime(cg, out, cap);
	case 4:
		return render_loadavg(cg, out, cap);
	case 5:
		return render_swaps(cg, out, cap);
	case 6:
	case 7:
	case 8:
		return render_cpu_range(cg, out, cap);
	default:
		return 0;
	}
}

/*
 * The FUSE path: resolve the asking process to its cgroup, then render
 * through the one function above. Split from it so a test can drive
 * every renderer against a crafted cgroup directory without FUSE, root,
 * or a real container -- and so what the test exercises is the code
 * that actually serves reads, not a second copy of the arithmetic
 * (#359).
 */
static size_t render_file(int index, uint32_t pid, char *out, size_t cap)
{
	char cgbuf[PATH_MAX];
	const char *cg = NULL;

	/* /proc/partitions is a function of the reader's mount namespace,
	 * not its cgroup, so it takes the pid directly (ADR-0286 tier 1). */
	if (index == PROCFUSE_INDEX_PARTITIONS)
		return render_partitions(pid, out, cap);

	if (cgroup_dir_for_pid(pid, cgbuf, sizeof(cgbuf)) == 0)
		cg = cgbuf;
	return procfuse_render_for_cgroup(index, cg, out, cap);
}


/* ------------------------------------------------------------------ */
/* the /dev/fuse protocol                                               */
/* ------------------------------------------------------------------ */

/*
 * Every reply is a fuse_out_header followed by an optional body, in one
 * write. The kernel matches it to the request by `unique`, so a reply
 * that loses that field is a request that never completes -- and a
 * process blocked in read() with no error anywhere, which is the worst
 * shape this server can fail in.
 */
static void reply(int fd, uint64_t unique, int error, const void *body, size_t body_len)
{
	char buf[PROCFUSE_CONTENT_MAX + sizeof(struct fuse_out_header)];
	struct fuse_out_header *h = (struct fuse_out_header *)buf;

	if (error != 0)
		body_len = 0;
	if (body_len > sizeof(buf) - sizeof(*h))
		body_len = sizeof(buf) - sizeof(*h);

	h->len = (uint32_t)(sizeof(*h) + body_len);
	h->error = error;
	h->unique = unique;
	if (body_len > 0 && body != NULL)
		memcpy(buf + sizeof(*h), body, body_len);
	/* A short or failed write here is not recoverable for this request;
	 * the kernel times it out. Nothing to do but continue serving. */
	if (write(fd, buf, sizeof(*h) + body_len) < 0)
		return;
}

/*
 * attr_valid and entry_valid are ZERO everywhere, deliberately.
 *
 * The content of every file here depends on WHO is reading it. A
 * non-zero validity lets the kernel serve one container's cached size or
 * mtime to another, which is not a stale number -- it is a different
 * container's number. ADR-0262 records this as load-bearing rather than
 * a tuning choice, and lxcfs sets it the same way for the same reason.
 */
static void fill_attr(struct fuse_attr *a, uint64_t ino)
{
	memset(a, 0, sizeof(*a));
	a->ino = ino;
	a->size = 0; /* a synthetic file has no size until it is read */
	a->blocks = 0;
	a->mode = (ino == FUSE_ROOT_ID) ? (S_IFDIR | 0555) : (S_IFREG | 0444);
	a->nlink = (ino == FUSE_ROOT_ID) ? 2 : 1;
	a->uid = 0;
	a->gid = 0;
	a->blksize = 4096;
}

/* Table index for an inode, or -1 if it is not one of ours. */
static int index_for_ino(uint64_t ino)
{
	if (ino < PROCFUSE_INO_FIRST || ino >= PROCFUSE_INO_FIRST + PROCFUSE_FILE_COUNT)
		return -1;
	return (int)(ino - PROCFUSE_INO_FIRST);
}

static void handle_lookup(int fd, const struct fuse_in_header *hdr, const char *name)
{
	struct fuse_entry_out out;
	int i;

	for (i = 0; i < PROCFUSE_FILE_COUNT; i++) {
		if (strcmp(name, g_files[i].name) != 0)
			continue;
		memset(&out, 0, sizeof(out));
		out.nodeid = PROCFUSE_INO_FIRST + i;
		out.generation = 1;
		out.entry_valid = 0;
		out.attr_valid = 0;
		fill_attr(&out.attr, out.nodeid);
		reply(fd, hdr->unique, 0, &out, sizeof(out));
		return;
	}
	reply(fd, hdr->unique, -ENOENT, NULL, 0);
}

static void handle_getattr(int fd, const struct fuse_in_header *hdr)
{
	struct fuse_attr_out out;

	if (hdr->nodeid != FUSE_ROOT_ID && index_for_ino(hdr->nodeid) < 0) {
		reply(fd, hdr->unique, -ENOENT, NULL, 0);
		return;
	}
	memset(&out, 0, sizeof(out));
	out.attr_valid = 0;
	fill_attr(&out.attr, hdr->nodeid);
	reply(fd, hdr->unique, 0, &out, sizeof(out));
}

static void handle_open(int fd, const struct fuse_in_header *hdr)
{
	struct fuse_open_out out;

	if (index_for_ino(hdr->nodeid) < 0) {
		reply(fd, hdr->unique, -ENOENT, NULL, 0);
		return;
	}
	memset(&out, 0, sizeof(out));
	/*
	 * FOPEN_DIRECT_IO is the other half of the per-reader correctness
	 * above: without it the page cache answers the second reader with
	 * the first reader's bytes, and the second reader is a different
	 * container. Not a performance knob.
	 */
	out.open_flags = FOPEN_DIRECT_IO;
	reply(fd, hdr->unique, 0, &out, sizeof(out));
}

static void handle_read(int fd, const struct fuse_in_header *hdr, const struct fuse_read_in *in)
{
	char content[PROCFUSE_CONTENT_MAX];
	size_t len;
	int idx = index_for_ino(hdr->nodeid);

	if (idx < 0) {
		reply(fd, hdr->unique, -ENOENT, NULL, 0);
		return;
	}
	/*
	 * Rendered per read, from the REQUESTER's pid -- hdr->pid, never
	 * this server's own cgroup. That single field is what makes one
	 * server correct for every container.
	 */
	len = render_file(idx, hdr->pid, content, sizeof(content));
	if (len > sizeof(content))
		len = sizeof(content);
	if (in->offset >= len) {
		reply(fd, hdr->unique, 0, NULL, 0);
		return;
	}
	len -= (size_t)in->offset;
	if (len > in->size)
		len = in->size;
	reply(fd, hdr->unique, 0, content + in->offset, len);
}

static void handle_readdir(int fd, const struct fuse_in_header *hdr, const struct fuse_read_in *in)
{
	char buf[PROCFUSE_CONTENT_MAX];
	size_t used = 0;
	int i;

	if (hdr->nodeid != FUSE_ROOT_ID) {
		reply(fd, hdr->unique, -ENOTDIR, NULL, 0);
		return;
	}
	for (i = 0; i < PROCFUSE_FILE_COUNT; i++) {
		struct fuse_dirent *d = (struct fuse_dirent *)(buf + used);
		size_t namelen = strlen(g_files[i].name);
		size_t entlen = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + namelen);

		/* offset is the index of the NEXT entry, so a resumed readdir
		 * continues rather than repeating. */
		if ((uint64_t)i < in->offset)
			continue;
		if (used + entlen > sizeof(buf) || used + entlen > in->size)
			break;
		memset(d, 0, entlen);
		d->ino = PROCFUSE_INO_FIRST + i;
		d->off = i + 1;
		d->namelen = (uint32_t)namelen;
		d->type = DT_REG;
		memcpy(d->name, g_files[i].name, namelen);
		used += entlen;
	}
	reply(fd, hdr->unique, 0, buf, used);
}

static void handle_init(int fd, const struct fuse_in_header *hdr, const struct fuse_init_in *in)
{
	struct fuse_init_out out;

	memset(&out, 0, sizeof(out));
	out.major = FUSE_KERNEL_VERSION;
	/*
	 * Never offer a minor the kernel did not ask for: the reply is the
	 * negotiated version, and claiming newer than the peer understands
	 * is how a mount comes up and then misbehaves in a way that reads
	 * as a filesystem bug.
	 */
	out.minor = in->minor < FUSE_KERNEL_MINOR_VERSION ? in->minor : FUSE_KERNEL_MINOR_VERSION;
	out.max_readahead = in->max_readahead;
	out.max_write = PROCFUSE_BUF_MAX - 4096;
	out.max_background = 8;
	out.congestion_threshold = 4;
	reply(fd, hdr->unique, 0, &out, sizeof(out));
}

static void handle_statfs(int fd, const struct fuse_in_header *hdr)
{
	struct fuse_statfs_out out;

	memset(&out, 0, sizeof(out));
	out.st.namelen = 255;
	out.st.bsize = 4096;
	out.st.files = PROCFUSE_FILE_COUNT;
	reply(fd, hdr->unique, 0, &out, sizeof(out));
}

/*
 * The server loop. Blocks in read() on /dev/fuse, which is precisely why
 * it is a separate process: ADR-0247 refuses a blocking read anywhere in
 * the reactor, and being forked is what makes blocking here correct
 * rather than a violation.
 */
static void procfuse_main(int fd)
{
	static char buf[PROCFUSE_BUF_MAX];

	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		const struct fuse_in_header *hdr;
		const void *body;

		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			/* ENODEV is the mount going away: an orderly end, not a
			 * failure, and the parent must not treat it as a crash to
			 * restart into a mount that no longer exists. */
			_exit(errno == ENODEV ? 0 : 1);
		}
		if ((size_t)n < sizeof(*hdr))
			continue;
		hdr = (const struct fuse_in_header *)buf;
		body = buf + sizeof(*hdr);

		switch (hdr->opcode) {
		case FUSE_INIT:
			handle_init(fd, hdr, (const struct fuse_init_in *)body);
			break;
		case FUSE_LOOKUP:
			handle_lookup(fd, hdr, (const char *)body);
			break;
		case FUSE_GETATTR:
			handle_getattr(fd, hdr);
			break;
		case FUSE_OPEN:
		case FUSE_OPENDIR:
			if (hdr->opcode == FUSE_OPENDIR) {
				struct fuse_open_out out;

				memset(&out, 0, sizeof(out));
				reply(fd, hdr->unique, 0, &out, sizeof(out));
			} else {
				handle_open(fd, hdr);
			}
			break;
		case FUSE_READ:
			handle_read(fd, hdr, (const struct fuse_read_in *)body);
			break;
		case FUSE_READDIR:
			handle_readdir(fd, hdr, (const struct fuse_read_in *)body);
			break;
		case FUSE_RELEASE:
		case FUSE_RELEASEDIR:
		case FUSE_FLUSH:
			reply(fd, hdr->unique, 0, NULL, 0);
			break;
		case FUSE_FORGET:
			/* No reply, by protocol. Replying to it desynchronises the
			 * channel for every request after it. */
			break;
		case FUSE_STATFS:
			handle_statfs(fd, hdr);
			break;
		default:
			reply(fd, hdr->unique, -ENOSYS, NULL, 0);
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                            */
/* ------------------------------------------------------------------ */

const char *procfuse_mount_path(void)
{
	return g_mounted ? g_mount_path : NULL;
}

int procfuse_start(const char *state_dir)
{
	char opts[256];
	int fd;
	pid_t pid;

	if (snprintf(g_mount_path, sizeof(g_mount_path), "%s/%s", state_dir,
	              PROCFUSE_MOUNT_DIRNAME) >= (int)sizeof(g_mount_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (mkdir(g_mount_path, 0755) != 0 && errno != EEXIST) {
		logstore_write("cixd", "error", "procfuse: cannot create %s: %s", g_mount_path,
		                strerror(errno));
		return -1;
	}

	/*
	 * Opened here, in the parent, so the reason reaches the log store
	 * with a real errno. A kernel without FUSE gives ENOENT on this
	 * open, and that is a diagnosis rather than a mystery -- the whole
	 * feature then stays off and containers read the host's /proc,
	 * which is what they do today.
	 */
	fd = open(FUSE_DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		logstore_write("cixd", "info",
		                "procfuse: %s is not available (%s) -- containers will read the host's "
		                "own /proc, as they did before this feature",
		                FUSE_DEV, strerror(errno));
		return -1;
	}

	/*
	 * allow_other, because the readers are containers.
	 *
	 * A FUSE mount is by default reachable only by the uid that mounted
	 * it -- root, here. Every container on this platform is idmapped by
	 * default (ADR-0207), so its processes run as mapped uids and would
	 * be refused with EACCES on a file bound in front of their /proc.
	 * Since the whole point is that a container reads these, the mount
	 * has to permit it.
	 *
	 * Safe to widen here in a way it would not be for a general
	 * filesystem: this one is read-only, serves six synthetic files,
	 * and every answer is computed from the READER's own cgroup -- so
	 * "another user can open it" grants them their own numbers, which
	 * is exactly what they would get anyway.
	 */
	snprintf(opts, sizeof(opts), "fd=%d,rootmode=40000,user_id=0,group_id=0,allow_other", fd);
	if (mount("cix-procfuse", g_mount_path, "fuse", MS_NOSUID | MS_NODEV, opts) != 0) {
		logstore_write("cixd", "error", "procfuse: mount %s failed: %s", g_mount_path,
		                strerror(errno));
		close(fd);
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		logstore_write("cixd", "error", "procfuse: fork failed: %s", strerror(errno));
		umount2(g_mount_path, MNT_DETACH);
		close(fd);
		return -1;
	}
	if (pid == 0) {
		/*
		 * A fork of cixd inherits the HTTPS listener, every accepted
		 * connection, the epoll set and every pidfd. A child holding
		 * the listener is a daemon that cannot be restarted, for a
		 * reason nothing in the restart path would name -- so close
		 * everything except the one descriptor this process exists to
		 * serve. stallwatch gets away without this because it opens
		 * nothing.
		 */
		int keep = fd;

		if (keep != 3) {
			if (dup2(keep, 3) < 0)
				_exit(1);
			keep = 3;
		}
		close_range(4, ~0U, 0);
		signal(SIGPIPE, SIG_IGN);
		/*
		 * Name the child so it is not a third anonymous "cixd" in
		 * GET /system/processes and the watchdog record (#456). The
		 * whole of #448 turned on identifying this exact process --
		 * pid 133, "cixd", wchan fuse_dev_do_read -- as the procfuse
		 * server by inference; a name says it outright. 15 chars fit
		 * TASK_COMM_LEN's budget exactly.
		 */
		prctl(PR_SET_NAME, (unsigned long)"cixd [procfuse]", 0UL, 0UL, 0UL);
		procfuse_main(keep);
		_exit(0);
	}

	close(fd); /* the child owns it now */
	g_server_pid = pid;
	g_mounted = 1;
	logstore_write("cixd", "info", "procfuse: serving %d virtualised /proc files at %s (pid %d)",
	                PROCFUSE_FILE_COUNT, g_mount_path, (int)pid);
	return 0;
}

void procfuse_stop(void)
{
	if (g_server_pid > 0) {
		kill(g_server_pid, SIGTERM);
		waitpid(g_server_pid, NULL, 0);
		g_server_pid = -1;
	}
	if (g_mounted) {
		umount2(g_mount_path, MNT_DETACH);
		g_mounted = 0;
	}
}
