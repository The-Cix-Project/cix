/*
 * ADR-0141 prerequisite: proves the one-time automatic upgrade path
 * for a box running the OLD flat layout (every state/rebuildable path
 * a direct child of the data dir) onto the NEW grouped one (STATE_DIR/
 * REBUILDABLE_DIR). Real content is created through the real daemon
 * and real API first -- not hand-written JSON that could drift from
 * the actual schema -- then manually relocated back to where it would
 * sit on an already-installed, not-yet-upgraded box, before a second
 * daemon instance (same data dir) is started to prove
 * migrate_flat_layout_to_grouped() moves it back into place with the
 * content intact and the daemon genuinely reading from the new
 * location afterward, not just a file happening to exist there.
 *
 * Covers both migration shapes this ADR's own review found: a whole
 * DIRECTORY (pki/, containing the bootstrapped CA's own key/cert
 * files plus its own nested pki_certs.json) and a plain top-level JSON
 * FILE (site_config.json).
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7679

static char g_data_dir[PATH_MAX];

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
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/thincd";
	dargv[1] = "--port=7679";
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

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

static int path_exists(const char *path)
{
	struct stat st;

	return lstat(path, &st) == 0;
}

/* mkdir -p equivalent, only ever needed here for one level below
 * g_data_dir, so a plain single mkdir() suffices -- g_data_dir itself
 * already exists. */
static int move_back_to_flat(const char *grouped_parent, const char *basename, const char *flat_dst)
{
	char src[PATH_MAX], dst[PATH_MAX];

	snprintf(src, sizeof(src), "%s/%s", grouped_parent, basename);
	snprintf(dst, sizeof(dst), "%s/%s", g_data_dir, flat_dst);
	if (rename(src, dst) != 0) {
		fprintf(stderr, "move_back_to_flat: rename %s -> %s failed: %s\n", src, dst, strerror(errno));
		return -1;
	}
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;
	char state_dir[PATH_MAX], flat_pki[PATH_MAX], grouped_pki[PATH_MAX];
	char flat_site[PATH_MAX], grouped_site[PATH_MAX];
	char cert_pem_before[8192] = { 0 };
	char instance_name_before[128] = { 0 };

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	/* Round 1: a completely fresh daemon -- creates the NEW grouped
	 * layout from scratch (today's real, current behavior), with real
	 * content via the real API. */
	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	kx_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon (round 1) never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pki/ca", "{\"common_name\":\"Upgrade Test CA\"}", &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST /v1/pki/ca, status=%d\n", r.status);
		ok = 0;
	}
	if (ok) {
		const char *pem = json_as_string(json_object_get(r.json, "cert_pem"));

		if (pem == NULL) {
			fprintf(stderr, "FAIL: POST /v1/pki/ca returned no cert_pem\n");
			ok = 0;
		} else {
			snprintf(cert_pem_before, sizeof(cert_pem_before), "%s", pem);
		}
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "PUT", "/v1/system/site",
	                              "{\"instance_name\":\"upgrade-test-box\",\"domain_suffix\":\"internal\"}", &r) !=
	               0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: PUT /v1/system/site, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);
	snprintf(instance_name_before, sizeof(instance_name_before), "upgrade-test-box");

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon (round 1) did not exit cleanly\n");
		ok = 0;
	}

	/* Confirm the grouped layout is really what round 1 produced (not
	 * assumed) before simulating the old-box scenario against it. */
	snprintf(state_dir, sizeof(state_dir), "%s/state", g_data_dir);
	snprintf(grouped_pki, sizeof(grouped_pki), "%s/pki", state_dir);
	snprintf(grouped_site, sizeof(grouped_site), "%s/site_config.json", state_dir);
	if (ok && (!path_exists(grouped_pki) || !path_exists(grouped_site))) {
		fprintf(stderr, "FAIL: round 1 did not produce the expected grouped layout (%s / %s)\n", grouped_pki,
		        grouped_site);
		ok = 0;
	}

	/* Simulate "an already-installed box that hasn't upgraded yet":
	 * move both back to the pre-ADR-0141 flat paths, same shape
	 * migrate_flat_layout_to_grouped() itself expects to find. */
	if (ok && move_back_to_flat(state_dir, "pki", "pki") != 0)
		ok = 0;
	if (ok && move_back_to_flat(state_dir, "site_config.json", "site_config.json") != 0)
		ok = 0;
	snprintf(flat_pki, sizeof(flat_pki), "%s/pki", g_data_dir);
	snprintf(flat_site, sizeof(flat_site), "%s/site_config.json", g_data_dir);
	if (ok && (!path_exists(flat_pki) || !path_exists(flat_site))) {
		fprintf(stderr, "FAIL: could not stage the old flat layout for round 2\n");
		ok = 0;
	}
	if (ok && (path_exists(grouped_pki) || path_exists(grouped_site))) {
		fprintf(stderr, "FAIL: grouped paths still present after staging the flat layout\n");
		ok = 0;
	}

	/* Round 2: same data dir, old flat layout staged -- the daemon's
	 * own startup migration must move it back into place before
	 * anything reads it. */
	if (ok) {
		daemon_pid = start_daemon();
		if (daemon_pid < 0) {
			ok = 0;
		} else if (wait_for_daemon(&client, 50) != 0) {
			fprintf(stderr, "FAIL: daemon (round 2) never accepted connections\n");
			kill(daemon_pid, SIGKILL);
			waitpid(daemon_pid, NULL, 0);
			ok = 0;
		}
	}

	if (ok && (path_exists(flat_pki) || path_exists(flat_site))) {
		fprintf(stderr, "FAIL: old flat paths still present after round 2 startup (migration did not run)\n");
		ok = 0;
	}
	if (ok && (!path_exists(grouped_pki) || !path_exists(grouped_site))) {
		fprintf(stderr, "FAIL: grouped paths not restored after round 2 startup\n");
		ok = 0;
	}

	/* The real proof: the daemon is genuinely reading from the new
	 * location, not just leaving a file sitting there unread. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "GET", "/v1/pki/ca", NULL, &r) != 0 || r.status != 200)) {
		fprintf(stderr, "FAIL: GET /v1/pki/ca after migration, status=%d\n", r.status);
		ok = 0;
	}
	if (ok) {
		const char *pem = json_as_string(json_object_get(r.json, "cert_pem"));

		if (pem == NULL || strcmp(pem, cert_pem_before) != 0) {
			fprintf(stderr, "FAIL: cert_pem after migration does not match the original\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "GET", "/v1/system/site", NULL, &r) != 0 || r.status != 200)) {
		fprintf(stderr, "FAIL: GET /v1/system/site after migration, status=%d\n", r.status);
		ok = 0;
	}
	if (ok) {
		const char *name = json_as_string(json_object_get(r.json, "instance_name"));

		if (name == NULL || strcmp(name, instance_name_before) != 0) {
			fprintf(stderr, "FAIL: instance_name after migration does not match the original\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	if (ok)
		printf("LAYOUT UPGRADE RESULT: PASS\n");
	else
		printf("LAYOUT UPGRADE RESULT: FAIL\n");

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);
	return ok ? 0 : 1;
}
