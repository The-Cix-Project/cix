#ifndef CIX_HANDOVER_H
#define CIX_HANDOVER_H

/*
 * ADR-0375 phase 2: the handover record.
 *
 * The running cixd writes it last, immediately before it execs itself in
 * place, and the new cixd reads it first, so the old and new builds
 * share no memory, only this contract. It carries what neither the
 * container's persisted body nor the kernel can give back:
 *
 *   format 1, written as JSON to STATE_DIR/handover.json
 *
 *   format          1; a reader refuses a newer one
 *   written_by      the writing build (CIX_BUILD_VERSION)
 *   written_at      seconds since the epoch
 *   slot            the writer's --slot= ("" for none); a reader whose
 *                   own slot differs is a root switch (phase 4)
 *   bind_addr       the address the writer's listener served on, which
 *                   only the boot's network bootstrap computes
 *   uplink_unattached  the uplink the boot could not attach ("" for
 *                   none), which boot confirmation reads
 *   kmodscan        optional: the last kernel module scan's report, as the
 *                   JSON text GET /v1/system/kmod-scan answers, so the
 *                   boot's sweep is still reported after a handover.
 *                   Absent from a 0.2.57-664 or -665 writer's record, and
 *                   ignored by their readers as an unknown key.
 *   procfuse_pid    procfuse's server, which survives the exec; 0 for none
 *   procfuse_load_fd  the memfd holding its load page (ADR-0349), passed
 *                   across execve; -1 for none
 *   consoles[]      tty, pid: each console shell, which survives the
 *                   exec; pid 0 is a respawn that was pending
 *   containers[]    every registry entry, running or not:
 *     name, image, image_version
 *     running, pid (0 when exited), started_at, paused
 *     exit_status, term_signal, last_exit_reason   (when exited)
 *     userns_enabled, disk_quota_bytes, reserve_devices, ip_forward,
 *     capture_requested, procfuse_expected, init_up, ready
 *     nets[]          every field of struct registry_network_attachment
 *     file_paths[]    what staging wrote (ADR-0375, e34684c5)
 *     dns_servers[]   what staging put in resolv.conf
 *     devices[], pending_devices[], interfaces[]
 *                     every field of their registry structs
 *     services[]      state, pid, has_exit, exit_kind, exit_value,
 *                     restarts, fail_reason, fail_detail, output_fd,
 *                     output_partial (a line read but not yet ended)
 *     fds             init_control, init_report, capture, bpf_prog
 *                     (descriptor numbers passed across execve, -1 absent)
 *     init_output_partial   the same, for cix-init's own stdio
 *     captured_output
 *
 * A pending crash restart is not carried: the new cixd re-arms it from
 * the container's definition. A stop in progress is not carried either:
 * the old cixd waits it out before it hands over.
 *
 * Unknown keys are ignored; a missing required key is a refusal.
 *
 * The record holds the registry's own attachment structs rather than a
 * second, plain copy of their fields: two definitions of one shape drift
 * the first time a field is added, which is the drift this contract
 * exists to prevent.
 */
#include "registry.h"

#include <sys/types.h>

#define HANDOVER_FORMAT 1
#define HANDOVER_FILE "handover.json"

/* The longest line cixd holds back while it waits for the line's end:
 * the size of a container output conn's line buffer, and of what the
 * record carries of it. */
#define HANDOVER_OUTPUT_LINE_MAX 1024

/* Console shells cixd runs (tty0 and ttyS0 today). */
#define HANDOVER_CONSOLES_MAX 4
#define HANDOVER_CONSOLE_TTY_MAX 32

struct handover_console {
	char tty[HANDOVER_CONSOLE_TTY_MAX];
	pid_t pid; /* 0: a respawn was pending */
};

struct handover_service {
	int state;
	pid_t pid;
	int has_exit;
	int exit_kind;
	int exit_value;
	int restarts;
	int fail_reason;
	int fail_detail;
	int output_fd;
	char output_partial[HANDOVER_OUTPUT_LINE_MAX];
};

struct handover_container {
	char name[REGISTRY_NAME_MAX];
	char image[REGISTRY_IMAGE_NAME_MAX];
	char image_version[65];
	int running;
	pid_t pid;
	long long started_at;
	int paused;
	int exit_status;
	int term_signal;
	char last_exit_reason[256];
	int userns_enabled;
	long long disk_quota_bytes;
	int reserve_devices;
	int ip_forward;
	int capture_requested;
	int procfuse_expected;
	int init_up;
	int ready;
	struct registry_network_attachment nets[CONTAINER_MAX_NETWORKS];
	int net_count;
	char file_paths[CONTAINER_MAX_FILES][CONTAINER_FILE_PATH_MAX];
	int file_count;
	char dns_servers[RESOLV_MAX_NAMESERVERS][RESOLV_IP_STRLEN];
	int dns_server_count;
	struct registry_device_attachment devices[CONTAINER_MAX_DEVICES];
	int device_count;
	struct registry_pending_device pending_devices[CONTAINER_MAX_DEVICES];
	int pending_device_count;
	struct registry_interface_record interfaces[CONTAINER_MAX_INTERFACES];
	char interface_hosts[CONTAINER_MAX_INTERFACES][CONTAINER_IFNAME_MAX]; /* "host" */
	int interface_count;
	struct handover_service services[REGISTRY_MAX_SERVICES];
	int service_count;
	int fd_init_control;
	int fd_init_report;
	int fd_capture;
	int fd_bpf_prog;
	char init_output_partial[HANDOVER_OUTPUT_LINE_MAX];
	char captured_output[REGISTRY_CAPTURED_OUTPUT_MAX];
	int captured_output_len;
};

struct handover_record {
	int format;
	char written_by[64];
	long long written_at;
	char slot[8];
	char bind_addr[64];
	char uplink_unattached[64];
	char *kmodscan; /* heap JSON text, or NULL; freed by handover_record_free() */
	pid_t procfuse_pid;
	int procfuse_load_fd;
	struct handover_console consoles[HANDOVER_CONSOLES_MAX];
	int console_count;
	int count;
	struct handover_container *containers; /* count entries, heap */
};

/* Allocates room for up to max containers; 0, or -1 with errno. */
int handover_record_init(struct handover_record *rec, int max);
void handover_record_free(struct handover_record *rec);

/* Writes rec to path atomically. 0, or -1 with err written. */
int handover_write(const char *path, const struct handover_record *rec, char *err,
                   size_t err_size);

/* Reads path into rec (which it allocates). 0, or -1 with err written:
 * unreadable, not JSON, a newer format, or a required key missing. */
int handover_read(const char *path, struct handover_record *rec, char *err, size_t err_size);

/*
 * The kernel checks, before anything else trusts the record: every
 * running container's pid is alive (its pidfd is returned in pidfds[i],
 * -1 for an exited one) and in its own cgroup under cgroup_root, and
 * every passed descriptor is open and of its type -- the cix-init pair a
 * SEQPACKET socket, the pipes FIFOs. FD_CLOEXEC is set back on each. 0,
 * or -1 with err naming the container and the check; pidfds already
 * opened are closed on failure.
 */
int handover_validate(const struct handover_record *rec, const char *cgroup_root, int *pidfds,
                      char *err, size_t err_size);

/* Is pid alive and a child of this process? A handover keeps the pid
 * across execve, so whatever the old cixd forked is still ours to reap;
 * anything else is not ours, whatever the record says. 1 or 0. */
int handover_pid_is_child(pid_t pid);

#endif
