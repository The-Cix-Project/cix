/*
 * Phase 2 demonstrable test: proves the mission's OverlayFS
 * requirement directly -- "the host OS acts as the shared lowerdir
 * for all containers; running instances only own their upperdir
 * diffs." Builds one minimal lowerdir, runs two containers against
 * it with independent upperdirs, and verifies from the host side that
 * writes land only in the writing container's own upperdir, lowerdir
 * is never mutated, and a second container never sees a first
 * container's diff.
 */
#include "container.h"
#include "linux_compat.h"
#include "overlay_test_common.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOWERDIR "/tmp/overlay_test/lower"

static int mkdir_p1(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	return 0;
}

static int write_file(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");
	if (f == NULL) {
		perror(path);
		return -1;
	}
	if (fputs(content, f) < 0) {
		perror(path);
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static int copy_file(const char *src_path, const char *dst_path)
{
	int src, dst;
	char buf[4096];
	ssize_t n;

	src = open(src_path, O_RDONLY);
	if (src < 0) {
		perror(src_path);
		return -1;
	}

	dst = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (dst < 0) {
		perror(dst_path);
		close(src);
		return -1;
	}

	while ((n = read(src, buf, sizeof(buf))) > 0) {
		if (write(dst, buf, (size_t)n) != n) {
			perror("write");
			close(src);
			close(dst);
			return -1;
		}
	}
	if (n < 0) {
		perror("read");
		close(src);
		close(dst);
		return -1;
	}

	close(src);
	close(dst);
	return 0;
}

/*
 * overlay_child is dynamically linked against system glibc (per the
 * project's TCC-wide decision -- never -static), so the lowerdir must
 * carry the dynamic linker and libc it needs at runtime. Paths are
 * this dev machine's actual glibc install, resolved past its /usr
 * merge symlinks -- reproducing a portable, general-purpose install
 * layout is package-manager scope (a later phase), not this test's.
 */
#define HOST_LD_SO "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"
#define HOST_LIBC "/usr/lib/x86_64-linux-gnu/libc.so.6"

static int rm_tree_visitor(const char *path, const struct stat *sb, int typeflag,
                            struct FTW *ftwbuf)
{
	(void)sb;
	(void)ftwbuf;
	if (typeflag == FTW_DP)
		return rmdir(path);
	return unlink(path);
}

/*
 * Fixed /tmp paths are reused across runs (the point of this test is
 * host-visible upperdir contents, so it can't use a fresh mkdtemp()
 * each time and still make its "pre-existing status.txt" assertion
 * meaningful) -- so leftovers from a prior run must be cleared first,
 * or a second invocation always fails.
 */
static void rm_tree(const char *path)
{
	nftw(path, rm_tree_visitor, 16, FTW_DEPTH | FTW_PHYS);
}

static int build_lowerdir(void)
{
	char path[256];

	rm_tree("/tmp/overlay_test");
	if (mkdir_p1("/tmp/overlay_test") != 0)
		return -1;
	if (mkdir_p1(LOWERDIR) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/bin", LOWERDIR);
	if (mkdir_p1(path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/bin/overlay_child", LOWERDIR);
	if (copy_file("build/overlay_child", path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/lib64", LOWERDIR);
	if (mkdir_p1(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib64/ld-linux-x86-64.so.2", LOWERDIR);
	if (copy_file(HOST_LD_SO, path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/lib", LOWERDIR);
	if (mkdir_p1(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu", LOWERDIR);
	if (mkdir_p1(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu/libc.so.6", LOWERDIR);
	if (copy_file(HOST_LIBC, path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/lower_marker.txt", LOWERDIR);
	if (write_file(path, OVERLAY_LOWER_MARKER_CONTENT) != 0)
		return -1;

	return 0;
}

static int read_status(const char *upperdir, int *out_prior_existed, int *out_lower_ok)
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

	*out_prior_existed = -1;
	*out_lower_ok = -1;

	while (fgets(line, sizeof(line), f) != NULL) {
		line[strcspn(line, "\n")] = '\0';
		if (strcmp(line, "PRIOR_EXISTED=yes") == 0)
			*out_prior_existed = 1;
		else if (strcmp(line, "PRIOR_EXISTED=no") == 0)
			*out_prior_existed = 0;
		else if (strcmp(line, "LOWER_OK=yes") == 0)
			*out_lower_ok = 1;
		else if (strcmp(line, "LOWER_OK=no") == 0)
			*out_lower_ok = 0;
	}
	fclose(f);
	return 0;
}

static int run_container(const char *name, const char *upperdir, const char *workdir,
                          const char *merged)
{
	char *child_argv[] = { "/bin/overlay_child", NULL };
	char *child_envp[] = { NULL };
	struct container_spec spec;
	struct container_handle handle;
	int exit_status;

	memset(&spec, 0, sizeof(spec));
	spec.ns.clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS |
	                       CLONE_NEWNET | CLONE_NEWCGROUP | CLONE_INTO_CGROUP;
	spec.ns.hostname = name;
	spec.cg.name = name;
	spec.cg.memory_max = 67108864;
	spec.cg.pids_max = 32;
	spec.cg.cpu_max = NULL;
	spec.ov.lowerdir = LOWERDIR;
	spec.ov.upperdir = upperdir;
	spec.ov.workdir = workdir;
	spec.ov.merged = merged;
	spec.mnt.put_old_rel = ".old_root";
	spec.argv = child_argv;
	spec.envp = child_envp;

	if (container_create(&spec, &handle) != 0) {
		perror("container_create");
		return -1;
	}

	if (container_wait(&handle, &exit_status) != 0) {
		perror("container_wait");
		return -1;
	}
	close(handle.pidfd);
	close(handle.cgroup_fd);

	printf("%s: exit status=%d\n", name, exit_status);
	if (exit_status != 42) {
		fprintf(stderr, "FAIL: %s expected exit status 42, got %d\n", name, exit_status);
		return -1;
	}
	return 0;
}

int main(void)
{
	int ok = 1;
	int prior_existed, lower_ok;
	char lower_status[256];
	struct stat st;

	if (build_lowerdir() != 0)
		return 1;

	if (mkdir_p1("/tmp/overlay_test/c1") != 0 ||
	    mkdir_p1("/tmp/overlay_test/c1/upper") != 0 ||
	    mkdir_p1("/tmp/overlay_test/c1/work") != 0 ||
	    mkdir_p1("/tmp/overlay_test/c1/merged") != 0)
		return 1;

	if (run_container("c1", "/tmp/overlay_test/c1/upper", "/tmp/overlay_test/c1/work",
	                   "/tmp/overlay_test/c1/merged") != 0)
		return 1;

	if (read_status("/tmp/overlay_test/c1/upper", &prior_existed, &lower_ok) != 0)
		return 1;
	if (prior_existed != 0) {
		fprintf(stderr, "FAIL: c1 saw a pre-existing /status.txt\n");
		ok = 0;
	}
	if (lower_ok != 1) {
		fprintf(stderr, "FAIL: c1 did not see the expected lowerdir marker content\n");
		ok = 0;
	}

	snprintf(lower_status, sizeof(lower_status), "%s/status.txt", LOWERDIR);
	if (stat(lower_status, &st) == 0) {
		fprintf(stderr, "FAIL: lowerdir was mutated -- %s exists\n", lower_status);
		ok = 0;
	} else if (errno != ENOENT) {
		perror(lower_status);
		ok = 0;
	}

	if (mkdir_p1("/tmp/overlay_test/c2") != 0 ||
	    mkdir_p1("/tmp/overlay_test/c2/upper") != 0 ||
	    mkdir_p1("/tmp/overlay_test/c2/work") != 0 ||
	    mkdir_p1("/tmp/overlay_test/c2/merged") != 0)
		return 1;

	if (run_container("c2", "/tmp/overlay_test/c2/upper", "/tmp/overlay_test/c2/work",
	                   "/tmp/overlay_test/c2/merged") != 0)
		return 1;

	if (read_status("/tmp/overlay_test/c2/upper", &prior_existed, &lower_ok) != 0)
		return 1;
	if (prior_existed != 0) {
		fprintf(stderr, "FAIL: c2 saw c1's /status.txt -- upperdir isolation broken\n");
		ok = 0;
	}
	if (lower_ok != 1) {
		fprintf(stderr, "FAIL: c2 did not see the expected lowerdir marker content\n");
		ok = 0;
	}

	printf(ok ? "OVERLAY RESULT: PASS\n" : "OVERLAY RESULT: FAIL\n");
	return ok ? 0 : 1;
}
