#include <time.h>
#include "libdirs.h"
#include "test_image_fixture.h"

#include <elf.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
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
	if (test_image_fixture_copy_file("/" CIX_LIB_DIR_RUNTIME "/ld-linux-x86-64.so.2", path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/" CIX_LIB_DIR_RUNTIME, image_root);
	if (mkdir_p(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/" CIX_LIB_DIR_RUNTIME "/libc.so.6", image_root);
	if (test_image_fixture_copy_file("/" CIX_LIB_DIR_RUNTIME "/libc.so.6", path) != 0)
		return -1;
	/*
	 * Second copy of ld-linux itself, same file as lib64/ above but at
	 * the path glibc >= 2.34's own libc.so.6 needs it reachable from --
	 * libc.so.6 carries a DT_NEEDED entry on ld-linux-x86-64.so.2
	 * itself, resolved via the ordinary runtime library search path
	 * (which does not include /lib64), not the one-time PT_INTERP
	 * lookup lib64/ld-linux-x86-64.so.2 above already satisfies.
	 *
	 * All three reads in this function (this one, libc.so.6 above, and
	 * lib64/ld-linux above) source from "/lib/x86_64-linux-gnu/", never
	 * "/usr/lib/x86_64-linux-gnu/" -- the real, final root cause of the
	 * "ld-linux-x86-64.so.2: No such file or directory" failure this
	 * function has produced on every self-hosted mkbootroot run all
	 * along, confirmed via strace against a genuinely non-merged-usr
	 * chroot: this function's own SOURCE reads used the "/usr/lib/..."
	 * form, which only resolves on a merged-usr dev sandbox (where
	 * /lib is itself a symlink to usr/lib) -- this project's own
	 * produced roots are deliberately NOT merged-usr (CLAUDE.md), so
	 * that path is simply absent when mkbootroot runs on one of its own
	 * prior builds (the self-hosted case, ADR-0057) -- the ONE case
	 * that matters, since a plain local dev-sandbox invocation never
	 * exercises this failure at all. "/lib/x86_64-linux-gnu/" resolves
	 * correctly in both environments: via the symlink here, and as the
	 * real, actual location there (matching exactly where this same
	 * function's own destination writes already place these files for
	 * the next generation). A prior attempt at this fix (superseded
	 * ADR-0083) got the destination right but left these three source
	 * reads on the wrong path -- see ADR-0085.
	 */
	snprintf(path, sizeof(path), "%s/" CIX_LIB_DIR_RUNTIME "/ld-linux-x86-64.so.2", image_root);
	if (test_image_fixture_copy_file("/" CIX_LIB_DIR_RUNTIME "/ld-linux-x86-64.so.2", path) != 0)
		return -1;

	return 0;
}

int test_image_fixture_copy_dir_files(const char *src_dir, const char *dst_dir)
{
	DIR *dir = opendir(src_dir);
	struct dirent *entry;
	char src_path[PATH_MAX], dst_path[PATH_MAX];
	struct stat st;

	if (dir == NULL) {
		perror(src_dir);
		return -1;
	}
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entry->d_name) >=
		            (int)sizeof(src_path) ||
		    snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, entry->d_name) >=
		            (int)sizeof(dst_path)) {
			fprintf(stderr, "path too long under %s\n", src_dir);
			closedir(dir);
			return -1;
		}
		if (stat(src_path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (test_image_fixture_copy_file(src_path, dst_path) != 0) {
			closedir(dir);
			return -1;
		}
	}
	closedir(dir);
	return 0;
}

int test_image_fixture_add_lib(const char *image_root, const char *host_lib_abs_path)
{
	char dst_path[PATH_MAX];
	char dst_dir[PATH_MAX];
	char resolved[PATH_MAX];
	char *slash;

	/*
	 * Find the library by NAME if it is not at the path asked for
	 * (#224).
	 *
	 * Callers name an absolute path because that is where the library
	 * sits on a Debian-derived build host: /lib/x86_64-linux-gnu/.
	 * That is an inherited layout rather than a decision (#184), and it
	 * is not the layout of a Cix build environment, where a package
	 * like ncurses installs to usr/lib/. So staging libtinfo.so.6 by
	 * its multiarch path failed with "No such file or directory" on
	 * exactly the host that was supposed to be able to do this -- and
	 * took six boot tests with it.
	 *
	 * The destination is unchanged: whatever the caller asked for is
	 * still where it lands inside the image. Only the SOURCE is
	 * searched, and only when the given path does not exist, so a host
	 * that does have the library where the caller said is completely
	 * unaffected.
	 */
	snprintf(resolved, sizeof(resolved), "%s", host_lib_abs_path);
	if (access(resolved, R_OK) != 0) {
		static const char *const dirs[] = CIX_LIB_DIRS_SEARCH;
		const char *base = strrchr(host_lib_abs_path, '/');
		size_t i;

		base = (base != NULL) ? base + 1 : host_lib_abs_path;
		for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
			snprintf(resolved, sizeof(resolved), "/%s/%s", dirs[i], base);
			if (access(resolved, R_OK) == 0)
				break;
			resolved[0] = '\0';
		}
	}

	if (resolved[0] == '\0')
		snprintf(resolved, sizeof(resolved), "%s", host_lib_abs_path); /* let the copy report it */

	/* Destination is always what the caller asked for -- the loader
	 * inside the image looks where the caller said, not where this
	 * host happens to keep its copy. */
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
	return test_image_fixture_copy_file(resolved, dst_path);
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
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;
	if (WIFEXITED(status))
		fprintf(stderr, "cp -a %s %s: exited with status %d\n", src, dst, WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		fprintf(stderr, "cp -a %s %s: killed by signal %d\n", src, dst, WTERMSIG(status));
	return -1;
}

int test_image_fixture_copy_dir_recursive(const char *src_dir, const char *dst_dir)
{
	return run_cp_a(src_dir, dst_dir);
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
	 * - /usr/share/gettext, /usr/share/aclocal, /usr/share/automake-1.16:
	 *   procps.recipe's own autogen.sh needs `autopoint` (a separate
	 *   Debian package from "gettext" itself, confirmed the hard way --
	 *   installing just "gettext" still left autogen.sh failing with the
	 *   same "you must have autopoint installed" error) to regenerate
	 *   its build system from a git-archive source snapshot (no
	 *   pre-generated ./configure ships in one, unlike a real `make
	 *   dist` release tarball). autopoint's own data files live under
	 *   /usr/share/gettext; the gettext.m4/iconv.m4/etc. macros
	 *   autoreconf expands live under /usr/share/aclocal; automake
	 *   itself (invoked by autogen.sh) is a thin wrapper script whose
	 *   real Perl implementation (Automake::Config and the rest of the
	 *   Automake:: package) lives under /usr/share/automake-1.16, not
	 *   next to the wrapper in /usr/bin -- found the same way as the
	 *   first two, a real build failing ("Can't locate
	 *   Automake/Config.pm in @INC") one step further into the same
	 *   autogen.sh run. None of the three are under the wholesale
	 *   include/lib/lib64/bin/libexec copy above.
	 * - /usr/local/go: this build host's own real Go toolchain
	 *   (GOROOT), staged so a recipe's pkg_build() can set
	 *   PATH=/usr/local/go/bin:$PATH and genuinely build Go packages --
	 *   proven end-to-end by gitea.recipe (a real, offline `make
	 *   backend` build, confirmed against a live daemon).
	 * - /usr/local/cargo, /usr/local/rustup: this build host's own real
	 *   Rust toolchain (CARGO_HOME/RUSTUP_HOME), staged the same way,
	 *   for a recipe's pkg_build() to set
	 *   PATH=/usr/local/cargo/bin:$PATH -- needed by lldap.recipe's own
	 *   Rust->WASM frontend build (ADR-0036). Includes wasm-pack
	 *   (`cargo install wasm-pack`) and the wasm32-unknown-unknown
	 *   target (`rustup target add`) pre-installed on this build host
	 *   before staging -- both need real network access to install
	 *   (crates.io / Rust's own distribution server) that the isolated
	 *   pkg_build() container doesn't have, the same reasoning
	 *   wasm-pack/the wasm32 target can't be installed at recipe-build
	 *   time at all, only pre-staged. Also includes
	 *   /usr/local/cargo/wasm-pack-cache -- wasm-pack's own
	 *   WASM_PACK_CACHE-pointed binary cache (wasm-bindgen-cli, matched
	 *   to this crate's Cargo.lock-pinned version, plus wasm-opt), found
	 *   the same way: a real isolated build failed outright
	 *   ("Could not resolve host: index.crates.io") the first time
	 *   wasm-pack tried to install wasm-bindgen-cli on demand. Prepopulated
	 *   once on this build host (`WASM_PACK_CACHE=/usr/local/cargo/wasm-pack-cache
	 *   wasm-pack build` against lldap's own app/), then rides along for
	 *   free with the existing wholesale /usr/local/cargo copy above --
	 *   no separate staging entry needed.
	 * - /usr/share/libtool, /usr/share/aclocal-1.16, /usr/share/misc:
	 *   three more procps.recipe autogen.sh gaps, found the same way, one
	 *   step further still. /usr/share/libtool is libtoolize's own
	 *   build-aux template directory ("$pkgauxdir is not a directory:
	 *   '/usr/share/libtool/build-aux'" -- libtoolize --force copies
	 *   ltmain.sh and the lt*.m4 macros from there, same "not part of the
	 *   wholesale bin/lib copy, it's a separate share/ data dir" shape as
	 *   automake-1.16 above). /usr/share/aclocal-1.16 is automake's own
	 *   version-specific additional-macro search directory ("aclocal:
	 *   error: couldn't open directory '/usr/share/aclocal-1.16'"),
	 *   distinct from the already-staged /usr/share/aclocal (which holds
	 *   gettext's/libtool's own aclocal contributions, not automake's).
	 *   /usr/share/misc holds the real config.guess/config.sub GNU
	 *   triplet scripts every autoconf project's `automake --add-missing`
	 *   step needs; automake's own copies under
	 *   /usr/share/automake-1.16/config.{guess,sub} (already staged
	 *   above) are themselves symlinks to ../misc/config.{guess,sub}
	 *   confirmed via `ls -la`, so staging automake-1.16 alone still left
	 *   ./configure failing with "cannot find required auxiliary files:
	 *   config.guess config.sub" until this directory was staged too.
	 */
	static const struct {
		const char *src;
		const char *rel_dst;
	} extras[] = {
		{ "/etc/alternatives", "etc/alternatives" },
		{ "/usr/share/bison", "usr/share/bison" },
		{ "/usr/share/autoconf", "usr/share/autoconf" },
		{ "/usr/share/perl", "usr/share/perl" },
		{ "/usr/share/gettext", "usr/share/gettext" },
		{ "/usr/share/aclocal", "usr/share/aclocal" },
		{ "/usr/share/automake-1.16", "usr/share/automake-1.16" },
		{ "/usr/share/libtool", "usr/share/libtool" },
		{ "/usr/share/aclocal-1.16", "usr/share/aclocal-1.16" },
		{ "/usr/share/misc", "usr/share/misc" },
		{ "/usr/local/go", "usr/local/go" },
		{ "/usr/local/cargo", "usr/local/cargo" },
		{ "/usr/local/rustup", "usr/local/rustup" },
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

int test_data_dir_create(char *out_path, size_t out_size)
{
	/*
	 * /run first, and it is not a preference (#224).
	 *
	 * A container's rootfs is an overlay mount, so when the suite runs
	 * inside a build container -- which is how it runs on a Cix host at
	 * all -- a data directory under /tmp sits ON that overlay. Every
	 * container a test daemon then creates asks the kernel to stack
	 * overlayfs on overlayfs, and the kernel refuses, in as many words:
	 *
	 *   overlay: filesystem on /tmp/cix_test_data_XXXXXX/containers/
	 *            c1/upper not supported as upperdir
	 *
	 * /run is a fresh tmpfs in every container (src/mountns.c mounts it
	 * precisely so it is not overlay-backed storage), and tmpfs is a
	 * supported upperdir. On a host with no such constraint /run is an
	 * ordinary tmpfs too, so this is not a container-only special case
	 * -- it is simply a better place for a scratch directory that has
	 * filesystems built on top of it.
	 *
	 * /tmp remains the fallback for any environment where /run is not
	 * writable, which is where every one of these tests ran before.
	 */
	char tmpl[] = "/run/cix_test_data_XXXXXX";
	char fallback[] = "/tmp/cix_test_data_XXXXXX";

	if (mkdtemp(tmpl) == NULL) {
		if (mkdtemp(fallback) == NULL) {
			perror("mkdtemp");
			return -1;
		}
		snprintf(tmpl, sizeof(tmpl), "%s", fallback);
	}
	if (snprintf(out_path, out_size, "%s", tmpl) >= (int)out_size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	/*
	 * ADR-0207 phase 3: the platform default is userns ON, but this dev
	 * sandbox's own LSM forbids the uid_map write outright (documented
	 * in CLAUDE.md -- even a "0 0 1" self-map is EPERM here), so every
	 * container a test daemon created would die in its handshake.
	 * Seeded through the ordinary daemon-config channel -- the same
	 * knob an operator has -- not a test-only code path. A test that
	 * wants the secure default exercises it on the .95 VM, the one
	 * environment whose kernel actually permits it.
	 */
	{
		char cfg_dir[PATH_MAX];
		char cfg[PATH_MAX];
		FILE *f;

		snprintf(cfg_dir, sizeof(cfg_dir), "%s/state", tmpl);
		if (mkdir(cfg_dir, 0755) != 0 && errno != EEXIST)
			return -1;
		snprintf(cfg, sizeof(cfg), "%s/daemon_config.json", cfg_dir);
		f = fopen(cfg, "w");
		if (f == NULL)
			return -1;
		fputs("{\"userns_default\": false}\n", f);
		fclose(f);
	}
	return 0;
}

void test_data_dir_cleanup(const char *path)
{
	char cmd[PATH_MAX + 16];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
	if (system(cmd) != 0)
		fprintf(stderr, "warning: cleanup of %s failed\n", path);
}

int test_image_fixture_write_manifest(const char *image_dir, const char *version)
{
	char manifest_path[PATH_MAX];
	char content[512];
	int n;
	FILE *f;

	if (mkdir_p(image_dir) != 0) {
		perror("mkdir_p (image_dir)");
		return -1;
	}
	snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", image_dir);
	n = snprintf(content, sizeof(content),
	             "{\"packages\":[],\"current_version\":\"%s\","
	             "\"versions\":[{\"version\":\"%s\",\"created_at\":0}]}",
	             version, version);
	f = fopen(manifest_path, "w");
	if (f == NULL) {
		perror("fopen (manifest.json)");
		return -1;
	}
	if (fwrite(content, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

int test_image_fixture_read_current_version(const char *image_dir, char *out_version,
                                             size_t out_size)
{
	char manifest_path[PATH_MAX];
	char buf[4096];
	FILE *f;
	size_t n;
	const char *key = "\"current_version\":\"";
	const char *p, *end;
	size_t len;

	snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", image_dir);
	f = fopen(manifest_path, "r");
	if (f == NULL)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	p = strstr(buf, key);
	if (p == NULL)
		return -1;
	p += strlen(key);
	end = strchr(p, '"');
	if (end == NULL)
		return -1;
	len = (size_t)(end - p);
	if (len >= out_size)
		return -1;
	memcpy(out_version, p, len);
	out_version[len] = '\0';
	return 0;
}

/*
 * The floor: the packages a build environment cannot be composed
 * without. Every build container is spawned as `/usr/bin/bash -c`
 * (daemon/src/pkg.c), so bash and its runtime are structural, not a
 * preference; the rest are what an autotools-style recipe reaches for
 * and what this project's own declared recipes already name.
 *
 * Versions are pinned to exactly the artifacts this repo carries under
 * build-inputs/floor-artifacts, and each is verified against the checksum in
 * its own recipe before it is used. Bumping one means fetching the new
 * artifact and re-verifying -- not editing this table alone.
 */
static const struct {
	const char *name;
	const char *version;
} floor_packages[] = {
	/*
	 * Exactly what a fixture build uses and nothing more, which is the
	 * same discipline the recipes themselves are now held to. A fixture
	 * pkg_build() runs `tcc -o hello hello.c` and its pkg_install()
	 * runs mkdir/cp, so: bash to run recipe.sh at all, coreutils for
	 * those two, tcc to compile, glibc for the headers and CRT it needs,
	 * and linux-headers for the kernel UAPI headers glibc's own
	 * limits.h chain includes.
	 *
	 * binutils used to be here and is deliberately gone: tcc has its
	 * own linker, nothing in a fixture reaches for ar or ld, and at
	 * 57 MB it dominated the cost -- copying it into every test's cache
	 * pushed the package tests past a ten-minute timeout. Its own
	 * declared closure (zlib, flex, m4) went with it. If a future
	 * fixture genuinely needs a linker, it declares binutils and this
	 * table gains it back, deliberately.
	 */
	/*
	 * glibc is the C library every composed build environment now gets
	 * implicitly (#186) -- the loader and libc that used to be copied
	 * off the build host by pkg_seed_image_baseline(). A fixture build
	 * cannot exec anything at all without it, so it belongs in the
	 * floor for exactly the reason the others do: an install that must
	 * be a cache hit, from a real artifact this platform built.
	 */
	/*
	 * libc-dev is gone from this floor (#187/#117), and its two jobs
	 * are split the way every real recipe now splits them: glibc owns
	 * the C headers and CRT objects, linux-headers owns the kernel UAPI
	 * headers glibc's own limits.h chain includes.
	 *
	 * This is not tidying. The floor was pinned to libc-dev 2.36-3 and
	 * glibc 2.44-6 -- so a package here compiled against glibc 2.36's
	 * headers while linking 2.44's library, which is exactly the defect
	 * #187 found on the real box, reproduced faithfully in the one
	 * place meant to catch it. Every local test run was validating the
	 * world being retired.
	 */
	{ "bash", "5.2.37-2" }, { "coreutils", "9.11-3" }, { "tcc", "0.9.27-7" },
	{ "glibc", "2.44-12" }, { "linux-headers", "6.18.40-4" },
};

static int sha256_file_hex(const char *path, char *out, size_t out_size)
{
	char cmd_path[] = "/usr/bin/sha256sum";
	int pfd[2];
	pid_t pid;
	ssize_t n;
	int status;

	if (out_size < 65 || pipe(pfd) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		char *argv[] = { cmd_path, (char *)path, NULL };

		dup2(pfd[1], 1);
		close(pfd[0]);
		close(pfd[1]);
		execve(cmd_path, argv, environ);
		_exit(127);
	}
	close(pfd[1]);
	n = read(pfd[0], out, out_size - 1);
	close(pfd[0]);
	waitpid(pid, &status, 0);
	if (n < 64)
		return -1;
	out[64] = '\0';
	return 0;
}

/* Reads pkg_artifact_sha256="..." out of a recipe -- the line that
 * approves those exact bytes, and the only thing that makes a cached
 * artifact trustworthy. */
static int recipe_artifact_sha(const char *name, const char *version, char *out, size_t out_size)
{
	char path[PATH_MAX];
	char line[512];
	FILE *f;
	int found = 0;

	snprintf(path, sizeof(path), "recipes/package/%s/%s/build.sh", name, version);
	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p = strstr(line, "pkg_artifact_sha256=\"");

		if (line == strstr(line, "pkg_artifact_sha256=\"") && p != NULL) {
			p += strlen("pkg_artifact_sha256=\"");
			snprintf(out, out_size, "%.64s", p);
			found = 1;
			break;
		}
	}
	fclose(f);
	return found ? 0 : -1;
}

static int copy_tree_via_cp(const char *src, const char *dst)
{
	/*
	 * TOOLCHAIN_CP_BIN, not a second literal -- and certainly not
	 * "/bin/cp", which is what this said and which never existed here.
	 * This platform's images ship bin/sh and usr/bin/<tool> and nothing
	 * else in /bin; coreutils installs cp at usr/bin/cp, confirmed
	 * against the package's own file list. Same trap CLAUDE.md already
	 * records for /bin/bash.
	 *
	 * The correct path was already defined at the top of this very
	 * file and used by the other copy helper, which is the part worth
	 * noticing: this was not unknown, it was just not reused.
	 */
	char *argv[] = { (char *)TOOLCHAIN_CP_BIN, (char *)"-a", (char *)src, (char *)dst, NULL };
	pid_t pid = fork();
	int status;

	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve(TOOLCHAIN_CP_BIN, argv, environ);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		/*
		 * Say which copy failed and how. This returned a bare -1, and
		 * its callers report only their own generic message, so a
		 * missing cp surfaced as "could not seed the build floor" --
		 * which points at the artifacts, which were fine. Exit 127 is
		 * the specific tell that the binary itself was not found.
		 */
		fprintf(stderr, "cp -a %s %s failed (%s)\n", src, dst,
		        WIFEXITED(status) && WEXITSTATUS(status) == 127
		            ? "/usr/bin/cp not found or not executable"
		            : "non-zero exit");
		return -1;
	}
	return 0;
}

int test_image_fixture_seed_floor_packages(const char *data_dir, const char *artifacts_dir)
{
	char pkg_dir[PATH_MAX], cache_dir[PATH_MAX], recipes_dir[PATH_MAX];
	size_t i;

	snprintf(pkg_dir, sizeof(pkg_dir), "%s/rebuildable/pkg", data_dir);
	snprintf(cache_dir, sizeof(cache_dir), "%s/cache", pkg_dir);
	snprintf(recipes_dir, sizeof(recipes_dir), "%s/recipes", pkg_dir);
	if (mkdir_p(cache_dir) != 0 || mkdir_p(recipes_dir) != 0)
		return -1;

	for (i = 0; i < sizeof(floor_packages) / sizeof(floor_packages[0]); i++) {
		const char *name = floor_packages[i].name;
		const char *version = floor_packages[i].version;
		char src[PATH_MAX], dst[PATH_MAX];
		char want[128], got[128];
		char recipe_src[PATH_MAX], recipe_dst_dir[PATH_MAX];

		snprintf(src, sizeof(src), "%s/%s-%s.tar.gz", artifacts_dir, name, version);
		if (access(src, R_OK) != 0) {
			fprintf(stderr,
			        "floor package %s@%s is not present at %s -- fetch the real artifacts "
			        "before running this test; they are not fabricated here\n",
			        name, version, src);
			return -1;
		}
		if (recipe_artifact_sha(name, version, want, sizeof(want)) != 0) {
			fprintf(stderr, "recipe for %s@%s has no pkg_artifact_sha256 to verify against\n",
			        name, version);
			return -1;
		}
		if (sha256_file_hex(src, got, sizeof(got)) != 0 || strcmp(want, got) != 0) {
			fprintf(stderr,
			        "floor package %s@%s does not match the checksum its own recipe approves "
			        "(recipe %.16s..., file %.16s...)\n",
			        name, version, want, got);
			return -1;
		}

		/* Into the cache: pkg_cache_has() is a stat(), so a present
		 * tarball makes this install a cache hit -- no build
		 * environment, no network, exactly as on a fresh host. */
		snprintf(dst, sizeof(dst), "%s/%s-%s.tar.gz", cache_dir, name, version);
		if (copy_tree_via_cp(src, dst) != 0)
			return -1;

		/* And the real recipe alongside it: the cache holds bytes, the
		 * recipe is what approves them. Copying the genuine recipe
		 * keeps one source of truth rather than a test-shaped
		 * imitation of one. */
		snprintf(recipe_dst_dir, sizeof(recipe_dst_dir), "%s/%s", recipes_dir, name);
		if (mkdir(recipe_dst_dir, 0755) != 0 && errno != EEXIST)
			return -1;
		snprintf(recipe_src, sizeof(recipe_src), "recipes/package/%s/%s", name, version);
		if (copy_tree_via_cp(recipe_src, recipe_dst_dir) != 0)
			return -1;
	}
	return 0;
}

int test_image_fixture_clear_floor_cache(const char *data_dir)
{
	char cache_dir[PATH_MAX];
	size_t i;

	snprintf(cache_dir, sizeof(cache_dir), "%s/rebuildable/pkg/cache", data_dir);
	for (i = 0; i < sizeof(floor_packages) / sizeof(floor_packages[0]); i++) {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s/%s-%s.tar.gz", cache_dir, floor_packages[i].name,
		         floor_packages[i].version);
		if (unlink(path) != 0 && errno != ENOENT)
			return -1;
	}
	return 0;
}

/* ---- real shared-library closure derivation (see the header) ---- */

#define CLOSURE_MAX_SEEN 128
#define CLOSURE_NAME_MAX 96

struct closure_seen {
	char name[CLOSURE_MAX_SEEN][CLOSURE_NAME_MAX];
	int n;
};

/* The runtime test_image_fixture_build() already stages. Re-staging
 * either from a different tree would replace a working loader. */
static int closure_is_runtime(const char *soname)
{
	return strcmp(soname, "libc.so.6") == 0 || strcmp(soname, "ld-linux-x86-64.so.2") == 0;
}

static int closure_seen_add(struct closure_seen *seen, const char *name)
{
	int i;

	for (i = 0; i < seen->n; i++) {
		if (strcmp(seen->name[i], name) == 0)
			return 1; /* already handled */
	}
	if (seen->n >= CLOSURE_MAX_SEEN) {
		fprintf(stderr, "shared-library closure exceeded %d entries at %s\n",
		        CLOSURE_MAX_SEEN, name);
		return -1;
	}
	snprintf(seen->name[seen->n], CLOSURE_NAME_MAX, "%s", name);
	seen->n++;
	return 0;
}

/*
 * Reads an ELF64 file's DT_NEEDED entries. Returns 0 with *out_n set
 * (0 for a static binary, or one with no PT_DYNAMIC), or -1.
 *
 * The whole file is read into memory rather than seeked around: these
 * are binaries and libraries of a few hundred KB, the parse touches the
 * program headers, one PT_DYNAMIC segment and one string table, and a
 * single read is both simpler and harder to get wrong than a series of
 * pread()s whose offsets all come from the file's own untrusted fields.
 */
static int elf_needed(const char *path, char out[][CLOSURE_NAME_MAX], int max, int *out_n)
{
	unsigned char *buf = NULL;
	off_t size;
	int fd, i, rc = -1;
	ssize_t got, off = 0;
	Elf64_Ehdr *eh;
	Elf64_Phdr *ph;
	Elf64_Dyn *dyn = NULL;
	size_t dyn_count = 0;
	Elf64_Addr strtab_vaddr = 0;
	const char *strtab = NULL;

	*out_n = 0;
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror(path);
		return -1;
	}
	size = lseek(fd, 0, SEEK_END);
	if (size <= 0 || lseek(fd, 0, SEEK_SET) != 0) {
		fprintf(stderr, "%s: cannot determine size\n", path);
		close(fd);
		return -1;
	}
	buf = malloc((size_t)size);
	if (buf == NULL) {
		perror("malloc");
		close(fd);
		return -1;
	}
	while (off < (ssize_t)size) {
		got = read(fd, buf + off, (size_t)size - (size_t)off);
		if (got <= 0) {
			perror(path);
			goto out;
		}
		off += got;
	}
	close(fd);
	fd = -1;

	if ((size_t)size < sizeof(Elf64_Ehdr) || memcmp(buf, ELFMAG, SELFMAG) != 0 ||
	    buf[EI_CLASS] != ELFCLASS64) {
		/* Not an ELF64 object at all -- a script, or something else
		 * entirely. It has no closure to stage, which is not an error. */
		rc = 0;
		goto out;
	}
	eh = (Elf64_Ehdr *)buf;
	if (eh->e_phoff == 0 || eh->e_phentsize != sizeof(Elf64_Phdr) ||
	    eh->e_phoff + (off_t)eh->e_phnum * eh->e_phentsize > size) {
		fprintf(stderr, "%s: malformed program headers\n", path);
		goto out;
	}

	/* PT_DYNAMIC holds the entries; its DT_STRTAB is a virtual address,
	 * so a PT_LOAD segment is needed to map it back to a file offset. */
	for (i = 0; i < eh->e_phnum; i++) {
		ph = (Elf64_Phdr *)(buf + eh->e_phoff + (size_t)i * sizeof(Elf64_Phdr));
		if (ph->p_type != PT_DYNAMIC)
			continue;
		if (ph->p_offset + ph->p_filesz > (Elf64_Xword)size) {
			fprintf(stderr, "%s: PT_DYNAMIC out of range\n", path);
			goto out;
		}
		dyn = (Elf64_Dyn *)(buf + ph->p_offset);
		dyn_count = ph->p_filesz / sizeof(Elf64_Dyn);
		break;
	}
	if (dyn == NULL) {
		rc = 0; /* static, or no dynamic section */
		goto out;
	}
	for (i = 0; (size_t)i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
		if (dyn[i].d_tag == DT_STRTAB)
			strtab_vaddr = dyn[i].d_un.d_ptr;
	}
	if (strtab_vaddr == 0) {
		fprintf(stderr, "%s: dynamic section has no DT_STRTAB\n", path);
		goto out;
	}
	for (i = 0; i < eh->e_phnum; i++) {
		ph = (Elf64_Phdr *)(buf + eh->e_phoff + (size_t)i * sizeof(Elf64_Phdr));
		if (ph->p_type != PT_LOAD)
			continue;
		if (strtab_vaddr < ph->p_vaddr || strtab_vaddr >= ph->p_vaddr + ph->p_filesz)
			continue;
		strtab = (const char *)(buf + ph->p_offset + (strtab_vaddr - ph->p_vaddr));
		break;
	}
	if (strtab == NULL) {
		fprintf(stderr, "%s: DT_STRTAB is in no PT_LOAD segment\n", path);
		goto out;
	}
	for (i = 0; (size_t)i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
		const char *name;

		if (dyn[i].d_tag != DT_NEEDED)
			continue;
		name = strtab + dyn[i].d_un.d_val;
		if (name < (const char *)buf || name >= (const char *)buf + size) {
			fprintf(stderr, "%s: DT_NEEDED name out of range\n", path);
			goto out;
		}
		if (*out_n >= max) {
			fprintf(stderr, "%s: more than %d DT_NEEDED entries\n", path, max);
			goto out;
		}
		snprintf(out[*out_n], CLOSURE_NAME_MAX, "%s", name);
		(*out_n)++;
	}
	rc = 0;
out:
	if (fd >= 0)
		close(fd);
	free(buf);
	return rc;
}

static int stage_closure_rec(const char *image_root, const char *binary_path,
                             const char *const *search_dirs, struct closure_seen *seen)
{
	char needed[32][CLOSURE_NAME_MAX];
	int n = 0, i, d;

	if (elf_needed(binary_path, needed, 32, &n) != 0)
		return -1;

	for (i = 0; i < n; i++) {
		char resolved[PATH_MAX];
		char dst[PATH_MAX];
		int added, found = 0;

		if (closure_is_runtime(needed[i]))
			continue;
		added = closure_seen_add(seen, needed[i]);
		if (added < 0)
			return -1;
		if (added == 1)
			continue;

		for (d = 0; search_dirs[d] != NULL; d++) {
			struct stat st;

			snprintf(resolved, sizeof(resolved), "%s/%s", search_dirs[d], needed[i]);
			if (stat(resolved, &st) == 0 && S_ISREG(st.st_mode)) {
				found = 1;
				break;
			}
		}
		if (!found) {
			fprintf(stderr, "%s needs %s, which is in none of the search directories:\n",
			        binary_path, needed[i]);
			for (d = 0; search_dirs[d] != NULL; d++)
				fprintf(stderr, "  %s\n", search_dirs[d]);
			return -1;
		}

		{
			char libdir[PATH_MAX];

			snprintf(libdir, sizeof(libdir), "%s/" CIX_LIB_DIR_RUNTIME, image_root);
			if (mkdir_p(libdir) != 0)
				return -1;
		}
		snprintf(dst, sizeof(dst), "%s/" CIX_LIB_DIR_RUNTIME "/%s", image_root, needed[i]);
		if (test_image_fixture_copy_file(resolved, dst) != 0)
			return -1;

		/* A library has its own DT_NEEDED entries, resolved from the
		 * same tree it was found in. */
		if (stage_closure_rec(image_root, resolved, search_dirs, seen) != 0)
			return -1;
	}
	return 0;
}

int test_image_fixture_stage_closure(const char *image_root, const char *binary_path,
                                     const char *const *search_dirs)
{
	struct closure_seen seen;

	seen.n = 0;
	return stage_closure_rec(image_root, binary_path, search_dirs, &seen);
}

int test_image_fixture_stage_build_image(const char *data_dir, const char *image_name)
{
	char rootfs[PATH_MAX];
	char manifest[PATH_MAX];
	FILE *f;

	snprintf(rootfs, sizeof(rootfs), "%s/rebuildable/images/%s/hbfixture/rootfs", data_dir,
	         image_name);
	if (test_image_fixture_stage_toolchain(rootfs) != 0)
		return -1;

	snprintf(manifest, sizeof(manifest), "%s/rebuildable/images/%s/manifest.json", data_dir,
	         image_name);
	f = fopen(manifest, "w");
	if (f == NULL) {
		perror(manifest);
		return -1;
	}
	fprintf(f,
	        "{\"packages\":[],\"current_version\":\"hbfixture\","
	        "\"versions\":[{\"version\":\"hbfixture\",\"created_at\":%ld}]}",
	        (long)time(NULL));
	fclose(f);
	return 0;
}
