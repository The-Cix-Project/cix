/*
 * test_procfuse -- the arithmetic behind a container's own /proc.
 *
 * #359. The first version of these renderers shipped and was verified
 * for the wrong property: /proc/stat advanced, so it looked right. What
 * it never did was report a believable PERCENTAGE. cgroup v2 accounts
 * only busy time, the idle column was left at zero, and every CPU tool
 * computes 100 x (total - idle) / total over a delta -- so the
 * denominator was the busy time itself and the answer was 100% at any
 * load above nothing. A container using half a CPU read 100%.
 *
 * So this file asserts the SUMS, not the formatting. The central case
 * is the one that was wrong: 7.5 CPU-seconds used, 15 seconds of
 * uptime, one CPU -- a tool must derive 50%.
 *
 * Every renderer takes a cgroup DIRECTORY, so all of it runs against a
 * crafted directory of ordinary files in /tmp: no FUSE, no /dev/fuse,
 * no root, no container. That matters because a build container can
 * create none of those (#224), and this is the gate that has to run
 * where the release is built.
 */
#include "procfuse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, what)                                                                       \
	do {                                                                                        \
		if (!(cond)) {                                                                          \
			fprintf(stderr, "FAIL: %s\n", (what));                                              \
			g_failures++;                                                                       \
		} else {                                                                                \
			printf("  ok: %s\n", (what));                                                       \
		}                                                                                       \
	} while (0)

/* procfuse.c logs through the log store; a test provides its own so the
 * renderers can be linked without the daemon behind them. */
void logstore_write(const char *source, const char *level, const char *fmt, ...);
void logstore_write(const char *source, const char *level, const char *fmt, ...)
{
	(void)source;
	(void)level;
	(void)fmt;
}

static char g_dir[256];

/*
 * Ages the fake cgroup directory to SECS seconds old, with matching
 * nanoseconds.
 *
 * Nanoseconds matter: utimes(2) takes microseconds, and zeroing them
 * leaves the directory somewhere between 15.00 and 16.00 seconds old
 * depending on where in the current second the test happens to run --
 * which moved the derived idle by 60 ticks between runs. That is the
 * #309 whole-second shape, met here in a test's own setup.
 */
static void age_dir(int secs)
{
	struct timespec now;
	struct timespec times[2];

	if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
		perror("clock_gettime");
		exit(1);
	}
	times[0].tv_sec = now.tv_sec - secs;
	times[0].tv_nsec = now.tv_nsec;
	times[1] = times[0];
	if (utimensat(AT_FDCWD, g_dir, times, 0) != 0) {
		perror("utimensat");
		exit(1);
	}
}

/*
 * Writes one cgroup file and RE-AGES the directory, because writing a
 * file into a directory updates that directory's own mtime -- which is
 * the fallback container_uptime() reads here, so without this every
 * mid-test edit silently reset the container's apparent age to zero and
 * took the idle column with it.
 *
 * That is not merely a test artefact. It is exactly the instability
 * that moved the production anchor off the directory's mtime and onto
 * the oldest starttime in cgroup.procs (#359): src/cgroup.c records
 * that a nested cixd creates cgroups under its own, and that would
 * reset a real container's uptime the same way this reset the test's.
 */
static void put(const char *name, const char *content)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", g_dir, name);
	f = fopen(path, "w");
	if (f == NULL) {
		fprintf(stderr, "FAIL: cannot write %s\n", path);
		exit(1);
	}
	fputs(content, f);
	fclose(f);
	age_dir(15);
}

/* The value of a "key: N kB" line, or -1 when the key is absent. */
static long long field_kb(const char *text, const char *key)
{
	const char *p = text;
	size_t keylen = strlen(key);

	while (p != NULL && *p != '\0') {
		if (strncmp(p, key, keylen) == 0 && p[keylen] == ':')
			return strtoll(p + keylen + 1, NULL, 10);
		p = strchr(p, '\n');
		if (p != NULL)
			p++;
	}
	return -1;
}

/* The ten columns of a named /proc/stat cpu line. */
static int cpu_line(const char *text, const char *name, long long *out)
{
	const char *p = text;
	size_t namelen = strlen(name);
	int i;

	while (p != NULL && *p != '\0') {
		if (strncmp(p, name, namelen) == 0 && (p[namelen] == ' ')) {
			const char *q = p + namelen;

			for (i = 0; i < 10; i++) {
				out[i] = strtoll(q, (char **)&q, 10);
			}
			return 0;
		}
		p = strchr(p, '\n');
		if (p != NULL)
			p++;
	}
	return -1;
}

int main(void)
{
	char buf[65536];
	size_t n;
	long long c[10];
	long long busy, idle;

	snprintf(g_dir, sizeof(g_dir), "/tmp/procfuse_test_%d", (int)getpid());
	if (mkdir(g_dir, 0755) != 0) {
		perror("mkdir");
		return 1;
	}

	/*
	 * THE SCENARIO IS THE ONE THAT WAS MEASURED WRONG on the box: one
	 * CPU, 1 GiB, and 7.5 CPU-seconds used over 15 seconds of uptime.
	 * A tool must derive 50%. The shipped renderer derived 100%,
	 * because idle was zero and the denominator was the busy time.
	 *
	 * The directory's mtime IS the uptime here -- cgroup.procs lists no
	 * live pid, so container_uptime() takes its documented fallback --
	 * so the directory is aged 15 seconds deliberately. Without that it
	 * would be zero seconds old, capacity would be zero, and idle would
	 * clamp to zero for an honest reason, which is a degenerate case
	 * rather than the bug.
	 */
	put("cpu.stat", "usage_usec 7500000\nuser_usec 7000000\nsystem_usec 500000\n");
	put("cpuset.cpus.effective", "0\n");
	put("memory.max", "1073741824\n");
	put("memory.current", "104857600\n");
	put("memory.swap.max", "0\n");
	put("memory.swap.current", "0\n");
	put("memory.stat",
	    "anon 52428800\nfile 41943040\ninactive_file 20971520\nactive_file 20971520\n"
	    "active_anon 31457280\ninactive_anon 20971520\nfile_dirty 1048576\n"
	    "file_writeback 524288\nfile_mapped 2097152\nshmem 1048576\nslab 4194304\n"
	    "slab_reclaimable 3145728\nslab_unreclaimable 1048576\nkernel_stack 262144\n"
	    "pagetables 524288\nunevictable 0\n");
	put("cgroup.procs", "");

	printf("1. /proc/stat: idle is real, so a percentage is derivable\n");
	n = procfuse_render_for_cgroup(2, g_dir, buf, sizeof(buf));
	buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
	CHECK(cpu_line(buf, "cpu ", c) == 0, "an aggregate cpu line is present");
	busy = c[0] + c[2];
	idle = c[3];
	CHECK(busy == 750, "7.5 CPU-seconds is 750 ticks of busy time");
	CHECK(idle > 0, "idle is NOT zero -- the #359 defect");
	/*
	 * The whole point, stated as a tool would compute it. Anything
	 * short of a real idle column makes this exactly 100.
	 */
	/*
	 * A BAND, NOT AN EQUALITY, and for the same reason test_timebounds
	 * exists: the uptime in this sum is real elapsed time, so it is
	 * 15.0-something seconds and idle lands within a few ticks of 750.
	 * The first draft of this assertion demanded exactly 750 and
	 * exactly 50%, and failed on the two-hundredths of a second the
	 * test itself takes to run -- a bound decided by timing noise
	 * rather than by the code under test, which is the shape #309 was
	 * about.
	 *
	 * The band is wide enough to be immune to that and far too narrow
	 * to admit the bug: the defect reads 100%, and nothing between 45
	 * and 55 is reachable from an idle column of zero.
	 */
	CHECK(idle >= 740 && idle <= 760,
	      "15s x 1 CPU less 750 ticks used leaves about 750 idle");
	{
		long long pct = 100 * busy / (busy + idle);

		CHECK(pct >= 45 && pct <= 55, "a tool derives ~50% -- the shipped renderer derived 100%");
		printf("     (derived %lld%%, busy=%lld idle=%lld)\n", pct, busy, idle);
	}

	printf("1b. the CPU CAP counts, not just the cpuset\n");
	{
		/*
		 * The same container, now also held to half a CPU by cpu.max.
		 * Its 7.5 CPU-seconds over 15 s IS half a CPU, so it is
		 * running flat out at its ceiling and a tool must say so.
		 *
		 * Reading the cpuset alone put a whole CPU in the denominator
		 * and reported 50% -- "half idle" for a workload that cannot
		 * go any faster, which is #278/#279's own shape: a number
		 * claiming headroom that does not exist.
		 */
		long long cc[10];
		long long cbusy, cidle, pct;

		put("cpu.max", "50000 100000");
		n = procfuse_render_for_cgroup(2, g_dir, buf, sizeof(buf));
		buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
		CHECK(cpu_line(buf, "cpu ", cc) == 0, "an aggregate cpu line is present under a quota");
		cbusy = cc[0] + cc[2];
		cidle = cc[3];
		pct = 100 * cbusy / (cbusy + cidle > 0 ? cbusy + cidle : 1);
		CHECK(pct >= 90, "a container pegged at its cpu.max reads ~100%, not 50%");
		printf("     (derived %lld%%, busy=%lld idle=%lld)\n", pct, cbusy, cidle);

		/* A quota LARGER than the cpuset cannot raise the ceiling. */
		put("cpu.max", "400000 100000");
		n = procfuse_render_for_cgroup(2, g_dir, buf, sizeof(buf));
		buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
		cpu_line(buf, "cpu ", cc);
		CHECK(cc[3] >= 740 && cc[3] <= 760,
		      "a quota above the cpuset's own count does not widen the denominator");

		/* "max" is the unlimited form and must not be read as a number. */
		put("cpu.max", "max 100000");
		n = procfuse_render_for_cgroup(2, g_dir, buf, sizeof(buf));
		buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
		cpu_line(buf, "cpu ", cc);
		CHECK(cc[3] >= 740 && cc[3] <= 760, "cpu.max of \"max\" means the cpuset alone decides");

		/*
		 * cpuinfo deliberately does NOT follow the quota: it is a
		 * parallelism bound, and a quota-derived processor count would
		 * turn every make -j$(nproc) into -j1 (procfuse.h).
		 */
		put("cpu.max", "50000 100000");
		n = procfuse_render_for_cgroup(1, g_dir, buf, sizeof(buf));
		buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
		CHECK(strstr(buf, "processor\t: 0\n") != NULL,
		      "cpuinfo still reports a whole processor under a half-CPU quota");

		/* Restored, so the cases below see the scenario they describe. */
		put("cpu.max", "max 100000");
	}

	printf("2. /proc/stat: the parts sum to the whole\n");
	n = procfuse_render_for_cgroup(2, g_dir, buf, sizeof(buf));
	buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
	{
		long long c0[10];

		/* Re-read the aggregate from THIS render: the per-CPU lines
		 * are compared against the total they were split from. */
		cpu_line(buf, "cpu ", c);

		CHECK(cpu_line(buf, "cpu0", c0) == 0, "a per-CPU line is present (htop draws nothing without one)");
		CHECK(c0[0] == c[0] && c0[2] == c[2] && c0[3] == c[3],
		      "on one CPU, cpu0 carries the whole aggregate");
	}
	CHECK(strstr(buf, "btime 0\n") == NULL,
	      "btime is not zero -- ps computes wall-clock start from it");
	CHECK(strstr(buf, "btime ") != NULL, "btime is passed through from the host");

	printf("3. /proc/uptime: the second field agrees with /proc/stat's idle\n");
	{
		char ubuf[4096];
		double up, uidle;
		long ticks = sysconf(_SC_CLK_TCK);

		n = procfuse_render_for_cgroup(3, g_dir, ubuf, sizeof(ubuf));
		ubuf[n < sizeof(ubuf) ? n : sizeof(ubuf) - 1] = '\0';
		CHECK(sscanf(ubuf, "%lf %lf", &up, &uidle) == 2, "uptime has both fields");
		CHECK(uidle > 0.0, "the idle field is not zero");
		/* Same quantity, different units: within one tick of rounding. */
		CHECK(uidle * ticks >= (double)idle - ticks && uidle * ticks <= (double)idle + ticks,
		      "uptime's idle and /proc/stat's idle are the same number");
	}

	printf("4. /proc/meminfo: the cgroup's own figures, and nothing dropped\n");
	n = procfuse_render_for_cgroup(0, g_dir, buf, sizeof(buf));
	buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
	CHECK(field_kb(buf, "MemTotal") == 1048576, "MemTotal is the cgroup's limit, not the host's");
	CHECK(field_kb(buf, "MemFree") == 1048576 - 102400, "MemFree is the limit less what is used");
	CHECK(field_kb(buf, "Cached") == 40960, "Cached comes from memory.stat's file");
	CHECK(field_kb(buf, "AnonPages") == 51200, "AnonPages comes from memory.stat's anon");
	CHECK(field_kb(buf, "Dirty") == 1024, "Dirty comes from memory.stat's file_dirty");
	CHECK(field_kb(buf, "Slab") == 4096, "Slab comes from memory.stat");
	CHECK(field_kb(buf, "SReclaimable") == 3072, "SReclaimable comes from memory.stat");
	CHECK(field_kb(buf, "Active") == 30720 + 20480, "Active sums the cgroup's two active lists");
	/*
	 * The failure this replaced: eight fields where the host has fifty,
	 * so everything else read nothing at all.
	 */
	{
		int lines = 0;
		const char *p = buf;

		while ((p = strchr(p, '\n')) != NULL) {
			lines++;
			p++;
		}
		CHECK(lines > 20, "the host's other fields are passed through, not dropped");
	}
	CHECK(field_kb(buf, "Active") <= field_kb(buf, "MemTotal"),
	      "no field exceeds MemTotal -- the host's own Active would");

	printf("5. /proc/cpuinfo: topology agrees with the processor count\n");
	n = procfuse_render_for_cgroup(1, g_dir, buf, sizeof(buf));
	buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
	{
		int procs = 0;
		const char *p = buf;

		while ((p = strstr(p, "processor")) != NULL) {
			procs++;
			p++;
		}
		CHECK(procs == 1, "one processor block for a one-CPU cpuset");
		CHECK(strstr(buf, "siblings\t: 1\n") != NULL || strstr(buf, "siblings") == NULL,
		      "siblings matches the processor count rather than the host's");
	}

	printf("6. an unconfined reader (no cgroup) still gets a valid file\n");
	n = procfuse_render_for_cgroup(2, NULL, buf, sizeof(buf));
	buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
	CHECK(cpu_line(buf, "cpu ", c) == 0, "the host's own /proc/stat shape is returned");

	{
		char cmd[512];

		snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
		if (system(cmd) != 0)
			fprintf(stderr, "  (could not remove %s)\n", g_dir);
	}

	if (g_failures != 0) {
		printf("PROCFUSE RESULT: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("PROCFUSE RESULT: PASS\n");
	return 0;
}
