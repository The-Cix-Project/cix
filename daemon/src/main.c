#include "connthrottle.h"
#include "container.h"
#include "containerdef.h"
#include "device.h"
#include "devicemap.h"
#include "disk.h"
#include "diskformat.h"
#include "diskrole.h"
#include "storagemigrate.h"
#include "containerstoragemigrate.h"
#include "backupconfig.h"
#include "hostauth.h"
#include "storageplacement.h"
#include "hostproc.h"
#include "logstore.h"
#include "ntp.h"
#include "ping.h"
#include "resolv.h"
#include "swap.h"
#include "syslogfwd.h"
#include "dns.h"
#include "ldap.h"
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
#include "quotamap.h"
#include "registry.h"
#include "rtnetlink.h"
#include "siteconfig.h"
#include "daemon_config.h"
#include "tlsconn.h"
#include "version.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include "staticfile.h"
#include "websocket.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
/* See daemon/src/logstore.c's own include-block comment: TCC can't
 * parse glibc's real <regex.h> regexec() prototype without this. */
#define __STDC_NO_VLA__ 1
#include <regex.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/quota.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/timerfd.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define DEFAULT_PORT 80
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
/*
 * ADR-0141: two grouping directories introduced so state-storage and
 * rebuildable-storage can each be relocated to a different disk as one
 * real directory move, rather than a scattered list of a dozen
 * unrelated paths -- every path below that's conceptually "this
 * platform's own definition of itself" nests under STATE_DIR; every
 * path that's "regenerable from recipes/sources if lost" nests under
 * REBUILDABLE_DIR. CONTAINERS_DIR/SWAP_DIR/DISKS_MOUNT_DIR deliberately
 * stay direct children of g_base_dir -- CONTAINERS_DIR already has its
 * own relocation mechanism (container-storage, ADR-0102), SWAP_DIR is
 * explicitly out of scope (ADR-0069's own separate mechanism), and
 * DISKS_MOUNT_DIR holds mount points for other disks, not data of its
 * own. LOG_DIR is ALSO independently relocatable (log-storage, ADR-0141
 * Phase 3) -- it doesn't need STATE_DIR/REBUILDABLE_DIR's own grouping
 * treatment since it already was its own single, clean subdirectory
 * with exactly one consumer (logstore.c), unlike the dozen-plus
 * unrelated things STATE_DIR groups.
 */
static char STATE_DIR[PATH_MAX];
static char REBUILDABLE_DIR[PATH_MAX];
static char IMAGES_DIR[PATH_MAX];
static char CONTAINERS_DIR[PATH_MAX];
static char NETWORKS_STATE_PATH[PATH_MAX];
static char DNS_RECORDS_STATE_PATH[PATH_MAX];
static char DNS_SERVERS_STATE_PATH[PATH_MAX];
static char LDAP_SERVERS_STATE_PATH[PATH_MAX];
static char LDAP_USERS_STATE_PATH[PATH_MAX];
static char LDAP_GROUPS_STATE_PATH[PATH_MAX];
static char LDAP_CONFIG_STATE_PATH[PATH_MAX];
static char PKI_DIR[PATH_MAX];
static char PKI_CERTS_STATE_PATH[PATH_MAX];
static char PKI_CERTS_DIR[PATH_MAX];
static char PKG_DIR[PATH_MAX];
static char PKG_INSTALLED_STATE_PATH[PATH_MAX];
static char PKG_RECIPES_DIR[PATH_MAX];
static char PKG_REPO_CONFIG_PATH[PATH_MAX]; /* ADR-0121 */
static char PKG_CACHE_DIR[PATH_MAX];              /* ADR-0122 */
static char PKG_CACHE_CONFIG_PATH[PATH_MAX];      /* ADR-0122 */
static char PKG_ARTIFACT_CONFIG_PATH[PATH_MAX];   /* ADR-0122 */
/* Where a hostbuild job's own harvested output lands (ADR-0056) --
 * ARTIFACTS_DIR/<name>/..., a plain host directory, never a container-
 * visible path. */
static char ARTIFACTS_DIR[PATH_MAX];
static char CONTAINER_DEFS_STATE_PATH[PATH_MAX];
static char ROLLING_CONFIG_PATH[PATH_MAX]; /* ADR-0124 */
static char SITE_CONFIG_PATH[PATH_MAX];
static char DEVICEMAP_STATE_PATH[PATH_MAX];
static char DISKROLE_STATE_PATH[PATH_MAX];
/* ADR-0141 Phase 2: which disk (if any) is the active placement for
 * state-storage/rebuildable-storage/log-storage -- g_base_dir-relative,
 * never STATE_DIR-relative, same bootstrap-circularity reasoning as
 * DISKROLE_STATE_PATH's own comment. */
static char STORAGE_PLACEMENT_PATH[PATH_MAX];
static char DAEMON_CONFIG_PATH[PATH_MAX];
static char QUOTAMAP_STATE_PATH[PATH_MAX];
/*
 * Where POST /v1/system/iso (ADR-0064) looks for the Secure Boot
 * signing key pair and writes its own output -- both new with this
 * mechanism, since building a *new* installer ISO server-side is the
 * first time kanxeod itself, rather than a dev machine's own manual
 * mkinstalleriso invocation, has ever needed either. SIGNING_KEYS_DIR
 * is deliberately operator-populated out of band (never fetched,
 * generated, or copied here by kanxeod itself, and never staged onto
 * any container image or target-disk install) -- the same real
 * security posture image/keys/ already has in this repo: a release-
 * signing private key must never propagate onto every deployed box,
 * only the specific build/release instance actually cutting installer
 * media. A box with nothing at SIGNING_KEYS_DIR simply can't serve
 * this endpoint, exactly like `pkg hostbuild kanxeo` can't complete
 * without a real git token -- a real, environment-specific
 * precondition, not a gap.
 */
static char SIGNING_KEYS_DIR[PATH_MAX];
static char ISO_DIR[PATH_MAX];
/* POST /v1/pkg/bootstrap's own toolchain_url mode (ADR-0065) -- a
 * fixed scratch path for the curl'd artifact, same "one fixed spot,
 * overwritten each time" convention ISO_OUTPUT_PATH already uses. */
static char PKGBUILD_TOOLCHAIN_FETCH_PATH[PATH_MAX];
/* ADR-0069: a single host-level swap file, off by default, enabled
 * on demand via POST /v1/system/swap. SWAP_DIR keeps the file and its
 * tiny persisted enabled/size state together, out of g_base_dir's own
 * root (the same "own subdirectory per subsystem" convention PKI_DIR/
 * PKG_DIR already use). */
static char SWAP_DIR[PATH_MAX];
static char SWAP_FILE_PATH[PATH_MAX];
static char SWAP_STATE_PATH[PATH_MAX];
/* Consolidated log store (kernel dmesg + kanxeod's own diagnostics +
 * a per-request audit trail, ADR-0070) -- its own subdirectory,
 * matching every other subsystem's "own directory under the base
 * data dir" convention (PKI_DIR/PKG_DIR/SWAP_DIR above). */
static char LOG_DIR[PATH_MAX];
static char LOG_STATE_PATH[PATH_MAX];
/* Multi-disk management Phase C: where an assigned-role disk gets
 * mounted once formatted (diskformat.h) -- <name> under here, e.g.
 * DISKS_MOUNT_DIR/sdb. Deliberately NOT CONTAINERS_DIR itself; Phase D
 * (container-storage migration) is the still-unbuilt mechanism that
 * would actually move container storage onto a mounted disk like this
 * one -- formatting/mounting alone never touches CONTAINERS_DIR. */
static char DISKS_MOUNT_DIR[PATH_MAX];
/* ADR-0076: the host's own outbound DNS resolver config, persisted in
 * literal resolv.conf format (not JSON) -- this file IS what a real
 * --init-mode boot bind-mounts onto /etc/resolv.conf, so writing it
 * via resolv_set() takes effect immediately, no reboot needed. */
static char RESOLV_CONF_PATH[PATH_MAX];
/* NTP (task #751-755): the host's own upstream server address list
 * (GET/PUT /v1/system/ntp) and registered NTP-serving container
 * bindings (POST/GET/DELETE /v1/ntp/servers) -- two independent
 * persisted files, same dual-path convention dns_init()'s own
 * records/servers pair already uses. */
static char NTP_STATE_PATH[PATH_MAX];
static char NTP_SERVERS_STATE_PATH[PATH_MAX];
static char SYSLOGFWD_STATE_PATH[PATH_MAX];
/* Per-source-IP HTTPS-handshake-failure throttling config (GET/PUT
 * /v1/system/tls-throttle) -- the tracked-peer table itself is
 * in-memory only, never persisted, same posture as every other
 * transient in-flight daemon state. */
static char CONNTHROTTLE_CONFIG_PATH[PATH_MAX];
/* ADR-0141 Phase 5: persisted disk/enabled/interval_hours config for
 * automatic state-storage backup snapshots (GET/PUT /v1/system/
 * backup-config). */
static char BACKUP_CONFIG_PATH[PATH_MAX];
/* ADR-0144: persisted admin-group/idle-timeout config for host
 * authentication (GET/PUT /v1/system/hostauth-config). */
static char HOSTAUTH_CONFIG_PATH[PATH_MAX];

/* Computes every path derived from g_base_dir -- called once, right
 * after argv parsing (so --data-dir= has already been applied) and
 * before anything (including boot_init(), which mounts the real
 * containers partition at g_base_dir under --init-mode) touches any
 * of them. */
/*
 * Every path that's STATE_DIR-relative (directly or transitively, e.g.
 * PKI_CERTS_DIR via PKI_DIR) -- split out of init_base_dir_paths() so
 * it can be re-run a second time (ADR-0141 Phase 2's own
 * resolve_state_storage_placement(), below) once STATE_DIR's real,
 * possibly-relocated value is known, without duplicating this list of
 * snprintf() calls. Callable any number of times; always recomputes
 * every one of these paths from STATE_DIR's *current* value.
 */
static void compute_state_dir_relative_paths(void)
{
	snprintf(NETWORKS_STATE_PATH, sizeof(NETWORKS_STATE_PATH), "%s/networks.json", STATE_DIR);
	snprintf(DNS_RECORDS_STATE_PATH, sizeof(DNS_RECORDS_STATE_PATH), "%s/dns_records.json", STATE_DIR);
	snprintf(DNS_SERVERS_STATE_PATH, sizeof(DNS_SERVERS_STATE_PATH), "%s/dns_servers.json", STATE_DIR);
	snprintf(LDAP_SERVERS_STATE_PATH, sizeof(LDAP_SERVERS_STATE_PATH), "%s/ldap_servers.json",
	         STATE_DIR);
	snprintf(LDAP_USERS_STATE_PATH, sizeof(LDAP_USERS_STATE_PATH), "%s/ldap_users.json", STATE_DIR);
	snprintf(LDAP_GROUPS_STATE_PATH, sizeof(LDAP_GROUPS_STATE_PATH), "%s/ldap_groups.json",
	         STATE_DIR);
	snprintf(LDAP_CONFIG_STATE_PATH, sizeof(LDAP_CONFIG_STATE_PATH), "%s/ldap_config.json",
	         STATE_DIR);
	snprintf(PKI_DIR, sizeof(PKI_DIR), "%s/pki", STATE_DIR);
	snprintf(PKI_CERTS_STATE_PATH, sizeof(PKI_CERTS_STATE_PATH), "%s/pki_certs.json", PKI_DIR);
	snprintf(PKI_CERTS_DIR, sizeof(PKI_CERTS_DIR), "%s/certs", PKI_DIR);
	snprintf(CONTAINER_DEFS_STATE_PATH, sizeof(CONTAINER_DEFS_STATE_PATH), "%s/container_defs.json", STATE_DIR);
	snprintf(ROLLING_CONFIG_PATH, sizeof(ROLLING_CONFIG_PATH), "%s/rolling_config.json", STATE_DIR);
	snprintf(SITE_CONFIG_PATH, sizeof(SITE_CONFIG_PATH), "%s/site_config.json", STATE_DIR);
	snprintf(DEVICEMAP_STATE_PATH, sizeof(DEVICEMAP_STATE_PATH), "%s/devicemaps.json", STATE_DIR);
	snprintf(DAEMON_CONFIG_PATH, sizeof(DAEMON_CONFIG_PATH), "%s/daemon_config.json", STATE_DIR);
	snprintf(QUOTAMAP_STATE_PATH, sizeof(QUOTAMAP_STATE_PATH), "%s/quota_projids.json", STATE_DIR);
	snprintf(SIGNING_KEYS_DIR, sizeof(SIGNING_KEYS_DIR), "%s/keys", STATE_DIR);
	snprintf(RESOLV_CONF_PATH, sizeof(RESOLV_CONF_PATH), "%s/resolv.conf", STATE_DIR);
	snprintf(NTP_STATE_PATH, sizeof(NTP_STATE_PATH), "%s/ntp.conf", STATE_DIR);
	snprintf(NTP_SERVERS_STATE_PATH, sizeof(NTP_SERVERS_STATE_PATH), "%s/ntp_servers.json", STATE_DIR);
	snprintf(SYSLOGFWD_STATE_PATH, sizeof(SYSLOGFWD_STATE_PATH), "%s/syslog_targets.json", STATE_DIR);
	snprintf(CONNTHROTTLE_CONFIG_PATH, sizeof(CONNTHROTTLE_CONFIG_PATH), "%s/tls_throttle.json", STATE_DIR);
	snprintf(BACKUP_CONFIG_PATH, sizeof(BACKUP_CONFIG_PATH), "%s/backup_config.json", STATE_DIR);
	snprintf(HOSTAUTH_CONFIG_PATH, sizeof(HOSTAUTH_CONFIG_PATH), "%s/hostauth_config.json", STATE_DIR);
}

/*
 * Every path that's REBUILDABLE_DIR-relative (directly or
 * transitively, e.g. PKG_INSTALLED_STATE_PATH via PKG_DIR) -- split
 * out of init_base_dir_paths() for the identical reason compute_
 * state_dir_relative_paths() was (ADR-0141 Phase 4's own resolve_
 * rebuildable_storage_placement(), below): re-runnable once
 * REBUILDABLE_DIR's real, possibly-relocated value is known.
 */
static void compute_rebuildable_dir_relative_paths(void)
{
	snprintf(IMAGES_DIR, sizeof(IMAGES_DIR), "%s/images", REBUILDABLE_DIR);
	snprintf(PKG_DIR, sizeof(PKG_DIR), "%s/pkg", REBUILDABLE_DIR);
	snprintf(PKG_INSTALLED_STATE_PATH, sizeof(PKG_INSTALLED_STATE_PATH), "%s/pkg_installed.json", PKG_DIR);
	snprintf(PKG_RECIPES_DIR, sizeof(PKG_RECIPES_DIR), "%s/recipes", PKG_DIR);
	snprintf(PKG_REPO_CONFIG_PATH, sizeof(PKG_REPO_CONFIG_PATH), "%s/repo_config.json", PKG_DIR);
	snprintf(PKG_CACHE_DIR, sizeof(PKG_CACHE_DIR), "%s/cache", PKG_DIR);
	snprintf(PKG_CACHE_CONFIG_PATH, sizeof(PKG_CACHE_CONFIG_PATH), "%s/cache_config.json", PKG_DIR);
	snprintf(PKG_ARTIFACT_CONFIG_PATH, sizeof(PKG_ARTIFACT_CONFIG_PATH), "%s/artifact_config.json",
	         PKG_DIR);
	snprintf(ARTIFACTS_DIR, sizeof(ARTIFACTS_DIR), "%s/artifacts", REBUILDABLE_DIR);
	snprintf(ISO_DIR, sizeof(ISO_DIR), "%s/iso", REBUILDABLE_DIR);
	snprintf(PKGBUILD_TOOLCHAIN_FETCH_PATH, sizeof(PKGBUILD_TOOLCHAIN_FETCH_PATH),
	         "%s/bootstrap_toolchain.squashfs", PKG_DIR);
}

static void init_base_dir_paths(void)
{
	snprintf(STATE_DIR, sizeof(STATE_DIR), "%s/state", g_base_dir);
	snprintf(REBUILDABLE_DIR, sizeof(REBUILDABLE_DIR), "%s/rebuildable", g_base_dir);

	snprintf(CONTAINERS_DIR, sizeof(CONTAINERS_DIR), "%s/containers", g_base_dir);
	/*
	 * Deliberately g_base_dir-relative, NOT STATE_DIR-relative (ADR-0141
	 * Phase 2 correction) -- disk role assignments are bootstrap-level
	 * data needed to even determine *where* STATE_DIR itself lives once
	 * state-storage becomes a relocatable, disk-role-driven placement:
	 * putting diskroles.json inside STATE_DIR would make "which disk is
	 * state-storage on" depend on reading a file that might itself be on
	 * that same disk, a real circular dependency caught before it ever
	 * shipped as a working migration. STORAGE_PLACEMENT_PATH (below)
	 * needs the same fixed treatment for the identical reason. */
	snprintf(DISKROLE_STATE_PATH, sizeof(DISKROLE_STATE_PATH), "%s/diskroles.json", g_base_dir);
	snprintf(STORAGE_PLACEMENT_PATH, sizeof(STORAGE_PLACEMENT_PATH), "%s/storage_placement.json",
	         g_base_dir);
	snprintf(SWAP_DIR, sizeof(SWAP_DIR), "%s/swap", g_base_dir);
	snprintf(SWAP_FILE_PATH, sizeof(SWAP_FILE_PATH), "%s/swapfile", SWAP_DIR);
	snprintf(SWAP_STATE_PATH, sizeof(SWAP_STATE_PATH), "%s/state.json", SWAP_DIR);
	snprintf(LOG_DIR, sizeof(LOG_DIR), "%s/logs", g_base_dir);
	snprintf(LOG_STATE_PATH, sizeof(LOG_STATE_PATH), "%s/state.json", LOG_DIR);
	snprintf(DISKS_MOUNT_DIR, sizeof(DISKS_MOUNT_DIR), "%s/disks", g_base_dir);

	compute_state_dir_relative_paths();
	compute_rebuildable_dir_relative_paths();
}

/*
 * ADR-0141 Phase 2: called once at boot, after diskrole_init() and
 * diskformat_remount_present_role_disks() have run (this needs both --
 * knowing state-storage's assigned disk, and that disk actually being
 * mounted) but before ensure_dir()/any subsystem _init() reads or
 * writes a STATE_DIR-relative path. If storageplacement_get(STORAGE_
 * KIND_STATE) names a disk, and disk_enumerate() confirms it's
 * currently mounted, STATE_DIR is repointed to that disk's own mount
 * path and every STATE_DIR-relative path is recomputed to match --
 * otherwise STATE_DIR is left at init_base_dir_paths()'s own default
 * (g_base_dir/state), unchanged.
 *
 * Deliberately fails loud (returns -1, main() treats this exactly like
 * any other _init() failure) rather than silently falling back to the
 * default location when a configured placement disk isn't currently
 * available -- falling back would mean starting this daemon against a
 * STATE_DIR that's either empty or stale, which is a far worse outcome
 * than refusing to boot with a clear, actionable error (reattach the
 * disk, or manually clear the placement via a future recovery path).
 */
static int resolve_state_storage_placement(void)
{
	const char *disk_name = storageplacement_get(STORAGE_KIND_STATE);
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n, i;

	if (disk_name == NULL)
		return 0; /* default OS-disk placement -- nothing to do */

	n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
	for (i = 0; i < n; i++) {
		if (strcmp(disks[i].name, disk_name) != 0)
			continue;
		if (!disks[i].mounted) {
			fprintf(stderr,
			        "resolve_state_storage_placement: state-storage's configured disk "
			        "'%s' is present but not currently mounted -- refusing to start\n",
			        disk_name);
			return -1;
		}
		snprintf(STATE_DIR, sizeof(STATE_DIR), "%s/%s/state", DISKS_MOUNT_DIR, disk_name);
		compute_state_dir_relative_paths();
		return 0;
	}
	fprintf(stderr,
	        "resolve_state_storage_placement: state-storage's configured disk '%s' is not "
	        "currently present -- refusing to start\n",
	        disk_name);
	return -1;
}

/*
 * ADR-0141 Phase 3: same shape and same "fail loud, never silently
 * fall back" reasoning as resolve_state_storage_placement() above, for
 * LOG_DIR -- log-storage's own single-consumer relocation (logstore.c
 * only) needs no compute_*_relative_paths()-style helper, just its one
 * dependent path (LOG_STATE_PATH) recomputed alongside it.
 */
static int resolve_log_storage_placement(void)
{
	const char *disk_name = storageplacement_get(STORAGE_KIND_LOG);
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n, i;

	if (disk_name == NULL)
		return 0; /* default OS-disk placement -- nothing to do */

	n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
	for (i = 0; i < n; i++) {
		if (strcmp(disks[i].name, disk_name) != 0)
			continue;
		if (!disks[i].mounted) {
			fprintf(stderr,
			        "resolve_log_storage_placement: log-storage's configured disk "
			        "'%s' is present but not currently mounted -- refusing to start\n",
			        disk_name);
			return -1;
		}
		snprintf(LOG_DIR, sizeof(LOG_DIR), "%s/%s/logs", DISKS_MOUNT_DIR, disk_name);
		snprintf(LOG_STATE_PATH, sizeof(LOG_STATE_PATH), "%s/state.json", LOG_DIR);
		return 0;
	}
	fprintf(stderr,
	        "resolve_log_storage_placement: log-storage's configured disk '%s' is not "
	        "currently present -- refusing to start\n",
	        disk_name);
	return -1;
}

/*
 * ADR-0141 Phase 4: same shape as resolve_state_storage_placement()/
 * resolve_log_storage_placement() above, for REBUILDABLE_DIR --
 * recomputes every REBUILDABLE_DIR-relative path via compute_
 * rebuildable_dir_relative_paths() once REBUILDABLE_DIR itself is
 * resolved, same "fail loud, never silently fall back" posture.
 */
static int resolve_rebuildable_storage_placement(void)
{
	const char *disk_name = storageplacement_get(STORAGE_KIND_REBUILDABLE);
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n, i;

	if (disk_name == NULL)
		return 0; /* default OS-disk placement -- nothing to do */

	n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
	for (i = 0; i < n; i++) {
		if (strcmp(disks[i].name, disk_name) != 0)
			continue;
		if (!disks[i].mounted) {
			fprintf(stderr,
			        "resolve_rebuildable_storage_placement: rebuildable-storage's "
			        "configured disk '%s' is present but not currently mounted -- "
			        "refusing to start\n",
			        disk_name);
			return -1;
		}
		snprintf(REBUILDABLE_DIR, sizeof(REBUILDABLE_DIR), "%s/%s/rebuildable", DISKS_MOUNT_DIR,
		         disk_name);
		compute_rebuildable_dir_relative_paths();
		return 0;
	}
	fprintf(stderr,
	        "resolve_rebuildable_storage_placement: rebuildable-storage's configured disk "
	        "'%s' is not currently present -- refusing to start\n",
	        disk_name);
	return -1;
}

/*
 * ADR-0141: moves one old-flat-layout entry (a file or a whole
 * directory -- rename(2) handles either) from directly under
 * g_base_dir into new_dir, if it still exists there. A no-op
 * (lstat() fails ENOENT) on a box that's already upgraded, or a
 * genuinely fresh install with nothing at the old flat path -- this
 * makes migrate_flat_layout_to_grouped() below safe to call
 * unconditionally on every single boot, with no separate "have I
 * already run" flag needed.
 */
static void migrate_one_flat_entry(const char *basename, const char *new_dir)
{
	char old_path[PATH_MAX];
	char new_path[PATH_MAX];
	struct stat st;

	snprintf(old_path, sizeof(old_path), "%s/%s", g_base_dir, basename);
	if (lstat(old_path, &st) != 0)
		return;

	if (persist_mkdir_p(new_dir) != 0) {
		fprintf(stderr, "migrate_flat_layout_to_grouped: mkdir %s failed: %s\n", new_dir,
		        strerror(errno));
		return;
	}
	snprintf(new_path, sizeof(new_path), "%s/%s", new_dir, basename);
	if (rename(old_path, new_path) != 0)
		fprintf(stderr, "migrate_flat_layout_to_grouped: rename %s -> %s failed: %s\n", old_path,
		        new_path, strerror(errno));
}

/*
 * One-time upgrade path (ADR-0141) for a box that ran before STATE_DIR/
 * REBUILDABLE_DIR existed -- its real state sits at the OLD flat paths,
 * directly under g_base_dir, since every one of them used to be
 * computed that way. Must run after boot_init() has mounted the real
 * containers partition onto g_base_dir under --init-mode (calling this
 * any earlier would see an empty pre-mount directory and migrate
 * nothing) and before anything below -- the ensure_dir() calls that
 * would otherwise create fresh empty grouped directories, and every
 * subsystem's own _init() -- ever reads or writes any of these paths.
 * rename(2), not a copy: both the old and new locations are still on
 * the same filesystem at this point, since this runs before any real
 * disk-role/migration machinery (which moves data *across* filesystems)
 * is even reachable.
 */
static void migrate_flat_layout_to_grouped(void)
{
	static const char *const state_entries[] = {
		"networks.json",       "dns_records.json",  "dns_servers.json",     "ldap_servers.json",
		"ldap_users.json",     "ldap_groups.json",  "ldap_config.json",
		"pki",                 "container_defs.json", "rolling_config.json", "site_config.json",
		"devicemaps.json",     "daemon_config.json", "quota_projids.json",
		"keys",                "resolv.conf",       "ntp.conf",             "ntp_servers.json",
		"syslog_targets.json", "tls_throttle.json",
		/*
		 * "diskroles.json" deliberately NOT here (ADR-0141 Phase 2
		 * correction) -- it stays a fixed g_base_dir child, never
		 * STATE_DIR-relative; see DISKROLE_STATE_PATH's own comment in
		 * init_base_dir_paths(). migrate_diskroles_out_of_state_dir()
		 * below handles the one-time reverse move for any box that
		 * already migrated it into STATE_DIR under the original,
		 * since-corrected Phase 0 list.
		 */
	};
	static const char *const rebuildable_entries[] = { "images", "pkg", "artifacts", "iso" };
	size_t i;

	for (i = 0; i < sizeof(state_entries) / sizeof(state_entries[0]); i++)
		migrate_one_flat_entry(state_entries[i], STATE_DIR);
	for (i = 0; i < sizeof(rebuildable_entries) / sizeof(rebuildable_entries[0]); i++)
		migrate_one_flat_entry(rebuildable_entries[i], REBUILDABLE_DIR);
}

/*
 * ADR-0141 Phase 2 correction, one-time and idempotent exactly like
 * migrate_flat_layout_to_grouped() above (same rename(2)-is-a-no-op-on-
 * ENOENT safety, safe to call unconditionally on every boot): moves
 * diskroles.json back OUT of STATE_DIR to its own fixed g_base_dir
 * location, for any box that already ran the original Phase 0 upgrade
 * (which incorrectly grouped it in). A no-op on a box that's never had
 * a STATE_DIR/diskroles.json at all -- a genuinely fresh install always
 * writes it straight to the fixed path via DISKROLE_STATE_PATH, nothing
 * to move. Must run before diskrole_init() ever reads DISKROLE_STATE_PATH,
 * same ordering discipline (and same two call sites -- inside boot_init()
 * and again in main() for the non-init-mode path) as its sibling.
 */
static void migrate_diskroles_out_of_state_dir(void)
{
	char old_path[PATH_MAX];
	struct stat st;

	if (snprintf(old_path, sizeof(old_path), "%s/diskroles.json", STATE_DIR) >=
	    (int)sizeof(old_path))
		return;
	if (lstat(old_path, &st) != 0)
		return; /* nothing to migrate -- already fixed, or a fresh install */
	if (rename(old_path, DISKROLE_STATE_PATH) != 0)
		fprintf(stderr, "migrate_diskroles_out_of_state_dir: rename %s -> %s failed: %s\n",
		        old_path, DISKROLE_STATE_PATH, strerror(errno));
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
/* The reserved network name bootstrap_management_network() creates on
 * a genuinely fresh install (ADR-0066's rename note) -- never used to
 * look up an already-flagged network on a pre-rename box, which keeps
 * whatever it's actually named (network_find_management() finds it by
 * its is_management flag, not by this string). */
#define MGMT_NETWORK_NAME "management"
/*
 * Partition 5, same fixed QEMU virtio-blk layout as ESP_DEVICE/
 * CONFIG_DEVICE above -- also absent on parts 1/2's throwaway 2/3-
 * partition test disks, so boot_init() falls back to a tmpfs at
 * BASE_DIR when this device doesn't exist, exactly like every other
 * "not a real installed system" case in this function.
 */
#define CONTAINERS_DEVICE "/dev/vda5"

/*
 * Real ext4 project-quota device resolution (Part 4, bare-metal-
 * readiness plan, ADR-0062). quotactl(2)'s own "special" argument
 * needs the real block device backing wherever CONTAINERS_DIR actually
 * lives -- which is CONTAINERS_DEVICE only under a real --init-mode
 * boot; every daemon-linked test and any --data-dir= override instead
 * points g_base_dir at an ordinary directory on whatever filesystem the
 * host/test environment's own root happens to be (see CONTAINERS_
 * DEVICE's own comment above for the tmpfs-fallback case, which is a
 * third possibility again). Hardcoding CONTAINERS_DEVICE here would be
 * silently wrong in both of those cases -- this project's own bare-
 * metal-readiness plan flagged this exact question explicitly ("device-
 * path resolution... must be confirmed, not assumed"), so it's resolved
 * for real instead: walk /proc/mounts and pick the longest-matching
 * mount point for `path` (the same "find the owning mount" algorithm
 * findmnt/df use internally), returning 0 and filling out_device on
 * success. -1 (errno set) if /proc/mounts can't be read or path isn't
 * under any mount point at all (should never happen for a legitimately
 * mounted directory).
 *
 * Deliberately does not decode octal-escaped whitespace in /proc/mounts'
 * own mountpoint field (e.g. "\040" for a literal space) -- no path this
 * project ever mounts anything at (CONTAINERS_DIR, PKI_DIR, or any
 * --data-dir=/mkdtemp() test path) contains a space, so handling that
 * general case would be real, unexercised complexity for a scenario
 * that can't occur here.
 */
static int resolve_backing_device(const char *path, char *out_device, size_t out_size)
{
	char real_path[PATH_MAX];
	FILE *f;
	char line[PATH_MAX * 2];
	size_t best_len = 0;
	int found = 0;

	if (realpath(path, real_path) == NULL)
		return -1;

	f = fopen("/proc/mounts", "r");
	if (f == NULL)
		return -1;

	while (fgets(line, sizeof(line), f) != NULL) {
		char device[PATH_MAX];
		char mountpoint[PATH_MAX];
		size_t mp_len;

		if (sscanf(line, "%4095s %4095s", device, mountpoint) != 2)
			continue;

		mp_len = strlen(mountpoint);
		if (strncmp(real_path, mountpoint, mp_len) != 0)
			continue;
		/* Exact match, or the next real_path char must be '/' -- so a
		 * mountpoint of "/var" never matches a real_path of
		 * "/variant". */
		if (real_path[mp_len] != '\0' && real_path[mp_len] != '/')
			continue;
		if (mp_len < best_len)
			continue;

		best_len = mp_len;
		if (snprintf(out_device, out_size, "%s", device) >= (int)out_size) {
			fclose(f);
			errno = ENAMETOOLONG;
			return -1;
		}
		found = 1;
	}
	fclose(f);

	if (!found) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

/*
 * Sets a real, kernel-enforced hard limit of quota_bytes for project id
 * projid on whatever device backs base_path (the container's own
 * container_base -- CONTAINERS_DIR/<name> by default, or
 * <disk's mount_path>/containers/<name> for a disk-placed container,
 * task #638 -- NOT always CONTAINERS_DIR itself: a container placed on
 * an alternate disk must have its quota set against THAT disk's own
 * backing device, or the limit would silently apply to the wrong
 * filesystem entirely while the actual files live elsewhere), via a
 * real quotactl(2) Q_SETQUOTA call -- independent of and order-agnostic with
 * src/overlay.c's own FS_IOC_FSSETXATTR tagging (that call says "these
 * files belong to project X"; this one says "project X's own limit is
 * Y" -- setting a limit for a project id the kernel has never seen an
 * inode tagged with yet is a completely normal, harmless no-op until
 * one shows up). dqb_bhardlimit is in real quota *blocks* (always
 * 1024 bytes each, regardless of the filesystem's own block size --
 * see /usr/include/x86_64-linux-gnu/sys/quota.h's own struct dqblk
 * comment), not raw bytes, hence the rounding-up conversion. No soft
 * limit / grace-period policy -- dqb_bsoftlimit is set equal to the
 * hard limit, so writes are refused (EDQUOT) the instant the real limit
 * is hit, not merely warned about after some grace period this project
 * has no mechanism to surface to an operator anyway. Returns 0 on
 * success, -1 (errno set by quotactl(2) -- ENOTSUP/EOPNOTSUPP if the
 * backing filesystem doesn't have the project-quota feature enabled at
 * all, exactly what a filesystem kanxeo-install.c didn't create via
 * mkfs.ext4 -O quota -E quotatype=prjquota reports) otherwise.
 */
static int set_disk_quota(const char *base_path, uint32_t projid, long long quota_bytes)
{
	char device[PATH_MAX];
	struct dqblk dq;

	if (resolve_backing_device(base_path, device, sizeof(device)) != 0)
		return -1;

	memset(&dq, 0, sizeof(dq));
	dq.dqb_bhardlimit = (uint64_t)((quota_bytes + 1023) / 1024);
	dq.dqb_bsoftlimit = dq.dqb_bhardlimit;
	dq.dqb_valid = QIF_BLIMITS;

	if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), device, (int)projid, (caddr_t)&dq) != 0)
		return -1;
	return 0;
}

#define MAX_EVENTS 64
#define CONTAINERS_PREFIX "/v1/containers/"
#define CONTAINER_RECIPES_PREFIX "/v1/containers/recipes/"
#define NETWORKS_PREFIX "/v1/networks/"
#define DNS_RECORDS_PREFIX "/v1/dns/records/"
#define DNS_SERVERS_PREFIX "/v1/dns/servers/"
#define LDAP_SERVERS_PREFIX "/v1/ldap/servers/"
#define LDAP_GROUPS_PREFIX "/v1/ldap/groups/"
#define LDAP_USERS_PREFIX "/v1/ldap/users/"
#define NTP_SERVERS_PREFIX "/v1/ntp/servers/"
#define SYSLOG_TARGETS_PREFIX "/v1/syslog/targets/"
#define PROCESSES_PREFIX "/v1/system/processes/"
#define PKI_CERTS_PREFIX "/v1/pki/certs/"
#define PKG_PREFIX "/v1/pkg/"
#define PKG_RECIPES_PREFIX "/v1/pkg/recipes/"
#define IMAGES_PREFIX "/v1/images/"
#define IMAGE_RECIPES_PREFIX "/v1/images/recipes/"
#define DEVICEMAPS_PREFIX "/v1/devicemaps/"
#define DISKROLES_PREFIX "/v1/diskroles/"
#define DISKS_PREFIX "/v1/disks/"

enum conn_kind {
	CONN_LISTENER,
	CONN_LISTENER_TLS, /* the HTTPS listener (Part 0.5) -- same accept4()
	                     * loop as CONN_LISTENER, distinguished only by
	                     * whether accept_loop() wraps the accepted fd in
	                     * a fresh SSL* before registering it */
	CONN_CLIENT,
	CONN_CONTAINER,
	CONN_PKG_FETCH,
	CONN_PKG_BUILD_OUTPUT,  /* pkg.c's build-output capture pipe, drained incrementally
	                          * as the container runs rather than once at exit (ADR-0087) */
	CONN_CONTAINER_OUTPUT,  /* an ordinary container's own stdout/stderr capture pipe --
	                          * always present since transparent container-log capture
	                          * landed (every container, not just capture_output=true
	                          * ones), drained incrementally into logstore.c (source
	                          * "container") exactly like CONN_PKG_BUILD_OUTPUT drains
	                          * into pkg.c's build-output buffer -- the same ADR-0087
	                          * discipline, a different destination. POST /v1/containers'
	                          * own "capture_output" opt-in now controls only whether
	                          * the SAME bytes are ALSO mirrored into this container's
	                          * own registry_entry->captured_output tail. */
	CONN_BOOTROOT_ASSEMBLE, /* server-side mkbootroot invocation (ADR-0057) */
	CONN_BOOTROOT_OUTPUT,   /* mkbootroot's own captured stdout/stderr, drained
	                          * incrementally exactly like CONN_PKG_BUILD_OUTPUT (ADR-0087) */
	CONN_ISO_ASSEMBLE,      /* server-side mkinstalleriso invocation (ADR-0064) */
	CONN_BOOTSTRAP_FETCH,   /* pkg/bootstrap's own toolchain_url curl fetch (ADR-0065) */
	CONN_PKG_SYNC_FETCH,    /* pkg sync's own repo-archive curl fetch (ADR-0121) */
	CONN_PKG_SYNC_PERIODIC_TIMER, /* permanent, re-arms itself -- fires pkg_sync_start() periodically if configured (ADR-0121) */
	CONN_BACKUP_PERIODIC_TIMER, /* permanent, re-arms itself -- fires do_backup_snapshot_now() periodically if enabled+configured (ADR-0141 Phase 5) */
	CONN_IMAGE_RECIPE_FETCH, /* image-recipe-apply's own whole-rootfs artifact curl fetch (ADR-0123) */
	CONN_DISK_FORMAT,       /* disk format+mount job (multi-disk management Phase C) */
	CONN_STORAGE_MIGRATE,   /* state/rebuildable/log-storage migration job (ADR-0141 Phase 2) */
	CONN_CONTAINER_STORAGE_MIGRATE, /* one container's own overlay-storage migration job (ADR-0142 Section 4) */
	CONN_PING,              /* GET/POST /v1/system/ping -- the raw ICMP socket half */
	CONN_PING_TIMER,        /* same job's paired timeout -- see ping_job_teardown() */
	CONN_NTP_PERIODIC_TIMER, /* permanent, re-arms itself -- fires ntp_sync_start() periodically (task #751) */
	CONN_NTP_SYNC,           /* one in-flight SNTP sync attempt's own UDP socket */
	CONN_NTP_SYNC_TIMER,     /* same job's per-candidate timeout -- see ntp_job_teardown() */
	CONN_RESTART_TIMER,
	CONN_ROLLING_RESTART_TIMER, /* jittered live-restart onto a newer rolling image
	                              * version (ADR-0124/Part 5) -- distinct from
	                              * CONN_RESTART_TIMER: that one only ever fires
	                              * after a container has already exited (crash
	                              * respawn); this one fires against a still-live
	                              * container, which needs an explicit stop first
	                              * -- see handle_rolling_restart_timer_event(). */
	CONN_BIND_IP_CLEANUP, /* deferred rtnl_addr_del_ipv4() of a superseded/cleared
	                        * bind_ip (ADR-0068) -- see arm_bind_ip_cleanup_timer() */
	CONN_CONSOLE_SHELL,
	CONN_CONSOLE_RESPAWN_TIMER,
	CONN_CONSOLE_WS,        /* GET /v1/containers/{name}/console -- client-facing WebSocket half */
	CONN_CONSOLE_PTY,       /* same session's other half -- the exec'd shell's pty master fd */
	CONN_PKG_BUILD_LOG_WS,  /* GET /v1/pkg/build/log -- live-tail of the in-flight build's own
	                          * output pipe (task #676), a one-way relay of CONN_PKG_BUILD_OUTPUT's
	                          * existing capture, not an exec/PTY session like CONN_CONSOLE_WS */
	CONN_KMSG,              /* /dev/kmsg -- feeds real kernel dmesg lines into the consolidated log store */
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
	SSL *ssl; /* CONN_CLIENT only, and only when accepted on the HTTPS
	           * listener -- NULL for every plain-HTTP connection (the
	           * overwhelming majority). Non-NULL but !SSL_is_init_
	           * finished() means the TLS handshake is still in
	           * progress; see handle_client_event(). */
	struct http_conn http;                 /* CONN_CLIENT only */
	char peer_ip[CONNTHROTTLE_IP_MAX];      /* CONN_CLIENT only -- captured at accept4() time */
	struct registry_entry *entry;           /* CONN_CONTAINER / CONN_CONTAINER_OUTPUT */
	/*
	 * CONN_CONTAINER_OUTPUT only: partial-line accumulation for the
	 * always-on container-log-capture forward into logstore (transparent
	 * log aggregation) -- independent of entry->captured_output's own
	 * raw-byte accumulation (which stays byte-oriented, unchanged,
	 * gated on entry->capture_requested). A line longer than this
	 * buffer is flushed to logstore as-is at the boundary rather than
	 * grown unboundedly -- the same "generous but bounded" posture
	 * every other capture buffer in this daemon already has.
	 */
	char output_line_buf[1024];             /* CONN_CONTAINER_OUTPUT only */
	int output_line_len;                    /* CONN_CONTAINER_OUTPUT only */
	pid_t pkg_fetch_pid;                    /* CONN_PKG_FETCH / CONN_BOOTROOT_ASSEMBLE / CONN_ISO_ASSEMBLE / CONN_BOOTSTRAP_FETCH / CONN_DISK_FORMAT / CONN_STORAGE_MIGRATE / CONN_CONTAINER_STORAGE_MIGRATE */
	enum storage_kind storage_migrate_kind; /* CONN_STORAGE_MIGRATE only */
	char container_storage_migrate_name[REGISTRY_NAME_MAX]; /* CONN_CONTAINER_STORAGE_MIGRATE only */
	char restart_name[REGISTRY_NAME_MAX];   /* CONN_RESTART_TIMER only */
	char cleanup_ifname[NETWORK_NAME_MAX];  /* CONN_BIND_IP_CLEANUP only */
	uint32_t cleanup_addr_be;               /* CONN_BIND_IP_CLEANUP only */
	int cleanup_prefix_len;                 /* CONN_BIND_IP_CLEANUP only */
	pid_t console_pid;                      /* CONN_CONSOLE_SHELL only */
	char console_tty[32];                   /* CONN_CONSOLE_SHELL / CONN_CONSOLE_RESPAWN_TIMER */
	struct console_exec_session *exec_session; /* CONN_CONSOLE_WS / CONN_CONSOLE_PTY only -- shared by both halves of one session */
	struct ws_conn ws;                      /* CONN_CONSOLE_WS / CONN_PKG_BUILD_LOG_WS -- incremental client-frame parser */
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
static struct conn g_kmsg_conn;
static struct conn g_https_listener_conn; /* .fd == -1 when HTTPS is disabled */
static SSL_CTX *g_tls_ctx;                 /* NULL when HTTPS is disabled */
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
	if (sess->ws_conn->ssl != NULL) {
		tls_unregister(sess->ws_conn->fd);
		SSL_free(sess->ws_conn->ssl);
	}
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
/* Backing storage for g_bind_addr when it's derived from the
 * management network's own address (Part 0.5) rather than taken directly
 * from argv's --bind= -- g_bind_addr has to remain valid for the rest
 * of the process's life, so this can't be a stack buffer. */
static char g_bind_addr_buf[INET_ADDRSTRLEN];
static int g_port;

/*
 * Real freshness tracking for the async kanxeo bootroot assembly
 * (spawn_kanxeo_bootroot_assembly()/handle_bootroot_assemble_event(),
 * ADR-0057) -- closes task #737's own gap: `pkg hostbuild kanxeo
 * --deploy` used to treat "kanxeod-root.squashfs exists" as "this
 * hostbuild round's own artifact is ready," but that file is a leftover
 * from whichever assembly last succeeded, not necessarily the one this
 * round's own hostbuild triggered -- a stale file from an earlier round
 * would be silently redeployed while the real new assembly was still
 * running. g_bootroot_assembly_started increments once per attempt
 * (right before the fork in spawn_kanxeo_bootroot_assembly());
 * g_bootroot_assembly_completed is only ever set to that attempt's own
 * generation number on a *real, confirmed success*
 * (handle_bootroot_assemble_event(), WIFEXITED && WEXITSTATUS==0) --
 * never bumped on failure, so a client that captured a baseline before
 * triggering a new hostbuild can tell "a newer assembly than the one I
 * already knew about actually finished" from "the last one is still the
 * only one that ever succeeded." g_bootroot_assembly_running covers the
 * third state (attempted but not yet resolved either way) so a client
 * polling this can also tell "still working" from "gave up, that
 * attempt failed" instead of spinning forever on a failure. Reported via
 * GET /system/boot (handle_system_boot() below) -- not a new resource of
 * its own, since this is squarely "state about the currently/most-
 * recently-assembled boot image," the same subject that endpoint already
 * owns; keeping it there (rather than folding it into pkg.c's own
 * generic pkg_get_one() JSON) keeps pkg.c fully agnostic to what any
 * hostbuild name *means*, exactly the separation ADR-0057's own "kanxeo"
 * special-case comment in this file already established.
 */
static long g_bootroot_assembly_started;
static long g_bootroot_assembly_completed;
static int g_bootroot_assembly_running;

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

/* Parses the simple key=value net.conf kanxeo-install writes to the
 * config partition (image/src/kanxeo-install.c's populate step) --
 * ip=/prefix=/gateway=/interface=, one per line. interface= (Part 0.5)
 * is the physical NIC to attach to the management network -- an explicit,
 * operator-chosen GRUB field rather than find_nic()'s old "whichever
 * readdir() returns first" guess. */
static int parse_net_conf(const char *path, char *out_ip, size_t ip_size, int *out_prefix,
                           char *out_gateway, size_t gateway_size, char *out_interface,
                           size_t interface_size)
{
	FILE *f;
	char line[256];
	int have_ip = 0, have_prefix = 0, have_gateway = 0, have_interface = 0;

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
		} else if (strncmp(line, "interface=", 10) == 0) {
			snprintf(out_interface, interface_size, "%s", line + 10);
			have_interface = 1;
		}
	}
	fclose(f);
	return (have_ip && have_prefix && have_gateway && have_interface) ? 0 : -1;
}

/*
 * Part 3 (bare-metal-readiness plan, ADR-0061): loads a curated,
 * boot-critical module list via the real, freshly-staged
 * /usr/bin/modprobe (kmod.recipe, staged onto this control-plane
 * squashfs by mkbootroot.c's own kmod_bin_dir argument) -- run before
 * bootstrap_management_network() below, not after: that function
 * attaches a *named* physical interface (net.conf's own --interface=),
 * which the kernel must have already detected as a real netdev by
 * then, and on real hardware that detection only happens once the
 * matching driver is loaded. The storage controller that might hold
 * root is never in this list -- see image/kernel/qemu-part1.config's
 * own comment on why that has to stay built directly into the kernel
 * instead, never a module.
 *
 * Best-effort per module, same "a real machine this project has never
 * seen before might simply not have this exact hardware" posture
 * cgroup_enable_controllers() already established for an unrelated
 * optional capability -- covers several different vendors' NIC
 * chipsets in one list, no single real machine has all of them, so a
 * missing driver here is the overwhelmingly common, expected case, not
 * a real error worth failing boot over (or even logging on every
 * single boot). A --data-dir= test invocation never reaches this at
 * all (init_mode is false), so it never needs a real /usr/bin/modprobe
 * to exist.
 */
static void load_boot_modules(void)
{
	static const char *const boot_modules[] = {
		/* NICs -- covers common real-hardware chipsets bootstrap_
		 * management_network() below might need probed first. */
		"e1000e", "igb", "ixgbe", "r8169", "tg3",
		/* USB -- never needed for root or the mgmt network itself,
		 * loaded here anyway since this is the one, single curated
		 * list this daemon ever runs modprobe against. */
		"ehci-hcd", "usb-storage",
	};
	size_t i;

	for (i = 0; i < sizeof(boot_modules) / sizeof(boot_modules[0]); i++) {
		pid_t pid;
		int status;
		char *argv[] = { (char *)"modprobe", (char *)boot_modules[i], NULL };

		pid = fork();
		if (pid < 0) {
			perror("fork (modprobe)");
			continue;
		}
		if (pid == 0) {
			execve("/usr/bin/modprobe", argv, environ);
			_exit(127);
		}
		waitpid(pid, &status, 0);
		/* Exit status deliberately unchecked/unlogged -- see the
		 * function-level comment above for why "no such hardware
		 * present" isn't a real error here. */
	}
}

/*
 * Bootstraps the management network from the static IP/gateway/interface
 * configured at install time (Part 0.5, superseding the old, invisible
 * apply_static_ip()) -- creates a real, persisted network_def (visible
 * at GET /v1/networks like any other), attaches the GRUB-chosen
 * physical interface to it, and designates it the daemon's own
 * management network (network_set_management()). g_bind_addr is then
 * derived from that network's own address -- the network is the one
 * authoritative source for it from this point on, not a
 * separately-carried --bind= argument. A missing or incomplete
 * net.conf is not an error (0, not -1): parts 1/2's own test disks
 * never write one, and that must stay a normal, inert boot, not a
 * failure.
 *
 * Two genuinely different concepts here, not to be confused (ADR-0058,
 * ADR-0067): the management network's own address_be -- an address
 * living directly on its bridge, now doing double duty as kanxeod's
 * own bind address -- versus net.conf's own "gateway=" field, the
 * box's *upstream* default route (the next-hop router this box's own
 * outbound traffic egresses through), which keeps its existing,
 * unrelated meaning and mechanism (rtnl_route_add_default_ipv4())
 * below. The former was itself once called "gateway" too (ADR-0037);
 * ADR-0067 renamed it specifically to stop it colliding with this
 * second, real gateway concept in name as well as in fact.
 */
static int bootstrap_management_network(void)
{
	char ip[64], upstream_gateway[64], iface[IFNAMSIZ];
	int prefix = 0;
	struct in_addr addr, gw, subnet;
	char subnet_str[INET_ADDRSTRLEN];
	uint32_t mask;
	struct network_def *net;
	enum network_error nerr;
	int rtfd;

	if (parse_net_conf(NET_CONF_PATH, ip, sizeof(ip), &prefix, upstream_gateway,
	                    sizeof(upstream_gateway), iface, sizeof(iface)) != 0)
		return 0;

	if (inet_pton(AF_INET, ip, &addr) != 1) {
		fprintf(stderr, "bootstrap_management_network: invalid ip %s\n", ip);
		return -1;
	}
	if (inet_pton(AF_INET, upstream_gateway, &gw) != 1) {
		fprintf(stderr, "bootstrap_management_network: invalid gateway %s\n", upstream_gateway);
		return -1;
	}
	if (prefix < 8 || prefix > 30) {
		fprintf(stderr, "bootstrap_management_network: invalid prefix %d\n", prefix);
		return -1;
	}

	mask = (uint32_t)0xFFFFFFFFu << (32 - prefix);
	subnet.s_addr = htonl(ntohl(addr.s_addr) & mask);
	if (inet_ntop(AF_INET, &subnet, subnet_str, sizeof(subnet_str)) == NULL) {
		perror("bootstrap_management_network: inet_ntop");
		return -1;
	}

	/* Flag-based lookup first, not by name (ADR-0066's rename note):
	 * an already-flagged network -- including a pre-rename box's own
	 * legacy "mgmt" network, reloaded from persisted state by
	 * network_init() before this ever runs -- is found here and the
	 * create-with-the-current-default-name branch below is never
	 * entered, so an existing box's own network is never orphaned or
	 * duplicated by a future rename of MGMT_NETWORK_NAME. Only a
	 * genuinely fresh install (nothing flagged yet) falls through to
	 * create one under today's default name. */
	net = network_find_management();
	if (net == NULL) {
		net = network_find(MGMT_NETWORK_NAME);
		if (net == NULL) {
			nerr = network_create(MGMT_NETWORK_NAME, subnet_str, prefix, ip, &net);
			if (nerr != NETWORK_OK) {
				fprintf(stderr, "bootstrap_management_network: network_create failed (%d)\n",
				        (int)nerr);
				return -1;
			}
			nerr = network_attach_interface(MGMT_NETWORK_NAME, iface, 0);
			if (nerr != NETWORK_OK) {
				fprintf(stderr,
				        "bootstrap_management_network: network_attach_interface failed (%d)\n",
				        (int)nerr);
				return -1;
			}
		}
	}
	/* Idempotent re-affirmation on every boot after the first --
	 * net->name is whatever this network is actually called (a legacy
	 * "mgmt" or today's "management"), never a fixed literal. */
	nerr = network_set_management(net->name);
	if (nerr != NETWORK_OK) {
		fprintf(stderr, "bootstrap_management_network: network_set_management failed (%d)\n",
		        (int)nerr);
		return -1;
	}

	snprintf(g_bind_addr_buf, sizeof(g_bind_addr_buf), "%s", ip);
	g_bind_addr = g_bind_addr_buf;

	rtfd = rtnl_open();
	if (rtfd < 0) {
		perror("rtnl_open");
		return -1;
	}
	if (rtnl_route_add_default_ipv4(rtfd, gw.s_addr) != 0) {
		perror("bootstrap_management_network: default route");
		rtnl_close(rtfd);
		return -1;
	}
	rtnl_close(rtfd);

	printf("init-mode: %s network %s/%d via %s, bind=%s, upstream gateway %s\n", net->name, subnet_str,
	       prefix, iface, ip, upstream_gateway);
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
	/*
	 * task #764: kanxeod's own exec_into_container() (daemon/src/exec.c,
	 * the console/exec feature, ADR pending #426) calls posix_openpt()
	 * and then open()s the slave device ptsname_r() hands back --
	 * that slave path only resolves to a real device node once the
	 * devpts filesystem is actually mounted at /dev/pts. devtmpfs
	 * auto-populates /dev/ptmx (CONFIG_DEVTMPFS_MOUNT) so posix_openpt()
	 * itself always succeeds regardless, which is exactly what made
	 * this gap so easy to miss: only the SECOND step (opening the
	 * slave) ever fails, with a generic ENOENT that gave no hint it
	 * was devpts-shaped until task #764's own diagnostic surfacing
	 * made "No such file or directory" visible at all. kanxeo-install.c's
	 * own early_mounts() has mounted devpts since ADR-0042/task #406 --
	 * but that binary only ever runs during one-time disk installation,
	 * never during kanxeod's own real boot_init() (--init-mode, a bare
	 * kernel with no initramfs) on an already-installed box, which is
	 * exactly why every console/exec attempt against 192.168.15.95
	 * failed with a bare 500 while the same code path passed cleanly
	 * in every dev-sandbox/QEMU test harness run (those inherit an
	 * already-mounted /dev/pts from their own outer environment,
	 * never exercising this gap). Same mount options as
	 * kanxeo-install.c's own copy, for the same reason (ptmxmode=0666
	 * -- devpts's own ptmx alias needs to be world-writable for
	 * posix_openpt() to work as any non-root exec'd process would
	 * expect, even though everything in this project currently execs
	 * as root).
	 */
	if (mkdir("/dev/pts", 0755) != 0 && errno != EEXIST) {
		perror("/dev/pts");
		return -1;
	}
	if (mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
	          "mode=0620,ptmxmode=0666") != 0) {
		perror("mount devpts");
		return -1;
	}
	if (mount(CONTAINERS_DEVICE, g_base_dir, "ext4", MS_NOSUID | MS_NODEV, NULL) != 0 &&
	    mount_or_fail("tmpfs", g_base_dir, "tmpfs", MS_NOSUID | MS_NODEV) != 0)
		return -1;
	/*
	 * ADR-0141 fix (found live, 192.168.15.95): must run here, not just
	 * from main()'s own later call below boot_init()'s return -- the
	 * resolv.conf open+bind-mount immediately below this comment (and
	 * any future boot_init() step touching a grouped-layout path) reads
	 * RESOLV_CONF_PATH/STATE_DIR/REBUILDABLE_DIR *before* main() ever
	 * gets a chance to migrate an old-flat-layout box's real content
	 * into them. Confirmed the hard way: on this box's first boot after
	 * the STATE_DIR/REBUILDABLE_DIR grouping shipped, boot_init()'s own
	 * O_CREAT (no O_TRUNC) opened a brand-new, empty RESOLV_CONF_PATH
	 * (nothing had migrated there yet) and bind-mounted /etc/resolv.conf
	 * onto *that* inode; main()'s later migrate_flat_layout_to_grouped()
	 * then rename(2)'d the box's real, historical resolv.conf into the
	 * same path -- which only repoints the directory entry, not the
	 * already-bind-mounted inode -- leaving /etc/resolv.conf silently,
	 * permanently empty (every GET/PUT /v1/system/resolv still worked
	 * correctly, since resolv.c operates on the path, not the stale
	 * mount) until a second reboot, by which point nothing was left to
	 * migrate and the race couldn't recur. Calling it here, before any
	 * grouped-path use in this function, closes the race outright.
	 * Idempotent (a no-op once already-migrated) -- main()'s own call
	 * right after boot_init() returns stays in place, unchanged, for
	 * the non-init-mode (test/dev) path that never reaches this
	 * function at all.
	 */
	migrate_flat_layout_to_grouped();
	migrate_diskroles_out_of_state_dir();
	/*
	 * ADR-0076: the host's own outbound DNS resolver config.
	 * RESOLV_CONF_PATH (STATE_DIR/resolv.conf) is real, persisted
	 * state -- ordinary create-if-missing (O_CREAT, no O_TRUNC, so a
	 * real reboot never wipes an operator-configured resolver) rather
	 * than resolv_init()'s own later, read-only load, since that
	 * doesn't run until well after this mount needs the file to
	 * already exist. Bind-mounted onto /etc/resolv.conf (a real,
	 * empty placeholder file already staged in the control-plane
	 * squashfs, mkbootroot.c) so kanxeod's own curl/openssl/etc.
	 * subprocesses -- and every future host-level tool -- resolve
	 * against whatever an operator sets via PUT /v1/system/resolv,
	 * with zero further wiring needed per tool. Best-effort: a
	 * failure here leaves the host with no outbound DNS, exactly
	 * today's status quo, not a boot-blocking condition.
	 */
	{
		int rfd = open(RESOLV_CONF_PATH, O_CREAT | O_WRONLY, 0644);

		if (rfd >= 0)
			close(rfd);
		if (mount(RESOLV_CONF_PATH, "/etc/resolv.conf", NULL, MS_BIND, NULL) != 0)
			perror("/etc/resolv.conf bind mount");
	}
	/* Needed to reach the loader entry confirm_boot() renames once this
	 * boot proves healthy (Phase 11 part 2) -- writable, not read-only
	 * like the root mounts, since that rename is a real write. */
	if (mount_or_fail(ESP_DEVICE, ESP_DIR, "vfat", MS_NOSUID | MS_NODEV | MS_NOEXEC) != 0)
		return -1;
	/* Deliberately non-fatal, unlike every mount above -- see
	 * CONFIG_DEVICE's own comment. Left mounted (not unmounted here) --
	 * bootstrap_management_network() reads net.conf from it later in
	 * main(), once network_init() has made network_create()/
	 * network_attach_interface() usable (Part 0.5) -- boot_init() itself
	 * stays purely about mounts and bringing lo up, as its name implies. */
	mount(CONFIG_DEVICE, CONFIG_DIR, "ext4", 0, NULL);

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
	case NETWORK_ERR_INVALID_ADDRESS:
		*out_msg = "invalid address (must be a real address within this subnet, "
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
	case NETWORK_ERR_IS_MANAGEMENT:
		*out_msg = "refused: this network carries kanxeod's own bind address -- "
		           "repoint the management network first (see /v1/system/daemon-config)";
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

/*
 * Build/slot/kernel identity -- split out of handle_health() (ADR-0077)
 * so the liveness poll every client runs every few seconds stays the
 * single-field response it always should have been, while this
 * lower-frequency identity check gets a real home of its own.
 */
static void handle_system_boot(int fd)
{
	struct json_writer w;
	struct utsname uts;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "build_version");
	jw_str(&w, KANXEO_BUILD_VERSION);
	jw_key(&w, "build_time");
	jw_str(&w, KANXEO_BUILD_TIME);
	jw_key(&w, "slot");
	if (g_slot != NULL)
		jw_str(&w, g_slot);
	else
		jw_null(&w);
	jw_key(&w, "kernel_version");
	if (uname(&uts) == 0)
		jw_str(&w, uts.release);
	else
		jw_null(&w);
	/*
	 * Real freshness signal for `pkg hostbuild kanxeo --deploy`
	 * (task #737, ADR-0058-follow-on comment in
	 * g_bootroot_assembly_started's own doc comment above): a client
	 * that captured bootroot_assembly_completed_generation *before*
	 * triggering a new hostbuild round can wait here for it to advance
	 * past that baseline rather than trusting "kanxeod-root.squashfs
	 * exists" (which is also true of a stale file left by an earlier,
	 * unrelated round).
	 */
	jw_key(&w, "bootroot_assembly_started_generation");
	jw_int(&w, g_bootroot_assembly_started);
	jw_key(&w, "bootroot_assembly_completed_generation");
	jw_int(&w, g_bootroot_assembly_completed);
	jw_key(&w, "bootroot_assembly_running");
	jw_bool(&w, g_bootroot_assembly_running);
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
	const char *active_device;
	char active_kernel_path[PATH_MAX];
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
		active_device = ROOT_A_DEVICE;
	} else if (strcmp(g_slot, "b") == 0) {
		inactive_slot = "a";
		device = ROOT_A_DEVICE;
		active_device = ROOT_B_DEVICE;
	} else {
		snprintf(out_errmsg, out_errmsg_size, "unrecognized --slot=, expected \"a\" or \"b\"");
		return 400;
	}
	snprintf(kernel_dest, sizeof(kernel_dest), "%s/kanxeo-bzImage-%s", ESP_DIR, inactive_slot);
	snprintf(active_kernel_path, sizeof(active_kernel_path), "%s/kanxeo-bzImage-%s", ESP_DIR, g_slot);

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
	/*
	 * ADR-0095: the real footgun this project's own guides have
	 * documented and worked around by hand ever since ADR-0031/
	 * ADR-0032 first split image_path/kernel_path apart -- a one-sided
	 * update used to leave the OTHER file in the inactive slot exactly
	 * as stale as it was from whenever that slot was last written,
	 * silently pairing (say) a brand-new kernel with a root squashfs
	 * from three updates ago. Closed by auto-copying the omitted half
	 * forward from the ACTIVE slot's own currently-running copy --
	 * already-booted, already-verified-good by definition, and the
	 * exact thing an operator supplying only one path actually wants
	 * paired with it. This is pure default-filling: an explicitly
	 * supplied image_path/kernel_path is used exactly as given, this
	 * only ever substitutes for one that's missing.
	 */
	if (image_path == NULL || image_path[0] == '\0')
		image_path = NULL; /* copy from active_device below */
	if (kernel_path == NULL || kernel_path[0] == '\0')
		kernel_path = NULL; /* copy from active_kernel_path below */

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

	/*
	 * write_file_to_device()'s own copy_bytes() loop reads until read()
	 * returns 0 -- true at end-of-file for a plain file, equally true
	 * at a block device's own real capacity for a raw device node, so
	 * active_device works as a src_path here with no separate device-
	 * to-device copy primitive needed.
	 */
	if (write_file_to_device(image_path != NULL ? image_path : active_device, device) != 0) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size,
		         image_path != NULL ? "failed to write image to inactive slot"
		                             : "failed to copy the active slot's own root forward");
		return 500;
	}
	if (write_file_to_esp(kernel_path != NULL ? kernel_path : active_kernel_path, kernel_dest) !=
	    0) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size,
		         kernel_path != NULL ? "failed to write kernel to inactive slot"
		                              : "failed to copy the active slot's own kernel forward");
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
	/*
	 * Both are always written now (ADR-0095) -- an omitted half is
	 * auto-filled from the active slot's own running copy rather than
	 * left stale, so both genuinely are fresh content in the inactive
	 * slot regardless of which one(s) the caller actually supplied.
	 */
	*out_updated_root = 1;
	*out_updated_kernel = 1;
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
 * packages should exist, plus this install's own site config), not
 * workload data or image content, and never PKI (see docs/adr/0033).
 * Each file is read via the existing
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

	jw_key(w, "site_config");
	if (persist_read_file(SITE_CONFIG_PATH, &buf, &len) == 0 && buf != NULL) {
		jw_str(w, buf);
		free(buf);
	} else {
		jw_str(w, "");
	}

	/* Every recipe version on disk, not just currently-installed
	 * packages -- they're cheap, and the operator may want them all
	 * preserved. ADR-0107's version-keyed layout (<name>/<version>/
	 * build.sh, ADR-0120's filename) needs a two-level directory walk,
	 * not the flat single-level opendir() this used before that
	 * migration landed -- that old code silently stopped matching
	 * anything the moment recipes moved to per-name subdirectories
	 * (every entry in PKG_RECIPES_DIR became a directory, none ending
	 * in ".recipe"), so backups have included zero recipes since.
	 * Each entry's key is "<name>/<version>" (never ambiguous, since a
	 * bare package name never itself contains '/') -- do_system_
	 * restore() below splits on the same separator to reconstruct the
	 * exact on-disk path. */
	jw_key(w, "pkg_recipes");
	jw_obj_open(w);
	d = opendir(PKG_RECIPES_DIR);
	if (d != NULL) {
		while ((de = readdir(d)) != NULL) {
			char name_dir[PATH_MAX];
			DIR *vd;
			struct dirent *vde;

			if (de->d_name[0] == '.')
				continue;
			snprintf(name_dir, sizeof(name_dir), "%s/%s", PKG_RECIPES_DIR, de->d_name);
			vd = opendir(name_dir);
			if (vd == NULL)
				continue;
			while ((vde = readdir(vd)) != NULL) {
				char script_path[PATH_MAX];
				char key[512];
				struct stat st;

				if (vde->d_name[0] == '.')
					continue;
				snprintf(script_path, sizeof(script_path), "%s/%s/build.sh", name_dir,
				         vde->d_name);
				if (stat(script_path, &st) != 0 || !S_ISREG(st.st_mode))
					continue;
				if (persist_read_file(script_path, &buf, &len) != 0 || buf == NULL)
					continue;
				snprintf(key, sizeof(key), "%s/%s", de->d_name, vde->d_name);
				jw_key(w, key);
				jw_str(w, buf);
				free(buf);
			}
			closedir(vd);
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
 * ADR-0141 Phase 5: the real backup snapshot write -- validates the
 * configured disk (must carry the "backup" role, must currently be
 * mounted, the same class of check storagemigrate_start() already
 * does for the other three storage kinds), writes do_system_backup()'s
 * own bundle to "<mount_path>/backup.json" (a single, always-current
 * snapshot -- see backupconfig.h's own top comment for why this is
 * deliberately not a timestamped history), and records the outcome via
 * backupconfig_record_attempt(). Synchronous (a plain JSON write, not
 * a network fetch or external process -- nothing here justifies this
 * daemon's usual fork+pidfd async-job machinery), called both from
 * POST /v1/system/backup-config/snapshot-now and the periodic timer
 * below.
 */
static void do_backup_snapshot_now(void)
{
	const char *disk_name = backupconfig_disk();
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n, i;
	const struct discovered_disk *found = NULL;
	char target_path[PATH_MAX];
	struct json_writer w;

	if (disk_name == NULL) {
		backupconfig_record_attempt(0, "no backup disk configured");
		return;
	}

	n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
	for (i = 0; i < n; i++) {
		if (strcmp(disks[i].name, disk_name) == 0) {
			found = &disks[i];
			break;
		}
	}
	if (found == NULL) {
		backupconfig_record_attempt(0, "configured backup disk is not currently present");
		return;
	}
	{
		const char *role = diskrole_lookup(disk_name);

		if (role == NULL || strcmp(role, "backup") != 0) {
			backupconfig_record_attempt(0,
			                             "configured backup disk no longer carries the backup role");
			return;
		}
	}
	if (!found->mounted) {
		backupconfig_record_attempt(0, "configured backup disk is present but not currently mounted");
		return;
	}

	snprintf(target_path, sizeof(target_path), "%s/backup.json", found->mount_path);

	do_system_backup(&w);
	if (persist_atomic_write(target_path, w.buf, w.len) != 0) {
		jw_free(&w);
		backupconfig_record_attempt(0, "failed to write snapshot to disk");
		return;
	}
	jw_free(&w);
	backupconfig_record_attempt(1, NULL);
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
	const struct json_value *jpkg_recipes, *jsite_config;
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
	jsite_config = json_object_get(root, "site_config");

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
			const char *key = jpkg_recipes->u.object.keys[i];
			const char *slash = strchr(key, '/');

			if (json_as_string(jpkg_recipes->u.object.values[i]) == NULL) {
				json_free(root);
				snprintf(out_errmsg, out_errmsg_size,
				         "pkg_recipes.%s is not a string", key);
				return 400;
			}
			/* ADR-0120: each key is "<name>/<version>" (do_system_
			 * backup()'s own emitted shape) -- a bare package name
			 * never itself contains '/', so a single separator with
			 * non-empty content on both sides is both necessary and
			 * sufficient here. */
			if (slash == NULL || slash == key || slash[1] == '\0') {
				json_free(root);
				snprintf(out_errmsg, out_errmsg_size,
				         "pkg_recipes key %s is not in <name>/<version> form", key);
				return 400;
			}
		}
	}
	if (jsite_config != NULL) {
		have_any = 1;
		if (!json_string_field_is_valid(jsite_config)) {
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size, "site_config is not valid JSON");
			return 400;
		}
	}

	if (!have_any) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size,
		         "at least one of container_defs/networks/dns_records/pkg_installed/pkg_recipes/"
		         "site_config required");
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
	if (jsite_config != NULL &&
	    restore_write_field(SITE_CONFIG_PATH, json_as_string(jsite_config)) != 0) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size, "failed to write site_config.json");
		return 500;
	}
	if (jpkg_recipes != NULL) {
		if (persist_mkdir_p(PKG_RECIPES_DIR) != 0) {
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size, "failed to create recipes directory");
			return 500;
		}
		for (i = 0; i < jpkg_recipes->u.object.count; i++) {
			const char *key = jpkg_recipes->u.object.keys[i];
			const char *slash = strchr(key, '/');
			char version_dir[PATH_MAX];
			char path[PATH_MAX];

			/* Already validated above (key is "<name>/<version>",
			 * slash guaranteed non-NULL and interior) -- rebuilds the
			 * exact <name>/<version>/build.sh path do_system_backup()
			 * read this same content from (ADR-0120). */
			snprintf(version_dir, sizeof(version_dir), "%s/%.*s/%s", PKG_RECIPES_DIR,
			         (int)(slash - key), key, slash + 1);
			if (persist_mkdir_p(version_dir) != 0) {
				json_free(root);
				snprintf(out_errmsg, out_errmsg_size, "failed to create recipe directory for %s",
				         key);
				return 500;
			}
			snprintf(path, sizeof(path), "%s/build.sh", version_dir);
			if (restore_write_field(path, json_as_string(jpkg_recipes->u.object.values[i])) !=
			    0) {
				json_free(root);
				snprintf(out_errmsg, out_errmsg_size, "failed to write recipe %s", key);
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

/*
 * Reissues this install's own well-known "host" leaf so its SAN
 * always matches the current site config (ADR-0050) -- a fixed
 * record name ("host"), never the FQDN itself, so a rename (any of
 * instance_name/site_name/domain_suffix changing) is a clean
 * delete-then-recreate under the identical stable identity rather
 * than orphaning one differently-named cert per rename. CN ends up
 * being "host" (pki_cert_create()'s own `name` doubles as CN), which
 * is fine and arguably correct -- modern TLS verification uses SAN,
 * not CN, for hostname matching; the SAN is the real, current FQDN.
 *
 * Best-effort, never fails the caller: no root CA bootstrapped yet is
 * the common, expected case on a fresh install with site config set
 * before PKI ever is -- logged, not surfaced as an error on whatever
 * unrelated operation (a site PUT, a CA bootstrap, a reset) triggered
 * this call.
 */
static void reissue_host_pki_cert(void)
{
	char fqdn[SITECONFIG_NAME_MAX * 3];
	const char *sans[1];
	struct json_writer scratch;
	enum pki_error perr;

	if (!pki_ca_bootstrapped())
		return;

	siteconfig_host_fqdn(fqdn, sizeof(fqdn));
	if (!dns_name_is_valid(fqdn)) {
		fprintf(stderr, "host: composed FQDN '%s' is not a valid DNS name, skipping "
		                 "PKI (re)issue\n",
		        fqdn);
		return;
	}

	pki_cert_delete("host"); /* PKI_ERR_NOT_FOUND (first time) is fine, ignored */

	sans[0] = fqdn;
	jw_init(&scratch);
	/* "__host" (not "host"): a reserved owner sentinel distinct from
	 * any real container name, so this cert's own ownership can never
	 * collide with (and be wrongly wiped by) a real container someone
	 * happens to name "host" -- same reserved-pseudo-name convention
	 * PKG_BUILD_CONTAINER_NAME ("__pkgbuild") already established. */
	perr = pki_cert_create("host", sans, 1, 365, "__host", &scratch);
	jw_free(&scratch);
	if (perr != PKI_OK)
		fprintf(stderr, "host: could not (re)issue PKI cert for %s (err=%d)\n", fqdn,
		        (int)perr);
}

/*
 * Auto-maintains a single DNS record for this install's own qualified
 * FQDN, pointed at the daemon's own --bind= address (ADR-0053) -- the
 * "reflects back" design: not a second, independently-editable
 * record, this record's own name simply tracks whatever the FQDN
 * currently is, the same relationship reissue_host_pki_cert() already
 * has with the PKI leaf above.
 *
 * Only meaningful when --bind= is a real, specific address the
 * operator explicitly chose: "0.0.0.0" (bind-everywhere) and
 * "127.0.0.1" (loopback-only, unreachable from anywhere else) have no
 * single correct IP to publish, so both are a deliberate no-op here,
 * never a guess.
 *
 * g_last_instance_dns_name tracks what this function itself last
 * registered, in-process only (not persisted) -- enough to delete the
 * old name before creating the new one when the FQDN changes while
 * the daemon keeps running (the actual, tested scenario). A rename
 * that happens to straddle a daemon restart with no PUT ever
 * registering the "old" name in this process's own lifetime is a
 * real, narrow, accepted gap (see ADR-0053's own Consequences), not
 * solved here.
 */
static char g_last_instance_dns_name[DNS_NAME_MAX];

static void reconcile_instance_dns_record(void)
{
	char fqdn[SITECONFIG_NAME_MAX * 3];
	struct in_addr addr;
	struct dns_record *rec;
	enum dns_error derr;

	if (strcmp(g_bind_addr, "0.0.0.0") == 0 || strcmp(g_bind_addr, "127.0.0.1") == 0)
		return;
	if (inet_pton(AF_INET, g_bind_addr, &addr) != 1)
		return;

	siteconfig_host_fqdn(fqdn, sizeof(fqdn));
	if (!dns_name_is_valid(fqdn)) {
		fprintf(stderr,
		        "instance DNS record: composed FQDN '%s' is not a valid DNS name, skipping\n",
		        fqdn);
		return;
	}

	if (g_last_instance_dns_name[0] != '\0' && strcmp(g_last_instance_dns_name, fqdn) != 0)
		dns_record_delete(g_last_instance_dns_name);
	dns_record_delete(fqdn); /* in case this exact name is already registered (idempotent re-call) */

	/* "__host" owner sentinel, same reasoning as reissue_host_pki_cert()
	 * above -- never a real container name, so this record's own
	 * ownership can't collide with one. */
	derr = dns_record_create(fqdn, addr.s_addr, "__host", &rec);
	if (derr != DNS_OK) {
		fprintf(stderr, "instance DNS record: could not (re)create %s (err=%d)\n", fqdn,
		        (int)derr);
		return;
	}
	snprintf(g_last_instance_dns_name, sizeof(g_last_instance_dns_name), "%s", fqdn);
}

/*
 * GET/PUT /v1/system/site (ADR-0046): this install's own declared
 * site_name/domain_suffix, a real convenience for client tooling to
 * suggest a default FQDN with, never enforced by anything on the
 * daemon side -- see siteconfig.h's own comment for the full
 * rationale. GET always 200s (siteconfig_init() already applied
 * defaults at startup; there is no "not configured yet" state the way
 * PKI's bootstrap-gated resources have).
 */
static void handle_site_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	siteconfig_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_site_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *instance_name, *site_name, *domain_suffix;
	enum siteconfig_error serr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	instance_name = json_as_string(json_object_get(root, "instance_name"));
	site_name = json_as_string(json_object_get(root, "site_name"));
	domain_suffix = json_as_string(json_object_get(root, "domain_suffix"));
	if (instance_name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "instance_name is required");
		return;
	}
	if (domain_suffix == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "domain_suffix is required");
		return;
	}

	serr = siteconfig_set(instance_name, site_name, domain_suffix);
	json_free(root);

	if (serr != SITECONFIG_OK) {
		switch (serr) {
		case SITECONFIG_ERR_INVALID_INSTANCE_NAME:
			respond_error(fd, 400, "Bad Request", "invalid instance_name");
			break;
		case SITECONFIG_ERR_INVALID_SITE_NAME:
			respond_error(fd, 400, "Bad Request", "invalid site_name");
			break;
		case SITECONFIG_ERR_INVALID_DOMAIN_SUFFIX:
			respond_error(fd, 400, "Bad Request", "invalid domain_suffix");
			break;
		case SITECONFIG_ERR_PERSIST_FAILED:
		default:
			respond_error(fd, 500, "Internal Server Error", "could not persist site config");
			break;
		}
		return;
	}

	reissue_host_pki_cert();
	reconcile_instance_dns_record();

	jw_init(&w);
	siteconfig_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Builds a fresh SSL_CTX from the PKI subsystem's already-issued
 * "host" leaf certificate (PKI_CERTS_DIR/host.{crt,key}, reissue_host_
 * pki_cert()) -- reusing that existing cert-issuance path rather than
 * inventing a second one, per Part 0.5's design. Returns NULL (with a
 * diagnostic already printed) if the CA hasn't been bootstrapped yet,
 * or the host cert/key can't be loaded/don't match -- HTTPS simply
 * isn't available yet in that case, exactly the same "not configured"
 * posture other PKI-gated resources in this daemon already have.
 *
 * ADR-0136: when an intermediate CA is bootstrapped, host.crt (like
 * every leaf pki_cert_create() issues) is signed BY the intermediate,
 * but is itself still just the one leaf certificate on disk --
 * SSL_CTX_use_certificate_file() below loads exactly that one
 * certificate and nothing past it, even if the file had more (it
 * doesn't parse a chain the way SSL_CTX_use_certificate_chain_file()
 * would). Confirmed live: a client trusting only the root CA can
 * never validate leaf->intermediate->root with just the leaf in hand
 * -- it needs the intermediate cert as part of the handshake too.
 * pki_intermediate_cert_pem() + SSL_CTX_add_extra_chain_cert() below
 * add it explicitly, the OpenSSL-idiomatic way to extend an
 * already-loaded leaf's own chain without touching host.crt itself
 * (which pki_cert_deliver()'s own, separate leaf+intermediate
 * chain-building for pki_issue-delivered container certs already
 * assumes is leaf-only -- changing that here would double the
 * intermediate there).
 */
static SSL_CTX *create_tls_ctx(void)
{
	SSL_CTX *ctx;
	char crt_path[PATH_MAX], key_path[PATH_MAX];

	if (!pki_ca_bootstrapped()) {
		fprintf(stderr, "create_tls_ctx: PKI not bootstrapped yet -- no host cert to serve HTTPS with\n");
		return NULL;
	}
	snprintf(crt_path, sizeof(crt_path), "%s/host.crt", PKI_CERTS_DIR);
	snprintf(key_path, sizeof(key_path), "%s/host.key", PKI_CERTS_DIR);

	ctx = SSL_CTX_new(TLS_server_method());
	if (ctx == NULL) {
		ERR_print_errors_fp(stderr);
		return NULL;
	}
	if (SSL_CTX_use_certificate_file(ctx, crt_path, SSL_FILETYPE_PEM) != 1 ||
	    SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1 ||
	    SSL_CTX_check_private_key(ctx) != 1) {
		ERR_print_errors_fp(stderr);
		SSL_CTX_free(ctx);
		return NULL;
	}

	{
		char *intermediate_pem = NULL;
		size_t intermediate_len = 0;
		int r = pki_intermediate_cert_pem(&intermediate_pem, &intermediate_len);

		if (r < 0) {
			fprintf(stderr, "create_tls_ctx: could not read the intermediate CA cert\n");
			SSL_CTX_free(ctx);
			return NULL;
		}
		if (r == 1) {
			BIO *bio = BIO_new_mem_buf(intermediate_pem, (int)intermediate_len);
			X509 *intermediate_x509 = bio != NULL ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;

			if (bio != NULL)
				BIO_free(bio);
			free(intermediate_pem);
			if (intermediate_x509 == NULL) {
				fprintf(stderr, "create_tls_ctx: could not parse the intermediate CA cert\n");
				SSL_CTX_free(ctx);
				return NULL;
			}
			/* Ownership transfers to ctx on success -- never X509_free()
			 * this one ourselves. */
			if (SSL_CTX_add_extra_chain_cert(ctx, intermediate_x509) != 1) {
				ERR_print_errors_fp(stderr);
				X509_free(intermediate_x509);
				SSL_CTX_free(ctx);
				return NULL;
			}
		}
	}

	return ctx;
}

/*
 * Creates, binds, and listens a real TCP socket at bind_addr:port --
 * the one source of truth for that sequence, shared by main()'s own
 * initial bind and rebind_listener()'s live-rebind path below (Part
 * 0.5). SOCK_NONBLOCK/SOCK_CLOEXEC for the same reason every other fd
 * this daemon holds open is CLOEXEC -- see main()'s own comment at its
 * call site. Returns a ready-to-epoll fd, or -1 (already logged) on
 * any failure -- never partially bound.
 */
static int create_listen_socket(const char *bind_addr, int port)
{
	int fd;
	int opt = 1;
	struct sockaddr_in addr;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
		fprintf(stderr, "create_listen_socket: invalid bind address %s\n", bind_addr);
		close(fd);
		return -1;
	}

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("create_listen_socket: bind");
		close(fd);
		return -1;
	}
	if (listen(fd, 128) != 0) {
		perror("create_listen_socket: listen");
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * Live listen-socket rebind (Part 0.5) -- kanxeod is real PID 1 under
 * --init-mode (confirmed: image/src/kanxeo-install.c's loader entry
 * uses "init=/bin/kanxeod"), so there is no "restart the daemon" to
 * pick up a new bind address or port; this closes the old listening
 * socket and opens a new one in-process instead. Already-accept()ed
 * connections (a separate fd from the listening socket) are completely
 * unaffected -- the request that triggered this rebind (e.g. PUT
 * /v1/system/daemon-config) finishes normally over its own connection.
 *
 * Ordering matters: the new socket is created and added to epoll
 * *before* the old one is torn down, so any failure rolls back to the
 * still-working old listener -- this daemon must never be left with
 * zero listeners, even transiently. A no-op (new == current) is
 * detected up front rather than relying on SO_REUSEADDR semantics for
 * an identical rebind.
 */
static int rebind_listener(const char *new_bind_addr, int new_port)
{
	int new_fd;
	int old_fd;
	struct kx_epoll_event ev;

	if (strcmp(new_bind_addr, g_bind_addr) == 0 && new_port == g_port)
		return 0;

	new_fd = create_listen_socket(new_bind_addr, new_port);
	if (new_fd < 0)
		return -1;

	old_fd = g_listener_conn.fd;
	g_listener_conn.fd = new_fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_listener_conn;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, new_fd, &ev) != 0) {
		perror("rebind_listener: epoll_ctl ADD");
		g_listener_conn.fd = old_fd;
		close(new_fd);
		return -1;
	}

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, old_fd, NULL);
	close(old_fd);

	snprintf(g_bind_addr_buf, sizeof(g_bind_addr_buf), "%s", new_bind_addr);
	g_bind_addr = g_bind_addr_buf;
	g_port = new_port;

	printf("kanxeod rebound listener to %s:%d\n", new_bind_addr, new_port);
	fflush(stdout);
	return 0;
}

/* Best-effort, same "never block daemon startup" posture as every
 * other non-critical reconciliation step (cgroup_enable_io_
 * accounting(), swap_init()'s own swapon() retry) -- a test/dev
 * invocation, or a kernel with /dev/kmsg unreadable for any other
 * reason, simply never gets kernel-source log entries; every other
 * source (kanxeod's own diagnostics, the audit trail) is unaffected. */
static void start_kmsg_watch(void)
{
	struct kx_epoll_event ev;
	int fd = logstore_kmsg_fd();

	if (fd < 0)
		return;
	g_kmsg_conn.kind = CONN_KMSG;
	g_kmsg_conn.fd = fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_kmsg_conn;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0)
		g_kmsg_conn.fd = -1;
}

static void handle_kmsg_event(struct conn *cc)
{
	(void)cc;
	logstore_kmsg_readable();
}

static int start_http_listener(const char *bind_addr, int port)
{
	struct kx_epoll_event ev;
	int fd = create_listen_socket(bind_addr, port);

	if (fd < 0)
		return -1;
	g_listener_conn.kind = CONN_LISTENER;
	g_listener_conn.fd = fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_listener_conn;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		perror("start_http_listener: epoll_ctl ADD");
		close(fd);
		g_listener_conn.fd = -1;
		return -1;
	}
	printf("kanxeod listening on %s:%d\n", bind_addr, port);
	fflush(stdout);
	return 0;
}

static void stop_http_listener(void)
{
	if (g_listener_conn.fd < 0)
		return;
	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_listener_conn.fd, NULL);
	close(g_listener_conn.fd);
	g_listener_conn.fd = -1;
}

/*
 * The four HTTPS listener lifecycle operations (Part 0.5), mirroring
 * the plain-HTTP ones above -- start/stop/rebind, plus create_tls_ctx()
 * itself for the cert-loading half. g_tls_ctx, once successfully
 * built, is deliberately kept alive across a later stop/restart cycle
 * (stop_https_listener() only tears down the socket) -- re-enabling
 * HTTPS doesn't need to re-read the host cert off disk every time.
 */
static int start_https_listener(const char *bind_addr, int port)
{
	struct kx_epoll_event ev;
	int fd;

	if (g_tls_ctx == NULL) {
		g_tls_ctx = create_tls_ctx();
		if (g_tls_ctx == NULL)
			return -1;
	}
	fd = create_listen_socket(bind_addr, port);
	if (fd < 0)
		return -1;

	g_https_listener_conn.kind = CONN_LISTENER_TLS;
	g_https_listener_conn.fd = fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_https_listener_conn;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		perror("start_https_listener: epoll_ctl ADD");
		close(fd);
		g_https_listener_conn.fd = -1;
		return -1;
	}
	printf("kanxeod listening (https) on %s:%d\n", bind_addr, port);
	fflush(stdout);
	return 0;
}

static void stop_https_listener(void)
{
	if (g_https_listener_conn.fd < 0)
		return;
	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_https_listener_conn.fd, NULL);
	close(g_https_listener_conn.fd);
	g_https_listener_conn.fd = -1;
}

static int rebind_https_listener(const char *new_bind_addr, int new_port)
{
	int new_fd;
	int old_fd;
	struct kx_epoll_event ev;

	/* Same no-op short-circuit rebind_listener() has, and for the same
	 * reason: binding new_bind_addr:new_port a second time while the
	 * existing https listener is still open on that identical tuple
	 * fails with EADDRINUSE even with SO_REUSEADDR (that only helps
	 * across a TIME_WAIT teardown, not two simultaneously-open
	 * listeners) -- a real, reproducible failure this project's own
	 * "verify before trusting" testing actually hit, not a hypothetical
	 * edge case. */
	if (strcmp(new_bind_addr, g_bind_addr) == 0 && new_port == daemon_config_https_port())
		return 0;

	new_fd = create_listen_socket(new_bind_addr, new_port);
	if (new_fd < 0)
		return -1;

	old_fd = g_https_listener_conn.fd;
	g_https_listener_conn.fd = new_fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_https_listener_conn;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, new_fd, &ev) != 0) {
		perror("rebind_https_listener: epoll_ctl ADD");
		g_https_listener_conn.fd = old_fd;
		close(new_fd);
		return -1;
	}

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, old_fd, NULL);
	close(old_fd);

	printf("kanxeod rebound https listener to %s:%d\n", new_bind_addr, new_port);
	fflush(stdout);
	return 0;
}

/*
 * Deferred rtnl_addr_del_ipv4() of a stale bind_ip (ADR-0068), arm'd by
 * handle_daemon_config_put() below instead of deleting synchronously --
 * a real hazard was confirmed the hard way, not assumed: when the very
 * connection carrying the PUT request that clears/replaces bind_ip has
 * that address as its own local endpoint (the common case, since an
 * operator naturally reaches the daemon at whatever it's currently
 * bound to), the response write() succeeds at the socket-buffer level
 * but the address disappearing before the kernel actually transmits it
 * leaves the client hanging forever, never receiving bytes that were
 * already "successfully" written. A short, fixed delay -- long enough
 * for a same-host TCP round trip under any real scheduler load, utterly
 * negligible for a background bridge address cleanup that has no
 * user-visible deadline -- sidesteps the whole class of problem without
 * needing to reason about exact ACK timing. Same one-shot timerfd +
 * conn shape as arm_restart_timer() (below, container restart/backoff)
 * uses -- this codebase's own existing precedent for "do something
 * shortly after, without blocking the event loop."
 */
#define BIND_IP_CLEANUP_DELAY_SECONDS 2

static void arm_bind_ip_cleanup_timer(const char *ifname, uint32_t addr_be, int prefix_len)
{
	int tfd;
	struct itimerspec its;
	struct conn *cc;
	struct kx_epoll_event ev;

	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (tfd < 0) {
		perror("timerfd_create (bind_ip cleanup)");
		return;
	}

	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = BIND_IP_CLEANUP_DELAY_SECONDS;
	if (timerfd_settime(tfd, 0, &its, NULL) != 0) {
		perror("timerfd_settime (bind_ip cleanup)");
		close(tfd);
		return;
	}

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (bind_ip cleanup timer conn)");
		close(tfd);
		return;
	}
	cc->kind = CONN_BIND_IP_CLEANUP;
	cc->fd = tfd;
	snprintf(cc->cleanup_ifname, sizeof(cc->cleanup_ifname), "%s", ifname);
	cc->cleanup_addr_be = addr_be;
	cc->cleanup_prefix_len = prefix_len;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, tfd, &ev) != 0) {
		perror("epoll_ctl ADD bind_ip cleanup timer");
		close(tfd);
		free(cc);
	}
}

static void handle_bind_ip_cleanup_timer_event(struct conn *cc)
{
	uint64_t expirations;
	int rtfd;

	if (read(cc->fd, &expirations, sizeof(expirations)) < 0)
		perror("read (bind_ip cleanup timerfd)");

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	close(cc->fd);

	rtfd = rtnl_open();
	if (rtfd >= 0) {
		rtnl_addr_del_ipv4(rtfd, cc->cleanup_ifname, cc->cleanup_addr_be, cc->cleanup_prefix_len);
		rtnl_close(rtfd);
	}
	/* Best-effort, same as before this became a deferred timer: a
	 * failure here leaves a harmless leftover address on the bridge,
	 * never persisted or bound to by kanxeod itself. */

	free(cc);
}

/*
 * GET/POST /v1/system/ping (task #679-681): a real ICMP echo, backed
 * by ping.c's own raw-socket mechanics. Two fds are in flight for one
 * logical job -- the raw socket (an echo reply arriving) and a
 * timerfd (the timeout) -- whichever fires first resolves the job and
 * tears down BOTH. Same batch-safety hazard console_session_teardown()
 * already solves (both could be EPOLLIN-ready in the same epoll_wait()
 * batch): torn-down conns are marked CONN_DEAD and queued via
 * queue_conn_free(), never free()'d directly.
 */
#define PING_TIMEOUT_MS 2000

static struct conn *g_ping_sock_conn;
static struct conn *g_ping_timer_conn;

static void ping_job_teardown(void)
{
	if (g_ping_sock_conn != NULL) {
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_ping_sock_conn->fd, NULL);
		close(g_ping_sock_conn->fd);
		g_ping_sock_conn->kind = CONN_DEAD;
		queue_conn_free(g_ping_sock_conn);
		g_ping_sock_conn = NULL;
	}
	if (g_ping_timer_conn != NULL) {
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_ping_timer_conn->fd, NULL);
		close(g_ping_timer_conn->fd);
		g_ping_timer_conn->kind = CONN_DEAD;
		queue_conn_free(g_ping_timer_conn);
		g_ping_timer_conn = NULL;
	}
}

static void handle_ping_socket_event(struct conn *cc)
{
	if (ping_handle_reply(cc->fd))
		ping_job_teardown();
}

static void handle_ping_timer_event(struct conn *cc)
{
	uint64_t expirations;

	if (read(cc->fd, &expirations, sizeof(expirations)) < 0)
		perror("read (ping timerfd)");
	ping_handle_timeout();
	ping_job_teardown();
}

/* Body: {"host": "A.B.C.D"}. 202 with the job's initial (pending)
 * status; GET on the same path polls it, same "kick off + poll"
 * shape as disks format/ISO build/every other uncertain-duration job
 * in this daemon. */
static void handle_ping_post(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *host_str;
	struct in_addr addr;
	int sockfd;
	enum ping_error perr;
	struct conn *sock_cc, *timer_cc;
	struct kx_epoll_event ev;
	int tfd;
	struct itimerspec its;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	host_str = json_as_string(json_object_get(root, "host"));
	if (host_str == NULL || inet_pton(AF_INET, host_str, &addr) != 1) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "host missing or not a valid IPv4 address");
		return;
	}
	json_free(root);

	perr = ping_start(addr.s_addr, &sockfd);
	if (perr == PING_ERR_BUSY) {
		respond_error(fd, 409, "Conflict", "another ping is already in flight");
		return;
	}
	if (perr != PING_OK) {
		respond_error(fd, 500, "Internal Server Error",
		              "could not send ICMP echo (raw socket failed -- CAP_NET_RAW?)");
		return;
	}

	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (tfd < 0) {
		perror("timerfd_create (ping timeout)");
		close(sockfd);
		respond_error(fd, 500, "Internal Server Error", "could not arm ping timeout");
		return;
	}
	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = PING_TIMEOUT_MS / 1000;
	its.it_value.tv_nsec = (long)(PING_TIMEOUT_MS % 1000) * 1000000L;
	if (timerfd_settime(tfd, 0, &its, NULL) != 0) {
		perror("timerfd_settime (ping timeout)");
		close(tfd);
		close(sockfd);
		respond_error(fd, 500, "Internal Server Error", "could not arm ping timeout");
		return;
	}

	sock_cc = malloc(sizeof(*sock_cc));
	timer_cc = malloc(sizeof(*timer_cc));
	if (sock_cc == NULL || timer_cc == NULL) {
		perror("malloc (ping job conns)");
		free(sock_cc);
		free(timer_cc);
		close(tfd);
		close(sockfd);
		respond_error(fd, 500, "Internal Server Error", "out of memory");
		return;
	}
	sock_cc->kind = CONN_PING;
	sock_cc->fd = sockfd;
	timer_cc->kind = CONN_PING_TIMER;
	timer_cc->fd = tfd;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = sock_cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, sockfd, &ev) != 0) {
		perror("epoll_ctl ADD ping socket");
		free(sock_cc);
		free(timer_cc);
		close(tfd);
		close(sockfd);
		respond_error(fd, 500, "Internal Server Error", "could not register ping socket");
		return;
	}
	ev.data.ptr = timer_cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, tfd, &ev) != 0) {
		perror("epoll_ctl ADD ping timer");
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, sockfd, NULL);
		free(sock_cc);
		free(timer_cc);
		close(tfd);
		close(sockfd);
		respond_error(fd, 500, "Internal Server Error", "could not register ping timeout");
		return;
	}

	g_ping_sock_conn = sock_cc;
	g_ping_timer_conn = timer_cc;

	jw_init(&w);
	ping_write_json_status(&w);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}

static void handle_ping_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	ping_write_json_status(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * NTP sync job (task #751-755): one in-flight SNTP attempt, mirroring
 * ping's own two-fd (socket + timeout) shape above -- the one real
 * difference is ntp.c's own candidate-retry logic means a timeout can
 * result in "move to the next candidate, re-arm the SAME timer" rather
 * than always tearing down (see arm_ntp_timeout_timer() below).
 */
#define NTP_SYNC_INTERVAL_SEC (60 * 60) /* re-sync hourly once a sync mechanism exists at all */

static struct conn *g_ntp_sync_conn;
static struct conn *g_ntp_timer_conn;
static struct conn g_ntp_periodic_conn; /* permanent -- registered once at startup */

static void ntp_job_teardown(void)
{
	if (g_ntp_sync_conn != NULL) {
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_ntp_sync_conn->fd, NULL);
		close(g_ntp_sync_conn->fd);
		g_ntp_sync_conn->kind = CONN_DEAD;
		queue_conn_free(g_ntp_sync_conn);
		g_ntp_sync_conn = NULL;
	}
	if (g_ntp_timer_conn != NULL) {
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_ntp_timer_conn->fd, NULL);
		close(g_ntp_timer_conn->fd);
		g_ntp_timer_conn->kind = CONN_DEAD;
		queue_conn_free(g_ntp_timer_conn);
		g_ntp_timer_conn = NULL;
	}
}

/* Re-arms g_ntp_timer_conn's own timerfd for the next candidate --
 * reused (not recreated) across every candidate in one sync attempt,
 * unlike ping's own always-one-shot timer. */
static int arm_ntp_timeout_timer(void)
{
	struct itimerspec its;

	if (g_ntp_timer_conn == NULL)
		return -1;
	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = NTP_SYNC_TIMEOUT_MS / 1000;
	its.it_value.tv_nsec = (long)(NTP_SYNC_TIMEOUT_MS % 1000) * 1000000L;
	return timerfd_settime(g_ntp_timer_conn->fd, 0, &its, NULL);
}

static void handle_ntp_sync_socket_event(struct conn *cc)
{
	if (ntp_sync_handle_reply(cc->fd)) {
		ntp_job_teardown();
		return;
	}
	if (arm_ntp_timeout_timer() != 0) {
		perror("timerfd_settime (ntp sync re-arm)");
		ntp_job_teardown();
	}
}

static void handle_ntp_sync_timer_event(struct conn *cc)
{
	uint64_t expirations;

	if (read(cc->fd, &expirations, sizeof(expirations)) < 0)
		perror("read (ntp sync timerfd)");
	if (ntp_sync_handle_timeout(g_ntp_sync_conn != NULL ? g_ntp_sync_conn->fd : -1)) {
		ntp_job_teardown();
		return;
	}
	if (arm_ntp_timeout_timer() != 0) {
		perror("timerfd_settime (ntp sync re-arm)");
		ntp_job_teardown();
	}
}

/* Kicks off one sync attempt if none is currently in flight. Returns
 * the outcome so callers can distinguish "started" from "nothing to
 * do" -- the periodic timer (best-effort, ignores the return value:
 * NTP_START_BUSY/NTP_START_NO_CANDIDATES are the common,
 * nothing-configured-yet case, not a real error, and the next fire
 * tries again regardless) vs. the manual POST /v1/system/ntp/sync
 * trigger below, which does report it to the caller. */
static enum ntp_start_error start_ntp_sync_job(void)
{
	int sockfd;
	enum ntp_start_error serr;
	struct conn *sock_cc, *timer_cc;
	struct kx_epoll_event ev;
	int tfd;
	struct itimerspec its;

	serr = ntp_sync_start(&sockfd);
	if (serr != NTP_START_OK)
		return serr;

	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (tfd < 0) {
		perror("timerfd_create (ntp sync timeout)");
		close(sockfd);
		return NTP_START_SOCKET_FAILED;
	}
	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = NTP_SYNC_TIMEOUT_MS / 1000;
	its.it_value.tv_nsec = (long)(NTP_SYNC_TIMEOUT_MS % 1000) * 1000000L;
	if (timerfd_settime(tfd, 0, &its, NULL) != 0) {
		perror("timerfd_settime (ntp sync timeout)");
		close(tfd);
		close(sockfd);
		return NTP_START_SOCKET_FAILED;
	}

	sock_cc = malloc(sizeof(*sock_cc));
	timer_cc = malloc(sizeof(*timer_cc));
	if (sock_cc == NULL || timer_cc == NULL) {
		perror("malloc (ntp sync job conns)");
		free(sock_cc);
		free(timer_cc);
		close(tfd);
		close(sockfd);
		return NTP_START_SOCKET_FAILED;
	}
	sock_cc->kind = CONN_NTP_SYNC;
	sock_cc->fd = sockfd;
	timer_cc->kind = CONN_NTP_SYNC_TIMER;
	timer_cc->fd = tfd;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = sock_cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, sockfd, &ev) != 0) {
		perror("epoll_ctl ADD ntp sync socket");
		free(sock_cc);
		free(timer_cc);
		close(tfd);
		close(sockfd);
		return NTP_START_SOCKET_FAILED;
	}
	ev.data.ptr = timer_cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, tfd, &ev) != 0) {
		perror("epoll_ctl ADD ntp sync timer");
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, sockfd, NULL);
		free(sock_cc);
		free(timer_cc);
		close(tfd);
		close(sockfd);
		return NTP_START_SOCKET_FAILED;
	}

	g_ntp_sync_conn = sock_cc;
	g_ntp_timer_conn = timer_cc;
	return NTP_START_OK;
}

/* Permanent, re-arming itself every NTP_SYNC_INTERVAL_SEC -- the
 * "schedule the next one-shot fire on completion" idiom this daemon
 * already uses everywhere else (e.g. the console-respawn timer),
 * rather than a genuinely recurring timerfd (it_interval) -- this
 * codebase has no precedent for the latter yet, and re-arming keeps
 * this consistent with every other timer here. */
static void arm_ntp_periodic_timer(void)
{
	struct itimerspec its;

	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = NTP_SYNC_INTERVAL_SEC;
	if (timerfd_settime(g_ntp_periodic_conn.fd, 0, &its, NULL) != 0)
		perror("timerfd_settime (ntp periodic re-arm)");
}

static void handle_ntp_periodic_timer_event(struct conn *cc)
{
	uint64_t expirations;

	if (read(cc->fd, &expirations, sizeof(expirations)) < 0)
		perror("read (ntp periodic timerfd)");
	start_ntp_sync_job();
	arm_ntp_periodic_timer();
}

/* Best-effort, same "never block daemon startup" posture as start_
 * kmsg_watch() above -- a timerfd_create() failure here just means no
 * automatic sync ever happens; GET/PUT /v1/system/ntp and /v1/ntp/
 * servers remain fully usable regardless. */
static void start_ntp_periodic_timer(void)
{
	struct kx_epoll_event ev;
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

	if (fd < 0) {
		perror("timerfd_create (ntp periodic)");
		return;
	}
	g_ntp_periodic_conn.kind = CONN_NTP_PERIODIC_TIMER;
	g_ntp_periodic_conn.fd = fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_ntp_periodic_conn;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		close(fd);
		g_ntp_periodic_conn.fd = -1;
		return;
	}
	arm_ntp_periodic_timer();
}

/*
 * GET/PUT /v1/system/ntp: the host's own upstream NTP server address
 * list (task #751), the exact same shape GET/PUT /v1/system/resolv
 * already has for DNS -- PUT {"upstream": [...]} replaces the full
 * list, an empty array clears it (the host then relies solely on any
 * registered server containers, if any -- see /v1/ntp/servers below).
 * Sync status (last attempt's outcome) is a separate resource, GET
 * /v1/system/ntp/status -- config and live job state are two
 * different concerns, the same split daemon_config/swap-status and
 * diskrole/format-status already keep.
 */
static void handle_ntp_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	ntp_write_json_config(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void respond_ntp_error(int fd, enum ntp_error err)
{
	switch (err) {
	case NTP_ERR_INVALID_IP:
		respond_error(fd, 400, "Bad Request", "upstream must be valid IPv4 addresses");
		break;
	case NTP_ERR_TOO_MANY: {
		char msg[64];

		snprintf(msg, sizeof(msg), "too many upstream addresses (max %d)", NTP_MAX_UPSTREAM);
		respond_error(fd, 400, "Bad Request", msg);
		break;
	}
	case NTP_ERR_PERSIST_FAILED:
		respond_error(fd, 500, "Internal Server Error", "could not persist ntp.conf");
		break;
	case NTP_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "this container is already registered");
		break;
	case NTP_ERR_FULL:
		respond_error(fd, 400, "Bad Request", "too many registered NTP servers");
		break;
	case NTP_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such registration");
		break;
	case NTP_ERR_CONTAINER_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such container");
		break;
	case NTP_ERR_CONTAINER_NOT_RUNNING:
		respond_error(fd, 404, "Not Found", "no such running container");
		break;
	case NTP_ERR_INVALID_TIME:
		respond_error(fd, 400, "Bad Request", "invalid unixtime, or clock_settime() failed");
		break;
	case NTP_OK:
		break;
	}
}

static void handle_ntp_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *arr;
	const char *upstream[NTP_MAX_UPSTREAM];
	int count;
	size_t i;
	enum ntp_error nerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	arr = json_object_get(root, "upstream");
	if (arr == NULL || arr->type != JSON_ARRAY) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "upstream (array) is required");
		return;
	}
	if (arr->u.array.count > NTP_MAX_UPSTREAM) {
		json_free(root);
		respond_ntp_error(fd, NTP_ERR_TOO_MANY);
		return;
	}
	count = (int)arr->u.array.count;
	for (i = 0; i < arr->u.array.count; i++) {
		upstream[i] = json_as_string(arr->u.array.items[i]);
		if (upstream[i] == NULL) {
			json_free(root);
			respond_ntp_error(fd, NTP_ERR_INVALID_IP);
			return;
		}
	}

	nerr = ntp_set_upstream(upstream, count);
	json_free(root);
	if (nerr != NTP_OK) {
		respond_ntp_error(fd, nerr);
		return;
	}

	jw_init(&w);
	ntp_write_json_config(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_ntp_status_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	ntp_write_json_status(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* POST /v1/system/ntp/sync: triggers one sync attempt on demand,
 * rather than waiting for the next hourly automatic fire
 * (NTP_SYNC_INTERVAL_SEC) -- an operator who just set upstream
 * servers or registered a server container has no other way to see
 * the result without waiting up to an hour. Same v1
 * one-job-in-flight/202-Accepted-then-poll-status shape POST
 * /v1/system/ping already established; GET /v1/system/ntp/status
 * reports the outcome once it lands. */
static void handle_ntp_sync_post(int fd)
{
	enum ntp_start_error serr = start_ntp_sync_job();

	if (serr == NTP_START_BUSY) {
		respond_error(fd, 409, "Conflict", "a sync attempt is already in flight");
		return;
	}
	if (serr == NTP_START_NO_CANDIDATES) {
		respond_error(fd, 400, "Bad Request",
		              "no NTP servers configured -- set upstream (PUT /v1/system/ntp) or "
		              "register a server container (POST /v1/ntp/servers) first");
		return;
	}
	if (serr != NTP_START_OK) {
		respond_error(fd, 500, "Internal Server Error",
		              "could not start sync (socket/timer setup failed)");
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 202, "Accepted", "application/json", "", 0);
}

/* POST/GET/DELETE /v1/ntp/servers (task #753): registering a running
 * container as an internal NTP time source, mirroring dns_server_
 * create/list/delete's own REST shape exactly -- see ntp.h's own
 * header comment for why no pid/pidfd is needed here (pure
 * bookkeeping, unlike DNS/LDAP server registration). */
static void handle_ntp_server_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *container_name;
	enum ntp_error nerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	container_name = json_as_string(json_object_get(root, "container"));
	if (container_name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "container missing");
		return;
	}

	nerr = ntp_server_register(container_name);
	if (nerr != NTP_OK) {
		json_free(root);
		respond_ntp_error(fd, nerr);
		return;
	}

	/* container_name still points into root -- build the response
	 * before freeing it, matching handle_dns_server_create()'s own
	 * use-after-free-avoidance ordering. */
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container_name);
	jw_obj_close(&w);
	json_free(root);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_ntp_server_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "servers");
	ntp_server_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_ntp_server_delete(int fd, const char *name)
{
	enum ntp_error nerr = ntp_server_unregister(name);

	if (nerr != NTP_OK) {
		respond_ntp_error(fd, nerr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void respond_syslogfwd_error(int fd, enum syslogfwd_error err)
{
	switch (err) {
	case SYSLOGFWD_ERR_PERSIST_FAILED:
		respond_error(fd, 500, "Internal Server Error", "could not persist syslog_targets.json");
		break;
	case SYSLOGFWD_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "this container is already registered");
		break;
	case SYSLOGFWD_ERR_FULL:
		respond_error(fd, 400, "Bad Request", "too many registered syslog forward targets");
		break;
	case SYSLOGFWD_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such registration");
		break;
	case SYSLOGFWD_ERR_CONTAINER_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such container");
		break;
	case SYSLOGFWD_ERR_CONTAINER_NOT_RUNNING:
		respond_error(fd, 404, "Not Found", "no such running container");
		break;
	case SYSLOGFWD_OK:
		break;
	}
}

/* POST/GET/DELETE /v1/syslog/targets (logging epic Part 2, ADR-0127):
 * registering a running container (typically syslog-1/syslog-2, a real
 * syslogd such as sysklogd.recipe) as an external forward target for
 * every container-sourced log line -- mirrors handle_ntp_server_
 * create/list/delete's own REST shape exactly. */
static void handle_syslog_target_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *container_name;
	enum syslogfwd_error serr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	container_name = json_as_string(json_object_get(root, "container"));
	if (container_name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "container missing");
		return;
	}

	serr = syslogfwd_target_register(container_name);
	if (serr != SYSLOGFWD_OK) {
		json_free(root);
		respond_syslogfwd_error(fd, serr);
		return;
	}

	/* container_name still points into root -- build the response
	 * before freeing it, matching handle_ntp_server_create()'s own
	 * use-after-free-avoidance ordering. */
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container_name);
	jw_obj_close(&w);
	json_free(root);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_syslog_target_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "targets");
	syslogfwd_target_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_syslog_target_delete(int fd, const char *name)
{
	enum syslogfwd_error serr = syslogfwd_target_unregister(name);

	if (serr != SYSLOGFWD_OK) {
		respond_syslogfwd_error(fd, serr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * GET/PUT /v1/system/time (task #752): the host's real current clock,
 * and a manual override -- an operator-facing escape hatch alongside
 * the automatic SNTP sync above, the same "live-apply, immediate
 * effect" relationship GET/PUT /v1/system/resolv already has to real
 * DNS resolution. PUT {"unixtime": N} calls clock_settime() directly.
 */
static void handle_time_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	ntp_write_json_time(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_time_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *junixtime;
	int64_t unixtime;
	enum ntp_error nerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	junixtime = json_object_get(root, "unixtime");
	if (junixtime == NULL || junixtime->type != JSON_NUMBER) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "unixtime (number) is required");
		return;
	}
	unixtime = (int64_t)junixtime->u.number;
	json_free(root);

	nerr = ntp_time_set(unixtime);
	if (nerr != NTP_OK) {
		respond_ntp_error(fd, nerr);
		return;
	}

	jw_init(&w);
	ntp_write_json_time(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * GET/PUT /v1/system/resolv (ADR-0076): the host's own outbound DNS
 * resolver config. PUT {"nameservers": [...]} replaces the full list
 * and takes effect immediately (resolv_set() rewrites the real,
 * bind-mounted file directly -- no reboot needed); an empty array
 * clears it.
 */
static void handle_resolv_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	resolv_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void respond_resolv_error(int fd, enum resolv_error err)
{
	switch (err) {
	case RESOLV_ERR_INVALID_IP:
		respond_error(fd, 400, "Bad Request", "nameservers must be valid IPv4 addresses");
		break;
	case RESOLV_ERR_TOO_MANY: {
		char msg[64];

		snprintf(msg, sizeof(msg), "too many nameservers (max %d)", RESOLV_MAX_NAMESERVERS);
		respond_error(fd, 400, "Bad Request", msg);
		break;
	}
	case RESOLV_ERR_PERSIST_FAILED:
		respond_error(fd, 500, "Internal Server Error", "could not persist resolv.conf");
		break;
	case RESOLV_OK:
		break;
	}
}

static void handle_resolv_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *arr;
	const char *nameservers[RESOLV_MAX_NAMESERVERS];
	int count;
	size_t i;
	enum resolv_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	arr = json_object_get(root, "nameservers");
	if (arr == NULL || arr->type != JSON_ARRAY) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "nameservers (array) is required");
		return;
	}
	if (arr->u.array.count > RESOLV_MAX_NAMESERVERS) {
		json_free(root);
		respond_resolv_error(fd, RESOLV_ERR_TOO_MANY);
		return;
	}
	count = (int)arr->u.array.count;
	for (i = 0; i < arr->u.array.count; i++) {
		nameservers[i] = json_as_string(arr->u.array.items[i]);
		if (nameservers[i] == NULL) {
			json_free(root);
			respond_resolv_error(fd, RESOLV_ERR_INVALID_IP);
			return;
		}
	}

	rerr = resolv_set(nameservers, count);
	json_free(root);
	if (rerr != RESOLV_OK) {
		respond_resolv_error(fd, rerr);
		return;
	}

	jw_init(&w);
	resolv_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * GET/PUT /v1/system/daemon-config (Part 0.5): kanxeod's own listen
 * port and which network is currently its management one -- a
 * dedicated resource, distinct from generic network CRUD, since
 * changing either has a real side effect (a live listen-socket
 * rebind) that plain PUT /v1/networks/... was never meant to trigger.
 * Which network is management is NOT this module's own state (see
 * daemon_config.h) -- reported here by querying network_find_
 * management() live, the one source of truth network.c already owns.
 * PUT accepts a partial body (only the fields being changed); "port"
 * and "management_network" may be given together, applied as a
 * single rebind rather than two.
 */
static void handle_daemon_config_get(int fd)
{
	struct json_writer w;
	struct network_def *mgmt;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "port");
	jw_int(&w, g_port);
	jw_key(&w, "bind");
	jw_str(&w, g_bind_addr);
	mgmt = network_find_management();
	jw_key(&w, "management_network");
	if (mgmt != NULL)
		jw_str(&w, mgmt->name);
	else
		jw_null(&w);
	jw_key(&w, "http_enabled");
	jw_bool(&w, g_listener_conn.fd >= 0);
	jw_key(&w, "https_enabled");
	jw_bool(&w, g_https_listener_conn.fd >= 0);
	jw_key(&w, "https_port");
	jw_int(&w, daemon_config_https_port());
	jw_key(&w, "bind_ip");
	if (daemon_config_bind_ip() != NULL)
		jw_str(&w, daemon_config_bind_ip());
	else
		jw_null(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_daemon_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jport, *jnetwork, *jhttp, *jhttps, *jhttps_port, *jbind_ip;
	int new_port = g_port;
	int new_https_port = daemon_config_https_port();
	char new_bind[INET_ADDRSTRLEN];
	char network_name[NETWORK_NAME_MAX];
	int have_network = 0;
	struct network_def *mgmt_target = NULL;
	int want_http = daemon_config_http_enabled();
	int want_https = daemon_config_https_enabled();
	int have_http_req = 0, have_https_req = 0;
	int have_bind_ip_key = 0, bind_ip_is_null = 0, applying_new_bind_ip = 0;
	char new_bind_ip_str[INET_ADDRSTRLEN];
	uint32_t new_bind_ip_be = 0;
	struct network_def *old_mgmt;
	char old_bind_ip[INET_ADDRSTRLEN];
	int had_old_bind_ip;
	enum daemon_config_error derr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	snprintf(new_bind, sizeof(new_bind), "%s", g_bind_addr);

	jport = json_object_get(root, "port");
	if (jport != NULL) {
		new_port = (int)json_as_number(jport);
		if (new_port < 1 || new_port > 65535) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "port must be 1-65535");
			return;
		}
	}

	jhttps_port = json_object_get(root, "https_port");
	if (jhttps_port != NULL) {
		new_https_port = (int)json_as_number(jhttps_port);
		if (new_https_port < 1 || new_https_port > 65535) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "https_port must be 1-65535");
			return;
		}
	}

	jhttp = json_object_get(root, "http_enabled");
	if (jhttp != NULL && jhttp->type == JSON_BOOL) {
		want_http = jhttp->u.boolean;
		have_http_req = 1;
	}
	jhttps = json_object_get(root, "https_enabled");
	if (jhttps != NULL && jhttps->type == JSON_BOOL) {
		want_https = jhttps->u.boolean;
		have_https_req = 1;
	}
	if (!want_http && !want_https) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "refusing to leave both http and https disabled");
		return;
	}

	jnetwork = json_object_get(root, "management_network");
	if (jnetwork != NULL) {
		const char *raw_name = json_as_string(jnetwork);

		if (raw_name == NULL) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "management_network must be a string");
			return;
		}
		mgmt_target = network_find(raw_name);
		if (mgmt_target == NULL) {
			json_free(root);
			respond_error(fd, 404, "Not Found", "no such network");
			return;
		}
		if (!mgmt_target->has_address) {
			json_free(root);
			respond_error(fd, 400, "Bad Request",
			              "network has no address for kanxeod to bind to");
			return;
		}
		{
			struct in_addr a;

			a.s_addr = mgmt_target->address_be;
			inet_ntop(AF_INET, &a, new_bind, sizeof(new_bind));
		}
		/* Copied out before json_free() below -- both raw_name and
		 * mgmt_target->name point into memory that call (or a future
		 * network_set_management()-driven mutation) could invalidate. */
		snprintf(network_name, sizeof(network_name), "%s", raw_name);
		have_network = 1;
	}

	/* bind_ip (ADR-0068): a dedicated second address on the management
	 * bridge, decoupled from that network's own address. A JSON `null`
	 * explicitly clears it; the key omitted entirely means "don't touch
	 * it". Syntax/subnet validation happens below, once the effective
	 * target network (mgmt_target if repointing this same call, else
	 * whichever network is already management) is known. */
	jbind_ip = json_object_get(root, "bind_ip");
	if (jbind_ip != NULL) {
		have_bind_ip_key = 1;
		if (jbind_ip->type == JSON_NULL) {
			bind_ip_is_null = 1;
		} else {
			const char *raw_ip = json_as_string(jbind_ip);

			if (raw_ip == NULL) {
				json_free(root);
				respond_error(fd, 400, "Bad Request", "bind_ip must be a string or null");
				return;
			}
			snprintf(new_bind_ip_str, sizeof(new_bind_ip_str), "%s", raw_ip);
		}
	}
	json_free(root);

	/* Captured before any mutation below -- old_mgmt is whichever
	 * network is management *right now* (before this request's own
	 * repoint, if any), which is exactly where any currently-persisted
	 * bind_ip actually lives on the real bridge. */
	old_mgmt = network_find_management();
	had_old_bind_ip = daemon_config_bind_ip() != NULL;
	if (had_old_bind_ip)
		snprintf(old_bind_ip, sizeof(old_bind_ip), "%s", daemon_config_bind_ip());

	if (have_bind_ip_key && !bind_ip_is_null) {
		struct network_def *effective_mgmt = have_network ? mgmt_target : old_mgmt;

		if (effective_mgmt == NULL) {
			respond_error(fd, 400, "Bad Request", "no management network to bind bind_ip within");
			return;
		}
		if (network_address_str_is_valid(effective_mgmt, new_bind_ip_str, &new_bind_ip_be) != 0) {
			respond_error(fd, 400, "Bad Request",
			              "bind_ip not a valid address within the management network's subnet");
			return;
		}
		/* Idempotent short-circuit: rtnl_addr_add_ipv4() uses NLM_F_EXCL,
		 * so re-submitting the address that's already assigned would
		 * otherwise fail as a spurious duplicate. */
		if (!had_old_bind_ip || strcmp(old_bind_ip, new_bind_ip_str) != 0) {
			int rtfd = rtnl_open();

			if (rtfd < 0) {
				respond_error(fd, 500, "Internal Server Error", "could not open rtnetlink socket");
				return;
			}
			if (rtnl_addr_add_ipv4(rtfd, effective_mgmt->name, new_bind_ip_be,
			                        effective_mgmt->prefix_len) != 0) {
				rtnl_close(rtfd);
				respond_error(fd, 500, "Internal Server Error",
				              "could not assign bind_ip to the management bridge");
				return;
			}
			rtnl_close(rtfd);
		}
		snprintf(new_bind, sizeof(new_bind), "%s", new_bind_ip_str);
		applying_new_bind_ip = 1;
	} else if (bind_ip_is_null && !have_network) {
		/* Explicit clear, no repoint in the same call -- fall back to
		 * the current management network's own address. */
		if (old_mgmt != NULL) {
			struct in_addr a;

			a.s_addr = old_mgmt->address_be;
			inet_ntop(AF_INET, &a, new_bind, sizeof(new_bind));
		}
	}
	/* Neither branch: bind_ip untouched and no repoint (new_bind already
	 * carries forward g_bind_addr unchanged), or a repoint with no fresh
	 * bind_ip (new_bind already set to mgmt_target's own address above)
	 * -- both leave new_bind exactly where it needs to be. */

	/* HTTP transition: start/stop/rebind depending on what's currently
	 * running versus what's being asked for. */
	if (want_http) {
		if (g_listener_conn.fd < 0) {
			if (start_http_listener(new_bind, new_port) != 0) {
				respond_error(fd, 500, "Internal Server Error", "could not start http listener");
				return;
			}
			/* rebind_listener() normally owns updating g_bind_addr/
			 * g_port; mirrored here for the freshly-started case,
			 * which never goes through rebind_listener() at all. */
			snprintf(g_bind_addr_buf, sizeof(g_bind_addr_buf), "%s", new_bind);
			g_bind_addr = g_bind_addr_buf;
			g_port = new_port;
		} else if (rebind_listener(new_bind, new_port) != 0) {
			respond_error(fd, 500, "Internal Server Error", "listener rebind failed");
			return;
		}
	} else if (g_listener_conn.fd >= 0) {
		stop_http_listener();
	}

	/* HTTPS transition, same shape. g_bind_addr is already authoritative
	 * (either unchanged, or just updated by the HTTP branch above --
	 * both listeners always share the same address, only the port
	 * differs) by the time this runs. */
	if (want_https) {
		if (g_https_listener_conn.fd < 0) {
			if (start_https_listener(g_bind_addr, new_https_port) != 0) {
				respond_error(fd, 500, "Internal Server Error",
				              "could not start https listener (no usable host cert yet?)");
				return;
			}
		} else if (rebind_https_listener(g_bind_addr, new_https_port) != 0) {
			respond_error(fd, 500, "Internal Server Error", "https listener rebind failed");
			return;
		}
	} else if (g_https_listener_conn.fd >= 0) {
		stop_https_listener();
	}

	if (have_network && network_set_management(network_name) != NETWORK_OK) {
		/* The socket(s) already moved -- this would be a genuinely
		 * inconsistent state (bound to target's address without
		 * target actually being the recorded management network).
		 * Not expected in practice (has_address was already confirmed
		 * above), but reported plainly rather than silently claiming
		 * success. */
		respond_error(fd, 500, "Internal Server Error",
		              "listener(s) rebound but could not persist management network");
		return;
	}

	/* https_enabled persisted before http_enabled deliberately -- see
	 * daemon_config_set_http_enabled()'s own guard: it refuses to
	 * persist http_enabled=0 unless https_enabled is *already*
	 * persisted true, so a single request disabling http while
	 * enabling https has to land https first. */
	if (have_https_req) {
		derr = daemon_config_set_https_enabled(want_https);
		if (derr != DAEMON_CONFIG_OK) {
			respond_error(fd, 500, "Internal Server Error",
			              "listener(s) changed but could not persist https_enabled");
			return;
		}
	}
	if (have_http_req) {
		derr = daemon_config_set_http_enabled(want_http);
		if (derr != DAEMON_CONFIG_OK) {
			respond_error(fd, 500, "Internal Server Error",
			              "listener(s) changed but could not persist http_enabled");
			return;
		}
	}
	if (jhttps_port != NULL) {
		derr = daemon_config_set_https_port(new_https_port);
		if (derr != DAEMON_CONFIG_OK) {
			respond_error(fd, 500, "Internal Server Error",
			              "listener(s) changed but could not persist https_port");
			return;
		}
	}
	derr = daemon_config_set_port(new_port);
	if (derr != DAEMON_CONFIG_OK) {
		respond_error(fd, 500, "Internal Server Error", "listener rebound but could not persist port");
		return;
	}

	/*
	 * bind_ip persistence + stale-address cleanup (ADR-0068), last of
	 * all -- the real bridge address was already added (if any) and
	 * the listener already rebound to it above, so nothing below this
	 * point can fail the request; only the old, now-unused address (if
	 * this call replaced or cleared one) gets torn down, mirroring the
	 * "create the new thing before removing the old one" ordering this
	 * whole handler already uses for listeners.
	 */
	if (applying_new_bind_ip) {
		derr = daemon_config_set_bind_ip(new_bind_ip_str);
		if (derr != DAEMON_CONFIG_OK) {
			respond_error(fd, 500, "Internal Server Error",
			              "bridge address changed but could not persist bind_ip");
			return;
		}
	} else if (bind_ip_is_null || (have_network && had_old_bind_ip)) {
		derr = daemon_config_set_bind_ip(NULL);
		if (derr != DAEMON_CONFIG_OK) {
			respond_error(fd, 500, "Internal Server Error", "could not clear persisted bind_ip");
			return;
		}
	}

	handle_daemon_config_get(fd);

	/*
	 * Stale bind_ip cleanup is deferred (arm_bind_ip_cleanup_timer(),
	 * ADR-0068), not done here synchronously -- confirmed the hard way,
	 * not assumed: this exact request's own accepted connection can
	 * have the about-to-be-removed address as its own *local* endpoint
	 * (e.g. clearing bind_ip via a client connected to that very
	 * bind_ip, the natural way an operator would do it), and deleting
	 * the address immediately after writing the response above still
	 * races the kernel's own async transmission of those bytes -- the
	 * client can hang forever waiting for a response that was
	 * "successfully" written but never actually delivered. See
	 * arm_bind_ip_cleanup_timer()'s own comment for the full story.
	 */
	if (had_old_bind_ip && old_mgmt != NULL &&
	    (!applying_new_bind_ip || strcmp(old_bind_ip, new_bind_ip_str) != 0)) {
		struct in_addr old_addr;

		if (inet_pton(AF_INET, old_bind_ip, &old_addr) == 1)
			arm_bind_ip_cleanup_timer(old_mgmt->name, old_addr.s_addr, old_mgmt->prefix_len);
	}
}

/* GET /v1/system/routes (ADR-0066): the box's own real kernel IPv4
 * routing table -- see network_write_routes_json()'s own comment for
 * why this exists (no SSH/general shell, the daemon is the only way
 * to ever see this). */
static void handle_route_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "routes");
	if (network_write_routes_json(&w) != 0) {
		jw_free(&w);
		respond_error(fd, 500, "Internal Server Error", "failed to read kernel routing table");
		return;
	}
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* POST /v1/system/routes (ADR-0067 Part 3): adds a real IPv4 route to
 * the host's own kernel routing table via rtnl_route_add_ipv4() --
 * the write-side counterpart to handle_route_list() above. dest/
 * prefix are optional together (omitted or prefix 0 means the
 * default route, matching rtnl_route_add_ipv4()'s own convention);
 * gateway is optional (omitted means a direct/on-link route). Scoped
 * to exactly what that primitive supports -- no RTA_OIF/interface
 * binding, no route replace semantics beyond what NLM_F_CREATE
 * already gives it. Not a persisted Kanxeo resource (see
 * network_write_routes_json()'s own comment) -- nothing here is
 * remembered across a reboot, same as this whole endpoint family. */
static void handle_route_add(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	const char *dest_str = NULL;
	const char *gateway_str = NULL;
	const struct json_value *jprefix;
	int prefix_len = 0;
	struct in_addr dest_addr;
	struct in_addr gateway_addr;
	uint32_t dest_be = 0;
	uint32_t gateway_be = 0;
	int rtfd;

	if (body != NULL && body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		dest_str = json_as_string(json_object_get(root, "dest"));
		gateway_str = json_as_string(json_object_get(root, "gateway"));
		jprefix = json_object_get(root, "prefix");
		if (jprefix != NULL)
			prefix_len = (int)json_as_number(jprefix);
	}

	if (prefix_len < 0 || prefix_len > 32) {
		if (root != NULL)
			json_free(root);
		respond_error(fd, 400, "Bad Request", "prefix must be in [0, 32]");
		return;
	}
	if (prefix_len > 0) {
		if (dest_str == NULL || inet_pton(AF_INET, dest_str, &dest_addr) != 1) {
			if (root != NULL)
				json_free(root);
			respond_error(fd, 400, "Bad Request", "dest missing or not a valid IPv4 address");
			return;
		}
		dest_be = dest_addr.s_addr;
	}
	if (gateway_str != NULL) {
		if (inet_pton(AF_INET, gateway_str, &gateway_addr) != 1) {
			if (root != NULL)
				json_free(root);
			respond_error(fd, 400, "Bad Request", "gateway not a valid IPv4 address");
			return;
		}
		gateway_be = gateway_addr.s_addr;
	}
	if (root != NULL)
		json_free(root);

	rtfd = rtnl_open();
	if (rtfd < 0) {
		respond_error(fd, 500, "Internal Server Error", "could not open rtnetlink socket");
		return;
	}
	if (rtnl_route_add_ipv4(rtfd, dest_be, prefix_len, gateway_be) != 0) {
		rtnl_close(rtfd);
		respond_error(fd, 400, "Bad Request",
		              "kernel rejected the route (already exists, unreachable gateway, or invalid)");
		return;
	}
	rtnl_close(rtfd);

	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/* DELETE /v1/system/routes: the mirror-image of handle_route_add()
 * above, via the new rtnl_route_del_ipv4(). Same body shape and same
 * default-route convention (prefix 0 or omitted means the default
 * route) identifies which route to remove. */
static void handle_route_del(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	const char *dest_str = NULL;
	const char *gateway_str = NULL;
	const struct json_value *jprefix;
	int prefix_len = 0;
	struct in_addr dest_addr;
	struct in_addr gateway_addr;
	uint32_t dest_be = 0;
	uint32_t gateway_be = 0;
	int rtfd;

	if (body != NULL && body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		dest_str = json_as_string(json_object_get(root, "dest"));
		gateway_str = json_as_string(json_object_get(root, "gateway"));
		jprefix = json_object_get(root, "prefix");
		if (jprefix != NULL)
			prefix_len = (int)json_as_number(jprefix);
	}

	if (prefix_len < 0 || prefix_len > 32) {
		if (root != NULL)
			json_free(root);
		respond_error(fd, 400, "Bad Request", "prefix must be in [0, 32]");
		return;
	}
	if (prefix_len > 0) {
		if (dest_str == NULL || inet_pton(AF_INET, dest_str, &dest_addr) != 1) {
			if (root != NULL)
				json_free(root);
			respond_error(fd, 400, "Bad Request", "dest missing or not a valid IPv4 address");
			return;
		}
		dest_be = dest_addr.s_addr;
	}
	if (gateway_str != NULL) {
		if (inet_pton(AF_INET, gateway_str, &gateway_addr) != 1) {
			if (root != NULL)
				json_free(root);
			respond_error(fd, 400, "Bad Request", "gateway not a valid IPv4 address");
			return;
		}
		gateway_be = gateway_addr.s_addr;
	}
	if (root != NULL)
		json_free(root);

	rtfd = rtnl_open();
	if (rtfd < 0) {
		respond_error(fd, 500, "Internal Server Error", "could not open rtnetlink socket");
		return;
	}
	if (rtnl_route_del_ipv4(rtfd, dest_be, prefix_len, gateway_be) != 0) {
		rtnl_close(rtfd);
		respond_error(fd, 404, "Not Found", "no matching route to delete");
		return;
	}
	rtnl_close(rtfd);

	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void respond_swap_error(int fd, enum swap_error serr)
{
	switch (serr) {
	case SWAP_ERR_ALREADY_ENABLED:
		respond_error(fd, 409, "Conflict", "swap is already enabled -- disable it first to resize");
		return;
	case SWAP_ERR_NOT_ENABLED:
		respond_error(fd, 409, "Conflict", "swap is not enabled");
		return;
	case SWAP_ERR_INVALID_SIZE:
		respond_error(fd, 400, "Bad Request", "size_mb out of range");
		return;
	case SWAP_ERR_IO:
		respond_error(fd, 500, "Internal Server Error", "swap file creation or activation failed");
		return;
	case SWAP_ERR_PERSIST_FAILED:
		respond_error(fd, 500, "Internal Server Error", "swap state could not be persisted");
		return;
	case SWAP_OK:
		return;
	}
}

static void handle_swap_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	swap_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_swap_enable(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jsize;
	int64_t size_mb;
	enum swap_error serr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jsize = json_object_get(root, "size_mb");
	if (jsize == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "size_mb missing");
		return;
	}
	size_mb = (int64_t)json_as_number(jsize);
	json_free(root);

	serr = swap_enable(size_mb);
	if (serr != SWAP_OK) {
		respond_swap_error(fd, serr);
		return;
	}

	jw_init(&w);
	swap_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_swap_disable(int fd)
{
	enum swap_error serr = swap_disable();

	if (serr != SWAP_OK) {
		respond_swap_error(fd, serr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * Reads /sys/class/net/<ifname>/statistics/<file> -- shared by
 * per-container stats (the host-side veth's own counters, the same
 * numbers a bridge/switch would see) and host-wide stats (real host
 * interfaces). A missing file (ENOENT -- e.g. a container's veth
 * already torn down) is not a request failure, just a 0 for that one
 * counter: GET .../stats stays a best-effort snapshot, not an
 * all-or-nothing report.
 */
static long long read_net_stat(const char *ifname, const char *file)
{
	char path[PATH_MAX];
	char buf[32];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", ifname, file);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	return strtoll(buf, NULL, 10);
}

/* Best-effort: a missing/malformed /proc/loadavg leaves all three at 0,
 * same "snapshot, not all-or-nothing" convention as read_net_stat(). */
static void read_loadavg(double *l1, double *l5, double *l15)
{
	FILE *f;

	*l1 = *l5 = *l15 = 0.0;
	f = fopen("/proc/loadavg", "r");
	if (f == NULL)
		return;
	fscanf(f, "%lf %lf %lf", l1, l5, l15);
	fclose(f);
}

/* /proc/stat's own first "cpu" line: user/nice/system/idle/iowait/irq/
 * softirq/steal jiffies, in that fixed kernel-documented order. Raw
 * cumulative counters since boot -- callers compute their own deltas,
 * same convention as every other stats endpoint in this daemon. */
static void read_cpu_jiffies(long long *user, long long *nice, long long *system_j,
                              long long *idle, long long *iowait, long long *irq,
                              long long *softirq, long long *steal)
{
	FILE *f;
	char label[16];

	*user = *nice = *system_j = *idle = *iowait = *irq = *softirq = *steal = 0;
	f = fopen("/proc/stat", "r");
	if (f == NULL)
		return;
	if (fscanf(f, "%15s %lld %lld %lld %lld %lld %lld %lld %lld",
	           label, user, nice, system_j, idle, iowait, irq, softirq, steal) < 9) {
		*user = *nice = *system_j = *idle = *iowait = *irq = *softirq = *steal = 0;
	}
	fclose(f);
}

/* /proc/meminfo: "Key:   value kB" lines, in no guaranteed order and
 * with keys this daemon doesn't care about interspersed -- parsed as a
 * single pass matching each line's key against the wanted set, rather
 * than assuming a fixed line count/order. All values kB -> bytes. */
static void read_meminfo(long long *total, long long *free_b, long long *avail,
                          long long *buffers, long long *cached,
                          long long *swap_total, long long *swap_free)
{
	FILE *f;
	char line[256];
	char key[64];
	long long val;

	*total = *free_b = *avail = *buffers = *cached = *swap_total = *swap_free = 0;
	f = fopen("/proc/meminfo", "r");
	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (sscanf(line, "%63s %lld", key, &val) != 2)
			continue;
		if (strcmp(key, "MemTotal:") == 0)
			*total = val * 1024;
		else if (strcmp(key, "MemFree:") == 0)
			*free_b = val * 1024;
		else if (strcmp(key, "MemAvailable:") == 0)
			*avail = val * 1024;
		else if (strcmp(key, "Buffers:") == 0)
			*buffers = val * 1024;
		else if (strcmp(key, "Cached:") == 0)
			*cached = val * 1024;
		else if (strcmp(key, "SwapTotal:") == 0)
			*swap_total = val * 1024;
		else if (strcmp(key, "SwapFree:") == 0)
			*swap_free = val * 1024;
	}
	fclose(f);
}

/*
 * GET /v1/system/stats: host-wide CPU/memory/disk/network load, mirroring
 * handle_container_stats()'s own conventions one level up -- nested
 * per-category objects, raw cumulative/monotonic counters only (never a
 * pre-computed rate; the client already does its own delta math for
 * container stats and does the same here). Real /proc and statvfs()
 * sources throughout, no shelling out, matching this project's own
 * syscall-first convention. Every field here is a best-effort read: a
 * missing source leaves that section zeroed rather than failing the
 * whole request, same as container stats' own network section.
 */
/*
 * Writes one cgroup_pressure as {"some":{...},"full":{...}} -- shared
 * by cpu.pressure/io.pressure/memory.pressure in both host and
 * per-container stats, so the shape only needs defining once.
 */
static void write_pressure_json(struct json_writer *w, const struct cgroup_pressure *p)
{
	jw_key(w, "some");
	jw_obj_open(w);
	jw_key(w, "avg10");
	jw_num(w, p->some_avg10);
	jw_key(w, "avg60");
	jw_num(w, p->some_avg60);
	jw_key(w, "avg300");
	jw_num(w, p->some_avg300);
	jw_key(w, "total_usec");
	jw_int(w, p->some_total);
	jw_obj_close(w);
	jw_key(w, "full");
	jw_obj_open(w);
	jw_key(w, "avg10");
	jw_num(w, p->full_avg10);
	jw_key(w, "avg60");
	jw_num(w, p->full_avg60);
	jw_key(w, "avg300");
	jw_num(w, p->full_avg300);
	jw_key(w, "total_usec");
	jw_int(w, p->full_total);
	jw_obj_close(w);
}

static void handle_system_stats(int fd)
{
	struct json_writer w;
	double load1, load5, load15;
	long long cpu_user, cpu_nice, cpu_system, cpu_idle, cpu_iowait, cpu_irq, cpu_softirq, cpu_steal;
	long long mem_total, mem_free, mem_avail, mem_buffers, mem_cached, swap_total, swap_free;
	struct statvfs vfs;
	long long disk_total = 0, disk_free = 0, disk_avail = 0;
	struct cgroup_pressure cpu_pressure, io_pressure, mem_pressure;
	DIR *d;
	struct dirent *ent;

	read_loadavg(&load1, &load5, &load15);
	read_cpu_jiffies(&cpu_user, &cpu_nice, &cpu_system, &cpu_idle, &cpu_iowait,
	                 &cpu_irq, &cpu_softirq, &cpu_steal);
	read_meminfo(&mem_total, &mem_free, &mem_avail, &mem_buffers, &mem_cached,
	             &swap_total, &swap_free);
	if (statvfs(g_base_dir, &vfs) == 0) {
		disk_total = (long long)vfs.f_blocks * vfs.f_frsize;
		disk_free = (long long)vfs.f_bfree * vfs.f_frsize;
		disk_avail = (long long)vfs.f_bavail * vfs.f_frsize;
	}

	memset(&cpu_pressure, 0, sizeof(cpu_pressure));
	memset(&io_pressure, 0, sizeof(io_pressure));
	memset(&mem_pressure, 0, sizeof(mem_pressure));
	{
		/*
		 * Host-wide PSI lives at the cgroup v2 root, not inside any
		 * single leaf -- opened here (O_PATH, no container involved)
		 * rather than threading a root fd through from daemon init,
		 * since this is the only call site that ever needs it.
		 */
		int root_fd = open("/sys/fs/cgroup", O_PATH | O_DIRECTORY);

		if (root_fd >= 0) {
			cgroup_read_pressure(root_fd, "cpu.pressure", &cpu_pressure);
			cgroup_read_pressure(root_fd, "io.pressure", &io_pressure);
			cgroup_read_pressure(root_fd, "memory.pressure", &mem_pressure);
			close(root_fd);
		}
	}

	jw_init(&w);
	jw_obj_open(&w);

	jw_key(&w, "load");
	jw_obj_open(&w);
	jw_key(&w, "load1");
	jw_num(&w, load1);
	jw_key(&w, "load5");
	jw_num(&w, load5);
	jw_key(&w, "load15");
	jw_num(&w, load15);
	jw_obj_close(&w);

	jw_key(&w, "cpu");
	jw_obj_open(&w);
	jw_key(&w, "user_jiffies");
	jw_int(&w, cpu_user);
	jw_key(&w, "nice_jiffies");
	jw_int(&w, cpu_nice);
	jw_key(&w, "system_jiffies");
	jw_int(&w, cpu_system);
	jw_key(&w, "idle_jiffies");
	jw_int(&w, cpu_idle);
	jw_key(&w, "iowait_jiffies");
	jw_int(&w, cpu_iowait);
	jw_key(&w, "irq_jiffies");
	jw_int(&w, cpu_irq);
	jw_key(&w, "softirq_jiffies");
	jw_int(&w, cpu_softirq);
	jw_key(&w, "steal_jiffies");
	jw_int(&w, cpu_steal);
	jw_key(&w, "pressure");
	jw_obj_open(&w);
	write_pressure_json(&w, &cpu_pressure);
	jw_obj_close(&w);
	jw_obj_close(&w);

	jw_key(&w, "memory");
	jw_obj_open(&w);
	jw_key(&w, "total_bytes");
	jw_int(&w, mem_total);
	jw_key(&w, "free_bytes");
	jw_int(&w, mem_free);
	jw_key(&w, "available_bytes");
	jw_int(&w, mem_avail);
	jw_key(&w, "buffers_bytes");
	jw_int(&w, mem_buffers);
	jw_key(&w, "cached_bytes");
	jw_int(&w, mem_cached);
	jw_key(&w, "swap_total_bytes");
	jw_int(&w, swap_total);
	jw_key(&w, "swap_free_bytes");
	jw_int(&w, swap_free);
	jw_key(&w, "pressure");
	jw_obj_open(&w);
	write_pressure_json(&w, &mem_pressure);
	jw_obj_close(&w);
	jw_obj_close(&w);

	jw_key(&w, "disk");
	jw_obj_open(&w);
	jw_key(&w, "total_bytes");
	jw_int(&w, disk_total);
	jw_key(&w, "free_bytes");
	jw_int(&w, disk_free);
	jw_key(&w, "avail_bytes");
	jw_int(&w, disk_avail);
	jw_key(&w, "pressure");
	jw_obj_open(&w);
	write_pressure_json(&w, &io_pressure);
	jw_obj_close(&w);
	jw_obj_close(&w);

	jw_key(&w, "networks");
	jw_arr_open(&w);
	d = opendir("/sys/class/net");
	if (d != NULL) {
		while ((ent = readdir(d)) != NULL) {
			if (ent->d_name[0] == '.')
				continue;
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, ent->d_name);
			jw_key(&w, "rx_bytes");
			jw_int(&w, read_net_stat(ent->d_name, "rx_bytes"));
			jw_key(&w, "tx_bytes");
			jw_int(&w, read_net_stat(ent->d_name, "tx_bytes"));
			jw_key(&w, "rx_packets");
			jw_int(&w, read_net_stat(ent->d_name, "rx_packets"));
			jw_key(&w, "tx_packets");
			jw_int(&w, read_net_stat(ent->d_name, "tx_packets"));
			jw_obj_close(&w);
		}
		closedir(d);
	}
	jw_arr_close(&w);

	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* GET/DELETE /v1/system/processes (logging/web-UI epic Part 6,
 * ADR-0131): a real, direct /proc scan of every process on the box,
 * correlated to a container by hostproc.c's own ppid-chain walk. */
static void handle_hostproc_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	hostproc_write_json_list(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_hostproc_kill(int fd, const char *pid_str)
{
	char *endptr;
	long pid;
	enum hostproc_error herr;

	pid = strtol(pid_str, &endptr, 10);
	if (*pid_str == '\0' || *endptr != '\0' || pid <= 0) {
		respond_error(fd, 400, "Bad Request", "pid must be a positive integer");
		return;
	}

	herr = hostproc_kill((pid_t)pid);
	switch (herr) {
	case HOSTPROC_OK:
		http_set_blocking(fd);
		http_write_response(fd, 204, "No Content", "application/json", "", 0);
		break;
	case HOSTPROC_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such pid currently running");
		break;
	case HOSTPROC_ERR_FORBIDDEN:
		respond_error(fd, 400, "Bad Request",
		              "refusing to kill pid 1 or this daemon's own pid");
		break;
	case HOSTPROC_ERR_KILL_FAILED:
		respond_error(fd, 500, "Internal Server Error", "kill(2) failed");
		break;
	}
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

/* Forward declaration -- defined later in this file (Part 5, ADR-0124);
 * called from try_start_queued_pkg_rebuild() below, which several
 * earlier call sites already depend on. */
static void apply_rolling_container_restarts(void);

/*
 * ADR-0107: called at every point a completed/failed pkg job might
 * have just freed pkg.c's own single-job-in-flight slot -- tries to
 * start whatever rolling-image rebuild is queued (pkg_recipe_add()'s
 * own trigger), registering its pidfd exactly like a fresh top-level
 * install already does if one actually started. A no-op (returns
 * immediately, no epoll registration) when the queue is empty or a
 * job is somehow already running -- safe to call unconditionally
 * after every job-completion path rather than threading a "did this
 * really free the slot" condition through each call site.
 *
 * Also runs apply_rolling_container_restarts() (Part 5, ADR-0124):
 * this same "a pkg job just finished" moment is exactly when an
 * image's current_version might have just moved (a rolling rebuild
 * completing is itself one more pkg job), so it's the natural single
 * hook for reconciling any follow_rolling container against it too --
 * one call site, not two independently-triggered mechanisms.
 */
static void try_start_queued_pkg_rebuild(void)
{
	pid_t pkg_pid;
	int pkg_pidfd;

	if (pkg_try_start_queued_rebuild(&pkg_pid, &pkg_pidfd))
		register_pkg_fetch_pidfd(pkg_pid, pkg_pidfd);
	apply_rolling_container_restarts();
}

/*
 * ADR-0087: registers pkg.c's own build-output capture pipe (see
 * pkg_build_output_fd()) directly with epoll, watching the pipe's fd
 * itself rather than a pidfd -- the CONN_KMSG pattern (start_kmsg_
 * watch()/handle_kmsg_event()), not the pidfd-then-single-read pattern
 * every other CONN_* kind above uses. This is deliberate: the build
 * container's own exit is already tracked separately via
 * register_container_pidfd()'s own CONN_CONTAINER registration for
 * PKG_BUILD_CONTAINER_NAME; this conn's only job is draining the
 * output pipe live, as data arrives, so it never fills its 64KB
 * kernel buffer and deadlocks the still-running build. No-op if
 * output_fd is -1 (pipe2() itself failed at build-spawn time, or no
 * build is in flight).
 */
static void register_pkg_build_output(int output_fd)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	if (output_fd < 0)
		return;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (pkg build output reactor conn)");
		abort();
	}
	cc->kind = CONN_PKG_BUILD_OUTPUT;
	cc->fd = output_fd;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD pkg build output fd");
		abort();
	}
}

/*
 * task #676: clients live-tailing the in-flight build's own output
 * over GET /v1/pkg/build/log (try_pkg_build_log_upgrade() below) --
 * a small fixed table, same "one build in flight at a time" invariant
 * pkg.c's own module-level build state already relies on, generous
 * enough for a few operators watching the same build at once.
 */
#define PKG_BUILD_LOG_WS_MAX 4
static struct conn *g_build_log_ws_conns[PKG_BUILD_LOG_WS_MAX];
static int g_build_log_ws_conn_count;

static void build_log_ws_detach(struct conn *cc)
{
	int i;

	for (i = 0; i < g_build_log_ws_conn_count; i++) {
		if (g_build_log_ws_conns[i] != cc)
			continue;
		g_build_log_ws_conns[i] = g_build_log_ws_conns[g_build_log_ws_conn_count - 1];
		g_build_log_ws_conn_count--;
		return;
	}
}

/* Sends a chunk of newly-drained build output to every attached
 * live-tail client. A write failure just detaches that one client
 * (its own next epoll event, or this immediate teardown, reflects a
 * gone-away peer) -- never lets one broken viewer affect the others
 * or the build itself, which never blocks on this at all. */
static void build_log_ws_broadcast(const void *data, size_t len)
{
	int i;

	for (i = 0; i < g_build_log_ws_conn_count;) {
		struct conn *cc = g_build_log_ws_conns[i];

		if (ws_write_frame(cc->fd, WS_OPCODE_TEXT, data, len) != 0) {
			kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
			ws_conn_free(&cc->ws);
			close(cc->fd);
			g_build_log_ws_conns[i] = g_build_log_ws_conns[g_build_log_ws_conn_count - 1];
			g_build_log_ws_conn_count--;
			free(cc);
			continue;
		}
		i++;
	}
}

/* Called once the build's own output pipe reaches EOF (the build
 * finished, one way or another) -- tells every still-attached
 * live-tail client the stream is over via a real WS close frame, then
 * tears each one down. Without this a client would just hang waiting
 * for more output that will never come. */
static void build_log_ws_teardown_all(void)
{
	int i;

	for (i = 0; i < g_build_log_ws_conn_count; i++) {
		struct conn *cc = g_build_log_ws_conns[i];

		ws_write_frame(cc->fd, WS_OPCODE_CLOSE, NULL, 0);
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		ws_conn_free(&cc->ws);
		close(cc->fd);
		free(cc);
	}
	g_build_log_ws_conn_count = 0;
}

/*
 * Fires whenever pkg_build_output_fd()'s fd becomes readable --
 * drains everything currently buffered into pkg.c's own capture
 * buffer, relaying whatever was newly read to any attached live-tail
 * clients (task #676) before checking for EOF. Once
 * pkg_build_output_readable() reports EOF/error (every write end
 * closed -- the build container and all its descendants have
 * exited), tears down this conn's own epoll registration and closes
 * the fd via pkg_build_output_close(), mirroring exactly how the
 * container's own CONN_CONTAINER exit path tears itself down, and
 * tells every live-tail client the stream has ended.
 */
static void handle_pkg_build_output_event(struct conn *cc)
{
	char new_data[4096];
	int new_len = 0;
	int eof;

	eof = pkg_build_output_readable(new_data, (int)sizeof(new_data), &new_len);
	if (new_len > 0 && g_build_log_ws_conn_count > 0)
		build_log_ws_broadcast(new_data, (size_t)new_len);
	if (eof) {
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		pkg_build_output_close();
		build_log_ws_teardown_all();
		free(cc);
	}
}

/*
 * The ordinary-container analog of register_pkg_build_output() above --
 * registers a container's own always-present capture pipe directly
 * with epoll, watching entry->output_fd itself rather than a pidfd,
 * for exactly the same reason: draining it live, incrementally, as
 * the container runs, so it never fills its 64KB kernel buffer and
 * blocks whatever the container itself is trying to write to stdout/
 * stderr (the same ADR-0087 discipline). No-op if output_fd is < 0
 * (pipe2() itself failed at container-create time -- capture
 * unavailable, the container still runs, exactly like pkg.c's own
 * build path degrades on the same failure).
 */
static void register_container_output(struct registry_entry *entry)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	if (entry->output_fd < 0)
		return;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (container output reactor conn)");
		abort();
	}
	memset(cc, 0, sizeof(*cc));
	cc->kind = CONN_CONTAINER_OUTPUT;
	cc->fd = entry->output_fd;
	cc->entry = entry;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD container output fd");
		abort();
	}
}

/*
 * Fires whenever any container's own output_fd becomes readable --
 * every container has one now (transparent log capture), not just
 * ones created with "capture_output":true. Two independent
 * destinations for the same bytes: (1) always, split into lines and
 * forwarded to logstore.c as source="container" entries (see
 * forward_container_output_to_logstore()) -- the "transparent... goes
 * to a common logging backend" mechanism; (2) only when
 * entry->capture_requested (this container's own client explicitly
 * opted in), the raw bytes are ALSO appended into
 * entry->captured_output (bounded at REGISTRY_CAPTURED_OUTPUT_MAX,
 * silently dropping anything past that -- the same "generous but
 * bounded" posture pkg.c's own build-output capture already
 * established, not a live-tail feature like CONN_PKG_BUILD_OUTPUT's WS
 * relay, just a diagnostic snapshot GET /v1/containers/{name} can
 * report). Guards entry->in_use: this registry slot could in principle
 * already have been reused by a different container by the time EOF
 * finally arrives (e.g. a very fast create/delete/create cycle racing
 * this conn's own teardown) -- skip both destinations rather than risk
 * attributing one container's output to another's slot. On EOF (every
 * copy of the pipe's write end closed
 * -- the container process and every descendant it forked have
 * exited), tears down this conn's own epoll registration and closes
 * the fd, mirroring handle_pkg_build_output_event() exactly.
 */
/*
 * Splits newly-read bytes on '\n', flushing each complete line into
 * logstore as its own "container" entry -- the transparent capture
 * this feature exists for, always running regardless of whether this
 * container's own client asked for capture_output. Partial lines
 * accumulate in cc->output_line_buf across calls (a line can span
 * multiple read()s); a line that fills the buffer with no '\n' yet is
 * flushed as-is rather than grown unboundedly, the same "generous but
 * bounded" posture entry->captured_output already has. Called with
 * flush_partial=1 at EOF so a container's final line (its very last
 * write before exit, which may have no trailing newline at all) isn't
 * silently dropped.
 *
 * Each flushed line is also fanned out to syslogfwd_send() (logging
 * epic Part 2, ADR-0127) -- a no-op unless at least one syslog forward
 * target is registered, so this costs nothing in the common case.
 */
static void forward_container_output_to_logstore(struct conn *cc, const char *data, size_t len,
                                                   int flush_partial)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (data[i] == '\n' ||
		    cc->output_line_len >= (int)sizeof(cc->output_line_buf) - 1) {
			cc->output_line_buf[cc->output_line_len] = '\0';
			if (cc->output_line_len > 0) {
				logstore_write_container(cc->entry->name, "info", "%s", cc->output_line_buf);
				syslogfwd_send(cc->entry->name, "info", cc->output_line_buf);
			}
			cc->output_line_len = 0;
			if (data[i] == '\n')
				continue;
		}
		cc->output_line_buf[cc->output_line_len++] = data[i];
	}

	if (flush_partial && cc->output_line_len > 0) {
		cc->output_line_buf[cc->output_line_len] = '\0';
		logstore_write_container(cc->entry->name, "info", "%s", cc->output_line_buf);
		syslogfwd_send(cc->entry->name, "info", cc->output_line_buf);
		cc->output_line_len = 0;
	}
}

static void handle_container_output_event(struct conn *cc)
{
	char new_data[4096];
	ssize_t n;
	int eof = 0;

	for (;;) {
		n = read(cc->fd, new_data, sizeof(new_data));
		if (n > 0) {
			if (cc->entry->in_use && cc->entry->output_fd == cc->fd) {
				if (cc->entry->capture_requested) {
					int room = (int)sizeof(cc->entry->captured_output) -
					           cc->entry->captured_output_len - 1;
					int take = (int)n < room ? (int)n : room;

					if (take > 0) {
						memcpy(cc->entry->captured_output + cc->entry->captured_output_len,
						       new_data, (size_t)take);
						cc->entry->captured_output_len += take;
						cc->entry->captured_output[cc->entry->captured_output_len] = '\0';
					}
				}
				forward_container_output_to_logstore(cc, new_data, (size_t)n, 0);
			}
			if (n < (ssize_t)sizeof(new_data))
				break; /* drained everything currently buffered */
			continue;
		}
		if (n == 0) {
			eof = 1;
			break;
		}
		/* n < 0 */
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break; /* nothing more right now, still open */
		eof = 1; /* a real read error -- treat like EOF, stop watching */
		break;
	}

	if (eof) {
		if (cc->entry->in_use && cc->entry->output_fd == cc->fd)
			forward_container_output_to_logstore(cc, "", 0, 1);
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		close(cc->fd);
		if (cc->entry->output_fd == cc->fd)
			cc->entry->output_fd = -1;
		free(cc);
	}
}

/*
 * ADR-0087: mkbootroot's own captured stdout/stderr, drained
 * incrementally via epoll exactly like pkg.c's own build-output pipe
 * above -- the identical class of bug (a single post-exit read of a
 * plain pipe deadlocks the moment cumulative output exceeds the 64KB
 * kernel buffer), just not yet observed live here since a
 * control-plane-only rebuild's own output has never been verbose
 * enough to fill it. Safe as a bare static: only one bootroot
 * assembly can ever be in flight at a time (triggered exclusively by
 * a "kanxeo" hostbuild completing, itself serialized by pkg.c's own
 * one-job-at-a-time invariant) -- the same reasoning pkg.c's own
 * module-level build state statics already document.
 */
static int g_bootroot_output_rd = -1;
#define BOOTROOT_OUTPUT_CAPTURE_MAX 2000
static char g_bootroot_output_captured[BOOTROOT_OUTPUT_CAPTURE_MAX + 1];
static int g_bootroot_output_captured_len;

static void bootroot_output_append(const char *data, int len)
{
	int take = (len > BOOTROOT_OUTPUT_CAPTURE_MAX) ? BOOTROOT_OUTPUT_CAPTURE_MAX : len;
	int new_total = g_bootroot_output_captured_len + take;

	if (new_total > BOOTROOT_OUTPUT_CAPTURE_MAX) {
		int overflow = new_total - BOOTROOT_OUTPUT_CAPTURE_MAX;

		memmove(g_bootroot_output_captured, g_bootroot_output_captured + overflow,
		        g_bootroot_output_captured_len - overflow);
		g_bootroot_output_captured_len -= overflow;
	}
	memcpy(g_bootroot_output_captured + g_bootroot_output_captured_len, data + (len - take), take);
	g_bootroot_output_captured_len += take;
}

/* Returns 1 once every write end has closed (EOF/real read error --
 * the caller should tear down its own epoll registration and call
 * bootroot_output_close()), 0 if there may still be more to come. */
static int bootroot_output_readable(void)
{
	char chunk[2048];
	ssize_t n;

	if (g_bootroot_output_rd < 0)
		return 1;

	for (;;) {
		n = read(g_bootroot_output_rd, chunk, sizeof(chunk));
		if (n > 0) {
			bootroot_output_append(chunk, (int)n);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		return 1;
	}
}

static void bootroot_output_close(void)
{
	if (g_bootroot_output_rd >= 0) {
		close(g_bootroot_output_rd);
		g_bootroot_output_rd = -1;
	}
}

/* Registers g_bootroot_output_rd with epoll -- same CONN_KMSG-style
 * direct-fd pattern as register_pkg_build_output() above, not the
 * pidfd-then-single-read shape the CONN_BOOTROOT_ASSEMBLE conn itself
 * still uses for tracking mkbootroot's own exit. No-op if the fd is
 * -1 (pipe2() failed at spawn time). */
static void register_bootroot_output(void)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	if (g_bootroot_output_rd < 0)
		return;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (bootroot output reactor conn)");
		abort();
	}
	cc->kind = CONN_BOOTROOT_OUTPUT;
	cc->fd = g_bootroot_output_rd;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD bootroot output fd");
		abort();
	}
}

static void handle_bootroot_output_event(struct conn *cc)
{
	if (bootroot_output_readable()) {
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		bootroot_output_close();
		free(cc);
	}
}

/* Same shape as register_pkg_fetch_pidfd(), for the mkbootroot child
 * spawn_kanxeo_bootroot_assembly() below just forked -- tracks only
 * its exit; its captured stdout/stderr is a separate, directly-
 * registered conn (register_bootroot_output()/g_bootroot_output_rd
 * above, ADR-0087), not this one. */
static void register_bootroot_assemble_pidfd(pid_t pid, int pidfd)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (bootroot assemble reactor conn)");
		abort();
	}
	cc->kind = CONN_BOOTROOT_ASSEMBLE;
	cc->fd = pidfd;
	cc->pkg_fetch_pid = pid;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD bootroot assemble pidfd");
		abort();
	}
}

/*
 * ADR-0078: the well-known name of the shared "host tools" image --
 * cp/rm/sha256sum/gzip (coreutils.recipe/gzip.recipe) plus openssl/
 * curl/tar/bzip2/xz/squashfs-tools/e2fsprogs, one real `pkg install
 * --image=kanxeo-hosttools` per recipe -- an operator builds this
 * exactly like "kanxeo-builder"/"dev" (docs/guides/building-kanxeo.md),
 * no special-cased creation path. spawn_kanxeo_bootroot_assembly()
 * below passes its rootfs to mkbootroot.c's own host_tools_dir
 * argument when present, purely additive: a box that never built this
 * image keeps today's dev-host-sourced behavior (mkbootroot.c's own
 * "" fallback), never a hard failure.
 */
#define HOST_TOOLS_IMAGE "kanxeo-hosttools"

/*
 * ADR-0057: when a hostbuild job named "kanxeo" completes, assembles a
 * fresh control-plane squashfs from its own just-harvested artifacts
 * by forking+exec'ing the real, unmodified build/mkbootroot binary --
 * server-side, entirely within the daemon, never CLI-invoked (the
 * API-First Mandate rules out the CLI shelling out to a build tool
 * directly). Mirrors start_fetch_for()'s own "daemon already forks
 * subprocesses for curl" precedent -- this is the same class of
 * capability, not a new one. Uses THIS SAME hostbuild round's own
 * freshly built mkbootroot (kanxeo.recipe now stages one alongside
 * kanxeod/kanxeoctl/web/) rather than assuming some earlier round's
 * copy is still present anywhere -- self-contained, no bootstrap-order
 * dependency. Failure here (missing mkbootroot, fork failure) is
 * logged, never fatal to the daemon -- the hostbuild itself already
 * succeeded and its own artifacts are still there for a later retry.
 */
static void spawn_kanxeo_bootroot_assembly(const char *artifact_dir)
{
	char mkbootroot_bin[PATH_MAX];
	char kanxeod_bin[PATH_MAX];
	char kanxeoctl_bin[PATH_MAX];
	char web_dir[PATH_MAX];
	char out_squashfs[PATH_MAX];
	char stage_dir[PATH_MAX];
	char host_tools_dir[PATH_MAX];
	struct stat host_tools_st;
	char *argv[11];
	pid_t pid;
	int pidfd;
	int output_pipe[2];

	snprintf(mkbootroot_bin, sizeof(mkbootroot_bin), "%s/mkbootroot", artifact_dir);
	snprintf(kanxeod_bin, sizeof(kanxeod_bin), "%s/kanxeod", artifact_dir);
	snprintf(kanxeoctl_bin, sizeof(kanxeoctl_bin), "%s/kanxeoctl", artifact_dir);
	snprintf(web_dir, sizeof(web_dir), "%s/web", artifact_dir);
	snprintf(out_squashfs, sizeof(out_squashfs), "%s/kanxeod-root.squashfs", artifact_dir);
	snprintf(stage_dir, sizeof(stage_dir), "%s/.bootroot-stage", artifact_dir);

	/*
	 * ADR-0107/0108: resolved via HOST_TOOLS_IMAGE's own current
	 * version, not a flat "<IMAGES_DIR>/<name>/rootfs" path -- images
	 * are pure filesystem state, no registry lookup needed (image.h's
	 * own doc comment), but they DO now need a manifest.json lookup.
	 * stat() instead of assuming presence: most installs will never
	 * have built this optional image, and mkbootroot.c's own ""
	 * fallback already exists precisely for that case (also covers
	 * "exists but has no current version yet," though that's
	 * unreachable via image_create()'s own always-produces-one
	 * guarantee).
	 */
	host_tools_dir[0] = '\0';
	{
		char host_tools_version[IMAGE_VERSION_MAX];

		if (image_current_version(HOST_TOOLS_IMAGE, host_tools_version,
		                           sizeof(host_tools_version)) == IMAGE_OK) {
			image_version_rootfs_path(HOST_TOOLS_IMAGE, host_tools_version, host_tools_dir,
			                           sizeof(host_tools_dir));
			if (stat(host_tools_dir, &host_tools_st) != 0 || !S_ISDIR(host_tools_st.st_mode))
				host_tools_dir[0] = '\0';
		}
	}

	argv[0] = mkbootroot_bin;
	argv[1] = stage_dir;
	argv[2] = kanxeod_bin;
	argv[3] = kanxeoctl_bin;
	argv[4] = web_dir;
	argv[5] = out_squashfs;
	argv[6] = ""; /* firmware dir -- a control-plane-only rebuild needs no GPU firmware re-staging */
	argv[7] = ""; /* modules dir -- a control-plane-only rebuild (kanxeod/kanxeoctl/web only,
	               * ADR-0057) touches no kernel module tree at all */
	argv[8] = ""; /* kmod bin dir -- same reasoning */
	argv[9] = host_tools_dir; /* "" if kanxeo-hosttools was never built on this box */
	argv[10] = NULL;

	/*
	 * O_CLOEXEC on both ends, same reasoning as container_create()'s own
	 * diag pipe (include/container.h): the write end vanishes on its own
	 * at a successful execve() below, and the read end (kept open here
	 * past this fork) shouldn't leak into any later child this daemon
	 * spawns either. Not fatal on failure -- assembly still proceeds
	 * exactly as before this capture existed, just back to only the
	 * bare exit-status logging. O_NONBLOCK on the read end only (added
	 * for ADR-0087) -- the write end stays blocking since it becomes
	 * mkbootroot's own stdout/stderr.
	 */
	if (pipe2(output_pipe, O_CLOEXEC) != 0) {
		perror("pipe2 (bootroot assembly output capture)");
		logstore_write("kanxeod", "error",
		                "kanxeo bootroot assembly: output capture pipe failed: %s",
		                strerror(errno));
		output_pipe[0] = output_pipe[1] = -1;
	} else if (fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) != 0) {
		perror("fcntl O_NONBLOCK (bootroot assembly output capture)");
		close(output_pipe[0]);
		close(output_pipe[1]);
		output_pipe[0] = output_pipe[1] = -1;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork (bootroot assembly)");
		logstore_write("kanxeod", "error", "kanxeo bootroot assembly: fork failed: %s",
		                strerror(errno));
		if (output_pipe[0] >= 0) {
			close(output_pipe[0]);
			close(output_pipe[1]);
		}
		return;
	}
	if (pid == 0) {
		if (output_pipe[1] >= 0) {
			dup2(output_pipe[1], STDOUT_FILENO);
			dup2(output_pipe[1], STDERR_FILENO);
		}
		execve(mkbootroot_bin, argv, environ);
		perror("child: execve mkbootroot");
		_exit(127);
	}

	/*
	 * A real child now exists and is genuinely attempting this
	 * assembly -- bump the generation counter here, not any earlier
	 * (a failed fork() above never got a real attempt at all) and not
	 * any later (pidfd_open() failing below still means a real mkbootroot
	 * process was launched; it just couldn't be reaped normally, so
	 * task #737's own freshness tracking still needs to count this as
	 * "attempted, then failed" rather than silently never happening).
	 */
	g_bootroot_assembly_started++;
	g_bootroot_assembly_running = 1;

	if (output_pipe[1] >= 0)
		close(output_pipe[1]);

	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		perror("pidfd_open (bootroot assembly)");
		logstore_write("kanxeod", "error", "kanxeo bootroot assembly: pidfd_open failed: %s",
		                strerror(errno));
		if (output_pipe[0] >= 0)
			close(output_pipe[0]);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		g_bootroot_assembly_running = 0;
		return;
	}
	g_bootroot_output_captured_len = 0;
	g_bootroot_output_rd = output_pipe[0];
	register_bootroot_output();
	register_bootroot_assemble_pidfd(pid, pidfd);
}

/*
 * POST/GET /v1/system/iso (ADR-0064): closes the API-First Mandate gap
 * ADR-0063 deliberately left open -- image/src/mkinstalleriso.c was a
 * dev-machine-only tool an operator had to run by hand; this makes ISO
 * assembly a real, REST/CLI-reachable daemon capability, the same
 * fork+exec+pidfd-tracked, non-blocking shape spawn_kanxeo_bootroot_
 * assembly() above already established for mkbootroot. Unlike that
 * mechanism (auto-triggered the moment a "kanxeo" hostbuild completes),
 * ISO assembly is explicitly, separately triggered -- it has real
 * inputs of its own (the target's disk/ip/prefix/gateway/interface)
 * a hostbuild completion has no way to supply, and reuses whatever the
 * most recent "kanxeo"/"kernel"/"isotools" hostbuild rounds already
 * harvested rather than forcing a fresh rebuild of any of them.
 *
 * Only one ISO build is ever in flight at a time (the same v1 single-
 * job constraint pkg.c's own install/hostbuild pipeline already has) --
 * g_iso_build_state is this mechanism's entire piece of state, never
 * persisted (an in-progress build that the daemon restarts through is
 * simply lost, exactly like an in-progress pkg install already is).
 */
enum iso_build_state { ISO_BUILD_NONE, ISO_BUILD_BUILDING, ISO_BUILD_READY, ISO_BUILD_FAILED };
static enum iso_build_state g_iso_build_state = ISO_BUILD_NONE;
static char g_iso_build_error[256];
static char ISO_OUTPUT_PATH[PATH_MAX];

static void register_iso_assemble_pidfd(pid_t pid, int pidfd)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (iso assemble reactor conn)");
		abort();
	}
	cc->kind = CONN_ISO_ASSEMBLE;
	cc->fd = pidfd;
	cc->pkg_fetch_pid = pid;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD iso assemble pidfd");
		abort();
	}
}

/*
 * Validates every real precondition (the "kanxeo"/"kernel" hostbuild
 * artifacts, the "isotools" hostbuild artifact, and the operator-
 * populated signing key pair at SIGNING_KEYS_DIR) up front, so a
 * missing one fails the POST itself with a specific, actionable
 * message -- never a background failure the caller has to poll for
 * to discover. Returns 0 and forks the real build (state -> BUILDING)
 * on success; -1 with err_msg filled otherwise. err_msg_size must be
 * at least 256. Caller (handle_system_iso_post()) is responsible for
 * the "already building" check -- kept there, not here, so it maps to
 * its own distinct 409 rather than this function's 400-shaped
 * "missing precondition" errors, mirroring respond_pkg_error()'s own
 * PKG_ERR_BUSY (409) vs. PKG_ERR_INVALID_TOOLCHAIN (400) split.
 */
static int iso_build_start(const char *disk, const char *ip, const char *prefix,
                            const char *gateway, const char *interface, char *err_msg,
                            size_t err_msg_size)
{
	char mkinstalleriso_bin[PATH_MAX];
	char kanxeo_install_bin[PATH_MAX];
	char kanxeo_recover_bin[PATH_MAX];
	char bzimage_path[PATH_MAX];
	char squashfs_path[PATH_MAX];
	char isotools_root[PATH_MAX];
	char signing_key[PATH_MAX];
	char signing_cert_pem[PATH_MAX];
	char signing_cert_der[PATH_MAX];
	char stage_dir[PATH_MAX];
	char kernel_args[512];
	char *argv[13];
	pid_t pid;
	int pidfd;
	struct {
		const char *path;
		const char *what;
	} required[] = {
	    {mkinstalleriso_bin, "mkinstalleriso (from a \"kanxeo\" hostbuild)"},
	    {kanxeo_install_bin, "kanxeo-install (from a \"kanxeo\" hostbuild)"},
	    {kanxeo_recover_bin, "kanxeo-recover (from a \"kanxeo\" hostbuild, ADR-0146)"},
	    {bzimage_path, "bzImage (from a \"kernel\" hostbuild)"},
	    {squashfs_path, "kanxeod-root.squashfs (from a \"kanxeo\" hostbuild)"},
	    {isotools_root, "isotools artifact directory (from an \"isotools\" hostbuild)"},
	    {signing_key, "signing key (operator-provided at SIGNING_KEYS_DIR)"},
	    {signing_cert_pem, "signing cert .crt (operator-provided at SIGNING_KEYS_DIR)"},
	    {signing_cert_der, "signing cert .cer (operator-provided at SIGNING_KEYS_DIR)"},
	};
	size_t i;

	snprintf(mkinstalleriso_bin, sizeof(mkinstalleriso_bin), "%s/kanxeo/mkinstalleriso",
	         ARTIFACTS_DIR);
	snprintf(kanxeo_install_bin, sizeof(kanxeo_install_bin), "%s/kanxeo/kanxeo-install",
	         ARTIFACTS_DIR);
	snprintf(kanxeo_recover_bin, sizeof(kanxeo_recover_bin), "%s/kanxeo/kanxeo-recover",
	         ARTIFACTS_DIR);
	snprintf(bzimage_path, sizeof(bzimage_path), "%s/kernel/bzImage", ARTIFACTS_DIR);
	snprintf(squashfs_path, sizeof(squashfs_path), "%s/kanxeo/kanxeod-root.squashfs",
	         ARTIFACTS_DIR);
	snprintf(isotools_root, sizeof(isotools_root), "%s/isotools", ARTIFACTS_DIR);
	snprintf(signing_key, sizeof(signing_key), "%s/kanxeo-signing.key", SIGNING_KEYS_DIR);
	snprintf(signing_cert_pem, sizeof(signing_cert_pem), "%s/kanxeo-signing.crt", SIGNING_KEYS_DIR);
	snprintf(signing_cert_der, sizeof(signing_cert_der), "%s/kanxeo-signing.cer", SIGNING_KEYS_DIR);

	for (i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
		if (access(required[i].path, R_OK) != 0) {
			snprintf(err_msg, err_msg_size, "missing %s: %s", required[i].what, required[i].path);
			return -1;
		}
	}

	snprintf(kernel_args, sizeof(kernel_args),
	         "--disk=%s --ip=%s --prefix=%s --gateway=%s --interface=%s",
	         (disk != NULL && disk[0] != '\0') ? disk : "CHANGEME",
	         (ip != NULL && ip[0] != '\0') ? ip : "CHANGEME",
	         (prefix != NULL && prefix[0] != '\0') ? prefix : "CHANGEME",
	         (gateway != NULL && gateway[0] != '\0') ? gateway : "CHANGEME",
	         (interface != NULL && interface[0] != '\0') ? interface : "CHANGEME");

	snprintf(stage_dir, sizeof(stage_dir), "%s/.stage", ISO_DIR);
	snprintf(ISO_OUTPUT_PATH, sizeof(ISO_OUTPUT_PATH), "%s/kanxeo-install.iso", ISO_DIR);

	argv[0] = mkinstalleriso_bin;
	argv[1] = stage_dir;
	argv[2] = kanxeo_install_bin;
	argv[3] = kanxeo_recover_bin;
	argv[4] = bzimage_path;
	argv[5] = squashfs_path;
	argv[6] = signing_key;
	argv[7] = signing_cert_pem;
	argv[8] = signing_cert_der;
	argv[9] = ISO_OUTPUT_PATH;
	argv[10] = kernel_args;
	argv[11] = isotools_root;
	argv[12] = NULL;

	pid = fork();
	if (pid < 0) {
		snprintf(err_msg, err_msg_size, "fork failed: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execve(mkinstalleriso_bin, argv, environ);
		perror("child: execve mkinstalleriso");
		_exit(127);
	}

	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		snprintf(err_msg, err_msg_size, "pidfd_open failed: %s", strerror(errno));
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return -1;
	}
	register_iso_assemble_pidfd(pid, pidfd);
	g_iso_build_state = ISO_BUILD_BUILDING;
	g_iso_build_error[0] = '\0';
	return 0;
}

/* Reaps iso_build_start()'s own mkinstalleriso child, same shape as
 * handle_bootroot_assemble_event() -- updates g_iso_build_state for
 * GET /v1/system/iso to report back, since no REST response is waiting
 * on this (the original POST already returned 202 long before this
 * fires). */
static void handle_iso_assemble_event(struct conn *cc)
{
	int status;

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	if (waitpid(cc->pkg_fetch_pid, &status, 0) == cc->pkg_fetch_pid && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 0) {
		g_iso_build_state = ISO_BUILD_READY;
		fprintf(stderr, "iso assembly: succeeded (%s)\n", ISO_OUTPUT_PATH);
	} else {
		g_iso_build_state = ISO_BUILD_FAILED;
		snprintf(g_iso_build_error, sizeof(g_iso_build_error), "mkinstalleriso failed (status 0x%x)",
		         (unsigned)status);
		fprintf(stderr, "iso assembly: failed\n");
	}
	close(cc->fd);
	free(cc);
}

/*
 * POST /v1/pkg/bootstrap's own toolchain_url mode (ADR-0065): closes a
 * real gap ADR-0031's own "the operator transfers it onto the box
 * out-of-band (scp)" assumption turned out to rest on -- a real,
 * freshly-installed Kanxeo box has no SSH server and no general shell
 * at all (deliberately, ADR-0034), so there was never actually a way
 * to get a multi-hundred-MB toolchain artifact onto one over the
 * network. Reuses the exact host-side curl-fetch primitive pkg.c's own
 * recipe pkg_source already relies on (a plain fork+execve of curl,
 * never the daemon's own hand-rolled HTTP server) rather than
 * inventing a second fetch mechanism -- the only new code is fetching
 * into a fixed scratch path and, once it lands, handing off to the
 * exact same pkg_bootstrap_from_toolchain() the toolchain_path mode
 * already uses for staging.
 */
enum bootstrap_fetch_state {
	BOOTSTRAP_FETCH_NONE,
	BOOTSTRAP_FETCH_FETCHING,
	BOOTSTRAP_FETCH_READY,
	BOOTSTRAP_FETCH_FAILED
};
static enum bootstrap_fetch_state g_bootstrap_fetch_state = BOOTSTRAP_FETCH_NONE;
static char g_bootstrap_fetch_error[256];
static char g_bootstrap_fetch_sha256_expected[PKG_SHA256_MAX];

static void register_bootstrap_fetch_pidfd(pid_t pid, int pidfd)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (bootstrap fetch reactor conn)");
		abort();
	}
	cc->kind = CONN_BOOTSTRAP_FETCH;
	cc->fd = pidfd;
	cc->pkg_fetch_pid = pid;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD bootstrap fetch pidfd");
		abort();
	}
}

/* ADR-0121: same shape as register_bootstrap_fetch_pidfd() immediately
 * above, one epoll-tracked conn per in-flight pkg_sync_start() curl
 * child. */
static void register_pkg_sync_fetch_pidfd(pid_t pid, int pidfd)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (pkg sync fetch reactor conn)");
		abort();
	}
	cc->kind = CONN_PKG_SYNC_FETCH;
	cc->fd = pidfd;
	cc->pkg_fetch_pid = pid;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD pkg sync fetch pidfd");
		abort();
	}
}

/* Reaps the curl child pkg_sync_start() spawned and hands its exit
 * status to pkg_sync_completed(), which does the real work (extract +
 * merge-add every recipe found). */
static void handle_pkg_sync_fetch_event(struct conn *cc)
{
	int status;
	int exit_status;

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	if (waitpid(cc->pkg_fetch_pid, &status, 0) == cc->pkg_fetch_pid && WIFEXITED(status))
		exit_status = WEXITSTATUS(status);
	else
		exit_status = -1;
	close(cc->fd);
	free(cc);

	pkg_sync_completed(exit_status);
}

/* ADR-0123: same shape as register_pkg_sync_fetch_pidfd() immediately
 * above, for an image-recipe-apply's own whole-rootfs artifact curl
 * fetch. */
static void register_image_recipe_fetch_pidfd(pid_t pid, int pidfd)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (image recipe fetch reactor conn)");
		abort();
	}
	cc->kind = CONN_IMAGE_RECIPE_FETCH;
	cc->fd = pidfd;
	cc->pkg_fetch_pid = pid;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD image recipe fetch pidfd");
		abort();
	}
}

/* Reaps the curl child pkg_image_recipe_apply_start() spawned and
 * hands its exit status to pkg_image_recipe_apply_completed(), which
 * does the real work (verify checksum, extract as the new version's
 * whole rootfs, record it, mirror g_packages[]). */
static void handle_image_recipe_fetch_event(struct conn *cc)
{
	int status;
	int exit_status;

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	if (waitpid(cc->pkg_fetch_pid, &status, 0) == cc->pkg_fetch_pid && WIFEXITED(status))
		exit_status = WEXITSTATUS(status);
	else
		exit_status = -1;
	close(cc->fd);
	free(cc);

	pkg_image_recipe_apply_completed(exit_status);
}

/* Permanent, re-arming itself every configured interval -- identical
 * shape to arm_ntp_periodic_timer()/handle_ntp_periodic_timer_event()
 * (task #751), except an interval of 0 means "disabled": the timer is
 * simply never (re)armed, and manual `pkg sync` remains the only
 * trigger until an operator sets a real interval via PUT /v1/pkg/
 * repo-config. */
static struct conn g_pkg_sync_periodic_conn;

static void arm_pkg_sync_periodic_timer(void)
{
	struct itimerspec its;
	int interval = pkg_repo_get_sync_interval_seconds();

	if (interval <= 0 || g_pkg_sync_periodic_conn.fd < 0)
		return;
	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = interval;
	if (timerfd_settime(g_pkg_sync_periodic_conn.fd, 0, &its, NULL) != 0)
		perror("timerfd_settime (pkg sync periodic re-arm)");
}

static void handle_pkg_sync_periodic_timer_event(struct conn *cc)
{
	uint64_t expirations;
	pid_t pid;
	int pidfd;

	if (read(cc->fd, &expirations, sizeof(expirations)) < 0)
		perror("read (pkg sync periodic timerfd)");
	if (!pkg_sync_in_progress() && pkg_sync_start(&pid, &pidfd) == PKG_OK)
		register_pkg_sync_fetch_pidfd(pid, pidfd);
	arm_pkg_sync_periodic_timer();
}

/* Best-effort, same "never block daemon startup" posture as start_ntp_
 * periodic_timer() -- a timerfd_create() failure just means no
 * automatic sync ever happens; every pkg sync/repo-config surface
 * remains fully usable via manual trigger regardless. */
static void start_pkg_sync_periodic_timer(void)
{
	struct kx_epoll_event ev;
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

	g_pkg_sync_periodic_conn.fd = -1;
	if (fd < 0) {
		perror("timerfd_create (pkg sync periodic)");
		return;
	}
	g_pkg_sync_periodic_conn.kind = CONN_PKG_SYNC_PERIODIC_TIMER;
	g_pkg_sync_periodic_conn.fd = fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_pkg_sync_periodic_conn;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		close(fd);
		g_pkg_sync_periodic_conn.fd = -1;
		return;
	}
	arm_pkg_sync_periodic_timer();
}

/*
 * ADR-0141 Phase 5: same shape as arm_pkg_sync_periodic_timer() --
 * interval_hours of 0 (the default, "no automatic schedule") or
 * `enabled` false means the timer is simply never (re)armed, and
 * manual POST /v1/system/backup-config/snapshot-now remains the only
 * trigger until an operator sets both a real interval and enabled:true.
 */
static struct conn g_backup_periodic_conn;

static void arm_backup_periodic_timer(void)
{
	struct itimerspec its;
	int interval_hours = backupconfig_interval_hours();

	if (!backupconfig_enabled() || interval_hours <= 0 || g_backup_periodic_conn.fd < 0)
		return;
	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = (time_t)interval_hours * 3600;
	if (timerfd_settime(g_backup_periodic_conn.fd, 0, &its, NULL) != 0)
		perror("timerfd_settime (backup periodic re-arm)");
}

static void handle_backup_periodic_timer_event(struct conn *cc)
{
	uint64_t expirations;

	if (read(cc->fd, &expirations, sizeof(expirations)) < 0)
		perror("read (backup periodic timerfd)");
	do_backup_snapshot_now();
	arm_backup_periodic_timer();
}

/* Best-effort, same "never block daemon startup" posture as start_ntp_
 * periodic_timer()/start_pkg_sync_periodic_timer() -- a timerfd_create()
 * failure just means no automatic snapshot ever happens; GET/PUT
 * /v1/system/backup-config and manual snapshot-now remain fully usable
 * regardless. */
static void start_backup_periodic_timer(void)
{
	struct kx_epoll_event ev;
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

	g_backup_periodic_conn.fd = -1;
	if (fd < 0) {
		perror("timerfd_create (backup periodic)");
		return;
	}
	g_backup_periodic_conn.kind = CONN_BACKUP_PERIODIC_TIMER;
	g_backup_periodic_conn.fd = fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_backup_periodic_conn;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		close(fd);
		g_backup_periodic_conn.fd = -1;
		return;
	}
	arm_backup_periodic_timer();
}

static void handle_backup_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	backupconfig_write_json(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * PUT /v1/system/backup-config -- mirrors PUT /v1/system/daemon-
 * config's own "only the fields given are changed" partial-update
 * convention (ADR-0141's own explicit design note): reads the three
 * current values back from backupconfig.c's own getters, overrides
 * whichever fields this request body actually supplied (disk's own
 * JSON_NULL is a real, meaningful "clear it" distinct from the field
 * being absent entirely, same distinction POST .../migrate's own body
 * already makes), then persists the merged triple as one unit --
 * backupconfig_set() itself has no partial-update concept of its own.
 * Does NOT itself arm/disarm the periodic timer as a side effect of
 * every unrelated field edit -- only re-arms when enabled/interval_
 * hours actually changed, so an operator flipping some future
 * unrelated field (none exist yet, but the check is here for when one
 * does) never silently resets an in-flight countdown.
 */
static void handle_backup_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jdisk, *jenabled, *jinterval;
	const char *disk_name;
	char disk_name_buf[64];
	int enabled;
	int interval_hours;
	int schedule_changed;
	enum backupconfig_error err;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	disk_name = backupconfig_disk();
	if (disk_name != NULL)
		snprintf(disk_name_buf, sizeof(disk_name_buf), "%s", disk_name);
	else
		disk_name_buf[0] = '\0';
	enabled = backupconfig_enabled();
	interval_hours = backupconfig_interval_hours();

	jdisk = json_object_get(root, "disk");
	if (jdisk != NULL) {
		if (jdisk->type == JSON_NULL) {
			disk_name_buf[0] = '\0';
		} else if (jdisk->type == JSON_STRING && json_as_string(jdisk) != NULL) {
			snprintf(disk_name_buf, sizeof(disk_name_buf), "%s", json_as_string(jdisk));
		} else {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "disk must be a string or null");
			return;
		}
	}
	jenabled = json_object_get(root, "enabled");
	if (jenabled != NULL) {
		if (jenabled->type != JSON_BOOL) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "enabled must be a boolean");
			return;
		}
		enabled = jenabled->u.boolean;
	}
	jinterval = json_object_get(root, "interval_hours");
	if (jinterval != NULL) {
		if (jinterval->type != JSON_NUMBER) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "interval_hours must be a number");
			return;
		}
		interval_hours = (int)json_as_number(jinterval);
	}
	json_free(root);

	schedule_changed = (enabled != backupconfig_enabled()) || (interval_hours != backupconfig_interval_hours());

	err = backupconfig_set(disk_name_buf[0] != '\0' ? disk_name_buf : NULL, enabled, interval_hours);
	if (err != BACKUPCONFIG_OK) {
		if (err == BACKUPCONFIG_ERR_INVALID_INTERVAL)
			respond_error(fd, 400, "Bad Request", "interval_hours must be 0 or a positive number");
		else
			respond_error(fd, 500, "Internal Server Error", "could not persist backup config");
		return;
	}
	if (schedule_changed)
		arm_backup_periodic_timer();

	{
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		backupconfig_write_json(&w);
		jw_obj_close(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

static void handle_backup_config_status_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	backupconfig_write_status_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_backup_config_snapshot_now_post(int fd)
{
	do_backup_snapshot_now();

	{
		struct json_writer w;

		jw_init(&w);
		backupconfig_write_status_json(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

/*
 * ADR-0144: POST /v1/login -- the one endpoint that always works
 * regardless of write-gating (dispatch() exempts this exact path, see
 * its own comment). Real bcrypt verification via hostauth_login()
 * (which itself calls ldap_user_check_password()) -- no LDAP bind in
 * this part of the ADR yet, that's a later part's own addition to
 * this same function's internals, not a new endpoint.
 */
static void handle_login(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *username, *password;
	char token[HOSTAUTH_TOKEN_LEN + 1];
	int expires_in_seconds;
	enum hostauth_login_result lerr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	username = json_as_string(json_object_get(root, "username"));
	password = json_as_string(json_object_get(root, "password"));
	if (username == NULL || password == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "username and password are both required");
		return;
	}

	lerr = hostauth_login(username, password, token, &expires_in_seconds);
	json_free(root);
	if (lerr == HOSTAUTH_LOGIN_INVALID_CREDENTIALS) {
		respond_error(fd, 401, "Unauthorized", "invalid username or password");
		return;
	}
	if (lerr == HOSTAUTH_LOGIN_TABLE_FULL) {
		respond_error(fd, 500, "Internal Server Error",
		              "too many active sessions -- try again shortly");
		return;
	}

	{
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "token");
		jw_str(&w, token);
		jw_key(&w, "expires_in_seconds");
		if (expires_in_seconds > 0)
			jw_int(&w, expires_in_seconds);
		else
			jw_null(&w);
		jw_obj_close(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

/*
 * POST /v1/logout -- always 204, even for an unknown/already-expired
 * token (hostauth_logout()'s own idempotent contract) -- a client
 * logging out never needs to know or care whether its session had
 * already lapsed server-side.
 */
static void handle_logout(int fd, const char *req_headers, size_t req_headers_len)
{
	/* Must fit "Bearer " (7) + the real token (HOSTAUTH_TOKEN_LEN) + NUL
	 * -- a too-small buffer here previously made http_find_header()
	 * silently report "doesn't fit" (a real, live bug: logout always
	 * responded 204 per its own idempotent contract, but never actually
	 * called hostauth_logout() at all, leaving the session valid). */
	char token[HOSTAUTH_TOKEN_LEN + 16];

	if (http_find_header(req_headers, req_headers_len, "Authorization", token, sizeof(token)) >= 0) {
		const char *bearer = strncmp(token, "Bearer ", 7) == 0 ? token + 7 : token;

		hostauth_logout(bearer);
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_hostauth_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	hostauth_write_config_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * PUT /v1/system/hostauth-config -- full replacement (admin_groups is
 * a list, not a single field with an obvious "partial update" meaning
 * the way backup-config's own disk/enabled/interval_hours are each
 * independent) -- the request always supplies both fields, mirroring
 * daemon-config's own full-object PUT shape for a config resource
 * whose fields are this tightly coupled (a lone idle_timeout_seconds
 * change makes little sense to send without knowing what admin_groups
 * currently is, unlike backup-config's own genuinely-independent
 * fields).
 */
static void handle_hostauth_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jgroups, *jidle, *jldapen, *jldapservers, *jldapport, *jldapbasedn;
	const char *admin_groups[HOSTAUTH_ADMIN_GROUPS_MAX];
	const char *ldap_servers[HOSTAUTH_LDAP_MAX_SERVERS];
	int admin_group_count = 0;
	int idle_timeout_seconds;
	int ldap_enabled = 0;
	int ldap_server_count = 0;
	int ldap_port = HOSTAUTH_LDAP_DEFAULT_PORT;
	const char *ldap_base_dn = "";
	enum hostauth_config_error err;
	size_t i;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jgroups = json_object_get(root, "admin_groups");
	jidle = json_object_get(root, "idle_timeout_seconds");
	if (jgroups == NULL || jgroups->type != JSON_ARRAY || jidle == NULL ||
	    jidle->type != JSON_NUMBER) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "admin_groups (array) and idle_timeout_seconds "
		                                       "(number) are both required");
		return;
	}
	if (jgroups->u.array.count > HOSTAUTH_ADMIN_GROUPS_MAX) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "admin_groups must have at most 8 entries");
		return;
	}
	for (i = 0; i < jgroups->u.array.count; i++) {
		admin_groups[i] = json_as_string(jgroups->u.array.items[i]);
		if (admin_groups[i] == NULL) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "admin_groups entries must be strings");
			return;
		}
	}
	admin_group_count = (int)jgroups->u.array.count;
	idle_timeout_seconds = (int)json_as_number(jidle);

	/*
	 * ADR-0144's own live-LDAP backend fields: all optional, defaulting
	 * to disabled/empty -- an older-shaped PUT body (just admin_groups/
	 * idle_timeout_seconds, this endpoint's original contract) still
	 * works exactly as before rather than being rejected outright.
	 */
	jldapen = json_object_get(root, "ldap_enabled");
	if (jldapen != NULL && jldapen->type == JSON_BOOL)
		ldap_enabled = jldapen->u.boolean ? 1 : 0;
	jldapservers = json_object_get(root, "ldap_servers");
	if (jldapservers != NULL) {
		if (jldapservers->type != JSON_ARRAY || jldapservers->u.array.count > HOSTAUTH_LDAP_MAX_SERVERS) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "ldap_servers must be an array of at most 3 entries");
			return;
		}
		for (i = 0; i < jldapservers->u.array.count; i++) {
			ldap_servers[i] = json_as_string(jldapservers->u.array.items[i]);
			if (ldap_servers[i] == NULL) {
				json_free(root);
				respond_error(fd, 400, "Bad Request", "ldap_servers entries must be strings");
				return;
			}
		}
		ldap_server_count = (int)jldapservers->u.array.count;
	}
	jldapport = json_object_get(root, "ldap_port");
	if (jldapport != NULL) {
		if (jldapport->type != JSON_NUMBER) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "ldap_port must be a number");
			return;
		}
		ldap_port = (int)json_as_number(jldapport);
	}
	jldapbasedn = json_object_get(root, "ldap_base_dn");
	if (jldapbasedn != NULL) {
		ldap_base_dn = json_as_string(jldapbasedn);
		if (ldap_base_dn == NULL) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "ldap_base_dn must be a string");
			return;
		}
	}

	err = hostauth_set_config(admin_groups, admin_group_count, idle_timeout_seconds, ldap_enabled,
	                           ldap_servers, ldap_server_count, ldap_port, ldap_base_dn);
	json_free(root);
	if (err != HOSTAUTH_CONFIG_OK) {
		if (err == HOSTAUTH_CONFIG_ERR_INVALID_FIELD)
			respond_error(fd, 400, "Bad Request",
			              "idle_timeout_seconds must be >= 0, admin_groups at most 8 entries, "
			              "ldap_servers at most 3 entries, ldap_port in 1..65535, and "
			              "ldap_enabled requires at least one ldap_servers entry plus a "
			              "non-empty ldap_base_dn");
		else
			respond_error(fd, 500, "Internal Server Error", "could not persist host-auth config");
		return;
	}

	{
		struct json_writer w;

		jw_init(&w);
		hostauth_write_config_json(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

static void handle_hostauth_sessions_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "sessions");
	hostauth_write_sessions_json(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* DELETE /v1/system/hostauth/sessions/{username} -- revokes every
 * active session for that user ("log out everywhere"). Always 204,
 * even if the user had no active session (the same idempotent-logout
 * posture handle_logout() already has), since the end state (no
 * active session for this user) is identical either way. */
static void handle_hostauth_sessions_revoke(int fd, const char *username)
{
	hostauth_revoke_sessions_for_user(username);
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * Returns 0 and forks the fetch (state -> FETCHING) on success; -1
 * with err_msg filled otherwise (both url and sha256 are required --
 * unlike a recipe's own pkg_source, there is no "trust whatever
 * shows up" mode for something that gets unsquashfs'd wholesale into
 * the build sandbox). err_msg_size must be at least 256. Caller is
 * responsible for the "already fetching" check, same split as
 * iso_build_start()'s own doc comment explains.
 */
static int bootstrap_fetch_start(const char *url, const char *sha256, char *err_msg,
                                  size_t err_msg_size)
{
	static char out_path[PATH_MAX];
	static char url_buf[PKG_URL_MAX];
	char *argv[8];
	pid_t pid;
	int pidfd;

	if (url == NULL || url[0] == '\0') {
		snprintf(err_msg, err_msg_size, "toolchain_url is required");
		return -1;
	}
	if (sha256 == NULL || strlen(sha256) != 64) {
		snprintf(err_msg, err_msg_size, "toolchain_sha256 is required and must be a 64-char hex sha256");
		return -1;
	}

	snprintf(out_path, sizeof(out_path), "%s", PKGBUILD_TOOLCHAIN_FETCH_PATH);
	snprintf(url_buf, sizeof(url_buf), "%s", url);

	argv[0] = (char *)PKG_CURL_BIN;
	argv[1] = "-fsSL";
	argv[2] = "--retry";
	argv[3] = "8";
	argv[4] = "-o";
	argv[5] = out_path;
	argv[6] = url_buf;
	argv[7] = NULL;

	pid = fork();
	if (pid < 0) {
		snprintf(err_msg, err_msg_size, "fork failed: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execve(PKG_CURL_BIN, argv, environ);
		perror("child: execve curl (bootstrap fetch)");
		_exit(127);
	}

	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		snprintf(err_msg, err_msg_size, "pidfd_open failed: %s", strerror(errno));
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return -1;
	}
	register_bootstrap_fetch_pidfd(pid, pidfd);
	snprintf(g_bootstrap_fetch_sha256_expected, sizeof(g_bootstrap_fetch_sha256_expected), "%s", sha256);
	g_bootstrap_fetch_state = BOOTSTRAP_FETCH_FETCHING;
	g_bootstrap_fetch_error[0] = '\0';
	return 0;
}

/*
 * Reaps bootstrap_fetch_start()'s own curl child. On a real, clean
 * exit, verifies the fetched artifact's checksum (pkg_run_capture_
 * sha256(), the exact same real check every recipe source already
 * gets, ADR-0036) before ever handing it to pkg_bootstrap_from_
 * toolchain() -- a corrupt or wrong-URL fetch must never get
 * unsquashfs'd into the build sandbox silently.
 */
static void handle_bootstrap_fetch_event(struct conn *cc)
{
	int status;
	char sha_out[PKG_SHA256_MAX];

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	if (waitpid(cc->pkg_fetch_pid, &status, 0) != cc->pkg_fetch_pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		g_bootstrap_fetch_state = BOOTSTRAP_FETCH_FAILED;
		snprintf(g_bootstrap_fetch_error, sizeof(g_bootstrap_fetch_error), "curl fetch failed (status 0x%x)",
		         (unsigned)status);
		fprintf(stderr, "bootstrap fetch: curl failed\n");
		close(cc->fd);
		free(cc);
		return;
	}
	close(cc->fd);
	free(cc);

	if (pkg_run_capture_sha256(PKGBUILD_TOOLCHAIN_FETCH_PATH, sha_out, sizeof(sha_out)) != 0 ||
	    strcasecmp(sha_out, g_bootstrap_fetch_sha256_expected) != 0) {
		g_bootstrap_fetch_state = BOOTSTRAP_FETCH_FAILED;
		snprintf(g_bootstrap_fetch_error, sizeof(g_bootstrap_fetch_error), "checksum mismatch");
		fprintf(stderr, "bootstrap fetch: checksum mismatch\n");
		return;
	}

	{
		enum pkg_error perr = pkg_bootstrap_from_toolchain(PKGBUILD_TOOLCHAIN_FETCH_PATH);

		if (perr != PKG_OK) {
			g_bootstrap_fetch_state = BOOTSTRAP_FETCH_FAILED;
			snprintf(g_bootstrap_fetch_error, sizeof(g_bootstrap_fetch_error),
			         "fetched toolchain failed to stage (invalid squashfs?)");
			fprintf(stderr, "bootstrap fetch: staging failed\n");
			return;
		}
	}

	g_bootstrap_fetch_state = BOOTSTRAP_FETCH_READY;
	fprintf(stderr, "bootstrap fetch: succeeded, toolchain staged\n");
}

/*
 * Multi-disk management Phase C: registers the async format+mount job
 * diskformat_start() (daemon/src/diskformat.c) already forked -- that
 * module has no epoll/conn knowledge of its own (mirrors pkg.c's own
 * start_fetch_for() convention), so main.c does the actual reactor
 * registration, the same split every other async host job here uses.
 */
static void register_disk_format_pidfd(pid_t pid, int pidfd)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (disk format reactor conn)");
		abort();
	}
	cc->kind = CONN_DISK_FORMAT;
	cc->fd = pidfd;
	cc->pkg_fetch_pid = pid;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD disk format pidfd");
		abort();
	}
}

/* Reaps diskformat_start()'s own child, hands the exit status straight
 * to diskformat_completed() -- no REST response is waiting on this
 * (the original POST already returned 202 long before this fires),
 * same shape as handle_iso_assemble_event()/handle_bootstrap_fetch_
 * event() above. */
static void handle_disk_format_event(struct conn *cc)
{
	int status;
	int exit_status;

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	if (waitpid(cc->pkg_fetch_pid, &status, 0) == cc->pkg_fetch_pid && WIFEXITED(status))
		exit_status = WEXITSTATUS(status);
	else
		exit_status = -1;
	close(cc->fd);
	free(cc);

	diskformat_completed(exit_status);
	fprintf(stderr, "disk format: job finished (exit_status=%d)\n", exit_status);
}

/*
 * ADR-0141 Phase 2: registers the async bulk-copy job storagemigrate_
 * start() already forked, the same split every other async host job
 * uses (storagemigrate.c has no epoll/conn knowledge of its own).
 */
static void register_storage_migrate_pidfd(enum storage_kind kind, pid_t pid, int pidfd)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (storage migrate reactor conn)");
		abort();
	}
	cc->kind = CONN_STORAGE_MIGRATE;
	cc->fd = pidfd;
	cc->pkg_fetch_pid = pid;
	cc->storage_migrate_kind = kind;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD storage migrate pidfd");
		abort();
	}
}

/* ADR-0142 Section 4: same shape as register_storage_migrate_pidfd()
 * above, keyed by container name instead of storage kind. */
static void register_container_storage_migrate_pidfd(const char *name, pid_t pid, int pidfd)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (container storage migrate reactor conn)");
		abort();
	}
	cc->kind = CONN_CONTAINER_STORAGE_MIGRATE;
	cc->fd = pidfd;
	cc->pkg_fetch_pid = pid;
	snprintf(cc->container_storage_migrate_name, sizeof(cc->container_storage_migrate_name), "%s", name);

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD container storage migrate pidfd");
		abort();
	}
}

/*
 * ADR-0141 Phase 2: the real repoint step, run synchronously once
 * storagemigrate_finalize(STORAGE_KIND_STATE)'s own second copy pass
 * has already succeeded -- repoints every STATE_DIR-backed module's
 * own private path (never a bare STATE_DIR string mutation alone;
 * each module caches its own copy at _init() time, see every *_repoint()
 * function's own doc comment), re-establishes the /etc/resolv.conf
 * bind mount against the new location (the exact Part 103 lesson:
 * a bind mount is tied to the inode it captured, not the path -- a
 * plain STATE_DIR change alone would leave it silently stale), persists
 * the new active placement, and removes the old location's data. Only
 * ever called with the daemon's single-threaded reactor as the sole
 * caller (ADR-0009) -- inherently atomic from every other request's
 * point of view, no locking needed.
 */
static void finalize_state_storage_migration(void)
{
	const char *target_dir = storagemigrate_job_target_dir(STORAGE_KIND_STATE);
	const char *target_disk = storagemigrate_job_target_disk(STORAGE_KIND_STATE);
	char old_source_dir[PATH_MAX];
	char new_resolv_path[PATH_MAX];

	snprintf(old_source_dir, sizeof(old_source_dir), "%s", storagemigrate_job_source_dir(STORAGE_KIND_STATE));

	if (storagemigrate_finalize(STORAGE_KIND_STATE) != 0) {
		fprintf(stderr, "state-storage migration: final copy pass failed, migration aborted\n");
		return;
	}

	snprintf(STATE_DIR, sizeof(STATE_DIR), "%s", target_dir);
	compute_state_dir_relative_paths();

	network_repoint(NETWORKS_STATE_PATH);
	dns_repoint(DNS_RECORDS_STATE_PATH, DNS_SERVERS_STATE_PATH);
	ldap_repoint(LDAP_SERVERS_STATE_PATH);
	ldap_record_repoint(LDAP_USERS_STATE_PATH, LDAP_GROUPS_STATE_PATH);
	ldap_config_repoint(LDAP_CONFIG_STATE_PATH);
	pki_repoint(PKI_DIR, PKI_CERTS_STATE_PATH);
	containerdef_repoint(CONTAINER_DEFS_STATE_PATH);
	containerdef_rolling_config_repoint(ROLLING_CONFIG_PATH);
	siteconfig_repoint(SITE_CONFIG_PATH);
	devicemap_repoint(DEVICEMAP_STATE_PATH);
	daemon_config_repoint(DAEMON_CONFIG_PATH);
	quotamap_repoint(QUOTAMAP_STATE_PATH);
	ntp_repoint(NTP_STATE_PATH, NTP_SERVERS_STATE_PATH);
	syslogfwd_repoint(SYSLOGFWD_STATE_PATH);
	connthrottle_config_repoint(CONNTHROTTLE_CONFIG_PATH);
	backupconfig_repoint(BACKUP_CONFIG_PATH);
	hostauth_repoint(HOSTAUTH_CONFIG_PATH);
	resolv_repoint(RESOLV_CONF_PATH);

	/* Re-establish the real bind mount -- see this function's own top
	 * comment. Best-effort: a failure here leaves /etc/resolv.conf
	 * stale until a real reboot re-does boot_init()'s own bind-mount
	 * setup against the now-correct RESOLV_CONF_PATH, not fatal to the
	 * migration itself (matches boot_init()'s own non-fatal posture for
	 * this exact bind mount). */
	snprintf(new_resolv_path, sizeof(new_resolv_path), "%s", RESOLV_CONF_PATH);
	if (umount2("/etc/resolv.conf", MNT_DETACH) != 0)
		perror("state-storage migration: /etc/resolv.conf umount");
	if (mount(new_resolv_path, "/etc/resolv.conf", NULL, MS_BIND, NULL) != 0)
		perror("state-storage migration: /etc/resolv.conf re-bind-mount");

	storageplacement_set(STORAGE_KIND_STATE, target_disk[0] != '\0' ? target_disk : NULL);

	if (persist_remove_tree(old_source_dir) != 0)
		fprintf(stderr, "state-storage migration: could not remove old location %s: %s\n",
		        old_source_dir, strerror(errno));

	fprintf(stderr, "state-storage migration: complete, now active on %s\n",
	        target_disk[0] != '\0' ? target_disk : "(default OS-disk placement)");
}

/*
 * ADR-0141 Phase 3: log-storage's own finalize step -- simpler than
 * state-storage's (single consumer, no /etc/resolv.conf-style bind
 * mount to redo), but the same overall shape: second synchronous copy
 * pass, repoint (logstore_repoint(), which itself handles the
 * persistently-open segment fd -- see its own doc comment), persist
 * the new placement, remove the old location.
 */
static void finalize_log_storage_migration(void)
{
	const char *target_dir = storagemigrate_job_target_dir(STORAGE_KIND_LOG);
	const char *target_disk = storagemigrate_job_target_disk(STORAGE_KIND_LOG);
	char old_source_dir[PATH_MAX];
	char new_state_path[PATH_MAX];

	snprintf(old_source_dir, sizeof(old_source_dir), "%s", storagemigrate_job_source_dir(STORAGE_KIND_LOG));

	if (storagemigrate_finalize(STORAGE_KIND_LOG) != 0) {
		fprintf(stderr, "log-storage migration: final copy pass failed, migration aborted\n");
		return;
	}

	snprintf(LOG_DIR, sizeof(LOG_DIR), "%s", target_dir);
	snprintf(new_state_path, sizeof(new_state_path), "%s/state.json", LOG_DIR);
	snprintf(LOG_STATE_PATH, sizeof(LOG_STATE_PATH), "%s", new_state_path);
	logstore_repoint(LOG_DIR, LOG_STATE_PATH);

	storageplacement_set(STORAGE_KIND_LOG, target_disk[0] != '\0' ? target_disk : NULL);

	if (persist_remove_tree(old_source_dir) != 0)
		fprintf(stderr, "log-storage migration: could not remove old location %s: %s\n",
		        old_source_dir, strerror(errno));

	fprintf(stderr, "log-storage migration: complete, now active on %s\n",
	        target_disk[0] != '\0' ? target_disk : "(default OS-disk placement)");
}

/*
 * ADR-0141 Phase 4: rebuildable-storage's own finalize -- like state-
 * storage's, several distinct modules (image.c, pkg.c's own several
 * concerns) each need repointing, but unlike state-storage there's no
 * /etc/resolv.conf-style external bind mount, and unlike log-storage
 * there's no persistently-open file handle -- every consumer here was
 * confirmed (not assumed, per ADR-0141's own original review note) to
 * cache nothing but plain path strings.
 */
static void finalize_rebuildable_storage_migration(void)
{
	const char *target_dir = storagemigrate_job_target_dir(STORAGE_KIND_REBUILDABLE);
	const char *target_disk = storagemigrate_job_target_disk(STORAGE_KIND_REBUILDABLE);
	char old_source_dir[PATH_MAX];

	snprintf(old_source_dir, sizeof(old_source_dir), "%s",
	         storagemigrate_job_source_dir(STORAGE_KIND_REBUILDABLE));

	if (storagemigrate_finalize(STORAGE_KIND_REBUILDABLE) != 0) {
		fprintf(stderr, "rebuildable-storage migration: final copy pass failed, migration aborted\n");
		return;
	}

	snprintf(REBUILDABLE_DIR, sizeof(REBUILDABLE_DIR), "%s", target_dir);
	compute_rebuildable_dir_relative_paths();

	image_init(IMAGES_DIR);
	pkg_repoint(PKG_DIR, PKG_INSTALLED_STATE_PATH, IMAGES_DIR, ARTIFACTS_DIR);
	pkg_repo_repoint(PKG_REPO_CONFIG_PATH);
	pkg_cache_repoint(PKG_CACHE_DIR, PKG_CACHE_CONFIG_PATH);
	pkg_artifact_repoint(PKG_ARTIFACT_CONFIG_PATH);

	storageplacement_set(STORAGE_KIND_REBUILDABLE, target_disk[0] != '\0' ? target_disk : NULL);

	if (persist_remove_tree(old_source_dir) != 0)
		fprintf(stderr, "rebuildable-storage migration: could not remove old location %s: %s\n",
		        old_source_dir, strerror(errno));

	fprintf(stderr, "rebuildable-storage migration: complete, now active on %s\n",
	        target_disk[0] != '\0' ? target_disk : "(default OS-disk placement)");
}

static void handle_storage_migrate_event(struct conn *cc)
{
	int status;
	int exit_status;
	enum storage_kind kind = cc->storage_migrate_kind;

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	if (waitpid(cc->pkg_fetch_pid, &status, 0) == cc->pkg_fetch_pid && WIFEXITED(status))
		exit_status = WEXITSTATUS(status);
	else
		exit_status = -1;
	close(cc->fd);
	free(cc);

	storagemigrate_completed(kind, exit_status);
	fprintf(stderr, "storage migrate: bulk copy job finished (kind=%d exit_status=%d)\n", (int)kind,
	        exit_status);
	if (exit_status == 0 && kind == STORAGE_KIND_STATE)
		finalize_state_storage_migration();
	else if (exit_status == 0 && kind == STORAGE_KIND_LOG)
		finalize_log_storage_migration();
	else if (exit_status == 0 && kind == STORAGE_KIND_REBUILDABLE)
		finalize_rebuildable_storage_migration();
}

static const char *iso_build_state_str(enum iso_build_state s)
{
	switch (s) {
	case ISO_BUILD_BUILDING:
		return "building";
	case ISO_BUILD_READY:
		return "ready";
	case ISO_BUILD_FAILED:
		return "failed";
	case ISO_BUILD_NONE:
	default:
		return "none";
	}
}

static void write_iso_status(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "state");
	jw_str(w, iso_build_state_str(g_iso_build_state));
	jw_key(w, "iso_path");
	if (g_iso_build_state == ISO_BUILD_READY)
		jw_str(w, ISO_OUTPUT_PATH);
	else
		jw_null(w);
	jw_key(w, "error");
	if (g_iso_build_state == ISO_BUILD_FAILED)
		jw_str(w, g_iso_build_error);
	else
		jw_null(w);
	jw_obj_close(w);
}

/* GET /v1/system/iso -- status/iso_path polling, the exact shape GET
 * /v1/pkg/hostbuild/{name} already established for its own async job. */
static void handle_system_iso_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	write_iso_status(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * POST /v1/system/iso (ADR-0064) -- every field is optional; an empty
 * body reproduces this tool's original default (a generic, edit-at-
 * the-GRUB-menu ISO, kernel_args left as the "CHANGEME" placeholder
 * image/src/mkinstalleriso.c's own usage text has always documented).
 */
static void handle_system_iso_post(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	const char *disk = NULL;
	const char *ip = NULL;
	const char *prefix = NULL;
	const char *gateway = NULL;
	const char *interface = NULL;
	char err_msg[256];
	struct json_writer w;

	if (body != NULL && body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		disk = json_as_string(json_object_get(root, "disk"));
		ip = json_as_string(json_object_get(root, "ip"));
		prefix = json_as_string(json_object_get(root, "prefix"));
		gateway = json_as_string(json_object_get(root, "gateway"));
		interface = json_as_string(json_object_get(root, "interface"));
	}

	if (g_iso_build_state == ISO_BUILD_BUILDING) {
		if (root != NULL)
			json_free(root);
		respond_error(fd, 409, "Conflict", "an ISO build is already in progress");
		return;
	}
	if (iso_build_start(disk, ip, prefix, gateway, interface, err_msg, sizeof(err_msg)) != 0) {
		if (root != NULL)
			json_free(root);
		respond_error(fd, 400, "Bad Request", err_msg);
		return;
	}
	if (root != NULL)
		json_free(root);

	jw_init(&w);
	write_iso_status(&w);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
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
 * This daemon's first (and, deliberately, narrowest-possible) query
 * string parser: GET .../files?path=... is the first route that ever
 * needs one. One key only, no repeated-key/array semantics, %XX
 * percent-decoding only (no "+" -> space -- this project has never had
 * a form-encoded body, no reason to invent that convention here).
 * full_path is the request's own req->path, "?"-and-all; key is looked
 * up among the "&"-separated pairs after the first "?". Returns 0 and
 * fills out[] on a match, -1 if the key is absent or the value doesn't
 * fit in out_size.
 */
static int url_query_param(const char *full_path, const char *key, char *out, size_t out_size)
{
	const char *q = strchr(full_path, '?');
	size_t key_len = strlen(key);

	if (q == NULL)
		return -1;
	q++;
	while (*q != '\0') {
		const char *amp = strchr(q, '&');
		size_t pair_len = amp != NULL ? (size_t)(amp - q) : strlen(q);

		if (pair_len > key_len && q[key_len] == '=' && strncmp(q, key, key_len) == 0) {
			const char *v = q + key_len + 1;
			size_t vlen = pair_len - key_len - 1;
			size_t oi = 0;
			size_t vi = 0;

			while (vi < vlen) {
				char c = v[vi];

				if (c == '%' && vi + 2 < vlen && isxdigit((unsigned char)v[vi + 1]) &&
				    isxdigit((unsigned char)v[vi + 2])) {
					char hex[3] = { v[vi + 1], v[vi + 2], '\0' };

					c = (char)strtol(hex, NULL, 16);
					vi += 3;
				} else {
					vi++;
				}
				if (oi + 1 >= out_size)
					return -1;
				out[oi++] = c;
			}
			out[oi] = '\0';
			return 0;
		}
		q = amp != NULL ? amp + 1 : q + pair_len;
	}
	return -1;
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
enum disk_resolve_result {
	DISK_RESOLVE_OK = 0,
	DISK_RESOLVE_NOT_FOUND,
	DISK_RESOLVE_NOT_MOUNTED,
	DISK_RESOLVE_NO_ROLE,
};

/*
 * Resolves disk_name (POST /v1/containers' own optional "disk" field,
 * task #638/ADR-0102) to the real host directory a container's own
 * overlay storage should live under, in place of the default
 * CONTAINERS_DIR: <the disk's own live mount_path>/containers.
 * Requires the disk to (a) actually exist, (b) currently be mounted
 * (ADR-0099's live /proc/mounts ground truth, not diskformat.c's own
 * ephemeral per-process job state), and (c) carry the
 * "container-storage" role (diskrole.c, Phase B) -- an arbitrary
 * mounted disk is not automatically a valid target. This is the one
 * thing that makes that role (assignable since Phase B but never
 * consumed by anything until now, per diskrole.h's own "Phase D"
 * comment) actually mean something, rather than being pure inert
 * metadata. Returns DISK_RESOLVE_OK and fills out_root, or the
 * specific reason it failed so the caller can give a precise 400.
 */
static int resolve_container_disk_root(const char *disk_name, char *out_root, size_t out_root_size)
{
	struct discovered_disk disks[DISK_ENUM_MAX];
	int count, i;

	count = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
	for (i = 0; i < count; i++) {
		const char *role;

		if (strcmp(disks[i].name, disk_name) != 0)
			continue;
		if (!disks[i].mounted)
			return DISK_RESOLVE_NOT_MOUNTED;
		role = diskrole_lookup(disk_name);
		if (role == NULL || strcmp(role, "container-storage") != 0)
			return DISK_RESOLVE_NO_ROLE;
		snprintf(out_root, out_root_size, "%s/containers", disks[i].mount_path);
		return DISK_RESOLVE_OK;
	}
	return DISK_RESOLVE_NOT_FOUND;
}

/*
 * The read-side counterpart to resolve_container_disk_root() -- given
 * a disk_name (an already-created entry's own e->disk_name, or one
 * freshly reparsed from a containerdef's own stored body when no live
 * entry exists -- see handle_delete()'s crashed-container cleanup
 * branch), reconstructs the same root its own container_base was
 * computed under, for handle_container_file_read() and
 * handle_container_stats(). Takes the disk name directly rather than
 * a struct registry_entry * so callers with no live entry (a
 * restart:"always" definition currently between a crash and its next
 * restart timer) can still resolve the right root. Re-resolves the
 * disk's current mount_path fresh via disk_enumerate() rather than
 * caching it, matching this project's own "live /proc/mounts is the
 * one source of truth" precedent (ADR-0099) -- if the disk is no
 * longer mounted (a real, rare operational anomaly: it was unmounted
 * after a container was placed on it), falls back to CONTAINERS_DIR,
 * which fails the lookup cleanly (ENOENT) rather than crashing.
 */
static void container_root_for(const char *disk_name, char *out, size_t out_size)
{
	struct discovered_disk disks[DISK_ENUM_MAX];
	int count, i;

	if (disk_name == NULL || disk_name[0] == '\0') {
		snprintf(out, out_size, "%s", CONTAINERS_DIR);
		return;
	}
	count = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
	for (i = 0; i < count; i++) {
		if (strcmp(disks[i].name, disk_name) == 0 && disks[i].mounted) {
			snprintf(out, out_size, "%s/containers", disks[i].mount_path);
			return;
		}
	}
	snprintf(out, out_size, "%s", CONTAINERS_DIR);
}

static int create_container_from_body(const char *body, size_t body_len,
                                       struct registry_entry **out_entry,
                                       char out_restart_policy[16], int *out_restart_delay_seconds,
                                       char out_depends_on[][REGISTRY_NAME_MAX],
                                       int *out_depends_on_count, int *out_has_readiness,
                                       int *out_readiness_tcp_port,
                                       int *out_readiness_timeout_seconds, int *out_follow_rolling,
                                       int *out_has_follow_rolling_jitter,
                                       int *out_follow_rolling_jitter_seconds, char *err_msg,
                                       size_t err_msg_size)
{
	struct json_value *root;
	const struct json_value *jname, *jimage, *jimage_version, *jcmd, *jmem, *jpids, *jcpu, *jcpuset,
	    *jnetworks, *jip_forward, *jroutes;
	const struct json_value *jdisk_quota;
	long long disk_quota_bytes;
	const struct json_value *jdevices;
	const struct json_value *jinterfaces;
	const struct json_value *jfiles, *jsysctls;
	const struct json_value *jrestart, *jrestart_delay, *jdepends_on, *jreadiness;
	const struct json_value *jfollow_rolling, *jfollow_rolling_jitter;
	const char *restart_str;
	long restart_delay, follow_rolling_jitter;
	const char *name, *image;
	char name_copy[REGISTRY_NAME_MAX];
	char lowerdir[PATH_MAX];
	char resolved_image_version[IMAGE_VERSION_MAX];
	char container_base[PATH_MAX];
	char upperdir[PATH_MAX], workdir[PATH_MAX], merged[PATH_MAX];
	struct stat st;
	struct container_spec spec;
	struct registry_entry *entry;
	enum registry_error rerr;
	int create_errno;
	char *argv_buf[CONTAINER_MAX_ARGV];
	char *empty_envp[1];
	size_t argc, i;
	struct registry_network_attachment net_attachments[CONTAINER_MAX_NETWORKS];
	int net_count = 0;
	int ip_forward = 0;
	const struct json_value *jcapture_output;
	int capture_output = 0;
	int output_pipe[2] = { -1, -1 };
	int stdio_write_fd = -1;
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
	const struct json_value *jdns_servers;
	char dns_server_ips[RESOLV_MAX_NAMESERVERS][RESOLV_IP_STRLEN];
	int dns_server_count = 0;
	const struct json_value *jdns_register;
	int dns_register = 0;
	const struct json_value *jpki_issue, *jpki_cert_dir, *jpki_days;
	int pki_issue = 0;
	char pki_cert_dir_buf[PATH_MAX];
	int pki_days = 365;
	const struct json_value *jldap_provision, *jldap_user, *jldap_group, *jldap_uid,
	    *jldap_secret_dir;
	int ldap_provision = 0;
	char ldap_user_buf[LDAP_USER_NAME_MAX];
	char ldap_group_buf[LDAP_GROUP_NAME_MAX];
	int ldap_uid = 0;
	char ldap_secret_dir_buf[PATH_MAX];
	const struct json_value *jdisk;
	const char *disk_name;
	char container_root[PATH_MAX];

	snprintf(out_restart_policy, 16, "no");
	*out_restart_delay_seconds = CONTAINERDEF_DEFAULT_RESTART_DELAY_SECONDS;
	*out_depends_on_count = 0;
	*out_has_readiness = 0;
	*out_follow_rolling = 0;
	*out_has_follow_rolling_jitter = 0;
	*out_follow_rolling_jitter_seconds = 0;

	root = json_parse(body, body_len);
	if (root == NULL) {
		snprintf(err_msg, err_msg_size, "invalid JSON body");
		return 400;
	}

	jname = json_object_get(root, "name");
	jimage = json_object_get(root, "image");
	jimage_version = json_object_get(root, "image_version");
	jcmd = json_object_get(root, "cmd");
	jnetworks = json_object_get(root, "networks");
	jip_forward = json_object_get(root, "ip_forward");
	jcapture_output = json_object_get(root, "capture_output");
	jroutes = json_object_get(root, "routes");
	jdevices = json_object_get(root, "devices");
	jinterfaces = json_object_get(root, "interfaces");
	jfiles = json_object_get(root, "files");
	jsysctls = json_object_get(root, "sysctls");
	jdns_servers = json_object_get(root, "dns_servers");
	jdns_register = json_object_get(root, "dns_register");
	jpki_issue = json_object_get(root, "pki_issue");
	jpki_cert_dir = json_object_get(root, "pki_cert_dir");
	jpki_days = json_object_get(root, "pki_days");
	jdisk = json_object_get(root, "disk");
	name = json_as_string(jname);
	image = json_as_string(jimage);
	disk_name = json_as_string(jdisk);
	ip_forward = (jip_forward != NULL && jip_forward->type == JSON_BOOL && jip_forward->u.boolean);
	capture_output = (jcapture_output != NULL && jcapture_output->type == JSON_BOOL &&
	                   jcapture_output->u.boolean);
	dns_register = (jdns_register != NULL && jdns_register->type == JSON_BOOL &&
	                jdns_register->u.boolean);
	pki_issue = (jpki_issue != NULL && jpki_issue->type == JSON_BOOL && jpki_issue->u.boolean);
	snprintf(pki_cert_dir_buf, sizeof(pki_cert_dir_buf), "%s",
	         json_as_string(jpki_cert_dir) != NULL ? json_as_string(jpki_cert_dir) :
	                                                  "/etc/kanxeo-tls");
	if (jpki_days != NULL)
		pki_days = (int)json_as_number(jpki_days);

	/* Task #727: auto-provisioning hook. ldap_user_buf left empty
	 * (rather than defaulted here) when omitted -- the firing block
	 * below defaults it to entry->name, a stable post-json_free()
	 * buffer, not the raw `name` pointer into root this parse
	 * function must not still be holding onto by then. */
	jldap_provision = json_object_get(root, "ldap_provision");
	jldap_user = json_object_get(root, "ldap_user");
	jldap_group = json_object_get(root, "ldap_group");
	jldap_uid = json_object_get(root, "ldap_uid");
	jldap_secret_dir = json_object_get(root, "ldap_secret_dir");
	ldap_provision = (jldap_provision != NULL && jldap_provision->type == JSON_BOOL &&
	                   jldap_provision->u.boolean);
	ldap_user_buf[0] = '\0';
	if (json_as_string(jldap_user) != NULL)
		snprintf(ldap_user_buf, sizeof(ldap_user_buf), "%s", json_as_string(jldap_user));
	ldap_group_buf[0] = '\0';
	if (json_as_string(jldap_group) != NULL)
		snprintf(ldap_group_buf, sizeof(ldap_group_buf), "%s", json_as_string(jldap_group));
	if (jldap_uid != NULL)
		ldap_uid = (int)json_as_number(jldap_uid);
	snprintf(ldap_secret_dir_buf, sizeof(ldap_secret_dir_buf), "%s",
	         json_as_string(jldap_secret_dir) != NULL ? json_as_string(jldap_secret_dir) :
	                                                     "/etc/kanxeo-ldap");

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

	/*
	 * Part 5 (ADR-0124): same "ignored, not an error" precedent
	 * restart_delay_seconds already established just above for
	 * restart:"no" -- meaningless without a persisted definition to
	 * follow, so it's simply never passed to containerdef_add() by
	 * handle_create()'s own restart_policy != "no" gate, rather than
	 * rejected here.
	 */
	jfollow_rolling = json_object_get(root, "follow_rolling");
	*out_follow_rolling =
	    (jfollow_rolling != NULL && jfollow_rolling->type == JSON_BOOL && jfollow_rolling->u.boolean);

	/*
	 * Part 5 follow-up: an optional per-container override of the
	 * daemon-wide rolling-restart jitter window -- same "ignored, not
	 * an error" precedent as follow_rolling itself just above when
	 * this container never actually follows rolling updates, or when
	 * restart is "no" (handle_create()'s own gate never persists it in
	 * either case). Range-validated the same 0-CONTAINERDEF_JITTER_MAX_
	 * SECONDS bound PUT /v1/system/rolling-config already enforces for
	 * the daemon-wide default, so a container-level override can never
	 * exceed what the daemon itself would ever accept.
	 */
	jfollow_rolling_jitter = json_object_get(root, "follow_rolling_jitter_seconds");
	if (jfollow_rolling_jitter != NULL) {
		follow_rolling_jitter = (long)json_as_number(jfollow_rolling_jitter);
		if (follow_rolling_jitter < 0 || follow_rolling_jitter > CONTAINERDEF_JITTER_MAX_SECONDS) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "follow_rolling_jitter_seconds must be 0-%d",
			         CONTAINERDEF_JITTER_MAX_SECONDS);
			return 400;
		}
		*out_has_follow_rolling_jitter = 1;
		*out_follow_rolling_jitter_seconds = (int)follow_rolling_jitter;
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
	if (ldap_provision) {
		/* Group must exist -- ldap_user_create() would reject an
		 * unresolvable primarygroup anyway, but checking here (before
		 * the container itself is created) matches pki_issue's own
		 * "fail fast on an obviously-unsatisfiable request" pattern,
		 * and reads by ldap.c's own in-memory table (no root pointer
		 * involved), so it's safe this early. Auto-creating the group
		 * is deliberately not done -- an operator's own group naming/
		 * gid-numbering choices shouldn't be second-guessed here. */
		if (ldap_group_buf[0] == '\0' || ldap_group_find(ldap_group_buf) == NULL) {
			json_free(root);
			snprintf(err_msg, err_msg_size,
			         "ldap_provision requires ldap_group to name an existing LDAP group");
			return 400;
		}
		/* An explicit ldap_user must already be a valid LDAP username
		 * (default-to-container-name happens later, post-json_free(),
		 * and is validated there too -- container names allow a wider
		 * charset than LDAP usernames do). */
		if (ldap_user_buf[0] != '\0' && !ldap_username_is_valid(ldap_user_buf)) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "ldap_user is not a valid LDAP username");
			return 400;
		}
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
			/*
			 * A persisted device mapping name (ADR-0048) takes priority
			 * over the raw device.h id namespace -- mapping names are
			 * validated with simple_name_is_valid() at creation time,
			 * which forbids ':', so they can never collide with a real
			 * id (every real id's own bus prefix always contains one).
			 * devicemap_resolve() returns -1 (not 0) when id doesn't
			 * name any mapping at all, which is what falls through to
			 * the existing raw-id path below -- a mapping that DOES
			 * exist but currently resolves to nothing (its device is
			 * unplugged) is a real error here, not silently retried as
			 * a raw id.
			 */
			n = devicemap_resolve(id, CONTAINERS_DIR, matches,
			                       CONTAINER_MAX_DEVICES - device_count);
			if (n < 0)
				n = device_find_group(id, CONTAINERS_DIR, matches,
				                       CONTAINER_MAX_DEVICES - device_count);
			if (n <= 0) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "unknown, unassignable, or not-currently-present device");
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
			dd = device_find(dev_id, CONTAINERS_DIR);
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
			const struct json_value *jowner = json_object_get(item, "owner");
			const struct json_value *jgroup = json_object_get(item, "group");
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
			/*
			 * owner/group (ADR-0144): a real, numeric uid/gid this
			 * exact file lands owned as, staged before the container's
			 * own process ever execve()s (same pre-clone3() write this
			 * whole block already does) -- no NSS lookup involved, no
			 * ordering dependency on some OTHER staged file (e.g.
			 * /etc/passwd) being written first. Omitted means root:root
			 * (this daemon's own real euid/egid, unchanged from before
			 * this field existed). Real need: a file only a specific
			 * non-root container user should be able to read (e.g. a
			 * bind credential an AuthorizedKeysCommand script -- never
			 * root -- must read but nothing else on the box should).
			 */
			if (jowner != NULL && (jowner->type != JSON_NUMBER || json_as_number(jowner) < 0)) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "files owner must be a non-negative uid");
				return 400;
			}
			if (jgroup != NULL && (jgroup->type != JSON_NUMBER || json_as_number(jgroup) < 0)) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "files group must be a non-negative gid");
				return 400;
			}
			if (jdns_servers != NULL && strcmp(path, "/etc/resolv.conf") == 0) {
				json_free(root);
				snprintf(err_msg, err_msg_size,
				         "cannot combine dns_servers with an explicit files[] entry for "
				         "/etc/resolv.conf -- pick one");
				return 400;
			}
		}
	}
	/*
	 * ADR-0143: optional real /etc/resolv.conf staged into the
	 * container's own upperdir, same shape files[] already uses --
	 * deliberately explicit (no auto-wiring to any registered internal
	 * DNS server), same posture ADR-0076 already established for the
	 * host's own equivalent PUT /system/resolv. RESOLV_MAX_NAMESERVERS
	 * is resolv.c's own cap (matches glibc's real resolv.conf MAXNS),
	 * reused here rather than a second invented limit.
	 */
	if (jdns_servers != NULL) {
		if (jdns_servers->type != JSON_ARRAY || jdns_servers->u.array.count > RESOLV_MAX_NAMESERVERS) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "dns_servers must be an array of at most %d entries",
			         RESOLV_MAX_NAMESERVERS);
			return 400;
		}
		for (i = 0; i < jdns_servers->u.array.count; i++) {
			const char *ip = json_as_string(jdns_servers->u.array.items[i]);
			struct in_addr addr;

			if (ip == NULL || inet_pton(AF_INET, ip, &addr) != 1) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "dns_servers entries must be valid IPv4 addresses");
				return 400;
			}
			snprintf(dns_server_ips[i], sizeof(dns_server_ips[i]), "%s", ip);
		}
		dns_server_count = (int)jdns_servers->u.array.count;
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

	/*
	 * ADR-0107/0108: an image's own rootfs is now one of potentially
	 * many immutable per-version directories, not one fixed path --
	 * "image_version" in the request body (present only on a
	 * daemon-restart/`.../start` replay of a create request this
	 * daemon already persisted once, spliced in by handle_container_create()
	 * below -- see its own comment) pins this container to the EXACT
	 * version it was originally created against, so a `pkg install`
	 * against the same image name in between never silently changes
	 * what a replayed/restarted container resolves to. A fresh, real
	 * client request never sets this field, so it resolves the
	 * image's own current version fresh, exactly as before.
	 */
	{
		const char *requested_version = json_as_string(jimage_version);

		if (requested_version != NULL && requested_version[0] != '\0') {
			snprintf(resolved_image_version, sizeof(resolved_image_version), "%s",
			         requested_version);
		} else if (image_current_version(image, resolved_image_version,
		                                  sizeof(resolved_image_version)) != IMAGE_OK) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "image rootfs does not exist");
			return 400;
		}
	}
	image_version_rootfs_path(image, resolved_image_version, lowerdir, sizeof(lowerdir));
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

	if (disk_name != NULL && disk_name[0] != '\0') {
		int drc = resolve_container_disk_root(disk_name, container_root, sizeof(container_root));

		if (drc != DISK_RESOLVE_OK) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "%s",
			         drc == DISK_RESOLVE_NOT_FOUND
			             ? "no such disk"
			             : drc == DISK_RESOLVE_NOT_MOUNTED
			                   ? "disk is not mounted"
			                   : "disk has no container-storage role assigned");
			return 400;
		}
	} else {
		snprintf(container_root, sizeof(container_root), "%s", CONTAINERS_DIR);
	}

	snprintf(container_base, sizeof(container_base), "%s/%s", container_root, name);
	if (persist_mkdir_p(container_base) != 0) {
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
			const struct json_value *jowner = json_object_get(item, "owner");
			const struct json_value *jgroup = json_object_get(item, "group");
			long mode = mode_str != NULL ? strtol(mode_str, NULL, 8) : 0644;
			uid_t owner = jowner != NULL ? (uid_t)json_as_number(jowner) : (uid_t)-1;
			gid_t group = jgroup != NULL ? (gid_t)json_as_number(jgroup) : (gid_t)-1;
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
			    (content_len > 0 && write(fd, content, content_len) != (ssize_t)content_len) ||
			    ((owner != (uid_t)-1 || group != (gid_t)-1) && fchown(fd, owner, group) != 0)) {
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
	if (dns_server_count > 0) {
		char content[RESOLV_MAX_NAMESERVERS * (RESOLV_IP_STRLEN + 16)];
		size_t content_len = 0;
		char target[PATH_MAX];
		char target_dir[PATH_MAX];
		int fd;

		content[0] = '\0';
		for (i = 0; i < (size_t)dns_server_count; i++)
			content_len += (size_t)snprintf(content + content_len, sizeof(content) - content_len,
			                                 "nameserver %s\n", dns_server_ips[i]);

		if (snprintf(target, sizeof(target), "%s/etc/resolv.conf", upperdir) >= (int)sizeof(target)) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "dns_servers path too long");
			return 500;
		}
		snprintf(target_dir, sizeof(target_dir), "%s/etc", upperdir);
		if (persist_mkdir_p(target_dir) != 0) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "failed to stage dns_servers");
			return 500;
		}
		fd = open(target, O_CREAT | O_TRUNC | O_WRONLY, 0644);
		if (fd < 0 || write(fd, content, content_len) != (ssize_t)content_len) {
			if (fd >= 0)
				close(fd);
			json_free(root);
			snprintf(err_msg, err_msg_size, "failed to stage dns_servers");
			return 500;
		}
		close(fd);
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
	jcpu = json_object_get(root, "cpu_max");
	spec.cg.cpu_max = json_as_string(jcpu);
	jcpuset = json_object_get(root, "cpuset_cpus");
	spec.cg.cpuset_cpus = json_as_string(jcpuset);
	jdisk_quota = json_object_get(root, "disk_quota_bytes");
	disk_quota_bytes = jdisk_quota != NULL ? (long long)json_as_number(jdisk_quota) : 0;
	if (disk_quota_bytes > 0) {
		/*
		 * btrfs has no quotactl(2) project-quota support at all
		 * (ADR-0103) -- overlay_create() enforces the limit itself
		 * there, via a qgroup set on the upperdir subvolume it
		 * creates, so there is no project id to assign and no
		 * quotactl(2) call to make up front here. ext4 (and any other
		 * quotactl-capable filesystem) keeps the existing Part 4/
		 * ADR-0062 flow completely unchanged.
		 */
		if (overlay_backing_is_btrfs(container_base)) {
			spec.ov.quota_bytes = disk_quota_bytes;
		} else {
			uint32_t projid;

			if (quotamap_get_or_assign(name, &projid) != 0) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "failed to assign a disk-quota project id");
				return 500;
			}
			/*
			 * Set before the container (and its overlay_create()'s own
			 * FS_IOC_FSSETXATTR tagging) is created -- order-agnostic per
			 * this call's own comment, but doing it first means the limit
			 * is already in force by the moment any file could possibly
			 * be tagged with this project id.
			 */
			if (set_disk_quota(container_base, projid, disk_quota_bytes) != 0) {
				json_free(root);
				snprintf(err_msg, err_msg_size,
				         "failed to set disk quota (backing filesystem may not have "
				         "project-quota support enabled)");
				return 500;
			}
			spec.ov.project_id = projid;
		}
	}
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
		spec.nets[i].has_address = net->has_address;
		spec.nets[i].address_ip_be = net->address_be;
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

	/*
	 * Always-on stdout/stderr capture for an ordinary, operator-created
	 * container -- the same pipe2()/O_NONBLOCK-on-the-read-end pattern
	 * pkg_build_start() already established for build containers
	 * (pkg.c). Unconditional (every container, not gated behind the
	 * client's own "capture_output" request) since transparent
	 * container-log capture landed: every line gets forwarded to
	 * logstore.c regardless, so a container that crash-loops with no
	 * other REST-visible diagnostic (e.g. sshd's -D -e exiting 1 on a
	 * config problem) always has its real stderr discoverable via
	 * GET /v1/system/logs?source=container. The client's own
	 * "capture_output" boolean (the `capture_output` local variable
	 * here) now controls only whether the SAME bytes are ALSO mirrored
	 * into this container's own GET /v1/containers/{name} "captured_output"
	 * tail (see register_container_output()'s call site below,
	 * entry->capture_requested) -- a separate, still-opt-in feature. A
	 * pipe2()/fcntl() failure here is treated as "capture unavailable"
	 * rather than a hard container-create failure -- the container
	 * still gets created, it just runs without capture, exactly like
	 * pkg.c's own build path degrades.
	 */
	if (pipe2(output_pipe, O_CLOEXEC) == 0) {
		if (fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) == 0) {
			spec.capture_output = 1;
			spec.stdout_fd = output_pipe[1];
			spec.stderr_fd = output_pipe[1];
			stdio_write_fd = output_pipe[1];
		} else {
			close(output_pipe[0]);
			close(output_pipe[1]);
			output_pipe[0] = -1;
			output_pipe[1] = -1;
		}
	} else {
		output_pipe[0] = -1;
		output_pipe[1] = -1;
	}

	rerr = registry_create(name, image, resolved_image_version, &spec, net_attachments, net_count,
	                        ip_forward,
	                        device_attachments, device_count, file_paths, file_count, disk_name,
	                        dns_server_ips, dns_server_count, &entry);
	/*
	 * Captured immediately, before json_free() below -- container_create()
	 * (via registry_create()) always preserves errno across every one of
	 * its own failure paths (each does `errno = saved_errno;` right
	 * before returning -1), but free()'s own internal bookkeeping isn't
	 * guaranteed to leave errno alone, so this is the last safe point to
	 * read it. This closes a real, previously-silent gap: every
	 * REGISTRY_ERR_CREATE_FAILED used to collapse into one generic
	 * "failed to create container" regardless of cause (a missing image,
	 * a cgroup controller the kernel never delegated -- see ADR-0079 --
	 * or anything else container_create() itself might fail on), giving
	 * an operator nothing to act on beyond a bare 500.
	 */
	create_errno = errno;
	/*
	 * The parent's own copy of the pipe's write end must be closed
	 * explicitly here, win or lose -- clone3() (no CLONE_FILES) gave
	 * the child an independent copy of the fd table, not a shared one,
	 * so this process's own copy of output_pipe[1] keeps the pipe
	 * "held open" from the read end's perspective (no EOF, ever) until
	 * it's closed, regardless of what the child itself does with its
	 * copy. Same discipline as handle_pkg_fetch_event()'s own
	 * `if (stdio_write_fd >= 0) close(stdio_write_fd);`.
	 */
	if (stdio_write_fd >= 0)
		close(stdio_write_fd);
	/*
	 * A real copy, not just the `name` pointer -- `name` is
	 * json_as_string(jname), pointing straight into `root`'s own
	 * string storage, which json_free() below invalidates. Confirmed
	 * live as a real use-after-free, not a hypothetical one: the log
	 * store's own "container %s: failed to create: ..." line (below)
	 * showed garbled bytes instead of the real container name in the
	 * web dashboard's log view. The success path already sidesteps
	 * this exact hazard by using entry->name instead of `name` (see
	 * its own comment a little further down) -- this was the one
	 * spot that still used the dangling pointer directly.
	 */
	snprintf(name_copy, sizeof(name_copy), "%s", name != NULL ? name : "");
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
		if (output_pipe[0] >= 0)
			close(output_pipe[0]);
		snprintf(err_msg, err_msg_size, "a container with this name already exists");
		return 409;
	}
	if (rerr == REGISTRY_ERR_FULL) {
		if (output_pipe[0] >= 0)
			close(output_pipe[0]);
		snprintf(err_msg, err_msg_size, "container table full");
		return 500;
	}
	if (rerr == REGISTRY_ERR_CREATE_FAILED) {
		if (output_pipe[0] >= 0)
			close(output_pipe[0]);
		snprintf(err_msg, err_msg_size, "failed to create container: %s", strerror(create_errno));
		logstore_write("kanxeod", "error", "container %s: failed to create: %s", name_copy,
		                strerror(create_errno));
		return 500;
	}

	register_container_pidfd(entry);

	if (output_pipe[0] >= 0) {
		entry->output_fd = output_pipe[0];
		/* The pipe/forwarding-to-logstore is now unconditional (see
		 * this function's own comment above output_pipe's setup) --
		 * capture_requested (the client's own "capture_output" opt-in)
		 * controls only whether captured_output itself gets populated. */
		entry->capture_requested = capture_output;
		register_container_output(entry);
	}

	if (dns_register) {
		/* entry->name, not the local `name`, which pointed into
		 * root and is no longer valid after json_free() above.
		 *
		 * siteconfig_qualify() (ADR-0052) applies the site's default
		 * suffix the same way the manual POST /v1/dns/records path
		 * already does -- this call was missing entirely until now
		 * (ADR-0092), leaving every auto-registered container record
		 * (e.g. dns-1/dns-2's own names) bare while every manually-
		 * created one got the suffix, a real, user-reported
		 * inconsistency with no reason behind it.
		 */
		char qualified_name[DNS_NAME_MAX];
		struct dns_record *rec;
		enum dns_error derr;

		siteconfig_qualify(entry->name, qualified_name, sizeof(qualified_name));
		derr = dns_record_create(qualified_name, spec.nets[0].container_ip_be, entry->name, &rec);

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

	if (ldap_provision) {
		/* task #727: a service/bind identity for this container itself --
		 * NOT a human login account (those are created directly via
		 * POST /v1/ldap/users, task #726). Mirrors pki_issue's own
		 * shape closely: pid-dependent (secret delivery needs a live
		 * container root), so it fires in this same post-pidfd block,
		 * and tolerates its own "already exists" case the identical
		 * way pki_issue tolerates PKI_ERR_DUPLICATE -- a respawn under
		 * restart-always gets a fresh pid and needs its secret
		 * delivered again even though the account itself already
		 * exists. The plaintext secret is never persisted anywhere
		 * (see ldap_generate_secret()'s own comment) -- a fresh one is
		 * generated and re-hashed into the account on every single
		 * fire of this block, respawn or not, and only this one
		 * delivery ever sees the plaintext. */
		const struct ldap_group *g = ldap_group_find(ldap_group_buf); /* validated non-empty above */
		const char *ldap_user_name = ldap_user_buf[0] != '\0' ? ldap_user_buf : entry->name;
		int uid = ldap_uid != 0 ? ldap_uid : ldap_uid_alloc();
		char secret[LDAP_PROVISION_SECRET_LEN + 1];
		struct ldap_user *u = NULL;
		enum ldap_record_error lerr = LDAP_RECORD_ERR_NOT_FOUND;

		if (!ldap_username_is_valid(ldap_user_name)) {
			fprintf(stderr,
			        "%s: ldap_provision requested but the container name isn't a valid "
			        "LDAP username -- pass ldap_user= explicitly\n",
			        entry->name);
		} else if (g == NULL || ldap_generate_secret(secret) != 0) {
			fprintf(stderr, "%s: ldap_provision requested but the group vanished or "
			                "/dev/urandom couldn't be read\n",
			        entry->name);
		} else {
			lerr = ldap_user_create(ldap_user_name, uid, g->gidnumber, NULL, 0, NULL, NULL, NULL,
			                         NULL, NULL, secret, 0, entry->name, 1, NULL, &u);
			if (lerr == LDAP_RECORD_ERR_DUPLICATE) {
				/* Respawn under restart-always: the account already
				 * exists (from this container's own first start) --
				 * re-hash the freshly generated secret into it so
				 * this respawn's own delivery below is valid. */
				u = ldap_user_find(ldap_user_name);
				if (u != NULL)
					lerr = ldap_user_update(ldap_user_name, NULL, uid, g->gidnumber,
					                         u->secondary_groups, u->secondary_group_count,
					                         u->givenname, u->sn, u->mail, u->loginshell,
					                         u->homedirectory, secret, u->disabled,
					                         u->ssh_public_key, u->can_search, &u);
			}
			if (lerr != LDAP_RECORD_OK || u == NULL) {
				fprintf(stderr,
				        "%s: ldap_provision requested but the account create/update failed "
				        "(err=%d)\n",
				        entry->name, (int)lerr);
			} else {
				char parent[PATH_MAX + 32], dst[PATH_MAX + 32];

				if (snprintf(parent, sizeof(parent), "/proc/%d/root%s",
				             (int)entry->handle.pid, ldap_secret_dir_buf) >=
				        (int)sizeof(parent) ||
				    snprintf(dst, sizeof(dst), "%s/bind.secret", parent) >= (int)sizeof(dst) ||
				    persist_mkdir_p(parent) != 0 ||
				    persist_atomic_write(dst, secret, strlen(secret)) != 0) {
					fprintf(stderr,
					        "%s: ldap_provision account ready but delivering its secret "
					        "into the container failed\n",
					        entry->name);
				} else {
					chmod(dst, 0600);
				}
			}
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
	int follow_rolling;
	int has_follow_rolling_jitter, follow_rolling_jitter_seconds;
	char err_msg[256];
	int status;
	struct json_writer w;

	status = create_container_from_body(body, body_len, &entry, restart_policy,
	                                     &restart_delay_seconds, depends_on, &depends_on_count,
	                                     &has_readiness, &readiness_tcp_port,
	                                     &readiness_timeout_seconds, &follow_rolling,
	                                     &has_follow_rolling_jitter, &follow_rolling_jitter_seconds,
	                                     err_msg, sizeof(err_msg));
	if (status != 0) {
		respond_error(fd, status, http_status_text(status), err_msg);
		return;
	}

	if (strcmp(restart_policy, "no") != 0) {
		/*
		 * ADR-0107/0108: splice "image_version" onto the raw request
		 * body before persisting it -- containerdef_add() stores this
		 * exact byte string, replayed verbatim (never re-parsed
		 * against fresh state) by handle_start()/
		 * handle_restart_timer_event()/containerdef_autostart_all() on
		 * every future revival of this definition. Without this, a
		 * replay would call image_current_version() fresh and
		 * silently re-pin a restarted container to whatever the
		 * image's current version has become by then -- exactly the
		 * bug this whole epic exists to close, just moved from "pkg
		 * install" to "daemon restart" as the trigger. Raw string
		 * surgery (find the body's own trailing '}', splice one more
		 * key in front of it) rather than a full JSON re-serialize --
		 * the body was already validated as a well-formed JSON object
		 * by create_container_from_body()'s own json_parse() above,
		 * so its last non-whitespace byte is guaranteed to be '}'.
		 */
		char *persisted_body = NULL;
		size_t persisted_len = body_len;
		const char *persist_src = body;

		if (entry->image_version[0] != '\0') {
			size_t trim = body_len;

			while (trim > 0 && (body[trim - 1] == ' ' || body[trim - 1] == '\t' ||
			                     body[trim - 1] == '\n' || body[trim - 1] == '\r'))
				trim--;
			if (trim > 0 && body[trim - 1] == '}') {
				persisted_body = malloc(trim + 128);
				if (persisted_body != NULL) {
					int n;

					memcpy(persisted_body, body, trim - 1);
					n = snprintf(persisted_body + (trim - 1), 128,
					             ",\"image_version\":\"%s\"}", entry->image_version);
					persisted_len = (trim - 1) + (size_t)n;
					persist_src = persisted_body;
				}
			}
		}

		if (containerdef_add(entry->name, persist_src, persisted_len, depends_on, depends_on_count,
		                      has_readiness, readiness_tcp_port, readiness_timeout_seconds,
		                      restart_policy, restart_delay_seconds, follow_rolling,
		                      has_follow_rolling_jitter, follow_rolling_jitter_seconds) != 0) {
			fprintf(stderr,
			        "%s: restart:\"%s\" requested but persisting its definition failed -- "
			        "it will not survive a daemon restart\n",
			        entry->name, restart_policy);
		}
		free(persisted_body);
	}

	jw_init(&w);
	registry_write_json_one(entry, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_delete(int fd, const char *name)
{
	struct registry_entry *e = registry_find(name);
	struct container_def *def = containerdef_find(name);
	struct conn *cc;
	char disk_name[DISKROLE_DISK_NAME_MAX];

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
	if (e == NULL && def == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}

	disk_name[0] = '\0';

	if (e != NULL) {
		if (e->reactor_conn != NULL) {
			cc = e->reactor_conn;
			kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
			free(cc);
			e->reactor_conn = NULL;
		}

		snprintf(disk_name, sizeof(disk_name), "%s", e->disk_name);

		/*
		 * registry_remove() SIGKILLs the process first (thawing it if
		 * paused, so the signal can actually be delivered) -- the disk
		 * cleanup below must run AFTER this, never before: overlay_create()
		 * mounted this container's own overlay (container_base/merged)
		 * directly in the daemon's own root mount namespace (the mount(2)
		 * call happens in create_container_from_body(), before clone3()
		 * ever forks the container's own separate namespace), and that
		 * mount is never explicitly torn down anywhere else in this
		 * codebase -- confirmed by grep, the only umount2() calls that
		 * exist at all are mountns_pivot()'s own old-root cleanup, which
		 * runs inside the container's own child process, a completely
		 * separate mount namespace. Running persist_remove_tree() while
		 * that mount is still live would be a real correctness hazard,
		 * not just an ordering nicety: nftw() without FTW_MOUNT freely
		 * crosses into a mounted subdirectory, so it would walk straight
		 * into the still-live overlay view and start unlinking through
		 * it -- silently mutating (or, on a still-alive process, actively
		 * corrupting) the upperdir via the overlay itself, then failing
		 * on the mountpoint's own rmdir() (EBUSY) partway through, an
		 * even worse outcome than doing nothing.
		 */
		registry_remove(name);
	} else {
		/*
		 * task #759 follow-up: e == NULL here does NOT mean "never
		 * autostarted, nothing on disk" -- it also covers a
		 * restart:"always" container that DID create successfully at
		 * least once and is simply between a crash and its next
		 * handle_restart_timer_event() retry (registry_remove() runs
		 * on every exit, live or crashed, long before this handler is
		 * ever reached). The previous version of this function skipped
		 * disk cleanup entirely for this case, based on the (false)
		 * assumption below it. Confirmed the hard way, live on
		 * 192.168.15.95: a jump box container crash-looped forever
		 * with "Unable to load host key: bad permissions" because its
		 * very first-ever creation left a 0644 host-key file in its
		 * upperdir, and every subsequent DELETE + recreate under the
		 * SAME name silently reused that same never-cleaned upperdir
		 * (files[] staging's own open(O_CREAT|O_TRUNC) never chmod()s
		 * an already-existing file to the newly requested mode) --
		 * repeating the identical crash forever, surviving any number
		 * of "delete and recreate" cycles. The overlay mount is still
		 * live too (nothing unmounts it on a plain crash, same
		 * reasoning as the e != NULL branch above), so this is exactly
		 * as safe to clean up here, not a special case.
		 *
		 * disk_name is re-derived from the persisted definition's own
		 * stored body (the same "disk" field create_container_from_body()
		 * itself reads), since there's no live registry entry to read
		 * e->disk_name from.
		 */
		struct json_value *defroot = json_parse(def->body, def->body_len);

		if (defroot != NULL) {
			const char *d = json_as_string(json_object_get(defroot, "disk"));

			if (d != NULL)
				snprintf(disk_name, sizeof(disk_name), "%s", d);
			json_free(defroot);
		}
	}

	/*
	 * task #738 (extended by the task #759 follow-up above to also
	 * cover the e == NULL/crashed case): DELETE never removed a
	 * container's own on-disk upper/work/merged directories -- a real,
	 * pre-existing disk-space leak for every deleted container
	 * (confirmed: no code anywhere in this codebase ever called
	 * anything equivalent to this in a container-teardown context; the
	 * quotamap.h doc comment that used to justify this as deliberate
	 * cited "ADR-0054's pre-existing backup/restore design" --
	 * ADR-0054 is entirely about host-side stats and says nothing
	 * about backup/restore at all; ADR-0033, the *real* backup/restore
	 * ADR, explicitly scopes workload data as "each container's own
	 * concern, not this endpoint's" and reconstructs a restored
	 * container via a fresh containerdef replay, never by resurrecting
	 * old upperdir content -- so no real design anywhere actually
	 * depended on this retention; it was a stale, incorrect citation
	 * for a genuine oversight). container_root_for() resolves the same
	 * root the container was actually created under (ADR-0102 -- the
	 * default CONTAINERS_DIR, or an operator-chosen disk). Best-effort
	 * throughout -- a failure here is logged, never blocks the delete
	 * itself from completing (the registry/containerdef state is the
	 * one source of truth for whether a container exists; leftover
	 * disk state after a failed cleanup is a nit, not a reason to
	 * leave the container definition half-deleted).
	 */
	{
		char container_root[PATH_MAX];
		char container_base[PATH_MAX];
		char merged[PATH_MAX];

		container_root_for(disk_name, container_root, sizeof(container_root));
		snprintf(container_base, sizeof(container_base), "%s/%s", container_root, name);
		snprintf(merged, sizeof(merged), "%s/merged", container_base);
		if (umount2(merged, MNT_DETACH) != 0 && errno != EINVAL && errno != ENOENT)
			fprintf(stderr, "DELETE %s: umount2(%s) failed: %s\n", name, merged,
			        strerror(errno));
		if (persist_remove_tree(container_base) != 0)
			fprintf(stderr, "DELETE %s: failed to remove %s: %s\n", name, container_base,
			        strerror(errno));
	}

	/*
	 * Unconditional, matching the disk cleanup above -- a crashed-but-
	 * still-defined container releasing its own DNS/LDAP/PKI/NTP/SSH/
	 * syslog-target ownership on DELETE is exactly as correct as a live one
	 * doing so; there is no reason a still-registered DNS record or
	 * LDAP SSH-target sync should outlive a definition that's being
	 * permanently removed just because the container happened to be
	 * mid-crash at the moment of the DELETE call.
	 */
	dns_server_forget(name);
	dns_record_forget_owner(name);
	ldap_server_forget(name);
	ldap_user_forget_owner(name);
	pki_cert_forget_owner(name);
	ntp_server_forget(name);
	syslogfwd_target_forget(name);

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
 * ADR-0142 Section 4: real cutover for a container-storage migration,
 * run synchronously once containerstoragemigrate_completed()'s own
 * bulk async copy pass has already succeeded -- mirrors finalize_
 * state_storage_migration()'s own overall shape (save the old source
 * dir first, run the second synchronous copy pass, only then commit),
 * but with the one real difference ADR-0142 calls out: a container's
 * own overlay is actively read/written by its own live process, so
 * unlike a daemon-wide singleton this needs a real stop before the
 * final copy pass can see a quiescent, fully consistent tree, and a
 * real restart afterward. Uses the exact same registry_remove()-plus-
 * reactor-conn-teardown primitive handle_stop()/handle_rolling_restart_
 * timer_event() already use, and the exact same create_container_
 * from_body() replay handle_rolling_restart_timer_event() already
 * uses to bring it back -- one source of truth for "how a container
 * stops" and "how a definition becomes live" respectively, not a third
 * bespoke version of either just for this. A container with no
 * persisted definition can never reach here (the POST handler below
 * rejects it before ever starting a job), so the replay always has a
 * real body to work from.
 *
 * On any failure after the container has already been stopped (final
 * copy pass fails, or the replay itself fails), the container is
 * restarted from its ORIGINAL location rather than left down -- the
 * migration is treated as a whole, all-or-nothing operation from the
 * caller's point of view, matching every other storage-placement
 * migration's own "don't guess, surface it, never leave things half
 * done" posture.
 */
static void finalize_container_storage_migration(const char *name)
{
	char old_source_dir[PATH_MAX];
	char target_dir[PATH_MAX];
	char target_disk[DISKROLE_DISK_NAME_MAX];
	char merged[PATH_MAX];
	struct registry_entry *live;
	struct container_def *def;

	snprintf(old_source_dir, sizeof(old_source_dir), "%s",
	         containerstoragemigrate_job_source_dir(name));
	snprintf(target_dir, sizeof(target_dir), "%s", containerstoragemigrate_job_target_dir(name));
	snprintf(target_disk, sizeof(target_disk), "%s", containerstoragemigrate_job_target_disk(name));

	live = registry_find(name);
	if (live != NULL) {
		if (live->reactor_conn != NULL) {
			struct conn *rc = live->reactor_conn;

			kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, rc->fd, NULL);
			free(rc);
			live->reactor_conn = NULL;
		}
		registry_remove(name);
	}

	/* Best-effort, same tolerant errno handling as DELETE's own crashed-
	 * container cleanup (handle_delete() above) -- see that function's
	 * comment for why this can legitimately be a no-op. */
	snprintf(merged, sizeof(merged), "%s/merged", old_source_dir);
	if (umount2(merged, MNT_DETACH) != 0 && errno != EINVAL && errno != ENOENT)
		fprintf(stderr, "%s: container-storage migration: umount2(%s) failed: %s\n", name, merged,
		        strerror(errno));

	if (containerstoragemigrate_finalize(name) != 0) {
		fprintf(stderr,
		        "%s: container-storage migration: final copy pass failed, restarting from the "
		        "original location\n",
		        name);
		def = containerdef_find(name);
		if (def != NULL) {
			struct registry_entry *entry;
			char restart_policy[16];
			int restart_delay_seconds, depends_on_count, has_readiness, readiness_tcp_port;
			int readiness_timeout_seconds, follow_rolling, has_follow_rolling_jitter;
			int follow_rolling_jitter_seconds;
			char depends_on[CONTAINERDEF_MAX_DEPENDS][REGISTRY_NAME_MAX];
			char err_msg[256];

			if (create_container_from_body(def->body, def->body_len, &entry, restart_policy,
			                                &restart_delay_seconds, depends_on, &depends_on_count,
			                                &has_readiness, &readiness_tcp_port,
			                                &readiness_timeout_seconds, &follow_rolling,
			                                &has_follow_rolling_jitter,
			                                &follow_rolling_jitter_seconds, err_msg,
			                                sizeof(err_msg)) != 0)
				fprintf(stderr, "%s: container-storage migration: restart after abort failed: %s\n",
				        name, err_msg);
		}
		return;
	}

	if (containerdef_patch_disk(name, target_disk[0] != '\0' ? target_disk : NULL) != 0)
		fprintf(stderr,
		        "%s: container-storage migration: failed to persist the new disk placement -- a "
		        "future daemon restart would replay the OLD location\n",
		        name);

	def = containerdef_find(name);
	if (def != NULL) {
		struct registry_entry *entry;
		char restart_policy[16];
		int restart_delay_seconds, depends_on_count, has_readiness, readiness_tcp_port;
		int readiness_timeout_seconds, follow_rolling, has_follow_rolling_jitter;
		int follow_rolling_jitter_seconds;
		char depends_on[CONTAINERDEF_MAX_DEPENDS][REGISTRY_NAME_MAX];
		char err_msg[256];

		if (create_container_from_body(def->body, def->body_len, &entry, restart_policy,
		                                &restart_delay_seconds, depends_on, &depends_on_count,
		                                &has_readiness, &readiness_tcp_port,
		                                &readiness_timeout_seconds, &follow_rolling,
		                                &has_follow_rolling_jitter, &follow_rolling_jitter_seconds,
		                                err_msg, sizeof(err_msg)) != 0) {
			containerstoragemigrate_mark_failed(name, err_msg);
			fprintf(stderr, "%s: container-storage migration: cutover restart failed: %s\n", name,
			        err_msg);
			return;
		}
	}

	if (persist_remove_tree(old_source_dir) != 0)
		fprintf(stderr, "%s: container-storage migration: could not remove old location %s: %s\n",
		        name, old_source_dir, strerror(errno));

	fprintf(stderr, "%s: container-storage migration: complete, now active on %s\n", name,
	        target_disk[0] != '\0' ? target_disk : "(default OS-disk placement)");
}

static void handle_container_storage_migrate_event(struct conn *cc)
{
	int status;
	int exit_status;
	char name[REGISTRY_NAME_MAX];

	snprintf(name, sizeof(name), "%s", cc->container_storage_migrate_name);

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	if (waitpid(cc->pkg_fetch_pid, &status, 0) == cc->pkg_fetch_pid && WIFEXITED(status))
		exit_status = WEXITSTATUS(status);
	else
		exit_status = -1;
	close(cc->fd);
	free(cc);

	containerstoragemigrate_completed(name, exit_status);
	fprintf(stderr, "%s: container-storage migrate: bulk copy job finished (exit_status=%d)\n", name,
	        exit_status);
	if (exit_status == 0)
		finalize_container_storage_migration(name);
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
	int follow_rolling;
	int has_follow_rolling_jitter, follow_rolling_jitter_seconds;
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
	                                     &readiness_timeout_seconds, &follow_rolling,
	                                     &has_follow_rolling_jitter, &follow_rolling_jitter_seconds,
	                                     err_msg, sizeof(err_msg));
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
 * GET /v1/containers/{name}/files?path=... (ADR-0055): the read-path
 * counterpart to the create-time "files[]" staging (which is write-
 * only, host->container, pre-clone3()) -- reads one file's raw bytes
 * back out of a container's rootfs, running or exited-but-not-removed.
 *
 * Path resolution branches on liveness, same reasoning ADR-0013's own
 * write path already established for a *running* container (a stale
 * registry_entry.handle.pid is never safe to trust once running==0 --
 * pids get reused -- so /proc/<pid>/root only applies while running==1):
 *   running   -> /proc/<pid>/root<path>  (kernel resolves through the
 *                container's own mount namespace/root, no setns())
 *   !running  -> <CONTAINERS_DIR>/<name>/upper<path> first (the COW
 *                upper layer the container itself wrote to -- still a
 *                real host directory even after every process in the
 *                overlay's own mount namespace has exited), falling
 *                back to <IMAGES_DIR>/<image>/rootfs<path> (the shared,
 *                read-only lowerdir -- entry->image survives exit).
 *
 * Response is this daemon's first non-JSON body: raw bytes via
 * http_write_response(), same primitive/pattern staticfile.c's own
 * static_serve() already uses for the web dashboard's static assets.
 */
static void handle_logs_get(int fd, const struct http_request *req)
{
	char source[LOGSTORE_SOURCE_MAX];
	char level[LOGSTORE_LEVEL_MAX];
	char container[LOGSTORE_CONTAINER_MAX];
	char msg_regex[256];
	char tail_str[32], since_str[32];
	const char *source_filter = NULL;
	const char *level_filter = NULL;
	const char *container_filter = NULL;
	const char *regex_filter = NULL;
	int64_t since = 0;
	int limit = 0;
	struct json_writer w;

	if (url_query_param(req->path, "source", source, sizeof(source)) == 0)
		source_filter = source;
	if (url_query_param(req->path, "level", level, sizeof(level)) == 0)
		level_filter = level;
	if (url_query_param(req->path, "container", container, sizeof(container)) == 0)
		container_filter = container;
	if (url_query_param(req->path, "tail", tail_str, sizeof(tail_str)) == 0)
		limit = atoi(tail_str);
	if (url_query_param(req->path, "since", since_str, sizeof(since_str)) == 0)
		since = (int64_t)atoll(since_str);
	if (url_query_param(req->path, "regex", msg_regex, sizeof(msg_regex)) == 0) {
		/* Compile-tested here, not just inside logstore_tail_ex() --
		 * a malformed pattern is a real client mistake (400), not
		 * something to silently match zero results for. */
		regex_t re;

		if (regcomp(&re, msg_regex, REG_EXTENDED | REG_NOSUB | REG_ICASE) != 0) {
			respond_error(fd, 400, "Bad Request", "invalid regex pattern");
			return;
		}
		regfree(&re);
		regex_filter = msg_regex;
	}

	jw_init(&w);
	logstore_tail_ex(source_filter, level_filter, container_filter, regex_filter, since, limit, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_logs_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "max_bytes");
	jw_int(&w, logstore_max_bytes());
	jw_key(&w, "min_level");
	jw_str(&w, logstore_min_level());
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Both fields are optional and independent -- a caller changing only
 * max_bytes doesn't need to resupply min_level and vice versa (each
 * setter validates/persists on its own, matching every other partial-
 * update PUT in this daemon, e.g. daemon-config's "only the fields
 * given are touched" convention). At least one of the two is
 * required, or this is a no-op PUT that would silently succeed
 * without changing anything.
 */
static void handle_logs_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jmax, *jlevel;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jmax = json_object_get(root, "max_bytes");
	jlevel = json_object_get(root, "min_level");
	if (jmax == NULL && jlevel == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "max_bytes and/or min_level required");
		return;
	}

	if (jmax != NULL) {
		enum logstore_error lerr = logstore_set_max_bytes((int64_t)json_as_number(jmax));

		if (lerr != LOGSTORE_OK) {
			json_free(root);
			respond_error(fd, lerr == LOGSTORE_ERR_INVALID_MAX_BYTES ? 400 : 500,
			              lerr == LOGSTORE_ERR_INVALID_MAX_BYTES ? "Bad Request"
			                                                     : "Internal Server Error",
			              lerr == LOGSTORE_ERR_INVALID_MAX_BYTES
			                  ? "max_bytes out of range"
			                  : "log config could not be persisted");
			return;
		}
	}
	if (jlevel != NULL) {
		enum logstore_error lerr = logstore_set_min_level(json_as_string(jlevel));

		if (lerr != LOGSTORE_OK) {
			json_free(root);
			respond_error(fd, lerr == LOGSTORE_ERR_INVALID_MIN_LEVEL ? 400 : 500,
			              lerr == LOGSTORE_ERR_INVALID_MIN_LEVEL ? "Bad Request"
			                                                     : "Internal Server Error",
			              lerr == LOGSTORE_ERR_INVALID_MIN_LEVEL
			                  ? "min_level must be one of emerg/alert/crit/err(or)/warning(warn)/"
			                    "notice/info/debug"
			                  : "log config could not be persisted");
			return;
		}
	}
	json_free(root);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "max_bytes");
	jw_int(&w, logstore_max_bytes());
	jw_key(&w, "min_level");
	jw_str(&w, logstore_min_level());
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_container_file_read(int fd, const char *name, const char *rel_path)
{
	struct registry_entry *e = registry_find(name);
	char full_path[PATH_MAX];
	char resolved_version[IMAGE_VERSION_MAX];
	int file_fd;
	struct stat st;
	char *buf;
	size_t total;

	if (e == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}
	if (rel_path == NULL || rel_path[0] != '/' || strlen(rel_path) >= CONTAINER_FILE_PATH_MAX ||
	    !file_path_is_safe(rel_path)) {
		respond_error(fd, 400, "Bad Request", "invalid or missing path");
		return;
	}

	if (e->running) {
		snprintf(full_path, sizeof(full_path), "/proc/%d/root%s", (int)e->handle.pid, rel_path);
		file_fd = open(full_path, O_RDONLY);
	} else {
		char container_root[PATH_MAX];

		container_root_for(e->disk_name, container_root, sizeof(container_root));
		snprintf(full_path, sizeof(full_path), "%s/%s/upper%s", container_root, name, rel_path);
		file_fd = open(full_path, O_RDONLY);
		if (file_fd < 0) {
			/*
			 * ADR-0107/0108: fall back to the EXACT version this
			 * container's own overlay lowerdir was built from
			 * (e->image_version, resolved once at creation time),
			 * never the image's current version -- an unchanged base
			 * file this container never wrote to still has to come
			 * from the same rootfs its own overlay actually used, not
			 * whatever a later `pkg install` against the same image
			 * name has since produced.
			 */
			char image_rootfs[PATH_MAX];

			if (e->image_version[0] != '\0') {
				image_version_rootfs_path(e->image, e->image_version, image_rootfs,
				                           sizeof(image_rootfs));
			} else if (image_current_version(e->image, resolved_version,
			                                  sizeof(resolved_version)) == IMAGE_OK) {
				image_version_rootfs_path(e->image, resolved_version, image_rootfs,
				                           sizeof(image_rootfs));
			} else {
				image_rootfs[0] = '\0';
			}
			snprintf(full_path, sizeof(full_path), "%s%s", image_rootfs, rel_path);
			file_fd = open(full_path, O_RDONLY);
		}
	}
	if (file_fd < 0) {
		respond_error(fd, 404, "Not Found", "no such file");
		return;
	}
	if (fstat(file_fd, &st) != 0) {
		close(file_fd);
		respond_error(fd, 500, "Internal Server Error", "stat failed");
		return;
	}
	if (S_ISDIR(st.st_mode)) {
		close(file_fd);
		respond_error(fd, 400, "Bad Request", "path is a directory, not a file");
		return;
	}

	buf = st.st_size > 0 ? malloc((size_t)st.st_size) : NULL;
	if (st.st_size > 0 && buf == NULL) {
		close(file_fd);
		respond_error(fd, 500, "Internal Server Error", "out of memory");
		return;
	}
	total = 0;
	while (total < (size_t)st.st_size) {
		ssize_t n = read(file_fd, buf + total, (size_t)st.st_size - total);

		if (n <= 0)
			break;
		total += (size_t)n;
	}
	close(file_fd);

	http_set_blocking(fd);
	http_write_response(fd, 200, "OK", "application/octet-stream", buf, total);
	free(buf);
}

/*
 * PUT /v1/containers/{name}/files?path=... (ADR-0153, task #861): the
 * write-path counterpart to handle_container_file_read() immediately
 * above -- same running/!running path resolution (running:
 * /proc/<pid>/root<path>, kernel-resolved through the container's own
 * mount namespace, no setns() needed; !running: <containers-dir>/
 * <name>/upper<path> directly, the same real host directory overlay_
 * create() already mounts as the upper layer). Reuses the exact
 * write-then-chmod/chown sequence create-time files[] staging already
 * established (handle_create()'s own inline loop) rather than a
 * second copy.
 *
 * Deliberately LIVE and EPHEMERAL, not persisted into the container's
 * own definition -- this is a hotfix primitive (patch a running
 * container the way an operator would hand-edit a file over SSH),
 * not a way to durably change what a future restart/recreate produces.
 * A restart replays the persisted create-body unchanged, and this
 * write is never folded into it, on purpose: merging one file update
 * into an arbitrary already-persisted JSON files[] array is real,
 * separate complexity (matching an existing entry by path vs.
 * appending a new one, preserving every other field) that doesn't
 * belong bolted onto a same primitive that's supposed to stay simple
 * and immediate. The durable path for "this change should survive a
 * recreate" is a container recipe (ADR-0151): capture the updated
 * file content there and re-apply, the same reproducible path this
 * project already uses for exactly that. Both together (live patch
 * now, recipe update for later) intentionally mirror how `pkg
 * install` (live, immediate) and an image recipe's own package list
 * (declared intent for a future build) already relate.
 */
static void handle_container_file_write(int fd, const char *name, const char *rel_path,
                                         const char *body, size_t body_len)
{
	struct registry_entry *e = registry_find(name);
	struct json_value *root;
	const char *raw_content;
	char *content;
	const char *mode_str;
	const struct json_value *jowner, *jgroup;
	long mode;
	uid_t owner;
	gid_t group;
	size_t content_len;
	char full_path[PATH_MAX];
	char target_dir[PATH_MAX];
	char *slash;
	int file_fd;

	if (e == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}
	if (rel_path == NULL || rel_path[0] != '/' || strlen(rel_path) >= CONTAINER_FILE_PATH_MAX ||
	    !file_path_is_safe(rel_path)) {
		respond_error(fd, 400, "Bad Request", "invalid or missing path");
		return;
	}

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	raw_content = json_as_string(json_object_get(root, "content"));
	if (raw_content == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "content is required");
		return;
	}
	/*
	 * A copy, not the json_value's own pointer -- content is used below
	 * in a real write(2) call, which must happen before root (and
	 * everything it owns, including raw_content) is freed. Freeing
	 * root first and using the string afterward was a real use-after-
	 * free caught live during this same feature's own verification
	 * pass (a real write produced garbage bytes, not the file's actual
	 * content) -- fixed here, not worked around.
	 */
	content = strdup(raw_content);
	if (content == NULL) {
		json_free(root);
		respond_error(fd, 500, "Internal Server Error", "out of memory");
		return;
	}
	mode_str = json_as_string(json_object_get(root, "mode"));
	mode = mode_str != NULL ? strtol(mode_str, NULL, 8) : 0644;
	jowner = json_object_get(root, "owner");
	jgroup = json_object_get(root, "group");
	owner = jowner != NULL ? (uid_t)json_as_number(jowner) : (uid_t)-1;
	group = jgroup != NULL ? (gid_t)json_as_number(jgroup) : (gid_t)-1;
	content_len = strlen(content);

	if (e->running) {
		snprintf(full_path, sizeof(full_path), "/proc/%d/root%s", (int)e->handle.pid, rel_path);
	} else {
		char container_root[PATH_MAX];

		container_root_for(e->disk_name, container_root, sizeof(container_root));
		snprintf(full_path, sizeof(full_path), "%s/%s/upper%s", container_root, name, rel_path);
	}
	json_free(root);

	snprintf(target_dir, sizeof(target_dir), "%s", full_path);
	slash = strrchr(target_dir, '/');
	if (slash != NULL)
		*slash = '\0';
	if (persist_mkdir_p(target_dir) != 0) {
		free(content);
		respond_error(fd, 500, "Internal Server Error", "failed to create parent directory");
		return;
	}

	file_fd = open(full_path, O_CREAT | O_TRUNC | O_WRONLY, (mode_t)mode);
	if (file_fd < 0 ||
	    (content_len > 0 && write(file_fd, content, content_len) != (ssize_t)content_len) ||
	    (fchmod(file_fd, (mode_t)mode) != 0) ||
	    ((owner != (uid_t)-1 || group != (gid_t)-1) && fchown(file_fd, owner, group) != 0)) {
		if (file_fd >= 0)
			close(file_fd);
		free(content);
		respond_error(fd, 500, "Internal Server Error", "failed to write file");
		return;
	}
	close(file_fd);
	free(content);

	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * GET /v1/containers/{name}/stats (ADR-0054): real, host-side CPU/
 * memory/disk/network usage for one container, gathered entirely from
 * kernel interfaces the daemon already has open (cgroup_fd) or can
 * derive (the host-side veth name, network.c's own vh<pid>-<idx>
 * convention) -- no in-container agent. A raw, point-in-time snapshot
 * on every call, deliberately: no server-side history/ring buffer,
 * see ADR-0054 for why the daemon stays stateless for this feature.
 * Works for an exited-but-still-registered container too (cgroup
 * leaves are never rmdir()'d, see cgroup_create()'s own comment) --
 * only a fully-removed entry (DELETE'd, or never existed) is a 404.
 */
static void handle_container_stats(int fd, const char *name)
{
	struct registry_entry *e = registry_find(name);
	struct json_writer w;
	long long cpu_usage, cpu_user, cpu_system;
	long long mem_current, mem_peak, mem_max;
	int mem_max_unlimited;
	long long disk_bytes = 0;
	long long io_rbytes, io_wbytes, io_rios, io_wios;
	struct cgroup_pressure cpu_pressure, io_pressure, mem_pressure;
	int i;

	if (e == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}

	cgroup_read_stat_key(e->handle.cgroup_fd, "cpu.stat", "usage_usec", &cpu_usage);
	cgroup_read_stat_key(e->handle.cgroup_fd, "cpu.stat", "user_usec", &cpu_user);
	cgroup_read_stat_key(e->handle.cgroup_fd, "cpu.stat", "system_usec", &cpu_system);
	cgroup_read_single_value(e->handle.cgroup_fd, "memory.current", &mem_current, &mem_max_unlimited);
	cgroup_read_single_value(e->handle.cgroup_fd, "memory.peak", &mem_peak, &mem_max_unlimited);
	cgroup_read_single_value(e->handle.cgroup_fd, "memory.max", &mem_max, &mem_max_unlimited);
	cgroup_read_io_totals(e->handle.cgroup_fd, &io_rbytes, &io_wbytes, &io_rios, &io_wios);
	cgroup_read_pressure(e->handle.cgroup_fd, "cpu.pressure", &cpu_pressure);
	cgroup_read_pressure(e->handle.cgroup_fd, "io.pressure", &io_pressure);
	cgroup_read_pressure(e->handle.cgroup_fd, "memory.pressure", &mem_pressure);

	{
		char container_root[PATH_MAX];
		char upperdir[PATH_MAX];

		container_root_for(e->disk_name, container_root, sizeof(container_root));
		snprintf(upperdir, sizeof(upperdir), "%s/%s/upper", container_root, name);
		overlay_upperdir_size(upperdir, &disk_bytes);
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "cpu");
	jw_obj_open(&w);
	jw_key(&w, "usage_usec");
	jw_int(&w, cpu_usage);
	jw_key(&w, "user_usec");
	jw_int(&w, cpu_user);
	jw_key(&w, "system_usec");
	jw_int(&w, cpu_system);
	jw_key(&w, "pressure");
	jw_obj_open(&w);
	write_pressure_json(&w, &cpu_pressure);
	jw_obj_close(&w);
	jw_obj_close(&w);
	jw_key(&w, "memory");
	jw_obj_open(&w);
	jw_key(&w, "current");
	jw_int(&w, mem_current);
	jw_key(&w, "peak");
	jw_int(&w, mem_peak);
	jw_key(&w, "max");
	if (mem_max_unlimited)
		jw_null(&w);
	else
		jw_int(&w, mem_max);
	jw_key(&w, "pressure");
	jw_obj_open(&w);
	write_pressure_json(&w, &mem_pressure);
	jw_obj_close(&w);
	jw_obj_close(&w);
	jw_key(&w, "disk");
	jw_obj_open(&w);
	jw_key(&w, "upper_bytes");
	jw_int(&w, disk_bytes);
	jw_key(&w, "read_bytes");
	jw_int(&w, io_rbytes);
	jw_key(&w, "write_bytes");
	jw_int(&w, io_wbytes);
	jw_key(&w, "read_ios");
	jw_int(&w, io_rios);
	jw_key(&w, "write_ios");
	jw_int(&w, io_wios);
	jw_key(&w, "pressure");
	jw_obj_open(&w);
	write_pressure_json(&w, &io_pressure);
	jw_obj_close(&w);
	jw_obj_close(&w);
	jw_key(&w, "networks");
	jw_arr_open(&w);
	for (i = 0; i < e->net_count; i++) {
		char veth[32];

		snprintf(veth, sizeof(veth), "vh%d-%d", (int)e->handle.pid, i);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, e->nets[i].name);
		jw_key(&w, "rx_bytes");
		jw_int(&w, read_net_stat(veth, "rx_bytes"));
		jw_key(&w, "tx_bytes");
		jw_int(&w, read_net_stat(veth, "tx_bytes"));
		jw_key(&w, "rx_packets");
		jw_int(&w, read_net_stat(veth, "rx_packets"));
		jw_key(&w, "tx_packets");
		jw_int(&w, read_net_stat(veth, "tx_packets"));
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
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
 *
 * A stop targeting PKG_BUILD_CONTAINER_NAME ("__pkgbuild") is a real,
 * previously-undiscovered special case: this path kills and reaps the
 * container directly via registry_remove() -- it never goes through
 * handle_container_event()'s own epoll-driven exit path (the pidfd is
 * explicitly pulled from epoll first, precisely so this synchronous
 * stop doesn't race a live event for the same fd), which is the ONLY
 * place pkg_build_completed() normally gets called. Without the check
 * below, manually stopping a stuck build left pkg.c's own
 * g_current_job_name lock permanently set (confirmed live: the
 * container was fully gone from the registry, but every subsequent
 * pkg install kept 409-ing "another package install is already in
 * progress" indefinitely) -- __pkgbuild was never expected to be
 * stopped this way before now. Fixed by calling pkg_build_completed()
 * here too, exactly like the normal exit path would have, using the
 * real exit_status registry_remove()'s own registry_mark_exited()
 * call just set on e (read after removal -- the slot is only flagged
 * not-in-use, not freed, so this is the same "read the entry once
 * more before something else can reuse it" pattern
 * handle_container_event() already relies on). A manually-killed
 * build's exit_status is never 0, so pkg_build_completed()'s own
 * success-only chaining logic correctly never triggers here.
 */
static void handle_stop(int fd, const char *name)
{
	struct registry_entry *e = registry_find(name);
	struct conn *cc;
	struct json_writer w;
	int was_pkgbuild = (strcmp(name, PKG_BUILD_CONTAINER_NAME) == 0);

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
		if (was_pkgbuild) {
			pid_t pkg_pid;
			int pkg_pidfd;
			char hostbuild_done_name[PKG_NAME_MAX];

			if (pkg_build_completed(name, e->exit_status, &pkg_pid, &pkg_pidfd,
			                         hostbuild_done_name))
				register_pkg_fetch_pidfd(pkg_pid, pkg_pidfd);
			else
				try_start_queued_pkg_rebuild();
		}
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

static void respond_container_storagemigrate_error(int fd, enum containerstoragemigrate_error err)
{
	switch (err) {
	case CONTAINERSTORAGEMIGRATE_ERR_BUSY:
		respond_error(fd, 409, "Conflict", "a storage migration for this container is already running");
		break;
	case CONTAINERSTORAGEMIGRATE_ERR_ALREADY_ACTIVE:
		respond_error(fd, 409, "Conflict", "this container's storage is already on that placement");
		break;
	case CONTAINERSTORAGEMIGRATE_ERR_TABLE_FULL:
		respond_error(fd, 500, "Internal Server Error",
		              "every container-storage migration job slot is in use -- try again shortly");
		break;
	case CONTAINERSTORAGEMIGRATE_ERR_SPAWN_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "could not start migration job");
		break;
	}
}

/*
 * POST /v1/containers/{name}/migrate-storage (ADR-0142 Section 4): the
 * exact same {"disk": "name"|null} contract every other storage-
 * placement migration endpoint already has, narrowed to one
 * container's own overlay directory. Requires a persisted definition
 * (restart_policy != "no") to exist -- the cutover this triggers
 * always ends with a real stop-then-replay, so a container with
 * nothing to replay from can never safely reach that point. disk_name
 * is validated via resolve_container_disk_root() -- the exact same
 * check POST /v1/containers itself already applies to a "disk" field
 * at creation time, reused rather than re-implemented (One Source of
 * Truth).
 */
static void handle_container_migrate_storage_post(int fd, const char *name, const char *body,
                                                    size_t body_len)
{
	struct json_value *root;
	const struct json_value *jdisk;
	const char *disk_name;
	char disk_name_buf[DISKROLE_DISK_NAME_MAX];
	char current_disk[DISKROLE_DISK_NAME_MAX];
	char source_root[PATH_MAX], target_root[PATH_MAX];
	char source_base[PATH_MAX], target_base[PATH_MAX];
	struct registry_entry *live = registry_find(name);
	struct container_def *def = containerdef_find(name);
	pid_t pid;
	int pidfd;
	enum containerstoragemigrate_error cerr;

	if (live == NULL && def == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}
	if (def == NULL) {
		respond_error(fd, 400, "Bad Request",
		              "this container has no persisted definition to restart from -- only a "
		              "restart_policy other than \"no\" supports storage migration");
		return;
	}

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jdisk = json_object_get(root, "disk");
	if (jdisk == NULL || (jdisk->type != JSON_NULL && jdisk->type != JSON_STRING)) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "disk is required -- a disk name, or null for the default OS-disk placement");
		return;
	}
	if (jdisk->type == JSON_NULL) {
		disk_name = NULL;
	} else {
		disk_name = json_as_string(jdisk);
		if (disk_name == NULL || disk_name[0] == '\0' || strlen(disk_name) >= DISKROLE_DISK_NAME_MAX) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "invalid disk name");
			return;
		}
		snprintf(disk_name_buf, sizeof(disk_name_buf), "%s", disk_name);
		disk_name = disk_name_buf;
	}
	json_free(root);

	current_disk[0] = '\0';
	if (live != NULL) {
		snprintf(current_disk, sizeof(current_disk), "%s", live->disk_name);
	} else {
		struct json_value *defroot = json_parse(def->body, def->body_len);

		if (defroot != NULL) {
			const char *d = json_as_string(json_object_get(defroot, "disk"));

			if (d != NULL)
				snprintf(current_disk, sizeof(current_disk), "%s", d);
			json_free(defroot);
		}
	}

	if (strcmp(current_disk, disk_name != NULL ? disk_name : "") == 0) {
		respond_error(fd, 409, "Conflict", "this container's storage is already on that placement");
		return;
	}

	if (disk_name != NULL) {
		int drc = resolve_container_disk_root(disk_name, target_root, sizeof(target_root));

		if (drc != DISK_RESOLVE_OK) {
			respond_error(fd, 400, "Bad Request",
			              drc == DISK_RESOLVE_NOT_FOUND
			                  ? "no such disk"
			                  : drc == DISK_RESOLVE_NOT_MOUNTED
			                        ? "disk is not mounted"
			                        : "disk has no container-storage role assigned");
			return;
		}
	} else {
		snprintf(target_root, sizeof(target_root), "%s", CONTAINERS_DIR);
	}

	container_root_for(current_disk[0] != '\0' ? current_disk : NULL, source_root, sizeof(source_root));
	snprintf(source_base, sizeof(source_base), "%s/%s", source_root, name);
	snprintf(target_base, sizeof(target_base), "%s/%s", target_root, name);

	if (persist_mkdir_p(target_base) != 0) {
		respond_error(fd, 500, "Internal Server Error", "failed to create target container directory");
		return;
	}

	cerr = containerstoragemigrate_start(name, source_base, target_base, disk_name, &pid, &pidfd);
	if (cerr != CONTAINERSTORAGEMIGRATE_OK) {
		respond_container_storagemigrate_error(fd, cerr);
		return;
	}
	register_container_storage_migrate_pidfd(name, pid, pidfd);

	{
		struct json_writer w;

		jw_init(&w);
		containerstoragemigrate_write_status_json(&w, name);
		respond_json(fd, 202, "Accepted", &w);
		jw_free(&w);
	}
}

static void handle_container_migrate_storage_get(int fd, const char *name)
{
	struct json_writer w;

	if (registry_find(name) == NULL && containerdef_find(name) == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}

	jw_init(&w);
	containerstoragemigrate_write_status_json(&w, name);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_device_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "devices");
	device_write_json_list(&w, CONTAINERS_DIR);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Real host block devices (Phase A, multi-disk management -- see
 * ROADMAP.md's own queue entry). Read-only, live-enumerated exactly
 * like GET /devices above (no persisted state yet -- role assignment
 * is a later phase). CONTAINERS_DIR is passed straight through so
 * disk.c can flag which one disk is the fixed OS disk without needing
 * any daemon-layer state of its own.
 */
static void handle_disk_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "disks");
	disk_write_json_list(&w, CONTAINERS_DIR);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void respond_devicemap_error(int fd, enum devicemap_error err)
{
	switch (err) {
	case DEVICEMAP_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid name");
		break;
	case DEVICEMAP_ERR_INVALID_KIND:
		respond_error(fd, 400, "Bad Request", "kind must be \"exact\" or \"vendor_model\"");
		break;
	case DEVICEMAP_ERR_INVALID_SELECTOR:
		respond_error(fd, 400, "Bad Request", "invalid selector");
		break;
	case DEVICEMAP_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "a device mapping with this name already exists");
		break;
	case DEVICEMAP_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "device mapping table full");
		break;
	case DEVICEMAP_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such device mapping");
		break;
	case DEVICEMAP_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "device mapping operation failed");
		break;
	}
}

static void handle_devicemap_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "devicemaps");
	devicemap_write_json_list(&w, CONTAINERS_DIR);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_devicemap_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name, *kind, *selector;
	enum devicemap_error derr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	name = json_as_string(json_object_get(root, "name"));
	kind = json_as_string(json_object_get(root, "kind"));
	selector = json_as_string(json_object_get(root, "selector"));
	if (name == NULL || kind == NULL || selector == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name, kind, and selector are required");
		return;
	}

	{
		/* name is a pointer into root's own parsed tree -- copied here
		 * since devicemap_write_json_one() below needs it again after
		 * json_free(root) makes the original dangling. */
		char name_buf[DEVICEMAP_NAME_MAX];

		snprintf(name_buf, sizeof(name_buf), "%s", name);
		derr = devicemap_create(name, kind, selector);
		json_free(root);

		if (derr != DEVICEMAP_OK) {
			respond_devicemap_error(fd, derr);
			return;
		}

		{
			struct json_writer w;

			jw_init(&w);
			devicemap_write_json_one(name_buf, CONTAINERS_DIR, &w);
			respond_json(fd, 201, "Created", &w);
			jw_free(&w);
		}
	}
}

static void handle_devicemap_delete(int fd, const char *name)
{
	enum devicemap_error derr = devicemap_delete(name);

	if (derr != DEVICEMAP_OK) {
		respond_devicemap_error(fd, derr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * Multi-disk management Phase B (ROADMAP.md): persisted disk role
 * assignment, the direct follow-up to Phase A's read-only GET /disks.
 * CONTAINERS_DIR is threaded through exactly like handle_disk_list()'s
 * own os_containers_dir param, so diskrole.c can reject assigning a
 * role to the real OS disk without needing any daemon-layer state of
 * its own.
 */
static void respond_diskrole_error(int fd, enum diskrole_error err)
{
	switch (err) {
	case DISKROLE_ERR_INVALID_DISK_NAME:
		respond_error(fd, 400, "Bad Request", "invalid disk_name");
		break;
	case DISKROLE_ERR_INVALID_ROLE:
		respond_error(fd, 400, "Bad Request", "role must be \"container-storage\" or \"backup\"");
		break;
	case DISKROLE_ERR_IS_OS_DISK:
		respond_error(fd, 400, "Bad Request",
		              "this disk holds the fixed OS layout -- it is never a role-assignment candidate");
		break;
	case DISKROLE_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "this disk already has a role assigned");
		break;
	case DISKROLE_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "disk role table full");
		break;
	case DISKROLE_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no role assigned to this disk");
		break;
	case DISKROLE_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "disk role operation failed");
		break;
	}
}

static void handle_diskrole_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "diskroles");
	diskrole_write_json_list(&w, CONTAINERS_DIR);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_diskrole_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *disk_name, *role;
	enum diskrole_error derr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	disk_name = json_as_string(json_object_get(root, "disk_name"));
	role = json_as_string(json_object_get(root, "role"));
	if (disk_name == NULL || role == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "disk_name and role are required");
		return;
	}

	{
		/* disk_name is a pointer into root's own parsed tree --
		 * copied here since diskrole_write_json_one() below needs
		 * it again after json_free(root) makes the original
		 * dangling. */
		char disk_name_buf[DISKROLE_DISK_NAME_MAX];

		snprintf(disk_name_buf, sizeof(disk_name_buf), "%s", disk_name);
		derr = diskrole_create(disk_name, role, CONTAINERS_DIR);
		json_free(root);

		if (derr != DISKROLE_OK) {
			respond_diskrole_error(fd, derr);
			return;
		}

		{
			struct json_writer w;

			jw_init(&w);
			diskrole_write_json_one(disk_name_buf, &w, CONTAINERS_DIR);
			respond_json(fd, 201, "Created", &w);
			jw_free(&w);
		}
	}
}

/*
 * ADR-0141: true if disk_name is the *currently active* placement for
 * any of the three daemon-wide storage singletons (state-storage/
 * log-storage/rebuildable-storage), OR the currently configured
 * backup-config disk -- removing the role out from under an in-use
 * placement, or destroying it via format, would leave the daemon's own
 * live-location tracking (or, for backup, the operator's own configured
 * disaster-recovery target) pointing at a disk that, per its own role
 * table, doesn't do that anymore. Shared by handle_diskrole_delete()
 * and handle_disk_format_post() below. Named for the three storage
 * kinds since they came first, but backup-config's own disk is checked
 * here too -- ADR-0141's own safety-check section lists it explicitly
 * alongside the three, not as an afterthought.
 */
static int is_active_storage_singleton_placement(const char *disk_name)
{
	const char *state_disk = storageplacement_get(STORAGE_KIND_STATE);
	const char *log_disk = storageplacement_get(STORAGE_KIND_LOG);
	const char *rebuildable_disk = storageplacement_get(STORAGE_KIND_REBUILDABLE);
	const char *backup_disk = backupconfig_disk();

	return (state_disk != NULL && strcmp(state_disk, disk_name) == 0) ||
	       (log_disk != NULL && strcmp(log_disk, disk_name) == 0) ||
	       (rebuildable_disk != NULL && strcmp(rebuildable_disk, disk_name) == 0) ||
	       (backup_disk != NULL && strcmp(backup_disk, disk_name) == 0);
}

/*
 * ADR-0142 Section 4's own natural extension of the safety check above:
 * a container-storage-role disk that one or more real containers are
 * actually placed on (via POST /containers' own original "disk" field,
 * ADR-0102 -- predating this ADR entirely, migrate-storage is simply
 * the first thing that ever needed this check to exist) is exactly as
 * unsafe to pull the role out from under, or reformat, as any of the
 * four singleton placements above -- doing so would silently orphan or
 * destroy that container's own live workload data. Checks every live
 * registry entry (e->disk_name) and every persisted definition
 * currently between a crash and its next restart (re-parsed from its
 * own stored body's "disk" field, the same fallback DELETE's own
 * crashed-container cleanup already uses) -- a container can be using
 * a disk in either state.
 */
static int disk_has_container_in_use(const char *disk_name)
{
	char names[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	int count = containerdef_resolve_order(names);
	int i;

	for (i = 0; i < count; i++) {
		struct registry_entry *live = registry_find(names[i]);

		if (live != NULL) {
			if (live->disk_name[0] != '\0' && strcmp(live->disk_name, disk_name) == 0)
				return 1;
			continue;
		}
		{
			struct container_def *def = containerdef_find(names[i]);
			struct json_value *defroot;

			if (def == NULL)
				continue;
			defroot = json_parse(def->body, def->body_len);
			if (defroot != NULL) {
				const char *d = json_as_string(json_object_get(defroot, "disk"));
				int match = (d != NULL && strcmp(d, disk_name) == 0);

				json_free(defroot);
				if (match)
					return 1;
			}
		}
	}
	return 0;
}

static void handle_diskrole_delete(int fd, const char *disk_name)
{
	enum diskrole_error derr;

	if (is_active_storage_singleton_placement(disk_name)) {
		respond_error(fd, 409, "Conflict",
		              "this disk is the active state-storage, log-storage, or rebuildable-storage "
		              "placement, or the configured backup-config disk -- migrate away first "
		              "(POST .../migrate to a different disk or disk: null, or PUT "
		              "/v1/system/backup-config with a different disk/null) before removing its role");
		return;
	}
	if (disk_has_container_in_use(disk_name)) {
		respond_error(fd, 409, "Conflict",
		              "one or more containers currently have their own storage on this disk -- "
		              "migrate them away first (POST /containers/{name}/migrate-storage) before "
		              "removing its role");
		return;
	}
	derr = diskrole_delete(disk_name);
	if (derr != DISKROLE_OK) {
		respond_diskrole_error(fd, derr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * Multi-disk management Phase C (ROADMAP.md): format + mount an
 * already-role-assigned disk. Deliberately a SEPARATE, explicit action
 * from Phase B's role assignment above (confirmed with the user) --
 * assigning a role never has a destructive side effect of its own, and
 * this endpoint requires the operator to name the exact target disk
 * again in the request body (confirm_disk_name, matching the URL's own
 * disk name) as a deliberate double-confirmation before anything
 * irreversible happens. See diskformat.h for the actual job mechanism.
 */
static void respond_diskformat_error(int fd, enum diskformat_error err)
{
	switch (err) {
	case DISKFORMAT_ERR_INVALID_DISK_NAME:
		respond_error(fd, 400, "Bad Request", "invalid disk name");
		break;
	case DISKFORMAT_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such disk");
		break;
	case DISKFORMAT_ERR_IS_OS_DISK:
		respond_error(fd, 400, "Bad Request",
		              "this disk holds the fixed OS layout -- it can never be formatted");
		break;
	case DISKFORMAT_ERR_NO_ROLE:
		respond_error(fd, 400, "Bad Request",
		              "this disk has no assigned role -- assign one via POST /v1/diskroles first");
		break;
	case DISKFORMAT_ERR_BUSY:
		respond_error(fd, 409, "Conflict", "a format job is already running");
		break;
	case DISKFORMAT_ERR_MKDIR_FAILED:
		respond_error(fd, 500, "Internal Server Error", "could not create mount point");
		break;
	case DISKFORMAT_ERR_SPAWN_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "could not start format job");
		break;
	}
}

static void handle_disk_format_post(int fd, const char *disk_name, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *confirm;
	const char *fs_type_str;
	enum diskformat_fs_type fs_type;
	pid_t pid;
	int pidfd;
	enum diskformat_error derr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	confirm = json_as_string(json_object_get(root, "confirm_disk_name"));
	if (confirm == NULL || strcmp(confirm, disk_name) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "confirm_disk_name must be given and must match the disk name in the URL -- "
		              "this is a destructive operation");
		return;
	}
	if (is_active_storage_singleton_placement(disk_name)) {
		json_free(root);
		respond_error(fd, 409, "Conflict",
		              "this disk is the active state-storage, log-storage, or rebuildable-storage placement, or the configured backup-config disk -- "
		              "formatting it would destroy live data; migrate away first");
		return;
	}
	if (disk_has_container_in_use(disk_name)) {
		json_free(root);
		respond_error(fd, 409, "Conflict",
		              "one or more containers currently have their own storage on this disk -- "
		              "formatting it would destroy live data; migrate them away first "
		              "(POST /containers/{name}/migrate-storage)");
		return;
	}
	/*
	 * fs_type (ADR-0104, task #732): optional, defaults to "ext4" --
	 * every pre-existing request body (no fs_type field at all) keeps
	 * its exact prior behavior. "btrfs" requires a real mkfs.btrfs to
	 * actually be staged on this box (btrfs-progs.recipe via a real
	 * kanxeo-hosttools image, ADR-0103's own scope note) -- if it
	 * isn't, the job still starts (this daemon has no cheap way to
	 * probe for the binary's presence without also handling every
	 * other reason execve() could fail the same way) but fails fast
	 * with a clear DISKFORMAT_STATE_FAILED/"mkfs.btrfs failed" once
	 * the child actually tries to exec it and gets ENOENT -- consistent
	 * with mkfs.ext4's own existing failure-reporting shape, not a
	 * new failure mode this field introduces.
	 */
	fs_type_str = json_as_string(json_object_get(root, "fs_type"));
	if (fs_type_str == NULL || strcmp(fs_type_str, "ext4") == 0) {
		fs_type = DISKFORMAT_FS_EXT4;
	} else if (strcmp(fs_type_str, "btrfs") == 0) {
		fs_type = DISKFORMAT_FS_BTRFS;
	} else {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "fs_type must be \"ext4\" or \"btrfs\"");
		return;
	}
	json_free(root);

	derr = diskformat_start(disk_name, CONTAINERS_DIR, DISKS_MOUNT_DIR, fs_type, &pid, &pidfd);
	if (derr != DISKFORMAT_OK) {
		respond_diskformat_error(fd, derr);
		return;
	}
	register_disk_format_pidfd(pid, pidfd);

	{
		struct json_writer w;

		jw_init(&w);
		diskformat_write_status_json(&w, disk_name);
		respond_json(fd, 202, "Accepted", &w);
		jw_free(&w);
	}
}

static void handle_disk_format_get(int fd, const char *disk_name)
{
	struct json_writer w;

	jw_init(&w);
	diskformat_write_status_json(&w, disk_name);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * ADR-0141 Phase 2/3: GET/POST /v1/system/{state,log}-storage(/migrate).
 * rebuildable-storage reuses the identical storagemigrate.c/
 * storageplacement.c machinery, just not exposed here yet (Phase 4).
 */
static const char *storage_kind_label(enum storage_kind kind)
{
	switch (kind) {
	case STORAGE_KIND_STATE:
		return "state-storage";
	case STORAGE_KIND_REBUILDABLE:
		return "rebuildable-storage";
	case STORAGE_KIND_LOG:
		return "log-storage";
	default:
		return "state-storage";
	}
}

static void handle_state_storage_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	storageplacement_write_json(&w, STORAGE_KIND_STATE);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_log_storage_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	storageplacement_write_json(&w, STORAGE_KIND_LOG);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void respond_storagemigrate_error(int fd, enum storage_kind kind, enum storagemigrate_error err)
{
	char msg[192];

	switch (err) {
	case STORAGEMIGRATE_ERR_BUSY:
		snprintf(msg, sizeof(msg), "a %s migration is already running", storage_kind_label(kind));
		respond_error(fd, 409, "Conflict", msg);
		break;
	case STORAGEMIGRATE_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such disk");
		break;
	case STORAGEMIGRATE_ERR_IS_OS_DISK:
		respond_error(fd, 400, "Bad Request",
		              "this disk holds the fixed OS layout -- it can never be a placement target");
		break;
	case STORAGEMIGRATE_ERR_WRONG_ROLE:
		snprintf(msg, sizeof(msg),
		         "this disk does not carry the %s role -- assign it via POST /v1/diskroles first",
		         storage_kind_label(kind));
		respond_error(fd, 400, "Bad Request", msg);
		break;
	case STORAGEMIGRATE_ERR_NOT_MOUNTED:
		respond_error(fd, 400, "Bad Request", "this disk carries the role but isn't currently mounted");
		break;
	case STORAGEMIGRATE_ERR_ALREADY_ACTIVE:
		snprintf(msg, sizeof(msg), "this is already the active %s placement", storage_kind_label(kind));
		respond_error(fd, 409, "Conflict", msg);
		break;
	case STORAGEMIGRATE_ERR_SPAWN_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "could not start migration job");
		break;
	}
}

static void handle_state_storage_migrate_post(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jdisk;
	const char *disk_name;
	char disk_name_buf[DISKROLE_DISK_NAME_MAX];
	char target_dir[PATH_MAX];
	pid_t pid;
	int pidfd;
	enum storagemigrate_error merr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jdisk = json_object_get(root, "disk");
	if (jdisk == NULL || (jdisk->type != JSON_NULL && jdisk->type != JSON_STRING)) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "disk is required -- a disk name, or null for the default OS-disk placement");
		return;
	}
	if (jdisk->type == JSON_NULL) {
		disk_name = NULL;
		snprintf(target_dir, sizeof(target_dir), "%s/state", g_base_dir);
	} else {
		disk_name = json_as_string(jdisk);
		if (disk_name == NULL || disk_name[0] == '\0' ||
		    strlen(disk_name) >= DISKROLE_DISK_NAME_MAX) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "invalid disk name");
			return;
		}
		snprintf(disk_name_buf, sizeof(disk_name_buf), "%s", disk_name);
		disk_name = disk_name_buf;
		snprintf(target_dir, sizeof(target_dir), "%s/%s/state", DISKS_MOUNT_DIR, disk_name);
	}
	json_free(root);

	merr = storagemigrate_start(STORAGE_KIND_STATE, STATE_DIR, target_dir, disk_name, CONTAINERS_DIR,
	                             &pid, &pidfd);
	if (merr != STORAGEMIGRATE_OK) {
		respond_storagemigrate_error(fd, STORAGE_KIND_STATE, merr);
		return;
	}
	register_storage_migrate_pidfd(STORAGE_KIND_STATE, pid, pidfd);

	{
		struct json_writer w;

		jw_init(&w);
		storagemigrate_write_status_json(&w, STORAGE_KIND_STATE);
		respond_json(fd, 202, "Accepted", &w);
		jw_free(&w);
	}
}

static void handle_log_storage_migrate_post(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jdisk;
	const char *disk_name;
	char disk_name_buf[DISKROLE_DISK_NAME_MAX];
	char target_dir[PATH_MAX];
	pid_t pid;
	int pidfd;
	enum storagemigrate_error merr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jdisk = json_object_get(root, "disk");
	if (jdisk == NULL || (jdisk->type != JSON_NULL && jdisk->type != JSON_STRING)) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "disk is required -- a disk name, or null for the default OS-disk placement");
		return;
	}
	if (jdisk->type == JSON_NULL) {
		disk_name = NULL;
		snprintf(target_dir, sizeof(target_dir), "%s/logs", g_base_dir);
	} else {
		disk_name = json_as_string(jdisk);
		if (disk_name == NULL || disk_name[0] == '\0' ||
		    strlen(disk_name) >= DISKROLE_DISK_NAME_MAX) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "invalid disk name");
			return;
		}
		snprintf(disk_name_buf, sizeof(disk_name_buf), "%s", disk_name);
		disk_name = disk_name_buf;
		snprintf(target_dir, sizeof(target_dir), "%s/%s/logs", DISKS_MOUNT_DIR, disk_name);
	}
	json_free(root);

	merr = storagemigrate_start(STORAGE_KIND_LOG, LOG_DIR, target_dir, disk_name, CONTAINERS_DIR,
	                             &pid, &pidfd);
	if (merr != STORAGEMIGRATE_OK) {
		respond_storagemigrate_error(fd, STORAGE_KIND_LOG, merr);
		return;
	}
	register_storage_migrate_pidfd(STORAGE_KIND_LOG, pid, pidfd);

	{
		struct json_writer w;

		jw_init(&w);
		storagemigrate_write_status_json(&w, STORAGE_KIND_LOG);
		respond_json(fd, 202, "Accepted", &w);
		jw_free(&w);
	}
}

static void handle_log_storage_migrate_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	storagemigrate_write_status_json(&w, STORAGE_KIND_LOG);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_state_storage_migrate_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	storagemigrate_write_status_json(&w, STORAGE_KIND_STATE);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_rebuildable_storage_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	storageplacement_write_json(&w, STORAGE_KIND_REBUILDABLE);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_rebuildable_storage_migrate_post(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jdisk;
	const char *disk_name;
	char disk_name_buf[DISKROLE_DISK_NAME_MAX];
	char target_dir[PATH_MAX];
	pid_t pid;
	int pidfd;
	enum storagemigrate_error merr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jdisk = json_object_get(root, "disk");
	if (jdisk == NULL || (jdisk->type != JSON_NULL && jdisk->type != JSON_STRING)) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "disk is required -- a disk name, or null for the default OS-disk placement");
		return;
	}
	if (jdisk->type == JSON_NULL) {
		disk_name = NULL;
		snprintf(target_dir, sizeof(target_dir), "%s/rebuildable", g_base_dir);
	} else {
		disk_name = json_as_string(jdisk);
		if (disk_name == NULL || disk_name[0] == '\0' ||
		    strlen(disk_name) >= DISKROLE_DISK_NAME_MAX) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "invalid disk name");
			return;
		}
		snprintf(disk_name_buf, sizeof(disk_name_buf), "%s", disk_name);
		disk_name = disk_name_buf;
		snprintf(target_dir, sizeof(target_dir), "%s/%s/rebuildable", DISKS_MOUNT_DIR, disk_name);
	}
	json_free(root);

	merr = storagemigrate_start(STORAGE_KIND_REBUILDABLE, REBUILDABLE_DIR, target_dir, disk_name,
	                             CONTAINERS_DIR, &pid, &pidfd);
	if (merr != STORAGEMIGRATE_OK) {
		respond_storagemigrate_error(fd, STORAGE_KIND_REBUILDABLE, merr);
		return;
	}
	register_storage_migrate_pidfd(STORAGE_KIND_REBUILDABLE, pid, pidfd);

	{
		struct json_writer w;

		jw_init(&w);
		storagemigrate_write_status_json(&w, STORAGE_KIND_REBUILDABLE);
		respond_json(fd, 202, "Accepted", &w);
		jw_free(&w);
	}
}

static void handle_rebuildable_storage_migrate_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	storagemigrate_write_status_json(&w, STORAGE_KIND_REBUILDABLE);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_network_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name, *subnet, *address;
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
	address = json_as_string(json_object_get(root, "address")); /* optional; NULL = no address */

	if (name == NULL || subnet == NULL || jprefix == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name/subnet/prefix_len missing");
		return;
	}
	prefix_len = (int)json_as_number(jprefix);

	nerr = network_create(name, subnet, prefix_len, address, &net);
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
	case IMAGE_ERR_INVALID_PACKAGE:
		respond_error(fd, 400, "Bad Request", "invalid package name");
		break;
	case IMAGE_ERR_INVALID_VERSION:
		respond_error(fd, 400, "Bad Request", "version missing, empty, or too long");
		break;
	case IMAGE_ERR_MANIFEST_FULL:
		respond_error(fd, 500, "Internal Server Error", "image manifest is full");
		break;
	case IMAGE_ERR_CREATE_FAILED:
	case IMAGE_ERR_DELETE_FAILED:
	case IMAGE_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "image operation failed");
		break;
	}
}

/*
 * Reopens a just-closed top-level JSON object so a caller can splice
 * in one more sibling key after the fact (used to add "manifest" onto
 * image_write_json_one()'s own already-closed {"name":...} without
 * teaching that function itself about manifests -- see its own two
 * call sites' comments for why). w must have had exactly one
 * jw_obj_close() as its very last write; the "}" it wrote is dropped
 * and the object's own comma-tracking frame is restored so a
 * subsequent jw_key()/jw_obj_close() behaves exactly as if the object
 * had never been closed.
 */
static void jw_reopen_object(struct json_writer *w)
{
	w->len--;
	w->depth++;
}

/*
 * ADR-0107/task #721: splices "current_version" (a bare string, "" if
 * somehow unresolvable -- IMAGE_ERR_NO_CURRENT_VERSION is unreachable
 * for anything created via image_create(), which always produces one,
 * but this reports rather than assumes) and "versions" (newest-first,
 * each {version, created_at} -- image_version_history_write_json())
 * onto an already-open image JSON object. Shared by handle_image_create()'s
 * 201 and handle_image_get_one()'s 200 so the two responses report the
 * exact same shape rather than one silently lagging the other.
 */
static void write_image_version_fields(const char *name, struct json_writer *w)
{
	char current_version[IMAGE_VERSION_MAX];

	jw_key(w, "current_version");
	if (image_current_version(name, current_version, sizeof(current_version)) == IMAGE_OK)
		jw_str(w, current_version);
	else
		jw_str(w, "");
	jw_key(w, "versions");
	image_version_history_write_json(name, w);
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
	jw_reopen_object(&w);
	jw_key(&w, "manifest");
	jw_arr_open(&w); /* a just-created image never has a manifest.json yet -- always empty */
	jw_arr_close(&w);
	/*
	 * name still points into root's own parsed tree -- json_free(root)
	 * must not run until every use of name is done (a real
	 * use-after-free was caught live here: write_image_version_fields()
	 * used to run AFTER json_free(root), the same class of bug already
	 * fixed once before for a different handler, task #713).
	 */
	write_image_version_fields(name, &w);
	json_free(root);
	jw_obj_close(&w);
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

/*
 * image_write_json_one() writes a bare {"name":...} object; the
 * manifest (ADR-0107) is spliced in as a sibling "manifest" key by
 * re-opening the object rather than by teaching image_write_json_one()
 * itself about manifests -- keeps that function's own contract (pure
 * filesystem-state report) unchanged for its other caller
 * (handle_image_create()'s 201 response, which has no manifest to
 * report for a just-created, empty-manifest image either way).
 */
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
	jw_reopen_object(&w);
	jw_key(&w, "manifest");
	ierr = image_manifest_write_json(name, &w);
	if (ierr != IMAGE_OK) {
		jw_free(&w);
		respond_image_error(fd, ierr);
		return;
	}
	write_image_version_fields(name, &w);
	jw_obj_close(&w);
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

/*
 * POST /v1/images/{name}/manifest (ADR-0107): upserts one {package,
 * mode, version} entry into name's own manifest -- operator-declared
 * package intent, independent of whatever the image's rootfs currently
 * contains. Does not itself trigger a rebuild (task #720's own job).
 */
static void handle_image_manifest_set(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *package;
	const char *mode_str;
	const char *version;
	enum image_pkg_mode mode;
	enum image_error ierr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	package = json_as_string(json_object_get(root, "package"));
	mode_str = json_as_string(json_object_get(root, "mode"));
	version = json_as_string(json_object_get(root, "version"));
	if (package == NULL || mode_str == NULL || version == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "package, mode, and version are all required");
		return;
	}
	if (strcmp(mode_str, "pinned") == 0) {
		mode = IMAGE_PKG_PINNED;
	} else if (strcmp(mode_str, "rolling") == 0) {
		mode = IMAGE_PKG_ROLLING;
	} else {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "mode must be \"pinned\" or \"rolling\"");
		return;
	}

	ierr = image_manifest_set(name, package, mode, version);
	json_free(root);
	if (ierr != IMAGE_OK) {
		respond_image_error(fd, ierr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/* DELETE /v1/images/{name}/manifest/{package} (ADR-0107). */
static void handle_image_manifest_unset(int fd, const char *name, const char *package)
{
	enum image_error ierr = image_manifest_unset(name, package);

	if (ierr != IMAGE_OK) {
		respond_image_error(fd, ierr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * ADR-0123: image recipes (Part 4 of the pkg/ redesign) share pkg_error
 * with package recipes, but respond_pkg_recipe_error()'s own
 * PKG_ERR_INVALID_RECIPE wording names pkg_name= (package-recipe-
 * specific) -- a real, misleading-message gap for an image recipe,
 * same reasoning respond_pkg_recipe_error()'s own doc comment already
 * gives for why it exists distinct from respond_pkg_error().
 */
static void respond_image_recipe_error(int fd, enum pkg_error err)
{
	switch (err) {
	case PKG_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid image recipe name");
		break;
	case PKG_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such image recipe");
		break;
	case PKG_ERR_INVALID_RECIPE:
		respond_error(fd, 400, "Bad Request",
		              "recipe content failed to parse -- image_packages= is required, each "
		              "entry \"name:pinned|rolling:version\"");
		break;
	case PKG_ERR_BUSY:
		respond_error(fd, 409, "Conflict",
		              "another package install/hostbuild/image-recipe-apply is already in progress");
		break;
	case PKG_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "image recipe operation failed");
		break;
	}
}

static void handle_image_recipe_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "recipes");
	image_recipe_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_image_recipe_add(int fd, const char *body, size_t body_len)
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

	perr = image_recipe_add(name, content);
	json_free(root);
	if (perr != PKG_OK) {
		respond_image_recipe_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_image_recipe_get(int fd, const char *name)
{
	char *content;
	size_t content_len;
	enum pkg_error perr;
	struct json_writer w;

	perr = image_recipe_get(name, &content, &content_len);
	if (perr != PKG_OK) {
		respond_image_recipe_error(fd, perr);
		return;
	}
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "content");
	jw_str(&w, content);
	jw_obj_close(&w);
	free(content);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_image_recipe_delete(int fd, const char *name)
{
	enum pkg_error perr = image_recipe_rm(name);

	if (perr != PKG_OK) {
		respond_image_recipe_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * POST /v1/images/{name}/apply-recipe -- 204 for the common, synchronous
 * bulk-declare case (async == 0, see pkg_image_recipe_apply_start()'s
 * own doc comment); 202 + a registered pidfd for the async artifact-
 * fetch fast path, poll GET /v1/images/recipe-apply-status for the
 * outcome, exactly the same "202, poll a status endpoint" convention
 * every other async pkg.c job here already has.
 */
static void handle_image_recipe_apply(int fd, const char *name)
{
	int async = 0;
	pid_t pid;
	int pidfd;
	enum pkg_error perr;
	struct json_writer w;

	perr = pkg_image_recipe_apply_start(name, &async, &pid, &pidfd);
	if (perr != PKG_OK) {
		respond_image_recipe_error(fd, perr);
		return;
	}
	if (!async) {
		http_set_blocking(fd);
		http_write_response(fd, 204, "No Content", "application/json", "", 0);
		return;
	}
	register_image_recipe_fetch_pidfd(pid, pidfd);
	jw_init(&w);
	pkg_image_recipe_apply_write_json_status(&w);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}

static void handle_image_recipe_apply_status_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	pkg_image_recipe_apply_write_json_status(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Container recipes (ADR-0151) -- same shape as the image-recipe
 * handlers immediately above, minus the async artifact-fetch path
 * (containers create synchronously, always).
 */
static void respond_container_recipe_error(int fd, enum pkg_error err)
{
	switch (err) {
	case PKG_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid container recipe name");
		break;
	case PKG_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such container recipe");
		break;
	case PKG_ERR_INVALID_RECIPE:
		respond_error(fd, 400, "Bad Request",
		              "recipe content failed to parse -- must be valid JSON matching a POST "
		              "/v1/containers body, including a \"name\" field equal to the recipe's "
		              "own name");
		break;
	case PKG_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "container recipe operation failed");
		break;
	}
}

static void handle_container_recipe_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "recipes");
	container_recipe_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_container_recipe_add(int fd, const char *body, size_t body_len)
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

	perr = container_recipe_add(name, content);
	json_free(root);
	if (perr != PKG_OK) {
		respond_container_recipe_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_container_recipe_get(int fd, const char *name)
{
	char *content;
	size_t content_len;
	enum pkg_error perr;
	struct json_writer w;

	perr = container_recipe_get(name, &content, &content_len);
	if (perr != PKG_OK) {
		respond_container_recipe_error(fd, perr);
		return;
	}
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "content");
	jw_str(&w, content);
	jw_obj_close(&w);
	free(content);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_container_recipe_delete(int fd, const char *name)
{
	enum pkg_error perr = container_recipe_rm(name);

	if (perr != PKG_OK) {
		respond_container_recipe_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * POST /v1/containers/recipes/{name}/apply -- renders the recipe
 * (substituting any {{SECRET:KEY}} tokens from the request body's own
 * "secrets" object) and hands the result to handle_create()'s own
 * body-handling path unchanged -- a container recipe apply IS a
 * container create, just sourced from a stored, git-syncable template
 * instead of an inline request body. handle_create() itself responds
 * (201 + the new container, or whatever error create_container_from_
 * body() surfaces) -- nothing left to do here afterward.
 */
static void handle_container_recipe_apply(int fd, const char *name, const char *body,
                                           size_t body_len)
{
	struct json_value *root = NULL;
	const struct json_value *secrets = NULL;
	char *rendered;
	enum pkg_error perr;

	if (body != NULL && body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		secrets = json_object_get(root, "secrets");
	}

	rendered = container_recipe_render(name, secrets, &perr);
	if (root != NULL)
		json_free(root);
	if (rendered == NULL) {
		respond_container_recipe_error(fd, perr);
		return;
	}

	handle_create(fd, rendered, strlen(rendered));
	free(rendered);
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
	char qualified_name[DNS_NAME_MAX];
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

	/* ADR-0052: a bare label (no dot) gets this site's default suffix
	 * appended server-side -- a name that already contains a dot is
	 * left exactly as typed, no daemon-side override, ever. */
	siteconfig_qualify(name, qualified_name, sizeof(qualified_name));

	derr = dns_record_create(qualified_name, addr.s_addr, NULL, &rec);
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
	char qualified_name[DNS_NAME_MAX];
	struct dns_record *rec;
	struct json_writer w;

	/* task #760: dns_record_find() matches the record's own stored,
	 * already-qualified name exactly (a plain strcmp() -- see dns.c) --
	 * handle_dns_record_create() has always qualified a bare label
	 * before storing one (ADR-0052), but this GET (and, until this
	 * fix, PUT/DELETE below) never did the same on the read side,
	 * so a record created with a bare --name=foo could only ever be
	 * looked back up by its full site-qualified FQDN, not the same
	 * bare name a caller just used to create it. Found live during
	 * the task #760 sweep. siteconfig_qualify() is a safe no-op on an
	 * already-qualified name (any label containing a '.' passes
	 * through unchanged), so this is correct for both a bare label
	 * and an already-fully-qualified name. */
	siteconfig_qualify(name, qualified_name, sizeof(qualified_name));
	rec = dns_record_find(qualified_name);

	if (rec == NULL) {
		respond_error(fd, 404, "Not Found", "no such DNS record");
		return;
	}
	jw_init(&w);
	dns_write_json_one(rec, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_dns_record_update(int fd, const char *name, const char *body, size_t body_len)
{
	char qualified_name[DNS_NAME_MAX];
	struct json_value *root;
	const char *ip;
	struct in_addr addr;
	struct dns_record *rec;
	enum dns_error derr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	ip = json_as_string(json_object_get(root, "ip"));
	if (ip == NULL || inet_pton(AF_INET, ip, &addr) != 1) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "ip missing or invalid");
		return;
	}
	json_free(root);

	/* task #760: see handle_dns_record_get_one()'s own comment above --
	 * same qualify-before-lookup fix, so a record created with a bare
	 * --name= can be updated back with that same bare name. */
	siteconfig_qualify(name, qualified_name, sizeof(qualified_name));
	derr = dns_record_update(qualified_name, addr.s_addr, &rec);
	if (derr != DNS_OK) {
		respond_dns_error(fd, derr);
		return;
	}

	jw_init(&w);
	dns_write_json_one(rec, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_dns_record_delete(int fd, const char *name)
{
	char qualified_name[DNS_NAME_MAX];
	enum dns_error derr;

	/* task #760: see handle_dns_record_get_one()'s own comment above --
	 * same qualify-before-lookup fix, so a record created with a bare
	 * --name= can be deleted back with that same bare name. */
	siteconfig_qualify(name, qualified_name, sizeof(qualified_name));
	derr = dns_record_delete(qualified_name);

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

static void respond_ldap_server_error(int fd, enum ldap_server_error err)
{
	switch (err) {
	case LDAP_SERVER_ERR_INVALID_PATH:
		respond_error(fd, 400, "Bad Request", "invalid config_path");
		break;
	case LDAP_SERVER_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "this container is already registered");
		break;
	case LDAP_SERVER_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "LDAP server binding table full");
		break;
	case LDAP_SERVER_ERR_PERSIST_FAILED:
		respond_error(fd, 500, "Internal Server Error", "failed to persist LDAP server binding");
		break;
	case LDAP_SERVER_ERR_NOT_FOUND:
	default:
		respond_error(fd, 404, "Not Found", "no such LDAP server binding");
		break;
	}
}

/*
 * Mirrors handle_dns_server_create()/handle_dns_server_list()/
 * handle_dns_server_delete() exactly (task #725) -- same REST shape,
 * same "container must already exist and be running" validation.
 * Unlike DNS, registration itself never writes to the container's
 * filesystem (see ldap.h's own header comment for why) -- task #726's
 * CRUD layer resolves /proc/<pid>/root/<config_path> fresh at write
 * time instead.
 */
static void handle_ldap_server_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *container_name, *config_path;
	struct registry_entry *entry;
	enum ldap_server_error serr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	container_name = json_as_string(json_object_get(root, "container"));
	config_path = json_as_string(json_object_get(root, "config_path"));

	if (container_name == NULL || config_path == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "container/config_path missing");
		return;
	}

	entry = registry_find(container_name);
	if (entry == NULL || !entry->running) {
		json_free(root);
		respond_error(fd, 404, "Not Found", "no such running container");
		return;
	}

	serr = ldap_server_register(container_name, config_path);

	if (serr != LDAP_SERVER_OK) {
		json_free(root);
		respond_ldap_server_error(fd, serr);
		return;
	}

	/* Task #726: populate the freshly-registered server's own config
	 * file from Kanxeo's own record store, the dns_server_sync_all()
	 * analog -- a fresh/replacement glauth instance starts current
	 * instead of empty. */
	ldap_record_sync_all();

	/* container_name/config_path still point into root -- build the
	 * response before freeing it (see handle_dns_server_create()'s own
	 * identical comment). */
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container_name);
	jw_key(&w, "config_path");
	jw_str(&w, config_path);
	jw_obj_close(&w);
	json_free(root);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_ldap_server_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "servers");
	ldap_server_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_ldap_server_delete(int fd, const char *name)
{
	enum ldap_server_error serr = ldap_server_unregister(name);

	if (serr != LDAP_SERVER_OK) {
		respond_ldap_server_error(fd, serr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/* ---- LDAP uid/gid allocation config (task #748) ---- */

static void handle_ldap_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	ldap_config_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_ldap_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	int start_uid, start_gid;
	enum ldap_record_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	start_uid = (int)json_as_number(json_object_get(root, "start_uid"));
	start_gid = (int)json_as_number(json_object_get(root, "start_gid"));
	json_free(root);

	rerr = ldap_config_set(start_uid, start_gid);
	if (rerr != LDAP_RECORD_OK) {
		respond_error(fd, 400, "Bad Request", "start_uid/start_gid must both be > 0");
		return;
	}

	jw_init(&w);
	ldap_config_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* ---- LDAP user/group CRUD (task #726) ---- */

static void respond_ldap_record_error(int fd, enum ldap_record_error err)
{
	switch (err) {
	case LDAP_RECORD_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid name");
		break;
	case LDAP_RECORD_ERR_INVALID_FIELD:
		respond_error(fd, 400, "Bad Request", "invalid uidnumber/gidnumber");
		break;
	case LDAP_RECORD_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "already exists");
		break;
	case LDAP_RECORD_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "LDAP record table full");
		break;
	case LDAP_RECORD_ERR_GROUP_NOT_FOUND:
		respond_error(fd, 400, "Bad Request",
		              "primarygroup or a secondary_groups entry does not name an existing group");
		break;
	case LDAP_RECORD_ERR_PERSIST_FAILED:
		respond_error(fd, 500, "Internal Server Error", "failed to persist LDAP record");
		break;
	case LDAP_RECORD_ERR_NOT_FOUND:
	default:
		respond_error(fd, 404, "Not Found", "no such LDAP user/group");
		break;
	}
}

static void handle_ldap_group_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name;
	const struct json_value *jgidnumber;
	int gidnumber;
	struct ldap_group *g;
	enum ldap_record_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	name = json_as_string(json_object_get(root, "name"));
	jgidnumber = json_object_get(root, "gidnumber");
	/* Omitted (task #748): auto-allocate from the configurable
	 * start_gid pool (GET/PUT /v1/ldap/config), same shape the
	 * container-creation LDAP hook already uses for uidnumber. */
	gidnumber = jgidnumber != NULL ? (int)json_as_number(jgidnumber) : ldap_gid_alloc();

	if (name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name missing");
		return;
	}

	rerr = ldap_group_create(name, gidnumber, &g);
	json_free(root); /* g points into ldap.c's own record store, not root -- safe past here */
	if (rerr != LDAP_RECORD_OK) {
		respond_ldap_record_error(fd, rerr);
		return;
	}

	jw_init(&w);
	ldap_group_write_json_one(g, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_ldap_group_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "groups");
	ldap_group_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_ldap_group_get_one(int fd, const char *name)
{
	struct ldap_group *g = ldap_group_find(name);
	struct json_writer w;

	if (g == NULL) {
		respond_error(fd, 404, "Not Found", "no such LDAP group");
		return;
	}
	jw_init(&w);
	ldap_group_write_json_one(g, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_ldap_group_update(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jgidnumber, *jname;
	int gidnumber;
	const char *new_name;
	struct ldap_group *g;
	enum ldap_record_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	jgidnumber = json_object_get(root, "gidnumber");
	gidnumber = jgidnumber != NULL ? (int)json_as_number(jgidnumber) : 0;
	/* ADR-0147: an explicit body "name" differing from the URL path's
	 * own {name} renames the group; omitted or identical means no
	 * rename, matching every other PUT field's own "give it to change
	 * it" convention here. */
	jname = json_object_get(root, "name");
	new_name = jname != NULL ? json_as_string(jname) : NULL;

	rerr = ldap_group_update(name, new_name, gidnumber, &g);
	json_free(root);
	if (rerr != LDAP_RECORD_OK) {
		respond_ldap_record_error(fd, rerr);
		return;
	}

	jw_init(&w);
	ldap_group_write_json_one(g, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_ldap_group_delete(int fd, const char *name)
{
	enum ldap_record_error rerr = ldap_group_delete(name);

	if (rerr != LDAP_RECORD_OK) {
		respond_ldap_record_error(fd, rerr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * Shared by create (task #726's POST) and update (PUT): parses every
 * user field out of root, leaving *password NULL when the "password"
 * key is absent from the body at all -- ldap_user_create()/update()
 * both treat NULL as "no credential change," matching PUT's own
 * documented "omit password to keep the existing one" semantics.
 */
static void parse_ldap_user_body(const struct json_value *root, const char **name, int *uidnumber,
                                  int *has_uidnumber, int *primarygroup,
                                  int secondary_groups[LDAP_USER_MAX_SECONDARY_GROUPS],
                                  int *secondary_group_count, const char **givenname,
                                  const char **sn, const char **mail, const char **loginshell,
                                  const char **homedirectory, const char **password, int *disabled,
                                  const char **ssh_public_key, int *can_search)
{
	const struct json_value *jdisabled = json_object_get(root, "disabled");
	const struct json_value *juidnumber = json_object_get(root, "uidnumber");
	const struct json_value *jsecondary = json_object_get(root, "secondary_groups");
	const struct json_value *jcan_search = json_object_get(root, "can_search");

	*name = json_as_string(json_object_get(root, "name"));
	*uidnumber = juidnumber != NULL ? (int)json_as_number(juidnumber) : 0;
	*has_uidnumber = juidnumber != NULL;
	*primarygroup = (int)json_as_number(json_object_get(root, "primarygroup"));
	*secondary_group_count = 0;
	if (jsecondary != NULL && jsecondary->type == JSON_ARRAY) {
		size_t i;

		for (i = 0; i < jsecondary->u.array.count && (int)i < LDAP_USER_MAX_SECONDARY_GROUPS; i++)
			secondary_groups[i] = (int)json_as_number(jsecondary->u.array.items[i]);
		*secondary_group_count = (int)i;
	}
	*givenname = json_as_string(json_object_get(root, "givenname"));
	*sn = json_as_string(json_object_get(root, "sn"));
	*mail = json_as_string(json_object_get(root, "mail"));
	*loginshell = json_as_string(json_object_get(root, "loginshell"));
	*homedirectory = json_as_string(json_object_get(root, "homedirectory"));
	*password = json_as_string(json_object_get(root, "password"));
	*disabled = (jdisabled != NULL && jdisabled->type == JSON_BOOL && jdisabled->u.boolean);
	*ssh_public_key = json_as_string(json_object_get(root, "ssh_public_key"));
	*can_search = (jcan_search != NULL && jcan_search->type == JSON_BOOL && jcan_search->u.boolean);
}

static void handle_ldap_user_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name, *givenname, *sn, *mail, *loginshell, *homedirectory, *password;
	const char *ssh_public_key;
	int uidnumber, has_uidnumber, primarygroup, disabled, can_search;
	int secondary_groups[LDAP_USER_MAX_SECONDARY_GROUPS], secondary_group_count;
	struct ldap_user *u;
	enum ldap_record_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	parse_ldap_user_body(root, &name, &uidnumber, &has_uidnumber, &primarygroup, secondary_groups,
	                      &secondary_group_count, &givenname, &sn, &mail, &loginshell,
	                      &homedirectory, &password, &disabled, &ssh_public_key, &can_search);
	if (name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name missing");
		return;
	}
	/* Omitted (task #748): auto-allocate from the configurable
	 * start_uid pool, same allocator the container-creation LDAP hook
	 * already uses. */
	if (!has_uidnumber)
		uidnumber = ldap_uid_alloc();

	/*
	 * ADR-0144 task #838: can_search now settable at create time via
	 * the real public API, not just the internal container-provisioning
	 * path (main.c's own auto-created-account call site) -- a real,
	 * durable bind/service account (nslcd's own binddn, or a live
	 * AuthorizedKeysCommand's own search bind) needs exactly this
	 * capability and has no other legitimate way to get it. owner_
	 * container stays NULL here: that field marks accounts this
	 * daemon itself auto-provisions and tears down with a specific
	 * container, not an operator-created one.
	 */
	rerr = ldap_user_create(name, uidnumber, primarygroup, secondary_groups, secondary_group_count,
	                         givenname, sn, mail, loginshell, homedirectory, password, disabled, NULL,
	                         can_search, ssh_public_key, &u);
	json_free(root); /* u points into ldap.c's own record store, not root -- safe past here */
	if (rerr != LDAP_RECORD_OK) {
		respond_ldap_record_error(fd, rerr);
		return;
	}

	jw_init(&w);
	ldap_user_write_json_one(u, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_ldap_user_update(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *body_name, *givenname, *sn, *mail, *loginshell, *homedirectory, *password;
	const char *ssh_public_key;
	int uidnumber, has_uidnumber, primarygroup, disabled, can_search;
	int secondary_groups[LDAP_USER_MAX_SECONDARY_GROUPS], secondary_group_count;
	struct ldap_user *u;
	enum ldap_record_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	parse_ldap_user_body(root, &body_name, &uidnumber, &has_uidnumber, &primarygroup,
	                      secondary_groups, &secondary_group_count, &givenname, &sn, &mail,
	                      &loginshell, &homedirectory, &password, &disabled, &ssh_public_key,
	                      &can_search);
	/* ADR-0147: body_name differing from the URL path's own {name} now
	 * renames the user -- previously discarded entirely (comment used
	 * to read "the URL path's name is authoritative for PUT, not the
	 * body's own", true for every OTHER field but left renaming with
	 * no path at all). */
	(void)has_uidnumber; /* PUT is full-field-replacement -- an omitted uidnumber here is 0,
	                       * same pre-existing semantics as every other omittable PUT field
	                       * (e.g. givenname/sn) resetting to empty; auto-allocation is only
	                       * for create, where "I don't have an opinion" is a real, common case. */

	rerr = ldap_user_update(name, body_name, uidnumber, primarygroup, secondary_groups,
	                         secondary_group_count, givenname, sn, mail, loginshell, homedirectory,
	                         password, disabled, ssh_public_key, can_search, &u);
	json_free(root);
	if (rerr != LDAP_RECORD_OK) {
		respond_ldap_record_error(fd, rerr);
		return;
	}

	jw_init(&w);
	ldap_user_write_json_one(u, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_ldap_user_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "users");
	ldap_user_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_ldap_user_get_one(int fd, const char *name)
{
	struct ldap_user *u = ldap_user_find(name);
	struct json_writer w;

	if (u == NULL) {
		respond_error(fd, 404, "Not Found", "no such LDAP user");
		return;
	}
	jw_init(&w);
	ldap_user_write_json_one(u, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_ldap_user_delete(int fd, const char *name)
{
	enum ldap_record_error rerr = ldap_user_delete(name);

	if (rerr != LDAP_RECORD_OK) {
		respond_ldap_record_error(fd, rerr);
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

	reissue_host_pki_cert();

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

static void handle_pki_intermediate_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	const char *common_name = "Kanxeo Intermediate CA";
	int days = 1825;
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

	perr = pki_intermediate_create(common_name, days);
	json_free(root);

	if (perr != PKI_OK) {
		/*
		 * respond_pki_error()'s own NOT_BOOTSTRAPPED/ALREADY_BOOTSTRAPPED
		 * wording is root-CA-specific ("POST /v1/pki/ca first" / "CA is
		 * already bootstrapped") -- both real but wrong words for this
		 * endpoint, so handled here instead of falling through to it.
		 */
		if (perr == PKI_ERR_NOT_BOOTSTRAPPED)
			respond_error(fd, 400, "Bad Request", "root CA not bootstrapped -- POST /v1/pki/ca first");
		else if (perr == PKI_ERR_ALREADY_BOOTSTRAPPED)
			respond_error(fd, 409, "Conflict", "intermediate is already bootstrapped");
		else
			respond_pki_error(fd, perr);
		return;
	}

	reissue_host_pki_cert(); /* now signed by the intermediate instead of the root */

	{
		struct json_writer w;

		jw_init(&w);
		if (pki_intermediate_get(&w) != PKI_OK) {
			jw_free(&w);
			respond_error(fd, 500, "Internal Server Error",
			              "intermediate created but could not be read back");
			return;
		}
		respond_json(fd, 201, "Created", &w);
		jw_free(&w);
	}
}

static void handle_pki_intermediate_get(int fd)
{
	struct json_writer w;
	enum pki_error perr;

	jw_init(&w);
	perr = pki_intermediate_get(&w);
	if (perr != PKI_OK) {
		jw_free(&w);
		/* Same 404-not-400 reasoning as handle_pki_ca_get() above --
		 * GET-ing an intermediate that doesn't exist yet is a missing
		 * resource, not a POST-precondition failure. */
		if (perr == PKI_ERR_NOT_BOOTSTRAPPED)
			respond_error(fd, 404, "Not Found", "intermediate not bootstrapped yet");
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
	char qualified_name[DNS_NAME_MAX];
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

	/* ADR-0052: same server-side default-qualification rule as DNS
	 * records above -- only the CN/default-SAN name, never an
	 * explicitly-supplied sans[] entry (an operator who lists exact
	 * SANs has already opted into precise control there). */
	siteconfig_qualify(name, qualified_name, sizeof(qualified_name));
	name = qualified_name;

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

/*
 * For every currently-live container that owns a just-reissued leaf
 * (name == the container's own name, owner_container == the
 * container's own name -- the exact convention create_container_from_
 * body()'s own pki_issue block always uses, near line 2319),
 * redeliver it so a running service's tls.crt/tls.key don't go stale
 * after a CA reset.
 *
 * Deliberately enumerated via registry_list_names(), not
 * containerdef_resolve_order() -- containerdef_add() only persists a
 * definition for restart != "no" (main.c's own POST /v1/containers
 * handler, a few hundred lines up); a plain, unpersisted restart:"no"
 * container is still a real, live pki_issue owner and must not be
 * silently skipped just because it has no containerdef entry to
 * enumerate through. pki_cert_owned_by() is checked against the PKI
 * index itself (the real source of truth for "does this container own
 * a cert"), not the original create request's own pki_issue flag --
 * robust regardless of whether a persisted definition happens to
 * exist to recover that flag from.
 *
 * pki_cert_dir is recovered from the persisted definition when one
 * exists (same field the original pki_issue block reads); a
 * restart:"no" container that used a non-default --pki-cert-dir= has
 * no persisted body to recover it from, so the default applies --
 * a known, narrow gap (documented in ADR-0049) rather than a silent
 * wrong-path write.
 */
static void redeliver_pki_certs_after_reset(void)
{
	char names[REGISTRY_MAX_CONTAINERS][REGISTRY_NAME_MAX];
	int count = registry_list_names(names, REGISTRY_MAX_CONTAINERS);
	int i;

	for (i = 0; i < count; i++) {
		struct registry_entry *entry = registry_find(names[i]);
		struct container_def *def;
		char cert_dir_buf[PATH_MAX];
		enum pki_error derr;

		if (entry == NULL || !entry->running)
			continue;
		if (!pki_cert_owned_by(names[i], names[i]))
			continue;

		snprintf(cert_dir_buf, sizeof(cert_dir_buf), "/etc/kanxeo-tls");
		def = containerdef_find(names[i]);
		if (def != NULL) {
			struct json_value *body_root = json_parse(def->body, def->body_len);

			if (body_root != NULL) {
				const char *dir = json_as_string(json_object_get(body_root, "pki_cert_dir"));

				if (dir != NULL)
					snprintf(cert_dir_buf, sizeof(cert_dir_buf), "%s", dir);
				json_free(body_root);
			}
		}

		derr = pki_cert_deliver(names[i], entry->handle.pid, cert_dir_buf);
		if (derr != PKI_OK)
			fprintf(stderr, "%s: pki reset redelivery failed (err=%d)\n", names[i],
			        (int)derr);
	}
}

static void handle_pki_reset(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	char root_cn[PKI_SUBJECT_MAX];
	char intermediate_cn[PKI_SUBJECT_MAX];
	int root_days = 3650;
	int intermediate_days = 1825;
	int leaf_days = 365;
	enum pki_error perr;
	struct json_writer w;

	snprintf(root_cn, sizeof(root_cn), "Kanxeo Root CA - %s", siteconfig_domain_suffix());
	snprintf(intermediate_cn, sizeof(intermediate_cn), "Kanxeo Intermediate CA - %s",
	         siteconfig_domain_suffix());

	if (body_len > 0) {
		const char *s;

		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		s = json_as_string(json_object_get(root, "root_common_name"));
		if (s != NULL)
			snprintf(root_cn, sizeof(root_cn), "%s", s);
		s = json_as_string(json_object_get(root, "intermediate_common_name"));
		if (s != NULL)
			snprintf(intermediate_cn, sizeof(intermediate_cn), "%s", s);
		if (json_object_get(root, "root_days") != NULL)
			root_days = (int)json_as_number(json_object_get(root, "root_days"));
		if (json_object_get(root, "intermediate_days") != NULL)
			intermediate_days = (int)json_as_number(json_object_get(root, "intermediate_days"));
		if (json_object_get(root, "leaf_days") != NULL)
			leaf_days = (int)json_as_number(json_object_get(root, "leaf_days"));
	}

	jw_init(&w);
	perr = pki_ca_reset(root_cn, intermediate_cn, root_days, intermediate_days, leaf_days, &w);
	json_free(root);

	if (perr != PKI_OK) {
		jw_free(&w);
		respond_pki_error(fd, perr);
		return;
	}

	redeliver_pki_certs_after_reset();

	/*
	 * Already reissued once above if "host" was tracked before this
	 * reset (pki_ca_reset()'s own generic per-leaf reissue loop) --
	 * called again here anyway, for the case where it wasn't (an
	 * existing chain that predates this feature, or one that was
	 * bootstrapped without ever going through a site config PUT). The
	 * redundant reissue in the already-tracked case is harmless (same
	 * FQDN, freshly signed either way) and keeps this guarantee simple:
	 * "host" always exists and is current after any PKI-affecting
	 * operation, full stop.
	 */
	reissue_host_pki_cert();

	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
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
	case PKG_ERR_DUPLICATE:
		/* ADR-0107: recipe versions are immutable once published --
		 * an already-published (name,version) pair is a real client
		 * error, not the old flat-file upsert-by-name behavior. */
		respond_error(fd, 409, "Conflict",
		              "this recipe version is already published -- versions are immutable, bump pkg_version= to publish a fix");
		break;
	case PKG_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "recipe operation failed");
		break;
	}
}

static const char *bootstrap_fetch_state_str(enum bootstrap_fetch_state s)
{
	switch (s) {
	case BOOTSTRAP_FETCH_FETCHING:
		return "fetching";
	case BOOTSTRAP_FETCH_READY:
		return "ready";
	case BOOTSTRAP_FETCH_FAILED:
		return "failed";
	case BOOTSTRAP_FETCH_NONE:
	default:
		return "none";
	}
}

static void write_bootstrap_fetch_status(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "state");
	jw_str(w, bootstrap_fetch_state_str(g_bootstrap_fetch_state));
	jw_key(w, "error");
	if (g_bootstrap_fetch_state == BOOTSTRAP_FETCH_FAILED)
		jw_str(w, g_bootstrap_fetch_error);
	else
		jw_null(w);
	jw_obj_close(w);
}

/* GET /v1/pkg/bootstrap (ADR-0065) -- polling for the toolchain_url
 * mode's own async fetch, the same shape GET /system/iso already
 * established. state is "none" until the first toolchain_url POST
 * this daemon has ever handled (the plain/toolchain_path modes below
 * are synchronous and never touch this state at all). */
static void handle_pkg_bootstrap_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	write_bootstrap_fetch_status(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * body/body_len optional: an empty body (the existing bare
 * `POST /v1/pkg/bootstrap` contract) keeps the live-copy fallback
 * unchanged. A JSON body with "toolchain_path" set switches to the
 * correct production path (pkg_bootstrap_from_toolchain()) instead --
 * a local path the operator has already scp'd a real toolchain
 * artifact to. Both of these stay exactly as they always were:
 * synchronous, 204 on success.
 *
 * A JSON body with "toolchain_url" (+ required "toolchain_sha256")
 * is the third mode ADR-0065 adds: the daemon fetches the artifact
 * itself, host-side, the same real curl primitive every recipe
 * source already uses -- closing the real gap the other two modes
 * both rest on (an operator "transferring it onto the box out-of-
 * band" assumes an SSH server a real, freshly-installed Kanxeo box
 * simply doesn't have, ADR-0034). Async, 202, poll GET /pkg/bootstrap.
 */
static void handle_pkg_bootstrap(int fd, const char *body, size_t body_len)
{
	enum pkg_error perr;
	const char *toolchain_path = NULL;
	const char *toolchain_url = NULL;
	const char *toolchain_sha256 = NULL;
	struct json_value *root = NULL;

	if (body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		toolchain_path = json_as_string(json_object_get(root, "toolchain_path"));
		toolchain_url = json_as_string(json_object_get(root, "toolchain_url"));
		toolchain_sha256 = json_as_string(json_object_get(root, "toolchain_sha256"));
	}

	if (toolchain_url != NULL && toolchain_url[0] != '\0') {
		char err_msg[256];
		struct json_writer w;

		if (g_bootstrap_fetch_state == BOOTSTRAP_FETCH_FETCHING) {
			if (root != NULL)
				json_free(root);
			respond_error(fd, 409, "Conflict", "a toolchain fetch is already in progress");
			return;
		}
		if (bootstrap_fetch_start(toolchain_url, toolchain_sha256, err_msg, sizeof(err_msg)) != 0) {
			if (root != NULL)
				json_free(root);
			respond_error(fd, 400, "Bad Request", err_msg);
			return;
		}
		if (root != NULL)
			json_free(root);

		jw_init(&w);
		write_bootstrap_fetch_status(&w);
		respond_json(fd, 202, "Accepted", &w);
		jw_free(&w);
		return;
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

/* ADR-0121: GET /v1/pkg/repo-config -- the configured sync source
 * (never echoes the auth token itself, see pkg_repo_write_json_
 * config()'s own doc comment). */
static void handle_pkg_repo_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	pkg_repo_write_json_config(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * PUT /v1/pkg/repo-config: a partial update -- any field omitted from
 * the body leaves that setting unchanged (pkg_repo_set_config()'s own
 * NULL-means-unchanged contract). "auth_token": "" explicitly clears
 * an already-configured token; omitting it entirely leaves whatever's
 * there. Changing sync_interval_seconds takes effect on the next
 * periodic-timer re-arm (immediately, via arm_pkg_sync_periodic_
 * timer() below) -- no daemon restart needed, same "live, no-restart"
 * posture every other *_config PUT in this codebase already has.
 */
static void handle_pkg_repo_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	const char *repo_url = NULL;
	const char *repo_kind = NULL;
	const char *ref = NULL;
	const char *auth_token = NULL;
	int sync_interval_seconds = -1;
	enum pkg_error perr;

	if (body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		repo_url = json_as_string(json_object_get(root, "repo_url"));
		repo_kind = json_as_string(json_object_get(root, "repo_kind"));
		ref = json_as_string(json_object_get(root, "ref"));
		auth_token = json_as_string(json_object_get(root, "auth_token"));
		{
			const struct json_value *iv = json_object_get(root, "sync_interval_seconds");

			if (iv != NULL)
				sync_interval_seconds = (int)json_as_number(iv);
		}
	}

	perr = pkg_repo_set_config(repo_url, repo_kind, ref, auth_token, sync_interval_seconds);
	if (root != NULL)
		json_free(root);
	if (perr != PKG_OK) {
		respond_pkg_error(fd, perr);
		return;
	}
	arm_pkg_sync_periodic_timer();
	handle_pkg_repo_config_get(fd);
}

/* POST /v1/pkg/sync: starts an async fetch+merge of the configured
 * repo's recipes (never binaries -- see pkg_sync_completed()'s own
 * doc comment). 409 if one is already running, 400 if no repo is
 * configured yet. 202, poll GET /v1/pkg/sync for the outcome. */
static void handle_pkg_sync_post(int fd)
{
	pid_t pid;
	int pidfd;
	enum pkg_error perr;
	struct json_writer w;

	perr = pkg_sync_start(&pid, &pidfd);
	if (perr == PKG_ERR_BUSY) {
		respond_error(fd, 409, "Conflict", "a sync is already in progress");
		return;
	}
	if (perr == PKG_ERR_NOT_FOUND) {
		respond_error(fd, 400, "Bad Request", "no repo configured (PUT /v1/pkg/repo-config first)");
		return;
	}
	if (perr != PKG_OK) {
		respond_pkg_error(fd, perr);
		return;
	}
	register_pkg_sync_fetch_pidfd(pid, pidfd);

	jw_init(&w);
	pkg_sync_write_json_status(&w);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}

static void handle_pkg_sync_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	pkg_sync_write_json_status(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* ADR-0122: GET /v1/pkg/cache-config -- the configured cache size cap. */
static void handle_pkg_cache_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "max_bytes");
	jw_int(&w, pkg_cache_get_max_bytes());
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pkg_cache_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *mb;
	enum pkg_error perr;

	if (body_len == 0) {
		respond_error(fd, 400, "Bad Request", "max_bytes is required");
		return;
	}
	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	mb = json_object_get(root, "max_bytes");
	if (mb == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "max_bytes is required");
		return;
	}
	perr = pkg_cache_set_max_bytes((long long)json_as_number(mb));
	json_free(root);
	if (perr != PKG_OK) {
		respond_pkg_error(fd, perr);
		return;
	}
	handle_pkg_cache_config_get(fd);
}

/*
 * Part 5 (ADR-0124): GET/PUT /v1/system/rolling-config -- mirrors
 * handle_pkg_cache_config_get/put()'s own shape exactly, one field,
 * same "PUT re-reads via GET on success" convention.
 */
static void handle_rolling_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "jitter_window_seconds");
	jw_int(&w, containerdef_jitter_window_get());
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_rolling_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jwindow;

	if (body_len == 0) {
		respond_error(fd, 400, "Bad Request", "jitter_window_seconds is required");
		return;
	}
	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jwindow = json_object_get(root, "jitter_window_seconds");
	if (jwindow == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "jitter_window_seconds is required");
		return;
	}
	if (containerdef_jitter_window_set((int)json_as_number(jwindow)) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "jitter_window_seconds must be 0-3600");
		return;
	}
	json_free(root);
	handle_rolling_config_get(fd);
}

/*
 * GET/PUT /v1/system/tls-throttle, GET /v1/system/tls-throttle/status
 * (ADR-0134) -- per-source-IP throttling for repeated failed HTTPS
 * handshakes. PUT is a real partial update, same convention pkg_repo_
 * set_config()/handle_pkg_repo_config_put() already established:
 * fields omitted from the body are left unchanged.
 */
static void handle_tls_throttle_get(int fd)
{
	struct throttle_config cfg = connthrottle_config_get();
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "enabled");
	jw_bool(&w, cfg.enabled);
	jw_key(&w, "threshold");
	jw_int(&w, cfg.threshold);
	jw_key(&w, "window_seconds");
	jw_int(&w, cfg.window_seconds);
	jw_key(&w, "block_seconds");
	jw_int(&w, cfg.block_seconds);
	jw_key(&w, "log_interval_seconds");
	jw_int(&w, cfg.log_interval_seconds);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_tls_throttle_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	int enabled_flag = -1;
	int threshold = -1;
	int window_seconds = -1;
	int block_seconds = -1;
	int log_interval_seconds = -1;

	if (body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		{
			const struct json_value *v = json_object_get(root, "enabled");

			if (v != NULL && v->type == JSON_BOOL)
				enabled_flag = v->u.boolean;
		}
		{
			const struct json_value *v = json_object_get(root, "threshold");

			if (v != NULL)
				threshold = (int)json_as_number(v);
		}
		{
			const struct json_value *v = json_object_get(root, "window_seconds");

			if (v != NULL)
				window_seconds = (int)json_as_number(v);
		}
		{
			const struct json_value *v = json_object_get(root, "block_seconds");

			if (v != NULL)
				block_seconds = (int)json_as_number(v);
		}
		{
			const struct json_value *v = json_object_get(root, "log_interval_seconds");

			if (v != NULL)
				log_interval_seconds = (int)json_as_number(v);
		}
	}

	if (connthrottle_config_set(enabled_flag, threshold, window_seconds, block_seconds,
	                             log_interval_seconds) != 0) {
		if (root != NULL)
			json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "threshold must be 1-100000, window_seconds 1-86400, block_seconds 1-604800, "
		              "log_interval_seconds 0-3600");
		return;
	}
	if (root != NULL)
		json_free(root);
	handle_tls_throttle_get(fd);
}

static void handle_tls_throttle_status_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "entries");
	connthrottle_write_status_json(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* GET /v1/pkg/cache -- current cache occupancy; DELETE /v1/pkg/cache --
 * clears every cached artifact (an explicit operator reset). */
static void handle_pkg_cache_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	pkg_cache_write_json_status(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pkg_cache_delete(int fd)
{
	pkg_cache_clear();
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/* ADR-0122: GET/PUT /v1/pkg/artifact-config -- the configured plain-
 * HTTP precompiled-artifact server (deliberately NOT the git-forge
 * repo config above -- see pkg.c's own module comment for why). */
static void handle_pkg_artifact_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	pkg_artifact_write_json_config(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pkg_artifact_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	const char *base_url = NULL;
	const char *auth_token = NULL;
	enum pkg_error perr;

	if (body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		base_url = json_as_string(json_object_get(root, "base_url"));
		auth_token = json_as_string(json_object_get(root, "auth_token"));
	}

	perr = pkg_artifact_set_config(base_url, auth_token);
	if (root != NULL)
		json_free(root);
	if (perr != PKG_OK) {
		respond_pkg_error(fd, perr);
		return;
	}
	handle_pkg_artifact_config_get(fd);
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
 * Real, ongoing recipe management (ADR-0040) -- an operator can publish
 * a new recipe version on an already-running system, no ISO rebuild/
 * reinstall needed. Versions are immutable once published (ADR-0107):
 * an already-published (name,version) is PKG_ERR_DUPLICATE, the same
 * meaning pkg_install_start()'s own "duplicate" already has -- fixing
 * a mistake means publishing a new version, not overwriting this one.
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
	/*
	 * ADR-0107: pkg_recipe_add()'s own queue_rolling_rebuilds_for()
	 * only enqueues -- it can't itself start a job (pkg.c has no epoll
	 * access). Every OTHER trigger point (try_start_queued_pkg_rebuild()'s
	 * own call sites) only fires reactively when a previously-running
	 * job finishes; if the daemon is completely idle right now (the
	 * common case -- publishing a new version doesn't require any job
	 * to already be in flight), nothing would otherwise ever start the
	 * rebuild this request just queued. This is the one place that
	 * actually kicks it off immediately when nothing else will.
	 */
	try_start_queued_pkg_rebuild();
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/* version NULL (no ?version= given) removes every published version of
 * name; a specific version removes only that one (ADR-0107). */
static void handle_pkg_recipe_delete(int fd, const char *name, const char *version)
{
	enum pkg_error perr = pkg_recipe_delete(name, version);

	if (perr != PKG_OK) {
		respond_pkg_recipe_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/* version NULL (no ?version= given) resolves to name's highest
 * available version (ADR-0107). */
static void handle_pkg_recipe_get(int fd, const char *name, const char *version)
{
	struct json_writer w;
	enum pkg_error perr;

	jw_init(&w);
	perr = pkg_recipe_get(name, version, &w);
	if (perr != PKG_OK) {
		jw_free(&w);
		respond_pkg_recipe_error(fd, perr);
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pkg_install(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name;
	const char *image;
	const char *version;
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
	/* ADR-0107: omitted (NULL) resolves to name's highest available
	 * recipe version, matching every pre-existing caller's behavior. */
	version = json_as_string(json_object_get(root, "version"));
	jupgrade = json_object_get(root, "upgrade");
	upgrade = (jupgrade != NULL && jupgrade->type == JSON_BOOL && jupgrade->u.boolean);

	perr = pkg_install_start(name, image, version, upgrade, started_name, sizeof(started_name),
	                          &pid, &pidfd);
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
 * POST /v1/pkg/hostbuild (ADR-0056): the second mode of the same
 * fetch/build pipeline handle_pkg_install() drives above -- builds a
 * standalone host artifact (a kernel bzImage, a fresh kanxeod-root
 * squashfs's own components) instead of installing into a container
 * image's rootfs. Reuses register_pkg_fetch_pidfd()/respond_pkg_error()
 * completely unmodified; the only new plumbing is pkg_hostbuild_start()
 * itself.
 */
static void handle_pkg_hostbuild(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name;
	const char *build_image;
	const char *version;
	const struct json_value *jupgrade;
	int upgrade;
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
	build_image = json_as_string(json_object_get(root, "build_image"));
	if (name == NULL || build_image == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name and build_image are both required");
		return;
	}
	/* ADR-0107: omitted (NULL) resolves to name's highest available
	 * recipe version. */
	version = json_as_string(json_object_get(root, "version"));
	jupgrade = json_object_get(root, "upgrade");
	upgrade = (jupgrade != NULL && jupgrade->type == JSON_BOOL && jupgrade->u.boolean);

	perr = pkg_hostbuild_start(name, build_image, version, upgrade, &pid, &pidfd);
	if (perr != PKG_OK) {
		json_free(root);
		respond_pkg_error(fd, perr);
		return;
	}

	jw_init(&w);
	if (pkg_get_one(name, PKG_HOSTBUILD_IMAGE, &w) != PKG_OK) {
		/* shouldn't happen -- pkg_hostbuild_start() just created it */
		jw_free(&w);
		json_free(root);
		respond_error(fd, 500, "Internal Server Error",
		              "hostbuild started but could not be read back");
		return;
	}
	json_free(root);
	register_pkg_fetch_pidfd(pid, pidfd);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}

/* GET /v1/pkg/hostbuild/{name} -- status/artifact_path polling for a
 * hostbuild job, the exact shape kanxeoctl pkg hostbuild --wait polls. */
static void handle_pkg_hostbuild_get(int fd, const char *name)
{
	struct json_writer w;
	enum pkg_error perr;

	jw_init(&w);
	perr = pkg_get_one(name, PKG_HOSTBUILD_IMAGE, &w);
	if (perr != PKG_OK) {
		jw_free(&w);
		respond_pkg_error(fd, perr);
		return;
	}
	respond_json(fd, 200, "OK", &w);
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

	perr = pkg_install_start(name, image, NULL, 1, started_name, sizeof(started_name), &pid,
	                          &pidfd);
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

	/* Every kanxeoctl command and every web UI action already goes
	 * through this exact function (API-First Mandate, no exceptions)
	 * -- one log call here is a complete audit trail of every real
	 * action taken via either client, with zero client-side
	 * instrumentation needed (ADR for the consolidated log store).
	 * Every GET is excluded, not just /v1/health: a GET is a query,
	 * never a mutation, and the web dashboard alone issues ~30 of them
	 * every single poll cycle (every 2s) purely to keep its own cache
	 * fresh -- none of that is a real, deliberate action worth
	 * auditing, the same reasoning that originally excluded /health
	 * specifically (both clients poll it every few seconds purely for
	 * a status dot) generalizes to every other routine GET the same
	 * way. Confirmed live: once the web dashboard's own bottom log
	 * panel started polling GET /v1/system/logs itself (ADR-0129), the
	 * audit trail became a fast-scrolling wall of the dashboard's own
	 * routine polling, drowning real actions and costing real render
	 * time client-side -- this is that fix, not scope creep: the
	 * original exclusion's own stated rationale already covered this
	 * case, it just hadn't been generalized yet. POST/PUT/DELETE (the
	 * real mutations) are completely unaffected. */
	if (strcmp(req->method, "GET") != 0)
		logstore_write("audit", "info", "%s %s", req->method, req->path);

	if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/v1/login") == 0) {
		handle_login(fd, req->body, req->body_len);
		return;
	}
	if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/v1/logout") == 0) {
		handle_logout(fd, req->headers, req->headers_len);
		return;
	}

	/*
	 * ADR-0144: write-gating -- the one authorization check every
	 * mutating request goes through, dispatch-wide, before any route
	 * below ever sees it. "Mutating" means every non-GET verb, plus
	 * one deliberate GET-verb exception: the container console
	 * upgrade is, in real effect, arbitrary command execution inside a
	 * container, judged here by intent rather than HTTP method (the
	 * same reasoning the audit-log exclusion just above already
	 * applies the other way -- a GET is usually "just a query," this
	 * one specifically isn't). hostauth_authorize_write() itself
	 * returns true unconditionally while gating isn't active yet (no
	 * admin-group user exists) -- a fresh install is never locked out
	 * of its own API by this. /v1/login (above) and /v1/logout are
	 * the only two paths that bypass this block entirely; every other
	 * GET is already exempt by construction (needs_auth stays 0).
	 */
	{
		size_t path_len = strlen(req->path);
		int is_console = strcmp(req->method, "GET") == 0 &&
		                  strncmp(req->path, CONTAINERS_PREFIX, strlen(CONTAINERS_PREFIX)) == 0 &&
		                  path_len > 8 && strcmp(req->path + path_len - 8, "/console") == 0;
		int needs_auth = strcmp(req->method, "GET") != 0 || is_console;

		if (needs_auth) {
			char token_hdr[HOSTAUTH_TOKEN_LEN + 32];
			const char *bearer = NULL;

			if (http_find_header(req->headers, req->headers_len, "Authorization", token_hdr,
			                      sizeof(token_hdr)) >= 0) {
				bearer = strncmp(token_hdr, "Bearer ", 7) == 0 ? token_hdr + 7 : token_hdr;
			}
			if (!hostauth_authorize_write(bearer)) {
				respond_error(fd, 401, "Unauthorized",
				              "authentication required -- POST /v1/login first");
				return;
			}
		}
	}

	if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/v1/health") == 0) {
		handle_health(fd);
		return;
	}
	if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/v1/system/boot") == 0) {
		handle_system_boot(fd);
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
	if (strcmp(req->path, "/v1/system/backup-config") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_backup_config_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_backup_config_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/v1/system/backup-config/status") == 0) {
		handle_backup_config_status_get(fd);
		return;
	}
	if (strcmp(req->method, "POST") == 0 &&
	    strcmp(req->path, "/v1/system/backup-config/snapshot-now") == 0) {
		handle_backup_config_snapshot_now_post(fd);
		return;
	}
	if (strcmp(req->path, "/v1/system/hostauth-config") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_hostauth_config_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_hostauth_config_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/hostauth/sessions") == 0 && strcmp(req->method, "GET") == 0) {
		handle_hostauth_sessions_get(fd);
		return;
	}
	if (strncmp(req->path, "/v1/system/hostauth/sessions/", 29) == 0 &&
	    strcmp(req->method, "DELETE") == 0) {
		const char *username = req->path + 29;

		if (username[0] != '\0') {
			handle_hostauth_sessions_revoke(fd, username);
			return;
		}
	}
	if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/v1/system/restore") == 0) {
		handle_system_restore(fd, req->body, req->body_len);
		return;
	}
	if (strcmp(req->path, "/v1/system/site") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_site_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_site_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/daemon-config") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_daemon_config_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_daemon_config_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/routes") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_route_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_route_add(fd, req->body, req->body_len);
			return;
		}
		if (strcmp(req->method, "DELETE") == 0) {
			handle_route_del(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/stats") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_system_stats(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/processes") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_hostproc_list(fd);
			return;
		}
	}
	if (strncmp(req->path, PROCESSES_PREFIX, strlen(PROCESSES_PREFIX)) == 0) {
		name = req->path + strlen(PROCESSES_PREFIX);
		if (name[0] != '\0' && strcmp(req->method, "DELETE") == 0) {
			handle_hostproc_kill(fd, name);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/ping") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_ping_get(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_ping_post(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/resolv") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_resolv_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_resolv_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/state-storage") == 0 && strcmp(req->method, "GET") == 0) {
		handle_state_storage_get(fd);
		return;
	}
	if (strcmp(req->path, "/v1/system/state-storage/migrate") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_state_storage_migrate_post(fd, req->body, req->body_len);
			return;
		}
		if (strcmp(req->method, "GET") == 0) {
			handle_state_storage_migrate_get(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/log-storage") == 0 && strcmp(req->method, "GET") == 0) {
		handle_log_storage_get(fd);
		return;
	}
	if (strcmp(req->path, "/v1/system/log-storage/migrate") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_log_storage_migrate_post(fd, req->body, req->body_len);
			return;
		}
		if (strcmp(req->method, "GET") == 0) {
			handle_log_storage_migrate_get(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/rebuildable-storage") == 0 && strcmp(req->method, "GET") == 0) {
		handle_rebuildable_storage_get(fd);
		return;
	}
	if (strcmp(req->path, "/v1/system/rebuildable-storage/migrate") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_rebuildable_storage_migrate_post(fd, req->body, req->body_len);
			return;
		}
		if (strcmp(req->method, "GET") == 0) {
			handle_rebuildable_storage_migrate_get(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/rolling-config") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_rolling_config_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_rolling_config_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/tls-throttle") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_tls_throttle_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_tls_throttle_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/tls-throttle/status") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_tls_throttle_status_get(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/ntp") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_ntp_config_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_ntp_config_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/ntp/status") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_ntp_status_get(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/ntp/sync") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_ntp_sync_post(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/time") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_time_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_time_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/ntp/servers") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_ntp_server_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_ntp_server_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, NTP_SERVERS_PREFIX, strlen(NTP_SERVERS_PREFIX)) == 0) {
		name = req->path + strlen(NTP_SERVERS_PREFIX);
		if (name[0] != '\0' && strcmp(req->method, "DELETE") == 0) {
			handle_ntp_server_delete(fd, name);
			return;
		}
	}
	if (strcmp(req->path, "/v1/syslog/targets") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_syslog_target_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_syslog_target_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, SYSLOG_TARGETS_PREFIX, strlen(SYSLOG_TARGETS_PREFIX)) == 0) {
		name = req->path + strlen(SYSLOG_TARGETS_PREFIX);
		if (name[0] != '\0' && strcmp(req->method, "DELETE") == 0) {
			handle_syslog_target_delete(fd, name);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/swap") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_swap_get(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_swap_enable(fd, req->body, req->body_len);
			return;
		}
		if (strcmp(req->method, "DELETE") == 0) {
			handle_swap_disable(fd);
			return;
		}
	}
	{
		/* "/v1/system/logs" may carry a trailing "?tail=.../source=..."
		 * query string (this codebase's own established shape for a
		 * GET route with query params, matching .../files' own
		 * "?path=..." handling above) -- matched by base-path length,
		 * not a raw strcmp against the full req->path. */
		size_t qlen = strcspn(req->path, "?");

		if (qlen == strlen("/v1/system/logs") &&
		    strncmp(req->path, "/v1/system/logs", qlen) == 0) {
			if (strcmp(req->method, "GET") == 0) {
				handle_logs_get(fd, req);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/system/logs/config") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_logs_config_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_logs_config_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/iso") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_system_iso_get(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_system_iso_post(fd, req->body, req->body_len);
			return;
		}
	}
	/*
	 * ADR-0151: container recipes' own reserved paths, checked before
	 * the generic CONTAINERS_PREFIX/{name} fallback below -- same
	 * "recipes" reserved-word boundary ADR-0123's own image recipes
	 * already established one level up (a container literally named
	 * "recipes" would otherwise be unreachable via GET/DELETE
	 * /v1/containers/{name}).
	 */
	if (strcmp(req->path, "/v1/containers/recipes") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_container_recipe_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_container_recipe_add(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, CONTAINER_RECIPES_PREFIX, strlen(CONTAINER_RECIPES_PREFIX)) == 0) {
		name = req->path + strlen(CONTAINER_RECIPES_PREFIX);
		if (name[0] != '\0') {
			size_t nlen = strlen(name);

			if (nlen > 6 && strcmp(name + nlen - 6, "/apply") == 0 &&
			    strcmp(req->method, "POST") == 0 && nlen - 6 < PKG_NAME_MAX) {
				char recipe_name[PKG_NAME_MAX];

				memcpy(recipe_name, name, nlen - 6);
				recipe_name[nlen - 6] = '\0';
				handle_container_recipe_apply(fd, recipe_name, req->body, req->body_len);
				return;
			}
			if (strcmp(req->method, "GET") == 0) {
				handle_container_recipe_get(fd, name);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_container_recipe_delete(fd, name);
				return;
			}
		}
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
			if (nlen > 6 && strcmp(name + nlen - 6, "/stats") == 0 &&
			    strcmp(req->method, "GET") == 0 && nlen - 6 < REGISTRY_NAME_MAX) {
				char container_name[REGISTRY_NAME_MAX];

				memcpy(container_name, name, nlen - 6);
				container_name[nlen - 6] = '\0';
				handle_container_stats(fd, container_name);
				return;
			}
			if (nlen > 16 && strcmp(name + nlen - 16, "/migrate-storage") == 0 &&
			    nlen - 16 < REGISTRY_NAME_MAX &&
			    (strcmp(req->method, "POST") == 0 || strcmp(req->method, "GET") == 0)) {
				char container_name[REGISTRY_NAME_MAX];

				memcpy(container_name, name, nlen - 16);
				container_name[nlen - 16] = '\0';
				if (strcmp(req->method, "POST") == 0)
					handle_container_migrate_storage_post(fd, container_name, req->body,
					                                       req->body_len);
				else
					handle_container_migrate_storage_get(fd, container_name);
				return;
			}
			{
				/*
				 * Unlike every other suffix here, "/files" can carry a
				 * trailing "?path=..." query string -- container names
				 * are still '/'-free, but qlen (not nlen) is the part
				 * that actually ends in "/files"; the raw, un-truncated
				 * name (with its "?..." intact) is what url_query_param()
				 * needs to find "path" in.
				 */
				size_t qlen = strcspn(name, "?");

				if (qlen > 6 && strncmp(name + qlen - 6, "/files", 6) == 0 &&
				    qlen - 6 < REGISTRY_NAME_MAX &&
				    (strcmp(req->method, "GET") == 0 || strcmp(req->method, "PUT") == 0)) {
					char container_name[REGISTRY_NAME_MAX];
					char rel_path[CONTAINER_FILE_PATH_MAX];

					memcpy(container_name, name, qlen - 6);
					container_name[qlen - 6] = '\0';
					if (url_query_param(req->path, "path", rel_path, sizeof(rel_path)) != 0) {
						respond_error(fd, 400, "Bad Request", "missing path query parameter");
						return;
					}
					if (strcmp(req->method, "GET") == 0)
						handle_container_file_read(fd, container_name, rel_path);
					else
						handle_container_file_write(fd, container_name, rel_path, req->body,
						                             req->body_len);
					return;
				}
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
	if (strcmp(req->path, "/v1/disks") == 0 && strcmp(req->method, "GET") == 0) {
		handle_disk_list(fd);
		return;
	}
	if (strncmp(req->path, DISKS_PREFIX, strlen(DISKS_PREFIX)) == 0) {
		name = req->path + strlen(DISKS_PREFIX);
		size_t nlen = strlen(name);

		if (nlen > 7 && strcmp(name + nlen - 7, "/format") == 0 &&
		    nlen - 7 < DISKROLE_DISK_NAME_MAX) {
			char disk_name[DISKROLE_DISK_NAME_MAX];

			memcpy(disk_name, name, nlen - 7);
			disk_name[nlen - 7] = '\0';
			if (strcmp(req->method, "POST") == 0) {
				handle_disk_format_post(fd, disk_name, req->body, req->body_len);
				return;
			}
			if (strcmp(req->method, "GET") == 0) {
				handle_disk_format_get(fd, disk_name);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/diskroles") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_diskrole_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_diskrole_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, DISKROLES_PREFIX, strlen(DISKROLES_PREFIX)) == 0) {
		name = req->path + strlen(DISKROLES_PREFIX);
		if (name[0] != '\0' && strcmp(req->method, "DELETE") == 0) {
			handle_diskrole_delete(fd, name);
			return;
		}
	}
	if (strcmp(req->path, "/v1/devicemaps") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_devicemap_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_devicemap_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, DEVICEMAPS_PREFIX, strlen(DEVICEMAPS_PREFIX)) == 0) {
		name = req->path + strlen(DEVICEMAPS_PREFIX);
		if (name[0] != '\0' && strcmp(req->method, "DELETE") == 0) {
			handle_devicemap_delete(fd, name);
			return;
		}
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
	/*
	 * ADR-0123: image recipes' own reserved paths, checked before the
	 * generic IMAGES_PREFIX/{name} fallback below -- same "recipes"/
	 * "recipe-apply-status" reserved-words boundary PKG_RECIPES_PREFIX
	 * already established for package recipes, one level up (an image
	 * literally named "recipes" would otherwise be unreachable via
	 * GET/DELETE /v1/images/{name}).
	 */
	if (strcmp(req->path, "/v1/images/recipes") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_image_recipe_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_image_recipe_add(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/images/recipe-apply-status") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_image_recipe_apply_status_get(fd);
			return;
		}
	}
	if (strncmp(req->path, IMAGE_RECIPES_PREFIX, strlen(IMAGE_RECIPES_PREFIX)) == 0) {
		name = req->path + strlen(IMAGE_RECIPES_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_image_recipe_get(fd, name);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_image_recipe_delete(fd, name);
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
			size_t nlen = strlen(name);

			/* ADR-0107: /v1/images/{name}/manifest (POST, upsert one
			 * entry) and /v1/images/{name}/manifest/{package} (DELETE,
			 * remove one). Image/package names are both '/'-free
			 * (namecheck.h), so plain suffix/substring checks
			 * unambiguously split them, the same precedent
			 * CONTAINERS_PREFIX's own "/start"-style suffixes and the
			 * pkg recipes route's "/manifest/" split above already
			 * established. */
			if (nlen > 9 && strcmp(name + nlen - 9, "/manifest") == 0 &&
			    strcmp(req->method, "POST") == 0 && nlen - 9 < PKG_IMAGE_NAME_MAX) {
				char image_name[PKG_IMAGE_NAME_MAX];

				memcpy(image_name, name, nlen - 9);
				image_name[nlen - 9] = '\0';
				handle_image_manifest_set(fd, image_name, req->body, req->body_len);
				return;
			}
			/* ADR-0123: /v1/images/{name}/apply-recipe (POST) --
			 * applies image's own already-stored recipe. */
			if (nlen > 13 && strcmp(name + nlen - 13, "/apply-recipe") == 0 &&
			    strcmp(req->method, "POST") == 0 && nlen - 13 < PKG_IMAGE_NAME_MAX) {
				char image_name[PKG_IMAGE_NAME_MAX];

				memcpy(image_name, name, nlen - 13);
				image_name[nlen - 13] = '\0';
				handle_image_recipe_apply(fd, image_name);
				return;
			}
			{
				const char *marker = strstr(name, "/manifest/");

				if (marker != NULL && strcmp(req->method, "DELETE") == 0) {
					size_t image_len = (size_t)(marker - name);
					const char *package = marker + strlen("/manifest/");

					if (image_len < PKG_IMAGE_NAME_MAX && package[0] != '\0') {
						char image_name[PKG_IMAGE_NAME_MAX];

						memcpy(image_name, name, image_len);
						image_name[image_len] = '\0';
						handle_image_manifest_unset(fd, image_name, package);
						return;
					}
				}
			}
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
			if (strcmp(req->method, "PUT") == 0) {
				handle_dns_record_update(fd, name, req->body, req->body_len);
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
	if (strcmp(req->path, "/v1/ldap/servers") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_ldap_server_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_ldap_server_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, LDAP_SERVERS_PREFIX, strlen(LDAP_SERVERS_PREFIX)) == 0) {
		name = req->path + strlen(LDAP_SERVERS_PREFIX);
		if (name[0] != '\0' && strcmp(req->method, "DELETE") == 0) {
			handle_ldap_server_delete(fd, name);
			return;
		}
	}
	if (strcmp(req->path, "/v1/ldap/config") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_ldap_config_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_ldap_config_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/ldap/groups") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_ldap_group_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_ldap_group_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, LDAP_GROUPS_PREFIX, strlen(LDAP_GROUPS_PREFIX)) == 0) {
		name = req->path + strlen(LDAP_GROUPS_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_ldap_group_get_one(fd, name);
				return;
			}
			if (strcmp(req->method, "PUT") == 0) {
				handle_ldap_group_update(fd, name, req->body, req->body_len);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_ldap_group_delete(fd, name);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/ldap/users") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_ldap_user_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_ldap_user_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, LDAP_USERS_PREFIX, strlen(LDAP_USERS_PREFIX)) == 0) {
		name = req->path + strlen(LDAP_USERS_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_ldap_user_get_one(fd, name);
				return;
			}
			if (strcmp(req->method, "PUT") == 0) {
				handle_ldap_user_update(fd, name, req->body, req->body_len);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_ldap_user_delete(fd, name);
				return;
			}
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
	if (strcmp(req->path, "/v1/pki/intermediate") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pki_intermediate_get(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_pki_intermediate_create(fd, req->body, req->body_len);
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
	if (strcmp(req->path, "/v1/pki/reset") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_pki_reset(fd, req->body, req->body_len);
			return;
		}
	}
	/*
	 * These reserved paths are checked before the generic
	 * PKG_PREFIX/{name} fallback below, exactly like every other
	 * resource's exact-match-then-prefix ordering in this dispatch --
	 * a package named "bootstrap"/"recipes"/"install"/"update-all"/
	 * "repo-config"/"sync" (ADR-0121)/"cache-config"/"cache"/
	 * "artifact-config" (ADR-0122) would be unreachable via
	 * GET/DELETE /v1/pkg/{name}, a deliberate, documented
	 * reserved-words boundary. PKG_RECIPES_PREFIX
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
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_bootstrap_get(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg/repo-config") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_repo_config_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_pkg_repo_config_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg/sync") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_pkg_sync_post(fd);
			return;
		}
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_sync_get(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg/cache-config") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_cache_config_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_pkg_cache_config_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg/cache") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_cache_get(fd);
			return;
		}
		if (strcmp(req->method, "DELETE") == 0) {
			handle_pkg_cache_delete(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg/artifact-config") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_artifact_config_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_pkg_artifact_config_put(fd, req->body, req->body_len);
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
		if (name[0] != '\0' && (strcmp(req->method, "DELETE") == 0 || strcmp(req->method, "GET") == 0)) {
			/* ADR-0107: an optional ?version= query param can follow
			 * the package name -- same "qlen, not nlen" pattern
			 * CONTAINERS_PREFIX's own .../files route already
			 * established for a path suffix that can carry a query
			 * string (name is still '/'-free, so a plain strcspn()
			 * unambiguously finds where it ends). */
			size_t qlen = strcspn(name, "?");
			char pkg_name[PKG_NAME_MAX];
			char version[PKG_VERSION_MAX];
			const char *version_ptr = NULL;

			if (qlen < sizeof(pkg_name)) {
				memcpy(pkg_name, name, qlen);
				pkg_name[qlen] = '\0';
				if (url_query_param(req->path, "version", version, sizeof(version)) == 0)
					version_ptr = version;
				if (strcmp(req->method, "DELETE") == 0) {
					handle_pkg_recipe_delete(fd, pkg_name, version_ptr);
					return;
				}
				handle_pkg_recipe_get(fd, pkg_name, version_ptr);
				return;
			}
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
	if (strcmp(req->path, "/v1/pkg/hostbuild") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_pkg_hostbuild(fd, req->body, req->body_len);
			return;
		}
	}
	{
		/* Checked before the generic PKG_PREFIX fallback below, the
		 * same "specific route before the catch-all" ordering
		 * PKG_RECIPES_PREFIX already establishes -- otherwise
		 * "hostbuild/<name>" would fall through as if it were a
		 * literal package named that. */
		static const char hostbuild_prefix[] = "/v1/pkg/hostbuild/";

		if (strncmp(req->path, hostbuild_prefix, sizeof(hostbuild_prefix) - 1) == 0) {
			name = req->path + sizeof(hostbuild_prefix) - 1;
			if (name[0] != '\0' && strcmp(req->method, "GET") == 0) {
				handle_pkg_hostbuild_get(fd, name);
				return;
			}
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
		/* task #764: exec_into_container()'s own diagnostics
		 * (daemon/src/exec.c's fprintf(stderr,...) calls) go to
		 * kanxeod's own stderr -- invisible to a REST client on a
		 * real installed box with no host shell access, exactly the
		 * gap that made a live-only console failure unreachable to
		 * diagnose during task #760's sweep. errno is preserved by
		 * every one of exec_into_container()'s own failure paths
		 * (confirmed by inspection), so surfacing strerror(errno)
		 * here costs nothing and gives the real reason directly in
		 * the HTTP response body instead of a bare generic message. */
		char errmsg[256];

		snprintf(errmsg, sizeof(errmsg), "failed to start console session: %s", strerror(errno));
		respond_error(cc->fd, 500, "Internal Server Error", errmsg);
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
	if (tls_write_all(cc->fd, response, (size_t)rlen) != 0) {
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

#define PKG_BUILD_LOG_PATH "/v1/pkg/build/log"

/*
 * GET /v1/pkg/build/log -- live-tail of the currently in-flight pkg
 * build's own stdout/stderr (task #676), over the same minimal
 * WebSocket upgrade try_console_upgrade() above uses. Unlike the
 * console, there's no PTY or exec'd process on the other end -- this
 * is a plain one-way relay of pkg.c's own already-epoll-drained
 * output pipe (handle_pkg_build_output_event()/build_log_ws_
 * broadcast() above), so this function is far shorter: no
 * exec_into_container(), no console_exec_session, no CONN_CONSOLE_PTY
 * counterpart. Reuses enum console_route_result -- the same three-
 * outcome shape (not matched / failed before cc touched / fully
 * handled) applies unchanged to this route too.
 */
static enum console_route_result try_pkg_build_log_upgrade(struct conn *cc, const struct http_request *req)
{
	char upgrade_val[32], connection_val[64], ws_key[256], ws_version[8];
	char accept_val[64];
	char response[512];
	int rlen;
	struct registry_entry *entry;
	/* Generously >= pkg.c's own private PKG_BUILD_OUTPUT_CAPTURE_MAX
	 * (3800) -- pkg_build_output_snapshot() takes an explicit cap and
	 * never writes past it regardless, so this only needs to be "big
	 * enough," not an exact mirror of a constant that's deliberately
	 * private to pkg.c. */
	char snapshot[4096];
	int snapshot_len;

	if (strcmp(req->method, "GET") != 0)
		return CONSOLE_NOT_MATCHED;
	if (strcmp(req->path, PKG_BUILD_LOG_PATH) != 0)
		return CONSOLE_NOT_MATCHED;

	if (http_find_header(req->headers, req->headers_len, "Upgrade", upgrade_val, sizeof(upgrade_val)) < 0 ||
	    strcasecmp(upgrade_val, "websocket") != 0) {
		respond_error(cc->fd, 400, "Bad Request", "this endpoint requires Upgrade: websocket");
		return CONSOLE_FAILED;
	}
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

	entry = registry_find(PKG_BUILD_CONTAINER_NAME);
	if (entry == NULL || !entry->running) {
		respond_error(cc->fd, 404, "Not Found", "no build in progress");
		return CONSOLE_FAILED;
	}
	if (g_build_log_ws_conn_count >= PKG_BUILD_LOG_WS_MAX) {
		respond_error(cc->fd, 503, "Service Unavailable", "too many live-tail viewers already attached");
		return CONSOLE_FAILED;
	}

	if (ws_compute_accept(ws_key, accept_val, sizeof(accept_val)) != 0) {
		respond_error(cc->fd, 500, "Internal Server Error", "failed to compute websocket accept");
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
		respond_error(cc->fd, 500, "Internal Server Error", "failed to build handshake response");
		return CONSOLE_FAILED;
	}

	/* cc is still an untouched, ordinary CONN_CLIENT up through this
	 * point -- every failure above and below, up until cc->kind
	 * actually changes further down, is still CONSOLE_FAILED. */
	http_set_blocking(cc->fd);
	if (tls_write_all(cc->fd, response, (size_t)rlen) != 0)
		return CONSOLE_FAILED; /* client already gone -- nothing left to respond with */

	/* From here on cc IS repurposed -- same "no more CONSOLE_FAILED"
	 * rule try_console_upgrade() documents, and the same reason: its
	 * own storage (cc->http's buffer) is about to be freed, so a
	 * caller-side teardown expecting an ordinary CONN_CLIENT would
	 * double-free it. Repurposed in place, exactly like
	 * try_console_upgrade()'s own cc -> CONN_CONSOLE_WS half -- no
	 * PTY/exec-session counterpart needed here, so unlike that route
	 * this is the ENTIRE repurposing, not just one half of a pair. */
	http_conn_free(&cc->http);
	cc->kind = CONN_PKG_BUILD_LOG_WS;
	ws_conn_init(&cc->ws);
	g_build_log_ws_conns[g_build_log_ws_conn_count++] = cc;

	/* cc->fd is already registered for EPOLLIN -- no epoll_ctl needed,
	 * same as try_console_upgrade()'s own WS half. */

	snapshot_len = pkg_build_output_snapshot(snapshot, sizeof(snapshot));
	if (snapshot_len > 0 && ws_write_frame(cc->fd, WS_OPCODE_TEXT, snapshot, (size_t)snapshot_len) != 0) {
		build_log_ws_detach(cc);
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		ws_conn_free(&cc->ws);
		close(cc->fd);
		free(cc);
	}

	return CONSOLE_HANDLED;
}

/*
 * A live-tail client never sends anything meaningful (this is a
 * one-way relay) -- the only thing this handler cares about is
 * noticing the client has gone away (a close frame, EOF, or a real
 * read error) so its slot in g_build_log_ws_conns[] is freed up.
 */
static void handle_pkg_build_log_ws_event(struct conn *cc)
{
	unsigned char buf[512];
	ssize_t n;
	struct ws_frame frame;
	int pr;

	for (;;) {
		n = tls_read(cc->fd, buf, sizeof(buf));
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n <= 0)
			break;
		if (ws_conn_feed(&cc->ws, buf, (size_t)n) != 0)
			break;

		for (;;) {
			pr = ws_conn_try_parse(&cc->ws, &frame);
			if (pr == 0)
				break;
			if (pr < 0)
				goto gone;
			if (frame.opcode == WS_OPCODE_CLOSE) {
				ws_conn_consume(&cc->ws, frame.frame_len);
				goto gone;
			}
			if (frame.opcode == WS_OPCODE_PING)
				ws_write_frame(cc->fd, WS_OPCODE_PONG, frame.payload, frame.payload_len);
			ws_conn_consume(&cc->ws, frame.frame_len);
		}

		if (cc->ssl == NULL || SSL_pending(cc->ssl) <= 0)
			return;
	}

gone:
	build_log_ws_detach(cc);
	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	ws_conn_free(&cc->ws);
	close(cc->fd);
	free(cc);
}

static void handle_console_ws_event(struct conn *cc)
{
	unsigned char buf[4096];
	ssize_t n;
	struct ws_frame frame;
	int pr;

	for (;;) {
		n = tls_read(cc->fd, buf, sizeof(buf));
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return; /* no more data ready this event -- wait for the next one */
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
				    kx_write_all(cc->exec_session->pty_conn->fd, frame.payload, frame.payload_len) !=
				        0) {
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

		/* Same real, non-blocking-TLS reason handle_client_event() has
		 * to re-check this: a TLS connection's kernel socket buffer
		 * being empty doesn't mean OpenSSL has no more decrypted data
		 * already buffered internally -- level-triggered epoll won't
		 * fire again on its own in that case. */
		if (cc->ssl == NULL || SSL_pending(cc->ssl) <= 0)
			return;
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

/*
 * A failed TLS handshake or SSL_new()/SSL_set_fd() is an entirely
 * ordinary, expected occurrence on a box whose HTTPS listener serves a
 * self-signed cert by default (ADR-0046's own PKI CA) -- any client
 * that hasn't been given that CA to trust rejects the handshake and
 * sends a real, correctly-formed TLS alert (most commonly 46,
 * "certificate unknown") before disconnecting; a stray non-TLS client
 * hitting the HTTPS port produces a similar, equally benign failure.
 * ERR_print_errors_fp(stderr) used to dump the *entire* raw OpenSSL
 * error queue (often several lines) straight to this daemon's own
 * stderr on every single one of these -- harmless on a dev box, but a
 * real problem on an installed one, where stderr is the physical/
 * serial console: confirmed live, a client repeatedly probing the
 * HTTPS port with an untrusted cert floods the console non-stop with
 * multi-line OpenSSL traces for something that isn't an error an
 * operator can or needs to act on. Routed through logstore instead
 * (source "kanxeod", "warning") as a single summary line per failure,
 * discoverable via the API like everything else, never spamming the
 * console; still drains the whole error queue (ERR_get_error() in a
 * loop) so it can't silently accumulate across repeated failures --
 * only the first (most specific) reason is actually logged.
 *
 * peer_ip (ADR-0134) is logged alongside the reason -- originally
 * absent, closing a real, confirmed gap: a sustained flood of these
 * warnings from one source had no way to identify which client it was
 * coming from.
 *
 * should_log (found live, again: a legitimate desktop's own browser
 * repeatedly failing against an untrusted self-signed cert flooded the
 * log store at 10+ lines/sec, well below any real block threshold)
 * lets a caller suppress the actual logstore_write() -- via
 * connthrottle_should_log_failure()'s own per-source rate limit --
 * while this function still always drains the full OpenSSL error
 * queue below regardless, so a suppressed line never lets errors
 * silently accumulate.
 */
static void log_tls_error(const char *context, const char *peer_ip, int should_log)
{
	unsigned long first = ERR_get_error();
	unsigned long e;

	if (first != 0 && should_log)
		logstore_write("kanxeod", "warning", "%s from %s: %s", context, peer_ip,
		                ERR_reason_error_string(first));
	while ((e = ERR_get_error()) != 0)
		; /* drain the rest of the queue silently -- see this function's own comment */
}

/*
 * Shared teardown for a CONN_CLIENT conn, from any of handle_client_
 * event()'s several exit points -- consolidated here (Part 0.5) since
 * TLS cleanup (tls_unregister()/SSL_free()) needs to happen at every
 * one of them, not just some.
 */
static void client_conn_teardown(struct conn *cc)
{
	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	if (cc->ssl != NULL) {
		tls_unregister(cc->fd);
		SSL_free(cc->ssl);
	}
	close(cc->fd);
	http_conn_free(&cc->http);
	free(cc);
}

/*
 * Drives cc's still-in-progress TLS handshake one step (Part 0.5) --
 * called instead of an ordinary HTTP read whenever cc->ssl != NULL and
 * the handshake hasn't finished yet. A non-blocking SSL_accept() can
 * legitimately need several round trips (WANT_READ: wait for the next
 * EPOLLIN and try again -- no epoll interest change needed; WANT_WRITE:
 * the one case this daemon's otherwise-EPOLLIN-only client connections
 * ever need EPOLLOUT too, so the epoll registration is widened, then
 * narrowed back down once the handshake actually completes). Returns
 * 1 once the handshake has completed (caller should fall through to
 * ordinary request processing in the same event, since SSL_pending()
 * may already have buffered application data no future epoll wakeup
 * is guaranteed to announce -- see the real, well-known non-blocking-
 * TLS gotcha this guards against), 0 if still in progress (caller
 * should simply return and wait for the next event), or -1 on a
 * genuine handshake failure (caller should tear down).
 */
static int client_conn_advance_handshake(struct conn *cc)
{
	int r;
	int err;
	struct kx_epoll_event ev;

	r = SSL_accept(cc->ssl);
	if (r == 1) {
		connthrottle_record_success(cc->peer_ip);
		return 1;
	}

	err = SSL_get_error(cc->ssl, r);
	if (err == SSL_ERROR_WANT_READ)
		return 0;
	if (err == SSL_ERROR_WANT_WRITE) {
		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN | EPOLLOUT;
		ev.data.ptr = cc;
		kx_epoll_ctl(g_epfd, EPOLL_CTL_MOD, cc->fd, &ev);
		return 0;
	}
	log_tls_error("https handshake failed", cc->peer_ip, connthrottle_should_log_failure(cc->peer_ip));
	connthrottle_record_failure(cc->peer_ip);
	return -1;
}

static void handle_client_event(struct conn *cc)
{
	char buf[4096];
	ssize_t n;
	struct http_request req;
	int pr;

	if (cc->ssl != NULL && !SSL_is_init_finished(cc->ssl)) {
		int hr = client_conn_advance_handshake(cc);

		if (hr < 0) {
			client_conn_teardown(cc);
			return;
		}
		if (hr == 0)
			return; /* still mid-handshake -- wait for the next event */

		/* Just completed: narrow epoll interest back down to plain
		 * EPOLLIN if client_conn_advance_handshake() had widened it
		 * for a WANT_WRITE step above -- every ordinary CONN_CLIENT
		 * conn is EPOLLIN-only otherwise. Falls through to read
		 * application data immediately below, not on the next event
		 * (SSL_pending() may already be nonzero). */
		{
			struct kx_epoll_event ev;

			memset(&ev, 0, sizeof(ev));
			ev.events = EPOLLIN;
			ev.data.ptr = cc;
			kx_epoll_ctl(g_epfd, EPOLL_CTL_MOD, cc->fd, &ev);
		}
	}

	for (;;) {
		n = tls_read(cc->fd, buf, sizeof(buf));
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return; /* no more data ready this event -- wait for the next one */
		if (n <= 0) {
			client_conn_teardown(cc);
			return;
		}

		if (http_conn_feed(&cc->http, buf, (size_t)n) != 0) {
			respond_error(cc->fd, 400, "Bad Request", "request too large");
			client_conn_teardown(cc);
			return;
		}

		pr = http_conn_try_parse(&cc->http, &req);
		if (pr < 0) {
			respond_error(cc->fd, 400, "Bad Request", "malformed request");
			client_conn_teardown(cc);
			return;
		}
		if (pr == 1) {
			enum console_route_result cr;

			/* A complete, well-formed HTTP request was actually
			 * received -- real evidence this source isn't currently
			 * misbehaving, on whichever listener it arrived on, so it
			 * resets the same failure count a bad TLS handshake would
			 * have raised (ADR-0134). Deliberately independent of what
			 * this specific request dispatches to (even a 404 still
			 * proves the client speaks HTTP correctly). */
			connthrottle_record_success(cc->peer_ip);

			cr = try_console_upgrade(cc, &req);

			if (cr == CONSOLE_HANDLED)
				return; /* cc repurposed into CONN_CONSOLE_WS (or already torn down) -- must not be touched again */
			if (cr == CONSOLE_NOT_MATCHED)
				cr = try_pkg_build_log_upgrade(cc, &req);
			if (cr == CONSOLE_HANDLED)
				return; /* cc repurposed into CONN_PKG_BUILD_LOG_WS -- must not be touched again */
			if (cr == CONSOLE_NOT_MATCHED)
				dispatch(cc->fd, &req);
			/* CONSOLE_FAILED: an error response (or nothing, if the
			 * client was already gone) was already written by
			 * whichever upgrade attempt failed -- cc still needs the
			 * same teardown every other handled request gets. */
			client_conn_teardown(cc);
			return;
		}
		/* pr == 0: request incomplete so far. A plain-fd connection
		 * always stops here and waits for the next EPOLLIN (matches
		 * this daemon's existing, unchanged behavior). A TLS
		 * connection additionally re-checks SSL_pending(): OpenSSL may
		 * have already buffered more decrypted application data
		 * internally than fit in one read (or the client's whole
		 * request rode in on the same TCP segment as the final
		 * handshake message above) -- the kernel socket buffer being
		 * empty in that case means level-triggered epoll will *not*
		 * fire again on its own, so that data has to be drained now,
		 * not assumed to arrive via a future wakeup that may never
		 * come. */
		if (cc->ssl == NULL || SSL_pending(cc->ssl) <= 0)
			return;
	}
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
		int follow_rolling;
		int has_follow_rolling_jitter, follow_rolling_jitter_seconds;
		char err_msg[256];
		int status = create_container_from_body(
		    def->body, def->body_len, &entry, restart_policy, &restart_delay_seconds, depends_on,
		    &depends_on_count, &has_readiness, &readiness_tcp_port, &readiness_timeout_seconds,
		    &follow_rolling, &has_follow_rolling_jitter, &follow_rolling_jitter_seconds, err_msg,
		    sizeof(err_msg));

		if (status != 0)
			fprintf(stderr, "%s: restart failed: %s\n", cc->restart_name, err_msg);
		/* else: create_container_from_body() already registered its
		 * own pidfd -- no separate call needed here either. */
	}

	free(cc);
}

/*
 * Part 5 (ADR-0124): arm_restart_timer()'s own shape, verbatim, but a
 * distinct conn kind -- see CONN_ROLLING_RESTART_TIMER's own comment
 * for why this can't just reuse arm_restart_timer() itself: that one's
 * paired handler assumes the container has already exited, this one's
 * own handler (below) must stop a still-live one first.
 *
 * delay_seconds == 0 is a real, intentional input here (an operator
 * explicitly disabling jitter, containerdef_jitter_window_get() == 0)
 * -- unlike arm_restart_timer()'s own delay, always a validated 1-300
 * from restart_delay_seconds, which can never be 0. timerfd_settime(2)
 * treats an all-zero it_value as "disarm this timer," not "fire
 * immediately," so a literal 0 here would silently never fire at all;
 * it_value.tv_nsec is set to 1 in that case, the smallest interval
 * that still counts as armed.
 */
static void arm_rolling_restart_timer(const char *name, int delay_seconds)
{
	int tfd;
	struct itimerspec its;
	struct conn *cc;
	struct kx_epoll_event ev;

	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (tfd < 0) {
		perror("timerfd_create (rolling restart)");
		return;
	}

	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = delay_seconds;
	if (delay_seconds <= 0)
		its.it_value.tv_nsec = 1;
	if (timerfd_settime(tfd, 0, &its, NULL) != 0) {
		perror("timerfd_settime (rolling restart)");
		close(tfd);
		return;
	}

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (rolling restart timer conn)");
		close(tfd);
		return;
	}
	cc->kind = CONN_ROLLING_RESTART_TIMER;
	cc->fd = tfd;
	snprintf(cc->restart_name, sizeof(cc->restart_name), "%s", name);

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, tfd, &ev) != 0) {
		perror("epoll_ctl ADD rolling restart timer");
		close(tfd);
		free(cc);
	}
}

/*
 * Fires once arm_rolling_restart_timer()'s jittered delay elapses.
 * def->stopped is checked exactly like handle_restart_timer_event()'s
 * own (an explicit POST .../stop during the jitter window wins, same
 * ADR-0027 precedent) and containerdef_find() returning NULL is the
 * same real, correct "already deleted during the delay window" case
 * too. The one genuine difference from that function: the container is
 * expected to still be live here (apply_rolling_container_restarts()
 * only ever arms this timer for one it just found running), so it must
 * be stopped -- the exact registry_remove()-plus-reactor-conn-teardown
 * primitive handle_stop() uses, minus that handler's own containerdef_
 * set_stopped() call (deliberately not marking it "stopped": this is a
 * restart, not an operator-requested stop, and the very next step
 * below immediately replays it) -- before create_container_from_body()
 * can safely reuse its name. By the time this fires, apply_rolling_
 * container_restarts() has already patched def's own body with the new
 * pinned image_version (before arming the timer), so the replay below
 * picks it up automatically, the same way every other consumer of a
 * persisted body already does.
 */
static void handle_rolling_restart_timer_event(struct conn *cc)
{
	struct container_def *def;
	uint64_t expirations;

	if (read(cc->fd, &expirations, sizeof(expirations)) < 0)
		perror("read (rolling restart timerfd)");

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	close(cc->fd);

	def = containerdef_find(cc->restart_name);
	if (def != NULL && !def->stopped) {
		struct registry_entry *live = registry_find(cc->restart_name);

		if (live != NULL) {
			if (live->reactor_conn != NULL) {
				struct conn *rc = live->reactor_conn;

				kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, rc->fd, NULL);
				free(rc);
				live->reactor_conn = NULL;
			}
			registry_remove(cc->restart_name);
		}
		/* else: an explicit DELETE/stop already took it down during the
		 * jitter delay -- def->stopped above already re-checked for the
		 * stop case; a delete would have removed def entirely, already
		 * handled by the containerdef_find() == NULL branch. Either way,
		 * falling through to replay the (already-patched) definition
		 * below is correct: it just becomes an ordinary restart of a
		 * currently-stopped-but-defined container. */

		{
			struct registry_entry *entry;
			char restart_policy[16];
			int restart_delay_seconds;
			char depends_on[CONTAINERDEF_MAX_DEPENDS][REGISTRY_NAME_MAX];
			int depends_on_count;
			int has_readiness, readiness_tcp_port, readiness_timeout_seconds;
			int follow_rolling;
			int has_follow_rolling_jitter, follow_rolling_jitter_seconds;
			char err_msg[256];
			int status = create_container_from_body(
			    def->body, def->body_len, &entry, restart_policy, &restart_delay_seconds,
			    depends_on, &depends_on_count, &has_readiness, &readiness_tcp_port,
			    &readiness_timeout_seconds, &follow_rolling, &has_follow_rolling_jitter,
			    &follow_rolling_jitter_seconds, err_msg, sizeof(err_msg));

			if (status != 0)
				fprintf(stderr, "%s: rolling restart failed: %s\n", cc->restart_name, err_msg);
		}
	}

	free(cc);
}

/*
 * Part 5 (ADR-0124): reads 4 bytes from /dev/urandom (same idiom
 * ldap_generate_secret() already established, daemon/src/ldap.c -- no
 * new randomness-source precedent) and reduces modulo (window + 1),
 * giving a uniform [0, window] jittered delay. window == 0 (an
 * operator explicitly wants no jitter) is handled by the caller never
 * invoking this at all, not by this function -- see its own call site.
 */
static int rolling_jitter_seconds(int window)
{
	unsigned char raw[4];
	int fd;
	ssize_t n;
	size_t total = 0;
	uint32_t v;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return 0;
	while (total < sizeof(raw)) {
		n = read(fd, raw + total, sizeof(raw) - total);
		if (n <= 0) {
			close(fd);
			return 0;
		}
		total += (size_t)n;
	}
	close(fd);

	v = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | raw[3];
	return (int)(v % (uint32_t)(window + 1));
}

/*
 * Part 5 (ADR-0124): the reconciliation pass this whole feature hinges
 * on -- see try_start_queued_pkg_rebuild()'s own updated comment for
 * why this runs from the exact same "a pkg job might have just
 * finished" hook rather than pkg.c threading a bespoke "this image
 * changed" signal back into main.c. Deliberately generic over *why* an
 * image's current_version moved (a rolling auto-rebuild, an operator's
 * plain `pkg install` against a rolling-manifested image, or Part 4's
 * own image-recipe artifact-tier apply) -- image_current_version() is
 * the one source of truth this checks against, not a duplicate signal
 * threaded through each of those call paths individually.
 *
 * containerdef_resolve_order() is reused purely as a "list every
 * currently-defined container name" enumerator here -- its own
 * dependency-order guarantee is irrelevant to this pass (each
 * container's own rolling-restart is independent and independently
 * jittered), but it's already the one function that does this
 * enumeration, and inventing a second one just for this would violate
 * One Source of Truth for no benefit.
 */
static void apply_rolling_container_restarts(void)
{
	char order[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	int count = containerdef_resolve_order(order);
	int i;

	for (i = 0; i < count; i++) {
		struct container_def *def = containerdef_find(order[i]);
		struct json_value *root;
		const char *image;
		const char *pinned_version;
		char current_version[IMAGE_VERSION_MAX];

		if (def == NULL || !def->follow_rolling)
			continue;

		root = json_parse(def->body, def->body_len);
		if (root == NULL)
			continue;
		image = json_as_string(json_object_get(root, "image"));
		pinned_version = json_as_string(json_object_get(root, "image_version"));
		if (image == NULL || pinned_version == NULL || image_current_version(image, current_version,
		                                                                      sizeof(current_version)) != 0) {
			json_free(root);
			continue;
		}

		if (strcmp(pinned_version, current_version) != 0) {
			/* Part 5 follow-up: this container's own explicit
			 * follow_rolling_jitter_seconds, if it set one, otherwise
			 * the daemon-wide default -- same override-over-default
			 * shape restart_delay_seconds's own base value has, just
			 * one level up (a per-container knob over a daemon-wide
			 * one, not a per-container knob over a hardcoded constant). */
			int jitter_window = def->has_follow_rolling_jitter ? def->follow_rolling_jitter_seconds
			                                                    : containerdef_jitter_window_get();
			int delay = jitter_window > 0 ? rolling_jitter_seconds(jitter_window) : 0;

			if (containerdef_patch_image_version(order[i], current_version) == 0) {
				if (registry_find(order[i]) != NULL)
					arm_rolling_restart_timer(order[i], delay);
				/* else: stopped-but-defined -- the patched pin alone is
				 * enough; nothing live to disrupt, and its next start
				 * already replays the patched body. */
			} else {
				fprintf(stderr, "%s: failed to patch rolling image_version pin\n", order[i]);
			}
		}
		json_free(root);
	}
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
	char hostbuild_done_name[PKG_NAME_MAX];

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
	chained = pkg_build_completed(entry->name, entry->exit_status, &pkg_pid, &pkg_pidfd,
	                               hostbuild_done_name);
	if (strcmp(entry->name, PKG_BUILD_CONTAINER_NAME) == 0)
		registry_remove(entry->name);
	if (chained)
		register_pkg_fetch_pidfd(pkg_pid, pkg_pidfd);
	else
		try_start_queued_pkg_rebuild();

	/*
	 * ADR-0057: "kanxeo" is the one hostbuild name this daemon gives
	 * any further meaning to -- everything else pkg.c handles is
	 * completely generic. Deliberately a plain string match here in
	 * main.c, not a flag/callback registered in pkg.c itself: pkg.c
	 * stays fully agnostic to what any package *means*.
	 */
	if (hostbuild_done_name[0] != '\0' && strcmp(hostbuild_done_name, "kanxeo") == 0) {
		char artifact_dir[PATH_MAX];

		snprintf(artifact_dir, sizeof(artifact_dir), "%s/%s", ARTIFACTS_DIR, hostbuild_done_name);
		spawn_kanxeo_bootroot_assembly(artifact_dir);
	}

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
	int stdio_write_fd;

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	if (waitpid(cc->pkg_fetch_pid, &status, 0) == cc->pkg_fetch_pid && WIFEXITED(status))
		exit_status = WEXITSTATUS(status);
	else
		exit_status = -1;
	close(cc->fd);
	free(cc);

	if (pkg_fetch_completed(exit_status, &spec, &stdio_write_fd)) {
		struct registry_entry *entry;
		enum registry_error rerr =
		    registry_create(PKG_BUILD_CONTAINER_NAME, "pkgbuild", "", &spec, NULL, 0, 0, NULL, 0,
		                     NULL, 0, NULL, NULL, 0, &entry);

		/*
		 * The child (if registry_create() actually forked one)
		 * already inherited its own copy of the write end via
		 * clone3 -- this is the daemon's own now-redundant copy,
		 * closed immediately regardless of outcome so a failed
		 * spawn doesn't leak it.
		 */
		if (stdio_write_fd >= 0)
			close(stdio_write_fd);

		if (rerr != REGISTRY_OK) {
			pkg_build_spawn_failed();
			try_start_queued_pkg_rebuild();
		} else {
			register_container_pidfd(entry);
			register_pkg_build_output(pkg_build_output_fd());
		}
	} else {
		try_start_queued_pkg_rebuild();
	}
}

/*
 * Reaps spawn_kanxeo_bootroot_assembly()'s own mkbootroot child.
 * Nothing further to dispatch on completion -- either
 * <artifact_dir>/kanxeod-root.squashfs now exists (success, ready for
 * `pkg hostbuild kanxeo --deploy` to pick up) or it doesn't (logged
 * failure, the hostbuild's own artifacts are still there to retry
 * from). No REST response is waiting on this -- the original POST
 * /v1/pkg/hostbuild already returned 202 long before this fires.
 *
 * logstore_write() alongside the original fprintf(stderr, ...) --
 * confirmed the hard way (this exact code path) that stderr-only
 * reporting here is a real, REST-invisible gap: nothing about this
 * assembly's own success/failure ever reached GET /system/logs, so an
 * operator with no serial console/SSH access (this project's own real
 * deployment target) had no way to tell a failed assembly from one
 * still running. WEXITSTATUS/WTERMSIG included so a real failure's
 * *reason* (not just "it failed") is visible remotely too.
 */
static void handle_bootroot_assemble_event(struct conn *cc)
{
	int status;
	pid_t reaped;
	char output[BOOTROOT_OUTPUT_CAPTURE_MAX + 1];
	ssize_t output_len;

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	reaped = waitpid(cc->pkg_fetch_pid, &status, 0);
	/*
	 * ADR-0087: g_bootroot_output_captured has already been
	 * incrementally filled by bootroot_output_readable() (see its own
	 * comment) as mkbootroot ran, via its own directly-registered
	 * epoll conn (CONN_BOOTROOT_OUTPUT) -- NOT read here in one shot,
	 * which is what used to deadlock mkbootroot itself the moment its
	 * combined output exceeded the pipe's 64KB kernel buffer. One
	 * last non-blocking drain catches anything written in the brief
	 * window between mkbootroot's own final write() and its exit (by
	 * the time waitpid() confirms the exit, the kernel has already
	 * torn down its fd table, so this either returns real data still
	 * sitting in the pipe or immediate EOF, never blocks). Read
	 * regardless of exit status: real, useful stdout (e.g. a progress
	 * line) isn't exclusively a failure-path signal, though only the
	 * failure branches below actually log it.
	 */
	bootroot_output_readable();
	output_len = g_bootroot_output_captured_len;
	memcpy(output, g_bootroot_output_captured, output_len);
	output[output_len] = '\0';
	/*
	 * task #737: only a real, confirmed success ever advances the
	 * completed generation -- at most one assembly is ever in flight
	 * (only triggered by the "kanxeo" hostbuild's own single-job-
	 * constrained completion event), so g_bootroot_assembly_started's
	 * current value is unambiguously *this* attempt's own generation
	 * number at the moment it resolves, whichever way it resolves.
	 */
	g_bootroot_assembly_running = 0;
	if (reaped == cc->pkg_fetch_pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		g_bootroot_assembly_completed = g_bootroot_assembly_started;
		fprintf(stderr, "kanxeo bootroot assembly: succeeded\n");
		logstore_write("kanxeod", "info", "kanxeo bootroot assembly: succeeded");
	} else if (reaped != cc->pkg_fetch_pid) {
		fprintf(stderr, "kanxeo bootroot assembly: waitpid failed\n");
		logstore_write("kanxeod", "error", "kanxeo bootroot assembly: waitpid failed: %s",
		                strerror(errno));
	} else if (WIFEXITED(status)) {
		fprintf(stderr, "kanxeo bootroot assembly: failed (exit %d)\n", WEXITSTATUS(status));
		logstore_write("kanxeod", "error", "kanxeo bootroot assembly: mkbootroot exited %d",
		                WEXITSTATUS(status));
		if (output_len > 0)
			logstore_write("kanxeod", "error", "kanxeo bootroot assembly: output: %s", output);
	} else if (WIFSIGNALED(status)) {
		fprintf(stderr, "kanxeo bootroot assembly: killed by signal %d\n", WTERMSIG(status));
		logstore_write("kanxeod", "error", "kanxeo bootroot assembly: mkbootroot killed by signal %d",
		                WTERMSIG(status));
		if (output_len > 0)
			logstore_write("kanxeod", "error", "kanxeo bootroot assembly: output: %s", output);
	} else {
		fprintf(stderr, "kanxeo bootroot assembly: failed\n");
		logstore_write("kanxeod", "error", "kanxeo bootroot assembly: failed");
	}
	close(cc->fd);
	free(cc);
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

/*
 * listener is either &g_listener_conn (plain HTTP) or &g_https_
 * listener_conn (Part 0.5) -- both accept4() loops are identical
 * except that a connection accepted on the HTTPS listener additionally
 * gets a fresh SSL* wrapped around it (SSL_accept() itself happens
 * later, driven from handle_client_event() the same way ordinary HTTP
 * request parsing is already driven from there -- non-blocking, one
 * epoll wakeup at a time, never a blocking call in this loop).
 */
static void accept_loop(struct conn *listener)
{
	int client_fd;
	struct conn *cc;
	struct kx_epoll_event ev;
	int is_tls = (listener->kind == CONN_LISTENER_TLS);
	struct sockaddr_in peer_addr;
	socklen_t peer_len;
	char peer_ip[CONNTHROTTLE_IP_MAX];

	for (;;) {
		peer_len = sizeof(peer_addr);
		client_fd = accept4(listener->fd, (struct sockaddr *)&peer_addr, &peer_len,
		                     SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (client_fd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			perror("accept4");
			break;
		}

		if (inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip, sizeof(peer_ip)) == NULL)
			snprintf(peer_ip, sizeof(peer_ip), "?");

		/* A source already blocked for repeated failed HTTPS
		 * handshakes (ADR-0134) is refused as cheaply as possible --
		 * before any malloc/SSL_new -- but only on the HTTPS listener
		 * itself (ADR-0137): only a TLS handshake failure ever counts
		 * toward the block in the first place (connthrottle_record_
		 * failure() is only ever called from the HTTPS handshake path),
		 * so enforcing it against the plain HTTP listener too achieves
		 * nothing the block is actually for and, found live, can strand
		 * an otherwise-reachable admin path behind an untrusted-cert
		 * client's own repeated TLS failures. */
		if (is_tls && connthrottle_should_block(peer_ip)) {
			close(client_fd);
			continue;
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
		cc->ssl = NULL;
		snprintf(cc->peer_ip, sizeof(cc->peer_ip), "%s", peer_ip);
		http_conn_init(&cc->http);

		if (is_tls) {
			cc->ssl = SSL_new(g_tls_ctx);
			if (cc->ssl == NULL || SSL_set_fd(cc->ssl, client_fd) != 1) {
				/* A local resource failure (SSL_new()/SSL_set_fd()),
				 * never the peer's own doing -- logged with its IP for
				 * context, but deliberately not counted toward that
				 * peer's own throttle score (connthrottle_record_
				 * failure() is reserved for a real handshake the peer
				 * actually attempted and failed). */
				log_tls_error("https accept failed", peer_ip, 1);
				if (cc->ssl != NULL)
					SSL_free(cc->ssl);
				close(client_fd);
				free(cc);
				continue;
			}
			SSL_set_accept_state(cc->ssl);
			tls_register(client_fd, cc->ssl);
		}

		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN;
		ev.data.ptr = cc;
		if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, client_fd, &ev) != 0) {
			perror("epoll_ctl ADD client");
			if (cc->ssl != NULL) {
				tls_unregister(client_fd);
				SSL_free(cc->ssl);
			}
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
		int follow_rolling;
		int has_follow_rolling_jitter, follow_rolling_jitter_seconds;
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
		                                     &readiness_timeout_seconds, &follow_rolling,
		                                     &has_follow_rolling_jitter, &follow_rolling_jitter_seconds,
		                                     err_msg, sizeof(err_msg));
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
	int port_explicit = 0; /* --port= was actually passed on argv -- see
	                         * daemon_config_port()'s override below */
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
	struct kx_epoll_event ev;
	struct sigaction sa;

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--port=", 7) == 0) {
			port = atoi(argv[i] + 7);
			port_explicit = 1;
		}
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
	tls_init();
	g_web_root = web_root;
	g_slot = slot;
	g_bind_addr = bind_addr;
	/* g_port is finalized later, once daemon_config_init() has had a
	 * chance to apply a persisted port override (Part 0.5) -- nothing
	 * between here and there reads it. */

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

	migrate_flat_layout_to_grouped();
	migrate_diskroles_out_of_state_dir();

	/*
	 * ADR-0141 Phase 2: g_base_dir and DISKS_MOUNT_DIR only, first --
	 * diskrole_init()/diskformat_remount_present_role_disks()/
	 * resolve_state_storage_placement() below need DISKS_MOUNT_DIR to
	 * already exist (so a disk can actually be mounted under it) and
	 * must themselves run before STATE_DIR's real, possibly-relocated
	 * value is known -- everything else's ensure_dir() moves below
	 * that resolution instead of staying bundled in one block the way
	 * it was before this phase.
	 */
	if (ensure_dir(g_base_dir) != 0 || ensure_dir(DISKS_MOUNT_DIR) != 0)
		return 1;
	if (diskrole_init(DISKROLE_STATE_PATH) != 0)
		return 1;
	/* ADR-0142: recover any role-assigned disk this box already
	 * formatted successfully (possibly in a prior daemon lifetime) but
	 * that isn't currently mounted -- diskformat.c's own job state is
	 * purely in-memory and forgotten across every restart, even though
	 * the real mount doesn't need to be. Best-effort, never fatal to
	 * startup. Must run before resolve_state_storage_placement() below,
	 * which depends on state-storage's own configured disk (if any)
	 * already being mounted by the time it checks. */
	diskformat_remount_present_role_disks(CONTAINERS_DIR, DISKS_MOUNT_DIR);
	if (storageplacement_init(STORAGE_PLACEMENT_PATH) != 0)
		return 1;
	if (resolve_state_storage_placement() != 0)
		return 1;
	if (resolve_log_storage_placement() != 0)
		return 1;
	if (resolve_rebuildable_storage_placement() != 0)
		return 1;

	/* STATE_DIR/REBUILDABLE_DIR/LOG_DIR themselves first (ADR-0141) --
	 * ensure_dir() is a plain single-level mkdir(), not mkdir -p, so
	 * every path nested under any of them below would otherwise fail
	 * with ENOENT on a genuinely fresh install (nothing for migrate_
	 * flat_layout_to_grouped() to have created them via already, since
	 * there was nothing at the old flat paths to migrate). Each of the
	 * three may be a relocated disk's own mount path (resolve_state_
	 * storage_placement()/resolve_log_storage_placement()/resolve_
	 * rebuildable_storage_placement() above) -- either way it already
	 * exists as a real, mounted directory by this point; ensure_dir()
	 * only needs to create the leaf subdirectory under it. */
	if (ensure_dir(STATE_DIR) != 0 ||
	    ensure_dir(REBUILDABLE_DIR) != 0 || ensure_dir(IMAGES_DIR) != 0 ||
	    ensure_dir(CONTAINERS_DIR) != 0 || ensure_dir(PKI_DIR) != 0 ||
	    ensure_dir(PKI_CERTS_DIR) != 0 || ensure_dir(PKG_DIR) != 0 ||
	    ensure_dir(ARTIFACTS_DIR) != 0 || ensure_dir(SIGNING_KEYS_DIR) != 0 ||
	    ensure_dir(ISO_DIR) != 0 || ensure_dir(SWAP_DIR) != 0 || ensure_dir(LOG_DIR) != 0)
		return 1;

	if (network_init(NETWORKS_STATE_PATH) != 0)
		return 1;
	/*
	 * Root-netns net.ipv4.ip_forward -- distinct from struct
	 * container_spec's own per-container ip_forward field (which
	 * governs a *container's own* netns, for a container acting as a
	 * router between two of its own attached networks, e.g. Phase
	 * 24's BIRD/keepalived pairs). This one governs the *host's own*
	 * root netns, which every ordinary (non-router) container's
	 * default route already points at (see ADR-0067: a network's own
	 * "address" is deliberately the target containers route through)
	 * -- without it, any container whose destination isn't the host
	 * itself or another peer already on the same bridge has its
	 * traffic silently dropped the moment it reaches the host's own
	 * IP stack. Confirmed live as a real, previously-undiscovered gap
	 * (ADR-0089): a container could reach the daemon's own management
	 * address but not the real upstream LAN gateway one hop further,
	 * even though both are equally reachable over the same bridge at
	 * the link layer -- nothing in this codebase had ever enabled
	 * root-netns forwarding, because no container before dns-1/dns-2
	 * had ever needed to originate a connection leaving the host at
	 * all (every prior "container needs the internet" case, package
	 * installs, was always a host-side curl, never container-
	 * initiated).
	 */
	{
		int fwd_fd;
		static const char one[] = "1\n";

		fwd_fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY);
		if (fwd_fd < 0 || write(fwd_fd, one, sizeof(one) - 1) != (ssize_t)(sizeof(one) - 1)) {
			perror("enable root-netns ip_forward");
			return 1;
		}
		close(fwd_fd);
	}
	/* Part 3: real hardware's NIC driver (needed for the interface
	 * attach below) may be a module -- must run before bootstrap_
	 * management_network(), not after. Same init_mode gate: a plain/
	 * test invocation has no real /usr/bin/modprobe to run at all. */
	if (init_mode)
		load_boot_modules();
	/* Only a real --init-mode boot has a GRUB-supplied net.conf to
	 * bootstrap from (Part 0.5) -- a plain/test invocation has no
	 * management network and simply keeps whatever --bind= it was given. */
	if (init_mode && bootstrap_management_network() != 0)
		return 1;
	if (daemon_config_init(DAEMON_CONFIG_PATH) != 0)
		return 1;
	/* A persisted port change (PUT /v1/system/daemon-config) survives a
	 * real reboot -- but an explicit --port= on argv (every test/dev
	 * invocation always passes one, to avoid colliding with other
	 * parallel test daemons) always wins over it. */
	if (!port_explicit && daemon_config_port() != 0)
		port = daemon_config_port();
	g_port = port;
	if (dns_init(DNS_RECORDS_STATE_PATH, DNS_SERVERS_STATE_PATH) != 0)
		return 1;
	if (ntp_init(NTP_STATE_PATH, NTP_SERVERS_STATE_PATH) != 0)
		return 1;
	if (syslogfwd_init(SYSLOGFWD_STATE_PATH) != 0)
		return 1;
	if (ldap_init(LDAP_SERVERS_STATE_PATH) != 0)
		return 1;
	if (ldap_record_init(LDAP_USERS_STATE_PATH, LDAP_GROUPS_STATE_PATH) != 0)
		return 1;
	if (ldap_config_init(LDAP_CONFIG_STATE_PATH) != 0)
		return 1;
	if (pki_init(PKI_DIR, PKI_CERTS_STATE_PATH) != 0)
		return 1;
	if (siteconfig_init(SITE_CONFIG_PATH) != 0)
		return 1;
	reconcile_instance_dns_record();
	if (pkg_init(PKG_DIR, PKG_INSTALLED_STATE_PATH, CONTAINERS_DIR, IMAGES_DIR, ARTIFACTS_DIR) != 0)
		return 1;
	if (pkg_repo_init(PKG_REPO_CONFIG_PATH) != 0)
		return 1;
	if (pkg_cache_init(PKG_CACHE_DIR, PKG_CACHE_CONFIG_PATH) != 0)
		return 1;
	if (pkg_artifact_init(PKG_ARTIFACT_CONFIG_PATH) != 0)
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
	if (containerdef_rolling_config_init(ROLLING_CONFIG_PATH) != 0)
		return 1;
	if (connthrottle_config_init(CONNTHROTTLE_CONFIG_PATH) != 0)
		return 1;
	if (backupconfig_init(BACKUP_CONFIG_PATH) != 0)
		return 1;
	if (hostauth_init(HOSTAUTH_CONFIG_PATH) != 0)
		return 1;
	if (devicemap_init(DEVICEMAP_STATE_PATH) != 0)
		return 1;
	/* diskrole_init()/diskformat_remount_present_role_disks() now run
	 * much earlier (ADR-0141 Phase 2) -- see the resolve_state_storage_
	 * placement() block above, which needs both before it can determine
	 * STATE_DIR's own real location. */
	if (quotamap_init(QUOTAMAP_STATE_PATH) != 0)
		return 1;
	if (swap_init(SWAP_STATE_PATH, SWAP_FILE_PATH) != 0)
		return 1;
	if (logstore_init(LOG_DIR, LOG_STATE_PATH) != 0)
		return 1;
	if (resolv_init(RESOLV_CONF_PATH) != 0)
		return 1;

	/*
	 * Best-effort, before any container's cgroup leaf can exist (see
	 * cgroup_enable_controllers()'s own comment for why "best-effort",
	 * why here, and why io/cpuset/memory/pids/cpu all need this) --
	 * without it, GET .../stats' disk.read_bytes/write_bytes/read_ios/
	 * write_ios stay 0 for every container, cpuset_cpus never takes
	 * effect, and on a real-PID-1 install with no systemd ever
	 * pre-delegating anything (confirmed live on 192.168.15.95),
	 * memory_max/pids_max/cpu_max fail container creation outright.
	 * Never a fatal startup condition.
	 */
	cgroup_enable_controllers();

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
	 * SOCK_CLOEXEC everywhere below: container_create()
	 * clone3()'s a new process for every container this daemon runs.
	 * Without close-on-exec, that child inherits a duplicate of every
	 * fd we hold open at the moment of the clone -- including the
	 * client connection socket for whatever request triggered the
	 * container's creation. The kernel won't deliver EOF on that
	 * connection to the client until *every* reference to it closes,
	 * so an un-CLOEXEC'd fd would silently stall that HTTP response
	 * until the container itself exits, defeating the entire
	 * non-blocking reactor. The container's own execve() closes any
	 * CLOEXEC fd immediately, well before it does real work. See
	 * create_listen_socket() for the socket/bind/listen sequence
	 * itself -- shared with rebind_listener()'s live-rebind path
	 * (Part 0.5), one source of truth for both.
	 */
	g_epfd = epoll_create1(EPOLL_CLOEXEC);
	if (g_epfd < 0) {
		perror("epoll_create1");
		return 1;
	}

	/*
	 * Part 0.5: http_enabled/https_enabled are each independently
	 * optional, but at least one must actually come up -- daemon_
	 * config.c's own setters already refuse a persisted state that
	 * would leave both off, so reaching this point with neither
	 * enabled would mean corrupted state, not a reachable operator
	 * choice; fail loudly rather than silently starting unreachable.
	 */
	g_listener_conn.fd = -1;
	g_https_listener_conn.fd = -1;
	if (daemon_config_http_enabled() && start_http_listener(bind_addr, port) != 0)
		return 1;
	if (daemon_config_https_enabled() && start_https_listener(bind_addr, daemon_config_https_port()) != 0)
		fprintf(stderr, "https_enabled but could not start the HTTPS listener -- continuing without it\n");
	listen_fd = g_listener_conn.fd;
	if (listen_fd < 0 && g_https_listener_conn.fd < 0) {
		fprintf(stderr, "no working listener (http and https both unavailable) -- refusing to start\n");
		return 1;
	}
	start_kmsg_watch(); /* needs g_epfd, only just created above -- best-effort, see its own comment */
	start_ntp_periodic_timer(); /* same g_epfd/best-effort posture, task #751 */
	start_pkg_sync_periodic_timer(); /* same posture, ADR-0121 -- no-op until an interval is configured */
	start_backup_periodic_timer(); /* same posture, ADR-0141 Phase 5 -- no-op until enabled+interval configured */
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

	/*
	 * Push the current record set to every dns_server_register()
	 * binding dns_init() loaded from disk -- at load time no container
	 * had started yet, so any binding whose container just came back
	 * up above is exactly the case dns_server_sync_all() itself
	 * documents needing a fresh call for (ADR-0091). A no-op if there
	 * are no persisted bindings (the common case on a box that's never
	 * run a DNS-serving container).
	 */
	dns_server_sync_all();

	/*
	 * The identical gap, for LDAP (ADR-0146): every registered LDAP
	 * server binding needs its own live config re-pushed here too, for
	 * the exact same reason dns_server_sync_all() above already has to
	 * be -- ldap_init() loaded the bindings themselves from disk before
	 * any container had started, and the server-side write in
	 * ldap_record_sync_all()/ldap_write_config_file() reaches through
	 * /proc/<pid>/root, which needs that container's pid to already be
	 * real. Without this, a freshly-restarted LDAP-serving container
	 * boots with an empty managed user/group section -- silently
	 * indistinguishable, from a login attempt's own perspective, from
	 * "every account was deleted," and a live-LDAP-backed admin login
	 * then gets a real, authoritative rejection instead of a stale-but-
	 * working one. Confirmed the hard way: this exact gap produced a
	 * genuine host-auth lockout on the real deployment the first time
	 * write-gating was activated against a host that had rebooted more
	 * than once since its last direct LDAP user/group mutation.
	 */
	ldap_record_sync_all();

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
			if (cc->kind == CONN_LISTENER || cc->kind == CONN_LISTENER_TLS)
				accept_loop(cc);
			else if (cc->kind == CONN_CONTAINER)
				handle_container_event(cc);
			else if (cc->kind == CONN_PKG_FETCH)
				handle_pkg_fetch_event(cc);
			else if (cc->kind == CONN_PKG_BUILD_OUTPUT)
				handle_pkg_build_output_event(cc);
			else if (cc->kind == CONN_CONTAINER_OUTPUT)
				handle_container_output_event(cc);
			else if (cc->kind == CONN_BOOTROOT_ASSEMBLE)
				handle_bootroot_assemble_event(cc);
			else if (cc->kind == CONN_BOOTROOT_OUTPUT)
				handle_bootroot_output_event(cc);
			else if (cc->kind == CONN_ISO_ASSEMBLE)
				handle_iso_assemble_event(cc);
			else if (cc->kind == CONN_BOOTSTRAP_FETCH)
				handle_bootstrap_fetch_event(cc);
			else if (cc->kind == CONN_PKG_SYNC_FETCH)
				handle_pkg_sync_fetch_event(cc);
			else if (cc->kind == CONN_PKG_SYNC_PERIODIC_TIMER)
				handle_pkg_sync_periodic_timer_event(cc);
			else if (cc->kind == CONN_BACKUP_PERIODIC_TIMER)
				handle_backup_periodic_timer_event(cc);
			else if (cc->kind == CONN_IMAGE_RECIPE_FETCH)
				handle_image_recipe_fetch_event(cc);
			else if (cc->kind == CONN_DISK_FORMAT)
				handle_disk_format_event(cc);
			else if (cc->kind == CONN_STORAGE_MIGRATE)
				handle_storage_migrate_event(cc);
			else if (cc->kind == CONN_CONTAINER_STORAGE_MIGRATE)
				handle_container_storage_migrate_event(cc);
			else if (cc->kind == CONN_PING)
				handle_ping_socket_event(cc);
			else if (cc->kind == CONN_PING_TIMER)
				handle_ping_timer_event(cc);
			else if (cc->kind == CONN_NTP_SYNC)
				handle_ntp_sync_socket_event(cc);
			else if (cc->kind == CONN_NTP_SYNC_TIMER)
				handle_ntp_sync_timer_event(cc);
			else if (cc->kind == CONN_NTP_PERIODIC_TIMER)
				handle_ntp_periodic_timer_event(cc);
			else if (cc->kind == CONN_RESTART_TIMER)
				handle_restart_timer_event(cc);
			else if (cc->kind == CONN_ROLLING_RESTART_TIMER)
				handle_rolling_restart_timer_event(cc);
			else if (cc->kind == CONN_BIND_IP_CLEANUP)
				handle_bind_ip_cleanup_timer_event(cc);
			else if (cc->kind == CONN_CONSOLE_SHELL)
				handle_console_shell_event(cc);
			else if (cc->kind == CONN_CONSOLE_RESPAWN_TIMER)
				handle_console_respawn_timer_event(cc);
			else if (cc->kind == CONN_CONSOLE_WS)
				handle_console_ws_event(cc);
			else if (cc->kind == CONN_CONSOLE_PTY)
				handle_console_pty_event(cc);
			else if (cc->kind == CONN_PKG_BUILD_LOG_WS)
				handle_pkg_build_log_ws_event(cc);
			else if (cc->kind == CONN_KMSG)
				handle_kmsg_event(cc);
			else
				handle_client_event(cc);
		}

		drain_pending_free();
	}

	if (listen_fd >= 0)
		close(listen_fd);
	if (g_https_listener_conn.fd >= 0) {
		close(g_https_listener_conn.fd);
		SSL_CTX_free(g_tls_ctx);
	}
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
