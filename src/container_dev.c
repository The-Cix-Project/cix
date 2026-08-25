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
	/* Prologue (4) + one 4-insn comparison block per device + a 2-insn
	 * deny epilogue + a 2-insn allow epilogue. */
	struct cix_bpf_insn prog[4 + 4 * CONTAINER_MAX_DEVICES + 4];
	struct cix_bpf_prog_load_attr load_attr;
	struct cix_bpf_prog_attach_attr attach_attr;
	static const char license[] = "GPL";
	int idx = 0;
	int allow_idx;
	int prog_fd;
	int i;

	if (device_count <= 0) {
		*out_prog_fd = -1;
		return 0;
	}

	allow_idx = 4 + 4 * device_count + 2;

	/* r2 = ctx->major; r3 = ctx->minor; r4 = ctx->access_type & 0xffff */
	emit(prog, &idx, CIX_OP_LDX_MEM_W, CIX_REG_2, CIX_REG_1, 4, 0);
	emit(prog, &idx, CIX_OP_LDX_MEM_W, CIX_REG_3, CIX_REG_1, 8, 0);
	emit(prog, &idx, CIX_OP_LDX_MEM_W, CIX_REG_4, CIX_REG_1, 0, 0);
	emit(prog, &idx, CIX_OP_ALU64_AND_K, CIX_REG_4, 0, 0, 0xffff);

	for (i = 0; i < device_count; i++) {
		int devtype = (devices[i].type == DEVICE_NODE_BLOCK) ? CIX_BPF_DEVCG_DEV_BLOCK
		                                                      : CIX_BPF_DEVCG_DEV_CHAR;

		/* Any mismatch falls through to the next block (or the deny
		 * epilogue, for the last device); a full match jumps to allow. */
		emit(prog, &idx, CIX_OP_JMP_JNE_K, CIX_REG_2, 0, 3, (int32_t)devices[i].major);
		emit(prog, &idx, CIX_OP_JMP_JNE_K, CIX_REG_3, 0, 2, (int32_t)devices[i].minor);
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
