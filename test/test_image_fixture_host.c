/*
 * test_image_fixture_host.c -- the fixture code that runs programs off
 * the host it is on, split out of test_image_fixture.c (#554).
 *
 * test_image_fixture.c is linked into cixd, mkbootroot and
 * mkinstalleriso, which run on the control-plane root, and that root
 * carries only the programs include/controlplane_programs.h lists. The
 * functions here fork `cp -a` and `sha256sum`, which it does not carry:
 * the toolchain staging mktoolchainimage does on a development host, the
 * recursive copy test_installer uses, and the ADR-0209 test floor. So
 * they live in a file only tests and mktoolchainimage link, and the
 * control-plane binaries cannot call them. Leaving them where they were
 * is how mkbootroot came to fork `cp` on a root that had none
 * (0.2.57-414's assembly failed on it).
 */
#include <time.h>
#include "libdirs.h"
#include "test_image_fixture.h"

#include <elf.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TOOLCHAIN_CP_BIN "/usr/bin/cp"

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

	if (test_mkdir_p(image_root) != 0)
		return -1;
	snprintf(usr_dst, sizeof(usr_dst), "%s/usr", image_root);
	if (test_mkdir_p(usr_dst) != 0)
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

		if (test_mkdir_p(dst_parent) != 0 || run_cp_a(extras[i].src, dst) != 0)
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
		if (test_mkdir_p(dev_dir) != 0)
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
		if (test_mkdir_p(tmp_dir) != 0)
			return -1;
		if (chmod(tmp_dir, 01777) != 0)
			return -1;
	}

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
	 * binutils, for strip. The finalize policy strips every ELF a
	 * package produces and refuses the build when strip is absent
	 * (daemon/policy/pkg-finalize.sh, ADR-0250/0251). A fixture that
	 * compiles hello with tcc produces ELF, so without binutils every
	 * such build failed: "cix: this package produced ELF output but the
	 * build image has no strip" (probe-cix-testreport on 192.168.15.95,
	 * 2026-09-23). binutils was once removed from this floor because its
	 * size pushed the package tests past a ten-minute timeout; cix-tests
	 * runs each test under its own 300 s limit and will show if that
	 * returns. Its runtime closure is zlib and flex, and since the
	 * floor moved to .cixpkg (#529) that flex is 2.6.4-6, which
	 * declares m4 -- so m4 is in the list below as flex's own runtime
	 * dependency. This sentence used to end "flex 2.6.4-2 is the
	 * approved revision that declares no m4", which the list two dozen
	 * lines down had already contradicted.
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
	 *
	 * glibc is 2.44-19 and not 2.44-12 for a second reason of the same
	 * kind: 2.44-19 is the first revision to ship the C.UTF-8 locale at
	 * /usr/lib/locale/C.utf8 (#518), and cbs takes a UTF-8 LC_CTYPE
	 * before it reads an archive header (cix-build-system#232). On the
	 * older one every CPDL build in the floor died at its first source:
	 * "error[CPDL-E6001]: source: source `kernel`: cannot initialize a
	 * UTF-8 archive locale" (probe-cix-testreport@21 on 192.168.15.95,
	 * 2026-09-25).
	 */
	/*
	 * cbs, because a CPDL recipe is built BY it: cixd composes a build
	 * environment holding the declared tools plus cbs itself, and with
	 * cbs installed nowhere there is nothing to compose from --
	 * "declared build tool \"cbs\" is not installed anywhere"
	 * (probe-cix-testreport@16 on 192.168.15.95, 2026-09-24). It was
	 * not needed while every fixture recipe was a shell one, and
	 * test_kmod_build's became CPDL in v2.57.263 so it could read a
	 * kmod-build's symbols through `args input` the way the real kernel
	 * recipe does (#517). Its artifact is a .cixpkg, which is what
	 * floor_artifact_find() above exists for.
	 */
	{ "bash", "5.2.37-6" }, { "coreutils", "9.11-8" }, { "tcc", "0.9.28rc-31" },
	{ "glibc", TEST_FLOOR_GLIBC_VERSION }, { "linux-headers", "6.18.40-10" },
	{ "zlib", "1.3.2-15" }, { "flex", "2.6.4-6" }, { "binutils", "2.42-15" },
	/*
	 * cbs and its runtime closure. cbs declares libarchive and zstd;
	 * libarchive declares zlib, xz and zstd, and zlib is already here.
	 * Without them cbs's own install is refused before it starts: "a
	 * dependency of this package could not be resolved"
	 * (probe-cix-testreport@18 on 192.168.15.95, 2026-09-24).
	 *
	 * All three are in test_floor_install[] below as well, rather than
	 * left to arrive behind cbs the way zlib and flex arrive behind
	 * binutils. Seeding alone was tried first and the run that followed
	 * reached "cbs installed in 0s" and then died with the build
	 * container's pid 1 exiting 127 after 0s -- an execve that never
	 * started, with nothing in the log saying whether cbs's libraries
	 * had reached the container at all (probe-cix-testreport@19,
	 * 2026-09-25). An explicit install is logged per package, so the
	 * next run answers that instead of leaving it assumed.
	 */
	{ "cbs", "v0.1.100-1" }, { "libarchive", "3.8.1-5" }, { "zstd", "1.5.7-5" },
	{ "xz", "5.8.3-11" },
	/* m4: flex@2.6.4-6 declares it as a RUNTIME dependency where the
	 * shell flex@2.6.4-2 did not, so binutils -- whose runtime deps are
	 * zlib and flex -- cannot resolve without it (cix#529). m4 declares
	 * no runtime package of its own, so it is a leaf. */
	{ "m4", "1.4.20-6" },
};

/*
 * The floor packages a test installs explicitly, in order, after seeding.
 * One list, where six tests each carried their own copy -- which is how
 * binutils reached the seeded floor above and none of the install lists
 * ("declared build tool binutils is not installed anywhere",
 * probe-cix-testreport@2, 2026-09-23). glibc is not here: the daemon
 * installs it into the default image itself. zlib and flex arrive as
 * binutils' runtime dependencies.
 *
 * IN ORDER matters for the tail: xz and zstd carry no dependency of
 * their own, libarchive wants both plus zlib, and cbs wants libarchive
 * and zstd -- so each one's dependencies are already installed when it
 * is reached, and a failure names the package that actually failed
 * rather than something resolved behind it.
 */
const char *const test_floor_install[] = { "bash",       "coreutils", "tcc",  "linux-headers",
                                           "binutils",   "xz",        "zstd", "libarchive",
                                           "cbs",        NULL };

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


/* Reads an artifact approval out of a recipe -- the line that
 * approves those exact bytes, and the only thing that makes a cached
 * artifact trustworthy. */
static int recipe_artifact_sha(const char *name, const char *version, char *out, size_t out_size)
{
	char path[PATH_MAX];
	char line[512];
	FILE *f;
	int found = 0;

	/*
	 * ADR-0305: a recipe is a shell one or a CBS one, and which is
	 * decided by the extension. Both are tried because the corpus
	 * holds a mix and a package converts on its own schedule -- a
	 * fixture that only knew build.sh would start failing the day the
	 * package it reads converts, which is a failure with no
	 * connection to what the test is checking.
	 */
	if (test_recipe_path("package", name, version, "sh", path, sizeof(path)) != 0)
		return -1;
	f = fopen(path, "r");
	if (f == NULL) {
		if (test_recipe_path("package", name, version, "cbs", path, sizeof(path)) != 0)
			return -1;
		f = fopen(path, "r");
	}
	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p;

		/* Shell: pkg_artifact_sha256="<64 hex>" at the line start. */
		if (line == strstr(line, "pkg_artifact_sha256=\"")) {
			p = line + strlen("pkg_artifact_sha256=\"");
			snprintf(out, out_size, "%.64s", p);
			found = 1;
			break;
		}
		/* CBS: "artifact_sha256" "<64 hex>" inside the metadata
		 * block, so indented rather than at the line start. */
		p = strstr(line, "\"artifact_sha256\"");
		if (p != NULL) {
			p = strchr(p + strlen("\"artifact_sha256\""), '"');
			if (p == NULL)
				continue;
			snprintf(out, out_size, "%.64s", p + 1);
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

/*
 * Which extension this floor artifact actually has, and its full path.
 * Returns the extension, or NULL with out holding the .tar.gz path the
 * caller should name in its error.
 *
 * Both exist and both are current: everything published before ADR-0307
 * is a .tar.gz and everything since is a .cixpkg, which is why the
 * daemon's own cache_artifact_path_existing() tries the two in this
 * order rather than assuming. This used to hardcode .tar.gz, so the
 * floor could hold nothing published recently -- including `cbs`,
 * without which a composed build environment cannot build a CPDL recipe
 * at all, and test_kmod_build's fixture became one in v2.57.263
 * ("declared build tool \"cbs\" is not installed anywhere",
 * probe-cix-testreport@16 on 192.168.15.95, 2026-09-24).
 */
static const char *floor_artifact_find(const char *artifacts_dir, const char *name,
                                        const char *version, char *out, size_t out_size)
{
	static const char *const exts[] = { "cixpkg", "tar.gz" };
	size_t i;

	for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		snprintf(out, out_size, "%s/%s-%s.%s", artifacts_dir, name, version, exts[i]);
		if (access(out, R_OK) == 0)
			return exts[i];
	}
	snprintf(out, out_size, "%s/%s-%s.tar.gz", artifacts_dir, name, version);
	return NULL;
}

int test_image_fixture_seed_floor_packages(const char *data_dir, const char *artifacts_dir)
{
	char pkg_dir[PATH_MAX], cache_dir[PATH_MAX], recipes_dir[PATH_MAX];
	size_t i;

	snprintf(pkg_dir, sizeof(pkg_dir), "%s/rebuildable/pkg", data_dir);
	snprintf(cache_dir, sizeof(cache_dir), "%s/cache", pkg_dir);
	snprintf(recipes_dir, sizeof(recipes_dir), "%s/recipes", pkg_dir);
	if (test_mkdir_p(cache_dir) != 0 || test_mkdir_p(recipes_dir) != 0)
		return -1;

	for (i = 0; i < sizeof(floor_packages) / sizeof(floor_packages[0]); i++) {
		const char *name = floor_packages[i].name;
		const char *version = floor_packages[i].version;
		char src[PATH_MAX], dst[PATH_MAX];
		char want[128], got[128];
		char recipe_src[PATH_MAX], recipe_dst_dir[PATH_MAX];
		const char *ext;

		ext = floor_artifact_find(artifacts_dir, name, version, src, sizeof(src));
		if (ext == NULL) {
			fprintf(stderr,
			        "floor package %s@%s is not present at %s (nor .cixpkg) -- fetch the "
			        "real artifacts before running this test; they are not fabricated "
			        "here\n",
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
		snprintf(dst, sizeof(dst), "%s/%s-%s.%s", cache_dir, name, version, ext);
		if (copy_tree_via_cp(src, dst) != 0)
			return -1;

		/* And the real recipe alongside it: the cache holds bytes, the
		 * recipe is what approves them. Copying the genuine recipe
		 * keeps one source of truth rather than a test-shaped
		 * imitation of one. */
		/* #505: the corpus is flat, so this copies one FILE rather
		 * than a version directory, and the destination has to name
		 * it. The daemon's own recipe store keeps its directory
		 * shape -- only the repository changed -- so the layout
		 * written here is still <name>/<version>/build.<ext>. */
		snprintf(recipe_dst_dir, sizeof(recipe_dst_dir), "%s/%s/%s", recipes_dir, name,
		         version);
		if (test_mkdir_p(recipe_dst_dir) != 0)
			return -1;
		if (test_recipe_path("package", name, version, "sh", recipe_src,
		                     sizeof(recipe_src)) != 0)
			return -1;
		snprintf(dst, sizeof(dst), "%s/build.sh", recipe_dst_dir);
		if (access(recipe_src, R_OK) != 0) {
			if (test_recipe_path("package", name, version, "cbs", recipe_src,
			                     sizeof(recipe_src)) != 0)
				return -1;
			snprintf(dst, sizeof(dst), "%s/build.cbs", recipe_dst_dir);
		}
		if (copy_tree_via_cp(recipe_src, dst) != 0)
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
		static const char *const exts[] = { "cixpkg", "tar.gz" };
		char path[PATH_MAX];
		size_t j;

		/* Both, because the seed writes whichever the artifact has
		 * and this has to leave no cache hit behind either way -- a
		 * missed one makes the very next install silently succeed
		 * from cache, which is the opposite of what a caller clearing
		 * the floor wants. */
		for (j = 0; j < sizeof(exts) / sizeof(exts[0]); j++) {
			snprintf(path, sizeof(path), "%s/%s-%s.%s", cache_dir,
			         floor_packages[i].name, floor_packages[i].version, exts[j]);
			if (unlink(path) != 0 && errno != ENOENT)
				return -1;
		}
	}
	return 0;
}
