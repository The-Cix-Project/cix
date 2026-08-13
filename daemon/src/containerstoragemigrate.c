#include "containerstoragemigrate.h"
#include "containerdef.h"
#include "linux_compat.h"
#include "treecopy.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct container_migrate_job {
	char container_name[REGISTRY_NAME_MAX];
	enum storagemigrate_state state;
	char source_dir[PATH_MAX];
	char target_dir[PATH_MAX];
	char target_disk[DISKROLE_DISK_NAME_MAX];
	char error[256];
	int in_use;
};

static struct container_migrate_job g_jobs[CONTAINERDEF_MAX];

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

	job = find_job(container_name);
	if (job != NULL) {
		if (job->state == STORAGEMIGRATE_STATE_RUNNING)
			return CONTAINERSTORAGEMIGRATE_ERR_BUSY;
		if (job->state == STORAGEMIGRATE_STATE_READY &&
		    strcmp(job->target_disk, target_disk_name != NULL ? target_disk_name : "") == 0)
			return CONTAINERSTORAGEMIGRATE_ERR_ALREADY_ACTIVE;
	}

	pid = fork();
	if (pid < 0)
		return CONTAINERSTORAGEMIGRATE_ERR_SPAWN_FAILED;
	if (pid == 0) {
		/* Same reasoning as storagemigrate_start()'s own child: real C
		 * logic (treecopy_recursive()), not an external binary. */
		_exit(treecopy_recursive(source_dir, target_dir) == 0 ? 0 : 1);
	}

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
		job->state = STORAGEMIGRATE_STATE_READY;
		job->error[0] = '\0';
		return;
	}
	job->state = STORAGEMIGRATE_STATE_FAILED;
	if (exit_status == 1)
		snprintf(job->error, sizeof(job->error), "bulk copy failed");
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
