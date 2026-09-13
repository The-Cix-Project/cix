#ifndef PROCFUSE_H
#define PROCFUSE_H

#include <stddef.h>

/* struct procfuse_file: the name/bind-target pair PROCFUSE_FILES is made of. */
#include "container.h"

/*
 * ADR-0262, issue #336: one host-side FUSE server whose files the
 * runtime bind-mounts into every container, so a process inside one
 * reads its OWN cgroup's limits rather than the host's.
 *
 * The failure this exists to stop is not cosmetic. #278/#279 was a
 * build sizing itself by host memory against a 2 GiB cgroup ceiling it
 * could not see; it surfaced as an unexplained OOM against a machine
 * reporting plenty free, and the person looking at it could not see the
 * number that mattered.
 *
 * WHY ONE SERVER, NOT ONE PER CONTAINER. A FUSE request carries the
 * requester's pid in its header. The server resolves that pid to its
 * cgroup through /proc/<pid>/cgroup and reads the limits from there, so
 * one process answers every container correctly. The alternative --
 * a server inside each container reading its own cgroup -- is N
 * processes, N mounts and N supervision problems for a job one process
 * does, and it cannot mount itself into the container anyway: by the
 * time cix-init runs, the container's capabilities are gone.
 *
 * Measured before any of this was written (probe-cgroup-view/1, and the
 * daemon's own GET /system/pkg-build-config): a build container's
 * cpu.max is "100000 100000" -- exactly one CPU -- while nproc inside
 * it reports 2 on a 2-CPU host. nproc reads sched_getaffinity, which a
 * cpuset constrains and a quota does not. So cpuinfo here follows
 * cpuset.cpus.effective, which is a real parallelism bound, and NOT the
 * quota: a quota-derived cpuinfo would report 1, turning every
 * `make -j$(nproc)` on this platform into -j1 with nothing in the build
 * output saying why.
 *
 * TWO LIMITS, AND EACH FILE ANSWERS THE ONE ITS READERS ACT ON.
 * `cpuset.cpus.effective` bounds PARALLELISM -- how many CPUs may run
 * this container's tasks at once -- and `cpu.max` bounds THROUGHPUT --
 * how much CPU time it may consume per period. They are different
 * numbers and a container commonly has both.
 *
 * cpuinfo answers the parallelism question, for the reason just given.
 * /proc/stat's idle column and /proc/uptime's second field answer the
 * throughput one, so their denominator is the smaller of the two, and
 * getting that wrong was a real bug (#359): a container pinned to one
 * CPU with a half-CPU quota, running flat out against that cap, had a
 * whole CPU in its denominator and every tool reported 50% -- "half
 * idle" for a workload that could not go one cycle faster. That is
 * #278/#279's own shape, a number claiming headroom that does not
 * exist, which is the failure this file was written to remove.
 *
 * NOT A SECURITY BOUNDARY. A process can still reach the host's real
 * figures by other paths. This makes tools size themselves correctly;
 * it does not hide the host.
 */

/*
 * Every file this server synthesises, in one place, because three
 * things need the same list: the readdir reply, the lookup, and the
 * runtime's own bind mounts. A second copy of it is how a file gets
 * served and never mounted.
 *
 * Inode numbers are the index into this table plus PROCFUSE_INO_FIRST;
 * FUSE_ROOT_ID (1) is the directory itself.
 */
/*
 * Two namespaces, one table. The first six are /proc; the last three
 * are sysfs, and they are here because a container's CPU count has two
 * independent answers and fixing only one of them fixed nothing that
 * mattered.
 *
 * `sysconf(_SC_NPROCESSORS_ONLN)` -- how a great many libraries size a
 * thread pool -- reads /sys/devices/system/cpu/online, not /proc. A
 * container pinned to one CPU measured CONF=2 ONLN=2 NPROC=1 on a
 * two-CPU host: nproc was right (it uses sched_getaffinity, which a
 * cpuset constrains) and every ONLN-based sizing decision was wrong by
 * a factor of two, in the same "claims headroom it does not have"
 * direction as #278/#279.
 *
 * All three report the container's own cpuset verbatim, which is
 * already a kernel range list and is the same set /proc/stat's per-cpu
 * lines are filtered to -- so the two views cannot disagree.
 *
 * What this deliberately does NOT fix: `_SC_NPROCESSORS_CONF` counts
 * cpuN DIRECTORIES under /sys/devices/system/cpu, and no file bind can
 * remove a directory. htop takes its meter count from that, which is
 * why it draws a meter for a CPU the container cannot use and marks it
 * offline. Fixing that means serving a synthetic directory in place of
 * the real one, which is a different and much larger job -- see #363.
 */
#define PROCFUSE_FILES                                                         \
	{                                                                          \
		{ "meminfo", "/proc/meminfo" }, { "cpuinfo", "/proc/cpuinfo" },         \
		{ "stat", "/proc/stat" }, { "uptime", "/proc/uptime" },                 \
		{ "loadavg", "/proc/loadavg" }, { "swaps", "/proc/swaps" },             \
		{ "cpu_online", "/sys/devices/system/cpu/online" },                     \
		{ "cpu_present", "/sys/devices/system/cpu/present" },                   \
		{ "cpu_possible", "/sys/devices/system/cpu/possible" },                 \
		{ "partitions", "/proc/partitions" }                                    \
	}
#define PROCFUSE_FILE_COUNT 10
#define PROCFUSE_INO_FIRST 2

/*
 * The table index of the /proc/partitions entry (ADR-0286 tier 1). It
 * is the one file whose content is a function of the reader's mount
 * NAMESPACE rather than its cgroup, so render_file() routes it to a
 * different renderer. Named here next to the table, rather than a bare
 * 9 in render_file(), so that reordering the table is a visible edit at
 * this line rather than a silent renumber that mis-serves one file's
 * bytes as another's.
 */
#define PROCFUSE_INDEX_PARTITIONS 9

/*
 * Where the server mounts. Under STATE_DIR rather than /tmp or /run so
 * it survives on the same filesystem the rest of the daemon's state
 * lives on, and so a reader of the mount table can tell whose it is.
 */
#define PROCFUSE_MOUNT_DIRNAME "procfuse"

/*
 * Mounts the filesystem and forks the server that answers it.
 *
 * The mount happens in the PARENT, before the fork, so a failure is
 * reported synchronously with a real errno instead of vanishing into a
 * child nobody is reading yet -- "the daemon started and containers
 * quietly have host numbers" is precisely the silent degradation this
 * feature exists to remove.
 *
 * Best-effort by design, and that is the same posture start_uevent_watch()
 * already takes: a kernel without FUSE, a /dev/fuse that is absent, or a
 * mount that is refused leaves every other capability of this daemon
 * untouched and containers reading the host's real /proc, which is what
 * they read today. Returns 0 when the server is running, -1 otherwise
 * with errno set and the reason logged.
 */
int procfuse_start(const char *state_dir);

/*
 * The absolute path of the mounted directory, or NULL when the server is
 * not running. The runtime asks before bind-mounting: a container
 * created while the server is down gets the host's real files rather
 * than a mount over nothing, which is degraded rather than broken.
 */
const char *procfuse_mount_path(void);

/* Unmounts and reaps, for daemon shutdown. Safe when never started. */
void procfuse_stop(void);

/*
 * Renders one file of PROCFUSE_FILES (by index) as it would be served
 * to a process in cgroup directory `cg`, or as the host's own when `cg`
 * is NULL. Returns the number of bytes written.
 *
 * This is the seam the FUSE read path itself goes through, exposed so
 * the arithmetic can be tested against a crafted cgroup directory with
 * no FUSE, no root and no container -- a build container can run none
 * of those (#224), and the defect that made this necessary was
 * arithmetic: /proc/stat's idle column was zero, which makes every CPU
 * tool report 100% at any load (#359). A test that drives this drives
 * what really serves reads, rather than a second copy of the sums.
 */
size_t procfuse_render_for_cgroup(int index, const char *cg, char *out, size_t cap);

/*
 * Renders /proc/partitions (ADR-0286 tier 1) filtered to the block
 * devices that back the mounts described by `mountinfo_path`, reusing
 * the rows of `partitions_path` (the host's real /proc/partitions)
 * verbatim so the format is byte-for-byte the kernel's own and nothing
 * is synthesised. Split out with both paths as parameters for the same
 * reason as procfuse_render_for_cgroup: a test drives it against
 * crafted files with no FUSE, no root and no container. Returns the
 * number of bytes written.
 */
size_t procfuse_render_partitions_from(const char *mountinfo_path,
                                       const char *partitions_path, char *out,
                                       size_t cap);

#endif /* PROCFUSE_H */
