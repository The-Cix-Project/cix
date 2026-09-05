#ifndef DAEMONPATHS_H
#define DAEMONPATHS_H

#include <limits.h>

/*
 * Every filesystem path this daemon derives from --data-dir.
 *
 * All 66 are computed once at startup from g_base_dir, and all 66 were
 * `static` in main.c. That is what actually kept the REST handlers in
 * that file: a handler for swap, or volumes, or packages needs the
 * directory its subsystem lives in, and could not see it from anywhere
 * else. Threading them through signatures was tried first and stopped
 * being honest at the third one -- a handler taking three path strings
 * it immediately passes on is not a better boundary, it is the same
 * coupling written out longhand.
 *
 * Two existing headers already document working around this. disk.h and
 * device.h both say, in as many words, that they have "no knowledge of
 * main.c's own CONTAINERS_DIR global, so the caller passes it". That
 * workaround was right while these were static and is unnecessary now.
 *
 * The definitions stay in main.c beside the code that computes them,
 * with the comments that explain what each one is for. This header only
 * makes them visible, so no call site anywhere changed.
 *
 * These are set once during startup and read-only afterwards. Nothing
 * outside main.c writes one.
 */
extern char STATE_DIR[PATH_MAX];
extern char REBUILDABLE_DIR[PATH_MAX];
extern char IMAGES_DIR[PATH_MAX];
extern char CONTAINERS_DIR[PATH_MAX];
extern char NETWORKS_STATE_PATH[PATH_MAX];
extern char DNS_RECORDS_STATE_PATH[PATH_MAX];
extern char DNS_SERVERS_STATE_PATH[PATH_MAX];
extern char LDAP_SERVERS_STATE_PATH[PATH_MAX];
extern char LDAP_USERS_STATE_PATH[PATH_MAX];
extern char LDAP_GROUPS_STATE_PATH[PATH_MAX];
extern char LDAP_CONFIG_STATE_PATH[PATH_MAX];
extern char SUBID_STATE_PATH[PATH_MAX];
extern char SERVERHEALTH_STATE_PATH[PATH_MAX];
extern char CPRESERVE_STATE_PATH[PATH_MAX];
extern char PKGPOLICY_STATE_PATH[PATH_MAX];
extern char BOOTCONSOLE_STATE_PATH[PATH_MAX];
extern char KERNELPOLICY_STATE_PATH[PATH_MAX];
extern char KERNEL_RELEASES_PATH[PATH_MAX];
extern char ZSWAP_STATE_PATH[PATH_MAX];
extern char KSM_STATE_PATH[PATH_MAX];
extern char DHCP_STATE_PATH[PATH_MAX];
extern char STALLWATCH_RECORDS_PATH[PATH_MAX];
extern char VOLUMES_STATE_PATH[PATH_MAX];
extern char VOLUMES_DIR[PATH_MAX];
extern char VOLUME_BACKUP_CONFIG_PATH[PATH_MAX];
extern char PKI_DIR[PATH_MAX];
extern char PKI_CERTS_STATE_PATH[PATH_MAX];
extern char PKI_CERTS_DIR[PATH_MAX];
extern char PKG_DIR[PATH_MAX];
extern char PKG_INSTALLED_STATE_PATH[PATH_MAX];
extern char PKG_RECIPES_DIR[PATH_MAX];
extern char PKG_REPO_CONFIG_PATH[PATH_MAX];
extern char PKG_CACHE_DIR[PATH_MAX];
extern char PKG_CACHE_CONFIG_PATH[PATH_MAX];
extern char PKG_ARTIFACT_CONFIG_PATH[PATH_MAX];
extern char PKG_BUILD_CONFIG_PATH[PATH_MAX];
extern char ARTIFACTS_DIR[PATH_MAX];
extern char CONTAINER_DEFS_STATE_PATH[PATH_MAX];
extern char ROLLING_CONFIG_PATH[PATH_MAX];
extern char SITE_CONFIG_PATH[PATH_MAX];
extern char DEVICEMAP_STATE_PATH[PATH_MAX];
extern char SYSCTLCONFIG_STATE_PATH[PATH_MAX];
extern char KMODCONFIG_STATE_PATH[PATH_MAX];
extern char DISKROLE_STATE_PATH[PATH_MAX];
extern char STORAGE_PLACEMENT_PATH[PATH_MAX];
extern char DAEMON_CONFIG_PATH[PATH_MAX];
extern char QUOTAMAP_STATE_PATH[PATH_MAX];
extern char SIGNING_KEYS_DIR[PATH_MAX];
extern char ISO_DIR[PATH_MAX];
extern char ISO_OUTPUT_PATH[PATH_MAX];
extern char BOOTROOT_DIR[PATH_MAX];
extern char PKGBUILD_TOOLCHAIN_FETCH_PATH[PATH_MAX];
extern char SYSTEM_UPDATE_FETCH_PATH[PATH_MAX];
extern char SWAP_DIR[PATH_MAX];
extern char SWAP_FILE_PATH[PATH_MAX];
extern char SWAP_STATE_PATH[PATH_MAX];
extern char LOG_DIR[PATH_MAX];
extern char LOG_STATE_PATH[PATH_MAX];
extern char DISKS_MOUNT_DIR[PATH_MAX];
extern char RESOLV_CONF_PATH[PATH_MAX];
extern char NTP_STATE_PATH[PATH_MAX];
extern char NTP_SERVERS_STATE_PATH[PATH_MAX];
extern char SYSLOGFWD_STATE_PATH[PATH_MAX];
extern char CONNTHROTTLE_CONFIG_PATH[PATH_MAX];
extern char BACKUP_CONFIG_PATH[PATH_MAX];
extern char HOSTAUTH_CONFIG_PATH[PATH_MAX];

#endif /* DAEMONPATHS_H */
