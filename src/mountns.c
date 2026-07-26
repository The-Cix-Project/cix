#include "internal.h"
#include "linux_compat.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

int mountns_pivot(const struct mount_spec *mnt)
{
	char put_old_path[PATH_MAX];

	/*
	 * Must happen before pivot_root: without this, host mount
	 * propagation leaks into the child (and vice versa) even
	 * though we're already in a new mount namespace.
	 */
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
		perror("mountns_pivot: mount(MS_REC|MS_PRIVATE)");
		return -1;
	}

	if (mkdir(mnt->root_source, 0700) != 0 && errno != EEXIST) {
		perror("mountns_pivot: mkdir(root_source)");
		return -1;
	}

	/*
	 * Bind-mount host / onto the scratch dir: this becomes the
	 * container's root. pivot_root also requires new_root to be a
	 * mount point in its own right, which this satisfies too.
	 */
	if (mount("/", mnt->root_source, NULL, MS_BIND | MS_REC, NULL) != 0) {
		perror("mountns_pivot: mount(MS_BIND|MS_REC)");
		return -1;
	}

	if (snprintf(put_old_path, sizeof(put_old_path), "%s/%s",
	             mnt->root_source, mnt->put_old_rel) >= (int)sizeof(put_old_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	if (mkdir(put_old_path, 0700) != 0 && errno != EEXIST) {
		perror("mountns_pivot: mkdir(put_old_path)");
		return -1;
	}

	if (chdir(mnt->root_source) != 0) {
		perror("mountns_pivot: chdir(root_source)");
		return -1;
	}

	if (sys_pivot_root(".", mnt->put_old_rel) != 0) {
		perror("mountns_pivot: pivot_root");
		return -1;
	}

	if (chdir("/") != 0) {
		perror("mountns_pivot: chdir(/)");
		return -1;
	}

	if (umount2(mnt->put_old_rel, MNT_DETACH) != 0) {
		perror("mountns_pivot: umount2(put_old, MNT_DETACH)");
		return -1;
	}

	if (mkdir("/proc", 0555) != 0 && errno != EEXIST) {
		perror("mountns_pivot: mkdir(/proc)");
		return -1;
	}

	/*
	 * MS_NOSUID|MS_NODEV|MS_NOEXEC: procfs can reveal/enable more than
	 * a nested, unprivileged mount namespace is allowed to grant. The
	 * kernel locks these flags on any mount namespace derived from an
	 * unprivileged one and rejects a new procfs/sysfs mount that isn't
	 * at least as restrictive (EPERM, "VFS: Mount too revealing" in
	 * dmesg). Every real container runtime mounts proc this way
	 * regardless of nesting; it isn't nesting-specific hardening.
	 */
	if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
		perror("mountns_pivot: mount(proc)");
		return -1;
	}

	/*
	 * /sys was carried in by the recursive host-root bind mount above,
	 * including whatever sub-mounts the host stacked under it (e.g. a
	 * separate writable sysfs instance at /sys/devices/virtual/net).
	 * Those sub-mounts are bind copies pinned to the netns active when
	 * the host set them up, not the netns we just created, so they'd
	 * keep showing host network devices. Detach the carried-in tree
	 * and mount a fresh sysfs, same as every real container runtime
	 * does after unsharing the network namespace.
	 */
	umount2("/sys/devices/virtual/net", MNT_DETACH);
	if (umount2("/sys", MNT_DETACH) != 0 && errno != EINVAL) {
		perror("mountns_pivot: umount2(/sys)");
		return -1;
	}
	if (mkdir("/sys", 0555) != 0 && errno != EEXIST) {
		perror("mountns_pivot: mkdir(/sys)");
		return -1;
	}
	if (mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
		perror("mountns_pivot: mount(sysfs)");
		return -1;
	}

	return 0;
}
