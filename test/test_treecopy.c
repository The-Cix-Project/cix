/*
 * ADR-0141's shared migration copy primitive: proves real permission
 * preservation (the exact gap found reviewing pkg.c's own merge_tree()/
 * copy_file_simple(), which hardcode 0755 regardless of the source --
 * harmless for that module's own callers, a real security regression
 * for this one's, since the state tree carries pki.c's 0600 private
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
#include <sys/sysmacros.h>
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

/* Separate so the test can report "needs real root" rather than a
 * confusing copy failure when run unprivileged. */
static int mknod_null(const char *dir)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/null", dir);
	return mknod(path, S_IFCHR | 0666, makedev(1, 3));
}

int main(void)
{
	char src[PATH_MAX], dst[PATH_MAX];
	char path[PATH_MAX];
	int ok = 1;

	snprintf(src, sizeof(src), "/tmp/cix_test_treecopy_src_%d", (int)getpid());
	snprintf(dst, sizeof(dst), "/tmp/cix_test_treecopy_dst_%d", (int)getpid());
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

	/*
	 * Issue #172: a device node must survive the copy, WITH its mode.
	 *
	 * treecopy used to skip device nodes silently, on a comment
	 * asserting they are "never expected under any of this project's
	 * own storage-placement trees". That is true for the state tree and
	 * log-storage and false for the image store, which is exactly what
	 * rebuildable-storage migration moves: pkg_seed_image_baseline()
	 * stages /dev/null, /dev/zero, /dev/full and /dev/ptmx into every
	 * image rootfs (ADR-0150 added ptmx so posix_openpt() works). A
	 * migration that "succeeded" would have produced an image store
	 * whose containers fail on /dev/null and cannot allocate a PTY,
	 * with no error at any point and the symptom appearing nowhere
	 * near the cause.
	 *
	 * The mode half matters just as much as the node existing: mknod()
	 * applies the umask, so a 0666 /dev/null lands at 0644 and any
	 * container not running as host root -- the default since ADR-0207
	 * phase 3 -- can no longer write to it.
	 */
	{
		char devsrc[PATH_MAX], devdst[PATH_MAX], cmd[PATH_MAX * 2 + 64];
		struct stat sst, dst_st;

		snprintf(devsrc, sizeof(devsrc), "%s/devnode", src);
		snprintf(devdst, sizeof(devdst), "%s/devnode", dst);
		snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s' && mkdir -p '%s' '%s'", devsrc, devdst,
		         devsrc, devdst);
		system(cmd);

		if (mknod_null(devsrc) != 0) {
			fprintf(stderr, "SKIP: cannot create a device node here (needs real root)\n");
		} else {
			char nullsrc[PATH_MAX], nulldst[PATH_MAX];

			snprintf(nullsrc, sizeof(nullsrc), "%s/null", devsrc);
			snprintf(nulldst, sizeof(nulldst), "%s/null", devdst);
			/* 0666 explicitly: mknod() above was subject to the umask,
			 * so without this the source itself would not exercise the
			 * mode-preservation half at all. */
			if (chmod(nullsrc, 0666) != 0)
				fprintf(stderr, "FAIL: could not chmod the source device node\n"), ok = 0;

			if (treecopy_recursive(devsrc, devdst) != 0) {
				fprintf(stderr, "FAIL: treecopy over a device node: %s\n",
				        treecopy_last_error());
				ok = 0;
			} else if (lstat(nulldst, &dst_st) != 0) {
				fprintf(stderr, "FAIL: the device node was dropped by the copy (#172)\n");
				ok = 0;
			} else if (!S_ISCHR(dst_st.st_mode)) {
				fprintf(stderr, "FAIL: copied /dev/null is not a character device\n");
				ok = 0;
			} else if (lstat(nullsrc, &sst) == 0 &&
			           (dst_st.st_mode & 07777) != (sst.st_mode & 07777)) {
				fprintf(stderr,
				        "FAIL: device node mode not preserved -- source %o, copy %o; mknod's "
				        "umask makes a 0666 /dev/null land at 0644, which no non-root "
				        "container can write to\n",
				        sst.st_mode & 07777, dst_st.st_mode & 07777);
				ok = 0;
			} else if (dst_st.st_rdev != sst.st_rdev) {
				fprintf(stderr, "FAIL: device node major/minor not preserved\n");
				ok = 0;
			}
		}
	}

	/*
	 * Issue #172, third defect: the destination ROOT must be created.
	 * treecopy_walk() mkdirs every directory it finds under the source
	 * but nothing created the root itself, so a migration onto a
	 * freshly formatted disk died on its first entry with
	 *   mkdir: .../sda/rebuildable/pkg: No such file or directory
	 * because .../rebuildable did not exist to hold pkg. That means
	 * rebuildable-storage migration had never once worked, invisible
	 * for as long as the only report was "bulk copy failed".
	 */
	{
		char rootsrc[PATH_MAX], rootdst[PATH_MAX], cmd[PATH_MAX * 2 + 64];
		struct stat st;

		snprintf(rootsrc, sizeof(rootsrc), "%s/rootcheck", src);
		/* deliberately several levels deep and NOT created first */
		snprintf(rootdst, sizeof(rootdst), "%s/rootcheck-dst/deeper/still", dst);
		snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s' && mkdir -p '%s/inner' && echo x > '%s/inner/f'",
		         rootsrc, rootdst, rootsrc, rootsrc);
		system(cmd);

		if (treecopy_recursive(rootsrc, rootdst) != 0) {
			fprintf(stderr, "FAIL: treecopy did not create its destination root: %s\n",
			        treecopy_last_error());
			ok = 0;
		} else {
			char inner[PATH_MAX];

			snprintf(inner, sizeof(inner), "%s/inner/f", rootdst);
			if (stat(inner, &st) != 0) {
				fprintf(stderr, "FAIL: content missing under a created destination root\n");
				ok = 0;
			}
		}
	}

	/*
	 * Issue #194: a source root that does not exist is a FAILURE, not
	 * an empty success.
	 *
	 * This is the shape that let the installer's package seed be
	 * copied from a path that never existed and still report a clean
	 * install. Every caller of this primitive is a migration, a backup
	 * or a restore, so "moved nothing" must never look like "moved
	 * everything" -- the caller repoints live state on a 0 return.
	 *
	 * The error must also NAME the directory. The subdirectory case
	 * already returned -1 but recorded nothing, so an operator got the
	 * previous call's message or "no error recorded".
	 */
	{
		char missing[PATH_MAX], into[PATH_MAX];

		snprintf(missing, sizeof(missing), "%s/definitely-not-here/nor-this", src);
		snprintf(into, sizeof(into), "%s/from-missing", dst);

		if (treecopy_recursive(missing, into) == 0) {
			fprintf(stderr, "FAIL: treecopy reported success for a source root "
			                "that does not exist -- a migration that moved nothing "
			                "is indistinguishable from one that worked (#194)\n");
			ok = 0;
		} else if (strstr(treecopy_last_error(), "definitely-not-here") == NULL) {
			fprintf(stderr, "FAIL: treecopy failed on a missing source root but did "
			                "not name it: %s\n", treecopy_last_error());
			ok = 0;
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
