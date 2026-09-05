#ifndef REGISTRY_H
#define REGISTRY_H

#include "container.h"
#include "diskrole.h"
#include "json.h"
#include "network.h"
#include "resolv.h"

#include <limits.h>
#include <time.h>

#define REGISTRY_MAX_CONTAINERS 256
/*
 * Console declarations (issue #248). Deliberately modest: every one of
 * these is multiplied by REGISTRY_MAX_CONTAINERS in a static array, and
 * a container offering more than a handful of consoles is describing
 * something a console is the wrong shape for. The API contract states
 * the same numbers.
 */
#define REGISTRY_MAX_CONSOLES 4
#define REGISTRY_CONSOLE_NAME_MAX 32
#define REGISTRY_CONSOLE_ARGC_MAX 8
#define REGISTRY_CONSOLE_ARG_MAX 128

struct registry_console {
	char name[REGISTRY_CONSOLE_NAME_MAX];
	char argv[REGISTRY_CONSOLE_ARGC_MAX][REGISTRY_CONSOLE_ARG_MAX];
	int argc;
};
#define REGISTRY_NAME_MAX 64

/*
 * ADR-0180: asynchronous teardown intents -- see struct
 * registry_entry's own teardown_kind comment for the lifecycle.
 */
#define REGISTRY_TEARDOWN_NONE 0
#define REGISTRY_TEARDOWN_STOP 1
#define REGISTRY_TEARDOWN_DELETE 2

/* Issue #119: a teardown still incomplete this long after its SIGKILL is
 * almost certainly wedged in the kernel (a pidns init stuck reaping),
 * not merely slow -- worth telling the operator about, once. */
#define REGISTRY_TEARDOWN_STALL_SECONDS 120
/* Same cap pkg.c's own PKG_BUILD_OUTPUT_CAPTURE_MAX uses for build-output
 * capture -- generous enough for a real startup failure's own diagnostic
 * text, bounded so one misbehaving container can't grow this indefinitely. */
#define REGISTRY_CAPTURED_OUTPUT_MAX 4096
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

/*
 * One extra environment variable this container was created with
 * (POST /v1/containers' own "env" field) -- display-only echo storage,
 * the same "id not content" precedent file_paths/sysctls above already
 * have. Recovered by registry_create() splitting spec->envp's own
 * "KEY=VALUE" strings on their first '=' (see that function's own
 * comment); env_key_is_safe() at parse time already guarantees no key
 * contains one, so the split is always unambiguous.
 */
struct container_env {
	char key[CONTAINER_ENV_KEY_MAX];
	char value[CONTAINER_ENV_VALUE_MAX];
};

struct registry_network_attachment {
	char name[NETWORK_NAME_MAX];
	uint32_t ip_be; /* network byte order */
	/*
	 * veth_host/ifname (ADR-0156): only ever populated for a LIVE
	 * attachment (POST .../networks on an already-running container,
	 * task #861) -- empty for every attachment made the ordinary way,
	 * at container-creation time, since those never need a standalone
	 * detach path (the whole container tears down together). veth_host
	 * is the real host-side veth interface name (deleting it removes
	 * both ends of the pair, including the container-side one, no
	 * setns() needed for detach); ifname is the interface name inside
	 * the container's own netns (echoed back so an operator can tell
	 * which live-attached interface is which without cross-referencing
	 * anything else).
	 */
	char veth_host[16];
	char ifname[16];
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
	/*
	 * ADR-0161 Phase D: the same fields struct device_spec (container.h)
	 * carries, needed to rebuild a live cgroup's own full BPF device
	 * list on a later attach/detach without re-resolving every already-
	 * granted device fresh from sysfs each time (a device could
	 * theoretically be renumbered between calls; these were correct at
	 * the moment this grant was actually made, which is what the
	 * currently-attached BPF program itself already encodes).
	 */
	enum device_node_type type;
	unsigned int major;
	unsigned int minor;
	/*
	 * 0 for a grant made at container creation time; 1 only for one
	 * made by a later POST .../devices live attach. Mirrors registry_
	 * network_attachment's own veth_host[0]!='\0' "is this live"
	 * convention, as an explicit named field here since dev_path is
	 * never empty for a real grant (unlike veth_host). DELETE
	 * .../devices/{id} refuses to remove an entry with live == 0 --
	 * a create-time grant has no standalone detach path, the same rule
	 * registry_network_detach() already enforces for networks.
	 */
	int live;
};

struct registry_entry {
	char name[REGISTRY_NAME_MAX];
	char image[REGISTRY_IMAGE_NAME_MAX]; /* the image this container's rootfs was built from */
	/*
	 * The specific image version (ADR-0107/0108, a sha256 manifest-
	 * identity hash -- see IMAGE_VERSION_MAX in daemon/include/image.h,
	 * not included here just for one array size) this container's own
	 * overlay lowerdir was pinned to at creation time -- resolved once,
	 * via image_current_version(), and never re-resolved afterward,
	 * even across a daemon-restart replay (main.c's own
	 * create_container_from_body() persists it back into the stored
	 * create-request body precisely so replay doesn't silently re-pin
	 * to whatever the image's current_version happens to be by then).
	 * This is the actual mechanism that fixes the bug the whole
	 * package/image versioning epic exists for: a later `pkg install`
	 * against the same image name can never change what an
	 * already-running (or restarted) container's own lowerdir points
	 * at. Empty for the one container that predates real image
	 * versioning at all -- the ephemeral PKG_BUILD_CONTAINER_NAME
	 * sandbox, rooted on g_pkgbuild_rootfs, not any named image.
	 */
	char image_version[65];
	/*
	 * The container's REAL overlay lowerdir, copied verbatim from the
	 * container_spec at create time (issue #61). Ordinary containers get
	 * their image-version rootfs here -- the same path the read-file
	 * fallback used to reconstruct from image/image_version -- but a
	 * build container ("__pkgbuild-N", registered under the synthetic
	 * image name "pkgbuild" with no version) gets g_pkgbuild_rootfs or a
	 * custom build image's rootfs, which that reconstruction could never
	 * produce. Storing what was actually used removes the guesswork and
	 * makes exited-container reads work for every container kind.
	 * Empty for a userns container, which uses a per-container rootfs
	 * rather than an overlay (ADR-0179 phase 2c).
	 */
	char lowerdir[PATH_MAX];
	struct container_handle handle;
	/*
	 * ADR-0246: 1 when this entry was re-adopted at worker startup
	 * rather than started by this worker. An adopted container is not
	 * this process's child -- it was reparented to the supervisor when
	 * the previous worker died -- so its exit cannot be collected with
	 * waitid() here and arrives over the supervisor's reap channel
	 * instead. Every other field means exactly what it does for a
	 * container this worker started.
	 */
	int adopted;
	int running;       /* 1 while the container's process is alive */
	int exit_status;   /* valid once running == 0; raw waitid() si_status */
	/*
	 * Companion to exit_status (issue #78): 0 if the container exited
	 * normally (exit_status is a real exit code), else the signal number
	 * it was killed by (exit_status is then that same signal number, not
	 * an exit code). A deliberate stop/delete SIGKILLs the container, so
	 * this is 9 for every stopped/deleted-while-running container. Valid
	 * once running == 0; not persisted, same as exit_status.
	 */
	int term_signal;
	/*
	 * Human-readable "why," valid once running == 0 alongside
	 * exit_status above -- either the child's own real diagnostic text
	 * (container_read_diag(), e.g. "child: execve(/usr/bin/sleep): No
	 * such file or directory") when the container's own diag pipe still
	 * had something in it, or container_decode_exit_status()'s own
	 * fixed category text as a fallback otherwise. Not persisted (like
	 * exit_status itself, and for the same reason: purely a live-run
	 * diagnostic, meaningless across a daemon restart since the process
	 * that produced it is long gone either way).
	 */
	char last_exit_reason[256];
	/*
	 * 1 while frozen via the cgroup v2 freezer (POST .../pause,
	 * ADR-0045) -- the process itself is still `running` (its pid is
	 * alive, waitid() would still block on it) but every task in the
	 * cgroup is stopped at the kernel level via cgroup.freeze, not
	 * SIGSTOP (SIGSTOP is visible to and can be ignored/handled by the
	 * traced process; the freezer is not). Only meaningful while
	 * running == 1 -- cleared, not persisted, on exit/removal, same as
	 * every other purely-live piece of registry_entry state.
	 */
	int paused;
	/*
	 * ADR-0180: which asynchronous teardown, if any, this entry is
	 * mid-flight in (REGISTRY_TEARDOWN_*). Set by registry_begin_kill()
	 * the moment an operator's DELETE/stop SIGKILLs a running
	 * container; read back by main.c's handle_container_event() when
	 * the pidfd actually fires, to run that teardown's own completion
	 * (disk cleanup, registry slot release) instead of the crash-
	 * restart logic. NONE for every ordinary running/exited entry.
	 * Purely live state, never persisted -- the durable half of a
	 * teardown's intent (containerdef removal for delete, the stopped
	 * flag for stop) is written synchronously in the handler itself
	 * before the kill, so a daemon restart mid-teardown can never
	 * resurrect the container either way.
	 */
	int teardown_kind;
	/*
	 * Issue #119: when teardown_kind became non-NONE (registry_begin_kill),
	 * and whether the "this teardown is stuck" warning has already been
	 * logged for it. A container whose pidns init wedges in the kernel
	 * (zap_pid_ns_processes, waiting to reap a process that will not die)
	 * never fires its pidfd, so its async teardown (ADR-0180) never
	 * completes and it sits in "deleting" indefinitely, holding its
	 * build slot. Nothing userspace can do forces the kernel to finish,
	 * but an operator should at least be TOLD, the same way a quiet
	 * build is (pkg_check_build_stalls). Reported once, not every tick.
	 */
	time_t teardown_started_at;
	int teardown_stall_reported;
	/*
	 * Issue #49: the disk quota requested at creation (bytes; 0 = none)
	 * -- mirrored here because unlike the cgroup limits (all read back
	 * live from the real cgroup files at serialization time) a project
	 * quota has no per-container kernel file to consult through a kept
	 * fd; the quotactl() readback needs the backing device + projid
	 * plumbing that deliberately lives in main.c. The mirror can't
	 * drift: nothing changes a container's quota after creation, and a
	 * daemon-restart replay re-applies the same persisted body.
	 */
	long long disk_quota_bytes;
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
	int userns_enabled; /* mirrors container_spec.userns_enabled (ADR-0207), for display */
	struct registry_device_attachment devices[CONTAINER_MAX_DEVICES];
	int device_count;  /* 0 = no devices granted */
	/*
	 * ADR-0161 Phase B: raw id/devicemap-name strings from an
	 * "optional": true POST /containers devices[] entry that didn't
	 * resolve to a present device at creation time. Live-run-only
	 * bookkeeping, same posture as every other purely-live field in
	 * this struct -- a restart-capable container's own persisted
	 * create-request body (containerdef.c) already carries these for
	 * free via replay; an unpersisted (restart_policy: "no") container
	 * simply loses them across a daemon restart, matching what happens
	 * to that container's entire registry_entry anyway. Phase C's
	 * hotplug listener matches a newly-appeared device's own id against
	 * every running container's pending list.
	 */
	char pending_devices[CONTAINER_MAX_DEVICES][96];
	int pending_device_count; /* 0 = nothing pending */
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
	 * Container-relative paths staged into this container's own
	 * upperdir at creation time (POST /v1/containers' own "files"
	 * field) -- paths only, for GET/ps/inspect to echo "what's there,"
	 * not content (echoing full file content back on every list/get
	 * would be needlessly heavy, and this project's own "id not
	 * content" precedent already applies to devices too).
	 */
	char file_paths[CONTAINER_MAX_FILES][CONTAINER_FILE_PATH_MAX];
	int file_count; /* 0 = no files staged */
	/*
	 * The consoles this container DECLARES (issue #248) -- what it
	 * offers, in declaration order, the first being what an attach
	 * with no selector gets.
	 *
	 * A count of 0 means this container has no console, which is a
	 * real answer and the default. Before this every container got
	 * the same hardcoded /usr/bin/bash with an argv of exactly one
	 * token, so a container without bash got a session that opened
	 * and instantly died, and a console needing arguments could not
	 * be expressed at all.
	 *
	 * Stored on the entry rather than read back from the persisted
	 * definition because this is live state a GET echoes, the same
	 * reasoning file_paths above already follows. Sized small on
	 * purpose: this struct is one of REGISTRY_MAX_CONTAINERS static
	 * entries, so the array is a real memory cost paid 256 times.
	 */
	struct registry_console consoles[REGISTRY_MAX_CONSOLES];
	int console_count; /* 0 = this container declares no console */
	/*
	 * net.* sysctls applied inside this container's own netns at
	 * creation time -- mirrors container_spec.sysctls[] directly
	 * (read from spec inside registry_create(), same as interfaces[]
	 * above; no separate explicit parameter needed since it's already
	 * part of spec, unlike files above which never touch container_spec
	 * at all).
	 */
	struct container_sysctl sysctls[CONTAINER_MAX_SYSCTLS];
	int sysctl_count; /* 0 = no sysctls applied */
	/*
	 * Opt-in exceptions to container_caps_drop()'s own fixed default
	 * capability deny-list (issue #29) this container was created with --
	 * mirrors container_spec.cap_add[] directly, same "read from spec
	 * inside registry_create(), no separate parameter" pattern sysctls[]
	 * above already uses. 0 = the fixed default deny-list applies with
	 * no exceptions (every container that predates this field, and any
	 * new one that doesn't ask for an exception).
	 */
	char cap_add[CONTAINER_MAX_CAP_ADD][CONTAINER_CAP_NAME_MAX];
	int cap_add_count;
	/*
	 * The entrypoint argv this container was created with (POST
	 * /v1/containers' own "cmd" field) -- copied in registry_create()
	 * the same way interfaces[]/sysctls[] already are (read directly
	 * from spec->argv, no separate parameter), so GET/inspect can
	 * finally answer "what is this container actually running," which
	 * previously nothing persisted anywhere: spec->argv itself only
	 * ever pointed into the create request's own parsed JSON tree,
	 * freed immediately after container_create() returns.
	 */
	char cmd[CONTAINER_MAX_ARGV][CONTAINER_ARGV_MAX];
	int cmd_count; /* always >= 1 for an in-use entry */
	/*
	 * Extra environment variables this container was created with
	 * (POST /v1/containers' own "env" field, see struct container_env's
	 * own doc comment above) -- copied in registry_create() the same
	 * way cmd[]/cmd_count above already are, by walking spec->envp
	 * until NULL (no separate explicit parameter needed, same "already
	 * part of spec" reasoning cmd[] itself already has).
	 */
	struct container_env env[CONTAINER_MAX_ENV];
	int env_count; /* 0 = no extra env vars */
	/*
	 * The disk (disk.h's own bare kernel name, e.g. "sdb") this
	 * container's own upper/work/merged overlay directories live
	 * under, or an empty string for the default (CONTAINERS_DIR, the
	 * OS disk) -- task #638/ADR-0102. Purely a memo for reconstructing
	 * the right base path at stats/file-read time (see main.c's
	 * container_base_for()); registry_create() itself is handed the
	 * already-resolved path, this field never drives any path
	 * computation on its own.
	 */
	char disk_name[DISKROLE_DISK_NAME_MAX];
	/*
	 * The real /etc/resolv.conf nameserver IPs staged into this
	 * container's own upperdir at creation time (POST /v1/containers'
	 * own "dns_servers" field, ADR-0143) -- display-only, the exact
	 * same "id not content" precedent file_paths above already has;
	 * RESOLV_MAX_NAMESERVERS is resolv.h's own cap, reused here rather
	 * than a second invented limit.
	 */
	char dns_server_ips[RESOLV_MAX_NAMESERVERS][RESOLV_IP_STRLEN];
	int dns_server_count; /* 0 = no dns_servers given */
	/*
	 * Opaque; owned exclusively by main.c's epoll bookkeeping
	 * (registry.c never reads or writes it beyond zeroing it here).
	 * Holds the reactor's `struct conn *` wrapper for this entry's
	 * pidfd registration while it's still in the epoll set, NULL once
	 * deregistered (container exited and was noticed, or removed).
	 */
	void *reactor_conn;
	/*
	 * stdout/stderr capture -- the ordinary-container analog of the
	 * pkg-build-container capture ADR-0056/0087 already established,
	 * closing a real observability gap: a container that execve()s
	 * cleanly and then exits on its own for a reason with no other
	 * visible signal (e.g. a daemon refusing to start over a config
	 * problem) previously had no way to say why, since neither the diag
	 * pipe (container_read_diag(), only ever written by Cix's own
	 * pre-exec setup steps) nor anything else captured what the
	 * exec'd program itself wrote. output_fd is main.c's own
	 * epoll-owned read end of the capture pipe (-1 once not capturing,
	 * or already drained to EOF) -- always present now (transparent
	 * container-log capture: every container's own output is always
	 * forwarded, line by line, into logstore.c as a source="container"
	 * entry, regardless of capture_requested below). captured_output/
	 * captured_output_len hold whatever was read so far, bounded and
	 * drained incrementally exactly like ADR-0087's build-output pipe
	 * (never letting the container itself block on a full pipe buffer)
	 * -- but only ever populated when capture_requested is set; this
	 * field pair is the client-facing GET /v1/containers/{name} tail,
	 * a separate, still-opt-in feature from the always-on logstore
	 * forwarding above, fed from the same underlying pipe.
	 */
	int output_fd;
	int capture_requested; /* 1 iff "capture_output" was set at creation time --
	                         * distinguishes "never asked for capture" (captured_output
	                         * stays "") from "asked, but nothing written yet" (same
	                         * empty string) for GET's own null-vs-"" reporting. Does NOT
	                         * gate output_fd's own existence or the logstore forward
	                         * anymore -- see this field's own sibling doc comment above. */
	char captured_output[REGISTRY_CAPTURED_OUTPUT_MAX];
	int captured_output_len;
};

void registry_init(void);

/*
 * Creates and starts a container named `name` per spec, storing it in
 * a fixed-size in-memory table (in-memory only -- see docs/roadmap/ROADMAP.md
 * Phase 3 for why that's safe: every container dies automatically via
 * PR_SET_PDEATHSIG if this daemon exits, so there's no restart-orphan
 * state to reconcile). nets/net_count/ip_forward/file_paths/file_count
 * are copied atomically as part of this call, not poked in by the
 * caller afterward -- this table reuses freed slots, and stale values
 * left over from a previous occupant would otherwise leak into a new
 * container. file_paths/file_count are display-only (the files were
 * already staged onto disk by the caller before this call -- see
 * daemon/src/main.c's own "files" handling -- this is purely so GET
 * can echo what's there); pass NULL/0 if none. spec->sysctls/
 * spec->sysctl_count are copied the same way interfaces[] already is,
 * read directly from spec rather than a separate parameter, since
 * unlike files they're already part of container_spec. On success
 * returns REGISTRY_OK and *out points at the stored entry (stable for
 * the process lifetime -- the table is a fixed array, never
 * reallocated). On REGISTRY_ERR_CREATE_FAILED, errno is set by the
 * failing container_create()/cgroup_create() call.
 */
/*
 * disk_name (task #638/ADR-0102): the disk this container's overlay
 * storage was placed on -- an empty string ("") for the default
 * (CONTAINERS_DIR). Purely recorded onto the new entry, same
 * display-only reasoning file_paths/file_count already have; the
 * caller has already resolved the actual filesystem paths (spec->ov.*)
 * before calling this.
 *
 * dns_server_ips/dns_server_count (ADR-0143): the same display-only
 * "already staged onto disk, this is purely so GET can echo it" shape
 * file_paths/file_count already have, for the "dns_servers" field's
 * own nameserver list; pass NULL/0 if none.
 */
enum registry_error registry_create(const char *name, const char *image,
                                     const char *image_version, const struct container_spec *spec,
                                     const struct registry_network_attachment *nets, int net_count,
                                     int ip_forward,
                                     const struct registry_device_attachment *devices,
                                     int device_count,
                                     const char file_paths[][CONTAINER_FILE_PATH_MAX],
                                     int file_count, const char *disk_name,
                                     const char dns_server_ips[][RESOLV_IP_STRLEN],
                                     int dns_server_count, int adopt_if_running,
                                     struct registry_entry **out);

struct registry_entry *registry_find(const char *name);

/* ADR-0246: lookup by the host pid of the container's init -- the only
 * identifier the supervisor's reap records carry. */
struct registry_entry *registry_find_by_pid(pid_t pid);

/*
 * Record what consoles a container declares (issue #248).
 *
 * A setter rather than four more parameters on registry_create(), which
 * already takes fourteen: consoles are a declaration this daemon echoes
 * back and resolves at attach time, and nothing in the container runtime
 * needs them, so threading them through container_spec would widen an
 * interface for a value it never reads. Called immediately after a
 * successful create; a container with none simply never calls it.
 */
void registry_set_consoles(const char *name, const struct registry_console *consoles, int count);

/*
 * Writes up to max in-use entries' own names into out_names (any
 * order), returns the count. For callers that need to enumerate every
 * currently-known container regardless of whether it also has a
 * persisted definition (containerdef.c only persists one for
 * restart != "no" -- a plain, unpersisted container is still a real,
 * live registry_entry and must not be invisible to this kind of scan).
 */
int registry_list_names(char out_names[][REGISTRY_NAME_MAX], int max);

/*
 * Freezes (freeze=1) or thaws (freeze=0) e via the cgroup v2 freezer
 * (ADR-0045), updating e->paused on success. See registry.c's own
 * comment for the full rationale (real kernel freeze, not SIGSTOP) and
 * why registry_remove() also calls this internally before killing a
 * paused container. Returns 0 on success, -1 (errno set) otherwise.
 */
int registry_set_paused(struct registry_entry *e, int freeze);

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
 * within a single fixed /24, so only the low octet varies), also
 * skipping exclude_be if it's nonzero (a network's own gateway
 * address, when it has one -- no longer always host-part 1, so the
 * caller can't just start host_min past it the way it used to).
 * Returns 0 and fills *out_ip_be, or -1 if every address in range is
 * taken. Deliberately topology-agnostic: the subnet itself, and
 * *why* exclude_be is reserved, are owned by daemon/src/main.c/
 * network.c, not hardcoded here -- this table only ever gets told
 * "skip this one address too," not "what a gateway is."
 * No corresponding "release" call is needed: a failed
 * registry_create() never sets in_use, so this scan never counted
 * that address as spent in the first place.
 */
int registry_alloc_ip(uint32_t network_base_be, int host_min, int host_max, uint32_t exclude_be,
                       uint32_t *out_ip_be);

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
 * Names the container currently holding an address, and whether it is
 * being torn down. Exists so a rejection can say WHICH container holds
 * the address rather than only that one does.
 *
 * The distinction matters because container deletion is asynchronous:
 * DELETE returns as soon as the teardown is under way, so an immediate
 * recreate at the same address legitimately races a holder that is
 * on its way out. Reported as "shutting down" rather than as a plain
 * conflict, because the two call for different responses -- retry
 * shortly, versus go and find what is using it.
 *
 * Returns 0 if nothing holds it.
 */
int registry_ip_holder(uint32_t candidate_be, char *out_name, size_t out_name_size,
                        int *out_tearing_down);

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
 * ADR-0246: record an exit whose status was collected by someone else --
 * the supervisor, for a container this worker re-adopted rather than
 * started. Same bookkeeping registry_mark_exited() does once it has
 * reaped one of its own children; the two share this implementation.
 */
void registry_mark_exited_with(struct registry_entry *entry, int status, int sig);

/*
 * Removes name from the table. If still running, sends SIGKILL via
 * the pidfd and reaps it before removing -- this blocks waiting for
 * the kernel to finish tearing the process down: microseconds in
 * practice, but NOT bounded in principle ("SIGKILL is unblockable" is
 * about signal masks, not about a task stuck in uninterruptible
 * D-state, where the kill only pends). The original comment here
 * deferred a fully-async kill+reap as "not warranted for a v1
 * skeleton" -- ADR-0180 makes exactly that call in the other
 * direction after two real production wedges (issue #67): a
 * delete/stop of a running container froze the entire single-threaded
 * daemon on this wait. Operator-facing delete/stop no longer come
 * through this running-kill branch at all (registry_begin_kill()
 * below + main.c's handle_container_event() completion); the branch
 * remains only for the internal storage-migration finalize path,
 * whose all-or-nothing job semantics genuinely want a synchronous
 * stop (tracked for async conversion on issue #67).
 * Returns 0, or -1 if no such container.
 */
int registry_remove(const char *name);

/*
 * ADR-0180: begin an asynchronous teardown of a RUNNING entry --
 * unconditionally thaws the cgroup first (a frozen cgroup blocks
 * SIGKILL delivery forever, and e->paused can in principle go stale;
 * thawing an unfrozen cgroup is a harmless no-op write), sends
 * SIGKILL via the pidfd, records teardown_kind, and returns
 * immediately -- never waits, never touches the reactor registration
 * (the entry's own pidfd epoll watch is precisely what will fire when
 * the process actually dies, at which point handle_container_event()
 * runs this teardown's completion). Caller is responsible for having
 * already written the teardown's durable intent (containerdef
 * removal / stopped flag) BEFORE calling, so no daemon restart in the
 * window can resurrect the container.
 */
void registry_begin_kill(struct registry_entry *e, int teardown_kind);

/*
 * ADR-0156/task #861: appends one live network attachment to e->nets[]
 * (which caller must have already fully populated, veth_host/ifname
 * included) and increments e->net_count. Returns 0, or -1 (net_count
 * already at CONTAINER_MAX_NETWORKS, or this exact network name is
 * already attached) without modifying e.
 */
int registry_network_attach(struct registry_entry *e, const struct registry_network_attachment *net);

/*
 * ADR-0156/task #861: the reverse -- finds network_name in e->nets[],
 * copies it into *out (so the caller can still reach its veth_host
 * after this call removes it from the table) and removes it, shifting
 * later entries down and decrementing net_count. Returns 0, or -1 (no
 * such attachment on e) leaving e and *out untouched.
 */
int registry_network_detach(struct registry_entry *e, const char *network_name,
                             struct registry_network_attachment *out);

/*
 * ADR-0161 Phase B: records raw[0..count-1] into e->pending_devices[]
 * (truncated at CONTAINER_MAX_DEVICES, which create_container_from_
 * body() already bounds count against via the same array it built
 * device_specs[] from). Call once, right after a successful
 * registry_create(), for every "optional": true devices[] entry that
 * didn't resolve to a present device at creation time. A no-op if
 * count == 0.
 */
void registry_set_pending_devices(struct registry_entry *e, const char pending[][96], int count);

/*
 * ADR-0161 Phase D: live-grants new_device/new_attachment to e's
 * already-running cgroup. Rebuilds the full struct device_spec list
 * from e->devices[] (already-granted, host-resolved fields, not
 * re-resolved from sysfs) plus new_device, and calls container_dev_
 * bpf_attach() again against the exact same cgroup_fd this container
 * was created with -- a plain re-ATTACH on an already-NONE-flag
 * cgroup atomically replaces the prior program (empirically verified,
 * see ADR-0161's own "Phase D verification"), so there is no window
 * where an already-granted device becomes briefly ungranted, or
 * (the real risk direction) briefly unrestricted. On success, the
 * now-superseded e->handle.bpf_prog_fd is closed and replaced, and
 * new_attachment (its own "live" field must be 1) is appended to
 * e->devices[]. Returns 0 on success; -1 (errno set, e left
 * completely unmodified) if e->device_count is already at
 * CONTAINER_MAX_DEVICES, this exact id is already granted, or the
 * real bpf(2) attach itself fails.
 */
int registry_device_live_attach(struct registry_entry *e, const struct device_spec *new_device,
                                 const struct registry_device_attachment *new_attachment);

/*
 * ADR-0161 Phase D: the reverse of registry_device_live_attach() --
 * finds id in e->devices[], removes it, and re-derives the cgroup's
 * own BPF program from what remains: a plain re-ATTACH if any devices
 * remain, or a real, explicit container_dev_bpf_detach() if this was
 * the last one (a live detach down to zero must not silently leave
 * the just-removed device's own grant in effect -- container_dev_bpf_
 * attach()'s own device_count == 0 shortcut is correct at container-
 * creation time, where "nothing attached yet" and "detach a program
 * that was already there" are the same state, but they are NOT the
 * same state here). *out is filled with the removed attachment on
 * success (0). Returns -1 (e untouched) if id isn't currently granted
 * to e at all, or if the real bpf(2) call itself fails.
 *
 * Deliberately carries NO "was this a create-time grant" policy check
 * of its own -- mirrors registry_network_detach()'s own shape exactly
 * (that function removes whatever matches by name; DELETE .../networks/
 * {network}'s own handler is what checks veth_host[0] first and
 * refuses before ever calling it). The equivalent live == 0 check for
 * devices lives in handle_container_device_detach() (main.c) for the
 * same reason: an operator-facing DELETE must refuse a create-time
 * grant (recreate the container instead), but Phase C's own hotplug-
 * driven unplug revocation must NOT be bound by that same restriction
 * -- a device physically going away needs its grant revoked
 * regardless of how it was originally attached, and reuses this exact
 * function to do it.
 */
int registry_device_live_detach(struct registry_entry *e, const char *id,
                                 struct registry_device_attachment *out);

/*
 * ADR-0161 Phase B/C: removes ref from e->pending_devices[] (a no-op
 * if not present), shifting later entries down and decrementing
 * pending_device_count. Called once a pending reference has either
 * been successfully live-attached (Phase C's own reconciliation pass)
 * or the operator otherwise no longer wants it tracked.
 */
void registry_clear_pending_device(struct registry_entry *e, const char *ref);

void registry_write_json_one(const struct registry_entry *entry, struct json_writer *w);
void registry_write_json_list(struct json_writer *w);

#endif /* REGISTRY_H */
