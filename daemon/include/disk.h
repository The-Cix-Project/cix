#ifndef DISK_H
#define DISK_H

#include "json.h"

/*
 * Real host block devices (whole disks, never partitions), discovered
 * fresh from sysfs on every call -- the same "never persisted, hardware
 * this daemon doesn't create or own, re-enumerated every time"
 * convention device.c's own struct discovered_device already
 * established for USB/PCI/net/GPU (ADR-0012's atomic-JSON persistence
 * is for daemon-owned state, not this).
 */
#define DISK_ENUM_MAX 64

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
	 * mechanism existed). Set true if any partition on this disk (or
	 * the whole-disk device itself) currently appears in /proc/mounts;
	 * mount_path is that mount's own real path. A disk can only really
	 * have one role-assigned mountpoint in this project's own model, so
	 * the first match found wins -- matching disk_enumerate()'s own
	 * already-established precedent (resolve_os_disk_name()'s
	 * longest-match walk) of treating /proc/mounts as the one real
	 * source of truth rather than anything this daemon merely believes.
	 */
	int mounted;
	char mount_path[256];
};

/*
 * Walks /sys/class/block fresh, writing up to cap whole-disk entries
 * into out (partitions -- anything with a "partition" sysfs attribute
 * -- are skipped entirely, they're not independently assignable).
 * Returns the count written.
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
int disk_enumerate(struct discovered_disk *out, int cap, const char *os_containers_dir);

void disk_write_json_one(const struct discovered_disk *d, struct json_writer *w);

/* Calls disk_enumerate() itself, then writes the whole list. */
void disk_write_json_list(struct json_writer *w, const char *os_containers_dir);

#endif /* DISK_H */
