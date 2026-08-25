#ifndef HOSTPROC_H
#define HOSTPROC_H

#include "json.h"

#include <sys/types.h>

/*
 * Host process list + kill (logging/web-UI epic Part 6, ADR-0131):
 * "I want a host process command with ls and kill maybe and stuff?
 * also maybe show the process as id, command, command_line, container,
 * user_ID, group_id, and so on? something that shows processes but
 * also shows containers?" A real, direct /proc scan -- every process
 * on the box, not just ones this daemon itself spawned (host-level
 * daemons, a shell an operator started over SSH into the jumpbox
 * container's own console, everything) -- with each one correlated to
 * a container by walking its own real host-visible ppid chain against
 * the registry's own known container root pids (registry_entry.
 * handle.pid), not by comparing PID-namespace identity: every
 * container's own init is a direct clone3() child of cixd itself,
 * so a real host ppid-chain walk from any process reaches either a
 * known container root (that process belongs to it) or cixd's own
 * pid / pid 1 (a plain host-level process) -- simpler than opening and
 * comparing /proc/<pid>/ns/pid for every candidate, and gives the same
 * answer for every case that actually matters here.
 */

#define HOSTPROC_COMM_MAX 256
#define HOSTPROC_CMDLINE_MAX 1024
#define HOSTPROC_WCHAN_MAX 64

struct hostproc_entry {
	pid_t pid;
	pid_t ppid;
	char comm[HOSTPROC_COMM_MAX];
	char cmdline[HOSTPROC_CMDLINE_MAX]; /* argv, space-joined; "[comm]" for a kernel thread */
	char container[64];                 /* empty if this pid isn't inside any known container */
	char state;                         /* R running, S sleeping, D uninterruptible, Z zombie */
	char wchan[HOSTPROC_WCHAN_MAX];     /* kernel symbol it is blocked in; empty if running */
	uid_t uid;
	gid_t gid;
};

/*
 * Writes the bare JSON array (caller wraps it, matching every other
 * *_write_json_list()'s own convention in this daemon). A real,
 * synchronous /proc walk -- no caching, no history -- same "point-in-
 * time snapshot, call again for a fresh one" posture GET /system/stats
 * and GET /containers/{name}/stats already have.
 */
void hostproc_write_json_list(struct json_writer *w);

enum hostproc_error {
	HOSTPROC_OK = 0,
	HOSTPROC_ERR_NOT_FOUND,   /* no such pid currently running */
	HOSTPROC_ERR_FORBIDDEN,   /* pid 1 or this daemon's own real pid -- refused outright */
	HOSTPROC_ERR_KILL_FAILED  /* kill(2) itself failed for some other reason (rare: a real race
	                            * where the process exited between the existence check and the
	                            * kill() call is reported as NOT_FOUND, not this) */
};

/*
 * A point-in-time snapshot of every process, as an array the caller
 * owns and frees. Same walk the JSON listing uses -- deliberately not
 * a second implementation.
 */
enum hostproc_error hostproc_snapshot(struct hostproc_entry **out, size_t *count_out);

/*
 * A real, immediate SIGKILL -- no grace period, unlike container stop
 * (which has real container-lifecycle semantics: SIGTERM then a
 * timeout). This is a blunt, general host-process admin primitive, not
 * a container operation -- an operator reaching for "kill" on a raw
 * pid already means "now." Refuses pid 1 and this daemon's own real
 * pid outright (killing either would crash or reboot the whole host,
 * on a real installed system where cixd runs as real PID 1) --
 * every other pid, including one that happens to belong to a running
 * container, is allowed (killing a container's own init pid this way
 * is exactly equivalent to the container crashing on its own; the
 * existing SIGCHLD-driven exit handling already covers it, no special
 * case needed here).
 */
enum hostproc_error hostproc_kill(pid_t pid);

#endif /* HOSTPROC_H */
