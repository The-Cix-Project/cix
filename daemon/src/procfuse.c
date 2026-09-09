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

static const char *const g_files[] = PROCFUSE_FILES;

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
 * MemTotal is the cgroup's limit; MemFree and MemAvailable are that
 * limit minus what the cgroup is actually using.
 *
 * ADR-0262 is explicit that these are never fractions of the limit. A
 * process deciding whether it can allocate reads exactly these fields,
 * and a fixed half is a lie in both directions -- it refuses work that
 * would fit and admits work that will not.
 *
 * Cached and Buffers come from memory.stat's `file` and `inactive_file`
 * rather than being invented: a tool subtracting Cached from used
 * memory gets an answer that means something.
 */
static size_t render_meminfo(const char *cg, char *out, size_t cap)
{
	long long total_kb, cur_kb, avail_kb, file_kb, inactive_kb, swap_total_kb, swap_cur_kb;
	long long limit = cgroup_ll(cg, "memory.max", -1);

	if (limit < 0) {
		/* Not confined: the host's own numbers are the true ones. */
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

	file_kb = cgroup_stat_key(cg, "memory.stat", "file", 0) / 1024;
	inactive_kb = cgroup_stat_key(cg, "memory.stat", "inactive_file", 0) / 1024;
	if (file_kb < 0)
		file_kb = 0;
	if (inactive_kb < 0)
		inactive_kb = 0;

	/*
	 * Reclaimable page cache counts as available even though it is in
	 * use, which is what MemAvailable means and why it is not MemFree.
	 */
	avail_kb = total_kb - cur_kb + inactive_kb;
	if (avail_kb < 0)
		avail_kb = 0;
	if (avail_kb > total_kb)
		avail_kb = total_kb;

	swap_total_kb = cgroup_ll(cg, "memory.swap.max", -1);
	swap_total_kb = swap_total_kb < 0 ? 0 : swap_total_kb / 1024;
	swap_cur_kb = cgroup_ll(cg, "memory.swap.current", 0) / 1024;
	if (swap_cur_kb < 0)
		swap_cur_kb = 0;

	return (size_t)snprintf(out, cap,
	                         "MemTotal:       %8lld kB\n"
	                         "MemFree:        %8lld kB\n"
	                         "MemAvailable:   %8lld kB\n"
	                         "Buffers:        %8lld kB\n"
	                         "Cached:         %8lld kB\n"
	                         "SwapTotal:      %8lld kB\n"
	                         "SwapFree:       %8lld kB\n"
	                         "Shmem:          %8lld kB\n",
	                         total_kb, total_kb - cur_kb, avail_kb, 0LL, file_kb, swap_total_kb,
	                         swap_total_kb - swap_cur_kb,
	                         cgroup_stat_key(cg, "memory.stat", "shmem", 0) / 1024);
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
		if (len < cap)
			len += (size_t)snprintf(out + len, cap - len, "%s", line);
	}
	fclose(f);
	return len < cap ? len : cap;
}

/* Seconds since this cgroup's own directory was created, which is the
 * closest honest answer to "how long has this container been up". Falls
 * back to the host's uptime when unconfined. */
static size_t render_uptime(const char *cg, char *out, size_t cap)
{
	struct stat st;
	double up = 0.0, idle = 0.0;

	if (cg != NULL && stat(cg, &st) == 0) {
		time_t now = time(NULL);

		if (now > st.st_mtime)
			up = (double)(now - st.st_mtime);
	} else {
		FILE *f = fopen("/proc/uptime", "r");

		if (f != NULL) {
			if (fscanf(f, "%lf %lf", &up, &idle) != 2)
				up = 0.0;
			fclose(f);
		}
	}
	return (size_t)snprintf(out, cap, "%.2f %.2f\n", up, idle);
}

/* cpu.stat's usage_usec, expressed the way /proc/stat expresses it. The
 * fields nothing here can know are zero rather than invented. */
static size_t render_stat(const char *cg, char *out, size_t cap)
{
	long long usage_usec = cgroup_stat_key(cg, "cpu.stat", "usage_usec", 0);
	long long user_usec = cgroup_stat_key(cg, "cpu.stat", "user_usec", 0);
	long long system_usec = cgroup_stat_key(cg, "cpu.stat", "system_usec", 0);
	long ticks = sysconf(_SC_CLK_TCK);

	if (ticks <= 0)
		ticks = 100;
	if (usage_usec < 0)
		usage_usec = 0;
	if (user_usec < 0)
		user_usec = 0;
	if (system_usec < 0)
		system_usec = 0;

	return (size_t)snprintf(out, cap,
	                         "cpu  %lld 0 %lld 0 0 0 0 0 0 0\n"
	                         "intr 0\n"
	                         "ctxt 0\n"
	                         "btime 0\n"
	                         "processes 0\n"
	                         "procs_running 1\n"
	                         "procs_blocked 0\n",
	                         user_usec / (1000000 / ticks), system_usec / (1000000 / ticks));
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
 * One entry point so the opcode handler never grows a second copy of
 * the file table's ordering.
 */
static size_t render_file(int index, uint32_t pid, char *out, size_t cap)
{
	char cgbuf[PATH_MAX];
	const char *cg = NULL;

	if (cgroup_dir_for_pid(pid, cgbuf, sizeof(cgbuf)) == 0)
		cg = cgbuf;

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
	default:
		return 0;
	}
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
		if (strcmp(name, g_files[i]) != 0)
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
		size_t namelen = strlen(g_files[i]);
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
		memcpy(d->name, g_files[i], namelen);
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

	snprintf(opts, sizeof(opts), "fd=%d,rootmode=40000,user_id=0,group_id=0", fd);
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
