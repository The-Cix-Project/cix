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
#include "hostauth.h" /* HOSTAUTH_LDAP_DEFAULT_PORT -- issue #84 filtering test */
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

/* Decodes a hex string into out -- passbcrypt's own rendered TOML form
 * is glauth's real expected hex encoding of the bcrypt hash's ASCII
 * bytes (see ldap.c's render_users_groups_toml() comment on why: glauth
 * calls hex.DecodeString() on this field before ever touching bcrypt).
 * Returns 0 on success, -1 on odd length, a non-hex digit, or
 * insufficient out_size. */
static int hex_decode(const char *hex, char *out, size_t out_size)
{
	size_t hexlen = strlen(hex);
	size_t i;

	if (hexlen % 2 != 0 || hexlen / 2 >= out_size)
		return -1;
	for (i = 0; i < hexlen; i += 2) {
		unsigned int byte;

		if (sscanf(hex + i, "%2x", &byte) != 1)
			return -1;
		out[i / 2] = (char)byte;
	}
	out[hexlen / 2] = '\0';
	return 0;
}

/* Extracts the quoted value following `key = "` in body (a real TOML
 * rendered field, e.g. `passbcrypt = "<hex-encoded bcrypt hash>"`) into
 * out. Returns 0 on success, -1 if key isn't found or the value is
 * unterminated. */
static int extract_toml_string_value(const char *body, size_t body_len, const char *key, char *out,
                                      size_t out_size)
{
	char needle[64];
	const void *pos;
	const char *val_start, *val_end;
	size_t val_len;

	snprintf(needle, sizeof(needle), "%s = \"", key);
	pos = memmem(body, body_len, needle, strlen(needle));
	if (pos == NULL)
		return -1;
	val_start = (const char *)pos + strlen(needle);
	val_end = memchr(val_start, '"', (size_t)(body + body_len - val_start));
	if (val_end == NULL)
		return -1;
	val_len = (size_t)(val_end - val_start);
	if (val_len >= out_size)
		return -1;
	memcpy(out, val_start, val_len);
	out[val_len] = '\0';
	return 0;
}

static int wait_for_daemon(const struct thinc_client *c, int max_attempts)
{
	int i;
	struct thinc_response r;

	for (i = 0; i < max_attempts; i++) {
		if (thinc_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			thinc_response_free(&r);
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
	struct thinc_client client;
	int ok = 1;
	struct thinc_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/ldaptest/v1/rootfs", g_data_dir);

	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/ldaptest", g_data_dir);
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

	thinc_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. a real, long-running container to register against */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"ldapsrv\",\"image\":\"ldaptest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST ldapsrv, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 2. validation: nonexistent container -> 404 */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/ldap/servers",
	                       "{\"container\":\"no-such-container\",\"config_path\":\"/etc/glauth/glauth.cfg\"}",
	                       &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: register nonexistent container expected 404, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 3. validation: non-absolute config_path -> 400 */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/ldap/servers",
	                       "{\"container\":\"ldapsrv\",\"config_path\":\"etc/glauth/glauth.cfg\"}", &r) !=
	        0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: non-absolute config_path expected 400, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 4. a real, valid registration -> 201, echoes container/config_path */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/ldap/servers",
	                       "{\"container\":\"ldapsrv\",\"config_path\":\"/etc/glauth/glauth.cfg\"}", &r) !=
	        0 ||
	    r.status != 201 || !str_eq(json_str_field(r.json, "container"), "ldapsrv") ||
	    !str_eq(json_str_field(r.json, "config_path"), "/etc/glauth/glauth.cfg")) {
		fprintf(stderr, "FAIL: register ldapsrv, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 5. duplicate registration -> 409 */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/ldap/servers",
	                       "{\"container\":\"ldapsrv\",\"config_path\":\"/etc/glauth/glauth.cfg\"}", &r) !=
	        0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate registration expected 409, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 6. GET reflects it */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/ldap/servers", NULL, &r) != 0 || r.status != 200 ||
	    !servers_list_contains(r.json, "ldapsrv", "/etc/glauth/glauth.cfg")) {
		fprintf(stderr, "FAIL: GET /v1/ldap/servers did not show ldapsrv, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

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
		if (thinc_client_request(&client, "GET", "/v1/ldap/servers", NULL, &r) != 0 ||
		    r.status != 200 || !servers_list_contains(r.json, "ldapsrv", "/etc/glauth/glauth.cfg")) {
			fprintf(stderr,
			        "FAIL: ldapsrv binding did not survive a daemon restart, status=%d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);
	}

	/* 8. unregister -> 204, then GET no longer shows it */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "DELETE", "/v1/ldap/servers/ldapsrv", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: unregister ldapsrv, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/ldap/servers", NULL, &r) != 0 || r.status != 200 ||
	    servers_list_contains(r.json, "ldapsrv", NULL)) {
		fprintf(stderr, "FAIL: ldapsrv binding survived unregister, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 9. unregister of something never registered -> 404 */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "DELETE", "/v1/ldap/servers/ldapsrv", NULL, &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: unregister already-gone binding expected 404, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

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
	if (thinc_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"ldapsrv2\",\"image\":\"ldaptest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST ldapsrv2, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/ldap/servers",
	                       "{\"container\":\"ldapsrv2\",\"config_path\":\"/etc/glauth/glauth.cfg\"}", &r) !=
	        0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: register ldapsrv2, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	thinc_client_request(&client, "DELETE", "/v1/containers/ldapsrv2", NULL, &r);
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/ldap/servers", NULL, &r) != 0 || r.status != 200 ||
	    servers_list_contains(r.json, "ldapsrv2", NULL)) {
		fprintf(stderr, "FAIL: ldap server binding survived container deletion\n");
		ok = 0;
	}
	thinc_response_free(&r);

	/*
	 * 11-19. Task #726: user/group CRUD, and -- the part actually worth
	 * proving -- that the write-through mechanism really preserves an
	 * operator-authored config prefix while rendering the managed
	 * [[groups]]/[[users]] tail correctly. "ldapcfg" doesn't run real
	 * glauth (this project has no way to verify glauth's own fsnotify
	 * reload from inside this test suite -- confirmed directly against
	 * glauth's real source instead, see ldap.h's own header comment);
	 * what's under test here is entirely thincd's own code: the
	 * marker-based prefix-preserving rewrite in ldap_write_config_
	 * file(), read back via the real GET .../files endpoint (ADR-0055)
	 * exactly the way an operator or a future test with real glauth
	 * would.
	 */
	{
		static const char base_config_prefix[] = "# base config\nwatchconfig = true\n";
		struct json_value *jval;
		char passbcrypt_val[128]; /* hex-encoded PWHASH_BCRYPT_LEN (60) bytes -- 120 hex chars + NUL */

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"ldapcfg\",\"image\":\"ldaptest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
		                       "\"files\":[{\"path\":\"/etc/glauth/glauth.cfg\","
		                       "\"content\":\"# base config\\nwatchconfig = true\\n\"}]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST ldapcfg, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/servers",
		                       "{\"container\":\"ldapcfg\",\"config_path\":\"/etc/glauth/glauth.cfg\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: register ldapcfg, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 12. group create */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/groups",
		                       "{\"name\":\"engineers\",\"gidnumber\":6001}", &r) != 0 ||
		    r.status != 201 || !str_eq(json_str_field(r.json, "name"), "engineers")) {
			fprintf(stderr, "FAIL: create group engineers, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 13. duplicate group -> 409 */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/groups",
		                       "{\"name\":\"engineers\",\"gidnumber\":6002}", &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: duplicate group expected 409, got %d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 14. user create with an unknown primarygroup -> 400 */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/users",
		                       "{\"name\":\"nogroup\",\"uidnumber\":5002,\"primarygroup\":9999}",
		                       &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: unknown primarygroup expected 400, got %d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 15. real user create, with a password -- passbcrypt must
		 * never come back over the API (only has_password). Also
		 * carries a real ssh_public_key (ADR-0144 task #838) to prove
		 * it renders as glauth's own real `sshkeys = [...]` array. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/users",
		                       "{\"name\":\"j_doe\",\"uidnumber\":5001,\"primarygroup\":6001,"
		                       "\"mail\":\"j.doe@thinc.internal\",\"password\":\"dogood\","
		                       "\"ssh_public_key\":\"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAItest "
		                       "j_doe@thinc\"}",
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
		thinc_response_free(&r);

		/* 16. the container's own config file now shows the preserved
		 * prefix plus a correctly-rendered managed tail, including a
		 * real bcrypt hash of "dogood" (ADR-0144 -- bcrypt is
		 * deliberately salted/non-deterministic, so unlike the old
		 * passsha256 field this can't be checked against a fixed
		 * literal; instead confirm the real $2b$<cost>$ shape and
		 * capture the exact value to prove it survives an update
		 * unchanged in step 17). */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET",
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
		           memmem(r.body, r.body_len, "mail = \"j.doe@thinc.internal\"",
		                  strlen("mail = \"j.doe@thinc.internal\"")) == NULL) {
			fprintf(stderr, "FAIL: rendered config missing expected group/user fields\n");
			ok = 0;
		} else if (memmem(r.body, r.body_len,
		                   "sshkeys = [\"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAItest j_doe@thinc\"]",
		                   strlen("sshkeys = [\"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAItest "
		                          "j_doe@thinc\"]")) == NULL) {
			fprintf(stderr,
			        "FAIL: rendered config missing glauth's real sshkeys = [...] array "
			        "(ADR-0144 task #838)\n");
			ok = 0;
		} else if (extract_toml_string_value(r.body, r.body_len, "passbcrypt", passbcrypt_val,
		                                      sizeof(passbcrypt_val)) != 0) {
			fprintf(stderr, "FAIL: rendered config missing a passbcrypt field\n");
			ok = 0;
		} else {
			char decoded[64];

			/* passbcrypt is rendered hex-encoded (glauth's own real
			 * config-backend expectation, see ldap.c's own comment) --
			 * decode it back to confirm it's genuinely a real $2b$
			 * bcrypt hash underneath, not just an opaque hex blob. */
			if (hex_decode(passbcrypt_val, decoded, sizeof(decoded)) != 0 ||
			    strncmp(decoded, "$2b$", 4) != 0) {
				fprintf(stderr,
				        "FAIL: rendered config's passbcrypt didn't hex-decode to a real "
				        "$2b$ bcrypt hash, got hex: %s\n",
				        passbcrypt_val);
				ok = 0;
			}
		}
		thinc_response_free(&r);

		/* 17. update: change mail, omit password -- the existing hash
		 * must survive unchanged (byte-for-byte the same value captured
		 * above) in the re-rendered file */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/users/j_doe",
		                       "{\"uidnumber\":5001,\"primarygroup\":6001,"
		                       "\"mail\":\"jd@thinc.internal\"}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: update user j_doe, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET",
		                       "/v1/containers/ldapcfg/files?path=%2Fetc%2Fglauth%2Fglauth.cfg", NULL,
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET ldapcfg config file after update, status=%d\n", r.status);
			ok = 0;
		} else {
			char passbcrypt_after[128];

			if (memmem(r.body, r.body_len, "mail = \"jd@thinc.internal\"",
			           strlen("mail = \"jd@thinc.internal\"")) == NULL) {
				fprintf(stderr, "FAIL: update lost the mail change\n");
				ok = 0;
			} else if (extract_toml_string_value(r.body, r.body_len, "passbcrypt", passbcrypt_after,
			                                      sizeof(passbcrypt_after)) != 0 ||
			           strcmp(passbcrypt_after, passbcrypt_val) != 0) {
				fprintf(stderr,
				        "FAIL: update lost the existing password hash (was %s, now %s)\n",
				        passbcrypt_val, passbcrypt_after);
				ok = 0;
			}
		}
		thinc_response_free(&r);

		/* 17b (ADR-0144): real secondary-group membership -- a second
		 * group, then j_doe gains it as a secondary group alongside its
		 * existing primarygroup, echoed correctly on GET and rendered
		 * as glauth's own "othergroups" array. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/groups", "{\"name\":\"ops\",\"gidnumber\":6099}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: create group ops, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/users/j_doe",
		                       "{\"uidnumber\":5001,\"primarygroup\":6001,"
		                       "\"mail\":\"jd@thinc.internal\",\"secondary_groups\":[6099]}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: update user j_doe with secondary_groups, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *jsecondary = json_object_get(r.json, "secondary_groups");

			if (jsecondary == NULL || jsecondary->type != JSON_ARRAY || jsecondary->u.array.count != 1 ||
			    (int)json_as_number(jsecondary->u.array.items[0]) != 6099) {
				fprintf(stderr, "FAIL: secondary_groups not echoed correctly on update response\n");
				ok = 0;
			}
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET",
		                       "/v1/containers/ldapcfg/files?path=%2Fetc%2Fglauth%2Fglauth.cfg", NULL,
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET ldapcfg config file after secondary_groups update, status=%d\n",
			        r.status);
			ok = 0;
		} else if (memmem(r.body, r.body_len, "othergroups = [6099]", strlen("othergroups = [6099]")) ==
		           NULL) {
			fprintf(stderr, "FAIL: rendered config missing othergroups = [6099]\n");
			ok = 0;
		}
		thinc_response_free(&r);

		/* An invalid secondary group (no such gidnumber) is rejected,
		 * same validation primarygroup already gets. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/users/j_doe",
		                       "{\"uidnumber\":5001,\"primarygroup\":6001,"
		                       "\"secondary_groups\":[999999]}",
		                       &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: update with unknown secondary group expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* Cleanup: drop the secondary group again before the group
		 * itself is deleted below (a still-referenced group can still
		 * be deleted per this module's own "no cascading validation"
		 * posture, but leaving a dangling reference around isn't the
		 * point of this test). */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/users/j_doe",
		                       "{\"uidnumber\":5001,\"primarygroup\":6001,"
		                       "\"mail\":\"jd@thinc.internal\"}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: clear secondary_groups, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "DELETE", "/v1/ldap/groups/ops", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: delete group ops, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 17c (ADR-0144 task #838): can_search, now a real public field
		 * (previously internal-only, set only by the auto-provisioning
		 * hook) -- off by default, settable via PUT, echoed on GET, and
		 * rendered as glauth's own [[users.capabilities]] stanza. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/users/j_doe",
		                       "{\"uidnumber\":5001,\"primarygroup\":6001,"
		                       "\"mail\":\"jd@thinc.internal\",\"can_search\":true}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: update user j_doe with can_search, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *jcan_search = json_object_get(r.json, "can_search");

			if (jcan_search == NULL || jcan_search->type != JSON_BOOL || !jcan_search->u.boolean) {
				fprintf(stderr, "FAIL: can_search not echoed true on update response\n");
				ok = 0;
			}
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET",
		                       "/v1/containers/ldapcfg/files?path=%2Fetc%2Fglauth%2Fglauth.cfg", NULL,
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET ldapcfg config file after can_search update, status=%d\n",
			        r.status);
			ok = 0;
		} else if (memmem(r.body, r.body_len, "[[users.capabilities]]",
		                   strlen("[[users.capabilities]]")) == NULL) {
			fprintf(stderr, "FAIL: rendered config missing [[users.capabilities]] stanza\n");
			ok = 0;
		}
		thinc_response_free(&r);

		/* PUT is full-field-replacement: omitting can_search now turns
		 * it back off, same as every other field here. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/users/j_doe",
		                       "{\"uidnumber\":5001,\"primarygroup\":6001,"
		                       "\"mail\":\"jd@thinc.internal\"}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: clear can_search, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *jcan_search = json_object_get(r.json, "can_search");

			if (jcan_search == NULL || jcan_search->type != JSON_BOOL || jcan_search->u.boolean) {
				fprintf(stderr, "FAIL: can_search not cleared back to false\n");
				ok = 0;
			}
		}
		thinc_response_free(&r);

		/* 18. delete the user -> the rendered file no longer names it,
		 * but the group stanza survives */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "DELETE", "/v1/ldap/users/j_doe", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: delete user j_doe, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET",
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
		thinc_response_free(&r);

		/* 19. delete the group too, list endpoints reflect it */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "DELETE", "/v1/ldap/groups/engineers", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: delete group engineers, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET", "/v1/ldap/groups", NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/ldap/groups, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *groups = json_object_get(r.json, "groups");

			if (groups == NULL || groups->type != JSON_ARRAY || groups->u.array.count != 0) {
				fprintf(stderr, "FAIL: /v1/ldap/groups not empty after delete\n");
				ok = 0;
			}
		}
		thinc_response_free(&r);

		thinc_client_request(&client, "DELETE", "/v1/containers/ldapcfg", NULL, &r);
		thinc_response_free(&r);
	}

	/*
	 * 20-25. Task #727: the automatic provisioning hook. Mirrors
	 * test_pki.c's own step 6b (pki_issue delivery verification) as
	 * closely as possible -- a service account (not a human login,
	 * task #726's own territory) auto-created for the container
	 * itself, owner set to the container's name, a "search"
	 * capability granted by default (glauth defaults to deny-all),
	 * and a freshly generated secret delivered into the container's
	 * own filesystem at /etc/thinc-ldap/bind.secret -- read directly
	 * via /proc/<pid>/root/, the same privilege pki_issue's own test
	 * already established (ADR-0013), not just assumed from a 201.
	 */
	{
		int provtest_pid = -1;

		/* 20. a group for provisioned accounts to join */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/groups",
		                       "{\"name\":\"svcaccts\",\"gidnumber\":7001}", &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: create group svcaccts, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 21. ldap_provision without ldap_group naming a real group -> 400 */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"badprov\",\"image\":\"ldaptest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
		                       "\"ldap_provision\":true}",
		                       &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: ldap_provision without a valid ldap_group expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 22. a real provisioned container -- default ldap_user
		 * (the container's own name), default ldap_uid (allocated),
		 * default ldap_secret_dir (/etc/thinc-ldap) */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers",
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
		thinc_response_free(&r);

		/* 23. the auto-created service account: owner==container name,
		 * primarygroup resolved, can_search granted, has_password */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET", "/v1/ldap/users/provtest", NULL, &r) != 0 ||
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
		thinc_response_free(&r);

		/* 24. the secret was really delivered into the container's own
		 * filesystem, at the default path, chmod 0600 */
		if (provtest_pid > 0) {
			char proc_path[192];
			struct stat st;
			FILE *f;
			char secret[128] = { 0 };
			size_t n = 0;

			snprintf(proc_path, sizeof(proc_path), "/proc/%d/root/etc/thinc-ldap/bind.secret",
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
		if (thinc_client_request(&client, "DELETE", "/v1/containers/provtest", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: delete provtest, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET", "/v1/ldap/users/provtest", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr,
			        "FAIL: provtest ldap user survived container deletion, status=%d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		thinc_client_request(&client, "DELETE", "/v1/ldap/groups/svcaccts", NULL, &r);
		thinc_response_free(&r);
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
		if (thinc_client_request(&client, "GET", "/v1/ldap/config", NULL, &r) != 0 ||
		    r.status != 200 ||
		    (long)json_as_number(json_object_get(r.json, "start_uid")) != 10000 ||
		    (long)json_as_number(json_object_get(r.json, "start_gid")) != 10000) {
			fprintf(stderr, "FAIL: GET default ldap config, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 27. group create with no gidnumber -> auto-allocated from start_gid */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/groups", "{\"name\":\"autogid1\"}",
		                       &r) != 0 ||
		    r.status != 201 ||
		    (long)json_as_number(json_object_get(r.json, "gidnumber")) != 10000) {
			fprintf(stderr, "FAIL: create group with no gidnumber, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 28. user create with no uidnumber -> auto-allocated from start_uid */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/users",
		                       "{\"name\":\"autouid1\",\"primarygroup\":10000}", &r) != 0 ||
		    r.status != 201 ||
		    (long)json_as_number(json_object_get(r.json, "uidnumber")) != 10000) {
			fprintf(stderr, "FAIL: create user with no uidnumber, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 29. PUT ldap config -- changes take effect for future allocations only */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/config",
		                       "{\"start_uid\":50000,\"start_gid\":50000}", &r) != 0 ||
		    r.status != 200 ||
		    (long)json_as_number(json_object_get(r.json, "start_uid")) != 50000 ||
		    (long)json_as_number(json_object_get(r.json, "start_gid")) != 50000) {
			fprintf(stderr, "FAIL: PUT ldap config, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 29.5. issue #66: client-login fields -- individually optional
		 * (this PUT must not disturb the floors just set), credential
		 * write-only (bind_password_set reported, the value never
		 * echoed), and the floors' own PUT must not disturb these. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/config",
		                       "{\"client_uri\":\"ldap://10.0.0.1:3893/\","
		                       "\"base_dn\":\"dc=t,dc=local\","
		                       "\"bind_dn\":\"cn=svc,dc=t,dc=local\","
		                       "\"bind_password\":\"s3cret\"}",
		                       &r) != 0 ||
		    r.status != 200 ||
		    !str_eq(json_str_field(r.json, "client_uri"), "ldap://10.0.0.1:3893/") ||
		    !str_eq(json_str_field(r.json, "base_dn"), "dc=t,dc=local") ||
		    !str_eq(json_str_field(r.json, "bind_dn"), "cn=svc,dc=t,dc=local") ||
		    json_object_get(r.json, "bind_password") != NULL ||
		    json_object_get(r.json, "bind_password_set") == NULL ||
		    (long)json_as_number(json_object_get(r.json, "start_uid")) != 50000) {
			fprintf(stderr, "FAIL: PUT ldap client config (status=%d)\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 30. a second auto-allocated group/user now starts from 50000,
		 * not colliding with autogid1/autouid1's own 10000 */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/groups", "{\"name\":\"autogid2\"}",
		                       &r) != 0 ||
		    r.status != 201 ||
		    (long)json_as_number(json_object_get(r.json, "gidnumber")) != 50000) {
			fprintf(stderr, "FAIL: create group after config change, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 31. group update -- PUT edits gidnumber in place */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/groups/autogid1",
		                       "{\"gidnumber\":10999}", &r) != 0 ||
		    r.status != 200 ||
		    (long)json_as_number(json_object_get(r.json, "gidnumber")) != 10999) {
			fprintf(stderr, "FAIL: PUT autogid1 update, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET", "/v1/ldap/groups/autogid1", NULL, &r) != 0 ||
		    r.status != 200 ||
		    (long)json_as_number(json_object_get(r.json, "gidnumber")) != 10999) {
			fprintf(stderr, "FAIL: GET autogid1 after update, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 32. group update colliding with a different group's gidnumber -> 409 */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/groups/autogid1",
		                       "{\"gidnumber\":50000}", &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: PUT autogid1 gidnumber collision expected 409, got %d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 33. group update on a nonexistent group -> 404 */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/groups/no-such-group",
		                       "{\"gidnumber\":10001}", &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: PUT nonexistent group expected 404, got %d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 34. ADR-0147: renaming a group in place -- old name gone, new
		 * name resolves with every other field (gidnumber) untouched. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/groups/autogid1",
		                       "{\"name\":\"autogid1renamed\",\"gidnumber\":10999}", &r) != 0 ||
		    r.status != 200 ||
		    strcmp(json_str_field(r.json, "name"), "autogid1renamed") != 0) {
			fprintf(stderr, "FAIL: rename group autogid1, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET", "/v1/ldap/groups/autogid1", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: old group name should be gone after rename, status=%d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET", "/v1/ldap/groups/autogid1renamed", NULL, &r) != 0 ||
		    r.status != 200 ||
		    (long)json_as_number(json_object_get(r.json, "gidnumber")) != 10999) {
			fprintf(stderr, "FAIL: renamed group not found under new name, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* Renaming to an already-taken name is a real, rejected
		 * collision, not silently accepted. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/groups/autogid1renamed",
		                       "{\"name\":\"autogid2\",\"gidnumber\":10999}", &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: rename group to an existing name expected 409, got %d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* 35. Same mechanics, for a user. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/ldap/users/autouid1",
		                       "{\"name\":\"autouid1renamed\",\"uidnumber\":10000,\"primarygroup\":10999}",
		                       &r) != 0 ||
		    r.status != 200 ||
		    strcmp(json_str_field(r.json, "name"), "autouid1renamed") != 0) {
			fprintf(stderr, "FAIL: rename user autouid1, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET", "/v1/ldap/users/autouid1", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: old user name should be gone after rename, status=%d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET", "/v1/ldap/users/autouid1renamed", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: renamed user not found under new name, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		thinc_client_request(&client, "DELETE", "/v1/ldap/users/autouid1renamed", NULL, &r);
		thinc_response_free(&r);
		thinc_client_request(&client, "DELETE", "/v1/ldap/groups/autogid1renamed", NULL, &r);
		thinc_response_free(&r);
		thinc_client_request(&client, "DELETE", "/v1/ldap/groups/autogid2", NULL, &r);
		thinc_response_free(&r);
	}

	/*
	 * ADR-0146: a real, previously-undiscovered gap -- ldap_init()
	 * loads registered server bindings from disk before any container
	 * has started, so a server-serving container that comes back up on
	 * daemon restart (restart:"always", the exact shape a real
	 * long-lived glauth deployment has) gets a fresh, empty managed
	 * user/group section unless something explicitly re-pushes current
	 * state afterward -- confirmed live to produce a real host-auth
	 * lockout (every account's own LDAP entry silently vanished after
	 * a routine reboot, with no new LDAP write in between to trigger a
	 * resync). Mirrors the identical, already-tested dns_server_sync_
	 * all()-after-restart fix (ADR-0091) -- this is ldap_record_sync_
	 * all()'s own missing counterpart, now called from the same
	 * daemon-startup point. Proven here the only way that actually
	 * matters: restart:"always" container, real content rendered
	 * in, a real daemon restart, then confirm the SAME content is
	 * still there with zero new LDAP writes in between.
	 */
	{
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"ldapresync\",\"image\":\"ldaptest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],\"restart\":\"always\","
		                       "\"files\":[{\"path\":\"/etc/glauth/glauth.cfg\","
		                       "\"content\":\"watchconfig = true\\n\"}]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST ldapresync, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/servers",
		                       "{\"container\":\"ldapresync\",\"config_path\":\"/etc/glauth/glauth.cfg\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: register ldapresync, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/groups",
		                       "{\"name\":\"resyncgrp\",\"gidnumber\":6501}", &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: create group resyncgrp, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET",
		                       "/v1/containers/ldapresync/files?path=%2Fetc%2Fglauth%2Fglauth.cfg",
		                       NULL, &r) != 0 ||
		    r.status != 200 ||
		    memmem(r.body, r.body_len, "name = \"resyncgrp\"", strlen("name = \"resyncgrp\"")) ==
		        NULL) {
			fprintf(stderr, "FAIL: ldapresync config missing resyncgrp before restart\n");
			ok = 0;
		}
		thinc_response_free(&r);

		if (stop_daemon(daemon_pid) != 0) {
			fprintf(stderr, "FAIL: daemon did not exit cleanly (ldapresync)\n");
			ok = 0;
		}
		daemon_pid = start_daemon();
		if (daemon_pid < 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
		if (wait_for_daemon(&client, 50) != 0) {
			fprintf(stderr, "FAIL: daemon never came back up after restart (ldapresync)\n");
			ok = 0;
		} else {
			/* No new LDAP write anywhere in between -- if this still
			 * shows resyncgrp, the fresh, restart:"always"-respawned
			 * container's own config was genuinely re-synced by the
			 * daemon's own startup path, not by anything this test
			 * itself did. */
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "GET",
			                       "/v1/containers/ldapresync/files?path=%2Fetc%2Fglauth%2Fglauth.cfg",
			                       NULL, &r) != 0 ||
			    r.status != 200 ||
			    memmem(r.body, r.body_len, "name = \"resyncgrp\"", strlen("name = \"resyncgrp\"")) ==
			        NULL) {
				fprintf(stderr,
				        "FAIL: ldapresync config lost resyncgrp after a daemon restart -- "
				        "startup resync regressed\n");
				ok = 0;
			}
			thinc_response_free(&r);
		}

		memset(&r, 0, sizeof(r));
		thinc_client_request(&client, "DELETE", "/v1/ldap/groups/resyncgrp", NULL, &r);
		thinc_response_free(&r);
	}

	/*
	 * ADR-0148: glauth's own baseDN becomes daemon-managed, derived
	 * from hostauth-config's own real, canonical ldap_base_dn -- not a
	 * fourth independently-typed copy. A registered server's own
	 * prefix carries a real baseDN line here (unlike every earlier
	 * test container in this file, which never sets one at all, so
	 * none of them exercise this path).
	 */
	{
		static const char basedn_prefix[] =
		    "[backend]\n  datastore = \"config\"\n  baseDN = \"dc=old,dc=example\"\n\n"
		    "[behaviors]\n  IgnoreCapabilities = true\n";

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"basedntest\",\"image\":\"ldaptest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
		                       "\"files\":[{\"path\":\"/etc/glauth/glauth.cfg\","
		                       "\"content\":\"[backend]\\n  datastore = \\\"config\\\"\\n  "
		                       "baseDN = \\\"dc=old,dc=example\\\"\\n\\n[behaviors]\\n  "
		                       "IgnoreCapabilities = true\\n\"}]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST basedntest, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/servers",
		                       "{\"container\":\"basedntest\",\"config_path\":\"/etc/glauth/glauth.cfg\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: register basedntest, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* Registration itself already triggers a sync -- confirm the
		 * rewrite already landed, before hostauth-config is even
		 * touched by anything else in this test file. Wait, actually:
		 * ldap_base_dn is still empty at this point (never configured
		 * in this test file before now), so the rewrite is a
		 * deliberate no-op here -- the OLD value must still be intact. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET",
		                       "/v1/containers/basedntest/files?path=%2Fetc%2Fglauth%2Fglauth.cfg",
		                       NULL, &r) != 0 ||
		    r.status != 200 ||
		    memmem(r.body, r.body_len, "dc=old,dc=example", strlen("dc=old,dc=example")) == NULL) {
			fprintf(stderr,
			        "FAIL: basedntest config should still show the old baseDN before "
			        "ldap_base_dn is ever configured, status=%d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* Now set the real, canonical ldap_base_dn. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "PUT", "/v1/system/hostauth-config",
		                       "{\"admin_groups\":[],\"idle_timeout_seconds\":900,"
		                       "\"ldap_base_dn\":\"dc=new,dc=test\"}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: PUT hostauth-config ldap_base_dn, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* Any LDAP mutation triggers ldap_record_sync_all() -- a
		 * throwaway group create is as good as any other for that. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/ldap/groups",
		                       "{\"name\":\"basedntrigger\",\"gidnumber\":6501}", &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: create group basedntrigger, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET",
		                       "/v1/containers/basedntest/files?path=%2Fetc%2Fglauth%2Fglauth.cfg",
		                       NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET basedntest config after ldap_base_dn set, status=%d\n",
			        r.status);
			ok = 0;
		} else {
			int has_new = memmem(r.body, r.body_len, "dc=new,dc=test", strlen("dc=new,dc=test")) !=
			              NULL;
			int has_old =
			    memmem(r.body, r.body_len, "dc=old,dc=example", strlen("dc=old,dc=example")) != NULL;
			int has_datastore =
			    memmem(r.body, r.body_len, "datastore = \"config\"",
			           strlen("datastore = \"config\"")) != NULL;
			int has_behavior =
			    memmem(r.body, r.body_len, "IgnoreCapabilities = true",
			           strlen("IgnoreCapabilities = true")) != NULL;

			if (!has_new || has_old || !has_datastore || !has_behavior) {
				fprintf(stderr,
				        "FAIL: baseDN rewrite incorrect (new=%d old=%d datastore=%d "
				        "behavior=%d), body=%.*s\n",
				        has_new, has_old, has_datastore, has_behavior, (int)r.body_len, r.body);
				ok = 0;
			}
		}
		thinc_response_free(&r);

		thinc_client_request(&client, "DELETE", "/v1/ldap/groups/basedntrigger", NULL, &r);
		thinc_response_free(&r);
	}

	/*
	 * Issue #84: health filtering must also apply to an EXPLICIT
	 * client_uri. It never did -- ldap_effective_client_uri() returned
	 * the configured string verbatim before any filtering ran, so
	 * everything issue #81 built was inert on any box with client_uri
	 * set, which included the real one. Drained or unhealthy servers
	 * were still handed to every client.
	 *
	 * Driven through GET /ldap/config's own effective_client_uri, which
	 * exists for exactly this reason: what clients are actually handed
	 * is a different question from what is configured, and until now
	 * nothing anywhere reported the first one.
	 */
	{
		char ip[64] = "";
		char body[512];

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/networks",
		                       "{\"name\":\"ldapfilt\",\"subnet\":\"10.77.0.0\",\"prefix_len\":24}",
		                       &r) != 0 ||
		    (r.status != 201 && r.status != 409)) {
			fprintf(stderr, "FAIL: create ldapfilt network, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"ldapfsrv\",\"image\":\"ldaptest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
		                       "\"networks\":[{\"name\":\"ldapfilt\"}]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST ldapfsrv, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *nets = json_object_get(r.json, "networks");
			const char *found = nets != NULL && nets->type == JSON_ARRAY && nets->u.array.count > 0
			                        ? json_str_field(nets->u.array.items[0], "ip")
			                        : NULL;

			if (found != NULL)
				snprintf(ip, sizeof(ip), "%s", found);
		}
		thinc_response_free(&r);

		if (ip[0] == '\0') {
			fprintf(stderr, "FAIL: ldapfsrv has no IP to filter on\n");
			ok = 0;
		} else {
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "POST", "/v1/ldap/servers",
			                       "{\"container\":\"ldapfsrv\","
			                       "\"config_path\":\"/etc/glauth/glauth.cfg\"}",
			                       &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr, "FAIL: register ldapfsrv, status=%d\n", r.status);
				ok = 0;
			}
			thinc_response_free(&r);

			/* Two URIs: one that IS this registered server, one that
			 * maps to nothing thinC manages. */
			snprintf(body, sizeof(body),
			         "{\"client_uri\":\"ldap://%s:%d/ ldap://198.51.100.7:%d/\"}", ip,
			         HOSTAUTH_LDAP_DEFAULT_PORT, HOSTAUTH_LDAP_DEFAULT_PORT);
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "PUT", "/v1/ldap/config", body, &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: PUT client_uri for filtering, status=%d\n", r.status);
				ok = 0;
			}
			thinc_response_free(&r);

			/* Healthy (never probed counts as in service): both kept. */
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "GET", "/v1/ldap/config", NULL, &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: GET ldap config, status=%d\n", r.status);
				ok = 0;
			} else {
				const char *eff = json_str_field(r.json, "effective_client_uri");

				if (eff == NULL || strstr(eff, ip) == NULL ||
				    strstr(eff, "198.51.100.7") == NULL) {
					fprintf(stderr, "FAIL: healthy effective_client_uri=%s\n",
					        eff != NULL ? eff : "(null)");
					ok = 0;
				}
			}
			thinc_response_free(&r);

			/* Drained: that URI must go, the unmanaged one must stay. */
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "PUT", "/v1/system/server-health/ldap/ldapfsrv",
			                       "{\"drained\":true}", &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: drain ldapfsrv, status=%d\n", r.status);
				ok = 0;
			}
			thinc_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "GET", "/v1/ldap/config", NULL, &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: GET ldap config after drain, status=%d\n", r.status);
				ok = 0;
			} else {
				const char *eff = json_str_field(r.json, "effective_client_uri");

				if (eff == NULL || strstr(eff, ip) != NULL ||
				    strstr(eff, "198.51.100.7") == NULL) {
					fprintf(stderr,
					        "FAIL: drained server not filtered from explicit client_uri "
					        "(effective=%s, drained ip=%s)\n",
					        eff != NULL ? eff : "(null)", ip);
					ok = 0;
				}
			}
			thinc_response_free(&r);

			/* Filtering to nothing must fall back to the configured
			 * list rather than hand a client an empty one -- a partial
			 * outage must never be turned into a total one. */
			snprintf(body, sizeof(body), "{\"client_uri\":\"ldap://%s:%d/\"}", ip,
			         HOSTAUTH_LDAP_DEFAULT_PORT);
			memset(&r, 0, sizeof(r));
			thinc_client_request(&client, "PUT", "/v1/ldap/config", body, &r);
			thinc_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "GET", "/v1/ldap/config", NULL, &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: GET ldap config for empty-fallback, status=%d\n",
				        r.status);
				ok = 0;
			} else {
				const char *eff = json_str_field(r.json, "effective_client_uri");

				if (eff == NULL || strstr(eff, ip) == NULL) {
					fprintf(stderr, "FAIL: all-filtered did not fall back (effective=%s)\n",
					        eff != NULL ? eff : "(null)");
					ok = 0;
				}
			}
			thinc_response_free(&r);
		}

		/* Both removed, network included: a leaked bridge outlives the
		 * daemon and makes the NEXT run fail at network creation with a
		 * 500 that looks nothing like its real cause. */
		memset(&r, 0, sizeof(r));
		thinc_client_request(&client, "DELETE", "/v1/containers/ldapfsrv", NULL, &r);
		thinc_response_free(&r);
		/* ADR-0180: container teardown is asynchronous, so the network
		 * still has an attachment for a moment after DELETE returns
		 * 204 and removing it immediately fails. Poll until the
		 * container is really gone, then remove the network. */
		{
			int i;

			for (i = 0; i < 50; i++) {
				memset(&r, 0, sizeof(r));
				if (thinc_client_request(&client, "GET", "/v1/containers/ldapfsrv", NULL, &r) == 0 &&
				    r.status == 404) {
					thinc_response_free(&r);
					break;
				}
				thinc_response_free(&r);
				usleep(100000);
			}
		}
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "DELETE", "/v1/networks/ldapfilt", NULL, &r) != 0 ||
		    (r.status != 204 && r.status != 404)) {
			fprintf(stderr, "FAIL: could not remove ldapfilt network (status=%d) -- a leaked "
			                "bridge breaks the NEXT run at network creation\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);
	}

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);

	printf("LDAP RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
