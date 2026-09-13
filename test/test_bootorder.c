/*
 * test_bootorder -- daemon_config is initialized before the management
 * network is bootstrapped (ADR-0287).
 *
 * This gates a bug that took 192.168.15.95 down on its first boot after
 * the ADR-0287 upgrade. bootstrap_management_network() now READS the
 * persisted management address (its first-priority source) and WRITES
 * it back when migrating a legacy box or seeding a fresh one. When
 * daemon_config_init() ran AFTER the bootstrap, the daemon_config module
 * had no path set, so the persisting write landed on the read-only
 * squashfs root -- "./.tmp: Read-only file system" -- returned -1, and
 * as PID 1 that parked the whole control plane. The console showed:
 *
 *     .tmp: Read-only file system
 *     bootstrap_management_network: could not persist management address
 *     cixd could not start, and this machine is running it as PID 1.
 *
 * No runtime test reaches this: the entire bootstrap is gated on
 * init_mode, which is false everywhere the suite runs (every test daemon
 * is a plain --data-dir invocation). So this is a STATIC scan, the same
 * instrument test_blocking_waits and test_listenbind use for a boot-path
 * property a green build would otherwise hide -- it asserts the ORDER of
 * the two calls in main.c's boot sequence, which is the thing that was
 * wrong.
 *
 * Crude on purpose (test_api_surfaces spirit): it finds the call text,
 * not syntax. Comment lines and the function's own definition do not
 * count -- only a real call site.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long len;

	if (f == NULL) {
		fprintf(stderr, "test_bootorder: cannot open %s\n", path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)len + 1);
	if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
		fprintf(stderr, "test_bootorder: cannot read %s\n", path);
		exit(1);
	}
	buf[len] = '\0';
	fclose(f);
	return buf;
}

/*
 * Byte offset of the first real call to `needle` in `hay`, skipping any
 * hit on a comment line or on `definition` (the function's own header).
 * Returns -1 if none.
 */
static long call_offset(const char *hay, const char *needle, const char *definition)
{
	const char *p = hay;

	while ((p = strstr(p, needle)) != NULL) {
		const char *line_start = p;
		char line[512];
		const char *line_end;
		size_t line_len;
		const char *t;

		while (line_start > hay && line_start[-1] != '\n')
			line_start--;
		line_end = strchr(p, '\n');
		if (line_end == NULL)
			line_end = hay + strlen(hay);
		line_len = (size_t)(line_end - line_start);
		if (line_len >= sizeof(line))
			line_len = sizeof(line) - 1;
		memcpy(line, line_start, line_len);
		line[line_len] = '\0';

		t = line;
		while (*t == ' ' || *t == '\t')
			t++;
		if (strncmp(t, "*", 1) != 0 && strncmp(t, "/*", 2) != 0 && strncmp(t, "//", 2) != 0 &&
		    (definition == NULL || strstr(line, definition) == NULL))
			return (long)(p - hay);
		p += strlen(needle);
	}
	return -1;
}

int main(void)
{
	char *src = slurp("daemon/src/main.c");
	long init_at, bootstrap_at;
	int fail = 0;

	printf("test_bootorder: daemon_config_init before bootstrap_management_network (ADR-0287)\n");

	init_at = call_offset(src, "daemon_config_init(DAEMON_CONFIG_PATH)", NULL);
	bootstrap_at = call_offset(src, "bootstrap_management_network()",
	                           "static int bootstrap_management_network(");

	if (init_at < 0) {
		printf("  FAIL: no call to daemon_config_init(DAEMON_CONFIG_PATH) found\n");
		fail = 1;
	}
	if (bootstrap_at < 0) {
		printf("  FAIL: no call to bootstrap_management_network() found\n");
		fail = 1;
	}
	if (!fail && init_at >= bootstrap_at) {
		printf("  FAIL: daemon_config_init runs AFTER bootstrap_management_network.\n"
		       "  The bootstrap reads and PERSISTS the management address; without a\n"
		       "  path set first, that write hits the read-only root and parks PID 1\n"
		       "  on the first real boot. Move daemon_config_init before the bootstrap.\n");
		fail = 1;
	}
	if (!fail)
		printf("  ok  daemon_config_init precedes bootstrap_management_network\n");

	free(src);
	if (fail) {
		printf("test_bootorder: FAIL\n");
		return 1;
	}
	printf("test_bootorder: all checks passed\n");
	return 0;
}
