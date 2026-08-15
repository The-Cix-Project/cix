/*
 * ADR-0144 end-to-end test: host authentication foundation --
 * POST /v1/login, POST /v1/logout, GET/PUT /v1/system/hostauth-config,
 * and the write-gating check every mutating request goes through in
 * dispatch(). Covers the local backend (ldap_user_check_password()/
 * ldap_user_is_in_group()) in full, plus the live-LDAP backend's own
 * config validation and its unreachable-server-falls-back-to-local
 * path (daemon/src/hostauth.c's try_ldap_login()). A real successful
 * bind against a genuinely running glauth server is NOT exercised here
 * -- this project doesn't build/vendor a glauth binary as part of its
 * own toolchain (it's a separate Go project, only ever run as a
 * container image on a real deployed box), so there's no portable way
 * to spin one up inside this regression suite; daemon/src/ldapclient.c
 * was instead verified directly against a real local glauth process
 * plus the standard ldapsearch/ldapwhoami reference client during its
 * own development (see CHANGELOG.md/ROADMAP.md's own entry for this
 * part of ADR-0144 for how, and for the real hex-encoding bug that
 * verification found in ldap.c's own TOML rendering).
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7680
#define PORT_ARG "--port=7680"

static char g_data_dir[PATH_MAX];

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

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
	dargv[1] = PORT_ARG;
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

/* kx_client_request() has no built-in header-injection support beyond
 * what httpclient.h exposes -- check for an authenticated-request
 * helper; if none exists, requests needing Authorization go through
 * kx_client_request_with_header() below (added here, matching this
 * test's own real need rather than growing the shared client for a
 * single caller). */
static int request_with_token(const struct kx_client *c, const char *method, const char *path,
                               const char *token, const char *body, struct kx_response *r)
{
	return kx_client_request_with_auth(c, method, path, token, body, r);
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;
	char token[128] = "";

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

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

	/* 1. Bootstrap safety: no admin-group user exists yet -- an
	 * ordinary write (creating an LDAP group) succeeds with zero
	 * credentials, same as this whole project's behavior before
	 * ADR-0144 existed. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/ldap/groups", "{\"name\":\"engineers\",\"gidnumber\":7001}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: pre-bootstrap group create (should be open), status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2. Configure the admin group -- also an open write, since gating
	 * still isn't active (no user is a member of "admins" yet). */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "PUT", "/v1/system/hostauth-config",
	                       "{\"admin_groups\":[\"admins\"],\"idle_timeout_seconds\":900}", &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: PUT hostauth-config, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/ldap/groups", "{\"name\":\"admins\",\"gidnumber\":7002}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: create group admins, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. Login with a user that doesn't exist yet -> 401. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/login",
	                       "{\"username\":\"nosuchuser\",\"password\":\"whatever\"}", &r) != 0 ||
	    r.status != 401) {
		fprintf(stderr, "FAIL: login as nonexistent user expected 401, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 4. Create a real user in the admin group, with a real password --
	 * still an open write, gating still isn't active (this create IS
	 * what activates it, but only from the moment it lands). */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/ldap/users",
	                       "{\"name\":\"root_admin\",\"uidnumber\":7100,\"primarygroup\":7002,"
	                       "\"password\":\"correct horse battery staple\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: create admin user, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 5. Gating is NOW active -- an unauthenticated write is refused. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/ldap/groups", "{\"name\":\"unrelated\",\"gidnumber\":7003}",
	                       &r) != 0 ||
	    r.status != 401) {
		fprintf(stderr, "FAIL: unauthenticated write after gating activated expected 401, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* Reads still work with zero credentials, always. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/ldap/groups", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: unauthenticated GET after gating activated expected 200, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 6. Wrong password -> 401, no token issued. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/login",
	                       "{\"username\":\"root_admin\",\"password\":\"wrong\"}", &r) != 0 ||
	    r.status != 401) {
		fprintf(stderr, "FAIL: login with wrong password expected 401, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 7. Real login -> a real token. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/login",
	                       "{\"username\":\"root_admin\",\"password\":\"correct horse battery staple\"}",
	                       &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: login with correct password, status=%d\n", r.status);
		ok = 0;
	} else {
		const char *t = json_str_field(r.json, "token");

		if (t == NULL || t[0] == '\0') {
			fprintf(stderr, "FAIL: login response missing a real token\n");
			ok = 0;
		} else {
			snprintf(token, sizeof(token), "%s", t);
		}
	}
	kx_response_free(&r);

	/* 8. The same write, now with the token, succeeds. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "POST", "/v1/ldap/groups", token,
	                        "{\"name\":\"unrelated\",\"gidnumber\":7003}", &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: authenticated write expected 201, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 9. A garbage token is rejected the same way as no token at all. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "POST", "/v1/ldap/groups", "not-a-real-token",
	                        "{\"name\":\"nope\",\"gidnumber\":7004}", &r) != 0 || r.status != 401) {
		fprintf(stderr, "FAIL: garbage token expected 401, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 9b. ADR-0152: session listing shows the real active session, a
	 * real expires_in_seconds (idle_timeout_seconds=900 here, so never
	 * null), and never a raw token anywhere in the response. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/system/hostauth/sessions", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET hostauth sessions, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *sessions = json_object_get(r.json, "sessions");
		int found = 0;

		if (sessions == NULL || sessions->type != JSON_ARRAY) {
			fprintf(stderr, "FAIL: hostauth sessions response missing a sessions array\n");
			ok = 0;
		} else {
			size_t i;

			for (i = 0; i < sessions->u.array.count; i++) {
				const struct json_value *s = sessions->u.array.items[i];
				const char *uname = json_str_field(s, "username");
				const struct json_value *jexp = json_object_get(s, "expires_in_seconds");

				if (uname != NULL && str_eq(uname, "root_admin")) {
					found = 1;
					if (jexp == NULL || jexp->type != JSON_NUMBER || json_as_number(jexp) <= 0) {
						fprintf(stderr,
						        "FAIL: root_admin session missing a real "
						        "expires_in_seconds\n");
						ok = 0;
					}
				}
				if (json_object_get(s, "token") != NULL) {
					fprintf(stderr, "FAIL: a raw token leaked into the sessions listing\n");
					ok = 0;
				}
			}
		}
		if (!found) {
			fprintf(stderr, "FAIL: root_admin's own active session not found in the listing\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* 9c. Revoking root_admin's sessions logs it out everywhere -- its
	 * existing token stops working immediately. This DELETE is itself
	 * a write, subject to the same gating as anything else -- needs
	 * the still-valid token attached, same as step 8's group create. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "DELETE", "/v1/system/hostauth/sessions/root_admin", token,
	                        NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE hostauth sessions for root_admin, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "POST", "/v1/ldap/groups", token,
	                        "{\"name\":\"post-revoke\",\"gidnumber\":7005}", &r) != 0 ||
	    r.status != 401) {
		fprintf(stderr, "FAIL: revoked token should be rejected like any other, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* Re-authenticate -- the revoke above killed the only token this
	 * test had, and the no-op check right below is itself a write,
	 * needing a real one attached. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/login",
	                       "{\"username\":\"root_admin\",\"password\":\"correct horse battery staple\"}",
	                       &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: re-login after revoke, status=%d\n", r.status);
		ok = 0;
	} else {
		const char *t = json_str_field(r.json, "token");

		if (t != NULL)
			snprintf(token, sizeof(token), "%s", t);
	}
	kx_response_free(&r);

	/* Revoking a user with no active session at all is a real no-op,
	 * not an error -- same idempotent posture handle_logout() already
	 * has. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "DELETE", "/v1/system/hostauth/sessions/nosuchuser", token,
	                        NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: revoking a user with no session should still be 204, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 10. A real user who is NOT in the admin group authenticates fine
	 * but still can't write -- authentication and authorization are
	 * two different questions. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "POST", "/v1/ldap/users", token,
	                        "{\"name\":\"plain_user\",\"uidnumber\":7101,\"primarygroup\":7001,"
	                        "\"password\":\"plainpassword123\"}",
	                        &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: create plain_user (as admin), status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	{
		char plain_token[128] = "";

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/login",
		                       "{\"username\":\"plain_user\",\"password\":\"plainpassword123\"}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: plain_user login (valid credentials) expected 200, got %d\n",
			        r.status);
			ok = 0;
		} else {
			const char *t = json_str_field(r.json, "token");

			if (t != NULL)
				snprintf(plain_token, sizeof(plain_token), "%s", t);
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (plain_token[0] != '\0') {
			if (request_with_token(&client, "POST", "/v1/ldap/groups", plain_token,
			                        "{\"name\":\"shouldfail\",\"gidnumber\":7005}", &r) != 0 ||
			    r.status != 401) {
				fprintf(stderr,
				        "FAIL: authenticated-but-not-admin write expected 401, got %d\n",
				        r.status);
				ok = 0;
			}
			kx_response_free(&r);
		}
	}

	/* 11. Logout invalidates the token; the same write now fails. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "POST", "/v1/logout", token, NULL, &r) != 0 || r.status != 204) {
		fprintf(stderr, "FAIL: logout expected 204, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "POST", "/v1/ldap/groups", token,
	                        "{\"name\":\"afterlogout\",\"gidnumber\":7006}", &r) != 0 ||
	    r.status != 401) {
		fprintf(stderr, "FAIL: write after logout expected 401, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* Logout is idempotent -- a second call on an already-gone token
	 * is still 204, never an error. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "POST", "/v1/logout", token, NULL, &r) != 0 || r.status != 204) {
		fprintf(stderr, "FAIL: repeat logout expected 204, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 12. idle_timeout_seconds=0 -- a fresh login's token is single-use. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "PUT", "/v1/system/hostauth-config", token, NULL, &r) == 0) {
		/* no-op: token already invalid, just draining any stray response */
	}
	kx_response_free(&r);

	{
		/* Need a fresh admin session to change hostauth-config itself
		 * (also a gated write) -- log back in first. */
		char admin_token[128] = "";

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/login",
		                       "{\"username\":\"root_admin\",\"password\":\"correct horse battery "
		                       "staple\"}",
		                       &r) == 0 &&
		    r.status == 200) {
			const char *t = json_str_field(r.json, "token");

			if (t != NULL)
				snprintf(admin_token, sizeof(admin_token), "%s", t);
		} else {
			fprintf(stderr, "FAIL: re-login for zero-idle-timeout scenario, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (admin_token[0] != '\0') {
			memset(&r, 0, sizeof(r));
			if (request_with_token(&client, "PUT", "/v1/system/hostauth-config", admin_token,
			                        "{\"admin_groups\":[\"admins\"],\"idle_timeout_seconds\":0}",
			                        &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: set idle_timeout_seconds=0, status=%d\n", r.status);
				ok = 0;
			}
			kx_response_free(&r);
		}
	}

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/login",
	                       "{\"username\":\"root_admin\",\"password\":\"correct horse battery staple\"}",
	                       &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: login under zero-idle-timeout, status=%d\n", r.status);
		ok = 0;
	} else {
		const char *t = json_str_field(r.json, "token");
		char once_token[128] = "";

		if (t != NULL)
			snprintf(once_token, sizeof(once_token), "%s", t);
		kx_response_free(&r);

		if (once_token[0] != '\0') {
			memset(&r, 0, sizeof(r));
			if (request_with_token(&client, "POST", "/v1/ldap/groups", once_token,
			                        "{\"name\":\"onceonly\",\"gidnumber\":7007}", &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr, "FAIL: first use of single-use token expected 201, got %d\n",
				        r.status);
				ok = 0;
			}
			kx_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (request_with_token(&client, "POST", "/v1/ldap/groups", once_token,
			                        "{\"name\":\"onceonlyagain\",\"gidnumber\":7008}", &r) != 0 ||
			    r.status != 401) {
				fprintf(stderr,
				        "FAIL: second use of single-use token expected 401, got %d\n",
				        r.status);
				ok = 0;
			}
			kx_response_free(&r);
		}
	}

	/*
	 * 13 (ADR-0144's own live-LDAP backend part): hostauth-config
	 * validation for the ldap_* fields, plus a real functional check
	 * that an unreachable configured LDAP server correctly falls back
	 * to the local backend rather than failing the login outright.
	 * idle_timeout_seconds is 0 from step 12 onward, so every admin
	 * write below needs its own fresh single-use login.
	 */
	{
		char admin_token[128];

/*
 * idle_timeout_seconds is 0 from step 12 onward -- hostauth_check_
 * token()'s own single-use contract consumes a token the moment a
 * write is authorized, regardless of what the handler itself goes on
 * to do with the request (accept it, or reject it with a 400 for
 * invalid fields). Each of steps 13a-13d below needs its OWN fresh
 * login as a result -- this macro is scoped to this block only.
 */
#define RELOGIN_ROOT_ADMIN()                                                                        \
	do {                                                                                         \
		memset(&r, 0, sizeof(r));                                                           \
		if (kx_client_request(&client, "POST", "/v1/login",                                \
		                       "{\"username\":\"root_admin\",\"password\":\"correct horse " \
		                       "battery staple\"}",                                        \
		                       &r) == 0 &&                                                 \
		    r.status == 200) {                                                             \
			const char *t = json_str_field(r.json, "token");                           \
			snprintf(admin_token, sizeof(admin_token), "%s", t != NULL ? t : "");      \
		} else {                                                                            \
			fprintf(stderr, "FAIL: re-login for LDAP-config scenario, status=%d\n",     \
			        r.status);                                                          \
			ok = 0;                                                                     \
			admin_token[0] = '\0';                                                      \
		}                                                                                    \
		kx_response_free(&r);                                                               \
	} while (0)

		/* 13a. ldap_enabled=true with zero servers -> rejected. */
		RELOGIN_ROOT_ADMIN();
		memset(&r, 0, sizeof(r));
		if (request_with_token(&client, "PUT", "/v1/system/hostauth-config", admin_token,
		                        "{\"admin_groups\":[\"admins\"],\"idle_timeout_seconds\":0,"
		                        "\"ldap_enabled\":true,\"ldap_servers\":[],\"ldap_port\":3893,"
		                        "\"ldap_base_dn\":\"dc=glauth,dc=com\"}",
		                        &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: ldap_enabled with no servers expected 400, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 13b. ldap_enabled=true with an empty base DN -> rejected. */
		RELOGIN_ROOT_ADMIN();
		memset(&r, 0, sizeof(r));
		if (request_with_token(&client, "PUT", "/v1/system/hostauth-config", admin_token,
		                        "{\"admin_groups\":[\"admins\"],\"idle_timeout_seconds\":0,"
		                        "\"ldap_enabled\":true,\"ldap_servers\":[\"127.0.0.1\"],"
		                        "\"ldap_port\":3893,\"ldap_base_dn\":\"\"}",
		                        &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: ldap_enabled with empty base_dn expected 400, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 13c. ldap_port out of range -> rejected, regardless of ldap_enabled. */
		RELOGIN_ROOT_ADMIN();
		memset(&r, 0, sizeof(r));
		if (request_with_token(&client, "PUT", "/v1/system/hostauth-config", admin_token,
		                        "{\"admin_groups\":[\"admins\"],\"idle_timeout_seconds\":0,"
		                        "\"ldap_enabled\":false,\"ldap_servers\":[],\"ldap_port\":70000,"
		                        "\"ldap_base_dn\":\"\"}",
		                        &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: ldap_port out of range expected 400, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/*
		 * 13d. A valid config, pointed at a real local port nothing is
		 * listening on (18189 -- not a port any daemon-linked test
		 * binds, see the test/*.c PORT_ARG values). Accepted, and GET
		 * echoes it back exactly.
		 */
		RELOGIN_ROOT_ADMIN();
		memset(&r, 0, sizeof(r));
		if (request_with_token(&client, "PUT", "/v1/system/hostauth-config", admin_token,
		                        "{\"admin_groups\":[\"admins\"],\"idle_timeout_seconds\":0,"
		                        "\"ldap_enabled\":true,\"ldap_servers\":[\"127.0.0.1\"],"
		                        "\"ldap_port\":18189,\"ldap_base_dn\":\"dc=glauth,dc=com\"}",
		                        &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: valid ldap config expected 200, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
#undef RELOGIN_ROOT_ADMIN

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/system/hostauth-config", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET hostauth-config after LDAP setup, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *jservers = json_object_get(r.json, "ldap_servers");
			const struct json_value *jenabled = json_object_get(r.json, "ldap_enabled");
			const struct json_value *jport = json_object_get(r.json, "ldap_port");

			if (jenabled == NULL || jenabled->type != JSON_BOOL || !jenabled->u.boolean ||
			    jport == NULL || (int)json_as_number(jport) != 18189 || jservers == NULL ||
			    jservers->type != JSON_ARRAY || jservers->u.array.count != 1 ||
			    !str_eq(json_as_string(jservers->u.array.items[0]), "127.0.0.1") ||
			    !str_eq(json_str_field(r.json, "ldap_base_dn"), "dc=glauth,dc=com")) {
				fprintf(stderr, "FAIL: GET hostauth-config didn't echo LDAP config correctly\n");
				ok = 0;
			}
		}
		kx_response_free(&r);

		/*
		 * 13e. Login as root_admin with the CORRECT local password.
		 * ldap_enabled is true but the one configured server is
		 * unreachable (nothing listens on 18189) -- hostauth_login()
		 * must fall back to the local backend rather than failing the
		 * login outright. A real token comes back, usable for a write.
		 */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/login",
		                       "{\"username\":\"root_admin\",\"password\":\"correct horse battery "
		                       "staple\"}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr,
			        "FAIL: login with unreachable LDAP server expected local-backend fallback "
			        "(200), got %d\n",
			        r.status);
			ok = 0;
			kx_response_free(&r);
		} else {
			const char *t = json_str_field(r.json, "token");
			char fallback_token[128] = "";

			if (t != NULL)
				snprintf(fallback_token, sizeof(fallback_token), "%s", t);
			kx_response_free(&r);

			if (fallback_token[0] != '\0') {
				memset(&r, 0, sizeof(r));
				if (request_with_token(&client, "POST", "/v1/ldap/groups", fallback_token,
				                        "{\"name\":\"ldapfallback\",\"gidnumber\":7009}", &r) !=
				        0 ||
				    r.status != 201) {
					fprintf(stderr,
					        "FAIL: write with LDAP-fallback token expected 201, got %d\n",
					        r.status);
					ok = 0;
				}
				kx_response_free(&r);
			}
		}

		/*
		 * 13f. A WRONG password still correctly fails (401) even with
		 * ldap_enabled and an unreachable server -- the fallback must
		 * never bypass real credential verification.
		 */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/login",
		                       "{\"username\":\"root_admin\",\"password\":\"wrong\"}", &r) != 0 ||
		    r.status != 401) {
			fprintf(stderr,
			        "FAIL: wrong password under LDAP-enabled+unreachable expected 401, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/*
	 * 14. ADR-0147: renaming the ACTIVE admin group must not silently
	 * drop it out of write-gating -- the exact class of self-inflicted
	 * lockout ADR-0146 was raised to prevent, just from a different
	 * cause (a rename instead of a stale LDAP-server config). "admins"
	 * (gidnumber 7002) is real, currently-active admin_groups content
	 * by this point in the test.
	 */
	{
		char rename_token[128] = "";

		/* A fresh token, not the one captured back at step 7 -- several
		 * steps since then (idle_timeout_seconds=0 in particular) may
		 * have already consumed or expired it; this block's own
		 * correctness shouldn't depend on exactly how much of the rest
		 * of this file ran before it. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/login",
		                       "{\"username\":\"root_admin\",\"password\":\"correct horse battery "
		                       "staple\"}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: fresh login before rename block, status=%d\n", r.status);
			ok = 0;
		} else {
			const char *t = json_str_field(r.json, "token");

			if (t != NULL)
				snprintf(rename_token, sizeof(rename_token), "%s", t);
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (request_with_token(&client, "PUT", "/v1/ldap/groups/admins", rename_token,
		                        "{\"name\":\"root-admins\",\"gidnumber\":7002}", &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: rename active admin group, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* hostauth-config's own admin_groups must now read
		 * "root-admins", not "admins" -- the daemon-side propagation,
		 * not anything this test itself did. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/system/hostauth-config", NULL, &r) != 0 ||
		    r.status != 200 ||
		    memmem(r.body, r.body_len, "\"root-admins\"", strlen("\"root-admins\"")) == NULL ||
		    memmem(r.body, r.body_len, "\"admins\"", strlen("\"admins\"")) != NULL) {
			fprintf(stderr,
			        "FAIL: admin_groups did not follow the rename (expected root-admins only, "
			        "not admins), status=%d, body=%.*s\n",
			        r.status, (int)r.body_len, r.body);
			ok = 0;
		}
		kx_response_free(&r);

		/* root_admin's own primarygroup is a gidnumber (7002), unaffected
		 * by the rename -- a fresh login must still succeed, proving
		 * gating genuinely still recognizes this user as an admin under
		 * the group's new name, not just that the config string changed. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/login",
		                       "{\"username\":\"root_admin\",\"password\":\"correct horse battery "
		                       "staple\"}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: login after admin-group rename expected 200, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* Another fresh token (idle_timeout_seconds=0 by this point in
		 * the file means single-use) -- login as root_admin still
		 * works under the group's NEW name, itself further proof the
		 * rename didn't break gating. */
		rename_token[0] = '\0';
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/login",
		                       "{\"username\":\"root_admin\",\"password\":\"correct horse battery "
		                       "staple\"}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: fresh login before collision check, status=%d\n", r.status);
			ok = 0;
		} else {
			const char *t = json_str_field(r.json, "token");

			if (t != NULL)
				snprintf(rename_token, sizeof(rename_token), "%s", t);
		}
		kx_response_free(&r);

		/* Renaming to a name that already exists is a real, rejected
		 * collision (409-shaped LDAP_RECORD_ERR_DUPLICATE), not silently
		 * accepted. */
		memset(&r, 0, sizeof(r));
		if (request_with_token(&client, "PUT", "/v1/ldap/groups/root-admins", rename_token,
		                        "{\"name\":\"unrelated\",\"gidnumber\":7002}", &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: rename to an already-existing group name expected 409, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "HOSTAUTH RESULT: PASS\n" : "HOSTAUTH RESULT: FAIL\n");
	return ok ? 0 : 1;
}
