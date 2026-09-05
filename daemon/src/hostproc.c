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
static int read_proc_stat(pid_t pid, char *comm_out, size_t comm_size, pid_t *ppid_out,
                           char *state_out)
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
	if (state_out != NULL)
		*state_out = *p; /* R running, S sleeping, D uninterruptible, Z zombie... */
	p += 1; /* step over the single-character state field */
	if (ppid_out != NULL)
		*ppid_out = (pid_t)strtol(p, NULL, 10);
	return 0;
}

/*
 * /proc/<pid>/wchan -- the kernel symbol a sleeping process is blocked
 * in, or empty when it is running.
 *
 * This is the single field that turns "the build is stuck" into a
 * diagnosis. A stalled gcc build showed every process at do_wait --
 * waiting for a child, which says nothing -- except one cc1 sitting in
 * __futex_wait, which said everything: a single-threaded process
 * waiting forever on a lock. Reading it required exec'ing into the
 * container by hand, because this daemon collected everything about a
 * process except what it was waiting for.
 */
static void read_proc_wchan(pid_t pid, char *out, size_t out_size)
{
	char path[64];
	FILE *fp;
	size_t n;

	out[0] = '\0';
	snprintf(path, sizeof(path), "/proc/%d/wchan", (int)pid);
	fp = fopen(path, "r");
	if (fp == NULL)
		return;
	n = fread(out, 1, out_size - 1, fp);
	fclose(fp);
	out[n] = '\0';
	/* "0" is the kernel's own way of saying "not blocked anywhere". */
	if (strcmp(out, "0") == 0)
		out[0] = '\0';
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
		if (read_proc_stat(cur, NULL, 0, &cur, NULL) != 0)
			break; /* ancestor already exited -- chain ends here, no match */
	}
	return 0;
}

/*
 * One /proc walk, two consumers: the JSON listing and the stall
 * reporter. Written once here rather than twice -- the second copy is
 * exactly where the two would drift, and the fields that matter for a
 * stall (state, wchan) are the ones a second implementation would be
 * most likely to omit, which is how they came to be missing in the
 * first place.
 *
 * Caller owns the array and frees it.
 */
enum hostproc_error hostproc_snapshot(struct hostproc_entry **out, size_t *count_out)
{
	struct hostproc_container_root roots[REGISTRY_MAX_CONTAINERS];
	char names[REGISTRY_MAX_CONTAINERS][REGISTRY_NAME_MAX];
	int root_count = 0;
	int n, r;
	struct hostproc_entry *list = NULL;
	size_t count = 0, cap = 0;
	DIR *d;
	struct dirent *de;

	*out = NULL;
	*count_out = 0;

	n = registry_list_names(names, REGISTRY_MAX_CONTAINERS);
	for (r = 0; r < n; r++) {
		struct registry_entry *re = registry_find(names[r]);

		if (re != NULL && re->running) {
			roots[root_count].pid = re->handle.pid;
			snprintf(roots[root_count].name, sizeof(roots[root_count].name), "%s", names[r]);
			root_count++;
		}
	}

	d = opendir("/proc");
	if (d == NULL)
		return HOSTPROC_ERR_NOT_FOUND;
	while ((de = readdir(d)) != NULL) {
		struct hostproc_entry ent;
		pid_t pid;
		char *endptr;

		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		pid = (pid_t)strtol(de->d_name, &endptr, 10);
		if (*endptr != '\0' || pid <= 0)
			continue;

		memset(&ent, 0, sizeof(ent));
		ent.pid = pid;
		ent.state = '?';
		if (read_proc_stat(pid, ent.comm, sizeof(ent.comm), &ent.ppid, &ent.state) != 0)
			continue; /* exited between readdir() and here -- gone, not an error */
		read_proc_cmdline(pid, ent.comm, ent.cmdline, sizeof(ent.cmdline));
		read_proc_wchan(pid, ent.wchan, sizeof(ent.wchan));
		read_proc_ids(pid, &ent.uid, &ent.gid);
		pid_belongs_to_container(pid, roots, root_count, ent.container, sizeof(ent.container));

		if (count == cap) {
			size_t new_cap = cap == 0 ? 128 : cap * 2;
			struct hostproc_entry *grown = realloc(list, new_cap * sizeof(*grown));

			if (grown == NULL) {
				free(list);
				closedir(d);
				return HOSTPROC_ERR_NOT_FOUND;
			}
			list = grown;
			cap = new_cap;
		}
		list[count++] = ent;
	}
	closedir(d);
	*out = list;
	*count_out = count;
	return HOSTPROC_OK;
}

void hostproc_write_json_list(struct json_writer *w)
{
	struct hostproc_entry *procs = NULL;
	size_t count = 0;
	size_t i;

	jw_arr_open(w);
	if (hostproc_snapshot(&procs, &count) != HOSTPROC_OK) {
		jw_arr_close(w);
		return;
	}
	for (i = 0; i < count; i++) {
		char state_str[2];

		jw_obj_open(w);
		jw_key(w, "pid");
		jw_int(w, (long long)procs[i].pid);
		jw_key(w, "ppid");
		jw_int(w, (long long)procs[i].ppid);
		jw_key(w, "comm");
		jw_str(w, procs[i].comm);
		jw_key(w, "command_line");
		jw_str(w, procs[i].cmdline);
		jw_key(w, "container");
		jw_str(w, procs[i].container);
		jw_key(w, "state");
		state_str[0] = procs[i].state;
		state_str[1] = '\0';
		jw_str(w, state_str);
		jw_key(w, "wchan");
		jw_str(w, procs[i].wchan);
		jw_key(w, "user_id");
		jw_int(w, (long long)procs[i].uid);
		jw_key(w, "group_id");
		jw_int(w, (long long)procs[i].gid);
		jw_obj_close(w);
	}
	free(procs);
	jw_arr_close(w);
}

enum hostproc_error hostproc_kill(pid_t pid, int supervised)
{
	char path[64];

	/*
	 * pid 1 is never killable. Before ADR-0246 that was this daemon
	 * itself; now it is the supervisor, and killing the supervisor is
	 * killing init either way.
	 */
	if (pid <= 1)
		return HOSTPROC_ERR_FORBIDDEN;

	/*
	 * ADR-0246: killing THIS daemon is allowed once something is above
	 * it to bring it back.
	 *
	 * The blanket refusal was correct when it was written, because
	 * cixd was pid 1 and killing it was indistinguishable from
	 * destroying the host -- there was no recovery to return to. Under
	 * a supervisor the worker is deliberately disposable: it is
	 * restarted within seconds and the containers it was managing keep
	 * running throughout, because they are re-adopted rather than
	 * recreated. "Restart the control plane" is then an ordinary
	 * operator action rather than an outage.
	 *
	 * Unsupervised it stays refused, and that is not a leftover: a
	 * cixd running as pid 1 that kills itself is a kernel panic.
	 */
	if (pid == getpid() && !supervised)
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
