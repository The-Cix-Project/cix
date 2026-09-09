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

/*
 * Mount a tmpfs that belongs to the CONTAINER, not to the host.
 *
 * Every mount in mountns_pivot() is made by a process that is still real
 * host root -- deliberately, and it cannot be otherwise: this code has to
 * pivot_root() and later mknod() real device nodes, and a process that has
 * already become the namespace's mapped root can do neither (the kernel
 * refuses device creation inside a user namespace outright). container.c
 * therefore does its setgid(0)/setuid(0) into the mapped root only after
 * all of this has run.
 *
 * The consequence is easy to miss, and was missed: host uid 0 has no entry
 * in the container's uid_map, so at mount time the mounting process is
 * UNMAPPED, and a filesystem whose root inode takes its owner from the
 * mounter comes out owned by the overflow uid (65534). The container's own
 * root -- the uid the payload actually runs as -- then cannot write to it.
 * Measured on a real container: uid_map "0 4425376 65536", "/" and "/etc"
 * correctly owned by 0 through ADR-0207's id-mapped rootfs, and "/run"
 * owned by 65534 with mkdir returning EACCES.
 *
 * uid=/gid= fixes it because tmpfs resolves those options through the
 * MOUNTING PROCESS'S user namespace, which is already the container's --
 * only its fsuid is unmapped, not its namespace -- so uid=0 names the
 * container's root rather than the host's. Verified both ways with a
 * standalone probe: without these options the tmpfs comes up owned by
 * 65534 and is unwritable by the container's root; with them it comes up
 * owned by 0 and is writable.
 *
 * chown() after mounting is NOT an alternative, and that was measured
 * rather than assumed: it fails EPERM, because a process whose own fsuid
 * is unmapped cannot give away a file owned by an unmapped uid, capability
 * set notwithstanding. The ownership has to be established by the mount.
 *
 * The options are unconditional -- no branch on whether this container has
 * a user namespace. Without one, the mounter is in the initial user
 * namespace where uid=0 resolves to host root, which is exactly what the
 * bare mount already produced; confirmed with the same probe. A branch
 * would be a second path to keep correct for no gain.
 *
 * This is a function rather than a longer options string at the one call
 * site so the rule travels with the act of mounting: any future writable
 * filesystem created for a container goes through here and cannot quietly
 * omit the ownership. That is precisely how /run came to be broken --
 * nothing about a bare mount() call said the ownership was a decision at
 * all.
 *
 * Deliberately NOT applied to /proc, /sys or cgroup2 below. They share the
 * same unmapped-owner cause and their roots do read as 65534, but they are
 * kernel-maintained views rather than storage this platform creates for
 * the container: their root directories are r-xr-xr-x, nothing writes to
 * them, and neither procfs nor sysfs accepts uid=/gid= at all. /dev/pts is
 * already correct -- devpts derives ownership the same way and already
 * resolves to the container's root. Extending this there would mean
 * inventing a mechanism for filesystems that do not want one.
 */
static int mount_container_tmpfs(const char *target, const char *opts, unsigned long flags)
{
	char full[256];

	if ((size_t)snprintf(full, sizeof(full), "%s,uid=0,gid=0", opts) >= sizeof(full)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return mount("tmpfs", target, "tmpfs", flags, full);
}

int mountns_pivot(const struct mount_spec *mnt)
{
	/*
	 * Relative to cwd, which the caller has already made the new root
	 * (see the header's contract). No path is walked from / here, so
	 * this works whether or not the process can still traverse the
	 * host path its rootfs lives under -- which, after a user
	 * namespace's privilege drop, it may not be able to.
	 */
	if (mkdir(mnt->put_old_rel, 0700) != 0 && errno != EEXIST) {
		perror("mountns_pivot: mkdir(put_old)");
		return MOUNTNS_PIVOT_ERR_MKDIR_PUT_OLD;
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
	if (mount_container_tmpfs("/run", "mode=0755", MS_NOSUID | MS_NODEV) != 0) {
		perror("mountns_pivot: mount(tmpfs /run)");
		return MOUNTNS_PIVOT_ERR_MOUNT_RUN;
	}

	/*
	 * /tmp, on exactly the same terms as /run and for the same reason:
	 * software assumes it exists (#343).
	 *
	 * These images have never had one, and it has cost three separate
	 * investigations. The last was hostapd_cli, which creates its OWN
	 * client socket before connecting and hardcodes that path to /tmp
	 * in wpa_ctrl.c -- so it printed "Could not connect to hostapd -
	 * re-trying" while hostapd was healthy the whole time, its control
	 * socket exactly where the config said, and nothing in the message
	 * pointed at /tmp. That is the shape of this gap every time: the
	 * failure names the thing that could not be reached, never the
	 * directory that was missing.
	 *
	 * MODE 01777, with the sticky bit, which is not decoration. /tmp is
	 * world-writable by definition, and without the sticky bit any
	 * process could delete another's files there. Every Unix has shipped
	 * it this way for decades and software depends on the guarantee.
	 *
	 * Fresh on every start, like /run: a tmpfs rather than
	 * overlay-persisted storage, so nothing survives a restart. That is
	 * what /tmp promises, and a /tmp that quietly persisted would break
	 * the same class of assumption in the opposite direction.
	 */
	if (mkdir("/tmp", 01777) != 0 && errno != EEXIST) {
		perror("mountns_pivot: mkdir(/tmp)");
		return MOUNTNS_PIVOT_ERR_MKDIR_TMP;
	}
	if (mount_container_tmpfs("/tmp", "mode=1777", MS_NOSUID | MS_NODEV) != 0) {
		perror("mountns_pivot: mount(tmpfs /tmp)");
		return MOUNTNS_PIVOT_ERR_MOUNT_TMP;
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
	 * ADR-0262/#336: the container's own /proc/meminfo and friends.
	 *
	 * This is the ONLY window in which it can be done. The source lives
	 * on the host, reachable now only through put_old, and the target is
	 * the fresh /proc mounted above -- so it must come after that mount
	 * and before the detach immediately below. A bind attempted anywhere
	 * else has either no source or no target.
	 *
	 * Every failure here is deliberately non-fatal. A container that
	 * does not get these files reads the host's real /proc, which is
	 * exactly what every container read before this feature existed --
	 * degraded, not broken, and never a reason to refuse to start a
	 * container.
	 *
	 * WHERE THE REASON ACTUALLY GOES, corrected: this comment used to
	 * claim the message reached "the child's diag pipe, which
	 * container_create() already surfaces". It does not. The diag pipe
	 * is written by child_diag() with an explicit fd; this child's
	 * stderr is dup2'd to the container's own output fd, so the text
	 * below lands there or nowhere, and a bind that failed for every
	 * file said nothing an operator could find. It stays here because
	 * capture_output makes it readable when someone is looking; the
	 * thing that actually reports is the parent, which counts the
	 * landed binds in the child's mount table right after creation
	 * (registry.c) and logs the count either way.
	 */
	if (mnt != NULL && mnt->procfuse_dir != NULL && mnt->procfuse_dir[0] != '\0' &&
	    mnt->procfuse_files != NULL) {
		int i;

		for (i = 0; i < mnt->procfuse_file_count; i++) {
			char src[PATH_MAX];
			char dst[PATH_MAX];

			if (snprintf(src, sizeof(src), "/%s%s/%s", mnt->put_old_rel, mnt->procfuse_dir,
			              mnt->procfuse_files[i]) >= (int)sizeof(src))
				continue;
			if (snprintf(dst, sizeof(dst), "/proc/%s", mnt->procfuse_files[i]) >=
			    (int)sizeof(dst))
				continue;
			/*
			 * MS_BIND only. A recursive bind would drag the whole FUSE
			 * mount in, and the target is one file over one file.
			 */
			if (mount(src, dst, NULL, MS_BIND, NULL) != 0) {
				/*
				 * Named in full: which source, which target, which
				 * errno. The first version of this printed a fixed
				 * sentence, and when nothing got bound it could not
				 * say whether the source was missing, the target was
				 * missing, or the mount was refused -- three different
				 * fixes.
				 */
				fprintf(stderr, "mountns_pivot: bind %s -> %s failed: %s (container reads the "
				                 "host's /proc for it)\n",
				         src, dst, strerror(errno));
			}
		}
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

	/*
	 * And remove the husk. mkdir(put_old) above runs AFTER the chdir
	 * into the new root, so the directory is created inside the
	 * container's own overlay -- it lands in the upperdir and persists
	 * for the life of the container, which is why an operator listing
	 * the root of a long-running container saw a stray .old_root
	 * sitting next to their own files.
	 *
	 * MNT_DETACH has already removed the mount from this namespace, so
	 * what is left is an ordinary empty directory and rmdir() takes it.
	 * Non-fatal: a container whose root is one empty directory untidier
	 * than it should be is still a correct container, and refusing to
	 * start one over cosmetics would be the worse trade.
	 */
	if (rmdir(mnt->put_old_rel) != 0 && errno != ENOENT)
		perror("mountns_pivot: rmdir(put_old)");

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
