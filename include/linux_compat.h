#ifndef LINUX_COMPAT_H
#define LINUX_COMPAT_H

#include <stdint.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_clone3
#define SYS_clone3 435
#endif

/*
 * open_tree(2)/move_mount(2) -- the new mount API (Linux 5.2+), used to
 * bind a host directory into a RUNNING container's mount namespace
 * (issue #92 part 2).
 *
 * The obvious approach fails in a way worth recording: a forked helper
 * that setns()es into the container's mount namespace and then bind
 * mounts from /proc/self/fd/<n> does not work, because the container's
 * /proc belongs to the container's PID namespace and the helper is not
 * in it -- /proc/self simply does not resolve there. open_tree() sidesteps
 * the whole problem: it detaches a mount tree into an fd BEFORE entering
 * the namespace, and move_mount() attaches that fd afterwards, so no
 * path in the host namespace ever has to be nameable from inside the
 * container's.
 *
 * No glibc wrappers exist for either, same as pivot_root/clone3 below.
 */
#ifndef SYS_open_tree
#define SYS_open_tree 428
#endif
#ifndef SYS_move_mount
#define SYS_move_mount 429
#endif
#ifndef OPEN_TREE_CLONE
#define OPEN_TREE_CLONE 1
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#endif
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif

static inline int cix_open_tree(int dfd, const char *path, unsigned int flags)
{
	return (int)syscall(SYS_open_tree, dfd, path, flags);
}

static inline int cix_move_mount(int from_dfd, const char *from_path, int to_dfd,
                                const char *to_path, unsigned int flags)
{
	return (int)syscall(SYS_move_mount, from_dfd, from_path, to_dfd, to_path, flags);
}

/*
 * mount_setattr(2) (ADR-0207 phase 3): applies MOUNT_ATTR_IDMAP to a
 * detached mount, presenting host-uid-0 ownership as the target
 * userns's mapped ids -- the primitive ADR-0179 proved works on plain
 * mounts (and only overlay could not use). The struct is the kernel's
 * own mount_attr, all-u64, naturally aligned -- no packing concerns.
 * The syscall takes sizeof() explicitly, so the kernel knows exactly
 * which fields this caller speaks.
 */
#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442
#endif
#ifndef MOUNT_ATTR_IDMAP
#define MOUNT_ATTR_IDMAP 0x00100000
#endif
#ifndef CIX_AT_EMPTY_PATH
#define CIX_AT_EMPTY_PATH 0x1000
#endif

struct cix_mount_attr {
	uint64_t attr_set;
	uint64_t attr_clr;
	uint64_t propagation;
	uint64_t userns_fd;
};

static inline int cix_mount_setattr(int dfd, const char *path, unsigned int flags,
                                   struct cix_mount_attr *attr)
{
	return (int)syscall(SYS_mount_setattr, dfd, path, flags, attr,
	                    sizeof(struct cix_mount_attr));
}

#ifndef SYS_pivot_root
#define SYS_pivot_root 155
#endif

#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

#ifndef SYS_bpf
#define SYS_bpf 321
#endif

/*
 * glibc declares no wrapper for clone3(2); the kernel uapi struct is
 * ABI-stable, so we declare it ourselves rather than pull in
 * <linux/sched.h>, which clashes with glibc's <sched.h> over CLONE_*
 * and struct sched_param.
 */
struct clone_args {
	uint64_t flags;
	uint64_t pidfd;
	uint64_t child_tid;
	uint64_t parent_tid;
	uint64_t exit_signal;
	uint64_t stack;
	uint64_t stack_size;
	uint64_t tls;
	uint64_t set_tid;
	uint64_t set_tid_size;
	uint64_t cgroup;
};

#ifndef CLONE_INTO_CGROUP
#define CLONE_INTO_CGROUP 0x200000000ULL
#endif

#ifndef CLONE_NEWCGROUP
#define CLONE_NEWCGROUP 0x02000000
#endif

#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x00001000
#endif

static inline long sys_clone3(struct clone_args *args, size_t size)
{
	return syscall(SYS_clone3, args, size);
}

static inline int sys_pivot_root(const char *new_root, const char *put_old)
{
	return (int)syscall(SYS_pivot_root, new_root, put_old);
}

static inline int sys_pidfd_send_signal(int pidfd, int sig)
{
	return (int)syscall(SYS_pidfd_send_signal, pidfd, sig, NULL, 0);
}

/*
 * TCC's bundled/system header view doesn't declare pidfd_open() even
 * though glibc 2.36 on this system actually has it (confirmed: only
 * SYS_pidfd_open, the syscall number macro, resolves) -- same class
 * of gap as pivot_root/clone3, raw syscall per ADR-0002. Needed for
 * async-tracking a plain fork()'d subprocess (e.g. Phase 10's package
 * fetch step) the same non-blocking way clone3()'s CLONE_PIDFD
 * already gives containers their pidfd for free.
 */
static inline int sys_pidfd_open(pid_t pid, unsigned int flags)
{
	return (int)syscall(SYS_pidfd_open, pid, flags);
}

/*
 * glibc declares no wrapper for bpf(2), and the real kernel uapi
 * union bpf_attr (<linux/bpf.h>) isn't safe to include directly: it
 * pulls in <linux/types.h>'s __u8/__u32/__u64 family, the same class
 * of clash struct clone_args already avoids by not including
 * <linux/sched.h>. We self-declare only the two bpf_attr variants
 * this project actually uses (BPF_PROG_LOAD, BPF_PROG_ATTACH/DETACH),
 * not the kernel's full union, for the same reason struct clone_args
 * only declares the clone3(2) fields it needs.
 *
 * This is safe against a newer kernel whose real union bpf_attr has
 * grown extra trailing fields (prog_btf_fd, func_info*, core_relo*,
 * ...): bpf(2)'s own size-negotiation contract treats a user-supplied
 * attr shorter than the kernel's own struct as having every
 * unsupplied trailing field implicitly zero, and only rejects a
 * *longer* user attr whose extra tail bytes are nonzero. Field order
 * and types below match linux/bpf.h's union bpf_attr exactly, offset
 * for offset, confirmed directly against /usr/include/linux/bpf.h.
 * No #pragma pack needed: every field is a naturally-aligned uint32_t
 * or uint64_t, same posture as struct clone_args.
 */
enum cix_bpf_cmd {
	CIX_BPF_PROG_LOAD = 5,
	CIX_BPF_PROG_ATTACH = 8,
	CIX_BPF_PROG_DETACH = 9,
};

enum cix_bpf_prog_type {
	CIX_BPF_PROG_TYPE_CGROUP_DEVICE = 15,
};

enum cix_bpf_attach_type {
	CIX_BPF_CGROUP_DEVICE = 6,
};

struct cix_bpf_prog_load_attr {
	uint32_t prog_type;
	uint32_t insn_cnt;
	uint64_t insns;			/* (uintptr_t) struct cix_bpf_insn[] */
	uint64_t license;		/* (uintptr_t) NUL-terminated string */
	uint32_t log_level;
	uint32_t log_size;
	uint64_t log_buf;
	uint32_t kern_version;		/* unused for this prog type */
	uint32_t prog_flags;
	char prog_name[16];		/* BPF_OBJ_NAME_LEN */
	uint32_t prog_ifindex;
	uint32_t expected_attach_type;	/* CIX_BPF_CGROUP_DEVICE */
};

struct cix_bpf_prog_attach_attr {
	uint32_t target_fd;		/* the cgroup's O_PATH fd */
	uint32_t attach_bpf_fd;	/* fd returned by CIX_BPF_PROG_LOAD */
	uint32_t attach_type;		/* CIX_BPF_CGROUP_DEVICE */
	uint32_t attach_flags;		/* 0: one program per cgroup leaf,
					 * every container owns its leaf
					 * 1:1, so there's never a sibling
					 * program to stack with. */
	uint32_t replace_bpf_fd;	/* 0 */
};

/*
 * struct bpf_insn (<linux/bpf.h>) packs dst_reg/src_reg into a single
 * byte via two 4-bit C bitfields. This project has never trusted
 * TCC's bitfield layout for a syscall-ABI struct (see the
 * cix_epoll_event note below -- TCC already silently mishandles
 * __attribute__((packed)) on a struct of our own writing), so the
 * combined register byte is built by hand with plain bit ops
 * (dst_reg in the low nibble, src_reg in the high nibble, matching
 * the x86_64 kernel/GCC bitfield layout) rather than declared as a
 * C bitfield here. 8 bytes total (1+1+2+4), no padding either
 * compiler could disagree on -- same "plain fields, no pragma needed"
 * posture as struct clone_args.
 */
struct cix_bpf_insn {
	uint8_t code;
	uint8_t regs;	/* (dst_reg & 0xf) | ((src_reg & 0xf) << 4) */
	int16_t off;
	int32_t imm;
};

static inline long sys_bpf(int cmd, void *attr, size_t size)
{
	return syscall(SYS_bpf, cmd, attr, size);
}

/*
 * TCC ignores __attribute__((packed)) entirely -- confirmed even on a
 * struct we write ourselves with the attribute, not just on system
 * headers. The real kernel ABI for struct epoll_event is 12 bytes
 * (uint32_t events at offset 0, an 8-byte data union at offset 4, no
 * padding), which is exactly why <sys/epoll.h> marks it packed in the
 * first place. Under TCC's default alignment the system struct
 * compiles to 16 bytes with data at offset 8, so every epoll_ctl()/
 * epoll_wait() call silently corrupts the data field -- intermittently,
 * depending on what garbage ends up at the misread offset.
 *
 * TCC does honor #pragma pack, so we define our own byte-exact
 * replacement and use it everywhere in place of the system struct.
 * The glibc epoll_ctl()/epoll_wait() wrappers only forward the
 * pointer to the kernel syscall -- they never interpret the struct's
 * fields themselves -- so casting our correctly-laid-out pointer to
 * `struct epoll_event *` at the call site is safe.
 */
#pragma pack(push, 1)
union cix_epoll_data {
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;
};
struct cix_epoll_event {
	uint32_t events;
	union cix_epoll_data data;
};
#pragma pack(pop)

static inline int cix_epoll_ctl(int epfd, int op, int fd, struct cix_epoll_event *ev)
{
	return epoll_ctl(epfd, op, fd, (struct epoll_event *)ev);
}

static inline int cix_epoll_wait(int epfd, struct cix_epoll_event *events, int maxevents,
                                 int timeout)
{
	return epoll_wait(epfd, (struct epoll_event *)events, maxevents, timeout);
}

/*
 * Real ext4/XFS-style project-quota tagging (Part 4, bare-metal-
 * readiness plan, ADR-0062) needs struct fsxattr and the
 * FS_IOC_FSGETXATTR/FS_IOC_FSSETXATTR ioctls -- declared here rather
 * than pulling in the kernel uapi <linux/fs.h> directly, which clashes
 * with glibc's own <fcntl.h> (SYNC_FILE_RANGE_WRITE_AND_WAIT defined
 * with a different value by each -- confirmed directly, the same class
 * of kernel-uapi-vs-glibc clash this file already works around for
 * clone3/epoll_event). ABI-stable, taken verbatim from the real kernel
 * header.
 */
struct cix_fsxattr {
	uint32_t fsx_xflags;
	uint32_t fsx_extsize;
	uint32_t fsx_nextents;
	uint32_t fsx_projid;
	uint32_t fsx_cowextsize;
	unsigned char fsx_pad[8];
};

#define CIX_FS_XFLAG_PROJINHERIT 0x00000200

#define CIX_FS_IOC_FSGETXATTR 0x801c581f
#define CIX_FS_IOC_FSSETXATTR 0x401c5820

/*
 * Real btrfs qgroup-based quota enforcement (task #678, ADR-0103) --
 * the btrfs-native counterpart to the ext4/XFS project-quota pair
 * above, needed because btrfs has no quotactl(2)/project-quota
 * support at all: quotas are per-subvolume (qgroups), addressed via
 * these ioctls directly, never quotactl(2). Declared here for the
 * exact same reason as the fsxattr pair -- <linux/btrfs.h> can't be
 * included directly (same class of kernel-uapi-vs-glibc header clash
 * this file already works around elsewhere) -- struct layouts and
 * ioctl numbers below are transcribed verbatim from the real kernel
 * header (confirmed against a live copy of <linux/btrfs.h> in this
 * checkout's own build environment) and the ioctl numbers independently
 * re-derived by hand from the standard _IOC(dir,type,nr,size) encoding
 * as a cross-check -- both agree, and the same derivation correctly
 * reproduces CIX_FS_IOC_FSGETXATTR/FSSETXATTR above byte for byte,
 * confirming the encoding is right.
 */
#define CIX_BTRFS_IOCTL_MAGIC 0x94
#define CIX_BTRFS_PATH_NAME_MAX 4087

struct cix_btrfs_ioctl_vol_args {
	int64_t fd;
	char name[CIX_BTRFS_PATH_NAME_MAX + 1];
};

/*
 * The v2 vol_args, used by SNAP_CREATE_V2 (ADR-0207): a writable
 * snapshot of the subvolume named by `fd` is created under the parent
 * the ioctl is issued on, with leaf name `name`. flags=0 means a
 * writable snapshot (BTRFS_SUBVOL_RDONLY, unused here, would make it
 * read-only). The struct is exactly 4096 bytes -- 8+8+8+32+4040 --
 * which the ioctl number below encodes; all members are naturally
 * 8-byte aligned before the trailing char array, so TCC's default
 * layout matches the kernel's without any packing (unlike the
 * epoll_event case, ADR-0008). `unused` stands in for the kernel's
 * size/qgroup_inherit union, which this project does not use.
 */
#define CIX_BTRFS_SUBVOL_NAME_MAX 4039
struct cix_btrfs_ioctl_vol_args_v2 {
	int64_t fd;
	uint64_t transid;
	uint64_t flags;
	uint64_t unused[4];
	char name[CIX_BTRFS_SUBVOL_NAME_MAX + 1];
};

struct cix_btrfs_qgroup_limit {
	uint64_t flags;
	uint64_t max_rfer;
	uint64_t max_excl;
	uint64_t rsv_rfer;
	uint64_t rsv_excl;
};

struct cix_btrfs_ioctl_qgroup_limit_args {
	uint64_t qgroupid;
	struct cix_btrfs_qgroup_limit lim;
};

struct cix_btrfs_ioctl_quota_ctl_args {
	uint64_t cmd;
	uint64_t status;
};

#define CIX_BTRFS_QGROUP_LIMIT_MAX_RFER (1ULL << 0)
#define CIX_BTRFS_QGROUP_LIMIT_MAX_EXCL (1ULL << 1)
#define CIX_BTRFS_QUOTA_CTL_ENABLE 1

/* _IOW(0x94, 14, struct cix_btrfs_ioctl_vol_args) -- sizeof() 4096 */
#define CIX_BTRFS_IOC_SUBVOL_CREATE 0x5000940e
/* _IOW(0x94, 15, struct cix_btrfs_ioctl_vol_args) -- sizeof() 4096 */
#define CIX_BTRFS_IOC_SNAP_DESTROY 0x5000940f
/* _IOW(0x94, 23, struct cix_btrfs_ioctl_vol_args_v2) -- sizeof() 4096 */
#define CIX_BTRFS_IOC_SNAP_CREATE_V2 0x50009417
/* _IOWR(0x94, 40, struct cix_btrfs_ioctl_quota_ctl_args) -- sizeof() 16 */
#define CIX_BTRFS_IOC_QUOTA_CTL 0xc0109428
/* _IOR(0x94, 43, struct cix_btrfs_ioctl_qgroup_limit_args) -- sizeof() 48 */
#define CIX_BTRFS_IOC_QGROUP_LIMIT 0x8030942b

/* statfs(2) f_type value for a btrfs filesystem (statfs.h's own
 * BTRFS_SUPER_MAGIC) -- no header clash risk for this one (it's a
 * bare integer constant, not a struct/ioctl-number pair), but kept
 * alongside the rest of this project's own btrfs constants for
 * locality rather than pulled from a system header inconsistently. */
#define CIX_BTRFS_SUPER_MAGIC 0x9123683e

#endif /* LINUX_COMPAT_H */
