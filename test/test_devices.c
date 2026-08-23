/*
 * Real-syscall verification for PCI/USB (character/block) device
 * passthrough: proves the actual security property end to end through
 * container_create(), not just that the eBPF program loads.
 *
 * 1. A standalone bpf(2) sanity check (no container involved) isolates
 *    a struct thinc_bpf_insn byte-packing bug from a device-logic bug,
 *    per this project's own "stress-test anything touching a raw
 *    syscall ABI struct" discipline (see ADR-0008/0009).
 * 2. Container A: one real device grant -- its own node opens, and
 *    fstat() confirms the major:minor the kernel actually reports
 *    matches what was granted.
 * 3. Container B: a different real device grant, plus an attempt to
 *    open a device baked into the shared base image but never granted
 *    to B -- denied with EPERM. The concrete "two containers, one
 *    denied" proof.
 * 4. Container C: no devices granted at all -- opens the same
 *    baked-in device fine, byte-for-byte identical to every
 *    container's behavior before this feature existed (No
 *    Regressions).
 */
#include "container.h"
#include "internal.h"
#include "linux_compat.h"
#include "test_image_fixture.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define IMAGE_ROOT "/tmp/device_test/lower"
#define SHARED_DEV_PATH IMAGE_ROOT "/dev/kxtest_shared"
/*
 * Real, universally-recognized device numbers (/dev/null, /dev/zero,
 * /dev/full's own major:minor), not synthetic ones: BPF_CGROUP_DEVICE
 * checks are hierarchical -- every cgroup level from the real root
 * down to this process's own leaf is consulted, and any single level
 * denying fails the whole chain, regardless of what our own leaf's
 * program says. This project's own dev/build environment turns out to
 * already run inside a confining ancestor cgroup (a privileged LXC
 * container's own default device policy) that permits the standard
 * devices below but not arbitrary made-up numbers -- confirmed
 * directly by hand (a fabricated major=234 device was denied even
 * for a plain root shell on the bare host, entirely outside any
 * container this project creates). Real deployment targets (bare
 * metal, or this project's own QEMU boot tests) have no such
 * ancestor, so this constraint is specific to nested dev/CI
 * environments, not a limitation of the mechanism itself.
 */
#define SHARED_MAJOR 1
#define SHARED_MINOR 3 /* /dev/null */

static int mkdir_p1(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	return 0;
}

/* Isolates a struct thinc_bpf_insn byte-packing bug from a device-logic
 * bug: a minimal "mov r0,1; exit" program, no context access, no
 * jumps -- if this alone fails to load, the bug is in the raw
 * instruction encoding, not in container_dev_bpf_attach()'s own
 * per-device comparison chain. */
static int bpf_encoding_sanity_check(void)
{
	struct thinc_bpf_insn prog[2];
	struct thinc_bpf_prog_load_attr attr;
	int fd;

	prog[0].code = 0xb7; /* BPF_ALU64|BPF_MOV|BPF_K */
	prog[0].regs = 0x00;
	prog[0].off = 0;
	prog[0].imm = 1;
	prog[1].code = 0x95; /* BPF_JMP|BPF_EXIT */
	prog[1].regs = 0x00;
	prog[1].off = 0;
	prog[1].imm = 0;

	memset(&attr, 0, sizeof(attr));
	attr.prog_type = THINC_BPF_PROG_TYPE_CGROUP_DEVICE;
	attr.insn_cnt = 2;
	attr.insns = (uint64_t)(uintptr_t)prog;
	attr.license = (uint64_t)(uintptr_t)"GPL";
	attr.expected_attach_type = THINC_BPF_CGROUP_DEVICE;

	fd = (int)sys_bpf(THINC_BPF_PROG_LOAD, &attr, sizeof(attr));
	if (fd < 0) {
		perror("bpf_encoding_sanity_check: BPF_PROG_LOAD");
		return -1;
	}
	close(fd);
	return 0;
}

static int build_container_spec(struct container_spec *spec, const char *cg_name,
                                 const char *scratch_dir, const struct device_spec *devices,
                                 int device_count, char **argv, char **envp)
{
	static char lowerdir[256];
	static char upperdir[256];
	static char workdir[256];
	static char merged[256];
	int i;

	snprintf(lowerdir, sizeof(lowerdir), "%s", IMAGE_ROOT);
	snprintf(upperdir, sizeof(upperdir), "%s/upper", scratch_dir);
	snprintf(workdir, sizeof(workdir), "%s/work", scratch_dir);
	snprintf(merged, sizeof(merged), "%s/merged", scratch_dir);

	if (mkdir_p1(scratch_dir) != 0 || mkdir_p1(upperdir) != 0 || mkdir_p1(workdir) != 0 ||
	    mkdir_p1(merged) != 0)
		return -1;

	memset(spec, 0, sizeof(*spec));
	spec->ns.clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET |
	                        CLONE_NEWCGROUP | CLONE_INTO_CGROUP;
	spec->ns.hostname = cg_name;
	spec->cg.name = cg_name;
	spec->cg.memory_max = 67108864;
	spec->cg.pids_max = 32;
	spec->ov.lowerdir = lowerdir;
	spec->ov.upperdir = upperdir;
	spec->ov.workdir = workdir;
	spec->ov.merged = merged;
	spec->mnt.put_old_rel = ".old_root";

	spec->device_count = device_count;
	for (i = 0; i < device_count; i++)
		spec->devices[i] = devices[i];

	spec->argv = argv;
	spec->envp = envp;
	return 0;
}

static int run_and_check(struct container_spec *spec, const char *label, int *ok)
{
	struct container_handle h;
	int exit_status = -1;

	if (container_create(spec, &h) != 0) {
		perror(label);
		*ok = 0;
		return -1;
	}
	if (container_wait(&h, &exit_status, NULL) != 0) {
		perror(label);
		*ok = 0;
	} else if (exit_status != 0) {
		fprintf(stderr, "FAIL: %s exit status %d, expected 0\n", label, exit_status);
		*ok = 0;
	}
	close(h.pidfd);
	if (h.bpf_prog_fd >= 0)
		close(h.bpf_prog_fd);
	close(h.cgroup_fd);
	return 0;
}

int main(void)
{
	int ok = 1;
	char *envp[] = { NULL };

	if (bpf_encoding_sanity_check() != 0) {
		fprintf(stderr, "FAIL: bpf instruction encoding sanity check\n");
		return 1;
	}
	printf("bpf encoding sanity check: PASS\n");

	if (test_image_fixture_build(IMAGE_ROOT, "build/dev_child", "dev_child") != 0)
		return 1;

	/* A device baked into the shared base image itself (like
	 * test_dns.c's own ensure_dev_node() precedent), visible to every
	 * container via the shared OverlayFS lowerdir -- but never in any
	 * container's own device grant list, so only a container with NO
	 * BPF program attached at all (device_count == 0) should be able
	 * to open it. */
	if (mkdir_p1(IMAGE_ROOT "/dev") != 0)
		return 1;
	unlink(SHARED_DEV_PATH);
	if (mknod(SHARED_DEV_PATH, S_IFCHR | 0666, makedev(SHARED_MAJOR, SHARED_MINOR)) != 0) {
		perror("mknod shared test device");
		return 1;
	}

	/* 2. Container A: one real grant (/dev/zero's major:minor), own
	 * node opens, rdev verified. */
	{
		struct container_spec specA;
		struct device_spec devA[1];
		char *argv[] = { "/bin/dev_child", "/dev/kxtestA", "1", "1:5", NULL };

		memset(devA, 0, sizeof(devA));
		devA[0].type = DEVICE_NODE_CHAR;
		devA[0].major = 1;
		devA[0].minor = 5;
		snprintf(devA[0].dev_path, sizeof(devA[0].dev_path), "/dev/kxtestA");

		if (build_container_spec(&specA, "tcc-dev-a", "/tmp/device_test/a", devA, 1, argv,
		                          envp) != 0)
			return 1;
		run_and_check(&specA, "container a (granted device)", &ok);
	}

	/* 3. Container B: a different real grant (/dev/full's own
	 * major:minor), PLUS an attempt at the shared-but-ungranted device
	 * -- must be denied with EPERM. The concrete "two containers, one
	 * denied" proof. */
	{
		struct container_spec specB;
		struct device_spec devB[1];
		char *argv[] = { "/bin/dev_child", "/dev/kxtestB", "1", "1:7", "/dev/kxtest_shared",
			          "0", "-", NULL };

		memset(devB, 0, sizeof(devB));
		devB[0].type = DEVICE_NODE_CHAR;
		devB[0].major = 1;
		devB[0].minor = 7;
		snprintf(devB[0].dev_path, sizeof(devB[0].dev_path), "/dev/kxtestB");

		if (build_container_spec(&specB, "tcc-dev-b", "/tmp/device_test/b", devB, 1, argv,
		                          envp) != 0)
			return 1;
		run_and_check(&specB, "container b (denied ungranted device)", &ok);
	}

	/* 4. Container C: no devices granted at all -- no BPF program is
	 * attached (container_dev_bpf_attach()'s device_count == 0 no-op),
	 * so the shared baked-in device opens exactly as it always has,
	 * proving this feature adds zero restriction to a container that
	 * doesn't opt in. */
	{
		struct container_spec specC;
		char *argv[] = { "/bin/dev_child", "/dev/kxtest_shared", "1", "1:3", NULL };

		if (build_container_spec(&specC, "tcc-dev-c", "/tmp/device_test/c", NULL, 0, argv,
		                          envp) != 0)
			return 1;
		run_and_check(&specC, "container c (no devices, No Regressions)", &ok);
	}

	printf(ok ? "DEVICE RESULT: PASS\n" : "DEVICE RESULT: FAIL\n");
	return ok ? 0 : 1;
}
