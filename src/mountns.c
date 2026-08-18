#include "internal.h"
#include "linux_compat.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

int mountns_make_private(void)
{
	/*
	 * Must happen before any other mount in this namespace (including
	 * overlay_create()'s overlay mount): without this, mount events
	 * propagate between this namespace and the host in both
	 * directions even though we're already in a new mount namespace.
	 */
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
		perror("mountns_make_private: mount(MS_REC|MS_PRIVATE)");
		return -1;
	}
	return 0;
}

int mountns_pivot(const char *new_root, const struct mount_spec *mnt)
{
	char put_old_path[PATH_MAX];

	if (snprintf(put_old_path, sizeof(put_old_path), "%s/%s",
	             new_root, mnt->put_old_rel) >= (int)sizeof(put_old_path)) {
		errno = ENAMETOOLONG;
		return MOUNTNS_PIVOT_ERR_PATH_TOO_LONG;
	}

	if (mkdir(put_old_path, 0700) != 0 && errno != EEXIST) {
		perror("mountns_pivot: mkdir(put_old_path)");
		return MOUNTNS_PIVOT_ERR_MKDIR_PUT_OLD;
	}

	if (chdir(new_root) != 0) {
		perror("mountns_pivot: chdir(new_root)");
		return MOUNTNS_PIVOT_ERR_CHDIR_NEW_ROOT;
	}

	if (sys_pivot_root(".", mnt->put_old_rel) != 0) {
		perror("mountns_pivot: pivot_root");
		return MOUNTNS_PIVOT_ERR_PIVOT_ROOT;
	}

	if (chdir("/") != 0) {
		perror("mountns_pivot: chdir(/)");
		return MOUNTNS_PIVOT_ERR_CHDIR_ROOT;
	}

	if (umount2(mnt->put_old_rel, MNT_DETACH) != 0) {
		perror("mountns_pivot: umount2(put_old, MNT_DETACH)");
		return MOUNTNS_PIVOT_ERR_UMOUNT_PUT_OLD;
	}

	if (mkdir("/proc", 0555) != 0 && errno != EEXIST) {
		perror("mountns_pivot: mkdir(/proc)");
		return MOUNTNS_PIVOT_ERR_MKDIR_PROC;
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
		return MOUNTNS_PIVOT_ERR_MOUNT_PROC;
	}

	/*
	 * /sys may not exist at all (a minimal lowerdir with no /sys entry),
	 * or may already be populated (e.g. an overlay lowerdir that itself
	 * contains host mounts), including sub-mounts stacked under it like
	 * a separate writable sysfs instance at /sys/devices/virtual/net.
	 * Any such sub-mount is pinned to whatever netns was active when it
	 * was set up, not the netns we just created, so it would keep
	 * showing devices from the wrong namespace. Detach anything carried
	 * in and mount a fresh sysfs, same as every real container runtime
	 * does after unsharing the network namespace.
	 */
	umount2("/sys/devices/virtual/net", MNT_DETACH);
	if (umount2("/sys", MNT_DETACH) != 0 && errno != EINVAL && errno != ENOENT) {
		perror("mountns_pivot: umount2(/sys)");
		return MOUNTNS_PIVOT_ERR_UMOUNT_SYS;
	}
	if (mkdir("/sys", 0555) != 0 && errno != EEXIST) {
		perror("mountns_pivot: mkdir(/sys)");
		return MOUNTNS_PIVOT_ERR_MKDIR_SYS;
	}
	if (mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
		perror("mountns_pivot: mount(sysfs)");
		return MOUNTNS_PIVOT_ERR_MOUNT_SYS;
	}

	/*
	 * /run must be a fresh tmpfs on every single container start, same
	 * as /proc and /sys above -- without this it's just ordinary
	 * overlay-persisted storage (upperdir survives a crash/restart by
	 * design, see overlay.c's own EEXIST-tolerant upperdir reuse), which
	 * silently breaks any daemon that assumes /run is cleared on start
	 * (the near-universal Linux/systemd convention). Confirmed the hard
	 * way: chronyd (-d mode) writes /run/chronyd.pid, and since every
	 * container gets its own fresh PID namespace chronyd is always PID
	 * 1 there -- on a respawn (crash, or a manual recreate reusing the
	 * same name after a reboot) it found its OWN stale pidfile
	 * containing "1", saw that PID legitimately exists, and refused to
	 * start with "another instance may already be running", exiting 1
	 * forever with no way out short of deleting the container outright.
	 */
	if (mkdir("/run", 0755) != 0 && errno != EEXIST) {
		perror("mountns_pivot: mkdir(/run)");
		return MOUNTNS_PIVOT_ERR_MKDIR_RUN;
	}
	if (mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") != 0) {
		perror("mountns_pivot: mount(tmpfs /run)");
		return MOUNTNS_PIVOT_ERR_MOUNT_RUN;
	}

	/*
	 * The same devpts gap task #764/ADR-pending-#426 already found and
	 * fixed for the daemon's own host-namespace console-exec feature
	 * (thincd's exec.c posix_openpt()s /dev/ptmx fine regardless --
	 * that device is always reachable -- but opening the SLAVE path
	 * ptsname_r() hands back fails with a bare ENOENT unless a real
	 * devpts filesystem is mounted at /dev/pts), one namespace layer
	 * deeper: each container gets its own mount namespace here, so the
	 * host's own /dev/pts mount is never inherited into it, and this
	 * project's own containers use a plain overlay /dev (no devtmpfs,
	 * unlike the host) with only pkg_seed_image_baseline()'s five
	 * static device nodes -- no /dev/ptmx at all. A container-side
	 * process needing a real pty (sshd's own interactive session
	 * allocation being the case that surfaced this) fails outright
	 * with no such device. Fresh on every start, same as /proc/​/sys/​
	 * /run above -- mount points never persist across a container
	 * restart, only the underlying rootfs files do. Same mount options
	 * as the host's own copy (main.c's boot_init(), thinc-install.c's
	 * early_mounts()) for the same reason: ptmxmode=0666 so a non-root
	 * process inside the container can still allocate a pty.
	 */
	/* Unlike /proc, /sys, /run (all fresh top-level dirs regardless of
	 * what the lowerdir provides), /dev itself is only guaranteed to
	 * exist for a real thinC-managed image (pkg_seed_image_baseline()
	 * creates it) -- a minimal hand-built rootfs with no /dev at all is
	 * a legitimate case (mkdir("/dev/pts", ...) would otherwise fail
	 * ENOENT, its parent missing), so /dev itself gets the same
	 * guaranteed-present treatment first. */
	if (mkdir("/dev", 0755) != 0 && errno != EEXIST) {
		perror("mountns_pivot: mkdir(/dev)");
		return MOUNTNS_PIVOT_ERR_MKDIR_DEV;
	}
	if (mkdir("/dev/pts", 0755) != 0 && errno != EEXIST) {
		perror("mountns_pivot: mkdir(/dev/pts)");
		return MOUNTNS_PIVOT_ERR_MKDIR_DEV_PTS;
	}
	if (mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
	          "mode=0620,ptmxmode=0666") != 0) {
		perror("mountns_pivot: mount(devpts)");
		return MOUNTNS_PIVOT_ERR_MOUNT_DEV_PTS;
	}

	return 0;
}
