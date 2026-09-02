#include "containerstoragemigrate.h"
#include "btrfs.h"
#include "containerdef.h"
#include "linux_compat.h"
#include "persist.h"
#include "treecopy.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct container_migrate_job {
	char container_name[REGISTRY_NAME_MAX];
	enum storagemigrate_state state;
	char source_dir[PATH_MAX];
	char target_dir[PATH_MAX];
	char target_disk[DISKROLE_DISK_NAME_MAX];
	char error[256];
	/*
	 * Read end of the child's error pipe, -1 when no job has run.
	 *
	 * Without it this reported the words "bulk copy failed" and
	 * nothing else -- no file, no errno -- which is exactly the
	 * complaint in #172 against the sibling storagemigrate.c. That one
	 * was fixed; this copy of the same code was not, so the fix
	 * covered one of two migration paths and the other kept its
	 * silence.
	 */
	int err_fd;
	int in_use;
};

static struct container_migrate_job g_jobs[CONTAINERDEF_MAX];

/*
 * err_fd is 0 from static zero-init, and 0 is stdin -- a real fd. Every
 * slot is set to -1 once, before anything can read it, so a "no job has
 * run" slot can never be mistaken for one holding a pipe. The sibling
 * file carries the identical note for the identical reason.
 */
static void jobs_init_once(void)
{
	static int done;
	int i;

	if (done)
		return;
	for (i = 0; i < CONTAINERDEF_MAX; i++)
		g_jobs[i].err_fd = -1;
	done = 1;
}

static struct container_migrate_job *find_job(const char *container_name)
{
	int i;

	for (i = 0; i < CONTAINERDEF_MAX; i++) {
		if (g_jobs[i].in_use && strcmp(g_jobs[i].container_name, container_name) == 0)
			return &g_jobs[i];
	}
	return NULL;
}

static struct container_migrate_job *find_or_alloc_job(const char *container_name)
{
	struct container_migrate_job *job = find_job(container_name);
	int i;

	if (job != NULL)
		return job;
	for (i = 0; i < CONTAINERDEF_MAX; i++) {
		if (!g_jobs[i].in_use) {
			memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
			snprintf(g_jobs[i].container_name, sizeof(g_jobs[i].container_name), "%s",
			         container_name);
			g_jobs[i].in_use = 1;
			return &g_jobs[i];
		}
	}
	return NULL;
}

enum containerstoragemigrate_error containerstoragemigrate_start(const char *container_name,
                                                                   const char *source_dir,
                                                                   const char *target_dir,
                                                                   const char *target_disk_name,
                                                                   pid_t *out_pid, int *out_pidfd)
{
	struct container_migrate_job *job;
	pid_t pid;
	int pidfd;
	int errpipe[2];

	job = find_job(container_name);
	if (job != NULL) {
		if (job->state == STORAGEMIGRATE_STATE_RUNNING)
			return CONTAINERSTORAGEMIGRATE_ERR_BUSY;
		if (job->state == STORAGEMIGRATE_STATE_READY &&
		    strcmp(job->target_disk, target_disk_name != NULL ? target_disk_name : "") == 0)
			return CONTAINERSTORAGEMIGRATE_ERR_ALREADY_ACTIVE;
	}

	jobs_init_once();

	if (pipe2(errpipe, O_CLOEXEC) != 0)
		return CONTAINERSTORAGEMIGRATE_ERR_SPAWN_FAILED;

	pid = fork();
	if (pid < 0) {
		close(errpipe[0]);
		close(errpipe[1]);
		return CONTAINERSTORAGEMIGRATE_ERR_SPAWN_FAILED;
	}
	if (pid == 0) {
		/* Same reasoning as storagemigrate_start()'s own child: real C
		 * logic, not an external binary. */
		char src_rootfs[PATH_MAX];
		char dst_rootfs[PATH_MAX];
		struct stat rst;
		int rc;

		/*
		 * A snapshot container (ADR-0207) keeps its whole state in one
		 * btrfs subvolume, <dir>/rootfs, and nothing else lives beside
		 * it -- unlike the overlay layout's upper/ and work/.
		 *
		 * treecopy_recursive() would reproduce it as an ordinary
		 * directory: every file present, and no longer a subvolume. It
		 * would look like it worked, and the container would break at
		 * the first thing that needed subvolume semantics -- another
		 * snapshot, or a qgroup disk quota. That is why this case was
		 * refused outright rather than copied, and cix_btrfs_subvol_
		 * copy() is what makes it safe to stop refusing: the target is
		 * created as a real subvolume before the data lands in it.
		 */
		snprintf(src_rootfs, sizeof(src_rootfs), "%s/rootfs", source_dir);
		snprintf(dst_rootfs, sizeof(dst_rootfs), "%s/rootfs", target_dir);
		if (stat(src_rootfs, &rst) == 0 && S_ISDIR(rst.st_mode)) {
			rc = persist_mkdir_p(target_dir);
			if (rc == 0)
				rc = cix_btrfs_subvol_copy(src_rootfs, dst_rootfs);
		} else {
			rc = treecopy_recursive(source_dir, target_dir);
		}

		if (rc != 0) {
			const char *why = treecopy_last_error();

			/* Best effort: if this write fails the parent simply
			 * reports the failure without a reason, which is what
			 * it did unconditionally before. */
			(void)!write(errpipe[1], why, strlen(why));
		}
		_exit(rc == 0 ? 0 : 1);
	}
	close(errpipe[1]);

	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return CONTAINERSTORAGEMIGRATE_ERR_SPAWN_FAILED;
	}

	job = find_or_alloc_job(container_name);
	if (job == NULL) {
		/* CONTAINERDEF_MAX distinct containers have each already run at
		 * least one migration this daemon lifetime -- vanishingly
		 * unlikely (that's every possible container slot), but the
		 * child is already spawned by this point, so let it finish
		 * harmlessly rather than SIGKILL-ing mid-copy; its result is
		 * simply never recorded. */
		return CONTAINERSTORAGEMIGRATE_ERR_TABLE_FULL;
	}

	snprintf(job->source_dir, sizeof(job->source_dir), "%s", source_dir);
	snprintf(job->target_dir, sizeof(job->target_dir), "%s", target_dir);
	snprintf(job->target_disk, sizeof(job->target_disk), "%s",
	         target_disk_name != NULL ? target_disk_name : "");
	job->error[0] = '\0';
	job->state = STORAGEMIGRATE_STATE_RUNNING;
	*out_pid = pid;
	*out_pidfd = pidfd;
	return CONTAINERSTORAGEMIGRATE_OK;
}

void containerstoragemigrate_completed(const char *container_name, int exit_status)
{
	struct container_migrate_job *job = find_job(container_name);

	if (job == NULL)
		return;

	if (exit_status == 0) {
		if (job->err_fd >= 0) {
			close(job->err_fd);
			job->err_fd = -1;
		}
		job->state = STORAGEMIGRATE_STATE_READY;
		job->error[0] = '\0';
		return;
	}
	job->state = STORAGEMIGRATE_STATE_FAILED;
	if (exit_status == 1) {
		char why[256];
		ssize_t n = -1;

		if (job->err_fd >= 0) {
			n = read(job->err_fd, why, sizeof(why) - 1);
			close(job->err_fd);
			job->err_fd = -1;
		}
		if (n > 0) {
			why[n] = '\0';
			snprintf(job->error, sizeof(job->error), "bulk copy failed -- %s", why);
		} else {
			snprintf(job->error, sizeof(job->error),
			         "bulk copy failed (no reason reported by the copy child)");
		}
	}
	else
		snprintf(job->error, sizeof(job->error), "migration job process exited abnormally (status %d)",
		         exit_status);
}

int containerstoragemigrate_finalize(const char *container_name)
{
	struct container_migrate_job *job = find_job(container_name);

	if (job == NULL)
		return -1;

	if (treecopy_recursive(job->source_dir, job->target_dir) != 0) {
		job->state = STORAGEMIGRATE_STATE_FAILED;
		snprintf(job->error, sizeof(job->error), "final synchronous copy pass failed: %s",
		         strerror(errno));
		return -1;
	}
	return 0;
}

const char *containerstoragemigrate_job_source_dir(const char *container_name)
{
	struct container_migrate_job *job = find_job(container_name);

	return job != NULL ? job->source_dir : "";
}

const char *containerstoragemigrate_job_target_dir(const char *container_name)
{
	struct container_migrate_job *job = find_job(container_name);

	return job != NULL ? job->target_dir : "";
}

const char *containerstoragemigrate_job_target_disk(const char *container_name)
{
	struct container_migrate_job *job = find_job(container_name);

	return job != NULL ? job->target_disk : "";
}

void containerstoragemigrate_mark_failed(const char *container_name, const char *message)
{
	struct container_migrate_job *job = find_job(container_name);

	if (job == NULL)
		return;
	job->state = STORAGEMIGRATE_STATE_FAILED;
	snprintf(job->error, sizeof(job->error), "%s", message);
}

static const char *state_str(enum storagemigrate_state s)
{
	switch (s) {
	case STORAGEMIGRATE_STATE_RUNNING:
		return "running";
	case STORAGEMIGRATE_STATE_READY:
		return "ready";
	case STORAGEMIGRATE_STATE_FAILED:
		return "failed";
	case STORAGEMIGRATE_STATE_NONE:
	default:
		return "none";
	}
}

void containerstoragemigrate_write_status_json(struct json_writer *w, const char *container_name)
{
	struct container_migrate_job *job = find_job(container_name);

	if (job == NULL) {
		jw_obj_open(w);
		jw_key(w, "state");
		jw_str(w, "none");
		jw_obj_close(w);
		return;
	}

	jw_obj_open(w);
	jw_key(w, "state");
	jw_str(w, state_str(job->state));
	jw_key(w, "disk");
	if (job->target_disk[0] != '\0')
		jw_str(w, job->target_disk);
	else
		jw_null(w);
	if (job->state == STORAGEMIGRATE_STATE_FAILED) {
		jw_key(w, "error");
		jw_str(w, job->error);
	}
	jw_obj_close(w);
}
