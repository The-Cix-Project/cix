/*
 * ADR-0141's shared migration copy primitive: proves real permission
 * preservation (the exact gap found reviewing pkg.c's own merge_tree()/
 * copy_file_simple(), which hardcode 0755 regardless of the source --
 * harmless for that module's own callers, a real security regression
 * for this one's, since state-storage carries pki.c's 0600 private
 * keys) and correct symlink handling (recreated verbatim, never
 * followed).
 */
#include "treecopy.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_file(const char *path, const char *content, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);

	if (fd < 0)
		return -1;
	if (write(fd, content, strlen(content)) != (ssize_t)strlen(content)) {
		close(fd);
		return -1;
	}
	close(fd);
	return chmod(path, mode); /* umask can narrow what open()'s own mode arg actually set */
}

static int read_file(const char *path, char *out, size_t out_size)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, out, out_size - 1);
	close(fd);
	if (n < 0)
		return -1;
	out[n] = '\0';
	return 0;
}

static int mode_of(const char *path, mode_t *out)
{
	struct stat st;

	if (lstat(path, &st) != 0)
		return -1;
	*out = st.st_mode & 07777;
	return 0;
}

int main(void)
{
	char src[PATH_MAX], dst[PATH_MAX];
	char path[PATH_MAX];
	int ok = 1;

	snprintf(src, sizeof(src), "/tmp/kanxeo_test_treecopy_src_%d", (int)getpid());
	snprintf(dst, sizeof(dst), "/tmp/kanxeo_test_treecopy_dst_%d", (int)getpid());
	mkdir(src, 0755);
	mkdir(dst, 0755);

	/* A 0600 "private key" -- the exact case this primitive exists for. */
	snprintf(path, sizeof(path), "%s/key.pem", src);
	if (write_file(path, "-----BEGIN PRIVATE KEY-----\nfake\n", 0600) != 0) {
		fprintf(stderr, "FAIL: could not stage key.pem\n");
		ok = 0;
	}

	/* A 0644 "certificate." */
	snprintf(path, sizeof(path), "%s/cert.pem", src);
	if (ok && write_file(path, "-----BEGIN CERTIFICATE-----\nfake\n", 0644) != 0) {
		fprintf(stderr, "FAIL: could not stage cert.pem\n");
		ok = 0;
	}

	/* A subdirectory with its own file, to prove recursion. */
	snprintf(path, sizeof(path), "%s/sub", src);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/sub/nested.json", src);
	if (ok && write_file(path, "{\"a\":1}", 0640) != 0) {
		fprintf(stderr, "FAIL: could not stage nested.json\n");
		ok = 0;
	}

	/* A symlink, which must be recreated verbatim, never followed. */
	snprintf(path, sizeof(path), "%s/link-to-cert", src);
	if (ok && symlink("cert.pem", path) != 0) {
		fprintf(stderr, "FAIL: could not stage symlink\n");
		ok = 0;
	}

	if (ok && treecopy_recursive(src, dst) != 0) {
		fprintf(stderr, "FAIL: treecopy_recursive failed: %s\n", strerror(errno));
		ok = 0;
	}

	/* key.pem: content + exact 0600 mode preserved. */
	if (ok) {
		char content[128];
		mode_t m;

		snprintf(path, sizeof(path), "%s/key.pem", dst);
		if (read_file(path, content, sizeof(content)) != 0 ||
		    strcmp(content, "-----BEGIN PRIVATE KEY-----\nfake\n") != 0) {
			fprintf(stderr, "FAIL: key.pem content mismatch after copy\n");
			ok = 0;
		}
		if (ok && (mode_of(path, &m) != 0 || m != 0600)) {
			fprintf(stderr, "FAIL: key.pem mode after copy = %o, expected 0600\n", ok ? m : 0);
			ok = 0;
		}
	}

	/* cert.pem: exact 0644 mode preserved (proves this isn't just
	 * "always narrow" -- the real source mode is what's replicated,
	 * whatever it is). */
	if (ok) {
		mode_t m;

		snprintf(path, sizeof(path), "%s/cert.pem", dst);
		if (mode_of(path, &m) != 0 || m != 0644) {
			fprintf(stderr, "FAIL: cert.pem mode after copy = %o, expected 0644\n", ok ? m : 0);
			ok = 0;
		}
	}

	/* Nested file survived recursion with its own distinct mode. */
	if (ok) {
		char content[128];
		mode_t m;

		snprintf(path, sizeof(path), "%s/sub/nested.json", dst);
		if (read_file(path, content, sizeof(content)) != 0 || strcmp(content, "{\"a\":1}") != 0) {
			fprintf(stderr, "FAIL: sub/nested.json content mismatch after copy\n");
			ok = 0;
		}
		if (ok && (mode_of(path, &m) != 0 || m != 0640)) {
			fprintf(stderr, "FAIL: sub/nested.json mode after copy = %o, expected 0640\n", ok ? m : 0);
			ok = 0;
		}
	}

	/* Symlink recreated verbatim, not followed/copied as a file. */
	if (ok) {
		char target[128];
		ssize_t len;

		snprintf(path, sizeof(path), "%s/link-to-cert", dst);
		len = readlink(path, target, sizeof(target) - 1);
		if (len < 0) {
			fprintf(stderr, "FAIL: link-to-cert is not a symlink after copy\n");
			ok = 0;
		} else {
			target[len] = '\0';
			if (strcmp(target, "cert.pem") != 0) {
				fprintf(stderr, "FAIL: link-to-cert target = '%s', expected 'cert.pem'\n", target);
				ok = 0;
			}
		}
	}

	if (ok)
		printf("TREECOPY RESULT: PASS\n");
	else
		printf("TREECOPY RESULT: FAIL\n");

	{
		char cmd[PATH_MAX * 2 + 32];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", src, dst);
		system(cmd);
	}

	return ok ? 0 : 1;
}
