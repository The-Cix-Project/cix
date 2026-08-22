#include "diskpart.h"
#include "disk.h"
#include "diskrole.h"
#include "namecheck.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
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
	if (out->mounted)
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
