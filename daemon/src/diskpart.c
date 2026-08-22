#include "diskpart.h"
#include "disk.h"
#include "diskrole.h"
#include "namecheck.h"

#include <errno.h>
#include <fcntl.h>
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

/* Same shape as image/src/thinc-install.c's own run_subprocess_stdin()
 * (that copy drives the exact same sfdisk scripted-partition-table
 * mode, just for the fixed install-time layout rather than an
 * operator-chosen data disk after install) -- duplicated rather than
 * shared across the daemon/image build boundary, this project's own
 * established convention for small process helpers with no other
 * coupling (see disk.c's own read_sysfs_attr() comment). */
static int run_sfdisk_stdin(char *const argv[], const char *script)
{
	int pipefd[2];
	pid_t pid;
	int status;
	size_t len = strlen(script);
	size_t written = 0;
	ssize_t n;

	if (pipe2(pipefd, O_CLOEXEC) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[0], STDIN_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execve(DISKPART_SFDISK_BIN, argv, environ);
		_exit(127);
	}
	close(pipefd[0]);
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

/* Shared preconditions for both diskpart_create_table() and
 * diskpart_add(): disk_name must be a currently-present whole disk,
 * not the OS disk, carrying no role of its own, and not mounted -- the
 * exact set of states in which rewriting/growing its partition table
 * is safe. */
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

enum diskpart_error diskpart_add(const char *disk_name, const char *os_containers_dir,
                                  const char *part_name, unsigned long long size_mib)
{
	struct discovered_disk d;
	char script[128];
	char *argv[4];
	int rc;
	enum diskpart_error err;

	if (!simple_name_is_valid(part_name, DISKPART_NAME_MAX))
		return DISKPART_ERR_INVALID_PART_NAME;

	err = check_whole_disk_writable(disk_name, os_containers_dir, &d);
	if (err != DISKPART_OK)
		return err;

	if (size_mib == 0)
		snprintf(script, sizeof(script), "type=linux, name=\"%s\"\n", part_name);
	else
		snprintf(script, sizeof(script), "size=%lluMiB, type=linux, name=\"%s\"\n", size_mib,
		         part_name);

	argv[0] = (char *)DISKPART_SFDISK_BIN;
	argv[1] = "--append";
	argv[2] = d.dev_path;
	argv[3] = NULL;
	rc = run_sfdisk_stdin(argv, script);
	if (rc == -2)
		return DISKPART_ERR_SFDISK_MISSING;
	if (rc != 0)
		return DISKPART_ERR_SFDISK_FAILED;
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
	if (part.is_os_disk)
		return DISKPART_ERR_IS_OS_DISK;
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
	 * daemon that deals in sectors already assumes 512 (thinc-install.c
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
