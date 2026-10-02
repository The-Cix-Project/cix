/*
 * ADR-0323: the forge commit client's pure halves, with no network.
 *
 * What a commit sends and what it reads back are the two places a
 * silent mistake would land a recipe nowhere or report a commit that
 * never happened. The request body is parsed back and its base64
 * decoded, rather than compared as text, so the check is about what
 * Gitea receives and not about how the JSON writer spaces things. The
 * response reader is checked against the FileResponse shape in
 * Gitea 1.25.4's own spec (git.home.arpa /swagger.v1.json, 2026-10-02):
 * the sha lives at commit.sha, and there is a `content` object beside it
 * whose own `sha` (a blob sha) must not be taken for the commit's.
 */
#include "base64.h"
#include "forgecommit.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

static void test_body(void)
{
	/* A recipe carries quotes, backslashes and newlines, and a commit
	 * message carries quotes; both must arrive intact. */
	static const char recipe[] = "package \"probe\" {\n    version \"1.0\" # a \\ b\n}\n";
	static const char message[] = "probe@1.0-1: written by \"discovery\"";
	char *body = forge_gitea_create_body(recipe, strlen(recipe), "main", message);
	struct json_value *root;
	const char *b64;
	unsigned char decoded[256];
	int n;

	check(body != NULL, "a body is built for an ordinary recipe");
	if (body == NULL)
		return;
	root = json_parse(body, strlen(body));
	check(root != NULL, "the body is valid JSON");
	if (root != NULL) {
		check(json_as_string(json_object_get(root, "branch")) != NULL &&
		          strcmp(json_as_string(json_object_get(root, "branch")), "main") == 0,
		      "branch is the requested one");
		check(json_as_string(json_object_get(root, "message")) != NULL &&
		          strcmp(json_as_string(json_object_get(root, "message")), message) == 0,
		      "the message survives its quotes");
		b64 = json_as_string(json_object_get(root, "content"));
		check(b64 != NULL, "content is present");
		if (b64 != NULL) {
			n = base64_decode(b64, decoded, sizeof(decoded));
			check(n == (int)strlen(recipe) && memcmp(decoded, recipe, strlen(recipe)) == 0,
			      "content is the recipe, base64, byte for byte");
		}
		json_free(root);
	}
	free(body);

	check(forge_gitea_create_body("", 0, "main", "m") == NULL, "an empty recipe is refused");
	check(forge_gitea_create_body(recipe, strlen(recipe), NULL, "m") == NULL,
	      "a missing branch is refused");
}

static void test_commit_sha(void)
{
	static const char ok[] =
	    "{\"content\":{\"name\":\"probe@1.0-1.cbs\",\"sha\":\"b10b5ba5e000000000000000000000000000000"
	    "0\"},\"commit\":{\"url\":\"https://git.home.arpa/api/v1/repos/o/r/git/commits/c0ffee\","
	    "\"sha\":\"c0ffee00000000000000000000000000000000ab\"},\"verification\":{\"verified\":"
	    "false}}";
	char sha[FORGE_COMMIT_SHA_MAX];

	check(forge_gitea_commit_sha(ok, strlen(ok), sha, sizeof(sha)) == 0 &&
	          strcmp(sha, "c0ffee00000000000000000000000000000000ab") == 0,
	      "the commit sha is read from commit.sha, not content.sha");
	check(forge_gitea_commit_sha("{\"content\":{\"sha\":\"b10b\"}}", 26, sha, sizeof(sha)) != 0,
	      "a response with no commit names none");
	check(forge_gitea_commit_sha("not json", 8, sha, sizeof(sha)) != 0 && sha[0] == '\0',
	      "garbage names no commit");
	check(forge_gitea_commit_sha(ok, strlen(ok), sha, 8) != 0, "a sha that does not fit is refused");
}

static void test_refusals(void)
{
	struct forge_target t = { "github", "https", "github.com", "o", "r", "tok", "main" };
	char sha[FORGE_COMMIT_SHA_MAX], err[256];
	long status = -1;

	check(forge_create_file(&t, "recipes/package/x@1-1.cbs", "x", 1, "m", "/nonexistent", sha,
	                        sizeof(sha), &status, err, sizeof(err)) != 0 &&
	          strstr(err, "gitea") != NULL,
	      "a forge kind with no client is refused by name, before any network");
	t.kind = "gitea";
	t.token = "";
	check(forge_create_file(&t, "recipes/package/x@1-1.cbs", "x", 1, "m", "/nonexistent", sha,
	                        sizeof(sha), &status, err, sizeof(err)) != 0 &&
	          strstr(err, "token") != NULL,
	      "no token is refused, before any network");
}

int main(void)
{
	printf("test_forgecommit\n");
	test_body();
	test_commit_sha();
	test_refusals();
	printf("FORGECOMMIT RESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL",
	       failures);
	return failures == 0 ? 0 : 1;
}
