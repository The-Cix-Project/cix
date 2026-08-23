/*
 * Issue #63: factory reset -- return an installed box to its
 * just-installed state without reinstalling.
 *
 * The property under test is the one that is easy to get wrong and
 * looks fine when you do: the wipe must happen BEFORE any subsystem
 * reads its state. Placed later, the earlier subsystems have already
 * loaded the old world into memory and will write it back on the next
 * change -- the files vanish, the API still answers with everything
 * that was supposed to be gone, and the reset appears to have worked.
 * That is exactly what the first implementation did, so this test
 * drives a real daemon across a real restart rather than checking a
 * status code.
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

#define TEST_PORT 7845
#define PORT_ARG "--port=7845"

static char g_data_dir[PATH_MAX];
static int g_failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		g_failures++;
	}
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/thincd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execve("build/thincd", dargv, environ);
		_exit(127);
	}
	return pid;
}

static int wait_up(const struct thinc_client *c)
{
	int i;

	for (i = 0; i < 100; i++) {
		struct thinc_response r;

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			thinc_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

int main(void)
{
	struct thinc_client client;
	struct thinc_response r;
	pid_t pid;
	int i;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	pid = start_daemon();
	if (pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	thinc_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_up(&client) != 0) {
		fprintf(stderr, "FAIL: daemon never came up\n");
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* Real operator state, of two different kinds, so the test cannot
	 * pass by one subsystem happening to be wiped. */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes", "{\"name\":\"frvol\"}", &r) == 0 &&
	          r.status == 201,
	      "a volume exists before the reset");
	thinc_response_free(&r);
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/images", "{\"name\":\"frimg\"}", &r) == 0 &&
	          r.status == 201,
	      "an image exists before the reset");
	thinc_response_free(&r);

	/*
	 * The confirmation is this install's own name, not a boolean: a
	 * boolean can be sent by a client that misunderstood the call, a
	 * name only by something that looked it up first.
	 */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/system/factory-reset", "{}", &r) == 0 &&
	          r.status == 400,
	      "a factory reset with no confirmation is refused");
	thinc_response_free(&r);
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/system/factory-reset",
	                         "{\"confirm\":\"not-this-box\"}", &r) == 0 &&
	          r.status == 400,
	      "a factory reset with the wrong name is refused");
	thinc_response_free(&r);

	/* Still all there after the refusals -- a refused reset must not
	 * have destroyed anything on its way to saying no. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/volumes", NULL, &r) == 0) {
		const struct json_value *v = json_object_get(r.json, "volumes");

		check(v != NULL && v->type == JSON_ARRAY && v->u.array.count == 1,
		      "a refused reset changed nothing");
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/system/factory-reset",
	                         "{\"confirm\":\"thinc\"}", &r) == 0 &&
	          r.status == 202,
	      "a confirmed factory reset is accepted");
	thinc_response_free(&r);

	/* It arms and reboots; here that means the daemon exits. */
	for (i = 0; i < 100; i++) {
		int st;

		if (waitpid(pid, &st, WNOHANG) == pid)
			break;
		usleep(100000);
	}
	check(i < 100, "the daemon exited to apply the reset");

	/*
	 * The real assertion. A fresh daemon on the SAME data dir must come
	 * up with nothing -- not merely with the files gone, which the
	 * first implementation also achieved while still answering every
	 * query with the old state it had already loaded.
	 */
	pid = start_daemon();
	check(pid > 0 && wait_up(&client) == 0, "the daemon came back after the reset");

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/volumes", NULL, &r) == 0 && r.status == 200) {
		const struct json_value *v = json_object_get(r.json, "volumes");

		check(v != NULL && v->type == JSON_ARRAY && v->u.array.count == 0,
		      "every volume is gone after the reset -- not just its file");
	} else {
		check(0, "every volume is gone after the reset -- not just its file");
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/images", NULL, &r) == 0 && r.status == 200) {
		const struct json_value *v = json_object_get(r.json, "images");

		check(v != NULL && v->type == JSON_ARRAY && v->u.array.count == 0,
		      "every image is gone after the reset");
	} else {
		check(0, "every image is gone after the reset");
	}
	thinc_response_free(&r);

	/* And the box is usable again rather than merely empty. */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes", "{\"name\":\"afterreset\"}", &r) == 0 &&
	          r.status == 201,
	      "the box works normally after a reset");
	thinc_response_free(&r);

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	test_data_dir_cleanup(g_data_dir);

	if (g_failures == 0) {
		printf("FACTORY RESET RESULT: PASS\n");
		return 0;
	}
	printf("FACTORY RESET RESULT: FAIL (%d)\n", g_failures);
	return 1;
}
