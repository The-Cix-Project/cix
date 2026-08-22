/*
 * Partition-level disk management (ROADMAP.md task #844) end-to-end
 * test: proves disk_enumerate()'s new partition-reporting pass and
 * diskpart.c's own validation logic, both over real HTTP against a
 * real thincd subprocess.
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
	    "/dev/sda1 /var/lib/thinc/disks/sda1 ext4 rw,relatime 0 0\n"
	    "/dev/sdb /mnt/whole ext4 rw,relatime 0 0\n"
	    "proc /proc proc rw 0 0\n";
	struct discovered_disk d[4];
	char path[] = "/tmp/thinc_test_mounts_XXXXXX";
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
	if (!d[1].mounted || strcmp(d[1].mount_path, "/var/lib/thinc/disks/sda1") != 0) {
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
		char path[] = "/tmp/thinc_test_fsprobe_XXXXXX";
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
		char path[] = "/tmp/thinc_test_fsprobe_XXXXXX";
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

		disk_probe_fs_type("/dev/thinc-no-such-device", got, sizeof(got));
		if (got[0] != '\0') {
			fprintf(stderr, "FAIL: missing device probed as \"%s\", expected \"\"\n", got);
			ok = 0;
		}
	}
	return ok;
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;
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

	kx_client_init(&client, "127.0.0.1", TEST_PORT);
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
	if (kx_client_request(&client, "GET", "/v1/disks", NULL, &r) != 0 || r.status != 200 ||
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
	kx_response_free(&r);

	if (ok && partitions_found == 0) {
		fprintf(stderr, "FAIL: no real partitions found on this sandbox's own disks -- "
		                "expected at least one (this test needs real, pre-existing "
		                "partitions to verify disk_enumerate()'s new partition pass against)\n");
		ok = 0;
	}

	/* 2. create-table/add-partition on a nonexistent disk -> 404. */
	if (ok) {
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/disks/nosuchdisk12345/partition-table",
		                       "{\"confirm_disk_name\":\"nosuchdisk12345\"}", &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: partition-table on nonexistent disk: expected 404, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}
	if (ok) {
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/disks/nosuchdisk12345/partitions",
		                       "{\"name\":\"data\"}", &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: add-partition on nonexistent disk: expected 404, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 3. create-table targeting a real partition (not a whole disk) -> 400. */
	if (ok) {
		char path[128], body[128];

		snprintf(path, sizeof(path), "/v1/disks/%s/partition-table", part_name);
		snprintf(body, sizeof(body), "{\"confirm_disk_name\":\"%s\"}", part_name);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", path, body, &r) != 0 || r.status != 400) {
			fprintf(stderr,
			        "FAIL: partition-table targeting a real partition (%s): expected 400, got %d\n",
			        part_name, r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 4. create-table on a real whole disk with a wrong confirm -> 400. */
	if (ok) {
		char path[128];

		snprintf(path, sizeof(path), "/v1/disks/%s/partition-table", part_parent);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", path, "{\"confirm_disk_name\":\"not-the-right-name\"}",
		                       &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: partition-table with wrong confirm: expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 5. add-partition on a real whole disk with no "name" field -> 400. */
	if (ok) {
		char path[128];

		snprintf(path, sizeof(path), "/v1/disks/%s/partitions", part_parent);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", path, "{}", &r) != 0 || r.status != 400) {
			fprintf(stderr, "FAIL: add-partition with no name: expected 400, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 6. delete a nonexistent partition -> 404. */
	if (ok) {
		char path[160];

		snprintf(path, sizeof(path), "/v1/disks/%s/partitions/nosuchpart999", part_parent);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 404) {
			fprintf(stderr, "FAIL: delete nonexistent partition: expected 404, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 7. delete targeting a whole disk (not a partition) -> 400. */
	if (ok) {
		char path[160];

		snprintf(path, sizeof(path), "/v1/disks/%s/partitions/%s", part_parent, part_parent);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 400) {
			fprintf(stderr, "FAIL: delete a whole disk as a partition: expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 8. delete a real partition via the wrong parent's URL -> 404. */
	if (ok && other_part_name[0] != '\0') {
		char path[160];

		snprintf(path, sizeof(path), "/v1/disks/%s/partitions/%s", other_part_parent, part_name);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 404) {
			fprintf(stderr, "FAIL: delete %s via wrong parent %s: expected 404, got %d\n",
			        part_name, other_part_parent, r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 9. invalid disk-name charset -> 400, distinct from not-found. */
	if (ok) {
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/disks/bad$name/partition-table",
		                       "{\"confirm_disk_name\":\"bad$name\"}", &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: partition-table with invalid disk name: expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
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
		if (kx_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: assigning backup role to %s: expected 201, got %d\n",
			        part_parent, r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}
	if (ok) {
		char path[128], body[128];

		snprintf(path, sizeof(path), "/v1/disks/%s/partition-table", part_parent);
		snprintf(body, sizeof(body), "{\"confirm_disk_name\":\"%s\"}", part_parent);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", path, body, &r) != 0 || r.status != 409) {
			fprintf(stderr,
			        "FAIL: partition-table on a role-assigned disk: expected 409, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}
	if (ok) {
		char path[128];

		snprintf(path, sizeof(path), "/v1/disks/%s/partitions", part_parent);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", path, "{\"name\":\"data\"}", &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: add-partition on a role-assigned disk: expected 409, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}
	{
		char path[128];

		snprintf(path, sizeof(path), "/v1/diskroles/%s", part_parent);
		memset(&r, 0, sizeof(r));
		kx_client_request(&client, "DELETE", path, NULL, &r);
		kx_response_free(&r);
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
		if (kx_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: assigning backup role to partition %s: expected 201, got %d\n",
			        part_name, r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}
	if (ok) {
		char path[160];

		snprintf(path, sizeof(path), "/v1/disks/%s/partitions/%s", part_parent, part_name);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 409) {
			fprintf(stderr,
			        "FAIL: deleting a role-assigned partition %s: expected 409, got %d\n",
			        part_name, r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}
	{
		char path[128];

		snprintf(path, sizeof(path), "/v1/diskroles/%s", part_name);
		memset(&r, 0, sizeof(r));
		kx_client_request(&client, "DELETE", path, NULL, &r);
		kx_response_free(&r);
	}

	if (!test_mount_attribution())
		ok = 0;
	if (!test_fs_probe())
		ok = 0;

	if (ok)
		printf("DISKPART RESULT: PASS\n");
	else
		printf("DISKPART RESULT: FAIL\n");

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);
	return ok ? 0 : 1;
}
