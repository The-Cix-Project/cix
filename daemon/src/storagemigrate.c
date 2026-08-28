#include "storagemigrate.h"
#include "disk.h"
#include "diskrole.h"
#include "linux_compat.h"
#include "persist.h"
#include "treecopy.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define STORAGE_KIND_COUNT 3

struct migrate_job {
	enum storagemigrate_state state;
	char source_dir[PATH_MAX];
	char target_dir[PATH_MAX];
	char target_disk[DISKROLE_DISK_NAME_MAX]; /* empty: migrated back to the default placement */
	char error[256];
	/*
	 * Issue #172: the copy runs in a forked child, so its reason for
	 * failing died with it -- two real migrations of a 15 GB image
	 * store failed with nothing recorded anywhere but the words "bulk
	 * copy failed". The child writes treecopy_last_error() here before
	 * exiting; the parent drains it at completion. Same shape as the
	 * artifact-export stderr capture that fixed the identical gap for
	 * image exports.
	 */
	int err_fd; /* read end, -1 when no job has run */
};

static struct migrate_job g_jobs[STORAGE_KIND_COUNT];

/* err_fd is 0 from static zero-init, which is a real fd (stdin) -- set
 * the sentinel explicitly the first time any job is touched rather than
 * relying on a value that happens to be wrong. */
static void jobs_init_once(void)
{
	static int done;
	int i;

	if (done)
		return;
	for (i = 0; i < STORAGE_KIND_COUNT; i++)
		g_jobs[i].err_fd = -1;
	done = 1;
}

static const char *role_str_for_kind(enum storage_kind kind)
{
	switch (kind) {
	case STORAGE_KIND_STATE:
		return "state-storage";
	case STORAGE_KIND_REBUILDABLE:
		return "rebuildable-storage";
	case STORAGE_KIND_LOG:
		return "log-storage";
	default:
		return "state-storage";
	}
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

enum storagemigrate_error storagemigrate_start(enum storage_kind kind, const char *source_dir,
                                                const char *target_dir, const char *target_disk_name,
                                                const char *os_containers_dir, pid_t *out_pid,
                                                int *out_pidfd)
{
	struct migrate_job *job;
	pid_t pid;
	int pidfd;

	jobs_init_once();
	if (kind < STORAGE_KIND_STATE || kind > STORAGE_KIND_LOG)
		return STORAGEMIGRATE_ERR_NOT_FOUND;
	job = &g_jobs[kind];
	if (job->state == STORAGEMIGRATE_STATE_RUNNING)
		return STORAGEMIGRATE_ERR_BUSY;

	if (target_disk_name != NULL) {
		struct discovered_disk d;
		const char *role;

		if (!find_disk(target_disk_name, os_containers_dir, &d))
			return STORAGEMIGRATE_ERR_NOT_FOUND;
		if (d.is_os_disk)
			return STORAGEMIGRATE_ERR_IS_OS_DISK;
		role = diskrole_lookup(target_disk_name);
		if (role == NULL || strcmp(role, role_str_for_kind(kind)) != 0)
			return STORAGEMIGRATE_ERR_WRONG_ROLE;
		if (!d.mounted)
			return STORAGEMIGRATE_ERR_NOT_MOUNTED;
	}

	if (strcmp(job->target_disk, target_disk_name != NULL ? target_disk_name : "") == 0 &&
	    job->state == STORAGEMIGRATE_STATE_READY)
		return STORAGEMIGRATE_ERR_ALREADY_ACTIVE;

	{
		int errpipe[2];

		if (pipe2(errpipe, O_CLOEXEC) != 0)
			return STORAGEMIGRATE_ERR_SPAWN_FAILED;

		pid = fork();
		if (pid < 0) {
			close(errpipe[0]);
			close(errpipe[1]);
			return STORAGEMIGRATE_ERR_SPAWN_FAILED;
		}
		if (pid == 0) {
			/* Deliberately does NOT execve() itself -- same reasoning
			 * as diskformat_start()'s own forked child: this is real C
			 * logic (treecopy_recursive()), not an external binary
			 * invocation. */
			int rc = treecopy_recursive(source_dir, target_dir);

			if (rc != 0) {
				const char *why = treecopy_last_error();

				/* Best effort: if this write fails the parent simply
				 * falls back to the generic message, which is what it
				 * always did. */
				(void)!write(errpipe[1], why, strlen(why));
			}
			_exit(rc == 0 ? 0 : 1);
		}
		close(errpipe[1]);
		if (job->err_fd >= 0)
			close(job->err_fd);
		job->err_fd = errpipe[0];
	}

	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return STORAGEMIGRATE_ERR_SPAWN_FAILED;
	}

	snprintf(job->source_dir, sizeof(job->source_dir), "%s", source_dir);
	snprintf(job->target_dir, sizeof(job->target_dir), "%s", target_dir);
	snprintf(job->target_disk, sizeof(job->target_disk), "%s",
	         target_disk_name != NULL ? target_disk_name : "");
	job->error[0] = '\0';
	job->state = STORAGEMIGRATE_STATE_RUNNING;
	*out_pid = pid;
	*out_pidfd = pidfd;
	return STORAGEMIGRATE_OK;
}

void storagemigrate_completed(enum storage_kind kind, int exit_status)
{
	struct migrate_job *job;

	jobs_init_once();
	if (kind < STORAGE_KIND_STATE || kind > STORAGE_KIND_LOG)
		return;
	job = &g_jobs[kind];

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
	} else
		snprintf(job->error, sizeof(job->error), "migration job process exited abnormally (status %d)",
		         exit_status);
}

int storagemigrate_finalize(enum storage_kind kind)
{
	struct migrate_job *job;

	if (kind < STORAGE_KIND_STATE || kind > STORAGE_KIND_LOG)
		return -1;
	job = &g_jobs[kind];

	if (treecopy_recursive(job->source_dir, job->target_dir) != 0) {
		job->state = STORAGEMIGRATE_STATE_FAILED;
		snprintf(job->error, sizeof(job->error), "final synchronous copy pass failed: %s",
		         strerror(errno));
		return -1;
	}
	return 0;
}

const char *storagemigrate_job_source_dir(enum storage_kind kind)
{
	if (kind < STORAGE_KIND_STATE || kind > STORAGE_KIND_LOG)
		return "";
	return g_jobs[kind].source_dir;
}

const char *storagemigrate_job_target_dir(enum storage_kind kind)
{
	if (kind < STORAGE_KIND_STATE || kind > STORAGE_KIND_LOG)
		return "";
	return g_jobs[kind].target_dir;
}

const char *storagemigrate_job_target_disk(enum storage_kind kind)
{
	if (kind < STORAGE_KIND_STATE || kind > STORAGE_KIND_LOG)
		return "";
	return g_jobs[kind].target_disk;
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

void storagemigrate_write_status_json(struct json_writer *w, enum storage_kind kind)
{
	const struct migrate_job *job;

	if (kind < STORAGE_KIND_STATE || kind > STORAGE_KIND_LOG || g_jobs[kind].state == STORAGEMIGRATE_STATE_NONE) {
		jw_obj_open(w);
		jw_key(w, "state");
		jw_str(w, "none");
		jw_obj_close(w);
		return;
	}
	job = &g_jobs[kind];

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
