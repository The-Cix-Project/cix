CC := tcc
BUILD := build
CFLAGS := -Wall -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 -Iinclude -Inetplane/include
DAEMON_CFLAGS := $(CFLAGS) -Idaemon/include -Itest -I$(BUILD)
CLIENT_CFLAGS := $(CFLAGS) -Iclient/include -Idaemon/include
NETPLANE_CFLAGS := $(CFLAGS)

LIB_SRCS := src/cgroup.c src/mountns.c src/ns_create.c src/container.c src/overlay.c src/container_net.c src/container_dev.c netplane/src/rtnetlink.c
DAEMON_SRCS := daemon/src/json.c daemon/src/http.c daemon/src/websocket.c daemon/src/exec.c daemon/src/registry.c daemon/src/staticfile.c daemon/src/network.c daemon/src/persist.c daemon/src/dns.c daemon/src/ldap.c daemon/src/pki.c daemon/src/pkg.c daemon/src/device.c daemon/src/devicemap.c daemon/src/image.c daemon/src/containerdef.c daemon/src/siteconfig.c daemon/src/daemon_config.c daemon/src/tlsconn.c daemon/src/quotamap.c daemon/src/swap.c daemon/src/logstore.c daemon/src/disk.c daemon/src/diskrole.c daemon/src/diskformat.c daemon/src/ping.c daemon/src/resolv.c daemon/src/ntp.c
CLIENT_SRCS := client/src/httpclient.c daemon/src/json.c
NETPLANE_SRCS := netplane/src/rtnetlink.c

.PHONY: all clean

all: $(BUILD)/test_toolchain $(BUILD)/test_harness $(BUILD)/harness_child $(BUILD)/test_overlay $(BUILD)/overlay_child $(BUILD)/kanxeod $(BUILD)/test_daemon $(BUILD)/daemon_child $(BUILD)/kanxeoctl $(BUILD)/test_cli $(BUILD)/test_web $(BUILD)/test_rtnetlink $(BUILD)/test_container_net $(BUILD)/net_child $(BUILD)/net_connect $(BUILD)/test_daemon_net $(BUILD)/test_networks $(BUILD)/test_network_interfaces $(BUILD)/test_images $(BUILD)/test_container_restart $(BUILD)/test_container_files $(BUILD)/tcp_listen_child $(BUILD)/test_dns $(BUILD)/test_ntp $(BUILD)/test_ldap $(BUILD)/test_pki $(BUILD)/test_pkg $(BUILD)/mkbootroot $(BUILD)/test_mkbootroot_firmware $(BUILD)/test_boot $(BUILD)/test_boot_ab $(BUILD)/kanxeo-install $(BUILD)/test_dual_console $(BUILD)/dual_console_child $(BUILD)/mkinstalleriso $(BUILD)/test_installer $(BUILD)/test_devices $(BUILD)/dev_child $(BUILD)/test_daemon_devices $(BUILD)/test_system_update $(BUILD)/test_boot_update $(BUILD)/test_system_backup $(BUILD)/test_console_shell $(BUILD)/mktoolchainimage $(BUILD)/test_console_pki_bootstrap $(BUILD)/test_console_exec $(BUILD)/test_container_lifecycle $(BUILD)/output_child $(BUILD)/stats_child $(BUILD)/test_container_stats $(BUILD)/test_disk_quota $(BUILD)/test_routes $(BUILD)/test_daemon_bind_ip $(BUILD)/test_pkg_build_log

$(BUILD):
	mkdir -p $(BUILD)

# Regenerated on every `make` invocation (.PHONY, not a real file dependency)
# so kanxeod always reports the commit it was actually built from -- a stale
# version string would be worse than none, and this project has no other
# build-system layer (Makefile shelling out to git here is the one place
# that happens; the actual C compilation stays TCC-only per CLAUDE.md).
.PHONY: $(BUILD)/version.h
$(BUILD)/version.h: | $(BUILD)
	@printf '#ifndef KANXEO_VERSION_H\n#define KANXEO_VERSION_H\n#define KANXEO_BUILD_VERSION "%s"\n#define KANXEO_BUILD_TIME "%s"\n#endif\n' \
		"$$(git -C $(CURDIR) describe --tags --always --dirty 2>/dev/null || echo unknown)" \
		"$$(date -u +%Y-%m-%dT%H:%M:%SZ)" > $@

$(BUILD)/test_toolchain: test/test_toolchain.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_harness: test/test_harness.c $(LIB_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/harness_child: test/harness_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_overlay: test/test_overlay.c test/test_image_fixture.c $(LIB_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/overlay_child: test/overlay_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/kanxeod: daemon/src/main.c $(DAEMON_SRCS) $(LIB_SRCS) test/test_image_fixture.c $(BUILD)/version.h | $(BUILD)
	$(CC) $(DAEMON_CFLAGS) daemon/src/main.c $(DAEMON_SRCS) $(LIB_SRCS) test/test_image_fixture.c -lssl -lcrypto -o $@

$(BUILD)/test_daemon: test/test_daemon.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/daemon_child: test/daemon_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/kanxeoctl: cli/src/main.c client/src/console.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_cli: test/test_cli.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_web: test/test_web.c test/test_image_fixture.c client/src/httpclient.c daemon/src/json.c | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_rtnetlink: test/test_rtnetlink.c $(NETPLANE_SRCS) src/ns_create.c | $(BUILD)
	$(CC) $(NETPLANE_CFLAGS) $^ -o $@

$(BUILD)/net_child: test/net_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/net_connect: test/net_connect.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_container_net: test/test_container_net.c test/test_image_fixture.c $(LIB_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_daemon_net: test/test_daemon_net.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_networks: test/test_networks.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_network_interfaces: test/test_network_interfaces.c test/test_image_fixture.c $(CLIENT_SRCS) $(NETPLANE_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_images: test/test_images.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_container_restart: test/test_container_restart.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/output_child: test/output_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_container_lifecycle: test/test_container_lifecycle.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_disk_quota: test/test_disk_quota.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_routes: test/test_routes.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_daemon_bind_ip: test/test_daemon_bind_ip.c test/test_image_fixture.c $(CLIENT_SRCS) netplane/src/rtnetlink.c | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_console_exec: test/test_console_exec.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_container_files: test/test_container_files.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_system_backup: test/test_system_backup.c test/test_image_fixture.c daemon/src/persist.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_system_update: test/test_system_update.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/tcp_listen_child: test/tcp_listen_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/dev_child: test/dev_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/stats_child: test/stats_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_container_stats: test/test_container_stats.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_devices: test/test_devices.c test/test_image_fixture.c $(LIB_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_daemon_devices: test/test_daemon_devices.c test/test_image_fixture.c $(CLIENT_SRCS) netplane/src/rtnetlink.c | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_dns: test/test_dns.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_ntp: test/test_ntp.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_ldap: test/test_ldap.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_pki: test/test_pki.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_pkg: test/test_pkg.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_pkg_build_log: test/test_pkg_build_log.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/mkbootroot: image/src/mkbootroot.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/test_mkbootroot_firmware: test/test_mkbootroot_firmware.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_boot: test/test_boot.c test/test_disk_image.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/test_boot_ab: test/test_boot_ab.c test/test_disk_image.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/test_boot_update: test/test_boot_update.c test/test_disk_image.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/test_console_shell: test/test_console_shell.c test/test_disk_image.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/test_console_pki_bootstrap: test/test_console_pki_bootstrap.c test/test_disk_image.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/test_console_pkg_bootstrap: test/test_console_pkg_bootstrap.c test/test_disk_image.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/kanxeo-install: image/src/kanxeo-install.c image/src/dual_console.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_dual_console: test/test_dual_console.c image/src/dual_console.c | $(BUILD)
	$(CC) $(CFLAGS) -Iimage/src $^ -o $@

$(BUILD)/dual_console_child: test/dual_console_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/mkinstalleriso: image/src/mkinstalleriso.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/mktoolchainimage: image/src/mktoolchainimage.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/test_installer: test/test_installer.c test/test_disk_image.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

clean:
	rm -rf $(BUILD)
