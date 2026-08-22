#ifndef DISK_H
#define DISK_H

#include "json.h"

/*
 * Real host block devices (whole disks, and -- since partition-level
 * disk management, ROADMAP.md task #844 -- their partitions too),
 * discovered fresh from sysfs on every call -- the same "never
 * persisted, hardware this daemon doesn't create or own, re-enumerated
 * every time" convention device.c's own struct discovered_device
 * already established for USB/PCI/net/GPU (ADR-0012's atomic-JSON
 * persistence is for daemon-owned state, not this).
 *
 * 64 (this constant's value until task #844) proved too small in
 * direct, real testing: a dev/build sandbox whose /sys/class/block is
 * shared with its own LXC host ends up seeing every one of the host's
 * own unrelated LVM dm-* volumes (72 of them, confirmed directly) in
 * the same enumeration -- readdir() order is not guaranteed to put a
 * real target disk's own whole-disk/partition entries before that
 * clutter, so the old cap could silently truncate a real disk (or, as
 * found here, all of its partitions) out of the result entirely with
 * no error. Not a new problem task #844 introduced -- whole-disk
 * enumeration already shared this same fixed-size array -- but adding
 * partitions to the same budget makes it easier to hit, which is how
 * it was actually found. 256 comfortably covers this sandbox's own
 * real 89-entry /sys/class/block plus headroom for a real target's own
 * disks and their partitions; a plain stack array of this size (a few
 * hundred bytes per entry) is still trivially cheap for every existing
 * single-level call site.
 */
#define DISK_ENUM_MAX 256

struct discovered_disk {
	char name[32];		/* e.g. "vda", "sdb", "nvme0n1" -- no "/dev/" prefix */
	char dev_path[48];	/* "/dev/<name>" */
	char model[128];	/* empty if sysfs has none */
	unsigned long long size_bytes;
	int removable;
	/*
	 * True for the one disk backing CONTAINERS_DIR (main.c's
	 * CONTAINERS_DEVICE under a real --init-mode boot) -- the fixed
	 * OS-disk layout (ESP/root-a/root-b/config/containers) stays
	 * exactly as the installer laid it out and is never a candidate
	 * for a role assignment of its own; every other disk is.
	 */
	int is_os_disk;
	/*
	 * Real, current ground truth from /proc/mounts -- deliberately NOT
	 * derived from diskformat.c's own job-state (DISKFORMAT_STATE_READY
	 * is purely in-memory, per-daemon-process, and forgotten across a
	 * restart even though the real mount persists; it also only ever
	 * knows about disks *this daemon itself* formatted, never one an
	 * operator mounted by hand or that survived from before this
	 * mechanism existed).
	 *
	 * True when THIS exact device appears in /proc/mounts, with
	 * mount_path its own real mountpoint. This used to attribute a
	 * mounted partition to its parent whole disk instead, which was
	 * correct while only whole disks were ever enumerated, and became
	 * wrong the moment ADR-0158 started reporting partitions too: a
	 * genuinely mounted partition reported mounted=0 while its parent
	 * reported mounted=1 at a path that was not its own. Confirmed live
	 * on real hardware, in both directions -- unmounting the mounted
	 * partition was refused as "not mounted", and DELETE of that
	 * partition succeeded while its filesystem was live, because the
	 * guard meant to stop exactly that reads this flag.
	 *
	 * The first match found wins, matching disk_enumerate()'s own
	 * precedent (resolve_os_disk_name()'s longest-match walk) of
	 * treating /proc/mounts as the one real source of truth rather than
	 * anything this daemon merely believes.
	 */
	int mounted;
	char mount_path[256];
	/*
	 * Whole disks only: true when any partition ON this disk is
	 * mounted, even though the disk device itself is not.
	 *
	 * Split out from `mounted` rather than folded into it because the
	 * two answer different questions and exactly one caller wants this
	 * one: rewriting or growing a partition table is unsafe while
	 * anything on the disk is in use (diskpart.c), whereas every other
	 * caller -- storage placement, volume host paths, format's remount,
	 * migration -- is asking "is this specific device mounted, and
	 * where", and needs the truthful per-device answer above.
	 */
	int has_mounted_partition;
	/*
	 * The filesystem actually on this device, probed from its own
	 * superblock (issue #90) -- "ext4", "btrfs", "vfat", "swap",
	 * "squashfs", or "" for none recognised. Independent of any role:
	 * the OS layout carries no role at all, and its partitions are
	 * exactly where "is this formatted, and as what" is least
	 * guessable. Distinct from diskrole_lookup_fs_type(), which is a
	 * persisted note of what a role-assigned disk was last mounted as,
	 * used to choose a mount type rather than to report truth.
	 */
	char fs_type[16];
	/*
	 * ADR-0142: real, live I/O counters straight from the kernel's own
	 * per-block-device accounting (/sys/block/<name>/stat -- see
	 * Documentation/admin-guide/iostats.rst upstream), fields 1/5/3/7/10
	 * respectively -- a monotonically-increasing lifetime-since-boot
	 * counter, the same shape /proc/net/dev-style counters this daemon
	 * already exposes elsewhere (container network stats) use; a client
	 * polling this endpoint computes its own rate by differencing two
	 * samples, this module never tracks a delta of its own. io_time_ms
	 * (field 10, "time spent doing I/Os") is this device's own I/O-busy
	 * time -- the closest real, cheaply-available proxy for "disk I/O
	 * delay" the kernel's own per-block-device stat file offers without
	 * cgroup io.stat/PSI-level integration, which this daemon has no
	 * other use for and isn't worth wiring up solely for this one field.
	 * All zero if /sys/block/<name>/stat couldn't be read.
	 */
	unsigned long long reads_completed;
	unsigned long long writes_completed;
	unsigned long long sectors_read;
	unsigned long long sectors_written;
	unsigned long long io_time_ms;
	/*
	 * ADR-0142: real statvfs(2) capacity, only meaningful (and only
	 * populated) when `mounted` -- an unmounted disk has no filesystem
	 * context to ask. Both zero when not mounted or if statvfs(2) itself
	 * failed; a client should treat "not mounted" (already reported) as
	 * the reason, not infer it from these being zero.
	 */
	unsigned long long used_bytes;
	unsigned long long free_bytes;
	/*
	 * Partition-level disk management: true for an entry that is itself
	 * one partition of a larger disk (e.g. "sdb1", "nvme0n1p1"), rather
	 * than an independently-addressable whole disk. parent_disk is that
	 * partition's own whole-disk name ("sdb", "nvme0n1") -- empty when
	 * !is_partition. A partition entry is otherwise a full, ordinary
	 * struct discovered_disk (same name/dev_path/size_bytes/mounted/
	 * is_os_disk/etc. fields, all populated the same way) so that
	 * diskrole.c/diskformat.c, which only ever operate on a disk's own
	 * name/dev_path/is_os_disk, work identically on a partition with no
	 * changes of their own -- model/removable are always empty/0 for a
	 * partition (neither sysfs attribute exists per-partition, only on
	 * the parent whole disk). is_os_disk is propagated from the parent:
	 * every partition of the real OS disk is just as much off-limits to
	 * role assignment/reformatting as the whole OS disk itself.
	 */
	int is_partition;
	char parent_disk[32];
};

/*
 * Walks /sys/class/block fresh, writing up to cap entries into out: every
 * whole disk, plus every partition on it (anything with a "partition"
 * sysfs attribute -- is_partition/parent_disk above). Returns the count
 * written.
 *
 * os_containers_dir, if non-NULL, is resolved (realpath + a /proc/mounts
 * walk, the same "find the owning mount" algorithm main.c's own
 * resolve_backing_device() already uses for quota purposes) to its
 * backing partition device, which is then mapped back to its parent
 * whole disk to set that one entry's is_os_disk -- disk.c has no
 * knowledge of main.c's own CONTAINERS_DIR global, so the caller passes
 * it in explicitly rather than this module reaching for daemon-layer
 * state of its own. Pass NULL (e.g. from a context with no real
 * containers partition at all, like most test/dev daemons) to leave
 * every entry's is_os_disk 0 -- never fatal either way.
 */
/*
 * Fills mounted/mount_path/has_mounted_partition on every entry in
 * out[] from a /proc/mounts-format file. Exposed only so a test can
 * exercise the attribution against a synthetic mounts file -- real
 * callers get this via disk_enumerate(). See disk.h's own `mounted`
 * comment for what the two flags mean and why they are separate.
 */
void disk_fill_mount_status_from(const char *mounts_path, struct discovered_disk *out, int count);

/*
 * Reads dev_path's own superblock and writes the filesystem found there
 * ("ext4"/"btrfs"/"vfat"/"swap"/"squashfs"), or "" for none recognised
 * or an unreadable device. Exposed both for disk_enumerate()'s own use
 * and so a test can drive it against crafted files -- this dev sandbox
 * has no block device nodes at all (/sys/class/block is visible, /dev
 * entries are not), so the real path cannot be exercised here.
 */
void disk_probe_fs_type(const char *dev_path, char *out, size_t out_size);

int disk_enumerate(struct discovered_disk *out, int cap, const char *os_containers_dir);

void disk_write_json_one(const struct discovered_disk *d, struct json_writer *w);

/* Calls disk_enumerate() itself, then writes the whole list. */
void disk_write_json_list(struct json_writer *w, const char *os_containers_dir);

#endif /* DISK_H */
