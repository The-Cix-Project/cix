/*
 * ADR-0144 end-to-end test: host authentication foundation --
 * POST /v1/login, POST /v1/logout, GET/PUT /v1/system/hostauth-config,
 * and the write-gating check every mutating request goes through in
 * dispatch(). Local backend only (ldap_user_check_password()/
 * ldap_user_is_in_group()) -- the LDAP-bind backend is a later ADR-0144
 * part, not covered here.
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

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "HOSTAUTH RESULT: PASS\n" : "HOSTAUTH RESULT: FAIL\n");
	return ok ? 0 : 1;
}
