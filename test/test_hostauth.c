/*
 * ADR-0144 end-to-end test: host authentication foundation --
 * POST /v1/login, POST /v1/logout, GET/PUT /v1/system/hostauth-config,
 * and the write-gating check every mutating request goes through in
 * dispatch(). Covers the local backend (ldap_user_check_password()/
 * ldap_user_is_in_group()) in full, plus the live-LDAP backend's own
 * config validation and its unreachable-server-falls-back-to-local
 * path (daemon/src/hostauth.c's try_ldap_client()). A real successful
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
#include "base64.h"
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
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

/*
 * #370: -1 when the key is absent or is not a boolean at all, so a
 * missing field fails loudly instead of reading as false -- the whole
 * point of these two fields is that their ABSENCE was the bug.
 */
static int json_bool_field(const struct json_value *obj, const char *key)
{
	const struct json_value *v = json_object_get(obj, key);

	if (v == NULL || v->type != JSON_BOOL)
		return -1;
	return v->u.boolean ? 1 : 0;
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

/* cix_client_request() has no built-in header-injection support beyond
 * what httpclient.h exposes -- check for an authenticated-request
 * helper; if none exists, requests needing Authorization go through
 * cix_client_request_with_header() below (added here, matching this
 * test's own real need rather than growing the shared client for a
 * single caller). */
static int request_with_token(const struct cix_client *c, const char *method, const char *path,
                               const char *token, const char *body, struct cix_response *r)
{
	return cix_client_request_with_auth(c, method, path, token, body, r);
}

/*
 * #541: once gating is active every read needs a session too. From
 * step 12 on idle_timeout_seconds is 0, so a session is single-use and
 * a read consumes it exactly as a write does -- each read here logs in
 * afresh, which is also what an operator with that setting does.
 */
static int admin_get(const struct cix_client *c, const char *path, struct cix_response *r)
{
	struct cix_response l;
	char tok[128] = "";

	memset(&l, 0, sizeof(l));
	if (cix_client_request(c, "POST", "/v1/login",
	                       "{\"username\":\"root_admin\",\"password\":\"correct horse battery "
	                       "staple\"}",
	                       &l) == 0 &&
	    l.status == 200 && json_str_field(l.json, "token") != NULL)
		snprintf(tok, sizeof(tok), "%s", json_str_field(l.json, "token"));
	cix_response_free(&l);
	return request_with_token(c, "GET", path, tok, NULL, r);
}

/*
 * #541: a WebSocket upgrade, sent raw -- the client library speaks plain
 * HTTP and cannot. The console and the build-log stream are answered
 * before the daemon's ordinary dispatch, which is why they were never
 * gated by the check this test otherwise exercises; this is how the
 * test reaches them. extra is one more header line with its CRLF, or
 * "". Returns the response's status code, -1 if there was none.
 */
static int ws_upgrade_status(const char *path, const char *extra)
{
	struct sockaddr_in sa;
	struct timeval tv = { 10, 0 };
	char req[1024], resp[256];
	size_t got = 0;
	int fd, n, status = -1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(TEST_PORT);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		close(fd);
		return -1;
	}
	n = snprintf(req, sizeof(req),
	             "GET %s HTTP/1.1\r\n"
	             "Host: 127.0.0.1\r\n"
	             "Upgrade: websocket\r\n"
	             "Connection: Upgrade\r\n"
	             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
	             "Sec-WebSocket-Version: 13\r\n"
	             "%s"
	             "\r\n",
	             path, extra);
	if (n < 0 || (size_t)n >= sizeof(req) || write(fd, req, (size_t)n) != n) {
		close(fd);
		return -1;
	}
	while (got < sizeof(resp) - 1) {
		ssize_t r = read(fd, resp + got, sizeof(resp) - 1 - got);

		if (r <= 0)
			break;
		got += (size_t)r;
		resp[got] = '\0';
		if (strstr(resp, "\r\n") != NULL)
			break;
	}
	resp[got] = '\0';
	if (sscanf(resp, "HTTP/1.1 %d", &status) != 1)
		status = -1;
	close(fd);
	return status;
}

/*
 * One raw HTTP request, sent without the client library -- which sends
 * only Bearer tokens and always from 127.0.0.1. src_ip binds the source
 * address when not NULL: the #547 authentication throttle exempts
 * loopback's 127.0.0.1 exactly as the TLS throttle does, so a test of
 * it connects from another loopback address (127.0.0.2), which the
 * daemon sees as an ordinary peer. auth_line is one header line with
 * its CRLF, or "". The daemon closes the connection after its response,
 * so this reads to EOF. Returns the status (-1 if none) and copies the
 * body into body.
 */
static int raw_http(const char *src_ip, const char *method, const char *path,
                    const char *auth_line, const char *req_body, char *body, size_t body_size)
{
	struct sockaddr_in sa;
	struct timeval tv = { 30, 0 };
	char req[2048], resp[16384];
	const char *hdr_end;
	size_t got = 0;
	int fd, n, status = -1;

	if (body_size > 0)
		body[0] = '\0';
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (src_ip != NULL) {
		struct sockaddr_in src;

		memset(&src, 0, sizeof(src));
		src.sin_family = AF_INET;
		if (inet_pton(AF_INET, src_ip, &src.sin_addr) != 1 ||
		    bind(fd, (struct sockaddr *)&src, sizeof(src)) != 0) {
			close(fd);
			return -1;
		}
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(TEST_PORT);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		close(fd);
		return -1;
	}
	n = snprintf(req, sizeof(req),
	             "%s %s HTTP/1.1\r\n"
	             "Host: 127.0.0.1\r\n"
	             "%s"
	             "Content-Type: application/json\r\n"
	             "Content-Length: %zu\r\n"
	             "Connection: close\r\n"
	             "\r\n"
	             "%s",
	             method, path, auth_line, req_body != NULL ? strlen(req_body) : 0,
	             req_body != NULL ? req_body : "");
	if (n < 0 || (size_t)n >= sizeof(req) || write(fd, req, (size_t)n) != n) {
		close(fd);
		return -1;
	}
	while (got < sizeof(resp) - 1) {
		ssize_t r = read(fd, resp + got, sizeof(resp) - 1 - got);

		if (r <= 0)
			break;
		got += (size_t)r;
	}
	resp[got] = '\0';
	close(fd);
	if (sscanf(resp, "HTTP/1.1 %d", &status) != 1)
		return -1;
	hdr_end = strstr(resp, "\r\n\r\n");
	if (hdr_end != NULL && body_size > 0)
		snprintf(body, body_size, "%s", hdr_end + 4);
	return status;
}

/* #543: one request authenticated with an app password as HTTP Basic. */
static int basic_request_from(const char *src_ip, const char *method, const char *path,
                              const char *user, const char *secret, const char *req_body,
                              char *body, size_t body_size)
{
	char cred[256], cred_b64[400], line[480];

	snprintf(cred, sizeof(cred), "%s:%s", user, secret);
	if (base64_encode((const unsigned char *)cred, strlen(cred), cred_b64, sizeof(cred_b64)) != 0)
		return -1;
	snprintf(line, sizeof(line), "Authorization: Basic %s\r\n", cred_b64);
	return raw_http(src_ip, method, path, line, req_body, body, body_size);
}

static int basic_request(const char *method, const char *path, const char *user,
                         const char *secret, const char *req_body, char *body, size_t body_size)
{
	return basic_request_from(NULL, method, path, user, secret, req_body, body, body_size);
}

/*
 * ADR-0317 (#540): the group -> permission mapping, on a daemon of its
 * own, and (#541) the refusals that keep someone holding identity:write.
 * Until step 5 nobody holds identity:write, so gating is inactive and
 * requests go without credentials (whoami is asked with a real
 * session); from step 5 on, keeper holds it and every request carries
 * keeper's session.
 */
static int body_has(const struct cix_response *r, const char *needle)
{
	return r->body != NULL && strstr(r->body, needle) != NULL;
}

static int expect_status(const struct cix_client *c, const char *token, const char *method,
                         const char *path, const char *body, int want, const char *what)
{
	struct cix_response r;
	int ok = 1;

	memset(&r, 0, sizeof(r));
	if (request_with_token(c, method, path, token, body, &r) != 0 || r.status != want) {
		fprintf(stderr, "FAIL: #540 %s: expected %d, got %d (%.200s)\n", what, want, r.status,
		        r.body != NULL ? r.body : "");
		ok = 0;
	}
	cix_response_free(&r);
	return ok;
}

/* Logs in as user; 1 with the session token in out, 0 otherwise. */
static int login_as(const struct cix_client *c, const char *user, const char *password, char *out,
                    size_t out_size)
{
	char body[256];
	struct cix_response r;
	int ok = 0;

	snprintf(body, sizeof(body), "{\"username\":\"%s\",\"password\":\"%s\"}", user, password);
	memset(&r, 0, sizeof(r));
	out[0] = '\0';
	if (cix_client_request(c, "POST", "/v1/login", body, &r) == 0 && r.status == 200 &&
	    json_str_field(r.json, "token") != NULL) {
		snprintf(out, out_size, "%s", json_str_field(r.json, "token"));
		ok = 1;
	}
	cix_response_free(&r);
	return ok;
}

static int test_permission_mapping(void)
{
	char saved_data_dir[PATH_MAX], dir[PATH_MAX], path[PATH_MAX], file[4096];
	char ktok[128] = "";
	struct cix_client c;
	struct cix_response r;
	pid_t pid;
	int ok = 1;
	FILE *f;
	size_t n;

	snprintf(saved_data_dir, sizeof(saved_data_dir), "%s", g_data_dir);
	if (test_data_dir_create(dir, sizeof(dir)) != 0)
		return 0;
	/* A pre-#540 state file: admin_groups and no "permissions". It
	 * carries the #542 marker, so the standard groups stay out of what
	 * this scenario counts; test_standard_groups() covers them. */
	snprintf(path, sizeof(path), "%s/state/hostauth_config.json", dir);
	f = fopen(path, "w");
	if (f == NULL) {
		fprintf(stderr, "FAIL: #540 could not write the pre-#540 state file\n");
		test_data_dir_cleanup(dir);
		return 0;
	}
	fputs("{\"admin_groups\":[\"legacyadmins\"],\"idle_timeout_seconds\":900,"
	      "\"standard_groups_provisioned\":true}\n",
	      f);
	fclose(f);
	snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir);

	cix_client_init(&c, "127.0.0.1", TEST_PORT);
	pid = start_daemon();
	if (pid < 0 || wait_for_daemon(&c, 50) != 0) {
		fprintf(stderr, "FAIL: #540 daemon never accepted connections\n");
		ok = 0;
		goto out;
	}

	/* 1. Migration: the old admin group now holds every permission. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&c, "GET", "/v1/system/hostauth/permissions", NULL, &r) != 0 ||
	    r.status != 200 || r.json == NULL) {
		fprintf(stderr, "FAIL: #540 GET permissions, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *vocab = json_object_get(r.json, "vocabulary");
		const struct json_value *groups = json_object_get(r.json, "groups");
		const struct json_value *legacy = json_object_get(groups, "legacyadmins");

		if (vocab == NULL || vocab->type != JSON_ARRAY || vocab->u.array.count < 10 ||
		    legacy == NULL || legacy->type != JSON_ARRAY ||
		    legacy->u.array.count != vocab->u.array.count) {
			fprintf(stderr, "FAIL: #540 a pre-#540 admin group was not migrated to every "
			                "permission: %.300s\n",
			        r.body != NULL ? r.body : "");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 2. A group granted some permissions is not an admin group. */
	ok &= expect_status(&c, NULL, "PUT", "/v1/system/hostauth/permissions/operators",
	                    "{\"permissions\":[\"containers:read\",\"containers:operate\"]}", 200,
	                    "PUT a partial group");
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&c, "GET", "/v1/system/hostauth-config", NULL, &r) != 0 ||
	    r.json == NULL) {
		ok = 0;
	} else {
		const struct json_value *admins = json_object_get(r.json, "admin_groups");

		if (admins == NULL || admins->type != JSON_ARRAY || admins->u.array.count != 1 ||
		    !str_eq(json_as_string(admins->u.array.items[0]), "legacyadmins")) {
			fprintf(stderr, "FAIL: #540 admin_groups is not exactly the groups holding every "
			                "permission: %.200s\n",
			        r.body != NULL ? r.body : "");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 3. A word outside the vocabulary is refused, by name. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&c, "PUT", "/v1/system/hostauth/permissions/operators",
	                       "{\"permissions\":[\"containers:fly\"]}", &r) != 0 ||
	    r.status != 400 || !body_has(&r, "containers:fly")) {
		fprintf(stderr, "FAIL: #540 an unknown permission must be a 400 naming it, got %d "
		                "(%.200s)\n",
		        r.status, r.body != NULL ? r.body : "");
		ok = 0;
	}
	cix_response_free(&r);

	/* 4. Union: a user in two groups holds both groups' permissions. */
	ok &= expect_status(&c, NULL, "POST", "/v1/ldap/groups", "{\"name\":\"permg1\",\"gidnumber\":7301}",
	                    201, "create permg1");
	ok &= expect_status(&c, NULL, "POST", "/v1/ldap/groups", "{\"name\":\"permg2\",\"gidnumber\":7302}",
	                    201, "create permg2");
	ok &= expect_status(&c, NULL, "PUT", "/v1/system/hostauth/permissions/permg1",
	                    "{\"permissions\":[\"containers:read\"]}", 200, "grant permg1");
	ok &= expect_status(&c, NULL, "PUT", "/v1/system/hostauth/permissions/permg2",
	                    "{\"permissions\":[\"images:write\"]}", 200, "grant permg2");
	ok &= expect_status(&c, NULL, "POST", "/v1/ldap/users",
	                    "{\"name\":\"permuser\",\"uidnumber\":7310,\"primarygroup\":7301,"
	                    "\"secondary_groups\":[7302],\"password\":\"union of two groups\"}",
	                    201, "create permuser");
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&c, "POST", "/v1/login",
	                       "{\"username\":\"permuser\",\"password\":\"union of two groups\"}",
	                       &r) != 0 ||
	    r.status != 200 || r.json == NULL || json_str_field(r.json, "token") == NULL) {
		fprintf(stderr, "FAIL: #540 login as permuser, status=%d\n", r.status);
		ok = 0;
		cix_response_free(&r);
	} else {
		char tok[128];
		struct cix_response w;

		snprintf(tok, sizeof(tok), "%s", json_str_field(r.json, "token"));
		cix_response_free(&r);
		memset(&w, 0, sizeof(w));
		if (request_with_token(&c, "GET", "/v1/whoami", tok, NULL, &w) != 0 || w.json == NULL) {
			ok = 0;
		} else {
			const struct json_value *p = json_object_get(w.json, "permissions");

			if (p == NULL || p->type != JSON_ARRAY || p->u.array.count != 2 ||
			    !body_has(&w, "\"containers:read\"") || !body_has(&w, "\"images:write\"")) {
				fprintf(stderr, "FAIL: #540 whoami must report exactly the union of the "
				                "user's groups: %.300s\n",
				        w.body != NULL ? w.body : "");
				ok = 0;
			}
		}
		cix_response_free(&w);
	}

	/*
	 * 5. No lockout: once someone holds identity:write, nothing may
	 * leave nobody holding it.
	 *
	 * This is also where gating turns ON (#541): it is active while
	 * any enabled user holds identity:write, so from the moment keeper
	 * exists every request below needs keeper's session -- and keepers
	 * is given identity:read as well, since reading the mapping is a
	 * read like any other. Everything above ran on an ungated box,
	 * which is the other half of the rule: a box where nobody can
	 * grant permissions answers everything.
	 */
	ok &= expect_status(&c, NULL, "POST", "/v1/ldap/groups",
	                    "{\"name\":\"keepers\",\"gidnumber\":7303}", 201, "create keepers");
	ok &= expect_status(&c, NULL, "PUT", "/v1/system/hostauth/permissions/keepers",
	                    "{\"permissions\":[\"identity:read\",\"identity:write\"]}", 200,
	                    "grant keepers");
	ok &= expect_status(&c, NULL, "POST", "/v1/ldap/users",
	                    "{\"name\":\"keeper\",\"uidnumber\":7311,\"primarygroup\":7303,"
	                    "\"password\":\"keeps the keys\"}",
	                    201, "create keeper");
	ok &= expect_status(&c, NULL, "GET", "/v1/system/hostauth/permissions", NULL, 401,
	                    "an unauthenticated read once someone holds identity:write");
	if (!login_as(&c, "keeper", "keeps the keys", ktok, sizeof(ktok))) {
		fprintf(stderr, "FAIL: #540 login as keeper\n");
		ok = 0;
	}
	ok &= expect_status(&c, ktok, "PUT", "/v1/system/hostauth/permissions/keepers",
	                    "{\"permissions\":[\"identity:read\"]}", 409,
	                    "taking identity:write from its only holder");
	ok &= expect_status(&c, ktok, "DELETE", "/v1/system/hostauth/permissions/keepers", NULL, 409,
	                    "deleting the only identity:write grant");
	/* #541's user- and group-side refusals: the same rule, reached
	 * through the directory instead of the mapping. */
	ok &= expect_status(&c, ktok, "DELETE", "/v1/ldap/groups/keepers", NULL, 409,
	                    "deleting the only group granting identity:write");
	ok &= expect_status(&c, ktok, "PUT", "/v1/ldap/groups/keepers",
	                    "{\"name\":\"keepers\",\"gidnumber\":7399}", 409,
	                    "renumbering the only group granting identity:write");
	ok &= expect_status(&c, ktok, "DELETE", "/v1/ldap/users/keeper", NULL, 409,
	                    "deleting the only holder of identity:write");
	ok &= expect_status(&c, ktok, "PUT", "/v1/ldap/users/keeper",
	                    "{\"name\":\"keeper\",\"uidnumber\":7311,\"primarygroup\":7303,"
	                    "\"disabled\":true}",
	                    409, "disabling the only holder of identity:write");
	ok &= expect_status(&c, ktok, "PUT", "/v1/system/hostauth-config",
	                    "{\"admin_groups\":[],\"idle_timeout_seconds\":900}", 200,
	                    "clearing admin_groups when the grantor is not in a full group");
	/* With a second user holding identity:write, removing one of them
	 * is an ordinary act: only the LAST holder is protected. */
	ok &= expect_status(&c, ktok, "POST", "/v1/ldap/groups",
	                    "{\"name\":\"legacyadmins\",\"gidnumber\":7305}", 201,
	                    "create legacyadmins");
	ok &= expect_status(&c, ktok, "PUT", "/v1/system/hostauth/permissions/legacyadmins",
	                    "{\"permissions\":[\"identity:read\",\"identity:write\"]}", 200,
	                    "grant legacyadmins");
	ok &= expect_status(&c, ktok, "POST", "/v1/ldap/users",
	                    "{\"name\":\"second\",\"uidnumber\":7312,\"primarygroup\":7305}", 201,
	                    "create a second grantor");
	ok &= expect_status(&c, ktok, "DELETE", "/v1/ldap/users/second", NULL, 204,
	                    "deleting a grantor while another remains");

	/* 6. A rename carries the grants; a deleted group takes them with it. */
	ok &= expect_status(&c, ktok, "PUT", "/v1/ldap/groups/permg1",
	                    "{\"gidnumber\":7301,\"name\":\"permg1renamed\"}", 200, "rename permg1");
	ok &= expect_status(&c, ktok, "POST", "/v1/ldap/groups",
	                    "{\"name\":\"permg3\",\"gidnumber\":7304}", 201, "create permg3");
	ok &= expect_status(&c, ktok, "PUT", "/v1/system/hostauth/permissions/permg3",
	                    "{\"permissions\":[\"pki:read\"]}", 200, "grant permg3");
	ok &= expect_status(&c, ktok, "DELETE", "/v1/ldap/groups/permg3", NULL, 204,
	                    "delete permg3");
	memset(&r, 0, sizeof(r));
	if (request_with_token(&c, "GET", "/v1/system/hostauth/permissions", ktok, NULL, &r) != 0 ||
	    r.json == NULL) {
		ok = 0;
	} else {
		const struct json_value *groups = json_object_get(r.json, "groups");

		if (json_object_get(groups, "permg1renamed") == NULL ||
		    json_object_get(groups, "permg1") != NULL) {
			fprintf(stderr, "FAIL: #540 a renamed group's grants did not move with it: %.300s\n",
			        r.body != NULL ? r.body : "");
			ok = 0;
		}
		if (json_object_get(groups, "permg3") != NULL) {
			fprintf(stderr, "FAIL: #540 a deleted group is still in the mapping, so a new "
			                "group of that name would inherit its grants\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 7. The state file carries the mapping, and admin_groups for a
	 * rollback to a pre-#540 build; a restart keeps the mapping.
	 * Sessions live in memory, so the read after it logs in again. */
	f = fopen(path, "r");
	n = f != NULL ? fread(file, 1, sizeof(file) - 1, f) : 0;
	file[n] = '\0';
	if (f != NULL)
		fclose(f);
	if (strstr(file, "\"permissions\"") == NULL || strstr(file, "\"admin_groups\"") == NULL ||
	    strstr(file, "\"operators\"") == NULL) {
		fprintf(stderr, "FAIL: #540 the state file does not carry both the mapping and "
		                "admin_groups: %.300s\n",
		        file);
		ok = 0;
	}
	if (stop_daemon(pid) != 0)
		ok = 0;
	pid = start_daemon();
	if (pid < 0 || wait_for_daemon(&c, 50) != 0) {
		ok = 0;
		goto out;
	}
	if (!login_as(&c, "keeper", "keeps the keys", ktok, sizeof(ktok))) {
		fprintf(stderr, "FAIL: #540 login as keeper after the restart\n");
		ok = 0;
	}
	memset(&r, 0, sizeof(r));
	if (request_with_token(&c, "GET", "/v1/system/hostauth/permissions", ktok, NULL, &r) != 0 ||
	    !body_has(&r, "\"operators\":[\"containers:read\",\"containers:operate\"]")) {
		fprintf(stderr, "FAIL: #540 the mapping did not survive a restart: %.300s\n",
		        r.body != NULL ? r.body : "");
		ok = 0;
	}
	cix_response_free(&r);

out:
	if (pid > 0 && stop_daemon(pid) != 0)
		ok = 0;
	test_data_dir_cleanup(dir);
	snprintf(g_data_dir, sizeof(g_data_dir), "%s", saved_data_dir);
	return ok;
}

/*
 * ADR-0317 section 7 (#542): -1 when group is absent from the mapping,
 * otherwise whether it holds word.
 */
static int mapping_group_has(const struct json_value *groups, const char *group, const char *word)
{
	const struct json_value *arr = json_object_get(groups, group);
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY)
		return -1;
	for (i = 0; i < arr->u.array.count; i++) {
		if (str_eq(json_as_string(arr->u.array.items[i]), word))
			return 1;
	}
	return 0;
}

/* GETs the mapping's "groups" into *r; NULL on failure. */
static const struct json_value *get_mapping_groups(const struct cix_client *c,
                                                   struct cix_response *r)
{
	memset(r, 0, sizeof(*r));
	if (cix_client_request(c, "GET", "/v1/system/hostauth/permissions", NULL, r) != 0 ||
	    r->status != 200 || r->json == NULL)
		return NULL;
	return json_object_get(r->json, "groups");
}

/*
 * ADR-0317 section 7 (#542): the standard groups. A host with no record
 * of having provisioned them gets cix-admins (everything), cix-operators
 * (public, every read, every operate) and cix-readers (every read), in
 * the directory and in the mapping; once, so a deleted one stays
 * deleted; and a group that already existed under one of those names
 * keeps its own grants. Nobody is a member of any of them here, so
 * gating stays off and every request goes without credentials.
 */
static int test_standard_groups(void)
{
	char saved_data_dir[PATH_MAX], dir[PATH_MAX], path[PATH_MAX];
	struct cix_client c;
	struct cix_response r;
	const struct json_value *groups;
	pid_t pid = -1;
	int ok = 1;

	snprintf(saved_data_dir, sizeof(saved_data_dir), "%s", g_data_dir);
	cix_client_init(&c, "127.0.0.1", TEST_PORT);

	/* 1. A fresh host: test_data_dir_create() seeds the marker, so
	 * removing its file is what makes this one fresh. */
	if (test_data_dir_create(dir, sizeof(dir)) != 0)
		return 0;
	snprintf(path, sizeof(path), "%s/state/hostauth_config.json", dir);
	unlink(path);
	snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir);
	pid = start_daemon();
	if (pid < 0 || wait_for_daemon(&c, 50) != 0) {
		fprintf(stderr, "FAIL: #542 daemon never accepted connections\n");
		ok = 0;
		goto out;
	}
	groups = get_mapping_groups(&c, &r);
	if (groups == NULL) {
		fprintf(stderr, "FAIL: #542 GET permissions, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *vocab = json_object_get(r.json, "vocabulary");
		const struct json_value *admins = json_object_get(groups, "cix-admins");

		if (vocab == NULL || admins == NULL || admins->type != JSON_ARRAY ||
		    admins->u.array.count != vocab->u.array.count) {
			fprintf(stderr, "FAIL: #542 cix-admins must hold every permission: %.300s\n",
			        r.body != NULL ? r.body : "");
			ok = 0;
		}
		if (mapping_group_has(groups, "cix-operators", "public") != 1 ||
		    mapping_group_has(groups, "cix-operators", "containers:read") != 1 ||
		    mapping_group_has(groups, "cix-operators", "containers:operate") != 1 ||
		    mapping_group_has(groups, "cix-operators", "containers:write") != 0 ||
		    mapping_group_has(groups, "cix-operators", "containers:console") != 0 ||
		    mapping_group_has(groups, "cix-operators", "identity:write") != 0) {
			fprintf(stderr, "FAIL: #542 cix-operators must be public + every read + every "
			                "operate, nothing else: %.300s\n",
			        r.body != NULL ? r.body : "");
			ok = 0;
		}
		if (mapping_group_has(groups, "cix-readers", "containers:read") != 1 ||
		    mapping_group_has(groups, "cix-readers", "identity:read") != 1 ||
		    mapping_group_has(groups, "cix-readers", "containers:operate") != 0 ||
		    mapping_group_has(groups, "cix-readers", "public") != 0 ||
		    mapping_group_has(groups, "cix-readers", "images:write") != 0) {
			fprintf(stderr, "FAIL: #542 cix-readers must be every read, nothing else: %.300s\n",
			        r.body != NULL ? r.body : "");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* ... and they are real directory groups, so users can be put in them. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&c, "GET", "/v1/ldap/groups", NULL, &r) != 0 || r.status != 200 ||
	    !body_has(&r, "\"cix-admins\"") || !body_has(&r, "\"cix-operators\"") ||
	    !body_has(&r, "\"cix-readers\"")) {
		fprintf(stderr, "FAIL: #542 the standard groups are not in the directory: %.300s\n",
		        r.body != NULL ? r.body : "");
		ok = 0;
	}
	cix_response_free(&r);

	/* 2. Once per host: a deleted one stays deleted, and a changed one
	 * is not re-granted, across a restart. */
	ok &= expect_status(&c, NULL, "DELETE", "/v1/ldap/groups/cix-readers", NULL, 204,
	                    "delete cix-readers");
	ok &= expect_status(&c, NULL, "PUT", "/v1/system/hostauth/permissions/cix-operators",
	                    "{\"permissions\":[\"containers:read\"]}", 200, "narrow cix-operators");
	if (stop_daemon(pid) != 0)
		ok = 0;
	pid = start_daemon();
	if (pid < 0 || wait_for_daemon(&c, 50) != 0) {
		ok = 0;
		goto out;
	}
	groups = get_mapping_groups(&c, &r);
	if (groups == NULL || mapping_group_has(groups, "cix-readers", "containers:read") != -1 ||
	    mapping_group_has(groups, "cix-operators", "containers:operate") != 0) {
		fprintf(stderr, "FAIL: #542 a restart must not bring back a deleted standard group or "
		                "re-grant a changed one: %.300s\n",
		        r.body != NULL ? r.body : "");
		ok = 0;
	}
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&c, "GET", "/v1/ldap/groups", NULL, &r) != 0 ||
	    body_has(&r, "\"cix-readers\"")) {
		fprintf(stderr, "FAIL: #542 a deleted standard group came back in the directory\n");
		ok = 0;
	}
	cix_response_free(&r);
	if (stop_daemon(pid) != 0)
		ok = 0;
	pid = -1;
	test_data_dir_cleanup(dir);

	/* 3. A directory group already called cix-operators, made before
	 * the host was provisioned, keeps its own grants -- none. */
	if (test_data_dir_create(dir, sizeof(dir)) != 0) {
		ok = 0;
		goto restore;
	}
	snprintf(path, sizeof(path), "%s/state/hostauth_config.json", dir);
	snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir);
	pid = start_daemon();
	if (pid < 0 || wait_for_daemon(&c, 50) != 0) {
		ok = 0;
		goto out;
	}
	ok &= expect_status(&c, NULL, "POST", "/v1/ldap/groups",
	                    "{\"name\":\"cix-operators\",\"gidnumber\":7401}", 201,
	                    "create a pre-existing cix-operators");
	if (stop_daemon(pid) != 0)
		ok = 0;
	unlink(path); /* now the host has never been provisioned */
	pid = start_daemon();
	if (pid < 0 || wait_for_daemon(&c, 50) != 0) {
		ok = 0;
		goto out;
	}
	groups = get_mapping_groups(&c, &r);
	if (groups == NULL || mapping_group_has(groups, "cix-operators", "public") != -1 ||
	    mapping_group_has(groups, "cix-readers", "containers:read") != 1 ||
	    mapping_group_has(groups, "cix-admins", "identity:write") != 1) {
		fprintf(stderr, "FAIL: #542 a pre-existing cix-operators must not be granted anything, "
		                "and the other two must still be provisioned: %.300s\n",
		        r.body != NULL ? r.body : "");
		ok = 0;
	}
	cix_response_free(&r);

out:
	if (pid > 0 && stop_daemon(pid) != 0)
		ok = 0;
	test_data_dir_cleanup(dir);
restore:
	snprintf(g_data_dir, sizeof(g_data_dir), "%s", saved_data_dir);
	return ok;
}

/*
 * #547: failed authentication is throttled per source address, before
 * the password is checked. From 127.0.0.2 -- 127.0.0.1 is exempt, as it
 * is from the TLS throttle -- with the threshold at 3 and a 3-second
 * block. Gating is on (an admin exists), so a throttled app password
 * has something to be refused from.
 */
static int login_from(const char *src_ip, const char *user, const char *password)
{
	char req[256], body[1024];

	snprintf(req, sizeof(req), "{\"username\":\"%s\",\"password\":\"%s\"}", user, password);
	return raw_http(src_ip, "POST", "/v1/login", "", req, body, sizeof(body));
}

static int test_auth_throttle(void)
{
	char saved_data_dir[PATH_MAX], dir[PATH_MAX], tok[128] = "", body[4096];
	struct cix_client c;
	struct cix_response r;
	pid_t pid;
	int ok = 1, st, i;

	snprintf(saved_data_dir, sizeof(saved_data_dir), "%s", g_data_dir);
	if (test_data_dir_create(dir, sizeof(dir)) != 0)
		return 0;
	snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir);
	cix_client_init(&c, "127.0.0.1", TEST_PORT);
	pid = start_daemon();
	if (pid < 0 || wait_for_daemon(&c, 50) != 0) {
		fprintf(stderr, "FAIL: #547 daemon never accepted connections\n");
		ok = 0;
		goto out;
	}

	/* Gating on: an admin group with a member. */
	ok &= expect_status(&c, NULL, "PUT", "/v1/system/hostauth-config",
	                    "{\"admin_groups\":[\"thradmins\"],\"idle_timeout_seconds\":900}", 200,
	                    "configure admin_groups");
	ok &= expect_status(&c, NULL, "POST", "/v1/ldap/groups",
	                    "{\"name\":\"thradmins\",\"gidnumber\":7501}", 201, "create thradmins");
	ok &= expect_status(&c, NULL, "POST", "/v1/ldap/users",
	                    "{\"name\":\"thradmin\",\"uidnumber\":7510,\"primarygroup\":7501,"
	                    "\"password\":\"the right one\"}",
	                    201, "create thradmin");
	if (!login_as(&c, "thradmin", "the right one", tok, sizeof(tok))) {
		fprintf(stderr, "FAIL: #547 login from 127.0.0.1\n");
		ok = 0;
	}
	ok &= expect_status(&c, tok, "PUT", "/v1/system/tls-throttle",
	                    "{\"threshold\":3,\"window_seconds\":60,\"block_seconds\":3}", 200,
	                    "set the throttle to 3 failures, 3 s block");

	/* 1. Three wrong passwords from one address trip the block ... */
	for (i = 0; i < 3; i++) {
		st = login_from("127.0.0.2", "thradmin", "wrong");
		if (st != 401) {
			fprintf(stderr, "FAIL: #547 wrong login %d from 127.0.0.2 expected 401, got %d\n",
			        i + 1, st);
			ok = 0;
		}
	}
	/* 2. ... after which even the RIGHT password is 429 from there: the
	 * check comes before the password is looked at, which is the point
	 * (each look is a ~400 ms bcrypt), and the block is per address, so
	 * a real user behind it waits too. */
	st = login_from("127.0.0.2", "thradmin", "the right one");
	if (st != 429) {
		fprintf(stderr, "FAIL: #547 a blocked address must get 429 even with the right "
		                "password, got %d\n",
		        st);
		ok = 0;
	}
	/* 3. An app password from the blocked address: 429, not "log in". */
	st = basic_request_from("127.0.0.2", "GET", "/v1/ldap/groups", "thradmin",
	                        "0123456789abcdef0123456789abcdef", NULL, body, sizeof(body));
	if (st != 429) {
		fprintf(stderr, "FAIL: #547 HTTP Basic from a blocked address expected 429, got %d\n",
		        st);
		ok = 0;
	}
	/* 4. A request with no credential is not an authentication attempt:
	 * still the ordinary 401. */
	st = raw_http("127.0.0.2", "GET", "/v1/ldap/groups", "", NULL, body, sizeof(body));
	if (st != 401) {
		fprintf(stderr, "FAIL: #547 no credential from a blocked address must still be 401, "
		                "got %d\n",
		        st);
		ok = 0;
	}
	/* 5. Another address is unaffected (127.0.0.1 is exempt anyway). */
	if (!login_as(&c, "thradmin", "the right one", tok, sizeof(tok))) {
		fprintf(stderr, "FAIL: #547 a block on 127.0.0.2 must not touch 127.0.0.1\n");
		ok = 0;
	}
	/* 6. The block is audited once, naming the address. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&c, "GET", "/v1/system/logs?source=audit&tail=60", tok, NULL, &r) != 0 ||
	    r.body == NULL || strstr(r.body, "127.0.0.2 authentication blocked") == NULL) {
		fprintf(stderr, "FAIL: #547 the block is not audited with its address\n");
		ok = 0;
	}
	cix_response_free(&r);

	/* 7. It ends: past block_seconds the right password works again. */
	sleep(4);
	st = login_from("127.0.0.2", "thradmin", "the right one");
	if (st != 200) {
		fprintf(stderr, "FAIL: #547 after the block the right password expected 200, got %d\n",
		        st);
		ok = 0;
	}
	/* 8. A success clears the count: 2 wrong, 1 right, 2 wrong is never
	 * 3 in a row, so the next right password still works. */
	login_from("127.0.0.2", "thradmin", "wrong");
	login_from("127.0.0.2", "thradmin", "wrong");
	login_from("127.0.0.2", "thradmin", "the right one");
	login_from("127.0.0.2", "thradmin", "wrong");
	login_from("127.0.0.2", "thradmin", "wrong");
	st = login_from("127.0.0.2", "thradmin", "the right one");
	if (st != 200) {
		fprintf(stderr, "FAIL: #547 a successful login must clear the failure count, got %d\n",
		        st);
		ok = 0;
	}

out:
	if (pid > 0 && stop_daemon(pid) != 0)
		ok = 0;
	test_data_dir_cleanup(dir);
	snprintf(g_data_dir, sizeof(g_data_dir), "%s", saved_data_dir);
	return ok;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	int ok = 1;
	struct cix_response r;
	char token[128] = "";

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

	/* 1. Bootstrap safety: no admin-group user exists yet -- an
	 * ordinary write (creating an LDAP group) succeeds with zero
	 * credentials, same as this whole project's behavior before
	 * ADR-0144 existed. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/ldap/groups", "{\"name\":\"engineers\",\"gidnumber\":7001}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: pre-bootstrap group create (should be open), status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 2. Configure the admin group -- also an open write, since gating
	 * still isn't active (no user is a member of "admins" yet). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT", "/v1/system/hostauth-config",
	                       "{\"admin_groups\":[\"admins\"],\"idle_timeout_seconds\":900}", &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: PUT hostauth-config, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/ldap/groups", "{\"name\":\"admins\",\"gidnumber\":7002}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: create group admins, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 3. Login with a user that doesn't exist yet -> 401. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/login",
	                       "{\"username\":\"nosuchuser\",\"password\":\"whatever\"}", &r) != 0 ||
	    r.status != 401) {
		fprintf(stderr, "FAIL: login as nonexistent user expected 401, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 4. Create a real user in the admin group, with a real password --
	 * still an open write, gating still isn't active (this create IS
	 * what activates it, but only from the moment it lands). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/ldap/users",
	                       "{\"name\":\"root_admin\",\"uidnumber\":7100,\"primarygroup\":7002,"
	                       "\"password\":\"correct horse battery staple\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: create admin user, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 5. Gating is NOW active -- an unauthenticated write is refused. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/ldap/groups", "{\"name\":\"unrelated\",\"gidnumber\":7003}",
	                       &r) != 0 ||
	    r.status != 401) {
		fprintf(stderr, "FAIL: unauthenticated write after gating activated expected 401, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * #541 (ADR-0317 section 3): reads need a session too, once gating
	 * is active. This used to assert the opposite -- "reads still work
	 * with zero credentials, always" -- which was the rule until the
	 * route table carried a permission for every operation. The pair
	 * that makes it mean something, a 200 for the same read WITH a
	 * session, is asserted at step 8 once root_admin has logged in.
	 * GET /v1/health is public and stays open (step 9a).
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/ldap/groups", NULL, &r) != 0 || r.status != 401) {
		fprintf(stderr, "FAIL: unauthenticated GET after gating activated expected 401, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * And a HEAD is a read (#498), which it was not until this gate
	 * stopped asking "is this a GET". `curl -sI` on a static asset
	 * answered 401 for as long as the gate existed, and every one of
	 * those was additionally audited as a refused write.
	 *
	 * Asserted here and not in test_web because it can only be
	 * asserted here: hostauth_authorize() answers OK
	 * unconditionally while gating is inactive, so on a daemon with
	 * no admin account a HEAD would sail through the gate and prove
	 * nothing about it. This is the one test that has gating
	 * genuinely on. What comes back is the dashboard's index -- a 200
	 * or a 404 depending on whether the asset is staged where this
	 * daemon is looking, and either one is the gate having let it
	 * past. A 401 is the failure.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "HEAD", "/", NULL, &r) != 0 || r.status == 401) {
		fprintf(stderr,
		        "FAIL: unauthenticated HEAD on a static asset after gating activated must not "
		        "be refused as a write, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * The other half, and the reason HEAD was folded into the read
	 * test rather than given its own exemption: the gated reads stay
	 * gated for HEAD. A gate a change of verb can step around is not
	 * a gate.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "HEAD", "/v1/ldap/users", NULL, &r) != 0 || r.status != 401) {
		fprintf(stderr,
		        "FAIL: HEAD on a gated identity read must still be 401 (#490 must survive "
		        "#498), got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 6. Wrong password -> 401, no token issued. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/login",
	                       "{\"username\":\"root_admin\",\"password\":\"wrong\"}", &r) != 0 ||
	    r.status != 401) {
		fprintf(stderr, "FAIL: login with wrong password expected 401, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 7. Real login -> a real token. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/login",
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
	cix_response_free(&r);

	/* 8. The same write, now with the token, succeeds. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "POST", "/v1/ldap/groups", token,
	                        "{\"name\":\"unrelated\",\"gidnumber\":7003}", &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: authenticated write expected 201, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* ... and the read step 5 was refused, now with the token. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "GET", "/v1/ldap/groups", token, NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: authenticated GET ldap groups expected 200, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * 8b. #370: the five ordinary operations that used to turn write
	 * authentication OFF for the whole API, silently, while leaving
	 * hostauth-config still naming an admin group. Each is refused
	 * with 409, and gating is confirmed still active afterwards --
	 * because the bug was never a wrong status code, it was an open
	 * control plane that looked exactly like a closed one.
	 *
	 * Group "admins" (gid 7002) is the configured admin group and
	 * "root_admin" is its only member at this point.
	 *
	 * RENAMING an admin group is deliberately NOT among them: ADR-0147
	 * already rewrites admin_groups in place when one is renamed, and
	 * a guard here refused that working feature -- the existing
	 * "admin_groups did not follow the rename" case below caught it.
	 */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "DELETE", "/v1/ldap/groups/admins", token, NULL, &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: deleting the configured admin group expected 409, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "PUT", "/v1/ldap/groups/admins", token,
	                        "{\"name\":\"admins\",\"gidnumber\":7099}", &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: renumbering the configured admin group expected 409, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "DELETE", "/v1/ldap/users/root_admin", token, NULL, &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: deleting the only admin user expected 409, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* A PUT that drops the admin gid: the proposed record is judged,
	 * not the stored one, since the stored one is still an admin. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "PUT", "/v1/ldap/users/root_admin", token,
	                        "{\"name\":\"root_admin\",\"uidnumber\":7100,\"primarygroup\":7001}",
	                        &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: de-admining the only admin expected 409, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* Disabling the only admin is the same hole by another route. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "PUT", "/v1/ldap/users/root_admin", token,
	                        "{\"name\":\"root_admin\",\"uidnumber\":7100,\"primarygroup\":7002,"
	                        "\"disabled\":true}",
	                        &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: disabling the only admin expected 409, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* Pointing admin_groups at a group nobody is in. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "PUT", "/v1/system/hostauth-config", token,
	                        "{\"admin_groups\":[\"unrelated\"],\"idle_timeout_seconds\":900}", &r) !=
	        0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: repointing admin_groups at an empty group expected 409, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * The point of all seven: gating is still on. A 409 that left the
	 * API open would be a worse bug than the one being fixed.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/ldap/groups", "{\"name\":\"sneak\",\"gidnumber\":7005}",
	                       &r) != 0 ||
	    r.status != 401) {
		fprintf(stderr, "FAIL: gating must still be active after the refusals, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 8c. #370 visibility: health and hostauth-config both report that
	 * gating is in force. An open control plane used to be reported
	 * nowhere at all, which is why it went unnoticed for four days. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/health", NULL, &r) != 0 || r.status != 200 ||
	    r.json == NULL || json_bool_field(r.json, "auth_gating_active") != 1) {
		fprintf(stderr, "FAIL: GET /v1/health must report auth_gating_active true, status=%d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (admin_get(&client, "/v1/system/hostauth-config", &r) != 0 ||
	    r.status != 200 || r.json == NULL || json_bool_field(r.json, "gating_active") != 1) {
		fprintf(stderr, "FAIL: hostauth-config must report gating_active true, status=%d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 9. A garbage token is rejected the same way as no token at all. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "POST", "/v1/ldap/groups", "not-a-real-token",
	                        "{\"name\":\"nope\",\"gidnumber\":7004}", &r) != 0 || r.status != 401) {
		fprintf(stderr, "FAIL: garbage token expected 401, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 9a. #490: the identity reads need a credential once gating is
	 * active. They were the first reads to (#490); since #541 every read
	 * does, and these two stay here as the ones that did first.
	 *
	 * Asserted before 9b below reads the same endpoint WITH a token,
	 * because the pair is what means something: either alone is
	 * consistent with a gate that is broken in one direction. The
	 * roster is checked too -- it is the other half of the same
	 * decision and it gates for a different reason (a per-account
	 * profile, not a live activity window). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/hostauth/sessions", NULL, &r) != 0 ||
	    r.status != 401) {
		fprintf(stderr, "FAIL: unauthenticated GET hostauth sessions expected 401, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/ldap/users", NULL, &r) != 0 || r.status != 401) {
		fprintf(stderr, "FAIL: unauthenticated GET ldap users expected 401, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	/* The control that keeps this from passing on a daemon that
	 * simply refuses everything: GET /v1/health is public (ADR-0317
	 * section 3) and answers without a session. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/health", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: unauthenticated GET health expected 200, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 9b. ADR-0152: session listing shows the real active session, a
	 * real expires_in_seconds (idle_timeout_seconds=900 here, so never
	 * null), and never a raw token anywhere in the response. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "GET", "/v1/system/hostauth/sessions", token, NULL, &r) != 0 ||
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
					/* When and where it logged in: this test's own
					 * logins, so within the last hour and from loopback
					 * (some steps bind another 127.x source). */
					{
						const struct json_value *jat = json_object_get(s, "logged_in_at");
						const char *src = json_str_field(s, "source_ip");
						double now = (double)time(NULL);

						if (jat == NULL || jat->type != JSON_NUMBER ||
						    json_as_number(jat) > now || json_as_number(jat) < now - 3600) {
							fprintf(stderr, "FAIL: root_admin session has no plausible "
							                "logged_in_at\n");
							ok = 0;
						}
						if (src == NULL || strncmp(src, "127.", 4) != 0) {
							fprintf(stderr, "FAIL: root_admin session source_ip is %s, "
							                "expected a loopback address\n",
							        src != NULL ? src : "(null)");
							ok = 0;
						}
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
	cix_response_free(&r);

	/*
	 * 9d. #541: the WebSocket upgrades are gated too. Measured on
	 * 192.168.15.95 at 0.2.57-391, before this: an upgrade of a
	 * container console with no credential at all reached the console
	 * handler, because upgrades are answered before the dispatcher the
	 * old gate lived in. A 404 below ("no such running container") is
	 * the handler having been reached, i.e. the session accepted; the
	 * browser's subprotocol form must be accepted exactly as the
	 * header is.
	 */
	{
		char hdr[256];
		int st;

		st = ws_upgrade_status("/v1/containers/nosuch/console", "");
		if (st != 401) {
			fprintf(stderr, "FAIL: a console upgrade with no credential expected 401, got %d\n",
			        st);
			ok = 0;
		}
		snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s\r\n", token);
		st = ws_upgrade_status("/v1/containers/nosuch/console", hdr);
		if (st != 404) {
			fprintf(stderr, "FAIL: a console upgrade with a session header expected to reach "
			                "the handler (404), got %d\n",
			        st);
			ok = 0;
		}
		snprintf(hdr, sizeof(hdr), "Sec-WebSocket-Protocol: cix, cix.bearer.%s\r\n", token);
		st = ws_upgrade_status("/v1/containers/nosuch/console", hdr);
		if (st != 404) {
			fprintf(stderr, "FAIL: a console upgrade with the session as a subprotocol expected "
			                "to reach the handler (404), got %d\n",
			        st);
			ok = 0;
		}
		st = ws_upgrade_status("/v1/pkg/build/log?name=nosuch", "");
		if (st != 401) {
			fprintf(stderr, "FAIL: a build-log upgrade with no credential expected 401, got %d\n",
			        st);
			ok = 0;
		}
	}

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
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "POST", "/v1/ldap/groups", token,
	                        "{\"name\":\"post-revoke\",\"gidnumber\":7005}", &r) != 0 ||
	    r.status != 401) {
		fprintf(stderr, "FAIL: revoked token should be rejected like any other, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* Re-authenticate -- the revoke above killed the only token this
	 * test had, and the no-op check right below is itself a write,
	 * needing a real one attached. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/login",
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
	cix_response_free(&r);

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
	cix_response_free(&r);

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
	cix_response_free(&r);

	{
		char plain_token[128] = "";

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/login",
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
		cix_response_free(&r);

		/*
		 * #541: a valid session without the permission is 403, not
		 * 401, and the body names what was missing: a 401 here
		 * would send the person to log in again, which cannot help.
		 * plain_user's only group (engineers, 7001) grants nothing yet.
		 */
		if (plain_token[0] != '\0') {
			memset(&r, 0, sizeof(r));
			if (request_with_token(&client, "POST", "/v1/ldap/groups", plain_token,
			                        "{\"name\":\"shouldfail\",\"gidnumber\":7005}", &r) != 0 ||
			    r.status != 403 || r.json == NULL ||
			    !str_eq(json_str_field(r.json, "permission"), "identity:write")) {
				fprintf(stderr,
				        "FAIL: a write without the permission expected 403 naming "
				        "identity:write, got %d (%.200s)\n",
				        r.status, r.body != NULL ? r.body : "");
				ok = 0;
			}
			cix_response_free(&r);

			/* Reads too: no grant, no read. */
			memset(&r, 0, sizeof(r));
			if (request_with_token(&client, "GET", "/v1/containers", plain_token, NULL, &r) !=
			        0 ||
			    r.status != 403 || r.json == NULL ||
			    !str_eq(json_str_field(r.json, "permission"), "containers:read")) {
				fprintf(stderr, "FAIL: a read without the permission expected 403 naming "
				                "containers:read, got %d\n",
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);

			/* Granted to engineers, it takes effect on the next request
			 * of a session opened before the grant: membership and the
			 * mapping are read per request, never cached in a session. */
			memset(&r, 0, sizeof(r));
			if (request_with_token(&client, "PUT", "/v1/system/hostauth/permissions/engineers",
			                        token, "{\"permissions\":[\"containers:read\"]}", &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: grant engineers containers:read, status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			if (request_with_token(&client, "GET", "/v1/containers", plain_token, NULL, &r) !=
			        0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: a granted read expected 200, got %d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);

			/* ... and read is not write. */
			memset(&r, 0, sizeof(r));
			if (request_with_token(&client, "POST", "/v1/containers", plain_token, "{}", &r) !=
			        0 ||
			    r.status != 403 || r.json == NULL ||
			    !str_eq(json_str_field(r.json, "permission"), "containers:write")) {
				fprintf(stderr, "FAIL: containers:read must not allow a create, expected 403 "
				                "naming containers:write, got %d\n",
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);

			/* HEAD answers as its GET does (#498): 403 for a read this
			 * session lacks, never a 404 that would hide the gate. */
			memset(&r, 0, sizeof(r));
			if (request_with_token(&client, "HEAD", "/v1/ldap/users", plain_token, NULL, &r) !=
			        0 ||
			    r.status != 403) {
				fprintf(stderr, "FAIL: HEAD of an ungranted read expected 403, got %d\n",
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);

			/* A shell is not a read: containers:read does not open a
			 * console, through the upgrade path either. */
			{
				char hdr[256];
				int st;

				snprintf(hdr, sizeof(hdr), "Sec-WebSocket-Protocol: cix, cix.bearer.%s\r\n",
				         plain_token);
				st = ws_upgrade_status("/v1/containers/nosuch/console", hdr);
				if (st != 403) {
					fprintf(stderr, "FAIL: a console upgrade without containers:console "
					                "expected 403, got %d\n",
					        st);
					ok = 0;
				}
			}

			/*
			 * #543 (ADR-0317 section 8): app passwords. An admin makes
			 * one for plain_user; it is accepted per request as HTTP
			 * Basic with exactly plain_user's permissions; the main
			 * password is not; revocation is immediate; and the audit
			 * line names it.
			 */
			{
				char secret[128] = "", own_secret[128] = "", body[4096];
				int st;

				memset(&r, 0, sizeof(r));
				if (request_with_token(&client, "POST", "/v1/ldap/users/plain_user/app-passwords",
				                        token, "{\"name\":\"ci\"}", &r) != 0 ||
				    r.status != 201 || r.json == NULL ||
				    json_str_field(r.json, "password") == NULL) {
					fprintf(stderr, "FAIL: #543 create an app password, status=%d (%.200s)\n",
					        r.status, r.body != NULL ? r.body : "");
					ok = 0;
				} else {
					snprintf(secret, sizeof(secret), "%s", json_str_field(r.json, "password"));
				}
				cix_response_free(&r);

				/* Its user's read, twice: the second is the memoized path. */
				st = basic_request("GET", "/v1/containers", "plain_user", secret, NULL, body,
				                   sizeof(body));
				if (st != 200) {
					fprintf(stderr, "FAIL: #543 a granted read by app password expected 200, "
					                "got %d\n",
					        st);
					ok = 0;
				}
				st = basic_request("GET", "/v1/containers", "plain_user", secret, NULL, body,
				                   sizeof(body));
				if (st != 200) {
					fprintf(stderr, "FAIL: #543 the same read a second time expected 200, got %d\n",
					        st);
					ok = 0;
				}
				/* ... and nothing more than its user holds. */
				st = basic_request("POST", "/v1/containers", "plain_user", secret, "{}", body,
				                   sizeof(body));
				if (st != 403 || strstr(body, "containers:write") == NULL) {
					fprintf(stderr, "FAIL: #543 an app password must carry exactly its user's "
					                "permissions, expected 403 naming containers:write, got %d "
					                "(%.200s)\n",
					        st, body);
					ok = 0;
				}
				/* The main password is not accepted per request. */
				st = basic_request("GET", "/v1/containers", "plain_user", "plainpassword123", NULL,
				                   body, sizeof(body));
				if (st != 401) {
					fprintf(stderr, "FAIL: #543 the main password as HTTP Basic expected 401, "
					                "got %d\n",
					        st);
					ok = 0;
				}
				st = basic_request("GET", "/v1/containers", "plain_user", "0123456789abcdef", NULL,
				                   body, sizeof(body));
				if (st != 401) {
					fprintf(stderr, "FAIL: #543 a wrong app password expected 401, got %d\n", st);
					ok = 0;
				}

				/* Own app passwords need a session, not an app password. */
				memset(&r, 0, sizeof(r));
				if (request_with_token(&client, "POST", "/v1/whoami/app-passwords", plain_token,
				                        "{\"name\":\"own\"}", &r) != 0 ||
				    r.status != 201 || r.json == NULL ||
				    json_str_field(r.json, "password") == NULL) {
					fprintf(stderr, "FAIL: #543 a user creating their own app password with a "
					                "session expected 201, got %d\n",
					        r.status);
					ok = 0;
				} else {
					snprintf(own_secret, sizeof(own_secret), "%s",
					         json_str_field(r.json, "password"));
				}
				cix_response_free(&r);
				st = basic_request("POST", "/v1/whoami/app-passwords", "plain_user", secret,
				                   "{\"name\":\"minted\"}", body, sizeof(body));
				if (st != 403) {
					fprintf(stderr, "FAIL: #543 an app password must not mint app passwords, "
					                "expected 403, got %d\n",
					        st);
					ok = 0;
				}

				/* A listing carries names, never a hash or a secret. */
				memset(&r, 0, sizeof(r));
				if (request_with_token(&client, "GET", "/v1/ldap/users/plain_user/app-passwords",
				                        token, NULL, &r) != 0 ||
				    r.status != 200 || r.body == NULL || strstr(r.body, "\"ci\"") == NULL ||
				    strstr(r.body, "\"own\"") == NULL || strstr(r.body, "passbcrypt") != NULL ||
				    strstr(r.body, "$2") != NULL ||
				    (secret[0] != '\0' && strstr(r.body, secret) != NULL)) {
					fprintf(stderr, "FAIL: #543 the listing must name both and reveal neither: "
					                "%.300s\n",
					        r.body != NULL ? r.body : "");
					ok = 0;
				}
				cix_response_free(&r);

				/* The audit trail names the app password. The refused
				 * create above is a write, so it was audited. */
				memset(&r, 0, sizeof(r));
				if (request_with_token(&client, "GET", "/v1/system/logs?source=audit&tail=50",
				                        token, NULL, &r) != 0 ||
				    r.body == NULL || strstr(r.body, "plain_user (app-password ci)") == NULL) {
					fprintf(stderr, "FAIL: #543 the audit log does not name the app password\n");
					ok = 0;
				}
				cix_response_free(&r);

				/* Revoked: the very next request is refused, and the
				 * user's other app password still works. */
				memset(&r, 0, sizeof(r));
				if (request_with_token(&client, "DELETE",
				                        "/v1/ldap/users/plain_user/app-passwords/ci", token, NULL,
				                        &r) != 0 ||
				    r.status != 204) {
					fprintf(stderr, "FAIL: #543 revoke an app password, status=%d\n", r.status);
					ok = 0;
				}
				cix_response_free(&r);
				st = basic_request("GET", "/v1/containers", "plain_user", secret, NULL, body,
				                   sizeof(body));
				if (st != 401) {
					fprintf(stderr, "FAIL: #543 a revoked app password expected 401 at once, "
					                "got %d\n",
					        st);
					ok = 0;
				}
				st = basic_request("GET", "/v1/containers", "plain_user", own_secret, NULL, body,
				                   sizeof(body));
				if (st != 200) {
					fprintf(stderr, "FAIL: #543 revoking one app password must leave the other, "
					                "expected 200, got %d\n",
					        st);
					ok = 0;
				}
				explicit_bzero(secret, sizeof(secret));
				explicit_bzero(own_secret, sizeof(own_secret));
			}
			/* whoami reports exactly what was granted. */
			memset(&r, 0, sizeof(r));
			if (request_with_token(&client, "GET", "/v1/whoami", plain_token, NULL, &r) != 0 ||
			    r.status != 200 || r.body == NULL ||
			    strstr(r.body, "\"permissions\":[\"containers:read\"]") == NULL) {
				fprintf(stderr, "FAIL: whoami must report plain_user's one permission: %.200s\n",
				        r.body != NULL ? r.body : "");
				ok = 0;
			}
			cix_response_free(&r);
		}
	}

	/* 11. Logout invalidates the token; the same write now fails. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "POST", "/v1/logout", token, NULL, &r) != 0 || r.status != 204) {
		fprintf(stderr, "FAIL: logout expected 204, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "POST", "/v1/ldap/groups", token,
	                        "{\"name\":\"afterlogout\",\"gidnumber\":7006}", &r) != 0 ||
	    r.status != 401) {
		fprintf(stderr, "FAIL: write after logout expected 401, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* Logout is idempotent -- a second call on an already-gone token
	 * is still 204, never an error. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "POST", "/v1/logout", token, NULL, &r) != 0 || r.status != 204) {
		fprintf(stderr, "FAIL: repeat logout expected 204, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 12. idle_timeout_seconds=0 -- a fresh login's token is single-use. */
	memset(&r, 0, sizeof(r));
	if (request_with_token(&client, "PUT", "/v1/system/hostauth-config", token, NULL, &r) == 0) {
		/* no-op: token already invalid, just draining any stray response */
	}
	cix_response_free(&r);

	{
		/* Need a fresh admin session to change hostauth-config itself
		 * (also a gated write) -- log back in first. */
		char admin_token[128] = "";

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/login",
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
		cix_response_free(&r);

		if (admin_token[0] != '\0') {
			memset(&r, 0, sizeof(r));
			if (request_with_token(&client, "PUT", "/v1/system/hostauth-config", admin_token,
			                        "{\"admin_groups\":[\"admins\"],\"idle_timeout_seconds\":0}",
			                        &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: set idle_timeout_seconds=0, status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);
		}
	}

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/login",
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
		cix_response_free(&r);

		if (once_token[0] != '\0') {
			memset(&r, 0, sizeof(r));
			if (request_with_token(&client, "POST", "/v1/ldap/groups", once_token,
			                        "{\"name\":\"onceonly\",\"gidnumber\":7007}", &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr, "FAIL: first use of single-use token expected 201, got %d\n",
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (request_with_token(&client, "POST", "/v1/ldap/groups", once_token,
			                        "{\"name\":\"onceonlyagain\",\"gidnumber\":7008}", &r) != 0 ||
			    r.status != 401) {
				fprintf(stderr,
				        "FAIL: second use of single-use token expected 401, got %d\n",
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);
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
		if (cix_client_request(&client, "POST", "/v1/login",                                \
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
		cix_response_free(&r);                                                               \
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
		cix_response_free(&r);

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
		cix_response_free(&r);

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
		cix_response_free(&r);

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
		cix_response_free(&r);
#undef RELOGIN_ROOT_ADMIN

		memset(&r, 0, sizeof(r));
		if (admin_get(&client, "/v1/system/hostauth-config", &r) != 0 ||
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
		cix_response_free(&r);

		/*
		 * 13e. Login as root_admin with the CORRECT local password.
		 * ldap_enabled is true but the one configured server is
		 * unreachable (nothing listens on 18189) -- hostauth_login()
		 * must fall back to the local backend rather than failing the
		 * login outright. A real token comes back, usable for a write.
		 */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/login",
		                       "{\"username\":\"root_admin\",\"password\":\"correct horse battery "
		                       "staple\"}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr,
			        "FAIL: login with unreachable LDAP server expected local-backend fallback "
			        "(200), got %d\n",
			        r.status);
			ok = 0;
			cix_response_free(&r);
		} else {
			const char *t = json_str_field(r.json, "token");
			char fallback_token[128] = "";

			if (t != NULL)
				snprintf(fallback_token, sizeof(fallback_token), "%s", t);
			cix_response_free(&r);

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
				cix_response_free(&r);
			}
		}

		/*
		 * 13f. A WRONG password still correctly fails (401) even with
		 * ldap_enabled and an unreachable server -- the fallback must
		 * never bypass real credential verification.
		 */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/login",
		                       "{\"username\":\"root_admin\",\"password\":\"wrong\"}", &r) != 0 ||
		    r.status != 401) {
			fprintf(stderr,
			        "FAIL: wrong password under LDAP-enabled+unreachable expected 401, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/*
		 * 13g (#416): ldap_tls for the DAEMON'S OWN bind. This daemon
		 * has no CA -- every daemon-linked test starts against a fresh
		 * mkdtemp data directory, so nothing has bootstrapped one --
		 * which is exactly the state the guard exists for.
		 *
		 * Asking for TLS together with ldap_enabled is refused 409,
		 * not 400: no single field is wrong, the combination is, and
		 * the operator's fix (bootstrap the CA) is not something a
		 * generic invalid-field message would name. Accepting it would
		 * save a config where every login silently failed its
		 * handshake and fell back to local records -- a state that
		 * reads as correct and does nothing it says.
		 */
		{
			char tls_token[128] = "";

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/login",
			                       "{\"username\":\"root_admin\",\"password\":\"correct "
			                       "horse battery staple\"}",
			                       &r) == 0 &&
			    r.status == 200) {
				const char *t = json_str_field(r.json, "token");

				snprintf(tls_token, sizeof(tls_token), "%s", t != NULL ? t : "");
			} else {
				fprintf(stderr, "FAIL: re-login for ldap_tls scenario, status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);

			if (tls_token[0] != '\0') {
				memset(&r, 0, sizeof(r));
				if (request_with_token(
				        &client, "PUT", "/v1/system/hostauth-config", tls_token,
				        "{\"admin_groups\":[\"admins\"],\"idle_timeout_seconds\":0,"
				        "\"ldap_enabled\":true,\"ldap_servers\":[\"127.0.0.1\"],"
				        "\"ldap_port\":18189,\"ldap_tls\":true,"
				        "\"ldap_base_dn\":\"dc=glauth,dc=com\"}",
				        &r) != 0 ||
				    r.status != 409) {
					fprintf(stderr,
					        "FAIL: ldap_tls with ldap_enabled and no CA expected 409, "
					        "got %d\n",
					        r.status);
					ok = 0;
				} else {
					/* This endpoint has a SECOND 409 (the #370
					 * gating guard, checked before the setter runs),
					 * so a bare status check could pass for entirely
					 * the wrong reason. The message has to name the
					 * cause. */
					const char *e = json_str_field(r.json, "error");

					if (e == NULL || strstr(e, "ldap_tls") == NULL) {
						fprintf(stderr,
						        "FAIL: ldap_tls 409 did not name ldap_tls as the cause: "
						        "%s\n",
						        e != NULL ? e : "(no error field)");
						ok = 0;
					}
				}
				cix_response_free(&r);
			}

			/*
			 * The same flag WITHOUT ldap_enabled is accepted, so the
			 * ordering is the operator's to choose: set the flag
			 * first, bootstrap the CA, then enable the backend. GET
			 * echoes it, which also proves the field survives the
			 * save/load round trip rather than only the response.
			 */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/login",
			                       "{\"username\":\"root_admin\",\"password\":\"correct "
			                       "horse battery staple\"}",
			                       &r) == 0 &&
			    r.status == 200) {
				const char *t = json_str_field(r.json, "token");

				snprintf(tls_token, sizeof(tls_token), "%s", t != NULL ? t : "");
			} else {
				tls_token[0] = '\0';
			}
			cix_response_free(&r);

			if (tls_token[0] != '\0') {
				memset(&r, 0, sizeof(r));
				if (request_with_token(
				        &client, "PUT", "/v1/system/hostauth-config", tls_token,
				        "{\"admin_groups\":[\"admins\"],\"idle_timeout_seconds\":0,"
				        "\"ldap_enabled\":false,\"ldap_servers\":[],"
				        "\"ldap_port\":18189,\"ldap_tls\":true,"
				        "\"ldap_base_dn\":\"dc=glauth,dc=com\"}",
				        &r) != 0 ||
				    r.status != 200) {
					fprintf(stderr,
					        "FAIL: ldap_tls without ldap_enabled expected 200, got %d\n",
					        r.status);
					ok = 0;
				}
				cix_response_free(&r);

				memset(&r, 0, sizeof(r));
				if (admin_get(&client, "/v1/system/hostauth-config", &r) != 0 ||
				    r.status != 200) {
					fprintf(stderr, "FAIL: GET hostauth-config after ldap_tls, status=%d\n",
					        r.status);
					ok = 0;
				} else {
					const struct json_value *jtls = json_object_get(r.json, "ldap_tls");

					if (jtls == NULL || jtls->type != JSON_BOOL || !jtls->u.boolean) {
						fprintf(stderr,
						        "FAIL: GET hostauth-config did not echo ldap_tls true\n");
						ok = 0;
					}
				}
				cix_response_free(&r);
			}
		}
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
		if (cix_client_request(&client, "POST", "/v1/login",
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
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (request_with_token(&client, "PUT", "/v1/ldap/groups/admins", rename_token,
		                        "{\"name\":\"root-admins\",\"gidnumber\":7002}", &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: rename active admin group, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* hostauth-config's own admin_groups must now read
		 * "root-admins", not "admins" -- the daemon-side propagation,
		 * not anything this test itself did. */
		memset(&r, 0, sizeof(r));
		if (admin_get(&client, "/v1/system/hostauth-config", &r) != 0 ||
		    r.status != 200 ||
		    memmem(r.body, r.body_len, "\"root-admins\"", strlen("\"root-admins\"")) == NULL ||
		    memmem(r.body, r.body_len, "\"admins\"", strlen("\"admins\"")) != NULL) {
			fprintf(stderr,
			        "FAIL: admin_groups did not follow the rename (expected root-admins only, "
			        "not admins), status=%d, body=%.*s\n",
			        r.status, (int)r.body_len, r.body);
			ok = 0;
		}
		cix_response_free(&r);

		/* root_admin's own primarygroup is a gidnumber (7002), unaffected
		 * by the rename -- a fresh login must still succeed, proving
		 * gating genuinely still recognizes this user as an admin under
		 * the group's new name, not just that the config string changed. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/login",
		                       "{\"username\":\"root_admin\",\"password\":\"correct horse battery "
		                       "staple\"}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: login after admin-group rename expected 200, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* Another fresh token (idle_timeout_seconds=0 by this point in
		 * the file means single-use) -- login as root_admin still
		 * works under the group's NEW name, itself further proof the
		 * rename didn't break gating. */
		rename_token[0] = '\0';
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/login",
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
		cix_response_free(&r);

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
		cix_response_free(&r);
	}

	/*
	 * The audit trail names WHO, not just what.
	 *
	 * Until this it recorded the method and the path and nothing else,
	 * which answers "what happened" and never "who did it" -- tolerable
	 * while the dashboard could only look, and not once it can change
	 * the box. Asserted end to end against the real log store rather
	 * than by reading the format string, because the value only exists
	 * if it survives dispatch, the write gate and the store.
	 */
	{
		int saw_actor = 0;
		int saw_login = 0;
		int saw_refusal = 0;

		memset(&r, 0, sizeof(r));
		if (admin_get(&client, "/v1/system/logs?source=audit&tail=200", &r) !=
		        0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET the audit log, status=%d\n", r.status);
			ok = 0;
		} else if (r.json != NULL && r.json->type == JSON_ARRAY) {
			size_t k;

			for (k = 0; k < r.json->u.array.count; k++) {
				const char *m = json_str_field(r.json->u.array.items[k], "msg");

				if (m == NULL)
					continue;
				/* A write this test really made, attributed. */
				if (strstr(m, "root_admin POST /v1/ldap/groups") != NULL)
					saw_actor = 1;
				if (strstr(m, "root_admin logged in") != NULL)
					saw_login = 1;
				/* A write it made WITHOUT a token, which must be
				 * recorded too -- auditing only what got past
				 * authorization would lose exactly the attempts an
				 * operator wants to see. */
				if (strstr(m, "REFUSED") != NULL)
					saw_refusal = 1;
			}
		}
		if (!saw_actor) {
			fprintf(stderr, "FAIL: the audit trail does not name who made a write\n");
			ok = 0;
		}
		if (!saw_login) {
			fprintf(stderr, "FAIL: a successful login is not audited by name\n");
			ok = 0;
		}
		if (!saw_refusal) {
			fprintf(stderr, "FAIL: a refused write leaves no audit trace\n");
			ok = 0;
		}
		cix_response_free(&r);
	}

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	if (!test_permission_mapping())
		ok = 0;
	if (!test_standard_groups())
		ok = 0;
	if (!test_auth_throttle())
		ok = 0;
	printf(ok ? "HOSTAUTH RESULT: PASS\n" : "HOSTAUTH RESULT: FAIL\n");
	return ok ? 0 : 1;
}
