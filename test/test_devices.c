/*
 * Real-syscall verification for PCI/USB (character/block) device
 * passthrough: proves the actual security property end to end through
 * container_create(), not just that the eBPF program loads.
 *
 * 1. A standalone bpf(2) sanity check (no container involved) isolates
 *    a struct cix_bpf_insn byte-packing bug from a device-logic bug,
 *    per this project's own "stress-test anything touching a raw
 *    syscall ABI struct" discipline (see ADR-0008/0009).
 * 2. Container A: one real device grant -- its own node opens, and
 *    fstat() confirms the major:minor the kernel actually reports
 *    matches what was granted.
 * 3. Container B: a different real device grant, plus BOTH baked-in
 *    devices. The baseline one (1:3) must OPEN although B never
 *    granted it -- ADR-0331/#578, and this assertion used to be the
 *    opposite, which was the bug written down as expected behaviour.
 *    The non-baseline one (5:1) must be denied with EPERM, which keeps
 *    the concrete "two containers, one denied" proof.
 * 4. Container C: no devices granted at all -- no program is attached,
 *    so BOTH baked-in devices open, byte-for-byte identical to every
 *    container's behavior before this feature existed (No
 *    Regressions). Its 5:1 arm is also the control that makes B's
 *    denial mean our policy refused it rather than something else.
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

#define IMAGE_ROOT "/run/device_test/lower"
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
#define SHARED_MINOR 3 /* /dev/null -- IN the image baseline since ADR-0331 */

/*
 * A second shared-but-ungranted node, at numbers the image baseline
 * does NOT contain (ADR-0331, #578).
 *
 * It exists because 1:3 stopped being able to prove a denial. Before
 * #578 this test proved "ungranted is denied" with the 1:3 node above,
 * and that assertion was the bug written down as expected behaviour:
 * /dev/null is in every image's baseline, so denying it to a container
 * that declared a GPU is exactly what ADR-0331 fixed. A denial proof
 * now needs a real device outside the baseline, and 5:1 is the
 * console's own pair.
 *
 * Its openability is not assumed. Container C declares no devices and
 * so gets no program at all, and it asserts this node OPENS -- so if
 * 5:1 is unopenable wherever this runs, C fails and names it, instead
 * of B's denial silently passing because an ancestor cgroup denied it
 * rather than our own program. The pair is the proof, the same
 * discipline a wrong-password control gives an LDAP bind.
 */
#define UNGRANTED_DEV_PATH IMAGE_ROOT "/dev/kxtest_ungranted"
#define UNGRANTED_MAJOR 5
#define UNGRANTED_MINOR 1 /* /dev/console */
static int mkdir_p1(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	return 0;
}

/* Isolates a struct cix_bpf_insn byte-packing bug from a device-logic
 * bug: a minimal "mov r0,1; exit" program, no context access, no
 * jumps -- if this alone fails to load, the bug is in the raw
 * instruction encoding, not in container_dev_bpf_attach()'s own
 * per-device comparison chain. */
static int bpf_encoding_sanity_check(void)
{
	struct cix_bpf_insn prog[2];
	struct cix_bpf_prog_load_attr attr;
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
	attr.prog_type = CIX_BPF_PROG_TYPE_CGROUP_DEVICE;
	attr.insn_cnt = 2;
	attr.insns = (uint64_t)(uintptr_t)prog;
	attr.license = (uint64_t)(uintptr_t)"GPL";
	attr.expected_attach_type = CIX_BPF_CGROUP_DEVICE;

	fd = (int)sys_bpf(CIX_BPF_PROG_LOAD, &attr, sizeof(attr));
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

	/* Two devices baked into the shared base image itself (like
	 * test_dns.c's own ensure_dev_node() precedent), visible to every
	 * container via the shared OverlayFS lowerdir and named in no
	 * container's own grant list. They are not equivalent any more,
	 * and that is the point of ADR-0331:
	 *
	 *   SHARED (1:3, /dev/null's pair) is in the IMAGE BASELINE, so a
	 *   container with a program attached opens it anyway. That used to
	 *   be the denial proof, and that assertion was #578 written down
	 *   as expected behaviour.
	 *
	 *   UNGRANTED (5:1, the console's pair) is in neither the baseline
	 *   nor any grant list, so it is what a denial is proved with now.
	 */
	if (mkdir_p1(IMAGE_ROOT "/dev") != 0)
		return 1;
	unlink(SHARED_DEV_PATH);
	if (mknod(SHARED_DEV_PATH, S_IFCHR | 0666, makedev(SHARED_MAJOR, SHARED_MINOR)) != 0) {
		perror("mknod shared test device");
		return 1;
	}
	unlink(UNGRANTED_DEV_PATH);
	if (mknod(UNGRANTED_DEV_PATH, S_IFCHR | 0666,
	           makedev(UNGRANTED_MAJOR, UNGRANTED_MINOR)) != 0) {
		perror("mknod ungranted test device");
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

		if (build_container_spec(&specA, "tcc-dev-a", "/run/device_test/a", devA, 1, argv,
		                          envp) != 0)
			return 1;
		run_and_check(&specA, "container a (granted device)", &ok);
	}

	/* 3. Container B: a different real grant (/dev/full's own
	 * major:minor), PLUS the two baked-in devices -- and this is the
	 * #578 gate.
	 *
	 * The baseline node (1:3) must OPEN even though B never declared
	 * it: that is ADR-0331, and before it this assertion was the
	 * opposite. The non-baseline node (5:1) must still be DENIED with
	 * EPERM, which keeps the concrete "two containers, one denied"
	 * proof this case has always carried.
	 *
	 * Reintroduce-to-prove-it: revert container_dev_bpf_attach()'s
	 * baseline blocks and the 1:3 triple below fails, because a
	 * container that declares one device gets a policy denying
	 * /dev/null. */
	{
		struct container_spec specB;
		struct device_spec devB[1];
		char *argv[] = { "/bin/dev_child", "/dev/kxtestB", "1", "1:7",
			          "/dev/kxtest_shared", "1", "1:3",
			          "/dev/kxtest_ungranted", "0", "-", NULL };

		memset(devB, 0, sizeof(devB));
		devB[0].type = DEVICE_NODE_CHAR;
		devB[0].major = 1;
		devB[0].minor = 7;
		snprintf(devB[0].dev_path, sizeof(devB[0].dev_path), "/dev/kxtestB");

		if (build_container_spec(&specB, "tcc-dev-b", "/run/device_test/b", devB, 1, argv,
		                          envp) != 0)
			return 1;
		run_and_check(&specB, "container b (denied ungranted device)", &ok);
	}

	/* 4. Container C: no devices granted at all -- no BPF program is
	 * attached (container_dev_bpf_attach()'s device_count == 0 no-op),
	 * so BOTH baked-in devices open exactly as they always have,
	 * proving this feature adds zero restriction to a container that
	 * doesn't opt in. ADR-0331 did not change this arm and must not:
	 * every container on 192.168.15.95 declares an empty list.
	 *
	 * The 5:1 triple is also the CONTROL for container B's denial. B
	 * asserts 5:1 is refused; that only means our program refused it if
	 * the node is openable here at all. If 5:1 turns out unopenable in
	 * some environment -- an ancestor cgroup, a kernel without the
	 * driver -- this arm fails and names it, instead of B's denial
	 * passing for a reason that has nothing to do with the policy. The
	 * pair is the proof, the same discipline a wrong-password control
	 * gives an LDAP bind. */
	{
		struct container_spec specC;
		char *argv[] = { "/bin/dev_child", "/dev/kxtest_shared", "1", "1:3",
			          "/dev/kxtest_ungranted", "1", "5:1", NULL };

		if (build_container_spec(&specC, "tcc-dev-c", "/run/device_test/c", NULL, 0, argv,
		                          envp) != 0)
			return 1;
		run_and_check(&specC, "container c (no devices, No Regressions)", &ok);
	}

	printf(ok ? "DEVICE RESULT: PASS\n" : "DEVICE RESULT: FAIL\n");
	return ok ? 0 : 1;
}
