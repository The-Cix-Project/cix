#include "test_image_fixture.h"

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
	if (test_image_fixture_copy_file("/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu", image_root);
	if (mkdir_p(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu/libc.so.6", image_root);
	if (test_image_fixture_copy_file("/lib/x86_64-linux-gnu/libc.so.6", path) != 0)
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
	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", image_root);
	if (test_image_fixture_copy_file("/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", path) != 0)
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
	char tmpl[] = "/tmp/cix_test_data_XXXXXX";

	if (mkdtemp(tmpl) == NULL) {
		perror("mkdtemp");
		return -1;
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
 * build/floor-artifacts, and each is verified against the checksum in
 * its own recipe before it is used. Bumping one means fetching the new
 * artifact and re-verifying -- not editing this table alone.
 */
static const struct {
	const char *name;
	const char *version;
} floor_packages[] = {
	{ "bash", "5.2.37-2" },   { "coreutils", "9.11-3" }, { "tcc", "0.9.27-7" },
	{ "make", "4.4.1-4" },    { "sed", "4.9-2" },        { "grep", "3.11-4" },
	{ "gawk", "5.3.0-2" },    { "binutils", "2.42-8" },
	/*
	 * The closure, not just the names above: binutils declares
	 * pkg_depends="zlib flex" and flex declares "m4", so an install of
	 * binutils resolves all three before it will start. Leaving them
	 * out fails as "no such recipe, or it failed to parse", which
	 * names the recipe being installed rather than the dependency
	 * actually missing -- found exactly that way.
	 */
	{ "zlib", "1.3.2-6" },    { "flex", "2.6.4-4" },     { "m4", "1.4.19-2" },
	/*
	 * libc-dev: headers and the CRT startup objects, without which tcc
	 * cannot compile anything at all. Pinned to 2.36-3 because that is
	 * the newest version a Cix host has actually built and published --
	 * 2.36-5 and 2.36-6 exist as recipes but have no artifact yet, and
	 * a version with no artifact cannot seed a floor that exists
	 * precisely to avoid needing a build environment.
	 */
	{ "libc-dev", "2.36-3" },
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
	char *argv[] = { (char *)"/bin/cp", (char *)"-a", (char *)src, (char *)dst, NULL };
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return -1;
	if (pid == 0) {
		execve("/bin/cp", argv, environ);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
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
