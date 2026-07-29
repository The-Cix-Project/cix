#ifndef REGISTRY_H
#define REGISTRY_H

#include "container.h"
#include "json.h"
#include "network.h"

#include <time.h>

#define REGISTRY_MAX_CONTAINERS 256
#define REGISTRY_NAME_MAX 64
/* Matches daemon's PKG_IMAGE_NAME_MAX -- see daemon/include/pkg.h. An
 * image name is not a distinct kind of identifier, just a directory
 * name, the same reasoning that constant's own comment already gives. */
#define REGISTRY_IMAGE_NAME_MAX 64

enum registry_error {
	REGISTRY_OK = 0,
	REGISTRY_ERR_DUPLICATE,
	REGISTRY_ERR_FULL,
	REGISTRY_ERR_CREATE_FAILED
};

struct registry_network_attachment {
	char name[NETWORK_NAME_MAX];
	uint32_t ip_be; /* network byte order */
};

/*
 * id echoes back GET /v1/devices' own stable identifier
 * (e.g. "usb:1058:2630:<serial>"); dev_path is where it was mknod()'d
 * inside the container. Devices are host hardware, not something this
 * registry owns or persists (see daemon/src/device.c) -- this struct
 * only mirrors what a running container was granted, for GET
 * /v1/containers to report, same role registry_network_attachment
 * plays for networks.
 */
struct registry_device_attachment {
	char id[96];
	char dev_path[64];
};

struct registry_entry {
	char name[REGISTRY_NAME_MAX];
	char image[REGISTRY_IMAGE_NAME_MAX]; /* the image this container's rootfs was built from */
	struct container_handle handle;
	int running;       /* 1 while the container's process is alive */
	int exit_status;   /* valid once running == 0 */
	/*
	 * Wall-clock time this entry's process was started -- used by
	 * handle_container_event() (daemon/src/main.c) to decide whether an
	 * exiting restart:"always"/"on-failure"/"unless-stopped" container
	 * had a stable-enough run to reset its own crash-restart backoff
	 * (see CONTAINER_RESTART_STABILITY_SECONDS, ADR-0027). Not echoed
	 * over REST -- purely an internal backoff-decision input.
	 */
	time_t started_at;
	int in_use;        /* 0 for free slots */
	struct registry_network_attachment nets[CONTAINER_MAX_NETWORKS];
	int net_count;     /* 0 = not attached to any network */
	int ip_forward;    /* mirrors container_spec.ip_forward, for display */
	struct registry_device_attachment devices[CONTAINER_MAX_DEVICES];
	int device_count;  /* 0 = no devices granted */
	/*
	 * Real host interface names moved into this container's netns --
	 * same bare-name shape container_spec.interfaces[] already has, so
	 * unlike nets/devices above no separate adapter struct is needed.
	 * handle.interfaces_netns_fd (see container.h) is what actually
	 * keeps these safely recoverable at teardown; this array only
	 * remembers which names to hand back to
	 * container_net_teardown_interfaces() when that time comes.
	 */
	char interfaces[CONTAINER_MAX_INTERFACES][CONTAINER_IFNAME_MAX];
	int interface_count; /* 0 = no interfaces granted */
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
 * state to reconcile). nets/net_count/ip_forward are copied atomically
 * as part of this call, not poked in by the caller afterward -- this
 * table reuses freed slots, and stale values left over from a
 * previous occupant would otherwise leak into a new container. On
 * success returns REGISTRY_OK and *out points at the stored entry
 * (stable for the process lifetime -- the table is a fixed array,
 * never reallocated). On REGISTRY_ERR_CREATE_FAILED, errno is set by
 * the failing container_create()/cgroup_create() call.
 */
enum registry_error registry_create(const char *name, const char *image,
                                     const struct container_spec *spec,
                                     const struct registry_network_attachment *nets, int net_count,
                                     int ip_forward,
                                     const struct registry_device_attachment *devices,
                                     int device_count, struct registry_entry **out);

struct registry_entry *registry_find(const char *name);

/*
 * True if any in-use entry currently reports network_name as its own
 * -- used by network_delete() to refuse removing a network something
 * is still attached to.
 */
int registry_network_in_use(const char *network_name);

/*
 * True if any in-use entry currently reports image as the image its
 * rootfs was built from -- used by image_delete() (daemon/src/image.c)
 * to refuse removing an image a running container still references.
 */
int registry_image_in_use(const char *image);

/*
 * Scans in-use entries' recorded IPs for the first unused host
 * address in [host_min, host_max] within network_base_be (the
 * network's address with its host bits already zero, e.g.
 * htonl-of-172.30.0.0 for a /24 -- this project only ever allocates
 * within a single fixed /24, so only the low octet varies). Returns
 * 0 and fills *out_ip_be, or -1 if every address in range is taken.
 * Deliberately topology-agnostic: the subnet itself is owned by
 * daemon/src/main.c, not hardcoded here, so this table doesn't need
 * to know what network topology the daemon happens to be using.
 * No corresponding "release" call is needed: a failed
 * registry_create() never sets in_use, so this scan never counted
 * that address as spent in the first place.
 */
int registry_alloc_ip(uint32_t network_base_be, int host_min, int host_max, uint32_t *out_ip_be);

/*
 * Pure collision check: true if no in-use entry's network attachment
 * already reports this exact address, false otherwise. Deliberately
 * has no notion of subnet/range/gateway -- same topology-agnostic
 * split as registry_alloc_ip() above; a caller validating an explicit,
 * operator-chosen IP (network_ip_available()) does that part itself,
 * this is only ever the final "is it free" check.
 */
int registry_ip_available(uint32_t candidate_be);

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
