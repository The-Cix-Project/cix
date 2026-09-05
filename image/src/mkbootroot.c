/*
 * Assembles a minimal Phase 11 boot root at a staging directory and
 * squashes it into a single read-only image: cixd + the ld.so/libc.so.6
 * it dynamically links against (per the project's TCC-wide "never -static"
 * rule), plus empty /proc, /sys, and BASE_DIR mountpoints for
 * cixd --init-mode's own boot-time mounts (daemon/src/main.c's
 * boot_init()) to mount onto. Reuses test_image_fixture_build() (test/)
 * rather than re-implementing the same ld.so/libc staging a second time --
 * that function's own contract ("shared by every test that needs a real,
 * working image") already covers exactly this need; nothing about it is
 * test-specific.
 *
 * The base container image (base/rootfs) and any container/package data
 * are deliberately never part of this image -- Phase 11's root A/B slots
 * are scoped to the control plane only (docs/roadmap/ROADMAP.md).
 *
 * Phase 14 part 2 (ADR-0029): optionally also stages GPU firmware
 * (amdgpu) into this same root, since the kernel's own request_firmware()
 * calls happen at driver-probe time, on the host root, before any
 * container exists -- nowhere else this could live. The firmware itself
 * isn't fetched by this tool or vendored into the repo; it's an
 * operator-supplied directory (see README.md's own fetch recipe --
 * upstream linux-firmware's amdgpu/ subtree, via a sparse clone).
 */
#include "libdirs.h"
#include "test_image_fixture.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define MKSQUASHFS_BIN "/usr/bin/mksquashfs"

static int ensure_dir(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	return 0;
}

static int ensure_dir_under(const char *image_root, const char *rel)
{
	char path[PATH_MAX];

	if (snprintf(path, sizeof(path), "%s/%s", image_root, rel) >= (int)sizeof(path)) {
		fprintf(stderr, "path too long: %s/%s\n", image_root, rel);
		return -1;
	}
	return ensure_dir(path);
}

/*
 * Same, for a relative path with more than one component.
 *
 * ensure_dir_under() creates a single level, which was enough while
 * every directory this file wrote into already existed in the staged
 * root. It stopped being enough when the platform's own library set
 * grew to cover the whole layout (#184): usr/lib did not exist in the
 * stage root at that point, so the copy into it failed with a bare
 * ENOENT naming the FILE rather than the missing directory --
 * "stage/usr/lib/libblkid.so: No such file or directory" on a
 * libblkid.so that was present and readable at the source.
 */
static int ensure_dir_path_under(const char *image_root, const char *rel)
{
	char path[PATH_MAX];
	char *p;

	if (snprintf(path, sizeof(path), "%s/%s", image_root, rel) >= (int)sizeof(path)) {
		fprintf(stderr, "path too long: %s/%s\n", image_root, rel);
		return -1;
	}
	/* Start past image_root, which already exists and may itself
	 * contain separators. */
	for (p = path + strlen(image_root) + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (ensure_dir(path) != 0)
			return -1;
		*p = '/';
	}
	return ensure_dir(path);
}

/*
 * The library search path for a binary run straight out of the
 * host-tools image (ADR-0154).
 *
 * Built from CIX_LIB_DIRS_SEARCH rather than spelled out, and built
 * ONCE: the same three-directory string used to be composed
 * separately for LD_LIBRARY_PATH and for the loader's own
 * --library-path, so the two could drift apart while looking correct
 * in isolation. A directory the image does not have costs nothing --
 * the loader skips one that is not there -- so covering the whole
 * layout is strictly safer than naming a subset of it, which is what
 * the old three entries were.
 *
 * Failing loudly on truncation matters here: a silently shortened
 * search path drops directories off the END, and the dynamic linker
 * then falls back to the build host's own libraries with no error at
 * all (ADR-0154's confirmed failure mode -- a clean exit 0 against a
 * library the binary was never built for).
 */
static int host_tools_lib_path(const char *host_tools_dir, char *out, size_t out_size)
{
	static const char *const dirs[] = CIX_LIB_DIRS_SEARCH;
	size_t i;
	int n = 0;

	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		int w = snprintf(out + n, out_size - (size_t)n, "%s%s/%s", (i > 0) ? ":" : "",
		                 host_tools_dir, dirs[i]);
		if (w < 0 || (size_t)(n + w) >= out_size) {
			fprintf(stderr, "path too long: library search path for %s\n", host_tools_dir);
			return -1;
		}
		n += w;
	}
	return 0;
}

/*
 * host_tools_dir, when non-empty, is a real, ordinary image rootfs
 * (squashfs-tools.recipe's own install, ADR-0078) -- mksquashfs is
 * dynamically linked against liblzma.so.5 (this project's own
 * "never -static" rule), which that recipe stages under one of the
 * layout's library directories (CIX_LIB_DIRS_SEARCH -- which one is
 * not this file's business, and changes as #184 collapses them) --
 * an arbitrary filesystem path the dynamic linker has no reason to
 * search: it isn't a chroot
 * root (unlike an ordinary pkg build container, which reaches its own
 * libs via a real pivot_root, mkbootroot execve()s this binary
 * directly off the bare host), so nothing wires that directory into
 * ld.so's search path on its own. A genuinely missing/unresolvable
 * shared library normally fails loudly (dynamic linker startup exits
 * nonzero before main() even runs, correctly caught by the
 * WIFEXITED/WEXITSTATUS check below) -- but if the box's own bare
 * host environment happens to already have *some* liblzma.so.5
 * resolvable via the ordinary system search path (a different build,
 * a different version), the dynamic linker silently prefers that one
 * instead, with no error at all: mksquashfs runs and exits 0, but
 * against a library it was never actually built/tested against,
 * which is exactly the failure this real, live task #865 deploy hit
 * (`POST /system/update` correctly rejected the result as "not a
 * squashfs image" -- a genuine on-disk corruption, not a
 * misdiagnosis). Fixed by explicitly prepending host_tools_dir's own
 * library directories to LD_LIBRARY_PATH for this one child only,
 * so its own liblzma.so.5 is what actually gets linked, not whatever
 * the bare host happens to already have lying around.
 */
static int run_mksquashfs(const char *mksquashfs_bin, const char *image_root,
                           const char *out_path, const char *host_tools_dir)
{
	pid_t pid;
	int status;
	struct stat st;
	char *argv[] = { (char *)mksquashfs_bin, (char *)image_root, (char *)out_path,
		          "-noappend", "-comp", "xz", "-quiet", NULL };
	/*
	 * Invoked THROUGH the host-tools image's own dynamic loader, when
	 * that image has one, rather than relying on LD_LIBRARY_PATH alone.
	 *
	 * ADR-0154 pointed LD_LIBRARY_PATH at this image so a binary
	 * exec'd off the bare host would find its libraries. But the
	 * loader itself is chosen by the binary's PT_INTERP, which is the
	 * fixed path /lib64/ld-linux-x86-64.so.2 -- on the HOST. So the
	 * process got the host's loader and the image's libc, and once
	 * those two came from different glibc builds it stopped working
	 * entirely:
	 *
	 *   mksquashfs: symbol lookup error: .../libc.so.6:
	 *   undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE
	 *
	 * ld.so and libc.so.6 share a private, version-locked interface.
	 * Naming the loader explicitly and letting it resolve the rest
	 * keeps both halves from the same tree, which is the only
	 * arrangement that is correct rather than merely lucky. It is also
	 * the standard way to run a binary against a libc other than the
	 * system's.
	 */
	char ld_so[PATH_MAX];
	char lib_path[PATH_MAX];
	char *ld_argv[11];
	char **use_argv = argv;
	char ld_library_path[PATH_MAX];
	char *child_envp[64];
	int envc;

	/*
	 * A precise, disambiguating check before exec -- ADR-0083's own
	 * "ld-linux-x86-64.so.2: No such file or directory" symptom was
	 * bare ENOENT text with no indication of *which* of "the binary
	 * itself is missing" vs "the binary exists but can't resolve its
	 * own dynamic linker" was actually true. stat() answers the first
	 * question directly, so a future failure here reads unambiguously
	 * instead of needing another investigation like this one.
	 */
	if (stat(mksquashfs_bin, &st) != 0) {
		fprintf(stderr, "mksquashfs binary not found at %s: %s\n", mksquashfs_bin,
		        strerror(errno));
		return -1;
	}

	/* A stale image from a prior run must not silently linger under a
	 * new build -- mksquashfs itself refuses to overwrite without
	 * -noappend, so any leftover has to go first. */
	unlink(out_path);

	envc = 0;
	if (host_tools_dir != NULL && host_tools_dir[0] != '\0') {
		int i;

		if (snprintf(ld_library_path, sizeof(ld_library_path), "LD_LIBRARY_PATH=") >=
		    (int)sizeof(ld_library_path)) {
			fprintf(stderr, "path too long: LD_LIBRARY_PATH for %s\n", host_tools_dir);
			return -1;
		}
		if (host_tools_lib_path(host_tools_dir, ld_library_path + strlen("LD_LIBRARY_PATH="),
		                        sizeof(ld_library_path) - strlen("LD_LIBRARY_PATH=")) != 0)
			return -1;
		/* Copy the real inherited environment forward -- this is a real
		 * child process replacing the whole environment would silently
		 * drop, not add to, whatever else the daemon's own process was
		 * started with. Only room for 62 real entries plus this one
		 * plus the NULL terminator (sizeof(child_envp)/sizeof(*) == 64)
		 * -- a real, live daemon environment this large would be
		 * genuinely abnormal, so failing loudly here is correct, not a
		 * silent truncation. */
		for (i = 0; environ[i] != NULL; i++) {
			if (envc >= (int)(sizeof(child_envp) / sizeof(child_envp[0])) - 2) {
				fprintf(stderr, "too many environment variables to add LD_LIBRARY_PATH\n");
				return -1;
			}
			child_envp[envc++] = environ[i];
		}
		child_envp[envc++] = ld_library_path;
	}
	child_envp[envc] = NULL;

	/*
	 * Use the image's own loader when it ships one. An older
	 * host-tools image without it falls back to the plain invocation,
	 * which is exactly what happened before this existed.
	 */
	if (host_tools_dir != NULL && host_tools_dir[0] != '\0') {
		struct stat lst;

		snprintf(ld_so, sizeof(ld_so), "%s/lib64/ld-linux-x86-64.so.2", host_tools_dir);
		if (host_tools_lib_path(host_tools_dir, lib_path, sizeof(lib_path)) != 0)
			return -1;
		if (stat(ld_so, &lst) == 0) {
			ld_argv[0] = ld_so;
			ld_argv[1] = (char *)"--library-path";
			ld_argv[2] = lib_path;
			ld_argv[3] = (char *)mksquashfs_bin;
			ld_argv[4] = (char *)image_root;
			ld_argv[5] = (char *)out_path;
			ld_argv[6] = (char *)"-noappend";
			ld_argv[7] = (char *)"-comp";
			ld_argv[8] = (char *)"xz";
			ld_argv[9] = (char *)"-quiet";
			ld_argv[10] = NULL;
			use_argv = ld_argv;
		}
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve(use_argv[0], use_argv, envc > 0 ? child_envp : environ);
		fprintf(stderr, "execve %s: %s\n", mksquashfs_bin, strerror(errno));
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid) {
		perror("waitpid");
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "mksquashfs failed (status %d)\n", status);
		return -1;
	}
	return 0;
}

/*
 * Every library the platform built must still be the one the root carries.
 *
 * Staging order decides this: whatever is copied last wins, and a later
 * host-sourced copy silently replacing one of glibc's own objects leaves the
 * root with mismatched halves of a version-locked set -- init dies with exit
 * 127 and the machine panics on boot. That is exactly what happened, twice,
 * and assembly reported success both times, because copying files is not the
 * same as producing a root that runs.
 *
 * So the invariant is checked rather than merely arranged for: right before
 * the image is sealed, re-read every platform library and confirm the root's
 * copy is still byte-identical. Reordering this file cannot quietly reopen
 * the hole again.
 */
static int files_identical(const char *a, const char *b)
{
	FILE *fa, *fb;
	int same = 1;

	fa = fopen(a, "rb");
	if (fa == NULL)
		return 0;
	fb = fopen(b, "rb");
	if (fb == NULL) {
		fclose(fa);
		return 0;
	}
	for (;;) {
		char ba[65536], bb[65536];
		size_t na = fread(ba, 1, sizeof(ba), fa);
		size_t nb = fread(bb, 1, sizeof(bb), fb);

		if (na != nb || memcmp(ba, bb, na) != 0) {
			same = 0;
			break;
		}
		if (na == 0)
			break;
	}
	fclose(fa);
	fclose(fb);
	return same;
}

static int verify_platform_libs_intact(const char *image_root, const char *host_tools_dir)
{
	static const char *const lib_dirs[] = CIX_LIB_DIRS_PLATFORM;
	size_t d;
	int checked = 0;

	if (host_tools_dir[0] == '\0')
		return 0;

	for (d = 0; d < sizeof(lib_dirs) / sizeof(lib_dirs[0]); d++) {
		char src_dir[PATH_MAX];
		DIR *dh;
		struct dirent *de;

		snprintf(src_dir, sizeof(src_dir), "%s/%s", host_tools_dir, lib_dirs[d]);
		dh = opendir(src_dir);
		if (dh == NULL)
			continue;
		while ((de = readdir(dh)) != NULL) {
			char src[PATH_MAX], dst[PATH_MAX];
			struct stat fst;

			if (strstr(de->d_name, ".so") == NULL)
				continue;
			snprintf(src, sizeof(src), "%s/%s", src_dir, de->d_name);
			if (stat(src, &fst) != 0 || !S_ISREG(fst.st_mode))
				continue;
			snprintf(dst, sizeof(dst), "%s/%s/%s", image_root, lib_dirs[d], de->d_name);
			if (!files_identical(src, dst)) {
				fprintf(stderr,
				        "%s/%s in the assembled root is not the copy this "
				        "platform built -- something staged after the platform's "
				        "own libraries overwrote it. Shipping a mixed glibc set "
				        "panics the machine at boot, so this image is refused "
				        "rather than written.\n",
				        lib_dirs[d], de->d_name);
				closedir(dh);
				return 1;
			}
			checked++;
		}
		closedir(dh);
	}
	if (checked > 0)
		fprintf(stderr, "verified %d platform libraries survived staging intact\n", checked);
	return 0;
}

int main(int argc, char **argv)
{
	const char *image_root;
	const char *cixd_bin;
	const char *cixctl_bin;
	const char *web_dir;
	const char *out_path;
	const char *firmware_dir;
	const char *modules_dir;
	const char *kmod_bin_dir;
	const char *host_tools_dir;

	if (argc != 10) {
		fprintf(stderr,
		        "usage: %s <staging-dir> <build/cixd> <build/cixctl> <web-dir> "
		        "<out.squashfs> <amdgpu-firmware-dir-or-\"\"> <modules-dir-or-\"\"> "
		        "<kmod-bin-dir-or-\"\"> <host-tools-image-rootfs-or-\"\">\n",
		        argv[0]);
		return 2;
	}
	image_root = argv[1];
	cixd_bin = argv[2];
	cixctl_bin = argv[3];
	web_dir = argv[4];
	out_path = argv[5];
	firmware_dir = argv[6];
	/*
	 * Part 3 (bare-metal-readiness plan): modules_dir is a real kernel
	 * hostbuild's own harvested "lib/modules" directory (recipes/
	 * package/kernel's own pkg_install(), INSTALL_MOD_PATH= + a real
	 * depmod already run there) -- containing exactly one
	 * <kernelrelease> subdirectory, copied wholesale below.
	 * kmod_bin_dir is a real kmod build's own "usr/bin" (recipes/
	 * package/kmod, e.g. extracted from wherever it was pkg-installed)
	 * -- modprobe/depmod/
	 * insmod/lsmod/modinfo/rmmod, all symlinks to one real "kmod"
	 * binary. Both default to "" (skip), the exact same tolerant-
	 * default shape firmware_dir above already established -- a plain
	 * QEMU/CI boot test needs neither.
	 */
	modules_dir = argv[7];
	kmod_bin_dir = argv[8];
	/*
	 * Part D (from-source host-tools bootstrap, ADR-0078): the rootfs of
	 * a real pkg-installed image carrying coreutils.recipe + gzip.recipe
	 * (an operator-built "host tools" image, not this dev sandbox's own
	 * pre-existing /usr/bin) -- when given, cp/rm/sha256sum/gzip below
	 * are copied from THIS tree instead of the dev build host, closing
	 * the "control-plane squashfs ships a raw copy of this sandbox's own
	 * pre-compiled binaries" gap for the tools that already have a real
	 * from-source recipe. "" (the default, every existing call site
	 * before this argument was added) keeps today's dev-host-sourced
	 * behavior -- the same tolerant-default shape firmware_dir/
	 * modules_dir/kmod_bin_dir above already established. The remaining
	 * shelled-out tools (openssl/curl/tar/bzip2/xz/unsquashfs/mkfs.ext4)
	 * have no such recipe yet and still come from the dev host either
	 * way -- a real, tracked gap (tasks #688-693), not silently masked
	 * by this argument's presence.
	 */
	host_tools_dir = argv[9];

	if (ensure_dir(image_root) != 0)
		return 1;
	if (test_image_fixture_build(image_root, cixd_bin, "cixd") != 0)
		return 1;
	/*
	 * ADR-0246: cix-init, the supervisor that becomes pid 1 and keeps
	 * a wedged worker recoverable.
	 *
	 * Its path is derived from cixd's own directory rather than taken
	 * as an eleventh positional argument. The two binaries are always
	 * produced and shipped together -- the cix recipe's pkg_install()
	 * copies both into PKG_DESTDIR, and a local build leaves both in
	 * build/ -- so a separate argument would be a second way of saying
	 * the same thing, and eight argv-building call sites (this
	 * daemon's assembly path plus seven tests) would all have to agree
	 * about it. Two of this change's own build failures were exactly
	 * that: one call site of a changed signature missed among several.
	 *
	 * The invariant is enforced, not assumed. A root that silently
	 * lacked its own init would fail at boot, as a panic, on a machine
	 * with no shell -- so a missing cix-init fails the assembly here
	 * instead, in the same spirit as verify_platform_libs_intact()
	 * below refusing to seal a root whose libraries no longer match.
	 */
	{
		char cixinit_bin[PATH_MAX];
		struct stat init_st;
		const char *slash = strrchr(cixd_bin, '/');

		if (slash != NULL) {
			int dirlen = (int)(slash - cixd_bin);

			if (snprintf(cixinit_bin, sizeof(cixinit_bin), "%.*s/cix-init", dirlen,
			              cixd_bin) >= (int)sizeof(cixinit_bin)) {
				fprintf(stderr, "mkbootroot: cix-init path too long\n");
				return 1;
			}
		} else {
			snprintf(cixinit_bin, sizeof(cixinit_bin), "cix-init");
		}
		if (stat(cixinit_bin, &init_st) != 0 || !S_ISREG(init_st.st_mode)) {
			fprintf(stderr,
			        "mkbootroot: %s is missing -- cix-init must be built and installed "
			        "alongside cixd (ADR-0246); refusing to write a root with no "
			        "supervisor\n",
			        cixinit_bin);
			return 1;
		}
		if (test_image_fixture_build(image_root, cixinit_bin, "cix-init") != 0)
			return 1;
	}
	/*
	 * cixctl itself: staged so cixd --init-mode (Phase 19) has
	 * something to execve() when it spawns a managed shell on each
	 * console -- previously absent from this image entirely, meaning
	 * even a fully working console-input path would have had nothing to
	 * launch. Needs no extra runtime libs of its own beyond what's
	 * already staged above (ld.so/libc.so.6, the same TCC dynamic-link
	 * dependency cixd itself has).
	 */
	if (test_image_fixture_build(image_root, cixctl_bin, "cixctl") != 0)
		return 1;
	/*
	 * cixd itself never needs libtinfo -- this is staged purely so
	 * the RUNNING system's own root reliably has it available at its
	 * real, well-known host path, the source pkg_seed_image_baseline()
	 * (daemon/src/pkg.c) copies from when seeding a container image's
	 * own C runtime. Without this, that mechanism would only ever find
	 * ld.so/libc.so.6 (already staged above) on a real installed
	 * system -- this dev sandbox's own rich /usr made that gap easy to
	 * miss (see ADR-0023).
	 *
	 * The path passed here is BOTH the read source on this build host
	 * AND (via test_image_fixture_add_lib()'s own "image_root + this
	 * path" convention) the destination inside the assembled image --
	 * it must be the bare "/lib/..." form, not "/usr/lib/...", to
	 * match exactly where pkg_seed_image_baseline()'s own
	 * runtime_libs[] table looks for it later on the real installed
	 * system's own root. Confirmed live as a real, previously-
	 * undiscovered bug (not just a theoretical mismatch): the wrong,
	 * "/usr/lib/..."-prefixed destination silently landed this file
	 * where nothing ever looked for it, so every hostbuild whose
	 * build_image needed bash (which needs libtinfo) failed with
	 * "libtinfo.so.6: cannot open shared object file" -- invisible in
	 * every local test, since a locally-run cixd reads its own
	 * sandbox's real /lib and /usr/lib directly (merged-usr symlinks
	 * here make both forms resolve identically for the READ side),
	 * never from an assembled squashfs mounted as root the way a real
	 * install does. Same "this dev sandbox's own rich /usr masks a
	 * real-install-only path bug" pattern as CONFIG_OVERLAY_FS/
	 * CONFIG_SWAP/the mkbootroot gzip-bzip2-xz gap earlier this
	 * session -- found this time via the new build-output capture
	 * mechanism reading the real "cannot open shared object file"
	 * error straight out of a live remote hostbuild attempt.
	 */
	if (test_image_fixture_add_lib(image_root, "/" CIX_LIB_DIR_RUNTIME "/libtinfo.so.6") != 0)
		return 1;

	/*
	 * The real binaries cixd itself shells out to at runtime --
	 * grep-confirmed against daemon/src/pki.c's/daemon/src/pkg.c's own
	 * hardcoded absolute-path _BIN macros, the authoritative list, not
	 * docs/roadmap/ROADMAP.md's own partly-stale "dnsmasq" mention (dnsmasq
	 * runs inside operator-created containers; cixd itself never
	 * execve()s it). Previously entirely absent from this image --
	 * confirmed live: booting a fresh install and running "pki ca
	 * bootstrap" on the console failed outright ("CA genpkey failed")
	 * because /usr/bin/openssl simply didn't exist there. Each
	 * binary's real shared-library closure (from a real `ldd` on this
	 * build host) is staged the same way test_dns.c's own
	 * DNSMASQ_LIBS[] already does for a real, unmodified third-party
	 * binary with more dependencies than the bare ld.so+libc pair --
	 * regenerate via `ldd /usr/bin/<name>` if a newer build of one of
	 * these ever needs a different dependency set. Unlike firmware_dir
	 * below, failure here is fatal, not silently skipped: these are
	 * unconditionally required for cixd's own core PKI/pkg
	 * functionality, not an operator-opt-in extra -- silently
	 * tolerating their absence is exactly the bug this closes.
	 */
	if (ensure_dir_under(image_root, "usr") != 0)
		return 1;
	if (ensure_dir_under(image_root, "usr/bin") != 0)
		return 1;
	if (ensure_dir_under(image_root, "usr/sbin") != 0)
		return 1;
	if (ensure_dir_under(image_root, "bin") != 0)
		return 1;
	{
		static const struct {
			const char *host_path;   /* where this build host has it */
			const char *rootfs_path; /* relative to image_root, matching the _BIN macro exactly */
		} shelled_bins[] = {
			{ "/usr/bin/openssl", "usr/bin/openssl" },     /* PKI_OPENSSL_BIN, daemon/src/pki.c */
			{ "/usr/bin/curl", "usr/bin/curl" },           /* PKG_CURL_BIN, daemon/src/pkg.c */
			{ "/usr/bin/tar", "usr/bin/tar" },             /* PKG_TAR_BIN */
			/* sha256sum/cp/rm/gzip: NOT here -- staged from host_tools_dir
			 * (coreutils.recipe/gzip.recipe) when given, see below. */
			{ "/usr/bin/unsquashfs", "usr/bin/unsquashfs" }, /* PKG_UNSQUASHFS_BIN -- the
			                                                   * pkg_bootstrap_from_toolchain()
			                                                   * import path, no mount/loop-device
			                                                   * needed (this dev sandbox's own
			                                                   * documented "no /dev/loop* at all"
			                                                   * constraint; real hardware
			                                                   * shouldn't need one for this either). */
			/*
			 * bzip2/xz -- not a _BIN macro of their own anywhere in
			 * daemon/src/pkg.c; needed because GNU tar (PKG_TAR_BIN)
			 * itself has no compression libraries linked in at all
			 * (confirmed via `ldd /usr/bin/tar`: libacl/libselinux/
			 * libc/libpcre2 only) -- it shells out to a bare "gzip"/
			 * "bzip2"/"xz" resolved via $PATH for every compressed
			 * tarball, confirmed via strace on a real extraction
			 * (execve("bzip2", ["bzip2","-d"], ...)). Previously
			 * entirely absent from this image: every recipe using a
			 * .tar.gz/.tar.bz2/.tar.xz source (nearly all of them)
			 * failed extract_tarball() on a genuinely fresh/minimal
			 * install with the opaque "could not prepare the build
			 * container" bucket error -- confirmed live on
			 * 192.168.15.95's tcc.recipe (.tar.bz2) install, invisible
			 * until pkg.c's own build-container-prep diagnostics were
			 * wired into the log store. Never caught by this sandbox's
			 * own dev-loop testing because a locally-run cixd here
			 * always had this rich dev host's own real /usr/bin/{gzip,
			 * bzip2,xz} reachable via $PATH -- the same "this dev
			 * sandbox's own rich /usr made the gap easy to miss"
			 * pattern ADR-0023 already names for libtinfo above.
			 * (gzip itself moved to host_tools_dir staging below,
			 * gzip.recipe -- bzip2/xz have no recipe yet.)
			 */
			{ "/usr/bin/bzip2", "usr/bin/bzip2" },
			{ "/usr/bin/xz", "usr/bin/xz" },
			/*
			 * mkfs.ext4 -- DISKFORMAT_MKFS_EXT4_BIN, daemon/src/diskformat.c
			 * (multi-disk management Phase C). "/usr/sbin/mkfs.ext4" is
			 * itself a symlink to the real binary "mke2fs" on this build
			 * host; test_image_fixture_copy_file()'s plain open()/read()
			 * transparently follows it, landing the real mke2fs ELF
			 * content at this path (no symlink staged, no "mke2fs" binary
			 * needed alongside it -- the daemon only ever invokes the
			 * "mkfs.ext4" name).
			 */
			{ "/usr/sbin/mkfs.ext4", "usr/sbin/mkfs.ext4" },
			/*
			 * mkfs.btrfs -- DISKFORMAT_MKFS_BTRFS_BIN, same file. The
			 * API has accepted fs_type: "btrfs" since multi-disk
			 * management shipped, and this binary was never staged, so
			 * on a real installed host that request could only ever
			 * fail at execve() -- reported by an operator who asked for
			 * btrfs and got ext4. (The ext4 they got was a separate
			 * dashboard bug; this is why btrfs could not have worked
			 * even once that was fixed.) btrfs is not a hypothetical
			 * here either: ADR-0103 uses btrfs qgroups for volume
			 * quotas, so the platform already treats it as a supported
			 * filesystem.
			 */
			{ "/usr/sbin/mkfs.btrfs", "usr/sbin/mkfs.btrfs" },
			/*
			 * sfdisk -- DISKPART_SFDISK_BIN, daemon/src/diskpart.c
			 * (partition-level disk management, ADR-0158). This was
			 * missing from the moment that feature shipped, which is
			 * exactly why it was never verified on real hardware: the
			 * code was correct, the binary simply was not there, so the
			 * very first real POST /disks/{name}/partition-table on an
			 * installed host could only ever fail. Easy to miss because
			 * cix-install.c drives the same sfdisk at install time and
			 * works fine -- the installer runs from the ISO, which has a
			 * full environment; the daemon on the installed host does
			 * not. Same "this build sandbox's own rich /usr made the gap
			 * invisible" pattern ADR-0023 names for libtinfo.
			 */
			{ "/usr/sbin/sfdisk", "usr/sbin/sfdisk" },
			/*
			 * resize2fs + e2fsck -- DISKPART_RESIZE2FS_BIN and
			 * DISKPART_E2FSCK_BIN, daemon/src/diskpart.c (issue #94).
			 * Growing a partition is two operations, not one: the table
			 * entry grows, and then the filesystem inside it has to be
			 * grown to match, or the extra space is simply invisible.
			 * resize2fs requires a clean filesystem, which is what
			 * e2fsck is for. Both need only libraries mkfs.ext4 above
			 * already pulls in.
			 */
			{ "/usr/sbin/resize2fs", "usr/sbin/resize2fs" },
			{ "/usr/sbin/e2fsck", "usr/sbin/e2fsck" },
		};
		/*
		 * Library NAMES, not paths (#184). Every entry used to spell
		 * out /lib/x86_64-linux-gnu/ for itself, forty times, which
		 * made a directory that is an inherited Debian convention into
		 * something this file asserted forty times over -- and made
		 * moving any package a forty-line edit here.
		 *
		 * Where each one is found is already flexible: add_lib()
		 * searches CIX_LIB_DIRS_SEARCH by name (#224). Where each one
		 * LANDS is CIX_LIB_DIR_RUNTIME, resolved once at the call site
		 * below, so this list says what the control plane needs and
		 * the layout says where it goes.
		 */
		static const char *const shelled_bin_libs[] = {
			/* openssl */
			"libssl.so.3",
			"libcrypto.so.3",
			/* curl */
			"libcurl.so.4",
			"libz.so.1",
			"libnghttp2.so.14",
			"libidn2.so.0",
			"librtmp.so.1",
			"libssh2.so.1",
			"libpsl.so.5",
			"libgssapi_krb5.so.2",
			"libldap-2.5.so.0",
			"liblber-2.5.so.0",
			"libzstd.so.1",
			"libbrotlidec.so.1",
			"libunistring.so.2",
			"libgnutls.so.30",
			"libhogweed.so.6",
			"libnettle.so.8",
			"libgmp.so.10",
			"libkrb5.so.3",
			"libk5crypto.so.3",
			"libcom_err.so.2",
			"libkrb5support.so.0",
			"libsasl2.so.2",
			"libbrotlicommon.so.1",
			"libp11-kit.so.0",
			"libtasn1.so.6",
			"libkeyutils.so.1",
			"libresolv.so.2",
			"libffi.so.8",
			/* tar + cp */
			"libacl.so.1",
			"libselinux.so.1",
			"libpcre2-8.so.0",
			"libattr.so.1",
			/* unsquashfs -- libz.so.1/libzstd.so.1 already listed above (curl) */
			"libpthread.so.0",
			"libm.so.6",
			"liblzma.so.5",
			"liblzo2.so.2",
			"liblz4.so.1",
			/*
			 * libpthread's own pthread_exit()/pthread_cancel() lazily
			 * dlopen() this for stack-unwinding support -- never a
			 * DT_NEEDED entry (confirmed via readelf -d: absent from
			 * every ldd-based closure this file's own comments already
			 * derived), so it was invisible to every prior "regenerate
			 * via ldd" pass. Confirmed missing the hard way: mksquashfs
			 * (host_tools_dir's own copy, ADR-0084) starts and runs
			 * fine, then aborts at its own normal pthread_exit() with
			 * "libgcc_s.so.1 must be installed for pthread_exit to
			 * work" -- reproduced via strace against a genuinely
			 * non-merged-usr chroot during the same investigation that
			 * found ADR-0085's ld-linux source-path bug. Needed by any
			 * shelled-out binary linking libpthread that actually exits
			 * a thread normally, not just unsquashfs/mksquashfs.
			 */
			"libgcc_s.so.1",
			/* bzip2 -- gzip needs only libc (already staged); xz needs
			 * only liblzma.so.5 (already staged above, for unsquashfs) */
			"libbz2.so.1.0",
			/* mkfs.ext4 (mke2fs) -- libcom_err.so.2 already listed above
			 * (curl/krb5) */
			"libext2fs.so.2",
			"libblkid.so.1",
			"libuuid.so.1",
			"libe2p.so.2",
			/* sfdisk -- libtinfo.so.6/libuuid.so.1/libblkid.so.1 are
			 * already staged above (curl/mke2fs), so only these three
			 * are new. libreadline is pulled in by libfdisk's own
			 * interactive-prompt support, which nothing here uses, but
			 * the dynamic linker resolves it at load time regardless. */
			"libfdisk.so.1",
			"libsmartcols.so.1",
			"libreadline.so.8",
		};
		size_t i;

		for (i = 0; i < sizeof(shelled_bins) / sizeof(shelled_bins[0]); i++) {
			char dst[PATH_MAX];

			if (snprintf(dst, sizeof(dst), "%s/%s", image_root, shelled_bins[i].rootfs_path) >=
			    (int)sizeof(dst)) {
				fprintf(stderr, "path too long: %s/%s\n", image_root, shelled_bins[i].rootfs_path);
				return 1;
			}
			if (test_image_fixture_copy_file(shelled_bins[i].host_path, dst) != 0)
				return 1;
		}
		{
			/*
			 * cp/rm/sha256sum/gzip -- the 4 shelled-out tools this
			 * project already has a real from-source recipe for
			 * (coreutils.recipe, gzip.recipe). host_tools_dir, when
			 * given, is that recipe's own installed image rootfs (a
			 * real `pkg install --image=<name>` result); each binary
			 * is sourced from THERE instead of this dev build host.
			 * "" (host_tools_dir unset) falls back to the dev-host
			 * path, matching every call site that predates this
			 * argument (test binaries, the server-side ADR-0057
			 * bootroot-assembly spawn) until they're updated to pass
			 * a real one.
			 */
			static const struct {
				const char *dev_host_path; /* fallback: this build host's own copy */
				const char *host_tools_rel; /* relative to host_tools_dir */
				const char *rootfs_path;    /* relative to image_root, matching the _BIN macro */
			} host_tool_bins[] = {
				{ "/usr/bin/sha256sum", "usr/bin/sha256sum", "usr/bin/sha256sum" }, /* PKG_SHA256SUM_BIN */
				{ "/usr/bin/cp", "usr/bin/cp", "usr/bin/cp" },                       /* PKG_CP_BIN */
				/*
				 * PKG_RM_BIN is "/bin/rm" -- rm's own rootfs_path below
				 * is "bin/rm", not "usr/bin/rm" like its three siblings
				 * here, so its dev_host_path fallback has to match: "/bin/rm",
				 * not "/usr/bin/rm" (which doesn't exist at all on this
				 * project's own non-merged-usr roots -- confirmed via
				 * strace against a genuinely non-merged-usr chroot,
				 * the same real-root-only gap ADR-0085 already found
				 * and fixed for test_image_fixture_build()'s own
				 * ld-linux/libc.so.6 source reads). "/bin/rm" resolves
				 * correctly on the dev sandbox too (bin -> usr/bin
				 * there), same reasoning as ADR-0085.
				 */
				{ "/bin/rm", "usr/bin/rm", "bin/rm" }, /* coreutils.recipe itself installs rm under usr/bin/rm */
				{ "/usr/bin/gzip", "usr/bin/gzip", "usr/bin/gzip" },
				/*
				 * btrfs -- DISKPART_BTRFS_BIN, daemon/src/diskpart.c
				 * (issue #163). The other half of mkfs.btrfs's story:
				 * the platform could CREATE a btrfs filesystem and do
				 * nothing whatsoever to maintain one, so a grown btrfs
				 * partition reported success and gained no usable
				 * space.
				 *
				 * HERE rather than in shelled_bins[] above, and that
				 * distinction cost a build. shelled_bins[] sources from
				 * the build host's own filesystem, which on a Cix host
				 * is the running control-plane root -- and that root
				 * only contains what a PREVIOUS mkbootroot staged into
				 * it. A binary that has never been staged is therefore
				 * not there to stage, and the assembly dies with
				 * "/usr/sbin/btrfs: No such file or directory". Anything
				 * genuinely new has to come from cix-hosttools, which
				 * is what host_tools_dir is. mkfs.btrfs gets away with
				 * shelled_bins[] only because it was first staged back
				 * when mkbootroot ran on a dev host with a full /usr.
				 *
				 * The dev fallback is /usr/bin/btrfs, not /usr/sbin: a
				 * merged-usr dev host puts it there, the same mismatch
				 * between fallback path and rootfs path that rm above
				 * documents.
				 */
				{ "/usr/bin/btrfs", "usr/sbin/btrfs", "usr/sbin/btrfs" },
				/*
				 * mksquashfs (issue #146). ADR-0078 called the
				 * host_tools_dir == "" path a safe fallback -- "a box
				 * that never built this image keeps today's
				 * dev-host-sourced behavior, never a hard failure."
				 * True on a dev machine, false on an installed host,
				 * where /usr/bin/mksquashfs does not exist and never
				 * has. So a freshly installed host could not assemble
				 * a control-plane image, i.e. could not deploy an
				 * update to itself.
				 *
				 * It belongs HERE rather than in shelled_bins[] above,
				 * and that distinction is the whole fix: shelled_bins
				 * are copied from the bare host unconditionally, so
				 * staging it there only works on a machine that
				 * already has it -- precisely the machines that do not
				 * need the fix. Sourced from cix-hosttools when built
				 * (squashfs-tools.recipe installs exactly this path),
				 * falling back to the dev host otherwise.
				 *
				 * Found the hard way: staged from shelled_bins[] first,
				 * which failed on the real box with a bare
				 * "/usr/bin/mksquashfs: No such file or directory" --
				 * the fix reproducing the very bug it was written for.
				 */
				{ "/usr/bin/mksquashfs", "usr/bin/mksquashfs", "usr/bin/mksquashfs" },
			};

			for (i = 0; i < sizeof(host_tool_bins) / sizeof(host_tool_bins[0]); i++) {
				char src[PATH_MAX];
				char dst[PATH_MAX];
				const char *use_src;

				if (host_tools_dir[0] != '\0') {
					if (snprintf(src, sizeof(src), "%s/%s", host_tools_dir,
					             host_tool_bins[i].host_tools_rel) >= (int)sizeof(src)) {
						fprintf(stderr, "path too long: %s/%s\n", host_tools_dir,
						        host_tool_bins[i].host_tools_rel);
						return 1;
					}
					use_src = src;
				} else {
					use_src = host_tool_bins[i].dev_host_path;
				}
				if (snprintf(dst, sizeof(dst), "%s/%s", image_root, host_tool_bins[i].rootfs_path) >=
				    (int)sizeof(dst)) {
					fprintf(stderr, "path too long: %s/%s\n", image_root, host_tool_bins[i].rootfs_path);
					return 1;
				}
				if (test_image_fixture_copy_file(use_src, dst) != 0)
					return 1;
			}
		}
		/*
		 * mkfs.btrfs -- DISKFORMAT_MKFS_BTRFS_BIN, daemon/src/diskformat.c
		 * (ADR-0104, task #732). Deliberately NOT added to host_tool_bins[]
		 * above: every entry there is unconditionally required (a dev-host
		 * fallback always exists), but this dev sandbox has no mkfs.btrfs
		 * of its own at all (confirmed directly -- unlike mke2fs, Debian
		 * doesn't ship btrfs-progs by default) and no real box may have
		 * built btrfs-progs.recipe onto its cix-hosttools image yet
		 * either. Staged tolerantly instead, matching firmware_dir/
		 * modules_dir/kmod_bin_dir's own "absent is a normal, silently-
		 * skipped state, not a build failure" precedent: only attempted
		 * when host_tools_dir is given AND that tree's own copy actually
		 * exists (stat()-gated) -- an older cix-hosttools image built
		 * before btrfs-progs.recipe existed is not an error here, just a
		 * box that can't format a disk btrfs yet (POST /v1/disks/{name}/
		 * format with fs_type=btrfs fails loud with ENOENT at exec time
		 * on such a box, not silently). Its runtime library closure
		 * (libuuid.so.1/libblkid.so.1/libz.so.1) needs no new entries in
		 * shelled_bin_libs[] below -- confirmed via a real local `ldd` on
		 * a real local mkfs.btrfs build: identical to mke2fs's own
		 * closure (already staged there) plus libz.so.1 (already staged
		 * there for curl/unsquashfs).
		 */
		if (host_tools_dir[0] != '\0') {
			char src[PATH_MAX];
			struct stat st;

			if (snprintf(src, sizeof(src), "%s/usr/sbin/mkfs.btrfs", host_tools_dir) >=
			    (int)sizeof(src)) {
				fprintf(stderr, "path too long: %s/usr/sbin/mkfs.btrfs\n", host_tools_dir);
				return 1;
			}
			if (stat(src, &st) == 0) {
				char dst[PATH_MAX];

				snprintf(dst, sizeof(dst), "%s/usr/sbin/mkfs.btrfs", image_root);
				if (test_image_fixture_copy_file(src, dst) != 0)
					return 1;
			}
		}
		for (i = 0; i < sizeof(shelled_bin_libs) / sizeof(shelled_bin_libs[0]); i++) {
			char lib_path[PATH_MAX];

			if (snprintf(lib_path, sizeof(lib_path), "/%s/%s", CIX_LIB_DIR_RUNTIME,
			             shelled_bin_libs[i]) >= (int)sizeof(lib_path)) {
				fprintf(stderr, "library path too long: /%s/%s\n", CIX_LIB_DIR_RUNTIME,
				        shelled_bin_libs[i]);
				return 1;
			}
			if (test_image_fixture_add_lib(image_root, lib_path) != 0)
				return 1;
		}

		/*
		 * The platform's own libraries go on LAST, after every
		 * host-sourced library above, because whichever is copied last
		 * is what the root actually carries.
		 *
		 * This block used to sit right after cixd was staged, near the
		 * top of this function -- and shelled_bin_libs[] above then
		 * overwrote libm.so.6, libpthread.so.0 and libresolv.so.2 with
		 * the build host's copies. Those are glibc's own objects, so
		 * the root ended up with a 2.44 libc.so.6 beside a 2.36
		 * libpthread, which is the mismatched-halves failure again:
		 * init dies with exit 127 and the machine panics. It took two
		 * outages to find, because assembly reported success every
		 * time -- copying files is not the same as producing an image
		 * that runs.
		 */
		/*
		 * Replace the C library the fixture just staged with the one this
		 * platform BUILT, taken from cix-hosttools (#181).
		 *
		 * test_image_fixture_build() reads libc.so.6 and ld-linux off the
		 * machine it runs on. For a server-side assembly that machine IS
		 * the control-plane root, so the root copies its own C library
		 * forward, forever -- which is how a glibc that came from Debian is
		 * still the one every Cix binary links. The cycle cannot be broken
		 * from inside it.
		 *
		 * EVERY shared library in the host-tools image is copied, not a
		 * chosen few, and that is the whole design. A first attempt
		 * replaced libc.so.6, libm.so.6 and the loader by name and left
		 * libpthread.so.0 and libresolv.so.2 -- also glibc's, also
		 * GLIBC_PRIVATE-coupled to libc -- sitting at the old version. The
		 * result booted nothing: cixd links OpenSSL, OpenSSL pulls
		 * libpthread, and a 2.36 libpthread against a 2.44 libc is the same
		 * mismatched-halves failure as a stale loader. "libc and its loader
		 * ship together" was understated; it is the entire glibc, 24 shared
		 * objects, and any hand-written subset of it is a partial
		 * replacement waiting to happen.
		 *
		 * So the rule is structural rather than enumerated: whatever
		 * libraries that image has, the root takes. It also self-maintains
		 * -- as more of this platform's libraries are built rather than
		 * borrowed, they land in cix-hosttools and the root picks them up
		 * with no list to update here.
		 *
		 * ORDERING, learned by taking the box down twice: the root must
		 * gain the new libc BEFORE anything built against it is deployed. A
		 * binary needs the glibc it was built against, or newer, never
		 * older.
		 */
		if (host_tools_dir != NULL && host_tools_dir[0] != '\0') {
			static const char *const lib_dirs[] = CIX_LIB_DIRS_PLATFORM;
			int staged_libs = 0;
			size_t d;

			for (d = 0; d < sizeof(lib_dirs) / sizeof(lib_dirs[0]); d++) {
				char src_dir[PATH_MAX];
				DIR *dh;
				struct dirent *de;

				snprintf(src_dir, sizeof(src_dir), "%s/%s", host_tools_dir, lib_dirs[d]);
				dh = opendir(src_dir);
				if (dh == NULL)
					continue;
				/* Only for a directory the host-tools image actually
				 * has -- creating the rest would leave empty
				 * directories in the root describing a layout it does
				 * not use. */
				if (ensure_dir_path_under(image_root, lib_dirs[d]) != 0) {
					closedir(dh);
					return 1;
				}
				while ((de = readdir(dh)) != NULL) {
					char src[PATH_MAX], dst[PATH_MAX];
					struct stat fst;

					if (strstr(de->d_name, ".so") == NULL)
						continue;
					snprintf(src, sizeof(src), "%s/%s", src_dir, de->d_name);
					if (stat(src, &fst) != 0 || !S_ISREG(fst.st_mode))
						continue;
					snprintf(dst, sizeof(dst), "%s/%s/%s", image_root, lib_dirs[d],
					         de->d_name);
					if (test_image_fixture_copy_file(src, dst) != 0) {
						fprintf(stderr,
						        "staging the platform's own libraries: %s/%s failed -- "
						        "a partial copy is worse than none, since glibc's "
						        "objects share a private, version-locked interface\n",
						        lib_dirs[d], de->d_name);
						closedir(dh);
						return 1;
					}
					staged_libs++;
				}
				closedir(dh);
			}
			if (staged_libs > 0)
				fprintf(stderr, "staged %d of this platform's own shared libraries from %s\n",
				        staged_libs, host_tools_dir);
			else
				fprintf(stderr,
				        "note: %s carries no shared libraries -- the control-plane root "
				        "keeps the build host's C library\n",
				        host_tools_dir);
		}

		/*
		 * openssl's own default config path -- found by an actual "pki
		 * ca bootstrap" failing, not guessed at: "req -x509 failed:
		 * Can't open /usr/lib/ssl/openssl.cnf". On this build host that
		 * path is itself a symlink chain into /etc/ssl/openssl.cnf --
		 * test_image_fixture_copy_file()'s plain open()/read()/write()
		 * transparently follows symlinks, so this lands the real config
		 * content as one ordinary file at the expected path, no /etc/ssl
		 * symlink chain needed on the target at all.
		 */
		if (ensure_dir_under(image_root, "usr/lib") != 0)
			return 1;
		if (ensure_dir_under(image_root, "usr/lib/ssl") != 0)
			return 1;
		{
			char dst[PATH_MAX];

			snprintf(dst, sizeof(dst), "%s/usr/lib/ssl/openssl.cnf", image_root);
			if (test_image_fixture_copy_file("/usr/lib/ssl/openssl.cnf", dst) != 0)
				return 1;
		}

		/*
		 * ADR-0096's own follow-on: curl.recipe's ./configure auto-
		 * detects a CA bundle path at build time from whatever machine
		 * builds it (Debian convention: /etc/ssl/certs/ca-certificates.crt)
		 * and compiles that path in as its default -- but nothing in this
		 * file has ever actually staged a real bundle there, so every
		 * host-side HTTPS pkg_source fetch (PKG_CURL_BIN) has had no way
		 * to verify a TLS cert at all. Found live via ADR-0096's own new
		 * diagnostic capture: "curl: (77) error setting certificate file:
		 * /etc/ssl/certs/ca-certificates.crt" on a real cix hostbuild
		 * fetch, previously invisible behind a bare "curl exit status 1".
		 * Same fix shape as openssl.cnf just above: copy the real bundle
		 * from wherever this tool itself runs -- test_image_fixture_copy_file()
		 * follows symlinks transparently, so a distro's usual
		 * ca-certificates -> a real file chain lands as one plain file.
		 */
		if (ensure_dir_under(image_root, "etc") != 0)
			return 1;
		if (ensure_dir_under(image_root, "etc/ssl") != 0)
			return 1;
		if (ensure_dir_under(image_root, "etc/ssl/certs") != 0)
			return 1;
		{
			char dst[PATH_MAX];

			snprintf(dst, sizeof(dst), "%s/etc/ssl/certs/ca-certificates.crt", image_root);
			if (test_image_fixture_copy_file("/etc/ssl/certs/ca-certificates.crt", dst) != 0)
				return 1;
		}
	}

	/*
	 * An empty /etc/resolv.conf placeholder (ADR-0076) -- this control-
	 * plane squashfs has no /etc directory at all otherwise (confirmed:
	 * nothing else in this whole file ever creates one). A real
	 * --init-mode boot bind-mounts <g_base_dir>/resolv.conf onto this
	 * exact path; mount(MS_BIND) requires the target to already exist,
	 * so this empty file has to be staged here, not created lazily at
	 * boot. Content is irrelevant (the bind mount replaces it entirely)
	 * -- an empty file, not a symlink or directory, matching what a
	 * real resolv.conf actually is.
	 */
	if (ensure_dir_under(image_root, "etc") != 0)
		return 1;
	{
		char dst[PATH_MAX];
		FILE *f;

		snprintf(dst, sizeof(dst), "%s/etc/resolv.conf", image_root);
		f = fopen(dst, "w");
		if (f == NULL) {
			perror(dst);
			return 1;
		}
		fclose(f);
	}

	/* cixd's DEFAULT_WEB_ROOT is "web", resolved relative to its own
	 * CWD -- PID 1 never chdir()s anywhere, so that's this squashfs
	 * image's own root, i.e. exactly image_root/web. */
	if (ensure_dir_under(image_root, "web") != 0)
		return 1;
	{
		char web_dst[PATH_MAX];

		if (snprintf(web_dst, sizeof(web_dst), "%s/web", image_root) >= (int)sizeof(web_dst)) {
			fprintf(stderr, "path too long: %s/web\n", image_root);
			return 1;
		}
		if (test_image_fixture_copy_dir_files(web_dir, web_dst) != 0)
			return 1;
	}

	/* Empty string means "skip" -- every test call site passes this,
	 * since a QEMU/CI boot test needs no real GPU firmware and this
	 * keeps the test suite's own fast, no-network posture completely
	 * unaffected. A non-empty firmware_dir is a real, explicit operator
	 * request, so unlike test_image_fixture_add_lib()'s own tolerant-
	 * if-missing precedent, test_image_fixture_copy_dir_files() failing here is fatal, not
	 * silently skipped -- an operator who asked for firmware staging
	 * and didn't get it should find out now, not at first GPU use on
	 * the installed system.
	 */
	if (firmware_dir[0] != '\0') {
		char fw_dst[PATH_MAX];

		if (ensure_dir_under(image_root, "lib") != 0)
			return 1;
		if (ensure_dir_under(image_root, "lib/firmware") != 0)
			return 1;
		if (ensure_dir_under(image_root, "lib/firmware/amdgpu") != 0)
			return 1;
		if (snprintf(fw_dst, sizeof(fw_dst), "%s/lib/firmware/amdgpu", image_root) >=
		    (int)sizeof(fw_dst)) {
			fprintf(stderr, "path too long: %s/lib/firmware/amdgpu\n", image_root);
			return 1;
		}
		if (test_image_fixture_copy_dir_files(firmware_dir, fw_dst) != 0)
			return 1;
	}

	/* Same "empty means skip, non-empty is a real explicit request and
	 * fatal if it fails" posture as firmware_dir above -- an operator
	 * who asked for module support and didn't get it should find out
	 * now, not at first real modprobe on the installed system.
	 * test_image_fixture_copy_dir_recursive() (real `cp -a`) is used
	 * here, not the flat copy_dir_files() above, since modules_dir
	 * nests by kernel/drivers/... -- its own contract requires the
	 * destination to not already exist, so unlike firmware_dir's own
	 * lib/firmware/amdgpu (pre-created via ensure_dir_under so a
	 * second run can add more files into the same directory), only
	 * "lib" itself is pre-created here; "lib/modules" is created BY
	 * the copy, fresh, every run (mksquashfs's own -noappend already
	 * means every run starts from a clean image_root regardless). */
	if (modules_dir[0] != '\0') {
		char modules_dst[PATH_MAX];

		if (ensure_dir_under(image_root, "lib") != 0)
			return 1;
		if (snprintf(modules_dst, sizeof(modules_dst), "%s/lib/modules", image_root) >=
		    (int)sizeof(modules_dst)) {
			fprintf(stderr, "path too long: %s/lib/modules\n", image_root);
			return 1;
		}
		if (test_image_fixture_copy_dir_recursive(modules_dir, modules_dst) != 0)
			return 1;
	}

	/* modprobe/depmod/insmod/lsmod/modinfo/rmmod -- all symlinks to one
	 * real "kmod" binary (recipes/package/kmod). copy_dir_files()'s
	 * own stat() (not lstat()) dereferences each symlink and copies the
	 * real bytes it points at -- a real, working "kmod" binary landing
	 * at each of the 6 tool names instead of a preserved symlink, a few
	 * extra hundred KB on disk in exchange for needing no new symlink-
	 * aware copy primitive. modprobe invoked under any of these names
	 * still works correctly either way -- kmod's own tools dispatch on
	 * argv[0], which is identical regardless of whether that path was
	 * reached via a symlink or a real file. usr/bin already exists
	 * (cixd/cixctl staged there above), so the flat copy merges
	 * into it rather than needing test_image_fixture_copy_dir_recursive()'s
	 * own "destination must not exist" contract.
	 */
	if (kmod_bin_dir[0] != '\0') {
		char kmod_dst[PATH_MAX];

		if (snprintf(kmod_dst, sizeof(kmod_dst), "%s/usr/bin", image_root) >=
		    (int)sizeof(kmod_dst)) {
			fprintf(stderr, "path too long: %s/usr/bin\n", image_root);
			return 1;
		}
		if (test_image_fixture_copy_dir_files(kmod_bin_dir, kmod_dst) != 0)
			return 1;
	}

	if (ensure_dir_under(image_root, "proc") != 0)
		return 1;
	if (ensure_dir_under(image_root, "sys") != 0)
		return 1;
	/* Kernel's own devtmpfs auto-mount (CONFIG_DEVTMPFS_MOUNT) needs an
	 * existing /dev to mount onto -- confirmed empirically (a real QEMU
	 * boot logged "devtmpfs: error mounting -2" without this). */
	if (ensure_dir_under(image_root, "dev") != 0)
		return 1;
	/* Phase 11 part 2: cixd --init-mode mounts the ESP here to reach
	 * the loader entry confirm_boot() renames once a boot proves
	 * healthy (daemon/src/main.c). */
	if (ensure_dir_under(image_root, "boot") != 0)
		return 1;
	/* Mountpoint parent for attached disks (DISKS_MOUNT_DIR). Shipped
	 * in the image rather than created at runtime so the root slot
	 * never has to be written to for it -- cixd mounts a tmpfs here at
	 * boot and every assigned-role disk mounts beneath that, which is
	 * what keeps them out of the data directory's own filesystem. */
	if (ensure_dir_under(image_root, "mnt") != 0)
		return 1;
	if (ensure_dir_under(image_root, "mnt/cix") != 0)
		return 1;
	/* Phase 11 part 3: cixd --init-mode mounts the config partition
	 * here for its static-IP net.conf (daemon/src/main.c's
	 * apply_static_ip()) -- absent (and harmlessly so) on parts 1/2's
	 * own throwaway disks, which have no partition 4 at all. */
	if (ensure_dir_under(image_root, "config") != 0)
		return 1;
	if (ensure_dir_under(image_root, "var") != 0)
		return 1;
	if (ensure_dir_under(image_root, "var/lib") != 0)
		return 1;
	if (ensure_dir_under(image_root, "var/lib/cix") != 0)
		return 1;

	/*
	 * mksquashfs itself is a build-time-only tool (assembles image_root
	 * into out_path) -- nothing inside the assembled control-plane root
	 * ever shells out to it (unlike unsquashfs/openssl/curl/tar above,
	 * which cixd itself invokes at runtime and which are therefore
	 * staged INTO image_root via shelled_bins[]). The hardcoded
	 * "/usr/bin/mksquashfs" this used to exec unconditionally only ever
	 * existed on this dev sandbox's own rich /usr -- when this same
	 * binary runs for real, server-side, via
	 * spawn_cix_bootroot_assembly() (daemon/src/main.c) on an
	 * installed host whose root IS a prior-generation assembled
	 * control-plane squashfs, "/usr/bin/mksquashfs" simply doesn't
	 * exist there (confirmed: never one of the shelled_bins[] staged
	 * above), and execve() fails outright. host_tools_dir (when built --
	 * squashfs-tools.recipe, ADR-0078) already has a real mksquashfs at
	 * a real, ordinary filesystem path outside the transient assembled
	 * root entirely (BASE_DIR/images/.../rootfs, not part of what gets
	 * squashed or what's mounted as /), so it's used directly, no
	 * staging-into-image_root needed for a tool nothing else consumes.
	 * "" (host_tools_dir never built) falls back to this dev sandbox's
	 * own copy, matching every other host_tools_dir-aware call site's
	 * tolerant-default shape above.
	 */
	{
		char mksquashfs_bin[PATH_MAX];
		const char *use_mksquashfs;

		if (host_tools_dir[0] != '\0') {
			if (snprintf(mksquashfs_bin, sizeof(mksquashfs_bin), "%s/usr/bin/mksquashfs",
			             host_tools_dir) >= (int)sizeof(mksquashfs_bin)) {
				fprintf(stderr, "path too long: %s/usr/bin/mksquashfs\n", host_tools_dir);
				return 1;
			}
			use_mksquashfs = mksquashfs_bin;
		} else {
			use_mksquashfs = MKSQUASHFS_BIN;
		}
		if (verify_platform_libs_intact(image_root, host_tools_dir) != 0)
			return 1;
		if (run_mksquashfs(use_mksquashfs, image_root, out_path, host_tools_dir) != 0)
			return 1;
	}

	printf("wrote %s\n", out_path);
	return 0;
}
