/*
 * test_http_put -- an incomplete PUT to the test fixture's artifact
 * server must not report success, and must not destroy the file
 * already at that path.
 *
 * This is cix#533's invariant, written as a test because the bug it
 * encodes was invisible exactly where it should have been caught.
 *
 * WHAT HAPPENED. http_serve_put() opened the destination with
 * `fopen(path, "wb")` -- truncating before reading any body -- read
 * until the first `read()` returning <= 0, and then answered "201
 * Created" regardless of how much had arrived. So a short read left a
 * truncated file AND called it success. Worse, because the truncation
 * happens at open, a second PUT of the same name destroyed a good
 * file already there; the hbpush scenario pushes the same artifact
 * name twice, so one short read on the second turned a correct
 * upload into an empty file.
 *
 * It surfaced as `FAIL: the published hostbuild artifact is a valid
 * gzip`, intermittently, inside a FLOOR_SELFTEST -- so it failed
 * releases at random and looked like a regression in whatever was in
 * flight. It cost a full investigation of an unrelated change before
 * an unchanged re-run passed.
 *
 * WHY A SEPARATE TEST. The failure needed a short read to reproduce,
 * which is a timing accident inside a multi-minute scenario. Here it
 * is deliberate: declare a Content-Length larger than the body, close
 * the connection, and assert on both the status and the bytes on
 * disk. No daemon, no containers, no timing -- it either holds or it
 * does not.
 */
#include "test_image_fixture.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

static void check(int ok, const char *what)
{
	printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		g_failures++;
}

/* Connects to the fixture server, sends a PUT whose Content-Length is
 * `declared` while actually writing `send_len` bytes of `body`, then
 * closes the write side. Returns the HTTP status, or -1. */
static int put_raw(int port, const char *name, const char *body, size_t send_len, long declared)
{
	struct sockaddr_in a;
	char head[512];
	char resp[512];
	int fd, status = -1;
	ssize_t n;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		close(fd);
		return -1;
	}
	snprintf(head, sizeof(head),
	         "PUT /%s HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer t\r\n"
	         "X-Cix-Sha256: deadbeef\r\nContent-Length: %ld\r\n\r\n",
	         name, declared);
	if (write(fd, head, strlen(head)) < 0) {
		close(fd);
		return -1;
	}
	if (send_len > 0 && write(fd, body, send_len) < 0) {
		close(fd);
		return -1;
	}
	/* Half-close so the server's body read sees EOF rather than
	 * blocking for bytes that will never come. */
	shutdown(fd, SHUT_WR);
	n = read(fd, resp, sizeof(resp) - 1);
	if (n > 0) {
		resp[n] = '\0';
		if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1)
			status = -1;
	}
	close(fd);
	return status;
}

/* The file's exact contents, or NULL. Caller frees. */
static char *slurp(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long len;

	*out_len = 0;
	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)len + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	*out_len = fread(buf, 1, (size_t)len, f);
	buf[*out_len] = '\0';
	fclose(f);
	return buf;
}

int main(void)
{
	char root[] = "/tmp/cix_test_httpput_XXXXXX";
	char target[512];
	char *got;
	size_t got_len;
	const char *original = "ORIGINAL-GOOD-BYTES";
	const char *replacement = "REPLACEMENT-BYTES";
	pid_t pid = 0;
	int port = 0;
	int status;
	FILE *f;

	if (mkdtemp(root) == NULL) {
		printf("HTTP PUT TEST: FAIL (mkdtemp)\n");
		return 1;
	}
	snprintf(target, sizeof(target), "%s/thing.tar.gz", root);

	/* A previous, complete upload already sitting at that name. */
	f = fopen(target, "wb");
	if (f == NULL) {
		printf("HTTP PUT TEST: FAIL (seed)\n");
		return 1;
	}
	fwrite(original, 1, strlen(original), f);
	fclose(f);

	if (test_http_server_start(root, 1, &port, &pid) != 0) {
		printf("HTTP PUT TEST: FAIL (server did not start)\n");
		return 1;
	}

	/*
	 * --- an INCOMPLETE PUT ---
	 *
	 * EXACTLY 400, not merely "not 201". The first draft of this
	 * check was `status != 201`, which a test that never reached the
	 * server also satisfies: put_raw() returns -1 on a failed
	 * connect, and -1 != 201. Both assertions in this case would then
	 * have passed for the worst possible reason -- nothing happened.
	 *
	 * That is the exact shape this project's "reintroduce the bug and
	 * watch the test catch it" rule exists to expose, and it is
	 * cheaper to make the assertion unable to pass vacuously than to
	 * demonstrate once that it does not.
	 */
	status = put_raw(port, "thing.tar.gz", "SHORT", 5, 100000);
	check(status == 400,
	      "incomplete PUT is refused with 400, not reported as created");
	got = slurp(target, &got_len);
	check(got != NULL && got_len == strlen(original) && strcmp(got, original) == 0,
	      "incomplete PUT left the existing file untouched");
	free(got);

	/* --- and a COMPLETE one still works --- */
	status = put_raw(port, "thing.tar.gz", replacement, strlen(replacement),
	                 (long)strlen(replacement));
	check(status == 201, "complete PUT is accepted");
	got = slurp(target, &got_len);
	check(got != NULL && got_len == strlen(replacement) && strcmp(got, replacement) == 0,
	      "complete PUT replaced the file with exactly its body");
	free(got);

	/* No .part file may survive either outcome. */
	{
		char part[600];
		struct stat st;

		snprintf(part, sizeof(part), "%s.part", target);
		check(stat(part, &st) != 0, "no .part left behind");
	}

	test_http_server_stop(pid);
	unlink(target);
	{
		char hdr[600];

		snprintf(hdr, sizeof(hdr), "%s.headers", target);
		unlink(hdr);
	}
	rmdir(root);

	if (g_failures != 0) {
		printf("HTTP PUT TEST: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("HTTP PUT TEST: PASS\n");
	return 0;
}
