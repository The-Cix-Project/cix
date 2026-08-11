#include "hostproc.h"
#include "registry.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * /proc/<pid>/stat's own comm+ppid fields -- comm is inside parens and
 * can itself contain spaces/parens/anything (the kernel truncates it
 * to 16 bytes but places no other restriction), so the standard,
 * correct parse is: find the FIRST '(' and the LAST ')' (the kernel
 * guarantees these bound the real comm value even if comm itself
 * contains ')'), then walk past exactly one more field (the single-
 * character state) to reach ppid.
 */
static int read_proc_stat(pid_t pid, char *comm_out, size_t comm_size, pid_t *ppid_out)
{
	char path[64];
	char buf[512];
	FILE *fp;
	char *open_paren, *close_paren, *p;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	fp = fopen(path, "r");
	if (fp == NULL)
		return -1;
	if (fgets(buf, sizeof(buf), fp) == NULL) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	open_paren = strchr(buf, '(');
	close_paren = strrchr(buf, ')');
	if (open_paren == NULL || close_paren == NULL || close_paren <= open_paren)
		return -1;

	if (comm_out != NULL) {
		size_t len = (size_t)(close_paren - open_paren - 1);

		if (len >= comm_size)
			len = comm_size - 1;
		memcpy(comm_out, open_paren + 1, len);
		comm_out[len] = '\0';
	}

	p = close_paren + 1;
	while (*p == ' ')
		p++;
	p += 1; /* skip the single-character state field */
	if (ppid_out != NULL)
		*ppid_out = (pid_t)strtol(p, NULL, 10);
	return 0;
}

/* /proc/<pid>/status's "Uid:"/"Gid:" lines -- four values each (real,
 * effective, saved, filesystem); the first (real) is what "who owns
 * this process" means here, matching ps(1)'s own default RUID column. */
static void read_proc_ids(pid_t pid, uid_t *uid_out, gid_t *gid_out)
{
	char path[64];
	FILE *fp;
	char line[256];

	*uid_out = (uid_t)-1;
	*gid_out = (gid_t)-1;
	snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
	fp = fopen(path, "r");
	if (fp == NULL)
		return;
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (strncmp(line, "Uid:", 4) == 0)
			*uid_out = (uid_t)strtoul(line + 4, NULL, 10);
		else if (strncmp(line, "Gid:", 4) == 0)
			*gid_out = (gid_t)strtoul(line + 4, NULL, 10);
	}
	fclose(fp);
}

/* /proc/<pid>/cmdline: argv, NUL-separated, no trailing anything
 * guaranteed. Empty for a kernel thread (or a real process caught
 * between execve() calls) -- ps(1)'s own convention for that case is
 * "[comm]", reused here rather than an empty string. */
static void read_proc_cmdline(pid_t pid, const char *comm, char *out, size_t out_size)
{
	char path[64];
	FILE *fp;
	size_t n;
	size_t i;

	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
	fp = fopen(path, "r");
	if (fp == NULL) {
		out[0] = '\0';
		return;
	}
	n = fread(out, 1, out_size - 1, fp);
	fclose(fp);
	if (n == 0) {
		snprintf(out, out_size, "[%s]", comm);
		return;
	}
	out[n] = '\0';
	for (i = 0; i < n; i++)
		if (out[i] == '\0')
			out[i] = ' ';
	while (n > 0 && out[n - 1] == ' ')
		out[--n] = '\0';
}

struct hostproc_container_root {
	pid_t pid;
	char name[REGISTRY_NAME_MAX];
};

/* Walks pid's own real host ppid chain -- see hostproc.h's own header
 * comment for why this, and not a /proc/<pid>/ns/pid comparison, is
 * the right correlation here. Bounded at 128 hops purely as a defensive
 * ceiling against a theoretical corrupt/cyclic ppid chain; a real
 * process tree is never remotely this deep. */
static int pid_belongs_to_container(pid_t pid, const struct hostproc_container_root *roots,
                                     int root_count, char *out_name, size_t out_name_size)
{
	pid_t cur = pid;
	int hops;

	for (hops = 0; hops < 128 && cur > 1; hops++) {
		int i;

		for (i = 0; i < root_count; i++) {
			if (roots[i].pid == cur) {
				snprintf(out_name, out_name_size, "%s", roots[i].name);
				return 1;
			}
		}
		if (read_proc_stat(cur, NULL, 0, &cur) != 0)
			break; /* ancestor already exited -- chain ends here, no match */
	}
	return 0;
}

void hostproc_write_json_list(struct json_writer *w)
{
	struct hostproc_container_root roots[REGISTRY_MAX_CONTAINERS];
	int root_count = 0;
	char names[REGISTRY_MAX_CONTAINERS][REGISTRY_NAME_MAX];
	int n, i;
	DIR *d;
	struct dirent *de;

	n = registry_list_names(names, REGISTRY_MAX_CONTAINERS);
	for (i = 0; i < n; i++) {
		struct registry_entry *e = registry_find(names[i]);

		if (e != NULL && e->running) {
			roots[root_count].pid = e->handle.pid;
			snprintf(roots[root_count].name, sizeof(roots[root_count].name), "%s", names[i]);
			root_count++;
		}
	}

	jw_arr_open(w);

	d = opendir("/proc");
	if (d == NULL) {
		jw_arr_close(w);
		return;
	}
	while ((de = readdir(d)) != NULL) {
		pid_t pid;
		pid_t ppid = 0;
		char comm[HOSTPROC_COMM_MAX];
		char cmdline[HOSTPROC_CMDLINE_MAX];
		char container[REGISTRY_NAME_MAX];
		uid_t uid;
		gid_t gid;
		char *endptr;

		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		pid = (pid_t)strtol(de->d_name, &endptr, 10);
		if (*endptr != '\0' || pid <= 0)
			continue;

		if (read_proc_stat(pid, comm, sizeof(comm), &ppid) != 0)
			continue; /* exited between readdir() and here -- not an error, just gone */

		read_proc_cmdline(pid, comm, cmdline, sizeof(cmdline));
		read_proc_ids(pid, &uid, &gid);
		container[0] = '\0';
		pid_belongs_to_container(pid, roots, root_count, container, sizeof(container));

		jw_obj_open(w);
		jw_key(w, "pid");
		jw_int(w, (long long)pid);
		jw_key(w, "ppid");
		jw_int(w, (long long)ppid);
		jw_key(w, "comm");
		jw_str(w, comm);
		jw_key(w, "command_line");
		jw_str(w, cmdline);
		jw_key(w, "container");
		jw_str(w, container);
		jw_key(w, "user_id");
		jw_int(w, (long long)uid);
		jw_key(w, "group_id");
		jw_int(w, (long long)gid);
		jw_obj_close(w);
	}
	closedir(d);
	jw_arr_close(w);
}

enum hostproc_error hostproc_kill(pid_t pid)
{
	char path[64];

	if (pid <= 1 || pid == getpid())
		return HOSTPROC_ERR_FORBIDDEN;

	snprintf(path, sizeof(path), "/proc/%d", (int)pid);
	if (access(path, F_OK) != 0)
		return HOSTPROC_ERR_NOT_FOUND;

	if (kill(pid, SIGKILL) != 0) {
		if (errno == ESRCH)
			return HOSTPROC_ERR_NOT_FOUND;
		return HOSTPROC_ERR_KILL_FAILED;
	}
	return HOSTPROC_OK;
}
