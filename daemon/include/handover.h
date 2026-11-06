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
 *   procfuse_pid    procfuse's server, which survives the exec
 *   containers[]    every registry entry, running or not:
 *     name, image, image_version
 *     running, pid (0 when exited), started_at, paused
 *     exit_status, term_signal, last_exit_reason   (when exited)
 *     restart_at      an armed crash restart's deadline, 0 for none
 *     userns_enabled, disk_quota_bytes, reserve_devices, ip_forward,
 *     capture_requested, procfuse_expected, init_up, ready
 *     nets[]          every field of struct registry_network_attachment
 *     file_paths[]    what staging wrote (ADR-0375, e34684c5)
 *     dns_servers[]   what staging put in resolv.conf
 *     devices[], pending_devices[], interfaces[]
 *                     every field of their registry structs
 *     services[]      state, pid, has_exit, exit_kind, exit_value,
 *                     restarts, fail_reason, fail_detail, output_fd
 *     fds             init_control, init_report, capture, bpf_prog
 *                     (descriptor numbers passed across execve, -1 absent)
 *     captured_output
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
	long long restart_at;
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
	char captured_output[REGISTRY_CAPTURED_OUTPUT_MAX];
	int captured_output_len;
};

struct handover_record {
	int format;
	char written_by[64];
	long long written_at;
	pid_t procfuse_pid;
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

#endif
