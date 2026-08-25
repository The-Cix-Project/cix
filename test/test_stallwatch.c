/*
 * Issue #100: proof that the control plane stopped answering, written
 * by something that is not the control plane.
 *
 * The real incident this comes from: `cixd` on the production host
 * accepted TCP while answering nothing for minutes, then recovered on
 * its own, and the log store had NOTHING from inside the window --
 * because the loop that would record its own silence is the loop that
 * is silent. This test reproduces that shape honestly: SIGSTOP freezes
 * the daemon exactly as a wedge does (accepts still complete in the
 * kernel, nothing is served), and nothing test-only is compiled into
 * the daemon to make it happen.
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

#define TEST_PORT 7650
#define PORT_ARG "--port=7650"
/* The daemon's own threshold is 5s; freezing it for longer than that
 * with margin keeps this deterministic without making it slow. */
#define FREEZE_SECONDS 8

static char g_data_dir[PATH_MAX];

static int wait_for_daemon(const struct cix_client *c, int max_attempts)
{
	int i;
	struct cix_response r;

	for (i = 0; i < max_attempts; i++) {
		if (cix_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			cix_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/cixd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/cixd", dargv, environ);
		perror("execve build/cixd");
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

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;
	int ok = 1;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. A daemon that is merely idle is not a stalled one -- the loop
	 * ticks on its own so silence means something. */
	sleep(2);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/stalls", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/system/stalls, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *stalls = json_object_get(r.json, "stalls");

		if (stalls == NULL || stalls->type != JSON_ARRAY) {
			fprintf(stderr, "FAIL: stalls is not an array\n");
			ok = 0;
		} else if (stalls->u.array.count != 0) {
			fprintf(stderr, "FAIL: an idle daemon reported %d stall(s)\n",
			        (int)stalls->u.array.count);
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 2. Freeze it. This is the real thing, not a simulation: the
	 * process stops running, the kernel keeps completing TCP handshakes
	 * on its listening socket, and nothing is served. */
	kill(daemon_pid, SIGSTOP);
	sleep(FREEZE_SECONDS);
	kill(daemon_pid, SIGCONT);
	sleep(2);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/stalls", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET stalls after the freeze, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *stalls = json_object_get(r.json, "stalls");
		int saw_stall = 0, saw_recovered = 0;
		size_t i;

		if (stalls == NULL || stalls->type != JSON_ARRAY || stalls->u.array.count == 0) {
			fprintf(stderr, "FAIL: the freeze left no record -- which is the whole bug\n");
			ok = 0;
		} else {
			for (i = 0; i < stalls->u.array.count; i++) {
				const struct json_value *s = stalls->u.array.items[i];
				const char *event = json_as_string(json_object_get(s, "event"));
				const char *wchan = json_as_string(json_object_get(s, "wchan"));
				long long seconds = (long long)json_as_number(json_object_get(s, "seconds"));

				if (event == NULL)
					continue;
				if (strcmp(event, "stall") == 0) {
					saw_stall = 1;
					if (seconds < 5) {
						fprintf(stderr, "FAIL: stall recorded at %llds, below the threshold\n",
						        seconds);
						ok = 0;
					}
					/* The kernel's own view of where it was: the single
					 * most useful fact about a wedge, and the one thing
					 * unavailable from inside it. */
					if (wchan == NULL || wchan[0] == '\0') {
						fprintf(stderr, "FAIL: stall record carries no wchan\n");
						ok = 0;
					}
				}
				if (strcmp(event, "recovered") == 0)
					saw_recovered = 1;
			}
			if (!saw_stall) {
				fprintf(stderr, "FAIL: no \"stall\" record after an %d-second freeze\n",
				        FREEZE_SECONDS);
				ok = 0;
			}
			if (!saw_recovered) {
				fprintf(stderr, "FAIL: no \"recovered\" record -- a stall with no end is only "
				                "half the story\n");
				ok = 0;
			}
		}
	}
	cix_response_free(&r);

	/* 3. And the daemon is fine afterwards -- the watchdog observes,
	 * it does not interfere. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/health", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: daemon not healthy after the freeze, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	stop_daemon(daemon_pid);

	/*
	 * 4. The record outlives the daemon that stalled. A wedge is
	 * usually followed by a restart (or a reboot), and a diagnostic
	 * that dies with the process it was diagnosing is no diagnostic.
	 */
	daemon_pid = start_daemon();
	if (daemon_pid < 0 || wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon did not come back\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/stalls", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET stalls after restart, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *stalls = json_object_get(r.json, "stalls");

		if (stalls == NULL || stalls->type != JSON_ARRAY || stalls->u.array.count == 0) {
			fprintf(stderr, "FAIL: the stall record did not survive a restart\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);

	printf("STALLWATCH RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
