#include "container.h"
#include "containerdef.h"
#include "device.h"
#include "dns.h"
#include "exec.h"
#include "http.h"
#include "image.h"
#include "iohelpers.h"
#include "json.h"
#include "linux_compat.h"
#include "namecheck.h"
#include "network.h"
#include "persist.h"
#include "pki.h"
#include "pkg.h"
#include "registry.h"
#include "rtnetlink.h"
#include "staticfile.h"
#include "websocket.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define DEFAULT_PORT 7620
#define DEFAULT_BIND "127.0.0.1"
#define DEFAULT_WEB_ROOT "web"
#define DEFAULT_BASE_DIR "/var/lib/kanxeo"
/*
 * Runtime-overridable via --data-dir=PATH (default DEFAULT_BASE_DIR,
 * unchanged from every prior release). Every subsystem already takes
 * its own path as an explicit init-time parameter (network_init(),
 * pkg_init(), image_init(), ... -- see main()'s own init sequence
 * below), so this is the ONLY place BASE_DIR needs to become
 * runtime-computed rather than a compile-time #define; nothing
 * downstream has a second hardcoded copy to also fix.
 *
 * Exists because this project's own test suite has zero isolation
 * from a live daemon sharing the same default path -- confirmed the
 * hard way (twice, same session): running the test suite against a
 * host that also has a real kanxeod on default paths silently wipes
 * that daemon's container/network/DNS/PKI state and any image content
 * package installs had built, because every test's own reset_state()
 * resets these exact same files. --data-dir= lets tests point
 * somewhere that can never collide with a real install.
 */
static char g_base_dir[PATH_MAX] = DEFAULT_BASE_DIR;
static char IMAGES_DIR[PATH_MAX];
static char CONTAINERS_DIR[PATH_MAX];
static char NETWORKS_STATE_PATH[PATH_MAX];
static char DNS_RECORDS_STATE_PATH[PATH_MAX];
static char PKI_DIR[PATH_MAX];
static char PKI_CERTS_STATE_PATH[PATH_MAX];
static char PKI_CERTS_DIR[PATH_MAX];
static char PKG_DIR[PATH_MAX];
static char PKG_INSTALLED_STATE_PATH[PATH_MAX];
static char PKG_RECIPES_DIR[PATH_MAX];
static char CONTAINER_DEFS_STATE_PATH[PATH_MAX];

/* Computes every path derived from g_base_dir -- called once, right
 * after argv parsing (so --data-dir= has already been applied) and
 * before anything (including boot_init(), which mounts the real
 * containers partition at g_base_dir under --init-mode) touches any
 * of them. */
static void init_base_dir_paths(void)
{
	snprintf(IMAGES_DIR, sizeof(IMAGES_DIR), "%s/images", g_base_dir);
	snprintf(CONTAINERS_DIR, sizeof(CONTAINERS_DIR), "%s/containers", g_base_dir);
	snprintf(NETWORKS_STATE_PATH, sizeof(NETWORKS_STATE_PATH), "%s/networks.json", g_base_dir);
	snprintf(DNS_RECORDS_STATE_PATH, sizeof(DNS_RECORDS_STATE_PATH), "%s/dns_records.json", g_base_dir);
	snprintf(PKI_DIR, sizeof(PKI_DIR), "%s/pki", g_base_dir);
	snprintf(PKI_CERTS_STATE_PATH, sizeof(PKI_CERTS_STATE_PATH), "%s/pki_certs.json", PKI_DIR);
	snprintf(PKI_CERTS_DIR, sizeof(PKI_CERTS_DIR), "%s/certs", PKI_DIR);
	snprintf(PKG_DIR, sizeof(PKG_DIR), "%s/pkg", g_base_dir);
	snprintf(PKG_INSTALLED_STATE_PATH, sizeof(PKG_INSTALLED_STATE_PATH), "%s/pkg_installed.json", PKG_DIR);
	snprintf(PKG_RECIPES_DIR, sizeof(PKG_RECIPES_DIR), "%s/recipes", PKG_DIR);
	snprintf(CONTAINER_DEFS_STATE_PATH, sizeof(CONTAINER_DEFS_STATE_PATH), "%s/container_defs.json", g_base_dir);
}
/*
 * Backoff cap and stability-reset threshold for the crash-restart
 * delay (ADR-0027) -- the base delay itself is per-container
 * (restart_delay_seconds, default CONTAINERDEF_DEFAULT_RESTART_DELAY_
 * SECONDS, see containerdef.h), but backoff itself (doubling per
 * consecutive failure) is uniform and not a further per-container
 * knob (YAGNI). A container is considered "stable" (its own
 * consecutive_failures resets to 0) once it's been running at least
 * CONTAINER_RESTART_STABILITY_SECONDS before exiting again -- a
 * crash-loop history from long before doesn't deserve the same
 * backoff as one from a moment ago.
 */
#define CONTAINER_RESTART_BACKOFF_CAP_SECONDS 30
#define CONTAINER_RESTART_STABILITY_SECONDS 30
/*
 * Fixed QEMU virtio-blk layout (Phase 11's stated fixed/known-hardware
 * scope, same posture as root=/dev/vda2 in the loader entry itself) --
 * a GPT-partition-type-based ESP lookup can wait for part 4's real
 * hardware if it turns out to be needed there.
 */
#define ESP_DEVICE "/dev/vda1"
#define ESP_DIR "/boot"
#define ESP_LOADER_ENTRIES_DIR ESP_DIR "/loader/entries"
/*
 * Partitions 2/3 in kanxeo-install's own layout (image/src/kanxeo-
 * install.c's auto_partition()), same fixed QEMU virtio-blk layout/
 * posture as ESP_DEVICE above -- a GPT-partition-name-based lookup
 * (find_partition_device() already exists for this purpose, but only
 * in kanxeo-install.c, installer-only code the daemon doesn't link)
 * can wait for part 4's real hardware, the same deferral ESP_DEVICE's
 * own comment and ADR-0018's Consequences section already state twice
 * for other partitions. root=%s2/%s3 in populate_esp()'s own loader
 * entries confirms this exact device-per-slot mapping.
 */
#define ROOT_A_DEVICE "/dev/vda2"
#define ROOT_B_DEVICE "/dev/vda3"
/*
 * Partition 4 in kanxeo-install's own layout (image/src/kanxeo-install.c)
 * -- absent on parts 1/2's own throwaway 2/3-partition test disks, so
 * mounting it is deliberately non-fatal (boot_init()'s only non-fatal
 * mount): its absence just means "not a real installed system," not a
 * broken boot.
 */
#define CONFIG_DEVICE "/dev/vda4"
#define CONFIG_DIR "/config"
#define NET_CONF_PATH CONFIG_DIR "/net.conf"
/*
 * Partition 5, same fixed QEMU virtio-blk layout as ESP_DEVICE/
 * CONFIG_DEVICE above -- also absent on parts 1/2's throwaway 2/3-
 * partition test disks, so boot_init() falls back to a tmpfs at
 * BASE_DIR when this device doesn't exist, exactly like every other
 * "not a real installed system" case in this function.
 */
#define CONTAINERS_DEVICE "/dev/vda5"
#define MAX_EVENTS 64
#define CONTAINERS_PREFIX "/v1/containers/"
#define NETWORKS_PREFIX "/v1/networks/"
#define DNS_RECORDS_PREFIX "/v1/dns/records/"
#define DNS_SERVERS_PREFIX "/v1/dns/servers/"
#define PKI_CERTS_PREFIX "/v1/pki/certs/"
#define PKG_PREFIX "/v1/pkg/"
#define PKG_RECIPES_PREFIX "/v1/pkg/recipes/"
#define IMAGES_PREFIX "/v1/images/"

enum conn_kind {
	CONN_LISTENER,
	CONN_CLIENT,
	CONN_CONTAINER,
	CONN_PKG_FETCH,
	CONN_RESTART_TIMER,
	CONN_CONSOLE_SHELL,
	CONN_CONSOLE_RESPAWN_TIMER,
	CONN_CONSOLE_WS,        /* GET /v1/containers/{name}/console -- client-facing WebSocket half */
	CONN_CONSOLE_PTY,       /* same session's other half -- the exec'd shell's pty master fd */
	/*
	 * A conn already torn down mid-batch (console_session_teardown()
	 * below) but not yet free()'d -- see g_pending_free's own comment
	 * for why the free is deferred. The event loop skips these
	 * immediately rather than dispatching on any of their other,
	 * already-invalid fields.
	 */
	CONN_DEAD
};

struct conn {
	enum conn_kind kind;
	int fd;
	struct http_conn http;                 /* CONN_CLIENT only */
	struct registry_entry *entry;           /* CONN_CONTAINER only */
	pid_t pkg_fetch_pid;                    /* CONN_PKG_FETCH only */
	char restart_name[REGISTRY_NAME_MAX];   /* CONN_RESTART_TIMER only */
	pid_t console_pid;                      /* CONN_CONSOLE_SHELL only */
	char console_tty[32];                   /* CONN_CONSOLE_SHELL / CONN_CONSOLE_RESPAWN_TIMER */
	struct console_exec_session *exec_session; /* CONN_CONSOLE_WS / CONN_CONSOLE_PTY only -- shared by both halves of one session */
	struct ws_conn ws;                      /* CONN_CONSOLE_WS only -- incremental client-frame parser */
};

/*
 * One exec-console session's shared state, referenced by both its own
 * conns (the client-facing WS half and the pty-master half) so either
 * side noticing the other has gone away can tear down the whole pair.
 * The first time this daemon has needed two independently-epoll-
 * registered fds to represent one logical session -- see
 * console_session_teardown() and g_pending_free below for the real
 * hazard that shape introduces and how it's handled.
 */
struct console_exec_session {
	struct conn *ws_conn;
	struct conn *pty_conn;
	pid_t exec_pid;
	int torn_down;
};

/*
 * What to do once the event loop actually stops -- reachable via
 * SIGTERM/SIGINT (unchanged, defaults to a graceful poweroff rather
 * than the bare `return 0` that, as PID 1, the kernel treats as "init
 * exited" and panics on) or the new /v1/system/{shutdown,reboot}
 * endpoints below. Only ever acted on when running --init-mode (real
 * PID 1) -- see main()'s own post-loop handling; a dev/test kanxeod
 * (no --init-mode, e.g. every test/*.c invocation) just exits normally
 * regardless of this value, exactly as it always has.
 */
enum shutdown_action { SHUTDOWN_ACTION_POWEROFF, SHUTDOWN_ACTION_REBOOT };

static int g_epfd;
static struct conn g_listener_conn;
static const char *g_web_root;
static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_shutdown_action = SHUTDOWN_ACTION_POWEROFF;

/*
 * conns torn down mid-batch are queued here instead of free()'d
 * immediately. A single kx_epoll_wait() call can report BOTH halves of
 * one console session's fd pair as ready in the same batch; freeing
 * the first one processed would leave the second event's own `struct
 * conn *` dangling for the rest of that same batch's for-loop. Drained
 * (actually freed) once the whole batch has been dispatched -- see the
 * main loop below. Sized to MAX_EVENTS since a batch can never report
 * more events than that, and console_session_teardown() is itself
 * idempotent (guarded by torn_down), so it can never queue more than
 * one free per conn regardless of how many events reference it.
 */
#define MAX_PENDING_FREE MAX_EVENTS
static struct conn *g_pending_free[MAX_PENDING_FREE];
static int g_pending_free_count;

static void queue_conn_free(struct conn *cc)
{
	if (g_pending_free_count < MAX_PENDING_FREE)
		g_pending_free[g_pending_free_count++] = cc;
	else
		free(cc); /* unreachable per MAX_PENDING_FREE's own bound -- fail safe rather than leak */
}

static void drain_pending_free(void)
{
	int i;

	for (i = 0; i < g_pending_free_count; i++)
		free(g_pending_free[i]);
	g_pending_free_count = 0;
}

/*
 * Kills (if still running -- SIGKILL is a harmless no-op/ESRCH if the
 * shell already exited on its own) and reaps the exec'd process, tears
 * down both fds/epoll registrations, and queues both conns for
 * deferred free. Safe to call from either side (the WS client
 * disconnecting, or the pty hitting EOF/EIO because the shell exited)
 * and safe to call twice (torn_down guards against it).
 */
static void console_session_teardown(struct console_exec_session *sess)
{
	if (sess->torn_down)
		return;
	sess->torn_down = 1;

	kill(sess->exec_pid, SIGKILL);
	waitpid(sess->exec_pid, NULL, 0);

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, sess->ws_conn->fd, NULL);
	close(sess->ws_conn->fd);
	ws_conn_free(&sess->ws_conn->ws);
	sess->ws_conn->kind = CONN_DEAD;
	queue_conn_free(sess->ws_conn);

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, sess->pty_conn->fd, NULL);
	close(sess->pty_conn->fd);
	sess->pty_conn->kind = CONN_DEAD;
	queue_conn_free(sess->pty_conn);

	free(sess);
}
/*
 * This boot's own slot and bind address, set once from argv in main()
 * -- previously plain locals, read only by confirm_boot()'s call site
 * and a diagnostic print. Promoted here (same file-scope-static
 * precedent as g_epfd) so handle_system_update() below can also reach
 * them: it needs g_slot to compute the *inactive* slot, and g_bind_addr
 * to embed the same address into the fresh loader entry it writes for
 * that slot. NULL/unset (g_slot) means this isn't a real --init-mode
 * boot -- handle_system_update() rejects the request rather than
 * guessing at an "inactive" slot that doesn't meaningfully exist.
 *
 * g_port joins them for the same reason (Phase 19): spawn_console_shell()
 * and its respawn-timer path need the real listening port to hand
 * kanxeoctl a --port= that actually reaches this daemon, and both are
 * called from places that don't otherwise have it in scope.
 */
static const char *g_slot;
static const char *g_bind_addr;
static int g_port;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static int ensure_dir(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	return 0;
}

static int mount_or_fail(const char *source, const char *target, const char *fstype,
                          unsigned long flags)
{
	if (mount(source, target, fstype, flags, NULL) != 0) {
		perror(target);
		return -1;
	}
	return 0;
}

/*
 * Only reached with --init-mode, i.e. kanxeod running as PID 1 on a bare
 * kernel boot with no initramfs (Phase 11) -- nothing else has mounted
 * /proc, /sys, or cgroup2 yet. devtmpfs is populated by the kernel itself
 * (CONFIG_DEVTMPFS_MOUNT) before init ever runs, so /dev needs no mount
 * here. Same proc mount flags mountns_pivot() already uses for each
 * container's own /proc (src/mountns.c) -- one already-correct flag set,
 * not a second one invented.
 *
 * BASE_DIR is the real kanxeo-containers partition (CONTAINERS_DEVICE) --
 * everything a running daemon persists (images/, containers/,
 * networks.json, dns_records.json, pki_certs.json, pkg_installed.json)
 * genuinely survives a real reboot, not just a plain daemon restart
 * within the same still-running kernel. Falls back to a tmpfs at the
 * same mount point when that device doesn't exist (parts 1/2's own
 * throwaway 2/3-partition test disks): a real, deliberate exception
 * for "not a real installed system," same posture as CONFIG_DEVICE's
 * own non-fatal mount below, not a bug in that fallback path. This
 * was originally meant to land in Phase 11 part 3 (the installer
 * already formats and names this partition for exactly this purpose)
 * but the swap itself never actually happened until now -- see
 * ADR-0018.
 */
/*
 * Finds the first real, non-loopback network interface -- not hardcoded
 * to a specific kernel-assigned name. Confirmed empirically that QEMU's
 * virtio-net gets "eth0" with no udev/systemd predictable-naming running
 * (this environment's own boot never printed a registration line, so
 * this was checked with a throwaway diagnostic init, not assumed from
 * dmesg silence), but a fixed name could differ on part 4's real target
 * hardware. "sit0" is always present once IPv6/SIT is compiled in -- a
 * pseudo-interface, never a real NIC, explicitly skipped.
 */
static int find_nic(char *out_name, size_t out_size)
{
	DIR *d;
	struct dirent *de;
	int found = 0;

	d = opendir("/sys/class/net");
	if (d == NULL) {
		perror("/sys/class/net");
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (strcmp(de->d_name, "lo") == 0)
			continue;
		if (strncmp(de->d_name, "sit", 3) == 0)
			continue;
		snprintf(out_name, out_size, "%s", de->d_name);
		found = 1;
		break;
	}
	closedir(d);
	if (!found) {
		fprintf(stderr, "find_nic: no real network interface found\n");
		return -1;
	}
	return 0;
}

/* Parses the simple key=value net.conf kanxeo-install writes to the
 * config partition (image/src/kanxeo-install.c's populate step) --
 * ip=/prefix=/gateway=, one per line. */
static int parse_net_conf(const char *path, char *out_ip, size_t ip_size, int *out_prefix,
                           char *out_gateway, size_t gateway_size)
{
	FILE *f;
	char line[256];
	int have_ip = 0, have_prefix = 0, have_gateway = 0;

	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		line[strcspn(line, "\n")] = '\0';
		if (strncmp(line, "ip=", 3) == 0) {
			snprintf(out_ip, ip_size, "%s", line + 3);
			have_ip = 1;
		} else if (strncmp(line, "prefix=", 7) == 0) {
			*out_prefix = atoi(line + 7);
			have_prefix = 1;
		} else if (strncmp(line, "gateway=", 8) == 0) {
			snprintf(out_gateway, gateway_size, "%s", line + 8);
			have_gateway = 1;
		}
	}
	fclose(f);
	return (have_ip && have_prefix && have_gateway) ? 0 : -1;
}

/*
 * Applies the static IP configured at install time (Phase 11 part 3) --
 * the concrete answer to "static IP configured at install time" from
 * this phase's own design. A missing or incomplete net.conf is not an
 * error (0, not -1): parts 1/2's own test disks never write one, and
 * that must stay a normal, inert boot, not a failure.
 */
static int apply_static_ip(void)
{
	char ip[64], gateway[64], nic[IFNAMSIZ];
	int prefix = 0;
	struct in_addr addr, gw;
	int rtfd;

	if (parse_net_conf(NET_CONF_PATH, ip, sizeof(ip), &prefix, gateway, sizeof(gateway)) != 0)
		return 0;

	if (find_nic(nic, sizeof(nic)) != 0)
		return -1;

	if (inet_pton(AF_INET, ip, &addr) != 1) {
		fprintf(stderr, "apply_static_ip: invalid ip %s\n", ip);
		return -1;
	}
	if (inet_pton(AF_INET, gateway, &gw) != 1) {
		fprintf(stderr, "apply_static_ip: invalid gateway %s\n", gateway);
		return -1;
	}

	rtfd = rtnl_open();
	if (rtfd < 0) {
		perror("rtnl_open");
		return -1;
	}
	if (rtnl_link_set_up(rtfd, nic) != 0 || rtnl_addr_add_ipv4(rtfd, nic, addr.s_addr, prefix) != 0 ||
	    rtnl_route_add_default_ipv4(rtfd, gw.s_addr) != 0) {
		perror("apply_static_ip");
		rtnl_close(rtfd);
		return -1;
	}
	rtnl_close(rtfd);

	printf("init-mode: applied static ip %s/%d via %s, gateway %s\n", ip, prefix, nic, gateway);
	fflush(stdout);
	return 0;
}

static int boot_init(void)
{
	int rtfd;

	if (mount_or_fail("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC) != 0)
		return -1;
	if (mount_or_fail("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC) != 0)
		return -1;
	if (mount_or_fail("cgroup2", "/sys/fs/cgroup", "cgroup2", 0) != 0)
		return -1;
	if (mount(CONTAINERS_DEVICE, g_base_dir, "ext4", MS_NOSUID | MS_NODEV, NULL) != 0 &&
	    mount_or_fail("tmpfs", g_base_dir, "tmpfs", MS_NOSUID | MS_NODEV) != 0)
		return -1;
	/* Needed to reach the loader entry confirm_boot() renames once this
	 * boot proves healthy (Phase 11 part 2) -- writable, not read-only
	 * like the root mounts, since that rename is a real write. */
	if (mount_or_fail(ESP_DEVICE, ESP_DIR, "vfat", MS_NOSUID | MS_NODEV | MS_NOEXEC) != 0)
		return -1;
	/* Deliberately non-fatal, unlike every mount above -- see
	 * CONFIG_DEVICE's own comment. */
	if (mount(CONFIG_DEVICE, CONFIG_DIR, "ext4", 0, NULL) == 0)
		apply_static_ip();

	/*
	 * A fresh kernel boot brings lo up as a device but leaves it
	 * administratively down (no IFF_UP) -- DEFAULT_BIND's bind() to
	 * 127.0.0.1 fails with EADDRNOTAVAIL until something sets it up.
	 * Reuses rtnl_link_set_up() (netplane/), the same primitive every
	 * container's own network setup already calls -- not a second,
	 * ioctl-based way to change link state.
	 */
	rtfd = rtnl_open();
	if (rtfd < 0) {
		perror("rtnl_open");
		return -1;
	}
	if (rtnl_link_set_up(rtfd, "lo") != 0) {
		perror("rtnl_link_set_up lo");
		rtnl_close(rtfd);
		return -1;
	}
	rtnl_close(rtfd);

	return 0;
}

/*
 * Renames whichever loader entry on the ESP matches this boot's slot
 * (kanxeo-<slot>[+<tries-left>[-<tries-done>]].conf) down to the bare
 * kanxeo-<slot>.conf, stripping systemd-boot's own Automatic Boot
 * Assessment counter suffix -- a plain rename(2), not a bootctl
 * subprocess call, the same hand-rolled-daemon posture ADR-0007/
 * ADR-0014 already established. Called only once this boot has actually
 * reached a healthy, serving state (main()'s own call site, right
 * before the reactor loop starts) -- "about to serve traffic" is the
 * honest definition of healthy this confirms.
 */
static int confirm_boot(const char *slot)
{
	DIR *d;
	struct dirent *de;
	char prefix[32];
	size_t prefix_len;
	char oldpath[PATH_MAX];
	char newpath[PATH_MAX];
	int found = 0;

	snprintf(prefix, sizeof(prefix), "kanxeo-%s", slot);
	prefix_len = strlen(prefix);

	d = opendir(ESP_LOADER_ENTRIES_DIR);
	if (d == NULL) {
		perror(ESP_LOADER_ENTRIES_DIR);
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, prefix, prefix_len) == 0) {
			found = 1;
			snprintf(oldpath, sizeof(oldpath), "%s/%s", ESP_LOADER_ENTRIES_DIR, de->d_name);
			break;
		}
	}
	closedir(d);

	if (!found) {
		fprintf(stderr, "confirm_boot: no loader entry found for slot %s\n", slot);
		return -1;
	}

	snprintf(newpath, sizeof(newpath), "%s/kanxeo-%s.conf", ESP_LOADER_ENTRIES_DIR, slot);
	if (strcmp(oldpath, newpath) == 0)
		return 0; /* already confirmed (no counter suffix) -- nothing to do */
	if (rename(oldpath, newpath) != 0) {
		perror("confirm_boot rename");
		return -1;
	}
	return 0;
}

static int name_is_valid(const char *name)
{
	return simple_name_is_valid(name, REGISTRY_NAME_MAX);
}

static void respond_json(int fd, int status, const char *status_text, struct json_writer *w)
{
	http_set_blocking(fd);
	http_write_response(fd, status, status_text, "application/json", w->buf, w->len);
}

static void respond_error(int fd, int status, const char *status_text, const char *msg)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "error");
	jw_str(&w, msg);
	jw_obj_close(&w);
	respond_json(fd, status, status_text, &w);
	jw_free(&w);
}

static const char *http_status_text(int status)
{
	switch (status) {
	case 400:
		return "Bad Request";
	case 409:
		return "Conflict";
	case 500:
		return "Internal Server Error";
	default:
		return "Error";
	}
}

/*
 * Pure (no fd) resolver, shared by respond_network_error() below and
 * create_container_from_body()'s own networks-array validation (which
 * has no fd yet to respond on directly when reused for boot autostart
 * /crash restart -- see containerdef.h).
 */
static int network_error_to_status(enum network_error err, const char **out_msg)
{
	switch (err) {
	case NETWORK_ERR_INVALID_NAME:
		*out_msg = "invalid network name";
		return 400;
	case NETWORK_ERR_INVALID_SUBNET:
		*out_msg = "invalid subnet/prefix_len";
		return 400;
	case NETWORK_ERR_INVALID_GATEWAY:
		*out_msg = "invalid gateway (must be a real address within this subnet, "
		           "not the network or broadcast address)";
		return 400;
	case NETWORK_ERR_DUPLICATE:
		*out_msg = "a network with this name already exists";
		return 409;
	case NETWORK_ERR_OVERLAP:
		*out_msg = "subnet overlaps an existing network";
		return 400;
	case NETWORK_ERR_FULL:
		*out_msg = "network table full";
		return 500;
	case NETWORK_ERR_NOT_FOUND:
		*out_msg = "no such network";
		return 404;
	case NETWORK_ERR_IN_USE:
		*out_msg = "network is still in use by a container";
		return 409;
	case NETWORK_ERR_IP_OUT_OF_RANGE:
		*out_msg = "ip is not a usable address on this network";
		return 400;
	case NETWORK_ERR_IP_TAKEN:
		*out_msg = "ip is already assigned to a running container";
		return 409;
	case NETWORK_ERR_INTERFACE_NOT_FOUND:
		*out_msg = "unknown or unassignable interface (see GET /v1/devices)";
		return 400;
	case NETWORK_ERR_INTERFACE_ATTACHED:
		*out_msg = "interface is already attached to this network";
		return 409;
	case NETWORK_ERR_INTERFACE_NOT_ATTACHED:
		*out_msg = "interface is not attached to this network";
		return 404;
	case NETWORK_ERR_INTERFACE_FULL:
		*out_msg = "this network's interface table is full";
		return 500;
	case NETWORK_ERR_INTERFACE_NAME_TOO_LONG:
		*out_msg = "ifname.vlan_id would not fit in IFNAMSIZ";
		return 400;
	case NETWORK_ERR_CREATE_FAILED:
	case NETWORK_ERR_DELETE_FAILED:
	default:
		*out_msg = "network operation failed";
		return 500;
	}
}

static void respond_network_error(int fd, enum network_error err)
{
	const char *msg;
	int status = network_error_to_status(err, &msg);

	respond_error(fd, status, http_status_text(status), msg);
}

static void handle_health(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "status");
	jw_str(&w, "ok");
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* Both just record what main()'s own post-loop cleanup should do once
 * the event loop actually stops (g_stop=1) -- responding here, before
 * that happens, so the client sees a real reply rather than the
 * connection simply dropping mid-shutdown. */
static void handle_shutdown(int fd)
{
	struct json_writer w;

	g_shutdown_action = SHUTDOWN_ACTION_POWEROFF;
	g_stop = 1;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "status");
	jw_str(&w, "shutting down");
	jw_obj_close(&w);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}

static void handle_reboot(int fd)
{
	struct json_writer w;

	g_shutdown_action = SHUTDOWN_ACTION_REBOOT;
	g_stop = 1;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "status");
	jw_str(&w, "rebooting");
	jw_obj_close(&w);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}

/*
 * Tries-left counter for a freshly-staged update's own loader entry --
 * matches kanxeo-install.c's own ROOT_A_TRIES value (3), the same
 * Automatic Boot Assessment convention applied uniformly to any fresh
 * slot, not just the very first install.
 */
#define ROOT_UPDATE_TRIES 3

/*
 * Writes the whole content of src_path onto dst device_path, raw --
 * mirrors kanxeo-install.c's own copy_file()/write_whole_file_to_
 * device() shape (plain open/read/write loop, no byte-offset math,
 * since device_path is a real partition block device here too, not
 * test/test_disk_image.c's flat-file-at-an-offset case). device_path
 * already exists as a block device, so unlike copy_file()'s
 * O_CREAT|O_TRUNC destination, this opens it O_WRONLY only. Kept as
 * its own small static rather than shared across the daemon/installer
 * binaries -- genuinely trivial, and the two binaries share no common
 * library today (the daemon can't link test/test_image_fixture.c,
 * which is test-scoped).
 */
/*
 * Copies every byte of src_fd onto dst_fd, then fsync()s dst_fd before
 * returning success -- without that, these bytes can still be sitting
 * in the page cache, not yet flushed through to the underlying device/
 * filesystem, when do_system_update() reports 200 back to the caller.
 * The whole point of this feature is a trustworthy "the new image is
 * safely on the inactive slot" signal; a crash/power-loss between that
 * response and an eventual background writeback could otherwise
 * silently lose it. Shared by write_file_to_device() (a raw partition
 * device) and write_file_to_esp() (a regular file on the already-
 * mounted ESP) -- callers own opening/closing both fds, since the
 * right open() flags genuinely differ between the two (a device
 * already exists; an ESP file may not).
 */
static int copy_bytes(int src_fd, int dst_fd)
{
	char buf[65536];
	ssize_t n;

	while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
		if (write(dst_fd, buf, (size_t)n) != n) {
			perror("write");
			return -1;
		}
	}
	if (n < 0) {
		perror("read");
		return -1;
	}
	if (fsync(dst_fd) != 0) {
		perror("fsync");
		return -1;
	}
	return 0;
}

/* device_path already exists as a block device -- O_WRONLY only, no
 * O_CREAT (unlike write_file_to_esp() below). */
static int write_file_to_device(const char *src_path, const char *device_path)
{
	int src, dst, rc;

	src = open(src_path, O_RDONLY);
	if (src < 0) {
		perror(src_path);
		return -1;
	}
	dst = open(device_path, O_WRONLY);
	if (dst < 0) {
		perror(device_path);
		close(src);
		return -1;
	}
	rc = copy_bytes(src, dst);
	close(src);
	close(dst);
	return rc;
}

/* esp_path is a plain file on the already-mounted ESP -- it may not
 * exist yet (first time this slot's kernel file is ever written) or
 * may already exist (overwriting a prior update), so O_CREAT|O_TRUNC,
 * unlike write_file_to_device()'s existing-device case above. */
static int write_file_to_esp(const char *src_path, const char *esp_path)
{
	int src, dst, rc;

	src = open(src_path, O_RDONLY);
	if (src < 0) {
		perror(src_path);
		return -1;
	}
	dst = open(esp_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (dst < 0) {
		perror(esp_path);
		close(src);
		return -1;
	}
	rc = copy_bytes(src, dst);
	close(src);
	close(dst);
	return rc;
}

/*
 * Pure (no fd) core of POST /v1/system/update -- writes a fresh
 * control-plane squashfs and/or a fresh kernel onto whichever slot
 * ISN'T this boot's own (g_slot), then stages a fresh systemd-boot
 * loader entry for it with its own Automatic Boot Assessment
 * tries-left counter -- the same "write to the inactive slot, let the
 * existing boot-counter/confirm_boot() machinery decide whether it
 * sticks" shape ADR-0014 already established for the very first
 * install, now reachable on an already-running system too (see
 * docs/adr/0031, docs/adr/0032). Split out from handle_system_update()
 * the same way create_container_from_body() is split from
 * handle_create() -- so test/test_boot_update.c's --test-update-
 * image=/--test-update-kernel= self-test (main(), no live HTTP round
 * trip needed) can exercise the exact same real device/ESP-write and
 * loader-entry logic a live request would, inside a real QEMU guest
 * with a real virtio-blk disk attached, without needing the daemon to
 * also be its own HTTP client.
 *
 * image_path/kernel_path are local paths the operator has already
 * transferred the new files to (e.g. scp) -- no upload/streaming HTTP
 * machinery is added here; see ADR-0031 for why. Independent and both
 * optional (at least one required): a kanxeod security fix needs no
 * new kernel, and new hardware support needs no new root.
 *
 * Deliberately does NOT reboot -- update and reboot stay two separate,
 * composable actions (ADR-0031). The operator (or a script) calls the
 * existing POST /v1/system/reboot separately once ready to cut over;
 * confirm_boot()/the boot counter, both entirely unchanged, decide
 * whether the fresh slot sticks.
 *
 * Returns the HTTP status this operation resolves to (200 on success);
 * *out_slot/*out_updated_root/*out_updated_kernel are filled in only
 * on success, *out_errmsg only otherwise.
 */
static int do_system_update(const char *body, size_t body_len, char *out_slot,
                             size_t out_slot_size, int *out_updated_root, int *out_updated_kernel,
                             char *out_errmsg, size_t out_errmsg_size)
{
	struct json_value *root;
	const char *image_path;
	const char *kernel_path;
	const char *inactive_slot;
	const char *device;
	int src;
	unsigned char magic4[4];
	unsigned char magic2[2];
	char kernel_dest[PATH_MAX];
	char entry_path[PATH_MAX];
	char entry_conf[512];

	if (g_slot == NULL) {
		snprintf(out_errmsg, out_errmsg_size,
		         "this daemon has no --slot=, there is no inactive slot to update");
		return 400;
	}
	if (strcmp(g_slot, "a") == 0) {
		inactive_slot = "b";
		device = ROOT_B_DEVICE;
	} else if (strcmp(g_slot, "b") == 0) {
		inactive_slot = "a";
		device = ROOT_A_DEVICE;
	} else {
		snprintf(out_errmsg, out_errmsg_size, "unrecognized --slot=, expected \"a\" or \"b\"");
		return 400;
	}
	snprintf(kernel_dest, sizeof(kernel_dest), "%s/kanxeo-bzImage-%s", ESP_DIR, inactive_slot);

	root = json_parse(body, body_len);
	if (root == NULL) {
		snprintf(out_errmsg, out_errmsg_size, "invalid JSON body");
		return 400;
	}
	image_path = json_as_string(json_object_get(root, "image_path"));
	kernel_path = json_as_string(json_object_get(root, "kernel_path"));
	if ((image_path == NULL || image_path[0] == '\0') &&
	    (kernel_path == NULL || kernel_path[0] == '\0')) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size, "image_path and/or kernel_path required");
		return 400;
	}

	/* Cheap, real safety checks before touching the inactive slot at
	 * all: squashfs's own on-disk magic ("hsqs", the little-endian
	 * bytes of 0x73717368) and a bzImage's own on-disk magic (the x86
	 * boot-sector signature 0x55 0xAA at offset 0x1FE, and "HdrS" --
	 * struct setup_header's own magic -- at offset 0x202, the same
	 * check bootloaders use) each turn a wrong path by mistake into a
	 * clean 400 instead of silently overwriting the inactive slot with
	 * garbage. Both files are fully validated before either is
	 * written, so a bad kernel_path never leaves a real image_path
	 * partially applied. */
	if (image_path != NULL && image_path[0] != '\0') {
		src = open(image_path, O_RDONLY);
		if (src < 0) {
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size,
			         "image_path does not exist or is not readable");
			return 400;
		}
		if (read(src, magic4, 4) != 4 || memcmp(magic4, "hsqs", 4) != 0) {
			close(src);
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size, "image_path is not a squashfs image");
			return 400;
		}
		close(src);
	}
	if (kernel_path != NULL && kernel_path[0] != '\0') {
		src = open(kernel_path, O_RDONLY);
		if (src < 0) {
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size,
			         "kernel_path does not exist or is not readable");
			return 400;
		}
		if (lseek(src, 0x1FE, SEEK_SET) != 0x1FE || read(src, magic2, 2) != 2 ||
		    magic2[0] != 0x55 || magic2[1] != 0xAA || lseek(src, 0x202, SEEK_SET) != 0x202 ||
		    read(src, magic4, 4) != 4 || memcmp(magic4, "HdrS", 4) != 0) {
			close(src);
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size, "kernel_path is not a valid bzImage");
			return 400;
		}
		close(src);
	}

	if (image_path != NULL && image_path[0] != '\0' && write_file_to_device(image_path, device) != 0) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size, "failed to write image to inactive slot");
		return 500;
	}
	if (kernel_path != NULL && kernel_path[0] != '\0' &&
	    write_file_to_esp(kernel_path, kernel_dest) != 0) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size, "failed to write kernel to inactive slot");
		return 500;
	}

	snprintf(entry_path, sizeof(entry_path), "%s/kanxeo-%s+%d.conf", ESP_LOADER_ENTRIES_DIR,
	         inactive_slot, ROOT_UPDATE_TRIES);
	/* version is this boot's own current timestamp -- always higher
	 * than whatever's already on disk, so systemd-boot sorts this
	 * entry first, with no need to parse the existing entry's own
	 * version back out first. linux always references this slot's own
	 * kanxeo-bzImage-<slot> (pre-staged for both slots at install
	 * time, ADR-0032) -- whether or not kernel_path was given this
	 * call, that file already exists and is exactly what should boot. */
	snprintf(entry_conf, sizeof(entry_conf),
	         "title Kanxeo (%s)\n"
	         "sort-key kanxeo\n"
	         "version %ld\n"
	         "linux /kanxeo-bzImage-%s\n"
	         "options console=tty0 console=ttyS0 root=%s rw init=/bin/kanxeod -- --init-mode "
	         "--slot=%s --bind=%s\n",
	         inactive_slot[0] == 'a' ? "A" : "B", (long)time(NULL), inactive_slot, device,
	         inactive_slot, g_bind_addr);

	{
		int efd = open(entry_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
		size_t len = strlen(entry_conf);

		/* fsync() for the same reason write_file_to_device()/write_
		 * file_to_esp() do -- this file is the ONLY thing that makes
		 * the just-written image/kernel bootable at all; it needs to
		 * be durable too, not still sitting in the page cache when
		 * this call returns. */
		if (efd < 0 || write(efd, entry_conf, len) != (ssize_t)len || fsync(efd) != 0) {
			if (efd >= 0)
				close(efd);
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size, "failed to write loader entry");
			return 500;
		}
		close(efd);
	}

	json_free(root);
	snprintf(out_slot, out_slot_size, "%s", inactive_slot);
	*out_updated_root = (image_path != NULL && image_path[0] != '\0');
	*out_updated_kernel = (kernel_path != NULL && kernel_path[0] != '\0');
	return 200;
}

static void handle_system_update(int fd, const char *body, size_t body_len)
{
	char slot[8];
	char errmsg[256];
	int status;
	int updated_root, updated_kernel;
	struct json_writer w;

	status = do_system_update(body, body_len, slot, sizeof(slot), &updated_root, &updated_kernel,
	                           errmsg, sizeof(errmsg));
	if (status != 200) {
		respond_error(fd, status, http_status_text(status), errmsg);
		return;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "status");
	jw_str(&w, "staged");
	jw_key(&w, "slot");
	jw_str(&w, slot);
	jw_key(&w, "updated");
	jw_arr_open(&w);
	if (updated_root)
		jw_str(&w, "root");
	if (updated_kernel)
		jw_str(&w, "kernel");
	jw_arr_close(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Pure (no fd) core of GET /v1/system/backup -- bundles platform
 * *configuration* state (what containers/networks/DNS records/
 * packages should exist), not workload data or image content, and
 * never PKI (see docs/adr/0033). Each file is read via the existing
 * persist_read_file() (daemon/src/persist.c) exactly as-is and
 * embedded as an escaped JSON string, the same "raw content as a JSON
 * string" shape ConfigFile.content already uses -- not a second,
 * raw-JSON-embedding mechanism. persist_read_file() already treats a
 * missing file as valid-empty (ENOENT -> 0-length), so a fresh system
 * with e.g. no DNS records yet needs no special-casing here.
 *
 * container_defs alone is already a complete reconstruction source:
 * ADR-0025 persists a container's entire raw create request verbatim
 * (including devices/interfaces/files/sysctls), so nothing here needs
 * its own separate per-feature export logic.
 *
 * Always succeeds (200) -- reading local state files cannot fail in a
 * way that should reject the request the way a bad squashfs/kernel
 * path can for /system/update; an individual unreadable recipe file
 * is skipped, not fatal to the whole bundle.
 */
static void do_system_backup(struct json_writer *w)
{
	char *buf;
	size_t len;
	DIR *d;
	struct dirent *de;

	jw_init(w);
	jw_obj_open(w);
	jw_key(w, "version");
	jw_int(w, 1);

	jw_key(w, "container_defs");
	if (persist_read_file(CONTAINER_DEFS_STATE_PATH, &buf, &len) == 0 && buf != NULL) {
		jw_str(w, buf);
		free(buf);
	} else {
		jw_str(w, "");
	}

	jw_key(w, "networks");
	if (persist_read_file(NETWORKS_STATE_PATH, &buf, &len) == 0 && buf != NULL) {
		jw_str(w, buf);
		free(buf);
	} else {
		jw_str(w, "");
	}

	jw_key(w, "dns_records");
	if (persist_read_file(DNS_RECORDS_STATE_PATH, &buf, &len) == 0 && buf != NULL) {
		jw_str(w, buf);
		free(buf);
	} else {
		jw_str(w, "");
	}

	jw_key(w, "pkg_installed");
	if (persist_read_file(PKG_INSTALLED_STATE_PATH, &buf, &len) == 0 && buf != NULL) {
		jw_str(w, buf);
		free(buf);
	} else {
		jw_str(w, "");
	}

	/* Every recipe on disk, not just currently-installed packages --
	 * they're cheap, and the operator may want them all preserved.
	 * Same opendir()/readdir()/".recipe" filtering shape pkg.c's own
	 * pkg_write_json_recipes() already uses. */
	jw_key(w, "pkg_recipes");
	jw_obj_open(w);
	d = opendir(PKG_RECIPES_DIR);
	if (d != NULL) {
		while ((de = readdir(d)) != NULL) {
			size_t nlen = strlen(de->d_name);
			char path[PATH_MAX];
			char name[256];

			if (nlen <= 7 || strcmp(de->d_name + nlen - 7, ".recipe") != 0)
				continue;
			snprintf(path, sizeof(path), "%s/recipes/%s", PKG_DIR, de->d_name);
			if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
				continue;
			snprintf(name, sizeof(name), "%.*s", (int)(nlen - 7), de->d_name);
			jw_key(w, name);
			jw_str(w, buf);
			free(buf);
		}
		closedir(d);
	}
	jw_obj_close(w);

	jw_obj_close(w);
}

static void handle_system_backup(int fd)
{
	struct json_writer w;

	do_system_backup(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Writes str (a JSON-string field's own already-validated content,
 * e.g. a whole container_defs.json) to path via the existing
 * persist_atomic_write() (daemon/src/persist.c) -- same atomic-
 * rewrite safety every other state file in this codebase already
 * gets, no new write primitive introduced for restore.
 */
static int restore_write_field(const char *path, const char *str)
{
	return persist_atomic_write(path, str, strlen(str));
}

/*
 * Pure (no fd) core of POST /v1/system/restore -- the reverse of
 * do_system_backup(): takes the identical bundle shape and writes each
 * field back to its own real path. Every embedded field is validated
 * BEFORE any file is written (container_defs/networks/dns_records/
 * pkg_installed must each themselves parse as valid JSON; pkg_recipes
 * must be an object of string values) -- one malformed field must
 * never leave the others half-applied, the same "validate everything,
 * then write" discipline do_system_update() already established.
 *
 * Deliberately does NOT reboot or hot-reload anything -- network_
 * init()/dns_init()/pkg_init()/containerdef_init() all run once at
 * daemon startup, so restored state only takes effect on the next
 * boot, via boot-time logic (containerdef_autostart_all() in
 * particular) that already exists and is already tested. This
 * function's whole job is "the files are now correct"; the operator
 * calls the pre-existing POST /system/reboot separately once ready,
 * the same "write, don't reboot, caller decides when to cut over"
 * shape /system/update already established (ADR-0031).
 *
 * Returns the HTTP status this operation resolves to (200 on
 * success); *out_errmsg is filled in only otherwise.
 */
/* True if v is a string whose own content is either empty or itself
 * valid JSON -- used to validate each of restore's JSON-file fields
 * before any of them are written. */
static int json_string_field_is_valid(const struct json_value *v)
{
	const char *s;
	struct json_value *check;

	s = json_as_string(v);
	if (s == NULL)
		return 0;
	if (s[0] == '\0')
		return 1;
	check = json_parse(s, strlen(s));
	if (check == NULL)
		return 0;
	json_free(check);
	return 1;
}

static int do_system_restore(const char *body, size_t body_len, char *out_errmsg,
                              size_t out_errmsg_size)
{
	struct json_value *root;
	const struct json_value *jcontainer_defs, *jnetworks, *jdns_records, *jpkg_installed;
	const struct json_value *jpkg_recipes;
	size_t i;
	int have_any = 0;

	root = json_parse(body, body_len);
	if (root == NULL) {
		snprintf(out_errmsg, out_errmsg_size, "invalid JSON body");
		return 400;
	}

	jcontainer_defs = json_object_get(root, "container_defs");
	jnetworks = json_object_get(root, "networks");
	jdns_records = json_object_get(root, "dns_records");
	jpkg_installed = json_object_get(root, "pkg_installed");
	jpkg_recipes = json_object_get(root, "pkg_recipes");

	/* Each present field must be a string whose own content is valid
	 * JSON -- checked for every field before any file is touched. */
	if (jcontainer_defs != NULL) {
		have_any = 1;
		if (!json_string_field_is_valid(jcontainer_defs)) {
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size, "container_defs is not valid JSON");
			return 400;
		}
	}
	if (jnetworks != NULL) {
		have_any = 1;
		if (!json_string_field_is_valid(jnetworks)) {
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size, "networks is not valid JSON");
			return 400;
		}
	}
	if (jdns_records != NULL) {
		have_any = 1;
		if (!json_string_field_is_valid(jdns_records)) {
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size, "dns_records is not valid JSON");
			return 400;
		}
	}
	if (jpkg_installed != NULL) {
		have_any = 1;
		if (!json_string_field_is_valid(jpkg_installed)) {
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size, "pkg_installed is not valid JSON");
			return 400;
		}
	}
	if (jpkg_recipes != NULL) {
		have_any = 1;
		if (jpkg_recipes->type != JSON_OBJECT) {
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size, "pkg_recipes must be an object");
			return 400;
		}
		for (i = 0; i < jpkg_recipes->u.object.count; i++) {
			if (json_as_string(jpkg_recipes->u.object.values[i]) == NULL) {
				json_free(root);
				snprintf(out_errmsg, out_errmsg_size,
				         "pkg_recipes.%s is not a string", jpkg_recipes->u.object.keys[i]);
				return 400;
			}
		}
	}

	if (!have_any) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size,
		         "at least one of container_defs/networks/dns_records/pkg_installed/pkg_recipes "
		         "required");
		return 400;
	}

	/* Every field validated -- now write. */
	if (jcontainer_defs != NULL &&
	    restore_write_field(CONTAINER_DEFS_STATE_PATH, json_as_string(jcontainer_defs)) != 0) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size, "failed to write container_defs.json");
		return 500;
	}
	if (jnetworks != NULL &&
	    restore_write_field(NETWORKS_STATE_PATH, json_as_string(jnetworks)) != 0) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size, "failed to write networks.json");
		return 500;
	}
	if (jdns_records != NULL &&
	    restore_write_field(DNS_RECORDS_STATE_PATH, json_as_string(jdns_records)) != 0) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size, "failed to write dns_records.json");
		return 500;
	}
	if (jpkg_installed != NULL &&
	    restore_write_field(PKG_INSTALLED_STATE_PATH, json_as_string(jpkg_installed)) != 0) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size, "failed to write pkg_installed.json");
		return 500;
	}
	if (jpkg_recipes != NULL) {
		if (persist_mkdir_p(PKG_RECIPES_DIR) != 0) {
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size, "failed to create recipes directory");
			return 500;
		}
		for (i = 0; i < jpkg_recipes->u.object.count; i++) {
			char path[PATH_MAX];

			snprintf(path, sizeof(path), "%s/%s.recipe", PKG_RECIPES_DIR,
			         jpkg_recipes->u.object.keys[i]);
			if (restore_write_field(path, json_as_string(jpkg_recipes->u.object.values[i])) !=
			    0) {
				json_free(root);
				snprintf(out_errmsg, out_errmsg_size, "failed to write recipe %s",
				         jpkg_recipes->u.object.keys[i]);
				return 500;
			}
		}
	}

	json_free(root);
	return 200;
}

static void handle_system_restore(int fd, const char *body, size_t body_len)
{
	char errmsg[256];
	int status;
	struct json_writer w;

	status = do_system_restore(body, body_len, errmsg, sizeof(errmsg));
	if (status != 200) {
		respond_error(fd, status, http_status_text(status), errmsg);
		return;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "status");
	jw_str(&w, "restored");
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "containers");
	registry_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_get_one(int fd, const char *name)
{
	struct registry_entry *e = registry_find(name);
	struct json_writer w;

	if (e == NULL) {
		/* Not live -- might still be a stopped-but-defined container
		 * (ADR-0045), same fallback GET /v1/containers' own list
		 * already makes via containerdef_write_json_stopped_list(). */
		jw_init(&w);
		if (containerdef_write_json_stopped_one(name, &w)) {
			respond_json(fd, 200, "OK", &w);
			jw_free(&w);
			return;
		}
		jw_free(&w);
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}
	jw_init(&w);
	registry_write_json_one(e, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void register_container_pidfd(struct registry_entry *entry)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		/*
		 * The container is already running with real resources
		 * committed; failing to register its pidfd would leave it
		 * permanently stuck reporting "running" even after it
		 * exits. Under this kind of memory pressure the daemon is
		 * already in a bad state, so fail loudly rather than carry
		 * a silently-wrong registry entry forever.
		 */
		perror("malloc (container reactor conn)");
		abort();
	}
	cc->kind = CONN_CONTAINER;
	cc->fd = entry->handle.pidfd;
	cc->entry = entry;
	entry->reactor_conn = cc;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD pidfd");
		abort();
	}
}

/*
 * Same shape as register_container_pidfd(), for a plain fork()'d
 * subprocess instead of a clone3()'d container -- Phase 10's package
 * fetch step (a `curl` subprocess) needs the exact same non-blocking
 * "tell me via epoll when this exits" treatment a container's own
 * CLONE_PIDFD-obtained pidfd already gets, via the explicit
 * sys_pidfd_open() equivalent for an already-forked pid.
 */
static void register_pkg_fetch_pidfd(pid_t pid, int pidfd)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (pkg fetch reactor conn)");
		abort();
	}
	cc->kind = CONN_PKG_FETCH;
	cc->fd = pidfd;
	cc->pkg_fetch_pid = pid;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD pkg fetch pidfd");
		abort();
	}
}

/*
 * A "networks" array entry is either a bare name (string, auto-
 * allocated IP -- unchanged v1 behavior) or an object
 * {"name":..., "ip":...} naming an explicit, operator-chosen address
 * instead. Returns 0 with name_out/*out_has_ip(/*out_ip_be) filled,
 * or -1 if item is neither shape, name is missing, or a present ip
 * doesn't parse as an IPv4 address.
 */
static int parse_network_entry(const struct json_value *item, char *name_out, size_t name_out_size,
                                uint32_t *out_ip_be, int *out_has_ip)
{
	const char *n;
	const char *ip_str;
	struct in_addr addr;

	*out_has_ip = 0;
	if (item->type == JSON_STRING) {
		n = json_as_string(item);
		if (n == NULL)
			return -1;
		snprintf(name_out, name_out_size, "%s", n);
		return 0;
	}
	if (item->type != JSON_OBJECT)
		return -1;
	n = json_as_string(json_object_get(item, "name"));
	if (n == NULL)
		return -1;
	snprintf(name_out, name_out_size, "%s", n);
	ip_str = json_as_string(json_object_get(item, "ip"));
	if (ip_str != NULL) {
		if (inet_pton(AF_INET, ip_str, &addr) != 1)
			return -1;
		*out_ip_be = addr.s_addr;
		*out_has_ip = 1;
	}
	return 0;
}

/*
 * Rejects a client-supplied container-relative path (POST /v1/containers'
 * own "files[].path") with any "." or ".." component, or an empty
 * component ("//" anywhere) -- the one place in the "files" feature
 * with real security weight, since a validated path becomes a real
 * host filesystem write target (see create_container_from_body()'s own
 * "files" staging). Caller has already checked path[0] == '/'.
 */
static int file_path_is_safe(const char *path)
{
	const char *p = path;

	while (*p == '/')
		p++;
	while (*p != '\0') {
		const char *start = p;
		size_t len;

		while (*p != '\0' && *p != '/')
			p++;
		len = (size_t)(p - start);
		if (len == 0 || (len == 1 && start[0] == '.') ||
		    (len == 2 && start[0] == '.' && start[1] == '.'))
			return 0;
		while (*p == '/')
			p++;
	}
	return 1;
}

/*
 * Deliberately restricted to the net.* sysctl tree (POST /v1/containers'
 * own "sysctls" object) -- most non-net.* sysctls (vm.*, fs.*, ...) are
 * not namespace-isolated at all in Linux, so allowing arbitrary keys
 * would make this a real host-wide write primitive from inside a
 * container, not a per-container knob. Requires "net." as the first
 * dot-separated component and every component non-empty and
 * [A-Za-z0-9_] only -- this also naturally rejects "net..foo" or
 * "net.ipv4.." (an empty component from what would otherwise become a
 * "/proc/sys/net/../.." traversal once '.' is translated to '/', see
 * container_net_apply_sysctl()).
 */
static int sysctl_key_is_safe(const char *key)
{
	const char *p = key;

	if (strncmp(p, "net.", 4) != 0)
		return 0;
	while (*p != '\0') {
		const char *start = p;

		while (*p != '\0' && *p != '.')
			p++;
		if (p == start)
			return 0;
		while (start < p) {
			if (!((*start >= 'a' && *start <= 'z') || (*start >= 'A' && *start <= 'Z') ||
			      (*start >= '0' && *start <= '9') || *start == '_'))
				return 0;
			start++;
		}
		if (*p == '.')
			p++;
	}
	return 1;
}

/*
 * Core of what POST /v1/containers does: parses+validates body,
 * builds a container_spec, calls registry_create(), calls
 * register_container_pidfd() on success (do NOT also call it at any
 * call site below -- double-registering the same pidfd with epoll is
 * a real bug, confirmed directly: it aborts the daemon), and runs the
 * requested dns_register/pki_issue side effects -- so replaying the
 * exact same body (boot autostart, crash restart, see
 * daemon/include/containerdef.h) gets the exact same result a fresh
 * POST would. Zero HTTP coupling. On success returns 0 and *out_entry
 * is the new registry entry; out_restart_policy/out_restart_delay_seconds/
 * out_depends_on(_count) are filled from the body's own "restart"/
 * "restart_delay_seconds"/"depends_on" fields regardless of outcome (a
 * caller deciding what to persist needs them either way, though only
 * a success is ever actually persisted). On
 * failure returns the same HTTP status code (400/409/500)
 * handle_create() has always returned for that condition, with
 * err_msg holding the exact same message text -- the REST wrapper
 * forwards both verbatim; boot/restart callers just log them.
 */
static int create_container_from_body(const char *body, size_t body_len,
                                       struct registry_entry **out_entry,
                                       char out_restart_policy[16], int *out_restart_delay_seconds,
                                       char out_depends_on[][REGISTRY_NAME_MAX],
                                       int *out_depends_on_count, int *out_has_readiness,
                                       int *out_readiness_tcp_port,
                                       int *out_readiness_timeout_seconds, char *err_msg,
                                       size_t err_msg_size)
{
	struct json_value *root;
	const struct json_value *jname, *jimage, *jcmd, *jmem, *jpids, *jnetworks, *jip_forward, *jroutes;
	const struct json_value *jdevices;
	const struct json_value *jinterfaces;
	const struct json_value *jfiles, *jsysctls;
	const struct json_value *jrestart, *jrestart_delay, *jdepends_on, *jreadiness;
	const char *restart_str;
	long restart_delay;
	const char *name, *image;
	char lowerdir[PATH_MAX];
	char container_base[PATH_MAX];
	char upperdir[PATH_MAX], workdir[PATH_MAX], merged[PATH_MAX];
	struct stat st;
	struct container_spec spec;
	struct registry_entry *entry;
	enum registry_error rerr;
	char *argv_buf[64];
	char *empty_envp[1];
	size_t argc, i;
	struct registry_network_attachment net_attachments[CONTAINER_MAX_NETWORKS];
	int net_count = 0;
	int ip_forward = 0;
	struct route_spec route_specs[CONTAINER_MAX_ROUTES];
	int route_count = 0;
	struct registry_device_attachment device_attachments[CONTAINER_MAX_DEVICES];
	struct device_spec device_specs[CONTAINER_MAX_DEVICES];
	int device_count = 0;
	char interface_names[CONTAINER_MAX_INTERFACES][CONTAINER_IFNAME_MAX];
	int interface_count = 0;
	char file_paths[CONTAINER_MAX_FILES][CONTAINER_FILE_PATH_MAX];
	int file_count = 0;
	struct container_sysctl sysctl_specs[CONTAINER_MAX_SYSCTLS];
	int sysctl_count = 0;
	const struct json_value *jdns_register;
	int dns_register = 0;
	const struct json_value *jpki_issue, *jpki_cert_dir, *jpki_days;
	int pki_issue = 0;
	char pki_cert_dir_buf[PATH_MAX];
	int pki_days = 365;

	snprintf(out_restart_policy, 16, "no");
	*out_restart_delay_seconds = CONTAINERDEF_DEFAULT_RESTART_DELAY_SECONDS;
	*out_depends_on_count = 0;
	*out_has_readiness = 0;

	root = json_parse(body, body_len);
	if (root == NULL) {
		snprintf(err_msg, err_msg_size, "invalid JSON body");
		return 400;
	}

	jname = json_object_get(root, "name");
	jimage = json_object_get(root, "image");
	jcmd = json_object_get(root, "cmd");
	jnetworks = json_object_get(root, "networks");
	jip_forward = json_object_get(root, "ip_forward");
	jroutes = json_object_get(root, "routes");
	jdevices = json_object_get(root, "devices");
	jinterfaces = json_object_get(root, "interfaces");
	jfiles = json_object_get(root, "files");
	jsysctls = json_object_get(root, "sysctls");
	jdns_register = json_object_get(root, "dns_register");
	jpki_issue = json_object_get(root, "pki_issue");
	jpki_cert_dir = json_object_get(root, "pki_cert_dir");
	jpki_days = json_object_get(root, "pki_days");
	name = json_as_string(jname);
	image = json_as_string(jimage);
	ip_forward = (jip_forward != NULL && jip_forward->type == JSON_BOOL && jip_forward->u.boolean);
	dns_register = (jdns_register != NULL && jdns_register->type == JSON_BOOL &&
	                jdns_register->u.boolean);
	pki_issue = (jpki_issue != NULL && jpki_issue->type == JSON_BOOL && jpki_issue->u.boolean);
	snprintf(pki_cert_dir_buf, sizeof(pki_cert_dir_buf), "%s",
	         json_as_string(jpki_cert_dir) != NULL ? json_as_string(jpki_cert_dir) :
	                                                  "/etc/kanxeo-tls");
	if (jpki_days != NULL)
		pki_days = (int)json_as_number(jpki_days);

	jrestart = json_object_get(root, "restart");
	restart_str = json_as_string(jrestart);
	if (restart_str != NULL && strcmp(restart_str, "always") != 0 &&
	    strcmp(restart_str, "on-failure") != 0 && strcmp(restart_str, "unless-stopped") != 0 &&
	    strcmp(restart_str, "no") != 0) {
		json_free(root);
		snprintf(err_msg, err_msg_size,
		         "restart must be \"always\", \"on-failure\", \"unless-stopped\", or \"no\"");
		return 400;
	}
	snprintf(out_restart_policy, 16, "%s", restart_str != NULL ? restart_str : "no");

	/*
	 * Given with restart:"no", ignored rather than rejected -- mirrors
	 * depends_on's own existing "ignored, not an error" precedent
	 * (nothing to persist, so no future restart to delay).
	 */
	jrestart_delay = json_object_get(root, "restart_delay_seconds");
	if (jrestart_delay != NULL) {
		restart_delay = (long)json_as_number(jrestart_delay);
		if (restart_delay < 1 || restart_delay > 300) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "restart_delay_seconds must be 1-300");
			return 400;
		}
		*out_restart_delay_seconds = (int)restart_delay;
	}

	jdepends_on = json_object_get(root, "depends_on");
	if (jdepends_on != NULL) {
		if (jdepends_on->type != JSON_ARRAY ||
		    jdepends_on->u.array.count > CONTAINERDEF_MAX_DEPENDS) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "depends_on must be an array of at most %d entries",
			         CONTAINERDEF_MAX_DEPENDS);
			return 400;
		}
		*out_depends_on_count = (int)jdepends_on->u.array.count;
		for (i = 0; i < (size_t)*out_depends_on_count; i++) {
			const char *dep = json_as_string(jdepends_on->u.array.items[i]);

			if (dep == NULL || !name_is_valid(dep)) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "invalid depends_on entry");
				return 400;
			}
			snprintf(out_depends_on[i], REGISTRY_NAME_MAX, "%s", dep);
		}
	}

	jreadiness = json_object_get(root, "readiness");
	if (jreadiness != NULL) {
		const struct json_value *jport, *jtimeout;
		long port, timeout;

		if (jreadiness->type != JSON_OBJECT) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "readiness must be an object");
			return 400;
		}
		jport = json_object_get(jreadiness, "tcp_port");
		port = jport != NULL ? (long)json_as_number(jport) : 0;
		if (jport == NULL || port < 1 || port > 65535) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "readiness.tcp_port must be 1-65535");
			return 400;
		}
		jtimeout = json_object_get(jreadiness, "timeout_seconds");
		timeout = jtimeout != NULL ? (long)json_as_number(jtimeout) : 30;
		if (timeout < 1 || timeout > 300) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "readiness.timeout_seconds must be 1-300");
			return 400;
		}
		/* jnetworks itself already guarantees >= 1 entry if non-NULL
		 * (its own validation below rejects an empty array), so this
		 * check doesn't need net_count, which isn't computed until the
		 * second networks pass, further down. */
		if (jnetworks == NULL) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "readiness requires networks");
			return 400;
		}
		*out_has_readiness = 1;
		*out_readiness_tcp_port = (int)port;
		*out_readiness_timeout_seconds = (int)timeout;
	}

	if (!name_is_valid(name) || image == NULL || image[0] == '\0' || jcmd == NULL ||
	    jcmd->type != JSON_ARRAY || jcmd->u.array.count == 0 ||
	    jcmd->u.array.count >= (sizeof(argv_buf) / sizeof(argv_buf[0]))) {
		json_free(root);
		snprintf(err_msg, err_msg_size, "name/image/cmd missing or invalid");
		return 400;
	}
	if (jnetworks != NULL) {
		if (jnetworks->type != JSON_ARRAY || jnetworks->u.array.count == 0 ||
		    jnetworks->u.array.count > CONTAINER_MAX_NETWORKS) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "networks must be a non-empty array of at most 64 entries");
			return 400;
		}
		for (i = 0; i < jnetworks->u.array.count; i++) {
			char n[NETWORK_NAME_MAX];
			uint32_t ip_be;
			int has_ip;

			if (parse_network_entry(jnetworks->u.array.items[i], n, sizeof(n), &ip_be,
			                         &has_ip) != 0) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "invalid networks entry");
				return 400;
			}
			if (network_find(n) == NULL) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "unknown network");
				return 400;
			}
			if (has_ip) {
				enum network_error ip_err = network_ip_available(n, ip_be);

				if (ip_err != NETWORK_OK) {
					const char *ip_msg;
					int ip_status = network_error_to_status(ip_err, &ip_msg);

					json_free(root);
					snprintf(err_msg, err_msg_size, "%s", ip_msg);
					return ip_status;
				}
			}
		}
	}
	if (dns_register && jnetworks == NULL) {
		json_free(root);
		snprintf(err_msg, err_msg_size, "dns_register requires networks");
		return 400;
	}
	if (pki_issue && !pki_ca_bootstrapped()) {
		json_free(root);
		snprintf(err_msg, err_msg_size, "pki_issue requires the CA to be bootstrapped -- POST /v1/pki/ca first");
		return 400;
	}
	if (jroutes != NULL) {
		if (jroutes->type != JSON_ARRAY || jroutes->u.array.count > CONTAINER_MAX_ROUTES) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "routes must be an array of at most 8 entries");
			return 400;
		}
		route_count = (int)jroutes->u.array.count;
		for (i = 0; i < (size_t)route_count; i++) {
			const struct json_value *item = jroutes->u.array.items[i];
			const char *dest = json_as_string(json_object_get(item, "dest"));
			const char *via = json_as_string(json_object_get(item, "via"));
			const struct json_value *jprefix = json_object_get(item, "prefix_len");
			struct in_addr dest_addr, via_addr;
			long prefix_len;

			if (dest == NULL || via == NULL || jprefix == NULL ||
			    inet_pton(AF_INET, dest, &dest_addr) != 1 ||
			    inet_pton(AF_INET, via, &via_addr) != 1) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "invalid routes entry");
				return 400;
			}
			prefix_len = (long)json_as_number(jprefix);
			if (prefix_len < 0 || prefix_len > 32) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "routes prefix_len must be 0-32");
				return 400;
			}
			route_specs[i].dest_be = dest_addr.s_addr;
			route_specs[i].dest_prefix_len = (int)prefix_len;
			route_specs[i].gateway_be = via_addr.s_addr;
		}
	}
	if (jdevices != NULL) {
		if (jdevices->type != JSON_ARRAY || jdevices->u.array.count > CONTAINER_MAX_DEVICES) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "devices must be an array of at most 16 entries");
			return 400;
		}
		/*
		 * device_count is the WRITE index here, decoupled from the
		 * request array's own read index i -- a single grouped id
		 * (e.g. "gpu:0", ADR-0028) can expand into several grants via
		 * device_find_group(), so one requested entry doesn't
		 * necessarily mean one device_specs slot. The array-length
		 * check above is only a coarse upfront guard (an array literally
		 * longer than the cap can never fit even unexpanded); the real
		 * bound is enforced incrementally below as groups expand.
		 */
		for (i = 0; i < jdevices->u.array.count; i++) {
			const char *id = json_as_string(jdevices->u.array.items[i]);
			const struct discovered_device *matches[CONTAINER_MAX_DEVICES];
			int n, j;

			if (id == NULL) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "devices entries must be strings");
				return 400;
			}
			n = device_find_group(id, matches, CONTAINER_MAX_DEVICES - device_count);
			if (n <= 0) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "unknown or unassignable device");
				return 400;
			}
			if (device_count + n > CONTAINER_MAX_DEVICES) {
				json_free(root);
				snprintf(err_msg, err_msg_size,
				         "too many devices requested (a grouped id can expand into "
				         "more than one grant)");
				return 400;
			}
			for (j = 0; j < n; j++) {
				const struct discovered_device *dd = matches[j];

				/* dev_path/major/minor always come from the daemon's
				 * own current sysfs snapshot (dd), never trusted from
				 * the request body -- a client only ever names a
				 * device by id. */
				memset(&device_specs[device_count], 0, sizeof(device_specs[device_count]));
				device_specs[device_count].type = dd->type;
				device_specs[device_count].major = dd->major;
				device_specs[device_count].minor = dd->minor;
				snprintf(device_specs[device_count].dev_path,
				         sizeof(device_specs[device_count].dev_path), "%s", dd->dev_path);
				memset(&device_attachments[device_count], 0,
				       sizeof(device_attachments[device_count]));
				snprintf(device_attachments[device_count].id,
				         sizeof(device_attachments[device_count].id), "%s", dd->id);
				snprintf(device_attachments[device_count].dev_path,
				         sizeof(device_attachments[device_count].dev_path), "%s",
				         dd->dev_path);
				device_count++;
			}
		}
	}
	if (jinterfaces != NULL) {
		if (jinterfaces->type != JSON_ARRAY || jinterfaces->u.array.count > CONTAINER_MAX_INTERFACES) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "interfaces must be an array of at most 16 entries");
			return 400;
		}
		interface_count = (int)jinterfaces->u.array.count;
		for (i = 0; i < (size_t)interface_count; i++) {
			const char *ifname = json_as_string(jinterfaces->u.array.items[i]);
			char dev_id[96];
			const struct discovered_device *dd;

			if (ifname == NULL) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "interfaces entries must be strings");
				return 400;
			}
			/* GET /v1/devices is the one source of truth for which real
			 * interfaces exist and are currently assignable -- an
			 * interface already moved into another running container's
			 * netns simply doesn't appear there at all anymore, so
			 * there is no separate "already claimed" check needed here
			 * beyond this same lookup every other device grant uses. */
			snprintf(dev_id, sizeof(dev_id), "net:%s", ifname);
			dd = device_find(dev_id);
			if (dd == NULL || !dd->assignable) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "unknown or unassignable interface");
				return 400;
			}
			snprintf(interface_names[i], sizeof(interface_names[i]), "%s", ifname);
		}
	}
	if (jfiles != NULL) {
		if (jfiles->type != JSON_ARRAY || jfiles->u.array.count > CONTAINER_MAX_FILES) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "files must be an array of at most 16 entries");
			return 400;
		}
		for (i = 0; i < jfiles->u.array.count; i++) {
			const struct json_value *item = jfiles->u.array.items[i];
			const char *path = json_as_string(json_object_get(item, "path"));
			const char *content = json_as_string(json_object_get(item, "content"));
			const char *mode_str = json_as_string(json_object_get(item, "mode"));
			long mode;

			if (path == NULL || content == NULL || path[0] != '/' ||
			    strlen(path) >= CONTAINER_FILE_PATH_MAX ||
			    strlen(content) > CONTAINER_FILE_CONTENT_MAX || !file_path_is_safe(path)) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "invalid files entry");
				return 400;
			}
			mode = mode_str != NULL ? strtol(mode_str, NULL, 8) : 0644;
			if (mode < 0 || mode > 0777) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "files mode must be 0-0777 octal");
				return 400;
			}
		}
	}
	if (jsysctls != NULL) {
		if (jsysctls->type != JSON_OBJECT || jsysctls->u.object.count > CONTAINER_MAX_SYSCTLS) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "sysctls must be an object of at most 32 entries");
			return 400;
		}
		sysctl_count = (int)jsysctls->u.object.count;
		for (i = 0; i < (size_t)sysctl_count; i++) {
			const char *key = jsysctls->u.object.keys[i];
			const char *value = json_as_string(jsysctls->u.object.values[i]);

			if (value == NULL || strlen(key) >= CONTAINER_SYSCTL_KEY_MAX ||
			    strlen(value) >= CONTAINER_SYSCTL_VALUE_MAX || !sysctl_key_is_safe(key)) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "invalid sysctls entry");
				return 400;
			}
			snprintf(sysctl_specs[i].key, sizeof(sysctl_specs[i].key), "%s", key);
			snprintf(sysctl_specs[i].value, sizeof(sysctl_specs[i].value), "%s", value);
		}
	}

	argc = jcmd->u.array.count;
	for (i = 0; i < argc; i++) {
		const char *s = json_as_string(jcmd->u.array.items[i]);

		if (s == NULL) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "cmd must be an array of strings");
			return 400;
		}
		argv_buf[i] = (char *)s;
	}
	argv_buf[argc] = NULL;
	empty_envp[0] = NULL;

	snprintf(lowerdir, sizeof(lowerdir), "%s/%s/rootfs", IMAGES_DIR, image);
	if (stat(lowerdir, &st) != 0) {
		json_free(root);
		snprintf(err_msg, err_msg_size, "image rootfs does not exist");
		return 400;
	}

	if (registry_find(name) != NULL) {
		json_free(root);
		snprintf(err_msg, err_msg_size, "a container with this name already exists");
		return 409;
	}

	if (jnetworks != NULL) {
		net_count = (int)jnetworks->u.array.count;
		for (i = 0; i < (size_t)net_count; i++) {
			char n[NETWORK_NAME_MAX];
			uint32_t ip_be;
			int has_ip;

			/* Already validated above (name exists; a given ip is
			 * in-range and free) -- re-parsed only to recover the
			 * values, no new failure mode expected here. */
			parse_network_entry(jnetworks->u.array.items[i], n, sizeof(n), &ip_be, &has_ip);
			if (!has_ip && network_alloc_ip(n, &ip_be) != 0) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "no free IP addresses");
				return 500;
			}
			memset(net_attachments[i].name, 0, sizeof(net_attachments[i].name));
			strncpy(net_attachments[i].name, n, sizeof(net_attachments[i].name) - 1);
			net_attachments[i].ip_be = ip_be;
		}
	}

	snprintf(container_base, sizeof(container_base), "%s/%s", CONTAINERS_DIR, name);
	if (mkdir(container_base, 0755) != 0 && errno != EEXIST) {
		json_free(root);
		snprintf(err_msg, err_msg_size, "failed to create container directory");
		return 500;
	}
	snprintf(upperdir, sizeof(upperdir), "%s/upper", container_base);
	snprintf(workdir, sizeof(workdir), "%s/work", container_base);
	snprintf(merged, sizeof(merged), "%s/merged", container_base);

	/*
	 * Written directly into the container's own upperdir, entirely on
	 * this (daemon/host) process, before registry_create()/clone3() is
	 * ever called -- overlay_create() (child-side, post-clone3())
	 * mkdir()s upperdir EEXIST-tolerant and mounts it unchanged, so
	 * these files are simply already there the moment the container's
	 * own process execve()s, unlike a post-hoc write into an already-
	 * running container's filesystem (ADR-0013's /proc/<pid>/root/
	 * pattern), which needs a live pid this early creation path doesn't
	 * have yet. Already fully validated above (path safety, content/
	 * mode bounds) -- this pass only does I/O.
	 */
	if (jfiles != NULL) {
		for (i = 0; i < jfiles->u.array.count; i++) {
			const struct json_value *item = jfiles->u.array.items[i];
			const char *path = json_as_string(json_object_get(item, "path"));
			const char *content = json_as_string(json_object_get(item, "content"));
			const char *mode_str = json_as_string(json_object_get(item, "mode"));
			long mode = mode_str != NULL ? strtol(mode_str, NULL, 8) : 0644;
			size_t content_len = strlen(content);
			char target[PATH_MAX];
			char target_dir[PATH_MAX];
			char *slash;
			int fd;

			if (snprintf(target, sizeof(target), "%s%s", upperdir, path) >=
			    (int)sizeof(target)) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "files path too long");
				return 500;
			}
			snprintf(target_dir, sizeof(target_dir), "%s", target);
			slash = strrchr(target_dir, '/');
			if (slash != NULL)
				*slash = '\0';
			if (persist_mkdir_p(target_dir) != 0) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "failed to stage files");
				return 500;
			}
			fd = open(target, O_CREAT | O_TRUNC | O_WRONLY, (mode_t)mode);
			if (fd < 0 ||
			    (content_len > 0 && write(fd, content, content_len) != (ssize_t)content_len)) {
				if (fd >= 0)
					close(fd);
				json_free(root);
				snprintf(err_msg, err_msg_size, "failed to stage files");
				return 500;
			}
			close(fd);
			snprintf(file_paths[i], sizeof(file_paths[i]), "%s", path);
		}
		file_count = (int)jfiles->u.array.count;
	}

	memset(&spec, 0, sizeof(spec));
	spec.ns.clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET |
	                       CLONE_NEWCGROUP | CLONE_INTO_CGROUP;
	spec.ns.hostname = name;
	spec.cg.name = name;
	jmem = json_object_get(root, "memory_max");
	spec.cg.memory_max = jmem != NULL ? (long long)json_as_number(jmem) : 0;
	jpids = json_object_get(root, "pids_max");
	spec.cg.pids_max = jpids != NULL ? (long long)json_as_number(jpids) : 0;
	spec.cg.cpu_max = NULL;
	spec.ov.lowerdir = lowerdir;
	spec.ov.upperdir = upperdir;
	spec.ov.workdir = workdir;
	spec.ov.merged = merged;
	spec.mnt.put_old_rel = ".old_root";
	spec.net_count = net_count;
	for (i = 0; i < (size_t)net_count; i++) {
		struct network_def *net = network_find(net_attachments[i].name);

		spec.nets[i].bridge = net->name;
		spec.nets[i].container_ip_be = net_attachments[i].ip_be;
		spec.nets[i].has_gateway = net->has_gateway;
		spec.nets[i].gateway_ip_be = net->gateway_be;
		spec.nets[i].prefix_len = net->prefix_len;
	}
	spec.ip_forward = ip_forward;
	spec.sysctl_count = sysctl_count;
	for (i = 0; i < (size_t)sysctl_count; i++)
		spec.sysctls[i] = sysctl_specs[i];
	spec.route_count = route_count;
	for (i = 0; i < (size_t)route_count; i++)
		spec.routes[i] = route_specs[i];
	spec.device_count = device_count;
	for (i = 0; i < (size_t)device_count; i++)
		spec.devices[i] = device_specs[i];
	spec.interface_count = interface_count;
	for (i = 0; i < (size_t)interface_count; i++)
		snprintf(spec.interfaces[i], sizeof(spec.interfaces[i]), "%s", interface_names[i]);
	spec.argv = argv_buf;
	spec.envp = empty_envp;

	rerr = registry_create(name, image, &spec, net_attachments, net_count, ip_forward,
	                        device_attachments, device_count, file_paths, file_count, &entry);
	/*
	 * Safe to free the JSON tree now even though spec.ns.hostname,
	 * spec.cg.name and spec.argv[] point into it: registry_create()
	 * has already returned, meaning container_create()'s clone3() has
	 * already happened. From that point on the child is a fully
	 * independent process with its own copy-on-write view of this
	 * memory -- nothing this process does to it afterward (including
	 * freeing it) is visible to the child, by the basic guarantee of
	 * copy-on-write.
	 */
	json_free(root);

	if (rerr == REGISTRY_ERR_DUPLICATE) {
		snprintf(err_msg, err_msg_size, "a container with this name already exists");
		return 409;
	}
	if (rerr == REGISTRY_ERR_FULL) {
		snprintf(err_msg, err_msg_size, "container table full");
		return 500;
	}
	if (rerr == REGISTRY_ERR_CREATE_FAILED) {
		snprintf(err_msg, err_msg_size, "failed to create container");
		return 500;
	}

	register_container_pidfd(entry);

	if (dns_register) {
		/* entry->name, not the local `name`, which pointed into
		 * root and is no longer valid after json_free() above. */
		struct dns_record *rec;
		enum dns_error derr = dns_record_create(entry->name, spec.nets[0].container_ip_be,
		                                         entry->name, &rec);

		if (derr != DNS_OK)
			fprintf(stderr,
			        "%s: dns_register requested but auto-registration failed (err=%d)\n",
			        entry->name, (int)derr);
	}

	if (pki_issue) {
		/* CN/SAN = the container's own name, matching pki_cert_create()'s
		 * existing manual-call default-SAN-to-name behavior. No IP SAN --
		 * pki_issue doesn't require networks, unlike dns_register, since
		 * delivery via /proc/<pid>/root/ works for any running container
		 * regardless of networking. */
		const char *pki_sans[1];
		struct json_writer scratch;
		enum pki_error perr;

		pki_sans[0] = entry->name;
		jw_init(&scratch);
		perr = pki_cert_create(entry->name, pki_sans, 1, pki_days, entry->name, &scratch);
		jw_free(&scratch);

		/* PKI_ERR_DUPLICATE means a cert for this name already exists --
		 * expected and harmless on every restart-always respawn after the
		 * first (each respawn gets a fresh pid, so delivery still needs to
		 * run again even though creation itself is a no-op the second time
		 * onward). Confirmed as a real, not hypothetical, failure mode:
		 * without this, a container whose process starts and reads its own
		 * TLS cert before pki_cert_deliver() finishes writing it (a real
		 * race -- register_container_pidfd() above runs before this whole
		 * block) would crash-loop forever under restart-always, since every
		 * respawn after the first hit DUPLICATE and never reached delivery
		 * at all. Any other error still aborts -- a genuinely failed
		 * create (OPENSSL_FAILED, PERSIST_FAILED, ...) has no existing cert
		 * to fall back to delivering. */
		if (perr != PKI_OK && perr != PKI_ERR_DUPLICATE) {
			fprintf(stderr,
			        "%s: pki_issue requested but cert issuance failed (err=%d)\n",
			        entry->name, (int)perr);
		} else {
			enum pki_error derr2 =
			    pki_cert_deliver(entry->name, entry->handle.pid, pki_cert_dir_buf);

			if (derr2 != PKI_OK)
				fprintf(stderr,
				        "%s: pki_issue cert issued but delivery into the container failed (err=%d)\n",
				        entry->name, (int)derr2);
		}
	}

	*out_entry = entry;
	return 0;
}

static void handle_create(int fd, const char *body, size_t body_len)
{
	struct registry_entry *entry;
	char restart_policy[16];
	int restart_delay_seconds;
	char depends_on[CONTAINERDEF_MAX_DEPENDS][REGISTRY_NAME_MAX];
	int depends_on_count;
	int has_readiness, readiness_tcp_port, readiness_timeout_seconds;
	char err_msg[256];
	int status;
	struct json_writer w;

	status = create_container_from_body(body, body_len, &entry, restart_policy,
	                                     &restart_delay_seconds, depends_on, &depends_on_count,
	                                     &has_readiness, &readiness_tcp_port,
	                                     &readiness_timeout_seconds, err_msg, sizeof(err_msg));
	if (status != 0) {
		respond_error(fd, status, http_status_text(status), err_msg);
		return;
	}

	if (strcmp(restart_policy, "no") != 0 &&
	    containerdef_add(entry->name, body, body_len, depends_on, depends_on_count, has_readiness,
	                      readiness_tcp_port, readiness_timeout_seconds, restart_policy,
	                      restart_delay_seconds) != 0) {
		fprintf(stderr,
		        "%s: restart:\"%s\" requested but persisting its definition failed -- "
		        "it will not survive a daemon restart\n",
		        entry->name, restart_policy);
	}

	jw_init(&w);
	registry_write_json_one(entry, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_delete(int fd, const char *name)
{
	struct registry_entry *e = registry_find(name);
	struct conn *cc;

	/*
	 * A restart:"always" definition that has never once managed to
	 * autostart (a cycle, an unknown dependency, an image that no
	 * longer exists, ...) has no live registry entry at all -- without
	 * this check, DELETE could never reach it, permanently stranding a
	 * broken definition with no way to remove it short of hand-editing
	 * container_defs.json on disk. Found directly: test_container_restart.c's
	 * own cleanup of exactly this case (a container skipped for a
	 * circular/unknown dependency) silently 404'd, leaking its
	 * definition into every subsequently-run test on this host.
	 */
	if (e == NULL && containerdef_find(name) == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}

	if (e != NULL) {
		if (e->reactor_conn != NULL) {
			cc = e->reactor_conn;
			kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
			free(cc);
			e->reactor_conn = NULL;
		}

		registry_remove(name);
		dns_server_forget(name);
		dns_record_forget_owner(name);
		pki_cert_forget_owner(name);
	}
	/*
	 * Unconditional, a no-op if this name never had a restart:"always"
	 * definition. DELETE always means gone for good: not on this
	 * crash-restart timer (none pending, since this path never goes
	 * through handle_container_event()) and not on the next daemon
	 * restart.
	 */
	containerdef_remove(name);
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * POST /v1/containers/{name}/start (ADR-0045) -- the counterpart to
 * .../stop: brings a stopped-but-still-defined container back to life
 * without a daemon restart, closing the gap .../stop's own comment
 * above alludes to (what "stopped" means going forward was previously
 * "wait for containerdef_autostart_all() at the next daemon restart,"
 * and only for restart:"always"/"on-failure" -- "unless-stopped" or
 * "no" had no way back at all short of DELETE + a fresh POST, which
 * itself 409s on a name that already has a persisted definition).
 *
 * Idempotent like .../stop: already-live is a plain 200, not an
 * error. A name with no persisted definition at all is 404 -- this
 * endpoint only ever replays an existing definition, it does not
 * accept a body and create a new one (that's POST /v1/containers).
 * Reuses create_container_from_body() verbatim on the definition's own
 * stored body -- the exact same replay containerdef_autostart_all()
 * already does at boot (main(), ~line 4841) -- one source of truth for
 * "how a definition becomes a live container," not a second bespoke
 * path. Unlike autostart, this endpoint is explicit and manual, so it
 * intentionally ignores restart_policy/"unless-stopped" gating
 * entirely: an operator asking to start a container by name always
 * means start it, regardless of what a *crash* would have done.
 */
static void handle_start(int fd, const char *name)
{
	struct registry_entry *entry;
	struct container_def *def;
	char restart_policy[16];
	int restart_delay_seconds;
	char depends_on[CONTAINERDEF_MAX_DEPENDS][REGISTRY_NAME_MAX];
	int depends_on_count;
	int has_readiness, readiness_tcp_port, readiness_timeout_seconds;
	char err_msg[256];
	int status;
	struct json_writer w;

	entry = registry_find(name);
	if (entry != NULL) {
		jw_init(&w);
		registry_write_json_one(entry, &w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
		return;
	}

	def = containerdef_find(name);
	if (def == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}

	status = create_container_from_body(def->body, def->body_len, &entry, restart_policy,
	                                     &restart_delay_seconds, depends_on, &depends_on_count,
	                                     &has_readiness, &readiness_tcp_port,
	                                     &readiness_timeout_seconds, err_msg, sizeof(err_msg));
	if (status != 0) {
		respond_error(fd, status, http_status_text(status), err_msg);
		return;
	}

	containerdef_set_stopped(name, 0);

	jw_init(&w);
	registry_write_json_one(entry, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * POST /v1/containers/{name}/pause and .../unpause (ADR-0045): real
 * cgroup v2 freezer control, not SIGSTOP -- freezing stops every task
 * in the cgroup at the kernel level, uninterceptable/unignorable by
 * the frozen process, unlike SIGSTOP which a process can catch or
 * handle. Both require an already-live registry entry (404 otherwise
 * -- pausing a stopped-but-defined or nonexistent container makes no
 * sense, unlike start/stop which are meaningfully defined over
 * persisted definitions too). Unlike start/stop's idempotent-200
 * double-call tolerance, a double-pause or double-unpause is a 409:
 * "is this container already paused" is state a caller should already
 * know from its last GET, and silently no-opping it could mask a real
 * caller bug (e.g. two racing pause requests) that stop/start's own
 * idempotency never has to worry about hiding.
 */
static void handle_pause(int fd, const char *name)
{
	struct registry_entry *e = registry_find(name);
	struct json_writer w;

	if (e == NULL) {
		respond_error(fd, 404, "Not Found", "no such running container");
		return;
	}
	if (e->paused) {
		respond_error(fd, 409, "Conflict", "already paused");
		return;
	}
	if (registry_set_paused(e, 1) != 0) {
		respond_error(fd, 500, "Internal Server Error", "cgroup freeze failed");
		return;
	}

	jw_init(&w);
	registry_write_json_one(e, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_unpause(int fd, const char *name)
{
	struct registry_entry *e = registry_find(name);
	struct json_writer w;

	if (e == NULL) {
		respond_error(fd, 404, "Not Found", "no such running container");
		return;
	}
	if (!e->paused) {
		respond_error(fd, 409, "Conflict", "not paused");
		return;
	}
	if (registry_set_paused(e, 0) != 0) {
		respond_error(fd, 500, "Internal Server Error", "cgroup thaw failed");
		return;
	}

	jw_init(&w);
	registry_write_json_one(e, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * POST /v1/containers/{name}/stop (ADR-0027) -- kills a live container
 * right now, without removing its persisted definition (unlike
 * DELETE): no containerdef_remove(), and deliberately no dns_server_
 * forget()/dns_record_forget_owner()/pki_cert_forget_owner() either --
 * this name's DNS/PKI ownership survives a stop, unlike a delete.
 * Reuses registry_remove() verbatim (already does SIGKILL + a
 * synchronous reap) -- no new kill/reap primitive, no asymmetry with
 * DELETE's own semantics. Applies uniformly regardless of restart
 * policy (kills any live container; containerdef_set_stopped() is a
 * harmless no-op for "no"/absent definitions) -- what stopped actually
 * *means* going forward is entirely up to the two sites that consult
 * it (containerdef_autostart_all(), handle_restart_timer_event()), not
 * this handler. Idempotent: calling twice is both 200.
 */
static void handle_stop(int fd, const char *name)
{
	struct registry_entry *e = registry_find(name);
	struct conn *cc;
	struct json_writer w;

	if (e == NULL && containerdef_find(name) == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}

	if (e != NULL) {
		if (e->reactor_conn != NULL) {
			cc = e->reactor_conn;
			kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
			free(cc);
			e->reactor_conn = NULL;
		}
		registry_remove(name);
	}
	containerdef_set_stopped(name, 1);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "status");
	jw_str(&w, "stopped");
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_device_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "devices");
	device_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_network_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name, *subnet, *gateway;
	const struct json_value *jprefix;
	int prefix_len;
	struct network_def *net;
	enum network_error nerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	name = json_as_string(json_object_get(root, "name"));
	subnet = json_as_string(json_object_get(root, "subnet"));
	jprefix = json_object_get(root, "prefix_len");
	gateway = json_as_string(json_object_get(root, "gateway")); /* optional; NULL = no gateway */

	if (name == NULL || subnet == NULL || jprefix == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name/subnet/prefix_len missing");
		return;
	}
	prefix_len = (int)json_as_number(jprefix);

	nerr = network_create(name, subnet, prefix_len, gateway, &net);
	json_free(root);

	if (nerr != NETWORK_OK) {
		respond_network_error(fd, nerr);
		return;
	}

	jw_init(&w);
	network_write_json_one(net, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_network_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "networks");
	network_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_network_get_one(int fd, const char *name)
{
	struct network_def *net = network_find(name);
	struct json_writer w;

	if (net == NULL) {
		respond_error(fd, 404, "Not Found", "no such network");
		return;
	}
	jw_init(&w);
	network_write_json_one(net, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_network_delete(int fd, const char *name)
{
	enum network_error nerr = network_delete(name);

	if (nerr != NETWORK_OK) {
		respond_network_error(fd, nerr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_network_attach_interface(int fd, const char *net_name, const char *body,
                                             size_t body_len)
{
	struct json_value *root;
	const char *ifname;
	const struct json_value *jvlan;
	int vlan_id;
	struct network_def *net;
	enum network_error nerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	ifname = json_as_string(json_object_get(root, "ifname"));
	jvlan = json_object_get(root, "vlan_id"); /* optional; absent/0 = untagged */
	vlan_id = jvlan != NULL ? (int)json_as_number(jvlan) : 0;

	if (ifname == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "ifname missing");
		return;
	}
	if (vlan_id < 0 || vlan_id > 4094) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "vlan_id must be in [1,4094], or omitted/0 for untagged");
		return;
	}

	nerr = network_attach_interface(net_name, ifname, vlan_id);
	json_free(root);

	if (nerr != NETWORK_OK) {
		respond_network_error(fd, nerr);
		return;
	}

	net = network_find(net_name);
	jw_init(&w);
	network_write_json_one(net, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_network_detach_interface(int fd, const char *net_name, const char *ifname)
{
	enum network_error nerr = network_detach_interface(net_name, ifname);

	if (nerr != NETWORK_OK) {
		respond_network_error(fd, nerr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void respond_image_error(int fd, enum image_error err)
{
	switch (err) {
	case IMAGE_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid image name");
		break;
	case IMAGE_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "an image with this name already exists");
		break;
	case IMAGE_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such image");
		break;
	case IMAGE_ERR_PROTECTED:
		respond_error(fd, 400, "Bad Request", "the base image cannot be removed");
		break;
	case IMAGE_ERR_IN_USE:
		respond_error(fd, 409, "Conflict", "image is still referenced by a running container");
		break;
	case IMAGE_ERR_HAS_PACKAGES:
		respond_error(fd, 409, "Conflict", "image still has packages installed -- remove them first");
		break;
	case IMAGE_ERR_CREATE_FAILED:
	case IMAGE_ERR_DELETE_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "image operation failed");
		break;
	}
}

static void handle_image_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name;
	enum image_error ierr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	name = json_as_string(json_object_get(root, "name"));
	if (name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name missing");
		return;
	}

	ierr = image_create(name);
	if (ierr != IMAGE_OK) {
		json_free(root);
		respond_image_error(fd, ierr);
		return;
	}

	jw_init(&w);
	image_write_json_one(name, &w);
	json_free(root);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_image_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "images");
	image_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_image_get_one(int fd, const char *name)
{
	struct json_writer w;
	enum image_error ierr;

	jw_init(&w);
	ierr = image_write_json_one(name, &w);
	if (ierr != IMAGE_OK) {
		jw_free(&w);
		respond_image_error(fd, ierr);
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_image_delete(int fd, const char *name)
{
	enum image_error ierr = image_delete(name);

	if (ierr != IMAGE_OK) {
		respond_image_error(fd, ierr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void respond_dns_error(int fd, enum dns_error err)
{
	switch (err) {
	case DNS_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid DNS record name");
		break;
	case DNS_ERR_INVALID_IP:
		respond_error(fd, 400, "Bad Request", "invalid ip");
		break;
	case DNS_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "a record with this name already exists");
		break;
	case DNS_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "DNS record table full");
		break;
	case DNS_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such DNS record");
		break;
	case DNS_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "DNS record operation failed");
		break;
	}
}

static void handle_dns_record_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name, *ip;
	struct in_addr addr;
	struct dns_record *rec;
	enum dns_error derr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	name = json_as_string(json_object_get(root, "name"));
	ip = json_as_string(json_object_get(root, "ip"));

	if (name == NULL || ip == NULL || inet_pton(AF_INET, ip, &addr) != 1) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name/ip missing or invalid");
		return;
	}

	derr = dns_record_create(name, addr.s_addr, NULL, &rec);
	json_free(root);

	if (derr != DNS_OK) {
		respond_dns_error(fd, derr);
		return;
	}

	jw_init(&w);
	dns_write_json_one(rec, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_dns_record_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "records");
	dns_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_dns_record_get_one(int fd, const char *name)
{
	struct dns_record *rec = dns_record_find(name);
	struct json_writer w;

	if (rec == NULL) {
		respond_error(fd, 404, "Not Found", "no such DNS record");
		return;
	}
	jw_init(&w);
	dns_write_json_one(rec, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_dns_record_delete(int fd, const char *name)
{
	enum dns_error derr = dns_record_delete(name);

	if (derr != DNS_OK) {
		respond_dns_error(fd, derr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void respond_dns_server_error(int fd, enum dns_server_error err)
{
	switch (err) {
	case DNS_SERVER_ERR_INVALID_PATH:
		respond_error(fd, 400, "Bad Request", "invalid hosts_path");
		break;
	case DNS_SERVER_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "this container is already registered");
		break;
	case DNS_SERVER_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "DNS server binding table full");
		break;
	case DNS_SERVER_ERR_WRITE_FAILED:
		respond_error(fd, 500, "Internal Server Error", "failed to write hosts file");
		break;
	case DNS_SERVER_ERR_NOT_FOUND:
	default:
		respond_error(fd, 404, "Not Found", "no such DNS server binding");
		break;
	}
}

static void handle_dns_server_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *container_name, *hosts_path;
	struct registry_entry *entry;
	enum dns_server_error serr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	container_name = json_as_string(json_object_get(root, "container"));
	hosts_path = json_as_string(json_object_get(root, "hosts_path"));

	if (container_name == NULL || hosts_path == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "container/hosts_path missing");
		return;
	}

	entry = registry_find(container_name);
	if (entry == NULL || !entry->running) {
		json_free(root);
		respond_error(fd, 404, "Not Found", "no such running container");
		return;
	}

	serr = dns_server_register(container_name, entry->handle.pid, entry->handle.pidfd,
	                            hosts_path);

	if (serr != DNS_SERVER_OK) {
		json_free(root);
		respond_dns_server_error(fd, serr);
		return;
	}

	/*
	 * container_name/hosts_path still point into root -- build the
	 * response before freeing it, not after (freeing first and then
	 * reading through these pointers would be a use-after-free).
	 */
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container_name);
	jw_key(&w, "hosts_path");
	jw_str(&w, hosts_path);
	jw_obj_close(&w);
	json_free(root);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_dns_server_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "servers");
	dns_server_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_dns_server_delete(int fd, const char *name)
{
	enum dns_server_error serr = dns_server_unregister(name);

	if (serr != DNS_SERVER_OK) {
		respond_dns_server_error(fd, serr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void respond_pki_error(int fd, enum pki_error err)
{
	switch (err) {
	case PKI_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid name/common_name/sans");
		break;
	case PKI_ERR_NOT_BOOTSTRAPPED:
		respond_error(fd, 400, "Bad Request", "CA not bootstrapped -- POST /v1/pki/ca first");
		break;
	case PKI_ERR_ALREADY_BOOTSTRAPPED:
		respond_error(fd, 409, "Conflict", "CA is already bootstrapped");
		break;
	case PKI_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "a cert with this name already exists");
		break;
	case PKI_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "PKI cert table full");
		break;
	case PKI_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such CA / cert");
		break;
	case PKI_ERR_OPENSSL_FAILED:
	case PKI_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "PKI operation failed");
		break;
	}
}

static void handle_pki_ca_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	const char *common_name = "Kanxeo Root CA";
	int days = 3650;
	enum pki_error perr;

	if (body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		if (json_as_string(json_object_get(root, "common_name")) != NULL)
			common_name = json_as_string(json_object_get(root, "common_name"));
		if (json_object_get(root, "days") != NULL)
			days = (int)json_as_number(json_object_get(root, "days"));
	}

	perr = pki_ca_create(common_name, days);
	json_free(root);

	if (perr != PKI_OK) {
		respond_pki_error(fd, perr);
		return;
	}

	{
		struct json_writer w;

		jw_init(&w);
		if (pki_ca_get(&w) != PKI_OK) {
			jw_free(&w);
			respond_error(fd, 500, "Internal Server Error", "CA created but could not be read back");
			return;
		}
		respond_json(fd, 201, "Created", &w);
		jw_free(&w);
	}
}

static void handle_pki_ca_get(int fd)
{
	struct json_writer w;
	enum pki_error perr;

	jw_init(&w);
	perr = pki_ca_get(&w);
	if (perr != PKI_OK) {
		jw_free(&w);
		/*
		 * PKI_ERR_NOT_BOOTSTRAPPED means two different things
		 * depending on the endpoint: for POST /v1/pki/certs it's a
		 * genuine "you can't do this yet" precondition (400, via
		 * respond_pki_error below). Here, GET-ing a CA that doesn't
		 * exist yet is exactly the same shape as GET
		 * /v1/dns/records/{name} or /v1/networks/{name} on a
		 * missing resource -- 404, matching every other single-
		 * resource GET in this API, not respond_pki_error's generic
		 * (POST-precondition-oriented) 400 mapping.
		 */
		if (perr == PKI_ERR_NOT_BOOTSTRAPPED)
			respond_error(fd, 404, "Not Found", "CA not bootstrapped yet");
		else
			respond_pki_error(fd, perr);
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pki_cert_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name;
	const struct json_value *jsans, *jdays;
	const char *sans_buf[PKI_MAX_SANS];
	int san_count;
	int days = 365;
	enum pki_error perr;
	struct json_writer w;
	size_t i;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	name = json_as_string(json_object_get(root, "name"));
	jsans = json_object_get(root, "sans");
	jdays = json_object_get(root, "days");
	if (jdays != NULL)
		days = (int)json_as_number(jdays);

	if (name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name missing");
		return;
	}

	if (jsans != NULL) {
		if (jsans->type != JSON_ARRAY || jsans->u.array.count == 0 ||
		    jsans->u.array.count > PKI_MAX_SANS) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "sans must be a non-empty array of at most 8 entries");
			return;
		}
		san_count = (int)jsans->u.array.count;
		for (i = 0; i < (size_t)san_count; i++) {
			sans_buf[i] = json_as_string(jsans->u.array.items[i]);
			if (sans_buf[i] == NULL) {
				json_free(root);
				respond_error(fd, 400, "Bad Request", "sans must be an array of strings");
				return;
			}
		}
	} else {
		sans_buf[0] = name;
		san_count = 1;
	}

	jw_init(&w);
	perr = pki_cert_create(name, sans_buf, san_count, days, NULL, &w);
	json_free(root);

	if (perr != PKI_OK) {
		jw_free(&w);
		respond_pki_error(fd, perr);
		return;
	}

	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_pki_cert_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "certs");
	pki_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pki_cert_get_one(int fd, const char *name)
{
	struct json_writer w;
	enum pki_error perr;

	jw_init(&w);
	perr = pki_cert_get_one(name, &w);
	if (perr != PKI_OK) {
		jw_free(&w);
		respond_pki_error(fd, perr);
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pki_cert_delete(int fd, const char *name)
{
	enum pki_error perr = pki_cert_delete(name);

	if (perr != PKI_OK) {
		respond_pki_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void respond_pkg_error(int fd, enum pkg_error err)
{
	switch (err) {
	case PKG_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid package name");
		break;
	case PKG_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such package");
		break;
	case PKG_ERR_INVALID_RECIPE:
		respond_error(fd, 400, "Bad Request", "no such recipe, or it failed to parse");
		break;
	case PKG_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "package is already installed");
		break;
	case PKG_ERR_BUSY:
		respond_error(fd, 409, "Conflict", "another package install is already in progress");
		break;
	case PKG_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "package table full");
		break;
	case PKG_ERR_INVALID_TOOLCHAIN:
		respond_error(fd, 400, "Bad Request", "toolchain_path missing, unreadable, or not a regular file");
		break;
	case PKG_ERR_SPAWN_FAILED:
	case PKG_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "package operation failed");
		break;
	}
}

/*
 * pkg_recipe_add()/pkg_recipe_delete() (ADR-0040) share pkg_error with
 * every other pkg.c operation, but "no such package"/"invalid package
 * name" read wrong for a recipe -- a real, user-visible wording gap
 * found live testing the new endpoints, not worth leaving in.
 */
static void respond_pkg_recipe_error(int fd, enum pkg_error err)
{
	switch (err) {
	case PKG_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid recipe name");
		break;
	case PKG_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such recipe");
		break;
	case PKG_ERR_INVALID_RECIPE:
		respond_error(fd, 400, "Bad Request",
		              "recipe content failed to parse, or its pkg_name= doesn't match name");
		break;
	case PKG_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "recipe operation failed");
		break;
	}
}

/*
 * body/body_len optional: an empty body (the existing bare
 * `POST /v1/pkg/bootstrap` contract) keeps the live-copy fallback
 * unchanged. A JSON body with "toolchain_path" set switches to the
 * correct production path (pkg_bootstrap_from_toolchain()) instead --
 * a local path the operator has already scp'd a real toolchain
 * artifact to, the same "local path, not an HTTP upload" precedent
 * /system/update's own image_path/kernel_path already established.
 */
static void handle_pkg_bootstrap(int fd, const char *body, size_t body_len)
{
	enum pkg_error perr;
	const char *toolchain_path = NULL;
	struct json_value *root = NULL;

	if (body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		toolchain_path = json_as_string(json_object_get(root, "toolchain_path"));
	}

	perr = (toolchain_path != NULL && toolchain_path[0] != '\0')
	           ? pkg_bootstrap_from_toolchain(toolchain_path)
	           : pkg_bootstrap_build_image();

	if (root != NULL)
		json_free(root);

	if (perr != PKG_OK) {
		respond_pkg_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_pkg_recipes_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "recipes");
	pkg_write_json_recipes(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Real, ongoing recipe management (ADR-0040) -- an operator can add or
 * update a recipe on an already-running system, no ISO rebuild/
 * reinstall needed. Upsert: an existing recipe with this name is
 * replaced, never duplicated (PKG_ERR_DUPLICATE doesn't apply here,
 * unlike pkg_install_start()'s own package-installation meaning of
 * "duplicate").
 */
static void handle_pkg_recipe_add(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name;
	const char *content;
	enum pkg_error perr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	name = json_as_string(json_object_get(root, "name"));
	content = json_as_string(json_object_get(root, "content"));
	if (name == NULL || content == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name and content both required");
		return;
	}

	perr = pkg_recipe_add(name, content);
	json_free(root);
	if (perr != PKG_OK) {
		respond_pkg_recipe_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_pkg_recipe_delete(int fd, const char *name)
{
	enum pkg_error perr = pkg_recipe_delete(name);

	if (perr != PKG_OK) {
		respond_pkg_recipe_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_pkg_install(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name;
	const char *image;
	const struct json_value *jupgrade;
	int upgrade;
	char started_name[PKG_NAME_MAX];
	pid_t pid;
	int pidfd;
	enum pkg_error perr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	name = json_as_string(json_object_get(root, "name"));
	if (name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name missing");
		return;
	}
	image = json_as_string(json_object_get(root, "image"));
	jupgrade = json_object_get(root, "upgrade");
	upgrade = (jupgrade != NULL && jupgrade->type == JSON_BOOL && jupgrade->u.boolean);

	perr = pkg_install_start(name, image, upgrade, started_name, sizeof(started_name), &pid,
	                          &pidfd);
	if (perr != PKG_OK) {
		json_free(root);
		respond_pkg_error(fd, perr);
		return;
	}

	/*
	 * started_name, not name -- if name needed a dependency installed
	 * first, that dependency (not name itself) is what's actually
	 * fetching right now, and that's the honest thing to describe.
	 */
	jw_init(&w);
	if (pkg_get_one(started_name, image, &w) != PKG_OK) {
		/* shouldn't happen -- pkg_install_start() just created it */
		jw_free(&w);
		json_free(root);
		respond_error(fd, 500, "Internal Server Error",
		              "package started but could not be read back");
		return;
	}
	json_free(root);
	register_pkg_fetch_pidfd(pid, pidfd);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}

/*
 * POST /v1/pkg/update-all -- the "automagic" package half of Phase 16
 * (ADR-0031), reusing pkg install --upgrade's entire existing mechanism
 * (pkg_install_start(..., upgrade=1, ...)) rather than a second rebuild
 * path. The only new thing is *finding* what needs it: the first
 * PKG_STATE_INSTALLED package (across every image) whose recipe's
 * version has drifted (pkg_find_update_candidate()). Starts exactly
 * one upgrade, honestly respecting the existing v1 single-job-in-
 * flight constraint (pkg_install_start() itself would return
 * PKG_ERR_BUSY otherwise) rather than pretending to parallelize past
 * it -- an operator or cron entry drains the whole backlog by calling
 * this again once each job finishes.
 */
static void handle_pkg_update_all(int fd)
{
	char name[PKG_NAME_MAX];
	char image[PKG_IMAGE_NAME_MAX];
	char started_name[PKG_NAME_MAX];
	pid_t pid;
	int pidfd;
	enum pkg_error perr;
	struct json_writer w;

	if (!pkg_find_update_candidate(name, sizeof(name), image, sizeof(image))) {
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "status");
		jw_str(&w, "nothing to update");
		jw_obj_close(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
		return;
	}

	perr = pkg_install_start(name, image, 1, started_name, sizeof(started_name), &pid, &pidfd);
	if (perr != PKG_OK) {
		respond_pkg_error(fd, perr);
		return;
	}

	jw_init(&w);
	if (pkg_get_one(started_name, image, &w) != PKG_OK) {
		/* shouldn't happen -- pkg_install_start() just created it */
		jw_free(&w);
		respond_error(fd, 500, "Internal Server Error",
		              "package started but could not be read back");
		return;
	}
	register_pkg_fetch_pidfd(pid, pidfd);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}

static void handle_pkg_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "packages");
	pkg_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Splits a "{name}" or "{name}@{image}" path segment (the compound
 * addressing form pkg_install_start()'s own image parameter documents --
 * bare name means PKG_DEFAULT_IMAGE) into two caller-owned buffers.
 * Returns 0 on success, -1 if either half doesn't fit its buffer.
 */
static int split_pkg_name_image(const char *raw, char *name_out, size_t name_out_size,
                                 char *image_out, size_t image_out_size)
{
	const char *at = strchr(raw, '@');

	if (at == NULL) {
		if (snprintf(name_out, name_out_size, "%s", raw) >= (int)name_out_size)
			return -1;
		image_out[0] = '\0';
		return 0;
	}
	if ((size_t)(at - raw) >= name_out_size)
		return -1;
	memcpy(name_out, raw, (size_t)(at - raw));
	name_out[at - raw] = '\0';
	if (snprintf(image_out, image_out_size, "%s", at + 1) >= (int)image_out_size)
		return -1;
	return 0;
}

static void handle_pkg_get_one(int fd, const char *raw_name)
{
	struct json_writer w;
	enum pkg_error perr;
	char name[PKG_NAME_MAX];
	char image[PKG_IMAGE_NAME_MAX];

	if (split_pkg_name_image(raw_name, name, sizeof(name), image, sizeof(image)) != 0) {
		respond_error(fd, 400, "Bad Request", "name@image too long");
		return;
	}

	jw_init(&w);
	perr = pkg_get_one(name, image[0] != '\0' ? image : NULL, &w);
	if (perr != PKG_OK) {
		jw_free(&w);
		respond_pkg_error(fd, perr);
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pkg_delete(int fd, const char *raw_name)
{
	char name[PKG_NAME_MAX];
	char image[PKG_IMAGE_NAME_MAX];
	enum pkg_error perr;

	if (split_pkg_name_image(raw_name, name, sizeof(name), image, sizeof(image)) != 0) {
		respond_error(fd, 400, "Bad Request", "name@image too long");
		return;
	}

	perr = pkg_delete(name, image[0] != '\0' ? image : NULL);
	if (perr != PKG_OK) {
		respond_pkg_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void dispatch(int fd, const struct http_request *req)
{
	const char *name;

	if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/v1/health") == 0) {
		handle_health(fd);
		return;
	}
	if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/v1/system/shutdown") == 0) {
		handle_shutdown(fd);
		return;
	}
	if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/v1/system/reboot") == 0) {
		handle_reboot(fd);
		return;
	}
	if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/v1/system/update") == 0) {
		handle_system_update(fd, req->body, req->body_len);
		return;
	}
	if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/v1/system/backup") == 0) {
		handle_system_backup(fd);
		return;
	}
	if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/v1/system/restore") == 0) {
		handle_system_restore(fd, req->body, req->body_len);
		return;
	}
	if (strcmp(req->path, "/v1/containers") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, CONTAINERS_PREFIX, strlen(CONTAINERS_PREFIX)) == 0) {
		name = req->path + strlen(CONTAINERS_PREFIX);
		if (name[0] != '\0') {
			/*
			 * Container names are [A-Za-z0-9_-] only (namecheck.h) --
			 * never contain '/' -- so a trailing "/stop" is unambiguous
			 * to detect with a plain suffix check, no generic
			 * sub-router needed for this one action endpoint.
			 */
			size_t nlen = strlen(name);

			if (nlen > 6 && strcmp(name + nlen - 6, "/start") == 0 &&
			    strcmp(req->method, "POST") == 0 && nlen - 6 < REGISTRY_NAME_MAX) {
				char container_name[REGISTRY_NAME_MAX];

				memcpy(container_name, name, nlen - 6);
				container_name[nlen - 6] = '\0';
				handle_start(fd, container_name);
				return;
			}
			if (nlen > 5 && strcmp(name + nlen - 5, "/stop") == 0 &&
			    strcmp(req->method, "POST") == 0 && nlen - 5 < REGISTRY_NAME_MAX) {
				char container_name[REGISTRY_NAME_MAX];

				memcpy(container_name, name, nlen - 5);
				container_name[nlen - 5] = '\0';
				handle_stop(fd, container_name);
				return;
			}
			if (nlen > 8 && strcmp(name + nlen - 8, "/unpause") == 0 &&
			    strcmp(req->method, "POST") == 0 && nlen - 8 < REGISTRY_NAME_MAX) {
				char container_name[REGISTRY_NAME_MAX];

				memcpy(container_name, name, nlen - 8);
				container_name[nlen - 8] = '\0';
				handle_unpause(fd, container_name);
				return;
			}
			if (nlen > 6 && strcmp(name + nlen - 6, "/pause") == 0 &&
			    strcmp(req->method, "POST") == 0 && nlen - 6 < REGISTRY_NAME_MAX) {
				char container_name[REGISTRY_NAME_MAX];

				memcpy(container_name, name, nlen - 6);
				container_name[nlen - 6] = '\0';
				handle_pause(fd, container_name);
				return;
			}
			if (strcmp(req->method, "GET") == 0) {
				handle_get_one(fd, name);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_delete(fd, name);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/devices") == 0 && strcmp(req->method, "GET") == 0) {
		handle_device_list(fd);
		return;
	}
	if (strcmp(req->path, "/v1/networks") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_network_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_network_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, NETWORKS_PREFIX, strlen(NETWORKS_PREFIX)) == 0) {
		name = req->path + strlen(NETWORKS_PREFIX);
		if (name[0] != '\0') {
			/*
			 * Network and interface names are both [A-Za-z0-9_-] only
			 * (namecheck.h) -- never contain '/' -- so "/interfaces" and
			 * "/interfaces/<ifname>" are unambiguous to detect with plain
			 * suffix/substring checks, the same precedent CONTAINERS_PREFIX's
			 * own "/stop" suffix check above already established, just one
			 * level deeper for the DELETE-a-specific-interface case.
			 */
			static const char iface_mid[] = "/interfaces/";
			char *sep = strstr(name, iface_mid);
			size_t nlen = strlen(name);

			if (sep != NULL && strcmp(req->method, "DELETE") == 0) {
				size_t net_name_len = (size_t)(sep - name);
				const char *ifname = sep + (sizeof(iface_mid) - 1);

				if (net_name_len > 0 && net_name_len < NETWORK_NAME_MAX && ifname[0] != '\0') {
					char net_name[NETWORK_NAME_MAX];

					memcpy(net_name, name, net_name_len);
					net_name[net_name_len] = '\0';
					handle_network_detach_interface(fd, net_name, ifname);
					return;
				}
			}
			if (nlen > 11 && strcmp(name + nlen - 11, "/interfaces") == 0 &&
			    strcmp(req->method, "POST") == 0 && nlen - 11 < NETWORK_NAME_MAX) {
				char net_name[NETWORK_NAME_MAX];

				memcpy(net_name, name, nlen - 11);
				net_name[nlen - 11] = '\0';
				handle_network_attach_interface(fd, net_name, req->body, req->body_len);
				return;
			}
			if (strcmp(req->method, "GET") == 0) {
				handle_network_get_one(fd, name);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_network_delete(fd, name);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/images") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_image_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_image_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, IMAGES_PREFIX, strlen(IMAGES_PREFIX)) == 0) {
		name = req->path + strlen(IMAGES_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_image_get_one(fd, name);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_image_delete(fd, name);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/dns/records") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_dns_record_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_dns_record_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, DNS_RECORDS_PREFIX, strlen(DNS_RECORDS_PREFIX)) == 0) {
		name = req->path + strlen(DNS_RECORDS_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_dns_record_get_one(fd, name);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_dns_record_delete(fd, name);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/dns/servers") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_dns_server_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_dns_server_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, DNS_SERVERS_PREFIX, strlen(DNS_SERVERS_PREFIX)) == 0) {
		name = req->path + strlen(DNS_SERVERS_PREFIX);
		if (name[0] != '\0' && strcmp(req->method, "DELETE") == 0) {
			handle_dns_server_delete(fd, name);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pki/ca") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pki_ca_get(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_pki_ca_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pki/certs") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pki_cert_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_pki_cert_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, PKI_CERTS_PREFIX, strlen(PKI_CERTS_PREFIX)) == 0) {
		name = req->path + strlen(PKI_CERTS_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_pki_cert_get_one(fd, name);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_pki_cert_delete(fd, name);
				return;
			}
		}
	}
	/*
	 * These reserved paths are checked before the generic
	 * PKG_PREFIX/{name} fallback below, exactly like every other
	 * resource's exact-match-then-prefix ordering in this dispatch --
	 * a package named "bootstrap"/"recipes"/"install"/"update-all"
	 * would be unreachable via GET/DELETE /v1/pkg/{name}, a
	 * deliberate, documented reserved-words boundary. PKG_RECIPES_PREFIX
	 * (/v1/pkg/recipes/{name}, DELETE) is checked here too, before the
	 * generic PKG_PREFIX/{name} fallback -- otherwise
	 * "/v1/pkg/recipes/bash" would wrongly match that fallback with
	 * name="recipes/bash" instead (ADR-0040: recipes are now a real,
	 * operator-managed catalog via this API, not baked into the ISO).
	 */
	if (strcmp(req->path, "/v1/pkg/bootstrap") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_pkg_bootstrap(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg/recipes") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_recipes_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_pkg_recipe_add(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, PKG_RECIPES_PREFIX, strlen(PKG_RECIPES_PREFIX)) == 0) {
		name = req->path + strlen(PKG_RECIPES_PREFIX);
		if (name[0] != '\0' && strcmp(req->method, "DELETE") == 0) {
			handle_pkg_recipe_delete(fd, name);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg/install") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_pkg_install(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg/update-all") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_pkg_update_all(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_list(fd);
			return;
		}
	}
	if (strncmp(req->path, PKG_PREFIX, strlen(PKG_PREFIX)) == 0) {
		name = req->path + strlen(PKG_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_pkg_get_one(fd, name);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_pkg_delete(fd, name);
				return;
			}
		}
	}

	/*
	 * Anything outside /v1/... isn't part of the API contract at all --
	 * it's the web dashboard's static assets (docs/adr/0010). An
	 * unrecognized /v1/... path still falls through to the JSON 404
	 * below, unchanged.
	 */
	if (strncmp(req->path, "/v1/", 4) != 0) {
		if (strcmp(req->method, "GET") == 0)
			static_serve(fd, g_web_root, req->path);
		else
			respond_error(fd, 404, "Not Found", "no such endpoint");
		return;
	}

	respond_error(fd, 404, "Not Found", "no such endpoint");
}

#define CONSOLE_SUFFIX "/console"
/*
 * Every real image this daemon has ever actually seen has no /bin at
 * all (packages stage into usr/bin -- confirmed directly against a
 * real running container while building this, see docs/roadmap/ROADMAP.md);
 * "/bin/sh" would fail on every one of them. This is only a default:
 * X-Kanxeo-Exec-Cmd overrides it, and a container whose image has
 * neither this nor an override installed simply fails to exec --
 * a real, expected limitation (see this phase's own ADR), not a bug.
 */
#define CONSOLE_DEFAULT_CMD "/usr/bin/bash"

/*
 * try_console_upgrade()'s own three possible outcomes -- plain 0/1
 * isn't enough here, unlike every other route: a failure can happen
 * either before cc has been touched at all (caller must still run its
 * normal teardown on cc) or after console_session_teardown() has
 * already closed and queued cc for deferred free (caller must NOT
 * touch it again -- see CONSOLE_HANDLED's own two cases below).
 */
enum console_route_result {
	CONSOLE_NOT_MATCHED,   /* not this route -- caller falls through to dispatch() as normal */
	CONSOLE_FAILED,        /* matched, but failed before cc was repurposed -- caller does its normal teardown on cc */
	CONSOLE_HANDLED        /* fully handled here (success, or already torn down via console_session_teardown()) -- caller must not touch cc again */
};

/*
 * GET /v1/containers/{name}/console -- an interactive shell inside an
 * already-running container, over a hand-rolled minimal WebSocket
 * (RFC 6455; see daemon/include/websocket.h for exactly what subset is
 * implemented). Not a normal REST request/response: on success this
 * repurposes cc in place (CONN_CLIENT -> CONN_CONSOLE_WS) and leaves
 * it registered in epoll indefinitely instead of the usual
 * dispatch()-then-close-the-fd path every other request takes.
 */
static enum console_route_result try_console_upgrade(struct conn *cc, const struct http_request *req)
{
	static const char suffix[] = CONSOLE_SUFFIX;
	size_t suffix_len = sizeof(suffix) - 1;
	const char *path_name;
	size_t path_name_len, name_len;
	char container_name[REGISTRY_NAME_MAX];
	struct registry_entry *entry;
	char upgrade_val[32], connection_val[64], ws_key[256], ws_version[8];
	char cmd_override[256];
	char *cmd_argv[2];
	const char *cmd;
	char accept_val[64];
	int master_fd;
	pid_t exec_pid;
	char response[512];
	int rlen;
	struct console_exec_session *sess;
	struct conn *pty_cc;
	struct kx_epoll_event ev;

	if (strcmp(req->method, "GET") != 0)
		return CONSOLE_NOT_MATCHED;
	if (strncmp(req->path, CONTAINERS_PREFIX, strlen(CONTAINERS_PREFIX)) != 0)
		return CONSOLE_NOT_MATCHED;

	path_name = req->path + strlen(CONTAINERS_PREFIX);
	path_name_len = strlen(path_name);
	if (path_name_len <= suffix_len || strcmp(path_name + path_name_len - suffix_len, suffix) != 0)
		return CONSOLE_NOT_MATCHED;

	name_len = path_name_len - suffix_len;
	if (name_len == 0 || name_len >= REGISTRY_NAME_MAX)
		return CONSOLE_NOT_MATCHED; /* not a well-formed container name -- let it fall through to the ordinary 404 */

	memcpy(container_name, path_name, name_len);
	container_name[name_len] = '\0';
	if (!name_is_valid(container_name))
		return CONSOLE_NOT_MATCHED;

	/* From here on this really is a console-upgrade request for a
	 * syntactically valid name -- every further failure gets a real,
	 * specific error response instead of falling through to a
	 * confusing generic 404. cc has not been touched yet in any of
	 * these branches, so CONSOLE_FAILED (caller runs its normal
	 * teardown on cc) is correct throughout this section. */

	if (http_find_header(req->headers, req->headers_len, "Upgrade", upgrade_val, sizeof(upgrade_val)) < 0 ||
	    strcasecmp(upgrade_val, "websocket") != 0) {
		respond_error(cc->fd, 400, "Bad Request", "this endpoint requires Upgrade: websocket");
		return CONSOLE_FAILED;
	}
	/* Connection is a comma-separated token list ("keep-alive, Upgrade"
	 * is common) -- a substring search for the one token that matters
	 * here, not an exact match. */
	if (http_find_header(req->headers, req->headers_len, "Connection", connection_val, sizeof(connection_val)) < 0 ||
	    strcasestr(connection_val, "upgrade") == NULL) {
		respond_error(cc->fd, 400, "Bad Request", "this endpoint requires Connection: Upgrade");
		return CONSOLE_FAILED;
	}
	if (http_find_header(req->headers, req->headers_len, "Sec-WebSocket-Key", ws_key, sizeof(ws_key)) < 0) {
		respond_error(cc->fd, 400, "Bad Request", "missing Sec-WebSocket-Key");
		return CONSOLE_FAILED;
	}
	if (http_find_header(req->headers, req->headers_len, "Sec-WebSocket-Version", ws_version, sizeof(ws_version)) < 0 ||
	    strcmp(ws_version, "13") != 0) {
		respond_error(cc->fd, 400, "Bad Request", "requires Sec-WebSocket-Version: 13");
		return CONSOLE_FAILED;
	}

	entry = registry_find(container_name);
	if (entry == NULL || !entry->running) {
		respond_error(cc->fd, 404, "Not Found", "no such running container");
		return CONSOLE_FAILED;
	}

	if (ws_compute_accept(ws_key, accept_val, sizeof(accept_val)) != 0) {
		respond_error(cc->fd, 500, "Internal Server Error", "failed to compute websocket accept");
		return CONSOLE_FAILED;
	}

	if (http_find_header(req->headers, req->headers_len, "X-Kanxeo-Exec-Cmd", cmd_override, sizeof(cmd_override)) >= 0 &&
	    cmd_override[0] != '\0')
		cmd = cmd_override;
	else
		cmd = CONSOLE_DEFAULT_CMD;
	cmd_argv[0] = (char *)cmd;
	cmd_argv[1] = NULL;

	/* Nothing from req is needed past this point -- safe to free
	 * cc->http's buffer (which req->headers/body point into) once cc
	 * is repurposed below; every value taken from req has already
	 * been copied into a local buffer above. */

	if (exec_into_container(entry->handle.pid, cmd_argv, &master_fd, &exec_pid) != 0) {
		respond_error(cc->fd, 500, "Internal Server Error", "failed to start console session");
		return CONSOLE_FAILED;
	}

	rlen = snprintf(response, sizeof(response),
	                 "HTTP/1.1 101 Switching Protocols\r\n"
	                 "Upgrade: websocket\r\n"
	                 "Connection: Upgrade\r\n"
	                 "Sec-WebSocket-Accept: %s\r\n"
	                 "\r\n",
	                 accept_val);
	if (rlen < 0 || (size_t)rlen >= sizeof(response)) {
		kill(exec_pid, SIGKILL);
		waitpid(exec_pid, NULL, 0);
		close(master_fd);
		respond_error(cc->fd, 500, "Internal Server Error", "failed to build handshake response");
		return CONSOLE_FAILED;
	}

	/* cc is still an untouched, ordinary CONN_CLIENT up through this
	 * point -- every failure above and below this comment, up until
	 * cc->kind actually changes further down, is still a CONSOLE_FAILED
	 * case the caller must tear down normally. */
	http_set_blocking(cc->fd);
	if (kx_write_all(cc->fd, response, (size_t)rlen) != 0) {
		/* Client already gone -- nothing left to respond with. */
		kill(exec_pid, SIGKILL);
		waitpid(exec_pid, NULL, 0);
		close(master_fd);
		return CONSOLE_FAILED;
	}

	sess = malloc(sizeof(*sess));
	pty_cc = malloc(sizeof(*pty_cc));
	if (sess == NULL || pty_cc == NULL) {
		free(sess);
		free(pty_cc);
		kill(exec_pid, SIGKILL);
		waitpid(exec_pid, NULL, 0);
		close(master_fd);
		/* The 101 response is already on the wire -- there's no
		 * meaningful HTTP error left to send after a successful
		 * upgrade, so this just drops the connection (cc is still
		 * untouched -- CONSOLE_FAILED's normal teardown handles it). */
		return CONSOLE_FAILED;
	}
	memset(pty_cc, 0, sizeof(*pty_cc));
	pty_cc->kind = CONN_CONSOLE_PTY;
	pty_cc->fd = master_fd;
	pty_cc->exec_session = sess;

	/* From here on cc IS repurposed -- any failure past this point must
	 * return CONSOLE_HANDLED, never CONSOLE_FAILED, since the caller's
	 * normal cc teardown would now double-free/double-close it. */
	http_conn_free(&cc->http);
	cc->kind = CONN_CONSOLE_WS;
	ws_conn_init(&cc->ws);
	cc->exec_session = sess;

	sess->ws_conn = cc;
	sess->pty_conn = pty_cc;
	sess->exec_pid = exec_pid;
	sess->torn_down = 0;

	ev.events = EPOLLIN;
	ev.data.ptr = pty_cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, pty_cc->fd, &ev) != 0) {
		/* Extremely unlikely (fd/epoll exhaustion) -- tear the whole
		 * session down the normal way (queues cc for deferred free
		 * too) rather than leaving half of it dangling. */
		console_session_teardown(sess);
		return CONSOLE_HANDLED;
	}

	/* cc->fd is already registered for EPOLLIN (accept_loop() did that
	 * when this connection was first accepted, as an ordinary
	 * CONN_CLIENT) -- only its kind and dispatch target change above;
	 * no epoll_ctl needed for it here. */
	return CONSOLE_HANDLED;
}

static void handle_console_ws_event(struct conn *cc)
{
	unsigned char buf[4096];
	ssize_t n;
	struct ws_frame frame;
	int pr;

	n = read(cc->fd, buf, sizeof(buf));
	if (n <= 0) {
		console_session_teardown(cc->exec_session);
		return;
	}
	if (ws_conn_feed(&cc->ws, buf, (size_t)n) != 0) {
		console_session_teardown(cc->exec_session);
		return;
	}

	for (;;) {
		pr = ws_conn_try_parse(&cc->ws, &frame);
		if (pr == 0)
			break;
		if (pr < 0) {
			console_session_teardown(cc->exec_session);
			return;
		}

		if (frame.opcode == WS_OPCODE_TEXT || frame.opcode == WS_OPCODE_BINARY) {
			if (frame.payload_len > 0 &&
			    kx_write_all(cc->exec_session->pty_conn->fd, frame.payload, frame.payload_len) != 0) {
				ws_conn_consume(&cc->ws, frame.frame_len);
				console_session_teardown(cc->exec_session);
				return;
			}
		} else if (frame.opcode == WS_OPCODE_CLOSE) {
			ws_conn_consume(&cc->ws, frame.frame_len);
			console_session_teardown(cc->exec_session);
			return;
		} else if (frame.opcode == WS_OPCODE_PING) {
			ws_write_frame(cc->fd, WS_OPCODE_PONG, frame.payload, frame.payload_len);
		}
		/* WS_OPCODE_PONG: nothing to do -- just a keepalive ack. */

		ws_conn_consume(&cc->ws, frame.frame_len);
	}
}

static void handle_console_pty_event(struct conn *cc)
{
	unsigned char buf[4096];
	ssize_t n;

	n = read(cc->fd, buf, sizeof(buf));
	if (n <= 0) {
		/* Shell exited (EOF) or the pty hung up (EIO, once every
		 * slave-side reference has closed) -- either way this
		 * session is over. */
		console_session_teardown(cc->exec_session);
		return;
	}

	if (ws_write_frame(cc->exec_session->ws_conn->fd, WS_OPCODE_BINARY, buf, (size_t)n) != 0)
		console_session_teardown(cc->exec_session);
}

static void handle_client_event(struct conn *cc)
{
	char buf[4096];
	ssize_t n;
	struct http_request req;
	int pr;

	n = read(cc->fd, buf, sizeof(buf));
	if (n <= 0) {
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		close(cc->fd);
		http_conn_free(&cc->http);
		free(cc);
		return;
	}

	if (http_conn_feed(&cc->http, buf, (size_t)n) != 0) {
		respond_error(cc->fd, 400, "Bad Request", "request too large");
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		close(cc->fd);
		http_conn_free(&cc->http);
		free(cc);
		return;
	}

	pr = http_conn_try_parse(&cc->http, &req);
	if (pr < 0) {
		respond_error(cc->fd, 400, "Bad Request", "malformed request");
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		close(cc->fd);
		http_conn_free(&cc->http);
		free(cc);
		return;
	}
	if (pr == 1) {
		enum console_route_result cr = try_console_upgrade(cc, &req);

		if (cr == CONSOLE_HANDLED)
			return; /* cc repurposed into CONN_CONSOLE_WS (or already torn down) -- must not be touched again */
		if (cr == CONSOLE_NOT_MATCHED)
			dispatch(cc->fd, &req);
		/* CONSOLE_FAILED: an error response (or nothing, if the client
		 * was already gone) was already written by
		 * try_console_upgrade() itself -- cc still needs the same
		 * teardown every other handled request gets below. */
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		close(cc->fd);
		http_conn_free(&cc->http);
		free(cc);
	}
	/* pr == 0: request incomplete, keep waiting on this fd. */
}

/*
 * Arms a one-shot, non-blocking timer (delay_seconds from now -- the
 * caller has already applied this container's own base delay plus any
 * backoff, see handle_container_event()) that, once it fires, replays
 * name's own persisted definition through create_container_from_body()
 * again -- the reactor's first-ever use of a timer, deliberately
 * isolated in a throwaway timerfd + CONN_RESTART_TIMER conn rather
 * than a blocking sleep(), which would freeze every other in-flight
 * request/event for the whole delay (this daemon's entire event loop
 * is single-threaded and non-blocking by design). A failure to arm is
 * logged and simply means this one restart doesn't happen -- not fatal
 * to the daemon.
 */
static void arm_restart_timer(const char *name, int delay_seconds)
{
	int tfd;
	struct itimerspec its;
	struct conn *cc;
	struct kx_epoll_event ev;

	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (tfd < 0) {
		perror("timerfd_create (container restart)");
		return;
	}

	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = delay_seconds;
	if (timerfd_settime(tfd, 0, &its, NULL) != 0) {
		perror("timerfd_settime (container restart)");
		close(tfd);
		return;
	}

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (container restart timer conn)");
		close(tfd);
		return;
	}
	cc->kind = CONN_RESTART_TIMER;
	cc->fd = tfd;
	snprintf(cc->restart_name, sizeof(cc->restart_name), "%s", name);

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, tfd, &ev) != 0) {
		perror("epoll_ctl ADD restart timer");
		close(tfd);
		free(cc);
	}
}

/*
 * Fires once arm_restart_timer()'s delay elapses. containerdef_find()
 * returning NULL here is a real, correct case, not an error: the
 * operator may have DELETEd this container during the delay window,
 * which already removed its definition (handle_delete()) -- nothing
 * to restart. def->stopped is checked unconditionally (any restart
 * policy, not just "unless-stopped") for the same reason: POST
 * .../stop may have fired during this same delay window, after
 * handle_container_event() already armed this timer but before it
 * fired -- without this check, that stop would be silently undone a
 * moment later. See ADR-0027.
 */
static void handle_restart_timer_event(struct conn *cc)
{
	struct container_def *def;
	uint64_t expirations;

	/* Required to clear the timerfd's own expiration count -- without
	 * this read() the fd would stay perpetually EPOLLIN-readable. Its
	 * value itself (how many periods elapsed -- always 1 for a
	 * one-shot timer that fired on time) isn't needed for anything. */
	if (read(cc->fd, &expirations, sizeof(expirations)) < 0)
		perror("read (restart timerfd)");

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	close(cc->fd);

	def = containerdef_find(cc->restart_name);
	if (def != NULL && !def->stopped) {
		struct registry_entry *entry;
		char restart_policy[16];
		int restart_delay_seconds;
		char depends_on[CONTAINERDEF_MAX_DEPENDS][REGISTRY_NAME_MAX];
		int depends_on_count;
		/* Readiness is boot-autostart-only (see containerdef_autostart_all()'s
		 * own comment on why) -- these are validated/extracted the same
		 * way regardless of caller, but a crash restart never acts on
		 * them. */
		int has_readiness, readiness_tcp_port, readiness_timeout_seconds;
		char err_msg[256];
		int status = create_container_from_body(
		    def->body, def->body_len, &entry, restart_policy, &restart_delay_seconds, depends_on,
		    &depends_on_count, &has_readiness, &readiness_tcp_port, &readiness_timeout_seconds,
		    err_msg, sizeof(err_msg));

		if (status != 0)
			fprintf(stderr, "%s: restart failed: %s\n", cc->restart_name, err_msg);
		/* else: create_container_from_body() already registered its
		 * own pidfd -- no separate call needed here either. */
	}

	free(cc);
}

static void handle_container_event(struct conn *cc)
{
	struct registry_entry *entry = cc->entry;
	char name_copy[REGISTRY_NAME_MAX];
	int exit_status;
	time_t started_at;
	struct container_def *def;
	pid_t pkg_pid;
	int pkg_pidfd;
	int chained;

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	registry_mark_exited(entry);
	entry->reactor_conn = NULL;
	free(cc);

	/* Copied before any registry_remove() below might reuse this
	 * slot -- entry->name/exit_status/started_at are only guaranteed
	 * valid until then. */
	snprintf(name_copy, sizeof(name_copy), "%s", entry->name);
	exit_status = entry->exit_status;
	started_at = entry->started_at;

	/*
	 * Unconditional, exactly like dns_record_forget_owner()/
	 * pki_cert_forget_owner() on every container delete -- pkg.c
	 * decides relevance (no-op unless this is PKG_BUILD_CONTAINER_NAME),
	 * so this hook stays trivial regardless of which container exited.
	 * A return of 1 means a dependency chain is advancing into another
	 * fetch -- register its pidfd exactly like a fresh top-level
	 * install already does.
	 */
	chained = pkg_build_completed(entry->name, entry->exit_status, &pkg_pid, &pkg_pidfd);
	if (strcmp(entry->name, PKG_BUILD_CONTAINER_NAME) == 0)
		registry_remove(entry->name);
	if (chained)
		register_pkg_fetch_pidfd(pkg_pid, pkg_pidfd);

	/*
	 * An unprompted exit (never reached for an explicit DELETE or
	 * POST .../stop, both of which remove the pidfd from epoll
	 * themselves before this event could ever fire -- see
	 * registry_remove()'s own doc comment) of a restart:"always"/
	 * "on-failure"/"unless-stopped" container. "on-failure" skips the
	 * restart only for a clean (exit_status == 0) exit -- this decision
	 * cannot survive a daemon restart (exit_status is never persisted),
	 * a stated v1 boundary, see ADR-0027.
	 */
	def = containerdef_find(name_copy);
	if (def != NULL) {
		int should_restart = !(strcmp(def->restart_policy, "on-failure") == 0 && exit_status == 0);

		/* registry_remove() is safe here even though entry->running is
		 * already 0 (its own kill/reap branch is skipped, so no signal
		 * is sent to a possibly-already-reused pid) -- frees the
		 * name/slot before a delayed restart re-creates it. */
		registry_remove(name_copy);

		if (should_restart) {
			time_t uptime = time(NULL) - started_at;
			int delay = def->restart_delay_seconds;
			int i;

			if (uptime >= CONTAINER_RESTART_STABILITY_SECONDS)
				def->consecutive_failures = 0;
			else
				def->consecutive_failures++;

			/* Self-terminating: stops doubling the moment delay would
			 * meet or exceed the cap, so this is safe regardless of how
			 * large consecutive_failures grows over a long daemon
			 * lifetime -- no risk of shift/multiply overflow. */
			for (i = 0; i < def->consecutive_failures && delay < CONTAINER_RESTART_BACKOFF_CAP_SECONDS;
			     i++)
				delay *= 2;
			if (delay > CONTAINER_RESTART_BACKOFF_CAP_SECONDS)
				delay = CONTAINER_RESTART_BACKOFF_CAP_SECONDS;

			arm_restart_timer(name_copy, delay);
		}
		/* else: on-failure, clean 0 exit -- left down for the rest of
		 * this daemon's uptime. The persisted definition is untouched
		 * and will be attempted again, unconditionally, at the next
		 * daemon boot (containerdef_autostart_all() has no notion of
		 * "how did it last exit"). */
	}
}

/*
 * Mirrors handle_container_event()'s shape for the package fetch
 * step's plain fork()'d curl subprocess: reap it (non-blocking here --
 * EPOLLIN on its pidfd already means it has exited), hand the exit
 * status to pkg_fetch_completed(), and if it says a build should
 * start, spawn it through the exact same registry_create() +
 * register_container_pidfd() path every other container already
 * goes through -- one source of truth for "what's running," the
 * package build container included.
 */
static void handle_pkg_fetch_event(struct conn *cc)
{
	int status;
	int exit_status;
	struct container_spec spec;

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	if (waitpid(cc->pkg_fetch_pid, &status, 0) == cc->pkg_fetch_pid && WIFEXITED(status))
		exit_status = WEXITSTATUS(status);
	else
		exit_status = -1;
	close(cc->fd);
	free(cc);

	if (pkg_fetch_completed(exit_status, &spec)) {
		struct registry_entry *entry;
		enum registry_error rerr =
		    registry_create(PKG_BUILD_CONTAINER_NAME, "pkgbuild", &spec, NULL, 0, 0, NULL, 0, NULL,
		                     0, &entry);

		if (rerr != REGISTRY_OK)
			pkg_build_spawn_failed();
		else
			register_container_pidfd(entry);
	}
}

/*
 * Same shape as register_pkg_fetch_pidfd(), for the console-shell child
 * spawn_console_shell() just forked -- its exit (operator typed "exit",
 * the tty vanished, whatever) needs the same non-blocking "tell me via
 * epoll" treatment, not a blocking wait or a SIGCHLD handler this
 * reactor has never needed before.
 */
static void register_console_shell_pidfd(pid_t pid, int pidfd, const char *tty_path)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (console shell reactor conn)");
		close(pidfd);
		return;
	}
	cc->kind = CONN_CONSOLE_SHELL;
	cc->fd = pidfd;
	cc->console_pid = pid;
	snprintf(cc->console_tty, sizeof(cc->console_tty), "%s", tty_path);

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD console shell pidfd");
		close(pidfd);
		free(cc);
	}
}

/*
 * Forks a console-login child bound to tty_path and execve()s kanxeoctl
 * into it with no command -- kanxeoctl's own isatty(STDIN_FILENO) check
 * (Phase 18) then drops it straight into run_shell(). setsid() detaches
 * any inherited controlling terminal (moot for a PID 1 caller, which
 * never had one) so the following open() of tty_path, being this new
 * session's first tty open without O_NOCTTY, makes it that session's
 * controlling terminal -- standard Linux tty semantics, no explicit
 * TIOCSCTTY needed. That isolation is what keeps a Ctrl-C typed at this
 * console from ever reaching kanxeod itself: it lands on this child's
 * own, separate session/process group only. Talks to kanxeod over real
 * HTTP via g_bind_addr/g_port like any other kanxeoctl invocation --
 * this is still a pure REST client, API-First Mandate intact, just
 * running on the same host it's talking to.
 *
 * A failed open()/execve() (tty genuinely absent, kanxeoctl missing) is
 * non-fatal: the child just exits, the pidfd event still fires, and
 * handle_console_shell_event() arms a respawn -- the same "keep trying
 * forever" a real getty already has for an unready device.
 */
static void spawn_console_shell(const char *tty_path)
{
	pid_t pid;
	char host_arg[64], port_arg[32];
	int pidfd;

	snprintf(host_arg, sizeof(host_arg), "--host=%s", g_bind_addr);
	snprintf(port_arg, sizeof(port_arg), "--port=%d", g_port);

	pid = fork();
	if (pid < 0) {
		perror("fork (console shell)");
		return;
	}
	if (pid == 0) {
		int fd;
		char *argv[] = { (char *)"kanxeoctl", host_arg, port_arg, NULL };

		setsid();
		fd = open(tty_path, O_RDWR);
		if (fd < 0) {
			perror(tty_path);
			_exit(1);
		}
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO)
			close(fd);
		execve("/bin/kanxeoctl", argv, environ);
		perror("execve /bin/kanxeoctl");
		_exit(127);
	}

	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		perror("pidfd_open (console shell)");
		return;
	}
	register_console_shell_pidfd(pid, pidfd, tty_path);
}

/*
 * Arms a one-shot delay before respawning a console shell on tty_path --
 * same timerfd + epoll shape as arm_restart_timer() (container crash-
 * restart backoff), reused here so a console shell that exits instantly
 * on every respawn (kanxeoctl missing, tty genuinely broken) can't spin
 * the reactor in a tight fork loop.
 */
static void arm_console_respawn_timer(const char *tty_path, int delay_seconds)
{
	int tfd;
	struct itimerspec its;
	struct conn *cc;
	struct kx_epoll_event ev;

	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (tfd < 0) {
		perror("timerfd_create (console respawn)");
		return;
	}

	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = delay_seconds;
	if (timerfd_settime(tfd, 0, &its, NULL) != 0) {
		perror("timerfd_settime (console respawn)");
		close(tfd);
		return;
	}

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (console respawn timer conn)");
		close(tfd);
		return;
	}
	cc->kind = CONN_CONSOLE_RESPAWN_TIMER;
	cc->fd = tfd;
	snprintf(cc->console_tty, sizeof(cc->console_tty), "%s", tty_path);

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, tfd, &ev) != 0) {
		perror("epoll_ctl ADD console respawn timer");
		close(tfd);
		free(cc);
	}
}

/* Reaps the exited console-shell child (mirrors handle_pkg_fetch_event()'s
 * reap shape) and arms a short respawn delay on the same tty -- keeps
 * console access persistent across "exit"/Ctrl-D the same way a real
 * getty would, without ever blocking the reactor. */
static void handle_console_shell_event(struct conn *cc)
{
	int status;
	char tty_path[sizeof(cc->console_tty)];

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	waitpid(cc->console_pid, &status, 0);
	close(cc->fd);
	snprintf(tty_path, sizeof(tty_path), "%s", cc->console_tty);
	free(cc);

	arm_console_respawn_timer(tty_path, 2);
}

/* Fires once arm_console_respawn_timer()'s delay elapses -- drains the
 * timerfd (same required read() arm_restart_timer()'s own handler
 * already documents) and forks a fresh console shell on the same tty. */
static void handle_console_respawn_timer_event(struct conn *cc)
{
	uint64_t expirations;
	char tty_path[sizeof(cc->console_tty)];

	if (read(cc->fd, &expirations, sizeof(expirations)) < 0)
		perror("read (console respawn timerfd)");

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	close(cc->fd);
	snprintf(tty_path, sizeof(tty_path), "%s", cc->console_tty);
	free(cc);

	spawn_console_shell(tty_path);
}

static void accept_loop(void)
{
	int client_fd;
	struct conn *cc;
	struct kx_epoll_event ev;

	for (;;) {
		client_fd = accept4(g_listener_conn.fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (client_fd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			perror("accept4");
			break;
		}

		cc = malloc(sizeof(*cc));
		if (cc == NULL) {
			/* Under memory pressure, drop this one connection
			 * attempt -- unlike register_container_pidfd(), no
			 * persistent state has been committed yet, so this
			 * isn't fatal. */
			close(client_fd);
			continue;
		}
		cc->kind = CONN_CLIENT;
		cc->fd = client_fd;
		http_conn_init(&cc->http);

		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN;
		ev.data.ptr = cc;
		if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, client_fd, &ev) != 0) {
			perror("epoll_ctl ADD client");
			close(client_fd);
			free(cc);
		}
	}
}

/*
 * Returns 1 if a TCP connection to ip_be:port succeeded within
 * timeout_seconds, 0 if it never did. Blocking, by design -- boot-time
 * autostart is already a blocking sequence (see
 * containerdef_autostart_all()'s own comment); this is consistent with
 * that existing, accepted boundary, not a new one, and is deliberately
 * NOT used from the crash-restart path (handle_restart_timer_event()),
 * which runs inside the reactor's own event loop where blocking would
 * violate the non-blocking design ADR-0025 established. No
 * non-blocking-connect-plus-poll() dance needed either: the
 * destination is always a directly L2-adjacent bridge network this
 * daemon itself manages, so a refused/not-yet-listening connection
 * returns immediately (ECONNREFUSED), never hangs the way a genuinely
 * unreachable route would.
 */
static int wait_for_tcp_ready(uint32_t ip_be, int port, int timeout_seconds)
{
	time_t deadline = time(NULL) + timeout_seconds;

	while (time(NULL) < deadline) {
		int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		struct sockaddr_in addr;
		int rc;

		if (fd < 0)
			return 0;

		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons((uint16_t)port);
		addr.sin_addr.s_addr = ip_be;

		rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
		close(fd);
		if (rc == 0)
			return 1;

		usleep(200000);
	}
	return 0;
}

/*
 * Starts every persisted restart:"always"/"on-failure"/"unless-stopped"
 * definition, in dependency order -- called once, right after
 * confirm_boot() (never before: see that function's own comment on why
 * "healthy" must stay defined as "about to serve traffic" alone, never
 * coupled to whether every container also happened to start cleanly).
 * Runs unconditionally, not just under --init-mode -- this is
 * "whenever this daemon process starts fresh" behavior, not specific
 * to the installed A/B boot flow. Blocking, like every other startup
 * step already is (network_init(), pkg_init(), ...); the kernel's own
 * listen backlog queues any incoming connection during this window,
 * none are dropped. A single definition failing to start is logged and
 * skipped, never fatal to the rest of boot.
 *
 * def->stopped only skips a definition here when restart_policy is
 * "unless-stopped" -- for "always"/"on-failure", a daemon restart is a
 * fresh chance regardless of a prior POST .../stop, matching Docker's
 * own real --restart=always semantics (a manual stop doesn't survive a
 * daemon restart). See ADR-0027.
 */
static void containerdef_autostart_all(void)
{
	char order[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	int count = containerdef_resolve_order(order);
	int i;

	for (i = 0; i < count; i++) {
		struct container_def *def = containerdef_find(order[i]);
		struct registry_entry *entry;
		char restart_policy[16];
		int restart_delay_seconds;
		char depends_on[CONTAINERDEF_MAX_DEPENDS][REGISTRY_NAME_MAX];
		int depends_on_count;
		int has_readiness, readiness_tcp_port, readiness_timeout_seconds;
		char err_msg[256];
		int status;

		if (def == NULL)
			continue; /* can't happen -- resolve_order() only ever names known defs */

		if (def->stopped && strcmp(def->restart_policy, "unless-stopped") == 0) {
			fprintf(stderr, "%s: unless-stopped, explicitly stopped -- not autostarting\n",
			        order[i]);
			continue;
		}

		status = create_container_from_body(def->body, def->body_len, &entry, restart_policy,
		                                     &restart_delay_seconds, depends_on, &depends_on_count,
		                                     &has_readiness, &readiness_tcp_port,
		                                     &readiness_timeout_seconds, err_msg, sizeof(err_msg));
		if (status != 0) {
			fprintf(stderr, "%s: autostart failed: %s\n", order[i], err_msg);
			continue;
		}
		/* create_container_from_body() already registered entry's own
		 * pidfd with epoll -- exactly the same shape POST /v1/containers'
		 * own success path relies on, no separate call needed here. */

		/*
		 * A def whose restart_policy is "always"/"on-failure" and was
		 * previously stopped=1 (manual POST .../stop, surviving that
		 * mattering only for "unless-stopped" per the check above) just
		 * successfully came back live here -- clear stopped now, the
		 * same thing handle_start() (ADR-0045) already does on its own
		 * success path. Without this, a real bug: def->stopped stays
		 * permanently stuck at 1 after this point, which
		 * handle_restart_timer_event() (the crash-restart path, checked
		 * unconditionally regardless of policy) would then read as
		 * "don't restart" forever -- silently defeating this
		 * container's own restart:"always"/"on-failure" policy for any
		 * crash after this boot, not just suppressing this one
		 * autostart. Harmless no-op if it was already 0.
		 */
		containerdef_set_stopped(order[i], 0);

		/* def (not the freshly-returned out-params above) is the
		 * authoritative, already-persisted source for readiness -- see
		 * wait_for_tcp_ready()'s own comment for why this wait is
		 * boot-autostart-only. */
		if (def->has_readiness &&
		    !wait_for_tcp_ready(entry->nets[0].ip_be, def->readiness_tcp_port,
		                         def->readiness_timeout_seconds)) {
			fprintf(stderr,
			        "%s: readiness check on tcp/%d did not succeed within %ds -- "
			        "starting dependents anyway\n",
			        entry->name, def->readiness_tcp_port, def->readiness_timeout_seconds);
		}

		printf("%s: autostarted (restart:%s)\n", entry->name, def->restart_policy);
	}
}

int main(int argc, char **argv)
{
	int port = DEFAULT_PORT;
	const char *bind_addr = DEFAULT_BIND;
	const char *web_root = DEFAULT_WEB_ROOT;
	int init_mode = 0;
	int simulate_unhealthy = 0;
	const char *slot = NULL;
	const char *test_update_image = NULL;
	const char *test_update_kernel = NULL;
	const char *test_bootstrap_toolchain = NULL;
	int i;
	int listen_fd;
	int opt = 1;
	struct sockaddr_in addr;
	struct kx_epoll_event ev;
	struct sigaction sa;

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--port=", 7) == 0)
			port = atoi(argv[i] + 7);
		else if (strncmp(argv[i], "--bind=", 7) == 0)
			bind_addr = argv[i] + 7;
		else if (strncmp(argv[i], "--web-root=", 11) == 0)
			web_root = argv[i] + 11;
		else if (strcmp(argv[i], "--init-mode") == 0)
			init_mode = 1;
		else if (strncmp(argv[i], "--slot=", 7) == 0)
			slot = argv[i] + 7;
		else if (strcmp(argv[i], "--simulate-unhealthy-boot") == 0)
			simulate_unhealthy = 1;
		else if (strncmp(argv[i], "--test-update-image=", 20) == 0)
			test_update_image = argv[i] + 20;
		else if (strncmp(argv[i], "--test-update-kernel=", 21) == 0)
			test_update_kernel = argv[i] + 21;
		else if (strncmp(argv[i], "--test-bootstrap-toolchain=", 27) == 0)
			test_bootstrap_toolchain = argv[i] + 27;
		else if (strncmp(argv[i], "--data-dir=", 11) == 0)
			snprintf(g_base_dir, sizeof(g_base_dir), "%s", argv[i] + 11);
	}
	init_base_dir_paths();
	g_web_root = web_root;
	g_slot = slot;
	g_bind_addr = bind_addr;
	g_port = port;

	if (init_mode) {
		/* The only observable serial-console signal for which slot
		 * actually booted this attempt (Phase 11 part 2's rollback
		 * test) -- --simulate-unhealthy-boot still reaches the normal
		 * "listening" line below, so that line alone can't tell slot
		 * A from slot B once both boot successfully. */
		printf("init-mode: slot=%s\n", slot != NULL ? slot : "(none)");
		fflush(stdout);
		if (boot_init() != 0)
			return 1;
	}

	/*
	 * Test-only, same precedent as --simulate-unhealthy-boot: makes
	 * POST /v1/system/update's real device-write/loader-entry logic
	 * (do_system_update()) observable from the serial console inside a
	 * real QEMU guest with a real virtio-blk disk attached, where
	 * ROOT_A_DEVICE/ROOT_B_DEVICE actually exist -- test/test_boot_
	 * update.c has no other way to reach this code path at all,
	 * host-to-guest HTTP being unavailable for a statically-addressed
	 * guest under this project's own test harness (see CLAUDE.md).
	 * Exercises the exact same function a live request would; only the
	 * thin JSON-parsing/HTTP-dispatch plumbing above it goes untested
	 * here, already covered structurally by every other JSON-bodied
	 * endpoint's own tests.
	 */
	if (test_update_image != NULL || test_update_kernel != NULL) {
		char body[2 * PATH_MAX + 64];
		char out_slot[8];
		char out_errmsg[256];
		int out_updated_root, out_updated_kernel;
		int status;
		size_t len;

		len = (size_t)snprintf(body, sizeof(body), "{");
		if (test_update_image != NULL)
			len += (size_t)snprintf(body + len, sizeof(body) - len, "\"image_path\":\"%s\"",
			                         test_update_image);
		if (test_update_kernel != NULL) {
			if (test_update_image != NULL)
				len += (size_t)snprintf(body + len, sizeof(body) - len, ",");
			len += (size_t)snprintf(body + len, sizeof(body) - len, "\"kernel_path\":\"%s\"",
			                         test_update_kernel);
		}
		snprintf(body + len, sizeof(body) - len, "}");

		status = do_system_update(body, strlen(body), out_slot, sizeof(out_slot),
		                           &out_updated_root, &out_updated_kernel, out_errmsg,
		                           sizeof(out_errmsg));
		if (status == 200)
			printf("test-update: status=200 slot=%s updated=%s%s%s\n", out_slot,
			       out_updated_root ? "root" : "", out_updated_root && out_updated_kernel ? "," : "",
			       out_updated_kernel ? "kernel" : "");
		else
			printf("test-update: status=%d err=%s\n", status, out_errmsg);
		fflush(stdout);
	}

	if (ensure_dir(g_base_dir) != 0 || ensure_dir(IMAGES_DIR) != 0 ||
	    ensure_dir(CONTAINERS_DIR) != 0 || ensure_dir(PKI_DIR) != 0 ||
	    ensure_dir(PKI_CERTS_DIR) != 0 || ensure_dir(PKG_DIR) != 0)
		return 1;

	if (network_init(NETWORKS_STATE_PATH) != 0)
		return 1;
	if (dns_init(DNS_RECORDS_STATE_PATH) != 0)
		return 1;
	if (pki_init(PKI_DIR, PKI_CERTS_STATE_PATH) != 0)
		return 1;
	if (pkg_init(PKG_DIR, PKG_INSTALLED_STATE_PATH, CONTAINERS_DIR, IMAGES_DIR) != 0)
		return 1;

	/*
	 * Test-only, same precedent as --test-update-image=: makes
	 * pkg_bootstrap_from_toolchain() (ADR-0035) observable from the
	 * serial console inside a real QEMU guest with a real toolchain
	 * squashfs attached on a scratch partition, where test/test_
	 * console_pkg_bootstrap.c has no other way to reach this code path
	 * at all (host-to-guest HTTP being unavailable for a statically-
	 * addressed guest under this project's own test harness). Placed
	 * after pkg_init() specifically -- unlike --test-update-image=
	 * above, this needs g_pkgbuild_rootfs already resolved. Prints a
	 * concrete, checkable fact (a real toolchain binary's presence
	 * afterward), not just the call's own return status -- proof
	 * unsquashfs genuinely extracted real content, not just that the
	 * function returned OK.
	 */
	if (test_bootstrap_toolchain != NULL) {
		enum pkg_error perr = pkg_bootstrap_from_toolchain(test_bootstrap_toolchain);

		if (perr == PKG_OK) {
			printf("test-bootstrap-toolchain: status=ok gcc=%s\n",
			       pkg_toolchain_has_gcc() ? "present" : "MISSING");
		} else {
			printf("test-bootstrap-toolchain: status=error(%d)\n", (int)perr);
		}
		fflush(stdout);
	}

	image_init(IMAGES_DIR);
	if (containerdef_init(CONTAINER_DEFS_STATE_PATH) != 0)
		return 1;

	registry_init();

	/*
	 * ensure_dir()'s directory scaffolding and each module's own
	 * state-init above are real writes onto BASE_DIR -- the real
	 * kanxeo-containers partition as of this change (ADR-0018), not a
	 * tmpfs. QEMU's default `-drive` cache mode (writeback) only
	 * guarantees those bytes reach the actual disk image once the
	 * guest itself issues a flush; without this, they'd sit in the
	 * guest kernel's own dirty-page cache, unbounded, until its
	 * periodic writeback timer next fires -- and this daemon printing
	 * "listening on" is exactly the signal an external test harness
	 * (or a real operator power-cycling the machine) treats as "boot
	 * finished successfully," so it needs to already be true by then.
	 * Same "flush before anything can go wrong" posture the shutdown
	 * path already has (ADR-0016's sync() before reboot(2)), applied
	 * here to the boot-time writes instead -- and it has to run
	 * *before* the "listening on" line below, not after, or a test
	 * harness (or operator) acting on that line as the all-clear can
	 * still race a sync that hasn't happened yet.
	 */
	if (init_mode)
		sync();

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	/*
	 * SOCK_CLOEXEC/EPOLL_CLOEXEC everywhere below: container_create()
	 * clone3()'s a new process for every container this daemon runs.
	 * Without close-on-exec, that child inherits a duplicate of every
	 * fd we hold open at the moment of the clone -- including the
	 * client connection socket for whatever request triggered the
	 * container's creation. The kernel won't deliver EOF on that
	 * connection to the client until *every* reference to it closes,
	 * so an un-CLOEXEC'd fd would silently stall that HTTP response
	 * until the container itself exits, defeating the entire
	 * non-blocking reactor. The container's own execve() closes any
	 * CLOEXEC fd immediately, well before it does real work.
	 */
	listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listen_fd < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
		fprintf(stderr, "invalid --bind address: %s\n", bind_addr);
		return 1;
	}

	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("bind");
		return 1;
	}
	if (listen(listen_fd, 128) != 0) {
		perror("listen");
		return 1;
	}

	g_epfd = epoll_create1(EPOLL_CLOEXEC);
	if (g_epfd < 0) {
		perror("epoll_create1");
		return 1;
	}

	g_listener_conn.kind = CONN_LISTENER;
	g_listener_conn.fd = listen_fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_listener_conn;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, listen_fd, &ev) != 0) {
		perror("epoll_ctl ADD listener");
		return 1;
	}

	printf("kanxeod listening on %s:%d\n", bind_addr, port);
	fflush(stdout);

	/* "About to serve traffic" is the honest definition of healthy this
	 * confirms -- everything above (mounts, network bring-up, the full
	 * existing state-init sequence, the listening socket itself) had to
	 * genuinely succeed to reach this line. */
	if (init_mode && slot != NULL && !simulate_unhealthy) {
		if (confirm_boot(slot) != 0)
			fprintf(stderr, "confirm_boot failed for slot %s (continuing anyway)\n", slot);
	}

	containerdef_autostart_all();

	/* Console login (Phase 19): a real console needs something to walk
	 * up to, once boot is fully healthy -- never for a dev/test kanxeod
	 * (no --init-mode), which is already running attached to a real
	 * developer's own terminal and must never fork a second process to
	 * fight it over. Both consoles get an independent instance -- either
	 * could be the one an operator is actually watching. */
	if (init_mode) {
		spawn_console_shell("/dev/tty0");
		spawn_console_shell("/dev/ttyS0");
	}

	while (!g_stop) {
		struct kx_epoll_event events[MAX_EVENTS];
		int n = kx_epoll_wait(g_epfd, events, MAX_EVENTS, -1);
		int j;
		struct conn *cc;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll_wait");
			break;
		}

		for (j = 0; j < n; j++) {
			cc = events[j].data.ptr;
			if (cc->kind == CONN_DEAD)
				continue; /* torn down earlier in this same batch -- see g_pending_free's own comment */
			if (cc->kind == CONN_LISTENER)
				accept_loop();
			else if (cc->kind == CONN_CONTAINER)
				handle_container_event(cc);
			else if (cc->kind == CONN_PKG_FETCH)
				handle_pkg_fetch_event(cc);
			else if (cc->kind == CONN_RESTART_TIMER)
				handle_restart_timer_event(cc);
			else if (cc->kind == CONN_CONSOLE_SHELL)
				handle_console_shell_event(cc);
			else if (cc->kind == CONN_CONSOLE_RESPAWN_TIMER)
				handle_console_respawn_timer_event(cc);
			else if (cc->kind == CONN_CONSOLE_WS)
				handle_console_ws_event(cc);
			else if (cc->kind == CONN_CONSOLE_PTY)
				handle_console_pty_event(cc);
			else
				handle_client_event(cc);
		}

		drain_pending_free();
	}

	close(listen_fd);
	close(g_epfd);
	printf("kanxeod shutting down\n");
	fflush(stdout);

	/* As real PID 1 (--init-mode), a bare `return 0` here is exactly
	 * "init exited" -- the kernel panics unconditionally regardless of
	 * how gracefully kanxeod itself shut down first. reboot(2) is the
	 * actual, correct way for an init process to end its own life; a
	 * dev/test invocation (no --init-mode, e.g. every test/*.c fork+
	 * execve) is never PID 1 and just returns normally, exactly as it
	 * always has -- reboot(2) is never reachable from there regardless
	 * of what set g_shutdown_action (SIGTERM/SIGINT default to
	 * SHUTDOWN_ACTION_POWEROFF; the same tests already send kanxeod
	 * SIGTERM to end sessions today). */
	if (init_mode) {
		sync();
		reboot(g_shutdown_action == SHUTDOWN_ACTION_REBOOT ? RB_AUTOBOOT : RB_POWER_OFF);
		perror("reboot");
		return 1;
	}
	return 0;
}
