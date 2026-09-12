#
# #421, take 2: reproduce the RACING delete+recreate deliberately.
#
# probe-delete-409/1 ran test_pki five times and passed five times, and
# that answered the wrong question: v2.57.124 had already replaced the
# racing recreate with a poll to 404, so it measured the fix rather
# than the cause. It did establish two useful things -- every DELETE
# landed (5 x "rc=0 status=204") and the teardown completes in under a
# second ("durablehost: exited (status 0, signal 0) after 0s"), which
# is why the window is small enough to have shown once in three gate
# runs.
#
# This one does what the FAILING code did: POST, DELETE, then POST the
# same name immediately with nothing in between, 100 times, printing
#
#   - the DELETE's own rc and status (was it delivered at all)
#   - the recreate's status and error body (which 409 message)
#   - GET /v1/containers/racer at the moment of the 409, whose own
#     "status" field is "running" / "deleting" / "stopping" / "exited"
#     and so names the registry state behind the message
#
# The two outcomes that matter:
#
#   container_status "deleting" alongside an "already exists" message
#       -> the daemon contradicts itself, and name_conflict_msg() is
#          reading a teardown_kind that GET's own status does not agree
#          with. A real bug, localised.
#   container_status "running" or "exited"
#       -> the entry genuinely was not in teardown and the message was
#          correct; the remaining question is which path left it there,
#          and the daemon's interleaved stderr answers it.
#
# The daemon's stderr is inherited into this log, so
# "racer: deleting -- shutdown asked of cix-init" marks each
# begin_container_stop() and "racer: exited (status N, signal M) after
# Ns" marks each completion, against the client's own ms timestamps.
#
# Fails on purpose -- the build log is the product.
#
pkg_name="probe-delete-409"
pkg_version="2"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.125.tar.gz"
pkg_sha256="0fd868f0093b8cf95652ba708b0f4dabb91851da3721d79faa9329995abc7485"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils sed grep gawk"
pkg_build_caps="CAP_SYS_ADMIN"
pkg_build_image="cix-builder"

pkg_changelog="2: probe 1 measured the fix instead of the cause -- v2.57.124 had already removed the race it was trying to reproduce. This drives the racing delete+recreate directly, 100 iterations, and prints GET /v1/containers/racer at the moment of each 409 so the registry state behind the message is named rather than inferred."

pkg_build() {
	echo "=== building what the driver needs ==="
	make build/cix-init build/cixd build/daemon_child 2>&1 | tail -3
	echo
	cat > /run/racer.c <<'RACER_EOF'
/*
 * #421: reproduce the racing delete+recreate deliberately, and print
 * the holder's own state at the moment of the 409.
 *
 * probe-delete-409/1 ran test_pki five times and passed five times --
 * which answered the wrong question. v2.57.124 had already replaced the
 * racing recreate with a poll to 404, so that probe measured the fix,
 * not the cause. This does what the FAILING code did: DELETE, then
 * immediately POST the same name, with no wait in between.
 *
 * What each iteration prints:
 *   DELETE rc/status            -- was the request delivered at all
 *   POST status + body          -- which 409 message, if any
 *   GET /v1/containers/<name>   -- the holder's own "status" field,
 *                                  verbatim, which is "running" /
 *                                  "deleting" / "stopping" / "exited"
 *                                  and so names the registry state
 *                                  behind the message
 *
 * "deleting" with an "already exists" message would be the daemon
 * contradicting itself. "running" or "exited" would mean the entry
 * genuinely was not in teardown, and the message was right.
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
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7641
#define PORT_ARG "--port=7641"
#define ITERATIONS 100

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

static const char *BODY =
    "{\"name\":\"racer\",\"image\":\"racetest\","
    "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\","
    "\"cmd\":[\"/bin/daemon_child\",\"20\"]}]}";

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
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
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execve("build/cixd", dargv, environ);
		_exit(127);
	}
	return pid;
}

static int wait_for_daemon(const struct cix_client *c)
{
	int i;

	for (i = 0; i < 200; i++) {
		struct cix_response r;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			cix_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

/* Print GET /v1/containers/racer's status field, and the raw body when
 * it is anything but a plain 404. */
static void dump_holder(const struct cix_client *c, const char *tag)
{
	struct cix_response r;
	const char *st = NULL;

	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "GET", "/v1/containers/racer", NULL, &r) != 0) {
		printf("      %s: GET rc=-1 (no response)\n", tag);
		cix_response_free(&r);
		return;
	}
	if (r.json != NULL)
		st = json_as_string(json_object_get(r.json, "status"));
	printf("      %s: GET status=%d container_status=%s pid=%.0f\n", tag, r.status,
	        st != NULL ? st : "(none)",
	        r.json != NULL ? json_as_number(json_object_get(r.json, "pid")) : -1.0);
	if (r.status != 404 && r.body != NULL)
		printf("      %s: body=%.400s\n", tag, r.body);
	cix_response_free(&r);
}

static void wait_gone(const struct cix_client *c)
{
	int i;

	for (i = 0; i < 200; i++) {
		struct cix_response r;
		int status = 0;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", "/v1/containers/racer", NULL, &r) == 0)
			status = r.status;
		cix_response_free(&r);
		if (status == 404)
			return;
		usleep(100000);
	}
	printf("      wait_gone: racer STILL PRESENT after 20s\n");
}

int main(void)
{
	struct cix_client client;
	pid_t daemon_pid;
	long t0;
	int i, conflicts = 0, delete_failures = 0;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/racetest/v1/rootfs",
	          g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/racetest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0)
			return 1;
	}
	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		printf("could not stage racetest image\n");
		return 1;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;
	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client) != 0) {
		printf("daemon never came up\n");
		kill(daemon_pid, SIGTERM);
		return 1;
	}
	t0 = now_ms();

	for (i = 1; i <= ITERATIONS; i++) {
		struct cix_response r;
		int rc, del_rc, del_status;

		/* create */
		memset(&r, 0, sizeof(r));
		rc = cix_client_request(&client, "POST", "/v1/containers", BODY, &r);
		if (rc != 0 || r.status != 201) {
			printf("[%4ldms] iter %d: initial POST rc=%d status=%d body=%.200s\n",
			        now_ms() - t0, i, rc, rc == 0 ? r.status : -1,
			        rc == 0 && r.body != NULL ? r.body : "(none)");
			cix_response_free(&r);
			dump_holder(&client, "after failed create");
			wait_gone(&client);
			continue;
		}
		cix_response_free(&r);

		/* delete, outcome recorded */
		memset(&r, 0, sizeof(r));
		del_rc = cix_client_request(&client, "DELETE", "/v1/containers/racer", NULL, &r);
		del_status = del_rc == 0 ? r.status : -1;
		cix_response_free(&r);
		if (del_rc != 0 || del_status != 204) {
			delete_failures++;
			printf("[%4ldms] iter %d: DELETE rc=%d status=%d  <-- DID NOT LAND\n",
			        now_ms() - t0, i, del_rc, del_status);
		}

		/* recreate IMMEDIATELY -- the racing sequence */
		memset(&r, 0, sizeof(r));
		rc = cix_client_request(&client, "POST", "/v1/containers", BODY, &r);
		if (rc != 0 || r.status != 201) {
			const char *err = r.json != NULL ? json_as_string(json_object_get(r.json, "error"))
			                                 : NULL;

			conflicts++;
			printf("[%4ldms] iter %d: recreate rc=%d status=%d err=%s (DELETE was rc=%d "
			        "status=%d)\n",
			        now_ms() - t0, i, rc, rc == 0 ? r.status : -1, err != NULL ? err : "(none)",
			        del_rc, del_status);
			cix_response_free(&r);
			dump_holder(&client, "holder at the 409");
			wait_gone(&client);
			continue;
		}
		cix_response_free(&r);

		/* tidy up for the next iteration */
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/containers/racer", NULL, &r);
		cix_response_free(&r);
		wait_gone(&client);
	}

	printf("\n=== %d iterations: %d recreate conflicts, %d deletes that did not land ===\n",
	        ITERATIONS, conflicts, delete_failures);

	kill(daemon_pid, SIGTERM);
	waitpid(daemon_pid, NULL, 0);
	test_data_dir_cleanup(g_data_dir);
	return 0;
}
RACER_EOF
	echo "=== compiling the driver ==="
	tcc -Wall -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 -Iinclude -Inetplane/include \
	    -Iclient/include -Idaemon/include -Itest \
	    /run/racer.c test/test_image_fixture.c client/src/httpclient.c daemon/src/json.c \
	    -o /run/racer
	echo
	echo "=== 100 racing delete+recreate rounds ==="
	/run/racer 2>&1
	echo "=== driver exit: $? ==="
	echo
	echo "PROBE COMPLETE -- failing deliberately so the log is kept"
	return 1
}

pkg_install() {
	:
}
