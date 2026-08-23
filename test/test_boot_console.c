/*
 * Issue #24: the installed system's own boot console, over REST.
 *
 * An installed thinC host boots through systemd-boot, and every loader
 * entry carried a hardcoded `console=tty0 console=ttyS0`. Real hardware
 * needs a serial console at a particular baud, or a framebuffer
 * argument to produce any output at all, or exactly the opposite when
 * the framebuffer is the problem -- and none of it was reachable
 * without reinstalling.
 *
 * The daemon under test is pointed at a throwaway entries directory
 * (--test-esp-entries-dir=), so this exercises the REAL rewrite against
 * a real file rather than a mocked one, without going anywhere near a
 * machine's actual ESP.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7649
#define PORT_ARG "--port=7649"

static char g_data_dir[PATH_MAX];
static char g_esp_dir[PATH_MAX];

static int wait_for_daemon(const struct kx_client *c, int max_attempts)
{
	int i;
	struct kx_response r;

	for (i = 0; i < max_attempts; i++) {
		if (kx_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			kx_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[5];
	static char data_dir_arg[PATH_MAX + 11];
	static char esp_arg[PATH_MAX + 24];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	snprintf(esp_arg, sizeof(esp_arg), "--test-esp-entries-dir=%s", g_esp_dir);
	dargv[0] = "build/thincd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = esp_arg;
	dargv[4] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/thincd", dargv, environ);
		perror("execve build/thincd");
		_exit(127);
	}
	return pid;
}

static int stop_daemon(pid_t pid)
{
	int status;

	kill(pid, SIGTERM);
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* The entry file exactly as populate_esp()/the A/B update path write it. */
static const char *ENTRY_BEFORE =
    "title thinC (A)\n"
    "sort-key thinc\n"
    "version 1\n"
    "linux /thinc-bzImage-a\n"
    "options console=tty0 console=ttyS0 root=/dev/vda2 rw init=/bin/thincd -- --init-mode "
    "--slot=a --bind=192.168.15.95\n";

static int write_entry(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");

	if (f == NULL)
		return -1;
	fputs(content, f);
	fclose(f);
	return 0;
}

static int read_entry(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (f == NULL)
		return -1;
	n = fread(buf, 1, cap - 1, f);
	fclose(f);
	buf[n] = '\0';
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	struct kx_response r;
	char entry_path[PATH_MAX];
	char after[4096];
	int ok = 1;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_esp_dir, sizeof(g_esp_dir), "%s/esp-entries", g_data_dir);
	if (mkdir(g_esp_dir, 0755) != 0) {
		perror("mkdir esp dir");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	snprintf(entry_path, sizeof(entry_path), "%s/thinc-a+3.conf", g_esp_dir);
	if (write_entry(entry_path, ENTRY_BEFORE) != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	kx_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. The default is exactly what was hardcoded before this existed,
	 * so an install that never touches the setting boots identically. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/system/boot-console", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET boot-console, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *cfg = json_object_get(r.json, "config");
		const char *rendered = cfg != NULL ? json_as_string(json_object_get(cfg, "rendered")) : NULL;

		if (rendered == NULL || strcmp(rendered, "console=tty0 console=ttyS0") != 0) {
			fprintf(stderr, "FAIL: default rendered='%s'\n", rendered != NULL ? rendered : "(null)");
			ok = 0;
		}
		/* The live entry is reported, not just the intent: on a box
		 * whose ESP is not writable those two differ, and that
		 * difference is the thing worth seeing. */
		if (json_object_get(r.json, "loader_entries") == NULL) {
			fprintf(stderr, "FAIL: GET boot-console did not report the loader entries\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* 2. A real change: serial console at a real baud, plus a
	 * framebuffer argument -- and the entry on disk is rewritten. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "PUT", "/v1/system/boot-console",
	                       "{\"consoles\":[\"ttyS0,115200n8\"],\"extra\":\"nomodeset\"}", &r) != 0 ||
	    r.status != 200 ||
	    (long)json_as_number(json_object_get(r.json, "loader_entries_updated")) != 1) {
		fprintf(stderr, "FAIL: PUT boot-console, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	if (read_entry(entry_path, after, sizeof(after)) != 0) {
		fprintf(stderr, "FAIL: could not read the rewritten entry\n");
		ok = 0;
	} else {
		/*
		 * Everything from root= onward has to survive byte for byte:
		 * that half of the line is what decides whether the machine
		 * boots at all, and this endpoint has no business touching it.
		 */
		if (strstr(after, "options console=ttyS0,115200n8 nomodeset root=/dev/vda2 rw "
		                  "init=/bin/thincd -- --init-mode --slot=a --bind=192.168.15.95") == NULL) {
			fprintf(stderr, "FAIL: rewritten entry is wrong:\n%s\n", after);
			ok = 0;
		}
		if (strstr(after, "console=tty0") != NULL) {
			fprintf(stderr, "FAIL: the old console survived the rewrite\n");
			ok = 0;
		}
		if (strstr(after, "title thinC (A)") == NULL || strstr(after, "linux /thinc-bzImage-a") == NULL) {
			fprintf(stderr, "FAIL: the rewrite lost the rest of the entry\n");
			ok = 0;
		}
	}

	/* 3. It survives a daemon restart -- a boot setting that forgets
	 * itself on restart is worse than no setting. */
	stop_daemon(daemon_pid);
	daemon_pid = start_daemon();
	if (daemon_pid < 0 || wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon did not come back\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/system/boot-console", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET after restart, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *cfg = json_object_get(r.json, "config");
		const char *rendered = cfg != NULL ? json_as_string(json_object_get(cfg, "rendered")) : NULL;

		if (rendered == NULL || strcmp(rendered, "console=ttyS0,115200n8 nomodeset") != 0) {
			fprintf(stderr, "FAIL: setting did not survive restart: '%s'\n",
			        rendered != NULL ? rendered : "(null)");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/*
	 * 4. Anything that could split the boot line, or smuggle in a
	 * parameter that decides how the machine boots, is refused --
	 * escaped-and-accepted would be the wrong answer for a file that
	 * decides whether this box comes back at all.
	 */
	{
		static const char *const bad[] = {
			"{\"consoles\":[\"tty0 root=/dev/attacker\"]}",
			"{\"consoles\":[\"tty0\\nrogue\"]}",
			"{\"extra\":\"init=/bin/sh\"}",
			"{\"extra\":\"root=/dev/attacker\"}",
			"{\"extra\":\"console=tty1\"}",
			"{\"consoles\":[\"/dev/ttyS0\"]}",
		};
		size_t i;

		for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "PUT", "/v1/system/boot-console", bad[i], &r) != 0 ||
			    r.status != 400) {
				fprintf(stderr, "FAIL: %s should be 400, got %d\n", bad[i], r.status);
				ok = 0;
			}
			kx_response_free(&r);
		}
	}

	/* And the refused attempts changed nothing. */
	if (read_entry(entry_path, after, sizeof(after)) == 0 &&
	    strstr(after, "options console=ttyS0,115200n8 nomodeset root=/dev/vda2") == NULL) {
		fprintf(stderr, "FAIL: a refused request still altered the entry:\n%s\n", after);
		ok = 0;
	}

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);

	printf("BOOT CONSOLE RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
