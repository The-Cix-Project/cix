/*
 * Phase 9 parts 1-2 end-to-end test: proves the PKI resource family
 * (POST/GET /v1/pki/ca, POST/GET/DELETE /v1/pki/certs -- daemon/src/pki.c)
 * over real HTTP, with real cryptographic verification as the actual
 * payoff -- not a config-file or string check. Actual crypto (keypair
 * generation, CSR signing) is done by the daemon shelling out to the
 * system's real, unmodified openssl binary (ADR-0007: hand-rolled
 * applies to this project's own platform components, not real
 * software it invokes). Part 2 additionally proves automatic
 * per-container cert issuance + delivery into a running container's
 * own filesystem via /proc/<pid>/root/ (ADR-0013).
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7627
#define PORT_ARG "--port=7627"

static char g_data_dir[PATH_MAX];
static char g_pki_state_dir[PATH_MAX];
static char g_pki_image_root[PATH_MAX];

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

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static int json_has_field(const struct json_value *obj, const char *key)
{
	return json_object_get(obj, key) != NULL;
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

/* Removes any pre-existing CA state so this test is deterministic and
 * rerunnable regardless of what a previous run left behind -- there
 * is no CA-delete endpoint (a deliberate v1 boundary: no CA
 * regeneration), so this is the only way to guarantee a clean slate
 * for the "before bootstrap" assertions on every run. */
static void reset_pki_state_dir(void)
{
	char cmd[PATH_MAX + 16];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_pki_state_dir);
	system(cmd);
}

/* Runs `openssl <args...>` (NULL-terminated argv after argv[0]) and
 * returns 0 if it exits successfully, -1 otherwise -- used for real
 * cryptographic verification (chain validation, SAN inspection), not
 * a string match against the returned PEM. */
static int run_openssl_argv(char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("/usr/bin/openssl", argv, environ);
		perror("execve openssl");
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int write_file(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");

	if (f == NULL) {
		perror(path);
		return -1;
	}
	if (fputs(content, f) == EOF) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	int ok = 1;
	struct cix_response r;
	char ca_cert_pem[8192] = { 0 };
	char leaf_cert_pem[8192] = { 0 };

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_pki_state_dir, sizeof(g_pki_state_dir), "%s/state/pki", g_data_dir);
	snprintf(g_pki_image_root, sizeof(g_pki_image_root), "%s/rebuildable/images/pkitest/v1/rootfs", g_data_dir);

	reset_pki_state_dir();
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/pkitest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}

	if (test_image_fixture_build(g_pki_image_root, "build/daemon_child", "daemon_child") != 0) {
		fprintf(stderr, "FAIL: could not stage pkitest image\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

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

	/* 1. before bootstrap: GET ca -> 404, POST certs -> 400 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/pki/ca", NULL, &r) != 0 || r.status != 404) {
		fprintf(stderr, "FAIL: GET /v1/pki/ca before bootstrap expected 404, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pki/certs", "{\"name\":\"svc\"}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: POST /v1/pki/certs before bootstrap expected 400, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"earlypki\",\"image\":\"pkitest\","
	                       "\"cmd\":[\"/bin/daemon_child\"],\"pki_issue\":true}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: pki_issue before CA bootstrap expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 2. bootstrap the CA; confirm subject/serial/dates populated and
	 * cert_pem is real, parseable PEM (round-tripped through openssl
	 * itself, not a string check); a second bootstrap is 409. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pki/ca", "{\"common_name\":\"Test Root CA\"}",
	                       &r) != 0 ||
	    r.status != 201 || !str_eq(json_str_field(r.json, "subject"), "CN = Test Root CA") ||
	    json_str_field(r.json, "serial") == NULL || json_str_field(r.json, "not_after") == NULL ||
	    json_str_field(r.json, "cert_pem") == NULL) {
		fprintf(stderr, "FAIL: POST /v1/pki/ca, status=%d\n", r.status);
		ok = 0;
	} else {
		snprintf(ca_cert_pem, sizeof(ca_cert_pem), "%s", json_str_field(r.json, "cert_pem"));
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pki/ca", "{}", &r) != 0 || r.status != 409) {
		fprintf(stderr, "FAIL: second POST /v1/pki/ca expected 409, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	if (ca_cert_pem[0] != '\0') {
		char scratch_dir[] = "/tmp/cix_test_pki_XXXXXX";
		char ca_path[128], leaf_path[128];

		if (mkdtemp(scratch_dir) == NULL) {
			fprintf(stderr, "FAIL: mkdtemp\n");
			ok = 0;
		} else {
			snprintf(ca_path, sizeof(ca_path), "%s/ca.crt", scratch_dir);
			write_file(ca_path, ca_cert_pem);

			{
				char *argv[] = { "/usr/bin/openssl", "x509", "-in", ca_path, "-noout",
					          "-subject", NULL };

				if (run_openssl_argv(argv) != 0) {
					fprintf(stderr, "FAIL: CA cert_pem did not parse as a real x509 cert\n");
					ok = 0;
				}
			}

			/* 3. issue a leaf cert with SANs; real crypto proof:
			 * verify it actually chains to the CA and confirm the
			 * SAN list round-trips through openssl. */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pki/certs",
			                       "{\"name\":\"svc.internal\",\"sans\":[\"svc.internal\",\"svc\"],"
			                       "\"days\":30}",
			                       &r) != 0 ||
			    r.status != 201 || !str_eq(json_str_field(r.json, "name"), "svc.internal") ||
			    json_str_field(r.json, "cert_pem") == NULL ||
			    json_str_field(r.json, "key_pem") == NULL) {
				fprintf(stderr, "FAIL: POST /v1/pki/certs, status=%d\n", r.status);
				ok = 0;
			} else {
				snprintf(leaf_cert_pem, sizeof(leaf_cert_pem), "%s",
				         json_str_field(r.json, "cert_pem"));
			}
			cix_response_free(&r);

			if (leaf_cert_pem[0] != '\0') {
				snprintf(leaf_path, sizeof(leaf_path), "%s/leaf.crt", scratch_dir);
				write_file(leaf_path, leaf_cert_pem);

				{
					char *argv[] = { "/usr/bin/openssl", "verify", "-CAfile", ca_path,
						          leaf_path, NULL };

					if (run_openssl_argv(argv) != 0) {
						fprintf(stderr,
						        "FAIL: leaf cert does not verify against the CA\n");
						ok = 0;
					}
				}
				{
					char sanbuf[256] = { 0 };
					int pipefd[2];
					pid_t pid;
					int status;

					if (pipe(pipefd) == 0 && (pid = fork()) >= 0) {
						if (pid == 0) {
							char *argv[] = { "/usr/bin/openssl", "x509", "-in", leaf_path,
								          "-noout", "-ext", "subjectAltName", NULL };

							close(pipefd[0]);
							dup2(pipefd[1], STDOUT_FILENO);
							close(pipefd[1]);
							execve("/usr/bin/openssl", argv, environ);
							_exit(127);
						}
						close(pipefd[1]);
						read(pipefd[0], sanbuf, sizeof(sanbuf) - 1);
						close(pipefd[0]);
						waitpid(pid, &status, 0);
					}
					if (strstr(sanbuf, "DNS:svc.internal") == NULL ||
					    strstr(sanbuf, "DNS:svc") == NULL) {
						fprintf(stderr,
						        "FAIL: leaf cert SAN list did not round-trip, got: %s\n",
						        sanbuf);
						ok = 0;
					}
				}
			}

			{
				char cmd[192];

				snprintf(cmd, sizeof(cmd), "rm -rf '%s'", scratch_dir);
				system(cmd);
			}
		}
	} else {
		fprintf(stderr, "FAIL: skipping crypto verification, CA cert_pem was never captured\n");
		ok = 0;
	}

	/* 4. duplicate name -> 409; invalid name -> 400 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pki/certs", "{\"name\":\"svc.internal\"}", &r) !=
	        0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate cert name expected 409, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pki/certs", "{\"name\":\"bad..name\"}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: invalid cert name expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 5. list is metadata-only; single-get has cert_pem but never key_pem */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/pki/certs", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/pki/certs, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *certs = json_object_get(r.json, "certs");
		int found = 0;
		size_t i;

		if (certs != NULL && certs->type == JSON_ARRAY) {
			for (i = 0; i < certs->u.array.count; i++) {
				const struct json_value *item = certs->u.array.items[i];

				if (str_eq(json_str_field(item, "name"), "svc.internal")) {
					found = 1;
					if (json_has_field(item, "key_pem") || json_has_field(item, "cert_pem")) {
						fprintf(stderr,
						        "FAIL: list view leaked cert_pem/key_pem\n");
						ok = 0;
					}
				}
			}
		}
		if (!found) {
			fprintf(stderr, "FAIL: svc.internal missing from GET /v1/pki/certs\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/pki/certs/svc.internal", NULL, &r) != 0 ||
	    r.status != 200 || json_str_field(r.json, "cert_pem") == NULL) {
		fprintf(stderr, "FAIL: GET /v1/pki/certs/svc.internal, status=%d\n", r.status);
		ok = 0;
	} else if (json_has_field(r.json, "key_pem")) {
		fprintf(stderr, "FAIL: GET single cert leaked key_pem -- the 'shown once' guarantee is broken\n");
		ok = 0;
	}
	cix_response_free(&r);

	/* 6. delete removes both the index entry and the on-disk files */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/pki/certs/svc.internal", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE svc.internal expected 204, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/pki/certs/svc.internal", NULL, &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: GET svc.internal after delete expected 404, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	{
		struct stat st;
		char key_path[PATH_MAX], crt_path[PATH_MAX];

		snprintf(key_path, sizeof(key_path), "%s/certs/svc.internal.key", g_pki_state_dir);
		snprintf(crt_path, sizeof(crt_path), "%s/certs/svc.internal.crt", g_pki_state_dir);
		if (stat(key_path, &st) == 0 || stat(crt_path, &st) == 0) {
			fprintf(stderr, "FAIL: svc.internal's key/cert files still exist on disk after delete\n");
			ok = 0;
		}
	}

	/*
	 * 6b. Phase 9 part 2: automatic per-container cert issuance +
	 * delivery. A container created with pki_issue:true gets its own
	 * cert (owner == its own name) delivered into its own filesystem
	 * at /etc/cix-tls/{tls.crt,tls.key} -- read directly via
	 * /proc/<pid>/root/, the same privilege the daemon itself uses
	 * (ADR-0013), not just assumed from a 201 response.
	 */
	{
		int webtls_pid = -1;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"webtls\",\"image\":\"pkitest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"20\"],\"pki_issue\":true}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST webtls (pki_issue), status=%d\n", r.status);
			ok = 0;
		} else {
			webtls_pid = (int)json_as_number(json_object_get(r.json, "pid"));
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pki/certs/webtls", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "owner"), "webtls")) {
			fprintf(stderr, "FAIL: GET webtls cert, status=%d, owner=%s\n", r.status,
			        json_str_field(r.json, "owner") ? json_str_field(r.json, "owner") : "(null)");
			ok = 0;
		}
		cix_response_free(&r);

		if (webtls_pid > 0) {
			char proc_path[160];
			struct stat st;

			snprintf(proc_path, sizeof(proc_path), "/proc/%d/root/etc/cix-tls/tls.key",
			         webtls_pid);
			if (stat(proc_path, &st) != 0 || (st.st_mode & 0777) != 0600) {
				fprintf(stderr,
				        "FAIL: delivered tls.key missing or not chmod 0600 (path=%s)\n",
				        proc_path);
				ok = 0;
			}

			snprintf(proc_path, sizeof(proc_path), "/proc/%d/root/etc/cix-tls/tls.crt",
			         webtls_pid);
			{
				FILE *f = fopen(proc_path, "r");
				char delivered_pem[8192] = { 0 };
				size_t n = 0;

				if (f == NULL) {
					fprintf(stderr, "FAIL: could not open delivered tls.crt (%s)\n",
					        proc_path);
					ok = 0;
				} else {
					n = fread(delivered_pem, 1, sizeof(delivered_pem) - 1, f);
					fclose(f);
					delivered_pem[n] = '\0';
				}

				if (n > 0 && ca_cert_pem[0] != '\0') {
					char scratch_dir2[] = "/tmp/cix_test_pki2_XXXXXX";

					if (mkdtemp(scratch_dir2) == NULL) {
						fprintf(stderr, "FAIL: mkdtemp (2)\n");
						ok = 0;
					} else {
						char ca_path2[192], leaf_path2[192], cmd2[224];

						snprintf(ca_path2, sizeof(ca_path2), "%s/ca.crt", scratch_dir2);
						snprintf(leaf_path2, sizeof(leaf_path2), "%s/leaf.crt", scratch_dir2);
						write_file(ca_path2, ca_cert_pem);
						write_file(leaf_path2, delivered_pem);

						{
							char *argv[] = { "/usr/bin/openssl", "verify", "-CAfile",
								          ca_path2, leaf_path2, NULL };

							if (run_openssl_argv(argv) != 0) {
								fprintf(stderr,
								        "FAIL: delivered webtls cert does not "
								        "verify against the CA\n");
								ok = 0;
							}
						}
						snprintf(cmd2, sizeof(cmd2), "rm -rf '%s'", scratch_dir2);
						system(cmd2);
					}
				}
			}
		} else {
			fprintf(stderr, "FAIL: webtls never got a pid, skipping delivery checks\n");
			ok = 0;
		}

		/*
		 * Ownership-scoped cleanup: a manually-created cert sharing a
		 * container's exact name must survive that container's
		 * deletion -- proves pki_cert_forget_owner() checks
		 * owner_container, not just the name. Also exercises the
		 * "issuance best-effort skipped on collision" path: creating
		 * container "shadow3" with pki_issue:true must still succeed
		 * (201) even though a cert named "shadow3" already exists.
		 */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pki/certs", "{\"name\":\"shadow3\"}", &r) !=
		        0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST shadow3 (manual), status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"shadow3\",\"image\":\"pkitest\","
		                       "\"cmd\":[\"/bin/daemon_child\"],\"pki_issue\":true}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr,
			        "FAIL: POST shadow3 container (colliding pki_issue), status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		cix_client_request(&client, "DELETE", "/v1/containers/shadow3", NULL, &r);
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pki/certs/shadow3", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr,
			        "FAIL: manually-created 'shadow3' cert did not survive same-named "
			        "container's deletion, status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		cix_client_request(&client, "DELETE", "/v1/containers/webtls", NULL, &r);
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pki/certs/webtls", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr,
			        "FAIL: webtls's auto-issued cert survived container deletion, status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}

	/* 7. restart-survival: the CA and any remaining cert index entries
	 * must persist across a daemon restart (issue one more cert first,
	 * so there's something in the index to check). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pki/certs", "{\"name\":\"persisted.internal\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST persisted.internal, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM (first instance)\n");
		ok = 0;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: restarted daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/pki/ca", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: restarted daemon forgot the CA, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/pki/certs/persisted.internal", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: restarted daemon forgot persisted.internal, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * Part 3 (ADR-0047): a real intermediate CA -- signed BY the root
	 * (not self-signed), leaf issuance transparently switches to it
	 * once bootstrapped, and the resulting 3-tier chain genuinely
	 * verifies with openssl, not just "cert_pem is non-empty." Reuses
	 * this same already-restarted daemon (persistence for THIS state
	 * relies on the exact same persist_atomic_write()/persist_read_file()
	 * primitive the root CA/certs state above already proved survives a
	 * restart -- not re-proven a second time here to keep this test's
	 * own wall-clock cost proportionate).
	 */
	{
		char intermediate_cert_pem[8192] = { 0 };
		char chainleaf_cert_pem[8192] = { 0 };

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pki/intermediate", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: GET intermediate before bootstrap expected 404, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pki/intermediate", "{}", &r) != 0 ||
		    r.status != 201 ||
		    !str_eq(json_str_field(r.json, "subject"), "CN = Cix Intermediate CA") ||
		    json_str_field(r.json, "cert_pem") == NULL) {
			fprintf(stderr, "FAIL: POST /v1/pki/intermediate, status=%d\n", r.status);
			ok = 0;
		} else {
			snprintf(intermediate_cert_pem, sizeof(intermediate_cert_pem), "%s",
			         json_str_field(r.json, "cert_pem"));
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pki/intermediate", "{}", &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: second POST /v1/pki/intermediate expected 409, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		if (intermediate_cert_pem[0] != '\0' && ca_cert_pem[0] != '\0') {
			char scratch_dir[] = "/tmp/cix_test_pki_intermediate_XXXXXX";
			char root_path[160], intermediate_path[160], chainleaf_path[160];

			if (mkdtemp(scratch_dir) == NULL) {
				fprintf(stderr, "FAIL: mkdtemp (intermediate scratch)\n");
				ok = 0;
			} else {
				snprintf(root_path, sizeof(root_path), "%s/root.crt", scratch_dir);
				snprintf(intermediate_path, sizeof(intermediate_path), "%s/intermediate.crt",
				         scratch_dir);
				write_file(root_path, ca_cert_pem);
				write_file(intermediate_path, intermediate_cert_pem);

				/* Real proof #1: the intermediate genuinely chains to
				 * the root -- it was actually signed by it, not just
				 * self-signed with CA:TRUE. */
				{
					char *argv[] = { "/usr/bin/openssl", "verify", "-CAfile", root_path,
						          intermediate_path, NULL };

					if (run_openssl_argv(argv) != 0) {
						fprintf(stderr,
						        "FAIL: intermediate cert does not verify against the root\n");
						ok = 0;
					}
				}

				/* A leaf issued NOW must be signed by the intermediate,
				 * transparently -- pki_cert_create()'s own call
				 * signature/response shape is unchanged. */
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "POST", "/v1/pki/certs",
				                       "{\"name\":\"chainleaf\",\"sans\":[\"chainleaf\"]}",
				                       &r) != 0 ||
				    r.status != 201 || json_str_field(r.json, "cert_pem") == NULL) {
					fprintf(stderr, "FAIL: POST chainleaf, status=%d\n", r.status);
					ok = 0;
				} else {
					snprintf(chainleaf_cert_pem, sizeof(chainleaf_cert_pem), "%s",
					         json_str_field(r.json, "cert_pem"));
				}
				cix_response_free(&r);

				if (chainleaf_cert_pem[0] != '\0') {
					snprintf(chainleaf_path, sizeof(chainleaf_path), "%s/chainleaf.crt",
					         scratch_dir);
					write_file(chainleaf_path, chainleaf_cert_pem);

					/* Real proof #2: chainleaf does NOT verify against
					 * the root alone (it was signed by the
					 * intermediate, not the root)... */
					{
						char *argv[] = { "/usr/bin/openssl", "verify", "-CAfile", root_path,
							          chainleaf_path, NULL };

						if (run_openssl_argv(argv) == 0) {
							fprintf(stderr,
							        "FAIL: chainleaf verified against the root alone -- "
							        "it should only verify with the intermediate completing "
							        "the chain, meaning it was NOT actually signed by the "
							        "intermediate\n");
							ok = 0;
						}
					}
					/* ...but DOES verify once the intermediate is
					 * supplied to complete the chain -- the actual,
					 * end-to-end trust-chain proof this whole ADR
					 * exists to deliver. */
					{
						char *argv[] = { "/usr/bin/openssl", "verify", "-CAfile", root_path,
							          "-untrusted", intermediate_path, chainleaf_path,
							          NULL };

						if (run_openssl_argv(argv) != 0) {
							fprintf(stderr,
							        "FAIL: chainleaf does not verify against root+intermediate\n");
							ok = 0;
						}
					}
				}

				{
					char cmd[224];

					snprintf(cmd, sizeof(cmd), "rm -rf '%s'", scratch_dir);
					system(cmd);
				}
			}
		} else {
			fprintf(stderr, "FAIL: skipping intermediate chain verification, "
			                "cert_pem was never captured\n");
			ok = 0;
		}

		cix_client_request(&client, "DELETE", "/v1/pki/certs/chainleaf", NULL, &r);
		cix_response_free(&r);
	}

	/*
	 * Part 4 (ADR-0046): site config -- real defaults, a real PUT round
	 * trip, and real validation (not just "the daemon didn't crash").
	 */
	{
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/system/site", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "instance_name"), "cix") ||
		    !str_eq(json_str_field(r.json, "domain_suffix"), "internal")) {
			fprintf(stderr, "FAIL: GET /v1/system/site defaults, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/site",
		                       "{\"instance_name\":\"cix1\",\"site_name\":\"lab1\","
		                       "\"domain_suffix\":\"corp.internal\"}",
		                       &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "instance_name"), "cix1") ||
		    !str_eq(json_str_field(r.json, "site_name"), "lab1") ||
		    !str_eq(json_str_field(r.json, "domain_suffix"), "corp.internal")) {
			fprintf(stderr, "FAIL: PUT /v1/system/site, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/system/site", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "instance_name"), "cix1") ||
		    !str_eq(json_str_field(r.json, "site_name"), "lab1")) {
			fprintf(stderr, "FAIL: GET /v1/system/site after PUT did not stick, status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/*
		 * ADR-0050: the site PUT above must have (re)issued this
		 * install's own "host" leaf -- SAN reflects the FQDN just set,
		 * fixed record name, owner "__host" (a reserved sentinel, ADR-0128
		 * -- never a real container name, distinguishing "not owned by
		 * any container" from "owned by a container literally named
		 * host").
		 */
		{
			char first_serial[128] = { 0 };

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pki/certs/host", NULL, &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: GET host cert after site PUT, status=%d\n", r.status);
				ok = 0;
			} else {
				const struct json_value *sans = json_object_get(r.json, "sans");
				int has_fqdn = 0;
				size_t i;

				if (sans != NULL && sans->type == JSON_ARRAY) {
					for (i = 0; i < sans->u.array.count; i++) {
						if (str_eq(json_as_string(sans->u.array.items[i]),
						           "cix1.lab1.corp.internal"))
							has_fqdn = 1;
					}
				}
				if (!has_fqdn) {
					fprintf(stderr,
					        "FAIL: host cert SAN missing cix1.lab1.corp.internal\n");
					ok = 0;
				}
				if (!str_eq(json_str_field(r.json, "owner"), "__host")) {
					fprintf(stderr, "FAIL: host cert should have owner=\"__host\", got %s\n",
					        json_str_field(r.json, "owner") ? json_str_field(r.json, "owner")
					                                          : "(null)");
					ok = 0;
				}
				snprintf(first_serial, sizeof(first_serial), "%s",
				         json_str_field(r.json, "serial"));
			}
			cix_response_free(&r);

			/* Changing instance_name must genuinely reissue "host" --
			 * new SAN, new serial, not left stale. */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "PUT", "/v1/system/site",
			                       "{\"instance_name\":\"cix2\",\"site_name\":\"lab1\","
			                       "\"domain_suffix\":\"corp.internal\"}",
			                       &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: PUT site (rename instance), status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pki/certs/host", NULL, &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: GET host cert after rename, status=%d\n", r.status);
				ok = 0;
			} else {
				const struct json_value *sans = json_object_get(r.json, "sans");
				int has_new_fqdn = 0;
				size_t i;

				if (sans != NULL && sans->type == JSON_ARRAY) {
					for (i = 0; i < sans->u.array.count; i++) {
						if (str_eq(json_as_string(sans->u.array.items[i]),
						           "cix2.lab1.corp.internal"))
							has_new_fqdn = 1;
					}
				}
				if (!has_new_fqdn) {
					fprintf(stderr,
					        "FAIL: host cert SAN did not follow instance_name rename\n");
					ok = 0;
				}
				if (first_serial[0] != '\0' &&
				    str_eq(json_str_field(r.json, "serial"), first_serial)) {
					fprintf(stderr,
					        "FAIL: host cert has the SAME serial after rename -- not "
					        "actually reissued\n");
					ok = 0;
				}
			}
			cix_response_free(&r);

			/* Restore instance_name to what the rest of this test
			 * expects below. */
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "PUT", "/v1/system/site",
			                   "{\"instance_name\":\"cix1\",\"site_name\":\"lab1\","
			                   "\"domain_suffix\":\"corp.internal\"}",
			                   &r);
			cix_response_free(&r);
		}

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/site",
		                       "{\"instance_name\":\"cix1\",\"site_name\":\"lab1\","
		                       "\"domain_suffix\":\"bad/suffix\"}",
		                       &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: PUT invalid domain_suffix expected 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/site",
		                       "{\"instance_name\":\"bad/name\",\"site_name\":\"lab1\","
		                       "\"domain_suffix\":\"internal\"}",
		                       &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: PUT invalid instance_name expected 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/site",
		                       "{\"site_name\":\"lab1\",\"domain_suffix\":\"internal\"}", &r) !=
		        0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: PUT missing instance_name expected 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/*
		 * ADR-0052: server-side default qualification for POST
		 * /v1/pki/certs -- site_name is still "lab1" at this point
		 * (the last successful PUT above). A bare name gets the CN
		 * qualified (and the default SAN, since none is supplied
		 * here); an explicit sans[] entry is left untouched.
		 */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pki/certs", "{\"name\":\"bareleaf\"}", &r) !=
		        0 ||
		    r.status != 201 || !str_eq(json_str_field(r.json, "name"), "bareleaf.lab1.corp.internal")) {
			fprintf(stderr, "FAIL: bare PKI cert name not qualified, got name=%s status=%d\n",
			        json_str_field(r.json, "name") ? json_str_field(r.json, "name") : "(null)",
			        r.status);
			ok = 0;
		} else {
			const struct json_value *sans = json_object_get(r.json, "sans");
			int has_qualified_san = 0;
			size_t i;

			if (sans != NULL && sans->type == JSON_ARRAY) {
				for (i = 0; i < sans->u.array.count; i++) {
					if (str_eq(json_as_string(sans->u.array.items[i]),
					           "bareleaf.lab1.corp.internal"))
						has_qualified_san = 1;
				}
			}
			if (!has_qualified_san) {
				fprintf(stderr, "FAIL: bare PKI cert's default SAN was not qualified\n");
				ok = 0;
			}
		}
		cix_response_free(&r);
		cix_client_request(&client, "DELETE", "/v1/pki/certs/bareleaf.lab1.corp.internal", NULL, &r);
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pki/certs",
		                       "{\"name\":\"explicit.other\",\"sans\":[\"explicit.other\"]}",
		                       &r) != 0 ||
		    r.status != 201 || !str_eq(json_str_field(r.json, "name"), "explicit.other")) {
			fprintf(stderr, "FAIL: dotted PKI cert name was qualified when it shouldn't be\n");
			ok = 0;
		}
		cix_response_free(&r);
		cix_client_request(&client, "DELETE", "/v1/pki/certs/explicit.other", NULL, &r);
		cix_response_free(&r);

		/* ADR-0052: siteconfig_qualify() is gated on site_name being
		 * non-empty -- reset it to "" here (keeping instance_name/
		 * domain_suffix, which Part 5 below still needs) so the bare
		 * PKI cert names Part 5/6 create below ("resettest", ...)
		 * store literally, exactly as their own assertions expect,
		 * rather than getting silently qualified into
		 * "resettest.lab1.corp.internal" the way Part 4's own
		 * successful PUT above would otherwise leave site_name set to
		 * cause. */
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "PUT", "/v1/system/site",
		                   "{\"instance_name\":\"cix1\",\"site_name\":\"\","
		                   "\"domain_suffix\":\"corp.internal\"}",
		                   &r);
		cix_response_free(&r);
	}

	/*
	 * Part 5 (ADR-0049): CA reset -- real reissue, not just "the
	 * response looked plausible." The intermediate is already
	 * bootstrapped (Part 3) and domain_suffix is already "corp.internal"
	 * (Part 4's successful PUT above), so a bare POST /v1/pki/reset with
	 * no body exercises both the domain_suffix-based default naming and
	 * the "only re-create an intermediate if one already existed" rule
	 * in one shot.
	 */
	{
		char old_root_cert_pem[8192] = { 0 };
		char old_leaf_cert_pem[8192] = { 0 };
		char old_leaf_serial[128] = { 0 };
		char new_leaf_serial[128] = { 0 };
		char old_delivered_pem[8192] = { 0 };
		int resetlive_pid = 0;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pki/ca", NULL, &r) == 0 && r.status == 200) {
			snprintf(old_root_cert_pem, sizeof(old_root_cert_pem), "%s",
			         json_str_field(r.json, "cert_pem"));
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pki/certs",
		                       "{\"name\":\"resettest\",\"sans\":[\"resettest\"]}", &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST resettest, status=%d\n", r.status);
			ok = 0;
		} else {
			snprintf(old_leaf_cert_pem, sizeof(old_leaf_cert_pem), "%s",
			         json_str_field(r.json, "cert_pem"));
			snprintf(old_leaf_serial, sizeof(old_leaf_serial), "%s",
			         json_str_field(r.json, "serial"));
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"resetlive\",\"image\":\"pkitest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"60\"],\"pki_issue\":true}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST resetlive, status=%d\n", r.status);
			ok = 0;
		} else {
			resetlive_pid = (int)json_as_number(json_object_get(r.json, "pid"));
		}
		cix_response_free(&r);

		if (resetlive_pid > 0) {
			char proc_path[160];
			FILE *f;

			snprintf(proc_path, sizeof(proc_path), "/proc/%d/root/etc/cix-tls/tls.crt",
			         resetlive_pid);
			f = fopen(proc_path, "r");
			if (f != NULL) {
				size_t n = fread(old_delivered_pem, 1, sizeof(old_delivered_pem) - 1, f);

				old_delivered_pem[n] = '\0';
				fclose(f);
			}
		}
		if (old_delivered_pem[0] == '\0') {
			fprintf(stderr, "FAIL: could not read resetlive's pre-reset delivered cert\n");
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pki/reset", "{}", &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: POST /v1/pki/reset, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *root_obj = json_object_get(r.json, "root");
			const struct json_value *intermediate_obj = json_object_get(r.json, "intermediate");
			const struct json_value *reissued = json_object_get(r.json, "reissued");
			const char *root_subject = root_obj != NULL ? json_str_field(root_obj, "subject") :
			                                               NULL;
			int found_resettest = 0, found_resetlive = 0;
			size_t i;

			if (root_subject == NULL || strstr(root_subject, "corp.internal") == NULL) {
				fprintf(stderr, "FAIL: reset root subject missing domain_suffix, got %s\n",
				        root_subject != NULL ? root_subject : "(null)");
				ok = 0;
			}
			if (intermediate_obj == NULL || intermediate_obj->type != JSON_OBJECT ||
			    json_str_field(intermediate_obj, "subject") == NULL ||
			    strstr(json_str_field(intermediate_obj, "subject"), "corp.internal") == NULL) {
				fprintf(stderr, "FAIL: reset intermediate missing/wrong -- an intermediate "
				                "existed before reset, so one must exist after\n");
				ok = 0;
			}
			if (reissued == NULL || reissued->type != JSON_ARRAY) {
				fprintf(stderr, "FAIL: reset response missing reissued array\n");
				ok = 0;
			} else {
				for (i = 0; i < reissued->u.array.count; i++) {
					const struct json_value *item = reissued->u.array.items[i];
					const char *name = json_str_field(item, "name");

					if (str_eq(name, "resettest")) {
						found_resettest = 1;
						snprintf(new_leaf_serial, sizeof(new_leaf_serial), "%s",
						         json_str_field(item, "serial"));
						if (json_str_field(item, "cert_pem") == NULL ||
						    json_str_field(item, "key_pem") == NULL) {
							fprintf(stderr,
							        "FAIL: reissued resettest missing cert_pem/key_pem\n");
							ok = 0;
						}
						if (str_eq(new_leaf_serial, old_leaf_serial)) {
							fprintf(stderr,
							        "FAIL: reissued resettest has the SAME serial as "
							        "before -- not actually reissued\n");
							ok = 0;
						}
					}
					if (str_eq(name, "resetlive"))
						found_resetlive = 1;
				}
			}
			if (!found_resettest) {
				fprintf(stderr, "FAIL: resettest not present in reissued array\n");
				ok = 0;
			}
			if (!found_resetlive) {
				fprintf(stderr, "FAIL: resetlive (auto-issued via pki_issue) not present "
				                "in reissued array\n");
				ok = 0;
			}

			/* Real proof: the OLD leaf cert no longer verifies against
			 * the NEW root -- its actual signer is gone, not just
			 * relabeled. */
			if (old_root_cert_pem[0] != '\0' && old_leaf_cert_pem[0] != '\0') {
				char scratch_dir[] = "/tmp/cix_test_pki_reset_XXXXXX";

				if (mkdtemp(scratch_dir) != NULL) {
					char new_root_path[160], old_leaf_path[160];
					const char *new_root_pem =
					    root_obj != NULL ? json_str_field(root_obj, "cert_pem") : NULL;

					snprintf(new_root_path, sizeof(new_root_path), "%s/new_root.crt",
					         scratch_dir);
					snprintf(old_leaf_path, sizeof(old_leaf_path), "%s/old_leaf.crt",
					         scratch_dir);
					if (new_root_pem != NULL) {
						char *argv[] = { "/usr/bin/openssl", "verify", "-CAfile",
							          new_root_path, old_leaf_path, NULL };

						write_file(new_root_path, new_root_pem);
						write_file(old_leaf_path, old_leaf_cert_pem);
						if (run_openssl_argv(argv) == 0) {
							fprintf(stderr,
							        "FAIL: old resettest cert still verifies against "
							        "the NEW root -- reset did not actually replace "
							        "the signing chain\n");
							ok = 0;
						}
					}
					{
						char cmd[224];

						snprintf(cmd, sizeof(cmd), "rm -rf '%s'", scratch_dir);
						system(cmd);
					}
				}
			}
		}
		cix_response_free(&r);

		/* GET must reflect the new serial, not a stale index entry. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pki/certs/resettest", NULL, &r) != 0 ||
		    r.status != 200 || new_leaf_serial[0] == '\0' ||
		    !str_eq(json_str_field(r.json, "serial"), new_leaf_serial)) {
			fprintf(stderr, "FAIL: GET resettest after reset does not reflect the new serial\n");
			ok = 0;
		}
		cix_response_free(&r);

		/* Redelivery proof: resetlive's own delivered tls.crt on disk
		 * must have actually changed -- not left stale after the leaf
		 * that owns it was reissued. */
		if (resetlive_pid > 0 && old_delivered_pem[0] != '\0') {
			char proc_path[160];
			char new_delivered_pem[8192] = { 0 };
			FILE *f;

			snprintf(proc_path, sizeof(proc_path), "/proc/%d/root/etc/cix-tls/tls.crt",
			         resetlive_pid);
			f = fopen(proc_path, "r");
			if (f != NULL) {
				size_t n = fread(new_delivered_pem, 1, sizeof(new_delivered_pem) - 1, f);

				new_delivered_pem[n] = '\0';
				fclose(f);
			}
			if (new_delivered_pem[0] == '\0' ||
			    strcmp(new_delivered_pem, old_delivered_pem) == 0) {
				fprintf(stderr,
				        "FAIL: resetlive's delivered tls.crt was not redelivered after reset\n");
				ok = 0;
			}
		}

		cix_client_request(&client, "DELETE", "/v1/containers/resetlive", NULL, &r);
		cix_response_free(&r);
		cix_client_request(&client, "DELETE", "/v1/pki/certs/resettest", NULL, &r);
		cix_response_free(&r);
	}

	/*
	 * Part 6 (ADR-0051): CA trust chain staged into a freshly created
	 * image -- read straight off disk (staging is a pkg_seed_image_
	 * baseline() side effect of image creation, not its own API), and
	 * proven as a real, working trust anchor via openssl verify against
	 * the currently-live "host" leaf, not just "the file exists."
	 */
	{
		char bundle_path[PATH_MAX];
		struct stat st;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/images", "{\"name\":\"imgtrust\"}", &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST /v1/images imgtrust, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		{
			char image_dir[PATH_MAX], version[128];

			snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/imgtrust", g_data_dir);
			if (test_image_fixture_read_current_version(image_dir, version, sizeof(version)) != 0)
				version[0] = '\0';
			snprintf(bundle_path, sizeof(bundle_path),
			         "%s/%s/rootfs/etc/ssl/certs/cix-ca-bundle.pem", image_dir, version);
		}
		if (stat(bundle_path, &st) != 0 || st.st_size == 0) {
			fprintf(stderr, "FAIL: cix-ca-bundle.pem missing or empty at %s\n", bundle_path);
			ok = 0;
		} else {
			char host_cert_pem[8192] = { 0 };

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pki/certs/host", NULL, &r) == 0 &&
			    r.status == 200) {
				snprintf(host_cert_pem, sizeof(host_cert_pem), "%s",
				         json_str_field(r.json, "cert_pem"));
			}
			cix_response_free(&r);

			if (host_cert_pem[0] == '\0') {
				fprintf(stderr,
				        "FAIL: could not fetch host cert_pem for trust bundle check\n");
				ok = 0;
			} else {
				char scratch_dir[] = "/tmp/cix_test_pki_trust_XXXXXX";

				if (mkdtemp(scratch_dir) == NULL) {
					fprintf(stderr, "FAIL: mkdtemp (trust bundle scratch)\n");
					ok = 0;
				} else {
					char leaf_path[192];
					char *argv[] = { "/usr/bin/openssl", "verify", "-CAfile", bundle_path,
						          leaf_path, NULL };
					char cmd[224];

					snprintf(leaf_path, sizeof(leaf_path), "%s/host.crt", scratch_dir);
					write_file(leaf_path, host_cert_pem);
					if (run_openssl_argv(argv) != 0) {
						fprintf(stderr,
						        "FAIL: host cert does not verify against the "
						        "staged image trust bundle\n");
						ok = 0;
					}
					snprintf(cmd, sizeof(cmd), "rm -rf '%s'", scratch_dir);
					system(cmd);
				}
			}
		}

		cix_client_request(&client, "DELETE", "/v1/images/imgtrust", NULL, &r);
		cix_response_free(&r);
	}

	/* cleanup */
	cix_client_request(&client, "DELETE", "/v1/pki/certs/persisted.internal", NULL, &r);
	cix_response_free(&r);
	cix_client_request(&client, "DELETE", "/v1/pki/certs/shadow3", NULL, &r);
	cix_response_free(&r);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM (second instance)\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "PKI RESULT: PASS\n" : "PKI RESULT: FAIL\n");
	return ok ? 0 : 1;
}
