/*
 * ADR-0339 (#599): the box tells its clients what changed.
 *
 * Against a real daemon, the first half of that promise -- the part a
 * client can check over plain HTTP:
 *
 *   - the answer to an announced read carries X-Cix-Generation,
 *     "<epoch>.<generation>", and GET /v1/events/status agrees with it;
 *   - reading never announces anything;
 *   - a change that succeeds raises the generation of every read its
 *     contract names in x-cix-invalidates;
 *   - a change that is refused raises nothing, because nothing changed;
 *   - an answer that is not an announced read carries no header.
 *
 * The site config is the instrument: putSystemSite invalidates
 * getSystemSite, both are cheap, and a fresh data directory has one.
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

#define TEST_PORT 7851

static char g_data_dir[PATH_MAX];
static int g_failures;

static void fail(const char *what, const char *detail)
{
	fprintf(stderr, "FAIL: %s%s%s\n", what, detail[0] != '\0' ? ": " : "", detail);
	g_failures++;
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/cixd";
	dargv[1] = "--port=7851";
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

/*
 * GET path; on a 200, copies its X-Cix-Generation into gen (empty when
 * absent) and returns 0. Anything else fails the named check.
 */
static int read_generation(const struct cix_client *c, const char *path, char *gen,
                           size_t gen_size, const char *what)
{
	struct cix_response r;

	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "GET", path, NULL, &r) != 0 || r.status != 200) {
		char detail[96];

		snprintf(detail, sizeof(detail), "GET %s answered %d", path, r.status);
		fail(what, detail);
		cix_response_free(&r);
		return -1;
	}
	snprintf(gen, gen_size, "%s", r.generation);
	cix_response_free(&r);
	return 0;
}

/* "<epoch>.<n>" split at the last dot. Returns -1 when malformed. */
static int split_generation(const char *gen, char *epoch, size_t epoch_size,
                            unsigned long long *n)
{
	const char *dot = strrchr(gen, '.');
	char *end;

	if (dot == NULL || dot == gen || (size_t)(dot - gen) >= epoch_size)
		return -1;
	memcpy(epoch, gen, (size_t)(dot - gen));
	epoch[dot - gen] = '\0';
	*n = strtoull(dot + 1, &end, 10);
	return end != dot + 1 && *end == '\0' ? 0 : -1;
}

/* PUT /v1/system/site with body; returns the status. */
static int put_site(const struct cix_client *c, const char *body, char *gen, size_t gen_size)
{
	struct cix_response r;
	int status;

	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "PUT", "/v1/system/site", body, &r) != 0)
		return -1;
	status = r.status;
	snprintf(gen, gen_size, "%s", r.generation);
	cix_response_free(&r);
	return status;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;
	char gen0[48], gen1[48], gen2[48], gen3[48], change_gen[48];
	char epoch0[24] = "", epoch1[24] = "";
	unsigned long long n0 = 0, n2 = 0, n3 = 0;
	int status;

	printf("test_events\n");
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

	/* 1. An announced read says which generation it is. */
	if (read_generation(&client, "/v1/system/site", gen0, sizeof(gen0),
	                    "an announced read answers") == 0 &&
	    split_generation(gen0, epoch0, sizeof(epoch0), &n0) != 0)
		fail("an announced read carries X-Cix-Generation as <epoch>.<n>", gen0);

	/* 2. The status read agrees with the header, and is not itself
	 *    announced (it is derived: it changes on every announcement). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/events/status", NULL, &r) != 0 ||
	    r.status != 200 || r.json == NULL) {
		fail("GET /v1/events/status answers 200 with JSON", "");
	} else {
		const struct json_value *ep = json_object_get(r.json, "epoch");
		const struct json_value *gens = json_object_get(r.json, "generations");
		const struct json_value *site =
		        gens != NULL ? json_object_get(gens, "getSystemSite") : NULL;

		if (ep == NULL || ep->type != JSON_STRING || strcmp(ep->u.string, epoch0) != 0)
			fail("events/status reports the epoch the header carries", epoch0);
		if (site == NULL || (unsigned long long)json_as_number(site) != n0)
			fail("events/status reports getSystemSite at the header's generation", gen0);
		if (gens != NULL && json_object_get(gens, "getSystemStats") != NULL)
			fail("a stream read has no generation", "getSystemStats is listed");
		if (r.generation[0] != '\0')
			fail("a derived read carries no X-Cix-Generation", r.generation);
	}
	cix_response_free(&r);

	/* 3. Reading announces nothing. */
	if (read_generation(&client, "/v1/system/site", gen1, sizeof(gen1),
	                    "a second read answers") == 0 &&
	    strcmp(gen1, gen0) != 0)
		fail("reading does not raise a generation", gen1);

	/* 4. A change that succeeds raises what it invalidates, once. */
	status = put_site(&client,
	                  "{\"instance_name\":\"evtest\",\"site_name\":\"lab\","
	                  "\"domain_suffix\":\"internal\"}",
	                  change_gen, sizeof(change_gen));
	if (status != 200) {
		char detail[32];

		snprintf(detail, sizeof(detail), "status %d", status);
		fail("a valid site change is accepted", detail);
	}
	if (change_gen[0] != '\0')
		fail("a change's own answer carries no X-Cix-Generation", change_gen);
	if (read_generation(&client, "/v1/system/site", gen2, sizeof(gen2),
	                    "the read after a change answers") == 0) {
		if (split_generation(gen2, epoch1, sizeof(epoch1), &n2) != 0 ||
		    strcmp(epoch1, epoch0) != 0)
			fail("the epoch is the same within one daemon's life", gen2);
		else if (n2 != n0 + 1)
			fail("a change raises what it invalidates, exactly once", gen2);
	}

	/* 5. A refused change raises nothing: nothing changed. */
	status = put_site(&client, "{", change_gen, sizeof(change_gen));
	if (status < 400 || status >= 500) {
		char detail[32];

		snprintf(detail, sizeof(detail), "status %d", status);
		fail("an invalid site change is refused", detail);
	}
	if (read_generation(&client, "/v1/system/site", gen3, sizeof(gen3),
	                    "the read after a refusal answers") == 0 &&
	    (split_generation(gen3, epoch1, sizeof(epoch1), &n3) != 0 || n3 != n2))
		fail("a refused change raises nothing", gen3);

	if (stop_daemon(daemon_pid) != 0)
		fail("the daemon exits cleanly", "");
	test_data_dir_cleanup(g_data_dir);
	if (g_failures != 0) {
		printf("FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
