CC := tcc
CFLAGS := -Wall -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 -Iinclude -Inetplane/include
DAEMON_CFLAGS := $(CFLAGS) -Idaemon/include
CLIENT_CFLAGS := $(CFLAGS) -Iclient/include -Idaemon/include
NETPLANE_CFLAGS := $(CFLAGS)
BUILD := build

LIB_SRCS := src/cgroup.c src/mountns.c src/ns_create.c src/container.c src/overlay.c src/container_net.c src/container_dev.c netplane/src/rtnetlink.c
DAEMON_SRCS := daemon/src/json.c daemon/src/http.c daemon/src/registry.c daemon/src/staticfile.c daemon/src/network.c daemon/src/persist.c daemon/src/dns.c daemon/src/pki.c daemon/src/pkg.c daemon/src/device.c
CLIENT_SRCS := client/src/httpclient.c daemon/src/json.c
NETPLANE_SRCS := netplane/src/rtnetlink.c

.PHONY: all clean

all: $(BUILD)/test_toolchain $(BUILD)/test_harness $(BUILD)/harness_child $(BUILD)/test_overlay $(BUILD)/overlay_child $(BUILD)/kanxeod $(BUILD)/test_daemon $(BUILD)/daemon_child $(BUILD)/kanxeoctl $(BUILD)/test_cli $(BUILD)/test_web $(BUILD)/test_rtnetlink $(BUILD)/test_container_net $(BUILD)/net_child $(BUILD)/net_connect $(BUILD)/test_daemon_net $(BUILD)/test_networks $(BUILD)/test_dns $(BUILD)/test_pki $(BUILD)/test_pkg $(BUILD)/mkbootroot $(BUILD)/test_boot $(BUILD)/test_boot_ab $(BUILD)/kanxeo-install $(BUILD)/mkinstalleriso $(BUILD)/test_installer $(BUILD)/test_devices $(BUILD)/dev_child $(BUILD)/test_daemon_devices

$(BUILD):
	mkdir -p $(BUILD)

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

$(BUILD)/kanxeod: daemon/src/main.c $(DAEMON_SRCS) $(LIB_SRCS) | $(BUILD)
	$(CC) $(DAEMON_CFLAGS) $^ -o $@

$(BUILD)/test_daemon: test/test_daemon.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/daemon_child: test/daemon_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/kanxeoctl: cli/src/main.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_cli: test/test_cli.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_web: test/test_web.c client/src/httpclient.c daemon/src/json.c | $(BUILD)
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

$(BUILD)/dev_child: test/dev_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_devices: test/test_devices.c test/test_image_fixture.c $(LIB_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_daemon_devices: test/test_daemon_devices.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_dns: test/test_dns.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_pki: test/test_pki.c test/test_image_fixture.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/test_pkg: test/test_pkg.c $(CLIENT_SRCS) | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) $^ -o $@

$(BUILD)/mkbootroot: image/src/mkbootroot.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/test_boot: test/test_boot.c test/test_disk_image.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/test_boot_ab: test/test_boot_ab.c test/test_disk_image.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/kanxeo-install: image/src/kanxeo-install.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/mkinstalleriso: image/src/mkinstalleriso.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

$(BUILD)/test_installer: test/test_installer.c test/test_disk_image.c test/test_image_fixture.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $^ -o $@

clean:
	rm -rf $(BUILD)
