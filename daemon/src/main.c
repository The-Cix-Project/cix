#include "container.h"
#include "containerdef.h"
#include "device.h"
#include "dns.h"
#include "http.h"
#include "image.h"
#include "json.h"
#include "linux_compat.h"
#include "namecheck.h"
#include "network.h"
#include "pki.h"
#include "pkg.h"
#include "registry.h"
#include "rtnetlink.h"
#include "staticfile.h"

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
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 7620
#define DEFAULT_BIND "127.0.0.1"
#define DEFAULT_WEB_ROOT "web"
#define BASE_DIR "/var/lib/kanxeo"
#define IMAGES_DIR BASE_DIR "/images"
#define CONTAINERS_DIR BASE_DIR "/containers"
#define NETWORKS_STATE_PATH BASE_DIR "/networks.json"
#define DNS_RECORDS_STATE_PATH BASE_DIR "/dns_records.json"
#define PKI_DIR BASE_DIR "/pki"
#define PKI_CERTS_STATE_PATH PKI_DIR "/pki_certs.json"
#define PKG_DIR BASE_DIR "/pkg"
#define PKG_INSTALLED_STATE_PATH PKG_DIR "/pkg_installed.json"
#define CONTAINER_DEFS_STATE_PATH BASE_DIR "/container_defs.json"
/*
 * Fixed, daemon-wide, not per-service (YAGNI for v1) -- how long to
 * wait before restarting a restart:"always" container after an
 * unprompted exit. Real, deliberate: without this, a genuinely
 * crash-looping container would restart as fast as clone3()+execve()
 * itself allows, a real operational hazard (CPU/log hammering) a
 * single fixed delay is enough to prevent without a full backoff
 * policy engine.
 */
#define CONTAINER_RESTART_DELAY_SECONDS 2
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
#define IMAGES_PREFIX "/v1/images/"

enum conn_kind { CONN_LISTENER, CONN_CLIENT, CONN_CONTAINER, CONN_PKG_FETCH, CONN_RESTART_TIMER };

struct conn {
	enum conn_kind kind;
	int fd;
	struct http_conn http;                 /* CONN_CLIENT only */
	struct registry_entry *entry;           /* CONN_CONTAINER only */
	pid_t pkg_fetch_pid;                    /* CONN_PKG_FETCH only */
	char restart_name[REGISTRY_NAME_MAX];   /* CONN_RESTART_TIMER only */
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
	if (mount(CONTAINERS_DEVICE, BASE_DIR, "ext4", MS_NOSUID | MS_NODEV, NULL) != 0 &&
	    mount_or_fail("tmpfs", BASE_DIR, "tmpfs", MS_NOSUID | MS_NODEV) != 0)
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
 * Core of what POST /v1/containers does: parses+validates body,
 * builds a container_spec, calls registry_create(), calls
 * register_container_pidfd() on success (do NOT also call it at any
 * call site below -- double-registering the same pidfd with epoll is
 * a real bug, confirmed directly: it aborts the daemon), and runs the
 * requested dns_register/pki_issue side effects -- so replaying the
 * exact same body (boot autostart, crash restart, see
 * daemon/include/containerdef.h) gets the exact same result a fresh
 * POST would. Zero HTTP coupling. On success returns 0 and *out_entry
 * is the new registry entry; *out_restart_always/out_depends_on(_count)
 * are filled from the body's own "restart"/"depends_on" fields
 * regardless of outcome (a caller deciding what to persist needs them
 * either way, though only a success is ever actually persisted). On
 * failure returns the same HTTP status code (400/409/500)
 * handle_create() has always returned for that condition, with
 * err_msg holding the exact same message text -- the REST wrapper
 * forwards both verbatim; boot/restart callers just log them.
 */
static int create_container_from_body(const char *body, size_t body_len,
                                       struct registry_entry **out_entry, int *out_restart_always,
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
	const struct json_value *jrestart, *jdepends_on, *jreadiness;
	const char *restart_str;
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
	const struct json_value *jdns_register;
	int dns_register = 0;
	const struct json_value *jpki_issue, *jpki_cert_dir, *jpki_days;
	int pki_issue = 0;
	char pki_cert_dir_buf[PATH_MAX];
	int pki_days = 365;

	*out_restart_always = 0;
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
	    strcmp(restart_str, "no") != 0) {
		json_free(root);
		snprintf(err_msg, err_msg_size, "restart must be \"always\" or \"no\"");
		return 400;
	}
	*out_restart_always = (restart_str != NULL && strcmp(restart_str, "always") == 0);

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
		device_count = (int)jdevices->u.array.count;
		for (i = 0; i < (size_t)device_count; i++) {
			const char *id = json_as_string(jdevices->u.array.items[i]);
			const struct discovered_device *dd;

			if (id == NULL) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "devices entries must be strings");
				return 400;
			}
			dd = device_find(id);
			if (dd == NULL || !dd->assignable) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "unknown or unassignable device");
				return 400;
			}
			/* dev_path/major/minor always come from the daemon's own
			 * current sysfs snapshot (dd), never trusted from the
			 * request body -- a client only ever names a device by id. */
			memset(&device_specs[i], 0, sizeof(device_specs[i]));
			device_specs[i].type = dd->type;
			device_specs[i].major = dd->major;
			device_specs[i].minor = dd->minor;
			snprintf(device_specs[i].dev_path, sizeof(device_specs[i].dev_path), "%s",
			         dd->dev_path);
			memset(&device_attachments[i], 0, sizeof(device_attachments[i]));
			snprintf(device_attachments[i].id, sizeof(device_attachments[i].id), "%s", dd->id);
			snprintf(device_attachments[i].dev_path, sizeof(device_attachments[i].dev_path),
			         "%s", dd->dev_path);
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
		spec.nets[i].gateway_ip_be = net->gateway_be;
		spec.nets[i].prefix_len = net->prefix_len;
	}
	spec.ip_forward = ip_forward;
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
	                        device_attachments, device_count, &entry);
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

		if (perr != PKI_OK) {
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
	int restart_always;
	char depends_on[CONTAINERDEF_MAX_DEPENDS][REGISTRY_NAME_MAX];
	int depends_on_count;
	int has_readiness, readiness_tcp_port, readiness_timeout_seconds;
	char err_msg[256];
	int status;
	struct json_writer w;

	status = create_container_from_body(body, body_len, &entry, &restart_always, depends_on,
	                                     &depends_on_count, &has_readiness, &readiness_tcp_port,
	                                     &readiness_timeout_seconds, err_msg, sizeof(err_msg));
	if (status != 0) {
		respond_error(fd, status, http_status_text(status), err_msg);
		return;
	}

	if (restart_always &&
	    containerdef_add(entry->name, body, body_len, depends_on, depends_on_count, has_readiness,
	                      readiness_tcp_port, readiness_timeout_seconds) != 0) {
		fprintf(stderr,
		        "%s: restart:\"always\" requested but persisting its definition failed -- "
		        "it will not survive a daemon restart\n",
		        entry->name);
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
	const char *name, *subnet;
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

	if (name == NULL || subnet == NULL || jprefix == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name/subnet/prefix_len missing");
		return;
	}
	prefix_len = (int)json_as_number(jprefix);

	nerr = network_create(name, subnet, prefix_len, &net);
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
	case PKG_ERR_SPAWN_FAILED:
	case PKG_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "package operation failed");
		break;
	}
}

static void handle_pkg_bootstrap(int fd)
{
	enum pkg_error perr = pkg_bootstrap_build_image();

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
	 * These three reserved paths are checked before the generic
	 * PKG_PREFIX/{name} fallback below, exactly like every other
	 * resource's exact-match-then-prefix ordering in this dispatch --
	 * a package named "bootstrap"/"recipes"/"install" would be
	 * unreachable via GET/DELETE /v1/pkg/{name}, a deliberate,
	 * documented reserved-words boundary (recipes are operator-
	 * provisioned out of band, easily avoided in practice).
	 */
	if (strcmp(req->path, "/v1/pkg/bootstrap") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_pkg_bootstrap(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg/recipes") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_recipes_list(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg/install") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_pkg_install(fd, req->body, req->body_len);
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
		dispatch(cc->fd, &req);
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		close(cc->fd);
		http_conn_free(&cc->http);
		free(cc);
	}
	/* pr == 0: request incomplete, keep waiting on this fd. */
}

/*
 * Arms a one-shot, non-blocking timer (CONTAINER_RESTART_DELAY_SECONDS
 * from now) that, once it fires, replays name's own persisted
 * definition through create_container_from_body() again -- the
 * reactor's first-ever use of a timer, deliberately isolated in a
 * throwaway timerfd + CONN_RESTART_TIMER conn rather than a blocking
 * sleep(), which would freeze every other in-flight request/event for
 * the whole delay (this daemon's entire event loop is single-
 * threaded and non-blocking by design). A failure to arm is logged
 * and simply means this one restart doesn't happen -- not fatal to
 * the daemon.
 */
static void arm_restart_timer(const char *name)
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
	its.it_value.tv_sec = CONTAINER_RESTART_DELAY_SECONDS;
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
 * to restart.
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
	if (def != NULL) {
		struct registry_entry *entry;
		int restart_always;
		char depends_on[CONTAINERDEF_MAX_DEPENDS][REGISTRY_NAME_MAX];
		int depends_on_count;
		/* Readiness is boot-autostart-only (see containerdef_autostart_all()'s
		 * own comment on why) -- these are validated/extracted the same
		 * way regardless of caller, but a crash restart never acts on
		 * them. */
		int has_readiness, readiness_tcp_port, readiness_timeout_seconds;
		char err_msg[256];
		int status = create_container_from_body(
		    def->body, def->body_len, &entry, &restart_always, depends_on, &depends_on_count,
		    &has_readiness, &readiness_tcp_port, &readiness_timeout_seconds, err_msg,
		    sizeof(err_msg));

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
	pid_t pkg_pid;
	int pkg_pidfd;
	int chained;

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	registry_mark_exited(entry);
	entry->reactor_conn = NULL;
	free(cc);

	/* Copied before any registry_remove() below might reuse this
	 * slot -- entry->name itself is only guaranteed valid until then. */
	snprintf(name_copy, sizeof(name_copy), "%s", entry->name);

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
	 * An unprompted exit (never reached for an explicit DELETE, which
	 * removes the pidfd from epoll itself before this event could ever
	 * fire -- see registry_remove()'s own doc comment) of a
	 * restart:"always" container. registry_remove() is safe here even
	 * though entry->running is already 0 (its own kill/reap branch is
	 * skipped, so no signal is sent to a possibly-already-reused pid)
	 * -- frees the name/slot before the delayed restart re-creates it.
	 */
	if (containerdef_find(name_copy) != NULL) {
		registry_remove(name_copy);
		arm_restart_timer(name_copy);
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
		    registry_create(PKG_BUILD_CONTAINER_NAME, "pkgbuild", &spec, NULL, 0, 0, NULL, 0,
		                     &entry);

		if (rerr != REGISTRY_OK)
			pkg_build_spawn_failed();
		else
			register_container_pidfd(entry);
	}
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
 * Starts every persisted restart:"always" definition, in dependency
 * order -- called once, right after confirm_boot() (never before: see
 * that function's own comment on why "healthy" must stay defined as
 * "about to serve traffic" alone, never coupled to whether every
 * container also happened to start cleanly). Runs unconditionally,
 * not just under --init-mode -- this is "whenever this daemon process
 * starts fresh" behavior, not specific to the installed A/B boot
 * flow. Blocking, like every other startup step already is
 * (network_init(), pkg_init(), ...); the kernel's own listen backlog
 * queues any incoming connection during this window, none are
 * dropped. A single definition failing to start is logged and
 * skipped, never fatal to the rest of boot.
 */
static void containerdef_autostart_all(void)
{
	char order[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	int count = containerdef_resolve_order(order);
	int i;

	for (i = 0; i < count; i++) {
		struct container_def *def = containerdef_find(order[i]);
		struct registry_entry *entry;
		int restart_always;
		char depends_on[CONTAINERDEF_MAX_DEPENDS][REGISTRY_NAME_MAX];
		int depends_on_count;
		int has_readiness, readiness_tcp_port, readiness_timeout_seconds;
		char err_msg[256];
		int status;

		if (def == NULL)
			continue; /* can't happen -- resolve_order() only ever names known defs */

		status = create_container_from_body(def->body, def->body_len, &entry, &restart_always,
		                                     depends_on, &depends_on_count, &has_readiness,
		                                     &readiness_tcp_port, &readiness_timeout_seconds,
		                                     err_msg, sizeof(err_msg));
		if (status != 0) {
			fprintf(stderr, "%s: autostart failed: %s\n", order[i], err_msg);
			continue;
		}
		/* create_container_from_body() already registered entry's own
		 * pidfd with epoll -- exactly the same shape POST /v1/containers'
		 * own success path relies on, no separate call needed here. */

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

		printf("%s: autostarted (restart:always)\n", entry->name);
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
	}
	g_web_root = web_root;

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

	if (ensure_dir(BASE_DIR) != 0 || ensure_dir(IMAGES_DIR) != 0 ||
	    ensure_dir(CONTAINERS_DIR) != 0 || ensure_dir(PKI_DIR) != 0 ||
	    ensure_dir(PKI_DIR "/certs") != 0 || ensure_dir(PKG_DIR) != 0)
		return 1;

	if (network_init(NETWORKS_STATE_PATH) != 0)
		return 1;
	if (dns_init(DNS_RECORDS_STATE_PATH) != 0)
		return 1;
	if (pki_init(PKI_DIR, PKI_CERTS_STATE_PATH) != 0)
		return 1;
	if (pkg_init(PKG_DIR, PKG_INSTALLED_STATE_PATH, CONTAINERS_DIR, IMAGES_DIR) != 0)
		return 1;
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
			if (cc->kind == CONN_LISTENER)
				accept_loop();
			else if (cc->kind == CONN_CONTAINER)
				handle_container_event(cc);
			else if (cc->kind == CONN_PKG_FETCH)
				handle_pkg_fetch_event(cc);
			else if (cc->kind == CONN_RESTART_TIMER)
				handle_restart_timer_event(cc);
			else
				handle_client_event(cc);
		}
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
