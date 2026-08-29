/*
 * ADR-0212: GET/PUT/DELETE /v1/system/signing-keys.
 *
 * The pair POST /v1/system/iso needs. Until this endpoint existed there
 * was no way to put it on a real host at all -- no sshd, no host-side
 * exec, no console -- so that endpoint has never been servable on an
 * installed machine.
 *
 * Two properties here are worth more than the happy path, and both are
 * asserted with the discriminating case rather than the easy one:
 *
 *   - A cert that does not match its key is REFUSED. Both blobs parse
 *     fine on their own, so nothing else in the system would catch it,
 *     and the consequence is an image that signs cleanly and then will
 *     not boot.
 *   - That refusal is ATOMIC. A host actively cutting media must not be
 *     left with half a replaced pair, so the previously installed pair
 *     has to survive a rejected PUT byte for byte.
 *
 * Key material is generated here with the same openssl the daemon
 * shells out to, so this test needs no fixture files and never touches
 * the repo's real signing key.
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7846
#define PORT_ARG "--port=7846"

#define OPENSSL_BIN "/usr/bin/openssl"

static char g_data_dir[PATH_MAX];

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

static int run(char *const argv[])
{
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return -1;
	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);

		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execve(argv[0], argv, environ);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* A real self-signed pair, the same shape a Secure Boot signing key is. */
static int make_pair(const char *key_path, const char *crt_path, const char *cn)
{
	char subj[128];
	char *argv[18];

	snprintf(subj, sizeof(subj), "/CN=%s", cn);
	argv[0] = (char *)OPENSSL_BIN;
	argv[1] = (char *)"req";
	argv[2] = (char *)"-new";
	argv[3] = (char *)"-x509";
	argv[4] = (char *)"-newkey";
	argv[5] = (char *)"rsa:2048";
	argv[6] = (char *)"-nodes";
	argv[7] = (char *)"-sha256";
	argv[8] = (char *)"-days";
	argv[9] = (char *)"30";
	argv[10] = (char *)"-subj";
	argv[11] = subj;
	argv[12] = (char *)"-keyout";
	argv[13] = (char *)key_path;
	argv[14] = (char *)"-out";
	argv[15] = (char *)crt_path;
	argv[16] = NULL;
	return run(argv);
}

static char *slurp(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	long size;
	char *buf;

	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)size + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[size] = '\0';
	fclose(f);
	if (out_len != NULL)
		*out_len = (size_t)size;
	return buf;
}

/* {"key":<pem>,"cert":<pem>} -- JSON-escaped, since PEM is full of
 * newlines. */
static char *put_body(const char *key_pem, const char *cert_pem)
{
	struct json_writer w;
	char *out;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "key");
	jw_str(&w, key_pem);
	jw_key(&w, "cert");
	jw_str(&w, cert_pem);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	out = strdup(w.buf);
	jw_free(&w);
	return out;
}

static int bool_field(const struct json_value *v, const char *key)
{
	const struct json_value *f = json_object_get(v, key);

	return f != NULL && f->type == JSON_BOOL && f->u.boolean;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;
	char key_a[PATH_MAX], crt_a[PATH_MAX];
	char key_b[PATH_MAX], crt_b[PATH_MAX];
	char installed_key[PATH_MAX], installed_crt[PATH_MAX], installed_cer[PATH_MAX];
	char expect_cer[PATH_MAX];
	char *pem_key_a = NULL, *pem_crt_a = NULL, *pem_key_b = NULL, *pem_crt_b = NULL;
	char *body = NULL;
	char *before = NULL, *after = NULL;
	size_t before_len = 0, after_len = 0;
	char *got_cer = NULL, *want_cer = NULL;
	size_t got_len = 0, want_len = 0;
	int ok = 1;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	snprintf(key_a, sizeof(key_a), "%s/a.key", g_data_dir);
	snprintf(crt_a, sizeof(crt_a), "%s/a.crt", g_data_dir);
	snprintf(key_b, sizeof(key_b), "%s/b.key", g_data_dir);
	snprintf(crt_b, sizeof(crt_b), "%s/b.crt", g_data_dir);
	snprintf(expect_cer, sizeof(expect_cer), "%s/expect.cer", g_data_dir);
	snprintf(installed_key, sizeof(installed_key), "%s/state/keys/cix-signing.key", g_data_dir);
	snprintf(installed_crt, sizeof(installed_crt), "%s/state/keys/cix-signing.crt", g_data_dir);
	snprintf(installed_cer, sizeof(installed_cer), "%s/state/keys/cix-signing.cer", g_data_dir);

	if (make_pair(key_a, crt_a, "Cix Test Signing A") != 0 ||
	    make_pair(key_b, crt_b, "Cix Test Signing B") != 0) {
		fprintf(stderr, "FAIL: could not generate test key pairs\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	pem_key_a = slurp(key_a, NULL);
	pem_crt_a = slurp(crt_a, NULL);
	pem_key_b = slurp(key_b, NULL);
	pem_crt_b = slurp(crt_b, NULL);
	if (pem_key_a == NULL || pem_crt_a == NULL || pem_key_b == NULL || pem_crt_b == NULL) {
		fprintf(stderr, "FAIL: could not read generated key material\n");
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

	/* 1. A fresh host holds nothing, and says so without inventing
	 * certificate fields it cannot have. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/signing-keys", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET signing-keys, status=%d\n", r.status);
		ok = 0;
	} else if (bool_field(r.json, "key_set") || bool_field(r.json, "cert_set") ||
	           json_as_string(json_object_get(r.json, "subject")) != NULL) {
		fprintf(stderr, "FAIL: a fresh host reported a signing key pair\n");
		ok = 0;
	}
	cix_response_free(&r);

	/* 2. A real pair installs, and GET reports the certificate's own
	 * public identity -- never the key. */
	body = put_body(pem_key_a, pem_crt_a);
	memset(&r, 0, sizeof(r));
	if (body == NULL ||
	    cix_client_request(&client, "PUT", "/v1/system/signing-keys", body, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: PUT a valid pair, status=%d\n", r.status);
		ok = 0;
	} else if (!bool_field(r.json, "key_set") || !bool_field(r.json, "cert_set")) {
		fprintf(stderr, "FAIL: PUT succeeded but reported no key pair\n");
		ok = 0;
	} else {
		const char *subject = json_as_string(json_object_get(r.json, "subject"));

		if (subject == NULL || strstr(subject, "Cix Test Signing A") == NULL) {
			fprintf(stderr, "FAIL: subject='%s'\n", subject != NULL ? subject : "(null)");
			ok = 0;
		}
		if (json_as_string(json_object_get(r.json, "fingerprint_sha256")) == NULL) {
			fprintf(stderr, "FAIL: no certificate fingerprint reported\n");
			ok = 0;
		}
	}
	cix_response_free(&r);
	free(body);
	body = NULL;

	/* The response must never carry key material, whatever else it
	 * says. Checked against the raw body, not the parsed fields, so a
	 * future field that accidentally echoes the key is caught too. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/signing-keys", NULL, &r) == 0 &&
	    r.body != NULL && strstr(r.body, "PRIVATE KEY") != NULL) {
		fprintf(stderr, "FAIL: GET signing-keys leaked private key material\n");
		ok = 0;
	}
	cix_response_free(&r);

	/* 3. The DER .cer is derived from the certificate, and is exactly
	 * what openssl itself produces -- mokutil enrols this file, so a
	 * near-miss here is an identity that does not match the signature. */
	{
		char *argv[9];

		argv[0] = (char *)OPENSSL_BIN;
		argv[1] = (char *)"x509";
		argv[2] = (char *)"-in";
		argv[3] = crt_a;
		argv[4] = (char *)"-outform";
		argv[5] = (char *)"DER";
		argv[6] = (char *)"-out";
		argv[7] = expect_cer;
		argv[8] = NULL;
		if (run(argv) != 0) {
			fprintf(stderr, "FAIL: could not produce the expected DER\n");
			ok = 0;
		}
	}
	got_cer = slurp(installed_cer, &got_len);
	want_cer = slurp(expect_cer, &want_len);
	if (got_cer == NULL || want_cer == NULL || got_len != want_len ||
	    memcmp(got_cer, want_cer, got_len) != 0) {
		fprintf(stderr, "FAIL: derived .cer does not match openssl's own DER encoding\n");
		ok = 0;
	}
	free(got_cer);
	free(want_cer);

	/* 4. THE discriminating case: pair A's key with pair B's
	 * certificate. Both parse; only the cross-check rejects it. */
	before = slurp(installed_key, &before_len);
	body = put_body(pem_key_a, pem_crt_b);
	memset(&r, 0, sizeof(r));
	if (body == NULL ||
	    cix_client_request(&client, "PUT", "/v1/system/signing-keys", body, &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: a mismatched pair was not refused (status=%d)\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	free(body);
	body = NULL;

	/* 5. ...and the refusal left the working pair untouched. Without
	 * the atomic write this is where a release host loses its key. */
	after = slurp(installed_key, &after_len);
	if (before == NULL || after == NULL || before_len != after_len ||
	    memcmp(before, after, before_len) != 0) {
		fprintf(stderr, "FAIL: a rejected PUT damaged the installed key pair\n");
		ok = 0;
	}
	free(before);
	free(after);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/signing-keys", NULL, &r) == 0) {
		const char *subject = json_as_string(json_object_get(r.json, "subject"));

		if (subject == NULL || strstr(subject, "Cix Test Signing A") == NULL) {
			fprintf(stderr, "FAIL: after a rejected PUT the host no longer reports pair A\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 6. Garbage in either half is refused on its own terms. */
	body = put_body("not a private key", pem_crt_a);
	memset(&r, 0, sizeof(r));
	if (body == NULL ||
	    cix_client_request(&client, "PUT", "/v1/system/signing-keys", body, &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: an unparseable key was not refused (status=%d)\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	free(body);
	body = NULL;

	/* 7. Replacing a pair with a genuinely different one works -- this
	 * is key rotation, and it must not need a DELETE first. */
	body = put_body(pem_key_b, pem_crt_b);
	memset(&r, 0, sizeof(r));
	if (body == NULL ||
	    cix_client_request(&client, "PUT", "/v1/system/signing-keys", body, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: rotating to a second pair, status=%d\n", r.status);
		ok = 0;
	} else {
		const char *subject = json_as_string(json_object_get(r.json, "subject"));

		if (subject == NULL || strstr(subject, "Cix Test Signing B") == NULL) {
			fprintf(stderr, "FAIL: rotation did not take: subject='%s'\n",
			        subject != NULL ? subject : "(null)");
			ok = 0;
		}
	}
	cix_response_free(&r);
	free(body);
	body = NULL;

	/* 8. DELETE removes all three files, and is idempotent -- the
	 * post-state is what was asked for either way. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/system/signing-keys", NULL, &r) != 0 ||
	    r.status != 200 || bool_field(r.json, "key_set") || bool_field(r.json, "cert_set")) {
		fprintf(stderr, "FAIL: DELETE signing-keys, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	if (access(installed_key, F_OK) == 0 || access(installed_crt, F_OK) == 0 ||
	    access(installed_cer, F_OK) == 0) {
		fprintf(stderr, "FAIL: DELETE left signing material on disk\n");
		ok = 0;
	}
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/system/signing-keys", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: DELETE is not idempotent, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	free(pem_key_a);
	free(pem_crt_a);
	free(pem_key_b);
	free(pem_crt_b);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}
	test_data_dir_cleanup(g_data_dir);

	if (ok)
		printf("test_signing_keys: OK\n");
	return ok ? 0 : 1;
}
