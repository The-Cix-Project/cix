#ifndef REGISTRY_H
#define REGISTRY_H

#include "container.h"
#include "json.h"

#define REGISTRY_MAX_CONTAINERS 256
#define REGISTRY_NAME_MAX 64

enum registry_error {
	REGISTRY_OK = 0,
	REGISTRY_ERR_DUPLICATE,
	REGISTRY_ERR_FULL,
	REGISTRY_ERR_CREATE_FAILED
};

struct registry_entry {
	char name[REGISTRY_NAME_MAX];
	struct container_handle handle;
	int running;       /* 1 while the container's process is alive */
	int exit_status;   /* valid once running == 0 */
	int in_use;        /* 0 for free slots */
	/*
	 * Opaque; owned exclusively by main.c's epoll bookkeeping
	 * (registry.c never reads or writes it beyond zeroing it here).
	 * Holds the reactor's `struct conn *` wrapper for this entry's
	 * pidfd registration while it's still in the epoll set, NULL once
	 * deregistered (container exited and was noticed, or removed).
	 */
	void *reactor_conn;
};

void registry_init(void);

/*
 * Creates and starts a container named `name` per spec, storing it in
 * a fixed-size in-memory table (in-memory only -- see docs/ROADMAP.md
 * Phase 3 for why that's safe: every container dies automatically via
 * PR_SET_PDEATHSIG if this daemon exits, so there's no restart-orphan
 * state to reconcile). On success returns REGISTRY_OK and *out points
 * at the stored entry (stable for the process lifetime -- the table
 * is a fixed array, never reallocated). On REGISTRY_ERR_CREATE_FAILED,
 * errno is set by the failing container_create()/cgroup_create() call.
 */
enum registry_error registry_create(const char *name, const struct container_spec *spec,
                                     struct registry_entry **out);

struct registry_entry *registry_find(const char *name);

/*
 * Call when epoll reports entry->handle.pidfd readable: reaps via
 * container_wait() (safe/non-blocking here -- readability is defined
 * as "the process has already exited") and marks the entry exited.
 * Caller is responsible for epoll_ctl(EPOLL_CTL_DEL) on the pidfd
 * first, since the fd stays open afterward (for GET to keep reporting
 * exit_status) and would otherwise keep firing EPOLLIN forever.
 */
void registry_mark_exited(struct registry_entry *entry);

/*
 * Removes name from the table. If still running, sends SIGKILL via
 * the pidfd and reaps it before removing -- this blocks briefly
 * (microseconds in practice; SIGKILL is unblockable) waiting for the
 * kernel to finish tearing the process down. A fully async kill+reap
 * would need a pending-removal state machine; not warranted for a v1
 * skeleton. Closing the pidfd also drops it from any epoll set.
 * Returns 0, or -1 if no such container.
 */
int registry_remove(const char *name);

void registry_write_json_one(const struct registry_entry *entry, struct json_writer *w);
void registry_write_json_list(struct json_writer *w);

#endif /* REGISTRY_H */
