/*
 * test_daemon_config -- daemon_config loads the file it writes (#702).
 *
 * save_state() writes "port": 0 and "https_port": 0 for ports nobody
 * set, which is every fresh install. load_state() refused 0 as an
 * invalid port and with it the whole file, so the management address
 * saved beside them was dropped at the next boot: the owner's bare-metal
 * box came up loopback-only after both reboots on 2026-11-21. This test
 * writes the file exactly as save_state() does, then loads it.
 */
#include "daemon_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *what)
{
	printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

static int write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (f == NULL)
		return -1;
	fputs(text, f);
	return fclose(f);
}

int main(void)
{
	char dir[] = "/tmp/test_daemon_config_XXXXXX";
	char path[256];
	const char *addr;

	printf("test_daemon_config: the file daemon_config writes is the file it reads (#702)\n");
	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(path, sizeof(path), "%s/daemon_config.json", dir);

	/* 1: a fresh install's file, as save_state() writes it */
	write_file(path, "{\"port\":0,\"http_enabled\":true,\"https_enabled\":true,"
	                 "\"userns_default\":true,\"https_port\":0,"
	                 "\"management_address\":\"192.168.253.1\"}");
	check(daemon_config_init(path) == 0, "a file with port 0 and https_port 0 loads");
	addr = daemon_config_management_address();
	check(addr != NULL && strcmp(addr, "192.168.253.1") == 0,
	      "its management address survives the load");
	check(daemon_config_port() == 0, "port 0 stays 'no override'");
	check(daemon_config_https_port() == 443, "https_port 0 falls back to 443");

	/* 2: the round trip itself -- set, then load what was saved */
	unlink(path);
	check(daemon_config_init(path) == 0, "no file: first start");
	check(daemon_config_set_management_address("10.0.0.7") == DAEMON_CONFIG_OK,
	      "set a management address on a fresh config");
	check(daemon_config_init(path) == 0, "the saved file loads");
	addr = daemon_config_management_address();
	check(addr != NULL && strcmp(addr, "10.0.0.7") == 0, "the address round-trips");

	/* 3: a real, out-of-range port is still refused */
	write_file(path, "{\"port\":70000,\"management_address\":\"10.0.0.7\"}");
	check(daemon_config_init(path) != 0, "port 70000 is refused");

	unlink(path);
	rmdir(dir);
	printf(failures == 0 ? "DAEMON CONFIG: PASS\n" : "DAEMON CONFIG: FAIL\n");
	return failures == 0 ? 0 : 1;
}
