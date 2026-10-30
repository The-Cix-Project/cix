/*
 * test_image_fixture_host.c -- the fixture code that runs programs off
 * the host it is on, split out of test_image_fixture.c (#554).
 *
 * test_image_fixture.c is linked into cixd, mkbootroot and
 * mkinstalleriso, which run on the control-plane root, and that root
 * carries only the programs include/controlplane_programs.h lists. The
 * functions here fork `cp -a` and `sha256sum`, which it does not carry:
 * the recursive copy test_installer uses, and the ADR-0209 test floor.
 * So they live in a file only tests link, and the
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
 * handling a hand-rolled recursive copier would get wrong ("real
 * software for a genuinely hard problem"). */
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
	 * off the build host by the old image baseline. A fixture build
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
	{ "cbs", "v0.1.107-1" }, { "libarchive", "3.8.1-5" }, { "zstd", "1.5.7-5" },
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

	/* The recipe is CPDL, the only kind there is (cix#569). */
	if (test_recipe_path("package", name, version, "cbs", path, sizeof(path)) != 0)
		return -1;
	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p;

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
 * This floor artifact's path: <name>-<version>.cixpkg, the only format
 * there is (cix#569). Returns its extension, or NULL with out holding
 * the path the caller should name in its error.
 */
static const char *floor_artifact_find(const char *artifacts_dir, const char *name,
                                        const char *version, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s-%s.cixpkg", artifacts_dir, name, version);
	return access(out, R_OK) == 0 ? "cixpkg" : NULL;
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
			        "floor package %s@%s is not present at %s -- fetch the "
			        "real artifacts before running this test; they are not fabricated "
			        "here\n",
			        name, version, src);
			return -1;
		}
		if (recipe_artifact_sha(name, version, want, sizeof(want)) != 0) {
			fprintf(stderr, "recipe for %s@%s has no artifact_sha256 to verify against\n",
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
		 * .cixpkg makes this install a cache hit -- no build
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
		if (test_recipe_path("package", name, version, "cbs", recipe_src,
		                     sizeof(recipe_src)) != 0)
			return -1;
		snprintf(dst, sizeof(dst), "%s/build.cbs", recipe_dst_dir);
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
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s/%s-%s.cixpkg", cache_dir, floor_packages[i].name,
		         floor_packages[i].version);
		if (unlink(path) != 0 && errno != ENOENT)
			return -1;
	}
	return 0;
}
