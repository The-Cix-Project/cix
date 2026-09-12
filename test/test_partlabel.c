/*
 * The GPT label lookup that replaced five hardcoded /dev/vda paths
 * (#305).
 *
 * This is boot-path code: if it is wrong the machine does not come up
 * at all, which is precisely the failure it was written to fix. So it
 * is tested against a real GPT rather than trusted -- a crafted one in
 * an ordinary file, the same technique test_diskpart.c uses for
 * superblock probing, and for the same reason: this sandbox has no
 * block device nodes, so the only honest way to exercise a parser that
 * reads one is to hand it bytes.
 */
#include "partlabel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail;

static void check(int cond, const char *what)
{
	printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
	if (!cond)
		g_fail = 1;
}

static void put_le32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v & 0xff);
	p[1] = (unsigned char)((v >> 8) & 0xff);
	p[2] = (unsigned char)((v >> 16) & 0xff);
	p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void put_le64(unsigned char *p, unsigned long long v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = (unsigned char)((v >> (i * 8)) & 0xff);
}

/*
 * A GPT with the five names cix-install.c actually writes, laid out the
 * way a real one is: protective MBR at LBA 0, header at LBA 1, entry
 * array at LBA 2, 128-byte entries whose name is 36 UTF-16LE units at
 * offset 56.
 */
static int write_gpt(const char *path)
{
	static const char *const names[] = { "cix-esp", "cix-root-a", "cix-root-b", "cix-config",
		                               "cix-containers" };
	unsigned char lba0[512], header[512], entries[5 * 128];
	FILE *f = fopen(path, "wb");
	size_t n, i;

	if (f == NULL)
		return -1;
	memset(lba0, 0, sizeof(lba0));
	memset(header, 0, sizeof(header));
	memset(entries, 0, sizeof(entries));

	memcpy(header, "EFI PART", 8);
	put_le64(header + 72, 2);   /* entry array starts at LBA 2 */
	put_le32(header + 80, 5);   /* five entries */
	put_le32(header + 84, 128); /* 128 bytes each */

	for (n = 0; n < 5; n++) {
		unsigned char *e = entries + n * 128;

		/* A non-zero type GUID, so the entry is not "unused". */
		memset(e, 0xa5, 16);
		/*
		 * A unique partition GUID with every byte distinct, so a
		 * byte-order mistake cannot accidentally produce the right
		 * string. Stored at offset 16, sixteen bytes, first three
		 * fields little-endian per the GPT spec.
		 */
		for (i = 0; i < 16; i++)
			e[16 + i] = (unsigned char)(n * 0x10 + i);
		for (i = 0; names[n][i] != '\0' && i < 36; i++) {
			e[56 + i * 2] = (unsigned char)names[n][i];
			e[57 + i * 2] = 0;
		}
	}

	if (fwrite(lba0, 1, sizeof(lba0), f) != sizeof(lba0) ||
	    fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
	    fwrite(entries, 1, sizeof(entries), f) != sizeof(entries)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

/*
 * One line into a file, for the fake sysfs below: a partition's
 * "partition" file, holding its number.
 */
static int write_number(const char *path, unsigned int n)
{
	FILE *f = fopen(path, "w");

	if (f == NULL)
		return -1;
	fprintf(f, "%u\n", n);
	return fclose(f) == 0 ? 0 : -1;
}

/*
 * TWO disks carrying the SAME five labels, and a /sys/class/block shaped
 * directory describing their partitions -- the situation #427 is about
 * and the one the real scan could never be handed here, since this
 * sandbox has no block device nodes to attach a second GPT to.
 *
 * <dir>/diskA and <dir>/diskB are real GPTs; <dir>/sys/diskA4 and
 * <dir>/sys/diskB4 are the partition directories the scan enumerates.
 * Partition 4 is cix-config and partition 1 is cix-esp on BOTH, so
 * every label in the platform's layout is ambiguous by construction.
 */
static int build_two_disk_tree(const char *dir, char *sys_dir, size_t sys_dir_size)
{
	static const char *const disks[] = { "diskA", "diskB" };
	static const unsigned int parts[] = { 1, 4 };
	size_t i, j;
	char path[512];

	if (snprintf(sys_dir, sys_dir_size, "%s/sys", dir) >= (int)sys_dir_size)
		return -1;
	if (mkdir(sys_dir, 0755) != 0)
		return -1;

	for (i = 0; i < sizeof(disks) / sizeof(disks[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, disks[i]);
		if (write_gpt(path) != 0)
			return -1;
		for (j = 0; j < sizeof(parts) / sizeof(parts[0]); j++) {
			snprintf(path, sizeof(path), "%s/%s%u", sys_dir, disks[i], parts[j]);
			if (mkdir(path, 0755) != 0)
				return -1;
			snprintf(path, sizeof(path), "%s/%s%u/partition", sys_dir, disks[i], parts[j]);
			if (write_number(path, parts[j]) != 0)
				return -1;
		}
	}
	return 0;
}

int main(void)
{
	char img[] = "/tmp/cix_partlabel_XXXXXX";
	char out[64];
	int fd;

	printf("PARTLABEL\n");

	fd = mkstemp(img);
	if (fd < 0) {
		fprintf(stderr, "FAIL: mkstemp\n");
		return 1;
	}
	close(fd);
	if (write_gpt(img) != 0) {
		fprintf(stderr, "FAIL: could not write the test GPT\n");
		unlink(img);
		return 1;
	}

	/* Every name cix-install writes, read back by partition number. */
	partlabel_read(img, 1, out, sizeof(out));
	check(strcmp(out, "cix-esp") == 0, "partition 1 reads as cix-esp");
	partlabel_read(img, 2, out, sizeof(out));
	check(strcmp(out, "cix-root-a") == 0, "partition 2 reads as cix-root-a");
	partlabel_read(img, 3, out, sizeof(out));
	check(strcmp(out, "cix-root-b") == 0, "partition 3 reads as cix-root-b");
	partlabel_read(img, 4, out, sizeof(out));
	check(strcmp(out, "cix-config") == 0, "partition 4 reads as cix-config");
	partlabel_read(img, 5, out, sizeof(out));
	check(strcmp(out, "cix-containers") == 0, "partition 5 reads as cix-containers");

	/*
	 * The refusals matter as much as the reads: each of these used to
	 * be a path that could hand a caller a stale buffer and have it
	 * mount something.
	 */
	partlabel_read(img, 0, out, sizeof(out));
	check(out[0] == '\0', "partition 0 is empty, never a stale buffer");
	partlabel_read(img, 6, out, sizeof(out));
	check(out[0] == '\0', "a partition past the entry count is empty");
	partlabel_read("/nonexistent/disk", 1, out, sizeof(out));
	check(out[0] == '\0', "an unopenable device is empty");
	{
		char empty[] = "/tmp/cix_nogpt_XXXXXX";

		fd = mkstemp(empty);
		if (fd >= 0) {
			close(fd);
			partlabel_read(empty, 1, out, sizeof(out));
			check(out[0] == '\0', "a file with no GPT signature is empty");
			unlink(empty);
		}
	}

	/*
	 * PARTUUID, which is the half a label cannot do: the kernel's own
	 * root= parser takes PARTUUID= and does not take PARTLABEL=, so
	 * this string is what stands between a renamed disk and a panic
	 * loop. Partition 1's bytes are 00..0f, so the expected text is
	 * the spec's mixed-endian rendering spelled out by hand -- if the
	 * first three fields were emitted in storage order this would
	 * read 00010203-0405-0607-... and the assert would catch it.
	 */
	partlabel_read_uuid(img, 1, out, sizeof(out));
	check(strcmp(out, "03020100-0504-0706-0809-0a0b0c0d0e0f") == 0,
	      "partition 1 PARTUUID is mixed-endian per the GPT spec");
	partlabel_read_uuid(img, 2, out, sizeof(out));
	check(strcmp(out, "13121110-1514-1716-1819-1a1b1c1d1e1f") == 0,
	      "partition 2 PARTUUID reads its own entry, not partition 1's");
	partlabel_read_uuid(img, 0, out, sizeof(out));
	check(out[0] == '\0', "PARTUUID of partition 0 is empty");
	partlabel_read_uuid(img, 9, out, sizeof(out));
	check(out[0] == '\0', "PARTUUID past the entry count is empty");
	{
		char tiny[8];

		strcpy(tiny, "poison");
		partlabel_read_uuid(img, 1, tiny, sizeof(tiny));
		check(tiny[0] == '\0', "a buffer too small for a UUID is emptied, never truncated");
	}

	unlink(img);

	/* The parent split, including the nvme separator. */
	partlabel_parent_name("sda4", out, sizeof(out));
	check(strcmp(out, "sda") == 0, "sda4 belongs to sda");
	partlabel_parent_name("vda5", out, sizeof(out));
	check(strcmp(out, "vda") == 0, "vda5 belongs to vda");
	partlabel_parent_name("nvme0n1p2", out, sizeof(out));
	check(strcmp(out, "nvme0n1") == 0, "nvme0n1p2 belongs to nvme0n1");
	partlabel_parent_name("sda", out, sizeof(out));
	check(strcmp(out, "sda") == 0, "a whole disk is its own parent");

	/*
	 * The scan itself. This sandbox has /sys/class/block but no block
	 * device nodes at all, so no label can resolve here -- what is
	 * being asserted is that it says so cleanly instead of returning
	 * a half-filled buffer a caller would then mount.
	 */
	strcpy(out, "poison");
	check(partlabel_find("cix-definitely-not-a-real-label", out, sizeof(out)) == -1 &&
	          out[0] == '\0',
	      "an unknown label returns -1 and clears the buffer");
	check(partlabel_find(NULL, out, sizeof(out)) == -1, "a NULL label is refused");
	check(partlabel_find("", out, sizeof(out)) == -1, "an empty label is refused");

	/*
	 * The root disk, which is the tie-break itself (#427). What it
	 * answers depends entirely on where this runs -- a real partition
	 * in an installed system, a dm volume in this sandbox, nothing at
	 * all in a container whose / is an overlay -- so what is asserted
	 * is the invariant that holds everywhere: it either succeeds with
	 * a bare disk NAME, or it fails with the buffer cleared. Never a
	 * path, never a leftover.
	 */
	strcpy(out, "poison");
	{
		int rc = partlabel_root_disk(out, sizeof(out));

		check(rc == 0 ? (out[0] != '\0' && strchr(out, '/') == NULL) : out[0] == '\0',
		      "the root disk is a bare disk name, or cleanly absent");
		printf("  (root disk here: %s)\n", rc == 0 ? out : "none");
	}
	check(partlabel_root_disk(NULL, 8) == -1, "a NULL root-disk buffer is refused");
	check(partlabel_root_disk(out, 0) == -1, "a zero-length root-disk buffer is refused");

	/*
	 * The duplicate-label tie-break, against two disks that both
	 * carry cix-config (#427). Before the scoping existed this was
	 * decided by readdir(3) order, which is not a contract and can
	 * differ between two boots of the same machine -- so /config
	 * could be mounted from the wrong disk, and the platform would
	 * have no way to tell.
	 */
	{
		char dir[] = "/tmp/cix_partlabel_dup_XXXXXX";
		char sys_dir[512];
		char expect[512];

		if (mkdtemp(dir) == NULL) {
			fprintf(stderr, "FAIL: mkdtemp for the duplicate-label tree\n");
			return 1;
		}
		if (build_two_disk_tree(dir, sys_dir, sizeof(sys_dir)) != 0) {
			fprintf(stderr, "FAIL: could not build the two-disk tree\n");
			return 1;
		}

		/*
		 * Both directions. A scoping bug that ignored only_disk
		 * entirely would still pass one of these by luck, so passing
		 * both is what proves the restriction is real.
		 */
		snprintf(expect, sizeof(expect), "%s/diskB4", dir);
		check(partlabel_find_in(sys_dir, dir, "diskB", "cix-config", out, sizeof(out)) == 0 &&
		          strcmp(out, expect) == 0,
		      "cix-config scoped to diskB resolves on diskB");
		snprintf(expect, sizeof(expect), "%s/diskA4", dir);
		check(partlabel_find_in(sys_dir, dir, "diskA", "cix-config", out, sizeof(out)) == 0 &&
		          strcmp(out, expect) == 0,
		      "the same label scoped to diskA resolves on diskA");

		/* A second label, so the answer is not partition 4 by accident. */
		snprintf(expect, sizeof(expect), "%s/diskB1", dir);
		check(partlabel_find_in(sys_dir, dir, "diskB", "cix-esp", out, sizeof(out)) == 0 &&
		          strcmp(out, expect) == 0,
		      "cix-esp scoped to diskB resolves to diskB's own partition 1");

		/*
		 * Unscoped is still first-match, and deliberately not
		 * asserted to be either disk: readdir order is not a
		 * contract, and pinning it here would assert the very thing
		 * #427 says cannot be relied on. What must hold is that the
		 * answer is one of the two real candidates.
		 */
		{
			char a[512], b[512];

			snprintf(a, sizeof(a), "%s/diskA4", dir);
			snprintf(b, sizeof(b), "%s/diskB4", dir);
			check(partlabel_find_in(sys_dir, dir, NULL, "cix-config", out, sizeof(out)) == 0 &&
			          (strcmp(out, a) == 0 || strcmp(out, b) == 0),
			      "unscoped, an ambiguous label still resolves to one of them");
		}

		/* A disk that is not in the tree resolves nothing, rather than falling
		 * back to a disk that is -- the scoping has to be a restriction, not a
		 * preference. */
		strcpy(out, "poison");
		check(partlabel_find_in(sys_dir, dir, "diskC", "cix-config", out, sizeof(out)) == -1 &&
		          out[0] == '\0',
		      "scoped to a disk that has no such label, nothing resolves");
		check(partlabel_find_in(sys_dir, dir, "diskA", "cix-not-a-label", out, sizeof(out)) == -1,
		      "an unknown label on a real disk resolves nothing");
		check(partlabel_find_in(NULL, dir, NULL, "cix-config", out, sizeof(out)) == -1 &&
		          partlabel_find_in(sys_dir, NULL, NULL, "cix-config", out, sizeof(out)) == -1,
		      "a NULL sysfs or /dev directory is refused");

		{
			char path[512];
			size_t i;
			static const char *const leaves[] = { "diskA1", "diskA4", "diskB1", "diskB4" };

			for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
				snprintf(path, sizeof(path), "%s/%s/partition", sys_dir, leaves[i]);
				unlink(path);
				snprintf(path, sizeof(path), "%s/%s", sys_dir, leaves[i]);
				rmdir(path);
			}
			rmdir(sys_dir);
			snprintf(path, sizeof(path), "%s/diskA", dir);
			unlink(path);
			snprintf(path, sizeof(path), "%s/diskB", dir);
			unlink(path);
			rmdir(dir);
		}
	}

	printf("PARTLABEL RESULT: %s\n", g_fail ? "FAIL" : "PASS");
	return g_fail;
}
