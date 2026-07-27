/*
 * Phase 9 part 1 end-to-end test: proves the PKI resource family
 * (POST/GET /v1/pki/ca, POST/GET/DELETE /v1/pki/certs -- daemon/src/pki.c)
 * over real HTTP, with real cryptographic verification as the actual
 * payoff -- not a config-file or string check. Actual crypto (keypair
 * generation, CSR signing) is done by the daemon shelling out to the
 * system's real, unmodified openssl binary (ADR-0007: hand-rolled
 * applies to this project's own platform components, not real
 * software it invokes).
 */
#include "httpclient.h"
#include "json.h"

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
#define PKI_STATE_DIR "/var/lib/kanxeo/pki"

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
	char *dargv[3];

	dargv[0] = "build/kanxeod";
	dargv[1] = PORT_ARG;
	dargv[2] = NULL;

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

/* Removes any pre-existing CA state so this test is deterministic and
 * rerunnable regardless of what a previous run left behind -- there
 * is no CA-delete endpoint (a deliberate v1 boundary: no CA
 * regeneration), so this is the only way to guarantee a clean slate
 * for the "before bootstrap" assertions on every run. */
static void reset_pki_state_dir(void)
{
	char cmd[256];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", PKI_STATE_DIR);
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
	struct kx_client client;
	int ok = 1;
	struct kx_response r;
	char ca_cert_pem[8192] = { 0 };
	char leaf_cert_pem[8192] = { 0 };

	reset_pki_state_dir();

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;

	kx_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	/* 1. before bootstrap: GET ca -> 404, POST certs -> 400 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/pki/ca", NULL, &r) != 0 || r.status != 404) {
		fprintf(stderr, "FAIL: GET /v1/pki/ca before bootstrap expected 404, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pki/certs", "{\"name\":\"svc\"}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: POST /v1/pki/certs before bootstrap expected 400, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2. bootstrap the CA; confirm subject/serial/dates populated and
	 * cert_pem is real, parseable PEM (round-tripped through openssl
	 * itself, not a string check); a second bootstrap is 409. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pki/ca", "{\"common_name\":\"Test Root CA\"}",
	                       &r) != 0 ||
	    r.status != 201 || !str_eq(json_str_field(r.json, "subject"), "CN = Test Root CA") ||
	    json_str_field(r.json, "serial") == NULL || json_str_field(r.json, "not_after") == NULL ||
	    json_str_field(r.json, "cert_pem") == NULL) {
		fprintf(stderr, "FAIL: POST /v1/pki/ca, status=%d\n", r.status);
		ok = 0;
	} else {
		snprintf(ca_cert_pem, sizeof(ca_cert_pem), "%s", json_str_field(r.json, "cert_pem"));
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pki/ca", "{}", &r) != 0 || r.status != 409) {
		fprintf(stderr, "FAIL: second POST /v1/pki/ca expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	if (ca_cert_pem[0] != '\0') {
		char scratch_dir[] = "/tmp/kanxeo_test_pki_XXXXXX";
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
			if (kx_client_request(&client, "POST", "/v1/pki/certs",
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
			kx_response_free(&r);

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
	if (kx_client_request(&client, "POST", "/v1/pki/certs", "{\"name\":\"svc.internal\"}", &r) !=
	        0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate cert name expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pki/certs", "{\"name\":\"bad..name\"}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: invalid cert name expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 5. list is metadata-only; single-get has cert_pem but never key_pem */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/pki/certs", NULL, &r) != 0 || r.status != 200) {
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
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/pki/certs/svc.internal", NULL, &r) != 0 ||
	    r.status != 200 || json_str_field(r.json, "cert_pem") == NULL) {
		fprintf(stderr, "FAIL: GET /v1/pki/certs/svc.internal, status=%d\n", r.status);
		ok = 0;
	} else if (json_has_field(r.json, "key_pem")) {
		fprintf(stderr, "FAIL: GET single cert leaked key_pem -- the 'shown once' guarantee is broken\n");
		ok = 0;
	}
	kx_response_free(&r);

	/* 6. delete removes both the index entry and the on-disk files */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/pki/certs/svc.internal", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE svc.internal expected 204, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/pki/certs/svc.internal", NULL, &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: GET svc.internal after delete expected 404, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	{
		struct stat st;

		if (stat(PKI_STATE_DIR "/certs/svc.internal.key", &st) == 0 ||
		    stat(PKI_STATE_DIR "/certs/svc.internal.crt", &st) == 0) {
			fprintf(stderr, "FAIL: svc.internal's key/cert files still exist on disk after delete\n");
			ok = 0;
		}
	}

	/* 7. restart-survival: the CA and any remaining cert index entries
	 * must persist across a daemon restart (issue one more cert first,
	 * so there's something in the index to check). */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pki/certs", "{\"name\":\"persisted.internal\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST persisted.internal, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

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
	if (kx_client_request(&client, "GET", "/v1/pki/ca", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: restarted daemon forgot the CA, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/pki/certs/persisted.internal", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: restarted daemon forgot persisted.internal, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* cleanup */
	kx_client_request(&client, "DELETE", "/v1/pki/certs/persisted.internal", NULL, &r);
	kx_response_free(&r);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM (second instance)\n");
		ok = 0;
	}

	printf(ok ? "PKI RESULT: PASS\n" : "PKI RESULT: FAIL\n");
	return ok ? 0 : 1;
}
