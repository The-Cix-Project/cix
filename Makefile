CC := tcc
BUILD := build
CFLAGS := -Wall -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 -Iinclude -Inetplane/include
DAEMON_CFLAGS := $(CFLAGS) -Idaemon/include -Itest -I$(BUILD)
CLIENT_CFLAGS := $(CFLAGS) -Iclient/include -Idaemon/include
NETPLANE_CFLAGS := $(CFLAGS)

LIB_SRCS := src/btrfs.c src/cgroup.c src/mountns.c src/ns_create.c src/container.c src/overlay.c src/container_net.c src/container_dev.c src/container_caps.c netplane/src/rtnetlink.c
DAEMON_SRCS := daemon/src/json.c daemon/src/http.c daemon/src/websocket.c daemon/src/exec.c daemon/src/registry.c daemon/src/staticfile.c daemon/src/network.c daemon/src/persist.c daemon/src/dns.c daemon/src/ldap.c daemon/src/pki.c daemon/src/opensslrun.c daemon/src/releasekey.c daemon/src/elfcheck.c daemon/src/pkg.c daemon/src/device.c daemon/src/devicemap.c daemon/src/image.c daemon/src/containerdef.c daemon/src/siteconfig.c daemon/src/daemon_config.c daemon/src/tlsconn.c daemon/src/quotamap.c daemon/src/swap.c daemon/src/logstore.c daemon/src/disk.c daemon/src/diskrole.c daemon/src/diskformat.c daemon/src/diskpart.c daemon/src/sysctlconfig.c daemon/src/kmod.c daemon/src/kmodconfig.c daemon/src/ping.c daemon/src/resolv.c daemon/src/ntp.c daemon/src/syslogfwd.c daemon/src/hostproc.c daemon/src/connthrottle.c daemon/src/treecopy.c daemon/src/storageplacement.c daemon/src/storagemigrate.c daemon/src/backupconfig.c daemon/src/containerstoragemigrate.c daemon/src/pwhash.c daemon/src/vendor/bcrypt.c daemon/src/vendor/blowfish.c daemon/src/hostauth.c daemon/src/ldapclient.c daemon/src/subid.c daemon/src/serverhealth.c daemon/src/volume.c daemon/src/volumebackup.c daemon/src/cpreserve.c daemon/src/pkgpolicy.c daemon/src/bootconsole.c daemon/src/esp.c daemon/src/stallwatch.c daemon/src/kernelpolicy.c daemon/src/zswap.c daemon/src/dhcp.c daemon/src/targz.c daemon/src/signingkeys.c daemon/src/childdiag.c daemon/src/apiroute.c
CLIENT_SRCS := client/src/httpclient.c daemon/src/json.c
NETPLANE_SRCS := netplane/src/rtnetlink.c

#
# The tests that need nothing but a filesystem: no daemon to fork, no
# containers, no privileged namespaces. They can run anywhere the tree
# can be built, which is what makes them usable as a build gate on a
# Cix host (issue #224).
#
# Every hostbuild already COMPILES all ~85 test binaries -- `all`
# depends on them -- and then throws them away. Running these costs
# seconds and turns that waste into a gate.
#
# What they guard is not incidental: the generated REST surface
# (test_apigen's deliberate route count), the documentation indexes,
# the gcc-exception count that enforces ADR-0224, the ELF install gate,
# and treecopy's device-node handling. Several of those caught real
# mistakes the day this target was written.
#
# The daemon-linked tests are deliberately NOT here: they fork a real
# cixd and need root and a writable data directory, which is a
# different question and needs its own answer.
#
SELFTESTS = \
	$(BUILD)/test_apigen $(BUILD)/test_apiroute $(BUILD)/test_api_surfaces \
	$(BUILD)/test_docindex $(BUILD)/test_toolchain_policy \
	$(BUILD)/test_elfcheck $(BUILD)/test_elfcheck_gcc \
	$(BUILD)/test_treecopy $(BUILD)/test_childdiag \
	$(BUILD)/test_kernelpolicy $(BUILD)/test_releasekey $(BUILD)/test_subid \
	$(BUILD)/test_btrfs $(BUILD)/test_toolchain
#
# Two tests were in this list and are deliberately NOT, because they
# fail in a composed build container for reasons that are not defects.
# Recorded rather than silently dropped, since "why isn't this in the
# gate" is the question someone will ask:
#
#   test_mkbootroot_firmware  asserts that every binary the daemon
#       shells out to by absolute path -- mkfs.ext4, mkfs.btrfs,
#       sfdisk, resize2fs, e2fsck, unsquashfs, openssl, mksquashfs --
#       is staged into a control-plane root from the BUILD HOST. That
#       is a real and valuable check, and it is a check ABOUT THE HOST:
#       it passes where those tools exist and fails where they do not.
#       A composed build container holds exactly the recipe's declared
#       tools (ADR-0199), so it fails there by construction. It belongs
#       wherever a real image is assembled, not in a build gate.
#
#   test_harness  creates a real overlay mount and namespaces. It
#       includes container.h and is a container-runtime test; my
#       original classification looked only at what it links.
#
#   test_dual_console  drives real terminal devices and expects echo
#       back from a console. A build container has no such consoles.
#
# Both were caught by running the gate rather than by reasoning about
# it, which is the argument for having it.
#

.PHONY: selftest
selftest: $(SELFTESTS)
	@fail=0; \
	for t in $(SELFTESTS); do \
		printf '  %-34s ' "$$(basename $$t)"; \
		if $$t >/tmp/selftest.$$$$.log 2>&1; then \
			echo PASS; \
		else \
			echo FAIL; \
			sed 's/^/      /' /tmp/selftest.$$$$.log; \
			fail=1; \
		fi; \
		rm -f /tmp/selftest.$$$$.log; \
	done; \
	if [ $$fail -ne 0 ]; then echo "SELFTEST: FAIL" >&2; exit 1; fi; \
	echo "SELFTEST: PASS"

.PHONY: all clean

all: $(BUILD)/test_toolchain $(BUILD)/test_harness $(BUILD)/harness_child $(BUILD)/test_overlay $(BUILD)/overlay_child $(BUILD)/test_container_pty $(BUILD)/pty_child $(BUILD)/cixd $(BUILD)/test_daemon $(BUILD)/daemon_child $(BUILD)/cixctl $(BUILD)/test_cli $(BUILD)/test_web $(BUILD)/test_rtnetlink $(BUILD)/test_container_net $(BUILD)/net_child $(BUILD)/net_connect $(BUILD)/test_daemon_net $(BUILD)/test_networks $(BUILD)/test_network_interfaces $(BUILD)/test_images $(BUILD)/test_container_restart $(BUILD)/test_container_files $(BUILD)/tcp_listen_child $(BUILD)/test_dns $(BUILD)/test_ntp $(BUILD)/test_ldap $(BUILD)/test_pki $(BUILD)/test_pkg $(BUILD)/mkbootroot $(BUILD)/test_mkbootroot_firmware $(BUILD)/test_boot $(BUILD)/test_boot_ab $(BUILD)/cix-install $(BUILD)/cix-recover $(BUILD)/test_dual_console $(BUILD)/dual_console_child $(BUILD)/mkinstalleriso $(BUILD)/test_installer $(BUILD)/test_devices $(BUILD)/dev_child $(BUILD)/test_daemon_devices $(BUILD)/test_system_update $(BUILD)/test_boot_update $(BUILD)/test_system_backup $(BUILD)/test_console_shell $(BUILD)/mktoolchainimage $(BUILD)/test_console_pki_bootstrap $(BUILD)/test_console_exec $(BUILD)/test_container_lifecycle $(BUILD)/output_child $(BUILD)/stats_child $(BUILD)/test_container_stats $(BUILD)/test_disk_quota $(BUILD)/test_diskpart $(BUILD)/test_sysctl $(BUILD)/test_kmod $(BUILD)/test_kmod_build $(BUILD)/test_routes $(BUILD)/test_daemon_bind_ip $(BUILD)/test_pkg_build_log $(BUILD)/test_pkg_concurrent_stress $(BUILD)/test_pkg_sync $(BUILD)/test_pkg_cache $(BUILD)/test_image_recipe $(BUILD)/test_container_recipe $(BUILD)/test_rolling_restart $(BUILD)/syslog_recv_child $(BUILD)/test_syslogfwd $(BUILD)/test_hostproc $(BUILD)/test_tls_throttle $(BUILD)/test_https_chain $(BUILD)/test_layout_upgrade $(BUILD)/test_treecopy $(BUILD)/test_storage_placement $(BUILD)/test_backup_config $(BUILD)/test_container_storage_migrate $(BUILD)/test_container_dns_servers $(BUILD)/test_hostauth $(BUILD)/test_device_hotplug $(BUILD)/test_subid $(BUILD)/test_volume $(BUILD)/volume_child $(BUILD)/test_factory_reset $(BUILD)/test_boot_console $(BUILD)/test_signing_keys $(BUILD)/test_pkg_recipe_approval $(BUILD)/test_stallwatch $(BUILD)/test_kernelpolicy $(BUILD)/test_dhcp $(BUILD)/test_artifact_export $(BUILD)/test_esp $(BUILD)/test_btrfs $(BUILD)/test_direct_rootfs $(BUILD)/test_targz $(BUILD)/targz_probe $(BUILD)/test_childdiag $(BUILD)/apigen $(BUILD)/test_apigen $(BUILD)/test_apiroute $(BUILD)/test_api_surfaces $(BUILD)/test_docindex $(BUILD)/test_toolchain_policy $(BUILD)/test_elfcheck $(BUILD)/test_elfcheck_gcc $(BUILD)/test_releasekey $(BUILD)/cix-boot.efi $(BUILD)/cix-xorriso

$(BUILD):
	mkdir -p $(BUILD)

# Regenerated on every `make` invocation (.PHONY, not a real file dependency)
# so cixd always reports the commit it was actually built from -- a stale
# version string would be worse than none, and this project has no other
# build-system layer (Makefile shelling out to git here is the one place
# that happens; the actual C compilation stays TCC-only per CLAUDE.md).
#
# CIX_VERSION (optional make variable): an on-box hostbuild compiles from
# a Gitea archive tarball with no .git directory at all, so `git describe`
# there can only ever fail -- every self-hosted build used to report
# "unknown" (user-reported: `cixctl boot` on a freshly-deployed box gave
# no way to tell WHICH build was running beyond its timestamp). The recipe
# already knows the exact tag it fetched (its own pkg_version), so it
# passes it explicitly (`make CIX_VERSION=v1.XX.0 ...`, cix.recipe);
# git describe stays the dev-tree default, "unknown" the last resort.
ifeq ($(strip $(CIX_VERSION)),)
VERSION_CMD = git -C $(CURDIR) describe --tags --always --dirty 2>/dev/null || echo unknown
else
VERSION_CMD = echo '$(CIX_VERSION)'
endif

.PHONY: $(BUILD)/version.h
$(BUILD)/version.h: | $(BUILD)
	@printf '#ifndef CIX_VERSION_H\n#define CIX_VERSION_H\n#define CIX_BUILD_VERSION "%s"\n#define CIX_BUILD_TIME "%s"\n#endif\n' \
		"$$($(VERSION_CMD))" \
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

$(BUILD)/test_container_pty: test/test_container_pty.c test/test_image_fixture.c $(LIB_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/pty_child: test/pty_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/cixd: daemon/src/main.c $(DAEMON_SRCS) $(LIB_SRCS) test/test_image_fixture.c $(BUILD)/version.h $(BUILD)/generated/api_routes.h web/api.js | $(BUILD)
	$(CC) $(DAEMON_CFLAGS) daemon/src/main.c $(DAEMON_SRCS) $(LIB_SRCS) test/test_image_fixture.c -lssl -lcrypto -o $@

$(BUILD)/test_daemon: test/test_daemon.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/daemon_child: test/daemon_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/cixctl: cli/src/main.c client/src/console.c $(CLIENT_SRCS) $(BUILD)/generated/cix_api.h | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) -I$(BUILD) cli/src/main.c client/src/console.c $(CLIENT_SRCS) -o $@

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

$(BUILD)/test_esp: test/test_esp.c test/test_image_fixture.c daemon/src/esp.c daemon/src/persist.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) -Idaemon/include $^ -o $@

$(BUILD)/test_artifact_export: test/test_artifact_export.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/targz_probe: test/targz_probe.c daemon/src/targz.c | $(BUILD)
	$(CC) $(CFLAGS) -Idaemon/include $^ -o $@

$(BUILD)/test_childdiag: test/test_childdiag.c daemon/src/childdiag.c | $(BUILD)
	$(CC) $(CFLAGS) -Idaemon/include $^ -o $@

$(BUILD)/test_targz: test/test_targz.c test/test_image_fixture.c daemon/src/targz.c | $(BUILD)
	$(CC) $(CFLAGS) -Idaemon/include -Itest $^ -o $@

$(BUILD)/test_container_restart: test/test_container_restart.c test/test_image_fixture.c test/test_cleanup.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/output_child: test/output_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_container_lifecycle: test/test_container_lifecycle.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_disk_quota: test/test_disk_quota.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_diskpart: test/test_diskpart.c test/test_image_fixture.c daemon/src/disk.c daemon/src/diskpart.c daemon/src/diskrole.c daemon/src/persist.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_sysctl: test/test_sysctl.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_kmod: test/test_kmod.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_kmod_build: test/test_kmod_build.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_device_hotplug: test/test_device_hotplug.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_storage_placement: test/test_storage_placement.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_backup_config: test/test_backup_config.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_boot_console: test/test_boot_console.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_signing_keys: test/test_signing_keys.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_pkg_recipe_approval: test/test_pkg_recipe_approval.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_stallwatch: test/test_stallwatch.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_container_storage_migrate: test/test_container_storage_migrate.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_container_dns_servers: test/test_container_dns_servers.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_hostauth: test/test_hostauth.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_btrfs: test/test_btrfs.c src/btrfs.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude $^ -o $@

$(BUILD)/test_subid: test/test_subid.c daemon/src/subid.c daemon/src/persist.c daemon/src/json.c | $(BUILD)
	$(CC) $(DAEMON_CFLAGS) $^ -o $@

$(BUILD)/test_dhcp: test/test_dhcp.c test/test_image_fixture.c client/src/httpclient.c daemon/src/json.c | $(BUILD)
	$(CC) $(DAEMON_CFLAGS) -Iclient/include $^ -o $@

$(BUILD)/test_kernelpolicy: test/test_kernelpolicy.c daemon/src/kernelpolicy.c daemon/src/persist.c daemon/src/json.c | $(BUILD)
	$(CC) $(DAEMON_CFLAGS) $^ -o $@

$(BUILD)/test_routes: test/test_routes.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_daemon_bind_ip: test/test_daemon_bind_ip.c test/test_image_fixture.c $(CLIENT_SRCS) netplane/src/rtnetlink.c | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_console_exec: test/test_console_exec.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_direct_rootfs: test/test_direct_rootfs.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_container_files: test/test_container_files.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/volume_child: test/volume_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_factory_reset: test/test_factory_reset.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_volume: test/test_volume.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
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

$(BUILD)/test_dns: test/test_dns.c test/test_image_fixture.c test/test_cleanup.c $(CLIENT_SRCS) | $(BUILD)
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

$(BUILD)/test_pkg_concurrent_stress: test/test_pkg_concurrent_stress.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_pkg_sync: test/test_pkg_sync.c test/test_image_fixture.c daemon/src/persist.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_pkg_cache: test/test_pkg_cache.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_image_recipe: test/test_image_recipe.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_container_recipe: test/test_container_recipe.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_rolling_restart: test/test_rolling_restart.c test/test_image_fixture.c $(CLIENT_SRCS) $(BUILD)/daemon_child | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) test/test_rolling_restart.c test/test_image_fixture.c $(CLIENT_SRCS) -o $@

$(BUILD)/syslog_recv_child: test/syslog_recv_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_syslogfwd: test/test_syslogfwd.c test/test_image_fixture.c $(CLIENT_SRCS) $(BUILD)/daemon_child $(BUILD)/output_child $(BUILD)/syslog_recv_child | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) test/test_syslogfwd.c test/test_image_fixture.c $(CLIENT_SRCS) -o $@

$(BUILD)/test_hostproc: test/test_hostproc.c test/test_image_fixture.c $(CLIENT_SRCS) $(BUILD)/daemon_child | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) test/test_hostproc.c test/test_image_fixture.c $(CLIENT_SRCS) -o $@

$(BUILD)/test_tls_throttle: test/test_tls_throttle.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) test/test_tls_throttle.c test/test_image_fixture.c $(CLIENT_SRCS) -o $@

$(BUILD)/test_https_chain: test/test_https_chain.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) test/test_https_chain.c test/test_image_fixture.c $(CLIENT_SRCS) -o $@

$(BUILD)/test_layout_upgrade: test/test_layout_upgrade.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) test/test_layout_upgrade.c test/test_image_fixture.c $(CLIENT_SRCS) -o $@

#
# ADR-0218: the API route extractor, and its own test.
#
# apigen is a BUILD TOOL -- it runs here and is never installed on a
# host, which is why it lives in tools/ rather than daemon/ or cli/.
# Everything it emits goes under $(BUILD), which .gitignore covers, so a
# generated route table is never committed and there is no "remember to
# regenerate" step: it is rebuilt from docs/api/openapi.yaml before
# anything compiles against it, the same posture $(BUILD)/version.h has.
#
# The one generated translation unit (ADR-0218): the dispatch table,
# rebuilt from the spec whenever the spec or the tool changes, consumed
# only by daemon/src/main.c.
$(BUILD)/generated/cix_api.h: docs/api/openapi.yaml $(BUILD)/apigen
	@mkdir -p $(BUILD)/generated
	$(BUILD)/apigen docs/api/openapi.yaml --emit-cli $@

# The dashboard's API map (ADR-0218). Unlike the C headers this cannot
# live under $(BUILD): the file is SERVED, so it has to be in the web
# root the daemon and the cix recipe both read. The property that
# matters -- never committed -- is kept by .gitignore instead, and the
# cix recipe's own `cp -r web/*` picks it up because pkg_build() runs
# make first.
web/api.js: docs/api/openapi.yaml $(BUILD)/apigen
	$(BUILD)/apigen docs/api/openapi.yaml --emit-web $@

$(BUILD)/generated/api_routes.h: docs/api/openapi.yaml $(BUILD)/apigen
	@mkdir -p $(BUILD)/generated
	$(BUILD)/apigen docs/api/openapi.yaml --emit-routes $@

$(BUILD)/apigen: tools/apigen.c | $(BUILD)
	$(CC) $(CFLAGS) tools/apigen.c -o $@

$(BUILD)/test_elfcheck_gcc: test/test_elfcheck_gcc.c daemon/src/elfcheck.c | $(BUILD)
	$(CC) $(CFLAGS) -Idaemon/include test/test_elfcheck_gcc.c daemon/src/elfcheck.c -o $@

$(BUILD)/test_elfcheck: test/test_elfcheck.c daemon/src/elfcheck.c | $(BUILD)
	$(CC) $(CFLAGS) -Idaemon/include test/test_elfcheck.c daemon/src/elfcheck.c -o $@

$(BUILD)/test_docindex: test/test_docindex.c | $(BUILD)
	$(CC) $(CFLAGS) test/test_docindex.c -o $@

$(BUILD)/test_toolchain_policy: test/test_toolchain_policy.c | $(BUILD)
	$(CC) $(CFLAGS) test/test_toolchain_policy.c -o $@

$(BUILD)/test_releasekey: test/test_releasekey.c daemon/src/releasekey.c daemon/src/opensslrun.c | $(BUILD)
	$(CC) $(CFLAGS) -Idaemon/include test/test_releasekey.c daemon/src/releasekey.c daemon/src/opensslrun.c -lssl -lcrypto -o $@

$(BUILD)/test_api_surfaces: test/test_api_surfaces.c | $(BUILD)
	$(CC) $(CFLAGS) test/test_api_surfaces.c -o $@

$(BUILD)/test_apiroute: test/test_apiroute.c daemon/src/apiroute.c | $(BUILD)
	$(CC) $(CFLAGS) -Idaemon/include test/test_apiroute.c daemon/src/apiroute.c -o $@

$(BUILD)/test_apigen: test/test_apigen.c $(BUILD)/apigen | $(BUILD)
	$(CC) $(CFLAGS) test/test_apigen.c -o $@

$(BUILD)/test_treecopy: test/test_treecopy.c daemon/src/treecopy.c | $(BUILD)
	$(CC) $(CFLAGS) -Idaemon/include test/test_treecopy.c daemon/src/treecopy.c -o $@

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

$(BUILD)/cix-install: image/src/cix-install.c image/src/dual_console.c daemon/src/treecopy.c | $(BUILD)
	$(CC) $(CFLAGS) -Idaemon/include $^ -o $@

#
# cix-boot: the UEFI boot manager (ADR-0215). The one thing in this
# repo not built with TCC, and not by preference -- UEFI uses the
# Microsoft calling convention, and TCC implements no
# __builtin_ms_va_list at all (ADR-0211 proved this directly while
# trying to build gnu-efi).
#
# Linked straight to PE/COFF with `ld -m i386pep`. No ELF-to-PE
# conversion step, which is what systemd's sd-boot needs Python and
# pyelftools for. --subsystem=10 is EFI_APPLICATION; --image-base is
# set so no section lands at address zero.
#
EFI_CC ?= /usr/bin/gcc
EFI_LD ?= ld
EFI_CFLAGS := -ffreestanding -fno-stack-protector -fno-strict-aliasing -fno-ident \
              -fno-asynchronous-unwind-tables \
              -fpic -fshort-wchar -mno-red-zone -Wall -Werror -Iinclude

#
# -s on the link is load-bearing for SECURE BOOT, not a size
# optimisation, and it cost a full QEMU install round to find.
#
# ld appends a COFF symbol table AFTER the last section. Nothing maps
# it, so the file is larger than the sum of its sections -- sbsign says
# so out loud:
#
#   warning: data remaining[9216 vs 11422]: gaps between PE/COFF sections?
#
# Authenticode hashes a PE by walking its sections, and a signer and a
# verifier that disagree about trailing bytes compute different hashes.
# sbsign signed it happily and sbverify accepted it (both used the same
# rules), while shim -- which is stricter, and is the one that matters --
# refused the image on a real boot:
#
#   Verification failed: (0x1A) Security Violation
#
# Stripping removes the symbol table, the file becomes exactly its
# sections, and every hasher agrees. -fno-asynchronous-unwind-tables
# drops .eh_frame for the same reason it is pointless here: nothing
# unwinds in an EFI application.
#
$(BUILD)/cix-xorriso: image/src/cix-xorriso.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/cix-boot.efi: image/src/cix-boot.c include/uefi.h | $(BUILD)
	$(EFI_CC) -c $(EFI_CFLAGS) image/src/cix-boot.c -o $(BUILD)/cix-boot.o
	$(EFI_LD) -m i386pep --subsystem=10 -e efi_main --image-base=0x10000 -s \
	          -o $@ $(BUILD)/cix-boot.o

$(BUILD)/cix-recover: image/src/cix-recover.c image/src/dual_console.c daemon/src/json.c | $(BUILD)
	$(CC) $(CFLAGS) -Idaemon/include $^ -o $@

$(BUILD)/test_dual_console: test/test_dual_console.c image/src/dual_console.c | $(BUILD)
	$(CC) $(CFLAGS) -Iimage/src $^ -o $@

$(BUILD)/dual_console_child: test/dual_console_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/mkinstalleriso: image/src/mkinstalleriso.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/mktoolchainimage: image/src/mktoolchainimage.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/test_installer: test/test_installer.c test/test_disk_image.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest -Idaemon/include $^ -o $@

clean:
	rm -rf $(BUILD)
