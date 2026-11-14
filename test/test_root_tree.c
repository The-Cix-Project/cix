/*
 * test_root_tree_build(): a control-plane root TREE assembled from a
 * source root, for the tests that boot one.
 *
 * Moved here whole from image/src/mkbootroot.c by ADR-0371 phase 3a.
 * Production assembles from the cix-root image's tree (mkbootroot's
 * three-argument form), which already holds every program, library and
 * file this staging used to collect. A test running in a build
 * container has no such image, but its composed root ("/") carries the
 * same Cix packages as declared tools, so it stages a tree from that
 * here and hands it to the same three-argument path production uses.
 * In phase 3a's release mkbootroot also linked this file, for the
 * ten-argument form the previous daemon called; phase 3b removed that.
 */
#include "test_root_tree.h"

#include "btrfs.h"
#include "controlplane_programs.h"
#include "libdirs.h"
#include "persist.h"
#include "test_image_fixture.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* cix_tree_copy() with both paths named on failure (mkbootroot.c has
 * the same wrapper for its own copy of the tree). */
static int stage_tree(const char *src, const char *dst)
{
	if (cix_tree_copy(src, dst) != 0) {
		fprintf(stderr, "copying %s to %s: %s\n", src, dst, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * How many regular files a staged tree actually holds.
 *
 * Assembly reporting success is not evidence the image is right -- this
 * platform has booted a machine into a kernel panic off a root that
 * assembled cleanly, twice. Firmware staging was entirely silent: the
 * only success output was "wrote <path>", so the first evidence that a
 * blob had landed was a device working, or not, after a reboot. On a
 * shell-less host that is a very expensive place to learn it.
 *
 * A count rather than a bare "staged firmware" line, because the
 * failure worth catching is a firmware root that exists and is EMPTY --
 * a cix-firmware image created but never installed into, say, which
 * copies nothing, succeeds, and is indistinguishable from a correct run
 * in any output that does not count.
 *
 * Errors are not fatal and are not reported: this walks a tree that has
 * just been written successfully, purely to describe it. A checkpoint
 * that could fail the build it is only observing would be worse than no
 * checkpoint.
 */
static long count_files_recursive(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	long n = 0;

	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL) {
		char path[PATH_MAX];
		struct stat st;

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path))
			continue;
		if (lstat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			long sub = count_files_recursive(path);

			if (sub > 0)
				n += sub;
		} else {
			n++;
		}
	}
	closedir(d);
	return n;
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

/*
 * A staged symlink must resolve to something inside the root.
 *
 * Recreating links instead of copying their targets is the correct
 * shape -- libfoo.so.5 has always been a link to libfoo.so.5.8.3, and
 * materialising both put 30.93 MiB of duplicate content into a root
 * that has 55.52 MiB of unique bytes. But a link is only better than a
 * copy while it resolves: a dangling one is a library the loader
 * cannot find, and a control-plane root whose loader cannot find libc
 * is a box that panics at boot with exit 127.
 *
 * So every link this staging creates is checked, here, against the
 * assembled root rather than against the source it came from.
 */
static int symlink_resolves_in_root(const char *image_root, const char *lib_dir,
                                    const char *target)
{
	char path[PATH_MAX];
	struct stat st;

	if (target[0] == '/') {
		/* Absolute inside the image: resolves against the root, which
		 * is what it will mean once this tree IS /. */
		if (snprintf(path, sizeof(path), "%s%s", image_root, target) >= (int)sizeof(path))
			return 0;
	} else {
		if (snprintf(path, sizeof(path), "%s/%s/%s", image_root, lib_dir, target) >=
		    (int)sizeof(path))
			return 0;
	}
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Reproduce a symlink at dst rather than copying what it points at. */
static int stage_symlink(const char *src, const char *dst)
{
	char target[PATH_MAX];
	ssize_t n;

	n = readlink(src, target, sizeof(target) - 1);
	if (n < 0) {
		perror(src);
		return -1;
	}
	target[n] = '\0';
	if (unlink(dst) != 0 && errno != ENOENT) {
		perror(dst);
		return -1;
	}
	if (symlink(target, dst) != 0) {
		perror(dst);
		return -1;
	}
	return 0;
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
			if (lstat(src, &fst) != 0)
				continue;
			snprintf(dst, sizeof(dst), "%s/%s/%s", image_root, lib_dirs[d], de->d_name);
			if (S_ISLNK(fst.st_mode)) {
				char want[PATH_MAX], got[PATH_MAX];
				ssize_t wn, gn;

				wn = readlink(src, want, sizeof(want) - 1);
				gn = readlink(dst, got, sizeof(got) - 1);
				if (wn < 0 || gn < 0) {
					fprintf(stderr,
					        "%s/%s is a symlink in the host-tools image but not in the "
					        "assembled root -- refusing to write a root whose library "
					        "layout does not match what it was built from\n",
					        lib_dirs[d], de->d_name);
					closedir(dh);
					return -1;
				}
				want[wn] = '\0';
				got[gn] = '\0';
				if (strcmp(want, got) != 0) {
					fprintf(stderr, "%s/%s points at %s in the root, %s in the source\n",
					        lib_dirs[d], de->d_name, got, want);
					closedir(dh);
					return -1;
				}
				/*
				 * The one that actually matters: a link is only
				 * better than a copy while it resolves. Checked
				 * against the ASSEMBLED root, because that is the
				 * tree that will be mounted as /.
				 */
				if (!symlink_resolves_in_root(image_root, lib_dirs[d], got)) {
					fprintf(stderr,
					        "%s/%s points at %s, which is not in the assembled root -- "
					        "a dangling library link is a loader that cannot find libc, "
					        "which is a box that panics at boot\n",
					        lib_dirs[d], de->d_name, got);
					closedir(dh);
					return -1;
				}
				checked++;
				continue;
			}
			if (!S_ISREG(fst.st_mode))
				continue;
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

/*
 * cix#350: the root's PROGRAM SET IS DERIVED from
 * include/controlplane_programs.h -- the same list the seal already
 * gates on -- and every entry comes from the cix-hosttools image
 * (ADR-0078), at the identical absolute path.
 *
 * This replaces two hand-maintained tables and two one-off blocks
 * (shelled_bins[], host_tool_bins[], a tolerant mkfs.btrfs stage and a
 * required cbs stage), which between them spelled out nine programs a
 * third time, in a third shape, beside the list that declares them and
 * the loop that verifies them. Every drift those tables had was in one
 * direction -- declared and not staged, which is a feature that can
 * only fail at execve() on a host that has already booted.
 *
 * THERE IS NO BUILD-HOST FALLBACK, and removing it is the point rather
 * than a simplification. Each of those tables carried one, and on an
 * installed host the build host IS the control-plane root -- so every
 * assembly copied the previous root's copy forward and whatever a dev
 * machine first put there stayed, for as long as the box lived
 * (cix#589). Measured on 192.168.15.95, 2026-10-10 from the daemon's
 * own per-package file lists: all nine paths below are present in the
 * cix-hosttools image, supplied by squashfs-tools, cbs, util-linux,
 * e2fsprogs and btrfs-progs. So the fallback was never reaching
 * anything the image could not supply; it was only reaching a worse
 * copy of it.
 *
 * An absence is therefore fatal and says which install fixes it, which
 * is the one thing a fallback genuinely bought: a box whose
 * cix-hosttools predates a program used to assemble a root silently
 * missing it. It now refuses to assemble one, at the moment the fix
 * costs a single install rather than a reboot.
 */
static int stage_controlplane_programs(const char *image_root, const char *host_tools_dir)
{
	static const struct {
		const char *path;
		enum controlplane_program_source source;
		const char *pkg;
	} programs[] = {
#define CP_STAGE_(id, path, source, pkg) { path, source, pkg },
		CONTROLPLANE_PROGRAMS(CP_STAGE_)
#undef CP_STAGE_
	};
	size_t i;
	int staged = 0;

	if (host_tools_dir[0] == '\0') {
		fprintf(stderr,
		        "no cix-hosttools root given, so this root would carry none of the programs "
		        "the control plane executes -- no CPDL engine to install a package with "
		        "(ADR-0307 clause 6), no mksquashfs to assemble its own next root with, and "
		        "no disk tools at all (ADR-0078, cix#350)\n");
		return 1;
	}
	for (i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) {
		char src[PATH_MAX];
		char dst[PATH_MAX];
		char parent[PATH_MAX];
		char *slash;
		struct stat st;

		if (programs[i].source != CP_HOSTTOOLS)
			continue;
		if (snprintf(src, sizeof(src), "%s%s", host_tools_dir, programs[i].path) >=
		        (int)sizeof(src) ||
		    snprintf(dst, sizeof(dst), "%s%s", image_root, programs[i].path) >= (int)sizeof(dst)) {
			fprintf(stderr, "path too long: %s\n", programs[i].path);
			return 1;
		}
		if (stat(src, &st) != 0) {
			fprintf(stderr,
			        "%s is absent, so the assembled root would have no %s -- code running on "
			        "the control plane executes it by that absolute path "
			        "(include/controlplane_programs.h). Fix it with: pkg install "
			        "--image=cix-hosttools %s -- or, when this is a build container rather "
			        "than a host-tools image, by declaring tool \"%s\" in the recipe\n",
			        src, programs[i].path, programs[i].pkg, programs[i].pkg);
			return 1;
		}
		/* Derived from the declared path, so a list that grows a new
		 * directory needs no second edit here. */
		snprintf(parent, sizeof(parent), "%s", programs[i].path + 1);
		slash = strrchr(parent, '/');
		if (slash != NULL) {
			*slash = '\0';
			if (ensure_dir_path_under(image_root, parent) != 0)
				return 1;
		}
		if (test_image_fixture_copy_file(src, dst) != 0)
			return 1;
		staged++;
	}
	fprintf(stderr, "staged %d control-plane programs from %s\n", staged, host_tools_dir);
	return 0;
}

/*
 * cix#350: the FILES the control plane needs that are not programs and
 * not libraries, from the same image and for the same reason.
 *
 * THE CA BUNDLE IS PUBLIC ROOTS ONLY, and that is the point (ADR-0350).
 * It is Mozilla's set as the ca-certificates package ships it. A site's
 * own CA is never written here, because this root is embedded in the
 * published installer ISO and would make every public install trust it.
 * An operator's CA is a trust anchor in the state directory instead:
 * cixd writes those plus this file into one bundle and hands it to
 * libcurl as CURLOPT_CAINFO (trustanchor.c, curlfetch.c). This file is
 * also what cixd falls back to when that bundle is absent, since it is
 * curl's compiled-in default path (ADR-0096).
 *
 * The order mattered: until 0.2.57-576 this file was the build host's
 * copy, which carried the site's CA (`CN=gibnet Intermediate CA` issues
 * git.home.arpa's certificate, measured 2026-10-10), and swapping it
 * for public roots before an anchor existed would have cut the box off
 * from its own forge -- the 0.2.57-481 failure again. The anchors were
 * registered on 192.168.15.95 on 2026-10-23, before this change.
 *
 * openssl.cnf belongs here for two reasons, the second stronger than
 * provenance: the config has to MATCH the library that reads it. #351
 * moved the PKI in-process to the libcrypto cixd links, and ADR-0337
 * made that Cix's own libcrypto from usr/lib -- so the config it reads
 * at init should be Cix's openssl's, not a Debian machine's.
 *
 * No fallback, for the reasons ADR-0337 states, and an absence names
 * the install that fixes it.
 */
static int stage_controlplane_files(const char *image_root, const char *host_tools_dir)
{
	static const struct {
		const char *path; /* identical in the image and in the root */
		const char *pkg;
		const char *why;
	} files[] = {
		{ "/usr/lib/ssl/openssl.cnf", "openssl",
		  "the config the libcrypto cixd links reads at init, and it should be the "
		  "config of the openssl this platform BUILT rather than a build machine's -- "
		  "found originally by a real `pki ca bootstrap` failing with \"req -x509 "
		  "failed: Can't open /usr/lib/ssl/openssl.cnf\", not guessed at" },
		{ "/etc/ssl/certs/ca-certificates.crt", "ca-certificates",
		  "the public roots every HTTPS fetch cixd makes verifies against, alone or "
		  "with the registered trust anchors (ADR-0350); without it no source, "
		  "forge or cache over HTTPS can be reached" },
	};
	size_t i;

	if (host_tools_dir[0] == '\0')
		return 1; /* stage_controlplane_programs() already said why */
	for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		char src[PATH_MAX];
		char dst[PATH_MAX];
		char parent[PATH_MAX];
		char *slash;
		struct stat st;

		if (snprintf(src, sizeof(src), "%s%s", host_tools_dir, files[i].path) >=
		        (int)sizeof(src) ||
		    snprintf(dst, sizeof(dst), "%s%s", image_root, files[i].path) >= (int)sizeof(dst)) {
			fprintf(stderr, "path too long: %s\n", files[i].path);
			return 1;
		}
		if (stat(src, &st) != 0) {
			fprintf(stderr,
			        "%s is absent, so the assembled root would have no %s -- %s. Fix it "
			        "with: pkg install --image=cix-hosttools %s\n",
			        src, files[i].path, files[i].why, files[i].pkg);
			return 1;
		}
		snprintf(parent, sizeof(parent), "%s", files[i].path + 1);
		slash = strrchr(parent, '/');
		if (slash != NULL) {
			*slash = '\0';
			if (ensure_dir_path_under(image_root, parent) != 0)
				return 1;
		}
		if (test_image_fixture_copy_file(src, dst) != 0)
			return 1;
	}
	fprintf(stderr, "staged %zu control-plane files from %s\n",
	        sizeof(files) / sizeof(files[0]), host_tools_dir);
	return 0;
}

int test_root_tree_build(const char *image_root, const char *cixd_bin, const char *cixctl_bin,
                         const char *web_dir, const char *firmware_dir, const char *modules_dir,
                         const char *kmod_bin_dir, const char *host_tools_dir)
{
	/*
	 * ADR-0253: the assembled root is this build's output, so it starts
	 * empty. Inheriting it is how a root came to carry glibc objects
	 * from two different builds -- libc.so.6 from one and
	 * libm/libpthread/libresolv from another -- which share a private,
	 * version-locked interface and panic pid 1 at boot. That cost two
	 * resets of a real host. verify_platform_libs_intact() below is the
	 * guard against staging order re-introducing it; this is the
	 * guard against the previous build re-introducing it.
	 */
	if (persist_fresh_output_dir(image_root) != 0) {
		fprintf(stderr, "could not start the image root %s empty\n", image_root);
		return 1;
	}
	if (ensure_dir(image_root) != 0)
		return 1;
	if (test_image_fixture_build(image_root, cixd_bin, "cixd") != 0)
		return 1;
	/*
	 * ADR-0260: cix-init, pid 1 in every container, travels beside
	 * cixd and is staged beside it -- the daemon finds it next to its
	 * own executable (/bin/cix-init next to /bin/cixd) and copies it
	 * into each container's tree at creation. Freestanding: no
	 * libraries to stage. Required, not optional: a control plane
	 * without it cannot create a container at all.
	 */
	{
		char init_src[PATH_MAX];
		char init_dst[PATH_MAX];
		const char *slash = strrchr(cixd_bin, '/');

		if (slash != NULL)
			snprintf(init_src, sizeof(init_src), "%.*s/cix-init", (int)(slash - cixd_bin), cixd_bin);
		else
			snprintf(init_src, sizeof(init_src), "cix-init");
		snprintf(init_dst, sizeof(init_dst), "%s/bin/cix-init", image_root);
		if (test_image_fixture_copy_file(init_src, init_dst) != 0) {
			fprintf(stderr, "cix-init is not beside %s (%s) -- build/cix-init must exist\n", cixd_bin,
			        init_src);
			return 1;
		}
	}
	/*
	 * cixctl itself: staged so cixd --init-mode (Phase 19) has
	 * something to execve() when it spawns a managed shell on each
	 * console -- previously absent from this image entirely, meaning
	 * even a fully working console-input path would have had nothing to
	 * launch. Needs no extra runtime libs of its own beyond what's
	 * already staged above (ld.so/libc.so.6, the same TCC dynamic-link
	 * dependency cixd itself has).
	 *
	 * At /usr/bin/cixctl, the path its package installs (ADR-0371
	 * decision C, the owner on 2026-11-10): the root built from the
	 * cix-root image will have it there, and one path is what both
	 * roots and the console agree on. It used to go through
	 * test_image_fixture_build(), which put it at /bin/cixctl and
	 * staged the C library a second time over cixd's.
	 */
	{
		char cixctl_dir[PATH_MAX];
		char cixctl_dst[PATH_MAX];

		snprintf(cixctl_dir, sizeof(cixctl_dir), "%s/usr/bin", image_root);
		snprintf(cixctl_dst, sizeof(cixctl_dst), "%s/usr/bin/cixctl", image_root);
		if (test_mkdir_p(cixctl_dir) != 0 ||
		    test_image_fixture_copy_file(cixctl_bin, cixctl_dst) != 0) {
			fprintf(stderr, "could not stage %s as %s\n", cixctl_bin, cixctl_dst);
			return 1;
		}
	}
	/*
	 * libtinfo.so.6 WAS STAGED HERE BY NAME, from the build host, and
	 * is not any more (cix#350).
	 *
	 * Its stated reason was that the running root is where the image
	 * baseline (daemon/src/pkg.c) copied a container image's C runtime
	 * from. Both halves of that are gone: ADR-0210 removed the
	 * installer's copy of the same three files, and ADR-0333 removed
	 * the baseline itself, so nothing has read this one since. A
	 * comment describing a mechanism that no longer exists is what the
	 * next reader believes instead of reading the code, so it goes
	 * with the staging rather than staying as a note.
	 *
	 * The root still carries libtinfo and carries it better: ncurses is
	 * in the cix-hosttools image (bash's and perl's dependency), so the
	 * platform block below brings its usr/lib/libtinfo.so.6 -- a
	 * symlink onto libncursesw.so.6 -- whereas this staging put a
	 * Debian build of it at lib/x86_64-linux-gnu, which the loader
	 * searches first (probe-rootlibs@5-1, 2026-10-10: 204,088 bytes of
	 * it, sitting in front of ours).
	 */

	/*
	 * The programs code on this root executes, and the libraries they
	 * need. Both are DERIVED now (cix#350): the programs from
	 * include/controlplane_programs.h, the libraries from what the
	 * cix-hosttools image carries, with the seal checking the result
	 * against every object's own DT_NEEDED.
	 *
	 * This comment used to call the daemon's `_BIN` defines "the
	 * authoritative list" and tell the reader to regenerate a library
	 * set with `ldd` on the build host. Both were true when written
	 * and are not now: the defines are declarations that
	 * controlplane_programs.h collects (#554), and a build host's `ldd`
	 * answers for a build host -- which on an installed Cix machine is
	 * this very root, so following that instruction is what carried
	 * Debian's libraries forward release after release (cix#589).
	 *
	 * What has not changed is why any of it is here, and it is worth
	 * keeping: these were once entirely absent, and booting a fresh
	 * install and running `pki ca bootstrap` on the console failed with
	 * "CA genpkey failed" because /usr/bin/openssl was simply not
	 * there. (That one is gone again for a better reason -- #351 moved
	 * the PKI in-process to the libcrypto cixd links.) Failure here is
	 * fatal rather than silently skipped, unlike firmware_dir below:
	 * tolerating an absence is the bug, not the kindness.
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
		if (stage_controlplane_programs(image_root, host_tools_dir) != 0)
			return 1;

		/*
		 * NO LIBRARY IS NAMED HERE ANY MORE (cix#350).
		 *
		 * shelled_bin_libs[] stood here: nine sonames, each copied from
		 * the BUILD HOST into CIX_LIB_DIR_RUNTIME. The platform block
		 * below already copies every shared object the cix-hosttools
		 * image carries, preserving that image's own directories -- so
		 * the two were staging the same libraries twice, from two
		 * different builds, into two different directories.
		 *
		 * AND THE FOREIGN COPY WON. glibc's compiled-in search path is
		 * slibdir then libdir (include/libdirs.h), which is
		 * lib/x86_64-linux-gnu then usr/lib -- and the hand list wrote
		 * into the first while five of the nine packages install into
		 * the second. Measured on 192.168.15.95, 2026-10-10 by
		 * probe-rootlibs@5-1, which unsquashed the real control-plane
		 * root out of cix-installer-0.2.57-481 and hashed every
		 * lib*.so* in it: SIX libraries existed twice, with different
		 * bytes, the Debian-sized copy first in the search order.
		 *
		 *   libcrypto.so.3   5,499,360 B vs 4,833,688 B
		 *   libssl.so.3        688,160 B vs   729,056 B
		 *   liblzma.so.5     3,233,805 B vs   327,440 B
		 *   libgcc_s.so.1      885,664 B vs   190,992 B
		 *   libz.so.1, libzstd.so.1, libtinfo.so.6  likewise
		 *
		 * The attribution is measured rather than argued: openssl's own
		 * published artifact was extracted beside the root in the same
		 * probe, and its usr/lib/libssl.so.3 is byte-identical
		 * (6fcdb10f...) to the root's usr/lib copy and different from
		 * the root's lib/x86_64-linux-gnu one. So cixd -- which needs
		 * libssl and libcrypto -- was linking a foreign OpenSSL while
		 * ours sat one directory later. That is exactly the libcurl
		 * finding of the day before (cix#589), and it had the same
		 * cause: a list that sources from the build host, which on an
		 * installed host is the control-plane root itself.
		 *
		 * All nine were already provided by the image, checked against
		 * the daemon's own file lists for the 21 packages installed in
		 * cix-hosttools, so this is a deletion rather than a
		 * substitution. What was true of the other three (libm,
		 * libpthread, libresolv) is worth stating because it is why
		 * this went unnoticed: glibc installs those AT
		 * lib/x86_64-linux-gnu, so the platform block overwrote the
		 * host's copies and the ordering comment below is the only
		 * thing that kept them right.
		 *
		 * WHAT REPLACES THE LIST IS THE SEAL, which already reads the
		 * root rather than a list: verify_root_closure() walks every
		 * ELF object in the assembled tree and refuses one whose
		 * DT_NEEDED soname nothing provides. A library the root needs
		 * and the image does not carry therefore fails the build,
		 * naming the file and the soname -- which is the same posture
		 * stage_controlplane_programs() takes for programs. The one
		 * thing no such walk can see is a dlopen(), and
		 * require_dlopened_libs() below carries that single case.
		 */
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
		 *
		 * DO NOT GO LOOKING FOR shelled_bin_libs[]: it is gone
		 * (cix#350, ADR-0337), and the hazard it created cannot recur
		 * because nothing writes into these directories any more. The
		 * ordering is kept for the reason that outlived it -- the
		 * libc.so.6 and loader that test_image_fixture_build() read
		 * off the BUILD HOST near the top of this function still have
		 * to be replaced by the ones this platform built, and
		 * verify_platform_libs_intact() below assumes this block ran
		 * last. The history is kept because it is why the guard
		 * exists, not because the list still does.
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
					/*
					 * lstat, not stat. stat() follows the link, so
					 * every libfoo.so -> .so.5 -> .so.5.8.3 chain
					 * passed S_ISREG and had its CONTENT copied three
					 * times over -- 30.93 MiB of duplicates in a root
					 * carrying 55.52 MiB of unique bytes, and a root
					 * with no symlinks at all where the ABI expects
					 * them.
					 */
					if (lstat(src, &fst) != 0)
						continue;
					if (!S_ISREG(fst.st_mode) && !S_ISLNK(fst.st_mode))
						continue;
					snprintf(dst, sizeof(dst), "%s/%s/%s", image_root, lib_dirs[d],
					         de->d_name);
					if (S_ISLNK(fst.st_mode)) {
						if (stage_symlink(src, dst) != 0) {
							fprintf(stderr, "staging the platform's own libraries: "
							                "%s/%s is a symlink and could not be "
							                "reproduced\n",
							        lib_dirs[d], de->d_name);
							closedir(dh);
							return 1;
						}
						staged_libs++;
						continue;
					}
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
			/* require_dlopened_libs() is mkbootroot's seal, run on the
			 * assembled root (ADR-0371 phase 3a). */
		}

		/*
		 * The C.UTF-8 locale, from the same host-tools image and for the
		 * same reason: it is glibc's (2.44-17 onwards), and cixd needs it
		 * (#518). cixd sets LC_CTYPE to C.UTF-8 so libarchive can extract
		 * a pax archive's UTF-8 path; with no locale in this root the call
		 * fails and every source tarball with a non-ASCII name is refused,
		 * which is how go1.24.9 and gcc-16.2.0 stopped building. glibc
		 * builds no locale in (probe-locale@1-1 measured `locale -a`
		 * listing only C and POSIX), so this root carries exactly the
		 * files glibc's own localedef produced. Absent from an older
		 * host-tools glibc, which is said rather than failed on: the root
		 * still boots, it just keeps that limit.
		 */
		if (host_tools_dir != NULL && host_tools_dir[0] != '\0') {
			char loc_src[PATH_MAX], loc_dst[PATH_MAX];
			struct stat lst;

			if (snprintf(loc_src, sizeof(loc_src), "%s/usr/lib/locale/C.utf8", host_tools_dir) >=
			            (int)sizeof(loc_src) ||
			    snprintf(loc_dst, sizeof(loc_dst), "%s/usr/lib/locale/C.utf8", image_root) >=
			            (int)sizeof(loc_dst)) {
				fprintf(stderr, "path too long staging the C.UTF-8 locale\n");
				return 1;
			}
			if (stat(loc_src, &lst) == 0 && S_ISDIR(lst.st_mode)) {
				if (ensure_dir_path_under(image_root, "usr/lib/locale") != 0)
					return 1;
				if (stage_tree(loc_src, loc_dst) != 0)
					return 1;
				fprintf(stderr, "staged the C.UTF-8 locale from %s\n", host_tools_dir);
			} else {
				fprintf(stderr,
				        "note: %s carries no C.UTF-8 locale -- cixd will run with an "
				        "ASCII codeset and refuse any source archive with a non-ASCII "
				        "path name (#518)\n",
				        host_tools_dir);
			}
		}

		/*
		 * openssl.cnf and the CA bundle, from this same image (cix#350,
		 * ADR-0350). Both used to be copied off the build host here,
		 * which on an installed host is the control-plane root --
		 * stage_controlplane_files() above carries the argument.
		 */
		if (stage_controlplane_files(image_root, host_tools_dir) != 0)
			return 1;

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
	 * since a QEMU/CI boot test needs no real firmware and this keeps
	 * the test suite's own fast, no-network posture completely
	 * unaffected. A non-empty firmware_dir is a real, explicit operator
	 * request, so unlike test_image_fixture_add_lib()'s own tolerant-
	 * if-missing precedent, a failure here is fatal, not silently
	 * skipped -- an operator who asked for firmware staging and didn't
	 * get it should find out now, not at first device use on the
	 * installed system.
	 *
	 * firmware_dir is a firmware ROOT: its contents mirror
	 * /lib/firmware exactly, so "amdgpu/vega10_smc.bin" and
	 * "rtw88/rtw8822b_fw.bin" and a bare "regulatory.db" all land where
	 * request_firmware() looks for them, with no knowledge here of
	 * which drivers exist.
	 *
	 * It used to be an amdgpu directory specifically -- a flat copy
	 * into a hardcoded lib/firmware/amdgpu (ADR-0029, when a GPU was
	 * the only device on this platform that wanted firmware). The
	 * access-point work (#30) added a second consumer, and a second
	 * hardcoded subdirectory beside the first would have been two
	 * mechanisms for one job, growing by one every time a device needs
	 * a blob. One recursive copy of a mirrored tree is the same
	 * capability without the branching, and amdgpu keeps working by
	 * being a subdirectory of the root rather than the whole of it.
	 */
	if (firmware_dir[0] != '\0') {
		char fw_dst[PATH_MAX];

		if (ensure_dir_under(image_root, "lib") != 0)
			return 1;
		if (snprintf(fw_dst, sizeof(fw_dst), "%s/lib/firmware", image_root) >=
		    (int)sizeof(fw_dst)) {
			fprintf(stderr, "path too long: %s/lib/firmware\n", image_root);
			return 1;
		}
		/* Recursive, and the destination is created BY the copy --
		 * same contract the modules staging below relies on, which is
		 * why "lib" alone is pre-created here. */
		if (stage_tree(firmware_dir, fw_dst) != 0)
			return 1;
		/*
		 * stderr, not stdout. Every other progress line here is
		 * stderr and therefore unbuffered; a printf to a pipe is
		 * block-buffered and flushes at exit, so this checkpoint
		 * appeared at the very END of the captured output instead of
		 * in sequence -- which is exactly where the log store's own
		 * truncation lands. A checkpoint that can be cut off is not
		 * one.
		 */
		fprintf(stderr, "staged %ld firmware file(s) from %s\n",
		        count_files_recursive(fw_dst), firmware_dir);
	}

	/* Same "empty means skip, non-empty is a real explicit request and
	 * fatal if it fails" posture as firmware_dir above -- an operator
	 * who asked for module support and didn't get it should find out
	 * now, not at first real modprobe on the installed system.
	 * stage_tree() is used here, not the flat copy_dir_files() above,
	 * since modules_dir nests by kernel/drivers/.... "lib" is
	 * pre-created and "lib/modules" is created by the copy, fresh,
	 * every run (mksquashfs's -noappend already means every run starts
	 * from a clean image_root regardless). */
	if (modules_dir[0] != '\0') {
		char modules_dst[PATH_MAX];

		if (ensure_dir_under(image_root, "lib") != 0)
			return 1;
		if (snprintf(modules_dst, sizeof(modules_dst), "%s/lib/modules", image_root) >=
		    (int)sizeof(modules_dst)) {
			fprintf(stderr, "path too long: %s/lib/modules\n", image_root);
			return 1;
		}
		if (stage_tree(modules_dir, modules_dst) != 0)
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
	 * (cixd/cixctl staged there above), so the flat copy merges into it.
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

	/* What used to run at the seal, before mksquashfs, and still belongs
	 * to staging from a source root: the platform's libraries in the tree
	 * are the bytes the source carries. */
	return verify_platform_libs_intact(image_root, host_tools_dir) == 0 ? 0 : 1;
}
