/*
 * ADR-0136: proves the HTTPS listener sends the real, complete
 * leaf+intermediate chain a client trusting only the root CA needs to
 * validate it -- not just that a leaf cert exists and is individually
 * well-formed (test_pki.c's own job). Found live on 192.168.15.95: the
 * daemon's own SSL_CTX only ever loaded the "host" leaf certificate
 * (SSL_CTX_use_certificate_file() loads exactly one certificate, never
 * a chain from the same file) -- once an intermediate CA existed, a
 * real browser trusting only the root could never complete a
 * handshake, no matter how correctly the root itself was trusted.
 *
 * Verification here shells out to the system's real, unmodified
 * openssl binary for both the live TLS handshake (s_client) and the
 * chain validation (verify) -- the same "real cryptographic
 * verification, not a string match" precedent test_pki.c's own
 * run_openssl_argv() already established, since this is exactly the
 * kind of correctness no JSON-shaped assertion could actually prove.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7664
#define PORT_ARG "--port=7664"
#define HTTPS_PORT 8443
#define STR(x) #x
#define TOSTR(x) STR(x)

static char g_data_dir[PATH_MAX];
static char g_scratch_dir[PATH_MAX];

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

/* Runs `openssl <args...>` (NULL-terminated argv after argv[0]), same
 * fork/execve precedent test_pki.c's own run_openssl_argv() already
 * established. Returns 0 on a clean exit, -1 otherwise. */
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

/* `openssl s_client -showcerts -connect 127.0.0.1:HTTPS_PORT </dev/null`,
 * capturing stdout -- the only way to see the real, live chain the
 * daemon's own TLS listener actually sends over the wire, as opposed
 * to what's merely stored on disk (this project's own build has no
 * OpenSSL client bindings available to a C test more directly). */
static int capture_s_client_output(const char *out_path)
{
	pid_t pid;
	int status;
	int devnull;
	int outfd;

	devnull = open("/dev/null", O_RDONLY);
	outfd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (devnull < 0 || outfd < 0) {
		if (devnull >= 0)
			close(devnull);
		if (outfd >= 0)
			close(outfd);
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		close(devnull);
		close(outfd);
		return -1;
	}
	if (pid == 0) {
		char *argv[] = { "/usr/bin/openssl", "s_client", "-showcerts", "-connect",
			          "127.0.0.1:" TOSTR(HTTPS_PORT), NULL };

		dup2(devnull, STDIN_FILENO);
		dup2(outfd, STDOUT_FILENO);
		dup2(outfd, STDERR_FILENO);
		execve("/usr/bin/openssl", argv, environ);
		perror("execve openssl s_client");
		_exit(127);
	}
	close(devnull);
	close(outfd);
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	/* s_client's own exit status isn't a reliable success signal here
	 * (it reflects the post-handshake session, not "did I capture a
	 * chain") -- the caller inspects the captured file's own content
	 * instead, the same way the real chain-count check below does. */
	return 0;
}

static int count_certificates_in_file(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[512];
	int count = 0;

	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strstr(line, "BEGIN CERTIFICATE") != NULL)
			count++;
	}
	fclose(f);
	return count;
}

int main(void)
{
	pid_t daemon_pid;
	struct thinc_client client;
	int ok = 1;
	struct thinc_response r;
	char root_pem_path[PATH_MAX];
	char chain_path[PATH_MAX];

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	if (test_data_dir_create(g_scratch_dir, sizeof(g_scratch_dir)) != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	snprintf(root_pem_path, sizeof(root_pem_path), "%s/root.pem", g_scratch_dir);
	snprintf(chain_path, sizeof(chain_path), "%s/chain.pem", g_scratch_dir);

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		test_data_dir_cleanup(g_scratch_dir);
		return 1;
	}

	thinc_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		test_data_dir_cleanup(g_scratch_dir);
		return 1;
	}

	/* 1. Root, then intermediate, then HTTPS on -- in that order, so
	 * create_tls_ctx() runs (at daemon-config PUT time) with the
	 * intermediate already bootstrapped, exactly the sequence a real
	 * install follows and the sequence that exposed this bug. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/pki/ca", "{}", &r) != 0 || r.status != 201) {
		fprintf(stderr, "FAIL: POST /v1/pki/ca, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "GET", "/v1/pki/ca", NULL, &r) != 0 || r.status != 200)) {
		fprintf(stderr, "FAIL: GET /v1/pki/ca, status=%d\n", r.status);
		ok = 0;
	}
	if (ok) {
		FILE *f = fopen(root_pem_path, "w");
		const char *cert_pem = json_as_string(json_object_get(r.json, "cert_pem"));

		if (f == NULL || cert_pem == NULL || fputs(cert_pem, f) < 0) {
			fprintf(stderr, "FAIL: could not save root CA cert_pem to disk\n");
			ok = 0;
		}
		if (f != NULL)
			fclose(f);
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "POST", "/v1/pki/intermediate", "{}", &r) != 0 ||
	           r.status != 201)) {
		fprintf(stderr, "FAIL: POST /v1/pki/intermediate, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/*
	 * https_port explicit, not left to whatever the daemon's own
	 * default happens to be right now -- ADR/task #860 already changed
	 * that default once (8443 -> 443), which silently broke this
	 * test's own hardcoded HTTPS_PORT client-side probe until this fix;
	 * owning the port explicitly here means a future default change
	 * can't do that again.
	 */
	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "PUT", "/v1/system/daemon-config",
	                              "{\"https_enabled\":true,\"https_port\":" TOSTR(
	                                      HTTPS_PORT) "}",
	                              &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: PUT https_enabled=true, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* Give the newly-added HTTPS listener a moment to actually be
	 * ready to accept -- daemon-config's own PUT response returns as
	 * soon as the rebind succeeds, but a real TLS handshake attempt
	 * moments later is still worth a short, bounded retry loop rather
	 * than assuming zero latency. */
	usleep(300000);

	/* 2. The real, live chain the listener actually sends must be 2
	 * certificates (leaf + intermediate) -- the actual bug this test
	 * exists to catch: SSL_CTX_use_certificate_file() alone only ever
	 * sends 1, regardless of what's true on disk or what the API
	 * separately reports. */
	if (ok && capture_s_client_output(chain_path) != 0) {
		fprintf(stderr, "FAIL: could not run openssl s_client\n");
		ok = 0;
	}
	if (ok) {
		int cert_count = count_certificates_in_file(chain_path);

		if (cert_count != 2) {
			fprintf(stderr, "FAIL: expected 2 certificates in the live chain (leaf+intermediate), got %d\n",
			        cert_count);
			ok = 0;
		}
	}

	/* 3. The real, definitive proof: a client trusting only the root
	 * CA (never told about the intermediate directly) must be able to
	 * validate the live chain end to end -- openssl verify, decoupled
	 * from any hostname/SAN matching concern (a separate, unrelated
	 * question this test isn't about). */
	if (ok) {
		char *argv[] = { "/usr/bin/openssl", "verify", "-CAfile", root_pem_path, "-untrusted",
			          chain_path, chain_path, NULL };

		if (run_openssl_argv(argv) != 0) {
			fprintf(stderr, "FAIL: openssl verify rejected the live chain against the trusted root\n");
			ok = 0;
		}
	}

	if (ok)
		printf("HTTPS CHAIN RESULT: PASS\n");
	else
		printf("HTTPS CHAIN RESULT: FAIL\n");

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);
	test_data_dir_cleanup(g_scratch_dir);
	return ok ? 0 : 1;
}
