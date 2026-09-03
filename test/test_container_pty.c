/*
 * Proves the devpts fix (task #865): a real "PTY allocation request
 * failed" was found live, interactively SSHing into jumpbox1 --
 * traced to mountns_pivot() (src/mountns.c) never mounting devpts
 * inside a container's own mount namespace, and pkg_seed_image_
 * baseline() (daemon/src/pkg.c) never seeding a /dev/ptmx node the
 * way it already does for null/zero/full/random/urandom. Same
 * minimal-lowerdir pattern test_overlay.c already established: a real
 * container is created and torn down, pty_child (this test's own exec
 * target) allocates a pty inside it exactly the way an interactive
 * SSH session does (posix_openpt/grantpt/unlockpt/ptsname_r/open the
 * slave/round-trip a byte), and the host reads its results back out
 * of the container's own upperdir.
 */
#include "container.h"
#include "linux_compat.h"
#include "test_image_fixture.h"

#include <errno.h>
#include <ftw.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define LOWERDIR "/tmp/container_pty_test/lower"
/*
 * "/lib/...", never "/usr/lib/..." (#224).
 *
 * The /usr form only resolves on a merged-usr system, where /lib is a
 * symlink into it. This project's own roots are deliberately not
 * merged-usr, and the build container the suite runs in is one of them,
 * so the /usr path is simply absent there and every test using it died
 * on "ld-linux-x86-64.so.2: No such file or directory". The /lib form
 * resolves in both. Exactly the root cause ADR-0085 records for
 * test_image_fixture.c, which had the same two reads wrong.
 */
#define HOST_LD_SO "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"
#define HOST_LIBC "/lib/x86_64-linux-gnu/libc.so.6"

static int mkdir_p1(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	return 0;
}

static int rm_tree_visitor(const char *path, const struct stat *sb, int typeflag,
                            struct FTW *ftwbuf)
{
	(void)sb;
	(void)ftwbuf;
	if (typeflag == FTW_DP)
		return rmdir(path);
	return unlink(path);
}

static void rm_tree(const char *path)
{
	nftw(path, rm_tree_visitor, 16, FTW_DEPTH | FTW_PHYS);
}

/*
 * Same five static device nodes plus the /dev/ptmx symlink
 * pkg_seed_image_baseline() (daemon/src/pkg.c) seeds into every real
 * image -- reproduced by hand here since this test, like
 * test_overlay.c, builds a minimal lowerdir directly rather than
 * going through the daemon's own image-baseline machinery.
 */
static int build_lowerdir(void)
{
	char path[256];
	size_t i;
	static const struct {
		const char *name;
		unsigned major, minor;
	} nodes[] = {
		{ "null", 1, 3 }, { "zero", 1, 5 }, { "full", 1, 7 },
		{ "random", 1, 8 }, { "urandom", 1, 9 },
	};

	rm_tree("/tmp/container_pty_test");
	if (mkdir_p1("/tmp/container_pty_test") != 0 || mkdir_p1(LOWERDIR) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/bin", LOWERDIR);
	if (mkdir_p1(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/bin/pty_child", LOWERDIR);
	if (test_image_fixture_copy_file("build/pty_child", path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/lib64", LOWERDIR);
	if (mkdir_p1(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib64/ld-linux-x86-64.so.2", LOWERDIR);
	if (test_image_fixture_copy_file(HOST_LD_SO, path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/lib", LOWERDIR);
	if (mkdir_p1(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu", LOWERDIR);
	if (mkdir_p1(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu/libc.so.6", LOWERDIR);
	if (test_image_fixture_copy_file(HOST_LIBC, path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/dev", LOWERDIR);
	if (mkdir_p1(path) != 0)
		return -1;
	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		snprintf(path, sizeof(path), "%s/dev/%s", LOWERDIR, nodes[i].name);
		if (mknod(path, S_IFCHR | 0666, makedev(nodes[i].major, nodes[i].minor)) != 0 &&
		    errno != EEXIST)
			return -1;
		chmod(path, 0666);
	}
	snprintf(path, sizeof(path), "%s/dev/ptmx", LOWERDIR);
	if (symlink("pts/ptmx", path) != 0 && errno != EEXIST)
		return -1;

	return 0;
}

static int read_status(const char *upperdir, int *out_ok)
{
	char path[256];
	char line[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/status.txt", upperdir);
	f = fopen(path, "r");
	if (f == NULL) {
		perror(path);
		return -1;
	}

	*out_ok = 1;
	while (fgets(line, sizeof(line), f) != NULL) {
		line[strcspn(line, "\n")] = '\0';
		printf("  %s\n", line);
		if (strcmp(line, "DEV_PTS_PTMX_EXISTS=yes") != 0 && strcmp(line, "POSIX_OPENPT=ok") != 0 &&
		    strcmp(line, "OPEN_SLAVE=ok") != 0 && strcmp(line, "ROUNDTRIP=ok") != 0 &&
		    strncmp(line, "SLAVE_PATH=", 11) != 0)
			*out_ok = 0;
	}
	fclose(f);
	return 0;
}

int main(void)
{
	char *child_argv[] = { "/bin/pty_child", NULL };
	char *child_envp[] = { NULL };
	struct container_spec spec;
	struct container_handle handle;
	int exit_status, ok;

	if (build_lowerdir() != 0)
		return 1;

	if (mkdir_p1("/tmp/container_pty_test/c1") != 0 ||
	    mkdir_p1("/tmp/container_pty_test/c1/upper") != 0 ||
	    mkdir_p1("/tmp/container_pty_test/c1/work") != 0 ||
	    mkdir_p1("/tmp/container_pty_test/c1/merged") != 0)
		return 1;

	memset(&spec, 0, sizeof(spec));
	spec.ns.clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET |
	                       CLONE_NEWCGROUP | CLONE_INTO_CGROUP;
	spec.ns.hostname = "ptytest";
	spec.cg.name = "ptytest";
	spec.cg.memory_max = 67108864;
	spec.cg.pids_max = 32;
	spec.cg.cpu_max = NULL;
	spec.ov.lowerdir = LOWERDIR;
	spec.ov.upperdir = "/tmp/container_pty_test/c1/upper";
	spec.ov.workdir = "/tmp/container_pty_test/c1/work";
	spec.ov.merged = "/tmp/container_pty_test/c1/merged";
	spec.mnt.put_old_rel = ".old_root";
	spec.argv = child_argv;
	spec.envp = child_envp;

	if (container_create(&spec, &handle) != 0) {
		perror("container_create");
		return 1;
	}
	if (container_wait(&handle, &exit_status, NULL) != 0) {
		perror("container_wait");
		return 1;
	}
	close(handle.pidfd);
	close(handle.cgroup_fd);

	printf("pty_child exit status=%d\n", exit_status);
	if (exit_status != 42) {
		fprintf(stderr, "FAIL: expected exit status 42, got %d\n", exit_status);
		return 1;
	}

	if (read_status("/tmp/container_pty_test/c1/upper", &ok) != 0)
		return 1;

	printf(ok ? "CONTAINER PTY RESULT: PASS\n" : "CONTAINER PTY RESULT: FAIL\n");
	return ok ? 0 : 1;
}
