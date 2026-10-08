#include "internal.h"
#include "linux_compat.h"
#include "pathutil.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/*
 * eBPF instruction-class/opcode bytes needed to build the
 * BPF_PROG_TYPE_CGROUP_DEVICE program below, named per the standard
 * (kernel-stable) eBPF ISA encoding rather than left as magic numbers
 * -- same "named constant, not a bare hex literal" posture this
 * project already uses for e.g. CLONE_INTO_CGROUP.
 *
 * BPF_LDX|BPF_MEM|BPF_W : load a 4-byte field out of the context
 * BPF_ALU64|BPF_AND|BPF_K : dst &= imm
 * BPF_ALU64|BPF_MOV|BPF_K : dst = imm
 * BPF_JMP|BPF_JNE|BPF_K : branch if dst != imm
 * BPF_JMP|BPF_JA         : unconditional branch
 * BPF_JMP|BPF_EXIT        : return r0
 */
#define CIX_OP_LDX_MEM_W  0x61
#define CIX_OP_ALU64_AND_K 0x57
#define CIX_OP_ALU64_MOV_K 0xb7
#define CIX_OP_JMP_JNE_K  0x55
#define CIX_OP_JMP_JA     0x05
#define CIX_OP_JMP_EXIT   0x95

#define CIX_REG_0 0
#define CIX_REG_1 1
#define CIX_REG_2 2
#define CIX_REG_3 3
#define CIX_REG_4 4

/* bpf_cgroup_dev_ctx.access_type low 16 bits, per security/device_cgroup.c */
#define CIX_BPF_DEVCG_DEV_BLOCK 1
#define CIX_BPF_DEVCG_DEV_CHAR  2

/*
 * The image baseline's own nodes, permitted ahead of whatever a
 * container declared (ADR-0331, #578).
 *
 * Expanded from the one list in container.h, which
 * pkg_seed_image_baseline() stages into every image. They were two
 * lists and disagreed: this program is built from the DECLARED devices
 * alone, so a container that declared one device got a deny epilogue
 * that denied /dev/null -- and nothing had hit it because a container
 * declaring nothing gets no program at all.
 *
 * Only the numbers are kept, because every baseline node is a char
 * device; the list's own comment says why that is deliberate.
 */
static const struct {
	unsigned int major, minor;
} g_baseline_devices[] = {
#define CIX_BASELINE_ENTRY_(name, maj, min) { maj, min },
	CIX_BASELINE_DEVICES(CIX_BASELINE_ENTRY_)
#undef CIX_BASELINE_ENTRY_
};
#define CIX_BASELINE_N ((int)(sizeof(g_baseline_devices) / sizeof(g_baseline_devices[0])))

static void emit(struct cix_bpf_insn *prog, int *idx, uint8_t code, uint8_t dst, uint8_t src,
                  int16_t off, int32_t imm)
{
	prog[*idx].code = code;
	prog[*idx].regs = (uint8_t)((dst & 0x0f) | ((src & 0x0f) << 4));
	prog[*idx].off = off;
	prog[*idx].imm = imm;
	(*idx)++;
}

int container_dev_bpf_attach(int cgroup_fd, const struct device_spec *devices, int device_count,
                              int *out_prog_fd)
{
	/* Prologue (4) + one 4-insn comparison block per device, baseline
	 * and declared alike + a 2-insn deny epilogue + a 2-insn allow
	 * epilogue. The baseline count is in the dimension because the
	 * program now carries those blocks too (ADR-0331). */
	struct cix_bpf_insn prog[4 + 4 * (CIX_BASELINE_N + CONTAINER_MAX_DEVICES) + 4];
	struct cix_bpf_prog_load_attr load_attr;
	struct cix_bpf_prog_attach_attr attach_attr;
	static const char license[] = "GPL";
	int idx = 0;
	int allow_idx;
	int prog_fd;
	int total;
	int i;

	/*
	 * UNCHANGED, and load-bearing: a container that declares no devices
	 * is governed by no program at all. ADR-0331 added the baseline to
	 * the allow set for containers that DO declare one; it must not
	 * start restricting the ones that were never restricted -- every
	 * container on 192.168.15.95 declares an empty list (measured
	 * 2026-10-07). test_devices gates this arm by name.
	 */
	if (device_count <= 0) {
		*out_prog_fd = -1;
		return 0;
	}

	total = CIX_BASELINE_N + device_count;
	allow_idx = 4 + 4 * total + 2;

	/* r2 = ctx->major; r3 = ctx->minor; r4 = ctx->access_type & 0xffff */
	emit(prog, &idx, CIX_OP_LDX_MEM_W, CIX_REG_2, CIX_REG_1, 4, 0);
	emit(prog, &idx, CIX_OP_LDX_MEM_W, CIX_REG_3, CIX_REG_1, 8, 0);
	emit(prog, &idx, CIX_OP_LDX_MEM_W, CIX_REG_4, CIX_REG_1, 0, 0);
	emit(prog, &idx, CIX_OP_ALU64_AND_K, CIX_REG_4, 0, 0, 0xffff);

	/*
	 * ONE loop over the baseline blocks then the declared ones, rather
	 * than two loops with the same body: the block shape is identical
	 * and a second copy of it is the parallel implementation this
	 * change exists to remove. Baseline first so a declared grant that
	 * happens to name a baseline node is simply a redundant block, not
	 * a conflict.
	 */
	for (i = 0; i < total; i++) {
		unsigned int major, minor;
		int devtype;

		if (i < CIX_BASELINE_N) {
			major = g_baseline_devices[i].major;
			minor = g_baseline_devices[i].minor;
			devtype = CIX_BPF_DEVCG_DEV_CHAR; /* every baseline node is char */
		} else {
			const struct device_spec *d = &devices[i - CIX_BASELINE_N];

			major = d->major;
			minor = d->minor;
			devtype = (d->type == DEVICE_NODE_BLOCK) ? CIX_BPF_DEVCG_DEV_BLOCK
			                                         : CIX_BPF_DEVCG_DEV_CHAR;
		}

		/* Any mismatch falls through to the next block (or the deny
		 * epilogue, for the last device); a full match jumps to allow. */
		emit(prog, &idx, CIX_OP_JMP_JNE_K, CIX_REG_2, 0, 3, (int32_t)major);
		emit(prog, &idx, CIX_OP_JMP_JNE_K, CIX_REG_3, 0, 2, (int32_t)minor);
		emit(prog, &idx, CIX_OP_JMP_JNE_K, CIX_REG_4, 0, 1, devtype);
		emit(prog, &idx, CIX_OP_JMP_JA, 0, 0, (int16_t)(allow_idx - idx - 1), 0);
	}

	/* deny */
	emit(prog, &idx, CIX_OP_ALU64_MOV_K, CIX_REG_0, 0, 0, 0);
	emit(prog, &idx, CIX_OP_JMP_EXIT, 0, 0, 0, 0);
	/* allow */
	emit(prog, &idx, CIX_OP_ALU64_MOV_K, CIX_REG_0, 0, 0, 1);
	emit(prog, &idx, CIX_OP_JMP_EXIT, 0, 0, 0, 0);

	memset(&load_attr, 0, sizeof(load_attr));
	load_attr.prog_type = CIX_BPF_PROG_TYPE_CGROUP_DEVICE;
	load_attr.insn_cnt = (uint32_t)idx;
	load_attr.insns = (uint64_t)(uintptr_t)prog;
	load_attr.license = (uint64_t)(uintptr_t)license;
	load_attr.expected_attach_type = CIX_BPF_CGROUP_DEVICE;
	memcpy(load_attr.prog_name, "cix_devcg", sizeof("cix_devcg"));

	prog_fd = (int)sys_bpf(CIX_BPF_PROG_LOAD, &load_attr, sizeof(load_attr));
	if (prog_fd < 0)
		return -1;

	memset(&attach_attr, 0, sizeof(attach_attr));
	attach_attr.target_fd = (uint32_t)cgroup_fd;
	attach_attr.attach_bpf_fd = (uint32_t)prog_fd;
	attach_attr.attach_type = CIX_BPF_CGROUP_DEVICE;

	if (sys_bpf(CIX_BPF_PROG_ATTACH, &attach_attr, sizeof(attach_attr)) != 0) {
		int saved_errno = errno;

		close(prog_fd);
		errno = saved_errno;
		return -1;
	}

	*out_prog_fd = prog_fd;
	return 0;
}

int container_dev_bpf_detach(int cgroup_fd, int prog_fd)
{
	struct cix_bpf_prog_attach_attr detach_attr;

	memset(&detach_attr, 0, sizeof(detach_attr));
	detach_attr.target_fd = (uint32_t)cgroup_fd;
	detach_attr.attach_bpf_fd = (uint32_t)prog_fd;
	detach_attr.attach_type = CIX_BPF_CGROUP_DEVICE;

	if (sys_bpf(CIX_BPF_PROG_DETACH, &detach_attr, sizeof(detach_attr)) != 0)
		return -1;
	return 0;
}

int container_dev_mknod(const struct device_spec *devices, int device_count)
{
	int i;

	for (i = 0; i < device_count; i++) {
		char dir[PATH_MAX];
		char *slash;
		mode_t mode;

		if (snprintf(dir, sizeof(dir), "%s", devices[i].dev_path) >= (int)sizeof(dir)) {
			errno = ENAMETOOLONG;
			return -1;
		}

		slash = strrchr(dir, '/');
		if (slash != NULL && slash != dir) {
			*slash = '\0';
			if (cix_mkdir_p(dir) != 0)
				return -1;
		}

		mode = (mode_t)((devices[i].type == DEVICE_NODE_BLOCK ? S_IFBLK : S_IFCHR) | 0666);
		if (mknod(devices[i].dev_path, mode,
		          makedev(devices[i].major, devices[i].minor)) != 0 &&
		    errno != EEXIST)
			return -1;
		/* mknod()'s own requested mode is subject to the calling
		 * process's umask (POSIX) -- explicit chmod() guarantees the
		 * real 0666 this function already intends, regardless of
		 * whatever umask this daemon happens to have inherited. See
		 * pkg_seed_image_baseline()'s own identical fix and comment
		 * for the live confirmation (a real /dev/null ended up 0644,
		 * not 0666, the same class of bug this device-passthrough
		 * path shares. */
		if (chmod(devices[i].dev_path, mode & 07777) != 0)
			return -1;
	}

	return 0;
}

/*
 * The standard device nodes every container gets, created HOST-SIDE into
 * a container's own rootfs before it starts (ADR-0333, #581).
 *
 * WHY HERE AND NOT IN AN IMAGE. These used to be staged into every image
 * by pkg_seed_image_baseline(), which made the platform a writer of
 * image content beside the package manager -- unversioned, undeclared,
 * and invisible, so a node added to the list reached newly created
 * images and no others (#577: `tty` shipped and `jumpbox` still could
 * not run sudo). An image's content is now only its packages
 * (ADR-0333), and these are per-container state like /etc/passwd and
 * /etc/resolv.conf already are.
 *
 * WHY THE PARENT AND NOT THE CHILD. container_dev_mknod() above runs in
 * the cloned child, after pivot_root, and cannot do this job: mknod is
 * gated on CAP_MKNOD against the INITIAL user namespace, which no
 * userns container has ever held (#321, src/container.c's own comment on
 * the id-mapped rootfs), and userns is the default. The daemon's own
 * process is real root in the initial namespace, so it can. That is
 * also why this takes a host path rather than operating relative to "/"
 * the way container_dev_mknod() does.
 *
 * WHY IN THIS FILE. container_dev_bpf_attach() below expands the same
 * CIX_BASELINE_DEVICES list for its allow-set, so the nodes a container
 * gets and the nodes its policy permits are one list in one file --
 * ADR-0331 exists because those two diverged (#578: a container with
 * /dev/null staged and a policy denying it). Keep them together.
 *
 * owner_offset is the container's subordinate uid base, or 0 for a
 * container that is not id-mapped -- the same convention
 * stage_container_bytes() uses for every other staged file: with an
 * offset, the node is owned by the offset itself, which is what the
 * container's own userns maps back to uid 0.
 *
 * rootfs_dir "" MEANS THE CALLER'S CURRENT ROOT, which is how the child
 * calls this after pivot_root. Both callers exist and the split is a
 * kernel constraint, not a preference:
 *
 *   - a USERNS container is staged by the PARENT, into its own rootfs
 *     copy, because its child holds no CAP_MKNOD in the initial
 *     namespace (#321);
 *   - a NON-USERNS container is staged by the CHILD, at "", because its
 *     child IS real root -- and because the parent must not pre-create
 *     an overlay upperdir it would otherwise get as a btrfs SUBVOLUME.
 *     overlay_create_btrfs_upperdir() tolerates EEXIST on the
 *     subvolume ioctl on the assumption that a previous run made a
 *     subvolume; a plain directory left there by this function would
 *     satisfy that tolerance and silently cost the container its disk
 *     quota (ADR-0062, #678). Measured by reading src/overlay.c:46-80
 *     before writing this, not after.
 */
int container_dev_stage_baseline(const char *rootfs_dir, uid_t owner_offset)
{
	static const struct {
		const char *name;
		unsigned int major, minor;
	} nodes[] = {
#define CIX_BASELINE_ENTRY_(dname, maj, min) { dname, maj, min },
		CIX_BASELINE_DEVICES(CIX_BASELINE_ENTRY_)
#undef CIX_BASELINE_ENTRY_
	};
	char dev_dir[PATH_MAX];
	char path[PATH_MAX];
	size_t i;

	if (rootfs_dir == NULL) {
		errno = EINVAL;
		return -1;
	}
	/* "" yields "/dev" -- the child's case, deliberately, rather than a
	 * second code path for it. */
	if (snprintf(dev_dir, sizeof(dev_dir), "%s/dev", rootfs_dir) >= (int)sizeof(dev_dir)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (cix_mkdir_p(dev_dir) != 0)
		return -1;
	/*
	 * /dev itself is chowned too, not just the nodes. The container's
	 * own init mounts devpts at /dev/pts from inside
	 * (mountns_pivot()), which is a create in this directory -- and for
	 * an id-mapped container that create is performed by the mapped
	 * root, which must therefore own it.
	 */
	if (owner_offset != 0 && chown(dev_dir, owner_offset, (gid_t)owner_offset) != 0)
		return -1;

	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		mode_t mode = (mode_t)(S_IFCHR | 0666);

		if (snprintf(path, sizeof(path), "%s/%s", dev_dir, nodes[i].name) >= (int)sizeof(path)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		if (mknod(path, mode, makedev(nodes[i].major, nodes[i].minor)) != 0 && errno != EEXIST)
			return -1;
		/*
		 * Unconditional, including on the EEXIST path: mknod()'s mode
		 * is subject to this process's umask (POSIX), and a real
		 * /dev/null created without this ended up 0644 rather than the
		 * 0666 asked for -- measured, and the same fix
		 * container_dev_mknod() above and pkg_seed_image_baseline()
		 * both carry. A standard device node must stay
		 * world-writable: a non-root process inside a container
		 * redirecting to /dev/null is entirely ordinary.
		 */
		if (chmod(path, mode & 07777) != 0)
			return -1;
		if (owner_offset != 0 && chown(path, owner_offset, (gid_t)owner_offset) != 0)
			return -1;
	}

	/*
	 * /dev/ptmx is a SYMLINK to pts/ptmx, not a node -- its target lives
	 * on the devpts mount the container makes for itself, so there is
	 * nothing here to permit and it is deliberately absent from
	 * CIX_BASELINE_DEVICES (see that list's own comment). lchown rather
	 * than chown: the target does not exist yet at this point, and
	 * chown would follow the link and fail ENOENT.
	 */
	if (snprintf(path, sizeof(path), "%s/ptmx", dev_dir) >= (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (symlink("pts/ptmx", path) != 0 && errno != EEXIST)
		return -1;
	if (owner_offset != 0 && lchown(path, owner_offset, (gid_t)owner_offset) != 0)
		return -1;

	return 0;
}
