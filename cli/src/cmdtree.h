/*
 * The CLI command tree (#151).
 *
 * One table describing what commands, subcommands and flags exist, so
 * completion can reach the end of a command instead of stopping at the
 * first word.
 *
 * Why a table rather than reading the parser: dispatch_command() is a
 * hand-written strcmp() chain and each cmd_* function parses its own
 * flags inline, so the surface exists only as control flow. Completion
 * needs it as data.
 *
 * The obvious risk is that this becomes a second model that drifts from
 * the parser -- and a completion offering a flag the parser rejects is
 * worse than no completion at all. test_clitree.c is what makes that a
 * build failure rather than a surprise: it re-derives the command set
 * from cli/src/main.c itself and refuses to agree to disagree.
 *
 * Generated once by structural extraction from main.c (the dispatcher
 * chain, the per-command strcmp() routing, and the "--flag=" literals in
 * each function), then committed as data. Every subcommand named in any
 * usage banner in main.c was covered by that extraction, which is how it
 * was checked rather than assumed.
 */
#ifndef CIX_CLI_CMDTREE_H
#define CIX_CLI_CMDTREE_H

struct cli_node {
	const char *name;
	const char *const *flags; /* NULL-terminated, or NULL for none */
	const struct cli_node *subs; /* name==NULL terminated, or NULL */
};

static const char *const n_login_flags[] = {
	"--password=",
	"--username=",
	NULL
};

static const char *const n_schedule_set_flags[] = {
	"--action=",
	"--catch-up",
	"--daily-at=",
	"--disabled",
	"--every-days=",
	"--every-hours=",
	"--every-minutes=",
	"--every-seconds=",
	"--weekly-at=",
	"--weekly-on=",
	"--window-minutes=",
	NULL
};

static const struct cli_node n_schedule_subs[] = {
	{ "actions", NULL, NULL },
	{ "ls", NULL, NULL },
	{ "rm", NULL, NULL },
	{ "run", NULL, NULL },
	{ "set", n_schedule_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_pipeline_flags[] = {
	"--all",
	NULL
};

static const char *const n_logout_flags[] = {
	"--host=",
	NULL
};

static const char *const n_update_flags[] = {
	"--image-sha256=",
	"--image-url=",
	"--image=",
	"--kernel=",
	NULL
};

static const struct cli_node n_boot_next_subs[] = {
	{ "a", NULL, NULL },
	{ "b", NULL, NULL },
	{ "clear", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_backup_flags[] = {
	"--output=",
	NULL
};

static const char *const n_restore_flags[] = {
	"--input=",
	NULL
};

static const char *const n_site_set_flags[] = {
	"--domain-suffix=",
	"--instance-name=",
	"--site-name=",
	NULL
};

static const struct cli_node n_site_subs[] = {
	{ "set", n_site_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_daemon_config_set_flags[] = {
	"--bind-ip=",
	"--clear-bind-ip",
	"--disable-http",
	"--disable-https",
	"--enable-http",
	"--enable-https",
	"--https-port=",
	"--management-network=",
	"--port=",
	NULL
};

static const struct cli_node n_daemon_config_subs[] = {
	{ "set", n_daemon_config_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_backup_config_set_flags[] = {
	"--clear-disk",
	"--disable",
	"--disk=",
	"--enable",
	NULL
};

static const struct cli_node n_backup_config_subs[] = {
	{ "set", n_backup_config_set_flags, NULL },
	{ "show", NULL, NULL },
	{ "snapshot-now", NULL, NULL },
	{ "status", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_rolling_config_set_flags[] = {
	"--jitter-window-seconds=",
	NULL
};

static const struct cli_node n_rolling_config_subs[] = {
	{ "set", n_rolling_config_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_pkg_build_config_set_flags[] = {
	"--cpu-max=",
	"--max-concurrent-jobs=",
	"--memory-max=",
	NULL
};

static const struct cli_node n_pkg_build_config_subs[] = {
	{ "set", n_pkg_build_config_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_tls_throttle_set_flags[] = {
	"--block-seconds=",
	"--disabled",
	"--enabled",
	"--log-interval-seconds=",
	"--threshold=",
	"--window-seconds=",
	NULL
};

static const struct cli_node n_tls_throttle_subs[] = {
	{ "set", n_tls_throttle_set_flags, NULL },
	{ "show", NULL, NULL },
	{ "status", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_control_plane_reservation_flags[] = {
	"--cpu-percent=",
	"--disabled",
	"--enabled",
	"--memory-bytes=",
	NULL
};

static const struct cli_node n_control_plane_reservation_subs[] = {
	{ "set", NULL, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_kernel_policy_flags[] = {
	"--channel=",
	NULL
};

static const struct cli_node n_kernel_policy_subs[] = {
	{ "refresh", NULL, NULL },
	{ "set", NULL, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_esp_flags[] = {
	"--default=",
	"--timeout=",
	NULL
};

static const struct cli_node n_esp_subs[] = {
	{ "rm-entry", NULL, NULL },
	{ "set", NULL, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_boot_console_flags[] = {
	"--console=",
	"--extra=",
	NULL
};

static const struct cli_node n_boot_console_subs[] = {
	{ "set", NULL, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_hostauth_config_set_flags[] = {
	"--admin-group=",
	"--idle-timeout-seconds=",
	"--ldap-base-dn=",
	"--ldap-disable",
	"--ldap-enable",
	"--ldap-port=",
	"--ldap-server=",
	NULL
};

static const struct cli_node n_hostauth_config_subs[] = {
	{ "set", n_hostauth_config_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const struct cli_node n_hostauth_sessions_subs[] = {
	{ "ls", NULL, NULL },
	{ "revoke", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_iso_build_flags[] = {
	"--disk=",
	"--gateway=",
	"--interface=",
	"--ip=",
	"--prefix=",
	"--wait",
	NULL
};

static const char *const n_iso_publish_flags[] = {
	"--wait",
	NULL
};

static const struct cli_node n_iso_subs[] = {
	{ "build", n_iso_build_flags, NULL },
	{ "publish", n_iso_publish_flags, NULL },
	{ "status", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_routes_add_flags[] = {
	"--default",
	"--dest=",
	"--gateway=",
	"--prefix=",
	NULL
};

static const char *const n_routes_rm_flags[] = {
	"--default",
	"--dest=",
	"--prefix=",
	NULL
};

static const struct cli_node n_assembly_subs[] = {
	{ "start", NULL, NULL },
	{ "status", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const struct cli_node n_routes_subs[] = {
	{ "add", n_routes_add_flags, NULL },
	{ "ls", NULL, NULL },
	{ "rm", n_routes_rm_flags, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_storage_add_partition_flags[] = {
	"--name=",
	"--size-mib=",
	NULL
};

static const char *const n_storage_format_flags[] = {
	"--fs-type=",
	NULL
};

static const char *const n_storage_grow_partition_flags[] = {
	"--size-mib=",
	NULL
};

static const char *const n_storage_logs_migrate_flags[] = {
	"--disk=",
	NULL
};

static const struct cli_node n_storage_logs_subs[] = {
	{ "migrate", n_storage_logs_migrate_flags, NULL },
	{ "migrate-status", NULL, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_storage_rebuildable_migrate_flags[] = {
	"--disk=",
	NULL
};

static const struct cli_node n_storage_rebuildable_subs[] = {
	{ "migrate", n_storage_rebuildable_migrate_flags, NULL },
	{ "migrate-status", NULL, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

/*
 * One `storage` command. The disk/partition subcommands and the
 * placement ones (where the log store and rebuildable data live) were
 * two top-level commands until the disks->storage rename collided
 * them; their subcommand names do not overlap, so they are one list
 * rather than an artificial `storage placement ...` level.
 */
static const struct cli_node n_storage_subs[] = {
	{ "add-partition", n_storage_add_partition_flags, NULL },
	{ "format", n_storage_format_flags, NULL },
	{ "format-status", NULL, NULL },
	{ "free-space", NULL, NULL },
	{ "grow-partition", n_storage_grow_partition_flags, NULL },
	{ "ls", NULL, NULL },
	{ "partition-table", NULL, NULL },
	{ "rm-partition", NULL, NULL },
	{ "logs", NULL, n_storage_logs_subs },
	{ "rebuildable", NULL, n_storage_rebuildable_subs },
	{ "unmount", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_storage_role_create_flags[] = {
	"--disk=",
	"--role=",
	NULL
};

static const struct cli_node n_storage_role_subs[] = {
	{ "create", n_storage_role_create_flags, NULL },
	{ "ls", NULL, NULL },
	{ "rm", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_logs_config_flags[] = {
	"--max-bytes=",
	"--min-level=",
	NULL
};

static const struct cli_node n_logs_subs[] = {
	{ "config", n_logs_config_flags, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_dhcp_flags[] = {
	"--hostname=",
	"--ip=",
	"--lease-seconds=",
	"--mac=",
	"--network=",
	"--range=",
	"--router=",
	"--server=",
	NULL
};

static const struct cli_node n_dhcp_subs[] = {
	{ "disable", NULL, NULL },
	{ "enable", NULL, NULL },
	{ "leases", NULL, NULL },
	{ "remove", NULL, NULL },
	{ "server", NULL, NULL },
	{ "show", NULL, NULL },
	{ "static", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_zswap_flags[] = {
	"--compressor=",
	"--disable",
	"--enable",
	"--max-pool-percent=",
	NULL
};

static const struct cli_node n_zswap_subs[] = {
	{ "set", NULL, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_swap_enable_flags[] = {
	"--disk=",
	"--size-mb=",
	NULL
};

static const struct cli_node n_swap_subs[] = {
	{ "disable", NULL, NULL },
	{ "enable", n_swap_enable_flags, NULL },
	{ "status", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const struct cli_node n_server_health_subs[] = {
	{ "drain", NULL, NULL },
	{ "ls", NULL, NULL },
	{ "undrain", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_factory_reset_flags[] = {
	"--confirm=",
	NULL
};

static const char *const n_volume_backups_flags[] = {
	"--disable",
	"--enable",
	"--retain=",
	"--while-running=",
	NULL
};

static const char *const n_volume_flags[] = {
	"--disk=",
	"--gid=",
	"--name=",
	"--owner-gid=",
	"--owner-uid=",
	"--recursive",
	"--root",
	"--uid=",
	NULL
};

static const struct cli_node n_volume_subs[] = {
	{ "backup", NULL, NULL },
	{ "backups", n_volume_backups_flags, NULL },
	{ "create", NULL, NULL },
	{ "ls", NULL, NULL },
	{ "migrate", NULL, NULL },
	{ "owner", NULL, NULL },
	{ "quota", NULL, NULL },
	{ "restore", NULL, NULL },
	{ "rm", NULL, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_kmsg_flags[] = {
	"--tail=",
	NULL
};

static const struct cli_node n_process_subs[] = {
	{ "kill", NULL, NULL },
	{ "ls", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_resolv_set_flags[] = {
	"--nameserver=",
	NULL
};

static const struct cli_node n_resolv_subs[] = {
	{ "set", n_resolv_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_console_flags[] = {
	"--console=",
	NULL
};

static const char *const n_release_key_set_flags[] = {
	"--key=",
	NULL
};

static const struct cli_node n_release_key_subs[] = {
	{ "clear", NULL, NULL },
	{ "set", n_release_key_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_signing_keys_set_flags[] = {
	"--cert=",
	"--key=",
	NULL
};

static const struct cli_node n_signing_keys_subs[] = {
	{ "clear", NULL, NULL },
	{ "set", n_signing_keys_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_sysctl_set_flags[] = {
	"--no-persist",
	"--value=",
	NULL
};

static const struct cli_node n_sysctl_subs[] = {
	{ "get", NULL, NULL },
	{ "rm", NULL, NULL },
	{ "set", n_sysctl_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_kmod_load_flags[] = {
	"--option=",
	NULL
};

static const struct cli_node n_kmod_subs[] = {
	{ "load", n_kmod_load_flags, NULL },
	{ "ls", NULL, NULL },
	{ "show", NULL, NULL },
	{ "unload", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_kmod_config_set_flags[] = {
	"--autoload",
	"--no-autoload",
	"--option=",
	NULL
};

static const struct cli_node n_kmod_config_subs[] = {
	{ "ls", NULL, NULL },
	{ "rm", NULL, NULL },
	{ "set", n_kmod_config_set_flags, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_kmod_build_flags[] = {
	"--build-image=",
	"--keep-on-failure",
	"--symbol=",
	"--upgrade",
	"--version=",
	"--wait",
	NULL
};

static const char *const n_ntp_config_set_flags[] = {
	"--server=",
	NULL
};

static const struct cli_node n_ntp_config_subs[] = {
	{ "set", n_ntp_config_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_ntp_server_register_flags[] = {
	"--container=",
	NULL
};

static const struct cli_node n_ntp_server_subs[] = {
	{ "ls", NULL, NULL },
	{ "register", n_ntp_server_register_flags, NULL },
	{ "unregister", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const struct cli_node n_ntp_subs[] = {
	{ "config", NULL, n_ntp_config_subs },
	{ "server", NULL, n_ntp_server_subs },
	{ "status", NULL, NULL },
	{ "sync", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_syslog_target_register_flags[] = {
	"--container=",
	NULL
};

static const struct cli_node n_syslog_target_subs[] = {
	{ "ls", NULL, NULL },
	{ "register", n_syslog_target_register_flags, NULL },
	{ "unregister", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const struct cli_node n_syslog_subs[] = {
	{ "target", NULL, n_syslog_target_subs },
	{ NULL, NULL, NULL },
};

static const char *const n_time_set_flags[] = {
	"--unixtime=",
	NULL
};

static const struct cli_node n_time_subs[] = {
	{ "set", n_time_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_container_apply_recipe_flags[] = {
	"--secret=",
	NULL
};

static const char *const n_container_console_flags[] = {
	"--console=",
	NULL
};

static const struct cli_node n_container_device_subs[] = {
	{ "attach", NULL, NULL },
	{ "detach", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_container_edit_flags[] = {
	"--json=",
	NULL
};

static const char *const n_container_files_get_flags[] = {
	"--output=",
	"--path=",
	NULL
};

static const char *const n_container_files_put_flags[] = {
	"--file=",
	"--mode=",
	"--path=",
	NULL
};

static const struct cli_node n_container_files_subs[] = {
	{ "get", n_container_files_get_flags, NULL },
	{ "put", n_container_files_put_flags, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_container_migrate_storage_flags[] = {
	"--disk=",
	NULL
};

static const char *const n_container_network_attach_flags[] = {
	"--ip=",
	"--network=",
	NULL
};

static const struct cli_node n_container_network_subs[] = {
	{ "attach", n_container_network_attach_flags, NULL },
	{ "detach", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_container_recipe_add_flags[] = {
	"--file=",
	"--name=",
	NULL
};

static const struct cli_node n_container_recipe_subs[] = {
	{ "add", n_container_recipe_add_flags, NULL },
	{ "ls", NULL, NULL },
	{ "rm", NULL, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_container_run_flags[] = {
	"--after=",
	"--cap-add=",
	"--capture-output",
	"--cpu-max=",
	"--cpuset=",
	"--depends-on=",
	"--device=",
	"--disk-quota=",
	"--disk=",
	"--dns-register",
	"--dns-server=",
	"--env=",
	"--file-owner=",
	"--file=",
	"--follow-rolling",
	"--follow-rolling-jitter-seconds=",
	"--image=",
	"--interface=",
	"--ip-forward",
	"--ksm",
	"--ldap-allow-group=",
	"--ldap-client",
	"--ldap-group=",
	"--ldap-provision",
	"--ldap-secret-dir=",
	"--ldap-uid=",
	"--ldap-user=",
	"--memory-max=",
	"--memory-swap-max=",
	"--name=",
	"--network=",
	"--on-exit=",
	"--oneshot=",
	"--optional-device=",
	"--pids-max=",
	"--pki-cert-dir=",
	"--pki-days=",
	"--pki-issue",
	"--ready=",
	"--restart-delay=",
	"--restart=",
	"--route=",
	"--service=",
	"--sysctl=",
	"--userns",
	"--volume=",
	NULL
};

static const struct cli_node n_container_service_subs[] = {
	{ "restart", NULL, NULL },
	{ "start", NULL, NULL },
	{ "stop", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_container_volume_attach_flags[] = {
	"--path=",
	"--read-only",
	"--volume=",
	NULL
};

static const struct cli_node n_container_volume_subs[] = {
	{ "attach", n_container_volume_attach_flags, NULL },
	{ "detach", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const struct cli_node n_container_subs[] = {
	{ "apply-recipe", n_container_apply_recipe_flags, NULL },
	{ "console", n_container_console_flags, NULL },
	{ "device", NULL, n_container_device_subs },
	{ "edit", n_container_edit_flags, NULL },
	{ "files", NULL, n_container_files_subs },
	{ "inspect", NULL, NULL },
	{ "ls", NULL, NULL },
	{ "migrate-storage", n_container_migrate_storage_flags, NULL },
	{ "migrate-storage-status", NULL, NULL },
	{ "network", NULL, n_container_network_subs },
	{ "pause", NULL, NULL },
	{ "recipe", NULL, n_container_recipe_subs },
	{ "rm", NULL, NULL },
	{ "run", n_container_run_flags, NULL },
	{ "service", NULL, n_container_service_subs },
	{ "start", NULL, NULL },
	{ "stats", NULL, NULL },
	{ "stop", NULL, NULL },
	{ "unpause", NULL, NULL },
	{ "volume", NULL, n_container_volume_subs },
	{ NULL, NULL, NULL },
};

static const char *const n_network_attach_interface_flags[] = {
	"--interface=",
	"--vlan=",
	NULL
};

static const char *const n_network_create_flags[] = {
	"--address=",
	"--alloc-end=",
	"--alloc-start=",
	"--name=",
	"--prefix=",
	"--subnet=",
	NULL
};

static const char *const n_network_detach_interface_flags[] = {
	"--interface=",
	NULL
};

static const char *const n_network_set_pool_flags[] = {
	"--alloc-end=",
	"--alloc-start=",
	NULL
};

static const struct cli_node n_network_subs[] = {
	{ "attach-interface", n_network_attach_interface_flags, NULL },
	{ "create", n_network_create_flags, NULL },
	{ "detach-interface", n_network_detach_interface_flags, NULL },
	{ "ls", NULL, NULL },
	{ "ports", NULL, NULL },
	{ "rm", NULL, NULL },
	{ "set-pool", n_network_set_pool_flags, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_image_create_flags[] = {
	"--name=",
	NULL
};

static const char *const n_image_gc_flags[] = {
	"--dry-run",
	"--measure",
	NULL
};

static const char *const n_image_manifest_rm_flags[] = {
	"--image=",
	"--package=",
	NULL
};

static const char *const n_image_manifest_set_flags[] = {
	"--image=",
	"--mode=",
	"--package=",
	"--version=",
	NULL
};

static const struct cli_node n_image_manifest_subs[] = {
	{ "rm", n_image_manifest_rm_flags, NULL },
	{ "set", n_image_manifest_set_flags, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_image_recipe_add_flags[] = {
	"--file=",
	"--name=",
	NULL
};

static const struct cli_node n_image_recipe_subs[] = {
	{ "add", n_image_recipe_add_flags, NULL },
	{ "ls", NULL, NULL },
	{ "rm", NULL, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const struct cli_node n_image_subs[] = {
	{ "apply-recipe", NULL, NULL },
	{ "create", n_image_create_flags, NULL },
	{ "gc", n_image_gc_flags, NULL },
	{ "ls", NULL, NULL },
	{ "manifest", NULL, n_image_manifest_subs },
	{ "materialize", NULL, NULL },
	{ "recipe", NULL, n_image_recipe_subs },
	{ "rm", NULL, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const struct cli_node n_device_subs[] = {
	{ "ls", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_devicemap_create_flags[] = {
	"--kind=",
	"--name=",
	"--selector=",
	NULL
};

static const struct cli_node n_devicemap_subs[] = {
	{ "create", n_devicemap_create_flags, NULL },
	{ "ls", NULL, NULL },
	{ "rm", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_dns_forwarders_flags[] = {
	"--forwarder=",
	NULL
};

static const struct cli_node n_dns_forwarders_subs[] = {
	{ "set", NULL, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_dns_provision_flags[] = {
	"--no-resolver",
	"--replica=",
	NULL
};

static const char *const n_dns_record_create_flags[] = {
	"--ip=",
	"--name=",
	NULL
};

static const char *const n_dns_record_update_flags[] = {
	"--ip=",
	"--name=",
	NULL
};

static const struct cli_node n_dns_record_subs[] = {
	{ "create", n_dns_record_create_flags, NULL },
	{ "ls", NULL, NULL },
	{ "rm", NULL, NULL },
	{ "update", n_dns_record_update_flags, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_dns_server_register_flags[] = {
	"--container=",
	"--hosts-path=",
	NULL
};

static const struct cli_node n_dns_server_subs[] = {
	{ "ls", NULL, NULL },
	{ "register", n_dns_server_register_flags, NULL },
	{ "unregister", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const struct cli_node n_dns_subs[] = {
	{ "forwarders", n_dns_forwarders_flags, n_dns_forwarders_subs },
	{ "provision", n_dns_provision_flags, NULL },
	{ "record", NULL, n_dns_record_subs },
	{ "server", NULL, n_dns_server_subs },
	{ NULL, NULL, NULL },
};

static const char *const n_ldap_config_set_flags[] = {
	"--base-dn=",
	"--bind-dn=",
	"--bind-password=",
	"--client-uri=",
	"--start-gid=",
	"--start-uid=",
	NULL
};

static const struct cli_node n_ldap_config_subs[] = {
	{ "set", n_ldap_config_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_ldap_group_add_flags[] = {
	"--gidnumber=",
	"--name=",
	NULL
};

static const char *const n_ldap_group_update_flags[] = {
	"--gidnumber=",
	"--name=",
	"--new-name=",
	NULL
};

static const struct cli_node n_ldap_group_subs[] = {
	{ "add", n_ldap_group_add_flags, NULL },
	{ "ls", NULL, NULL },
	{ "rm", NULL, NULL },
	{ "update", n_ldap_group_update_flags, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_ldap_server_register_flags[] = {
	"--config-path=",
	"--container=",
	NULL
};

static const struct cli_node n_ldap_server_subs[] = {
	{ "ls", NULL, NULL },
	{ "register", n_ldap_server_register_flags, NULL },
	{ "unregister", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const struct cli_node n_ldap_user_subs[] = {
	{ "add", NULL, NULL },
	{ "ls", NULL, NULL },
	{ "rm", NULL, NULL },
	{ "update", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const struct cli_node n_ldap_subs[] = {
	{ "config", NULL, n_ldap_config_subs },
	{ "group", NULL, n_ldap_group_subs },
	{ "server", NULL, n_ldap_server_subs },
	{ "user", NULL, n_ldap_user_subs },
	{ NULL, NULL, NULL },
};

static const char *const n_pki_ca_bootstrap_flags[] = {
	"--common-name=",
	"--days=",
	NULL
};

static const struct cli_node n_pki_ca_subs[] = {
	{ "bootstrap", n_pki_ca_bootstrap_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_pki_cert_create_flags[] = {
	"--days=",
	"--name=",
	"--sans=",
	NULL
};

static const struct cli_node n_pki_cert_subs[] = {
	{ "create", n_pki_cert_create_flags, NULL },
	{ "ls", NULL, NULL },
	{ "rm", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_pki_intermediate_bootstrap_flags[] = {
	"--common-name=",
	"--days=",
	NULL
};

static const struct cli_node n_pki_intermediate_subs[] = {
	{ "bootstrap", n_pki_intermediate_bootstrap_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_pki_reset_flags[] = {
	"--intermediate-common-name=",
	"--intermediate-days=",
	"--leaf-days=",
	"--root-common-name=",
	"--root-days=",
	NULL
};

static const struct cli_node n_pki_subs[] = {
	{ "ca", NULL, n_pki_ca_subs },
	{ "cert", NULL, n_pki_cert_subs },
	{ "intermediate", NULL, n_pki_intermediate_subs },
	{ "reset", n_pki_reset_flags, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_pkg_artifact_config_set_flags[] = {
	"--clear-token",
	"--no-push",
	"--push",
	"--token=",
	"--url=",
	NULL
};

static const struct cli_node n_pkg_artifact_config_subs[] = {
	{ "set", n_pkg_artifact_config_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_pkg_artifact_export_flags[] = {
	"--out=",
	NULL
};

static const char *const n_pkg_bootstrap_flags[] = {
	"--toolchain-sha256=",
	"--toolchain-url=",
	"--toolchain=",
	"--wait",
	NULL
};

static const char *const n_pkg_build_log_flags[] = {
	"--image=",
	"--name=",
	NULL
};

static const char *const n_pkg_build_logs_flags[] = {
	"--file=",
	"--last",
	NULL
};

static const struct cli_node n_pkg_buildenv_subs[] = {
	{ "ls", NULL, NULL },
	{ "rm", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_pkg_cache_config_set_flags[] = {
	"--max-bytes=",
	NULL
};

static const struct cli_node n_pkg_cache_config_subs[] = {
	{ "set", n_pkg_cache_config_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_pkg_cancel_flags[] = {
	"--image=",
	"--name=",
	NULL
};

static const char *const n_pkg_hostbuild_flags[] = {
	"--build-image=",
	"--deploy",
	"--keep-on-failure",
	"--upgrade",
	"--version=",
	"--wait",
	NULL
};

static const char *const n_pkg_install_flags[] = {
	"--image=",
	"--keep-on-failure",
	"--name=",
	"--upgrade",
	"--version=",
	NULL
};

static const char *const n_pkg_policy_flags[] = {
	"--policy=",
	"--version=",
	NULL
};

static const char *const n_pkg_source_policy_flags[] = {
	"--channel=",
	"--depth=",
	NULL
};

static const struct cli_node n_pkg_source_policy_subs[] = {
	{ "clear", NULL, NULL },
	{ "ls", NULL, NULL },
	{ "set", NULL, NULL },
	{ "set-default", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const struct cli_node n_pkg_policy_subs[] = {
	{ "clear", NULL, NULL },
	{ "ls", NULL, NULL },
	{ "set", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_pkg_recipe_add_flags[] = {
	"--file=",
	"--name=",
	NULL
};

static const char *const n_pkg_recipe_rm_flags[] = {
	"--version=",
	NULL
};

static const char *const n_pkg_recipe_show_flags[] = {
	"--version=",
	NULL
};

static const struct cli_node n_pkg_recipe_subs[] = {
	{ "add", n_pkg_recipe_add_flags, NULL },
	{ "rm", n_pkg_recipe_rm_flags, NULL },
	{ "show", n_pkg_recipe_show_flags, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_pkg_repo_config_set_flags[] = {
	"--clear-token",
	"--kind=",
	"--ref=",
	"--token=",
	"--url=",
	NULL
};

static const struct cli_node n_pkg_repo_config_subs[] = {
	{ "set", n_pkg_repo_config_set_flags, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const char *const n_pkg_resume_flags[] = {
	"--image=",
	"--keep-on-failure",
	"--name=",
	"--version=",
	NULL
};

static const char *const n_pkg_sync_flags[] = {
	"--refetch=",
	"--wait",
	NULL
};

static const struct cli_node n_pkg_subs[] = {
	{ "artifact-config", NULL, n_pkg_artifact_config_subs },
	{ "artifact-export", n_pkg_artifact_export_flags, NULL },
	{ "artifact-publish", NULL, NULL },
	{ "bootstrap", n_pkg_bootstrap_flags, NULL },
	{ "bootstrap-status", NULL, NULL },
	{ "build-log", n_pkg_build_log_flags, NULL },
	{ "build-logs", n_pkg_build_logs_flags, NULL },
	{ "buildenv", NULL, n_pkg_buildenv_subs },
	{ "cache-clear", NULL, NULL },
	{ "cache-config", NULL, n_pkg_cache_config_subs },
	{ "cache-status", NULL, NULL },
	{ "cancel", n_pkg_cancel_flags, NULL },
	{ "drift", NULL, NULL },
	{ "verify", NULL, NULL },
	{ "hostbuild", n_pkg_hostbuild_flags, NULL },
	{ "install", n_pkg_install_flags, NULL },
	{ "ls", NULL, NULL },
	{ "policy", n_pkg_policy_flags, n_pkg_policy_subs },
	{ "source-policy", n_pkg_source_policy_flags, n_pkg_source_policy_subs },
	{ "upstreams", NULL, NULL },
	{ "source-catalogue", NULL, NULL },
	{ "rebuilds", NULL, NULL },
	{ "recipe", NULL, n_pkg_recipe_subs },
	{ "recipes", NULL, NULL },
	{ "repo-config", NULL, n_pkg_repo_config_subs },
	{ "resume", n_pkg_resume_flags, NULL },
	{ "rm", NULL, NULL },
	{ "sync", n_pkg_sync_flags, NULL },
	{ "sync-status", NULL, NULL },
	{ "update-all", NULL, NULL },
	{ NULL, NULL, NULL },
};

static const struct cli_node n_show_subs[] = {
	{ "running-config", NULL, NULL },
	{ NULL, NULL, NULL },
};


static const char *const n_ksm_flags[] = {
	"--disable",
	"--enable",
	"--pages-to-scan=",
	"--sleep-millisecs=",
	NULL
};

static const struct cli_node n_ksm_subs[] = {
	{ "set", NULL, NULL },
	{ "show", NULL, NULL },
	{ NULL, NULL, NULL },
};

/* The 62 top-level commands, in dispatcher order. */
static const struct cli_node CLI_TREE[] = {
	{ "health", NULL, NULL },
	{ "login", n_login_flags, NULL },
	{ "logout", n_logout_flags, NULL },
	{ "boot", NULL, NULL },
	{ "shutdown", NULL, NULL },
	{ "reboot", NULL, NULL },
	{ "update", n_update_flags, NULL },
	{ "boot-next", NULL, n_boot_next_subs },
	{ "backup", n_backup_flags, NULL },
	{ "restore", n_restore_flags, NULL },
	{ "site", NULL, n_site_subs },
	{ "daemon-config", NULL, n_daemon_config_subs },
	{ "backup-config", NULL, n_backup_config_subs },
	{ "rolling-config", NULL, n_rolling_config_subs },
	{ "pkg-build-config", NULL, n_pkg_build_config_subs },
	{ "tls-throttle", NULL, n_tls_throttle_subs },
	{ "control-plane-reservation", n_control_plane_reservation_flags, n_control_plane_reservation_subs },
	{ "kernel-policy", n_kernel_policy_flags, n_kernel_policy_subs },
	{ "esp", n_esp_flags, n_esp_subs },
	{ "boot-console", n_boot_console_flags, n_boot_console_subs },
	{ "stalls", NULL, NULL },
	{ "assembly", NULL, n_assembly_subs },
	{ "hostauth-config", NULL, n_hostauth_config_subs },
	{ "hostauth-sessions", NULL, n_hostauth_sessions_subs },
	{ "iso", NULL, n_iso_subs },
	{ "routes", NULL, n_routes_subs },
	{ "storage", NULL, n_storage_subs },
	{ "storage-role", NULL, n_storage_role_subs },
	{ "logs", NULL, n_logs_subs },
	{ "dhcp", n_dhcp_flags, n_dhcp_subs },
	{ "ksm", n_ksm_flags, n_ksm_subs },
	{ "zswap", n_zswap_flags, n_zswap_subs },
	{ "swap", NULL, n_swap_subs },
	{ "host-stats", NULL, NULL },
	{ "server-health", NULL, n_server_health_subs },
	{ "factory-reset", n_factory_reset_flags, NULL },
	{ "software", NULL, NULL },
	{ "volume", n_volume_flags, n_volume_subs },
	{ "kmsg", n_kmsg_flags, NULL },
	{ "process", NULL, n_process_subs },
	{ "ping", NULL, NULL },
	{ "resolv", NULL, n_resolv_subs },
	{ "console", n_console_flags, NULL },
	{ "release-key", NULL, n_release_key_subs },
	{ "signing-keys", NULL, n_signing_keys_subs },
	{ "sysctl", NULL, n_sysctl_subs },
	{ "kmod", NULL, n_kmod_subs },
	{ "kmod-config", NULL, n_kmod_config_subs },
	{ "kmod-build", n_kmod_build_flags, NULL },
	{ "ntp", NULL, n_ntp_subs },
	{ "syslog", NULL, n_syslog_subs },
	{ "time", NULL, n_time_subs },
	{ "container", NULL, n_container_subs },
	{ "network", NULL, n_network_subs },
	{ "image", NULL, n_image_subs },
	{ "device", NULL, n_device_subs },
	{ "devicemap", NULL, n_devicemap_subs },
	{ "dns", NULL, n_dns_subs },
	{ "ldap", NULL, n_ldap_subs },
	{ "pki", NULL, n_pki_subs },
	{ "pipeline", n_pipeline_flags, NULL },
	{ "schedule", NULL, n_schedule_subs },
	{ "pkg", NULL, n_pkg_subs },
	{ "show", NULL, n_show_subs },
	{ NULL, NULL, NULL },
};

#endif /* CIX_CLI_CMDTREE_H */
