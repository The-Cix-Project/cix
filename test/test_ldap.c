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
                                  const char *want_db_path)
{
	const struct json_value *servers = json_object_get(root, "servers");
	size_t i;

	if (servers == NULL || servers->type != JSON_ARRAY)
		return 0;
	for (i = 0; i < servers->u.array.count; i++) {
		const struct json_value *item = servers->u.array.items[i];

		if (str_eq(json_str_field(item, "container"), container)) {
			if (want_db_path != NULL)
				return str_eq(json_str_field(item, "db_path"), want_db_path);
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
	                       "{\"container\":\"no-such-container\",\"db_path\":\"/var/lib/glauth/gl.db\"}",
	                       &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: register nonexistent container expected 404, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. validation: non-absolute db_path -> 400 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/ldap/servers",
	                       "{\"container\":\"ldapsrv\",\"db_path\":\"var/lib/glauth/gl.db\"}", &r) !=
	        0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: non-absolute db_path expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 4. a real, valid registration -> 201, echoes container/db_path */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/ldap/servers",
	                       "{\"container\":\"ldapsrv\",\"db_path\":\"/var/lib/glauth/gl.db\"}", &r) !=
	        0 ||
	    r.status != 201 || !str_eq(json_str_field(r.json, "container"), "ldapsrv") ||
	    !str_eq(json_str_field(r.json, "db_path"), "/var/lib/glauth/gl.db")) {
		fprintf(stderr, "FAIL: register ldapsrv, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 5. duplicate registration -> 409 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/ldap/servers",
	                       "{\"container\":\"ldapsrv\",\"db_path\":\"/var/lib/glauth/gl.db\"}", &r) !=
	        0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate registration expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 6. GET reflects it */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/ldap/servers", NULL, &r) != 0 || r.status != 200 ||
	    !servers_list_contains(r.json, "ldapsrv", "/var/lib/glauth/gl.db")) {
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
		    r.status != 200 || !servers_list_contains(r.json, "ldapsrv", "/var/lib/glauth/gl.db")) {
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
	                       "{\"container\":\"ldapsrv2\",\"db_path\":\"/var/lib/glauth/gl.db\"}", &r) !=
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

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);

	printf("LDAP RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
