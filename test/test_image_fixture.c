#include "test_image_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TOOLCHAIN_CP_BIN "/usr/bin/cp"

static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	size_t len;
	char *p;

	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	len = strlen(tmp);
	if (len > 0 && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';

	for (p = tmp + 1; *p != '\0'; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
				perror(tmp);
				return -1;
			}
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
		perror(tmp);
		return -1;
	}
	return 0;
}

int test_image_fixture_copy_file(const char *src_path, const char *dst_path)
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
	if (n < 0)
		perror(src_path);
	close(src);
	close(dst);
	return n < 0 ? -1 : 0;
}

int test_image_fixture_build(const char *image_root, const char *child_binary_path,
                              const char *child_basename)
{
	char path[PATH_MAX];

	if (mkdir_p(image_root) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/bin", image_root);
	if (mkdir_p(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/bin/%s", image_root, child_basename);
	if (test_image_fixture_copy_file(child_binary_path, path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/lib64", image_root);
	if (mkdir_p(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib64/ld-linux-x86-64.so.2", image_root);
	if (test_image_fixture_copy_file("/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu", image_root);
	if (mkdir_p(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu/libc.so.6", image_root);
	if (test_image_fixture_copy_file("/usr/lib/x86_64-linux-gnu/libc.so.6", path) != 0)
		return -1;

	return 0;
}

int test_image_fixture_add_lib(const char *image_root, const char *host_lib_abs_path)
{
	char dst_path[PATH_MAX];
	char dst_dir[PATH_MAX];
	char *slash;

	if (snprintf(dst_path, sizeof(dst_path), "%s%s", image_root, host_lib_abs_path) >=
	    (int)sizeof(dst_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	snprintf(dst_dir, sizeof(dst_dir), "%s", dst_path);
	slash = strrchr(dst_dir, '/');
	if (slash != NULL)
		*slash = '\0';

	if (mkdir_p(dst_dir) != 0)
		return -1;
	return test_image_fixture_copy_file(host_lib_abs_path, dst_path);
}

/* fork()+execve()+waitpid() for "cp -a" -- correct symlink/permission
 * handling a hand-rolled recursive copier would get wrong, the same
 * "real software for a genuinely hard problem" reasoning this
 * mechanism's own daemon-side sibling (pkg_bootstrap_build_image(),
 * before this extraction) already used. */
static int run_cp_a(const char *src, const char *dst)
{
	pid_t pid;
	int status;
	char *argv[] = { (char *)TOOLCHAIN_CP_BIN, "-a", (char *)src, (char *)dst, NULL };

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve(TOOLCHAIN_CP_BIN, argv, environ);
		perror("execve cp");
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid) {
		perror("waitpid");
		return -1;
	}
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

int test_image_fixture_stage_toolchain(const char *image_root)
{
	static const char *const subdirs[] = { "include", "lib", "lib64", "bin", "libexec" };
	static const struct {
		const char *link;
		const char *target;
	} compat[] = {
		{ "bin", "usr/bin" }, { "lib", "usr/lib" }, { "lib64", "usr/lib64" }, { "sbin", "usr/bin" },
	};
	/*
	 * Targeted extras beyond the wholesale /usr/{include,lib,lib64,bin,
	 * libexec} copy above, each found by an actual package build
	 * failing, not guessed at -- staging the *entirety* of e.g.
	 * /usr/share (hundreds of MB on a real dev host, almost none of it
	 * relevant to building software) just to reach one tool's own data
	 * files would bloat this artifact for no real benefit. Extend this
	 * list as a real build surfaces a real need for one, the same
	 * "verify empirically, don't speculate" discipline this project
	 * applies everywhere else:
	 *
	 * - /etc/alternatives: Debian's "alternatives" system points many
	 *   /usr/bin/* tools (awk, and others a real build can reach for)
	 *   at an *absolute* /etc/alternatives/<name> symlink -- cp -a on
	 *   /usr/bin above preserves that symlink exactly as-is, target
	 *   string included, so it still reads /etc/alternatives/<name>
	 *   once inside the container. Confirmed directly: bash's own
	 *   config.status failed outright ("awk: command not found")
	 *   because /etc was never staged at all.
	 * - /usr/share/bison: bison itself needs its own bundled M4 macro
	 *   library (m4sugar.m4 and friends) at *run* time, not just build
	 *   time, to generate a parser from any .y grammar -- confirmed
	 *   directly building iproute2's tc (its ematch grammar needs
	 *   bison): "bison: .../m4sugar.m4: cannot open: No such file or
	 *   directory".
	 * - /usr/local/go: this build host's own real Go toolchain
	 *   (GOROOT), staged so a recipe's pkg_build() can set
	 *   PATH=/usr/local/go/bin:$PATH and genuinely build Go packages
	 *   (e.g. gitea) -- not itself proven end-to-end by any recipe yet,
	 *   real, separate follow-up work; staging it is the low-cost,
	 *   tolerant-if-absent part of that story.
	 */
	static const struct {
		const char *src;
		const char *rel_dst;
	} extras[] = {
		{ "/etc/alternatives", "etc/alternatives" },
		{ "/usr/share/bison", "usr/share/bison" },
		{ "/usr/share/autoconf", "usr/share/autoconf" },
		{ "/usr/share/perl", "usr/share/perl" },
		{ "/usr/local/go", "usr/local/go" },
	};
	static const struct {
		const char *name;
		unsigned int major, minor;
	} dev_nodes[] = {
		{ "null", 1, 3 }, { "zero", 1, 5 }, { "full", 1, 7 }, { "random", 1, 8 },
		{ "urandom", 1, 9 },
	};
	char usr_dst[PATH_MAX];
	size_t i;

	if (mkdir_p(image_root) != 0)
		return -1;
	snprintf(usr_dst, sizeof(usr_dst), "%s/usr", image_root);
	if (mkdir_p(usr_dst) != 0)
		return -1;

	for (i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
		char src[PATH_MAX], dst[PATH_MAX];
		struct stat src_st, dst_st;

		snprintf(src, sizeof(src), "/usr/%s", subdirs[i]);
		snprintf(dst, sizeof(dst), "%s/usr/%s", image_root, subdirs[i]);

		if (stat(src, &src_st) != 0)
			continue; /* not present on this host -- skip, not fatal */
		if (stat(dst, &dst_st) == 0)
			continue; /* already staged -- idempotent */
		if (run_cp_a(src, dst) != 0)
			return -1;
	}

	for (i = 0; i < sizeof(compat) / sizeof(compat[0]); i++) {
		char linkpath[PATH_MAX];

		snprintf(linkpath, sizeof(linkpath), "%s/%s", image_root, compat[i].link);
		symlink(compat[i].target, linkpath); /* EEXIST tolerated -- idempotent */
	}

	for (i = 0; i < sizeof(extras) / sizeof(extras[0]); i++) {
		char dst[PATH_MAX], dst_parent[PATH_MAX], *slash;
		struct stat st;

		snprintf(dst, sizeof(dst), "%s/%s", image_root, extras[i].rel_dst);
		if (stat(extras[i].src, &st) != 0 || stat(dst, &st) == 0)
			continue; /* not present on this host, or already staged */

		snprintf(dst_parent, sizeof(dst_parent), "%s", dst);
		slash = strrchr(dst_parent, '/');
		if (slash != NULL)
			*slash = '\0';

		if (mkdir_p(dst_parent) != 0 || run_cp_a(extras[i].src, dst) != 0)
			return -1;
	}

	/*
	 * Same standard-FHS-baseline reasoning /dev/null-needing configure
	 * scripts already established: a real build's own ./configure
	 * routinely redirects to /dev/null while probing the compiler, and
	 * needs a writable, sticky-bit /tmp for its own temp files
	 * (config.guess, mktemp, plenty of Makefiles all assume this).
	 * /dev/tty is deliberately omitted -- nothing in a batch
	 * "./configure && make && make install" sequence needs a
	 * controlling terminal.
	 */
	{
		char dev_dir[PATH_MAX];

		snprintf(dev_dir, sizeof(dev_dir), "%s/dev", image_root);
		if (mkdir_p(dev_dir) != 0)
			return -1;
		for (i = 0; i < sizeof(dev_nodes) / sizeof(dev_nodes[0]); i++) {
			char path[PATH_MAX];

			snprintf(path, sizeof(path), "%s/%s", dev_dir, dev_nodes[i].name);
			if (mknod(path, S_IFCHR | 0666, makedev(dev_nodes[i].major, dev_nodes[i].minor)) != 0 &&
			    errno != EEXIST)
				return -1;
		}
	}
	{
		char tmp_dir[PATH_MAX];

		snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp", image_root);
		if (mkdir_p(tmp_dir) != 0)
			return -1;
		if (chmod(tmp_dir, 01777) != 0)
			return -1;
	}

	return 0;
}
