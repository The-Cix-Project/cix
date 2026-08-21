#include "container.h"
#include "internal.h"
#include "linux_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Writes "prefix: strerror(errno)\n" to fd (the diag pipe's write end,
 * always valid in the child by the time this is ever called -- see
 * container_create()'s own comment on why this pipe is unconditional,
 * not opt-in). dprintf() reads errno itself via strerror(), so callers
 * just need to call this immediately after the failing call, exactly
 * like the perror() it replaces.
 */
static void child_diag(int fd, const char *prefix)
{
	dprintf(fd, "%s: %s\n", prefix, strerror(errno));
}

/*
 * mountns_pivot()'s own distinct step, for the exact same reason
 * overlay_step_names[] (container_decode_exit_status(), below) exists
 * -- ~15 real syscalls behind one function, a flat "child: mountns_pivot"
 * diag message left genuinely no way to tell which one actually failed
 * on a shell-less production box (issue #34's own investigation).
 * Reads errno in the same dprintf() call that resolves the step name,
 * matching child_diag()'s own contract -- no snprintf/other call
 * in between that could touch it first.
 */
static void child_diag_mountns_pivot(int fd, int mountns_pivot_ret)
{
	static const struct {
		int code;
		const char *step;
	} steps[] = {
		{ MOUNTNS_PIVOT_ERR_MKDIR_PUT_OLD, "mkdir(put_old)" },
		{ MOUNTNS_PIVOT_ERR_CHDIR_NEW_ROOT, "chdir(new_root)" },
		{ MOUNTNS_PIVOT_ERR_PIVOT_ROOT, "pivot_root" },
		{ MOUNTNS_PIVOT_ERR_CHDIR_ROOT, "chdir(/)" },
		{ MOUNTNS_PIVOT_ERR_UMOUNT_PUT_OLD, "umount2(put_old)" },
		{ MOUNTNS_PIVOT_ERR_MKDIR_PROC, "mkdir(/proc)" },
		{ MOUNTNS_PIVOT_ERR_MOUNT_PROC, "mount(proc)" },
		{ MOUNTNS_PIVOT_ERR_UMOUNT_SYS, "umount2(/sys)" },
		{ MOUNTNS_PIVOT_ERR_MKDIR_SYS, "mkdir(/sys)" },
		{ MOUNTNS_PIVOT_ERR_MOUNT_SYS, "mount(sysfs)" },
		{ MOUNTNS_PIVOT_ERR_MKDIR_RUN, "mkdir(/run)" },
		{ MOUNTNS_PIVOT_ERR_MOUNT_RUN, "mount(tmpfs /run)" },
		{ MOUNTNS_PIVOT_ERR_MKDIR_DEV, "mkdir(/dev)" },
		{ MOUNTNS_PIVOT_ERR_MKDIR_DEV_PTS, "mkdir(/dev/pts)" },
		{ MOUNTNS_PIVOT_ERR_MOUNT_DEV_PTS, "mount(devpts)" },
		{ MOUNTNS_PIVOT_ERR_PATH_TOO_LONG, "path too long" },
	};
	const char *step = "unknown step";
	size_t i;

	for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		if (steps[i].code == mountns_pivot_ret) {
			step = steps[i].step;
			break;
		}
	}
	dprintf(fd, "child: mountns_pivot: %s: %s\n", step, strerror(errno));
}

/*
 * Parent-side equivalent of child_diag() above, for every failure
 * point in container_create()/cgroup_create() that runs before
 * clone3() (or its own post-fork parent-side steps) -- these have no
 * child process to write a diag_pipe message through, and this
 * project's single-threaded event loop makes a plain static buffer
 * safe: only ever written immediately before a synchronous -1 return,
 * only ever read by the same caller immediately after, never
 * concurrently. See container_create_last_error_step()'s own doc
 * comment in container.h for the production regression this closes.
 */
static char g_last_error_step[256];

void container_set_last_error_step(const char *prefix)
{
	snprintf(g_last_error_step, sizeof(g_last_error_step), "%s: %s", prefix, strerror(errno));
}

const char *container_create_last_error_step(void)
{
	return g_last_error_step;
}

/*
 * ADR-0179 (issue #29) phase 2: the PARENT writes a freshly-cloned
 * CLONE_NEWUSER child's uid/gid maps, then releases it. The child blocks
 * on userns_pipe before touching anything -- until these maps land it has
 * no valid mapped identity, so any privileged op (the overlay mount
 * included) would run with the wrong credentials. Ordering is the
 * documented kernel requirement: "deny" to setgroups (a task may only
 * gain, never re-drop, group membership across a map) BEFORE gid_map,
 * then uid_map. Each map is the single line "0 <base> <len>": the
 * container's own 0..len-1 onto host [base, base+len). Raw /proc writes,
 * no glibc wrapper needed -- same posture as this file's other raw
 * syscalls.
 */
static int write_proc_line(pid_t pid, const char *which, const char *val)
{
	char path[64];
	int fd;
	ssize_t n;
	size_t len = strlen(val);

	snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, which);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	n = write(fd, val, len);
	close(fd);
	return (n == (ssize_t)len) ? 0 : -1;
}

static int write_userns_maps(pid_t pid, long long base, long long len)
{
	char map[64];

	if (write_proc_line(pid, "setgroups", "deny") != 0)
		return -1;
	snprintf(map, sizeof(map), "0 %lld %lld", base, len);
	if (write_proc_line(pid, "gid_map", map) != 0)
		return -1;
	if (write_proc_line(pid, "uid_map", map) != 0)
		return -1;
	return 0;
}

/*
 * ADR-0179 (issue #29) phase 2c: the new mount API (open_tree/move_mount/
 * mount_setattr) has no glibc wrappers -- raw syscalls, x86_64 numbers,
 * same posture as this file's other rawsyscalls. struct kx_mount_attr is
 * the uapi struct mount_attr (four naturally-aligned u64s, no packing).
 */
#ifndef __NR_open_tree
#define __NR_open_tree 428
#endif
#ifndef __NR_move_mount
#define __NR_move_mount 429
#endif
#ifndef __NR_mount_setattr
#define __NR_mount_setattr 442
#endif
#define KX_OPEN_TREE_CLONE 1
#define KX_MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#define KX_MOUNT_ATTR_IDMAP 0x00100000

struct kx_mount_attr {
	unsigned long long attr_set;
	unsigned long long attr_clr;
	unsigned long long propagation;
	unsigned long long userns_fd;
};

static long kx_open_tree(int dfd, const char *path, unsigned int flags)
{
	return syscall(__NR_open_tree, dfd, path, flags);
}

static long kx_move_mount(int from_dfd, const char *from, int to_dfd, const char *to,
                          unsigned int flags)
{
	return syscall(__NR_move_mount, from_dfd, from, to_dfd, to, flags);
}

static long kx_mount_setattr(int dfd, const char *path, unsigned int flags,
                             struct kx_mount_attr *a, size_t size)
{
	return syscall(__NR_mount_setattr, dfd, path, flags, a, size);
}

#ifndef __NR_fsopen
#define __NR_fsopen 430
#endif
#ifndef __NR_fsconfig
#define __NR_fsconfig 431
#endif
#ifndef __NR_fsmount
#define __NR_fsmount 432
#endif
#define KX_FSCONFIG_SET_STRING 1
#define KX_FSCONFIG_SET_FD 5
#define KX_FSCONFIG_CMD_CREATE 6

static long kx_fsopen(const char *fsname, unsigned int flags)
{
	return syscall(__NR_fsopen, fsname, flags);
}

static long kx_fsconfig(int fd, unsigned int cmd, const char *key, const void *value, int aux)
{
	return syscall(__NR_fsconfig, fd, cmd, key, value, aux);
}

static long kx_fsmount(int fd, unsigned int flags, unsigned int attr_flags)
{
	return syscall(__NR_fsmount, fd, flags, attr_flags);
}

/*
 * ADR-0179 phase 2c: build the user namespace that carries the id-mapped
 * mount's translation. It is NOT the container's own userns -- it is the
 * INVERSE mapping: uid_map "<base> 0 <len>" (filesystem ids [0,len) are
 * presented as [base, base+len)). So a rootfs file owned by host uid 0 on
 * disk is presented as host uid <base>, which the container's own userns
 * ("0 <base> <len>") then resolves to container uid 0 -- the mapped root
 * genuinely owns its rootfs, killing the EOVERFLOW that unmapped host-0
 * ownership caused (phase 2b finding). A short-lived helper process holds
 * the namespace only long enough for us to open a handle to it; the open
 * fd keeps the userns object alive after the helper is reaped.
 */
static int create_idmap_userns_fd(long long base, long long len)
{
	int pipefd[2];   /* go: parent -> child (release) */
	int readyfd[2];  /* ready: child -> parent (unshare done) */
	pid_t helper;
	char path[64], map[64], c;
	int fd, status;

	if (pipe(pipefd) != 0)
		return -1;
	if (pipe(readyfd) != 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	helper = fork();
	if (helper < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		close(readyfd[0]);
		close(readyfd[1]);
		return -1;
	}
	if (helper == 0) {
		char rc;

		close(pipefd[1]);
		close(readyfd[0]);
		if (unshare(CLONE_NEWUSER) != 0)
			_exit(1);
		/*
		 * Signal the parent only AFTER unshare has landed -- otherwise
		 * the parent can race ahead and write our uid_map while we're
		 * still in the init userns, which fails EPERM (confirmed live).
		 */
		if (write(readyfd[1], "r", 1) != 1)
			_exit(3);
		if (read(pipefd[0], &rc, 1) != 1)
			_exit(2);
		_exit(0);
	}
	close(pipefd[0]);
	close(readyfd[1]);

	/* Wait for the child to confirm it is in its new user namespace. */
	if (read(readyfd[0], &c, 1) != 1) {
		container_set_last_error_step("create_idmap_userns_fd: unshare(CLONE_NEWUSER)");
		close(readyfd[0]);
		close(pipefd[1]);
		waitpid(helper, &status, 0);
		return -1;
	}
	close(readyfd[0]);

	snprintf(map, sizeof(map), "%lld 0 %lld", base, len);
	if (write_proc_line(helper, "setgroups", "deny") != 0) {
		container_set_last_error_step("create_idmap_userns_fd: setgroups");
		close(pipefd[1]);
		waitpid(helper, &status, 0);
		return -1;
	}
	if (write_proc_line(helper, "gid_map", map) != 0) {
		container_set_last_error_step("create_idmap_userns_fd: gid_map");
		close(pipefd[1]);
		waitpid(helper, &status, 0);
		return -1;
	}
	if (write_proc_line(helper, "uid_map", map) != 0) {
		container_set_last_error_step("create_idmap_userns_fd: uid_map");
		close(pipefd[1]);
		waitpid(helper, &status, 0);
		return -1;
	}

	snprintf(path, sizeof(path), "/proc/%d/ns/user", (int)helper);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		container_set_last_error_step("create_idmap_userns_fd: open ns/user");

	close(pipefd[1]); /* release the helper; our open fd keeps the userns alive */
	waitpid(helper, &status, 0);
	return fd; /* -1 on open failure */
}

/*
 * open_tree(OPEN_TREE_CLONE) a path into a detached mount, then id-map it with
 * mount_setattr(MOUNT_ATTR_IDMAP). ext4-backed subtrees (this project's overlay
 * layers) support id-mapping; the overlay *merged* mount does not, which is the
 * whole reason we id-map the layers here rather than the result. Returns a
 * detached, id-mapped mount fd, or -1 (errno set).
 */
static int idmap_bind(const char *path, int idmap_fd)
{
	struct kx_mount_attr a;
	int fd = (int)kx_open_tree(-1, path, KX_OPEN_TREE_CLONE);

	if (fd < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.attr_set = KX_MOUNT_ATTR_IDMAP;
	a.userns_fd = (unsigned long long)idmap_fd;
	if (kx_mount_setattr(fd, "", AT_EMPTY_PATH, &a, sizeof(a)) != 0) {
		int saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	return fd;
}

/*
 * ADR-0179 phase 2c: build an id-mapped overlay via the fd-based mount API
 * (fsopen/fsconfig/fsmount), with ALL layers id-mapped by the inverse idmap
 * userns so ownership is consistent end to end: the shared host-uid-0 rootfs
 * (lower) and any copy-up into the per-container upper both present as the
 * container's mapped root, killing the EOVERFLOW without chowning the shared
 * image. The lower layer is its own id-mapped mount. upperdir and workdir are
 * opened THROUGH a single id-mapped mount of their shared base dir, because
 * overlay requires upper and work on the same vfsmount (ovl_get_workdir).
 * Overlay captures each layer fd's own f_path.mnt (kernel fs/overlayfs/params.c
 * ovl_parse_layer -> fs_value_is_file), so the id-mapping rides along, and
 * clone_private_mount preserves it. Returns the overlay mount fd (the child
 * move_mounts it), or -1 with a step set.
 */
static int build_idmapped_overlay_fd(const struct overlay_spec *ov, int idmap_fd)
{
	int lower_fd = -1, base_fd = -1, upper_fd = -1, work_fd = -1, fs_fd = -1, mnt_fd = -1;
	const char *step = "idmap_bind(lowerdir)";
	const char *upper_name, *work_name;
	int saved;

	/* upperdir/workdir are "<base>/upper" and "<base>/work" -- open the last
	 * component through the id-mapped base mount so they share its vfsmount. */
	upper_name = strrchr(ov->upperdir, '/');
	work_name = strrchr(ov->workdir, '/');
	upper_name = upper_name ? upper_name + 1 : ov->upperdir;
	work_name = work_name ? work_name + 1 : ov->workdir;

	lower_fd = idmap_bind(ov->lowerdir, idmap_fd);
	if (lower_fd < 0)
		goto out;
	step = "idmap_bind(base)";
	base_fd = idmap_bind(ov->base, idmap_fd);
	if (base_fd < 0)
		goto out;
	step = "openat(upper)";
	upper_fd = openat(base_fd, upper_name, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (upper_fd < 0)
		goto out;
	step = "openat(work)";
	work_fd = openat(base_fd, work_name, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (work_fd < 0)
		goto out;
	step = "fsopen(overlay)";
	fs_fd = (int)kx_fsopen("overlay", 0);
	if (fs_fd < 0)
		goto out;
	step = "fsconfig(lowerdir+)";
	if (kx_fsconfig(fs_fd, KX_FSCONFIG_SET_FD, "lowerdir+", NULL, lower_fd) != 0)
		goto out;
	step = "fsconfig(upperdir)";
	if (kx_fsconfig(fs_fd, KX_FSCONFIG_SET_FD, "upperdir", NULL, upper_fd) != 0)
		goto out;
	step = "fsconfig(workdir)";
	if (kx_fsconfig(fs_fd, KX_FSCONFIG_SET_FD, "workdir", NULL, work_fd) != 0)
		goto out;
	step = "fsconfig(create)";
	if (kx_fsconfig(fs_fd, KX_FSCONFIG_CMD_CREATE, NULL, NULL, 0) != 0)
		goto out;
	step = "fsmount(overlay)";
	mnt_fd = (int)kx_fsmount(fs_fd, 0, 0);

out:
	saved = errno;
	if (mnt_fd < 0) {
		char buf[80];

		snprintf(buf, sizeof(buf), "build_idmapped_overlay_fd: %s", step);
		errno = saved;
		container_set_last_error_step(buf);
	}
	if (lower_fd >= 0)
		close(lower_fd);
	if (base_fd >= 0)
		close(base_fd);
	if (upper_fd >= 0)
		close(upper_fd);
	if (work_fd >= 0)
		close(work_fd);
	if (fs_fd >= 0)
		close(fs_fd);
	errno = saved;
	return mnt_fd;
}

int container_create(const struct container_spec *spec, struct container_handle *out)
{
	int cgroup_fd;
	int pidfd = -1;
	int bpf_prog_fd = -1;
	long ret;
	int net_pipe[2] = { -1, -1 };
	int diag_pipe[2] = { -1, -1 };
	int userns_pipe[2] = { -1, -1 };
	int overlay_mnt_fd = -1; /* phase 2c: detached id-mapped overlay mount */
	int want_net = (spec->net_count > 0);
	int i;

	if (cgroup_create(&spec->cg, &cgroup_fd) != 0)
		return -1;

	/*
	 * Before ns_clone3(): CLONE_INTO_CGROUP places the child into
	 * cgroup_fd atomically as part of that syscall, so the device
	 * policy must already be attached for there to be no race window,
	 * and because the child's own container_dev_mknod() calls are
	 * themselves subject to a BPF_DEVCG_ACC_MKNOD check under this
	 * same program.
	 */
	if (container_dev_bpf_attach(cgroup_fd, spec->devices, spec->device_count,
	                              &bpf_prog_fd) != 0) {
		int saved_errno = errno;
		perror("container_create: container_dev_bpf_attach");
		container_set_last_error_step("container_create: container_dev_bpf_attach");
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	if (want_net && pipe(net_pipe) != 0) {
		int saved_errno = errno;
		perror("container_create: pipe(net_pipe)");
		container_set_last_error_step("container_create: pipe(net_pipe)");
		if (bpf_prog_fd >= 0)
			close(bpf_prog_fd);
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	/*
	 * O_CLOEXEC on both ends: the write end vanishes on its own at a
	 * successful execve() (see container_create()'s own header
	 * comment) without any extra code in the child; the read end
	 * (kept open here in the parent past this container's own
	 * clone3()) shouldn't leak into any *later* container's own
	 * clone3()/execve() either.
	 */
	if (pipe2(diag_pipe, O_CLOEXEC) != 0) {
		int saved_errno = errno;
		perror("container_create: pipe2(diag_pipe)");
		container_set_last_error_step("container_create: pipe2(diag_pipe)");
		if (want_net) {
			close(net_pipe[0]);
			close(net_pipe[1]);
		}
		if (bpf_prog_fd >= 0)
			close(bpf_prog_fd);
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	/*
	 * ADR-0179 phase 2: the userns sync pipe. Plain blocking pipe (same
	 * posture as net_pipe) -- the child blocks reading it right after
	 * clone3(), the parent writes the maps then releases it. Only created
	 * when userns is actually requested, so every non-userns create is
	 * byte-for-byte unchanged.
	 */
	if (spec->userns_enabled && pipe(userns_pipe) != 0) {
		int saved_errno = errno;
		perror("container_create: pipe(userns_pipe)");
		container_set_last_error_step("container_create: pipe(userns_pipe)");
		close(diag_pipe[0]);
		close(diag_pipe[1]);
		if (want_net) {
			close(net_pipe[0]);
			close(net_pipe[1]);
		}
		if (bpf_prog_fd >= 0)
			close(bpf_prog_fd);
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	/*
	 * ADR-0179 phase 2c: build the userns container's overlay HERE, in the
	 * parent (init userns, full privilege), as a detached, id-mapped mount
	 * the child later move_mounts into its own namespace and pivots into.
	 * The child, as the namespace's mapped root, can neither mount the
	 * overlay itself (phase 2a EACCES) nor usefully inherit a plain parent
	 * mount -- that both locks pivot_root (phase 2b) and leaves the rootfs
	 * owned by unmapped host uid 0 (phase 2b EOVERFLOW). Id-mapping the
	 * *layers* (build_idmapped_overlay_fd; the merged overlay mount itself
	 * cannot be id-mapped -- phase 2c EINVAL) presents host-uid-0 rootfs
	 * files as the container's mapped root, so it owns and can write its
	 * rootfs. The child inherits the returned fd (not O_CLOEXEC).
	 */
	if (spec->userns_enabled) {
		int overlay_ret = overlay_prepare_dirs(&spec->ov);
		int idmap_fd = -1;
		int saved_errno = errno;
		const char *fail_step = "container_create: overlay_prepare_dirs (userns)";

		if (overlay_ret == 0) {
			/*
			 * thincd (host uid 0) is the overlay's mounter, but on the
			 * id-mapped upper/work it is presented as a non-owner (those
			 * host-0 dirs present as owned by <base>), so overlay's own
			 * work/ creation and copy-up -- performed as thincd -- get
			 * EACCES ("failed to create directory /work/work ... mounting
			 * read-only", seen live via /v1/system/kmsg). Make these two
			 * per-container host dirs mode-permissive so the privileged
			 * mounter can populate them; they are private (under this
			 * container's own dir), never shared or exposed.
			 */
			chmod(spec->ov.upperdir, 0777);
			chmod(spec->ov.workdir, 0777);
			idmap_fd = create_idmap_userns_fd(spec->userns_uid_base, spec->userns_len);
			if (idmap_fd < 0) {
				saved_errno = errno;
				fail_step = NULL; /* create_idmap_userns_fd set the step */
				overlay_ret = -1;
			} else {
				overlay_mnt_fd = build_idmapped_overlay_fd(&spec->ov, idmap_fd);
				if (overlay_mnt_fd < 0) {
					saved_errno = errno;
					fail_step = NULL; /* build_idmapped_overlay_fd set the step */
					overlay_ret = -1;
				}
				close(idmap_fd);
				idmap_fd = -1;
			}
		} else {
			saved_errno = errno;
		}
		if (overlay_ret != 0) {
			if (overlay_mnt_fd >= 0) {
				close(overlay_mnt_fd);
				overlay_mnt_fd = -1;
			}
			errno = saved_errno;
			if (fail_step != NULL)
				container_set_last_error_step(fail_step);
			close(diag_pipe[0]);
			close(diag_pipe[1]);
			if (want_net) {
				close(net_pipe[0]);
				close(net_pipe[1]);
			}
			close(userns_pipe[0]);
			close(userns_pipe[1]);
			if (bpf_prog_fd >= 0)
				close(bpf_prog_fd);
			close(cgroup_fd);
			errno = saved_errno;
			return -1;
		}
	}

	ret = ns_clone3(spec->ns.clone_flags, cgroup_fd, &pidfd);
	if (ret < 0) {
		int saved_errno = errno;
		perror("container_create: ns_clone3");
		container_set_last_error_step("container_create: ns_clone3");
		close(diag_pipe[0]);
		close(diag_pipe[1]);
		if (want_net) {
			close(net_pipe[0]);
			close(net_pipe[1]);
		}
		if (spec->userns_enabled) {
			close(userns_pipe[0]);
			close(userns_pipe[1]);
			if (overlay_mnt_fd >= 0)
				close(overlay_mnt_fd); /* the detached idmapped overlay is freed with its last fd */
		}
		if (bpf_prog_fd >= 0)
			close(bpf_prog_fd);
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	if (ret == 0) {
		/* Child: from here on we live inside the new namespaces. */
		close(diag_pipe[0]);
		if (want_net)
			close(net_pipe[1]);

		/*
		 * ADR-0179 phase 2: with CLONE_NEWUSER in the clone flags,
		 * block until the parent has written our uid/gid maps. Until
		 * then this process holds no valid mapped identity, and every
		 * privileged step below (the overlay mount included) would run
		 * with the wrong credentials. Closing our own write end first
		 * makes a parent that dies before writing surface as a clean
		 * EOF here (read returns 0) rather than an indefinite hang.
		 */
		if (spec->userns_enabled) {
			char c;

			close(userns_pipe[1]);
			if (read(userns_pipe[0], &c, 1) != 1) {
				child_diag(diag_pipe[1], "child: userns map sync");
				_exit(121);
			}
			close(userns_pipe[0]);
		}

		/*
		 * Each pre-exec setup step gets its own exit code (110-119)
		 * rather than a single uniform 126 -- the parent (and anything
		 * further up the chain that only ever sees a plain wait()
		 * exit status, e.g. pkg_build_completed()'s "build failed
		 * (exit status N)") had no way to tell these ten completely
		 * different failure modes apart otherwise. child_diag() writes
		 * the same "step: strerror(errno)" text perror() used to send
		 * to this process's own stderr into diag_pipe[1] instead (see
		 * container_create()'s own header comment for why this is a
		 * real, always-on diagnostic channel rather than the numeric
		 * code being the only signal a caller ever sees), but a
		 * distinct code STILL matters too: container_decode_exit_status()
		 * needs it as a fallback category when the diag pipe itself
		 * is unavailable (a daemon restart losing an already-exited
		 * container's own pipe fd). 127 is deliberately left alone:
		 * it's execve()'s own existing code below, already a
		 * recognizable, conventional "exec failed" signal independent
		 * of this scheme.
		 */
		if (mountns_make_private() != 0) {
			child_diag(diag_pipe[1], "child: mountns_make_private");
			_exit(110);
		}
		/*
		 * ADR-0179 phase 2b: a userns container's overlay was already
		 * mounted by the parent (above) and is inherited through this
		 * child's CLONE_NEWNS copy of the mount tree -- the child only
		 * pivots into it, never mounts it (the mapped root can't). So the
		 * child-side overlay_create runs for the non-userns case only.
		 */
		if (!spec->userns_enabled) {
			int overlay_ret = overlay_create(&spec->ov);

			/*
			 * overlay_create() itself already distinguishes six
			 * named steps plus (for the actual mount(2) call) the
			 * real errno -- translated here into its own small,
			 * disjoint exit-code range (130-136 for the six named
			 * steps, 141-255 for a mount(2) errno in [1,115], the
			 * same shared range the final execve() below also uses)
			 * so a daemon-layer caller with log-store access (pkg.c)
			 * doesn't just learn "overlay_create failed" but exactly
			 * which of its own six steps, and for the mount itself,
			 * the kernel's own real reason. The "overlay mount" vs
			 * "exec" ambiguity that same shared 141-255 range would
			 * otherwise leave in the bare exit code (both reachable
			 * only in mutual exclusion, but genuinely indistinguishable
			 * from the number alone) is exactly what the diag pipe's
			 * own distinct message prefixes below resolve.
			 */
			if (overlay_ret != 0) {
				child_diag(diag_pipe[1], "child: overlay_create");
				if (overlay_ret <= OVERLAY_ERR_MOUNT_ERRNO_BASE)
					_exit(140 + (OVERLAY_ERR_MOUNT_ERRNO_BASE - overlay_ret));
				_exit(129 - overlay_ret);
			}
		}
		/*
		 * ADR-0179 phase 2c: attach the parent's detached, id-mapped
		 * overlay mount into THIS child's own mount namespace with
		 * move_mount(). A mount the child attaches itself is not
		 * MNT_LOCKED (so pivot_root accepts it, unlike a userns-inherited
		 * mount -- phase 2b), and move_mount preserves the id-mapping the
		 * parent set (unlike a plain bind, which would strip it). Non-userns
		 * mounted merged itself in overlay_create above and needs none of
		 * this.
		 */
		if (spec->userns_enabled) {
			if (kx_move_mount(overlay_mnt_fd, "", -1, spec->ov.merged,
			                  KX_MOVE_MOUNT_F_EMPTY_PATH) != 0) {
				child_diag(diag_pipe[1], "child: move_mount idmapped overlay");
				_exit(122);
			}
			close(overlay_mnt_fd);
		}
		{
			int mountns_pivot_ret = mountns_pivot(spec->ov.merged, &spec->mnt);

			if (mountns_pivot_ret != 0) {
				child_diag_mountns_pivot(diag_pipe[1], mountns_pivot_ret);
				_exit(112);
			}
		}
		if (container_dev_mknod(spec->devices, spec->device_count) != 0) {
			child_diag(diag_pipe[1], "child: container_dev_mknod");
			_exit(113);
		}

		if (spec->ns.hostname != NULL &&
		    sethostname(spec->ns.hostname, strlen(spec->ns.hostname)) != 0) {
			child_diag(diag_pipe[1], "child: sethostname");
			_exit(114);
		}

		if (want_net) {
			if (container_net_child_configure(spec->nets, spec->net_count, net_pipe[0]) != 0) {
				child_diag(diag_pipe[1], "child: container_net_child_configure");
				_exit(115);
			}
			close(net_pipe[0]);
		}

		if (container_net_install_routes(spec->routes, spec->route_count) != 0) {
			child_diag(diag_pipe[1], "child: container_net_install_routes");
			_exit(116);
		}
		if (spec->ip_forward && container_net_enable_ip_forward() != 0) {
			child_diag(diag_pipe[1], "child: container_net_enable_ip_forward");
			_exit(117);
		}
		for (i = 0; i < spec->sysctl_count; i++) {
			if (container_net_apply_sysctl(spec->sysctls[i].key, spec->sysctls[i].value) != 0) {
				child_diag(diag_pipe[1], "child: container_net_apply_sysctl");
				_exit(118);
			}
		}

		if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
			child_diag(diag_pipe[1], "child: prctl(PR_SET_PDEATHSIG)");
			_exit(119);
		}

		if (spec->capture_output) {
			dup2(spec->stdout_fd, STDOUT_FILENO);
			dup2(spec->stderr_fd, STDERR_FILENO);
			/* Close the original fds once dup2()'d onto 1/2 --
			 * both often alias the same pipe write end (see
			 * pkg.c), so a bare close() without the guard would
			 * double-close a already-closed fd if stderr_fd ==
			 * stdout_fd and both already collapsed onto the same
			 * number as one of the standard streams. */
			if (spec->stdout_fd != STDOUT_FILENO && spec->stdout_fd != STDERR_FILENO)
				close(spec->stdout_fd);
			if (spec->stderr_fd != STDOUT_FILENO && spec->stderr_fd != STDERR_FILENO &&
			    spec->stderr_fd != spec->stdout_fd)
				close(spec->stderr_fd);
		}

		if (container_caps_drop(spec->cap_add, spec->cap_add_count) != 0) {
			child_diag(diag_pipe[1], "child: container_caps_drop");
			_exit(120);
		}

		execve(spec->argv[0], spec->argv, spec->envp);
		{
			/*
			 * The exact same encoding overlay_create()'s own mount(2)
			 * errno already uses (140 + errno, 141-255) -- deliberately
			 * the SAME shared range, not a second disjoint one: the
			 * two are mutually exclusive within a single run (this
			 * execve() is only ever reached once overlay_create() has
			 * already succeeded), so reusing it is unambiguous, and it
			 * gives this, the last and most likely to actually matter
			 * spot where a bare "_exit(127)" hid real information
			 * (ENOENT the interpreter/binary genuinely missing, EACCES
			 * not executable -- lost +x bit or a noexec mount, ENOEXEC
			 * bad ELF format, ELIBBAD 80 "corrupted shared library" --
			 * a real, confirmed-live case that needed exactly this
			 * wide a range), the full width of the byte rather than a
			 * cramped half of it. 127 itself stays the fallback for an
			 * errno too large even for this, preserving its own
			 * existing meaning as "some exec-class failure."
			 */
			int exec_errno = errno;
			char prefix[PATH_MAX + 32];

			snprintf(prefix, sizeof(prefix), "child: execve(%s)", spec->argv[0]);
			errno = exec_errno;
			child_diag(diag_pipe[1], prefix);
			if (exec_errno > 0 && exec_errno <= OVERLAY_ERR_MOUNT_ERRNO_MAX)
				_exit(140 + exec_errno);
			_exit(127);
		}
	}

	/* Parent. */
	close(diag_pipe[1]);
	/*
	 * Phase 2c: the child inherited its own copy of the detached id-mapped
	 * overlay fd at clone3; drop the parent's copy. The detached mount
	 * stays alive on the child's copy until it move_mounts it in.
	 */
	if (spec->userns_enabled && overlay_mnt_fd >= 0)
		close(overlay_mnt_fd);

	/*
	 * ADR-0179 phase 2: write the child's uid/gid maps, then release it.
	 * This must happen before the child can reach any privileged step, so
	 * it comes first in the parent -- ahead of the veth handshake below,
	 * which the child only reaches long after the userns barrier. On any
	 * failure, closing the write end EOFs the child's blocked read (clean
	 * _exit(121)); we then also SIGKILL+reap to abort creation, matching
	 * the interface-attach failure path's own posture.
	 */
	if (spec->userns_enabled) {
		int userns_ok, saved_errno;

		close(userns_pipe[0]);
		userns_ok = (write_userns_maps((pid_t)ret, spec->userns_uid_base,
		                               spec->userns_len) == 0) &&
		            (write(userns_pipe[1], "x", 1) == 1);
		saved_errno = errno;
		close(userns_pipe[1]);
		if (!userns_ok) {
			siginfo_t info;

			errno = saved_errno;
			perror("container_create: userns map/release");
			container_set_last_error_step("container_create: userns map/release");
			if (want_net) {
				close(net_pipe[0]);
				close(net_pipe[1]);
			}
			sys_pidfd_send_signal(pidfd, SIGKILL);
			waitid(P_PIDFD, pidfd, &info, WEXITED);
			close(diag_pipe[0]);
			close(pidfd);
			if (bpf_prog_fd >= 0)
				close(bpf_prog_fd);
			close(cgroup_fd);
			errno = saved_errno;
			return -1;
		}
	}

	if (want_net) {
		close(net_pipe[0]);
		if (container_net_host_setup(spec->nets, spec->net_count, (pid_t)ret, net_pipe[1]) != 0) {
			/*
			 * The child is already blocked reading net_pipe[0]
			 * waiting for the veth name; closing our write end
			 * without writing makes its read() see EOF and fail
			 * cleanly (_exit(115)) instead of hanging forever.
			 */
			int saved_errno = errno;
			siginfo_t info;

			perror("container_create: container_net_host_setup");
			container_set_last_error_step("container_create: container_net_host_setup");
			close(net_pipe[1]);
			waitid(P_PIDFD, pidfd, &info, WEXITED);
			close(diag_pipe[0]);
			close(pidfd);
			if (bpf_prog_fd >= 0)
				close(bpf_prog_fd);
			close(cgroup_fd);
			errno = saved_errno;
			return -1;
		}
		close(net_pipe[1]);
	}

	out->interfaces_netns_fd = -1;
	if (spec->interface_count > 0) {
		const char *iface_ptrs[CONTAINER_MAX_INTERFACES];
		int i;

		for (i = 0; i < spec->interface_count; i++)
			iface_ptrs[i] = spec->interfaces[i];

		if (container_net_host_attach_interfaces(iface_ptrs, spec->interface_count, (pid_t)ret,
		                                          &out->interfaces_netns_fd) != 0) {
			/*
			 * Unlike the veth path above, the child was never blocked
			 * waiting on anything interface-related -- it may already
			 * be running. There is no clean "let it fail on its own"
			 * signal to send, so an explicit kill is the only correct
			 * way to abort creation here.
			 */
			int saved_errno = errno;
			siginfo_t info;

			perror("container_create: container_net_host_attach_interfaces");
			container_set_last_error_step("container_create: container_net_host_attach_interfaces");
			sys_pidfd_send_signal(pidfd, SIGKILL);
			waitid(P_PIDFD, pidfd, &info, WEXITED);
			close(diag_pipe[0]);
			close(pidfd);
			if (bpf_prog_fd >= 0)
				close(bpf_prog_fd);
			close(cgroup_fd);
			errno = saved_errno;
			return -1;
		}
	}

	out->pid = (pid_t)ret;
	out->cgroup_fd = cgroup_fd;
	out->pidfd = pidfd;
	out->bpf_prog_fd = bpf_prog_fd;
	out->diag_fd = diag_pipe[0];
	return 0;
}

int container_wait(const struct container_handle *h, int *exit_status)
{
	siginfo_t info;

	if (waitid(P_PIDFD, h->pidfd, &info, WEXITED) != 0)
		return -1;

	*exit_status = info.si_status;
	return 0;
}

ssize_t container_read_diag(struct container_handle *h, char *buf, size_t bufsize)
{
	ssize_t total = 0, n;

	buf[0] = '\0';
	if (h->diag_fd < 0)
		return 0;

	while (total < (ssize_t)bufsize - 1) {
		n = read(h->diag_fd, buf + total, bufsize - 1 - (size_t)total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(h->diag_fd);
			h->diag_fd = -1;
			return -1;
		}
		if (n == 0)
			break;
		total += n;
	}
	buf[total] = '\0';
	/* Trim the single trailing '\n' child_diag() always writes -- the
	 * caller's own format string (logstore_write()/a JSON field) adds
	 * its own line ending or delimiter, doesn't need ours too. */
	if (total > 0 && buf[total - 1] == '\n')
		buf[total - 1] = '\0';
	close(h->diag_fd);
	h->diag_fd = -1;
	return total;
}

void container_decode_exit_status(int exit_status, char *buf, size_t bufsize)
{
	static const struct {
		int code;
		const char *desc;
	} named[] = {
		{ 110, "mountns_make_private failed" },
		{ 112, "mountns_pivot failed" },
		{ 113, "container_dev_mknod failed" },
		{ 114, "sethostname failed" },
		{ 115, "container_net_child_configure failed" },
		{ 116, "container_net_install_routes failed" },
		{ 117, "container_net_enable_ip_forward failed" },
		{ 118, "container_net_apply_sysctl failed" },
		{ 119, "prctl(PR_SET_PDEATHSIG) failed" },
		{ 120, "container_caps_drop failed" },
		{ 121, "userns map sync failed (parent never released the child)" },
		{ 122, "userns id-mapped overlay move_mount failed (pre-pivot_root)" },
		{ 127, "exec failed (errno out of encodable range)" },
		{ 130, "overlay: lowerdir stat failed" },
		{ 131, "overlay: upperdir mkdir failed" },
		{ 132, "overlay: disk quota setup failed" },
		{ 133, "overlay: workdir mkdir failed" },
		{ 134, "overlay: merged mountpoint mkdir failed" },
		{ 135, "overlay: mount options too long" },
		{ 136, "overlay: mount(2) itself failed" },
	};
	size_t i;

	if (exit_status == 0) {
		snprintf(buf, bufsize, "clean exit");
		return;
	}
	for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
		if (named[i].code == exit_status) {
			snprintf(buf, bufsize, "%s", named[i].desc);
			return;
		}
	}
	if (exit_status >= 141 && exit_status <= 140 + OVERLAY_ERR_MOUNT_ERRNO_MAX) {
		/*
		 * Genuinely ambiguous from the number alone (overlay mount(2)
		 * vs. the final execve(), see container_create()'s own
		 * comment on why they share this range) -- the real
		 * disambiguation lives in container_read_diag()'s own text
		 * ("child: overlay_create: ..." vs "child: execve(...): ..."),
		 * not here.
		 */
		snprintf(buf, bufsize, "overlay mount or exec failed: %s",
		         strerror(exit_status - 140));
		return;
	}
	snprintf(buf, bufsize, "exited with status %d", exit_status);
}
