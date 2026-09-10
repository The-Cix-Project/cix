/*
 * test_btrfs -- the btrfs storage primitives (ADR-0207 phase 1).
 *
 * The dev sandbox is ext4 with no loop devices (documented in
 * CLAUDE.md), so a real btrfs subvolume cannot be created or mounted
 * here at all -- the ioctl paths are verified on the .95 VM against a
 * btrfs disk, not here. What IS fully testable here, and what this
 * asserts, is the transition contract that keeps ext4 hosts working
 * untouched: on a non-btrfs backing, every primitive falls back to the
 * plain-directory behaviour the image store had before ADR-0207.
 *
 * That fallback is the load-bearing part during the migration -- if it
 * regressed, every ext4 host would break the moment this module went
 * live -- so it earns real coverage even though the btrfs half cannot
 * run here.
 */
#include "btrfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                                                            \
	do {                                                                                       \
		if (!(cond)) {                                                                      \
			fprintf(stderr, "FAIL: ");                                                 \
			fprintf(stderr, __VA_ARGS__);                                              \
			fprintf(stderr, "\n");                                                     \
			failures++;                                                                \
		}                                                                                  \
	} while (0)

static int exists(const char *p)
{
	struct stat st;

	return lstat(p, &st) == 0;
}

static int is_dir(const char *p)
{
	struct stat st;

	return lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int write_file(const char *p, const char *content)
{
	FILE *f = fopen(p, "w");

	if (f == NULL)
		return -1;
	fputs(content, f);
	fclose(f);
	return 0;
}

static int read_first_line(const char *p, char *out, size_t out_size)
{
	FILE *f = fopen(p, "r");

	out[0] = '\0';
	if (f == NULL)
		return -1;
	if (fgets(out, (int)out_size, f) == NULL) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

int main(void)
{
	char base[] = "/tmp/cix_test_btrfs_XXXXXX";
	char path[512];
	char sub[512];
	char line[128];

	if (mkdtemp(base) == NULL) {
		perror("mkdtemp");
		return 1;
	}

	/* The sandbox is ext4, so this must report not-btrfs -- which is
	 * what steers every primitive onto its fallback below. If this
	 * ever returns true here, the whole test is meaningless, so assert
	 * it explicitly rather than assume it. */
	CHECK(cix_btrfs_is_backing(base) == 0,
	      "sandbox backing should not be btrfs -- the fallback paths are what this test covers");

	/* is_backing on a missing path is the safe answer (0), not a crash. */
	snprintf(path, sizeof(path), "%s/nope", base);
	CHECK(cix_btrfs_is_backing(path) == 0, "is_backing on a missing path should be 0");

	/* create_or_dir -> a plain directory on ext4. */
	snprintf(path, sizeof(path), "%s/img", base);
	CHECK(cix_btrfs_subvol_create_or_dir(path) == 0, "create_or_dir should succeed on ext4");
	CHECK(is_dir(path), "create_or_dir should have made a directory on ext4");

	/* Idempotent: a second create over the same path is success. */
	CHECK(cix_btrfs_subvol_create_or_dir(path) == 0, "create_or_dir should be idempotent");

	/*
	 * #364: is_subvolume must say NO for a plain directory, and must
	 * say it without needing btrfs to be present at all.
	 *
	 * The volume quota path branches on this to decide whether a qgroup
	 * can attach, so a false positive here would apply a limit to
	 * something that cannot carry one and report success, and a false
	 * negative on real btrfs would refuse every volume. On ext4 the
	 * only correct answer is 0, for a directory that exists and for one
	 * that does not -- the inode-256 test must not mistake an ordinary
	 * ext4 inode that happens to be 256 for a subvolume either, which
	 * is why is_backing gates it rather than the inode number alone.
	 */
	CHECK(cix_btrfs_is_subvolume(path) == 0,
	      "a plain ext4 directory is not a subvolume");
	{
		char missing[512];

		snprintf(missing, sizeof(missing), "%s/does-not-exist", base);
		CHECK(cix_btrfs_is_subvolume(missing) == 0,
		      "is_subvolume on a missing path should be 0, not a crash");
	}

	/* Populate it so the copy fallback has real content, including a
	 * subdirectory and a symlink -- a seeded rootfs has both. */
	snprintf(path, sizeof(path), "%s/img/etc", base);
	CHECK(mkdir(path, 0755) == 0, "mkdir img/etc");
	snprintf(path, sizeof(path), "%s/img/etc/hostname", base);
	CHECK(write_file(path, "cixhost\n") == 0, "write img/etc/hostname");
	snprintf(path, sizeof(path), "%s/img/link", base);
	CHECK(symlink("etc/hostname", path) == 0, "symlink img/link");

	/* snapshot_or_copy -> a recursive copy on ext4, content preserved. */
	snprintf(sub, sizeof(sub), "%s/clone", base);
	snprintf(path, sizeof(path), "%s/img", base);
	CHECK(cix_btrfs_snapshot_or_copy(path, sub) == 0, "snapshot_or_copy should copy on ext4");
	snprintf(path, sizeof(path), "%s/clone/etc/hostname", base);
	CHECK(read_first_line(path, line, sizeof(line)) == 0 && strcmp(line, "cixhost\n") == 0,
	      "copied clone should carry the file content, got '%s'", line);
	snprintf(path, sizeof(path), "%s/clone/link", base);
	{
		char target[128];
		ssize_t n = readlink(path, target, sizeof(target) - 1);

		if (n > 0)
			target[n] = '\0';
		CHECK(n > 0 && strcmp(target, "etc/hostname") == 0,
		      "the symlink should be copied as a symlink, got '%.*s'", (int)(n > 0 ? n : 0),
		      n > 0 ? target : "");
	}

	/* The clone is independent of the source (a real copy, not a link):
	 * changing the clone must not touch the source. */
	snprintf(path, sizeof(path), "%s/clone/etc/hostname", base);
	CHECK(write_file(path, "changed\n") == 0, "rewrite clone hostname");
	snprintf(path, sizeof(path), "%s/img/etc/hostname", base);
	CHECK(read_first_line(path, line, sizeof(line)) == 0 && strcmp(line, "cixhost\n") == 0,
	      "source must be unchanged after the clone was modified, got '%s'", line);

	/* delete_or_rmtree -> recursive remove on ext4. */
	snprintf(path, sizeof(path), "%s/clone", base);
	CHECK(cix_btrfs_subvol_delete_or_rmtree(path) == 0, "delete_or_rmtree should succeed");
	CHECK(!exists(path), "delete_or_rmtree should have removed the tree");

	/* Deleting an already-absent path is success, not an error. */
	CHECK(cix_btrfs_subvol_delete_or_rmtree(path) == 0, "delete of an absent path should be 0");

	snprintf(path, sizeof(path), "%s/img", base);
	CHECK(cix_btrfs_subvol_delete_or_rmtree(path) == 0, "delete_or_rmtree img");

	/*
	 * cix_btrfs_qgroup_query (#369). This sandbox has no loop devices,
	 * so no btrfs can be mounted here and the SUCCESS path belongs on a
	 * real host -- what is gated here is the refusal contract, which is
	 * where the dangerous failure lives: INO_LOOKUP on a plain
	 * directory succeeds and answers with the PARENT subvolume's
	 * accounting, so a query that did not refuse one would return a
	 * confident wrong number rather than an error.
	 *
	 * errno is seeded with a sentinel first, the pattern
	 * test_rtnetlink established: a helper that returns -1 without
	 * setting errno makes its caller report whatever unrelated syscall
	 * failed last, which cost this project two rounds of work in #341.
	 */
	snprintf(path, sizeof(path), "%s/plaindir", base);
	CHECK(mkdir(path, 0755) == 0, "create a plain directory to query");
	errno = 0x5a5a;
	CHECK(cix_btrfs_qgroup_query(path, NULL, NULL, NULL) == -1,
	      "qgroup_query must refuse a plain directory, not answer with its parent's numbers");
	CHECK(errno == EINVAL, "refusing a plain directory should be EINVAL, got %d (%s)", errno,
	      strerror(errno));

	/* An absent path is also a refusal, and also not the sentinel. */
	snprintf(path, sizeof(path), "%s/nothing-here", base);
	errno = 0x5a5a;
	CHECK(cix_btrfs_qgroup_query(path, NULL, NULL, NULL) == -1, "qgroup_query on an absent path");
	CHECK(errno != 0x5a5a, "a failed qgroup_query must set errno, it was left at the sentinel");

	/* Out-params are cleared before any failure can return, so a caller
	 * that ignores the return value cannot read a stale stack value as
	 * a real measurement. */
	{
		unsigned long long u = 12345, l = 67890;
		enum cix_btrfs_qgroup_state st = CIX_BTRFS_QGROUP_SIMPLE;

		snprintf(path, sizeof(path), "%s/plaindir", base);
		CHECK(cix_btrfs_qgroup_query(path, &u, &l, &st) == -1, "refusal with out-params");
		CHECK(u == 0 && l == 0 && st == CIX_BTRFS_QGROUP_OK,
		      "out-params must be zeroed even on refusal, got used=%llu limit=%llu state=%d", u,
		      l, (int)st);
	}
	snprintf(path, sizeof(path), "%s/plaindir", base);
	rmdir(path);

	/* Clean up the temp root itself. */
	rmdir(base);

	printf(failures == 0 ? "BTRFS RESULT: PASS\n" : "BTRFS RESULT: FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
