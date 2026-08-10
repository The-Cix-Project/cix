/*
 * Task #725 end-to-end test: proves LDAP server registration
 * (POST/GET/DELETE /v1/ldap/servers) over real HTTP against a live
 * daemon -- register/unregister validation, persistence across a
 * real daemon restart (same discipline ADR-0091 established for
 * dns_server_register()'s own bindings), and container-delete
 * cleanup (ldap_server_forget()). Unlike test_dns.c's own server
 * binding coverage, this needs no real LDAP-serving container or
 * protocol-level verification (ldapsearch) -- registration itself is
 * pure bookkeeping (see daemon/include/ldap.h's own header comment
 * for why), so a real *running* container is enough to exercise the
 * REST surface; task #726/#727's own tests are where a real glauth
 * container and real LDAP queries belong.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7635
#define PORT_ARG "--port=7635"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

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

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static int servers_list_contains(struct json_value *root, const char *container,
                                  const char *want_config_path)
{
	const struct json_value *servers = json_object_get(root, "servers");
	size_t i;

	if (servers == NULL || servers->type != JSON_ARRAY)
		return 0;
	for (i = 0; i < servers->u.array.count; i++) {
		const struct json_value *item = servers->u.array.items[i];

		if (str_eq(json_str_field(item, "container"), container)) {
			if (want_config_path != NULL)
				return str_eq(json_str_field(item, "config_path"), want_config_path);
			return 1;
		}
	}
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/images/ldaptest/v1/rootfs", g_data_dir);

	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/images/ldaptest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}
	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

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

	/* 1. a real, long-running container to register against */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"ldapsrv\",\"image\":\"ldaptest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST ldapsrv, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2. validation: nonexistent container -> 404 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/ldap/servers",
	                       "{\"container\":\"no-such-container\",\"config_path\":\"/etc/glauth/glauth.cfg\"}",
	                       &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: register nonexistent container expected 404, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. validation: non-absolute config_path -> 400 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/ldap/servers",
	                       "{\"container\":\"ldapsrv\",\"config_path\":\"etc/glauth/glauth.cfg\"}", &r) !=
	        0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: non-absolute config_path expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 4. a real, valid registration -> 201, echoes container/config_path */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/ldap/servers",
	                       "{\"container\":\"ldapsrv\",\"config_path\":\"/etc/glauth/glauth.cfg\"}", &r) !=
	        0 ||
	    r.status != 201 || !str_eq(json_str_field(r.json, "container"), "ldapsrv") ||
	    !str_eq(json_str_field(r.json, "config_path"), "/etc/glauth/glauth.cfg")) {
		fprintf(stderr, "FAIL: register ldapsrv, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 5. duplicate registration -> 409 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/ldap/servers",
	                       "{\"container\":\"ldapsrv\",\"config_path\":\"/etc/glauth/glauth.cfg\"}", &r) !=
	        0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate registration expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 6. GET reflects it */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/ldap/servers", NULL, &r) != 0 || r.status != 200 ||
	    !servers_list_contains(r.json, "ldapsrv", "/etc/glauth/glauth.cfg")) {
		fprintf(stderr, "FAIL: GET /v1/ldap/servers did not show ldapsrv, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/*
	 * 7. persistence across a real daemon restart -- same discipline
	 * ADR-0091 established for dns_server_register()'s own bindings
	 * (a real, previously-undiscovered gap there: purely in-memory
	 * state silently lost on every restart). The container itself
	 * doesn't need to still be running for this check -- only the
	 * binding table's own persistence is under test here.
	 */
	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly\n");
		ok = 0;
	}
	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never came back up after restart\n");
		ok = 0;
	} else {
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/ldap/servers", NULL, &r) != 0 ||
		    r.status != 200 || !servers_list_contains(r.json, "ldapsrv", "/etc/glauth/glauth.cfg")) {
			fprintf(stderr,
			        "FAIL: ldapsrv binding did not survive a daemon restart, status=%d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 8. unregister -> 204, then GET no longer shows it */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/ldap/servers/ldapsrv", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: unregister ldapsrv, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/ldap/servers", NULL, &r) != 0 || r.status != 200 ||
	    servers_list_contains(r.json, "ldapsrv", NULL)) {
		fprintf(stderr, "FAIL: ldapsrv binding survived unregister, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 9. unregister of something never registered -> 404 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/ldap/servers/ldapsrv", NULL, &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: unregister already-gone binding expected 404, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/*
	 * 10. container-delete cleanup: a FRESH container (the original
	 * "ldapsrv" is no longer running after step 7's real daemon
	 * restart -- it was never created with restart:"always", so it
	 * isn't replayed, same as any ordinary container; not what this
	 * step is testing), register it, then delete the container
	 * itself -- ldap_server_forget() (called from the same
	 * container-delete cleanup block as dns_server_forget()) must
	 * remove the now-stale binding, exactly mirroring test_dns.c's
	 * own step 7.
	 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"ldapsrv2\",\"image\":\"ldaptest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST ldapsrv2, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/ldap/servers",
	                       "{\"container\":\"ldapsrv2\",\"config_path\":\"/etc/glauth/glauth.cfg\"}", &r) !=
	        0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: register ldapsrv2, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	kx_client_request(&client, "DELETE", "/v1/containers/ldapsrv2", NULL, &r);
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/ldap/servers", NULL, &r) != 0 || r.status != 200 ||
	    servers_list_contains(r.json, "ldapsrv2", NULL)) {
		fprintf(stderr, "FAIL: ldap server binding survived container deletion\n");
		ok = 0;
	}
	kx_response_free(&r);

	/*
	 * 11-19. Task #726: user/group CRUD, and -- the part actually worth
	 * proving -- that the write-through mechanism really preserves an
	 * operator-authored config prefix while rendering the managed
	 * [[groups]]/[[users]] tail correctly. "ldapcfg" doesn't run real
	 * glauth (this project has no way to verify glauth's own fsnotify
	 * reload from inside this test suite -- confirmed directly against
	 * glauth's real source instead, see ldap.h's own header comment);
	 * what's under test here is entirely kanxeod's own code: the
	 * marker-based prefix-preserving rewrite in ldap_write_config_
	 * file(), read back via the real GET .../files endpoint (ADR-0055)
	 * exactly the way an operator or a future test with real glauth
	 * would.
	 */
	{
		static const char base_config_prefix[] = "# base config\nwatchconfig = true\n";
		struct json_value *jval;

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"ldapcfg\",\"image\":\"ldaptest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
		                       "\"files\":[{\"path\":\"/etc/glauth/glauth.cfg\","
		                       "\"content\":\"# base config\\nwatchconfig = true\\n\"}]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST ldapcfg, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/ldap/servers",
		                       "{\"container\":\"ldapcfg\",\"config_path\":\"/etc/glauth/glauth.cfg\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: register ldapcfg, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 12. group create */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/ldap/groups",
		                       "{\"name\":\"engineers\",\"gidnumber\":6001}", &r) != 0 ||
		    r.status != 201 || !str_eq(json_str_field(r.json, "name"), "engineers")) {
			fprintf(stderr, "FAIL: create group engineers, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 13. duplicate group -> 409 */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/ldap/groups",
		                       "{\"name\":\"engineers\",\"gidnumber\":6002}", &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: duplicate group expected 409, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 14. user create with an unknown primarygroup -> 400 */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/ldap/users",
		                       "{\"name\":\"nogroup\",\"uidnumber\":5002,\"primarygroup\":9999}",
		                       &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: unknown primarygroup expected 400, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 15. real user create, with a password -- passsha256 must
		 * never come back over the API (only has_password) */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/ldap/users",
		                       "{\"name\":\"j_doe\",\"uidnumber\":5001,\"primarygroup\":6001,"
		                       "\"mail\":\"j.doe@kanxeo.internal\",\"password\":\"dogood\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: create user j_doe, status=%d\n", r.status);
			ok = 0;
		}
		jval = r.json != NULL ? (struct json_value *)json_object_get(r.json, "has_password") : NULL;
		if (jval == NULL || jval->type != JSON_BOOL || !jval->u.boolean) {
			fprintf(stderr, "FAIL: create user j_doe did not report has_password=true\n");
			ok = 0;
		}
		kx_response_free(&r);

		/* 16. the container's own config file now shows the preserved
		 * prefix plus a correctly-rendered managed tail, including the
		 * real SHA-256 of "dogood" (independently verified via `printf
		 * dogood | sha256sum`) */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET",
		                       "/v1/containers/ldapcfg/files?path=%2Fetc%2Fglauth%2Fglauth.cfg", NULL,
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET ldapcfg config file, status=%d\n", r.status);
			ok = 0;
		} else if (r.body == NULL || r.body_len < sizeof(base_config_prefix) - 1 ||
		           memcmp(r.body, base_config_prefix, sizeof(base_config_prefix) - 1) != 0) {
			fprintf(stderr, "FAIL: rendered config lost its operator-authored prefix\n");
			ok = 0;
		} else if (memmem(r.body, r.body_len, "name = \"engineers\"", strlen("name = \"engineers\"")) ==
		               NULL ||
		           memmem(r.body, r.body_len, "gidnumber = 6001", strlen("gidnumber = 6001")) ==
		               NULL ||
		           memmem(r.body, r.body_len, "name = \"j_doe\"", strlen("name = \"j_doe\"")) == NULL ||
		           memmem(r.body, r.body_len, "mail = \"j.doe@kanxeo.internal\"",
		                  strlen("mail = \"j.doe@kanxeo.internal\"")) == NULL ||
		           memmem(r.body, r.body_len,
		                  "passsha256 = \"6478579e37aff45f013e14eeb30b3cc56c72ccdc310123bcdf53e0333e"
		                  "3f416a\"",
		                  strlen("passsha256 = \"6478579e37aff45f013e14eeb30b3cc56c72ccdc310123bcdf53e"
		                         "0333e3f416a\"")) == NULL) {
			fprintf(stderr, "FAIL: rendered config missing expected group/user fields\n");
			ok = 0;
		}
		kx_response_free(&r);

		/* 17. update: change mail, omit password -- the existing hash
		 * must survive unchanged in the re-rendered file */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "PUT", "/v1/ldap/users/j_doe",
		                       "{\"uidnumber\":5001,\"primarygroup\":6001,"
		                       "\"mail\":\"jd@kanxeo.internal\"}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: update user j_doe, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET",
		                       "/v1/containers/ldapcfg/files?path=%2Fetc%2Fglauth%2Fglauth.cfg", NULL,
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET ldapcfg config file after update, status=%d\n", r.status);
			ok = 0;
		} else if (memmem(r.body, r.body_len, "mail = \"jd@kanxeo.internal\"",
		                  strlen("mail = \"jd@kanxeo.internal\"")) == NULL ||
		           memmem(r.body, r.body_len,
		                  "passsha256 = \"6478579e37aff45f013e14eeb30b3cc56c72ccdc310123bcdf53e0333e"
		                  "3f416a\"",
		                  strlen("passsha256 = \"6478579e37aff45f013e14eeb30b3cc56c72ccdc310123bcdf53e"
		                         "0333e3f416a\"")) == NULL) {
			fprintf(stderr, "FAIL: update lost the mail change or the existing password hash\n");
			ok = 0;
		}
		kx_response_free(&r);

		/* 18. delete the user -> the rendered file no longer names it,
		 * but the group stanza survives */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/ldap/users/j_doe", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: delete user j_doe, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET",
		                       "/v1/containers/ldapcfg/files?path=%2Fetc%2Fglauth%2Fglauth.cfg", NULL,
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET ldapcfg config file after user delete, status=%d\n", r.status);
			ok = 0;
		} else if (memmem(r.body, r.body_len, "name = \"j_doe\"", strlen("name = \"j_doe\"")) != NULL ||
		           memmem(r.body, r.body_len, "name = \"engineers\"",
		                  strlen("name = \"engineers\"")) == NULL) {
			fprintf(stderr, "FAIL: user delete didn't remove j_doe or dropped the group\n");
			ok = 0;
		}
		kx_response_free(&r);

		/* 19. delete the group too, list endpoints reflect it */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/ldap/groups/engineers", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: delete group engineers, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/ldap/groups", NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/ldap/groups, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *groups = json_object_get(r.json, "groups");

			if (groups == NULL || groups->type != JSON_ARRAY || groups->u.array.count != 0) {
				fprintf(stderr, "FAIL: /v1/ldap/groups not empty after delete\n");
				ok = 0;
			}
		}
		kx_response_free(&r);

		kx_client_request(&client, "DELETE", "/v1/containers/ldapcfg", NULL, &r);
		kx_response_free(&r);
	}

	/*
	 * 20-25. Task #727: the automatic provisioning hook. Mirrors
	 * test_pki.c's own step 6b (pki_issue delivery verification) as
	 * closely as possible -- a service account (not a human login,
	 * task #726's own territory) auto-created for the container
	 * itself, owner set to the container's name, a "search"
	 * capability granted by default (glauth defaults to deny-all),
	 * and a freshly generated secret delivered into the container's
	 * own filesystem at /etc/kanxeo-ldap/bind.secret -- read directly
	 * via /proc/<pid>/root/, the same privilege pki_issue's own test
	 * already established (ADR-0013), not just assumed from a 201.
	 */
	{
		int provtest_pid = -1;

		/* 20. a group for provisioned accounts to join */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/ldap/groups",
		                       "{\"name\":\"svcaccts\",\"gidnumber\":7001}", &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: create group svcaccts, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 21. ldap_provision without ldap_group naming a real group -> 400 */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"badprov\",\"image\":\"ldaptest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
		                       "\"ldap_provision\":true}",
		                       &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: ldap_provision without a valid ldap_group expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 22. a real provisioned container -- default ldap_user
		 * (the container's own name), default ldap_uid (allocated),
		 * default ldap_secret_dir (/etc/kanxeo-ldap) */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"provtest\",\"image\":\"ldaptest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
		                       "\"ldap_provision\":true,\"ldap_group\":\"svcaccts\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST provtest (ldap_provision), status=%d\n", r.status);
			ok = 0;
		} else {
			provtest_pid = (int)json_as_number(json_object_get(r.json, "pid"));
		}
		kx_response_free(&r);

		/* 23. the auto-created service account: owner==container name,
		 * primarygroup resolved, can_search granted, has_password */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/ldap/users/provtest", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "owner"), "provtest") ||
		    (int)json_as_number(json_object_get(r.json, "primarygroup")) != 7001) {
			fprintf(stderr, "FAIL: GET provtest ldap user, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *jsearch = json_object_get(r.json, "can_search");
			const struct json_value *jpass = json_object_get(r.json, "has_password");

			if (jsearch == NULL || jsearch->type != JSON_BOOL || !jsearch->u.boolean ||
			    jpass == NULL || jpass->type != JSON_BOOL || !jpass->u.boolean) {
				fprintf(stderr, "FAIL: provtest ldap user missing can_search/has_password\n");
				ok = 0;
			}
		}
		kx_response_free(&r);

		/* 24. the secret was really delivered into the container's own
		 * filesystem, at the default path, chmod 0600 */
		if (provtest_pid > 0) {
			char proc_path[192];
			struct stat st;
			FILE *f;
			char secret[128] = { 0 };
			size_t n = 0;

			snprintf(proc_path, sizeof(proc_path), "/proc/%d/root/etc/kanxeo-ldap/bind.secret",
			         provtest_pid);
			if (stat(proc_path, &st) != 0 || (st.st_mode & 0777) != 0600) {
				fprintf(stderr, "FAIL: delivered bind.secret missing or not chmod 0600 (%s)\n",
				        proc_path);
				ok = 0;
			}
			f = fopen(proc_path, "r");
			if (f == NULL) {
				fprintf(stderr, "FAIL: could not open delivered bind.secret (%s)\n", proc_path);
				ok = 0;
			} else {
				n = fread(secret, 1, sizeof(secret) - 1, f);
				fclose(f);
				secret[n] = '\0';
				/* ldap_generate_secret() reads LDAP_PROVISION_SECRET_LEN/2
				 * (16) raw bytes and hex-encodes them -- 32 hex chars. */
				if (n != 32) {
					fprintf(stderr,
					        "FAIL: delivered bind.secret has wrong length (got %zu, want 32)\n",
					        n);
					ok = 0;
				}
			}
		} else {
			fprintf(stderr, "FAIL: provtest never got a pid, skipping delivery checks\n");
			ok = 0;
		}

		/* 25. deleting the container removes the auto-provisioned
		 * account too (ldap_user_forget_owner(), mirroring
		 * dns_record_forget_owner()/pki_cert_forget_owner()) */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/containers/provtest", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: delete provtest, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/ldap/users/provtest", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr,
			        "FAIL: provtest ldap user survived container deletion, status=%d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		kx_client_request(&client, "DELETE", "/v1/ldap/groups/svcaccts", NULL, &r);
		kx_response_free(&r);
	}

	/*
	 * 26-33. Task #748 (user-requested): configurable start_uid/
	 * start_gid, auto-allocated when uidnumber/gidnumber are omitted
	 * on create. Task #750 (user-requested): PUT /v1/ldap/groups/{name}
	 * edits an existing group's gidnumber in place.
	 */
	{
		/* 26. default config */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/ldap/config", NULL, &r) != 0 ||
		    r.status != 200 ||
		    (long)json_as_number(json_object_get(r.json, "start_uid")) != 10000 ||
		    (long)json_as_number(json_object_get(r.json, "start_gid")) != 10000) {
			fprintf(stderr, "FAIL: GET default ldap config, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 27. group create with no gidnumber -> auto-allocated from start_gid */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/ldap/groups", "{\"name\":\"autogid1\"}",
		                       &r) != 0 ||
		    r.status != 201 ||
		    (long)json_as_number(json_object_get(r.json, "gidnumber")) != 10000) {
			fprintf(stderr, "FAIL: create group with no gidnumber, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 28. user create with no uidnumber -> auto-allocated from start_uid */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/ldap/users",
		                       "{\"name\":\"autouid1\",\"primarygroup\":10000}", &r) != 0 ||
		    r.status != 201 ||
		    (long)json_as_number(json_object_get(r.json, "uidnumber")) != 10000) {
			fprintf(stderr, "FAIL: create user with no uidnumber, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 29. PUT ldap config -- changes take effect for future allocations only */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "PUT", "/v1/ldap/config",
		                       "{\"start_uid\":50000,\"start_gid\":50000}", &r) != 0 ||
		    r.status != 200 ||
		    (long)json_as_number(json_object_get(r.json, "start_uid")) != 50000 ||
		    (long)json_as_number(json_object_get(r.json, "start_gid")) != 50000) {
			fprintf(stderr, "FAIL: PUT ldap config, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 30. a second auto-allocated group/user now starts from 50000,
		 * not colliding with autogid1/autouid1's own 10000 */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/ldap/groups", "{\"name\":\"autogid2\"}",
		                       &r) != 0 ||
		    r.status != 201 ||
		    (long)json_as_number(json_object_get(r.json, "gidnumber")) != 50000) {
			fprintf(stderr, "FAIL: create group after config change, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 31. group update -- PUT edits gidnumber in place */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "PUT", "/v1/ldap/groups/autogid1",
		                       "{\"gidnumber\":10999}", &r) != 0 ||
		    r.status != 200 ||
		    (long)json_as_number(json_object_get(r.json, "gidnumber")) != 10999) {
			fprintf(stderr, "FAIL: PUT autogid1 update, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/ldap/groups/autogid1", NULL, &r) != 0 ||
		    r.status != 200 ||
		    (long)json_as_number(json_object_get(r.json, "gidnumber")) != 10999) {
			fprintf(stderr, "FAIL: GET autogid1 after update, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 32. group update colliding with a different group's gidnumber -> 409 */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "PUT", "/v1/ldap/groups/autogid1",
		                       "{\"gidnumber\":50000}", &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: PUT autogid1 gidnumber collision expected 409, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 33. group update on a nonexistent group -> 404 */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "PUT", "/v1/ldap/groups/no-such-group",
		                       "{\"gidnumber\":10001}", &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: PUT nonexistent group expected 404, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		kx_client_request(&client, "DELETE", "/v1/ldap/users/autouid1", NULL, &r);
		kx_response_free(&r);
		kx_client_request(&client, "DELETE", "/v1/ldap/groups/autogid1", NULL, &r);
		kx_response_free(&r);
		kx_client_request(&client, "DELETE", "/v1/ldap/groups/autogid2", NULL, &r);
		kx_response_free(&r);
	}

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);

	printf("LDAP RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
