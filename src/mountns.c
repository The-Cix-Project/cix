#include "internal.h"
#include "linux_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
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

	/*
	 * put_old (the old root, carrying the inherited, fully-visible /proc
	 * and /sys) is deliberately NOT detached yet -- it is detached at the
	 * very end, after the fresh proc/sysfs mounts below. ADR-0179 phase
	 * 2b: an unprivileged user namespace may only mount proc/sysfs when a
	 * fully-visible instance of the same filesystem already exists in the
	 * mount namespace (the kernel's mount_too_revealing() check); detaching
	 * the inherited one first made mount(proc) fail EPERM in a userns
	 * (confirmed live). The non-userns path is unaffected by the reorder --
	 * it holds CAP_SYS_ADMIN in the initial userns and bypasses the check
	 * either way, and the end state (old root detached, fresh mounts in
	 * place) is identical.
	 */

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
	 * A cgroup tree for a container that runs containers of its own
	 * (#257).
	 *
	 * The fresh sysfs above has nothing mounted at /sys/fs/cgroup, and
	 * src/cgroup.c writes to exactly that path, so a nested cixd could
	 * not create a single cgroup: every container it tried to make
	 * failed at mkdir with ENOENT. Measured as 25 of 48 failures the
	 * first time this platform's own test suite ran on a real host.
	 *
	 * What makes this safe to expose is the cgroup NAMESPACE, which
	 * the caller has already established (it is half of the condition
	 * for setting this flag). With CLONE_NEWCGROUP the mount shows the
	 * container its OWN cgroup as the root of the tree -- it cannot
	 * see, read or write anything above itself, so the parent's
	 * budgets stay in force and ADR-0165's shared build-slot ceiling
	 * still applies to everything created underneath.
	 *
	 * MS_NOSUID | MS_NODEV | MS_NOEXEC for the same reason the sysfs
	 * mount above carries them: nothing here is ever meant to be
	 * executed or to name a device.
	 */
	if (mnt != NULL && mnt->mount_cgroup2) {
		/*
		 * Opportunistic, deliberately: a failure here warns and
		 * carries on rather than refusing to start the container.
		 *
		 * This code sits in the path of EVERY package build container
		 * on every host, and those are exactly the containers that
		 * ask for it. Making the mount fatal would mean any kernel or
		 * host where it does not work stops the machine building
		 * anything at all -- trading a capability some containers want
		 * for the one function every host needs. A container that
		 * cannot get the mount lands exactly where it was before this
		 * existed, which is a known and survivable state, and the
		 * perror() says so on the way past.
		 */
		if ((mkdir("/sys/fs", 0755) != 0 && errno != EEXIST) ||
		    (mkdir("/sys/fs/cgroup", 0755) != 0 && errno != EEXIST))
			perror("mountns_pivot: mkdir(/sys/fs/cgroup) -- nested cgroups unavailable");
		else if (mount("cgroup2", "/sys/fs/cgroup", "cgroup2",
		                MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0)
			perror("mountns_pivot: mount(cgroup2) -- nested cgroups unavailable");
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
	 * (cixd's exec.c posix_openpt()s /dev/ptmx fine regardless --
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
	 * as the host's own copy (main.c's boot_init(), cix-install.c's
	 * early_mounts()) for the same reason: ptmxmode=0666 so a non-root
	 * process inside the container can still allocate a pty.
	 */
	/* Unlike /proc, /sys, /run (all fresh top-level dirs regardless of
	 * what the lowerdir provides), /dev itself is only guaranteed to
	 * exist for a real Cix-managed image (pkg_seed_image_baseline()
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

	/*
	 * Now that the fresh proc/sysfs are mounted (satisfying the userns
	 * visibility check while the inherited ones were still present), detach
	 * the old root. MNT_DETACH lazily removes the whole put_old subtree as
	 * a unit, which is permitted even for the locked mounts a userns child
	 * inherited (the lock forbids separating them, not detaching the group).
	 */
	if (umount2(mnt->put_old_rel, MNT_DETACH) != 0) {
		perror("mountns_pivot: umount2(put_old, MNT_DETACH)");
		return MOUNTNS_PIVOT_ERR_UMOUNT_PUT_OLD;
	}

	return 0;
}

/*
 * Issue #92 part 2: bind-mount a host directory into an ALREADY-RUNNING
 * container's mount namespace.
 *
 * Until now a volume could only be attached at creation, because the
 * bind mount has to land inside the container's own mount namespace and
 * that namespace only exists between clone3() and pivot_root(). The
 * primitive to reach an existing one was described as missing; it was
 * not. container_net.c has done exactly this for the NET namespace all
 * along -- a short-lived forked helper that setns()es in and does the
 * part only reachable from inside. This is the same dance for the mount
 * namespace.
 *
 * A forked helper is not a style choice: setns() is whole-process, so
 * the daemon entering a container's mount namespace itself would leave
 * every subsequent path it resolves -- its state directory, its log
 * store, every other container's overlay -- resolving inside that
 * container. The helper exists to be thrown away.
 *
 * The source directory is opened BEFORE setns(), for the same reason
 * container_net.c opens root_fd first: once inside the container's
 * mount namespace the host path may not resolve at all. The already-open
 * fd is then named through /proc/self/fd, which refers to the helper
 * itself and stays valid across the namespace change.
 *
 * Returns 0, or -1 with the helper's own failure reported through its
 * exit status.
 */
int mountns_bind_into(pid_t child_pid, const char *host_dir, const char *container_path,
                      int read_only)
{
	char ns_path[64];
	int mntns_fd;
	int src_fd;
	pid_t helper;
	int status;

	/*
	 * Detach a clone of the source tree into an fd BEFORE entering the
	 * container's namespace. This is the whole reason open_tree() is
	 * used rather than a plain bind: after setns() the host path is not
	 * nameable from inside, and the obvious workaround -- naming the
	 * already-open fd through /proc/self/fd -- does not work either,
	 * because the container's /proc belongs to its own PID namespace
	 * and the helper is not in it, so /proc/self does not resolve.
	 */
	src_fd = cix_open_tree(AT_FDCWD, host_dir, OPEN_TREE_CLONE | AT_RECURSIVE);
	if (src_fd < 0) {
		fprintf(stderr, "mountns_bind_into: open_tree(%s): %s\n", host_dir, strerror(errno));
		return -1;
	}
	snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/mnt", (int)child_pid);
	mntns_fd = open(ns_path, O_RDONLY | O_CLOEXEC);
	if (mntns_fd < 0) {
		fprintf(stderr, "mountns_bind_into: open(%s): %s\n", ns_path, strerror(errno));
		close(src_fd);
		return -1;
	}

	helper = fork();
	if (helper < 0) {
		close(src_fd);
		close(mntns_fd);
		return -1;
	}
	if (helper == 0) {
		if (setns(mntns_fd, CLONE_NEWNS) != 0)
			_exit(2);
		/* The mount point may not exist in the image at all -- a volume
		 * mounted at a /data the rootfs never had is normal, and the
		 * create-time path already creates it. */
		if (mkdir(container_path, 0755) != 0 && errno != EEXIST)
			_exit(3);
		if (cix_move_mount(src_fd, "", AT_FDCWD, container_path, MOVE_MOUNT_F_EMPTY_PATH) != 0)
			_exit(4);
		if (read_only) {
			/*
			 * A read-only bind needs the second, remount call -- the
			 * flag is ignored on the initial attach. Fatal rather than
			 * best-effort: a read-only mount that is silently writable
			 * is a guarantee that is not one.
			 */
			if (mount(NULL, container_path, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) != 0)
				_exit(5);
		}
		_exit(0);
	}
	close(src_fd);
	close(mntns_fd);
	if (waitpid(helper, &status, 0) != helper || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "mountns_bind_into: helper failed (status %d) binding %s -> %s\n",
		        WIFEXITED(status) ? WEXITSTATUS(status) : -1, host_dir, container_path);
		return -1;
	}
	return 0;
}
