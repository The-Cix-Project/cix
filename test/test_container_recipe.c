/*
 * Proves ADR-0151: container recipes -- a git-syncable, reproducible
 * template for a real POST /v1/containers body (cmd/files/network/
 * restart-policy/sysctls, everything an image recipe deliberately
 * doesn't cover), and the {{SECRET:KEY}} substitution mechanism that
 * lets a recipe's own staged file content reference a credential
 * without ever committing the real value to git.
 */
#include "httpclient.h"
#include "test_cleanup.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7654
#define PORT_ARG "--port=7654"

static char g_data_dir[PATH_MAX];
static int g_failures;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s\n", msg); \
			g_failures++; \
		} \
	} while (0)

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

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
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

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never became healthy\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* ADR-0210: the daemon materializes the default image at boot, so
	 * it already exists here -- creating it again is a duplicate, not
	 * a fresh 201. This used to be the test's own job precisely
	 * because nothing else did it, which was the bug. */
	CHECK(cix_client_request(&client, "POST", "/v1/images", "{\"name\":\"base\"}", &r) == 0 &&
	          r.status == 409,
	      "the default image already exists at boot");
	cix_response_free(&r);

	/*
	 * Give that image a real C runtime before anything is run out of
	 * it (#186, ADR-0216). Containers here are created with
	 * cmd=/bin/true, and until the glibc floor was closed both the
	 * loader and libc arrived by being copied into every image off the
	 * build host -- so this test was creating containers from an image
	 * that had a runtime nobody had put there, and /bin/true never
	 * existed at all. POST /v1/containers now refuses an image with no
	 * loader, which is what surfaced it.
	 *
	 * Staged with the same fixture helper every other container test
	 * uses, so there is one answer to "how does a test image become
	 * runnable" rather than a second one written here: it places a
	 * real binary at /bin/<name> alongside the ld.so + libc pair it
	 * needs. daemon_child with no arguments exits immediately, which
	 * is the behaviour /bin/true is standing in for.
	 */
	{
		char image_dir[PATH_MAX];
		char version[128];
		char rootfs[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/base", g_data_dir);
		if (test_image_fixture_read_current_version(image_dir, version, sizeof(version)) != 0) {
			fprintf(stderr, "FAIL: could not read the base image's current version\n");
			g_failures++;
		} else {
			snprintf(rootfs, sizeof(rootfs), "%s/%s/rootfs", image_dir, version);
			if (test_image_fixture_build(rootfs, "build/daemon_child", "true") != 0) {
				fprintf(stderr, "FAIL: could not stage a C runtime into the base image\n");
				g_failures++;
			}
		}
	}

	/* --- scenario 1: fresh recipe list is empty --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/containers/recipes", NULL, &r) == 0 &&
	          r.status == 200,
	      "GET /v1/containers/recipes (fresh)");
	if (r.json != NULL) {
		const struct json_value *recipes = json_object_get(r.json, "recipes");

		CHECK(recipes != NULL && recipes->type == JSON_ARRAY && recipes->u.array.count == 0,
		      "fresh recipe list is empty");
	}
	cix_response_free(&r);

	/* --- scenario 2: name mismatch between the upload name and the
	 * content's own "name" field is rejected (mirrors pkg_recipe_add()'s
	 * pkg_name= contract). --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/containers/recipes",
	                         "{\"name\":\"crtest\",\"content\":\"{\\\"name\\\":\\\"different\\\"}\"}",
	                         &r) == 0 &&
	          r.status == 400,
	      "name mismatch between upload name and content's own name is rejected");
	cix_response_free(&r);

	/* --- scenario 3: malformed JSON content is rejected --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/containers/recipes",
	                         "{\"name\":\"crtest\",\"content\":\"not json at all\"}", &r) == 0 &&
	          r.status == 400,
	      "non-JSON content is rejected");
	cix_response_free(&r);

	/*
	 * --- issue #301: a recipe may declare the server role its
	 * container provides ---
	 *
	 * A recipe is only a faithful record of a container if it can
	 * carry everything a create can. handle_create() was taught to
	 * register a declared dns_server/ntp_server/syslog_target/
	 * ldap_server, and the four fields were not added to the shared
	 * allowlist -- so that code was unreachable. Measured against a
	 * real daemon (v2.53.71 on 192.168.15.95): POST /v1/containers
	 * with a dns_server block returned {"error":"unknown field:
	 * dns_server"}, and container recipe add refused the same field
	 * with "unknown field in recipe content: dns_server". The feature
	 * never fired and nothing said so.
	 *
	 * Asserted at recipe-ADD time deliberately: this is where issue
	 * #68 puts the check, so a field the catalog will not accept is
	 * refused now rather than sitting latent until some future apply
	 * silently drops it. One allowlist serves both the recipe check
	 * and create_container_persisted(), so accepting it here is the
	 * same statement as accepting it on a create.
	 */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/containers/recipes",
	                         "{\"name\":\"crroles\",\"content\":\"{\\\"name\\\":\\\"crroles\\\","
	                         "\\\"image\\\":\\\"base\\\",\\\"cmd\\\":[\\\"/bin/true\\\"],"
	                         "\\\"dns_server\\\":{\\\"hosts_path\\\":\\\"/etc/hosts\\\"},"
	                         "\\\"ntp_server\\\":true,\\\"syslog_target\\\":true,"
	                         "\\\"ldap_server\\\":{\\\"config_path\\\":\\\"/etc/glauth.cfg\\\"}}\"}",
	                         &r) == 0 &&
	          r.status == 204,
	      "#301 a recipe may declare dns_server/ntp_server/syslog_target/ldap_server");
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/recipes/crroles", NULL, &r);
	cix_response_free(&r);

	/* --- scenario 4: real add, with a {{SECRET:PW}} token embedded in
	 * staged file content --- */
	{
		struct json_writer w;
		const char *content =
		    "{\"name\":\"crtest\",\"image\":\"base\",\"cmd\":[\"/bin/true\"],\"restart\":\"no\","
		    "\"files\":[{\"path\":\"/etc/secret.conf\",\"content\":\"password={{SECRET:PW}}\\n\"}]}";

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, "crtest");
		jw_key(&w, "content");
		jw_str(&w, content);
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		CHECK(cix_client_request(&client, "POST", "/v1/containers/recipes", w.buf, &r) == 0 &&
		          r.status == 204,
		      "POST /v1/containers/recipes (crtest)");
		jw_free(&w);
		cix_response_free(&r);
	}

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/containers/recipes", NULL, &r) == 0 &&
	          r.status == 200,
	      "GET /v1/containers/recipes (one entry)");
	if (r.json != NULL) {
		const struct json_value *recipes = json_object_get(r.json, "recipes");

		CHECK(recipes != NULL && recipes->type == JSON_ARRAY && recipes->u.array.count == 1,
		      "recipe list has exactly one entry after add");
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/containers/recipes/crtest", NULL, &r) == 0 &&
	          r.status == 200,
	      "GET /v1/containers/recipes/crtest");
	if (r.json != NULL) {
		const char *c = json_str_field(r.json, "content");

		CHECK(c != NULL && strstr(c, "{{SECRET:PW}}") != NULL,
		      "raw recipe content shows the unsubstituted placeholder, never a guess");
	}
	cix_response_free(&r);

	/* --- scenario 5: apply with the secret supplied -- real container
	 * created, real substitution, correctly JSON-escaped (the value
	 * deliberately contains a double quote). --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/containers/recipes/crtest/apply",
	                         "{\"secrets\":{\"PW\":\"hunter\\\"2\"}}", &r) == 0 &&
	          r.status == 201,
	      "apply-recipe with the secret supplied creates a real container (201)");
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/containers/crtest/files?path=/etc/secret.conf",
	                         NULL, &r) == 0 &&
	          r.status == 200,
	      "GET the staged file back out of the real container");
	CHECK(r.body != NULL && strstr(r.body, "password=hunter\"2") != NULL,
	      "secret was substituted, correctly JSON-escaped through the pipeline");
	cix_response_free(&r);

	CHECK(cix_client_request(&client, "DELETE", "/v1/containers/crtest", NULL, &r) == 0,
	      "rm crtest to free the name for scenario 6");
	cix_response_free(&r);
	{
		/*
		 * ADR-0180: settle the async delete before reusing the name.
		 * This one was missed when the suite was converted -- DELETE
		 * returns as soon as the intent is durable, so the create below
		 * could still hit the old container's name and 409. It only
		 * showed under load (the whole suite running before it), which
		 * is exactly how a race that is always present manages to look
		 * like an unrelated regression.
		 */
		int i;

		for (i = 0; i < 50; i++) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/containers/crtest", NULL, &r) == 0 &&
			    r.status == 404) {
				cix_response_free(&r);
				break;
			}
			cix_response_free(&r);
			usleep(100 * 1000);
		}
	}

	/* --- scenario 6: apply with NO secret supplied leaves the token
	 * untouched (never silently swallowed) --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/containers/recipes/crtest/apply", "{}", &r) ==
	              0 &&
	          r.status == 201,
	      "apply-recipe with no secrets still creates the container (201)");
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/containers/crtest/files?path=/etc/secret.conf",
	                         NULL, &r) == 0 &&
	          r.status == 200,
	      "GET the staged file back out (unsubstituted case)");
	CHECK(r.body != NULL && strstr(r.body, "{{SECRET:PW}}") != NULL,
	      "an unmatched token is left exactly as-is, not swallowed or blanked");
	cix_response_free(&r);

	/* --- scenario 6.5 (issue #66): {{LDAP:FIELD}} tokens resolve from
	 * the daemon's own LDAP client config -- no per-apply secret, no
	 * copied values in the recipe. Also proves the two token families
	 * coexist in one file and the bind credential flows only into the
	 * rendered container, never out of GET /ldap/config. --- */
	CHECK(cix_client_request(&client, "DELETE", "/v1/containers/crtest", NULL, &r) == 0,
	      "rm crtest to free the name for the LDAP-token scenario");
	cix_response_free(&r);
	{
		int i;

		/* ADR-0180: settle the async delete before reusing the name. */
		for (i = 0; i < 50; i++) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/containers/crtest", NULL, &r) == 0 &&
			    r.status == 404) {
				cix_response_free(&r);
				break;
			}
			cix_response_free(&r);
			usleep(100 * 1000);
		}
	}
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "PUT", "/v1/ldap/config",
	                         "{\"client_uri\":\"ldap://10.9.9.9:3893/\","
	                         "\"base_dn\":\"dc=crt,dc=local\","
	                         "\"bind_dn\":\"cn=svc,dc=crt,dc=local\","
	                         "\"bind_password\":\"tokpw\"}", &r) == 0 &&
	          r.status == 200,
	      "PUT ldap client config for token substitution");
	cix_response_free(&r);
	{
		static const char ldap_recipe_body[] =
		    "{\"name\":\"crtest\",\"content\":\"{"
		    "\\\"name\\\":\\\"crtest\\\",\\\"image\\\":\\\"base\\\","
		    "\\\"cmd\\\":[\\\"/bin/true\\\"],"
		    "\\\"files\\\":[{\\\"path\\\":\\\"/etc/nslcd.conf\\\","
		    "\\\"content\\\":\\\"uri {{LDAP:URI}}\\\\nbase {{LDAP:BASE_DN}}\\\\n"
		    "binddn {{LDAP:BIND_DN}}\\\\nbindpw {{LDAP:BIND_PASSWORD}}\\\\n\\\"}]}\"}";

		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "POST", "/v1/containers/recipes", ldap_recipe_body,
		                         &r) == 0 &&
		          (r.status == 204 || r.status == 201),
		      "add recipe carrying {{LDAP:*}} tokens");
		cix_response_free(&r);
	}
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/containers/recipes/crtest/apply", "{}", &r) ==
	              0 &&
	          r.status == 201,
	      "apply the LDAP-token recipe with NO secrets at all (201)");
	if (r.status != 201)
		fprintf(stderr, "  apply said: status=%d body=%s\n", r.status,
		        r.body != NULL ? r.body : "(null)");
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/containers/crtest/files?path=/etc/nslcd.conf",
	                         NULL, &r) == 0 &&
	          r.status == 200,
	      "GET the rendered nslcd.conf back out of the real container");
	CHECK(r.body != NULL && strstr(r.body, "uri ldap://10.9.9.9:3893/") != NULL &&
	          strstr(r.body, "base dc=crt,dc=local") != NULL &&
	          strstr(r.body, "bindpw tokpw") != NULL,
	      "all four {{LDAP:*}} tokens substituted from daemon config");
	cix_response_free(&r);

	/* --- scenario 6.6 (#327): {{LDAP:URI}} is DERIVED from the
	 * registered LDAP servers, not read straight off client_uri.
	 *
	 * Scenario 6.5 above sets an explicit client_uri, so it only ever
	 * exercised the branch that already worked -- which is exactly why
	 * this shipped. With no explicit list, the daemon's own nslcd.conf
	 * rendering used ldap_effective_client_uri() while the token path
	 * read the raw (empty) field and left "{{LDAP:URI}}" verbatim, so
	 * one container create produced two files disagreeing about the
	 * same servers. That is #66's own spec unmet: the token is meant to
	 * be "rendered from the registered-servers list".
	 *
	 * The failure it caused was narrow and misleading rather than loud:
	 * nslcd holds the good value, so password and NSS lookups work,
	 * while sshd's AuthorizedKeysCommand sources the token file and
	 * searches a nonsense URI -- reporting no keys for the user instead
	 * of a broken configuration. --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "DELETE", "/v1/containers/crtest", NULL, &r) == 0,
	      "rm crtest to free the name for the derived-URI scenario");
	cix_response_free(&r);
	{
		int i;

		/* ADR-0180: settle the async delete before reusing the name. */
		for (i = 0; i < 50; i++) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/containers/crtest", NULL, &r) == 0 &&
			    r.status == 404) {
				cix_response_free(&r);
				break;
			}
			cix_response_free(&r);
			usleep(100 * 1000);
		}
	}
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/networks",
	                         "{\"name\":\"crtnet\",\"subnet\":\"172.41.0.0\",\"prefix_len\":24}",
	                         &r) == 0 &&
	          r.status == 201,
	      "create a network, so a registered server has a real address to be derived from");
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"crtldap\",\"image\":\"base\","
	                         "\"cmd\":[\"/bin/true\",\"60\"],"
	                         "\"networks\":[\"crtnet\"]}",
	                         &r) == 0 &&
	          r.status == 201,
	      "create a container that stays up long enough to serve as an LDAP server");
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/ldap/servers",
	                         "{\"container\":\"crtldap\","
	                         "\"config_path\":\"/etc/glauth/glauth.cfg\"}",
	                         &r) == 0 &&
	          r.status == 201,
	      "register that container as an LDAP server");
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "PUT", "/v1/ldap/config", "{\"client_uri\":\"\"}", &r) == 0 &&
	          r.status == 200,
	      "clear client_uri -- the registered server is now the only thing naming the URI");
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/containers/recipes/crtest/apply", "{}", &r) ==
	              0 &&
	          r.status == 201,
	      "re-apply the LDAP-token recipe with no explicit client_uri");
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/containers/crtest/files?path=/etc/nslcd.conf",
	                         NULL, &r) == 0 &&
	          r.status == 200,
	      "GET the rendered nslcd.conf for the derived case");
	CHECK(r.body != NULL && strstr(r.body, "{{LDAP:URI}}") == NULL,
	      "{{LDAP:URI}} is not left verbatim when only a registered server names the URI");
	CHECK(r.body != NULL && strstr(r.body, "uri ldap://172.41.0.") != NULL &&
	          strstr(r.body, ":3893/") != NULL,
	      "{{LDAP:URI}} resolved to the registered server's own live address");
	if (r.body != NULL && strstr(r.body, "uri ldap://172.41.0.") == NULL)
		fprintf(stderr, "  rendered nslcd.conf was: %s\n", r.body);
	cix_response_free(&r);

	/* --- scenario 7: apply on a name with no stored recipe is 404 --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/containers/recipes/noexist/apply", "{}", &r) ==
	              0 &&
	          r.status == 404,
	      "apply-recipe on an unknown recipe name is 404");
	cix_response_free(&r);

	/* --- scenario 8: recipe rm --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "DELETE", "/v1/containers/recipes/crtest", NULL, &r) == 0 &&
	          r.status == 204,
	      "DELETE /v1/containers/recipes/crtest");
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/containers/recipes/crtest", NULL, &r) == 0 &&
	          r.status == 404,
	      "GET removed recipe is 404");
	cix_response_free(&r);

	/* Enumerated cleanup, never a hand-kept list: a leaked bridge makes
	 * the NEXT run's network create fail with a 500 that reads exactly
	 * like a code regression (test_cleanup.h). */
	CHECK(test_cleanup_containers_and_network(&client, "crtnet") == 0,
	      "containers and crtnet removed, leaving no host bridge behind");

	CHECK(stop_daemon(daemon_pid) == 0, "daemon shut down cleanly");
	test_data_dir_cleanup(g_data_dir);

	if (g_failures > 0) {
		fprintf(stderr, "test_container_recipe: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_container_recipe: OK\n");
	return 0;
}
