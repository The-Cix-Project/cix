#include "diskpart.h"
#include "disk.h"
#include "diskrole.h"
#include "namecheck.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/*
 * Distinguishes "sfdisk never ran" from "sfdisk ran and refused".
 *
 * Both used to collapse into one bare "sfdisk failed", which is how a
 * missing binary stayed invisible for an entire release: the code was
 * correct and the error said nothing that pointed at /usr/sbin/sfdisk
 * not existing. On a host with no shell (ADR-0034) an error message is
 * the only diagnostic there is, so the two conditions have to be
 * distinguishable -- the same lesson Part 193 learned adding per-step
 * diagnostics to mountns_pivot().
 *
 * 127 is the exit code run_sfdisk_*()'s own child uses after a failed
 * execve(), matching the shell convention for "command not found".
 */
static int sfdisk_status_to_rc(int status)
{
	if (!WIFEXITED(status))
		return -1;
	if (WEXITSTATUS(status) == 0)
		return 0;
	if (WEXITSTATUS(status) == 127)
		return -2; /* execve() failed -- sfdisk is not installed */
	return -1;
}

/* Same shape as image/src/cix-install.c's own run_subprocess_stdin()
 * (that copy drives the exact same sfdisk scripted-partition-table
 * mode, just for the fixed install-time layout rather than an
 * operator-chosen data disk after install) -- duplicated rather than
 * shared across the daemon/image build boundary, this project's own
 * established convention for small process helpers with no other
 * coupling (see disk.c's own read_sysfs_attr() comment). */
/*
 * sfdisk's own last words, kept for the caller to report.
 *
 * Found the hard way: appending a partition to a live OS disk was
 * refused, and the only thing reaching an operator was the daemon's
 * own generic guess ("the disk may already have the partition table or
 * layout being asked for, or there is not enough free space"). sfdisk
 * had said something specific on stderr and it went nowhere -- the
 * same class as #125/#132/#172, where a real failure's only
 * explanation is unreadable. A guess presented as a diagnosis is worse
 * than no diagnosis, because it sends the reader somewhere wrong.
 */
static char g_sfdisk_last_error[512];

const char *diskpart_last_tool_error(void)
{
	return g_sfdisk_last_error;
}

/* Drains what the child wrote, bounded, and trims it to one line's
 * worth of the most useful part -- sfdisk is chatty on success and
 * terse on failure, so the tail is what matters. */
static void capture_tool_stderr(int fd)
{
	char buf[sizeof(g_sfdisk_last_error)];
	size_t total = 0;
	ssize_t n;

	g_sfdisk_last_error[0] = '\0';
	while (total < sizeof(buf) - 1) {
		n = read(fd, buf + total, sizeof(buf) - 1 - total);
		if (n <= 0)
			break;
		total += (size_t)n;
	}
	buf[total] = '\0';
	/* Collapse newlines so the whole thing survives a single-line
	 * error field and a single log entry. */
	{
		size_t i;

		for (i = 0; i < total; i++) {
			if (buf[i] == '\n' || buf[i] == '\r')
				buf[i] = ' ';
		}
	}
	snprintf(g_sfdisk_last_error, sizeof(g_sfdisk_last_error), "%s", buf);
}

static int run_sfdisk_stdin(char *const argv[], const char *script)
{
	int pipefd[2];
	int errpipe[2];
	pid_t pid;
	int status;
	size_t len = strlen(script);
	size_t written = 0;
	ssize_t n;

	g_sfdisk_last_error[0] = '\0';
	if (pipe2(pipefd, O_CLOEXEC) != 0)
		return -1;
	if (pipe2(errpipe, O_CLOEXEC) != 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		close(errpipe[0]);
		close(errpipe[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[0], STDIN_FILENO);
		/* Both streams: sfdisk puts some of its complaint on stdout. */
		dup2(errpipe[1], STDOUT_FILENO);
		dup2(errpipe[1], STDERR_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		close(errpipe[0]);
		close(errpipe[1]);
		execve(DISKPART_SFDISK_BIN, argv, environ);
		_exit(127);
	}
	close(pipefd[0]);
	close(errpipe[1]);
	while (written < len) {
		n = write(pipefd[1], script + written, len - written);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		written += (size_t)n;
	}
	close(pipefd[1]);
	/* Drained BEFORE waitpid: sfdisk can fill the pipe and block on
	 * write while we wait for it to exit, which would deadlock. */
	capture_tool_stderr(errpipe[0]);
	close(errpipe[0]);
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return sfdisk_status_to_rc(status);
}

/* sfdisk --delete takes no stdin script at all -- a plain argv
 * invocation, same fork/execve/waitpid shape minus the pipe. */
static int run_sfdisk_argv(char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execve(DISKPART_SFDISK_BIN, argv, environ);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return sfdisk_status_to_rc(status);
}

/* Finds disk_name (or partition_name) in a fresh disk_enumerate() pass.
 * NULL if not currently present. */
static int find_disk(const char *name, const char *os_containers_dir, struct discovered_disk *out)
{
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n = disk_enumerate(disks, DISK_ENUM_MAX, os_containers_dir);
	int i;

	for (i = 0; i < n; i++) {
		if (strcmp(disks[i].name, name) == 0) {
			*out = disks[i];
			return 1;
		}
	}
	return 0;
}

/*
 * Issue #140: which partitions on the OS disk are structurally
 * untouchable.
 *
 * The installer lays the OS disk out in a fixed order (see
 * image/src/cix-install.c): 1 cix-esp, 2 cix-root-a, 3 cix-root-b,
 * 4 cix-config, then 5 cix-containers, then whatever the operator left
 * unallocated. The first four carry the ESP, both A/B root slots and
 * /config -- destroy any of them and the machine does not boot, with
 * no remote recovery.
 *
 * Everything AFTER them is ordinary space. The previous blanket
 * `is_os_disk` refusal made no such distinction, so an operator who
 * had deliberately sized cix-containers smaller than the disk -- in
 * order to keep the remainder allocatable -- could not use the space
 * they had reserved for themselves. That is the whole bug.
 *
 * Keyed on the partition NUMBER rather than the label, deliberately.
 * The number is what sfdisk operates on and what the kernel exposes;
 * a label is cosmetic, can be edited by any tool that writes the
 * table, and a protection that a rename can lift is not a protection.
 */
/*
 * The trailing partition number: "vda5" -> 5, "nvme0n1p5" -> 5.
 * Returns 0 when there is no numeric suffix, which callers treat as
 * "cannot tell" and therefore as protected.
 */
int diskpart_partition_number(const char *name)
{
	size_t len = strlen(name);
	size_t i = len;

	while (i > 0 && name[i - 1] >= '0' && name[i - 1] <= '9')
		i--;
	if (i == len)
		return 0;
	return atoi(name + i);
}

/*
 * True when this partition must never be offered for delete or resize.
 *
 * Fails CLOSED: a partition on the OS disk whose number cannot be
 * determined is treated as protected. The cost of being wrong in that
 * direction is an operator having to do something by hand; the cost of
 * being wrong in the other direction is an unbootable machine.
 */
int diskpart_partition_protected(const char *name, int is_os_disk)
{
	int n;

	if (!is_os_disk)
		return 0;
	n = diskpart_partition_number(name);
	if (n <= 0)
		return 1;
	return n <= DISKPART_OS_PROTECTED_PARTITIONS;
}

/*
 * True when this device must never be given a role, formatted or
 * unmounted (issue #9).
 *
 * The same question as diskpart_partition_protected() above, asked of a
 * whole disk as well as a partition, and it exists because asking it
 * three different ways is what left the reserved space unusable.
 *
 * #140 made appending a partition to the OS disk's free space legal --
 * that was its whole purpose, so that an operator who deliberately
 * sized cix-containers smaller than the disk could use the remainder.
 * But role assignment, format and unmount each kept their own blanket
 * `is_os_disk` refusal, and a partition inherits is_os_disk from its
 * parent. So the appended partition could be created and then never
 * given a role, never formatted, and never mounted: reserved space that
 * could be carved and not used, which is the same bug #140 fixed,
 * surviving one layer down.
 *
 * The OS disk AS A WHOLE stays refused -- formatting it is destroying
 * the machine -- and so do the structural partitions, by exactly the
 * rule delete and resize already use. Nothing else about those
 * operations relaxes: a format still demands a role, an unmounted
 * target and a matching confirm_disk_name.
 */
int diskpart_os_layout_untouchable(const char *name, int is_os_disk, int is_partition)
{
	if (!is_os_disk)
		return 0;
	if (!is_partition)
		return 1; /* the OS disk itself */
	return diskpart_partition_protected(name, is_os_disk);
}

/*
 * Preconditions for APPENDING a partition (diskpart_add).
 *
 * Issue #140: deliberately weaker than the create-table check below,
 * because the two operations are not comparably dangerous.
 *
 * `sfdisk --append` adds an entry and does not rewrite the existing
 * ones. That is the whole safety argument, and it is why this is
 * allowed on the OS disk while rewriting its table stays refused: the
 * ESP, both root slots and /config are not read, moved or modified by
 * appending after them.
 *
 * Mounted partitions are likewise no obstacle to appending -- the OS
 * disk always has some (root, /config, containers), so refusing on
 * that basis is what made the reserved free space unusable. A mounted
 * WHOLE-disk device is still refused: that is a disk being used
 * un-partitioned, where a table is not a table yet.
 */
static enum diskpart_error check_disk_appendable(const char *disk_name,
                                                   const char *os_containers_dir,
                                                   struct discovered_disk *out)
{
	if (!simple_name_is_valid(disk_name, DISKROLE_DISK_NAME_MAX))
		return DISKPART_ERR_INVALID_DISK_NAME;
	if (!find_disk(disk_name, os_containers_dir, out))
		return DISKPART_ERR_NOT_FOUND;
	if (out->is_partition)
		return DISKPART_ERR_IS_PARTITION;
	if (diskrole_lookup(disk_name) != NULL)
		return DISKPART_ERR_HAS_ROLE;
	if (out->mounted)
		return DISKPART_ERR_MOUNTED;
	return DISKPART_OK;
}

/* Preconditions for REWRITING a disk's partition table
 * (diskpart_create_table): disk_name must be a currently-present whole
 * disk, not the OS disk, carrying no role of its own, and not mounted
 * -- the exact set of states in which rewriting its partition table is
 * safe.
 *
 * The OS-disk refusal here is permanent and deliberate (issue #140):
 * a table rewrite is precisely the operation that can destroy the ESP
 * and both root slots, and no amount of free space at the end makes
 * that safe. Appending is a different operation and has its own,
 * weaker check above. */
static enum diskpart_error check_whole_disk_writable(const char *disk_name,
                                                       const char *os_containers_dir,
                                                       struct discovered_disk *out)
{
	if (!simple_name_is_valid(disk_name, DISKROLE_DISK_NAME_MAX))
		return DISKPART_ERR_INVALID_DISK_NAME;
	if (!find_disk(disk_name, os_containers_dir, out))
		return DISKPART_ERR_NOT_FOUND;
	if (out->is_partition)
		return DISKPART_ERR_IS_PARTITION;
	if (out->is_os_disk)
		return DISKPART_ERR_IS_OS_DISK;
	if (diskrole_lookup(disk_name) != NULL)
		return DISKPART_ERR_HAS_ROLE;
	/* Anything mounted on this disk -- the whole-disk device itself, or
	 * any partition on it -- makes rewriting or growing its partition
	 * table unsafe. The second half used to be carried by `mounted`
	 * itself and was silently lost when that became per-device. */
	if (out->mounted || out->has_mounted_partition)
		return DISKPART_ERR_MOUNTED;
	return DISKPART_OK;
}

enum diskpart_error diskpart_create_table(const char *disk_name, const char *os_containers_dir)
{
	struct discovered_disk d;
	char *argv[3];
	int rc;
	enum diskpart_error err = check_whole_disk_writable(disk_name, os_containers_dir, &d);

	if (err != DISKPART_OK)
		return err;

	argv[0] = (char *)DISKPART_SFDISK_BIN;
	argv[1] = d.dev_path;
	argv[2] = NULL;
	rc = run_sfdisk_stdin(argv, "label: gpt\n");
	if (rc == -2)
		return DISKPART_ERR_SFDISK_MISSING;
	if (rc != 0)
		return DISKPART_ERR_SFDISK_FAILED;
	return DISKPART_OK;
}

/*
 * Issue #140: telling the kernel about ONE newly-appended partition.
 *
 * sfdisk refuses to touch a disk carrying any mounted filesystem --
 * a pre-flight BLKRRPART check, confirmed on 192.168.15.95's own OS
 * disk. That check is right for a repartition and wrong for an append,
 * which by construction does not read, move or modify existing
 * entries. So the append runs with --no-reread (suppress that one
 * check) rather than --force (overrule every check): the difference
 * matters, because every other guard sfdisk performs is still wanted.
 *
 * --no-tell-kernel goes with it so sfdisk never touches the kernel's
 * view at all. That is not caution for its own sake -- it makes the
 * behaviour identical on a busy disk and an idle one, instead of
 * "sometimes the kernel already knows, sometimes it does not". The
 * kernel is then told about exactly the new partition, and nothing
 * else, via BLKPG_ADD_PARTITION.
 *
 * BLKPG adds a single partition to the kernel's view without the
 * whole-disk re-read that EBUSYs while anything is mounted. It is the
 * mechanism partx(8) itself uses, and the reason adding a partition to
 * a running system is an ordinary operation rather than a reboot.
 *
 * The structures are declared here rather than taken from
 * <linux/blkpg.h>: this project has been bitten once by trusting a
 * system header's layout under TCC (struct epoll_event, ADR-0008), and
 * these are three fields and two fixed buffers.
 */
static int run_sfdisk_capture(char *const argv[], char *out, size_t out_size, size_t *out_len);

#define CIX_BLKPG _IO(0x12, 105)
#define CIX_BLKPG_ADD_PARTITION 1

struct cix_blkpg_partition {
	long long start;  /* BYTES, not sectors -- the kernel's own unit here */
	long long length; /* bytes */
	int pno;
	char devname[64];
	char volname[64];
};

struct cix_blkpg_ioctl_arg {
	int op;
	int flags;
	int datalen;
	void *data;
};

/*
 * Reads back the partition sfdisk just wrote.
 *
 * Deliberately re-reads the table rather than trusting the size that
 * was requested: sfdisk aligns a partition to the disk's own
 * granularity, so the start and length it actually chose are the only
 * correct ones to hand the kernel. Passing the requested size would
 * register a partition whose bounds disagree with the table on disk --
 * a discrepancy that would not surface until something wrote near the
 * end of it.
 */
static int read_appended_partition(const char *dev_path, int *out_pno,
                                    unsigned long long *out_start_sector,
                                    unsigned long long *out_sectors)
{
	char buf[16384];
	char *argv[4];
	char *line;
	char *saveptr = NULL;
	int best = -1;
	int rc;

	argv[0] = (char *)DISKPART_SFDISK_BIN;
	argv[1] = "-d";
	argv[2] = (char *)dev_path;
	argv[3] = NULL;
	rc = run_sfdisk_capture(argv, buf, sizeof(buf), NULL);
	if (rc != 0)
		return -1;

	for (line = strtok_r(buf, "\n", &saveptr); line != NULL;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		const char *colon = strchr(line, ':');
		const char *sp, *sz;
		unsigned long long start, sectors;
		int pno;
		size_t dlen = strlen(dev_path);

		if (colon == NULL || strncmp(line, dev_path, dlen) != 0)
			continue;
		pno = atoi(line + dlen);
		if (pno <= 0)
			continue;
		sp = strstr(colon, "start=");
		sz = strstr(colon, "size=");
		if (sp == NULL || sz == NULL)
			continue;
		start = strtoull(sp + 6, NULL, 10);
		sectors = strtoull(sz + 5, NULL, 10);
		if (sectors == 0)
			continue;
		/* The appended one is the highest-numbered entry: sfdisk
		 * --append always allocates the next free slot. */
		if (pno > best) {
			best = pno;
			*out_pno = pno;
			*out_start_sector = start;
			*out_sectors = sectors;
		}
	}
	return best > 0 ? 0 : -1;
}

#define CIX_BLKPG_DEL_PARTITION 2

/*
 * Issue #140: tell the kernel to forget a partition we just removed
 * from the table.
 *
 * Found by deleting the test partition appended to a live OS disk:
 * `sfdisk --delete` updated the table correctly -- free space returned
 * to exactly its previous extent -- and the kernel went on exposing
 * /dev/vda6 for a region now marked free. The API returned 204, so it
 * reported complete success for a half-done job.
 *
 * That stale node is worse than untidy. The space it maps is free, so
 * the very next append hands the same sectors to a new partition while
 * the old node still points at them: two device nodes, one region, and
 * whichever is written first loses. Removing the entry from the kernel
 * is part of deleting the partition, not a cosmetic follow-up.
 *
 * ENXIO/EINVAL mean the kernel does not have this partition, which is
 * the desired end state -- treated as success for the same reason
 * EBUSY is on the add path.
 */
static int blkpg_del_partition(const char *dev_path, int pno)
{
	struct cix_blkpg_partition part;
	struct cix_blkpg_ioctl_arg arg;
	int fd;
	int rc;

	memset(&part, 0, sizeof(part));
	memset(&arg, 0, sizeof(arg));
	part.pno = pno;

	arg.op = CIX_BLKPG_DEL_PARTITION;
	arg.flags = 0;
	arg.datalen = (int)sizeof(part);
	arg.data = &part;

	fd = open(dev_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	rc = ioctl(fd, CIX_BLKPG, &arg);
	if (rc != 0 && (errno == ENXIO || errno == EINVAL))
		rc = 0;
	close(fd);
	return rc == 0 ? 0 : -1;
}

/* Returns 0, or -1 with errno set. EBUSY here means the kernel already
 * knows about this partition number, which is success for our purposes
 * rather than a failure. */
static int blkpg_add_partition(const char *dev_path, int pno, unsigned long long start_sector,
                                unsigned long long sectors)
{
	struct cix_blkpg_partition part;
	struct cix_blkpg_ioctl_arg arg;
	int fd;
	int rc;

	memset(&part, 0, sizeof(part));
	memset(&arg, 0, sizeof(arg));
	part.start = (long long)start_sector * 512;
	part.length = (long long)sectors * 512;
	part.pno = pno;

	arg.op = CIX_BLKPG_ADD_PARTITION;
	arg.flags = 0;
	arg.datalen = (int)sizeof(part);
	arg.data = &part;

	fd = open(dev_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	rc = ioctl(fd, CIX_BLKPG, &arg);
	if (rc != 0 && errno == EBUSY)
		rc = 0; /* already registered -- the desired end state */
	close(fd);
	return rc == 0 ? 0 : -1;
}

enum diskpart_error diskpart_add(const char *disk_name, const char *os_containers_dir,
                                  const char *part_name, unsigned long long size_mib)
{
	struct discovered_disk d;
	char script[128];
	char *argv[6];
	int rc;
	enum diskpart_error err;

	if (!simple_name_is_valid(part_name, DISKPART_NAME_MAX))
		return DISKPART_ERR_INVALID_PART_NAME;

	err = check_disk_appendable(disk_name, os_containers_dir, &d);
	if (err != DISKPART_OK)
		return err;

	if (size_mib == 0)
		snprintf(script, sizeof(script), "type=linux, name=\"%s\"\n", part_name);
	else
		snprintf(script, sizeof(script), "size=%lluMiB, type=linux, name=\"%s\"\n", size_mib,
		         part_name);

	argv[0] = (char *)DISKPART_SFDISK_BIN;
	argv[1] = "--append";
	/*
	 * Issue #140. --no-reread suppresses sfdisk's pre-flight
	 * "is anyone using this disk" check, which refuses any disk with a
	 * mounted filesystem. That check is correct for a repartition and
	 * wrong for an append: appending does not read, move or modify an
	 * existing entry, so a mounted partition elsewhere on the disk is
	 * not endangered by it. Deliberately NOT --force, which would
	 * overrule every other check sfdisk makes as well.
	 *
	 * --no-tell-kernel keeps sfdisk away from the kernel's view
	 * entirely, so behaviour does not differ between a busy disk and
	 * an idle one; the kernel is told about exactly the new partition
	 * below.
	 */
	argv[2] = "--no-reread";
	argv[3] = "--no-tell-kernel";
	argv[4] = d.dev_path;
	argv[5] = NULL;
	rc = run_sfdisk_stdin(argv, script);
	if (rc == -2)
		return DISKPART_ERR_SFDISK_MISSING;
	if (rc != 0)
		return DISKPART_ERR_SFDISK_FAILED;

	/*
	 * The table on disk is correct at this point; the kernel does not
	 * know yet. A partition that exists in the table with no device
	 * node is the worst of both worlds -- it consumes the space and
	 * nothing can use it -- so a failure here is reported rather than
	 * swallowed, even though the write itself succeeded.
	 */
	{
		int pno = 0;
		unsigned long long start_sector = 0, sectors = 0;

		if (read_appended_partition(d.dev_path, &pno, &start_sector, &sectors) != 0) {
			snprintf(g_sfdisk_last_error, sizeof(g_sfdisk_last_error),
			         "the partition was written to the table, but reading it back to tell the "
			         "kernel about it failed -- it will appear after a reboot");
			return DISKPART_ERR_SFDISK_FAILED;
		}
		if (blkpg_add_partition(d.dev_path, pno, start_sector, sectors) != 0) {
			snprintf(g_sfdisk_last_error, sizeof(g_sfdisk_last_error),
			         "partition %d was written to the table, but the kernel refused to add it "
			         "(%s) -- it will appear after a reboot",
			         pno, strerror(errno));
			return DISKPART_ERR_SFDISK_FAILED;
		}
	}
	return DISKPART_OK;
}

enum diskpart_error diskpart_delete(const char *disk_name, const char *partition_name,
                                     const char *os_containers_dir)
{
	struct discovered_disk parent, part;
	char partno_str[16];
	char *argv[5];
	const char *partno;
	int rc;

	if (!simple_name_is_valid(disk_name, DISKROLE_DISK_NAME_MAX))
		return DISKPART_ERR_INVALID_DISK_NAME;
	if (!simple_name_is_valid(partition_name, sizeof(part.name)))
		return DISKPART_ERR_INVALID_PART_NAME;

	if (!find_disk(partition_name, os_containers_dir, &part))
		return DISKPART_ERR_NOT_FOUND;
	if (!part.is_partition)
		return DISKPART_ERR_NOT_A_PARTITION;
	if (strcmp(part.parent_disk, disk_name) != 0)
		return DISKPART_ERR_WRONG_PARENT;
	/* Issue #140: only the OS disk's first four are untouchable, not
	 * the whole disk -- see partition_is_protected(). */
	if (diskpart_partition_protected(part.name, part.is_os_disk))
		return DISKPART_ERR_PROTECTED_PARTITION;
	if (diskrole_lookup(partition_name) != NULL)
		return DISKPART_ERR_HAS_ROLE;
	if (part.mounted)
		return DISKPART_ERR_MOUNTED;

	if (!find_disk(disk_name, os_containers_dir, &parent))
		return DISKPART_ERR_NOT_FOUND; /* parent vanished between the two lookups */

	/* partition_name is disk_name's own name with a numeric suffix
	 * (plus, for an nvme-style parent, a "p" separator) -- exactly
	 * what disk_name_from_partition() strips off in reverse; here the
	 * suffix itself (the partition number sfdisk --delete wants) is
	 * what's needed, so it's read directly rather than via that
	 * helper. */
	partno = partition_name + strlen(disk_name);
	if (partno[0] == 'p' && partno[1] >= '0' && partno[1] <= '9')
		partno++;
	if (partno[0] < '0' || partno[0] > '9')
		return DISKPART_ERR_WRONG_PARENT; /* shouldn't happen given the checks above */
	snprintf(partno_str, sizeof(partno_str), "%s", partno);

	argv[0] = (char *)DISKPART_SFDISK_BIN;
	argv[1] = "--delete";
	argv[2] = parent.dev_path;
	argv[3] = partno_str;
	argv[4] = NULL;
	rc = run_sfdisk_argv(argv);
	if (rc == -2)
		return DISKPART_ERR_SFDISK_MISSING;
	if (rc != 0)
		return DISKPART_ERR_SFDISK_FAILED;

	/*
	 * The table no longer has this partition; the kernel still does
	 * until told. Leaving that gap open means the next append can
	 * allocate the same sectors while the old device node still maps
	 * them -- see blkpg_del_partition()'s own comment. Reported rather
	 * than swallowed: a 204 for a half-done delete is what made this
	 * invisible in the first place.
	 */
	if (blkpg_del_partition(parent.dev_path, atoi(partno_str)) != 0) {
		snprintf(g_sfdisk_last_error, sizeof(g_sfdisk_last_error),
		         "partition %s was removed from the table, but the kernel still has its device "
		         "node (%s) -- do not create another partition on this disk until it has been "
		         "rebooted",
		         partno_str, strerror(errno));
		return DISKPART_ERR_SFDISK_FAILED;
	}
	return DISKPART_OK;
}

/*
 * Reads sfdisk's own free-space report for a disk (issue #95).
 *
 * Adding a partition means choosing a size, and until now nothing could
 * say how much room there was: a client could subtract the reported
 * partition sizes from the disk size, but that is arithmetic over the
 * reported set rather than the truth -- it ignores alignment, the GPT's
 * own reserved areas at both ends, and any gap left behind by an
 * earlier delete. sfdisk already knows all three exactly.
 *
 * Deliberately NOT folded into disk_enumerate(): that runs on every
 * poll, for every disk, and this forks a subprocess. It is answered on
 * demand instead, which is also when it is actually wanted -- while
 * filling in a size.
 *
 * Output shape (util-linux 2.38):
 *
 *   Unpartitioned space /dev/sda: 822.98 MiB, 862961152 bytes, 1685471 sectors
 *   Units: sectors of 1 * 512 = 512 bytes
 *   Sector size (logical/physical): 512 bytes / 512 bytes
 *
 *    Start     End Sectors  Size
 *     2048  206847  204800  100M
 *   616448 2097118 1480671  723M
 *
 * A disk with a table but no room reports "0 B, 0 bytes, 0 sectors" and
 * no rows. A disk with NO partition table at all prints nothing at all
 * and still exits 0 -- which is why has_table is reported separately
 * rather than inferred from a zero total: those are different states
 * and only one of them is fixable by deleting something.
 */
static int run_sfdisk_capture(char *const argv[], char *out, size_t out_size, size_t *out_len)
{
	int pipefd[2];
	pid_t pid;
	int status;
	size_t total = 0;

	if (pipe2(pipefd, O_CLOEXEC) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execve(DISKPART_SFDISK_BIN, argv, environ);
		_exit(127);
	}
	close(pipefd[1]);
	while (total + 1 < out_size) {
		ssize_t n = read(pipefd[0], out + total, out_size - total - 1);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		total += (size_t)n;
	}
	close(pipefd[0]);
	out[total] = '\0';
	if (out_len != NULL)
		*out_len = total;
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return sfdisk_status_to_rc(status);
}

/*
 * The parsing half, against a device path directly. Split out so a test
 * can drive it against a real disk image -- sfdisk operates identically
 * on a plain file, which is how this project already verified the
 * partition-script syntax, and this sandbox has no block device nodes
 * to use instead.
 */
enum diskpart_error diskpart_free_space_from_path(const char *dev_path,
                                                   struct diskpart_free_space *out)
{
	char buf[8192];
	char *argv[4];
	char *line;
	char *saveptr = NULL;
	int in_table = 0;
	int rc;

	memset(out, 0, sizeof(*out));

	argv[0] = (char *)DISKPART_SFDISK_BIN;
	argv[1] = "--list-free";
	argv[2] = (char *)dev_path;
	argv[3] = NULL;
	rc = run_sfdisk_capture(argv, buf, sizeof(buf), NULL);
	if (rc == -2)
		return DISKPART_ERR_SFDISK_MISSING;
	if (rc != 0)
		return DISKPART_ERR_SFDISK_FAILED;

	for (line = strtok_r(buf, "\n", &saveptr); line != NULL;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		unsigned long long start, end, sectors;

		if (strncmp(line, "Unpartitioned space", 19) == 0) {
			const char *comma = strchr(line, ',');

			out->has_table = 1;
			/* "...: 822.98 MiB, 862961152 bytes, 1685471 sectors" --
			 * the exact byte count is the second field; the first is a
			 * rounded human string deliberately not parsed. */
			if (comma != NULL)
				out->total_free_bytes = strtoull(comma + 1, NULL, 10);
			continue;
		}
		if (strstr(line, "Start") != NULL && strstr(line, "Sectors") != NULL) {
			in_table = 1;
			continue;
		}
		if (!in_table)
			continue;
		if (sscanf(line, " %llu %llu %llu", &start, &end, &sectors) != 3)
			continue;
		if (out->extent_count < DISKPART_FREE_EXTENT_MAX) {
			out->extents[out->extent_count].start_sector = start;
			out->extents[out->extent_count].sectors = sectors;
			out->extent_count++;
		}
		if (sectors > out->largest_free_sectors)
			out->largest_free_sectors = sectors;
	}
	/*
	 * Sector size is not parsed out of the header: every path in this
	 * daemon that deals in sectors already assumes 512 (cix-install.c
	 * and the sfdisk scripts above both do), and inventing a second,
	 * differently-sourced answer here would be the disagreement rather
	 * than the fix. If a 4Kn disk ever needs supporting, it needs
	 * supporting everywhere at once.
	 */
	out->sector_bytes = 512;
	return DISKPART_OK;
}

enum diskpart_error diskpart_free_space(const char *disk_name, const char *os_containers_dir,
                                         struct diskpart_free_space *out)
{
	struct discovered_disk d;

	memset(out, 0, sizeof(*out));

	if (!simple_name_is_valid(disk_name, DISKROLE_DISK_NAME_MAX))
		return DISKPART_ERR_INVALID_DISK_NAME;
	if (!find_disk(disk_name, os_containers_dir, &d))
		return DISKPART_ERR_NOT_FOUND;
	/* Free space is a property of a partition TABLE, so a partition
	 * has no answer to give -- the question belongs to its parent. */
	if (d.is_partition)
		return DISKPART_ERR_IS_PARTITION;

	return diskpart_free_space_from_path(d.dev_path, out);
}

/*
 * Runs a filesystem tool (e2fsck/resize2fs) on a device, discarding its
 * output. Same fork/execve/waitpid shape as run_sfdisk_argv() above but
 * against a different binary, so the exit-status mapping is shared and
 * a missing tool is still distinguishable from a tool that ran and
 * refused -- the distinction issue #9 had to learn the hard way.
 */
/*
 * Runs a filesystem tool and KEEPS what it said.
 *
 * This used to send the child's stdout and stderr to /dev/null, so
 * every distinct failure -- a missing shared library, a filesystem the
 * tool refused, a bad argument -- reached the operator as the same bare
 * "the filesystem could not be grown". That is exactly the
 * guess-as-diagnosis failure g_sfdisk_last_error was introduced for on
 * the sfdisk path, and it cost a real debugging round on the btrfs path
 * before this changed.
 *
 * Note this cannot go through run_sfdisk_capture(): that one hardcodes
 * execve(DISKPART_SFDISK_BIN, ...) and ignores argv[0] entirely, so
 * calling it for any other tool silently runs sfdisk with the other
 * tool's arguments.
 */
static int run_tool_argv(const char *bin, char *const argv[])
{
	int pipefd[2];
	pid_t pid;
	int status;

	if (pipe2(pipefd, O_CLOEXEC) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execve(bin, argv, environ);
		_exit(127);
	}
	close(pipefd[1]);
	capture_tool_stderr(pipefd[0]);
	close(pipefd[0]);
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	if (!WIFEXITED(status))
		return -1;
	if (WEXITSTATUS(status) == 127)
		return -2;
	return WEXITSTATUS(status);
}

/*
 * Grows one partition, and the filesystem inside it.
 *
 * GROW ONLY, deliberately. Shrinking is not the mirror image of
 * growing: the filesystem has to shrink FIRST, and if the table entry
 * is cut before the filesystem is, the tail of a live filesystem is
 * simply gone. Refusing is not a limitation to apologise for -- it is
 * the difference between an operation that cannot lose data and one
 * that can, and "make it bigger" is what people actually want anyway.
 *
 * The two halves are both required. Growing the table entry alone
 * leaves the extra space invisible to everything using the filesystem,
 * which looks like the resize silently did nothing.
 *
 * The filesystem is grown for ext4 and for btrfs, or skipped entirely
 * for an unformatted partition where there is nothing to grow.
 *
 * btrfs really is a different flow rather than another binary name
 * (#163): `btrfs filesystem resize` operates on a MOUNTED filesystem,
 * the exact opposite of resize2fs, while the table rewrite above needs
 * it unmounted. So it arrives unmounted, the table grows, and only then
 * is it mounted at a private scratch point for the resize and unmounted
 * again -- leaving it exactly as it was found. This used to be refused
 * outright, which meant that on a platform whose own storage substrate
 * is btrfs (ADR-0207), the one filesystem that matters most could not
 * be grown at all.
 */
enum diskpart_error diskpart_resize(const char *disk_name, const char *partition_name,
                                     const char *os_containers_dir, unsigned long long size_mib)
{
	struct discovered_disk parent, part;
	struct diskpart_free_space fs;
	unsigned long long current_bytes, want_bytes, room_bytes = 0;
	unsigned long long part_end_sector;
	char script[64];
	char partno_str[16];
	char *argv[6];
	const char *partno;
	int i, rc;

	if (!simple_name_is_valid(disk_name, DISKROLE_DISK_NAME_MAX))
		return DISKPART_ERR_INVALID_DISK_NAME;
	if (!simple_name_is_valid(partition_name, sizeof(part.name)))
		return DISKPART_ERR_INVALID_PART_NAME;
	if (!find_disk(partition_name, os_containers_dir, &part))
		return DISKPART_ERR_NOT_FOUND;
	if (!part.is_partition)
		return DISKPART_ERR_NOT_A_PARTITION;
	if (strcmp(part.parent_disk, disk_name) != 0)
		return DISKPART_ERR_WRONG_PARENT;
	/* Issue #140: only the OS disk's first four are untouchable. */
	if (diskpart_partition_protected(part.name, part.is_os_disk))
		return DISKPART_ERR_PROTECTED_PARTITION;
	/*
	 * Unmounted only. resize2fs can grow a mounted ext4 online, but
	 * sfdisk rewriting the table underneath a live filesystem is a
	 * different risk entirely, and the two have to happen in order.
	 */
	if (part.mounted)
		return DISKPART_ERR_MOUNTED;
	if (!find_disk(disk_name, os_containers_dir, &parent))
		return DISKPART_ERR_NOT_FOUND;

	/* Only filesystems this can actually finish the job for (#163). */
	if (part.fs_type[0] != '\0' && strcmp(part.fs_type, "ext4") != 0 &&
	    strcmp(part.fs_type, "btrfs") != 0)
		return DISKPART_ERR_FS_UNSUPPORTED;

	/*
	 * How much room is genuinely available: free space is only usable
	 * if it starts exactly where this partition ends. Free space
	 * elsewhere on the disk cannot extend this partition, and reporting
	 * a total would promise room that does not apply here.
	 */
	if (diskpart_free_space_from_path(parent.dev_path, &fs) != DISKPART_OK)
		return DISKPART_ERR_SFDISK_FAILED;
	current_bytes = part.size_bytes;
	part_end_sector = 0;
	if (fs.sector_bytes > 0)
		part_end_sector = part.start_sector + (current_bytes / fs.sector_bytes);
	for (i = 0; i < fs.extent_count; i++) {
		if (fs.extents[i].start_sector == part_end_sector) {
			room_bytes = fs.extents[i].sectors * fs.sector_bytes;
			break;
		}
	}

	want_bytes = size_mib * 1024ULL * 1024ULL;
	if (size_mib == 0)
		want_bytes = current_bytes + room_bytes; /* "everything after it" */
	if (want_bytes <= current_bytes)
		return DISKPART_ERR_SHRINK_REFUSED;
	if (want_bytes > current_bytes + room_bytes)
		return DISKPART_ERR_NO_ROOM_AFTER;

	/* Step 1: the table entry. */
	partno = partition_name + strlen(disk_name);
	if (partno[0] == 'p' && partno[1] >= '0' && partno[1] <= '9')
		partno++;
	if (partno[0] < '0' || partno[0] > '9')
		return DISKPART_ERR_WRONG_PARENT;
	snprintf(partno_str, sizeof(partno_str), "%s", partno);
	snprintf(script, sizeof(script), "size=%lluMiB\n", want_bytes / (1024ULL * 1024ULL));

	argv[0] = (char *)DISKPART_SFDISK_BIN;
	argv[1] = "-N";
	argv[2] = partno_str;
	argv[3] = "--force";
	argv[4] = parent.dev_path;
	argv[5] = NULL;
	rc = run_sfdisk_stdin(argv, script);
	if (rc == -2)
		return DISKPART_ERR_SFDISK_MISSING;
	if (rc != 0)
		return DISKPART_ERR_SFDISK_FAILED;

	/* Step 2: the filesystem, if there is one. An unformatted
	 * partition is finished -- there is nothing inside it to grow. */
	if (part.fs_type[0] == '\0')
		return DISKPART_OK;

	/*
	 * Before growing anything: has the KERNEL actually noticed?
	 *
	 * sfdisk rewrote the table on disk, and then asks the kernel to
	 * re-read it -- which fails with EBUSY while anything else on the
	 * disk is mounted. If that happened, the kernel still reports the
	 * old size, and both resize2fs and `btrfs filesystem resize max`
	 * would then grow the filesystem to the OLD bound and exit 0. The
	 * operator would be told the resize succeeded, and would have
	 * gained nothing.
	 *
	 * That is the exact silent-success failure this two-step path
	 * exists to prevent, so it is checked rather than assumed (#163).
	 * The table on disk is already correct at this point, so a reboot
	 * or a later retry finishes the job -- which is what the error
	 * says.
	 */
	{
		struct discovered_disk after;

		if (!find_disk(partition_name, os_containers_dir, &after))
			return DISKPART_ERR_NOT_FOUND;
		if (after.size_bytes <= current_bytes)
			return DISKPART_ERR_KERNEL_SIZE_STALE;
	}

	/*
	 * btrfs grows MOUNTED, which is the opposite of resize2fs and the
	 * reason this is its own flow rather than another binary name.
	 *
	 * The partition arrives here unmounted (required, so the table
	 * could be rewritten safely) and leaves unmounted: it is mounted at
	 * a private scratch point purely for the duration of the resize.
	 *
	 * The scratch point goes under os_containers_dir, which is a real
	 * directory this daemon owns and writes to constantly. The obvious
	 * choice, /run, is wrong here and the failure was instructive:
	 * CLAUDE.md records that this project's minimal container images
	 * have /run and no /tmp, and that is a fact about IMAGES. The host's
	 * own control-plane root is a squashfs with no /run at all, so the
	 * mkdir failed with ENOENT and could not have been fixed by
	 * creating it either. Found in one shot only because this path had
	 * just been taught to report the tool's real error instead of a
	 * generic one.
	 */
	if (strcmp(part.fs_type, "btrfs") == 0) {
		char mnt[PATH_MAX];
		int mrc;

		if (snprintf(mnt, sizeof(mnt), "%s/.resize-%s", os_containers_dir, part.name) >=
		    (int)sizeof(mnt))
			return DISKPART_ERR_RESIZE_FS_FAILED;
		if (mkdir(mnt, 0700) != 0 && errno != EEXIST) {
			snprintf(g_sfdisk_last_error, sizeof(g_sfdisk_last_error),
			         "could not create the scratch mount point %s: %s", mnt, strerror(errno));
			return DISKPART_ERR_RESIZE_FS_FAILED;
		}
		if (mount(part.dev_path, mnt, "btrfs", 0, NULL) != 0) {
			snprintf(g_sfdisk_last_error, sizeof(g_sfdisk_last_error),
			         "could not mount %s at %s to grow it: %s", part.dev_path, mnt,
			         strerror(errno));
			rmdir(mnt);
			return DISKPART_ERR_RESIZE_FS_FAILED;
		}

		argv[0] = (char *)DISKPART_BTRFS_BIN;
		argv[1] = "filesystem";
		argv[2] = "resize";
		argv[3] = "max";
		argv[4] = mnt;
		argv[5] = NULL;
		rc = run_tool_argv(DISKPART_BTRFS_BIN, argv);

		/* Unmounted on every outcome, including failure: leaving a
		 * scratch mount behind would make the partition look busy to
		 * every later operation, including the retry. */
		mrc = umount(mnt);
		rmdir(mnt);

		if (rc == -2) {
			snprintf(g_sfdisk_last_error, sizeof(g_sfdisk_last_error),
			         "%s is not present on this host", DISKPART_BTRFS_BIN);
			return DISKPART_ERR_SFDISK_MISSING;
		}
		if (rc != 0)
			return DISKPART_ERR_RESIZE_FS_FAILED; /* capture already holds the reason */
		if (mrc != 0) {
			snprintf(g_sfdisk_last_error, sizeof(g_sfdisk_last_error),
			         "the filesystem was grown but the scratch mount at %s could not be "
			         "released: %s",
			         mnt, strerror(errno));
			return DISKPART_ERR_RESIZE_FS_FAILED;
		}
		return DISKPART_OK;
	}

	/*
	 * resize2fs requires a clean filesystem. -p ("preen") makes only
	 * the automatic, unambiguous repairs and refuses anything needing
	 * a human, which is exactly the line worth drawing for something
	 * running unattended: exit >= 4 means it found something it will
	 * not decide on alone, and that is reported rather than pressed
	 * through.
	 */
	argv[0] = (char *)DISKPART_E2FSCK_BIN;
	argv[1] = "-f";
	argv[2] = "-p";
	argv[3] = part.dev_path;
	argv[4] = NULL;
	rc = run_tool_argv(DISKPART_E2FSCK_BIN, argv);
	if (rc == -2)
		return DISKPART_ERR_SFDISK_MISSING;
	if (rc < 0 || rc >= 4)
		return DISKPART_ERR_FS_UNCLEAN;

	argv[0] = (char *)DISKPART_RESIZE2FS_BIN;
	argv[1] = part.dev_path;
	argv[2] = NULL;
	rc = run_tool_argv(DISKPART_RESIZE2FS_BIN, argv);
	if (rc == -2)
		return DISKPART_ERR_SFDISK_MISSING;
	if (rc != 0)
		return DISKPART_ERR_RESIZE_FS_FAILED;

	return DISKPART_OK;
}
