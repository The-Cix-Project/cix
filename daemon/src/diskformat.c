#include "diskformat.h"
#include "disk.h"
#include "diskrole.h"
#include "linux_compat.h"
#include "namecheck.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* The one in-flight/most-recent job this daemon lifetime -- same v1
 * single-job constraint every other async host job in this daemon
 * already has (see diskformat.h). */
static enum diskformat_state g_state = DISKFORMAT_STATE_NONE;
static char g_disk_name[DISKROLE_DISK_NAME_MAX];
static char g_mount_path[PATH_MAX];
static char g_error[256];
static enum diskformat_fs_type g_fs_type;

static const char *fs_type_str(enum diskformat_fs_type t)
{
	return t == DISKFORMAT_FS_BTRFS ? "btrfs" : "ext4";
}

static int find_disk(const char *disk_name, const char *os_containers_dir, struct discovered_disk *out)
{
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n = disk_enumerate(disks, DISK_ENUM_MAX, os_containers_dir);
	int i;

	for (i = 0; i < n; i++) {
		if (strcmp(disks[i].name, disk_name) == 0) {
			*out = disks[i];
			return 1;
		}
	}
	return 0;
}

enum diskformat_error diskformat_start(const char *disk_name, const char *os_containers_dir,
                                        const char *mount_base_dir, enum diskformat_fs_type fs_type,
                                        pid_t *out_pid, int *out_pidfd)
{
	struct discovered_disk d;
	pid_t pid;
	int pidfd;

	if (!simple_name_is_valid(disk_name, DISKROLE_DISK_NAME_MAX))
		return DISKFORMAT_ERR_INVALID_DISK_NAME;
	if (g_state == DISKFORMAT_STATE_RUNNING)
		return DISKFORMAT_ERR_BUSY;
	if (!find_disk(disk_name, os_containers_dir, &d))
		return DISKFORMAT_ERR_NOT_FOUND;
	if (d.is_os_disk)
		return DISKFORMAT_ERR_IS_OS_DISK;
	if (diskrole_lookup(disk_name) == NULL)
		return DISKFORMAT_ERR_NO_ROLE;

	if (snprintf(g_mount_path, sizeof(g_mount_path), "%s/%s", mount_base_dir, disk_name) >=
	    (int)sizeof(g_mount_path))
		return DISKFORMAT_ERR_MKDIR_FAILED;
	if (mkdir(g_mount_path, 0755) != 0 && errno != EEXIST)
		return DISKFORMAT_ERR_MKDIR_FAILED;

	pid = fork();
	if (pid < 0)
		return DISKFORMAT_ERR_SPAWN_FAILED;
	if (pid == 0) {
		/*
		 * Deliberately does NOT execve() itself (unlike every other
		 * async job's forked child in this daemon) -- see diskformat.h's
		 * own doc comment for why: format+mount is two sequential
		 * steps and execve() can't be "returned from" to run the
		 * second one.
		 *
		 * mkfs.btrfs takes no "-F" (force) flag of its own -- it
		 * always overwrites an existing signature without prompting
		 * when run non-interactively (confirmed via a real local
		 * build+run of mkfs.btrfs --version/-h: no equivalent flag
		 * exists, and btrfs-progs' own docs describe this as the
		 * default non-tty behavior), so the mkfs.ext4 branch's own
		 * "-F" is simply omitted rather than passed as a no-op.
		 */
		const char *mkfs_bin = fs_type == DISKFORMAT_FS_BTRFS ?
		                        DISKFORMAT_MKFS_BTRFS_BIN : DISKFORMAT_MKFS_EXT4_BIN;
		char *ext4_argv[] = { (char *)DISKFORMAT_MKFS_EXT4_BIN, "-F", (char *)d.dev_path, NULL };
		char *btrfs_argv[] = { (char *)DISKFORMAT_MKFS_BTRFS_BIN, (char *)d.dev_path, NULL };
		char **argv = fs_type == DISKFORMAT_FS_BTRFS ? btrfs_argv : ext4_argv;
		pid_t sub;
		int status;

		sub = fork();
		if (sub < 0)
			_exit(1);
		if (sub == 0) {
			execve(mkfs_bin, argv, environ);
			_exit(127);
		}
		if (waitpid(sub, &status, 0) != sub || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
			_exit(1);

		/* A plain fork() (no CLONE_NEWNS) shares the parent's mount
		 * namespace -- this becomes visible daemon-wide immediately. */
		if (mount(d.dev_path, g_mount_path, fs_type_str(fs_type), MS_NOSUID | MS_NODEV, NULL) != 0)
			_exit(2);
		_exit(0);
	}

	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return DISKFORMAT_ERR_SPAWN_FAILED;
	}

	snprintf(g_disk_name, sizeof(g_disk_name), "%s", disk_name);
	g_error[0] = '\0';
	g_fs_type = fs_type;
	g_state = DISKFORMAT_STATE_RUNNING;
	*out_pid = pid;
	*out_pidfd = pidfd;
	return DISKFORMAT_OK;
}

void diskformat_completed(int exit_status)
{
	if (exit_status == 0) {
		g_state = DISKFORMAT_STATE_READY;
		g_error[0] = '\0';
		return;
	}
	g_state = DISKFORMAT_STATE_FAILED;
	if (exit_status == 1)
		snprintf(g_error, sizeof(g_error), "mkfs.%s failed", fs_type_str(g_fs_type));
	else if (exit_status == 2)
		snprintf(g_error, sizeof(g_error), "mkfs.%s succeeded but mount(2) failed: %s",
		         fs_type_str(g_fs_type), strerror(errno));
	else
		snprintf(g_error, sizeof(g_error), "format job process exited abnormally (status %d)",
		         exit_status);
}

static const char *state_str(enum diskformat_state s)
{
	switch (s) {
	case DISKFORMAT_STATE_RUNNING:
		return "running";
	case DISKFORMAT_STATE_READY:
		return "ready";
	case DISKFORMAT_STATE_FAILED:
		return "failed";
	case DISKFORMAT_STATE_NONE:
	default:
		return "none";
	}
}

void diskformat_write_status_json(struct json_writer *w, const char *want_disk_name)
{
	if (g_state == DISKFORMAT_STATE_NONE ||
	    (want_disk_name != NULL && strcmp(want_disk_name, g_disk_name) != 0)) {
		jw_obj_open(w);
		jw_key(w, "state");
		jw_str(w, "none");
		jw_obj_close(w);
		return;
	}

	jw_obj_open(w);
	jw_key(w, "disk_name");
	jw_str(w, g_disk_name);
	jw_key(w, "state");
	jw_str(w, state_str(g_state));
	if (g_state == DISKFORMAT_STATE_RUNNING || g_state == DISKFORMAT_STATE_READY) {
		jw_key(w, "fs_type");
		jw_str(w, fs_type_str(g_fs_type));
		jw_key(w, "mount_path");
		jw_str(w, g_mount_path);
	}
	if (g_state == DISKFORMAT_STATE_FAILED) {
		jw_key(w, "error");
		jw_str(w, g_error);
	}
	jw_obj_close(w);
}
