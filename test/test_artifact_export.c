/*
 * Issue #129: proves the artifact export resource over real HTTP, for
 * BOTH subjects it serves -- a hostbuild package's harvested output
 * (ADR-0056) and an image's rootfs (issue #126, which shipped with no
 * test of its own; generalizing that code is what closed the gap).
 *
 * The hostbuild case is the one that forced the endpoint to exist: a
 * hostbuild artifact lives in a plain host directory that is
 * deliberately never container-visible, so GET /v1/containers/{n}/files
 * cannot reach it and there was previously no way to retrieve it at
 * all -- the self-hosted kernel existed only as bytes on one box's
 * disk, unbackupable and unpushable.
 *
 * Covered here: export of a seeded hostbuild artifact, its bytes
 * arriving intact through the chunked download, an image export over
 * the same state machine, that the two kinds do NOT cross-match on a
 * shared name, 404 for an unknown package, and 404 for a download with
 * no ready export behind it.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7686
#define PORT_ARG "--port=7686"

/* The seeded hostbuild's own name and version -- deliberately also
 * used as an IMAGE name below, to prove the two kinds stay separate. */
#define HB_NAME "faux"
#define HB_VERSION "1.0-1"

static char g_data_dir[PATH_MAX];
static int g_failures;

static void fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	printf("FAIL: ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	g_failures++;
}

static int run_cmd(const char *fmt, ...)
{
	char cmd[2048];
	va_list ap;
	int rc;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	rc = system(cmd);
	return (rc == 0) ? 0 : -1;
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

/*
 * Seeds a hostbuild package exactly as a real one would be left on
 * disk after a successful build: an INSTALLED row in the persisted
 * registry (save_state()'s own array-of-objects shape) plus the
 * harvested output under rebuildable/artifacts/<name>/. Written
 * before the daemon starts, so pkg_init() loads it as real state.
 *
 * Seeding rather than running a real hostbuild is deliberate: a real
 * one needs a build image and minutes of compilation, none of which
 * this endpoint's behaviour depends on -- what it depends on is an
 * installed row plus a readable artifact directory, which is exactly
 * what this produces.
 */
static int seed_hostbuild(void)
{
	char path[PATH_MAX];
	FILE *f;

	if (run_cmd("mkdir -p '%s/rebuildable/pkg' '%s/rebuildable/artifacts/" HB_NAME "'",
	            g_data_dir, g_data_dir) != 0)
		return -1;

	/* Two files, one of them nested, so the archive proves it carries a
	 * tree rather than a single blob. */
	if (run_cmd("printf 'fake-bzImage-bytes\\n' > '%s/rebuildable/artifacts/" HB_NAME "/bzImage'",
	            g_data_dir) != 0)
		return -1;
	if (run_cmd("mkdir -p '%s/rebuildable/artifacts/" HB_NAME "/lib/modules/1.2.3' && "
	            "printf 'dep-line\\n' > '%s/rebuildable/artifacts/" HB_NAME
	            "/lib/modules/1.2.3/modules.dep'",
	            g_data_dir, g_data_dir) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/rebuildable/pkg/pkg_installed.json", g_data_dir);
	f = fopen(path, "w");
	if (f == NULL) {
		perror(path);
		return -1;
	}
	fprintf(f,
	        "[{\"name\":\"" HB_NAME "\",\"image\":\"__hostbuild\",\"version\":\"" HB_VERSION
	        "\",\"depends\":\"\",\"files\":[\"bzImage\"]}]\n");
	fclose(f);
	return 0;
}

/* Polls GET .../export until it leaves "building". Returns the final
 * state string in out_state, and size_bytes via out_size. */
static int poll_export(struct cix_client *c, const char *path, char *out_state, size_t state_size,
                       long long *out_size)
{
	int i;

	for (i = 0; i < 100; i++) {
		struct cix_response r;
		struct json_value *root;
		const char *st;

		if (cix_client_request(c, "GET", path, NULL, &r) != 0)
			return -1;
		root = json_parse(r.body, r.body_len);
		if (root == NULL) {
			cix_response_free(&r);
			return -1;
		}
		st = json_as_string(json_object_get(root, "state"));
		snprintf(out_state, state_size, "%s", st != NULL ? st : "");
		if (out_size != NULL) {
			const struct json_value *sz = json_object_get(root, "size_bytes");

			*out_size = (sz != NULL) ? (long long)json_as_number(sz) : -1;
		}
		json_free(root);
		cix_response_free(&r);
		if (strcmp(out_state, "building") != 0)
			return 0;
		usleep(100000);
	}
	return -1;
}

int main(void)
{
	struct cix_client c;
	pid_t daemon_pid;
	struct cix_response r;
	char state[32];
	long long size = -1;
	char dl_path[256];
	char out_tar[PATH_MAX];

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0) {
		fprintf(stderr, "could not create test data dir\n");
		return 1;
	}
	if (seed_hostbuild() != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	cix_client_init(&c, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&c, 100) != 0) {
		fprintf(stderr, "daemon did not become ready\n");
		stop_daemon(daemon_pid);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* --- the seeded hostbuild really is loaded as installed state --- */
	if (cix_client_request(&c, "GET", "/v1/pkg/" HB_NAME "@__hostbuild", NULL, &r) == 0) {
		struct json_value *root = json_parse(r.body, r.body_len);
		const char *st = json_as_string(json_object_get(root, "state"));

		if (r.status != 200 || st == NULL || strcmp(st, "installed") != 0)
			fail("seeded hostbuild not loaded as installed (status=%d state=%s)", r.status,
			     st != NULL ? st : "(null)");
		json_free(root);
		cix_response_free(&r);
	} else {
		fail("GET /v1/pkg/" HB_NAME "@__hostbuild failed");
	}

	/* --- download with no ready export is a 404, not a 500 --- */
	if (cix_client_request(&c, "GET", "/v1/pkg/" HB_NAME "/artifact/export/download?offset=0&length=16",
	                       NULL, &r) == 0) {
		if (r.status != 404)
			fail("download with no export: expected 404, got %d", r.status);
		cix_response_free(&r);
	} else {
		fail("download-with-no-export request failed");
	}

	/* --- unknown package is a 404 --- */
	if (cix_client_request(&c, "POST", "/v1/pkg/nosuchthing/artifact/export", "", &r) == 0) {
		if (r.status != 404)
			fail("export of unknown package: expected 404, got %d", r.status);
		cix_response_free(&r);
	} else {
		fail("unknown-package export request failed");
	}

	/* --- the real thing: export the hostbuild artifact --- */
	if (cix_client_request(&c, "POST", "/v1/pkg/" HB_NAME "/artifact/export", "", &r) == 0) {
		if (r.status != 202)
			fail("hostbuild export POST: expected 202, got %d", r.status);
		cix_response_free(&r);
	} else {
		fail("hostbuild export POST failed");
	}
	if (poll_export(&c, "/v1/pkg/" HB_NAME "/artifact/export", state, sizeof(state), &size) != 0) {
		fail("hostbuild export never left building");
	} else if (strcmp(state, "ready") != 0) {
		fail("hostbuild export state=%s, expected ready", state);
	} else if (size <= 0) {
		fail("hostbuild export ready but size_bytes=%lld", size);
	} else {
		/* --- pull the bytes and prove they are the seeded tree --- */
		FILE *out;
		long long got = 0;

		snprintf(out_tar, sizeof(out_tar), "%s/pulled.tar.gz", g_data_dir);
		out = fopen(out_tar, "wb");
		if (out == NULL) {
			fail("cannot open %s", out_tar);
		} else {
			while (got < size) {
				long long want = size - got;

				if (want > 65536)
					want = 65536;
				snprintf(dl_path, sizeof(dl_path),
				         "/v1/pkg/" HB_NAME
				         "/artifact/export/download?offset=%lld&length=%lld",
				         got, want);
				if (cix_client_request(&c, "GET", dl_path, NULL, &r) != 0 ||
				    r.status != 200) {
					fail("download chunk at offset %lld failed", got);
					cix_response_free(&r);
					break;
				}
				fwrite(r.body, 1, r.body_len, out);
				got += (long long)r.body_len;
				cix_response_free(&r);
			}
			fclose(out);
			if (got != size)
				fail("downloaded %lld bytes, size_bytes said %lld", got, size);

			/* A real gzip holding both seeded files, at top level
			 * (tar -C <src> . ), which is the shape a consuming host
			 * extracts straight into place. */
			if (run_cmd("gzip -t '%s' 2>/dev/null", out_tar) != 0)
				fail("downloaded artifact is not a valid gzip");
			if (run_cmd("tar -tzf '%s' 2>/dev/null | grep -q '^\\./bzImage$'", out_tar) != 0)
				fail("downloaded artifact does not contain ./bzImage");
			if (run_cmd("tar -tzf '%s' 2>/dev/null | "
			            "grep -q '^\\./lib/modules/1.2.3/modules.dep$'",
			            out_tar) != 0)
				fail("downloaded artifact does not contain the nested module file");
			/* Content, not just presence -- an archive that lists the
			 * right names while carrying the wrong bytes is exactly
			 * the class of bug #122 was. */
			if (run_cmd("tar -xzOf '%s' ./bzImage 2>/dev/null | grep -q '^fake-bzImage-bytes$'",
			            out_tar) != 0)
				fail("./bzImage content did not survive the round trip");
		}
	}

	/*
	 * --- the two kinds do not cross-match on a shared name ---
	 * An image named exactly like the hostbuild package must report
	 * its own export state ("none"), not inherit the ready hostbuild
	 * export sitting in the shared state machine.
	 */
	if (cix_client_request(&c, "GET", "/v1/images/" HB_NAME "/export", NULL, &r) == 0) {
		struct json_value *root = json_parse(r.body, r.body_len);
		const char *st = json_as_string(json_object_get(root, "state"));

		if (st == NULL || strcmp(st, "none") != 0)
			fail("image export state leaked from the hostbuild export: state=%s",
			     st != NULL ? st : "(null)");
		json_free(root);
		cix_response_free(&r);
	} else {
		fail("image export GET failed");
	}
	if (cix_client_request(&c, "GET",
	                       "/v1/images/" HB_NAME "/export/download?offset=0&length=16", NULL,
	                       &r) == 0) {
		if (r.status != 404)
			fail("image download must not serve the hostbuild export (got %d)", r.status);
		cix_response_free(&r);
	} else {
		fail("image export download request failed");
	}

	/* --- the image kind still works over the shared machine (#126) --- */
	if (cix_client_request(&c, "POST", "/v1/images",
	                       "{\"name\":\"expimg\",\"description\":\"export test\"}", &r) == 0) {
		if (r.status != 201 && r.status != 409)
			fail("could not create test image (status %d)", r.status);
		cix_response_free(&r);
	}
	if (cix_client_request(&c, "POST", "/v1/images/expimg/export", "", &r) == 0) {
		if (r.status != 202)
			fail("image export POST: expected 202, got %d", r.status);
		cix_response_free(&r);
	} else {
		fail("image export POST failed");
	}
	size = -1;
	if (poll_export(&c, "/v1/images/expimg/export", state, sizeof(state), &size) != 0)
		fail("image export never left building");
	else if (strcmp(state, "ready") != 0)
		fail("image export state=%s, expected ready", state);
	else if (size <= 0)
		fail("image export ready but size_bytes=%lld", size);

	if (stop_daemon(daemon_pid) != 0)
		fail("daemon did not exit cleanly on SIGTERM");
	test_data_dir_cleanup(g_data_dir);

	if (g_failures > 0) {
		printf("test_artifact_export: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_artifact_export: all checks passed\n");
	return 0;
}
