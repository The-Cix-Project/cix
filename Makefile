CC := tcc
CFLAGS := -Wall -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 -Iinclude
DAEMON_CFLAGS := $(CFLAGS) -Idaemon/include
BUILD := build

LIB_SRCS := src/cgroup.c src/mountns.c src/ns_create.c src/container.c src/overlay.c
DAEMON_SRCS := daemon/src/json.c daemon/src/http.c daemon/src/registry.c

.PHONY: all clean

all: $(BUILD)/test_toolchain $(BUILD)/test_harness $(BUILD)/harness_child $(BUILD)/test_overlay $(BUILD)/overlay_child $(BUILD)/kanxeod $(BUILD)/test_daemon $(BUILD)/daemon_child

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_toolchain: test/test_toolchain.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_harness: test/test_harness.c $(LIB_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/harness_child: test/harness_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_overlay: test/test_overlay.c $(LIB_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/overlay_child: test/overlay_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/kanxeod: daemon/src/main.c $(DAEMON_SRCS) $(LIB_SRCS) | $(BUILD)
	$(CC) $(DAEMON_CFLAGS) $^ -o $@

$(BUILD)/test_daemon: test/test_daemon.c daemon/src/json.c | $(BUILD)
	$(CC) $(DAEMON_CFLAGS) $^ -o $@

$(BUILD)/daemon_child: test/daemon_child.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf $(BUILD)
