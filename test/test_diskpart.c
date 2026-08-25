/*
 * Partition-level disk management (ROADMAP.md task #844) end-to-end
 * test: proves disk_enumerate()'s new partition-reporting pass and
 * diskpart.c's own validation logic, both over real HTTP against a
 * real cixd subprocess.
 *
 * What this test deliberately does NOT prove, and cannot in this
 * sandbox: the real sfdisk-invoking success path of
 * diskpart_create_table()/diskpart_add()/diskpart_delete(). This
 * sandbox has no loop devices (confirmed directly: `losetup -f` on a
 * fresh scratch file fails with "cannot find an unused loop device",
 * matching the same class of gap CLAUDE.md already documents for
 * test_disk_quota.c) and diskpart.c's own public functions only ever
 * accept a disk name disk_enumerate() itself discovered from real
 * /sys/class/block -- there is no safe, disposable real block device
 * in this sandbox to actually run sfdisk against without risking this
 * host's own real disks. The exact sfdisk script syntax
 * diskpart_create_table()/diskpart_add()/diskpart_delete() use (a
 * fresh "label: gpt" table, `sfdisk --append` with/without a size=
 * field, `sfdisk --delete <device> <partno>`) was instead verified by
 * hand against a scratch disk image file -- sfdisk operates
 * identically on a plain file as on a real block device (the same
 * property test/test_disk_image.c's own sfdisk-driving tests already
 * rely on) -- see docs/adr/0158-partition-level-disk-management.md
 * for the exact commands run and their output.
 *
 * What IS verified here, for real:
 *   1. GET /v1/disks reports is_partition/parent_disk correctly for
 *      this sandbox's own real, pre-existing partitions -- proving
 *      disk_enumerate()'s new second pass and parent-lookup are
 *      correct against real kernel sysfs state, not just a unit test
 *      of code in isolation.
 *   2. Every validation-only rejection path in diskpart.c that
 *      returns before ever invoking sfdisk (invalid name, not found,
 *      wrong resource kind -- a partition where a whole disk was
 *      expected or vice versa, wrong parent, a role already assigned)
 *      -- exercised as real HTTP requests against the real daemon.
 */
#include "diskpart.h"
#include "disk.h"
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7681
#define PORT_ARG "--port=7681"

static char g_data_dir[PATH_MAX];

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

static int jbool(const struct json_value *obj, const char *key)
{
	const struct json_value *v = json_object_get(obj, key);

	return (v != NULL && v->type == JSON_BOOL && v->u.boolean);
}

static const char *jstr(const struct json_value *obj, const char *key)
{
	const char *s = json_as_string(json_object_get(obj, key));

	return s != NULL ? s : "";
}

/*
 * The mount attribution itself (issue #9). This is not incidental
 * bookkeeping: diskpart_delete() reads `mounted` to decide whether a
 * partition may be removed, so getting it wrong means deleting a
 * partition out from under a live filesystem -- which is exactly what
 * happened on real hardware before this was fixed. The old code mapped
 * every mounted device to its PARENT whole disk, which was right while
 * only whole disks were enumerated and wrong the moment ADR-0158 added
 * partitions.
 *
 * Driven off a synthetic /proc/mounts because the real one cannot be
 * arranged to order inside a test, and this sandbox has no loop devices
 * to mount anything on.
 */
static int test_mount_attribution(void)
{
	static const char *const mounts =
	    "/dev/sda1 /var/lib/cix/disks/sda1 ext4 rw,relatime 0 0\n"
	    "/dev/sdb /mnt/whole ext4 rw,relatime 0 0\n"
	    "proc /proc proc rw 0 0\n";
	struct discovered_disk d[4];
	char path[] = "/tmp/cix_test_mounts_XXXXXX";
	int fd;
	int ok = 1;
	int i;

	memset(d, 0, sizeof(d));
	snprintf(d[0].name, sizeof(d[0].name), "sda");  /* parent, not itself mounted */
	snprintf(d[1].name, sizeof(d[1].name), "sda1"); /* the genuinely mounted partition */
	snprintf(d[2].name, sizeof(d[2].name), "sda2"); /* sibling, untouched */
	snprintf(d[3].name, sizeof(d[3].name), "sdb");  /* whole disk mounted directly */

	fd = mkstemp(path);
	if (fd < 0) {
		fprintf(stderr, "FAIL: mkstemp for synthetic mounts file\n");
		return 0;
	}
	if (write(fd, mounts, strlen(mounts)) != (ssize_t)strlen(mounts)) {
		fprintf(stderr, "FAIL: writing synthetic mounts file\n");
		close(fd);
		unlink(path);
		return 0;
	}
	close(fd);

	disk_fill_mount_status_from(path, d, 4);
	unlink(path);

	/* The mounted partition reports ITSELF mounted, at its own path.
	 * This is the assertion that fails against the old behaviour, and
	 * the one the delete guard depends on. */
	if (!d[1].mounted || strcmp(d[1].mount_path, "/var/lib/cix/disks/sda1") != 0) {
		fprintf(stderr,
		        "FAIL: mounted partition sda1 reports mounted=%d path=\"%s\" -- the delete "
		        "guard reads this, so a live filesystem can be destroyed\n",
		        d[1].mounted, d[1].mount_path);
		ok = 0;
	}
	/* Its parent is NOT itself mounted, but does carry the separate
	 * flag the repartition guard needs. */
	if (d[0].mounted || d[0].mount_path[0] != '\0') {
		fprintf(stderr, "FAIL: sda reports mounted=%d path=\"%s\" -- the disk device itself "
		                "is not mounted, its partition is\n",
		        d[0].mounted, d[0].mount_path);
		ok = 0;
	}
	if (!d[0].has_mounted_partition) {
		fprintf(stderr, "FAIL: sda has_mounted_partition=0 -- repartitioning it would be "
		                "allowed while sda1 is live\n");
		ok = 0;
	}
	/* An unmounted sibling stays clean. */
	if (d[2].mounted || d[2].has_mounted_partition) {
		fprintf(stderr, "FAIL: unmounted sibling sda2 marked mounted=%d has_part=%d\n",
		        d[2].mounted, d[2].has_mounted_partition);
		ok = 0;
	}
	/* A whole disk mounted directly still works, and never claims a
	 * mounted partition it does not have. */
	if (!d[3].mounted || strcmp(d[3].mount_path, "/mnt/whole") != 0 ||
	    d[3].has_mounted_partition) {
		fprintf(stderr, "FAIL: directly-mounted whole disk sdb: mounted=%d path=\"%s\" "
		                "has_part=%d\n",
		        d[3].mounted, d[3].mount_path, d[3].has_mounted_partition);
		ok = 0;
	}
	for (i = 0; i < 4; i++)
		(void)i;
	return ok;
}

/*
 * Filesystem probing (issue #90). Driven against crafted files rather
 * than real devices because this dev sandbox has no block device nodes
 * at all -- /sys/class/block is visible, which is why enumeration lists
 * disks here, but /dev/sda and friends do not exist, so every open()
 * returns ENOENT. Verified directly rather than assumed.
 *
 * Each case writes the real on-disk magic at its real offset, so this
 * checks the offsets themselves, which is the only part that can be
 * wrong in a way that silently mislabels an operator's disk.
 */
static int write_probe_file(char *path, size_t size, size_t offset, const void *magic,
                            size_t magic_len)
{
	int fd = mkstemp(path);
	unsigned char *zero;

	if (fd < 0)
		return -1;
	zero = calloc(1, size);
	if (zero == NULL) {
		close(fd);
		unlink(path);
		return -1;
	}
	memcpy(zero + offset, magic, magic_len);
	if (write(fd, zero, size) != (ssize_t)size) {
		free(zero);
		close(fd);
		unlink(path);
		return -1;
	}
	free(zero);
	close(fd);
	return 0;
}

static int test_fs_probe(void)
{
	static const unsigned char ext4_magic[2] = { 0x53, 0xEF };
	struct {
		const char *expect;
		size_t size;
		size_t offset;
		const void *magic;
		size_t magic_len;
	} cases[] = {
		{ "squashfs", 4096, 0, "hsqs", 4 },
		{ "vfat", 4096, 0x52, "FAT32", 5 },
		{ "ext4", 8192, 1024 + 56, ext4_magic, 2 },
		{ "btrfs", 0x11000, 0x10040, "_BHRfS_M", 8 },
		{ "swap", 4096, 4096 - 10, "SWAPSPACE2", 10 },
	};
	size_t i;
	int ok = 1;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char path[] = "/tmp/cix_test_fsprobe_XXXXXX";
		char got[16];

		if (write_probe_file(path, cases[i].size, cases[i].offset, cases[i].magic,
		                      cases[i].magic_len) != 0) {
			fprintf(stderr, "FAIL: could not write probe file for %s\n", cases[i].expect);
			ok = 0;
			continue;
		}
		disk_probe_fs_type(path, got, sizeof(got));
		unlink(path);
		if (strcmp(got, cases[i].expect) != 0) {
			fprintf(stderr, "FAIL: fs probe read \"%s\", expected \"%s\"\n", got,
			        cases[i].expect);
			ok = 0;
		}
	}

	/* An all-zero device is unformatted and must say so rather than
	 * guessing -- a partition reported as ext4 when it is blank is
	 * worse than one reported as unknown. */
	{
		char path[] = "/tmp/cix_test_fsprobe_XXXXXX";
		char got[16];

		if (write_probe_file(path, 0x11000, 0, "", 0) == 0) {
			disk_probe_fs_type(path, got, sizeof(got));
			unlink(path);
			if (got[0] != '\0') {
				fprintf(stderr, "FAIL: blank device probed as \"%s\", expected \"\"\n", got);
				ok = 0;
			}
		}
	}

	/* A path that does not exist is the normal case in this sandbox
	 * and must not be reported as a filesystem. */
	{
		char got[16];

		disk_probe_fs_type("/dev/cix-no-such-device", got, sizeof(got));
		if (got[0] != '\0') {
			fprintf(stderr, "FAIL: missing device probed as \"%s\", expected \"\"\n", got);
			ok = 0;
		}
	}
	return ok;
}

/*
 * Free-space reporting (issue #95). Driven against real disk *images*
 * rather than block devices -- sfdisk operates identically on a plain
 * file, which is how this file's own partition-syntax verification was
 * already done, and this sandbox has no block device nodes to use
 * instead.
 *
 * The gap case is the point of the whole feature: after deleting a
 * partition from the middle, total free space and the largest usable
 * extent are different numbers, and a client subtracting partition
 * sizes from the disk size cannot tell them apart.
 */
static int run_sfdisk_script(const char *img, const char *script, const char *extra_arg)
{
	char cmd[512];

	if (extra_arg != NULL)
		snprintf(cmd, sizeof(cmd), "printf '%%s\\n' '%s' | /usr/sbin/sfdisk %s %s >/dev/null 2>&1",
		         script, extra_arg, img);
	else
		snprintf(cmd, sizeof(cmd), "printf '%%s\\n' '%s' | /usr/sbin/sfdisk %s >/dev/null 2>&1",
		         script, img);
	return system(cmd);
}

static int test_free_space(void)
{
	char img[] = "/tmp/cix_test_freespace_XXXXXX";
	char cmd[512];
	struct diskpart_free_space fs;
	int fd;
	int ok = 1;

	fd = mkstemp(img);
	if (fd < 0) {
		fprintf(stderr, "FAIL: mkstemp for free-space image\n");
		return 0;
	}
	close(fd);
	snprintf(cmd, sizeof(cmd), "truncate -s 1G %s", img);
	if (system(cmd) != 0) {
		fprintf(stderr, "FAIL: could not create free-space image\n");
		unlink(img);
		return 0;
	}

	/* A raw device with no table at all: sfdisk prints nothing and
	 * exits 0, which must NOT read as "partitioned and completely
	 * full" -- different problem, different fix. */
	diskpart_free_space_from_path(img, &fs);
	if (fs.has_table) {
		fprintf(stderr, "FAIL: an unpartitioned image reported has_table=1\n");
		ok = 0;
	}

	run_sfdisk_script(img, "label: gpt", NULL);
	diskpart_free_space_from_path(img, &fs);
	if (!fs.has_table || fs.total_free_bytes == 0 || fs.largest_free_sectors == 0) {
		fprintf(stderr,
		        "FAIL: empty GPT reported has_table=%d total=%llu largest=%llu -- an empty "
		        "table is all free space\n",
		        fs.has_table, fs.total_free_bytes, fs.largest_free_sectors);
		ok = 0;
	}

	run_sfdisk_script(img, "size=100MiB, type=linux, name=\"a\"", "--append");
	run_sfdisk_script(img, "size=200MiB, type=linux, name=\"b\"", "--append");
	diskpart_free_space_from_path(img, &fs);
	{
		unsigned long long before_total = fs.total_free_bytes;
		unsigned long long before_largest = fs.largest_free_sectors;

		if (fs.extent_count != 1) {
			fprintf(stderr, "FAIL: two partitions at the front should leave one free extent, got %d\n",
			        fs.extent_count);
			ok = 0;
		}

		/* Delete the FIRST partition, leaving a hole in the middle. */
		snprintf(cmd, sizeof(cmd), "/usr/sbin/sfdisk --delete %s 1 >/dev/null 2>&1", img);
		if (system(cmd) != 0) {
			fprintf(stderr, "FAIL: could not delete partition 1\n");
			ok = 0;
		}
		diskpart_free_space_from_path(img, &fs);
		if (fs.extent_count != 2) {
			fprintf(stderr, "FAIL: a deleted middle partition should leave two free extents, got %d\n",
			        fs.extent_count);
			ok = 0;
		}
		if (fs.total_free_bytes <= before_total) {
			fprintf(stderr, "FAIL: deleting a partition did not increase total free space\n");
			ok = 0;
		}
		/* The whole point: total grew, but the biggest single usable
		 * piece did not -- the freed 100MiB is its own separate hole.
		 * A client subtracting sizes would report the larger number
		 * and be wrong about what actually fits. */
		if (fs.largest_free_sectors != before_largest) {
			fprintf(stderr,
			        "FAIL: largest free extent changed (%llu -> %llu) when the freed space was a "
			        "separate hole -- total and largest must be distinct answers\n",
			        before_largest, fs.largest_free_sectors);
			ok = 0;
		}
	}

	unlink(img);
	return ok;
}

/*
 * Growing a partition (issue #94).
 *
 * The table half is driven against a real disk image, where sfdisk
 * behaves identically to a block device. The filesystem half is
 * verified separately against a plain ext4 image, because putting a
 * filesystem *inside* a partition of an image needs a loop device and
 * this sandbox has none -- so the two mechanisms are each proven, and
 * their combination is what needs a real box.
 */
static unsigned long long partition_sectors(const char *img, int partno)
{
	char cmd[512];
	FILE *f;
	char line[512];
	unsigned long long sectors = 0;
	int n = 0;

	snprintf(cmd, sizeof(cmd), "/usr/sbin/sfdisk -d %s 2>/dev/null", img);
	f = popen(cmd, "r");
	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *size = strstr(line, "size=");

		if (size == NULL)
			continue;
		n++;
		if (n == partno) {
			sectors = strtoull(size + 5, NULL, 10);
			break;
		}
	}
	pclose(f);
	return sectors;
}

static int test_partition_grow(void)
{
	char img[] = "/tmp/cix_test_grow_XXXXXX";
	char cmd[512];
	int fd;
	int ok = 1;
	unsigned long long before, after;

	fd = mkstemp(img);
	if (fd < 0) {
		fprintf(stderr, "FAIL: mkstemp for grow image\n");
		return 0;
	}
	close(fd);
	snprintf(cmd, sizeof(cmd), "truncate -s 1G %s", img);
	if (system(cmd) != 0) {
		unlink(img);
		fprintf(stderr, "FAIL: could not create grow image\n");
		return 0;
	}
	run_sfdisk_script(img, "label: gpt", NULL);
	run_sfdisk_script(img, "size=100MiB, type=linux, name=\"a\"", "--append");

	before = partition_sectors(img, 1);

	/* The table half: sfdisk -N grows the entry in place. */
	snprintf(cmd, sizeof(cmd),
	         "printf 'size=300MiB\\n' | /usr/sbin/sfdisk -N 1 --force %s >/dev/null 2>&1", img);
	if (system(cmd) != 0) {
		fprintf(stderr, "FAIL: sfdisk -N could not grow the partition\n");
		ok = 0;
	}
	after = partition_sectors(img, 1);
	if (after <= before) {
		fprintf(stderr, "FAIL: partition did not grow (%llu -> %llu sectors)\n", before, after);
		ok = 0;
	}

	/* And the safety property this whole feature rests on: sfdisk
	 * refuses to grow one partition over another, and leaves the table
	 * untouched when it does. Without that, a grow could silently eat
	 * the next partition. */
	run_sfdisk_script(img, "size=100MiB, type=linux, name=\"b\"", "--append");
	{
		unsigned long long p1_before = partition_sectors(img, 1);
		unsigned long long p2_before = partition_sectors(img, 2);

		snprintf(cmd, sizeof(cmd),
		         "printf 'size=800MiB\\n' | /usr/sbin/sfdisk -N 1 --force %s >/dev/null 2>&1", img);
		if (system(cmd) == 0) {
			fprintf(stderr, "FAIL: sfdisk accepted a grow that would overlap the next partition\n");
			ok = 0;
		}
		if (partition_sectors(img, 1) != p1_before || partition_sectors(img, 2) != p2_before) {
			fprintf(stderr, "FAIL: a refused grow still altered the table\n");
			ok = 0;
		}
	}
	unlink(img);

	/* The filesystem half, on its own image: after the space behind it
	 * grows, resize2fs makes the filesystem use it. Growing the table
	 * entry alone would leave the extra space invisible, which is why
	 * both halves are one operation. */
	{
		char fsimg[] = "/tmp/cix_test_growfs_XXXXXX";
		char blocks_before[64] = "";
		char blocks_after[64] = "";
		FILE *f;

		fd = mkstemp(fsimg);
		if (fd < 0) {
			fprintf(stderr, "FAIL: mkstemp for grow-fs image\n");
			return 0;
		}
		close(fd);
		snprintf(cmd, sizeof(cmd), "truncate -s 64M %s && /usr/sbin/mkfs.ext4 -q -F %s 2>/dev/null",
		         fsimg, fsimg);
		if (system(cmd) != 0) {
			fprintf(stderr, "FAIL: could not make an ext4 image\n");
			unlink(fsimg);
			return 0;
		}
		snprintf(cmd, sizeof(cmd),
		         "/usr/sbin/dumpe2fs -h %s 2>/dev/null | awk '/Block count/{print $3}'", fsimg);
		f = popen(cmd, "r");
		if (f != NULL) {
			if (fgets(blocks_before, sizeof(blocks_before), f) == NULL)
				blocks_before[0] = '\0';
			pclose(f);
		}
		snprintf(cmd, sizeof(cmd), "truncate -s 192M %s", fsimg);
		if (system(cmd) != 0)
			ok = 0;
		snprintf(cmd, sizeof(cmd), "/usr/sbin/resize2fs %s >/dev/null 2>&1", fsimg);
		if (system(cmd) != 0) {
			fprintf(stderr, "FAIL: resize2fs could not grow the filesystem\n");
			ok = 0;
		}
		snprintf(cmd, sizeof(cmd),
		         "/usr/sbin/dumpe2fs -h %s 2>/dev/null | awk '/Block count/{print $3}'", fsimg);
		f = popen(cmd, "r");
		if (f != NULL) {
			if (fgets(blocks_after, sizeof(blocks_after), f) == NULL)
				blocks_after[0] = '\0';
			pclose(f);
		}
		if (blocks_before[0] == '\0' || blocks_after[0] == '\0' ||
		    strtoull(blocks_after, NULL, 10) <= strtoull(blocks_before, NULL, 10)) {
			fprintf(stderr,
			        "FAIL: filesystem block count did not grow (\"%s\" -> \"%s\") -- the extra "
			        "space would be invisible to anything using it\n",
			        blocks_before, blocks_after);
			ok = 0;
		}
		unlink(fsimg);
	}
	return ok;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	int ok = 1;
	struct cix_response r;
	char part_name[64] = "";
	char part_parent[64] = "";
	char other_part_name[64] = "";
	char other_part_parent[64] = "";
	int partitions_found = 0;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

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

	/*
	 * 1. GET /v1/disks: real, live kernel state -- find at least one
	 * real partition and confirm its is_partition/parent_disk/model/
	 * removable fields, and that its parent appears in the same list
	 * as a whole-disk (is_partition == false) entry of the same name.
	 * Also remembers a second partition with a *different* parent, if
	 * one exists, for the wrong-parent check below.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/disks", NULL, &r) != 0 || r.status != 200 ||
	    r.json == NULL) {
		fprintf(stderr, "FAIL: GET /v1/disks failed (status %d)\n", r.status);
		ok = 0;
	}
	if (ok) {
		const struct json_value *disks = json_object_get(r.json, "disks");
		size_t i, j;

		if (disks == NULL || disks->type != JSON_ARRAY) {
			fprintf(stderr, "FAIL: GET /v1/disks: no disks array\n");
			ok = 0;
		} else {
			for (i = 0; i < disks->u.array.count; i++) {
				const struct json_value *d = disks->u.array.items[i];

				if (!jbool(d, "is_partition"))
					continue;

				if (part_name[0] == '\0') {
					snprintf(part_name, sizeof(part_name), "%s", jstr(d, "name"));
					snprintf(part_parent, sizeof(part_parent), "%s", jstr(d, "parent_disk"));
				} else if (other_part_name[0] == '\0' &&
				           strcmp(jstr(d, "parent_disk"), part_parent) != 0) {
					snprintf(other_part_name, sizeof(other_part_name), "%s", jstr(d, "name"));
					snprintf(other_part_parent, sizeof(other_part_parent), "%s",
					         jstr(d, "parent_disk"));
				}
				partitions_found++;

				if (jstr(d, "parent_disk")[0] == '\0') {
					fprintf(stderr, "FAIL: partition %s has empty parent_disk\n",
					        jstr(d, "name"));
					ok = 0;
				}
				if (jstr(d, "model")[0] != '\0') {
					fprintf(stderr, "FAIL: partition %s has non-empty model %s\n",
					        jstr(d, "name"), jstr(d, "model"));
					ok = 0;
				}
				if (jbool(d, "removable")) {
					fprintf(stderr, "FAIL: partition %s reported removable\n",
					        jstr(d, "name"));
					ok = 0;
				}

				/* Its parent must appear as its own, non-partition entry. */
				{
					int parent_found = 0;

					for (j = 0; j < disks->u.array.count; j++) {
						const struct json_value *p = disks->u.array.items[j];

						if (strcmp(jstr(p, "name"), jstr(d, "parent_disk")) == 0) {
							parent_found = 1;
							if (jbool(p, "is_partition")) {
								fprintf(stderr,
								        "FAIL: %s's parent %s is itself "
								        "reported as a partition\n",
								        jstr(d, "name"), jstr(p, "name"));
								ok = 0;
							}
							break;
						}
					}
					if (!parent_found) {
						fprintf(stderr,
						        "FAIL: %s's parent_disk %s has no whole-disk entry\n",
						        jstr(d, "name"), jstr(d, "parent_disk"));
						ok = 0;
					}
				}
			}
		}
	}
	cix_response_free(&r);

	if (ok && partitions_found == 0) {
		fprintf(stderr, "FAIL: no real partitions found on this sandbox's own disks -- "
		                "expected at least one (this test needs real, pre-existing "
		                "partitions to verify disk_enumerate()'s new partition pass against)\n");
		ok = 0;
	}

	/* 2. create-table/add-partition on a nonexistent disk -> 404. */
	if (ok) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/disks/nosuchdisk12345/partition-table",
		                       "{\"confirm_disk_name\":\"nosuchdisk12345\"}", &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: partition-table on nonexistent disk: expected 404, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}
	if (ok) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/disks/nosuchdisk12345/partitions",
		                       "{\"name\":\"data\"}", &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: add-partition on nonexistent disk: expected 404, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}

	/* 3. create-table targeting a real partition (not a whole disk) -> 400. */
	if (ok) {
		char path[128], body[128];

		snprintf(path, sizeof(path), "/v1/disks/%s/partition-table", part_name);
		snprintf(body, sizeof(body), "{\"confirm_disk_name\":\"%s\"}", part_name);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", path, body, &r) != 0 || r.status != 400) {
			fprintf(stderr,
			        "FAIL: partition-table targeting a real partition (%s): expected 400, got %d\n",
			        part_name, r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}

	/* 4. create-table on a real whole disk with a wrong confirm -> 400. */
	if (ok) {
		char path[128];

		snprintf(path, sizeof(path), "/v1/disks/%s/partition-table", part_parent);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", path, "{\"confirm_disk_name\":\"not-the-right-name\"}",
		                       &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: partition-table with wrong confirm: expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}

	/* 5. add-partition on a real whole disk with no "name" field -> 400. */
	if (ok) {
		char path[128];

		snprintf(path, sizeof(path), "/v1/disks/%s/partitions", part_parent);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", path, "{}", &r) != 0 || r.status != 400) {
			fprintf(stderr, "FAIL: add-partition with no name: expected 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}

	/* 6. delete a nonexistent partition -> 404. */
	if (ok) {
		char path[160];

		snprintf(path, sizeof(path), "/v1/disks/%s/partitions/nosuchpart999", part_parent);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 404) {
			fprintf(stderr, "FAIL: delete nonexistent partition: expected 404, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}

	/* 7. delete targeting a whole disk (not a partition) -> 400. */
	if (ok) {
		char path[160];

		snprintf(path, sizeof(path), "/v1/disks/%s/partitions/%s", part_parent, part_parent);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 400) {
			fprintf(stderr, "FAIL: delete a whole disk as a partition: expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}

	/* 8. delete a real partition via the wrong parent's URL -> 404. */
	if (ok && other_part_name[0] != '\0') {
		char path[160];

		snprintf(path, sizeof(path), "/v1/disks/%s/partitions/%s", other_part_parent, part_name);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 404) {
			fprintf(stderr, "FAIL: delete %s via wrong parent %s: expected 404, got %d\n",
			        part_name, other_part_parent, r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}

	/* 9. invalid disk-name charset -> 400, distinct from not-found. */
	if (ok) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/disks/bad$name/partition-table",
		                       "{\"confirm_disk_name\":\"bad$name\"}", &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: partition-table with invalid disk name: expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}

	/*
	 * 10. A disk with a role assigned directly to it must reject both
	 * create-table and add-partition (409) -- assigning a diskrole is
	 * pure daemon-side bookkeeping, so this is safe to exercise for
	 * real against part_parent without touching any real disk content.
	 */
	if (ok) {
		char body[128];

		snprintf(body, sizeof(body), "{\"disk_name\":\"%s\",\"role\":\"backup\"}", part_parent);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: assigning backup role to %s: expected 201, got %d\n",
			        part_parent, r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}
	if (ok) {
		char path[128], body[128];

		snprintf(path, sizeof(path), "/v1/disks/%s/partition-table", part_parent);
		snprintf(body, sizeof(body), "{\"confirm_disk_name\":\"%s\"}", part_parent);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", path, body, &r) != 0 || r.status != 409) {
			fprintf(stderr,
			        "FAIL: partition-table on a role-assigned disk: expected 409, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}
	if (ok) {
		char path[128];

		snprintf(path, sizeof(path), "/v1/disks/%s/partitions", part_parent);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", path, "{\"name\":\"data\"}", &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: add-partition on a role-assigned disk: expected 409, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}
	{
		char path[128];

		snprintf(path, sizeof(path), "/v1/diskroles/%s", part_parent);
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", path, NULL, &r);
		cix_response_free(&r);
	}

	/*
	 * 11. A partition with a role assigned to it directly must reject
	 * deletion (409) -- proves diskrole.c's name-agnostic design
	 * already works for a partition name with zero changes, and
	 * diskpart_delete()'s own HAS_ROLE check.
	 */
	if (ok) {
		char body[128];

		snprintf(body, sizeof(body), "{\"disk_name\":\"%s\",\"role\":\"backup\"}", part_name);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: assigning backup role to partition %s: expected 201, got %d\n",
			        part_name, r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}
	if (ok) {
		char path[160];

		snprintf(path, sizeof(path), "/v1/disks/%s/partitions/%s", part_parent, part_name);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 409) {
			fprintf(stderr,
			        "FAIL: deleting a role-assigned partition %s: expected 409, got %d\n",
			        part_name, r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}
	{
		char path[128];

		snprintf(path, sizeof(path), "/v1/diskroles/%s", part_name);
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", path, NULL, &r);
		cix_response_free(&r);
	}

	if (!test_mount_attribution())
		ok = 0;
	if (!test_fs_probe())
		ok = 0;
	if (!test_free_space())
		ok = 0;
	if (!test_partition_grow())
		ok = 0;

	if (ok)
		printf("DISKPART RESULT: PASS\n");
	else
		printf("DISKPART RESULT: FAIL\n");

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);
	return ok ? 0 : 1;
}
