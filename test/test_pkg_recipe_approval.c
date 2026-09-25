/*
 * A published recipe version may gain its artifact checksum, and
 * nothing else.
 *
 * pkg.c's artifact tier is only entered when a recipe declares
 * pkg_artifact_sha256=, and that checksum cannot be known until after
 * the version has been built and published -- strictly later than the
 * recipe has to exist. With publication immutable, the approval could
 * never be added at all: on the first real box, 12 of 37 installed
 * packages sat in the artifact cache unapproved, so every other host
 * rebuilt them from source while a verified artifact went unread.
 *
 * The relaxation has to be narrow or it is just mutability. What is
 * asserted here is the boundary, not the happy path:
 *
 *   - adding the checksum to an otherwise identical recipe: accepted
 *   - changing an existing checksum: refused (one version must never
 *     name two byte sequences -- the #145 failure)
 *   - adding the checksum WHILE changing anything else: refused
 *   - republishing an identical recipe: still refused
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

#define TEST_PORT 7847
#define PORT_ARG "--port=7847"

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
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execve("build/cixd", dargv, environ);
		_exit(127);
	}
	return pid;
}

/*
 * A minimal but real recipe. `extra` is a metadata line appended
 * verbatim inside the metadata block, so a caller can add an artifact
 * approval; `depends` names a runtime package, or is empty for none.
 *
 * CPDL since cix#516. The approval moved with it: a shell recipe
 * carries it as pkg_artifact_sha256=, a PBS one as an "artifact_sha256"
 * entry in metadata, and this test is about the rule that an approval
 * is written once and never replaced -- which is a property of the
 * daemon, not of either syntax.
 */
static char *recipe_text(const char *extra, const char *depends)
{
	static char buf[2048];
	char runtime[256] = "";

	if (depends != NULL && depends[0] != '\0')
		snprintf(runtime, sizeof(runtime),
		         "        runtime {\n"
		         "            package \"%s\"\n"
		         "        }\n",
		         depends);

	snprintf(buf, sizeof(buf),
	         "package \"approvaltest\" {\n"
	         "    version \"1.0\"\n"
	         "    release 1\n"
	         "    format \"cixpkg\"\n"
	         "\n"
	         "    sources {\n"
	         "        main \"approvaltest\" {\n"
	         "            url \"https://example.invalid/approvaltest-1.0.tar.gz\"\n"
	         "            sha256 \"%064d\"\n"
	         "        }\n"
	         "    }\n"
	         "\n"
	         "    requires {\n"
	         "        build {\n"
	         "            tool \"bash\"\n"
	         "            tool \"coreutils\"\n"
	         "        }\n"
	         "%s"
	         "    }\n"
	         "\n"
	         "    metadata {\n"
	         "%s"
	         "    }\n"
	         "\n"
	         "    build {\n"
	         "        run \"true\" {\n"
	         "        }\n"
	         "    }\n"
	         "\n"
	         "    install {\n"
	         "        mkdir \"${dest}/usr/share/approvaltest\"\n"
	         "    }\n"
	         "}\n",
	         0, runtime, extra);
	return buf;
}

static int post_recipe(const struct cix_client *c, const char *content, int *out_status)
{
	struct json_writer w;
	struct cix_response r;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, "approvaltest");
	jw_key(&w, "content");
	jw_str(&w, content);
	jw_key(&w, "format");
	jw_str(&w, "pbs");
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	rc = cix_client_request(c, "POST", "/v1/pkg/recipes", w.buf, &r);
	jw_free(&w);
	if (rc != 0)
		return -1;
	*out_status = r.status;
	cix_response_free(&r);
	return 0;
}

#define APPROVAL "        \"artifact_sha256\" \"" \
	"1111111111111111111111111111111111111111111111111111111111111111\"\n"
#define APPROVAL2 "        \"artifact_sha256\" \"" \
	"2222222222222222222222222222222222222222222222222222222222222222\"\n"

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;
	int status;
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

	/* 1. First publication, with no approval -- the ordinary case. */
	if (post_recipe(&client, recipe_text("", ""), &status) != 0 || status != 204) {
		fprintf(stderr, "FAIL: first publish, status=%d\n", status);
		ok = 0;
	}

	/* 2. Republishing it unchanged is still refused. The relaxation is
	 * for adding an approval, not for overwriting. */
	if (post_recipe(&client, recipe_text("", ""), &status) != 0 || status != 409) {
		fprintf(stderr, "FAIL: identical republish should be refused, status=%d\n", status);
		ok = 0;
	}

	/* 3. Adding ONLY the approval is accepted -- the whole point. */
	if (post_recipe(&client, recipe_text(APPROVAL, ""), &status) != 0 || status != 204) {
		fprintf(stderr, "FAIL: adding an approval should be accepted, status=%d\n", status);
		ok = 0;
	}

	/* ...and it really took: the stored recipe now carries it. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/pkg/recipes/approvaltest", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET recipe after approval, status=%d\n", r.status);
		ok = 0;
	} else {
		const char *content = json_as_string(json_object_get(r.json, "content"));

		if (content == NULL || strstr(content, "pkg_artifact_sha256=\"1111") == NULL) {
			fprintf(stderr, "FAIL: stored recipe does not carry the approval\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 4. THE discriminating case for immutability: an approval already
	 * exists, so replacing it is refused. One version naming two byte
	 * sequences is precisely what this must not allow. */
	if (post_recipe(&client, recipe_text(APPROVAL2, ""), &status) != 0 || status != 409) {
		fprintf(stderr, "FAIL: replacing an approval should be refused, status=%d\n", status);
		ok = 0;
	}

	/* ...and the original approval survived the attempt. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/pkg/recipes/approvaltest", NULL, &r) == 0) {
		const char *content = json_as_string(json_object_get(r.json, "content"));

		if (content == NULL || strstr(content, "pkg_artifact_sha256=\"1111") == NULL) {
			fprintf(stderr, "FAIL: a refused replacement damaged the stored approval\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 5. THE discriminating case for "only the approval": a second
	 * package version that adds an approval AND changes a build input
	 * must be refused, or this is just mutability with extra steps. */
	{
		char smuggled[2048];

		snprintf(smuggled, sizeof(smuggled), "%s", recipe_text("", ""));
		if (post_recipe(&client, smuggled, &status) != 0) {
			fprintf(stderr, "FAIL: could not re-post baseline\n");
			ok = 0;
		}
	}
	if (post_recipe(&client, recipe_text(APPROVAL, "somethingelse"), &status) != 0 ||
	    status != 409) {
		fprintf(stderr,
		        "FAIL: an approval smuggled in alongside a changed runtime dependency should be "
		        "refused, status=%d\n",
		        status);
		ok = 0;
	}

	kill(daemon_pid, SIGTERM);
	waitpid(daemon_pid, NULL, 0);
	test_data_dir_cleanup(g_data_dir);

	if (ok)
		printf("test_pkg_recipe_approval: OK\n");
	return ok ? 0 : 1;
}
