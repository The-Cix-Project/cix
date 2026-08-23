#include "connthrottle.h"
#include "container.h"
#include "containerdef.h"
#include "internal.h"
#include "device.h"
#include "devicemap.h"
#include "disk.h"
#include "diskformat.h"
#include "diskpart.h"
#include "diskrole.h"
#include "kmod.h"
#include "kmodconfig.h"
#include "sysctlconfig.h"
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
#include "subid.h"
#include "serverhealth.h"
#include "volume.h"
#include "volumebackup.h"
#include "cpreserve.h"
#include "pkgpolicy.h"
#include "bootconsole.h"
#include "kernelpolicy.h"
#include "zswap.h"
#include "dhcp.h"
#include "stallwatch.h"
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
#include <ftw.h>
#include <limits.h>
#include <linux/netlink.h>
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
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define DEFAULT_PORT 80
#define DEFAULT_BIND "127.0.0.1"
#define DEFAULT_WEB_ROOT "web"
#define DEFAULT_BASE_DIR "/var/lib/thinc"
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
 * host that also has a real thincd on default paths silently wipes
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
static char SUBID_STATE_PATH[PATH_MAX]; /* ADR-0179 */
static char SERVERHEALTH_STATE_PATH[PATH_MAX]; /* issue #81 -- drain flags only */
static char CPRESERVE_STATE_PATH[PATH_MAX]; /* issue #86 -- control-plane reservation */
static char PKGPOLICY_STATE_PATH[PATH_MAX];  /* issue #64 -- per-package rolling policy */
static char BOOTCONSOLE_STATE_PATH[PATH_MAX]; /* issue #24 -- boot console parameters */
static char KERNELPOLICY_STATE_PATH[PATH_MAX]; /* issue #65 -- which kernel line this box tracks */
static char KERNEL_RELEASES_PATH[PATH_MAX];    /* issue #65 -- cached kernel.org releases.json */
static char ZSWAP_STATE_PATH[PATH_MAX];        /* issue #51 -- compressed swap cache settings */
static char DHCP_STATE_PATH[PATH_MAX];         /* DHCP ranges and static reservations */
static char STALLWATCH_RECORDS_PATH[PATH_MAX]; /* issue #100 -- control-plane stall records */
static char VOLUMES_STATE_PATH[PATH_MAX];      /* issue #88 */
static char VOLUMES_DIR[PATH_MAX];             /* issue #88 -- where volume data lives */
static char VOLUME_BACKUP_CONFIG_PATH[PATH_MAX]; /* issue #96 -- the volume-snapshot schedule */
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
static char PKG_BUILD_CONFIG_PATH[PATH_MAX];      /* ADR-0157 Phase 3 */
/* Where a hostbuild job's own harvested output lands (ADR-0056) --
 * ARTIFACTS_DIR/<name>/..., a plain host directory, never a container-
 * visible path. */
static char ARTIFACTS_DIR[PATH_MAX];
static char CONTAINER_DEFS_STATE_PATH[PATH_MAX];
static char ROLLING_CONFIG_PATH[PATH_MAX]; /* ADR-0124 */
static char SITE_CONFIG_PATH[PATH_MAX];
static char DEVICEMAP_STATE_PATH[PATH_MAX];
static char SYSCTLCONFIG_STATE_PATH[PATH_MAX]; /* ADR-0160 */
static char KMODCONFIG_STATE_PATH[PATH_MAX]; /* ADR-0159 */
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
 * first time thincd itself, rather than a dev machine's own manual
 * mkinstalleriso invocation, has ever needed either. SIGNING_KEYS_DIR
 * is deliberately operator-populated out of band (never fetched,
 * generated, or copied here by thincd itself, and never staged onto
 * any container image or target-disk install) -- the same real
 * security posture image/keys/ already has in this repo: a release-
 * signing private key must never propagate onto every deployed box,
 * only the specific build/release instance actually cutting installer
 * media. A box with nothing at SIGNING_KEYS_DIR simply can't serve
 * this endpoint, exactly like `pkg hostbuild thinc` can't complete
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
/* Consolidated log store (kernel dmesg + thincd's own diagnostics +
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
	snprintf(SUBID_STATE_PATH, sizeof(SUBID_STATE_PATH), "%s/subid.json", STATE_DIR);
	snprintf(SERVERHEALTH_STATE_PATH, sizeof(SERVERHEALTH_STATE_PATH), "%s/server_health.json",
	         STATE_DIR);
	snprintf(VOLUMES_STATE_PATH, sizeof(VOLUMES_STATE_PATH), "%s/volumes.json", STATE_DIR);
	snprintf(CPRESERVE_STATE_PATH, sizeof(CPRESERVE_STATE_PATH),
	         "%s/control_plane_reservation.json", STATE_DIR);
	snprintf(PKGPOLICY_STATE_PATH, sizeof(PKGPOLICY_STATE_PATH), "%s/pkg_policies.json",
	         STATE_DIR);
	snprintf(BOOTCONSOLE_STATE_PATH, sizeof(BOOTCONSOLE_STATE_PATH), "%s/boot_console.json",
	         STATE_DIR);
	snprintf(KERNELPOLICY_STATE_PATH, sizeof(KERNELPOLICY_STATE_PATH), "%s/kernel_policy.json",
	         STATE_DIR);
	/*
	 * The fetched releases.json is cached on disk rather than in
	 * memory alone: a box that has resolved its channel once can still
	 * answer "how far behind am I" after a restart, and after a
	 * reboot, without needing the network to be up first.
	 */
	snprintf(KERNEL_RELEASES_PATH, sizeof(KERNEL_RELEASES_PATH), "%s/kernel_releases.json",
	         STATE_DIR);
	snprintf(ZSWAP_STATE_PATH, sizeof(ZSWAP_STATE_PATH), "%s/zswap.json", STATE_DIR);
	snprintf(DHCP_STATE_PATH, sizeof(DHCP_STATE_PATH), "%s/dhcp.json", STATE_DIR);
	snprintf(STALLWATCH_RECORDS_PATH, sizeof(STALLWATCH_RECORDS_PATH),
	         "%s/control_plane_stalls.jsonl", STATE_DIR);
	/*
	 * Issue #88: volume data is a direct child of the base dir, alongside
	 * CONTAINERS_DIR rather than inside it -- a volume deliberately
	 * outlives every container, so nesting it under container storage
	 * would be exactly the wrong lifetime association.
	 */
	snprintf(VOLUMES_DIR, sizeof(VOLUMES_DIR), "%s/volumes", g_base_dir);
	snprintf(PKI_DIR, sizeof(PKI_DIR), "%s/pki", STATE_DIR);
	snprintf(PKI_CERTS_STATE_PATH, sizeof(PKI_CERTS_STATE_PATH), "%s/pki_certs.json", PKI_DIR);
	snprintf(PKI_CERTS_DIR, sizeof(PKI_CERTS_DIR), "%s/certs", PKI_DIR);
	snprintf(CONTAINER_DEFS_STATE_PATH, sizeof(CONTAINER_DEFS_STATE_PATH), "%s/container_defs.json", STATE_DIR);
	snprintf(ROLLING_CONFIG_PATH, sizeof(ROLLING_CONFIG_PATH), "%s/rolling_config.json", STATE_DIR);
	snprintf(SITE_CONFIG_PATH, sizeof(SITE_CONFIG_PATH), "%s/site_config.json", STATE_DIR);
	snprintf(DEVICEMAP_STATE_PATH, sizeof(DEVICEMAP_STATE_PATH), "%s/devicemaps.json", STATE_DIR);
	snprintf(SYSCTLCONFIG_STATE_PATH, sizeof(SYSCTLCONFIG_STATE_PATH), "%s/sysctl_config.json", STATE_DIR);
	snprintf(KMODCONFIG_STATE_PATH, sizeof(KMODCONFIG_STATE_PATH), "%s/kmod_config.json", STATE_DIR);
	snprintf(DAEMON_CONFIG_PATH, sizeof(DAEMON_CONFIG_PATH), "%s/daemon_config.json", STATE_DIR);
	snprintf(QUOTAMAP_STATE_PATH, sizeof(QUOTAMAP_STATE_PATH), "%s/quota_projids.json", STATE_DIR);
	snprintf(SIGNING_KEYS_DIR, sizeof(SIGNING_KEYS_DIR), "%s/keys", STATE_DIR);
	snprintf(RESOLV_CONF_PATH, sizeof(RESOLV_CONF_PATH), "%s/resolv.conf", STATE_DIR);
	snprintf(NTP_STATE_PATH, sizeof(NTP_STATE_PATH), "%s/ntp.conf", STATE_DIR);
	snprintf(NTP_SERVERS_STATE_PATH, sizeof(NTP_SERVERS_STATE_PATH), "%s/ntp_servers.json", STATE_DIR);
	snprintf(SYSLOGFWD_STATE_PATH, sizeof(SYSLOGFWD_STATE_PATH), "%s/syslog_targets.json", STATE_DIR);
	snprintf(CONNTHROTTLE_CONFIG_PATH, sizeof(CONNTHROTTLE_CONFIG_PATH), "%s/tls_throttle.json", STATE_DIR);
	snprintf(BACKUP_CONFIG_PATH, sizeof(BACKUP_CONFIG_PATH), "%s/backup_config.json", STATE_DIR);
	snprintf(VOLUME_BACKUP_CONFIG_PATH, sizeof(VOLUME_BACKUP_CONFIG_PATH),
	         "%s/volume_backup_config.json", STATE_DIR);
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
	snprintf(PKG_BUILD_CONFIG_PATH, sizeof(PKG_BUILD_CONFIG_PATH), "%s/build_config.json", PKG_DIR);
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
 * issue #28: same "which disk, if any, is this daemon-wide singleton
 * currently placed on" lookup the three resolvers above share, but a
 * deliberately SOFTER failure posture -- ADR-0069's own swap mechanism
 * has always been "best-effort, never blocks daemon startup if the
 * file is somehow missing or stale" (swap_init()'s own existing
 * swapon() reconciliation already behaves this way), and a swap file
 * is not platform self-definition the way STATE_DIR is, so there is no
 * reason to newly start refusing to boot over it. Unlike the three
 * resolvers above, this doesn't mutate a shared global path -- it
 * fills out_file_path with either the resolved disk's own swapfile
 * path or the plain default (SWAP_FILE_PATH), always succeeding, for
 * the caller to hand straight to swap_init().
 */
static void resolve_swap_placement(char *out_file_path, size_t out_file_path_size)
{
	const char *disk_name = storageplacement_get(STORAGE_KIND_SWAP);
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n, i;

	snprintf(out_file_path, out_file_path_size, "%s", SWAP_FILE_PATH);
	if (disk_name == NULL)
		return; /* default OS-disk placement -- nothing to resolve */

	n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
	for (i = 0; i < n; i++) {
		const char *role;

		if (strcmp(disks[i].name, disk_name) != 0)
			continue;
		if (!disks[i].mounted) {
			fprintf(stderr,
			        "resolve_swap_placement: swap's configured disk '%s' is present but not "
			        "currently mounted -- using the default location instead this boot\n",
			        disk_name);
			return;
		}
		role = diskrole_lookup(disk_name);
		if (role == NULL || strcmp(role, "swap") != 0) {
			fprintf(stderr,
			        "resolve_swap_placement: swap's configured disk '%s' no longer carries the "
			        "swap role -- using the default location instead this boot\n",
			        disk_name);
			return;
		}
		snprintf(out_file_path, out_file_path_size, "%s/%s/swapfile", DISKS_MOUNT_DIR, disk_name);
		return;
	}
	fprintf(stderr,
	        "resolve_swap_placement: swap's configured disk '%s' is not currently present -- "
	        "using the default location instead this boot\n",
	        disk_name);
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
 * The real path above, resolved once at startup so a test can point it
 * somewhere harmless (--test-esp-entries-dir=). Every reader and writer
 * of loader entries goes through this, so there is still exactly one
 * answer to "where do boot entries live" -- the flag changes it, it
 * does not add a second copy of it.
 */
static char g_esp_entries_dir[PATH_MAX] = ESP_LOADER_ENTRIES_DIR;
/*
 * Partitions 2/3 in thinc-install's own layout (image/src/thinc-
 * install.c's auto_partition()), same fixed QEMU virtio-blk layout/
 * posture as ESP_DEVICE above -- a GPT-partition-name-based lookup
 * (find_partition_device() already exists for this purpose, but only
 * in thinc-install.c, installer-only code the daemon doesn't link)
 * can wait for part 4's real hardware, the same deferral ESP_DEVICE's
 * own comment and ADR-0018's Consequences section already state twice
 * for other partitions. root=%s2/%s3 in populate_esp()'s own loader
 * entries confirms this exact device-per-slot mapping.
 */
#define ROOT_A_DEVICE "/dev/vda2"
#define ROOT_B_DEVICE "/dev/vda3"
/*
 * Partition 4 in thinc-install's own layout (image/src/thinc-install.c)
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
 * all, exactly what a filesystem thinc-install.c didn't create via
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
#define SYSCTL_PREFIX "/v1/system/sysctl/" /* ADR-0160 */
#define KMOD_PREFIX "/v1/system/kmod/" /* ADR-0159 */
#define KMODCONFIG_PREFIX "/v1/system/kmod-config/" /* ADR-0159 */
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
	CONN_KERNEL_RELEASES_FETCH, /* kernel.org releases.json curl fetch (issue #65) */
	CONN_PKG_SYNC_FETCH,    /* pkg sync's own repo-archive curl fetch (ADR-0121) */
	CONN_PKG_SYNC_PERIODIC_TIMER, /* permanent, re-arms itself -- fires pkg_sync_start() periodically if configured (ADR-0121) */
	CONN_BACKUP_PERIODIC_TIMER, /* permanent, re-arms itself -- fires do_backup_snapshot_now() periodically if enabled+configured (ADR-0141 Phase 5) */
	CONN_IMAGE_RECIPE_FETCH, /* image-recipe-apply's own whole-rootfs artifact curl fetch (ADR-0123) */
	CONN_DISK_FORMAT,       /* disk format+mount job (multi-disk management Phase C) */
	CONN_STORAGE_MIGRATE,   /* state/rebuildable/log-storage migration job (ADR-0141 Phase 2) */
	CONN_CONTAINER_STORAGE_MIGRATE, /* one container's own overlay-storage migration job (ADR-0142 Section 4) */
	CONN_EXEC_OUTPUT,       /* issue #62 -- the pipe carrying an exec job's output */
	CONN_EXEC_CHILD,        /* same job's pidfd, so its exit is noticed without polling */
	CONN_EXEC_TIMER,        /* same job's deadline */
	CONN_PING,              /* GET/POST /v1/system/ping -- the raw ICMP socket half */
	CONN_PING_TIMER,        /* same job's paired timeout -- see ping_job_teardown() */
	CONN_NTP_PERIODIC_TIMER, /* permanent, re-arms itself -- fires ntp_sync_start() periodically (task #751) */
	CONN_NTP_SYNC,           /* one in-flight SNTP sync attempt's own UDP socket */
	CONN_NTP_SYNC_TIMER,     /* same job's per-candidate timeout -- see ntp_job_teardown() */
	/*
	 * Issue #81 server health. The probe is a NON-BLOCKING connect()
	 * driven by this same reactor -- a blocking probe would stall the
	 * whole control plane on an unreachable server, which is exactly the
	 * failure ADR-0180 exists to prevent.
	 */
	CONN_SERVERHEALTH_TIMER, /* permanent, re-arms itself -- fires one probe sweep per interval */
	CONN_SERVERHEALTH_PROBE, /* one in-flight TCP probe's own socket */
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
	CONN_UEVENT,            /* NETLINK_KOBJECT_UEVENT multicast socket (ADR-0161 Phase C) -- real
	                          * kernel hotplug add/remove events, modeled on CONN_KMSG's own async
	                          * registration shape, not rtnetlink.c's synchronous open/request/close one */
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
	int pkg_chain_idx;                      /* CONN_PKG_FETCH / CONN_PKG_BUILD_OUTPUT / CONN_PKG_BUILD_LOG_WS -- ADR-0157 Phase 2: which g_chains[] slot this conn belongs to */
	enum storage_kind storage_migrate_kind; /* CONN_STORAGE_MIGRATE only */
	char container_storage_migrate_name[REGISTRY_NAME_MAX]; /* CONN_CONTAINER_STORAGE_MIGRATE only */
	char restart_name[REGISTRY_NAME_MAX];   /* CONN_RESTART_TIMER only */
	/* CONN_SERVERHEALTH_PROBE only -- which registered server this
	 * in-flight probe is for, so its result can be attributed when the
	 * socket becomes writable (or the sweep times it out). */
	char probe_kind[SERVERHEALTH_KIND_MAX];
	char probe_container[REGISTRY_NAME_MAX];
	char probe_desc[SERVERHEALTH_PROBE_MAX];
	time_t probe_started_at;
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
 * PID 1) -- see main()'s own post-loop handling; a dev/test thincd
 * (no --init-mode, e.g. every test/*.c invocation) just exits normally
 * regardless of this value, exactly as it always has.
 */
enum shutdown_action { SHUTDOWN_ACTION_POWEROFF, SHUTDOWN_ACTION_REBOOT };

static int g_epfd;
static struct conn g_listener_conn;
static struct conn g_kmsg_conn;
static struct conn g_uevent_conn;
static struct conn g_https_listener_conn; /* .fd == -1 when HTTPS is disabled */
static SSL_CTX *g_tls_ctx;                 /* NULL when HTTPS is disabled */
static const char *g_web_root;
static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_shutdown_action = SHUTDOWN_ACTION_POWEROFF;

/*
 * conns torn down mid-batch are queued here instead of free()'d
 * immediately. A single thinc_epoll_wait() call can report BOTH halves of
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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, sess->ws_conn->fd, NULL);
	if (sess->ws_conn->ssl != NULL) {
		tls_unregister(sess->ws_conn->fd);
		SSL_free(sess->ws_conn->ssl);
	}
	close(sess->ws_conn->fd);
	ws_conn_free(&sess->ws_conn->ws);
	sess->ws_conn->kind = CONN_DEAD;
	queue_conn_free(sess->ws_conn);

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, sess->pty_conn->fd, NULL);
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
 * thincctl a --port= that actually reaches this daemon, and both are
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
 * Real freshness tracking for the async thinc bootroot assembly
 * (spawn_thinc_bootroot_assembly()/handle_bootroot_assemble_event(),
 * ADR-0057) -- closes task #737's own gap: `pkg hostbuild thinc
 * --deploy` used to treat "thincd-root.squashfs exists" as "this
 * hostbuild round's own artifact is ready," but that file is a leftover
 * from whichever assembly last succeeded, not necessarily the one this
 * round's own hostbuild triggered -- a stale file from an earlier round
 * would be silently redeployed while the real new assembly was still
 * running. g_bootroot_assembly_started increments once per attempt
 * (right before the fork in spawn_thinc_bootroot_assembly());
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
 * hostbuild name *means*, exactly the separation ADR-0057's own "thinc"
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
 * Only reached with --init-mode, i.e. thincd running as PID 1 on a bare
 * kernel boot with no initramfs (Phase 11) -- nothing else has mounted
 * /proc, /sys, or cgroup2 yet. devtmpfs is populated by the kernel itself
 * (CONFIG_DEVTMPFS_MOUNT) before init ever runs, so /dev needs no mount
 * here. Same proc mount flags mountns_pivot() already uses for each
 * container's own /proc (src/mountns.c) -- one already-correct flag set,
 * not a second one invented.
 *
 * BASE_DIR is the real thinc-containers partition (CONTAINERS_DEVICE) --
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

/* Parses the simple key=value net.conf thinc-install writes to the
 * config partition (image/src/thinc-install.c's populate step) --
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
 * ADR-0160: applies every persisted GET/PUT /v1/system/sysctl/{key}
 * entry at boot. Runs after load_boot_modules() (a net.* sysctl could
 * plausibly depend on a just-loaded NIC driver's own /proc/sys nodes
 * existing) but before bootstrap_management_network() below -- a
 * net.* sysctl can affect how that bootstrap itself behaves (e.g.
 * net.ipv4.ip_forward), so it needs to already be applied by then.
 * Best-effort per key, matching load_boot_modules()'s own posture: a
 * key that fails (e.g. a kernel built without the feature it
 * controls, so its /proc/sys node doesn't exist at all) is logged and
 * skipped, never a reason to fail boot.
 */
static void apply_one_configured_sysctl(const char *key, const char *value, void *ctx)
{
	(void)ctx;
	if (container_net_apply_sysctl(key, value) != 0)
		fprintf(stderr, "apply_configured_sysctls: %s=%s failed: %s\n", key, value,
		        strerror(errno));
}

static void apply_configured_sysctls(void)
{
	sysctlconfig_foreach(apply_one_configured_sysctl, NULL);
}

/*
 * ADR-0159 Phase A: the REST-managed autoload counterpart to
 * load_boot_modules() above -- runs last of the four (after
 * bootstrap_management_network(), not before it): operator-configured
 * autoload is the least boot-critical of the four steps, and a
 * container-usable driver loaded slightly later than the management
 * network is fine, unlike load_boot_modules()'s own hardware-detection
 * list, which genuinely must precede that bootstrap. Best-effort per
 * module, same posture as load_boot_modules(): a module an operator
 * configured for autoload but that fails to load (hardware not
 * present, or never actually got built) is logged and skipped, never a
 * reason to fail boot.
 */
static void load_one_configured_module(const char *name, const char *options, void *ctx)
{
	(void)ctx;
	if (kmod_load(name, options) != 0)
		fprintf(stderr, "load_configured_modules: %s failed\n", name);
}

static void load_configured_modules(void)
{
	kmodconfig_foreach_autoload(load_one_configured_module, NULL);
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
 * living directly on its bridge, now doing double duty as thincd's
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
			nerr = network_create(MGMT_NETWORK_NAME, subnet_str, prefix, ip, NULL, NULL, &net);
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
	 * task #764: thincd's own exec_into_container() (daemon/src/exec.c,
	 * the console/exec feature, ADR pending #426) calls posix_openpt()
	 * and then open()s the slave device ptsname_r() hands back --
	 * that slave path only resolves to a real device node once the
	 * devpts filesystem is actually mounted at /dev/pts. devtmpfs
	 * auto-populates /dev/ptmx (CONFIG_DEVTMPFS_MOUNT) so posix_openpt()
	 * itself always succeeds regardless, which is exactly what made
	 * this gap so easy to miss: only the SECOND step (opening the
	 * slave) ever fails, with a generic ENOENT that gave no hint it
	 * was devpts-shaped until task #764's own diagnostic surfacing
	 * made "No such file or directory" visible at all. thinc-install.c's
	 * own early_mounts() has mounted devpts since ADR-0042/task #406 --
	 * but that binary only ever runs during one-time disk installation,
	 * never during thincd's own real boot_init() (--init-mode, a bare
	 * kernel with no initramfs) on an already-installed box, which is
	 * exactly why every console/exec attempt against 192.168.15.95
	 * failed with a bare 500 while the same code path passed cleanly
	 * in every dev-sandbox/QEMU test harness run (those inherit an
	 * already-mounted /dev/pts from their own outer environment,
	 * never exercising this gap). Same mount options as
	 * thinc-install.c's own copy, for the same reason (ptmxmode=0666
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
	 * squashfs, mkbootroot.c) so thincd's own curl/openssl/etc.
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
 * (thinc-<slot>[+<tries-left>[-<tries-done>]].conf) down to the bare
 * thinc-<slot>.conf, stripping systemd-boot's own Automatic Boot
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

	snprintf(prefix, sizeof(prefix), "thinc-%s", slot);
	prefix_len = strlen(prefix);

	d = opendir(g_esp_entries_dir);
	if (d == NULL) {
		perror(g_esp_entries_dir);
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, prefix, prefix_len) == 0) {
			found = 1;
			snprintf(oldpath, sizeof(oldpath), "%s/%s", g_esp_entries_dir, de->d_name);
			break;
		}
	}
	closedir(d);

	if (!found) {
		fprintf(stderr, "confirm_boot: no loader entry found for slot %s\n", slot);
		return -1;
	}

	snprintf(newpath, sizeof(newpath), "%s/thinc-%s.conf", g_esp_entries_dir, slot);
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
		*out_msg = "refused: this network carries thincd's own bind address -- "
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
	jw_str(&w, THINC_BUILD_VERSION);
	jw_key(&w, "build_time");
	jw_str(&w, THINC_BUILD_TIME);
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
	 * Real freshness signal for `pkg hostbuild thinc --deploy`
	 * (task #737, ADR-0058-follow-on comment in
	 * g_bootroot_assembly_started's own doc comment above): a client
	 * that captured bootroot_assembly_completed_generation *before*
	 * triggering a new hostbuild round can wait here for it to advance
	 * past that baseline rather than trusting "thincd-root.squashfs
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

/* ---- Issue #63: factory reset ---- */

/*
 * Returning a box to its just-installed state, without reinstalling.
 *
 * The mechanism is deliberately a SENTINEL plus a reboot, rather than
 * wiping state and carrying on. Two reasons, and both are correctness
 * rather than taste:
 *
 * - The daemon holds most of this state in memory and rewrites its
 *   files on any change. Deleting them underneath a running daemon is a
 *   race it usually loses -- the next container event, health probe or
 *   config touch writes the file straight back, and the reset silently
 *   half-happens.
 * - It is crash-safe. Once the sentinel is written the reset WILL
 *   happen, on this boot or the next one. A reset that got as far as
 *   "the operator was told yes" and then evaporated because the reboot
 *   did not complete is the worst possible outcome for an operation
 *   whose entire purpose is being sure of the starting state.
 *
 * The wipe itself runs at startup before any subsystem loads, so
 * nothing has read the old state yet and nothing can write it back.
 */
#define FACTORY_RESET_SENTINEL "FACTORY-RESET"

static void factory_reset_sentinel_path(char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s", g_base_dir, FACTORY_RESET_SENTINEL);
}

/*
 * Wipes every operator-owned tree back to first-boot empty. Called at
 * startup only, before boot_subsystem_init() reads anything.
 *
 * What is deliberately NOT touched: the OS itself. The two root slots,
 * the kernel, the ESP and the install-time config partition all live
 * outside this base directory, so a reset returns the box to how
 * thinc-install left it rather than to nothing.
 *
 * What IS destroyed, and worth naming because it is the part that
 * cannot be undone: volumes and everything in them. A just-installed
 * box has no volumes, so a reset that kept them would not be a reset --
 * but this is the one category here that is genuinely irreplaceable
 * workload data rather than regenerable platform state.
 */
static void factory_reset_apply_if_pending(void)
{
	char sentinel[PATH_MAX];
	struct stat st;
	const char *trees[] = { "state", "containers", "rebuildable", "logs", "volumes", "disks" };
	size_t i;

	factory_reset_sentinel_path(sentinel, sizeof(sentinel));
	if (stat(sentinel, &st) != 0)
		return;

	fprintf(stderr, "factory reset: wiping all operator state back to first-boot defaults\n");
	for (i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s/%s", g_base_dir, trees[i]);
		/*
		 * "disks" holds the mountpoints of role-assigned disks, not
		 * their contents -- removing the directory does not touch the
		 * filesystems themselves, which is right: a factory reset
		 * forgets which disks were assigned what, it does not reformat
		 * hardware behind the operator's back.
		 */
		persist_remove_tree(path);
		fprintf(stderr, "factory reset: removed %s\n", path);
	}
	unlink(sentinel);
	fprintf(stderr, "factory reset: complete -- this boot starts from install defaults\n");
}

/*
 * POST /v1/system/factory-reset
 *
 * The most destructive endpoint this platform has, so the confirmation
 * is the instance's own name typed back -- not a boolean. A boolean can
 * be sent by a client that misunderstood what it was calling; a name
 * can only be sent by something that looked it up first.
 */
static void handle_factory_reset(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *confirm;
	const char *instance = siteconfig_instance_name();
	char sentinel[PATH_MAX];
	int sfd;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	confirm = json_as_string(json_object_get(root, "confirm"));
	if (instance == NULL || instance[0] == '\0')
		instance = "thinc";
	if (confirm == NULL || strcmp(confirm, instance) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "confirm must be this install's own instance name (GET /v1/system/site) -- "
		              "this destroys every container, image, volume and all their data, and "
		              "cannot be undone");
		return;
	}
	json_free(root);

	factory_reset_sentinel_path(sentinel, sizeof(sentinel));
	sfd = open(sentinel, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (sfd < 0) {
		respond_error(fd, 500, "Internal Server Error", "could not arm the factory reset");
		return;
	}
	close(sfd);
	/*
	 * Logged before the reboot, not after: the log store is one of the
	 * things about to be wiped, so this line exists to reach whatever
	 * is forwarding logs off the box, and to be the last thing in the
	 * local store if nothing is.
	 */
	logstore_write("system", "warn",
	               "FACTORY RESET armed -- rebooting, and all operator state will be wiped on the "
	               "next boot");

	g_shutdown_action = SHUTDOWN_ACTION_REBOOT;
	g_stop = 1;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "status");
	jw_str(&w, "factory reset armed -- rebooting");
	jw_key(&w, "wipes");
	jw_arr_open(&w);
	jw_str(&w, "containers and their storage");
	jw_str(&w, "images and every version of them");
	jw_str(&w, "volumes AND ALL DATA IN THEM");
	jw_str(&w, "networks, routes, DNS, PKI, LDAP, NTP, syslog registrations");
	jw_str(&w, "package install state, recipes, build cache and artifacts");
	jw_str(&w, "the consolidated log store");
	jw_arr_close(&w);
	jw_key(&w, "keeps");
	jw_arr_open(&w);
	jw_str(&w, "the installed OS itself (both root slots, kernel, ESP, install config)");
	jw_str(&w, "the contents of any role-assigned disk -- roles are forgotten, disks are not reformatted");
	jw_arr_close(&w);
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
 * matches thinc-install.c's own ROOT_A_TRIES value (3), the same
 * Automatic Boot Assessment convention applied uniformly to any fresh
 * slot, not just the very first install.
 */
#define ROOT_UPDATE_TRIES 3

/*
 * Writes the whole content of src_path onto dst device_path, raw --
 * mirrors thinc-install.c's own copy_file()/write_whole_file_to_
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
 * optional (at least one required): a thincd security fix needs no
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
	snprintf(kernel_dest, sizeof(kernel_dest), "%s/thinc-bzImage-%s", ESP_DIR, inactive_slot);
	snprintf(active_kernel_path, sizeof(active_kernel_path), "%s/thinc-bzImage-%s", ESP_DIR, g_slot);

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

	snprintf(entry_path, sizeof(entry_path), "%s/thinc-%s+%d.conf", g_esp_entries_dir,
	         inactive_slot, ROOT_UPDATE_TRIES);
	/* version is this boot's own current timestamp -- always higher
	 * than whatever's already on disk, so systemd-boot sorts this
	 * entry first, with no need to parse the existing entry's own
	 * version back out first. linux always references this slot's own
	 * thinc-bzImage-<slot> (pre-staged for both slots at install
	 * time, ADR-0032) -- whether or not kernel_path was given this
	 * call, that file already exists and is exactly what should boot. */
	{
		/* Issue #24: the console portion comes from the operator's own
		 * configuration rather than being fixed here. Everything else
		 * on this line -- root=, rw, init= and the daemon's own
		 * arguments -- stays this function's business: those decide
		 * whether the machine boots at all, not what it displays. */
		char console_opts[512];

		bootconsole_render(console_opts, sizeof(console_opts));
		snprintf(entry_conf, sizeof(entry_conf),
		         "title thinC (%s)\n"
		         "sort-key thinc\n"
		         "version %ld\n"
		         "linux /thinc-bzImage-%s\n"
		         "options %s%sroot=%s rw init=/bin/thincd -- --init-mode "
		         "--slot=%s --bind=%s\n",
		         inactive_slot[0] == 'a' ? "A" : "B", (long)time(NULL), inactive_slot,
		         console_opts, console_opts[0] != '\0' ? " " : "", device, inactive_slot,
		         g_bind_addr);
	}

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

	/*
	 * Issue #88: the volume REGISTRY -- which volumes exist and where
	 * they are placed. That is configuration, so it belongs here; a
	 * volume's contents are workload data and deliberately stay out,
	 * the same boundary ADR-0033 draws for image content.
	 *
	 * This is not optional polish. container_defs reference volumes by
	 * name, and a container naming an unknown volume is a hard 400 by
	 * design (ADR-0183) -- so a bundle carrying the defs but not the
	 * registry restores onto a box where every container with a volume
	 * fails to start, which is a broken restore rather than a partial
	 * one. Restoring recreates the volumes empty; refilling them is the
	 * operator's own concern, exactly as it is for image content.
	 */
	jw_key(w, "volumes");
	if (persist_read_file(VOLUMES_STATE_PATH, &buf, &len) == 0 && buf != NULL) {
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
	const struct json_value *jpkg_recipes, *jsite_config, *jvolumes;
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
	jvolumes = json_object_get(root, "volumes"); /* issue #88 */

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
	if (jvolumes != NULL) {
		have_any = 1;
		if (!json_string_field_is_valid(jvolumes)) {
			json_free(root);
			snprintf(out_errmsg, out_errmsg_size, "volumes is not valid JSON");
			return 400;
		}
	}

	if (!have_any) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size,
		         "at least one of container_defs/networks/dns_records/pkg_installed/pkg_recipes/"
		         "site_config/volumes required");
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
	if (jvolumes != NULL &&
	    restore_write_field(VOLUMES_STATE_PATH, json_as_string(jvolumes)) != 0) {
		json_free(root);
		snprintf(out_errmsg, out_errmsg_size, "failed to write volumes.json");
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
 * Live listen-socket rebind (Part 0.5) -- thincd is real PID 1 under
 * --init-mode (confirmed: image/src/thinc-install.c's loader entry
 * uses "init=/bin/thincd"), so there is no "restart the daemon" to
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, new_fd, &ev) != 0) {
		perror("rebind_listener: epoll_ctl ADD");
		g_listener_conn.fd = old_fd;
		close(new_fd);
		return -1;
	}

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, old_fd, NULL);
	close(old_fd);

	snprintf(g_bind_addr_buf, sizeof(g_bind_addr_buf), "%s", new_bind_addr);
	g_bind_addr = g_bind_addr_buf;
	g_port = new_port;

	printf("thincd rebound listener to %s:%d\n", new_bind_addr, new_port);
	fflush(stdout);
	return 0;
}

/* Best-effort, same "never block daemon startup" posture as every
 * other non-critical reconciliation step (cgroup_enable_io_
 * accounting(), swap_init()'s own swapon() retry) -- a test/dev
 * invocation, or a kernel with /dev/kmsg unreadable for any other
 * reason, simply never gets kernel-source log entries; every other
 * source (thincd's own diagnostics, the audit trail) is unaffected. */
static void start_kmsg_watch(void)
{
	struct thinc_epoll_event ev;
	int fd = logstore_kmsg_fd();

	if (fd < 0)
		return;
	g_kmsg_conn.kind = CONN_KMSG;
	g_kmsg_conn.fd = fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_kmsg_conn;
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0)
		g_kmsg_conn.fd = -1;
}

static void handle_kmsg_event(struct conn *cc)
{
	(void)cc;
	logstore_kmsg_readable();
}

/* Forward declarations -- real definitions live alongside the manual
 * POST/DELETE /v1/containers/{name}/devices handlers further down
 * (ADR-0161 Phase D), needed here already since Phase C's hotplug
 * reconciliation (below) is just another caller of the same live-
 * attach/detach primitives those handlers use. */
static int live_mknod_device(pid_t pid, const struct device_spec *dev);
static int live_unlink_device(pid_t pid, const char *dev_path);
static int live_attach_one_device(struct registry_entry *e, const struct discovered_device *dd);

/*
 * ADR-0161 Phase C: a persistent NETLINK_KOBJECT_UEVENT multicast
 * socket, registered with the daemon's own epoll reactor once at
 * startup -- modeled on start_kmsg_watch()'s own async-registration
 * shape (never registered with epoll before), not rtnetlink.c's
 * synchronous open/request/close one, which this genuinely isn't:
 * kernel uevents arrive whenever real hardware changes, with no
 * request/response pairing at all. Best-effort, same posture as
 * start_kmsg_watch(): a kernel/sandbox with no CAP_NET_ADMIN, or one
 * that otherwise can't bind this socket, simply never gets hotplug
 * reactions -- every other capability this daemon has is unaffected.
 */
static void start_uevent_watch(void)
{
	struct thinc_epoll_event ev;
	int fd;
	struct sockaddr_nl addr;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	if (fd < 0)
		return;

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_groups = 1; /* kernel events multicast group */

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return;
	}

	g_uevent_conn.kind = CONN_UEVENT;
	g_uevent_conn.fd = fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_uevent_conn;
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		close(fd);
		g_uevent_conn.fd = -1;
	}
}

/*
 * ADR-0161 Phase B/C: for every running container's own pending_
 * devices[] (an "optional": true devices[] reference that didn't
 * resolve at creation time), re-attempts resolution now that a real
 * hotplug event just fired. Contention (resolved decision): if more
 * than one running container's own pending reference resolves to the
 * SAME device, neither gets it -- logged, not silently arbitrated by
 * registry scan order. A device this pass successfully grants is
 * removed from the winning container's own pending list; every other
 * pending reference (no match, or lost a contention) is left in place
 * for the next hotplug event to retry.
 */
struct pending_device_claim {
	struct registry_entry *e;
	char ref[96];
	char resolved_id[96];
};

static void reconcile_pending_device_attachments(void)
{
	char names[REGISTRY_MAX_CONTAINERS][REGISTRY_NAME_MAX];
	int ncount = registry_list_names(names, REGISTRY_MAX_CONTAINERS);
	static struct pending_device_claim claims[REGISTRY_MAX_CONTAINERS * CONTAINER_MAX_DEVICES];
	int claim_count = 0;
	int i, j;

	for (i = 0; i < ncount; i++) {
		struct registry_entry *e = registry_find(names[i]);

		if (e == NULL || !e->running || e->pending_device_count == 0)
			continue;
		for (j = 0; j < e->pending_device_count; j++) {
			const struct discovered_device *matches[1];
			int n;

			n = devicemap_resolve(e->pending_devices[j], CONTAINERS_DIR, matches, 1);
			if (n < 0)
				n = device_find_group(e->pending_devices[j], CONTAINERS_DIR, matches, 1);
			if (n <= 0 || !matches[0]->assignable)
				continue;
			if (claim_count >= (int)(sizeof(claims) / sizeof(claims[0])))
				continue;
			claims[claim_count].e = e;
			snprintf(claims[claim_count].ref, sizeof(claims[claim_count].ref), "%s",
			         e->pending_devices[j]);
			snprintf(claims[claim_count].resolved_id, sizeof(claims[claim_count].resolved_id), "%s",
			         matches[0]->id);
			claim_count++;
		}
	}

	for (i = 0; i < claim_count; i++) {
		int contenders = 0;
		const struct discovered_device *dd;

		for (j = 0; j < claim_count; j++) {
			if (strcmp(claims[i].resolved_id, claims[j].resolved_id) == 0)
				contenders++;
		}
		if (contenders > 1) {
			/* Logged once per losing claim, not once per contending
			 * pair -- a real operator-visible signal without becoming
			 * noisy for a device with many contenders. */
			logstore_write("thincd", "warning",
			                "container %s: hotplugged device %s matches \"%s\" but another "
			                "running container also claims it -- granted to neither",
			                claims[i].e->name, claims[i].resolved_id, claims[i].ref);
			continue;
		}

		dd = device_find(claims[i].resolved_id, CONTAINERS_DIR);
		if (dd == NULL)
			continue; /* raced with another unplug between resolve and here */

		if (live_attach_one_device(claims[i].e, dd) == 0) {
			registry_clear_pending_device(claims[i].e, claims[i].ref);
			logstore_write("thincd", "info",
			                "container %s: hotplug live-attached device %s (matched \"%s\")",
			                claims[i].e->name, dd->id, claims[i].ref);
		} else {
			logstore_write("thincd", "error",
			                "container %s: hotplug live-attach of %s (matched \"%s\") failed: %s",
			                claims[i].e->name, dd->id, claims[i].ref, strerror(errno));
		}
	}
}

/*
 * ADR-0161 Phase C: symmetric unplug reaction -- for every running
 * container's own currently-granted devices[], checks whether the
 * underlying host device can still be found at all; a device whose
 * hardware just disappeared has its grant actively revoked (BPF
 * program updated, /dev node unlinked), never left in place as an
 * inert grant for hardware that no longer exists. Applies to EVERY
 * grant regardless of how it was originally made (create-time or
 * live) -- unlike the operator-facing DELETE endpoint, which refuses
 * to touch a create-time grant, a physical unplug is not an operator
 * request and revokes it either way; registry_device_live_detach()
 * itself carries no such restriction (see its own header comment).
 */
static void reconcile_live_device_revocations(void)
{
	char names[REGISTRY_MAX_CONTAINERS][REGISTRY_NAME_MAX];
	int ncount = registry_list_names(names, REGISTRY_MAX_CONTAINERS);
	int i, j;

	for (i = 0; i < ncount; i++) {
		struct registry_entry *e = registry_find(names[i]);

		if (e == NULL || !e->running)
			continue;
		/* Iterated backward: registry_device_live_detach() shifts
		 * later entries down by one on every successful removal, so a
		 * forward scan would skip the entry that just slid into the
		 * current index. */
		for (j = e->device_count - 1; j >= 0; j--) {
			char id_copy[96];
			struct registry_device_attachment removed;

			if (device_find(e->devices[j].id, CONTAINERS_DIR) != NULL)
				continue;

			snprintf(id_copy, sizeof(id_copy), "%s", e->devices[j].id);
			if (registry_device_live_detach(e, id_copy, &removed) != 0) {
				logstore_write("thincd", "error",
				                "container %s: failed to revoke unplugged device %s: %s",
				                e->name, id_copy, strerror(errno));
				continue;
			}
			live_unlink_device(e->handle.pid, removed.dev_path);
			logstore_write("thincd", "info",
			                "container %s: revoked grant for unplugged device %s", e->name,
			                id_copy);
		}
	}
}

/*
 * ADR-0161 Phase C: one datagram is one uevent -- a NUL-separated
 * "KEY=VALUE" list, first line the summary ("add@/devices/.../3-3"),
 * every line after it a real KEY=VALUE env var (ACTION=, SUBSYSTEM=,
 * DEVPATH=, ...). Filtered to SUBSYSTEM=usb and a whole-device DEVPATH
 * only (trailing path component containing no ':' -- the identical
 * rule device.c's own usb_name_is_device() applies to its sysfs walk,
 * inlined here rather than exported across modules for one line of
 * logic) -- an interface-level uevent for the same physical device is
 * deliberately ignored, matching the whole-device passthrough model.
 * Deliberately does NOT try to derive the specific device's own
 * stable id (vendor/product/serial) from the uevent payload itself --
 * real hardware doesn't populate those fields identically across
 * every device class, and this daemon already has a robust, fresh way
 * to discover exactly that (device.c's own sysfs walk). The uevent is
 * treated purely as a trigger ("something USB just changed, go
 * recheck"), not as a source of device identity.
 */
static void handle_uevent_event(struct conn *cc)
{
	char buf[2048];
	ssize_t n;
	int is_add = 0, is_remove = 0, is_usb = 0, is_whole_device = 0;
	size_t off;

	n = recv(cc->fd, buf, sizeof(buf) - 1, 0);
	if (n <= 0)
		return;
	buf[n] = '\0';

	for (off = 0; off < (size_t)n;) {
		const char *line = buf + off;
		size_t line_len = strlen(line);

		if (strncmp(line, "ACTION=", 7) == 0) {
			is_add = strcmp(line + 7, "add") == 0;
			is_remove = strcmp(line + 7, "remove") == 0;
		} else if (strncmp(line, "SUBSYSTEM=", 10) == 0) {
			is_usb = strcmp(line + 10, "usb") == 0;
		} else if (strncmp(line, "DEVPATH=", 8) == 0) {
			const char *base = strrchr(line + 8, '/');

			base = (base != NULL) ? base + 1 : line + 8;
			is_whole_device = strchr(base, ':') == NULL;
		}
		off += line_len + 1;
	}

	if (!is_usb || !is_whole_device || (!is_add && !is_remove))
		return;

	if (is_add)
		reconcile_pending_device_attachments();
	else
		reconcile_live_device_revocations();
}

static int start_http_listener(const char *bind_addr, int port)
{
	struct thinc_epoll_event ev;
	int fd = create_listen_socket(bind_addr, port);

	if (fd < 0)
		return -1;
	g_listener_conn.kind = CONN_LISTENER;
	g_listener_conn.fd = fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_listener_conn;
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		perror("start_http_listener: epoll_ctl ADD");
		close(fd);
		g_listener_conn.fd = -1;
		return -1;
	}
	printf("thincd listening on %s:%d\n", bind_addr, port);
	fflush(stdout);
	return 0;
}

static void stop_http_listener(void)
{
	if (g_listener_conn.fd < 0)
		return;
	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_listener_conn.fd, NULL);
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
	struct thinc_epoll_event ev;
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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		perror("start_https_listener: epoll_ctl ADD");
		close(fd);
		g_https_listener_conn.fd = -1;
		return -1;
	}
	printf("thincd listening (https) on %s:%d\n", bind_addr, port);
	fflush(stdout);
	return 0;
}

static void stop_https_listener(void)
{
	if (g_https_listener_conn.fd < 0)
		return;
	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_https_listener_conn.fd, NULL);
	close(g_https_listener_conn.fd);
	g_https_listener_conn.fd = -1;
}

static int rebind_https_listener(const char *new_bind_addr, int new_port)
{
	int new_fd;
	int old_fd;
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, new_fd, &ev) != 0) {
		perror("rebind_https_listener: epoll_ctl ADD");
		g_https_listener_conn.fd = old_fd;
		close(new_fd);
		return -1;
	}

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, old_fd, NULL);
	close(old_fd);

	printf("thincd rebound https listener to %s:%d\n", new_bind_addr, new_port);
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, tfd, &ev) != 0) {
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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	close(cc->fd);

	rtfd = rtnl_open();
	if (rtfd >= 0) {
		rtnl_addr_del_ipv4(rtfd, cc->cleanup_ifname, cc->cleanup_addr_be, cc->cleanup_prefix_len);
		rtnl_close(rtfd);
	}
	/* Best-effort, same as before this became a deferred timer: a
	 * failure here leaves a harmless leftover address on the bridge,
	 * never persisted or bound to by thincd itself. */

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
		thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_ping_sock_conn->fd, NULL);
		close(g_ping_sock_conn->fd);
		g_ping_sock_conn->kind = CONN_DEAD;
		queue_conn_free(g_ping_sock_conn);
		g_ping_sock_conn = NULL;
	}
	if (g_ping_timer_conn != NULL) {
		thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_ping_timer_conn->fd, NULL);
		close(g_ping_timer_conn->fd);
		g_ping_timer_conn->kind = CONN_DEAD;
		queue_conn_free(g_ping_timer_conn);
		g_ping_timer_conn = NULL;
	}
}

/* ---- Issue #62: non-interactive exec ---- */

/*
 * Run a command inside a running container and collect its output.
 *
 * The two ways to do this before were both poor diagnostics: the
 * interactive console mangles piped input through its pty line
 * discipline (a corrupted one-liner fed a wrong hang diagnosis during
 * Part 201), and a throwaway container with capture_output gets a fresh
 * namespace, which is useless for inspecting the state of an
 * already-running one -- the actual need during an investigation.
 *
 * An async job with a poll endpoint, not a blocking handler. Holding
 * the epoll loop for the length of someone's command is exactly the
 * wedge ADR-0180 exists to prevent, and this daemon already has this
 * shape for every other slow thing (ping, disk format, storage
 * migration): start it, respond, let the client poll.
 *
 * One at a time, deliberately. Concurrent execs would need per-job
 * state and identifiers for a diagnostic tool that is used one command
 * at a time; "another exec is already running" is a clearer answer than
 * silently queueing.
 */
#define EXEC_OUTPUT_MAX 65536
#define EXEC_DEFAULT_TIMEOUT_SECONDS 30
#define EXEC_MAX_TIMEOUT_SECONDS 300
#define EXEC_MAX_ARGV 32

static struct {
	int in_use;
	int done;
	int timed_out;
	int exit_status;
	pid_t child_pid;
	char container[REGISTRY_NAME_MAX];
	char output[EXEC_OUTPUT_MAX];
	size_t output_len;
	int truncated;
	struct conn *out_conn;
	struct conn *child_conn;
	struct conn *timer_conn;
} g_exec;

static void exec_job_close_conn(struct conn **slot)
{
	if (*slot == NULL)
		return;
	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, (*slot)->fd, NULL);
	close((*slot)->fd);
	free(*slot);
	*slot = NULL;
}

/*
 * Tears the job's own fds down but KEEPS the result, so a client that
 * polls after completion still gets the output and exit status. The
 * result is replaced by the next exec, not cleared here.
 */
static void exec_job_finish(int exit_status, int timed_out)
{
	g_exec.exit_status = exit_status;
	g_exec.timed_out = timed_out;
	g_exec.done = 1;
	if (g_exec.child_pid > 0 && timed_out) {
		/* A command that outran its deadline is killed rather than
		 * left running invisibly -- an exec nobody is waiting for any
		 * more is exactly the kind of orphan that turns up later as a
		 * mystery process. */
		kill(g_exec.child_pid, SIGKILL);
		waitpid(g_exec.child_pid, NULL, WNOHANG);
	}
	exec_job_close_conn(&g_exec.out_conn);
	exec_job_close_conn(&g_exec.child_conn);
	exec_job_close_conn(&g_exec.timer_conn);
	g_exec.child_pid = -1;
}

static void handle_exec_output_event(struct conn *cc)
{
	char buf[4096];
	ssize_t n = read(cc->fd, buf, sizeof(buf));

	if (n > 0) {
		size_t room = sizeof(g_exec.output) - 1 - g_exec.output_len;

		if ((size_t)n > room) {
			/* Capped rather than grown without bound, and the cap is
			 * reported: silently dropping the tail of a diagnostic is
			 * how someone concludes the wrong thing from it. */
			n = (ssize_t)room;
			g_exec.truncated = 1;
		}
		memcpy(g_exec.output + g_exec.output_len, buf, (size_t)n);
		g_exec.output_len += (size_t)n;
		g_exec.output[g_exec.output_len] = '\0';
		return;
	}
	/* EOF: the command closed stdout/stderr. Its exit status arrives
	 * separately via the pidfd, so this half just stops listening. */
	exec_job_close_conn(&g_exec.out_conn);
}

static void handle_exec_child_event(struct conn *cc)
{
	siginfo_t info;

	memset(&info, 0, sizeof(info));
	if (waitid(P_PID, (id_t)g_exec.child_pid, &info, WEXITED | WNOHANG) == 0) {
		/*
		 * Drain whatever is still buffered in the pipe before
		 * reporting: a command that writes and exits immediately would
		 * otherwise be reported with empty output purely because its
		 * exit was noticed first.
		 */
		if (g_exec.out_conn != NULL) {
			for (;;) {
				char buf[4096];
				ssize_t n = read(g_exec.out_conn->fd, buf, sizeof(buf));
				size_t room;

				if (n <= 0)
					break;
				room = sizeof(g_exec.output) - 1 - g_exec.output_len;
				if ((size_t)n > room) {
					n = (ssize_t)room;
					g_exec.truncated = 1;
				}
				if (n <= 0)
					break;
				memcpy(g_exec.output + g_exec.output_len, buf, (size_t)n);
				g_exec.output_len += (size_t)n;
				g_exec.output[g_exec.output_len] = '\0';
			}
		}
		exec_job_finish(info.si_code == CLD_EXITED ? info.si_status : 128 + info.si_status, 0);
	}
	(void)cc;
}

static void handle_exec_timer_event(struct conn *cc)
{
	uint64_t ticks;

	if (read(cc->fd, &ticks, sizeof(ticks)) != (ssize_t)sizeof(ticks))
		; /* a short read just means no tick to act on */
	exec_job_finish(-1, 1);
}

static void handle_container_exec_get(int fd, const char *name)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, g_exec.in_use ? g_exec.container : name);
	jw_key(&w, "state");
	if (!g_exec.in_use)
		jw_str(&w, "none");
	else if (!g_exec.done)
		jw_str(&w, "running");
	else if (g_exec.timed_out)
		jw_str(&w, "timeout");
	else
		jw_str(&w, "done");
	jw_key(&w, "exit_status");
	if (g_exec.in_use && g_exec.done && !g_exec.timed_out)
		jw_int(&w, g_exec.exit_status);
	else
		jw_null(&w);
	jw_key(&w, "output");
	jw_str(&w, g_exec.in_use ? g_exec.output : "");
	jw_key(&w, "truncated");
	jw_bool(&w, g_exec.in_use && g_exec.truncated);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static struct conn *exec_register(int fd, enum conn_kind kind)
{
	struct conn *cc = malloc(sizeof(*cc));
	struct thinc_epoll_event ev;

	if (cc == NULL)
		return NULL;
	memset(cc, 0, sizeof(*cc));
	cc->kind = kind;
	cc->fd = fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		free(cc);
		return NULL;
	}
	return cc;
}

static void handle_container_exec_post(int fd, const char *name, const char *body, size_t body_len)
{
	struct registry_entry *e = registry_find(name);
	struct json_value *root;
	const struct json_value *jargv;
	char *argv_storage[EXEC_MAX_ARGV + 1];
	char argv_buf[EXEC_MAX_ARGV][512];
	long timeout_s = EXEC_DEFAULT_TIMEOUT_SECONDS;
	int argc = 0;
	int read_fd = -1;
	pid_t child = -1;
	int pidfd, tfd;
	struct itimerspec its;
	size_t i;

	if (e == NULL || !e->running) {
		respond_error(fd, 409, "Conflict",
		              "this container is not running -- there is no namespace to run a command in. "
		              "For a container that has exited, read its files instead "
		              "(GET /v1/containers/{name}/files)");
		return;
	}
	if (e->paused) {
		respond_error(fd, 409, "Conflict",
		              "this container is paused -- a command started in it would be frozen too");
		return;
	}
	if (g_exec.in_use && !g_exec.done) {
		respond_error(fd, 409, "Conflict", "another exec is already running");
		return;
	}

	root = json_parse(body, body_len);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jargv = json_object_get(root, "argv");
	if (jargv == NULL || jargv->type != JSON_ARRAY || jargv->u.array.count == 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "argv must be a non-empty array");
		return;
	}
	if (jargv->u.array.count > EXEC_MAX_ARGV) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "too many argv entries");
		return;
	}
	for (i = 0; i < jargv->u.array.count; i++) {
		const char *a = json_as_string(jargv->u.array.items[i]);

		if (a == NULL) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "every argv entry must be a string");
			return;
		}
		snprintf(argv_buf[argc], sizeof(argv_buf[argc]), "%s", a);
		argv_storage[argc] = argv_buf[argc];
		argc++;
	}
	argv_storage[argc] = NULL;
	{
		const struct json_value *jt = json_object_get(root, "timeout_seconds");

		if (jt != NULL && jt->type == JSON_NUMBER) {
			timeout_s = (long)json_as_number(jt);
			if (timeout_s < 1 || timeout_s > EXEC_MAX_TIMEOUT_SECONDS) {
				json_free(root);
				respond_error(fd, 400, "Bad Request", "timeout_seconds must be between 1 and 300");
				return;
			}
		}
	}
	json_free(root);

	/*
	 * argv[0] is exec'd directly -- no shell, so no quoting, globbing
	 * or word splitting happens anywhere. That is the point: a shell
	 * between the caller and the command is another thing that can
	 * reinterpret what was asked for, which is the class of problem
	 * this endpoint exists to avoid.
	 */
	if (exec_into_container_piped(e->handle.pid, argv_storage, &read_fd, &child) != 0) {
		respond_error(fd, 500, "Internal Server Error",
		              "could not enter the container's namespaces to run the command");
		return;
	}

	memset(&g_exec, 0, sizeof(g_exec));
	g_exec.in_use = 1;
	g_exec.child_pid = child;
	snprintf(g_exec.container, sizeof(g_exec.container), "%s", name);

	pidfd = sys_pidfd_open(child, 0);
	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (pidfd < 0 || tfd < 0) {
		if (pidfd >= 0) close(pidfd);
		if (tfd >= 0) close(tfd);
		close(read_fd);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		memset(&g_exec, 0, sizeof(g_exec));
		respond_error(fd, 500, "Internal Server Error", "could not watch the command");
		return;
	}
	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = timeout_s;
	timerfd_settime(tfd, 0, &its, NULL);

	g_exec.out_conn = exec_register(read_fd, CONN_EXEC_OUTPUT);
	g_exec.child_conn = exec_register(pidfd, CONN_EXEC_CHILD);
	g_exec.timer_conn = exec_register(tfd, CONN_EXEC_TIMER);
	if (g_exec.out_conn == NULL || g_exec.child_conn == NULL || g_exec.timer_conn == NULL) {
		exec_job_finish(-1, 0);
		memset(&g_exec, 0, sizeof(g_exec));
		respond_error(fd, 500, "Internal Server Error", "could not watch the command");
		return;
	}
	handle_container_exec_get(fd, name);
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
	struct thinc_epoll_event ev;
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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, sockfd, &ev) != 0) {
		perror("epoll_ctl ADD ping socket");
		free(sock_cc);
		free(timer_cc);
		close(tfd);
		close(sockfd);
		respond_error(fd, 500, "Internal Server Error", "could not register ping socket");
		return;
	}
	ev.data.ptr = timer_cc;
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, tfd, &ev) != 0) {
		perror("epoll_ctl ADD ping timer");
		thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, sockfd, NULL);
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
		thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_ntp_sync_conn->fd, NULL);
		close(g_ntp_sync_conn->fd);
		g_ntp_sync_conn->kind = CONN_DEAD;
		queue_conn_free(g_ntp_sync_conn);
		g_ntp_sync_conn = NULL;
	}
	if (g_ntp_timer_conn != NULL) {
		thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_ntp_timer_conn->fd, NULL);
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
	struct thinc_epoll_event ev;
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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, sockfd, &ev) != 0) {
		perror("epoll_ctl ADD ntp sync socket");
		free(sock_cc);
		free(timer_cc);
		close(tfd);
		close(sockfd);
		return NTP_START_SOCKET_FAILED;
	}
	ev.data.ptr = timer_cc;
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, tfd, &ev) != 0) {
		perror("epoll_ctl ADD ntp sync timer");
		thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, sockfd, NULL);
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
/* ---- Issue #81: registered-server health probing ---- */

/*
 * How often a full probe sweep runs. 30s is a deliberate middle ground:
 * fast enough that a dead directory server is noticed long before an
 * operator finishes reading a failed login, slow enough that four
 * subsystems' worth of probes never become their own load.
 */
#define SERVERHEALTH_SWEEP_SECONDS 30
/*
 * A probe that has neither connected nor been refused within this long
 * counts as failed. Kept well under the sweep interval so a sweep can
 * never overlap the previous one's stragglers.
 */
#define SERVERHEALTH_PROBE_TIMEOUT_SECONDS 5

static struct conn g_serverhealth_timer_conn;

/*
 * The one place that says what "probing" means for each kind. Adding a
 * fifth registered-server subsystem means adding a row here, nothing
 * else -- the record, the state machine and the whole REST/CLI/web
 * surface are already shared.
 *
 * port == 0 means this kind has no TCP service to connect to (NTP and
 * syslog are UDP, and a TCP connect against them would be a meaningless
 * check dressed up as a real one). Those fall back to container
 * liveness, and the record's own `probe` field reports "process" so an
 * operator can see exactly how much the verdict is worth.
 */
struct serverhealth_kind_spec {
	const char *kind;
	int port;
	int (*list)(char out[][REGISTRY_NAME_MAX], int max);
};

static int serverhealth_list_ldap(char out[][REGISTRY_NAME_MAX], int max)
{
	return ldap_server_list_containers(out, max);
}
static int serverhealth_list_dns(char out[][REGISTRY_NAME_MAX], int max)
{
	return dns_server_list_containers(out, max);
}
static int serverhealth_list_ntp(char out[][REGISTRY_NAME_MAX], int max)
{
	return ntp_server_list_containers(out, max);
}
static int serverhealth_list_syslog(char out[][REGISTRY_NAME_MAX], int max)
{
	return syslogfwd_target_list_containers(out, max);
}

static const struct serverhealth_kind_spec g_serverhealth_kinds[] = {
	{ "ldap", HOSTAUTH_LDAP_DEFAULT_PORT, serverhealth_list_ldap },
	{ "dns", 53, serverhealth_list_dns },
	{ "ntp", 0, serverhealth_list_ntp },
	{ "syslog", 0, serverhealth_list_syslog },
};

/*
 * In-flight probes, so a sweep can time out stragglers rather than
 * leaving them to the kernel's own multi-minute TCP timeout (which would
 * let sweeps pile up on an unreachable server). Bounded by the same cap
 * as the health table itself -- there can never be more probes in flight
 * than there are registered servers.
 */
static struct conn *g_serverhealth_inflight[SERVERHEALTH_MAX];

static void serverhealth_inflight_add(struct conn *cc)
{
	int i;

	for (i = 0; i < SERVERHEALTH_MAX; i++) {
		if (g_serverhealth_inflight[i] == NULL) {
			g_serverhealth_inflight[i] = cc;
			return;
		}
	}
}

static void serverhealth_inflight_remove(struct conn *cc)
{
	int i;

	for (i = 0; i < SERVERHEALTH_MAX; i++) {
		if (g_serverhealth_inflight[i] == cc)
			g_serverhealth_inflight[i] = NULL;
	}
}

static void serverhealth_probe_teardown(struct conn *cc)
{
	serverhealth_inflight_remove(cc);
	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	close(cc->fd);
	free(cc);
}

/* Fails every probe that has been outstanding too long -- called at the
 * start of each sweep, so a hung server is recorded as failing rather
 * than silently accumulating sockets. */
static void serverhealth_reap_stale_probes(void)
{
	time_t now = time(NULL);
	int i;

	for (i = 0; i < SERVERHEALTH_MAX; i++) {
		struct conn *cc = g_serverhealth_inflight[i];

		if (cc == NULL)
			continue;
		if (now - cc->probe_started_at < SERVERHEALTH_PROBE_TIMEOUT_SECONDS)
			continue;
		serverhealth_record_result(cc->probe_kind, cc->probe_container, cc->probe_desc, 0,
		                            "probe timed out -- no response");
		serverhealth_probe_teardown(cc);
	}
}

/*
 * Resolves a registered server container's own management IP -- the same
 * "look up the registry entry fresh, never cache" resolution
 * ldap_effective_client_uri() already does. Returns 0 if the container
 * isn't running or has no address yet, which is itself a real answer:
 * a server that isn't up cannot be serving.
 */
static int serverhealth_resolve_ip(const char *container, char *out, size_t out_size)
{
	struct registry_entry *e = registry_find(container);
	struct in_addr a;

	if (e == NULL || !e->running || e->net_count == 0 || e->nets[0].ip_be == 0)
		return 0;
	a.s_addr = e->nets[0].ip_be;
	return inet_ntop(AF_INET, &a, out, (socklen_t)out_size) != NULL;
}

/*
 * Starts one non-blocking TCP probe. The connect() is expected to return
 * EINPROGRESS; the verdict is taken later, when the reactor reports the
 * socket writable (see handle_serverhealth_probe_event()). Nothing here
 * ever blocks.
 */
static void serverhealth_start_tcp_probe(const char *kind, const char *container, const char *ip,
                                          int port)
{
	struct sockaddr_in sa;
	struct conn *cc;
	struct thinc_epoll_event ev;
	char desc[SERVERHEALTH_PROBE_MAX];
	int fd;

	snprintf(desc, sizeof(desc), "tcp:%d", port);
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		serverhealth_record_result(kind, container, desc, 0, "socket() failed");
		return;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
		close(fd);
		serverhealth_record_result(kind, container, desc, 0, "bad server address");
		return;
	}
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 && errno != EINPROGRESS) {
		char err[SERVERHEALTH_ERROR_MAX];

		snprintf(err, sizeof(err), "connect: %s", strerror(errno));
		close(fd);
		serverhealth_record_result(kind, container, desc, 0, err);
		return;
	}
	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		close(fd);
		return;
	}
	memset(cc, 0, sizeof(*cc));
	cc->kind = CONN_SERVERHEALTH_PROBE;
	cc->fd = fd;
	snprintf(cc->probe_kind, sizeof(cc->probe_kind), "%s", kind);
	snprintf(cc->probe_container, sizeof(cc->probe_container), "%s", container);
	snprintf(cc->probe_desc, sizeof(cc->probe_desc), "%s", desc);
	cc->probe_started_at = time(NULL);

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLOUT;
	ev.data.ptr = cc;
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		close(fd);
		free(cc);
		serverhealth_record_result(kind, container, desc, 0, "epoll registration failed");
		return;
	}
	serverhealth_inflight_add(cc);
}

/* One sweep: every registered server of every kind, probed once. */
static void serverhealth_sweep(void)
{
	size_t k;

	serverhealth_reap_stale_probes();

	for (k = 0; k < sizeof(g_serverhealth_kinds) / sizeof(g_serverhealth_kinds[0]); k++) {
		const struct serverhealth_kind_spec *spec = &g_serverhealth_kinds[k];
		char names[SERVERHEALTH_MAX][REGISTRY_NAME_MAX];
		int count = spec->list(names, SERVERHEALTH_MAX);
		int i;

		for (i = 0; i < count; i++) {
			char ip[INET_ADDRSTRLEN];

			if (!serverhealth_resolve_ip(names[i], ip, sizeof(ip))) {
				serverhealth_record_result(spec->kind, names[i],
				                            spec->port > 0 ? "tcp" : "process", 0,
				                            "container is not running, or has no address");
				continue;
			}
			if (spec->port == 0) {
				/* UDP service: the honest check available without
				 * speaking its protocol is that its container is
				 * genuinely up, and the record says exactly that. */
				serverhealth_record_result(spec->kind, names[i], "process", 1, NULL);
				continue;
			}
			serverhealth_start_tcp_probe(spec->kind, names[i], ip, spec->port);
		}
	}
}

static void arm_serverhealth_timer(void)
{
	struct itimerspec its;

	if (g_serverhealth_timer_conn.fd < 0)
		return;
	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = SERVERHEALTH_SWEEP_SECONDS;
	if (timerfd_settime(g_serverhealth_timer_conn.fd, 0, &its, NULL) != 0)
		perror("timerfd_settime (server health sweep re-arm)");
}

/* Best-effort, same posture as every other periodic timer here: failing
 * to create it means no automatic probing, never a failed startup. */
/*
 * Issue #86: keep thincd schedulable and un-killable under overload.
 * Best-effort by design -- see the call site's own comment. Reports what
 * it could not do rather than failing silently, so a real install where
 * one of these is unexpectedly denied is visible in the log.
 */
static void protect_control_plane(void)
{
	int fd;

	/*
	 * Strongly negative nice: ordinary workloads (containers, package
	 * builds) then cannot push the daemon off the run queue no matter how
	 * much CPU they collectively demand. -20 rather than something milder
	 * because there is nothing on the box whose latency matters more than
	 * the one interface an operator has.
	 */
	errno = 0;
	if (setpriority(PRIO_PROCESS, 0, -20) != 0 && errno != 0)
		fprintf(stderr, "control plane: could not raise scheduling priority: %s\n",
		        strerror(errno));

	/*
	 * And never be the OOM killer's choice. -1000 is the strongest
	 * available "do not kill this" hint; if the daemon is the thing that
	 * dies under memory pressure, the operator loses the box rather than
	 * a workload.
	 */
	fd = open("/proc/self/oom_score_adj", O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "control plane: could not open oom_score_adj: %s\n", strerror(errno));
		return;
	}
	if (write(fd, "-1000", 5) != 5)
		fprintf(stderr, "control plane: could not set oom_score_adj: %s\n", strerror(errno));
	close(fd);
}

static void start_serverhealth_timer(void)
{
	struct thinc_epoll_event ev;
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

	g_serverhealth_timer_conn.fd = -1;
	if (fd < 0) {
		perror("timerfd_create (server health)");
		return;
	}
	g_serverhealth_timer_conn.kind = CONN_SERVERHEALTH_TIMER;
	g_serverhealth_timer_conn.fd = fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_serverhealth_timer_conn;
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		close(fd);
		g_serverhealth_timer_conn.fd = -1;
		return;
	}
	arm_serverhealth_timer();
}

/*
 * The probe socket became writable: SO_ERROR carries the real verdict.
 * 0 means the server accepted a connection -- a genuine service check,
 * not a guess. ECONNREFUSED and friends mean it is listening on nothing,
 * which is exactly the "registered but not serving" state this whole
 * mechanism exists to surface.
 */
static void handle_serverhealth_probe_event(struct conn *cc)
{
	int soerr = 0;
	socklen_t len = sizeof(soerr);

	if (getsockopt(cc->fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0)
		soerr = errno;
	if (soerr == 0) {
		serverhealth_record_result(cc->probe_kind, cc->probe_container, cc->probe_desc, 1, NULL);
	} else {
		char err[SERVERHEALTH_ERROR_MAX];

		snprintf(err, sizeof(err), "connect: %s", strerror(soerr));
		serverhealth_record_result(cc->probe_kind, cc->probe_container, cc->probe_desc, 0, err);
	}
	serverhealth_probe_teardown(cc);
}

static void start_ntp_periodic_timer(void)
{
	struct thinc_epoll_event ev;
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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
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
 * ADR-0160: GET/PUT/DELETE /v1/system/sysctl/{key}, GET /v1/system/sysctl
 * -- host-level /proc/sys REST surface, live, fully open passthrough
 * (host-auth write-gating is the only access control -- no allowlist,
 * confirmed with the user, despite the real blast radius some vm./
 * kernel. keys carry). Key translation (dots -> slashes) reuses
 * container_net_apply_sysctl() (src/container_net.c) directly for
 * writes -- the exact same scheme the existing per-container `sysctl`
 * field already uses, this daemon's own root netns instead of a
 * container's; that function only ever writes, so a small local read
 * counterpart mirrors its translation for GET.
 */
static int read_proc_sysctl(const char *key, char *out, size_t out_size)
{
	char path[16 + SYSCTL_KEY_MAX];
	char *p;
	int fd;
	ssize_t n;

	if (snprintf(path, sizeof(path), "/proc/sys/%s", key) >= (int)sizeof(path))
		return -1;
	for (p = path; *p != '\0'; p++) {
		if (*p == '.')
			*p = '/';
	}

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, out, out_size - 1);
	close(fd);
	if (n < 0)
		return -1;
	out[n] = '\0';
	/* /proc/sys values are conventionally newline-terminated -- trimmed
	 * so a single-token value's own JSON string doesn't carry a
	 * trailing "\n" nothing else in this project's JSON output does. */
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		out[--n] = '\0';
	return 0;
}

/* Normalizes a PUT body's "value" field -- a plain JSON string, or a
 * JSON array of strings (joined with a single space, the real
 * kernel's own separator convention for every known tuple-shaped
 * sysctl) -- into the one canonical string form both the live write
 * and (if persisted) sysctlconfig_set() use. */
static int sysctl_value_from_json(const struct json_value *jval, char *out, size_t out_size)
{
	if (jval == NULL)
		return -1;
	if (jval->type == JSON_STRING) {
		const char *s = json_as_string(jval);

		if (s == NULL || strlen(s) >= out_size)
			return -1;
		snprintf(out, out_size, "%s", s);
		return 0;
	}
	if (jval->type == JSON_ARRAY) {
		size_t i;
		size_t len = 0;

		out[0] = '\0';
		for (i = 0; i < jval->u.array.count; i++) {
			const char *tok = json_as_string(jval->u.array.items[i]);
			size_t tok_len;

			if (tok == NULL)
				return -1;
			tok_len = strlen(tok);
			if (len + (i > 0 ? 1 : 0) + tok_len >= out_size)
				return -1;
			if (i > 0)
				out[len++] = ' ';
			memcpy(out + len, tok, tok_len);
			len += tok_len;
		}
		out[len] = '\0';
		return 0;
	}
	return -1;
}

static void handle_sysctl_get(int fd, const char *key)
{
	char value[SYSCTL_VALUE_MAX];
	struct json_writer w;

	if (!sysctl_key_is_valid(key)) {
		respond_error(fd, 400, "Bad Request", "invalid sysctl key");
		return;
	}
	if (read_proc_sysctl(key, value, sizeof(value)) != 0) {
		respond_error(fd, 404, "Not Found", "no such sysctl key");
		return;
	}
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "key");
	jw_str(&w, key);
	jw_key(&w, "value");
	sysctl_value_write_json(value, &w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_sysctl_put(int fd, const char *key, const char *body, size_t body_len)
{
	struct json_value *root;
	char value[SYSCTL_VALUE_MAX];
	int persist = 1;
	const struct json_value *jpersist;

	if (!sysctl_key_is_valid(key)) {
		respond_error(fd, 400, "Bad Request", "invalid sysctl key");
		return;
	}

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	if (sysctl_value_from_json(json_object_get(root, "value"), value, sizeof(value)) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "value (string or array of strings) is required");
		return;
	}
	jpersist = json_object_get(root, "persist");
	if (jpersist != NULL && jpersist->type == JSON_BOOL)
		persist = jpersist->u.boolean;
	json_free(root);

	if (container_net_apply_sysctl(key, value) != 0) {
		respond_error(fd, 400, "Bad Request", "the kernel rejected this key/value");
		return;
	}

	if (persist) {
		enum sysctlconfig_error serr = sysctlconfig_set(key, value);

		if (serr != SYSCTLCONFIG_OK) {
			/* The live write already succeeded -- there is no clean way
			 * to "unwrite" a sysctl, and the live value is now correct
			 * regardless of whether it ends up remembered for next
			 * boot, so this is reported, not rolled back. */
			respond_error(fd, 500, "Internal Server Error",
			              "sysctl applied live but could not be persisted");
			return;
		}
	}

	{
		char out_value[SYSCTL_VALUE_MAX];
		struct json_writer w;

		if (read_proc_sysctl(key, out_value, sizeof(out_value)) != 0)
			snprintf(out_value, sizeof(out_value), "%s", value);
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "key");
		jw_str(&w, key);
		jw_key(&w, "value");
		sysctl_value_write_json(out_value, &w);
		jw_obj_close(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

static void handle_sysctl_delete(int fd, const char *key)
{
	enum sysctlconfig_error serr;

	if (!sysctl_key_is_valid(key)) {
		respond_error(fd, 400, "Bad Request", "invalid sysctl key");
		return;
	}
	serr = sysctlconfig_delete(key);
	if (serr == SYSCTLCONFIG_ERR_NOT_FOUND) {
		respond_error(fd, 404, "Not Found", "no persisted sysctl entry for this key");
		return;
	}
	if (serr != SYSCTLCONFIG_OK) {
		respond_error(fd, 500, "Internal Server Error", "could not persist removal");
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_sysctl_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "sysctls");
	sysctlconfig_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * ADR-0159 Phase A: POST/DELETE/GET /v1/system/kmod/{name} (real
 * modprobe/modinfo, see kmod.c) and GET /v1/system/kmod (a live
 * /proc/modules dump). A modprobe/modinfo failure -- overwhelmingly
 * "no such module" in practice, the one case a bare exit code can't
 * usefully distinguish from "bad option" or "tool not staged" -- is
 * reported as 404 uniformly across all three, rather than guessing at
 * a finer-grained status from output this project never parses for
 * that purpose.
 */
static void handle_kmod_post(int fd, const char *name, const char *body, size_t body_len)
{
	char options[KMOD_OPTIONS_MAX];
	struct json_value *root = NULL;
	const struct json_value *joptions = NULL;

	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}

	if (body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		joptions = json_object_get(root, "options");
	}

	if (joptions != NULL) {
		if (kmod_options_from_json(joptions, options, sizeof(options)) != 0) {
			json_free(root);
			respond_error(fd, 400, "Bad Request",
			              "options must be an object of string values");
			return;
		}
	} else {
		const char *def = kmodconfig_get_options(name);

		snprintf(options, sizeof(options), "%s", def != NULL ? def : "");
	}
	json_free(root);

	if (kmod_load(name, options) != 0) {
		respond_error(fd, 404, "Not Found", "modprobe could not load this module");
		return;
	}

	{
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, name);
		jw_key(&w, "options");
		kmod_options_write_json(options, &w);
		jw_obj_close(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

static void handle_kmod_delete(int fd, const char *name)
{
	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}
	if (kmod_unload(name) != 0) {
		respond_error(fd, 404, "Not Found", "modprobe -r could not unload this module");
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_kmod_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "modules");
	kmod_write_json_loaded(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_kmod_get(int fd, const char *name)
{
	struct json_writer w;

	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}
	jw_init(&w);
	if (kmod_write_json_info(name, &w) != 0) {
		jw_free(&w);
		respond_error(fd, 404, "Not Found", "no such module (not built/available)");
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void respond_kmodconfig_error(int fd, enum kmodconfig_error kerr)
{
	switch (kerr) {
	case KMODCONFIG_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid module name, or options too long");
		break;
	case KMODCONFIG_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "kmod config table full");
		break;
	case KMODCONFIG_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no persisted kmod config for this module");
		break;
	case KMODCONFIG_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "kmod config operation failed");
		break;
	}
}

static void handle_kmodconfig_put(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *joptions;
	const struct json_value *jautoload;
	char options[KMOD_OPTIONS_MAX];
	const char *options_ptr = NULL;
	int has_autoload = 0;
	int autoload_value = 0;
	enum kmodconfig_error kerr;

	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	joptions = json_object_get(root, "default_options");
	if (joptions != NULL) {
		if (kmod_options_from_json(joptions, options, sizeof(options)) != 0) {
			json_free(root);
			respond_error(fd, 400, "Bad Request",
			              "default_options must be an object of string values");
			return;
		}
		options_ptr = options;
	}

	jautoload = json_object_get(root, "autoload");
	if (jautoload != NULL) {
		if (jautoload->type != JSON_BOOL) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "autoload must be a boolean");
			return;
		}
		has_autoload = 1;
		autoload_value = jautoload->u.boolean;
	}
	json_free(root);

	if (options_ptr == NULL && !has_autoload) {
		respond_error(fd, 400, "Bad Request",
		              "at least one of default_options/autoload is required");
		return;
	}

	kerr = kmodconfig_set(name, options_ptr, has_autoload, autoload_value);
	if (kerr != KMODCONFIG_OK) {
		respond_kmodconfig_error(fd, kerr);
		return;
	}

	{
		struct json_writer w;

		jw_init(&w);
		kmodconfig_write_json_one(name, &w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

static void handle_kmodconfig_delete(int fd, const char *name)
{
	enum kmodconfig_error kerr;

	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}
	kerr = kmodconfig_delete(name);
	if (kerr != KMODCONFIG_OK) {
		respond_kmodconfig_error(fd, kerr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_kmodconfig_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "kmod_config");
	kmodconfig_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * GET/PUT /v1/system/daemon-config (Part 0.5): thincd's own listen
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
			              "network has no address for thincd to bind to");
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
	 * differs) by the time this runs.
	 *
	 * A failed start is only a hard error (500) when THIS request
	 * explicitly asked for https_enabled (have_https_req) -- an
	 * explicit ask that can't be honored deserves a clear failure.
	 * https_enabled now defaults to true on a fresh install (ADR-0171),
	 * so want_https is routinely true on a request that never mentioned
	 * https at all (e.g. only port/management_network/bind_ip) -- for
	 * that case, a pre-PKI-bootstrap install with no usable host cert
	 * yet must not block the actually-requested change; soft-fail (log,
	 * continue), the same non-fatal posture the boot-time attempt
	 * already has. */
	if (want_https) {
		if (g_https_listener_conn.fd < 0) {
			if (start_https_listener(g_bind_addr, new_https_port) != 0) {
				if (have_https_req) {
					respond_error(fd, 500, "Internal Server Error",
					              "could not start https listener (no usable host cert yet?)");
					return;
				}
				fprintf(stderr,
				        "https_enabled (default) but could not start the HTTPS listener -- "
				        "continuing without it\n");
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
 * already gives it. Not a persisted thinC resource (see
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
	swap_write_json(&w, storageplacement_get(STORAGE_KIND_SWAP));
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * issue #28: resolves disk_name to a real, currently-mounted disk
 * carrying the swap role, writing its swapfile path into out_path.
 * Same three checks (present, mounted, correct role) do_backup_
 * snapshot_now() already established for backup's own identically-shaped
 * "operator-named disk, validated at the point of real use" field --
 * deliberately not shared as a common helper, matching that this
 * project has never factored these three checks out for the other
 * three (state/rebuildable/log) singleton placements either. Returns
 * NULL (a real, specific reason) on failure, or out_path on success.
 */
static const char *resolve_swap_disk_now(const char *disk_name, char *out_path, size_t out_path_size)
{
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n, i;

	n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
	for (i = 0; i < n; i++) {
		const char *role;

		if (strcmp(disks[i].name, disk_name) != 0)
			continue;
		if (!disks[i].mounted)
			return NULL;
		role = diskrole_lookup(disk_name);
		if (role == NULL || strcmp(role, "swap") != 0)
			return NULL;
		snprintf(out_path, out_path_size, "%s/%s/swapfile", DISKS_MOUNT_DIR, disk_name);
		return out_path;
	}
	return NULL;
}

static void handle_swap_enable(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jsize, *jdisk;
	int64_t size_mb;
	const char *disk_name;
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
	jdisk = json_object_get(root, "disk");
	disk_name = jdisk != NULL ? json_as_string(jdisk) : NULL;
	if (jdisk != NULL && disk_name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "disk must be a string");
		return;
	}

	/*
	 * Checked before touching repoint/placement at all: repointing
	 * while already enabled would desync g_file_path from the file
	 * swapon(2) is actually holding open, corrupting swap_write_json()'s
	 * own "path" field, for a call about to fail with
	 * SWAP_ERR_ALREADY_ENABLED regardless (swap_enable() below re-checks
	 * this itself -- this is not a substitute for that, just avoiding a
	 * side effect ahead of a call already known to fail).
	 */
	if (swap_is_enabled()) {
		json_free(root);
		respond_swap_error(fd, SWAP_ERR_ALREADY_ENABLED);
		return;
	}

	/*
	 * Every call repoints, whether disk_name is given or not -- an
	 * omitted disk explicitly means "the default location," not
	 * "whatever the last call happened to leave it at." disk_name is
	 * copied out of root before json_free() below, since resolve_swap_
	 * disk_now()/storageplacement_set() both need it to outlive that.
	 */
	if (disk_name != NULL) {
		char resolved_path[PATH_MAX];
		char disk_name_copy[DISKROLE_DISK_NAME_MAX];

		snprintf(disk_name_copy, sizeof(disk_name_copy), "%s", disk_name);
		json_free(root);
		if (resolve_swap_disk_now(disk_name_copy, resolved_path, sizeof(resolved_path)) == NULL) {
			respond_error(fd, 400, "Bad Request",
			              "disk is not currently present, not mounted, or does not carry the "
			              "swap role (POST /diskroles first)");
			return;
		}
		swap_repoint(resolved_path);
		storageplacement_set(STORAGE_KIND_SWAP, disk_name_copy);
	} else {
		json_free(root);
		swap_repoint(SWAP_FILE_PATH);
		storageplacement_set(STORAGE_KIND_SWAP, NULL);
	}

	serr = swap_enable(size_mb);
	if (serr != SWAP_OK) {
		respond_swap_error(fd, serr);
		return;
	}

	jw_init(&w);
	swap_write_json(&w, storageplacement_get(STORAGE_KIND_SWAP));
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

/*
 * The host-side veth for a container's Nth network attachment. The
 * name is a convention (src/container_net.c coins it at creation from
 * the child's pid), not something stored -- so it is derived in one
 * place rather than re-spelled at each call site, which is how the
 * stats reader and the teardown path came to carry the same format
 * string twice.
 *
 * A live-attached network (ADR-0156) is the exception: it was added to
 * an already-running container and carries its real name, since it was
 * never coined from the pid at all.
 */
static void container_veth_host_name(const struct registry_entry *e, int idx, char *out,
                                      size_t out_size)
{
	if (e->nets[idx].veth_host[0] != '\0') {
		snprintf(out, out_size, "%s", e->nets[idx].veth_host);
		return;
	}
	snprintf(out, out_size, "vh%d-%d", (int)e->handle.pid, idx);
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

/*
 * Host uptime, straight from /proc/uptime's own first field (seconds
 * since boot, as a float). Best-effort like read_loadavg(): an
 * unreadable /proc/uptime reports 0 rather than failing the whole
 * stats response, and 0 is distinguishable from any real answer
 * because a booted box has always been up for something.
 */
static long long read_host_uptime_seconds(void)
{
	FILE *f;
	double up = 0.0;

	f = fopen("/proc/uptime", "r");
	if (f == NULL)
		return 0;
	if (fscanf(f, "%lf", &up) != 1)
		up = 0.0;
	fclose(f);
	return (long long)up;
}

/*
 * When this daemon process itself started, stamped once at startup.
 * Deliberately separate from host uptime: they differ after a daemon
 * restart that was not a reboot (a `system/update` confirm, a crash
 * and respawn), and "the box has been up for days but the control
 * plane restarted four minutes ago" is exactly the thing an operator
 * needs to be able to see.
 */
static time_t g_daemon_started_at;

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

	jw_key(&w, "uptime");
	jw_obj_open(&w);
	jw_key(&w, "host_seconds");
	jw_int(&w, read_host_uptime_seconds());
	jw_key(&w, "daemon_seconds");
	jw_int(&w, (long long)(time(NULL) - g_daemon_started_at));
	jw_obj_close(&w);
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
/*
 * GET /v1/system/server-health (issue #81) -- every registered server of
 * every kind, with its live health, whether it is in service, and how it
 * was probed.
 */
/*
 * Issue #83: health can only ever describe servers thinC KNOWS about, so
 * an unregistered one is invisible to it by construction -- which is
 * exactly how a real box ran for its whole life with DNS records that
 * resolved nothing, because dns-1/dns-2 were never registered and
 * dns_record_sync_all() only ever writes into registered servers.
 *
 * The precisely detectable version of that failure is "thinC is managing
 * state it has nowhere to deliver": records/accounts exist, zero servers
 * are registered to receive them. Reported as real warnings alongside the
 * health list. Deliberately only for the two subsystems that actually
 * PUSH managed state to their servers -- NTP and syslog have no
 * equivalent stranded state, so inventing a check for them would be
 * noise dressed up as diagnostics.
 */
static void serverhealth_write_warnings(struct json_writer *w)
{
	char names[SERVERHEALTH_MAX][REGISTRY_NAME_MAX];
	char msg[256];
	int records, servers;

	jw_arr_open(w);

	records = dns_record_count();
	servers = dns_server_list_containers(names, SERVERHEALTH_MAX);
	if (records > 0 && servers == 0) {
		snprintf(msg, sizeof(msg),
		         "%d DNS record(s) are managed but no DNS server is registered -- they are "
		         "not being served by anything. Register the container running your resolver "
		         "(POST /v1/dns/servers).",
		         records);
		jw_str(w, msg);
	}

	servers = ldap_server_list_containers(names, SERVERHEALTH_MAX);
	if (servers == 0 && (ldap_user_count() > 0 || ldap_group_count() > 0)) {
		snprintf(msg, sizeof(msg),
		         "LDAP users/groups are managed but no LDAP server is registered -- they are "
		         "not being served by anything. Register the container running your directory "
		         "(POST /v1/ldap/servers).");
		jw_str(w, msg);
	}
	jw_arr_close(w);
}

/* ---- Issue #88: persistent volumes ---- */

static void respond_volume_error(int fd, enum volume_error e)
{
	switch (e) {
	case VOLUME_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request",
		              "invalid volume name -- letters, digits, '_' and '-' only, not starting "
		              "with '.' or '-'");
		return;
	case VOLUME_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "a volume with this name already exists");
		return;
	case VOLUME_ERR_FULL:
		respond_error(fd, 507, "Insufficient Storage", "volume table is full");
		return;
	case VOLUME_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	case VOLUME_ERR_IN_USE:
		respond_error(fd, 409, "Conflict",
		              "this volume is still referenced by a container definition -- delete or "
		              "edit that container first");
		return;
	case VOLUME_ERR_IO:
		respond_error(fd, 500, "Internal Server Error", "could not create the volume directory");
		return;
	case VOLUME_ERR_TARGET_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such disk or partition to migrate onto");
		return;
	case VOLUME_ERR_TARGET_NOT_READY:
		respond_error(fd, 409, "Conflict",
		              "that disk is not mounted -- give it a role and format it first");
		return;
	case VOLUME_ERR_SAME_PLACE:
		respond_error(fd, 409, "Conflict", "this volume is already there");
		return;
	case VOLUME_ERR_IN_USE_RUNNING:
		respond_error(fd, 409, "Conflict",
		              "a container mounting this volume is running -- a bind mount resolves to a "
		              "host path when the container starts, so moving the data underneath it "
		              "would leave it writing to the old location. Stop the container first");
		return;
	case VOLUME_ERR_COPY_FAILED:
		respond_error(fd, 500, "Internal Server Error",
		              "could not copy the volume's data to the new location -- nothing was moved "
		              "and the volume still points at its original data");
		return;
	default:
		respond_error(fd, 500, "Internal Server Error", "could not persist the volume");
		return;
	}
}

static void handle_volume_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "volumes");
	volume_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_volume_get(int fd, const char *name)
{
	struct volume *v = volume_find(name);
	struct json_writer w;

	if (v == NULL) {
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	}
	jw_init(&w);
	volume_write_json_one(v, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_volume_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const char *name, *disk;
	struct volume *v = NULL;
	enum volume_error verr;
	struct json_writer w;

	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	name = json_as_string(json_object_get(root, "name"));
	disk = json_as_string(json_object_get(root, "disk"));
	if (name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name is required");
		return;
	}
	{
		/*
		 * Issue #102: owner at creation, because the moment a volume
		 * exists is the only moment its contents are certainly empty
		 * -- setting it later means deciding what to do about files
		 * that are already there, which is a question worth not having
		 * to ask.
		 */
		const struct json_value *ju = json_object_get(root, "owner_uid");
		const struct json_value *jg = json_object_get(root, "owner_gid");
		int uid = ju != NULL ? (int)json_as_number(ju) : -1;
		int gid = jg != NULL ? (int)json_as_number(jg) : -1;

		if ((ju != NULL) != (jg != NULL)) {
			json_free(root);
			respond_error(fd, 400, "Bad Request",
			              "owner_uid and owner_gid go together -- a volume owned by one user and "
			              "an unrelated group is almost always a typo");
			return;
		}
		if (ju != NULL && (uid < 0 || gid < 0)) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "owner_uid/owner_gid must not be negative");
			return;
		}

		verr = volume_create(name, disk, &v);
		if (verr != VOLUME_OK) {
			json_free(root);
			respond_volume_error(fd, verr);
			return;
		}
		if (ju != NULL) {
			verr = volume_set_owner(name, uid, gid);
			if (verr == VOLUME_OK)
				verr = volume_apply_owner(v, 0);
			if (verr != VOLUME_OK) {
				json_free(root);
				/* The directory exists and is root-owned: report the
				 * real outcome rather than a 201 that implies an owner
				 * that was never applied. */
				respond_volume_error(fd, verr);
				return;
			}
		}
		/*
		 * Freed only now. `name` points INTO this tree, and every call
		 * above takes it -- freeing before them left volume_set_owner()
		 * looking up a name in freed memory and answering "no such
		 * volume" for one it had just created.
		 */
		json_free(root);
	}
	jw_init(&w);
	volume_write_json_one(v, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

/*
 * Issue #88: a volume outlives containers by design, so deleting one is
 * refused while any container DEFINITION still names it -- not merely
 * while one is running. A stopped container whose definition references
 * the volume will come back and expect its data, and silently deleting
 * it underneath would be a data-loss bug that only surfaces later.
 */
/*
 * Does this container's definition mount that volume. Split out of
 * volume_referenced_by_container() below, which answers "is anything
 * using it" and stops at the first match -- pausing needs every one.
 */
static int container_mounts_volume(const char *container_name, const char *volume_name)
{
	struct container_def *def = containerdef_find(container_name);
	struct json_value *root;
	const struct json_value *jvols;
	size_t k;
	int found = 0;

	if (def == NULL || def->body == NULL)
		return 0;
	root = json_parse(def->body, def->body_len);
	if (root == NULL)
		return 0;
	jvols = json_object_get(root, "volumes");
	if (jvols != NULL && jvols->type == JSON_ARRAY) {
		for (k = 0; k < jvols->u.array.count && !found; k++) {
			const char *n = json_as_string(json_object_get(jvols->u.array.items[k], "name"));

			if (n != NULL && strcmp(n, volume_name) == 0)
				found = 1;
		}
	}
	json_free(root);
	return found;
}

static int volume_referenced_by_container(const char *volume_name, char *out_container,
                                           size_t out_size)
{
	char order[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	int count = containerdef_resolve_order(order);
	int i;

	for (i = 0; i < count; i++) {
		struct container_def *def = containerdef_find(order[i]);
		struct json_value *root;
		const struct json_value *jvols;
		size_t k;

		if (def == NULL)
			continue;
		root = json_parse(def->body, def->body_len);
		if (root == NULL)
			continue;
		jvols = json_object_get(root, "volumes");
		if (jvols != NULL && jvols->type == JSON_ARRAY) {
			for (k = 0; k < jvols->u.array.count; k++) {
				const char *n = json_as_string(json_object_get(jvols->u.array.items[k], "name"));

				if (n != NULL && strcmp(n, volume_name) == 0) {
					snprintf(out_container, out_size, "%s", order[i]);
					json_free(root);
					return 1;
				}
			}
		}
		json_free(root);
	}
	return 0;
}

/*
 * Issue #96: does any container mounting this volume have a live
 * process right now.
 *
 * Injected into volumebackup.c rather than looked up there, so that
 * module needs no knowledge of the registry or of container
 * definitions -- the same shape containerdef.c already gets its own
 * liveness predicate through.
 *
 * registry_find() alone is NOT the test: since ADR-0181 the registry
 * also holds exited containers, so a container that ran once and
 * stopped would block every snapshot forever.
 */
static int volume_has_running_container(const char *volume_name)
{
	char user[REGISTRY_NAME_MAX];
	struct registry_entry *e;

	if (!volume_referenced_by_container(volume_name, user, sizeof(user)))
		return 0;
	e = registry_find(user);
	return e != NULL && e->running;
}

/*
 * Issue #93: apply a volume's size limit to its real directory.
 *
 * Two operations, the same pair a container overlay already uses: tag
 * the directory with a project id so everything inside it counts, and
 * set that project's byte limit via quotactl(2). Reusing the overlay's
 * own tagging call rather than repeating the ioctls here -- a second
 * implementation of something that must stay identical, on a path where
 * getting it subtly wrong yields a quota that reports as applied and is
 * not.
 *
 * btrfs is refused rather than silently unenforced. There the limit
 * lives on a qgroup attached to a subvolume, and a volume directory is
 * not a subvolume -- making it one is a real change to how volumes are
 * created, not another call. Accepting the request and quietly not
 * enforcing it would be exactly the "false promise" overlay.c's own
 * comment warns about.
 *
 * quota_bytes of 0 clears the limit.
 */
static int volume_apply_quota(const char *volume_name, const char *dir, long long quota_bytes,
                              char *err, size_t err_size)
{
	uint32_t projid;

	if (overlay_backing_is_btrfs(dir)) {
		snprintf(err, err_size,
		         "this volume is on a btrfs filesystem, where a size limit needs the volume to be "
		         "its own subvolume -- not supported yet, and refused rather than accepted and "
		         "not enforced");
		return -1;
	}
	/* A distinct project id per volume, from the same allocator
	 * containers use -- ids are never reused, so a deleted volume's id
	 * can never silently start limiting a new one. */
	if (quotamap_get_or_assign(volume_name, &projid) != 0) {
		snprintf(err, err_size, "could not assign a quota project id");
		return -1;
	}
	if (set_disk_quota(dir, projid, quota_bytes) != 0) {
		snprintf(err, err_size,
		         "could not set the limit (%s) -- the filesystem holding this volume may not have "
		         "project quotas enabled",
		         strerror(errno));
		return -1;
	}
	/* Tagging after the limit is set, matching the container path's own
	 * ordering: the limit is already in force by the moment any file
	 * can carry this project id. */
	if (quota_bytes > 0 && overlay_tag_project_id(dir, projid, "volume") != 0) {
		snprintf(err, err_size, "could not tag the volume directory with its quota project id");
		return -1;
	}
	return 0;
}

/*
 * POST /v1/volumes/{name}/migrate -- move a volume's data to another
 * disk or partition, then repoint it.
 *
 * Refused while any container mounting this volume is RUNNING. A bind
 * mount resolves to a host path once, when the container starts
 * (ADR-0183), so moving the data underneath a live container would
 * leave it writing to the old location with nothing to indicate
 * anything had changed -- a silent split-brain rather than an error. A
 * stopped container is fine: it picks up the new location on its next
 * start, like any other definition change.
 */
static void handle_volume_migrate(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *target;
	char user[REGISTRY_NAME_MAX];
	enum volume_error verr;

	if (volume_find(name) == NULL) {
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	}
	root = json_parse(body, body_len);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	target = json_as_string(json_object_get(root, "disk"));
	if (target == NULL)
		target = ""; /* omitted means the default OS-disk placement */

	/*
	 * Only a genuinely RUNNING container blocks this. registry_find()
	 * alone is not the test -- since ADR-0181 the registry also holds
	 * exited containers, so using it would refuse a migrate because
	 * something that ran once and stopped still has an entry. An exited
	 * or stopped container picks up the new location on its next start,
	 * exactly like any other definition change; only a live process has
	 * a bind mount already resolved to the old path.
	 */
	{
		struct registry_entry *e = NULL;

		if (volume_referenced_by_container(name, user, sizeof(user)))
			e = registry_find(user);
		if (e != NULL && e->running) {
			json_free(root);
			respond_volume_error(fd, VOLUME_ERR_IN_USE_RUNNING);
			return;
		}
	}

	verr = volume_migrate(name, target);
	json_free(root);
	if (verr != VOLUME_OK) {
		respond_volume_error(fd, verr);
		return;
	}
	{
		/*
		 * Issue #93: a project-quota tag lives on the directory, so a
		 * volume that moved to another filesystem arrives untagged and
		 * unlimited. Re-applied here, or the migrate would silently
		 * drop a limit the operator still believes is in force -- which
		 * is worse than never having set one.
		 */
		struct volume *moved = volume_find(name);
		char path[PATH_MAX];
		char qerr[256];

		if (moved != NULL && moved->quota_bytes > 0 &&
		    volume_host_path(moved, path, sizeof(path)) == 0 &&
		    volume_apply_quota(name, path, moved->quota_bytes, qerr, sizeof(qerr)) != 0) {
			logstore_write("volume", "error",
			               "volume %s moved, but its size limit could not be re-applied at the new "
			               "location: %s",
			               name, qerr);
		}
	}
	handle_volume_get(fd, name);
}

/*
 * PUT /v1/volumes/{name}/owner (issue #102) -- who may write to this
 * volume.
 *
 * A volume is a directory the daemon creates as root, and nothing could
 * change that: a workload not running as root could not write to its
 * own volume. Found the plain way, by an operator logging into the jump
 * box and finding their home directory -- a volume -- owned by root.
 *
 * `recursive` is the caller's explicit choice and defaults to false. A
 * volume that has been in use holds files whose ownership someone may
 * have set deliberately, and rewriting all of it because the top-level
 * owner changed is a quiet kind of data loss.
 */
static void handle_volume_owner_put(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const struct json_value *ju, *jg, *jr;
	struct volume *v;
	enum volume_error verr;
	int uid, gid, recursive;
	struct json_writer w;

	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	v = volume_find(name);
	if (v == NULL) {
		json_free(root);
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	}
	ju = json_object_get(root, "uid");
	jg = json_object_get(root, "gid");
	jr = json_object_get(root, "recursive");
	recursive = (jr != NULL && jr->type == JSON_BOOL && jr->u.boolean);

	if (ju == NULL || jg == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "uid and gid are both required -- pass the value it already has to leave "
		              "one alone, or null for both to hand the volume back to root");
		return;
	}
	if (ju->type == JSON_NULL && jg->type == JSON_NULL) {
		uid = -1;
		gid = -1;
	} else {
		uid = (int)json_as_number(ju);
		gid = (int)json_as_number(jg);
		if (uid < 0 || gid < 0) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "uid/gid must not be negative");
			return;
		}
	}
	json_free(root);

	verr = volume_set_owner(name, uid, gid);
	if (verr != VOLUME_OK) {
		respond_volume_error(fd, verr);
		return;
	}
	v = volume_find(name);
	/* Clearing back to root is a real request too: chown it to 0:0
	 * rather than leaving whoever owned it last still owning it. */
	if (uid < 0) {
		char path[PATH_MAX];

		if (volume_host_path(v, path, sizeof(path)) == 0 && lchown(path, 0, 0) != 0) {
			respond_error(fd, 500, "Internal Server Error",
			              "could not hand the volume directory back to root");
			return;
		}
	} else {
		verr = volume_apply_owner(v, recursive);
		if (verr != VOLUME_OK) {
			respond_volume_error(fd, verr);
			return;
		}
	}

	jw_init(&w);
	volume_write_json_one(v, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * PUT /v1/volumes/{name}/quota -- set or clear a volume's size limit.
 */
static void handle_volume_quota_put(int fd, const char *name, const char *body, size_t body_len)
{
	struct volume *v = volume_find(name);
	struct json_value *root;
	const struct json_value *jq;
	long long bytes;
	char path[PATH_MAX];
	char err[256];
	enum volume_error verr;

	if (v == NULL) {
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	}
	root = json_parse(body, body_len);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jq = json_object_get(root, "quota_bytes");
	if (jq == NULL || jq->type != JSON_NUMBER || json_as_number(jq) < 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "quota_bytes must be a non-negative number (0 clears it)");
		return;
	}
	bytes = (long long)json_as_number(jq);
	json_free(root);

	if (volume_host_path(v, path, sizeof(path)) != 0) {
		respond_error(fd, 500, "Internal Server Error", "could not resolve the volume's own path");
		return;
	}
	if (volume_apply_quota(name, path, bytes, err, sizeof(err)) != 0) {
		respond_error(fd, 409, "Conflict", err);
		return;
	}
	verr = volume_set_quota(name, bytes);
	if (verr != VOLUME_OK) {
		respond_volume_error(fd, verr);
		return;
	}
	handle_volume_get(fd, name);
}

/* ---- Issue #96: volume content snapshots ---- */
/*
 * Issue #96: freeze (or thaw) every running container mounting this
 * volume. Returns how many were acted on, or -1 if any failed.
 *
 * ALL of them, not just one. Any container with the volume mounted
 * could be writing to it, so freezing a subset would leave the copy
 * exposed to the rest -- and a snapshot that is only mostly quiesced is
 * a crash-consistent one wearing a consistent one's label.
 *
 * The freeze is the cgroup freezer (ADR-0045), not SIGSTOP: a process
 * can neither ignore nor handle it, which is what makes the resulting
 * snapshot genuinely consistent rather than merely likely to be.
 *
 * On a partial failure the ones already frozen are thawed again before
 * returning, so a failure to quiesce never leaves containers stopped.
 */
static int volume_set_containers_paused(const char *volume_name, int freeze)
{
	char order[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	int count = containerdef_resolve_order(order);
	int i, acted = 0, failed = 0;

	for (i = 0; i < count; i++) {
		struct registry_entry *e;

		if (!container_mounts_volume(order[i], volume_name))
			continue;
		e = registry_find(order[i]);
		if (e == NULL || !e->running)
			continue;
		if (freeze && e->paused)
			continue; /* already frozen by someone else -- leave it alone */
		if (registry_set_paused(e, freeze) != 0) {
			failed = 1;
			break;
		}
		acted++;
	}
	if (failed && freeze) {
		/* Undo what this call managed before giving up. */
		for (i = 0; i < count; i++) {
			struct registry_entry *e = registry_find(order[i]);

			if (e != NULL && e->running && e->paused && container_mounts_volume(order[i], volume_name))
				registry_set_paused(e, 0);
		}
		return -1;
	}
	return acted;
}

static const struct volumebackup_hooks g_volumebackup_hooks = {
	volume_has_running_container,
	volume_set_containers_paused,
};



static void respond_volumebackup_error(int fd, enum volumebackup_error e)
{
	switch (e) {
	case VOLUMEBACKUP_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such volume");
		break;
	case VOLUMEBACKUP_ERR_NO_SUCH_SNAPSHOT:
		respond_error(fd, 404, "Not Found", "no such snapshot");
		break;
	case VOLUMEBACKUP_ERR_NO_DISK:
		respond_error(fd, 409, "Conflict",
		              "no backup disk configured -- set one via PUT /v1/system/volume-backup-config "
		              "(it must be a disk or partition carrying the 'backup' role)");
		break;
	case VOLUMEBACKUP_ERR_DISK_NOT_READY:
		respond_error(fd, 409, "Conflict",
		              "the configured backup disk is not mounted -- format and mount it first");
		break;
	case VOLUMEBACKUP_ERR_IN_USE_RUNNING:
		respond_error(fd, 409, "Conflict",
		              "a container mounting this volume is running -- copying its data now would "
		              "capture a half-written state, which looks exactly like a good snapshot "
		              "until someone restores it. Stop the container first");
		break;
	case VOLUMEBACKUP_ERR_COPY_FAILED:
		respond_error(fd, 500, "Internal Server Error", "copying the volume's data failed");
		break;
	case VOLUMEBACKUP_ERR_INVALID:
		respond_error(fd, 400, "Bad Request", "invalid request");
		break;
	default:
		respond_error(fd, 500, "Internal Server Error", "could not persist the backup config");
		break;
	}
}

static void handle_volume_backup_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	volumebackup_write_config_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_volume_backup_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const struct json_value *jen, *jiv;
	const char *disk;
	enum volumebackup_error verr;

	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	disk = json_as_string(json_object_get(root, "disk"));
	jen = json_object_get(root, "enabled");
	jiv = json_object_get(root, "interval_hours");
	verr = volumebackup_set(disk != NULL ? disk : "",
	                        jen != NULL && jen->type == JSON_BOOL && jen->u.boolean,
	                        (jiv != NULL && jiv->type == JSON_NUMBER) ? (int)json_as_number(jiv)
	                                                                  : volumebackup_interval_hours());
	json_free(root);
	if (verr != VOLUMEBACKUP_OK) {
		respond_volumebackup_error(fd, verr);
		return;
	}
	handle_volume_backup_config_get(fd);
}

/* The volume's own policy plus its snapshots and last attempt, in one
 * response -- they are always wanted together and splitting them across
 * three calls would be three round trips to render one panel. */
static void handle_volume_backups_get(int fd, const char *name)
{
	struct volume *v = volume_find(name);
	struct json_writer w;

	if (v == NULL) {
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	}
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "volume");
	jw_str(&w, name);
	jw_key(&w, "enabled");
	jw_bool(&w, v->backup_enabled);
	jw_key(&w, "retain");
	jw_int(&w, v->backup_retain > 0 ? v->backup_retain : VOLUMEBACKUP_DEFAULT_RETAIN);
	/* Whether this volume can be snapshotted while a container using it
	 * is running -- without it, an always-on service's volume is never
	 * backed up at all, which is the state most worth surfacing. */
	jw_key(&w, "while_running");
	jw_str(&w, volume_running_mode_name(v->backup_while_running));
	jw_key(&w, "last_backup_at");
	jw_int(&w, (long long)v->backup_last_at);
	jw_key(&w, "snapshots");
	volumebackup_write_list_json(&w, name);
	jw_key(&w, "status");
	volumebackup_write_status_json(&w, name);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_volume_backup_policy_put(int fd, const char *name, const char *body,
                                             size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const struct json_value *jen, *jre;
	int retain = 0;
	enum volume_error verr;

	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jen = json_object_get(root, "enabled");
	jre = json_object_get(root, "retain");
	if (jre != NULL) {
		if (jre->type != JSON_NUMBER || json_as_number(jre) < 1 ||
		    json_as_number(jre) > VOLUMEBACKUP_MAX_RETAIN) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "retain must be between 1 and 365");
			return;
		}
		retain = (int)json_as_number(jre);
	}
	{
		const char *jwr = json_as_string(json_object_get(root, "while_running"));
		struct volume *cur = volume_find(name);

		/* Omitted leaves the current mode alone -- a partial update
		 * that silently rewrites a field it was not given is how
		 * settings get lost. */
		verr = volume_set_backup_policy(
		    name, jen != NULL && jen->type == JSON_BOOL && jen->u.boolean, retain,
		    jwr != NULL ? volume_running_mode_parse(jwr)
		                : (cur != NULL ? cur->backup_while_running : VOLUME_RUNNING_REFUSE));
	}
	json_free(root);
	if (verr != VOLUME_OK) {
		respond_volume_error(fd, verr);
		return;
	}
	handle_volume_backups_get(fd, name);
}

static void handle_volume_backup_now(int fd, const char *name)
{
	enum volumebackup_error verr = volumebackup_take(name, &g_volumebackup_hooks);

	if (verr != VOLUMEBACKUP_OK) {
		respond_volumebackup_error(fd, verr);
		return;
	}
	handle_volume_backups_get(fd, name);
}

/*
 * Restoring replaces the volume's contents outright. Guarded like every
 * other destructive operation in this API: the caller has to name the
 * volume back, so it can never be something a stray click did.
 */
static void handle_volume_restore(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const char *stamp, *confirm;
	enum volumebackup_error verr;

	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	stamp = json_as_string(json_object_get(root, "snapshot"));
	confirm = json_as_string(json_object_get(root, "confirm_volume_name"));
	if (confirm == NULL || strcmp(confirm, name) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "confirm_volume_name must be given and must match the volume in the URL -- "
		              "restoring replaces everything currently in this volume");
		return;
	}
	if (stamp == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "snapshot is required");
		return;
	}
	verr = volumebackup_restore(name, stamp, &g_volumebackup_hooks);
	json_free(root);
	if (verr != VOLUMEBACKUP_OK) {
		respond_volumebackup_error(fd, verr);
		return;
	}
	handle_volume_backups_get(fd, name);
}

static void handle_volume_backup_delete(int fd, const char *name, const char *stamp)
{
	enum volumebackup_error verr = volumebackup_delete_snapshot(name, stamp);

	if (verr != VOLUMEBACKUP_OK) {
		respond_volumebackup_error(fd, verr);
		return;
	}
	handle_volume_backups_get(fd, name);
}

static void handle_volume_delete(int fd, const char *name)
{
	char user[REGISTRY_NAME_MAX];
	enum volume_error verr;

	if (volume_find(name) == NULL) {
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	}
	if (volume_referenced_by_container(name, user, sizeof(user))) {
		char msg[256];

		snprintf(msg, sizeof(msg),
		         "still referenced by container '%s' -- delete or edit that container first", user);
		respond_error(fd, 409, "Conflict", msg);
		return;
	}
	verr = volume_delete(name);
	if (verr != VOLUME_OK) {
		respond_volume_error(fd, verr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_serverhealth_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "servers");
	serverhealth_write_json_list(&w);
	jw_key(&w, "warnings");
	serverhealth_write_warnings(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * PUT /v1/system/server-health/{kind}/{container} -- the operator drain
 * override: {"drained": true|false}. Deliberately separate from the
 * probe's own verdict: draining is an intent ("I am doing maintenance
 * on this one"), which is why it is the one part of a health record that
 * is persisted across a daemon restart.
 */
static void handle_serverhealth_set(int fd, const char *kind, const char *container,
                                     const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const struct json_value *jdrained;
	struct json_writer w;

	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jdrained = json_object_get(root, "drained");
	if (jdrained == NULL || jdrained->type != JSON_BOOL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "drained (boolean) is required");
		return;
	}
	if (serverhealth_set_drained(kind, container, jdrained->u.boolean) != 0) {
		json_free(root);
		respond_error(fd, 500, "Internal Server Error", "could not record the drain state");
		return;
	}
	json_free(root);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "servers");
	serverhealth_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

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
		/* Not live -- might still be a kept-but-not-running container
		 * (ADR-0045, extended by ADR-0181's persist-all), same fallback
		 * GET /v1/containers' own list already makes via
		 * containerdef_write_json_inactive_list(). */
		jw_init(&w);
		if (containerdef_write_json_inactive_one(name, &w)) {
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
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
static void register_pkg_fetch_pidfd(pid_t pid, int pidfd, int chain_idx)
{
	struct conn *cc;
	struct thinc_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (pkg fetch reactor conn)");
		abort();
	}
	cc->kind = CONN_PKG_FETCH;
	cc->fd = pidfd;
	cc->pkg_fetch_pid = pid;
	cc->pkg_chain_idx = chain_idx;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
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
	int pkg_chain_idx;

	if (pkg_try_start_queued_rebuild(&pkg_pid, &pkg_pidfd, &pkg_chain_idx))
		register_pkg_fetch_pidfd(pkg_pid, pkg_pidfd, pkg_chain_idx);
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
static void register_pkg_build_output(int output_fd, int chain_idx)
{
	struct conn *cc;
	struct thinc_epoll_event ev;

	if (output_fd < 0)
		return;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (pkg build output reactor conn)");
		abort();
	}
	cc->kind = CONN_PKG_BUILD_OUTPUT;
	cc->fd = output_fd;
	cc->pkg_chain_idx = chain_idx;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
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
 * live-tail client watching THIS chain (ADR-0157 Phase 2 -- a client
 * watching a different concurrent build must never see another
 * build's output). A write failure just detaches that one client
 * (its own next epoll event, or this immediate teardown, reflects a
 * gone-away peer) -- never lets one broken viewer affect the others
 * or the build itself, which never blocks on this at all. */
static void build_log_ws_broadcast(int chain_idx, const void *data, size_t len)
{
	int i;

	for (i = 0; i < g_build_log_ws_conn_count;) {
		struct conn *cc = g_build_log_ws_conns[i];

		if (cc->pkg_chain_idx != chain_idx) {
			i++;
			continue;
		}
		if (ws_write_frame(cc->fd, WS_OPCODE_TEXT, data, len) != 0) {
			thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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

/* Called once THIS chain's build output pipe reaches EOF (that chain's
 * build finished, one way or another) -- tells every still-attached
 * live-tail client watching that same chain the stream is over via a
 * real WS close frame, then tears each one down. Without this a client
 * would just hang waiting for more output that will never come. Only
 * touches conns watching chain_idx -- a client attached to a different,
 * still-running concurrent build is left alone. */
static void build_log_ws_teardown_all(int chain_idx)
{
	int i;

	for (i = 0; i < g_build_log_ws_conn_count;) {
		struct conn *cc = g_build_log_ws_conns[i];

		if (cc->pkg_chain_idx != chain_idx) {
			i++;
			continue;
		}
		ws_write_frame(cc->fd, WS_OPCODE_CLOSE, NULL, 0);
		thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		ws_conn_free(&cc->ws);
		close(cc->fd);
		free(cc);
		g_build_log_ws_conns[i] = g_build_log_ws_conns[g_build_log_ws_conn_count - 1];
		g_build_log_ws_conn_count--;
	}
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

	eof = pkg_build_output_readable(cc->pkg_chain_idx, new_data, (int)sizeof(new_data), &new_len);
	if (new_len > 0 && g_build_log_ws_conn_count > 0)
		build_log_ws_broadcast(cc->pkg_chain_idx, new_data, (size_t)new_len);
	if (eof) {
		thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		pkg_build_output_close(cc->pkg_chain_idx);
		build_log_ws_teardown_all(cc->pkg_chain_idx);
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
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
		thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
 * a "thinc" hostbuild completing, itself serialized by pkg.c's own
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD bootroot output fd");
		abort();
	}
}

static void handle_bootroot_output_event(struct conn *cc)
{
	if (bootroot_output_readable()) {
		thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		bootroot_output_close();
		free(cc);
	}
}

/* Same shape as register_pkg_fetch_pidfd(), for the mkbootroot child
 * spawn_thinc_bootroot_assembly() below just forked -- tracks only
 * its exit; its captured stdout/stderr is a separate, directly-
 * registered conn (register_bootroot_output()/g_bootroot_output_rd
 * above, ADR-0087), not this one. */
static void register_bootroot_assemble_pidfd(pid_t pid, int pidfd)
{
	struct conn *cc;
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD bootroot assemble pidfd");
		abort();
	}
}

/*
 * ADR-0078: the well-known name of the shared "host tools" image --
 * cp/rm/sha256sum/gzip (coreutils.recipe/gzip.recipe) plus openssl/
 * curl/tar/bzip2/xz/squashfs-tools/e2fsprogs, one real `pkg install
 * --image=thinc-hosttools` per recipe -- an operator builds this
 * exactly like "thinc-builder"/"dev" (docs/guides/building-thinc.md),
 * no special-cased creation path. spawn_thinc_bootroot_assembly()
 * below passes its rootfs to mkbootroot.c's own host_tools_dir
 * argument when present, purely additive: a box that never built this
 * image keeps today's dev-host-sourced behavior (mkbootroot.c's own
 * "" fallback), never a hard failure.
 */
#define HOST_TOOLS_IMAGE "thinc-hosttools"

/*
 * ADR-0057: when a hostbuild job named "thinc" completes, assembles a
 * fresh control-plane squashfs from its own just-harvested artifacts
 * by forking+exec'ing the real, unmodified build/mkbootroot binary --
 * server-side, entirely within the daemon, never CLI-invoked (the
 * API-First Mandate rules out the CLI shelling out to a build tool
 * directly). Mirrors start_fetch_for()'s own "daemon already forks
 * subprocesses for curl" precedent -- this is the same class of
 * capability, not a new one. Uses THIS SAME hostbuild round's own
 * freshly built mkbootroot (thinc.recipe now stages one alongside
 * thincd/thincctl/web/) rather than assuming some earlier round's
 * copy is still present anywhere -- self-contained, no bootstrap-order
 * dependency. Failure here (missing mkbootroot, fork failure) is
 * logged, never fatal to the daemon -- the hostbuild itself already
 * succeeded and its own artifacts are still there for a later retry.
 */
static void spawn_thinc_bootroot_assembly(const char *artifact_dir)
{
	char mkbootroot_bin[PATH_MAX];
	char thincd_bin[PATH_MAX];
	char thincctl_bin[PATH_MAX];
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
	snprintf(thincd_bin, sizeof(thincd_bin), "%s/thincd", artifact_dir);
	snprintf(thincctl_bin, sizeof(thincctl_bin), "%s/thincctl", artifact_dir);
	snprintf(web_dir, sizeof(web_dir), "%s/web", artifact_dir);
	snprintf(out_squashfs, sizeof(out_squashfs), "%s/thincd-root.squashfs", artifact_dir);
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
	argv[2] = thincd_bin;
	argv[3] = thincctl_bin;
	argv[4] = web_dir;
	argv[5] = out_squashfs;
	argv[6] = ""; /* firmware dir -- a control-plane-only rebuild needs no GPU firmware re-staging */
	argv[7] = ""; /* modules dir -- a control-plane-only rebuild (thincd/thincctl/web only,
	               * ADR-0057) touches no kernel module tree at all */
	argv[8] = ""; /* kmod bin dir -- same reasoning */
	argv[9] = host_tools_dir; /* "" if thinc-hosttools was never built on this box */
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
		logstore_write("thincd", "error",
		                "thinc bootroot assembly: output capture pipe failed: %s",
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
		logstore_write("thincd", "error", "thinc bootroot assembly: fork failed: %s",
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
		logstore_write("thincd", "error", "thinc bootroot assembly: pidfd_open failed: %s",
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
 * fork+exec+pidfd-tracked, non-blocking shape spawn_thinc_bootroot_
 * assembly() above already established for mkbootroot. Unlike that
 * mechanism (auto-triggered the moment a "thinc" hostbuild completes),
 * ISO assembly is explicitly, separately triggered -- it has real
 * inputs of its own (the target's disk/ip/prefix/gateway/interface)
 * a hostbuild completion has no way to supply, and reuses whatever the
 * most recent "thinc"/"kernel"/"isotools" hostbuild rounds already
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD iso assemble pidfd");
		abort();
	}
}

/*
 * Validates every real precondition (the "thinc"/"kernel" hostbuild
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
	char thinc_install_bin[PATH_MAX];
	char thinc_recover_bin[PATH_MAX];
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
	    {mkinstalleriso_bin, "mkinstalleriso (from a \"thinc\" hostbuild)"},
	    {thinc_install_bin, "thinc-install (from a \"thinc\" hostbuild)"},
	    {thinc_recover_bin, "thinc-recover (from a \"thinc\" hostbuild, ADR-0146)"},
	    {bzimage_path, "bzImage (from a \"kernel\" hostbuild)"},
	    {squashfs_path, "thincd-root.squashfs (from a \"thinc\" hostbuild)"},
	    {isotools_root, "isotools artifact directory (from an \"isotools\" hostbuild)"},
	    {signing_key, "signing key (operator-provided at SIGNING_KEYS_DIR)"},
	    {signing_cert_pem, "signing cert .crt (operator-provided at SIGNING_KEYS_DIR)"},
	    {signing_cert_der, "signing cert .cer (operator-provided at SIGNING_KEYS_DIR)"},
	};
	size_t i;

	snprintf(mkinstalleriso_bin, sizeof(mkinstalleriso_bin), "%s/thinc/mkinstalleriso",
	         ARTIFACTS_DIR);
	snprintf(thinc_install_bin, sizeof(thinc_install_bin), "%s/thinc/thinc-install",
	         ARTIFACTS_DIR);
	snprintf(thinc_recover_bin, sizeof(thinc_recover_bin), "%s/thinc/thinc-recover",
	         ARTIFACTS_DIR);
	snprintf(bzimage_path, sizeof(bzimage_path), "%s/kernel/bzImage", ARTIFACTS_DIR);
	snprintf(squashfs_path, sizeof(squashfs_path), "%s/thinc/thincd-root.squashfs",
	         ARTIFACTS_DIR);
	snprintf(isotools_root, sizeof(isotools_root), "%s/isotools", ARTIFACTS_DIR);
	snprintf(signing_key, sizeof(signing_key), "%s/thinc-signing.key", SIGNING_KEYS_DIR);
	snprintf(signing_cert_pem, sizeof(signing_cert_pem), "%s/thinc-signing.crt", SIGNING_KEYS_DIR);
	snprintf(signing_cert_der, sizeof(signing_cert_der), "%s/thinc-signing.cer", SIGNING_KEYS_DIR);

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
	snprintf(ISO_OUTPUT_PATH, sizeof(ISO_OUTPUT_PATH), "%s/thinc-install.iso", ISO_DIR);

	argv[0] = mkinstalleriso_bin;
	argv[1] = stage_dir;
	argv[2] = thinc_install_bin;
	argv[3] = thinc_recover_bin;
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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
 * freshly-installed thinC box has no SSH server and no general shell
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
	struct thinc_epoll_event ev;
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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
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
	struct thinc_epoll_event ev;
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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
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

/*
 * GET /v1/whoami -- introspection only, never mutates a session
 * (hostauth_peek_token(), not hostauth_check_token() -- see that
 * function's own doc comment for why the distinction matters under a
 * single-use/idle_timeout_seconds==0 config). Exists specifically so a
 * client can answer "is my current bearer token actually still valid"
 * without write-gating's own GETs-are-always-open rule making that
 * otherwise undeterminable (every ordinary GET succeeds whether or not
 * a token is supplied, by design) -- thincctl's own interactive shell
 * prompt (ADR-0164) is the first real caller. No Authorization header
 * at all, or one naming an unknown/expired/absent-session token, both
 * report the same authenticated:false -- this endpoint doesn't
 * distinguish "never logged in" from "session lapsed," the same way
 * GET /v1/health doesn't distinguish flavors of "not ok."
 */
static void handle_whoami(int fd, const char *req_headers, size_t req_headers_len)
{
	char token_hdr[HOSTAUTH_TOKEN_LEN + 16];
	char username[HOSTAUTH_USERNAME_MAX];
	struct json_writer w;
	int authenticated = 0;

	if (http_find_header(req_headers, req_headers_len, "Authorization", token_hdr,
	                      sizeof(token_hdr)) >= 0) {
		const char *bearer = strncmp(token_hdr, "Bearer ", 7) == 0 ? token_hdr + 7 : token_hdr;

		authenticated = hostauth_peek_token(bearer, username, sizeof(username));
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "authenticated");
	jw_bool(&w, authenticated);
	jw_key(&w, "username");
	if (authenticated)
		jw_str(&w, username);
	else
		jw_null(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD storage migrate pidfd");
		abort();
	}
}

/* ADR-0142 Section 4: same shape as register_storage_migrate_pidfd()
 * above, keyed by container name instead of storage kind. */
static void register_container_storage_migrate_pidfd(const char *name, pid_t pid, int pidfd)
{
	struct conn *cc;
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
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
	serverhealth_repoint(SERVERHEALTH_STATE_PATH);
	volume_set_dir(VOLUMES_DIR);
	volume_repoint(VOLUMES_STATE_PATH);
	volumebackup_repoint(VOLUME_BACKUP_CONFIG_PATH);
	pki_repoint(PKI_DIR, PKI_CERTS_STATE_PATH);
	containerdef_repoint(CONTAINER_DEFS_STATE_PATH);
	containerdef_rolling_config_repoint(ROLLING_CONFIG_PATH);
	siteconfig_repoint(SITE_CONFIG_PATH);
	devicemap_repoint(DEVICEMAP_STATE_PATH);
	sysctlconfig_repoint(SYSCTLCONFIG_STATE_PATH);
	kmodconfig_repoint(KMODCONFIG_STATE_PATH);
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
	pkg_build_config_repoint(PKG_BUILD_CONFIG_PATH);

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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
 * POST /v1/containers' own "env" field's key validation -- POSIX
 * portable environment variable name rules (IEEE Std 1003.1-2017
 * 8.1): letters, digits, underscore, not starting with a digit. Real
 * enforcement, not cosmetic: a key containing '=' would make
 * daemon/src/registry.c's own spec->envp "KEY=VALUE" split (see
 * registry_create()) recover the wrong key/value boundary.
 */
static int env_key_is_safe(const char *key)
{
	const char *p = key;

	if (*p == '\0' || (*p >= '0' && *p <= '9'))
		return 0;
	for (; *p != '\0'; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '_'))
			return 0;
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

/*
 * Issue #68: the strict-input half of the limits-visibility fix (#49 is
 * the read-back half). The create parser reads exactly the keys below
 * and always simply ignored everything else -- so a typo'd field name
 * ("cpuset" for "cpuset_cpus", the live-confirmed case) produced a
 * container silently missing its requested limits, with (pre-#49) no
 * way to even see the absence. HTTP-facing entry points (create, and
 * container-recipe add/apply) reject unknown keys with a 400 naming
 * the offender; the daemon-internal replay paths (autostart, rolling
 * restart, storage migration -- all replaying bodies this daemon
 * itself persisted) deliberately stay lenient so an upgrade can never
 * strand an existing definition.
 */
static const char *container_body_unknown_key(const struct json_value *root)
{
	static const char *const known[] = {
		"name", "image", "image_version", "cmd", "networks", "ip_forward",
		"capture_output", "routes", "devices", "interfaces", "cap_add", "files",
		"sysctls", "env", "dns_servers", "dns_register", "pki_issue", "pki_cert_dir",
		"pki_days", "disk", "ldap_provision", "ldap_user", "ldap_group", "ldap_uid",
		"ldap_secret_dir", "restart", "restart_delay_seconds", "follow_rolling",
		"follow_rolling_jitter_seconds", "depends_on", "readiness", "memory_max", "memory_swap_max",
		"cpu_max", "pids_max", "cpuset_cpus", "disk_quota_bytes", "ldap_client",
		"ldap_allow_groups",
		"userns",
		/* Issue #88: persistent volumes. */
		"volumes",
	};
	size_t i, k;

	if (root == NULL || root->type != JSON_OBJECT)
		return NULL;
	for (i = 0; i < root->u.object.count; i++) {
		int found = 0;

		for (k = 0; k < sizeof(known) / sizeof(known[0]); k++) {
			if (strcmp(root->u.object.keys[i], known[k]) == 0) {
				found = 1;
				break;
			}
		}
		if (!found)
			return root->u.object.keys[i];
	}
	return NULL;
}

/*
 * Issue #66: stage one file directly into a container's upperdir, the
 * same open(O_CREAT|O_TRUNC)/write/optional-fchown sequence
 * create_container_from_body()'s own files[] loop already uses --
 * factored out so the ldap_client synthetic files below share exactly
 * that path (and its own already-audited safety: file_path_is_safe()
 * is enforced by the caller for user files; these synthetic paths are
 * fixed literals). Returns 0, or -1 with errno set. Records nothing in
 * file_paths[] itself -- the caller owns that array's indexing.
 */
/*
 * ADR-0179 phase 2c option (a): a userns container gets its OWN rootfs, a
 * copy of the image tree made here with `cp --reflink=auto -a` -- CoW (near
 * free) on a reflink-capable backing (btrfs/xfs), a real copy elsewhere. Its
 * own tree is what makes the container's rootfs directly writable+persistent
 * under an id-mapped mount, without the overlay-in-userns wall (ADR-0179).
 * Minimal fork/exec (no shell): argv is a fixed-arity vector, so no quoting/
 * injection surface. Returns 0 on a clean exit, -1 otherwise.
 */
static int run_cmd(const char *const argv[])
{
	pid_t p = fork();
	int status;

	if (p < 0)
		return -1;
	if (p == 0) {
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	if (waitpid(p, &status, 0) < 0)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/*
 * ADR-0179 phase 2c option (a): recursively chown a userns container's own
 * rootfs copy to its subordinate base id, so its mapped root (host <base>)
 * genuinely OWNS every file -- id-mapped mounts proved too subtle to make the
 * child a writable owner, whereas plain ownership is unambiguous: the
 * container's userns maps <base> straight back to uid 0. Done once at first
 * create (a copy the container then keeps). nftw + lchown, no external binary;
 * single-threaded event loop makes the static target ids safe.
 */
static uid_t g_chown_uid;
static gid_t g_chown_gid;

static int chown_tree_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftw)
{
	(void)sb;
	(void)typeflag;
	(void)ftw;
	if (lchown(path, g_chown_uid, g_chown_gid) != 0)
		return -1;
	return 0;
}

static int chown_tree(const char *path, uid_t uid, gid_t gid)
{
	g_chown_uid = uid;
	g_chown_gid = gid;
	return nftw(path, chown_tree_cb, 20, FTW_PHYS);
}

static int stage_container_file(const char *upperdir, const char *path, const char *content,
                                 mode_t mode, uid_t owner, gid_t group)
{
	char target[PATH_MAX];
	char target_dir[PATH_MAX];
	char *slash;
	size_t content_len = strlen(content);
	int fd;

	if (snprintf(target, sizeof(target), "%s%s", upperdir, path) >= (int)sizeof(target)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	snprintf(target_dir, sizeof(target_dir), "%s", target);
	slash = strrchr(target_dir, '/');
	if (slash != NULL)
		*slash = '\0';
	if (persist_mkdir_p(target_dir) != 0)
		return -1;
	fd = open(target, O_CREAT | O_TRUNC | O_WRONLY, mode);
	if (fd < 0)
		return -1;
	if (content_len > 0 && write(fd, content, content_len) != (ssize_t)content_len) {
		close(fd);
		return -1;
	}
	if ((owner != (uid_t)-1 || group != (gid_t)-1) && fchown(fd, owner, group) != 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/*
 * Issue #84: which registered LDAP server, if any, a single URI from an
 * explicitly-configured client_uri list refers to.
 *
 * Health is keyed by container name; client_uri is free-form URIs, so
 * the two only meet by resolving each registered server's live IP and
 * comparing. Returns the matching registered container name, or NULL
 * for a URI that maps to nothing thinC manages -- which is a real,
 * legitimate case (an operator may point at a directory this platform
 * knows nothing about) and must be left strictly alone rather than
 * filtered on evidence that does not exist.
 *
 * A port is only allowed to match when it is the port health actually
 * probes. Same IP on a different port is a different service, and
 * dropping it on the strength of a probe that never touched it would be
 * a guess dressed up as a health decision.
 */
static const char *ldap_uri_registered_server(const char *uri, char names[][LDAP_SERVER_NAME_MAX],
                                               int count)
{
	const char *authority, *p;
	char host[128];
	size_t hlen;
	int port = HOSTAUTH_LDAP_DEFAULT_PORT;
	int i;

	authority = strstr(uri, "://");
	authority = authority != NULL ? authority + 3 : uri;
	if (*authority == '[') /* IPv6 literal -- nothing here is IPv6, so never ours */
		return NULL;
	for (p = authority; *p != '\0' && *p != '/'; p++)
		;
	hlen = (size_t)(p - authority);
	if (hlen == 0 || hlen >= sizeof(host))
		return NULL;
	memcpy(host, authority, hlen);
	host[hlen] = '\0';
	{
		char *colon = strrchr(host, ':');

		if (colon != NULL) {
			*colon = '\0';
			port = atoi(colon + 1);
		}
	}
	if (port != HOSTAUTH_LDAP_DEFAULT_PORT)
		return NULL;

	for (i = 0; i < count; i++) {
		struct registry_entry *se = registry_find(names[i]);
		struct in_addr a;
		char ipbuf[INET_ADDRSTRLEN];

		if (se == NULL || se->net_count == 0 || se->nets[0].ip_be == 0)
			continue;
		a.s_addr = se->nets[0].ip_be;
		if (inet_ntop(AF_INET, &a, ipbuf, sizeof(ipbuf)) == NULL)
			continue;
		if (strcmp(ipbuf, host) == 0)
			return names[i];
	}
	return NULL;
}

/*
 * Issue #84: the same health filtering the derived list gets, applied to
 * an explicitly-configured client_uri. Everything #81 built was inert on
 * any box with client_uri set -- which included the real one -- because
 * the explicit list short-circuited before any filtering ran.
 *
 * Three rules, and the two conservative ones matter more than the
 * filtering itself: a URI that maps to no registered server is kept
 * untouched (it may be a directory thinC does not manage), and if
 * filtering would leave nothing, the original list stands. Handing a
 * client a server that might be down beats handing it none -- the
 * client retries; an empty list turns a partial outage into a total one.
 */
static void ldap_filter_configured_client_uri(const char *configured, char *out, size_t out_size)
{
	char names[LDAP_SERVER_MAX][LDAP_SERVER_NAME_MAX];
	char work[sizeof(((struct ldap_config *)0)->client_uri)];
	char *tok, *save;
	size_t off = 0;
	int count;

	snprintf(out, out_size, "%s", configured);
	count = ldap_server_list_containers(names, LDAP_SERVER_MAX);
	if (count == 0)
		return;

	snprintf(work, sizeof(work), "%s", configured);
	for (tok = strtok_r(work, " \t", &save); tok != NULL; tok = strtok_r(NULL, " \t", &save)) {
		const char *server = ldap_uri_registered_server(tok, names, count);
		int written;

		if (server != NULL && !serverhealth_in_service("ldap", server))
			continue;
		written = snprintf(out + off, out_size - off, "%s%s", off > 0 ? " " : "", tok);
		if (written > 0 && (size_t)written < out_size - off)
			off += (size_t)written;
	}
	if (off == 0)
		snprintf(out, out_size, "%s", configured);
}

/*
 * Issue #66 (auto-derivation): the effective LDAP client URI list for
 * ldap_client -- the explicitly-configured client_uri if set, else one
 * built from the IPs of the containers an operator has already
 * registered as LDAP servers (ldap server register), so registering
 * the servers is the only step needed. Writes into out (returns 1) or
 * returns 0 if neither source yields anything. A registered server
 * whose container isn't currently running (no resolvable primary IP)
 * is skipped -- a best-effort list of the live ones, same posture DNS
 * server delivery already takes.
 */
/*
 * The LDAP URI list handed to client containers. Built from the live IPs
 * of registered LDAP servers when no explicit client_uri is configured.
 *
 * Issue #81: servers that are drained or confirmed unhealthy are dropped
 * -- the direct fix for #80's shape, where a registered-but-not-serving
 * pair silently broke every login. Two deliberate safety rules keep that
 * filtering from ever becoming its own outage:
 *   - a never-yet-probed server counts as in service, so turning health
 *     tracking on can't black-hole a working deployment during the very
 *     first sweep;
 *   - if filtering would leave NOTHING, the unfiltered list is used
 *     instead. Handing a client a server that might be down is strictly
 *     better than handing it nothing at all -- the client retries, and
 *     an empty URI list would turn a partial outage into a total one.
 */
static int ldap_effective_client_uri(char *out, size_t out_size)
{
	const struct ldap_config *lc = ldap_config_get();
	char names[LDAP_SERVER_MAX][LDAP_SERVER_NAME_MAX];
	int count, i, pass;
	size_t off = 0;

	if (lc->client_uri[0] != '\0') {
		/* Issue #84: filtered, not passed through -- an explicit list
		 * used to skip every health rule below it. */
		ldap_filter_configured_client_uri(lc->client_uri, out, out_size);
		return 1;
	}
	count = ldap_server_list_containers(names, LDAP_SERVER_MAX);

	/* pass 0: in-service servers only. pass 1 (only if that produced an
	 * empty list): every reachable server, health ignored. */
	for (pass = 0; pass < 2 && off == 0; pass++) {
		out[0] = '\0';
		for (i = 0; i < count; i++) {
			struct registry_entry *se = registry_find(names[i]);
			struct in_addr a;
			char ipbuf[INET_ADDRSTRLEN];
			int written;

			if (se == NULL || se->net_count == 0 || se->nets[0].ip_be == 0)
				continue;
			if (pass == 0 && !serverhealth_in_service("ldap", names[i]))
				continue;
			a.s_addr = se->nets[0].ip_be;
			if (inet_ntop(AF_INET, &a, ipbuf, sizeof(ipbuf)) == NULL)
				continue;
			written = snprintf(out + off, out_size - off, "%sldap://%s:%d/",
			                    off > 0 ? " " : "", ipbuf, HOSTAUTH_LDAP_DEFAULT_PORT);
			if (written > 0 && (size_t)written < out_size - off)
				off += (size_t)written;
		}
	}
	return off > 0;
}


/*
 * Issue #86: the workload cgroup every container and every build lives
 * under, carrying "the machine minus the control plane's reservation".
 *
 * Priority (nice -20, oom_score_adj -1000) is the cheap 80% of not
 * losing the box; this is the structurally sound part. Rather than
 * asking the scheduler to favour the daemon, everything that is not the
 * daemon is bounded, so what the control plane has left is a
 * kernel-enforced remainder rather than a hope.
 *
 * Derived from LIVE host totals on every apply, never stored: the same
 * "keep a tenth of the box" config has to keep meaning that when the
 * box is replaced by a bigger one, which is about to happen -- this
 * project's next install is real hardware, not the 2-CPU VM every
 * number here was first chosen against.
 *
 * Best-effort, deliberately: a sandbox where cgroup writes are not
 * permitted must still run containers. A safety margin that refuses to
 * start the system it protects is not one.
 */
#define CGROUP_WORKLOAD_PARENT "thinc-workload"

static void workload_parent_ensure(void)
{
	const struct cpreserve_config *c = cpreserve_get();
	struct cgroup_limits lim;
	char cpu_max[64];
	long long mem_total, mem_free, mem_avail, mem_buffers, mem_cached, swap_total, swap_free;
	long cpus;

	memset(&lim, 0, sizeof(lim));
	lim.name = CGROUP_WORKLOAD_PARENT;
	/* -1 = leave memory.swap.max alone; 0 would forbid swapping
	 * entirely, which is not what "no swap limit configured" means.
	 * See struct cgroup_limits. */
	lim.memory_swap_max = -1;

	/*
	 * Disabled means an UNLIMITED parent, not a different shape: every
	 * container and build stays exactly where it is in the hierarchy
	 * either way, and only the ceiling appears or disappears. Moving
	 * the topology with the setting would make turning the reservation
	 * off a migration, and would leave existing containers accounted
	 * somewhere their successors are not.
	 */
	cpus = c->enabled ? sysconf(_SC_NPROCESSORS_ONLN) : 0;
	if (cpus > 0) {
		/* cgroup v2 cpu.max is "<quota> <period>" in microseconds, and
		 * 100000 is the conventional period, so one whole CPU is
		 * 100000. Truncation lands in the safe direction (marginally
		 * more reserved than asked), and the floor stops a very small
		 * box ending up with a workload ceiling of nothing. */
		long long quota = (long long)cpus * 100000LL * (100 - c->cpu_percent) / 100;

		if (quota < 10000)
			quota = 10000;
		snprintf(cpu_max, sizeof(cpu_max), "%lld 100000", quota);
		lim.cpu_max = cpu_max;
	}

	if (c->enabled) {
		read_meminfo(&mem_total, &mem_free, &mem_avail, &mem_buffers, &mem_cached, &swap_total,
		             &swap_free);
		/* Only when the reservation still leaves workloads the clear
		 * majority of RAM. On a box too small for that, a memory
		 * ceiling would do more harm than the starvation it guards
		 * against. */
		if (mem_total > c->memory_bytes * 2)
			lim.memory_max = mem_total - c->memory_bytes;
	}

	cgroup_create_parent(&lim);
}

/* Where a container's own cgroup leaf goes -- always under the workload
 * parent, whose ceiling is present or absent according to the
 * reservation. See workload_parent_ensure() for why the shape does not
 * move with the setting. */
static void workload_cgroup_path(const char *name, char *out, size_t out_size)
{
	workload_parent_ensure();
	snprintf(out, out_size, "%s/%s", CGROUP_WORKLOAD_PARENT, name);
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
	const struct json_value *jname, *jimage, *jimage_version, *jcmd, *jmem, *jswapmax, *jpids, *jcpu, *jcpuset,
	    *jnetworks, *jip_forward, *jroutes;
	const struct json_value *jdisk_quota;
	long long disk_quota_bytes;
	const struct json_value *jdevices;
	const struct json_value *jinterfaces;
	const struct json_value *jcap_add;
	const struct json_value *jfiles, *jsysctls, *jenv;
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
	char container_cgroup_path[PATH_MAX]; /* issue #86 -- under the workload parent */
	char userns_rootfs[PATH_MAX]; /* ADR-0179 phase 2c option (a) */
	const char *stage_dir;        /* where container files are staged: upper, or the userns rootfs */
	struct stat st;
	struct container_spec spec;
	struct registry_entry *entry;
	enum registry_error rerr;
	int create_errno;
	char *argv_buf[CONTAINER_MAX_ARGV];
	char env_buf[CONTAINER_MAX_ENV][CONTAINER_ENV_ENTRY_MAX];
	char *envp_ptrs[CONTAINER_MAX_ENV + 1];
	int env_count = 0;
	size_t argc, i;
	struct registry_network_attachment net_attachments[CONTAINER_MAX_NETWORKS];
	int net_count = 0;
	int ip_forward = 0;
	const struct json_value *jcapture_output;
	int capture_output = 0;
	const struct json_value *juserns; /* ADR-0179 #29 phase 2 opt-in */
	int userns = 0;
	int ldap_client = 0; /* issue #66 */
	/*
	 * Issue #76: which LDAP groups may log in here. Empty means no
	 * restriction -- any valid account in the directory, which is what
	 * an ldap_client container has always done. Naming groups turns
	 * that into "these groups only", enforced by nslcd itself rather
	 * than by anything this daemon has to be running to check.
	 */
	const struct json_value *jallow_groups = NULL;
	int output_pipe[2] = { -1, -1 };
	int stdio_write_fd = -1;
	struct route_spec route_specs[CONTAINER_MAX_ROUTES];
	int route_count = 0;
	struct registry_device_attachment device_attachments[CONTAINER_MAX_DEVICES];
	struct device_spec device_specs[CONTAINER_MAX_DEVICES];
	int device_count = 0;
	/* ADR-0161 Phase B: raw id/devicemap-name references from an
	 * "optional": true devices[] entry that didn't resolve at creation
	 * time -- handed to registry_set_pending_devices() once entry is
	 * known valid, below. */
	char pending_device_refs[CONTAINER_MAX_DEVICES][96];
	int pending_device_count = 0;
	char interface_names[CONTAINER_MAX_INTERFACES][CONTAINER_IFNAME_MAX];
	int interface_count = 0;
	char cap_add_names[CONTAINER_MAX_CAP_ADD][CONTAINER_CAP_NAME_MAX];
	int cap_add_count = 0;
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
	juserns = json_object_get(root, "userns");
	jroutes = json_object_get(root, "routes");
	jdevices = json_object_get(root, "devices");
	jinterfaces = json_object_get(root, "interfaces");
	jcap_add = json_object_get(root, "cap_add");
	jfiles = json_object_get(root, "files");
	jsysctls = json_object_get(root, "sysctls");
	jenv = json_object_get(root, "env");
	jdns_servers = json_object_get(root, "dns_servers");
	jdns_register = json_object_get(root, "dns_register");
	{
		const struct json_value *jll = json_object_get(root, "ldap_client");

		ldap_client = (jll != NULL && jll->type == JSON_BOOL && jll->u.boolean);
	}
	jallow_groups = json_object_get(root, "ldap_allow_groups");
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
	userns = (juserns != NULL && juserns->type == JSON_BOOL && juserns->u.boolean);
	dns_register = (jdns_register != NULL && jdns_register->type == JSON_BOOL &&
	                jdns_register->u.boolean);
	pki_issue = (jpki_issue != NULL && jpki_issue->type == JSON_BOOL && jpki_issue->u.boolean);
	snprintf(pki_cert_dir_buf, sizeof(pki_cert_dir_buf), "%s",
	         json_as_string(jpki_cert_dir) != NULL ? json_as_string(jpki_cert_dir) :
	                                                  "/etc/thinc-tls");
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
	                                                     "/etc/thinc-ldap");

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
	 * restart_delay_seconds already established just above. Since ADR-0181
	 * every container -- restart:"no" included -- has a persisted
	 * definition, so follow_rolling IS stored for it; it is simply never
	 * acted on, because apply_rolling_container_restarts() deliberately
	 * skips restart:"no" defs ("never auto-restart" includes "never
	 * auto-recreated by a rolling update"). Parsed-and-stored-but-inert,
	 * not rejected.
	 */
	jfollow_rolling = json_object_get(root, "follow_rolling");
	*out_follow_rolling =
	    (jfollow_rolling != NULL && jfollow_rolling->type == JSON_BOOL && jfollow_rolling->u.boolean);

	/*
	 * Part 5 follow-up: an optional per-container override of the
	 * daemon-wide rolling-restart jitter window -- same "ignored, not
	 * an error" precedent as follow_rolling itself just above when
	 * this container never actually follows rolling updates, or when
	 * restart is "no" (ADR-0181: persisted like any other, but its
	 * rolling-follow is inert -- see the follow_rolling note above).
	 * Range-validated the same 0-CONTAINERDEF_JITTER_MAX_
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
	/*
	 * Issue #76: an allow-list only means anything to nslcd, so naming
	 * one without ldap_client is a request that cannot do what it says
	 * -- refused rather than silently ignored, which would read as a
	 * restriction that is in force when it is not.
	 */
	if (jallow_groups != NULL && !ldap_client) {
		json_free(root);
		snprintf(err_msg, err_msg_size,
		         "ldap_allow_groups requires ldap_client -- there is nothing to restrict without it");
		return 400;
	}
	if (jallow_groups != NULL && jallow_groups->type != JSON_ARRAY) {
		json_free(root);
		snprintf(err_msg, err_msg_size, "ldap_allow_groups must be an array of group names");
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
			const struct json_value *item = jdevices->u.array.items[i];
			const char *id;
			int optional = 0;
			const struct discovered_device *matches[CONTAINER_MAX_DEVICES];
			int n, j;

			/*
			 * ADR-0161 Phase B: a bare string (every pre-existing
			 * caller's exact shape, zero opt-in required) or
			 * {"id": "...", "optional": true} -- "id" shorthand,
			 * "optional" defaults false, so an object entry with
			 * "optional" omitted behaves identically to the bare-
			 * string form too.
			 */
			if (item->type == JSON_STRING) {
				id = json_as_string(item);
			} else if (item->type == JSON_OBJECT) {
				const struct json_value *joptional = json_object_get(item, "optional");

				id = json_as_string(json_object_get(item, "id"));
				if (joptional != NULL) {
					if (joptional->type != JSON_BOOL) {
						json_free(root);
						snprintf(err_msg, err_msg_size, "devices[].optional must be a boolean");
						return 400;
					}
					optional = joptional->u.boolean;
				}
			} else {
				id = NULL;
			}

			if (id == NULL) {
				json_free(root);
				snprintf(err_msg, err_msg_size,
				         "devices entries must be a string, or {\"id\":..., \"optional\":...}");
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
			 * unplugged) is a real error here, UNLESS optional is true
			 * (ADR-0161 Phase B): the container is then still created,
			 * without this grant, and id is remembered in pending_
			 * device_refs[] for Phase C/D's later hotplug matching.
			 */
			n = devicemap_resolve(id, CONTAINERS_DIR, matches,
			                       CONTAINER_MAX_DEVICES - device_count);
			if (n < 0)
				n = device_find_group(id, CONTAINERS_DIR, matches,
				                       CONTAINER_MAX_DEVICES - device_count);
			if (n <= 0) {
				if (optional) {
					if (pending_device_count < CONTAINER_MAX_DEVICES) {
						snprintf(pending_device_refs[pending_device_count],
						         sizeof(pending_device_refs[0]), "%s", id);
						pending_device_count++;
					}
					continue;
				}
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
				device_attachments[device_count].type = dd->type;
				device_attachments[device_count].major = dd->major;
				device_attachments[device_count].minor = dd->minor;
				/* .live stays 0 (memset above) -- a create-time grant,
				 * never independently detachable (ADR-0161 Phase D). */
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
	if (jcap_add != NULL) {
		if (jcap_add->type != JSON_ARRAY || jcap_add->u.array.count > CONTAINER_MAX_CAP_ADD) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "cap_add must be an array of at most 8 entries");
			return 400;
		}
		cap_add_count = (int)jcap_add->u.array.count;
		for (i = 0; i < (size_t)cap_add_count; i++) {
			const char *cap_name = json_as_string(jcap_add->u.array.items[i]);

			if (cap_name == NULL || !container_cap_name_valid(cap_name)) {
				json_free(root);
				snprintf(err_msg, err_msg_size,
				         "cap_add entries must be a recognized capability name "
				         "that's actually on the default deny-list (e.g. \"CAP_SYS_TIME\")");
				return 400;
			}
			snprintf(cap_add_names[i], sizeof(cap_add_names[i]), "%s", cap_name);
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
	 * Issue #66: ldap_client stages the two identity-resolution files
	 * that would otherwise hand-carry the daemon's own LDAP client
	 * config -- /etc/nsswitch.conf (passwd/group/shadow: files ldap)
	 * and /etc/nslcd.conf (uri/base/binddn/bindpw from
	 * ldap_config_get()). The container still owns its own service
	 * wiring (PAM, sshd, the startup script that actually runs nslcd)
	 * and its own baseline /etc/passwd -- this flag is only about the
	 * client config values that are properly the daemon's, not the
	 * container's, to know. A container needing a *custom* nslcd.conf
	 * uses the {{LDAP:*}} recipe tokens instead and leaves this unset;
	 * both paths coexist. Refused with a clear error when the daemon
	 * has no client URI/base configured (there would be nothing useful
	 * to render), and budget-checked against CONTAINER_MAX_FILES up
	 * front so a create can't half-stage and then 500.
	 */
	if (ldap_client) {
		const struct ldap_config *lc = ldap_config_get();
		int user_files = (jfiles != NULL) ? (int)jfiles->u.array.count : 0;
		char probe_uri[600];

		if (lc->base_dn[0] == '\0' || !ldap_effective_client_uri(probe_uri, sizeof(probe_uri))) {
			json_free(root);
			snprintf(err_msg, err_msg_size,
			         "ldap_client requires base_dn (PUT /ldap/config) and either client_uri "
			         "or at least one running registered LDAP server");
			return 400;
		}
		if (user_files + 2 > CONTAINER_MAX_FILES) {
			json_free(root);
			snprintf(err_msg, err_msg_size,
			         "ldap_client needs 2 file slots; too many files[] entries already");
			return 400;
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
	if (jenv != NULL) {
		if (jenv->type != JSON_OBJECT || jenv->u.object.count > CONTAINER_MAX_ENV) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "env must be an object of at most 32 entries");
			return 400;
		}
		env_count = (int)jenv->u.object.count;
		for (i = 0; i < (size_t)env_count; i++) {
			const char *key = jenv->u.object.keys[i];
			const char *value = json_as_string(jenv->u.object.values[i]);

			if (value == NULL || strlen(key) >= CONTAINER_ENV_KEY_MAX ||
			    strlen(value) >= CONTAINER_ENV_VALUE_MAX || !env_key_is_safe(key)) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "invalid env entry");
				return 400;
			}
			snprintf(env_buf[i], sizeof(env_buf[i]), "%s=%s", key, value);
			envp_ptrs[i] = env_buf[i];
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
	envp_ptrs[env_count] = NULL;

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
			memset(&net_attachments[i], 0, sizeof(net_attachments[i]));
			strncpy(net_attachments[i].name, n, sizeof(net_attachments[i].name) - 1);
			net_attachments[i].ip_be = ip_be;
			snprintf(net_attachments[i].ifname, sizeof(net_attachments[i].ifname), "eth%d", (int)i);
			/* veth_host stays empty -- only a live attachment (task #861)
			 * ever needs a standalone detach path; see its own comment
			 * in registry.h. */
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
	userns_rootfs[0] = '\0';
	stage_dir = upperdir;

	/*
	 * ADR-0179 phase 2c option (a): a userns container does not use an
	 * overlay -- it gets its own per-container rootfs (a CoW copy of the
	 * image), which container_create() id-maps and pivots into directly.
	 * Create that copy here (host-side), the merged mountpoint the child
	 * pivots onto, and stage the container's own files straight into the
	 * rootfs instead of an overlay upperdir. The copy is made only on first
	 * create (reused on restart, so writes persist).
	 */
	if (userns) {
		struct stat rst;

		snprintf(userns_rootfs, sizeof(userns_rootfs), "%s/rootfs", container_base);
		if (stat(userns_rootfs, &rst) != 0) {
			const char *cp_argv[] = { "/usr/bin/cp", "--reflink=auto", "-a",
			                          lowerdir, userns_rootfs, NULL };
			long long base = 0;

			if (run_cmd(cp_argv) != 0) {
				json_free(root);
				snprintf(err_msg, err_msg_size,
				         "failed to copy image rootfs for userns container");
				return 500;
			}
			/* chown the whole copy to the container's subordinate base id so
			 * its mapped root owns it (subid_lookup_or_assign is idempotent --
			 * the spec block below re-derives the same base). */
			if (subid_lookup_or_assign(name, &base) != 0 ||
			    chown_tree(userns_rootfs, (uid_t)base, (gid_t)base) != 0) {
				json_free(root);
				snprintf(err_msg, err_msg_size,
				         "failed to chown userns rootfs to its subordinate id");
				return 500;
			}
		}
		if (persist_mkdir_p(merged) != 0) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "failed to create userns pivot mountpoint");
			return 500;
		}
		stage_dir = userns_rootfs;
	}

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

			if (snprintf(target, sizeof(target), "%s%s", stage_dir, path) >=
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
	if (ldap_client) {
		/* Rendered from the daemon's own LDAP client config (validated
		 * present above). nslcd.conf holds the bind credential -> 0600.
		 * nsswitch.conf is the standard "files then ldap" resolution
		 * order. `pam_authc_ppolicy no` matches this project's own
		 * glauth deployment (jump's own working recipe), which doesn't
		 * implement the ppolicy control nslcd probes for by default. */
		const struct ldap_config *lc;
		char nslcd[1024];
		char effective_uri[600];

		/*
		 * Issue #80: make sure the nslcd service/bind account this
		 * container is about to be handed actually exists, before we
		 * render its bind_dn/bind_password. Idempotent -- a no-op once
		 * provisioned, or if the operator configured a custom bind. This
		 * is the right moment: a client container is being created, so
		 * the registered glauth servers are up to receive the account.
		 */
		ldap_ensure_service_bind_account();
		lc = ldap_config_get();

		ldap_effective_client_uri(effective_uri, sizeof(effective_uri));

		if (stage_container_file(stage_dir, "/etc/nsswitch.conf",
		                          "passwd:         files ldap\n"
		                          "group:          files ldap\n"
		                          "shadow:         files ldap\n",
		                          0644, (uid_t)-1, (gid_t)-1) != 0) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "failed to stage ldap_client nsswitch.conf");
			return 500;
		}
		snprintf(file_paths[file_count], sizeof(file_paths[file_count]), "%s",
		         "/etc/nsswitch.conf");
		file_count++;

		{
			/*
			 * Issue #76: host-scoped authorisation.
			 *
			 * Without ldap_allow_groups this file carries no
			 * pam_authz_search at all and any valid account in the
			 * directory may log in -- what an ldap_client container has
			 * always done. With it, nslcd runs the search below after
			 * authenticating and refuses the login if it matches
			 * nothing, so "which users" is enforced by the same daemon
			 * that resolves them rather than by anything of ours that
			 * has to still be running.
			 *
			 * Group membership is the mechanism, and it is what this
			 * directory actually supports: verified live against the
			 * deployed glauth that a user entry carries memberOf DNs of
			 * the form "ou=<group>,ou=groups,<base>" for BOTH its
			 * primary and its secondary groups, that filtering on them
			 * matches a member and returns nothing for a non-member.
			 * (The `host` attribute this issue originally suggested
			 * would need custom-attribute support this glauth's config
			 * backend does not expose; groups are a first-class concept
			 * here already, and a user can be in many, which is exactly
			 * what "may log into several hosts" needs.)
			 */
			char authz[512] = "";

			if (jallow_groups != NULL && jallow_groups->type == JSON_ARRAY &&
			    jallow_groups->u.array.count > 0) {
				size_t gi;
				size_t off = 0;

				off += (size_t)snprintf(authz + off, sizeof(authz) - off,
				                         "pam_authz_search "
				                         "(&(objectClass=posixAccount)(uid=$username)(|");
				for (gi = 0; gi < jallow_groups->u.array.count; gi++) {
					const char *g = json_as_string(jallow_groups->u.array.items[gi]);

					/*
					 * Refused, never escaped: this string is written
					 * into a file nslcd parses as an LDAP filter, and a
					 * group name is already constrained to a safe set
					 * elsewhere in this daemon -- anything outside it is
					 * a mistake worth reporting, not something to encode
					 * around.
					 */
					if (g == NULL || !simple_name_is_valid(g, LDAP_GROUP_NAME_MAX)) {
						json_free(root);
						snprintf(err_msg, err_msg_size,
						         "ldap_allow_groups entries must be plain group names");
						return 400;
					}
					if (ldap_group_find(g) == NULL) {
						/*
						 * The message is built BEFORE the tree is freed:
						 * g points into it.
						 */
						snprintf(err_msg, err_msg_size,
						         "ldap_allow_groups names group \"%s\", which does not exist -- "
						         "create it first, so a typo cannot silently lock everyone out",
						         g);
						json_free(root);
						return 400;
					}
					off += (size_t)snprintf(authz + off, sizeof(authz) - off,
					                         "(memberOf=ou=%s,ou=groups,%s)", g, lc->base_dn);
					if (off >= sizeof(authz)) {
						json_free(root);
						snprintf(err_msg, err_msg_size, "too many ldap_allow_groups entries");
						return 400;
					}
				}
				snprintf(authz + off, sizeof(authz) - off, "))\n");
			}

			snprintf(nslcd, sizeof(nslcd),
			         "uri %s\nbase %s\nbinddn %s\nbindpw %s\npam_authc_ppolicy no\n%s",
			         effective_uri, lc->base_dn, lc->bind_dn, lc->bind_password, authz);
		}
		if (stage_container_file(stage_dir, "/etc/nslcd.conf", nslcd, 0600, (uid_t)-1,
		                          (gid_t)-1) != 0) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "failed to stage ldap_client nslcd.conf");
			return 500;
		}
		snprintf(file_paths[file_count], sizeof(file_paths[file_count]), "%s", "/etc/nslcd.conf");
		file_count++;
	}
	/*
	 * ADR-0197: a container that is already a registered DNS server is
	 * also a DHCP server, so it gets its own rendered DHCP files staged
	 * HERE, before its process starts.
	 *
	 * That timing is the whole point, and it was found the hard way on
	 * the real box: dnsmasq reads its conf file only at startup, and a
	 * restart re-stages the container's own creation-time files[] --
	 * so a conf written into the running container was overwritten by
	 * the empty placeholder on the very next restart, and the range
	 * silently never took effect. Staging it as part of creation means
	 * the file dnsmasq opens is the rendered one, every time.
	 *
	 * Staged after the caller's own files[] deliberately, so the
	 * rendered content wins over the placeholder an operator staged to
	 * let dnsmasq start the first time.
	 */
	if (dhcp_server_is_registered(name)) {
		char conf[4096];
		char hosts[8192];

		if (dhcp_render_conf(name, conf, sizeof(conf)) >= 0 &&
		    dhcp_render_hosts(hosts, sizeof(hosts)) >= 0) {
			if (stage_container_file(stage_dir, DHCP_CONF_PATH, conf, 0644, (uid_t)-1,
			                          (gid_t)-1) == 0 &&
			    file_count < CONTAINER_MAX_FILES) {
				snprintf(file_paths[file_count], sizeof(file_paths[file_count]), "%s",
				         DHCP_CONF_PATH);
				file_count++;
			}
			if (stage_container_file(stage_dir, DHCP_HOSTS_PATH, hosts, 0644, (uid_t)-1,
			                          (gid_t)-1) == 0 &&
			    file_count < CONTAINER_MAX_FILES) {
				snprintf(file_paths[file_count], sizeof(file_paths[file_count]), "%s",
				         DHCP_HOSTS_PATH);
				file_count++;
			}
		}
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

		if (snprintf(target, sizeof(target), "%s/etc/resolv.conf", stage_dir) >= (int)sizeof(target)) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "dns_servers path too long");
			return 500;
		}
		snprintf(target_dir, sizeof(target_dir), "%s/etc", stage_dir);
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
	/*
	 * Issue #88: persistent volumes. Resolved here, at create time, from
	 * names to real host paths -- the container runtime is handed paths,
	 * never names, so it never needs to know the volume registry exists.
	 * A named volume that does not exist is a hard 400 rather than an
	 * implicit create: a typo silently producing a brand-new empty
	 * volume is precisely how someone loses data and then concludes
	 * persistence "didn't work".
	 *
	 * Placed deliberately AFTER spec's own memset below -- populating
	 * it any earlier is silently undone, which is exactly what happened
	 * on the first attempt (the container came up with no /home at all
	 * and no error, because volume_count had been zeroed back to 0).
	 */
	{
		const struct json_value *jvolumes = json_object_get(root, "volumes");

		if (jvolumes != NULL && jvolumes->type == JSON_ARRAY) {
			size_t vi;

			if (jvolumes->u.array.count > CONTAINER_MAX_VOLUMES) {
				json_free(root);
				snprintf(err_msg, err_msg_size, "too many volumes (max %d)",
				         CONTAINER_MAX_VOLUMES);
				return 400;
			}
			for (vi = 0; vi < jvolumes->u.array.count; vi++) {
				const struct json_value *item = jvolumes->u.array.items[vi];
				const char *vname = json_as_string(json_object_get(item, "name"));
				const char *vpath = json_as_string(json_object_get(item, "path"));
				const struct json_value *jro = json_object_get(item, "read_only");
				struct volume *vol;
				char hostpath[PATH_MAX];

				if (vname == NULL || vpath == NULL || vpath[0] != '/' ||
				    !file_path_is_safe(vpath)) {
					json_free(root);
					snprintf(err_msg, err_msg_size,
					         "each volume needs a name and an absolute path with no '..'");
					return 400;
				}
				vol = volume_find(vname);
				if (vol == NULL) {
					json_free(root);
					snprintf(err_msg, err_msg_size, "no such volume: %s", vname);
					return 400;
				}
				if (volume_host_path(vol, hostpath, sizeof(hostpath)) != 0) {
					json_free(root);
					snprintf(err_msg, err_msg_size, "could not resolve volume %s", vname);
					return 500;
				}
				snprintf(spec.volumes[spec.volume_count].host_path,
				         sizeof(spec.volumes[spec.volume_count].host_path), "%s", hostpath);
				snprintf(spec.volumes[spec.volume_count].mount_path,
				         sizeof(spec.volumes[spec.volume_count].mount_path), "%s", vpath);
				spec.volumes[spec.volume_count].read_only =
				    (jro != NULL && jro->type == JSON_BOOL && jro->u.boolean);
				spec.volume_count++;
			}
		}
	}

	/*
	 * ADR-0179 (issue #29) phase 2 opt-in: "userns":true gives this
	 * container its own user namespace, its root mapped onto a dedicated
	 * host subordinate-ID range. Keyed on the container name for now (the
	 * ADR's own host-auth-off fallback key) -- preferring the caller's
	 * resolved uidnumber is a later refinement; both share the same
	 * allocator. Opt-in during verification specifically so the platform's
	 * existing running containers are untouched until this path is proven
	 * on real hardware, after which the ADR flips it to default-on.
	 */
	if (userns) {
		long long base;

		if (subid_lookup_or_assign(name, &base) != 0) {
			json_free(root);
			snprintf(err_msg, err_msg_size, "failed to allocate a userns subordinate-ID range");
			return 500;
		}
		spec.ns.clone_flags |= CLONE_NEWUSER;
		spec.userns_enabled = 1;
		spec.userns_uid_base = base;
		spec.userns_gid_base = base;
		spec.userns_len = SUBID_RANGE_LEN;
	}
	spec.ns.hostname = name;
	/* Issue #86: leaves live under the workload parent, so their
	 * COLLECTIVE demand is bounded and the control plane keeps a real
	 * reservation rather than whatever happens to be left over. */
	workload_cgroup_path(name, container_cgroup_path, sizeof(container_cgroup_path));
	spec.cg.name = container_cgroup_path;
	jmem = json_object_get(root, "memory_max");
	spec.cg.memory_max = jmem != NULL ? (long long)json_as_number(jmem) : 0;
	jswapmax = json_object_get(root, "memory_swap_max");
	/*
	 * Issue #52: absent is -1 (leave the kernel's own "max" alone), not
	 * 0 -- 0 is the operator asking for a container that may not swap
	 * at all, and the two must not collapse into one another.
	 */
	spec.cg.memory_swap_max = jswapmax != NULL ? (long long)json_as_number(jswapmax) : -1;
	if (jswapmax != NULL && spec.cg.memory_swap_max < 0) {
		json_free(root);
		snprintf(err_msg, err_msg_size,
		         "memory_swap_max must be a non-negative byte count -- 0 means this container "
		         "may not swap at all; omit the field to leave swap unlimited");
		return 400;
	}
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
	spec.ov.userns_rootfs = userns ? userns_rootfs : NULL;
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
	spec.cap_add_count = cap_add_count;
	for (i = 0; i < (size_t)cap_add_count; i++)
		snprintf(spec.cap_add[i], sizeof(spec.cap_add[i]), "%s", cap_add_names[i]);
	spec.argv = argv_buf;
	spec.envp = envp_ptrs;

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
	/* Issue #49: mirror the requested quota for read-back (see
	 * registry.h's own field comment for why this one isn't read live
	 * from the kernel like the cgroup limits are). */
	if (rerr == REGISTRY_OK && entry != NULL)
		entry->disk_quota_bytes = disk_quota_bytes;
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
		snprintf(err_msg, err_msg_size, "failed to create container: %s (%s)",
		         strerror(create_errno), container_create_last_error_step());
		logstore_write("thincd", "error", "container %s: failed to create: %s (%s)", name_copy,
		                strerror(create_errno), container_create_last_error_step());
		return 500;
	}

	register_container_pidfd(entry);

	/* ADR-0161 Phase B: live-run-only bookkeeping (see registry.h's own
	 * comment) -- a restart-capable container's own persisted body
	 * already carries these refs for free via replay; this call is
	 * what makes them visible to Phase C's hotplug listener for the
	 * rest of THIS daemon process's life, restart-capable or not. */
	if (pending_device_count > 0)
		registry_set_pending_devices(entry, pending_device_refs, pending_device_count);

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

	/* Issue #68: HTTP creates are strict about unknown keys -- see
	 * container_body_unknown_key()'s own comment. Parsed once extra
	 * here (create_container_from_body() re-parses); negligible cost
	 * against a container creation, and keeps the shared parser's
	 * replay callers lenient without a signature change across its
	 * five call sites. */
	{
		struct json_value *root = json_parse(body, body_len);
		const char *bad = container_body_unknown_key(root);

		if (bad != NULL) {
			snprintf(err_msg, sizeof(err_msg), "unknown field: %s", bad);
			json_free(root);
			respond_error(fd, 400, "Bad Request", err_msg);
			return;
		}
		json_free(root);
	}

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

	/*
	 * ADR-0181 (issue #73), superseding ADR-0027: persist EVERY container's
	 * definition, regardless of restart policy. `restart` now governs only
	 * auto-restart (on exit/boot/rolling); it no longer decides whether the
	 * container exists after it stops. So `stop` always keeps the container
	 * (startable), and only `delete` removes it -- stop is stop, delete is
	 * delete. `restart:"no"` means "persisted, never auto-restarted."
	 */
	{
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
			        "%s: persisting its definition failed -- it will not survive a daemon "
			        "restart and `start` after a stop will not find it (restart:\"%s\")\n",
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

	/*
	 * ADR-0180 (issue #67): a RUNNING container is never killed-and-
	 * waited-for synchronously here anymore -- that wait
	 * (registry_remove()'s own waitid()) is unbounded in principle (a
	 * task stuck in uninterruptible D-state only ever *pends* SIGKILL),
	 * and because this daemon is one epoll loop, it froze the entire
	 * control plane twice in real production before this existed.
	 * Instead: every piece of durable intent is written NOW (service-
	 * ownership forgets + containerdef removal below -- so no daemon
	 * restart in the window can resurrect the container), the SIGKILL
	 * is sent without waiting (registry_begin_kill()), and the entry's
	 * own already-registered pidfd epoll watch -- the exact machinery
	 * that detects ordinary crashes -- completes the teardown
	 * (registry slot release + disk cleanup) from
	 * handle_container_event() when the process actually dies. Until
	 * then the entry reports status "deleting", and a same-name create
	 * 409s off the still-in-use registry slot, exactly as it would
	 * against any live container.
	 */
	if (e != NULL && e->running && e->teardown_kind == REGISTRY_TEARDOWN_NONE) {
		dns_server_forget(name);
		dhcp_server_forget(name);
		dns_record_forget_owner(name);
		ldap_server_forget(name);
		ldap_user_forget_owner(name);
		pki_cert_forget_owner(name);
		ntp_server_forget(name);
		syslogfwd_target_forget(name);
		serverhealth_forget(name); /* issue #81 -- no health record for a gone container */
		containerdef_remove(name);
		registry_begin_kill(e, REGISTRY_TEARDOWN_DELETE);
		http_set_blocking(fd);
		http_write_response(fd, 204, "No Content", "application/json", "", 0);
		return;
	}
	/* A repeat DELETE while a prior one's teardown is still mid-flight:
	 * idempotent -- the intent is already fully recorded. A DELETE
	 * landing while a STOP's teardown is in flight upgrades the intent
	 * in place (records delete's own durable half now; the one pending
	 * pidfd event completes whichever kind it finds recorded). */
	if (e != NULL && e->running && e->teardown_kind != REGISTRY_TEARDOWN_NONE) {
		if (e->teardown_kind == REGISTRY_TEARDOWN_STOP) {
			dns_server_forget(name);
			dns_record_forget_owner(name);
			ldap_server_forget(name);
			ldap_user_forget_owner(name);
			pki_cert_forget_owner(name);
			ntp_server_forget(name);
			syslogfwd_target_forget(name);
			serverhealth_forget(name); /* issue #81 */
			containerdef_remove(name);
			e->teardown_kind = REGISTRY_TEARDOWN_DELETE;
		}
		http_set_blocking(fd);
		http_write_response(fd, 204, "No Content", "application/json", "", 0);
		return;
	}

	disk_name[0] = '\0';

	if (e != NULL) {
		if (e->reactor_conn != NULL) {
			cc = e->reactor_conn;
			thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
	dhcp_server_forget(name);
	dns_record_forget_owner(name);
	ldap_server_forget(name);
	ldap_user_forget_owner(name);
	pki_cert_forget_owner(name);
	ntp_server_forget(name);
	syslogfwd_target_forget(name);
	serverhealth_forget(name); /* issue #81 */

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

			thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, rc->fd, NULL);
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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
	if (entry != NULL && entry->running) {
		/* Genuinely live (or paused, which is still running=1) --
		 * start is idempotent, so this is a plain 200, not an error. */
		jw_init(&w);
		registry_write_json_one(entry, &w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
		return;
	}
	if (entry != NULL) {
		/*
		 * ADR-0181 (#73): a NOT-running entry here is a container that
		 * exited on its own and was deliberately retained as its own
		 * "exited" record (real exit code preserved). Starting it must
		 * actually bring it back, not silently 200 the stale exited
		 * entry -- so drop that record first, then replay the persisted
		 * definition below exactly as a stopped-but-defined container's
		 * own start already does. registry_remove() is safe on a
		 * non-running entry (its kill/reap branch is skipped, no signal
		 * sent to a possibly-reused pid) and frees the name/slot before
		 * create_container_from_body() re-creates it.
		 */
		registry_remove(name);
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
 * Issue #92: attach/detach a volume on an EXISTING container.
 *
 * Deliberately the opposite posture to POST .../networks, which is live
 * and ephemeral (ADR-0156): these edit the container's own persisted
 * definition and take effect on its NEXT start. A bind mount has to
 * land inside the container's own mount namespace, which only exists
 * between clone3() and pivot_root (ADR-0183) -- attaching to an
 * already-running container needs a primitive to enter another
 * namespace from outside that this daemon does not have. Rather than
 * pretend otherwise, the durable half ships now and says so plainly;
 * the live half stays tracked on #92.
 *
 * Editing the definition body is the whole mechanism -- no second
 * store of "which volumes does this container have", so nothing can
 * disagree with the definition that a restart actually replays.
 */
static void respond_container_volumes(int fd, int status, const char *status_text,
                                       const char *name, const struct json_value *body_root,
                                       const char *applies)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	containerdef_write_json_volumes(body_root, &w);
	jw_key(&w, "applies");
	/* Said explicitly rather than left for the caller to discover.
	 * "now" means the mount is already live in the running container;
	 * "on next start" means the definition is updated and the container
	 * will pick it up when it next starts. */
	jw_str(&w, applies);
	jw_obj_close(&w);
	respond_json(fd, status, status_text, &w);
	jw_free(&w);
}


/*
 * PATCH /v1/containers/{name} (issue #11) -- edit a container's stored
 * definition in place, instead of the delete-and-recreate that was the
 * only way to change a cmd, an env var or a file.
 *
 * Applies at the container's next start, and the response says so
 * rather than leaving the caller to guess. That is not a limitation
 * being papered over: a running process's argv cannot be changed
 * without re-exec'ing it, so "change cmd on a running container" is
 * `restart` by another name, and pretending otherwise would be the
 * kind of half-truth this API is supposed to avoid. What genuinely
 * CAN change live already does, through its own endpoints -- volumes
 * (issue #92) and network attach (ADR-0156).
 *
 * Merge semantics: a key present in the patch replaces that key; a key
 * set to null removes it; everything else is untouched. Top-level only
 * -- a deep merge would make "how do I clear one entry of files[]"
 * unanswerable.
 *
 * The four index fields (restart, depends_on, readiness,
 * follow_rolling) are deliberately refused with a 400 naming them.
 * They feed containerdef's cached index, whose one parser lives in the
 * create path; a second parser here for the same fields is exactly the
 * parallel implementation this project forbids. Lifting that needs
 * that parser extracted first -- a real follow-up, tracked, not a
 * placeholder left in the code.
 */
static void handle_container_patch(int fd, const char *name, const char *body, size_t body_len)
{
	static const char *const index_fields[] = { "restart", "restart_delay_seconds", "depends_on",
		                                        "readiness", "follow_rolling",
		                                        "follow_rolling_jitter_seconds" };
	struct container_def *def = containerdef_find(name);
	struct registry_entry *e = registry_find(name);
	struct json_value *patch, *current;
	struct json_writer w;
	size_t i, k;
	int rc;

	if (def == NULL) {
		respond_error(fd, 404, "Not Found",
		              "no such container definition -- only a container this daemon has a "
		              "persisted definition for can be edited");
		return;
	}
	patch = json_parse(body, body_len);
	if (patch == NULL || patch->type != JSON_OBJECT) {
		json_free(patch);
		respond_error(fd, 400, "Bad Request", "body must be a JSON object of fields to change");
		return;
	}
	for (i = 0; i < patch->u.object.count; i++) {
		if (strcmp(patch->u.object.keys[i], "name") == 0) {
			json_free(patch);
			respond_error(fd, 400, "Bad Request",
			              "a container's name is its identity -- create a new one instead");
			return;
		}
		for (k = 0; k < sizeof(index_fields) / sizeof(index_fields[0]); k++) {
			if (strcmp(patch->u.object.keys[i], index_fields[k]) != 0)
				continue;
			json_free(patch);
			{
				char msg[256];

				snprintf(msg, sizeof(msg),
				         "'%s' cannot be edited in place yet -- it feeds the definition index, "
				         "whose only parser is the create path; recreate the container to change "
				         "it",
				         index_fields[k]);
				respond_error(fd, 400, "Bad Request", msg);
			}
			return;
		}
	}

	current = json_parse(def->body, def->body_len);
	if (current == NULL || current->type != JSON_OBJECT) {
		json_free(patch);
		json_free(current);
		respond_error(fd, 500, "Internal Server Error",
		              "this container's stored definition could not be parsed");
		return;
	}

	jw_init(&w);
	jw_obj_open(&w);
	for (i = 0; i < current->u.object.count; i++) {
		const struct json_value *replacement = json_object_get(patch, current->u.object.keys[i]);

		/* An explicit null removes the key; anything else replaces it. */
		if (replacement != NULL && replacement->type == JSON_NULL)
			continue;
		jw_key(&w, current->u.object.keys[i]);
		jw_value(&w, replacement != NULL ? replacement : current->u.object.values[i]);
	}
	for (i = 0; i < patch->u.object.count; i++) {
		if (json_object_get(current, patch->u.object.keys[i]) != NULL)
			continue; /* already written above, in the stored body's own order */
		if (patch->u.object.values[i]->type == JSON_NULL)
			continue;
		jw_key(&w, patch->u.object.keys[i]);
		jw_value(&w, patch->u.object.values[i]);
	}
	jw_obj_close(&w);

	if (w.buf == NULL) {
		jw_free(&w);
		json_free(patch);
		json_free(current);
		respond_error(fd, 500, "Internal Server Error", "could not build the updated definition");
		return;
	}
	rc = containerdef_set_body(name, w.buf, w.len);
	jw_free(&w);
	json_free(patch);
	json_free(current);
	if (rc != 0) {
		respond_error(fd, 500, "Internal Server Error", "could not persist the updated definition");
		return;
	}

	def = containerdef_find(name);
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "applies");
	jw_str(&w, "next-start");
	jw_key(&w, "restart_required");
	jw_bool(&w, e != NULL && e->running);
	jw_key(&w, "definition");
	{
		struct json_value *merged = json_parse(def->body, def->body_len);

		if (merged != NULL)
			jw_value(&w, merged);
		else
			jw_null(&w);
		json_free(merged);
	}
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Rewrites def's stored body with `volumes` replaced by new_volumes
 * (which may be NULL to drop the key entirely). Every other field is
 * copied through the parsed tree verbatim.
 */
static int container_def_replace_volumes(struct container_def *def, const struct json_value *root,
                                          const struct json_value *const *new_volumes,
                                          size_t new_count)
{
	struct json_writer w;
	size_t i;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	for (i = 0; i < root->u.object.count; i++) {
		if (strcmp(root->u.object.keys[i], "volumes") == 0)
			continue;
		jw_key(&w, root->u.object.keys[i]);
		jw_value(&w, root->u.object.values[i]);
	}
	if (new_count > 0) {
		jw_key(&w, "volumes");
		jw_arr_open(&w);
		for (i = 0; i < new_count; i++)
			jw_value(&w, new_volumes[i]);
		jw_arr_close(&w);
	}
	jw_obj_close(&w);
	if (w.buf == NULL) {
		jw_free(&w);
		return -1;
	}
	rc = containerdef_set_body(def->name, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static void handle_container_volume_attach(int fd, const char *container_name, const char *body,
                                            size_t body_len)
{
	struct container_def *def = containerdef_find(container_name);
	struct json_value *req = NULL;
	struct json_value *root = NULL;
	const struct json_value *jvols;
	const struct json_value *existing[CONTAINER_MAX_VOLUMES + 1];
	size_t count = 0;
	const char *vname;
	const char *vpath;
	char vname_copy[VOLUME_NAME_MAX];
	char vpath_copy[VOLUME_MOUNT_PATH_MAX];
	int ro_copy = 0;
	size_t i;

	if (def == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}
	req = json_parse(body, body_len);
	if (req == NULL || req->type != JSON_OBJECT) {
		json_free(req);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	vname = json_as_string(json_object_get(req, "name"));
	vpath = json_as_string(json_object_get(req, "path"));
	if (vname == NULL || vpath == NULL || vpath[0] != '/' || !file_path_is_safe(vpath)) {
		json_free(req);
		respond_error(fd, 400, "Bad Request",
		              "name and an absolute path with no '..' are both required");
		return;
	}
	if (volume_find(vname) == NULL) {
		json_free(req);
		respond_error(fd, 404, "Not Found",
		              "no such volume -- create it first (POST /v1/volumes); a container is "
		              "never given a volume that does not already exist");
		return;
	}

	root = json_parse(def->body, def->body_len);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		json_free(req);
		respond_error(fd, 500, "Internal Server Error",
		              "this container's stored definition could not be parsed");
		return;
	}
	jvols = json_object_get(root, "volumes");
	if (jvols != NULL && jvols->type == JSON_ARRAY) {
		for (i = 0; i < jvols->u.array.count && count < CONTAINER_MAX_VOLUMES; i++) {
			const struct json_value *item = jvols->u.array.items[i];
			const char *n = json_as_string(json_object_get(item, "name"));
			const char *pth = json_as_string(json_object_get(item, "path"));

			/* Two different volumes cannot share a mount path, and
			 * the same volume twice is a no-op the caller almost
			 * certainly did not mean -- both are 409 rather than a
			 * silently-ignored duplicate. */
			if (n != NULL && strcmp(n, vname) == 0) {
				json_free(root);
				json_free(req);
				respond_error(fd, 409, "Conflict",
				              "this container already mounts that volume");
				return;
			}
			if (pth != NULL && strcmp(pth, vpath) == 0) {
				json_free(root);
				json_free(req);
				respond_error(fd, 409, "Conflict",
				              "this container already mounts a volume at that path");
				return;
			}
			existing[count++] = item;
		}
	}
	if (count >= CONTAINER_MAX_VOLUMES) {
		json_free(root);
		json_free(req);
		respond_error(fd, 409, "Conflict", "this container already has the maximum volumes");
		return;
	}
	existing[count++] = req;

	/* Copied before the parsed body is freed: the live-attach step
	 * below still needs them, and they point into `req`. */
	{
		const struct json_value *jro2 = json_object_get(req, "read_only");

		snprintf(vname_copy, sizeof(vname_copy), "%s", vname);
		snprintf(vpath_copy, sizeof(vpath_copy), "%s", vpath);
		ro_copy = (jro2 != NULL && jro2->type == JSON_BOOL && jro2->u.boolean);
	}
	if (container_def_replace_volumes(def, root, existing, count) != 0) {
		json_free(root);
		json_free(req);
		respond_error(fd, 500, "Internal Server Error", "could not persist the definition");
		return;
	}
	json_free(root);
	json_free(req);

	/*
	 * Issue #92 part 2: if the container is running, apply it NOW as
	 * well, rather than only on its next start.
	 *
	 * Deliberately after the definition is persisted, not instead of
	 * it. A live-only attach would vanish on the next restart with
	 * nothing to say so, which is the sharp edge ADR-0156's own
	 * live/ephemeral network attach explicitly accepts and documents;
	 * here the durable record is the source of truth and the live mount
	 * is it taking effect early.
	 *
	 * A failure to mount live is reported but does NOT undo the
	 * definition: the volume genuinely is part of this container now,
	 * and it will be there on the next start. Rolling back a correct
	 * definition because one optional step failed would be the worse
	 * outcome.
	 */
	{
		struct registry_entry *e = registry_find(container_name);
		char hostpath[PATH_MAX];
		struct volume *vol = volume_find(vname_copy);
		int live = 0;

		if (e != NULL && e->running && !e->paused && vol != NULL &&
		    volume_host_path(vol, hostpath, sizeof(hostpath)) == 0)
			live = mountns_bind_into(e->handle.pid, hostpath, vpath_copy, ro_copy) == 0;

		def = containerdef_find(container_name);
		root = def != NULL ? json_parse(def->body, def->body_len) : NULL;
		respond_container_volumes(fd, 200, "OK", container_name, root,
		                           e != NULL && e->running ? (live ? "now" : "on next start")
		                                                   : "on next start");
		json_free(root);
	}
}

static void handle_container_volume_detach(int fd, const char *container_name,
                                            const char *volume_name)
{
	struct container_def *def = containerdef_find(container_name);
	struct json_value *root;
	const struct json_value *jvols;
	const struct json_value *kept[CONTAINER_MAX_VOLUMES];
	size_t count = 0;
	int found = 0;
	size_t i;

	if (def == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}
	root = json_parse(def->body, def->body_len);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 500, "Internal Server Error",
		              "this container's stored definition could not be parsed");
		return;
	}
	jvols = json_object_get(root, "volumes");
	if (jvols != NULL && jvols->type == JSON_ARRAY) {
		for (i = 0; i < jvols->u.array.count && count < CONTAINER_MAX_VOLUMES; i++) {
			const struct json_value *item = jvols->u.array.items[i];
			const char *n = json_as_string(json_object_get(item, "name"));

			if (n != NULL && strcmp(n, volume_name) == 0) {
				found = 1;
				continue;
			}
			kept[count++] = item;
		}
	}
	if (!found) {
		json_free(root);
		respond_error(fd, 404, "Not Found", "this container does not mount that volume");
		return;
	}
	if (container_def_replace_volumes(def, root, kept, count) != 0) {
		json_free(root);
		respond_error(fd, 500, "Internal Server Error", "could not persist the definition");
		return;
	}
	json_free(root);

	def = containerdef_find(container_name);
	root = def != NULL ? json_parse(def->body, def->body_len) : NULL;
	respond_container_volumes(fd, 200, "OK", container_name, root, "on next start");
	json_free(root);
}

/*
 * POST /v1/containers/{name}/networks (ADR-0156, task #861): attaches
 * one more network to an ALREADY-RUNNING container, live -- no
 * recreate. Deliberately LIVE and EPHEMERAL, the same posture ADR-0153
 * already established for PUT .../files: never touches the
 * container's own persisted create-request body, so a future restart
 * or recreate replays the original definition, this attachment gone.
 * If it needs to survive a recreate, the durable path is editing the
 * container's own definition (a container recipe, ADR-0151) and
 * re-applying it -- not this endpoint, same division of responsibility
 * ADR-0153 already drew for files.
 *
 * Body shape is deliberately identical to one entry of POST
 * /containers' own "networks" array (parse_network_entry(), reused
 * verbatim) -- a bare network-name string for an auto-allocated IP, or
 * {"name":..., "ip":...} for an operator-chosen one.
 */
static void handle_container_network_attach(int fd, const char *container_name, const char *body,
                                              size_t body_len)
{
	struct registry_entry *e;
	struct json_value *root;
	char net_name[NETWORK_NAME_MAX];
	uint32_t ip_be;
	int has_ip;
	struct network_def *net;
	struct registry_network_attachment att;
	char veth_host[16], veth_ctr[16], ifname[16];
	struct json_writer w;
	int i;

	e = registry_find(container_name);
	if (e == NULL || !e->running) {
		respond_error(fd, 404, "Not Found", "no such running container");
		return;
	}

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	if (parse_network_entry(root, net_name, sizeof(net_name), &ip_be, &has_ip) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid network entry");
		return;
	}
	json_free(root);

	net = network_find(net_name);
	if (net == NULL) {
		respond_error(fd, 404, "Not Found", "no such network");
		return;
	}
	for (i = 0; i < e->net_count; i++) {
		if (strcmp(e->nets[i].name, net_name) == 0) {
			respond_error(fd, 409, "Conflict", "already attached to this network");
			return;
		}
	}
	if (e->net_count >= CONTAINER_MAX_NETWORKS) {
		respond_error(fd, 409, "Conflict", "container already has the maximum number of networks");
		return;
	}

	if (has_ip) {
		if (network_ip_available(net_name, ip_be) != NETWORK_OK) {
			respond_error(fd, 400, "Bad Request", "ip is not available on this network");
			return;
		}
	} else if (network_alloc_ip(net_name, &ip_be) != 0) {
		respond_error(fd, 500, "Internal Server Error", "no free IP addresses");
		return;
	}

	/* Same vh<pid>-<idx>/vc<pid>-<idx> naming scheme
	 * container_net_host_setup() uses at create time -- "a<idx>" instead
	 * of a bare "<idx>" only to keep a live-attached veth's own name
	 * visibly distinct from a create-time one at a glance (both are
	 * already guaranteed collision-free by pid+idx alone). idx is this
	 * container's own net_count *before* this attachment, exactly like
	 * create-time attachments' own idx is their position in that same
	 * array -- collision-free against both earlier create-time and
	 * earlier live attachments. */
	snprintf(veth_host, sizeof(veth_host), "vh%d-a%d", (int)e->handle.pid, e->net_count);
	snprintf(veth_ctr, sizeof(veth_ctr), "vc%d-a%d", (int)e->handle.pid, e->net_count);
	snprintf(ifname, sizeof(ifname), "eth%d", e->net_count);

	if (container_net_attach_running(net->name, ip_be, net->prefix_len, e->handle.pid, veth_host,
	                                  veth_ctr, ifname) != 0) {
		respond_error(fd, 500, "Internal Server Error", "failed to attach network");
		return;
	}

	memset(&att, 0, sizeof(att));
	strncpy(att.name, net_name, sizeof(att.name) - 1);
	att.ip_be = ip_be;
	strncpy(att.veth_host, veth_host, sizeof(att.veth_host) - 1);
	strncpy(att.ifname, ifname, sizeof(att.ifname) - 1);
	if (registry_network_attach(e, &att) != 0) {
		/* Shouldn't happen (bounds/dup already checked above) but the
		 * veth pair is already live at this point -- tear it back down
		 * rather than leaving an orphan the registry doesn't know
		 * about. */
		container_net_detach_running(veth_host);
		respond_error(fd, 500, "Internal Server Error", "failed to record network attachment");
		return;
	}

	jw_init(&w);
	registry_write_json_one(e, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * DELETE /v1/containers/{name}/networks/{network} (ADR-0156, task #861):
 * the reverse of the above. Only ever removes a LIVE attachment (its
 * own veth_host is non-empty) -- a network attached the ordinary way,
 * at container creation, has no standalone detach path (same
 * reasoning ADR-0153 gives for why a create-time "files[]" entry isn't
 * independently removable either): tearing it down mid-life would
 * silently diverge the running container from its own persisted
 * definition in a way nothing else in this API does.
 */
static void handle_container_network_detach(int fd, const char *container_name,
                                              const char *network_name)
{
	struct registry_entry *e;
	struct registry_network_attachment removed;
	struct json_writer w;
	int i;
	int is_live = 0;

	e = registry_find(container_name);
	if (e == NULL || !e->running) {
		respond_error(fd, 404, "Not Found", "no such running container");
		return;
	}
	for (i = 0; i < e->net_count; i++) {
		if (strcmp(e->nets[i].name, network_name) == 0) {
			is_live = e->nets[i].veth_host[0] != '\0';
			break;
		}
	}
	if (i == e->net_count) {
		respond_error(fd, 404, "Not Found", "not attached to this network");
		return;
	}
	if (!is_live) {
		respond_error(fd, 409, "Conflict",
		              "this network was attached at container creation, not live -- recreate the "
		              "container to remove it");
		return;
	}

	if (registry_network_detach(e, network_name, &removed) != 0) {
		respond_error(fd, 500, "Internal Server Error", "failed to update registry");
		return;
	}
	if (container_net_detach_running(removed.veth_host) != 0) {
		/* Registry already updated (GET now correctly shows it gone) --
		 * a real, orphaned host-side veth left over from a failed
		 * delete is a rare kernel-level failure, not something worth
		 * re-adding the registry entry over (that would just make GET
		 * lie about the network being reachable, which it now isn't
		 * either way). */
		fprintf(stderr, "%s: failed to remove live veth %s for network %s\n", container_name,
		        removed.veth_host, network_name);
	}

	jw_init(&w);
	registry_write_json_one(e, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * ADR-0161 Phase D: mknod()s dev's own node directly into an already-
 * running container's private /dev, reaching in via /proc/<pid>/root
 * -- the identical host-side mechanism ADR-0153's own live file write
 * (handle_container_file_write() above) already established and
 * proved correct, reused rather than re-invented for the mknod half
 * (container_dev_mknod() itself, src/container_dev.c, is CHILD-side
 * code -- it mknod()s at dev->dev_path directly, correct only when
 * already inside the target mount namespace post-pivot, which a
 * live-attach call from this daemon process never is). Real 0666
 * mode plus an explicit chmod() after mknod(), same reasoning
 * container_dev_mknod()'s own comment gives (this project's
 * containers run as full root with no user namespace, so the real
 * access gate is the BPF program, not these POSIX bits; mknod()'s
 * own requested mode is subject to this daemon's own umask).
 */
static int live_mknod_device(pid_t pid, const struct device_spec *dev)
{
	char full_path[PATH_MAX];
	char target_dir[PATH_MAX];
	char *slash;
	mode_t mode;

	if (snprintf(full_path, sizeof(full_path), "/proc/%d/root%s", (int)pid, dev->dev_path) >=
	    (int)sizeof(full_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	snprintf(target_dir, sizeof(target_dir), "%s", full_path);
	slash = strrchr(target_dir, '/');
	if (slash != NULL && slash != target_dir) {
		*slash = '\0';
		if (persist_mkdir_p(target_dir) != 0)
			return -1;
	}

	mode = (mode_t)((dev->type == DEVICE_NODE_BLOCK ? S_IFBLK : S_IFCHR) | 0666);
	if (mknod(full_path, mode, makedev(dev->major, dev->minor)) != 0 && errno != EEXIST)
		return -1;
	if (chmod(full_path, mode & 07777) != 0)
		return -1;
	return 0;
}

static int live_unlink_device(pid_t pid, const char *dev_path)
{
	char full_path[PATH_MAX];

	if (snprintf(full_path, sizeof(full_path), "/proc/%d/root%s", (int)pid, dev_path) >=
	    (int)sizeof(full_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (unlink(full_path) != 0 && errno != ENOENT)
		return -1;
	return 0;
}

/*
 * ADR-0161 Phase D: live-grants one already-resolved device to an
 * already-running container -- the real work shared by both the
 * manual POST /v1/containers/{name}/devices handler below and Phase
 * C's hotplug listener (handle_uevent_add()), so hotplug automation
 * is just another caller of this same primitive, not a separate code
 * path (the same two-layer shape ADR-0156 already established for
 * networks). BPF grant first (registry_device_live_attach(), atomic
 * replace, empirically verified safe -- see the ADR's own "Phase D
 * verification"), mknod second; on a live_mknod_device() failure the
 * BPF grant is rolled back rather than left dangling (permission
 * granted with no node to open it through is inert but not something
 * this daemon's own bookkeeping should silently carry forward).
 * Returns 0 on success, -1 (errno set) otherwise.
 */
static int live_attach_one_device(struct registry_entry *e, const struct discovered_device *dd)
{
	struct device_spec dev;
	struct registry_device_attachment att;

	memset(&dev, 0, sizeof(dev));
	dev.type = dd->type;
	dev.major = dd->major;
	dev.minor = dd->minor;
	snprintf(dev.dev_path, sizeof(dev.dev_path), "%s", dd->dev_path);

	memset(&att, 0, sizeof(att));
	snprintf(att.id, sizeof(att.id), "%s", dd->id);
	snprintf(att.dev_path, sizeof(att.dev_path), "%s", dd->dev_path);
	att.type = dd->type;
	att.major = dd->major;
	att.minor = dd->minor;
	att.live = 1;

	if (registry_device_live_attach(e, &dev, &att) != 0)
		return -1;

	if (live_mknod_device(e->handle.pid, &dev) != 0) {
		int saved_errno = errno;
		struct registry_device_attachment removed;

		registry_device_live_detach(e, dd->id, &removed);
		errno = saved_errno;
		return -1;
	}
	return 0;
}

/*
 * POST /v1/containers/{name}/devices (ADR-0161 Phase D): attaches one
 * more device to an ALREADY-RUNNING container, live -- no recreate.
 * Deliberately LIVE and EPHEMERAL, the same posture ADR-0153/ADR-0156
 * already established for files/networks: never touches the
 * container's own persisted create-request body.
 *
 * Body shape matches one entry of POST /containers' own "devices"
 * array bare-string form -- id is resolved exactly like a creation-
 * time entry is (devicemap_resolve() first, device_find_group()
 * fallback), so a grouped id (e.g. "gpu:0") can still expand into
 * more than one grant in a single call.
 */
static void handle_container_device_attach(int fd, const char *container_name, const char *body,
                                             size_t body_len)
{
	struct registry_entry *e;
	struct json_value *root;
	const char *id;
	const struct discovered_device *matches[CONTAINER_MAX_DEVICES];
	int n, j, attached;
	struct json_writer w;

	e = registry_find(container_name);
	if (e == NULL || !e->running) {
		respond_error(fd, 404, "Not Found", "no such running container");
		return;
	}

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	id = json_as_string(json_object_get(root, "id"));
	if (id == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "id is required");
		return;
	}

	n = devicemap_resolve(id, CONTAINERS_DIR, matches, CONTAINER_MAX_DEVICES - e->device_count);
	if (n < 0)
		n = device_find_group(id, CONTAINERS_DIR, matches,
		                       CONTAINER_MAX_DEVICES - e->device_count);
	if (n <= 0) {
		json_free(root);
		respond_error(fd, 404, "Not Found",
		              "unknown, unassignable, or not-currently-present device");
		return;
	}
	if (e->device_count + n > CONTAINER_MAX_DEVICES) {
		json_free(root);
		respond_error(fd, 409, "Conflict", "container already has the maximum number of devices");
		return;
	}
	for (j = 0; j < n; j++) {
		if (matches[j]->assignable)
			continue;
		json_free(root);
		respond_error(fd, 400, "Bad Request", "device is not currently assignable");
		return;
	}
	json_free(root);

	for (attached = 0; attached < n; attached++) {
		if (live_attach_one_device(e, matches[attached]) != 0)
			break;
	}
	if (attached < n) {
		/* Partial group failure -- unwind exactly what this call itself
		 * just granted (never anything the container already had
		 * before this request), same "leave no half-applied state"
		 * posture live_attach_one_device() itself already has for a
		 * single grant's own BPF-vs-mknod half. */
		int k;
		struct registry_device_attachment removed;

		for (k = 0; k < attached; k++) {
			registry_device_live_detach(e, matches[k]->id, &removed);
			live_unlink_device(e->handle.pid, matches[k]->dev_path);
		}
		respond_error(fd, 500, "Internal Server Error", "failed to attach device");
		return;
	}

	jw_init(&w);
	registry_write_json_one(e, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * DELETE /v1/containers/{name}/devices/{id} (ADR-0161 Phase D): the
 * reverse of the above. Refuses (409) a device that was granted at
 * container creation time (registry_device_live_detach()'s own -2)
 * -- no standalone detach path for one of those, the same rule
 * registry_network_detach() already enforces for networks.
 */
static void handle_container_device_detach(int fd, const char *container_name, const char *id)
{
	struct registry_entry *e;
	struct registry_device_attachment removed;
	int i;
	int is_live = 0;
	struct json_writer w;

	e = registry_find(container_name);
	if (e == NULL || !e->running) {
		respond_error(fd, 404, "Not Found", "no such running container");
		return;
	}
	for (i = 0; i < e->device_count; i++) {
		if (strcmp(e->devices[i].id, id) == 0) {
			is_live = e->devices[i].live;
			break;
		}
	}
	if (i == e->device_count) {
		respond_error(fd, 404, "Not Found", "not attached to this device");
		return;
	}
	if (!is_live) {
		respond_error(fd, 409, "Conflict",
		              "this device was attached at container creation, not live -- recreate the "
		              "container to remove it");
		return;
	}

	if (registry_device_live_detach(e, id, &removed) != 0) {
		respond_error(fd, 500, "Internal Server Error", "failed to update device grant");
		return;
	}
	if (live_unlink_device(e->handle.pid, removed.dev_path) != 0)
		fprintf(stderr, "%s: failed to remove live device node %s (id %s)\n", container_name,
		        removed.dev_path, id);

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
 *                container's own mount namespace/root, no setns()) --
 *                and, if that misses, the on-disk layers below too:
 *                running == 1 only means this daemon hasn't PROCESSED
 *                the exit yet, so /proc/<pid> can already be gone for a
 *                container that died the instant it started.
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
/* GET /v1/system/kmsg?tail=N -- tail of the kernel ring buffer (/dev/kmsg).
 * The only kernel-log window a shell-less installed host has: dmesg-class
 * diagnostics (mount failures, driver probes, OOM) over the REST API, the same
 * spirit as /v1/system/logs but for the KERNEL's own messages, not thincd's.
 * Single-threaded event loop, so a static ring buffer is safe. ADR-0179 phase
 * 2c needed this to read overlayfs's own "mounting read-only" pr_warn on the
 * shell-less .95 box. */
static void handle_kmsg(int fd, const struct http_request *req)
{
	static struct {
		long long ts;
		int prio;
		char text[256];
	} ring[512];
	struct json_writer w;
	char rbuf[8192], tail_str[16];
	int kfd, count = 0, head = 0, tail = 200, emit, start, i;
	ssize_t n;

	if (url_query_param(req->path, "tail", tail_str, sizeof(tail_str)) == 0) {
		tail = atoi(tail_str);
		if (tail < 1)
			tail = 1;
		if (tail > 512)
			tail = 512;
	}

	kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (kfd < 0) {
		respond_error(fd, 500, "Internal Server Error", "cannot open /dev/kmsg");
		return;
	}
	for (;;) {
		char *semi, *msgtext, *nl;
		int prio = 0;
		long long seq = 0, ts = 0;

		n = read(kfd, rbuf, sizeof(rbuf) - 1);
		if (n < 0) {
			if (errno == EPIPE) /* ring overwritten mid-read; skip ahead */
				continue;
			break; /* EAGAIN = drained, or a real error */
		}
		if (n == 0)
			break;
		rbuf[n] = '\0';
		/* Record header is "prio,seq,ts_usec,flags;message[\n continuation]". */
		sscanf(rbuf, "%d,%lld,%lld", &prio, &seq, &ts);
		semi = strchr(rbuf, ';');
		msgtext = (semi != NULL) ? semi + 1 : rbuf;
		nl = strchr(msgtext, '\n');
		if (nl != NULL)
			*nl = '\0';
		ring[head].ts = ts;
		ring[head].prio = prio;
		snprintf(ring[head].text, sizeof(ring[head].text), "%s", msgtext);
		head = (head + 1) % 512;
		if (count < 512)
			count++;
	}
	close(kfd);

	emit = (count < tail) ? count : tail;
	start = (head - emit + 512) % 512;
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "entries");
	jw_arr_open(&w);
	for (i = 0; i < emit; i++) {
		int idx = (start + i) % 512;

		jw_obj_open(&w);
		jw_key(&w, "ts_usec");
		jw_int(&w, ring[idx].ts);
		jw_key(&w, "priority");
		jw_int(&w, ring[idx].prio);
		jw_key(&w, "message");
		jw_str(&w, ring[idx].text);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

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

/*
 * Issue #61: list a directory inside a container.
 *
 * A path resolving to a directory was a 400 with no alternative, so
 * post-mortem navigation was blind path-guessing -- finding gcc's
 * include-fixed tree during the Part 201 investigation took several
 * probe recipes that one listing call would have replaced.
 *
 * A RUNNING container is listed through /proc/<pid>/root, which is the
 * kernel's own merged overlay view: correct by construction, whiteouts
 * and all.
 *
 * An EXITED one has no such view, so its two layers are merged here --
 * upper first, then lower for anything upper did not have. That needs
 * whiteouts handled properly rather than ignored: overlayfs marks a
 * deleted file with a character device of rdev 0 in the upper layer, so
 * listing naively would show a file the container had deleted, which is
 * a listing that lies. Those entries are skipped AND suppress the
 * lower-layer name behind them, which is what deletion means.
 *
 * Opaque directories (a directory replaced wholesale, marked with the
 * trusted.overlay.opaque xattr) are NOT handled -- reading xattrs adds
 * a dependency for a case this daemon never creates, and the failure
 * mode is showing a stale name rather than hiding a real one.
 */
static int dirent_is_whiteout(const char *dir, const char *name)
{
	char path[PATH_MAX];
	struct stat st;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (lstat(path, &st) != 0)
		return 0;
	return S_ISCHR(st.st_mode) && st.st_rdev == 0;
}

static void handle_container_dir_list(int fd, const char *name, const char *rel_path)
{
	struct registry_entry *e = registry_find(name);
	char dirs[2][PATH_MAX];
	int dir_count = 0;
	char seen[512][256];
	int seen_count = 0;
	struct json_writer w;
	struct stat st;
	int i;
	int any = 0;

	if (e == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}
	if (e->running) {
		snprintf(dirs[0], sizeof(dirs[0]), "/proc/%d/root%s", (int)e->handle.pid, rel_path);
		if (stat(dirs[0], &st) == 0 && S_ISDIR(st.st_mode))
			dir_count = 1;
	}
	if (dir_count == 0) {
		char container_root[PATH_MAX];

		container_root_for(e->disk_name, container_root, sizeof(container_root));
		snprintf(dirs[0], sizeof(dirs[0]), "%s/%s/upper%s", container_root, name, rel_path);
		if (stat(dirs[0], &st) == 0 && S_ISDIR(st.st_mode))
			dir_count = 1;
		if (e->lowerdir[0] != '\0') {
			snprintf(dirs[dir_count], sizeof(dirs[dir_count]), "%s%s", e->lowerdir, rel_path);
			if (stat(dirs[dir_count], &st) == 0 && S_ISDIR(st.st_mode))
				dir_count++;
		}
	}
	if (dir_count == 0) {
		respond_error(fd, 404, "Not Found", "no such directory");
		return;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "path");
	jw_str(&w, rel_path);
	jw_key(&w, "entries");
	jw_arr_open(&w);
	for (i = 0; i < dir_count; i++) {
		DIR *d = opendir(dirs[i]);
		struct dirent *de;

		if (d == NULL)
			continue;
		any = 1;
		while ((de = readdir(d)) != NULL && seen_count < 512) {
			char entry_path[PATH_MAX];
			struct stat est;
			int dup = 0;
			int k;

			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;
			for (k = 0; k < seen_count; k++) {
				if (strcmp(seen[k], de->d_name) == 0) {
					dup = 1;
					break;
				}
			}
			if (dup)
				continue;
			/* Record the name before deciding whether to emit it: a
			 * whiteout must also hide the lower layer's copy, which is
			 * the whole point of it. */
			snprintf(seen[seen_count], sizeof(seen[seen_count]), "%s", de->d_name);
			seen_count++;
			if (dirent_is_whiteout(dirs[i], de->d_name))
				continue;

			snprintf(entry_path, sizeof(entry_path), "%s/%s", dirs[i], de->d_name);
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, de->d_name);
			jw_key(&w, "type");
			if (lstat(entry_path, &est) == 0)
				jw_str(&w, S_ISDIR(est.st_mode)    ? "dir"
				           : S_ISLNK(est.st_mode)  ? "symlink"
				           : S_ISREG(est.st_mode)  ? "file"
				                                   : "other");
			else
				jw_str(&w, "unknown");
			jw_key(&w, "size");
			jw_int(&w, (lstat(entry_path, &est) == 0 && S_ISREG(est.st_mode))
			               ? (long long)est.st_size
			               : 0);
			jw_obj_close(&w);
		}
		closedir(d);
	}
	jw_arr_close(&w);
	jw_key(&w, "truncated");
	/* Said rather than implied: a directory with more entries than the
	 * cap would otherwise look complete and short. */
	jw_bool(&w, seen_count >= 512);
	jw_obj_close(&w);
	if (!any) {
		jw_free(&w);
		respond_error(fd, 404, "Not Found", "no such directory");
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_container_file_read(int fd, const char *name, const char *rel_path)
{
	struct registry_entry *e = registry_find(name);
	char full_path[PATH_MAX];
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

	file_fd = -1;
	if (e->running) {
		snprintf(full_path, sizeof(full_path), "/proc/%d/root%s", (int)e->handle.pid, rel_path);
		file_fd = open(full_path, O_RDONLY);
	}
	/*
	 * Fall through to the on-disk layers whenever the /proc path didn't
	 * produce the file -- not only when running == 0. running == 1 means
	 * "this daemon has not yet PROCESSED the exit", not "the process is
	 * definitely alive": a container whose own exec fails (or which exits
	 * the instant it starts) is genuinely dead while its pidfd event is
	 * still sitting unread in epoll, and /proc/<pid> is already gone by
	 * then. Without this fallback such a read 404s purely on event timing
	 * -- observed deterministically via a recipe-applied container running
	 * a cmd its image doesn't actually contain, where the staged file was
	 * sitting in the upper layer the whole time. Reading the layers is
	 * always safe here: they are this container's OWN directories, keyed
	 * by name, never a reused pid.
	 */
	if (file_fd < 0) {
		char container_root[PATH_MAX];

		container_root_for(e->disk_name, container_root, sizeof(container_root));
		snprintf(full_path, sizeof(full_path), "%s/%s/upper%s", container_root, name, rel_path);
		file_fd = open(full_path, O_RDONLY);
		if (file_fd < 0) {
			/*
			 * Not in the upper layer -- fall back to the container's
			 * own read-only lowerdir, i.e. the exact tree its overlay
			 * was actually built on (entry->lowerdir, recorded verbatim
			 * at creation time). Issue #61: this used to RECONSTRUCT
			 * that path from image/image_version instead, which works
			 * only for an ordinary container. A build container
			 * ("__pkgbuild-N") is registered under the synthetic image
			 * name "pkgbuild" with an empty version, so both branches of
			 * that reconstruction missed and it fell through to an EMPTY
			 * prefix -- turning the read into a bare open() of rel_path
			 * on the HOST filesystem. That both failed to find real
			 * lowerdir content (the reported bug: /usr/bin/gcc 404ing on
			 * a preserved build container while /build/* worked) and, far
			 * worse, could serve the daemon host's OWN file at that path
			 * through a per-container endpoint. Using the recorded
			 * lowerdir fixes both at once and keeps ADR-0107/0108's
			 * guarantee intact -- for an ordinary container this IS the
			 * pinned image-version rootfs, byte-for-byte the same path
			 * the reconstruction produced, never the image's current
			 * version. An empty lowerdir (userns containers, which have
			 * no overlay) correctly reads as "no fallback".
			 */
			if (e->lowerdir[0] != '\0') {
				snprintf(full_path, sizeof(full_path), "%s%s", e->lowerdir, rel_path);
				file_fd = open(full_path, O_RDONLY);
			}
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

	/*
	 * Same running-but-already-dead case the GET path above documents:
	 * running == 1 only means this daemon hasn't processed the exit yet.
	 * A container that died the instant it started is a zombie whose
	 * /proc/<pid>/root is already a dangling link, so writing through it
	 * fails; its own upper layer is the correct target then. Probing
	 * /proc/<pid>/root is safe and unambiguous -- the daemon still holds
	 * the pidfd, so the pid cannot have been recycled by an unrelated
	 * process.
	 */
	{
		int use_proc = 0;

		if (e->running) {
			char proc_root[64];

			snprintf(proc_root, sizeof(proc_root), "/proc/%d/root", (int)e->handle.pid);
			use_proc = (access(proc_root, F_OK) == 0);
		}
		if (use_proc) {
			snprintf(full_path, sizeof(full_path), "/proc/%d/root%s", (int)e->handle.pid, rel_path);
		} else {
			char container_root[PATH_MAX];

			container_root_for(e->disk_name, container_root, sizeof(container_root));
			snprintf(full_path, sizeof(full_path), "%s/%s/upper%s", container_root, name, rel_path);
		}
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
	/* Issue #52: a swap limit with no way to see swap use is half a
	 * feature -- the number an operator acts on is how close the
	 * container is to it. */
	long long swap_current = 0, swap_max = 0;
	int swap_max_unlimited = 1;
	int swap_current_unlimited = 0;
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
	if (cgroup_read_single_value(e->handle.cgroup_fd, "memory.swap.current", &swap_current,
	                              &swap_current_unlimited) != 0)
		swap_current = 0;
	if (cgroup_read_single_value(e->handle.cgroup_fd, "memory.swap.max", &swap_max,
	                              &swap_max_unlimited) != 0)
		swap_max_unlimited = 1;
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
	jw_key(&w, "swap_current");
	jw_int(&w, swap_current);
	jw_key(&w, "swap_max");
	if (swap_max_unlimited)
		jw_null(&w);
	else
		jw_int(&w, swap_max);
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

		container_veth_host_name(e, i, veth, sizeof(veth));
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
 * policy (kills any live container). Since ADR-0181 every container has
 * a persisted definition, so a stop always leaves one behind -- the
 * container reappears as "stopped", never vanishing, which is the whole
 * point of that ADR ("stop is stop, delete is delete"). containerdef_
 * set_stopped() records the intent on that def (still a harmless no-op
 * for a genuinely def-less internal container like "__pkgbuild"); for a
 * restart:"no" def it sets the flag but changes no behavior, since "no"
 * is already never autostarted or crash-restarted. What stopped means
 * going forward is up to the two sites that consult it (containerdef_
 * autostart_all(), handle_restart_timer_event()), not this handler.
 * Idempotent: calling twice is both 200.
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
	int was_pkgbuild = (pkg_build_container_chain_index(name) >= 0);

	if (e == NULL && containerdef_find(name) == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}

	/*
	 * ADR-0180 (issue #67): same asynchronous-teardown conversion as
	 * DELETE's own running branch (see handle_delete()'s comment for
	 * the full reasoning -- this handler shared the identical
	 * unbounded registry_remove() wait). The durable half of stop's
	 * intent (the stopped flag, which is what keeps a restart:"always"
	 * definition down across daemon restarts) is written NOW; the
	 * registry slot release -- and, for a pkgbuild container, the
	 * pkg_build_completed() bookkeeping that needs the real exit
	 * status -- completes from handle_container_event() when the
	 * process actually dies. Idempotent for a stop already in flight;
	 * a stop during a DELETE teardown changes nothing (delete's intent
	 * strictly supersedes).
	 */
	if (e != NULL && e->running) {
		containerdef_set_stopped(name, 1);
		if (e->teardown_kind == REGISTRY_TEARDOWN_NONE)
			registry_begin_kill(e, REGISTRY_TEARDOWN_STOP);

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, name);
		jw_key(&w, "status");
		jw_str(&w, "stopping");
		jw_obj_close(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
		return;
	}

	if (e != NULL) {
		/* Exited-but-still-registered (a crashed entry, or a
		 * keep_on_failure-preserved build container): the original
		 * synchronous flow -- registry_remove()'s kill/wait branch is
		 * skipped for a non-running entry, so nothing here can block. */
		if (e->reactor_conn != NULL) {
			cc = e->reactor_conn;
			thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
			free(cc);
			e->reactor_conn = NULL;
		}
		registry_remove(name);
		if (was_pkgbuild) {
			pid_t pkg_pid;
			int pkg_pidfd;
			int pkg_chain_idx;
			char hostbuild_done_name[PKG_NAME_MAX];
			/* An explicit operator stop always tears the container
			 * down (registry_remove() above already ran) regardless
			 * of keep_on_failure -- that flag is only ever about an
			 * unprompted build failure, never a deliberately-killed
			 * one. Discarded here for exactly that reason. */
			int kept_ignored;

			if (pkg_build_completed(name, e->exit_status, &pkg_pid, &pkg_pidfd,
			                         &pkg_chain_idx, hostbuild_done_name, &kept_ignored))
				register_pkg_fetch_pidfd(pkg_pid, pkg_pidfd, pkg_chain_idx);
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
 * container's own overlay directory. Requires a persisted definition to
 * exist -- the cutover this triggers always ends with a real stop-then-
 * replay, so a container with nothing to replay from can never safely
 * reach that point. Since ADR-0181 (issue #73) every container IS
 * persisted, restart:"no" included, so that check is now defence in
 * depth for a genuinely def-less internal container rather than the
 * routine restart-policy gate it originally was. disk_name
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
	const char *swap_disk = storageplacement_get(STORAGE_KIND_SWAP);

	return (state_disk != NULL && strcmp(state_disk, disk_name) == 0) ||
	       (log_disk != NULL && strcmp(log_disk, disk_name) == 0) ||
	       (rebuildable_disk != NULL && strcmp(rebuildable_disk, disk_name) == 0) ||
	       (backup_disk != NULL && strcmp(backup_disk, disk_name) == 0) ||
	       (swap_disk != NULL && strcmp(swap_disk, disk_name) == 0);
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
		              "placement, the configured backup-config disk, or the current swap placement "
		              "(issue #28) -- migrate away first (POST .../migrate to a different disk or "
		              "disk: null, PUT /v1/system/backup-config with a different disk/null, or "
		              "POST /v1/system/swap with a different disk/omitted) before removing its role");
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
		              "this disk holds the fixed OS layout -- it can never be formatted or unmounted");
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
	case DISKFORMAT_ERR_NOT_MOUNTED:
		respond_error(fd, 400, "Bad Request", "this disk is not currently mounted");
		break;
	case DISKFORMAT_ERR_UMOUNT_FAILED:
		respond_error(fd, 500, "Internal Server Error",
		              "umount2(2) failed -- something on this disk may still be busy/in use");
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
		              "this disk is the active state-storage, log-storage, or rebuildable-storage "
		              "placement, the configured backup-config disk, or the current swap placement "
		              "(issue #28) -- formatting it would destroy live data; migrate away first");
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
	 * thinc-hosttools image, ADR-0103's own scope note) -- if it
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

/*
 * Found live, issue #34: a disk an operator has already `diskrole rm`'d
 * stays mounted forever -- there was no way to actually let go of it,
 * and a real, currently-mounted-but-role-less disk turned out to
 * correlate with a genuine mountns_pivot() EXDEV failure in every
 * subsequent container creation. Synchronous (diskformat_unmount()'s
 * own doc comment), not async like format -- a real umount2(2) is fast.
 * Same double-confirmation shape as handle_disk_format_post() (this is
 * a real, if less destructive, action -- data on the disk survives, but
 * anything still relying on it being mounted stops working the instant
 * this succeeds) and the identical two data-safety checks that handler
 * already has: refuses a disk that's the active placement for any
 * storage singleton/backup-config/swap, or that a live container has
 * its own storage on, exactly the same reasoning -- unmounting out from
 * under either would silently break something already relying on it.
 */
static void handle_disk_unmount_post(int fd, const char *disk_name, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *confirm;
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
		              "confirm_disk_name must be given and must match the disk name in the URL");
		return;
	}
	json_free(root);

	if (is_active_storage_singleton_placement(disk_name)) {
		respond_error(fd, 409, "Conflict",
		              "this disk is the active state-storage, log-storage, or rebuildable-storage "
		              "placement, the configured backup-config disk, or the current swap placement -- "
		              "unmounting it would break whatever currently relies on it; migrate away first");
		return;
	}
	if (disk_has_container_in_use(disk_name)) {
		respond_error(fd, 409, "Conflict",
		              "one or more containers currently have their own storage on this disk -- "
		              "unmounting it would break them; migrate them away first "
		              "(POST /containers/{name}/migrate-storage)");
		return;
	}

	derr = diskformat_unmount(disk_name, CONTAINERS_DIR);
	if (derr != DISKFORMAT_OK) {
		respond_diskformat_error(fd, derr);
		return;
	}

	{
		struct discovered_disk disks[DISK_ENUM_MAX];
		struct discovered_disk d;
		int n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
		int i;
		struct json_writer w;

		memset(&d, 0, sizeof(d));
		snprintf(d.name, sizeof(d.name), "%s", disk_name);
		for (i = 0; i < n; i++) {
			if (strcmp(disks[i].name, disk_name) == 0) {
				d = disks[i];
				break;
			}
		}
		jw_init(&w);
		disk_write_json_one(&d, &w);
		respond_json(fd, 200, "OK", &w);
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
 * Partition-level disk management (ROADMAP.md task #844) -- diskpart.h's
 * own doc comment covers the design; these three handlers are thin REST
 * wrappers, the same shape every other disk-management handler above
 * already has.
 */
static void respond_diskpart_error(int fd, enum diskpart_error err)
{
	switch (err) {
	case DISKPART_ERR_INVALID_DISK_NAME:
		respond_error(fd, 400, "Bad Request", "invalid disk name");
		break;
	case DISKPART_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such disk or partition");
		break;
	case DISKPART_ERR_IS_PARTITION:
		respond_error(fd, 400, "Bad Request",
		              "this name is itself a partition -- partition-table operations target a whole disk");
		break;
	case DISKPART_ERR_NOT_A_PARTITION:
		respond_error(fd, 400, "Bad Request", "this name is a whole disk, not a partition");
		break;
	case DISKPART_ERR_WRONG_PARENT:
		respond_error(fd, 404, "Not Found", "this partition does not belong to the named disk");
		break;
	case DISKPART_ERR_IS_OS_DISK:
		respond_error(fd, 400, "Bad Request",
		              "this disk holds the fixed OS layout -- it can never be repartitioned");
		break;
	case DISKPART_ERR_HAS_ROLE:
		respond_error(fd, 409, "Conflict",
		              "this disk or partition already has a role assigned -- remove it first "
		              "(DELETE /v1/diskroles/{name})");
		break;
	case DISKPART_ERR_MOUNTED:
		respond_error(fd, 409, "Conflict", "this disk or partition is currently mounted");
		break;
	case DISKPART_ERR_INVALID_PART_NAME:
		respond_error(fd, 400, "Bad Request", "invalid partition name");
		break;
	case DISKPART_ERR_INVALID_SIZE:
		respond_error(fd, 400, "Bad Request", "invalid size_mib");
		break;
	case DISKPART_ERR_SHRINK_REFUSED:
		respond_error(fd, 400, "Bad Request",
		              "a partition can only be grown, never shrunk -- shrinking requires the "
		              "filesystem inside it to be shrunk first, and cutting the table entry "
		              "before that destroys the tail of a live filesystem");
		break;
	case DISKPART_ERR_NO_ROOM_AFTER:
		respond_error(fd, 409, "Conflict",
		              "there is not enough free space immediately after this partition -- free "
		              "space elsewhere on the disk cannot extend it (see GET "
		              "/v1/disks/{name}/free-space)");
		break;
	case DISKPART_ERR_FS_UNSUPPORTED:
		respond_error(fd, 400, "Bad Request",
		              "only an ext4 or unformatted partition can be grown -- growing a btrfs "
		              "filesystem needs it mounted, and this operation needs it unmounted");
		break;
	case DISKPART_ERR_FS_UNCLEAN:
		respond_error(fd, 409, "Conflict",
		              "the filesystem needs a check that cannot be made automatically; the "
		              "partition table was NOT changed. Check it by hand before retrying");
		break;
	case DISKPART_ERR_RESIZE_FS_FAILED:
		respond_error(fd, 500, "Internal Server Error",
		              "the partition grew but the filesystem inside it could not be grown to "
		              "match -- the extra space is real but not yet usable; run resize2fs "
		              "against it by hand");
		break;
	case DISKPART_ERR_SFDISK_MISSING:
		respond_error(fd, 500, "Internal Server Error",
		              "sfdisk is not installed on this host -- partitioning needs "
		              "/usr/sbin/sfdisk, which this control-plane image does not carry. "
		              "Update to an image built after this was fixed.");
		break;
	case DISKPART_ERR_SFDISK_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error",
		              "sfdisk ran and rejected the request -- the disk may already have the "
		              "partition table or layout being asked for, or there is not enough free "
		              "space left on it for the requested size");
		break;
	}
}

/*
 * GET /v1/disks/{name}/free-space (issue #95) -- how much room is
 * actually left in this disk's partition table, from sfdisk rather than
 * from subtracting the partition sizes a client can already see. That
 * subtraction is wrong in three ways it cannot detect: partition
 * alignment, the GPT's own reserved areas at both ends, and any gap an
 * earlier delete left in the middle.
 *
 * Its own endpoint rather than a field on GET /disks, because it forks
 * a subprocess and GET /disks runs on every poll for every disk.
 */
static void handle_disk_free_space(int fd, const char *disk_name)
{
	struct diskpart_free_space fs;
	enum diskpart_error derr;
	struct json_writer w;
	int i;

	derr = diskpart_free_space(disk_name, CONTAINERS_DIR, &fs);
	if (derr != DISKPART_OK) {
		respond_diskpart_error(fd, derr);
		return;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "disk_name");
	jw_str(&w, disk_name);
	jw_key(&w, "has_partition_table");
	jw_bool(&w, fs.has_table);
	jw_key(&w, "total_free_bytes");
	jw_int(&w, (long long)fs.total_free_bytes);
	/*
	 * The largest single extent, which is the number that actually
	 * bounds a new partition -- total free space can be spread across
	 * gaps no one request can use.
	 */
	jw_key(&w, "largest_free_bytes");
	jw_int(&w, (long long)(fs.largest_free_sectors * fs.sector_bytes));
	jw_key(&w, "largest_free_mib");
	jw_int(&w, (long long)(fs.largest_free_sectors * fs.sector_bytes / (1024 * 1024)));
	jw_key(&w, "extents");
	jw_arr_open(&w);
	for (i = 0; i < fs.extent_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "start_sector");
		jw_int(&w, (long long)fs.extents[i].start_sector);
		jw_key(&w, "sectors");
		jw_int(&w, (long long)fs.extents[i].sectors);
		jw_key(&w, "bytes");
		jw_int(&w, (long long)(fs.extents[i].sectors * fs.sector_bytes));
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_disk_partition_table_post(int fd, const char *disk_name, const char *body,
                                              size_t body_len)
{
	struct json_value *root;
	const char *confirm;
	enum diskpart_error derr;
	struct discovered_disk d;

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
	json_free(root);

	derr = diskpart_create_table(disk_name, CONTAINERS_DIR);
	if (derr != DISKPART_OK) {
		respond_diskpart_error(fd, derr);
		return;
	}

	{
		struct discovered_disk disks[DISK_ENUM_MAX];
		int n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
		int i;
		struct json_writer w;

		for (i = 0; i < n; i++) {
			if (strcmp(disks[i].name, disk_name) == 0) {
				d = disks[i];
				break;
			}
		}
		jw_init(&w);
		disk_write_json_one(&d, &w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

static void handle_disk_partitions_post(int fd, const char *disk_name, const char *body,
                                         size_t body_len)
{
	struct json_value *root;
	const char *part_name;
	const struct json_value *jsize;
	unsigned long long size_mib = 0;
	enum diskpart_error derr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	part_name = json_as_string(json_object_get(root, "name"));
	if (part_name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name is required");
		return;
	}
	jsize = json_object_get(root, "size_mib");
	if (jsize != NULL) {
		if (jsize->type != JSON_NUMBER || json_as_number(jsize) < 0) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "size_mib must be a non-negative number");
			return;
		}
		size_mib = (unsigned long long)json_as_number(jsize);
	}

	{
		char part_name_buf[DISKPART_NAME_MAX];

		snprintf(part_name_buf, sizeof(part_name_buf), "%s", part_name);
		json_free(root);

		derr = diskpart_add(disk_name, CONTAINERS_DIR, part_name_buf, size_mib);
		if (derr != DISKPART_OK) {
			respond_diskpart_error(fd, derr);
			return;
		}
	}

	{
		struct discovered_disk disks[DISK_ENUM_MAX];
		int n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
		int i;
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "disk_name");
		jw_str(&w, disk_name);
		jw_key(&w, "partitions");
		jw_arr_open(&w);
		for (i = 0; i < n; i++) {
			if (disks[i].is_partition && strcmp(disks[i].parent_disk, disk_name) == 0)
				disk_write_json_one(&disks[i], &w);
		}
		jw_arr_close(&w);
		jw_obj_close(&w);
		respond_json(fd, 201, "Created", &w);
		jw_free(&w);
	}
}

/*
 * POST /v1/disks/{disk}/partitions/{partition}/resize (issue #94).
 *
 * Grow only, and both halves of the job: the table entry, then the
 * filesystem inside it. Growing only the entry would leave the extra
 * space invisible to everything using the filesystem, which reads as
 * the resize having silently done nothing.
 */
static void handle_disk_partition_resize(int fd, const char *disk_name, const char *partition_name,
                                          const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jsize;
	unsigned long long size_mib = 0;
	enum diskpart_error derr;

	root = json_parse(body, body_len);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jsize = json_object_get(root, "size_mib");
	if (jsize != NULL) {
		if (jsize->type != JSON_NUMBER || jsize->u.number < 0) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "size_mib must be a non-negative number");
			return;
		}
		size_mib = (unsigned long long)jsize->u.number;
	}
	json_free(root);

	derr = diskpart_resize(disk_name, partition_name, CONTAINERS_DIR, size_mib);
	if (derr != DISKPART_OK) {
		respond_diskpart_error(fd, derr);
		return;
	}
	{
		struct discovered_disk disks[DISK_ENUM_MAX];
		int n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
		int i;
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "disk_name");
		jw_str(&w, disk_name);
		jw_key(&w, "partitions");
		jw_arr_open(&w);
		for (i = 0; i < n; i++) {
			if (disks[i].is_partition && strcmp(disks[i].parent_disk, disk_name) == 0)
				disk_write_json_one(&disks[i], &w);
		}
		jw_arr_close(&w);
		jw_obj_close(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

static void handle_disk_partition_delete(int fd, const char *disk_name, const char *partition_name)
{
	enum diskpart_error derr = diskpart_delete(disk_name, partition_name, CONTAINERS_DIR);

	if (derr != DISKPART_OK) {
		respond_diskpart_error(fd, derr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
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

	/* Issue #70: optional auto-allocation window (dotted IPs, in-subnet);
	 * omitted = full default range. */
	{
		const char *alloc_start = json_as_string(json_object_get(root, "alloc_start"));
		const char *alloc_end = json_as_string(json_object_get(root, "alloc_end"));

		nerr = network_create(name, subnet, prefix_len, address, alloc_start, alloc_end, &net);
	}
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

/*
 * Issue #26: a network's ports, as a switch panel would show them.
 *
 * The port list comes from the KERNEL -- everything currently enslaved
 * to this network's bridge, read from /sys/class/net/<bridge>/brif --
 * not from what this daemon believes it attached. If the two ever
 * disagree, the kernel is the one that is right, and a port we cannot
 * account for is reported as such rather than dropped: a veth on the
 * bridge that belongs to no container we know of is exactly the kind
 * of thing an operator needs to see, and the only way to see it is to
 * ask the switch what is plugged into it.
 *
 * The registry is the annotation layer on top: which container owns a
 * port, what that interface is called inside it, which IP it holds.
 */
struct net_port {
	char ifname[32];
	const char *kind;                 /* "uplink" / "container" / "unattributed" */
	int vlan_id;                      /* uplinks only; -1 when not applicable */
	char container[REGISTRY_NAME_MAX];
	char container_ifname[16];
	char ip[16];
	int sort_group;                   /* uplinks first, then containers, then the rest */
};

#define NET_PORTS_MAX 256

static int net_port_cmp(const void *a, const void *b)
{
	const struct net_port *pa = a;
	const struct net_port *pb = b;

	if (pa->sort_group != pb->sort_group)
		return pa->sort_group - pb->sort_group;
	if (pa->sort_group == 1) {
		int c = strcmp(pa->container, pb->container);

		if (c != 0)
			return c;
	}
	return strcmp(pa->ifname, pb->ifname);
}

static void net_port_write_link_state(struct json_writer *w, const char *ifname)
{
	char path[PATH_MAX];
	char buf[32];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifname);
	f = fopen(path, "r");
	if (f == NULL || fgets(buf, sizeof(buf), f) == NULL) {
		if (f != NULL)
			fclose(f);
		/* The interface vanished between listing the bridge and asking
		 * about it -- a container stopping mid-request. Unknown is the
		 * true answer; "down" would be a guess. */
		jw_null(w);
		return;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	jw_str(w, buf);
}

static void handle_network_ports_get(int fd, const char *name)
{
	struct network_def *net = network_find(name);
	struct net_port ports[NET_PORTS_MAX];
	int port_count = 0;
	char cnames[REGISTRY_MAX_CONTAINERS][REGISTRY_NAME_MAX];
	int ccount;
	char brif_path[PATH_MAX];
	DIR *d;
	struct dirent *de;
	struct json_writer w;
	int i;
	int j;

	if (net == NULL) {
		respond_error(fd, 404, "Not Found", "no such network");
		return;
	}

	snprintf(brif_path, sizeof(brif_path), "/sys/class/net/%s/brif", name);
	d = opendir(brif_path);
	if (d == NULL) {
		/*
		 * No bridge on this host: a network defined in state but not
		 * realised, or a dev daemon that never had the privilege to
		 * create one. An empty port list with the reason said out loud
		 * beats a 500 that reads as a fault.
		 */
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "network");
		jw_str(&w, name);
		jw_key(&w, "bridge_present");
		jw_bool(&w, 0);
		jw_key(&w, "ports");
		jw_arr_open(&w);
		jw_arr_close(&w);
		jw_obj_close(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
		return;
	}

	ccount = registry_list_names(cnames, REGISTRY_MAX_CONTAINERS);
	while ((de = readdir(d)) != NULL && port_count < NET_PORTS_MAX) {
		struct net_port *p;

		if (de->d_name[0] == '.')
			continue;
		p = &ports[port_count];
		memset(p, 0, sizeof(*p));
		snprintf(p->ifname, sizeof(p->ifname), "%s", de->d_name);
		p->vlan_id = -1;
		p->kind = "unattributed";
		p->sort_group = 2;

		/* An uplink: a real host interface this network was told to
		 * bridge onto, including a VLAN sub-interface, which is a port
		 * in its own right while its parent NIC is not one. */
		for (i = 0; i < net->interface_count; i++) {
			if (strcmp(net->interfaces[i].ifname, p->ifname) == 0) {
				p->kind = "uplink";
				p->vlan_id = net->interfaces[i].vlan_id;
				p->sort_group = 0;
				break;
			}
		}
		if (p->sort_group == 0) {
			port_count++;
			continue;
		}
		for (i = 0; i < ccount && p->sort_group != 1; i++) {
			struct registry_entry *e = registry_find(cnames[i]);

			if (e == NULL || !e->running)
				continue;
			for (j = 0; j < e->net_count; j++) {
				char veth[32];

				if (strcmp(e->nets[j].name, name) != 0)
					continue;
				container_veth_host_name(e, j, veth, sizeof(veth));
				if (strcmp(veth, p->ifname) != 0)
					continue;
				p->kind = "container";
				p->sort_group = 1;
				snprintf(p->container, sizeof(p->container), "%s", e->name);
				snprintf(p->container_ifname, sizeof(p->container_ifname), "%s",
				         e->nets[j].ifname[0] != '\0' ? e->nets[j].ifname : "eth0");
				{
					struct in_addr a;

					a.s_addr = e->nets[j].ip_be;
					if (inet_ntop(AF_INET, &a, p->ip, sizeof(p->ip)) == NULL)
						p->ip[0] = '\0';
				}
				break;
			}
		}
		port_count++;
	}
	closedir(d);

	/*
	 * Deterministic order so a rendered panel does not reshuffle
	 * between polls. It is an ORDER, not an identity: ports come and go
	 * with containers, so any number this handed out would move, and
	 * ifname is the thing that actually names a port.
	 */
	qsort(ports, (size_t)port_count, sizeof(ports[0]), net_port_cmp);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "network");
	jw_str(&w, name);
	jw_key(&w, "bridge_present");
	jw_bool(&w, 1);
	jw_key(&w, "ports");
	jw_arr_open(&w);
	for (i = 0; i < port_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "ifname");
		jw_str(&w, ports[i].ifname);
		jw_key(&w, "kind");
		jw_str(&w, ports[i].kind);
		jw_key(&w, "vlan_id");
		if (ports[i].vlan_id < 0)
			jw_null(&w);
		else
			jw_int(&w, ports[i].vlan_id);
		jw_key(&w, "container");
		if (ports[i].container[0] != '\0')
			jw_str(&w, ports[i].container);
		else
			jw_null(&w);
		jw_key(&w, "container_ifname");
		if (ports[i].container_ifname[0] != '\0')
			jw_str(&w, ports[i].container_ifname);
		else
			jw_null(&w);
		jw_key(&w, "ip");
		if (ports[i].ip[0] != '\0')
			jw_str(&w, ports[i].ip);
		else
			jw_null(&w);
		jw_key(&w, "link");
		net_port_write_link_state(&w, ports[i].ifname);
		/*
		 * From the PORT's own side, which is the switch's side: rx is
		 * what arrived at the switch from whatever is plugged in, tx is
		 * what the switch sent to it. That is the inverse of what the
		 * container sees on its own interface, and saying which way
		 * round it is here is the difference between a useful number
		 * and a misleading one.
		 *
		 * Raw counters only; the caller computes rates -- the same
		 * convention every other stats endpoint in this daemon uses.
		 */
		jw_key(&w, "rx_bytes");
		jw_int(&w, read_net_stat(ports[i].ifname, "rx_bytes"));
		jw_key(&w, "tx_bytes");
		jw_int(&w, read_net_stat(ports[i].ifname, "tx_bytes"));
		jw_key(&w, "rx_packets");
		jw_int(&w, read_net_stat(ports[i].ifname, "rx_packets"));
		jw_key(&w, "tx_packets");
		jw_int(&w, read_net_stat(ports[i].ifname, "tx_packets"));
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* Defined with the rest of the DHCP handlers further down; needed here
 * so deleting a network can drop its DHCP config in the same breath. */
static void dhcp_apply_and_maybe_restart(void);

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
	/* A DHCP config for a network that no longer exists is a range
	 * nothing can serve, kept alive by nothing but our own forgetting
	 * to drop it. */
	dhcp_forget_network(name);
	dhcp_apply_and_maybe_restart();
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
	case PKG_ERR_TARGET_IMAGE_NOT_FOUND:
		respond_error(fd, 404, "Not Found",
		              "the recipe parsed fine, but the image it names doesn't exist yet -- "
		              "create it first (image create --name=...)");
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

/*
 * GET /v1/software (issue #97) -- what is DECLARED against what is
 * actually INSTALLED, reconciled here rather than in every client.
 *
 * The gap this closes is real and was found on a live box: 11 images
 * existed, 8 had recipes. Two of the three without one were debris
 * nobody had noticed, because nothing anywhere put the declared set
 * next to the installed set. An image with no recipe cannot be rebuilt
 * from source control, which on a platform whose premise is
 * "reproducible from source" is exactly the state worth surfacing.
 *
 * Three states, each meaning something different and each actionable:
 *
 *   declared + installed    normal
 *   installed, not declared drift -- capture a recipe, or it is debris
 *   declared, not installed a recipe nobody has applied
 *
 * Computed server-side deliberately. Two clients would otherwise each
 * implement this join across four endpoints and drift from each other,
 * the way disk role and partition label did before #90 folded that join
 * in. The API-First Mandate makes the endpoint the thing that exists;
 * the dashboard and CLI both render it.
 */
static void software_write_kind(struct json_writer *w, const char *kind,
                                 char declared[][PKG_IMAGE_NAME_MAX], int declared_count,
                                 char installed[][PKG_IMAGE_NAME_MAX], int installed_count)
{
	int i, k;

	for (i = 0; i < installed_count; i++) {
		int is_declared = 0;

		for (k = 0; k < declared_count; k++) {
			if (strcmp(installed[i], declared[k]) == 0) {
				is_declared = 1;
				break;
			}
		}
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, installed[i]);
		jw_key(w, "kind");
		jw_str(w, kind);
		jw_key(w, "declared");
		jw_bool(w, is_declared);
		jw_key(w, "installed");
		jw_bool(w, 1);
		jw_obj_close(w);
	}
	/* Declared but not installed -- the other half of the picture, and
	 * the reason this is a reconciliation rather than a flag on the
	 * image list. */
	for (k = 0; k < declared_count; k++) {
		int is_installed = 0;

		for (i = 0; i < installed_count; i++) {
			if (strcmp(installed[i], declared[k]) == 0) {
				is_installed = 1;
				break;
			}
		}
		if (is_installed)
			continue;
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, declared[k]);
		jw_key(w, "kind");
		jw_str(w, kind);
		jw_key(w, "declared");
		jw_bool(w, 1);
		jw_key(w, "installed");
		jw_bool(w, 0);
		jw_obj_close(w);
	}
}

static void handle_software_list(int fd)
{
	static char declared[PKG_MAX_PACKAGES][PKG_IMAGE_NAME_MAX];
	static char installed[PKG_MAX_PACKAGES][PKG_IMAGE_NAME_MAX];
	struct json_writer w;
	int dn, in_;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "software");
	jw_arr_open(&w);

	dn = image_recipe_list_names(declared, PKG_MAX_PACKAGES);
	in_ = image_list_names(installed, PKG_MAX_PACKAGES);
	software_write_kind(&w, "image", declared, dn, installed, in_);

	dn = pkg_recipe_list_names(declared, PKG_MAX_PACKAGES);
	in_ = pkg_installed_list_names(installed, PKG_MAX_PACKAGES);
	software_write_kind(&w, "package", declared, dn, installed, in_);

	{
		/*
		 * A container's "installed" is a persisted definition. Every
		 * container is persisted since ADR-0181, so this is simply
		 * every container that exists, running or not.
		 */
		char order[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
		int n = containerdef_resolve_order(order);
		int i;

		for (i = 0; i < n && i < PKG_MAX_PACKAGES; i++)
			snprintf(installed[i], PKG_IMAGE_NAME_MAX, "%s", order[i]);
		if (n > PKG_MAX_PACKAGES)
			n = PKG_MAX_PACKAGES;
		dn = container_recipe_list_names(declared, PKG_MAX_PACKAGES);
		software_write_kind(&w, "container", declared, dn, installed, n);
	}

	jw_arr_close(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
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

	/* Issue #68: reject unknown keys at ADD time, not first apply --
	 * the recipe's content is itself a create body, so a typo'd limit
	 * field would otherwise sit latent in the catalog until it
	 * silently dropped on some future apply. */
	{
		struct json_value *content_root = json_parse(content, strlen(content));
		const char *bad = container_body_unknown_key(content_root);

		if (bad != NULL) {
			char msg[128];

			snprintf(msg, sizeof(msg), "unknown field in recipe content: %s", bad);
			json_free(content_root);
			json_free(root);
			respond_error(fd, 400, "Bad Request", msg);
			return;
		}
		json_free(content_root);
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
	 * file from thinC's own record store, the dns_server_sync_all()
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
	char effective[600];

	jw_init(&w);
	/*
	 * ldap_config_write_json() opens the object and writes the stored
	 * fields; effective_client_uri is appended into that same object
	 * before it closes, because it is not stored anywhere -- it is what
	 * ldap_client containers are actually handed right now, after
	 * derivation from registered servers and after health filtering
	 * (issues #66, #81, #84).
	 *
	 * Worth surfacing precisely because #84 was invisible for as long
	 * as it existed: with an explicit client_uri configured, health
	 * filtering was silently inert, and nothing anywhere reported what
	 * a client would really receive. Configuration and effect are two
	 * different questions and now have two different fields.
	 */
	ldap_config_write_json_open(&w);
	jw_key(&w, "effective_client_uri");
	if (ldap_effective_client_uri(effective, sizeof(effective)))
		jw_str(&w, effective);
	else
		jw_null(&w);
	jw_obj_close(&w);
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

	/* Issue #66: two independent groups of fields, each optional as a
	 * group -- the allocation floors (both-or-neither, unchanged
	 * semantics) and the client-login fields (each individually
	 * optional; absent = unchanged, "" = clear). A body touching only
	 * one group leaves the other exactly as it was. */
	{
		const struct json_value *juid = json_object_get(root, "start_uid");
		const struct json_value *jgid = json_object_get(root, "start_gid");
		const char *client_uri = json_as_string(json_object_get(root, "client_uri"));
		const char *base_dn = json_as_string(json_object_get(root, "base_dn"));
		const char *bind_dn = json_as_string(json_object_get(root, "bind_dn"));
		const char *bind_password = json_as_string(json_object_get(root, "bind_password"));

		if (juid != NULL || jgid != NULL) {
			start_uid = (int)json_as_number(juid);
			start_gid = (int)json_as_number(jgid);
			rerr = ldap_config_set(start_uid, start_gid);
			if (rerr != LDAP_RECORD_OK) {
				json_free(root);
				respond_error(fd, 400, "Bad Request",
				              "start_uid/start_gid must both be > 0");
				return;
			}
		}
		if (client_uri != NULL || base_dn != NULL || bind_dn != NULL ||
		    bind_password != NULL) {
			rerr = ldap_config_set_client(client_uri, base_dn, bind_dn, bind_password);
			if (rerr != LDAP_RECORD_OK) {
				json_free(root);
				respond_error(fd, 500, "Internal Server Error",
				              "failed to persist LDAP config");
				return;
			}
		}
	}
	json_free(root);

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
	const char *common_name = "thinC Root CA";
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
	const char *common_name = "thinC Intermediate CA";
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

		snprintf(cert_dir_buf, sizeof(cert_dir_buf), "/etc/thinc-tls");
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

	snprintf(root_cn, sizeof(root_cn), "thinC Root CA - %s", siteconfig_domain_suffix());
	snprintf(intermediate_cn, sizeof(intermediate_cn), "thinC Intermediate CA - %s",
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
	/*
	 * Issue #17: "none" reads as "no bootstrap ever happened" but
	 * actually only means "this daemon has never handled a
	 * toolchain_url POST /pkg/bootstrap" -- the plain/toolchain_path
	 * modes (the ones a normal `pkg install --name=tcc` or a real
	 * gcc/dev build actually exercises) are synchronous and never
	 * touch this state at all. Rather than a deeper fix (tracking
	 * every bootstrap mode in one unified state, a real behavior
	 * change with its own risk), this scope note ships instead --
	 * always present, so `state:"none"` stops reading as a gap or an
	 * error on a box that has genuinely bootstrapped successfully via
	 * a different mode.
	 */
	jw_key(w, "scope");
	jw_str(w, "tracks only the async toolchain_url fetch mode of POST /pkg/bootstrap; "
	           "\"none\" does not mean no bootstrap has ever happened on this daemon");
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
 * band" assumes an SSH server a real, freshly-installed thinC box
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
static void handle_pkg_sync_post(int fd, const char *body, size_t body_len)
{
	pid_t pid;
	int pidfd;
	enum pkg_error perr;
	struct json_writer w;

	/*
	 * Issue #59: an optional, explicit "refetch": "name@version" --
	 * this sync may REPLACE that one recipe version instead of skipping
	 * it as a duplicate. Everything else keeps the immutability
	 * ADR-0107's resolution rules depend on; without this the only way
	 * to correct a recipe under development was to burn a new version
	 * number per iteration, which is how the catalogue collected five
	 * dead kernel pins and four dead gcc pins from one investigation.
	 */
	pkg_sync_set_refetch(NULL, NULL);
	if (body != NULL && body_len > 0) {
		struct json_value *root = json_parse(body, body_len);
		const char *refetch = root != NULL ? json_as_string(json_object_get(root, "refetch")) : NULL;

		if (refetch != NULL) {
			const char *at = strchr(refetch, '@');
			char name[PKG_NAME_MAX];

			if (at == NULL || at == refetch || at[1] == '\0' ||
			    (size_t)(at - refetch) >= sizeof(name)) {
				json_free(root);
				respond_error(fd, 400, "Bad Request",
				              "refetch must be \"name@version\" -- immutability is per version, so "
				              "the escape hatch is too");
				return;
			}
			snprintf(name, sizeof(name), "%.*s", (int)(at - refetch), refetch);
			pkg_sync_set_refetch(name, at + 1);
		}
		json_free(root);
	}

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
 * GET /v1/pkg/policies, PUT/DELETE /v1/pkg/policies/{name} (issue #64).
 *
 * Which version an omitted version resolves to, per package. `highest`
 * (dpkg-style, the rolling-release default) is what every package has
 * unless an operator says otherwise, so "no policy set" and "the
 * default policy" are the same state rather than two to keep in step.
 */
static void handle_pkg_policies_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "policies");
	pkgpolicy_write_json(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pkg_policy_put(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	enum pkg_policy_kind kind;
	const char *policy_str, *version;
	struct json_writer w;

	if (!simple_name_is_valid(name, PKG_NAME_MAX)) {
		respond_error(fd, 400, "Bad Request", "invalid package name");
		return;
	}
	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	policy_str = json_as_string(json_object_get(root, "policy"));
	version = json_as_string(json_object_get(root, "version"));
	if (pkgpolicy_kind_parse(policy_str, &kind) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "policy must be \"highest\", \"newest\" or \"pinned\"");
		return;
	}
	if (pkgpolicy_set(name, kind, version) != 0) {
		json_free(root);
		/* The only reachable failure worth its own words: a pin with no
		 * version would claim to hold something while meaning
		 * "highest" -- the exact drift a pin exists to stop. */
		respond_error(fd, 400, "Bad Request",
		              "\"pinned\" needs a version to pin to -- a pin without one is not a hold");
		return;
	}
	json_free(root);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "policies");
	pkgpolicy_write_json(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pkg_policy_delete(int fd, const char *name)
{
	if (!simple_name_is_valid(name, PKG_NAME_MAX)) {
		respond_error(fd, 400, "Bad Request", "invalid package name");
		return;
	}
	/* Clearing a policy that was never set is a 204, not a 404: the
	 * caller's intent ("this package is on the default") is satisfied
	 * either way, and reporting a failure for an already-correct state
	 * makes scripts handle a distinction that does not matter. */
	if (pkgpolicy_clear(name) != 0) {
		respond_error(fd, 500, "Internal Server Error", "could not persist the policy change");
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * GET /v1/pkg/build-logs (issue #57) -- every persisted build log,
 * newest first. The complete output of each build, not the ~4KB tail
 * the log store keeps and not the live-only stream: a finished build's
 * full output used to survive nowhere at all.
 */
static void handle_pkg_build_logs_list(int fd)
{
	struct pkg_build_log_entry entries[128];
	struct json_writer w;
	int count, i;

	count = pkg_build_log_list(entries, (int)(sizeof(entries) / sizeof(entries[0])));

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "logs");
	jw_arr_open(&w);
	for (i = 0; i < count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "file");
		jw_str(&w, entries[i].file);
		jw_key(&w, "size_bytes");
		jw_int(&w, entries[i].size_bytes);
		jw_key(&w, "modified_at");
		jw_int(&w, entries[i].modified_at);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * GET /v1/pkg/build-logs/{file} -- one log, as plain text.
 *
 * Tail-first when it does not fit the response cap: someone reading a
 * 40 MB build log wants the end, where the failure is. The full size is
 * reported in a header so a truncated read is never mistaken for the
 * whole thing -- a silently truncated log is exactly the failure mode
 * this endpoint exists to remove.
 */
#define PKG_BUILD_LOG_RESPONSE_MAX (2 * 1024 * 1024)

static void handle_pkg_build_log_get(int fd, const char *file)
{
	char *buf = malloc(PKG_BUILD_LOG_RESPONSE_MAX);
	long long total = 0, n;
	char header[512];

	if (buf == NULL) {
		respond_error(fd, 500, "Internal Server Error", "out of memory");
		return;
	}
	n = pkg_build_log_read(file, buf, PKG_BUILD_LOG_RESPONSE_MAX, &total);
	if (n < 0) {
		free(buf);
		respond_error(fd, 404, "Not Found", "no such build log");
		return;
	}
	/* Whether this is the whole log is stated IN the body's first line
	 * rather than only in a header: this endpoint exists because a
	 * silently truncated log cost real debugging time, and a reader
	 * piping it to a file would never see a header. */
	if (total > n) {
		snprintf(header, sizeof(header),
		         "[thinC: showing the last %lld of %lld bytes -- the tail is where the failure "
		         "is; fetch the file itself for the whole log]\n",
		         n, total);
	} else {
		header[0] = '\0';
	}
	{
		size_t hlen = strlen(header);
		char *body = malloc(hlen + (size_t)n);

		if (body == NULL) {
			free(buf);
			respond_error(fd, 500, "Internal Server Error", "out of memory");
			return;
		}
		memcpy(body, header, hlen);
		memcpy(body + hlen, buf, (size_t)n);
		http_set_blocking(fd);
		http_write_response(fd, 200, "OK", "text/plain; charset=utf-8", body, hlen + (size_t)n);
		free(body);
	}
	free(buf);
}

/*
 * ADR-0157 Phase 3: GET/PUT /v1/system/pkg-build-config -- mirrors
 * handle_pkg_cache_config_get/put()'s own shape exactly, one field,
 * same "PUT re-reads via GET on success" convention. Controls how
 * many pkg install/hostbuild chains may genuinely run at once, within
 * [1, PKG_MAX_CONCURRENT_JOBS] (a real compile-time array bound, not
 * just a soft suggestion) -- see pkg_build_set_max_jobs()'s own doc
 * comment for what happens to already-in-flight chains when this is
 * lowered.
 */
static void handle_pkg_build_config_get(int fd)
{
	struct json_writer w;
	const char *cpu_max = pkg_build_get_cpu_max();

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "max_concurrent_jobs");
	jw_int(&w, pkg_build_get_max_jobs());
	jw_key(&w, "memory_max");
	jw_int(&w, pkg_build_get_memory_max());
	jw_key(&w, "cpu_max");
	if (cpu_max != NULL)
		jw_str(&w, cpu_max);
	else
		jw_null(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* Partial update -- only the fields given are changed, same convention
 * daemon-config/hostauth-config already use for a multi-field config
 * resource (ADR-0165 adds memory_max/cpu_max alongside the original
 * max_concurrent_jobs, which stays required-when-present for backward
 * behavior but is no longer the only field a caller can set). */
static void handle_pkg_build_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *mj, *jmem, *jcpu;
	enum pkg_error perr;

	if (body_len == 0) {
		respond_error(fd, 400, "Bad Request",
		              "at least one of max_concurrent_jobs/memory_max/cpu_max is required");
		return;
	}
	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	mj = json_object_get(root, "max_concurrent_jobs");
	jmem = json_object_get(root, "memory_max");
	jcpu = json_object_get(root, "cpu_max");
	if (mj == NULL && jmem == NULL && jcpu == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "at least one of max_concurrent_jobs/memory_max/cpu_max is required");
		return;
	}
	if (mj != NULL) {
		perr = pkg_build_set_max_jobs((int)json_as_number(mj));
		if (perr == PKG_ERR_INVALID_NAME) {
			char msg[80];

			snprintf(msg, sizeof(msg), "max_concurrent_jobs must be 1-%d",
			         PKG_MAX_CONCURRENT_JOBS);
			json_free(root);
			respond_error(fd, 400, "Bad Request", msg);
			return;
		}
		if (perr != PKG_OK) {
			json_free(root);
			respond_pkg_error(fd, perr);
			return;
		}
	}
	if (jmem != NULL) {
		perr = pkg_build_set_memory_max(jmem->type == JSON_NULL ? 0 : (long long)json_as_number(jmem));
		if (perr == PKG_ERR_INVALID_NAME) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "memory_max must be >= 0");
			return;
		}
		if (perr != PKG_OK) {
			json_free(root);
			respond_pkg_error(fd, perr);
			return;
		}
	}
	if (jcpu != NULL) {
		perr = pkg_build_set_cpu_max(jcpu->type == JSON_NULL ? NULL : json_as_string(jcpu));
		if (perr == PKG_ERR_INVALID_NAME) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "cpu_max is too long");
			return;
		}
		if (perr != PKG_OK) {
			json_free(root);
			respond_pkg_error(fd, perr);
			return;
		}
	}
	json_free(root);
	handle_pkg_build_config_get(fd);
}



/*
 * GET/PUT /v1/system/boot-console (issue #24) -- what the installed
 * system's own boot line says about consoles.
 *
 * An installed host boots through systemd-boot, and every loader entry
 * carried a hardcoded `console=tty0 console=ttyS0`. Fine as a default,
 * bad as a permanent one: real hardware needs a serial console at a
 * particular baud, or a framebuffer argument to produce any output at
 * all, or exactly the opposite when the framebuffer is the problem.
 * None of it was reachable without reinstalling.
 *
 * A change is applied to the loader entries already on the ESP, not
 * only to future ones -- an operator who cannot see the console is not
 * in a position to wait for the next A/B update to fix it.
 */

/* Rewrites one loader entry's console parameters in place, preserving
 * everything from `root=` onward (which is what actually decides
 * whether the machine boots, and is never this endpoint's business).
 * Returns 1 if the file was rewritten, 0 if it was left alone. */
static int bootconsole_rewrite_entry(const char *path, const char *console_opts)
{
	char buf[4096];
	char out[4096];
	int fd;
	ssize_t n;
	char *opt_line, *root_at, *line_end;
	size_t prefix_len, out_len;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';

	opt_line = strstr(buf, "\noptions ");
	if (opt_line == NULL && strncmp(buf, "options ", 8) == 0)
		opt_line = buf;
	else if (opt_line != NULL)
		opt_line++; /* past the newline */
	if (opt_line == NULL)
		return 0;
	line_end = strchr(opt_line, '\n');
	root_at = strstr(opt_line, "root=");
	/* An entry whose options line has no root= is not one this daemon
	 * wrote; leaving it untouched is the only safe reading. */
	if (root_at == NULL || (line_end != NULL && root_at > line_end))
		return 0;

	prefix_len = (size_t)(opt_line - buf);
	if (prefix_len >= sizeof(out))
		return 0;
	memcpy(out, buf, prefix_len);
	out_len = prefix_len;
	out_len += (size_t)snprintf(out + out_len, sizeof(out) - out_len, "options %s%s%s",
	                             console_opts, console_opts[0] != '\0' ? " " : "", root_at);
	if (out_len >= sizeof(out))
		return 0;

	fd = open(path, O_WRONLY | O_TRUNC);
	if (fd < 0)
		return 0;
	if (write(fd, out, out_len) != (ssize_t)out_len || fsync(fd) != 0) {
		close(fd);
		return 0;
	}
	close(fd);
	return 1;
}

static int bootconsole_apply_to_esp(void)
{
	char console_opts[512];
	DIR *d;
	struct dirent *de;
	int rewritten = 0;

	bootconsole_render(console_opts, sizeof(console_opts));
	d = opendir(g_esp_entries_dir);
	if (d == NULL)
		return 0; /* no ESP here (a dev sandbox) -- nothing to apply to */
	while ((de = readdir(d)) != NULL) {
		char path[PATH_MAX];

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", g_esp_entries_dir, de->d_name);
		rewritten += bootconsole_rewrite_entry(path, console_opts);
	}
	closedir(d);
	return rewritten;
}


/*
 * GET /v1/system/stalls (issue #100) -- times the control plane stopped
 * going round its own loop, recorded by the watchdog process because
 * the loop cannot report its own silence. Each record carries how long
 * the loop was quiet, the kernel function it was sleeping in
 * (/proc/<pid>/wchan, which is the single most useful fact about a
 * wedge), its process state, and the request it was serving.
 */
static void handle_stalls_get(int fd, const struct http_request *req)
{
	struct json_writer w;
	int limit = 50;
	const char *q = strchr(req->path, '?');

	if (q != NULL && strstr(q, "limit=") != NULL)
		limit = atoi(strstr(q, "limit=") + 6);
	if (limit <= 0)
		limit = 50;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "stalls");
	stallwatch_write_json(&w, limit);
	jw_key(&w, "threshold_seconds");
	jw_int(&w, 5);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* The options line each loader entry currently carries, so the response
 * shows what the machine will really boot with rather than only what
 * was asked for -- the two differ on any box whose ESP is not writable
 * from here, and that difference is the thing worth seeing. */
static void bootconsole_write_entries_json(struct json_writer *w)
{
	DIR *d = opendir(g_esp_entries_dir);
	struct dirent *de;

	jw_arr_open(w);
	if (d == NULL) {
		jw_arr_close(w);
		return;
	}
	while ((de = readdir(d)) != NULL) {
		char path[PATH_MAX];
		char buf[4096];
		int fd;
		ssize_t n;
		char *opt_line, *line_end;

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", g_esp_entries_dir, de->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[n] = '\0';
		opt_line = strstr(buf, "\noptions ");
		if (opt_line != NULL)
			opt_line += 9;
		else if (strncmp(buf, "options ", 8) == 0)
			opt_line = buf + 8;
		if (opt_line == NULL)
			continue;
		line_end = strchr(opt_line, '\n');
		if (line_end != NULL)
			*line_end = '\0';

		jw_obj_open(w);
		jw_key(w, "entry");
		jw_str(w, de->d_name);
		jw_key(w, "options");
		jw_str(w, opt_line);
		jw_obj_close(w);
	}
	closedir(d);
	jw_arr_close(w);
}

/* ---------- Issue #65: kernel release-channel policy ---------- */

/*
 * kernel.org's own machine-readable index of what each moniker
 * currently means. Fetched, never mirrored: a copy of this in the repo
 * would be a second source of truth for a fact that changes weekly
 * without anyone here noticing it had.
 */
#define KERNEL_RELEASES_URL "https://www.kernel.org/releases.json"

static int g_kernel_releases_fetching;
static char g_kernel_releases_error[256];

static void register_kernel_releases_pidfd(pid_t pid, int pidfd)
{
	struct conn *cc;
	struct thinc_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (kernel releases reactor conn)");
		abort();
	}
	cc->kind = CONN_KERNEL_RELEASES_FETCH;
	cc->fd = pidfd;
	cc->pkg_fetch_pid = pid;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD kernel releases pidfd");
		abort();
	}
}

/*
 * Fetched by the same forked-curl-watched-by-pidfd shape every other
 * outbound fetch in this daemon uses -- never a blocking curl, which
 * would stall the whole control plane behind a network timeout on a
 * box whose resolvers are not configured (ADR-0076, and exactly the
 * class of wedge ADR-0189's watchdog exists to catch).
 */
static int kernel_releases_fetch_start(char *err_msg, size_t err_msg_size)
{
	static char out_path[PATH_MAX];
	char *argv[10];
	pid_t pid;
	int pidfd;

	if (g_kernel_releases_fetching) {
		snprintf(err_msg, err_msg_size, "a kernel release refresh is already running");
		return -1;
	}
	snprintf(out_path, sizeof(out_path), "%s", KERNEL_RELEASES_PATH);

	argv[0] = (char *)PKG_CURL_BIN;
	argv[1] = "-fsSL";
	argv[2] = "--max-time";
	argv[3] = "30";
	argv[4] = "-o";
	argv[5] = out_path;
	argv[6] = (char *)KERNEL_RELEASES_URL;
	argv[7] = NULL;

	pid = fork();
	if (pid < 0) {
		snprintf(err_msg, err_msg_size, "fork failed: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execve(PKG_CURL_BIN, argv, environ);
		perror("child: execve curl (kernel releases fetch)");
		_exit(127);
	}
	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		snprintf(err_msg, err_msg_size, "pidfd_open failed: %s", strerror(errno));
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return -1;
	}
	register_kernel_releases_pidfd(pid, pidfd);
	g_kernel_releases_fetching = 1;
	g_kernel_releases_error[0] = '\0';
	return 0;
}

static void handle_kernel_releases_fetch_event(struct conn *cc)
{
	int status;

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	g_kernel_releases_fetching = 0;
	if (waitpid(cc->pkg_fetch_pid, &status, 0) != cc->pkg_fetch_pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		/*
		 * A box with no resolvers configured cannot reach kernel.org
		 * at all, and that is by far the likeliest cause here -- say
		 * so, rather than leaving an operator to guess at a bare exit
		 * code (CLAUDE.md, ADR-0076).
		 */
		snprintf(g_kernel_releases_error, sizeof(g_kernel_releases_error),
		         "could not fetch %s (curl exit status %d) -- a host with no upstream "
		         "resolvers set cannot resolve it at all, see PUT /v1/system/resolv",
		         KERNEL_RELEASES_URL, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		fprintf(stderr, "kernel releases: %s\n", g_kernel_releases_error);
		close(cc->fd);
		free(cc);
		return;
	}
	close(cc->fd);
	free(cc);
	if (kernelpolicy_ingest_releases(KERNEL_RELEASES_PATH, (long)time(NULL)) != 0) {
		snprintf(g_kernel_releases_error, sizeof(g_kernel_releases_error),
		         "fetched %s but it named no usable release -- the previous answer is kept",
		         KERNEL_RELEASES_URL);
		fprintf(stderr, "kernel releases: %s\n", g_kernel_releases_error);
		return;
	}
	fprintf(stderr, "kernel releases: refreshed from %s\n", KERNEL_RELEASES_URL);
}

/*
 * The running kernel's own version, which is what every "how far
 * behind" question is measured against. Read from uname() rather than
 * from the recipe pin: the pin says what was last built, the running
 * kernel says what actually booted, and after a failed A/B update
 * those are not the same fact.
 */
static void kernel_running_version(char *out, size_t out_size)
{
	struct utsname uts;

	if (uname(&uts) == 0)
		snprintf(out, out_size, "%s", uts.release);
	else
		snprintf(out, out_size, "%s", "");
}

static void kernel_policy_write_json(struct json_writer *w)
{
	char running[128];
	char running_series[KERNEL_SERIES_MAX] = "";
	struct kernel_resolution res;
	enum kernel_channel channel = kernelpolicy_channel();

	kernel_running_version(running, sizeof(running));
	(void)kernel_version_series(running, running_series, sizeof(running_series));
	kernelpolicy_resolve(channel, running_series, &res);

	jw_obj_open(w);
	jw_key(w, "channel");
	jw_str(w, kernel_channel_name(channel));
	jw_key(w, "running_version");
	jw_str(w, running);
	jw_key(w, "running_series");
	jw_str(w, running_series);
	jw_key(w, "resolved_version");
	if (res.version[0] != '\0')
		jw_str(w, res.version);
	else
		jw_null(w);
	jw_key(w, "resolved_source_url");
	if (res.source_url[0] != '\0')
		jw_str(w, res.source_url);
	else
		jw_null(w);
	/*
	 * Whether an update is available is a question with three answers,
	 * not two: yes, no, and "nothing has been resolved yet" -- which is
	 * why this is null rather than false before a first refresh. A
	 * false there would read as "you are up to date".
	 */
	jw_key(w, "behind");
	if (res.version[0] == '\0' || running[0] == '\0')
		jw_null(w);
	else
		jw_bool(w, strncmp(running, res.version, strlen(res.version)) != 0 ||
		            (running[strlen(res.version)] != '\0' &&
		             running[strlen(res.version)] != '-'));
	jw_key(w, "running_series_maintained");
	if (!res.known)
		jw_null(w);
	else
		jw_bool(w, res.series_maintained);
	jw_key(w, "newest_longterm");
	if (res.newest_longterm[0] != '\0')
		jw_str(w, res.newest_longterm);
	else
		jw_null(w);
	jw_key(w, "newest_longterm_series");
	if (res.newest_longterm_series[0] != '\0')
		jw_str(w, res.newest_longterm_series);
	else
		jw_null(w);
	jw_key(w, "releases_fetched_at");
	if (kernelpolicy_fetched_at() == 0)
		jw_null(w);
	else
		jw_int(w, kernelpolicy_fetched_at());
	jw_key(w, "refreshing");
	jw_bool(w, g_kernel_releases_fetching);
	jw_key(w, "last_refresh_error");
	if (g_kernel_releases_error[0] != '\0')
		jw_str(w, g_kernel_releases_error);
	else
		jw_null(w);
	jw_key(w, "source");
	jw_str(w, KERNEL_RELEASES_URL);
	jw_obj_close(w);
}

/* ---------- DHCP (ADR-0197) ---------- */

/*
 * Whether an address lies inside a network's own subnet, and whether it
 * lies inside an inclusive range. Both are two lines and are used only
 * by the DHCP validation below -- worth naming rather than open-coding
 * a mask twice, not worth a new module.
 */
static int dhcp_ip_in_subnet(const struct network_def *net, uint32_t ip_be)
{
	uint32_t mask = net->prefix_len == 0 ? 0 : htonl(0xffffffffu << (32 - net->prefix_len));

	return (ip_be & mask) == (net->base_be & mask);
}

static int dhcp_ip_in_range(uint32_t ip_be, uint32_t start_be, uint32_t end_be)
{
	uint32_t ip = ntohl(ip_be);

	return ip >= ntohl(start_be) && ip <= ntohl(end_be);
}

static void arm_rolling_restart_timer(const char *name, int delay_seconds);
static int rolling_jitter_seconds(int window);

/*
 * Pushes the rendered files into every running DNS server and, if the
 * ranges themselves changed, rolls those servers so the new ones are
 * actually in force.
 *
 * The restart goes through the SAME jittered timer a rolling image
 * update uses, rather than a second restart path of its own: with two
 * servers registered, that jitter is what keeps them from going down
 * together, which is the whole reason there are two.
 */
#define DHCP_RESTART_JITTER_SECONDS 20

static void dhcp_apply_and_maybe_restart(void)
{
	char names[DNS_SERVER_MAX][DNS_SERVER_NAME_MAX];
	int count = 0;
	int i;

	/* Only the servers whose OWN conf changed. A range on one network
	 * is nothing to a server on another, and restarting it anyway
	 * would make an unrelated edit cost a DNS outage. */
	dhcp_sync_all(names, DNS_SERVER_MAX, &count);
	if (count == 0)
		return;
	for (i = 0; i < count; i++) {
		struct registry_entry *e = registry_find(names[i]);

		if (e == NULL || !e->running)
			continue;
		/*
		 * Restarted whatever its restart policy says. ADR-0181's rule
		 * -- restart:"no" is never auto-recreated -- is about
		 * recreation nobody asked for, a rolling image update
		 * happening on its own schedule. This restart is the direct,
		 * immediate consequence of an operator changing a setting on
		 * this very container, and declining to carry it out would
		 * leave them with a range that reports itself in force and is
		 * not. Checked rather than assumed: a container has a
		 * persisted definition regardless of its restart policy, so
		 * the timer really does replay it.
		 */
		arm_rolling_restart_timer(names[i], rolling_jitter_seconds(DHCP_RESTART_JITTER_SECONDS));
	}
	logstore_write("thincd", "info",
	               "dhcp: ranges changed -- rolling %d DNS/DHCP server(s) so they take effect",
	               count);
}

static void handle_dhcp_servers_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "servers");
	dhcp_servers_write_json(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_dhcp_server_register(int fd, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const char *container;
	struct registry_entry *e;
	enum dhcp_error err;
	struct json_writer w;

	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	container = json_as_string(json_object_get(root, "container"));
	if (container == NULL || container[0] == '\0') {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "container is required");
		return;
	}
	e = registry_find(container);
	if (e == NULL) {
		json_free(root);
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}
	err = dhcp_server_register(container);
	json_free(root);
	if (err == DHCP_ERR_DUPLICATE) {
		respond_error(fd, 409, "Conflict", "that container is already a DHCP server");
		return;
	}
	if (err == DHCP_ERR_FULL) {
		respond_error(fd, 507, "Insufficient Storage", "too many DHCP servers");
		return;
	}
	if (err != DHCP_OK) {
		respond_error(fd, 500, "Internal Server Error", "failed to persist the registration");
		return;
	}
	/* It has no DHCP files yet and every range it joins re-splits, so
	 * render and roll whoever that affected -- which is exactly the set
	 * whose own conf changed. */
	dhcp_apply_and_maybe_restart();
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "servers");
	dhcp_servers_write_json(&w);
	jw_obj_close(&w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_dhcp_server_unregister(int fd, const char *container)
{
	enum dhcp_error err = dhcp_server_unregister(container);

	if (err == DHCP_ERR_NOT_FOUND) {
		respond_error(fd, 404, "Not Found", "that container is not a DHCP server");
		return;
	}
	if (err != DHCP_OK) {
		respond_error(fd, 500, "Internal Server Error", "failed to persist the removal");
		return;
	}
	dhcp_apply_and_maybe_restart();
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_dhcp_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	dhcp_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_dhcp_leases_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "leases");
	dhcp_leases_write_json(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_dhcp_static_post(int fd, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	struct dhcp_static entry;
	const char *mac;
	const char *ip;
	const char *host;
	struct in_addr addr;
	enum dhcp_error err;
	struct json_writer w;

	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	memset(&entry, 0, sizeof(entry));
	mac = json_as_string(json_object_get(root, "mac"));
	ip = json_as_string(json_object_get(root, "ip"));
	host = json_as_string(json_object_get(root, "hostname"));
	if (mac != NULL)
		snprintf(entry.mac, sizeof(entry.mac), "%s", mac);
	if (host != NULL)
		snprintf(entry.hostname, sizeof(entry.hostname), "%s", host);
	if (ip != NULL && inet_pton(AF_INET, ip, &addr) == 1)
		entry.ip_be = addr.s_addr;
	json_free(root);

	err = dhcp_static_add(&entry);
	if (err == DHCP_ERR_INVALID) {
		respond_error(fd, 400, "Bad Request",
		               "mac must be lower-case aa:bb:cc:dd:ee:ff, ip must be a valid IPv4 "
		               "address, and hostname (if given) must be a plain DNS label");
		return;
	}
	if (err == DHCP_ERR_DUPLICATE) {
		respond_error(fd, 409, "Conflict",
		               "that MAC already has a reservation, or that address is already "
		               "reserved for a different MAC");
		return;
	}
	if (err == DHCP_ERR_FULL) {
		respond_error(fd, 507, "Insufficient Storage", "too many static reservations");
		return;
	}
	if (err != DHCP_OK) {
		respond_error(fd, 500, "Internal Server Error", "failed to persist the reservation");
		return;
	}
	/* Static entries are live: the servers re-read them on SIGHUP, so
	 * no restart is involved and none is triggered. */
	dhcp_apply_and_maybe_restart();
	jw_init(&w);
	dhcp_write_json(&w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_dhcp_static_delete(int fd, const char *mac)
{
	enum dhcp_error err = dhcp_static_delete(mac);

	if (err == DHCP_ERR_NOT_FOUND) {
		respond_error(fd, 404, "Not Found", "no reservation for that MAC");
		return;
	}
	if (err != DHCP_OK) {
		respond_error(fd, 500, "Internal Server Error", "failed to persist the removal");
		return;
	}
	dhcp_apply_and_maybe_restart();
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_network_dhcp_get(int fd, const char *network)
{
	const struct dhcp_network *cfg;
	struct dhcp_network empty;
	struct json_writer w;

	if (network_find(network) == NULL) {
		respond_error(fd, 404, "Not Found", "no such network");
		return;
	}
	cfg = dhcp_network_find(network);
	if (cfg == NULL) {
		/*
		 * A network with no DHCP config is not an error and not an
		 * empty body: it is a network with DHCP off, which is a real
		 * answer and the one every network starts with.
		 */
		memset(&empty, 0, sizeof(empty));
		snprintf(empty.network, sizeof(empty.network), "%s", network);
		/* The same default a PUT would start from, so what you read
		 * here and what you would get by enabling it are the same
		 * number rather than a 0 that means "unset" in one place. */
		empty.lease_seconds = DHCP_DEFAULT_LEASE_SECONDS;
		cfg = &empty;
	}
	jw_init(&w);
	dhcp_network_write_json(cfg, 1, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_network_dhcp_put(int fd, const char *network, const char *body, size_t body_len)
{
	struct json_value *root;
	struct dhcp_network cfg;
	const struct dhcp_network *existing;
	const struct json_value *jen;
	const char *s;
	struct in_addr addr;
	enum dhcp_error err;
	struct json_writer w;
	struct network_def *net = network_find(network);

	if (net == NULL) {
		respond_error(fd, 404, "Not Found", "no such network");
		return;
	}
	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	/* Partial update over whatever is configured now, the same
	 * convention every other config PUT here uses. */
	existing = dhcp_network_find(network);
	if (existing != NULL) {
		cfg = *existing;
	} else {
		memset(&cfg, 0, sizeof(cfg));
		cfg.lease_seconds = DHCP_DEFAULT_LEASE_SECONDS;
	}
	snprintf(cfg.network, sizeof(cfg.network), "%s", network);
	jen = json_object_get(root, "enabled");
	if (jen != NULL && jen->type == JSON_BOOL)
		cfg.enabled = jen->u.boolean;
	s = json_as_string(json_object_get(root, "range_start"));
	if (s != NULL && inet_pton(AF_INET, s, &addr) == 1)
		cfg.range_start_be = addr.s_addr;
	s = json_as_string(json_object_get(root, "range_end"));
	if (s != NULL && inet_pton(AF_INET, s, &addr) == 1)
		cfg.range_end_be = addr.s_addr;
	s = json_as_string(json_object_get(root, "router"));
	if (s != NULL && inet_pton(AF_INET, s, &addr) == 1)
		cfg.router_be = addr.s_addr;
	if (json_object_get(root, "lease_seconds") != NULL)
		cfg.lease_seconds = (int)json_as_number(json_object_get(root, "lease_seconds"));
	{
		const struct json_value *jsrv = json_object_get(root, "servers");

		if (jsrv != NULL && jsrv->type == JSON_ARRAY) {
			size_t k;

			cfg.server_count = 0;
			memset(cfg.servers, 0, sizeof(cfg.servers));
			for (k = 0; k < jsrv->u.array.count && cfg.server_count < DHCP_MAX_RANGE_SERVERS; k++) {
				const char *nm = json_as_string(jsrv->u.array.items[k]);

				if (nm == NULL || nm[0] == '\0')
					continue;
				snprintf(cfg.servers[cfg.server_count], DHCP_SERVER_NAME_MAX, "%s", nm);
				cfg.server_count++;
			}
		}
	}
	json_free(root);

	if (cfg.enabled) {
		char errbuf[256];

		if (!dhcp_ip_in_subnet(net, cfg.range_start_be) ||
		    !dhcp_ip_in_subnet(net, cfg.range_end_be)) {
			/*
			 * A range outside its own subnet is a range no client on
			 * that wire can ever be given. Checked here rather than
			 * left to dnsmasq, which would take it, start, and answer
			 * nothing.
			 */
			respond_error(fd, 400, "Bad Request",
			               "the range must lie inside this network's own subnet");
			return;
		}
		if (net->has_address &&
		    dhcp_ip_in_range(net->address_be, cfg.range_start_be, cfg.range_end_be)) {
			respond_error(fd, 400, "Bad Request",
			               "the range covers this network's own address -- handing the host's "
			               "own IP to a client is a conflict, not a lease");
			return;
		}
		/*
		 * DHCP is served by EVERY registered DNS server, so there has
		 * to be at least one -- otherwise this stores a range nothing
		 * will ever answer for.
		 */
		{
			int servers = cfg.server_count;
			uint32_t first = ntohl(cfg.range_start_be);
			uint32_t last = ntohl(cfg.range_end_be);
			int si;

			if (servers == 0) {
				respond_error(fd, 400, "Bad Request",
				               "name at least one server to serve this range -- register one "
				               "with POST /v1/dhcp/servers first");
				return;
			}
			/* Every named server must be registered and on this
			 * network: one that is neither would be a slice of
			 * addresses handed to something that cannot answer for
			 * them, which looks like coverage and is not. */
			for (si = 0; si < servers; si++) {
				struct registry_entry *se;
				int on_net = 0;
				int j;

				if (!dhcp_server_is_registered(cfg.servers[si])) {
					snprintf(errbuf, sizeof(errbuf),
					         "\"%s\" is not a registered DHCP server", cfg.servers[si]);
					respond_error(fd, 400, "Bad Request", errbuf);
					return;
				}
				se = registry_find(cfg.servers[si]);
				for (j = 0; se != NULL && j < se->net_count; j++)
					if (strcmp(se->nets[j].name, network) == 0)
						on_net = 1;
				if (!on_net) {
					snprintf(errbuf, sizeof(errbuf),
					         "\"%s\" is not attached to network \"%s\" -- it has no interface "
					         "in this subnet, so it could not answer here",
					         cfg.servers[si], network);
					respond_error(fd, 400, "Bad Request", errbuf);
					return;
				}
			}
			/*
			 * The range is split into one disjoint slice per server
			 * (dnsmasq has no failover protocol, so disjoint pools are
			 * what makes two servers safe). A range with fewer
			 * addresses than servers cannot be split, and rounding one
			 * server down to nothing would silently make it not a
			 * server at all.
			 */
			if (last - first + 1 < (uint32_t)servers) {
				snprintf(errbuf, sizeof(errbuf),
				         "the range holds %u address(es) but %d servers must split it -- each "
				         "needs at least one, since they have no shared lease database and "
				         "only disjoint pools keep them from colliding",
				         last - first + 1, servers);
				respond_error(fd, 400, "Bad Request", errbuf);
				return;
			}
		}
	}

	err = dhcp_network_set(&cfg);
	if (err == DHCP_ERR_INVALID) {
		respond_error(fd, 400, "Bad Request",
		               "enabling DHCP needs range_start, range_end (start <= end), a "
		               "lease_seconds between 60 and 30 days, and a server");
		return;
	}
	if (err == DHCP_ERR_FULL) {
		respond_error(fd, 507, "Insufficient Storage", "too many DHCP-configured networks");
		return;
	}
	if (err != DHCP_OK) {
		respond_error(fd, 500, "Internal Server Error", "failed to persist the DHCP config");
		return;
	}
	dhcp_apply_and_maybe_restart();
	jw_init(&w);
	dhcp_network_write_json(dhcp_network_find(network), 1, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_network_dhcp_delete(int fd, const char *network)
{
	enum dhcp_error err = dhcp_network_delete(network);

	if (err == DHCP_ERR_NOT_FOUND) {
		respond_error(fd, 404, "Not Found", "this network has no DHCP configuration");
		return;
	}
	if (err != DHCP_OK) {
		respond_error(fd, 500, "Internal Server Error", "failed to persist the removal");
		return;
	}
	dhcp_apply_and_maybe_restart();
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/* ---------- Issue #51: zswap ---------- */

static void handle_zswap_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	zswap_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_zswap_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const struct json_value *jen;
	const struct json_value *jpct;
	const char *jcomp;
	struct zswap_config next;
	enum zswap_error err;
	struct json_writer w;

	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	/* Partial update: absent fields keep what is configured now, the
	 * same convention every other config PUT here uses. */
	next = *zswap_get();
	jen = json_object_get(root, "enabled");
	jpct = json_object_get(root, "max_pool_percent");
	jcomp = json_as_string(json_object_get(root, "compressor"));
	if (jen != NULL && jen->type == JSON_BOOL)
		next.enabled = jen->u.boolean;
	if (jpct != NULL && jpct->type == JSON_NUMBER)
		next.max_pool_percent = (int)json_as_number(jpct);
	if (jcomp != NULL)
		snprintf(next.compressor, sizeof(next.compressor), "%s", jcomp);
	json_free(root);

	err = zswap_set(&next);
	if (err == ZSWAP_ERR_INVALID) {
		respond_error(fd, 400, "Bad Request",
		               "max_pool_percent must be 1-100 and compressor must be a plain "
		               "algorithm name");
		return;
	}
	if (err == ZSWAP_ERR_UNSUPPORTED) {
		/*
		 * Two different unsupported things, one status: this kernel
		 * has no zswap at all, or has it but was not built with that
		 * compressor. Both mean the request cannot be honoured, and
		 * GET's own available_compressors is where the difference is
		 * visible -- an empty list is the first case.
		 */
		respond_error(fd, 409, "Conflict",
		               "this kernel cannot do that -- either it has no zswap at all, or it "
		               "was not built with the requested compressor (GET this endpoint for "
		               "the list it does have)");
		return;
	}
	if (err == ZSWAP_ERR_APPLY_FAILED) {
		respond_error(fd, 500, "Internal Server Error",
		               "the kernel refused the settings; the previous ones were restored");
		return;
	}
	if (err == ZSWAP_ERR_PERSIST_FAILED) {
		respond_error(fd, 500, "Internal Server Error",
		               "applied to the running kernel but could not be persisted -- it will "
		               "not survive a reboot");
		return;
	}
	jw_init(&w);
	zswap_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_kernel_policy_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	kernel_policy_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_kernel_policy_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const struct json_value *jchannel;
	enum kernel_channel channel;
	struct json_writer w;

	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jchannel = json_object_get(root, "channel");
	if (kernel_channel_from_name(json_as_string(jchannel), &channel) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		               "channel must be one of pinned, longterm, stable, mainline "
		               "-- kernel.org's own monikers");
		return;
	}
	json_free(root);
	if (kernelpolicy_set_channel(channel) != 0) {
		respond_error(fd, 500, "Internal Server Error", "failed to persist kernel policy");
		return;
	}
	jw_init(&w);
	kernel_policy_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_kernel_policy_refresh(int fd)
{
	char err[256];
	struct json_writer w;

	if (kernel_releases_fetch_start(err, sizeof(err)) != 0) {
		respond_error(fd, 409, "Conflict", err);
		return;
	}
	jw_init(&w);
	kernel_policy_write_json(&w);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}

static void handle_bootconsole_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "config");
	bootconsole_write_json(&w);
	jw_key(&w, "loader_entries");
	bootconsole_write_entries_json(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_bootconsole_put(int fd, const char *body, size_t body_len)
{
	char consoles[BOOTCONSOLE_MAX_CONSOLES][BOOTCONSOLE_CONSOLE_MAX];
	int console_count = 0;
	const struct bootconsole_config *cur = bootconsole_get();
	struct json_value *root;
	const struct json_value *jconsoles, *jextra;
	const char *extra;
	enum bootconsole_error err;
	struct json_writer w;
	int applied;
	size_t i;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	/* Absent means unchanged, same convention as every other config
	 * endpoint here -- an operator changing only `extra` should not
	 * have to restate the console list to keep it. */
	jconsoles = json_object_get(root, "consoles");
	if (jconsoles != NULL && jconsoles->type == JSON_ARRAY) {
		for (i = 0; i < jconsoles->u.array.count && console_count < BOOTCONSOLE_MAX_CONSOLES; i++) {
			const char *c = json_as_string(jconsoles->u.array.items[i]);

			snprintf(consoles[console_count], BOOTCONSOLE_CONSOLE_MAX, "%s", c != NULL ? c : "");
			console_count++;
		}
	} else {
		for (console_count = 0; console_count < cur->console_count; console_count++)
			snprintf(consoles[console_count], BOOTCONSOLE_CONSOLE_MAX, "%s",
			         cur->consoles[console_count]);
	}
	jextra = json_object_get(root, "extra");
	extra = jextra != NULL ? json_as_string(jextra) : cur->extra;

	err = bootconsole_set(consoles, console_count, extra);
	json_free(root);
	if (err == BOOTCONSOLE_ERR_INVALID) {
		respond_error(fd, 400, "Bad Request",
		              "a console is a bare tty name with optional comma-separated options "
		              "(\"ttyS0,115200n8\"), and extra parameters are plain kernel arguments -- "
		              "console=/root=/init= are not accepted there, and nothing that could split "
		              "the boot line is accepted at all");
		return;
	}
	if (err != BOOTCONSOLE_OK) {
		respond_error(fd, 500, "Internal Server Error", "could not persist the console settings");
		return;
	}

	/* Applied to the entries already on the ESP, not just future ones:
	 * an operator who cannot see the console cannot wait for the next
	 * A/B update to fix it. */
	applied = bootconsole_apply_to_esp();

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "config");
	bootconsole_write_json(&w);
	jw_key(&w, "loader_entries_updated");
	jw_int(&w, applied);
	jw_key(&w, "loader_entries");
	bootconsole_write_entries_json(&w);
	jw_key(&w, "applies");
	jw_str(&w, "next-boot");
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * GET/PUT /v1/system/control-plane-reservation (issue #86) -- how much
 * of the machine is held back for the daemon itself.
 *
 * Reported with the DERIVED workload ceiling alongside the stored
 * numbers, because "10 percent" is not an answer anyone can act on: the
 * operator wants to know what workloads may actually use on this box,
 * and a percentage plus a live core count is a calculation the API
 * should do once rather than every caller doing it differently.
 */
static void handle_cpreserve_get(int fd)
{
	const struct cpreserve_config *c = cpreserve_get();
	struct json_writer w;
	long long mem_total, mem_free, mem_avail, mem_buffers, mem_cached, swap_total, swap_free;
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);

	read_meminfo(&mem_total, &mem_free, &mem_avail, &mem_buffers, &mem_cached, &swap_total,
	             &swap_free);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "enabled");
	jw_bool(&w, c->enabled);
	jw_key(&w, "cpu_percent");
	jw_int(&w, c->cpu_percent);
	jw_key(&w, "memory_bytes");
	jw_int(&w, c->memory_bytes);
	jw_key(&w, "host_cpus");
	jw_int(&w, cpus > 0 ? cpus : 0);
	jw_key(&w, "host_memory_bytes");
	jw_int(&w, mem_total);
	jw_key(&w, "workload_cpu_max");
	if (c->enabled && cpus > 0) {
		char cpu_max[64];
		long long quota = (long long)cpus * 100000LL * (100 - c->cpu_percent) / 100;

		if (quota < 10000)
			quota = 10000;
		snprintf(cpu_max, sizeof(cpu_max), "%lld 100000", quota);
		jw_str(&w, cpu_max);
	} else {
		jw_null(&w);
	}
	jw_key(&w, "workload_memory_max");
	if (c->enabled && mem_total > c->memory_bytes * 2)
		jw_int(&w, mem_total - c->memory_bytes);
	else
		jw_null(&w);
	jw_key(&w, "cgroup");
	jw_str(&w, CGROUP_WORKLOAD_PARENT);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_cpreserve_put(int fd, const char *body, size_t body_len)
{
	const struct cpreserve_config *c = cpreserve_get();
	struct json_value *root;
	const struct json_value *j;
	int enabled = c->enabled;
	int cpu_percent = c->cpu_percent;
	long long memory_bytes = c->memory_bytes;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	/* Every field optional, absent = unchanged -- the same convention
	 * every other config endpoint here uses, so a caller can flip
	 * `enabled` without restating numbers it does not care about. */
	j = json_object_get(root, "enabled");
	if (j != NULL && j->type == JSON_BOOL)
		enabled = j->u.boolean;
	j = json_object_get(root, "cpu_percent");
	if (j != NULL)
		cpu_percent = (int)json_as_number(j);
	j = json_object_get(root, "memory_bytes");
	if (j != NULL)
		memory_bytes = (long long)json_as_number(j);
	json_free(root);

	if (cpreserve_set(enabled, cpu_percent, memory_bytes) != 0) {
		respond_error(fd, 400, "Bad Request",
		              "cpu_percent must be 1-50 and memory_bytes at least 64 MiB -- a "
		              "reservation larger than that is not a safety margin, it is a second "
		              "workload budget");
		return;
	}
	/* Applied immediately, not at the next container creation: an
	 * operator raising the reservation because the box is under strain
	 * needs it to take effect while it is under strain. */
	workload_parent_ensure();
	handle_cpreserve_get(fd);
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
	const struct json_value *jkeep;
	int upgrade;
	int keep_on_failure;
	char started_name[PKG_NAME_MAX];
	pid_t pid;
	int pidfd;
	int chain_idx;
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
	/* ADR-0175/issue #35: omitted (or false) matches every pre-existing
	 * caller's behavior exactly -- a failed build container is torn
	 * down like always. */
	jkeep = json_object_get(root, "keep_on_failure");
	keep_on_failure = (jkeep != NULL && jkeep->type == JSON_BOOL && jkeep->u.boolean);

	perr = pkg_install_start(name, image, version, upgrade, keep_on_failure, started_name,
	                          sizeof(started_name), &pid, &pidfd, &chain_idx);
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
	register_pkg_fetch_pidfd(pid, pidfd, chain_idx);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}

/*
 * POST /v1/pkg/hostbuild (ADR-0056): the second mode of the same
 * fetch/build pipeline handle_pkg_install() drives above -- builds a
 * standalone host artifact (a kernel bzImage, a fresh thincd-root
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
	const struct json_value *jkeep;
	int upgrade;
	int keep_on_failure;
	pid_t pid;
	int pidfd;
	int chain_idx;
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
	/* ADR-0175/issue #35: see handle_pkg_install()'s own identical field. */
	jkeep = json_object_get(root, "keep_on_failure");
	keep_on_failure = (jkeep != NULL && jkeep->type == JSON_BOOL && jkeep->u.boolean);

	perr = pkg_hostbuild_start(name, build_image, version, upgrade, NULL, keep_on_failure, &pid,
	                            &pidfd, &chain_idx);
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
	register_pkg_fetch_pidfd(pid, pidfd, chain_idx);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}

/*
 * POST /v1/pkg/resume (ADR-0177/issue #46): resumes a build container a
 * prior keep_on_failure attempt (ADR-0175/issue #35) left preserved,
 * skipping the fetch+extract+build-from-scratch restart entirely --
 * the real, repeated cost this exists to eliminate. Unlike handle_pkg_
 * install()/handle_pkg_hostbuild() above, pkg_resume_build() has no
 * fetch subprocess at all -- it's synchronous and returns a ready-to-
 * run container_spec directly, so this handler drives registry_create()
 * itself, right here, the exact same sequence handle_pkg_fetch_event()
 * runs after a successful pkg_fetch_completed() (down to the same
 * REGISTRY_ERR_* logging), with one extra registry_remove() first
 * since build_container_name here is a reuse of the kept container's
 * own name, not a fresh one.
 */
static void handle_pkg_resume(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name_ptr;
	const char *image_ptr;
	const char *version;
	const char *extra_config_symbols;
	const struct json_value *jkeep;
	int keep_on_failure;
	struct container_spec spec;
	int stdio_write_fd;
	int chain_idx;
	enum pkg_error perr;
	char name[PKG_NAME_MAX];
	/* Own copies, not root's own string storage -- pkg_get_one() below
	 * needs name/image again after json_free(root) already ran (this
	 * function's own JSON body is freed right after pkg_resume_build()
	 * returns, well before the container is even spawned, matching
	 * every other pkg handler's own ordering; a bare pointer into
	 * root's storage would dangle by the time pkg_get_one() ran). */
	char image[PKG_IMAGE_NAME_MAX];
	char build_container_name[PKG_NAME_MAX];
	struct registry_entry *entry;
	enum registry_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	name_ptr = json_as_string(json_object_get(root, "name"));
	if (name_ptr == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name missing");
		return;
	}
	snprintf(name, sizeof(name), "%s", name_ptr);
	image_ptr = json_as_string(json_object_get(root, "image"));
	image[0] = '\0';
	if (image_ptr != NULL)
		snprintf(image, sizeof(image), "%s", image_ptr);
	/* ADR-0107: omitted (NULL) resolves to name's highest available
	 * recipe version -- letting a resume pick up a newly-published,
	 * fixed recipe version without needing to reuse the failed
	 * attempt's own exact version string. */
	version = json_as_string(json_object_get(root, "version"));
	extra_config_symbols = json_as_string(json_object_get(root, "extra_config_symbols"));
	jkeep = json_object_get(root, "keep_on_failure");
	keep_on_failure = (jkeep != NULL && jkeep->type == JSON_BOOL && jkeep->u.boolean);

	perr = pkg_resume_build(name, image_ptr, version, extra_config_symbols, keep_on_failure, &spec,
	                         &chain_idx, &stdio_write_fd);
	if (perr != PKG_OK) {
		json_free(root);
		respond_pkg_error(fd, perr);
		return;
	}
	json_free(root);

	pkg_build_container_name(chain_idx, build_container_name, sizeof(build_container_name));
	/*
	 * Frees the registry slot only -- disk (the overlay upper/work dirs
	 * this resume just reused unchanged) is completely untouched,
	 * confirmed directly in registry.c. Not running (the original
	 * build already exited, which is exactly why it had a
	 * kept_build_container to resume in the first place), so this is a
	 * plain table removal, no SIGKILL/reap wait.
	 */
	registry_remove(build_container_name);

	rerr = registry_create(build_container_name, "pkgbuild", "", &spec, NULL, 0, 0, NULL, 0, NULL,
	                        0, NULL, NULL, 0, &entry);
	/* See handle_pkg_fetch_event()'s own identical close -- the child
	 * (if registry_create() actually forked one) already inherited its
	 * own copy via clone3. */
	if (stdio_write_fd >= 0)
		close(stdio_write_fd);

	if (rerr == REGISTRY_ERR_CREATE_FAILED)
		logstore_write("thincd", "error", "pkgbuild resume (%s): container_create failed: %s",
		                build_container_name, container_create_last_error_step());
	else if (rerr == REGISTRY_ERR_DUPLICATE)
		logstore_write("thincd", "error",
		                "pkgbuild resume (%s): a registry entry with this name already exists "
		                "(stale leftover?)",
		                build_container_name);
	else if (rerr == REGISTRY_ERR_FULL)
		logstore_write("thincd", "error", "pkgbuild resume (%s): registry table full",
		                build_container_name);

	if (rerr != REGISTRY_OK) {
		pkg_build_spawn_failed(chain_idx);
		try_start_queued_pkg_rebuild();
		respond_error(fd, 500, "Internal Server Error", "resume failed to spawn build container");
		return;
	}
	register_container_pidfd(entry);
	register_pkg_build_output(pkg_build_output_fd(chain_idx), chain_idx);

	jw_init(&w);
	if (pkg_get_one(name, image, &w) != PKG_OK) {
		/* shouldn't happen -- pkg_resume_build() just re-validated this
		 * exact entry above. */
		jw_free(&w);
		respond_error(fd, 500, "Internal Server Error", "resumed but could not be read back");
		return;
	}
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}

/* GET /v1/pkg/hostbuild/{name} -- status/artifact_path polling for a
 * hostbuild job, the exact shape thincctl pkg hostbuild --wait polls. */
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
 * ADR-0159 Phase B: a bare CONFIG_* symbol name -- the shape every
 * token of a POST /v1/system/kmod-build config_symbols array must
 * have. Deliberately strict (real Kconfig symbol charset only) since
 * this string is later written, unmodified, into a real kernel
 * .config-format fragment file inside the build container.
 */
static int kmod_build_symbol_is_valid(const char *s)
{
	size_t i, len;

	if (s == NULL)
		return 0;
	len = strlen(s);
	if (len < 8 || len >= 64 || strncmp(s, "CONFIG_", 7) != 0)
		return 0;
	for (i = 7; i < len; i++) {
		char c = s[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
			return 0;
	}
	return 1;
}

/*
 * POST /v1/system/kmod-build (ADR-0159 Phase B): an ordinary hostbuild
 * against the "kernel" recipe itself -- the exact existing
 * pkg_hostbuild_start() mechanism handle_pkg_hostbuild() above already
 * drives, gaining only an optional extra-symbols parameter. Simpler
 * than a second recipe or a new persistent kernel-build-tree mechanism
 * (see ADR-0159's own "keep the mechanics simple" design note): the
 * result is a complete new bzImage + full lib/modules/ tree, cut over
 * via the existing A/B kernel-update mechanism, never a live, same-
 * boot addition. Status/artifact_path polling reuses the existing
 * GET /v1/pkg/hostbuild/kernel unchanged -- no new GET endpoint.
 */
static void handle_kmod_build_post(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *build_image_raw;
	const char *version_raw;
	/* Copied out of root before it's freed below -- build_image/version
	 * are pkg_hostbuild_start()'s own arguments, needed after json_free()
	 * has already run (config_symbols validation below can fail deep
	 * into the array and needs to free root at that point too, so root
	 * can't simply be kept alive until the end). */
	char build_image[PKG_IMAGE_NAME_MAX];
	char version[PKG_VERSION_MAX];
	const struct json_value *jupgrade;
	const struct json_value *jsymbols;
	const struct json_value *jkeep;
	int upgrade;
	int keep_on_failure;
	char symbols[PKG_HOSTBUILD_EXTRA_SYMBOLS_MAX];
	size_t symbols_len = 0;
	pid_t pid;
	int pidfd;
	int chain_idx;
	enum pkg_error perr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	build_image_raw = json_as_string(json_object_get(root, "build_image"));
	if (build_image_raw == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "build_image is required");
		return;
	}
	snprintf(build_image, sizeof(build_image), "%s", build_image_raw);
	version_raw = json_as_string(json_object_get(root, "version"));
	snprintf(version, sizeof(version), "%s", version_raw != NULL ? version_raw : "");
	jupgrade = json_object_get(root, "upgrade");
	upgrade = (jupgrade != NULL && jupgrade->type == JSON_BOOL && jupgrade->u.boolean);
	/* ADR-0175/issue #35: this endpoint (a hostbuild against the
	 * "kernel" recipe specifically) is the primary motivating use case
	 * for this flag -- see pkg_install_start()'s own doc comment. */
	jkeep = json_object_get(root, "keep_on_failure");
	keep_on_failure = (jkeep != NULL && jkeep->type == JSON_BOOL && jkeep->u.boolean);

	symbols[0] = '\0';
	jsymbols = json_object_get(root, "config_symbols");
	if (jsymbols != NULL) {
		size_t i;

		if (jsymbols->type != JSON_ARRAY) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "config_symbols must be an array of strings");
			return;
		}
		for (i = 0; i < jsymbols->u.array.count; i++) {
			const char *sym = json_as_string(jsymbols->u.array.items[i]);
			size_t sym_len;

			if (!kmod_build_symbol_is_valid(sym)) {
				json_free(root);
				respond_error(fd, 400, "Bad Request",
				              "every config_symbols entry must be a bare CONFIG_* name");
				return;
			}
			sym_len = strlen(sym);
			if (symbols_len + (i > 0 ? 1 : 0) + sym_len >= sizeof(symbols)) {
				json_free(root);
				respond_error(fd, 400, "Bad Request", "config_symbols is too long");
				return;
			}
			if (i > 0)
				symbols[symbols_len++] = ' ';
			memcpy(symbols + symbols_len, sym, sym_len);
			symbols_len += sym_len;
		}
		symbols[symbols_len] = '\0';
	}
	json_free(root);

	perr = pkg_hostbuild_start("kernel", build_image, version[0] != '\0' ? version : NULL, upgrade,
	                            symbols[0] != '\0' ? symbols : NULL, keep_on_failure, &pid, &pidfd,
	                            &chain_idx);
	if (perr != PKG_OK) {
		respond_pkg_error(fd, perr);
		return;
	}

	jw_init(&w);
	if (pkg_get_one("kernel", PKG_HOSTBUILD_IMAGE, &w) != PKG_OK) {
		jw_free(&w);
		respond_error(fd, 500, "Internal Server Error",
		              "hostbuild started but could not be read back");
		return;
	}
	register_pkg_fetch_pidfd(pid, pidfd, chain_idx);
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
	int chain_idx;
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

	/* An automatic, system-triggered rebuild -- never worth preserving a
	 * build container for (ADR-0175/issue #35 is opt-in, operator-
	 * driven debugging only). */
	perr = pkg_install_start(name, image, NULL, 1, 0, started_name, sizeof(started_name), &pid,
	                          &pidfd, &chain_idx);
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
	register_pkg_fetch_pidfd(pid, pidfd, chain_idx);
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
	/*
	 * Issue #100: what the loop is working on, readable by the
	 * watchdog process. Set before any handler runs and cleared after,
	 * so a request that never returns is named in the stall record
	 * rather than leaving "something, somewhere" as the only clue.
	 */
	{
		char activity[160];

		snprintf(activity, sizeof(activity), "%s %s", req->method, req->path);
		stallwatch_activity(activity);
	}

	/* Every thincctl command and every web UI action already goes
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
	if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/v1/whoami") == 0) {
		handle_whoami(fd, req->headers, req->headers_len);
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
	if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/v1/system/factory-reset") == 0) {
		handle_factory_reset(fd, req->body, req->body_len);
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
	/* Issue #88: persistent volumes. */
	if (strcmp(req->path, "/v1/volumes") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_volume_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_volume_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, "/v1/volumes/", 12) == 0) {
		const char *vname = req->path + 12;

		if (vname[0] != '\0') {
			/*
			 * Sub-resources are matched BEFORE the plain volume GET and
			 * DELETE below. A volume name can never contain '/', so a
			 * path with one is unambiguously a sub-resource -- and
			 * checking the bare handlers first made GET .../backups
			 * look up a volume literally named "bk/backups" and 404.
			 */
			const char *slash = strchr(vname, '/');

			if (slash != NULL && (size_t)(slash - vname) < VOLUME_NAME_MAX) {
				char volume_name[VOLUME_NAME_MAX];

				memcpy(volume_name, vname, slash - vname);
				volume_name[slash - vname] = '\0';

				if (strcmp(slash, "/migrate") == 0 && strcmp(req->method, "POST") == 0) {
					handle_volume_migrate(fd, volume_name, req->body, req->body_len);
					return;
				}
				if (strcmp(slash, "/owner") == 0 && strcmp(req->method, "PUT") == 0) {
					handle_volume_owner_put(fd, volume_name, req->body, req->body_len);
					return;
				}
				if (strcmp(slash, "/quota") == 0 && strcmp(req->method, "PUT") == 0) {
					handle_volume_quota_put(fd, volume_name, req->body, req->body_len);
					return;
				}
				if (strcmp(slash, "/backups") == 0 && strcmp(req->method, "GET") == 0) {
					handle_volume_backups_get(fd, volume_name);
					return;
				}
				if (strcmp(slash, "/backups") == 0 && strcmp(req->method, "PUT") == 0) {
					handle_volume_backup_policy_put(fd, volume_name, req->body, req->body_len);
					return;
				}
				if (strcmp(slash, "/backup") == 0 && strcmp(req->method, "POST") == 0) {
					handle_volume_backup_now(fd, volume_name);
					return;
				}
				if (strcmp(slash, "/restore") == 0 && strcmp(req->method, "POST") == 0) {
					handle_volume_restore(fd, volume_name, req->body, req->body_len);
					return;
				}
				if (strncmp(slash, "/backups/", 9) == 0 && strcmp(req->method, "DELETE") == 0 &&
				    slash[9] != '\0') {
					handle_volume_backup_delete(fd, volume_name, slash + 9);
					return;
				}
			}
			if (strcmp(req->method, "GET") == 0) {
				handle_volume_get(fd, vname);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_volume_delete(fd, vname);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/software") == 0 && strcmp(req->method, "GET") == 0) {
		handle_software_list(fd);
		return;
	}
	if (strcmp(req->path, "/v1/system/volume-backup-config") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_volume_backup_config_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_volume_backup_config_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/server-health") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_serverhealth_list(fd);
			return;
		}
	}
	if (strncmp(req->path, "/v1/system/server-health/", 25) == 0) {
		/* .../{kind}/{container} -- split on the one separating slash. */
		const char *rest = req->path + 25;
		const char *slash = strchr(rest, '/');

		if (slash != NULL && slash[1] != '\0' && strcmp(req->method, "PUT") == 0) {
			char kindbuf[SERVERHEALTH_KIND_MAX];
			size_t klen = (size_t)(slash - rest);

			if (klen > 0 && klen < sizeof(kindbuf)) {
				memcpy(kindbuf, rest, klen);
				kindbuf[klen] = '\0';
				handle_serverhealth_set(fd, kindbuf, slash + 1, req->body, req->body_len);
				return;
			}
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
	if (strcmp(req->path, "/v1/system/sysctl") == 0 && strcmp(req->method, "GET") == 0) {
		handle_sysctl_list(fd);
		return;
	}
	if (strncmp(req->path, SYSCTL_PREFIX, strlen(SYSCTL_PREFIX)) == 0) {
		name = req->path + strlen(SYSCTL_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_sysctl_get(fd, name);
				return;
			}
			if (strcmp(req->method, "PUT") == 0) {
				handle_sysctl_put(fd, name, req->body, req->body_len);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_sysctl_delete(fd, name);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/system/kmod") == 0 && strcmp(req->method, "GET") == 0) {
		handle_kmod_list(fd);
		return;
	}
	if (strncmp(req->path, KMOD_PREFIX, strlen(KMOD_PREFIX)) == 0) {
		name = req->path + strlen(KMOD_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_kmod_get(fd, name);
				return;
			}
			if (strcmp(req->method, "POST") == 0) {
				handle_kmod_post(fd, name, req->body, req->body_len);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_kmod_delete(fd, name);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/system/kmod-config") == 0 && strcmp(req->method, "GET") == 0) {
		handle_kmodconfig_list(fd);
		return;
	}
	if (strncmp(req->path, KMODCONFIG_PREFIX, strlen(KMODCONFIG_PREFIX)) == 0) {
		name = req->path + strlen(KMODCONFIG_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "PUT") == 0) {
				handle_kmodconfig_put(fd, name, req->body, req->body_len);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_kmodconfig_delete(fd, name);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/system/kmod-build") == 0 && strcmp(req->method, "POST") == 0) {
		handle_kmod_build_post(fd, req->body, req->body_len);
		return;
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
	if (strcmp(req->path, "/v1/system/pkg-build-config") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_build_config_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_pkg_build_config_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, "/v1/system/stalls", 17) == 0 &&
	    (req->path[17] == '\0' || req->path[17] == '?') && strcmp(req->method, "GET") == 0) {
		handle_stalls_get(fd, req);
		return;
	}
	if (strcmp(req->path, "/v1/system/boot-console") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_bootconsole_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_bootconsole_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/kernel-policy") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_kernel_policy_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_kernel_policy_put(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/system/kernel-policy/refresh") == 0 &&
	    strcmp(req->method, "POST") == 0) {
		handle_kernel_policy_refresh(fd);
		return;
	}
	if (strcmp(req->path, "/v1/system/control-plane-reservation") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_cpreserve_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_cpreserve_put(fd, req->body, req->body_len);
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
	if (strcmp(req->path, "/v1/dhcp/servers") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_dhcp_servers_get(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_dhcp_server_register(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, "/v1/dhcp/servers/", 17) == 0 && strcmp(req->method, "DELETE") == 0) {
		handle_dhcp_server_unregister(fd, req->path + 17);
		return;
	}
	if (strncmp(req->path, "/v1/dhcp/networks/", 18) == 0) {
		const char *net_name = req->path + 18;

		if (net_name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_network_dhcp_get(fd, net_name);
				return;
			}
			if (strcmp(req->method, "PUT") == 0) {
				handle_network_dhcp_put(fd, net_name, req->body, req->body_len);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_network_dhcp_delete(fd, net_name);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/dhcp") == 0 && strcmp(req->method, "GET") == 0) {
		handle_dhcp_get(fd);
		return;
	}
	if (strcmp(req->path, "/v1/dhcp/leases") == 0 && strcmp(req->method, "GET") == 0) {
		handle_dhcp_leases_get(fd);
		return;
	}
	if (strcmp(req->path, "/v1/dhcp/static") == 0 && strcmp(req->method, "POST") == 0) {
		handle_dhcp_static_post(fd, req->body, req->body_len);
		return;
	}
	if (strncmp(req->path, "/v1/dhcp/static/", 16) == 0 && strcmp(req->method, "DELETE") == 0) {
		handle_dhcp_static_delete(fd, req->path + 16);
		return;
	}
	if (strcmp(req->path, "/v1/system/zswap") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_zswap_get(fd);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			handle_zswap_put(fd, req->body, req->body_len);
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
		if (qlen == strlen("/v1/system/kmsg") &&
		    strncmp(req->path, "/v1/system/kmsg", qlen) == 0) {
			if (strcmp(req->method, "GET") == 0) {
				handle_kmsg(fd, req);
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
				/*
				 * ADR-0197: a server that has just come back gets the
				 * current DHCP files. Creation already stages them, so
				 * this covers the narrow window where a reservation
				 * changed while the container was still starting --
				 * during which dhcp_sync_all() skips it for not being
				 * running yet, and nothing would otherwise revisit it.
				 * No restart: the conf it started with is this one.
				 */
				dhcp_sync_all(NULL, 0, NULL);
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
					if (strcmp(req->method, "GET") == 0) {
						char list_flag[8];

						/* ?list=1 asks for a directory listing instead of
						 * file content -- a separate mode rather than
						 * "GET a directory implicitly lists it", so a
						 * client that meant to read a file and hit a
						 * directory still gets told so. */
						if (url_query_param(req->path, "list", list_flag, sizeof(list_flag)) == 0 &&
						    list_flag[0] != '\0' && strcmp(list_flag, "0") != 0)
							handle_container_dir_list(fd, container_name, rel_path);
						else
							handle_container_file_read(fd, container_name, rel_path);
					}
					else
						handle_container_file_write(fd, container_name, rel_path, req->body,
						                             req->body_len);
					return;
				}
			}
			{
				/*
				 * "/networks" (attach) and "/networks/{network}"
				 * (detach) -- container names are '/'-free, so the
				 * first '/' in name (if any) unambiguously starts this
				 * sub-resource, same reasoning the "/files" block above
				 * already uses.
				 */
				char *slash = strchr(name, '/');

				if (slash != NULL && (size_t)(slash - name) < REGISTRY_NAME_MAX) {
					if (strcmp(slash, "/networks") == 0 && strcmp(req->method, "POST") == 0) {
						char container_name[REGISTRY_NAME_MAX];

						memcpy(container_name, name, slash - name);
						container_name[slash - name] = '\0';
						handle_container_network_attach(fd, container_name, req->body,
						                                 req->body_len);
						return;
					}
					if (strncmp(slash, "/networks/", 10) == 0 &&
					    strcmp(req->method, "DELETE") == 0 && slash[10] != '\0' &&
					    strlen(slash + 10) < NETWORK_NAME_MAX) {
						char container_name[REGISTRY_NAME_MAX];

						memcpy(container_name, name, slash - name);
						container_name[slash - name] = '\0';
						handle_container_network_detach(fd, container_name, slash + 10);
						return;
					}
					/* "/volumes" (attach) / "/volumes/{volume}" (detach),
					 * issue #92 -- same splitting as "/networks" above.
					 * Both edit the persisted definition and apply on the
					 * container's next start, never live. */
					if (strcmp(slash, "/exec") == 0 && strcmp(req->method, "POST") == 0) {
						char container_name[REGISTRY_NAME_MAX];

						memcpy(container_name, name, slash - name);
						container_name[slash - name] = '\0';
						handle_container_exec_post(fd, container_name, req->body, req->body_len);
						return;
					}
					if (strcmp(slash, "/exec") == 0 && strcmp(req->method, "GET") == 0) {
						char container_name[REGISTRY_NAME_MAX];

						memcpy(container_name, name, slash - name);
						container_name[slash - name] = '\0';
						handle_container_exec_get(fd, container_name);
						return;
					}
					if (strcmp(slash, "/volumes") == 0 && strcmp(req->method, "POST") == 0) {
						char container_name[REGISTRY_NAME_MAX];

						memcpy(container_name, name, slash - name);
						container_name[slash - name] = '\0';
						handle_container_volume_attach(fd, container_name, req->body,
						                                req->body_len);
						return;
					}
					if (strncmp(slash, "/volumes/", 9) == 0 &&
					    strcmp(req->method, "DELETE") == 0 && slash[9] != '\0') {
						char container_name[REGISTRY_NAME_MAX];

						memcpy(container_name, name, slash - name);
						container_name[slash - name] = '\0';
						handle_container_volume_detach(fd, container_name, slash + 9);
						return;
					}
					/* "/devices" (attach)/"/devices/{id}" (detach)
					 * (ADR-0161 Phase D) -- same shape as "/networks"
					 * above, a real device id can itself contain '/'
					 * (e.g. "usb:1058:2630:port2-1.3"), so this only
					 * ever splits on the FIRST '/' after the container
					 * name, never a second one inside id itself. */
					if (strcmp(slash, "/devices") == 0 && strcmp(req->method, "POST") == 0) {
						char container_name[REGISTRY_NAME_MAX];

						memcpy(container_name, name, slash - name);
						container_name[slash - name] = '\0';
						handle_container_device_attach(fd, container_name, req->body,
						                                req->body_len);
						return;
					}
					if (strncmp(slash, "/devices/", 9) == 0 && strcmp(req->method, "DELETE") == 0 &&
					    slash[9] != '\0') {
						char container_name[REGISTRY_NAME_MAX];

						memcpy(container_name, name, slash - name);
						container_name[slash - name] = '\0';
						handle_container_device_detach(fd, container_name, slash + 9);
						return;
					}
				}
			}
			if (strcmp(req->method, "GET") == 0) {
				handle_get_one(fd, name);
				return;
			}
			/* Issue #11: edit the stored definition in place, instead
			 * of delete-and-recreate being the only way to change a
			 * cmd, an env var or a file. */
			if (strcmp(req->method, "PATCH") == 0) {
				handle_container_patch(fd, name, req->body, req->body_len);
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
		if (nlen > 8 && strcmp(name + nlen - 8, "/unmount") == 0 &&
		    nlen - 8 < DISKROLE_DISK_NAME_MAX && strcmp(req->method, "POST") == 0) {
			char disk_name[DISKROLE_DISK_NAME_MAX];

			memcpy(disk_name, name, nlen - 8);
			disk_name[nlen - 8] = '\0';
			handle_disk_unmount_post(fd, disk_name, req->body, req->body_len);
			return;
		}
		{
			/*
			 * "/partition-table" (create), "/partitions" (add),
			 * "/partitions/{partition_name}" (delete) -- disk
			 * names are '/'-free, so the first '/' in name (if
			 * any) unambiguously starts this sub-resource, same
			 * reasoning the container "/networks" block already
			 * uses.
			 */
			char *slash = strchr(name, '/');

			if (slash != NULL && (size_t)(slash - name) < DISKROLE_DISK_NAME_MAX) {
				char disk_name[DISKROLE_DISK_NAME_MAX];

				memcpy(disk_name, name, slash - name);
				disk_name[slash - name] = '\0';

				if (strcmp(slash, "/free-space") == 0 && strcmp(req->method, "GET") == 0) {
					handle_disk_free_space(fd, disk_name);
					return;
				}
				if (strcmp(slash, "/partition-table") == 0 &&
				    strcmp(req->method, "POST") == 0) {
					handle_disk_partition_table_post(fd, disk_name, req->body,
					                                  req->body_len);
					return;
				}
				if (strcmp(slash, "/partitions") == 0 && strcmp(req->method, "POST") == 0) {
					handle_disk_partitions_post(fd, disk_name, req->body, req->body_len);
					return;
				}
				if (strncmp(slash, "/partitions/", 12) == 0 &&
				    strcmp(req->method, "POST") == 0) {
					const char *rest = slash + 12;
					const char *tail = strstr(rest, "/resize");

					if (tail != NULL && tail[7] == '\0' && tail != rest &&
					    (size_t)(tail - rest) < DISKROLE_DISK_NAME_MAX) {
						char part_name[DISKROLE_DISK_NAME_MAX];

						memcpy(part_name, rest, tail - rest);
						part_name[tail - rest] = '\0';
						handle_disk_partition_resize(fd, disk_name, part_name, req->body,
						                              req->body_len);
						return;
					}
				}
				if (strncmp(slash, "/partitions/", 12) == 0 &&
				    strcmp(req->method, "DELETE") == 0 && slash[12] != '\0' &&
				    strlen(slash + 12) < DISKROLE_DISK_NAME_MAX) {
					handle_disk_partition_delete(fd, disk_name, slash + 12);
					return;
				}
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
			if (nlen > 6 && strcmp(name + nlen - 6, "/ports") == 0 &&
			    strcmp(req->method, "GET") == 0 && nlen - 6 < NETWORK_NAME_MAX) {
				char net_name[NETWORK_NAME_MAX];

				memcpy(net_name, name, nlen - 6);
				net_name[nlen - 6] = '\0';
				handle_network_ports_get(fd, net_name);
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
			handle_pkg_sync_post(fd, req->body, req->body_len);
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
	if (strcmp(req->path, "/v1/pkg/policies") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_policies_get(fd);
			return;
		}
	}
	if (strncmp(req->path, "/v1/pkg/policies/", 17) == 0) {
		const char *pname = req->path + 17;

		if (pname[0] != '\0') {
			if (strcmp(req->method, "PUT") == 0) {
				handle_pkg_policy_put(fd, pname, req->body, req->body_len);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_pkg_policy_delete(fd, pname);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/pkg/build-logs") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_build_logs_list(fd);
			return;
		}
	}
	if (strncmp(req->path, "/v1/pkg/build-logs/", 19) == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_build_log_get(fd, req->path + 19);
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
	if (strcmp(req->path, "/v1/pkg/resume") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_pkg_resume(fd, req->body, req->body_len);
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
 * X-thinC-Exec-Cmd overrides it, and a container whose image has
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
	struct thinc_epoll_event ev;

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

	if (http_find_header(req->headers, req->headers_len, "X-thinC-Exec-Cmd", cmd_override, sizeof(cmd_override)) >= 0 &&
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
		 * thincd's own stderr -- invisible to a REST client on a
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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, pty_cc->fd, &ev) != 0) {
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
	size_t qlen;
	char target_name[PKG_NAME_MAX];
	char target_image[PKG_IMAGE_NAME_MAX];
	char build_container_name[PKG_NAME_MAX];
	int chain_idx;

	if (strcmp(req->method, "GET") != 0)
		return CONSOLE_NOT_MATCHED;
	/* May carry a trailing "?name=...&image=..." query string (ADR-0157
	 * Phase 2: which of the now-possibly-several concurrent builds to
	 * attach to) -- matched by base-path length, the same convention
	 * "/v1/system/logs"/.../files' own "?..." handling already
	 * established, not a raw strcmp against the full req->path. */
	qlen = strcspn(req->path, "?");
	if (qlen != strlen(PKG_BUILD_LOG_PATH) || strncmp(req->path, PKG_BUILD_LOG_PATH, qlen) != 0)
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

	/*
	 * ADR-0157 Phase 2: ?name= (required if given; ?image= optional,
	 * defaults like every other image-optional entry point) picks which
	 * of the now-possibly-several concurrent builds to attach to. A
	 * caller supplying neither (every client predating this phase, and
	 * the common real-world case even now: PKG_MAX_CONCURRENT_JOBS
	 * being > 1 doesn't mean an operator usually has more than one
	 * build actually running at once) falls back to "the one build in
	 * progress" when that's unambiguous.
	 */
	if (url_query_param(req->path, "name", target_name, sizeof(target_name)) == 0) {
		if (url_query_param(req->path, "image", target_image, sizeof(target_image)) != 0)
			target_image[0] = '\0';
		chain_idx = pkg_chain_index_for_target(target_name, target_image);
	} else {
		int active[PKG_MAX_CONCURRENT_JOBS];
		int active_count = pkg_active_chain_indices(active);

		if (active_count == 0) {
			chain_idx = -1;
		} else if (active_count == 1) {
			chain_idx = active[0];
		} else {
			respond_error(cc->fd, 400, "Bad Request",
			              "multiple builds in progress -- specify ?name=&image=");
			return CONSOLE_FAILED;
		}
	}
	if (chain_idx < 0) {
		respond_error(cc->fd, 404, "Not Found", "no build in progress");
		return CONSOLE_FAILED;
	}
	pkg_build_container_name(chain_idx, build_container_name, sizeof(build_container_name));
	entry = registry_find(build_container_name);
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
	cc->pkg_chain_idx = chain_idx;
	ws_conn_init(&cc->ws);
	g_build_log_ws_conns[g_build_log_ws_conn_count++] = cc;

	/* cc->fd is already registered for EPOLLIN -- no epoll_ctl needed,
	 * same as try_console_upgrade()'s own WS half. */

	snapshot_len = pkg_build_output_snapshot(chain_idx, snapshot, sizeof(snapshot));
	if (snapshot_len > 0 && ws_write_frame(cc->fd, WS_OPCODE_TEXT, snapshot, (size_t)snapshot_len) != 0) {
		build_log_ws_detach(cc);
		thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
				    thinc_write_all(cc->exec_session->pty_conn->fd, frame.payload, frame.payload_len) !=
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
 * (source "thincd", "warning") as a single summary line per failure,
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
		logstore_write("thincd", "warning", "%s from %s: %s", context, peer_ip,
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
	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
	struct thinc_epoll_event ev;

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
		thinc_epoll_ctl(g_epfd, EPOLL_CTL_MOD, cc->fd, &ev);
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
			struct thinc_epoll_event ev;

			memset(&ev, 0, sizeof(ev));
			ev.events = EPOLLIN;
			ev.data.ptr = cc;
			thinc_epoll_ctl(g_epfd, EPOLL_CTL_MOD, cc->fd, &ev);
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
			if (cr == CONSOLE_NOT_MATCHED) {
				dispatch(cc->fd, &req);
				/* Cleared only on the way out: anything still set when
				 * the loop goes quiet is, by construction, the request
				 * that did not come back (issue #100). */
				stallwatch_activity_clear();
			}
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, tfd, &ev) != 0) {
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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, tfd, &ev) != 0) {
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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	close(cc->fd);

	def = containerdef_find(cc->restart_name);
	if (def != NULL && !def->stopped) {
		struct registry_entry *live = registry_find(cc->restart_name);

		if (live != NULL) {
			if (live->reactor_conn != NULL) {
				struct conn *rc = live->reactor_conn;

				thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, rc->fd, NULL);
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

		/*
		 * ADR-0181 (#73): restart:"no" means "never auto-restart",
		 * which includes never being auto-recreated by a rolling
		 * update. Such a container follows rolling only when started
		 * by hand against the new version.
		 */
		if (strcmp(def->restart_policy, "no") == 0)
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
	char disk_copy[DISKROLE_DISK_NAME_MAX];
	int exit_status;
	int teardown_kind;
	time_t started_at;
	struct container_def *def;
	pid_t pkg_pid;
	int pkg_pidfd;
	int pkg_chain_idx;
	int chained;
	char hostbuild_done_name[PKG_NAME_MAX];
	int kept_build_container;

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	registry_mark_exited(entry);
	entry->reactor_conn = NULL;
	free(cc);

	/* Copied before any registry_remove() below might reuse this
	 * slot -- entry->name/exit_status/started_at are only guaranteed
	 * valid until then. teardown_kind/disk_name join them (ADR-0180):
	 * the async-teardown completion further down runs after the
	 * pkgbuild block's own possible registry_remove(). */
	snprintf(name_copy, sizeof(name_copy), "%s", entry->name);
	exit_status = entry->exit_status;
	started_at = entry->started_at;
	teardown_kind = entry->teardown_kind;
	snprintf(disk_copy, sizeof(disk_copy), "%s", entry->disk_name);

	/*
	 * Unconditional, exactly like dns_record_forget_owner()/
	 * pki_cert_forget_owner() on every container delete -- pkg.c
	 * decides relevance (no-op unless this is PKG_BUILD_CONTAINER_NAME),
	 * so this hook stays trivial regardless of which container exited.
	 * A return of 1 means a dependency chain is advancing into another
	 * fetch -- register its pidfd exactly like a fresh top-level
	 * install already does.
	 *
	 * ADR-0175/issue #35: pkg_build_completed() sets kept_build_container
	 * to 1 exactly when this was a keep_on_failure-requested failure of
	 * the chain's own final job -- registry_remove() below is skipped
	 * in that one case, deliberately leaving the exited build
	 * container's registry entry and overlay mount in place for later
	 * inspection (GET .../files) or manual cleanup (DELETE/stop, same
	 * as any other exited container) instead of the normal, immediate
	 * teardown every other pkgbuild exit still gets.
	 */
	chained = pkg_build_completed(entry->name, entry->exit_status, &pkg_pid, &pkg_pidfd,
	                               &pkg_chain_idx, hostbuild_done_name, &kept_build_container);
	if (pkg_build_container_chain_index(entry->name) >= 0 && !kept_build_container)
		registry_remove(entry->name);
	if (chained)
		register_pkg_fetch_pidfd(pkg_pid, pkg_pidfd, pkg_chain_idx);
	else
		try_start_queued_pkg_rebuild();

	/*
	 * ADR-0057: "thinc" is the one hostbuild name this daemon gives
	 * any further meaning to -- everything else pkg.c handles is
	 * completely generic. Deliberately a plain string match here in
	 * main.c, not a flag/callback registered in pkg.c itself: pkg.c
	 * stays fully agnostic to what any package *means*.
	 */
	if (hostbuild_done_name[0] != '\0' && strcmp(hostbuild_done_name, "thinc") == 0) {
		char artifact_dir[PATH_MAX];

		snprintf(artifact_dir, sizeof(artifact_dir), "%s/%s", ARTIFACTS_DIR, hostbuild_done_name);
		spawn_thinc_bootroot_assembly(artifact_dir);
	}

	/*
	 * ADR-0180 (issue #67): this exit was an operator-initiated
	 * DELETE/stop's own SIGKILL finally landing -- run that teardown's
	 * completion instead of the crash-restart logic below. The durable
	 * half (containerdef removal / stopped flag, service-ownership
	 * forgets) already happened synchronously in the handler that sent
	 * the kill; what remains is exactly what needed the process to be
	 * really dead first: releasing the registry slot (safe now --
	 * registry_remove()'s kill/wait branch is skipped for a
	 * non-running entry) and, for delete, the disk cleanup (same
	 * umount-before-rmtree ordering handle_delete()'s own synchronous
	 * exited-container branch documents). registry_find() first: a
	 * pkgbuild container's own pkg_build_completed() block above may
	 * have already removed the entry. For a deleted container the
	 * unconditional remove here also deliberately overrides
	 * keep_on_failure preservation -- an explicit DELETE always means
	 * gone, same as stop's own long-standing "operator intent beats
	 * the keep flag" rule.
	 */
	if (teardown_kind == REGISTRY_TEARDOWN_DELETE || teardown_kind == REGISTRY_TEARDOWN_STOP) {
		if (registry_find(name_copy) != NULL)
			registry_remove(name_copy);
		if (teardown_kind == REGISTRY_TEARDOWN_DELETE) {
			char container_root[PATH_MAX];
			char container_base[PATH_MAX];
			char merged[PATH_MAX];

			container_root_for(disk_copy, container_root, sizeof(container_root));
			snprintf(container_base, sizeof(container_base), "%s/%s", container_root,
			         name_copy);
			snprintf(merged, sizeof(merged), "%s/merged", container_base);
			if (umount2(merged, MNT_DETACH) != 0 && errno != EINVAL && errno != ENOENT)
				fprintf(stderr, "DELETE %s (async completion): umount2(%s) failed: %s\n",
				        name_copy, merged, strerror(errno));
			if (persist_remove_tree(container_base) != 0)
				fprintf(stderr, "DELETE %s (async completion): failed to remove %s: %s\n",
				        name_copy, container_base, strerror(errno));
		}
		return;
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
		/*
		 * ADR-0181 (#73): restart:"no" containers are now persisted too,
		 * so this exit path is reached for them -- they must NOT auto-
		 * restart. They simply become an exited-but-kept definition,
		 * startable by hand. "on-failure" still skips a clean (0) exit.
		 */
		int should_restart = strcmp(def->restart_policy, "no") != 0 &&
		                     !(strcmp(def->restart_policy, "on-failure") == 0 && exit_status == 0);

		if (should_restart) {
			time_t uptime = time(NULL) - started_at;
			int delay = def->restart_delay_seconds;
			int i;

			/* registry_remove() is safe here even though entry->running
			 * is already 0 (its own kill/reap branch is skipped, so no
			 * signal is sent to a possibly-already-reused pid) -- frees
			 * the name/slot before the delayed restart re-creates it.
			 * Done ONLY on the restart path: on the no-restart path
			 * (ADR-0181, below) the entry is deliberately KEPT. */
			registry_remove(name_copy);

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
		/*
		 * else (restart:"no", or on-failure with a clean 0 exit): the
		 * container is NOT restarted, and -- ADR-0181 (#73) -- its live
		 * registry entry is deliberately LEFT in place as its own
		 * "exited" record (registry_mark_exited() already ran at the top
		 * of this handler, so status is "exited" with exit_status
		 * preserved, and the pidfd is already torn down). This is what
		 * makes an exited-on-its-own container both observable (real exit
		 * code, not a bare "stopped") AND name-reserving (a create of the
		 * same name still 409s, matching Docker's exited-container
		 * semantics). The persisted definition survives a daemon restart
		 * independently; there it reappears as a "stopped" def (exit_status
		 * isn't persisted -- ADR-0027's stated boundary) and, for
		 * restart:"no", is not re-attempted at boot (ADR-0181,
		 * containerdef_autostart_all()).
		 */
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
	int chain_idx = cc->pkg_chain_idx;

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	if (waitpid(cc->pkg_fetch_pid, &status, 0) == cc->pkg_fetch_pid && WIFEXITED(status))
		exit_status = WEXITSTATUS(status);
	else
		exit_status = -1;
	close(cc->fd);
	free(cc);

	if (pkg_fetch_completed(chain_idx, exit_status, &spec, &stdio_write_fd)) {
		struct registry_entry *entry;
		char build_container_name[PKG_NAME_MAX];
		enum registry_error rerr;

		pkg_build_container_name(chain_idx, build_container_name, sizeof(build_container_name));
		rerr = registry_create(build_container_name, "pkgbuild", "", &spec, NULL, 0, 0, NULL, 0,
		                        NULL, 0, NULL, NULL, 0, &entry);
		/*
		 * This is the one real, queryable place this failure was
		 * ever diagnosable from: cgroup_create()'s/container_create()'s
		 * own perror() calls write to this daemon's real stderr,
		 * which nothing mirrors into the log store (confirmed live,
		 * the hard way, chasing a real ADR-0165 production regression
		 * on a box with no shell to read raw stderr from at all --
		 * logstore_write() is a one-way "also print to stderr for
		 * boot visibility" call, never the reverse).
		 * container_create_last_error_step() (read below, in the
		 * REGISTRY_ERR_CREATE_FAILED branch) stays valid across the
		 * close() just below -- it's a plain static buffer set by
		 * container_create()'s own last failure, untouched by
		 * anything this function does afterward.
		 */
		{
		/*
		 * The child (if registry_create() actually forked one)
		 * already inherited its own copy of the write end via
		 * clone3 -- this is the daemon's own now-redundant copy,
		 * closed immediately regardless of outcome so a failed
		 * spawn doesn't leak it.
		 */
		if (stdio_write_fd >= 0)
			close(stdio_write_fd);

		if (rerr == REGISTRY_ERR_CREATE_FAILED)
			logstore_write("thincd", "error", "pkgbuild container spawn (%s): container_create failed: %s",
			                build_container_name, container_create_last_error_step());
		else if (rerr == REGISTRY_ERR_DUPLICATE)
			logstore_write("thincd", "error",
			                "pkgbuild container spawn (%s): a registry entry with this name "
			                "already exists (stale leftover?)",
			                build_container_name);
		else if (rerr == REGISTRY_ERR_FULL)
			logstore_write("thincd", "error",
			                "pkgbuild container spawn (%s): registry table full", build_container_name);

		if (rerr != REGISTRY_OK) {
			pkg_build_spawn_failed(chain_idx);
			try_start_queued_pkg_rebuild();
		} else {
			register_container_pidfd(entry);
			register_pkg_build_output(pkg_build_output_fd(chain_idx), chain_idx);
		}
		}
	} else {
		try_start_queued_pkg_rebuild();
	}
}

/*
 * Reaps spawn_thinc_bootroot_assembly()'s own mkbootroot child.
 * Nothing further to dispatch on completion -- either
 * <artifact_dir>/thincd-root.squashfs now exists (success, ready for
 * `pkg hostbuild thinc --deploy` to pick up) or it doesn't (logged
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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
	 * (only triggered by the "thinc" hostbuild's own single-job-
	 * constrained completion event), so g_bootroot_assembly_started's
	 * current value is unambiguously *this* attempt's own generation
	 * number at the moment it resolves, whichever way it resolves.
	 */
	g_bootroot_assembly_running = 0;
	if (reaped == cc->pkg_fetch_pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		g_bootroot_assembly_completed = g_bootroot_assembly_started;
		fprintf(stderr, "thinc bootroot assembly: succeeded\n");
		logstore_write("thincd", "info", "thinc bootroot assembly: succeeded");
		/*
		 * A real diagnostic gap closed here, not just for tonight:
		 * mkbootroot exiting 0 was previously trusted as the whole
		 * story, but a genuinely successful process can still print a
		 * real, useful warning on its way to producing bad output (a
		 * dynamic-linker note, a compressor warning) -- exactly the
		 * class of thing that made ADR-0154's own mksquashfs/
		 * LD_LIBRARY_PATH bug hard to diagnose the first time: "exit 0"
		 * and "correct output" were silently being treated as the same
		 * fact when they aren't. Logged at "info" here (unlike the
		 * "error"-level failure branches below) since a successful run
		 * printing something is not itself a problem, just worth
		 * keeping visible.
		 */
		if (output_len > 0)
			logstore_write("thincd", "info", "thinc bootroot assembly: output: %s", output);
	} else if (reaped != cc->pkg_fetch_pid) {
		fprintf(stderr, "thinc bootroot assembly: waitpid failed\n");
		logstore_write("thincd", "error", "thinc bootroot assembly: waitpid failed: %s",
		                strerror(errno));
	} else if (WIFEXITED(status)) {
		fprintf(stderr, "thinc bootroot assembly: failed (exit %d)\n", WEXITSTATUS(status));
		logstore_write("thincd", "error", "thinc bootroot assembly: mkbootroot exited %d",
		                WEXITSTATUS(status));
		if (output_len > 0)
			logstore_write("thincd", "error", "thinc bootroot assembly: output: %s", output);
	} else if (WIFSIGNALED(status)) {
		fprintf(stderr, "thinc bootroot assembly: killed by signal %d\n", WTERMSIG(status));
		logstore_write("thincd", "error", "thinc bootroot assembly: mkbootroot killed by signal %d",
		                WTERMSIG(status));
		if (output_len > 0)
			logstore_write("thincd", "error", "thinc bootroot assembly: output: %s", output);
	} else {
		fprintf(stderr, "thinc bootroot assembly: failed\n");
		logstore_write("thincd", "error", "thinc bootroot assembly: failed");
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
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD console shell pidfd");
		close(pidfd);
		free(cc);
	}
}

/*
 * Forks a console-login child bound to tty_path and execve()s thincctl
 * into it with no command -- thincctl's own isatty(STDIN_FILENO) check
 * (Phase 18) then drops it straight into run_shell(). setsid() detaches
 * any inherited controlling terminal (moot for a PID 1 caller, which
 * never had one) so the following open() of tty_path, being this new
 * session's first tty open without O_NOCTTY, makes it that session's
 * controlling terminal -- standard Linux tty semantics, no explicit
 * TIOCSCTTY needed. That isolation is what keeps a Ctrl-C typed at this
 * console from ever reaching thincd itself: it lands on this child's
 * own, separate session/process group only. Talks to thincd over real
 * HTTP via g_bind_addr/g_port like any other thincctl invocation --
 * this is still a pure REST client, API-First Mandate intact, just
 * running on the same host it's talking to.
 *
 * A failed open()/execve() (tty genuinely absent, thincctl missing) is
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
		char *argv[] = { (char *)"thincctl", host_arg, port_arg, NULL };

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
		execve("/bin/thincctl", argv, environ);
		perror("execve /bin/thincctl");
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
 * on every respawn (thincctl missing, tty genuinely broken) can't spin
 * the reactor in a tight fork loop.
 */
static void arm_console_respawn_timer(const char *tty_path, int delay_seconds)
{
	int tfd;
	struct itimerspec its;
	struct conn *cc;
	struct thinc_epoll_event ev;

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
	if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, tfd, &ev) != 0) {
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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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

	thinc_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
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
	struct thinc_epoll_event ev;
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
		if (thinc_epoll_ctl(g_epfd, EPOLL_CTL_ADD, client_fd, &ev) != 0) {
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

		if (strcmp(def->restart_policy, "no") == 0) {
			/*
			 * ADR-0181 (#73): restart:"no" is now a persisted policy
			 * meaning "never auto-restart", boot included. It stays a
			 * kept-but-down definition, brought up only by an explicit
			 * `container start`.
			 */
			continue;
		}

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

/*
 * Every one of the ~20 boot_subsystem_init() call sites below loads one
 * subsystem's own persisted JSON state file during boot. A load failure
 * there (malformed/truncated/empty JSON, an invalid field) almost always
 * means "this one file is stale or foreign," not "the host itself is
 * broken" -- each loader already applies its own safe defaults before
 * attempting to parse anything, so its in-memory state is well-defined
 * either way (an empty/default subsystem, not a partially-constructed
 * one -- the parse failure is always caught before any field-level
 * mutation of that state). Treating that as fatal made sense for a
 * dev/test invocation (fail fast, not real PID 1, an ordinary process
 * exit) but is a genuine, confirmed-the-hard-way bug for a real
 * --init-mode boot: thincd IS real PID 1 there, so main() returning at
 * all is a kernel panic ("Attempted to kill init!"), not just a failed
 * daemon start -- a single corrupted state file (a bad restore, a
 * truncated write, disk corruption) was capable of taking the entire
 * box down, unrecoverable short of a full reinstall. This is the one
 * gate every one of those calls goes through now: --init-mode logs and
 * continues (that subsystem keeps its already-applied defaults), a
 * plain/test invocation keeps the original fail-fast behavior exactly
 * as before. Deliberately NOT applied to the handful of boot steps with
 * real mount/network side effects (bootstrap_management_network(), the
 * resolve_*_storage_placement() family, ensure_dir()) -- those fail in
 * a materially different way than "one JSON file didn't parse" and
 * deserve their own dedicated review, not a blanket fix bundled in here.
 */
static int boot_subsystem_init(int init_mode, const char *name, int rc)
{
	if (rc == 0 || !init_mode)
		return rc;
	fprintf(stderr, "warning: %s failed to initialize (rc=%d) -- continuing with defaults\n", name, rc);
	return 0;
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
	struct thinc_epoll_event ev;
	struct sigaction sa;

	/* Stamped before anything else so a slow startup (image scan, state
	 * replay) counts as uptime rather than being invisible. */
	g_daemon_started_at = time(NULL);

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
		else if (strncmp(argv[i], "--test-esp-entries-dir=", 23) == 0)
			snprintf(g_esp_entries_dir, sizeof(g_esp_entries_dir), "%s", argv[i] + 23);
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
	/*
	 * These three are plain config loads with no dependency beyond
	 * their own state paths, and they sit HERE rather than with the
	 * rest of the subsystem init below because the test-update block
	 * immediately following performs a real A/B update -- and that
	 * writes a loader entry whose console parameters come from
	 * bootconsole. Initialised afterwards, the entry was written with
	 * no console= at all, and the machine booted with nothing on the
	 * serial console: found exactly that way, as a boot test that
	 * timed out waiting for output that could no longer exist.
	 */
	cpreserve_init(CPRESERVE_STATE_PATH);   /* issue #86 */
	pkgpolicy_init(PKGPOLICY_STATE_PATH);   /* issue #64 */
	bootconsole_init(BOOTCONSOLE_STATE_PATH); /* issue #24 */
	kernelpolicy_init(KERNELPOLICY_STATE_PATH); /* issue #65 */
	/* Issue #51: applied here, not just on PUT -- the kernel default is
	 * deliberately off, so this is the setting's only chance to survive
	 * a reboot. */
	zswap_init(ZSWAP_STATE_PATH);
	dhcp_init(DHCP_STATE_PATH);
	/* A cached answer from a previous run, if there is one. Its own
	 * mtime is the fetch time -- the file IS the record, so there is
	 * no second place for "when did we last ask" to go stale. */
	{
		struct stat rst;

		if (stat(KERNEL_RELEASES_PATH, &rst) == 0)
			kernelpolicy_ingest_releases(KERNEL_RELEASES_PATH, (long)rst.st_mtime);
	}

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
	/*
	 * Issue #63: before the FIRST subsystem reads anything.
	 *
	 * Placement is the whole correctness of this. Put later in the
	 * sequence -- which is where it was first written, and what the
	 * test caught -- the earlier subsystems have already loaded their
	 * state into memory, so deleting the files leaves a daemon running
	 * with the old world still in it, ready to write every bit of it
	 * back on the next change. The wipe looked like it worked and the
	 * API still answered with everything that was supposed to be gone.
	 */
	factory_reset_apply_if_pending();

	if (boot_subsystem_init(init_mode, "diskrole", diskrole_init(DISKROLE_STATE_PATH)) != 0)
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
	if (boot_subsystem_init(init_mode, "storageplacement", storageplacement_init(STORAGE_PLACEMENT_PATH)) != 0)
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

	if (boot_subsystem_init(init_mode, "network", network_init(NETWORKS_STATE_PATH)) != 0)
		return 1;
	/* Must be initialized before apply_configured_sysctls() below can
	 * read anything out of it (ADR-0160) -- moved up from alongside
	 * this function's other, later _init() calls specifically for
	 * this ordering requirement. */
	if (boot_subsystem_init(init_mode, "sysctlconfig", sysctlconfig_init(SYSCTLCONFIG_STATE_PATH)) != 0)
		return 1;
	/* Same reasoning as sysctlconfig_init() above, for load_configured_
	 * modules() (ADR-0159) -- that step runs much later (after
	 * bootstrap_management_network()), but there's no reason to defer
	 * loading its own persisted table that long, and keeping it next to
	 * its sibling here is clearer than reproducing the same "must run
	 * before its own boot step" comment a second time further down. */
	if (boot_subsystem_init(init_mode, "kmodconfig", kmodconfig_init(KMODCONFIG_STATE_PATH)) != 0)
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
	/* ADR-0160: same init_mode gate as its neighbors -- a persisted
	 * sysctl config is real, ordinary daemon state (safely test-
	 * isolated the same way every other STATE_DIR-relative file
	 * already is), but applying it against a real host's own live
	 * /proc/sys is exactly the class of side effect this project
	 * reserves for a genuine --init-mode boot, matching
	 * load_boot_modules()/bootstrap_management_network() below. Must
	 * run before bootstrap_management_network(): a net.* sysctl (e.g.
	 * net.ipv4.ip_forward) can affect how that bootstrap itself
	 * behaves. */
	if (init_mode)
		apply_configured_sysctls();
	/* Only a real --init-mode boot has a GRUB-supplied net.conf to
	 * bootstrap from (Part 0.5) -- a plain/test invocation has no
	 * management network and simply keeps whatever --bind= it was given. */
	if (init_mode && bootstrap_management_network() != 0)
		return 1;
	/* ADR-0159 Phase A: last of the four boot-time module/sysctl steps
	 * (see load_configured_modules()'s own comment for why it runs
	 * here, after the management network rather than before it). Same
	 * init_mode gate as its three neighbors above -- no real /usr/bin/
	 * modprobe to run in a plain/test invocation. */
	if (init_mode)
		load_configured_modules();
	if (boot_subsystem_init(init_mode, "daemon_config", daemon_config_init(DAEMON_CONFIG_PATH)) != 0)
		return 1;
	/* A persisted port change (PUT /v1/system/daemon-config) survives a
	 * real reboot -- but an explicit --port= on argv (every test/dev
	 * invocation always passes one, to avoid colliding with other
	 * parallel test daemons) always wins over it. */
	if (!port_explicit && daemon_config_port() != 0)
		port = daemon_config_port();
	g_port = port;
	if (boot_subsystem_init(init_mode, "dns", dns_init(DNS_RECORDS_STATE_PATH, DNS_SERVERS_STATE_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "ntp", ntp_init(NTP_STATE_PATH, NTP_SERVERS_STATE_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "syslogfwd", syslogfwd_init(SYSLOGFWD_STATE_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "ldap", ldap_init(LDAP_SERVERS_STATE_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "ldap_record", ldap_record_init(LDAP_USERS_STATE_PATH, LDAP_GROUPS_STATE_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "subid", subid_init(SUBID_STATE_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "serverhealth",
	                         serverhealth_init(SERVERHEALTH_STATE_PATH)) != 0)
		return 1;
	volume_set_dir(VOLUMES_DIR);
	if (boot_subsystem_init(init_mode, "volume", volume_init(VOLUMES_STATE_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "ldap_config", ldap_config_init(LDAP_CONFIG_STATE_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "pki", pki_init(PKI_DIR, PKI_CERTS_STATE_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "siteconfig", siteconfig_init(SITE_CONFIG_PATH)) != 0)
		return 1;
	reconcile_instance_dns_record();
	if (boot_subsystem_init(init_mode, "pkg", pkg_init(PKG_DIR, PKG_INSTALLED_STATE_PATH, CONTAINERS_DIR, IMAGES_DIR, ARTIFACTS_DIR)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "pkg_repo", pkg_repo_init(PKG_REPO_CONFIG_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "pkg_cache", pkg_cache_init(PKG_CACHE_DIR, PKG_CACHE_CONFIG_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "pkg_artifact", pkg_artifact_init(PKG_ARTIFACT_CONFIG_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "pkg_build_config", pkg_build_config_init(PKG_BUILD_CONFIG_PATH)) != 0)
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
	if (boot_subsystem_init(init_mode, "containerdef", containerdef_init(CONTAINER_DEFS_STATE_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "containerdef_rolling_config", containerdef_rolling_config_init(ROLLING_CONFIG_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "connthrottle_config", connthrottle_config_init(CONNTHROTTLE_CONFIG_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "volumebackup",
	                         volumebackup_init(VOLUME_BACKUP_CONFIG_PATH)) != 0)
		return -1;
	if (boot_subsystem_init(init_mode, "backupconfig", backupconfig_init(BACKUP_CONFIG_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "hostauth", hostauth_init(HOSTAUTH_CONFIG_PATH)) != 0)
		return 1;
	if (boot_subsystem_init(init_mode, "devicemap", devicemap_init(DEVICEMAP_STATE_PATH)) != 0)
		return 1;
	/* diskrole_init()/diskformat_remount_present_role_disks() now run
	 * much earlier (ADR-0141 Phase 2) -- see the resolve_state_storage_
	 * placement() block above, which needs both before it can determine
	 * STATE_DIR's own real location. */
	if (boot_subsystem_init(init_mode, "quotamap", quotamap_init(QUOTAMAP_STATE_PATH)) != 0)
		return 1;
	{
		/* issue #28: resolve_swap_placement() itself never blocks
		 * boot -- see its own doc comment. */
		char resolved_swap_file_path[PATH_MAX];

		resolve_swap_placement(resolved_swap_file_path, sizeof(resolved_swap_file_path));
		if (boot_subsystem_init(init_mode, "swap",
		                         swap_init(SWAP_STATE_PATH, resolved_swap_file_path)) != 0)
			return 1;
	}
	if (boot_subsystem_init(init_mode, "logstore", logstore_init(LOG_DIR, LOG_STATE_PATH)) != 0)
		return 1;
	/*
	 * Issue #40: after image_init (it produces a real image version)
	 * AND after logstore_init, so what it did is visible where an
	 * operator actually looks. Reporting it to stderr instead put the
	 * one line that says whether a migration ran somewhere nothing
	 * reads -- this project's own recorded lesson about stderr never
	 * reaching the log store, learned again.
	 */
	pkg_migrate_build_sandbox();
	if (boot_subsystem_init(init_mode, "resolv", resolv_init(RESOLV_CONF_PATH)) != 0)
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

	/*
	 * Issue #86: make the control plane hard to starve.
	 *
	 * On a real install thincd IS the machine's only management channel:
	 * no SSH, no general shell (ADR-0034). Losing it does not degrade the
	 * box, it removes every way in short of the hypervisor -- which is
	 * exactly what happened when four concurrent package builds
	 * oversubscribed a 2-CPU host (issue #85) and the daemon stopped
	 * answering HTTP while the kernel itself stayed perfectly healthy
	 * (ping 0% loss throughout).
	 *
	 * Two cheap, standard protections, both best-effort: a real install
	 * runs this as PID 1 with full privilege, while a dev/test run under
	 * an ordinary user simply won't be permitted to raise its own
	 * priority, and must not fail to start over it.
	 */
	protect_control_plane();

	registry_init();

	/*
	 * ensure_dir()'s directory scaffolding and each module's own
	 * state-init above are real writes onto BASE_DIR -- the real
	 * thinc-containers partition as of this change (ADR-0018), not a
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
	start_uevent_watch(); /* ADR-0161 Phase C -- same g_epfd/best-effort posture as start_kmsg_watch() */
	start_ntp_periodic_timer(); /* same g_epfd/best-effort posture, task #751 */
	start_pkg_sync_periodic_timer(); /* same posture, ADR-0121 -- no-op until an interval is configured */
	start_serverhealth_timer(); /* issue #81 -- probes every registered server on an interval */
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
	 * up to, once boot is fully healthy -- never for a dev/test thincd
	 * (no --init-mode), which is already running attached to a real
	 * developer's own terminal and must never fork a second process to
	 * fight it over. Both consoles get an independent instance -- either
	 * could be the one an operator is actually watching. */
	if (init_mode) {
		spawn_console_shell("/dev/tty0");
		spawn_console_shell("/dev/ttyS0");
	}

	/*
	 * Issue #100: a separate process that notices when this loop stops
	 * going round. It has to be separate -- the loop cannot report its
	 * own silence, which is exactly why a real multi-minute outage on
	 * the production box left no trace anywhere.
	 */
	stallwatch_start(STALLWATCH_RECORDS_PATH);

	while (!g_stop) {
		struct thinc_epoll_event events[MAX_EVENTS];
		/*
		 * One-second timeout rather than an indefinite block: the
		 * heartbeat below has to happen even with nothing to do, or an
		 * idle daemon would be indistinguishable from a wedged one.
		 * A wakeup per second costs nothing next to the per-2s polling
		 * every dashboard client already does.
		 */
		int n = thinc_epoll_wait(g_epfd, events, MAX_EVENTS, 1000);
		int j;
		struct conn *cc;

		stallwatch_heartbeat();

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
			else if (cc->kind == CONN_KERNEL_RELEASES_FETCH)
				handle_kernel_releases_fetch_event(cc);
			else if (cc->kind == CONN_PKG_SYNC_FETCH)
				handle_pkg_sync_fetch_event(cc);
			else if (cc->kind == CONN_PKG_SYNC_PERIODIC_TIMER)
				handle_pkg_sync_periodic_timer_event(cc);
			else if (cc->kind == CONN_EXEC_OUTPUT)
				handle_exec_output_event(cc);
			else if (cc->kind == CONN_EXEC_CHILD)
				handle_exec_child_event(cc);
			else if (cc->kind == CONN_EXEC_TIMER)
				handle_exec_timer_event(cc);
			else if (cc->kind == CONN_BACKUP_PERIODIC_TIMER) {
				/*
				 * Issue #96: the volume sweep rides this same tick
				 * rather than arming a second timer. Both are "copy
				 * something to the backup disk on a schedule", the
				 * intervals are independently configured and each is
				 * checked against its own last-run time, and one timer
				 * is one thing to reason about when backups do not run.
				 */
				volumebackup_sweep(time(NULL), &g_volumebackup_hooks);
				handle_backup_periodic_timer_event(cc);
			}
			else if (cc->kind == CONN_SERVERHEALTH_TIMER) {
				uint64_t ticks;

				/* Drain the timerfd, sweep, re-arm -- the same
				 * self-re-arming shape every other periodic timer
				 * here uses (issue #81). */
				if (read(cc->fd, &ticks, sizeof(ticks)) != (ssize_t)sizeof(ticks))
					; /* a short read just means no tick to act on */
				serverhealth_sweep();
				arm_serverhealth_timer();
			} else if (cc->kind == CONN_SERVERHEALTH_PROBE)
				handle_serverhealth_probe_event(cc);
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
			else if (cc->kind == CONN_UEVENT)
				handle_uevent_event(cc);
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
	printf("thincd shutting down\n");
	fflush(stdout);

	/* As real PID 1 (--init-mode), a bare `return 0` here is exactly
	 * "init exited" -- the kernel panics unconditionally regardless of
	 * how gracefully thincd itself shut down first. reboot(2) is the
	 * actual, correct way for an init process to end its own life; a
	 * dev/test invocation (no --init-mode, e.g. every test/*.c fork+
	 * execve) is never PID 1 and just returns normally, exactly as it
	 * always has -- reboot(2) is never reachable from there regardless
	 * of what set g_shutdown_action (SIGTERM/SIGINT default to
	 * SHUTDOWN_ACTION_POWEROFF; the same tests already send thincd
	 * SIGTERM to end sessions today). */
	if (init_mode) {
		sync();
		reboot(g_shutdown_action == SHUTDOWN_ACTION_REBOOT ? RB_AUTOBOOT : RB_POWER_OFF);
		perror("reboot");
		return 1;
	}
	return 0;
}
