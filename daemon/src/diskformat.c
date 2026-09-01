#include "diskformat.h"
#include "childdiag.h"
#include "disk.h"
#include "diskpart.h"
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

/*
 * mkfs's own stdout/stderr. Without this a failed format reported
 * "mkfs.btrfs failed" and nothing else -- the tool knew exactly what was
 * wrong and had nowhere to say it. That gap has now cost a debugging
 * cycle four separate times in this codebase (#125, #132, the
 * artifact-export stderr fix, and mkinstalleriso's), so it is closed
 * here the same way: capture the child's own words and put them in the
 * error an operator actually reads.
 *
 * Read once at completion rather than drained incrementally, unlike the
 * ISO assembly's: mkfs prints a few lines, not a progress stream, so a
 * 64K pipe cannot fill and block the child. Safe as a bare static --
 * diskformat_start() refuses a second job while one is in flight.
 */
#define DISKFORMAT_OUTPUT_MAX 1024
static int g_output_rd = -1;
static char g_output[DISKFORMAT_OUTPUT_MAX + 1];

enum diskformat_error diskformat_start(const char *disk_name, const char *os_containers_dir,
                                        const char *mount_base_dir, enum diskformat_fs_type fs_type,
                                        pid_t *out_pid, int *out_pidfd)
{
	struct discovered_disk d;
	pid_t pid;
	int pidfd;
	int output_pipe[2];

	if (!simple_name_is_valid(disk_name, DISKROLE_DISK_NAME_MAX))
		return DISKFORMAT_ERR_INVALID_DISK_NAME;
	if (g_state == DISKFORMAT_STATE_RUNNING)
		return DISKFORMAT_ERR_BUSY;
	if (!find_disk(disk_name, os_containers_dir, &d))
		return DISKFORMAT_ERR_NOT_FOUND;
	if (diskpart_os_layout_untouchable(d.name, d.is_os_disk, d.is_partition))
		return DISKFORMAT_ERR_IS_OS_DISK;
	if (diskrole_lookup(disk_name) == NULL)
		return DISKFORMAT_ERR_NO_ROLE;

	if (snprintf(g_mount_path, sizeof(g_mount_path), "%s/%s", mount_base_dir, disk_name) >=
	    (int)sizeof(g_mount_path))
		return DISKFORMAT_ERR_MKDIR_FAILED;
	if (mkdir(g_mount_path, 0755) != 0 && errno != EEXIST)
		return DISKFORMAT_ERR_MKDIR_FAILED;

	if (g_output_rd >= 0) {
		close(g_output_rd);
		g_output_rd = -1;
	}
	g_output[0] = '\0';
	if (pipe2(output_pipe, O_CLOEXEC) != 0)
		output_pipe[0] = output_pipe[1] = -1;

	pid = fork();
	if (pid < 0) {
		if (output_pipe[0] >= 0) {
			close(output_pipe[0]);
			close(output_pipe[1]);
		}
		return DISKFORMAT_ERR_SPAWN_FAILED;
	}
	if (pid == 0) {
		if (output_pipe[1] >= 0) {
			dup2(output_pipe[1], STDOUT_FILENO);
			dup2(output_pipe[1], STDERR_FILENO);
		}
		/*
		 * Deliberately does NOT execve() itself (unlike every other
		 * async job's forked child in this daemon) -- see diskformat.h's
		 * own doc comment for why: format+mount is two sequential
		 * steps and execve() can't be "returned from" to run the
		 * second one.
		 *
		 * BOTH branches force. The btrfs one used to omit it on the
		 * stated grounds that "mkfs.btrfs takes no -F flag of its own
		 * -- it always overwrites an existing signature without
		 * prompting when run non-interactively". That is wrong, and it
		 * meant btrfs formatting had never worked on a disk that
		 * already had a filesystem -- which is every real case.
		 *
		 * btrfs-progs uses lowercase -f, not -F: mkfs/main.c's own
		 * option table reads
		 *   OPTLINE("-f, --force", "force overwrite of existing filesystem")
		 * and test_dev_for_mkfs(file, force_overwrite) refuses outright
		 * when a filesystem is present and force is false. Confirmed
		 * live: formatting a real ext4 sda on 192.168.15.95 failed
		 * every time, and only said so once this function started
		 * capturing mkfs's own output.
		 */
		const char *mkfs_bin = fs_type == DISKFORMAT_FS_BTRFS ?
		                        DISKFORMAT_MKFS_BTRFS_BIN : DISKFORMAT_MKFS_EXT4_BIN;
		char *ext4_argv[] = { (char *)DISKFORMAT_MKFS_EXT4_BIN, "-F", (char *)d.dev_path, NULL };
		char *btrfs_argv[] = { (char *)DISKFORMAT_MKFS_BTRFS_BIN, "-f", (char *)d.dev_path,
		                        NULL };
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
		/*
		 * Propagate the mkfs child's OWN 127 rather than collapsing
		 * every failure into 1. diskformat_completed()'s 127 branch
		 * carries the one message that actually names the cause ("the
		 * control-plane image was built without it") -- flattening it
		 * here made that branch unreachable for exactly the case it
		 * was written for, so a host missing mkfs.btrfs reported the
		 * useless "mkfs.btrfs failed" instead. Confirmed live on
		 * 192.168.15.95: a real format attempt against a control-plane
		 * image built without btrfs-progs reported the generic text
		 * while the specific message sat right there, dead.
		 */
		if (waitpid(sub, &status, 0) != sub || !WIFEXITED(status))
			_exit(1);
		if (WEXITSTATUS(status) != 0)
			_exit(WEXITSTATUS(status) == 127 ? 127 : 1);

		/* A plain fork() (no CLONE_NEWNS) shares the parent's mount
		 * namespace -- this becomes visible daemon-wide immediately. */
		if (mount(d.dev_path, g_mount_path, fs_type_str(fs_type), MS_NOSUID | MS_NODEV, NULL) != 0)
			_exit(2);
		_exit(0);
	}

	if (output_pipe[1] >= 0)
		close(output_pipe[1]);
	g_output_rd = output_pipe[0];

	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		if (g_output_rd >= 0) {
			close(g_output_rd);
			g_output_rd = -1;
		}
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
		/* ADR-0142: this job's own state is purely in-memory, forgotten
		 * across a restart -- persist which real fstype succeeded
		 * alongside the role itself so a later boot's own auto-remount
		 * pass (main.c) knows what to mount with instead of guessing.
		 * Best-effort: a persist failure here doesn't undo the real,
		 * already-successful format+mount this job just completed, it
		 * only means a future restart won't auto-remount this specific
		 * disk (surfaced there, not here, as an unmounted disk needing
		 * a manual look, not a silent failure). */
		diskrole_set_fs_type(g_disk_name, fs_type_str(g_fs_type));
		return;
	}
	g_state = DISKFORMAT_STATE_FAILED;
	/*
	 * Whatever mkfs said before it gave up. Read here, after the child
	 * has exited, so this either returns what is still buffered or an
	 * immediate EOF -- it never blocks.
	 */
	if (g_output_rd >= 0) {
		ssize_t n;
		size_t total = 0;

		while (total + 1 < sizeof(g_output)) {
			n = read(g_output_rd, g_output + total, sizeof(g_output) - total - 1);
			if (n <= 0)
				break;
			total += (size_t)n;
		}
		g_output[total] = '\0';
		close(g_output_rd);
		g_output_rd = -1;
		while (total > 0 && (g_output[total - 1] == '\n' || g_output[total - 1] == '\r'))
			g_output[--total] = '\0';
		/*
		 * Keep the LAST line. mkfs prints a version banner and progress
		 * before it ever prints a complaint, so a message truncated from
		 * the front would faithfully report the banner and drop the
		 * reason -- which is worse than saying nothing, because it looks
		 * like an answer.
		 */
		/*
		 * Which line to report is a question this daemon answers in one
		 * place now (daemon/src/childdiag.c): mkfs.btrfs ends its
		 * failures with "See https://btrfs.readthedocs.io for more
		 * information." -- a footer, not a reason -- and reporting that
		 * instead of the ERROR line above it is how the first capture
		 * here still failed to say what was wrong. The ISO assembly hit
		 * the mirror image of the same bug, which is why the rule moved
		 * out of this file rather than being copied into that one.
		 */
		childdiag_reduce_to_error_line(g_output);
	}
	if (exit_status == 127)
		/*
		 * The child's own execve() failed, and by far the likeliest
		 * reason is that this binary is not on this host at all --
		 * which is exactly what happened to btrfs: the API accepted
		 * fs_type "btrfs" from the day multi-disk management shipped
		 * while mkbootroot never staged mkfs.btrfs, so every real
		 * attempt died here and reported only "exited abnormally
		 * (status 127)". Saying which binary is missing is the
		 * difference between a shrug and a fix.
		 */
		snprintf(g_error, sizeof(g_error),
		         "mkfs.%s could not be run -- it is not present on this host (the control-plane "
		         "image was built without it)",
		         fs_type_str(g_fs_type));
	else if (exit_status == 1)
		if (g_output[0] != '\0')
		snprintf(g_error, sizeof(g_error), "mkfs.%s failed: %s", fs_type_str(g_fs_type),
		         g_output);
	else
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

/* Single mkdir+mount(2) attempt, shared by both the remembered-fs_type
 * path and the probe path below. Returns 0 on success. */
static int try_mount_one(const struct discovered_disk *d, const char *fs_type, const char *mount_base_dir,
                          char *mount_path_out, size_t mount_path_out_size)
{
	if (snprintf(mount_path_out, mount_path_out_size, "%s/%s", mount_base_dir, d->name) >=
	    (int)mount_path_out_size)
		return -1;
	if (mkdir(mount_path_out, 0755) != 0 && errno != EEXIST)
		return -1;
	return mount(d->dev_path, mount_path_out, fs_type, MS_NOSUID | MS_NODEV, NULL);
}

void diskformat_remount_present_role_disks(const char *os_containers_dir, const char *mount_base_dir)
{
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n = disk_enumerate(disks, DISK_ENUM_MAX, os_containers_dir);
	int i;

	for (i = 0; i < n; i++) {
		const char *role;
		const char *fs_type;
		char mount_path[PATH_MAX];

		if (disks[i].is_os_disk || disks[i].mounted)
			continue;
		role = diskrole_lookup(disks[i].name);
		if (role == NULL)
			continue;
		fs_type = diskrole_lookup_fs_type(disks[i].name);

		if (fs_type != NULL) {
			if (try_mount_one(&disks[i], fs_type, mount_base_dir, mount_path, sizeof(mount_path)) !=
			    0) {
				fprintf(stderr,
				        "diskformat_remount_present_role_disks: %s: mount(2) failed: %s\n",
				        disks[i].name, strerror(errno));
				continue;
			}
			fprintf(stderr, "diskformat_remount_present_role_disks: remounted %s (%s) at %s\n",
			        disks[i].name, fs_type, mount_path);
			continue;
		}

		/*
		 * No remembered fs_type -- a disk role-assigned and formatted
		 * before diskrole_set_fs_type() existed (confirmed live: a
		 * real, already-deployed box had exactly this disk, formatted
		 * during the original Phase C work, well before this
		 * persistence mechanism shipped). Rather than leaving it
		 * permanently unmounted until an operator destructively
		 * re-formats it, probe the two filesystem types this project's
		 * own format mechanism has ever produced (fs_type_str()'s own
		 * complete range) -- mount(2) with a mismatched fstype string
		 * just fails cleanly (EINVAL, typically), the same safe,
		 * standard technique real mount tooling already relies on when
		 * a filesystem type isn't specified up front. Once one
		 * succeeds, the discovery is persisted so every future boot
		 * uses the direct, remembered path instead of probing again.
		 */
		{
			static const char *const probe_types[] = { "ext4", "btrfs" };
			size_t j;
			int mounted = 0;

			for (j = 0; j < sizeof(probe_types) / sizeof(probe_types[0]) && !mounted; j++) {
				if (try_mount_one(&disks[i], probe_types[j], mount_base_dir, mount_path,
				                   sizeof(mount_path)) == 0) {
					mounted = 1;
					fs_type = probe_types[j];
				}
			}
			if (!mounted) {
				fprintf(stderr,
				        "diskformat_remount_present_role_disks: %s: has a role but no "
				        "remembered fs_type, and probing ext4/btrfs both failed -- left "
				        "unmounted\n",
				        disks[i].name);
				continue;
			}
			diskrole_set_fs_type(disks[i].name, fs_type);
			fprintf(stderr,
			        "diskformat_remount_present_role_disks: remounted %s (%s, discovered by "
			        "probe) at %s\n",
			        disks[i].name, fs_type, mount_path);
		}
	}
}

enum diskformat_error diskformat_unmount(const char *disk_name, const char *os_containers_dir)
{
	struct discovered_disk d;

	if (!simple_name_is_valid(disk_name, DISKROLE_DISK_NAME_MAX))
		return DISKFORMAT_ERR_INVALID_DISK_NAME;
	if (!find_disk(disk_name, os_containers_dir, &d))
		return DISKFORMAT_ERR_NOT_FOUND;
	if (diskpart_os_layout_untouchable(d.name, d.is_os_disk, d.is_partition))
		return DISKFORMAT_ERR_IS_OS_DISK;
	if (!d.mounted)
		return DISKFORMAT_ERR_NOT_MOUNTED;

	if (umount2(d.mount_path, 0) != 0)
		return DISKFORMAT_ERR_UMOUNT_FAILED;
	return DISKFORMAT_OK;
}
