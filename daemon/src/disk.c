#include "disk.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYS_BLOCK_DIR "/sys/class/block"

/* Same small helper device.c's own read_sysfs_attr() is, duplicated
 * rather than shared across modules with no other coupling -- this
 * project's own established precedent (each device-class module reads
 * its own sysfs attributes directly, see device.c/devicemap.c). */
static int read_sysfs_attr(const char *path, char *out, size_t out_size)
{
	FILE *f;
	size_t n;

	out[0] = '\0';
	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	if (fgets(out, (int)out_size, f) == NULL) {
		fclose(f);
		out[0] = '\0';
		return -1;
	}
	fclose(f);
	n = strlen(out);
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
		out[--n] = '\0';
	return 0;
}

static int sysfs_path_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/*
 * Maps a partition device name (e.g. "vda5", "sda1", "nvme0n1p3") back
 * to its parent whole-disk name ("vda", "sda", "nvme0n1"). Strips a
 * trailing run of digits, then -- only for the nvme-style "pN" suffix
 * convention -- also strips a trailing 'p'. A name with no trailing
 * digits at all (already a whole disk) is returned unchanged.
 */
static void disk_name_from_partition(const char *part_name, char *out, size_t out_size)
{
	size_t len = strlen(part_name);
	size_t end = len;

	while (end > 0 && isdigit((unsigned char)part_name[end - 1]))
		end--;
	if (end > 0 && end < len && part_name[end - 1] == 'p' &&
	    end >= 2 && isdigit((unsigned char)part_name[end - 2]))
		end--;
	if (end == 0 || end == len)
		end = len; /* no trailing digits -- already a whole disk name */
	snprintf(out, out_size, "%.*s", (int)end, part_name);
}

/*
 * Resolves os_containers_dir to its backing partition's parent whole
 * disk name via /proc/mounts -- mirrors main.c's own
 * resolve_backing_device() (longest-matching mountpoint wins), kept as
 * a separate, self-contained copy here rather than shared: the two
 * differ in what they do with the result (a quotactl(2) "special"
 * argument there, a disk name here) and in the input this module
 * genuinely has no daemon-layer state to reach for on its own (see
 * disk.h's own doc comment on the os_containers_dir parameter).
 */
static int resolve_os_disk_name(const char *os_containers_dir, char *out, size_t out_size)
{
	char real_path[PATH_MAX];
	FILE *f;
	char line[PATH_MAX * 2];
	char best_device[PATH_MAX] = "";
	size_t best_len = 0;
	const char *base;

	if (os_containers_dir == NULL || realpath(os_containers_dir, real_path) == NULL)
		return -1;

	f = fopen("/proc/mounts", "r");
	if (f == NULL)
		return -1;

	while (fgets(line, sizeof(line), f) != NULL) {
		char device[PATH_MAX];
		char mountpoint[PATH_MAX];
		size_t mp_len;

		if (sscanf(line, "%4095s %4095s", device, mountpoint) != 2)
			continue;
		mp_len = strlen(mountpoint);
		if (strncmp(real_path, mountpoint, mp_len) != 0)
			continue;
		if (real_path[mp_len] != '\0' && real_path[mp_len] != '/')
			continue;
		if (mp_len < best_len)
			continue;
		best_len = mp_len;
		snprintf(best_device, sizeof(best_device), "%s", device);
	}
	fclose(f);

	if (best_device[0] == '\0')
		return -1;

	base = strrchr(best_device, '/');
	base = (base != NULL) ? base + 1 : best_device;
	disk_name_from_partition(base, out, out_size);
	return 0;
}

/*
 * ADR-0142: fills in e's own I/O counters from /sys/block/<name>/stat
 * -- a fixed-format line of whitespace-separated integers (see
 * Documentation/admin-guide/iostats.rst upstream); this project only
 * ever reads fields 1 (reads completed), 3 (sectors read), 5 (writes
 * completed), 7 (sectors written), and 10 (time spent doing I/Os, ms).
 * Leaves every field 0 (already the case, out is always memset by the
 * caller) if the file can't be read or doesn't parse -- never fatal to
 * the rest of disk_enumerate().
 */
static void fill_io_stats(struct discovered_disk *e)
{
	char path[PATH_MAX];
	FILE *f;
	unsigned long long f1, f2, f3, f4, f5, f6, f7, f8, f9, f10;

	snprintf(path, sizeof(path), "%s/%s/stat", SYS_BLOCK_DIR, e->name);
	f = fopen(path, "r");
	if (f == NULL)
		return;
	if (fscanf(f, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &f1, &f2, &f3, &f4, &f5, &f6,
	           &f7, &f8, &f9, &f10) == 10) {
		e->reads_completed = f1;
		e->sectors_read = f3;
		e->writes_completed = f5;
		e->sectors_written = f7;
		e->io_time_ms = f10;
	}
	fclose(f);
}

/*
 * ADR-0142: real statvfs(2) capacity for e's own mount_path -- caller's
 * responsibility to only call this once e->mounted is already known
 * true (fill_mount_status() below runs first). Leaves both fields 0 on
 * any statvfs(2) failure -- never fatal.
 */
static void fill_capacity(struct discovered_disk *e)
{
	struct statvfs st;

	if (statvfs(e->mount_path, &st) != 0)
		return;
	e->used_bytes = ((unsigned long long)st.f_blocks - (unsigned long long)st.f_bfree) *
	                (unsigned long long)st.f_frsize;
	e->free_bytes = (unsigned long long)st.f_bavail * (unsigned long long)st.f_frsize;
}

/*
 * One real pass over /proc/mounts (the same ground-truth source
 * resolve_os_disk_name() above already trusts), recording the first
 * mountpoint found for each entry in out[]. O(disks * mounts) with both
 * counts always small in practice (a real host has a handful of disks
 * and a few dozen mounts at most) -- not worth a hash table for this.
 *
 * Two distinct things are recorded, and conflating them was a real bug
 * (see disk.h's own `mounted` comment): the mounted device's OWN entry
 * gets mounted/mount_path, and its parent whole disk separately gets
 * has_mounted_partition. Before partitions were enumerated at all, only
 * the second existed and was stored in the first's fields, which read
 * correctly right up until ADR-0158 put partitions in out[] too.
 */
void disk_fill_mount_status_from(const char *mounts_path, struct discovered_disk *out,
                                 int count)
{
	FILE *f;
	char line[PATH_MAX * 2];

	f = fopen(mounts_path, "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL) {
		char device[PATH_MAX];
		char mountpoint[PATH_MAX];
		char disk_name[32];
		const char *base;
		int i;

		if (sscanf(line, "%4095s %4095s", device, mountpoint) != 2)
			continue;
		base = strrchr(device, '/');
		base = (base != NULL) ? base + 1 : device;
		if (base[0] == '\0')
			continue;
		disk_name_from_partition(base, disk_name, sizeof(disk_name));
		for (i = 0; i < count; i++) {
			/* The device that is actually mounted. */
			if (!out[i].mounted && strcmp(out[i].name, base) == 0) {
				out[i].mounted = 1;
				snprintf(out[i].mount_path, sizeof(out[i].mount_path), "%s", mountpoint);
				fill_capacity(&out[i]);
			}
			/* Its parent whole disk, when the mount is a partition --
			 * disk_name_from_partition() returns `base` unchanged for a
			 * whole-disk device, so the strcmp keeps this to the
			 * genuine parent case only. */
			if (strcmp(base, disk_name) != 0 && strcmp(out[i].name, disk_name) == 0)
				out[i].has_mounted_partition = 1;
		}
	}
	fclose(f);
}

/*
 * The real-host entry point. Split from the function above purely so a
 * test can feed it a synthetic mounts file: the attribution logic is
 * safety-critical (it gates whether a mounted partition can be deleted)
 * and had a real bug in it, but /proc/mounts cannot be arranged to order
 * inside a test, and this sandbox has no loop devices to mount anything
 * on. See test_diskpart.c.
 */
static void fill_mount_status(struct discovered_disk *out, int count)
{
	disk_fill_mount_status_from("/proc/mounts", out, count);
}

/*
 * Fills in the fields common to both a whole disk and a partition entry
 * (name/dev_path/size_bytes/io_stats) -- everything disk_enumerate()'s
 * two passes below share. model/removable/is_os_disk are set by the
 * caller afterward, since they differ (or need a parent lookup) between
 * the two passes.
 */
static void fill_common(struct discovered_disk *e, const char *d_name, const char *size_str)
{
	memset(e, 0, sizeof(*e));
	snprintf(e->name, sizeof(e->name), "%s", d_name);
	snprintf(e->dev_path, sizeof(e->dev_path), "/dev/%s", d_name);
	/* sysfs "size" is always in 512-byte sectors, regardless of the
	 * device's own real logical block size. */
	e->size_bytes = strtoull(size_str, NULL, 10) * 512ULL;
	fill_io_stats(e);
}

int disk_enumerate(struct discovered_disk *out, int cap, const char *os_containers_dir)
{
	DIR *d;
	struct dirent *ent;
	int count = 0;
	char os_disk_name[32] = "";

	resolve_os_disk_name(os_containers_dir, os_disk_name, sizeof(os_disk_name));

	/* Pass 1: whole disks only -- every partition's own is_os_disk
	 * (pass 2 below) is propagated from its parent's entry here, so
	 * every whole disk must already be in out[] before pass 2 looks
	 * any of them up; readdir() order is not guaranteed to put a
	 * parent before its own partitions. */
	d = opendir(SYS_BLOCK_DIR);
	if (d == NULL)
		return 0;

	while (count < cap && (ent = readdir(d)) != NULL) {
		char base[PATH_MAX];
		char attr_path[PATH_MAX];
		char partition_marker[PATH_MAX];
		char size_str[32];
		struct discovered_disk *e;

		if (ent->d_name[0] == '.')
			continue;

		snprintf(base, sizeof(base), "%s/%s", SYS_BLOCK_DIR, ent->d_name);

		snprintf(partition_marker, sizeof(partition_marker), "%s/partition", base);
		if (sysfs_path_exists(partition_marker))
			continue; /* handled in pass 2 below */

		snprintf(attr_path, sizeof(attr_path), "%s/size", base);
		if (read_sysfs_attr(attr_path, size_str, sizeof(size_str)) != 0)
			continue; /* not a real block device (e.g. "loop-control") */

		e = &out[count];
		fill_common(e, ent->d_name, size_str);

		snprintf(attr_path, sizeof(attr_path), "%s/device/model", base);
		read_sysfs_attr(attr_path, e->model, sizeof(e->model));

		{
			char removable_str[8];

			snprintf(attr_path, sizeof(attr_path), "%s/removable", base);
			if (read_sysfs_attr(attr_path, removable_str, sizeof(removable_str)) == 0)
				e->removable = (strcmp(removable_str, "1") == 0);
		}

		if (os_disk_name[0] != '\0' && strcmp(e->name, os_disk_name) == 0)
			e->is_os_disk = 1;

		count++;
	}
	closedir(d);

	/* Pass 2: partitions -- a second, fresh directory walk (cheap;
	 * /sys/class/block has at most a few dozen entries on a real
	 * host) rather than buffering dirents from pass 1, since nothing
	 * else in this function needs to remember them. */
	d = opendir(SYS_BLOCK_DIR);
	if (d == NULL)
		return count;

	while (count < cap && (ent = readdir(d)) != NULL) {
		char base[PATH_MAX];
		char attr_path[PATH_MAX];
		char partition_marker[PATH_MAX];
		char size_str[32];
		char parent_name[32];
		struct discovered_disk *e;
		int i;

		if (ent->d_name[0] == '.')
			continue;

		snprintf(base, sizeof(base), "%s/%s", SYS_BLOCK_DIR, ent->d_name);

		snprintf(partition_marker, sizeof(partition_marker), "%s/partition", base);
		if (!sysfs_path_exists(partition_marker))
			continue; /* a whole disk -- already handled in pass 1 */

		snprintf(attr_path, sizeof(attr_path), "%s/size", base);
		if (read_sysfs_attr(attr_path, size_str, sizeof(size_str)) != 0)
			continue;

		e = &out[count];
		fill_common(e, ent->d_name, size_str);
		e->is_partition = 1;

		disk_name_from_partition(ent->d_name, parent_name, sizeof(parent_name));
		snprintf(e->parent_disk, sizeof(e->parent_disk), "%s", parent_name);
		for (i = 0; i < count; i++) {
			if (strcmp(out[i].name, parent_name) == 0) {
				e->is_os_disk = out[i].is_os_disk;
				break;
			}
		}

		count++;
	}
	closedir(d);

	fill_mount_status(out, count);
	return count;
}

void disk_write_json_one(const struct discovered_disk *d, struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, d->name);
	jw_key(w, "dev_path");
	jw_str(w, d->dev_path);
	jw_key(w, "model");
	jw_str(w, d->model);
	jw_key(w, "size_bytes");
	jw_int(w, (long long)d->size_bytes);
	jw_key(w, "removable");
	jw_bool(w, d->removable);
	jw_key(w, "is_os_disk");
	jw_bool(w, d->is_os_disk);
	jw_key(w, "mounted");
	jw_bool(w, d->mounted);
	jw_key(w, "mount_path");
	jw_str(w, d->mount_path);
	/* Whole disks only, and always false for a partition. Reported
	 * because it is the reason a repartition gets refused, and an
	 * operator with no shell has no other way to see it. */
	jw_key(w, "has_mounted_partition");
	jw_bool(w, d->has_mounted_partition);
	jw_key(w, "reads_completed");
	jw_int(w, (long long)d->reads_completed);
	jw_key(w, "writes_completed");
	jw_int(w, (long long)d->writes_completed);
	jw_key(w, "sectors_read");
	jw_int(w, (long long)d->sectors_read);
	jw_key(w, "sectors_written");
	jw_int(w, (long long)d->sectors_written);
	jw_key(w, "io_time_ms");
	jw_int(w, (long long)d->io_time_ms);
	jw_key(w, "used_bytes");
	jw_int(w, (long long)d->used_bytes);
	jw_key(w, "free_bytes");
	jw_int(w, (long long)d->free_bytes);
	jw_key(w, "is_partition");
	jw_bool(w, d->is_partition);
	jw_key(w, "parent_disk");
	jw_str(w, d->parent_disk);
	jw_obj_close(w);
}

void disk_write_json_list(struct json_writer *w, const char *os_containers_dir)
{
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n = disk_enumerate(disks, DISK_ENUM_MAX, os_containers_dir);
	int i;

	jw_arr_open(w);
	for (i = 0; i < n; i++)
		disk_write_json_one(&disks[i], w);
	jw_arr_close(w);
}
