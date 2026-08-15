/*
 * ADR-0159 Phase A end-to-end test: kernel module management (real
 * modprobe/modinfo, see daemon/src/kmod.c) and its persisted
 * kmod-config sibling (default_options + autoload, daemon/src/
 * kmodconfig.c).
 *
 * This project's own dev/build sandbox has no real /usr/bin/modprobe
 * or /usr/bin/modinfo staged at all -- kmod.recipe only stages them
 * onto a real installed image (see main.c's own load_boot_modules()
 * comment for the identical, already-established caveat about
 * modprobe). So POST/DELETE /v1/system/kmod/{name} and GET
 * /v1/system/kmod/{name} can only be proven here to fail *cleanly*
 * (a real 404, never a crash or hang) when the real binary is
 * missing -- their actual success path needs verifying on a real
 * installed box, the same class of sandbox gap CLAUDE.md's own NTP
 * clock_settime() bullet and BPF probe bullet already document. GET
 * /v1/system/kmod (a plain /proc/modules read, no fork at all) and
 * every kmod-config CRUD path need no such caveat -- both are proven
 * fully, for real, right here.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7683
#define PORT_ARG "--port=7683"

static char g_data_dir[PATH_MAX];

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/kanxeod";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/kanxeod", dargv, environ);
		perror("execve build/kanxeod");
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

/* Ground truth from the real, always-present /proc/modules, read
 * directly by the test -- independent of the daemon's own parsing, so
 * a match actually proves the daemon read the real kernel state, not
 * just that it echoed back something plausible-looking. NULL if
 * /proc/modules is somehow empty (won't happen on a real Linux
 * kernel, but this test degrades gracefully rather than asserting a
 * name that doesn't exist). */
static int real_loaded_module_name(char *out, size_t out_size)
{
	FILE *f = fopen("/proc/modules", "r");
	char line[256];
	int found = 0;

	if (f == NULL)
		return -1;
	if (fgets(line, sizeof(line), f) != NULL) {
		char *sp = strchr(line, ' ');

		if (sp != NULL) {
			size_t len = (size_t)(sp - line);

			if (len < out_size) {
				memcpy(out, line, len);
				out[len] = '\0';
				found = 1;
			}
		}
	}
	fclose(f);
	return found ? 0 : -1;
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;
	char real_name[64];
	int have_real_name;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

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

	have_real_name = (real_loaded_module_name(real_name, sizeof(real_name)) == 0);

	/* 1. GET /v1/system/kmod (loaded modules) against real
	 * /proc/modules -- the one live-management surface fully provable
	 * in this sandbox, since it's a plain file read, no fork. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/system/kmod", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/system/kmod: expected 200, got %d\n", r.status);
		ok = 0;
	} else if (have_real_name) {
		const struct json_value *arr = json_object_get(r.json, "modules");
		int found = 0;
		size_t i;

		if (arr == NULL || arr->type != JSON_ARRAY) {
			fprintf(stderr, "FAIL: GET /v1/system/kmod: no modules array\n");
			ok = 0;
		} else {
			for (i = 0; i < arr->u.array.count; i++) {
				const char *n = json_as_string(json_object_get(arr->u.array.items[i], "name"));

				if (n != NULL && strcmp(n, real_name) == 0)
					found = 1;
			}
			if (!found) {
				fprintf(stderr,
				        "FAIL: GET /v1/system/kmod: real module '%s' (from /proc/modules "
				        "directly) not reported\n",
				        real_name);
				ok = 0;
			}
		}
	}
	kx_response_free(&r);

	/* 2. kmod-config: PUT with both fields, GET (via list), round-trip
	 * the options object, DELETE, re-DELETE 404. Real daemon state,
	 * fully testable with no real modprobe/modinfo involved at all. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "PUT", "/v1/system/kmod-config/e1000e",
	                              "{\"default_options\":{\"debug\":\"1\",\"mode\":\"auto\"},"
	                              "\"autoload\":true}",
	                              &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: PUT kmod-config e1000e: expected 200, got %d\n", r.status);
		ok = 0;
	} else if (ok) {
		const char *name = json_as_string(json_object_get(r.json, "name"));
		const struct json_value *opts = json_object_get(r.json, "default_options");
		const struct json_value *autoload = json_object_get(r.json, "autoload");

		if (name == NULL || strcmp(name, "e1000e") != 0) {
			fprintf(stderr, "FAIL: PUT kmod-config: wrong/missing name in response\n");
			ok = 0;
		}
		if (opts == NULL || opts->type != JSON_OBJECT || opts->u.object.count != 2) {
			fprintf(stderr, "FAIL: PUT kmod-config: default_options didn't round-trip\n");
			ok = 0;
		}
		if (autoload == NULL || autoload->type != JSON_BOOL || autoload->u.boolean != 1) {
			fprintf(stderr, "FAIL: PUT kmod-config: autoload didn't round-trip as true\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* Partial update: only autoload, options must survive untouched
	 * (read-modify-write, matching daemon-config's own established PUT
	 * shape). */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "PUT", "/v1/system/kmod-config/e1000e",
	                              "{\"autoload\":false}", &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: PUT kmod-config e1000e (autoload only): expected 200, got %d\n",
		        r.status);
		ok = 0;
	} else if (ok) {
		const struct json_value *opts = json_object_get(r.json, "default_options");
		const struct json_value *autoload = json_object_get(r.json, "autoload");

		if (opts == NULL || opts->type != JSON_OBJECT || opts->u.object.count != 2) {
			fprintf(stderr,
			        "FAIL: PUT kmod-config (autoload only): default_options was not "
			        "preserved\n");
			ok = 0;
		}
		if (autoload == NULL || autoload->type != JSON_BOOL || autoload->u.boolean != 0) {
			fprintf(stderr, "FAIL: PUT kmod-config (autoload only): autoload not updated\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "GET", "/v1/system/kmod-config", NULL, &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: GET /v1/system/kmod-config: expected 200, got %d\n", r.status);
		ok = 0;
	} else if (ok) {
		const struct json_value *arr = json_object_get(r.json, "kmod_config");
		int found = 0;
		size_t i;

		if (arr == NULL || arr->type != JSON_ARRAY) {
			fprintf(stderr, "FAIL: GET /v1/system/kmod-config: no kmod_config array\n");
			ok = 0;
		} else {
			for (i = 0; i < arr->u.array.count; i++) {
				const char *n = json_as_string(json_object_get(arr->u.array.items[i], "name"));

				if (n != NULL && strcmp(n, "e1000e") == 0)
					found = 1;
			}
			if (!found) {
				fprintf(stderr, "FAIL: GET /v1/system/kmod-config: e1000e not listed\n");
				ok = 0;
			}
		}
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "DELETE", "/v1/system/kmod-config/e1000e", NULL, &r) !=
	               0 ||
	           r.status != 204)) {
		fprintf(stderr, "FAIL: DELETE kmod-config e1000e: expected 204, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "DELETE", "/v1/system/kmod-config/e1000e", NULL, &r) !=
	               0 ||
	           r.status != 404)) {
		fprintf(stderr, "FAIL: second DELETE kmod-config e1000e: expected 404, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. Live load/unload/show -- this sandbox has no real modprobe/
	 * modinfo staged at all, so the only provable behavior here is a
	 * clean 404, never a crash/hang. Real success-path verification
	 * needs a real installed box (see the file-level comment above). */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "POST", "/v1/system/kmod/e1000e", "{}", &r) != 0 ||
	           r.status != 404)) {
		fprintf(stderr,
		        "FAIL: POST /v1/system/kmod/e1000e (no real modprobe in this sandbox): "
		        "expected 404, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "GET", "/v1/system/kmod/e1000e", NULL, &r) != 0 ||
	           r.status != 404)) {
		fprintf(stderr,
		        "FAIL: GET /v1/system/kmod/e1000e (no real modinfo in this sandbox): expected "
		        "404, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "DELETE", "/v1/system/kmod/e1000e", NULL, &r) != 0 ||
	           r.status != 404)) {
		fprintf(stderr,
		        "FAIL: DELETE /v1/system/kmod/e1000e (no real modprobe in this sandbox): "
		        "expected 404, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 4. Error paths: invalid names everywhere a name is taken. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "GET", "/v1/system/kmod/bad/name", NULL, &r) != 0 ||
	           r.status != 400)) {
		fprintf(stderr, "FAIL: GET kmod with '/' in name: expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "PUT", "/v1/system/kmod-config/e1000e", "{}", &r) != 0 ||
	           r.status != 400)) {
		fprintf(stderr, "FAIL: PUT kmod-config with neither field: expected 400, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "PUT", "/v1/system/kmod-config/e1000e",
	                              "{\"default_options\":\"not-an-object\"}", &r) != 0 ||
	           r.status != 400)) {
		fprintf(stderr, "FAIL: PUT kmod-config with non-object default_options: expected 400, "
		                "got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	if (ok)
		printf("KMOD RESULT: PASS\n");
	else
		printf("KMOD RESULT: FAIL\n");

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);
	return ok ? 0 : 1;
}
