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
 * Phase 14 part 2 (ADR-0029): optionally also stages device firmware
 * into this same root -- originally amdgpu only, now any firmware root
 * whose layout mirrors /lib/firmware (#30) -- since request_firmware()
 * calls happen at driver-probe time, on the host root, before any
 * container exists -- nowhere else this could live. The firmware itself
 * isn't fetched by this tool or vendored into the repo; it's an
 * operator-supplied directory (see README.md's own fetch recipe --
 * a real Cix-built image's own lib/firmware, e.g. the cix-firmware
 * image carrying rtw88-firmware and wireless-regdb).
 */
#include "osrelease.h"
#include "persist.h"
#include "version.h"
#include "libdirs.h"
#include "btrfs.h"
#include "controlplane_programs.h"
#include "elfcheck.h"
#include "test_image_fixture.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

/*
 * Copies the tree at src to dst in-process, as `cp -a` would (owner,
 * mode, times, symlinks as symlinks), and names both paths on failure.
 *
 * In-process because this runs on the control-plane root, which has had
 * no cp since 0.2.57-412 (#464). The firmware, module and locale copies
 * below forked `cp -a` until then, and the first assembly run on a 412
 * root failed with `execve cp: No such file or directory`.
 */
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
/*
 * Written to a temporary name and renamed onto out_path only once
 * mksquashfs has exited successfully (#481).
 *
 * mksquashfs used to be pointed straight at the final path, so that
 * path WAS the write target for the whole multi-minute xz
 * compression, and anything that interrupted it -- a reboot, a
 * failure, a killed daemon -- left a fragment sitting where the
 * platform's most consequential artifact belongs. That fragment is
 * what an operator then stages and boots.
 *
 * POST /system/update refuses it (it checks squashfs magic), and that
 * refusal is the only reason this never shipped a dead machine. But
 * the ISO builder consumed the same file behind an access(R_OK) check
 * and would have baked the fragment into signed installer media; and
 * after a reboot the daemon's assembly counters are back to 0, so
 * nothing distinguishes a fragment from a root that was never built.
 *
 * rename(2) within one directory is atomic, so the final path only
 * ever holds a whole image: this build's, or -- if this build fails --
 * the previous one's, untouched. The previous root surviving a failed
 * assembly is the point and not a side effect; it is the root the
 * machine is currently running from.
 *
 * fsync of the file before the rename and of the directory after it,
 * because a rename is atomic in the namespace but not durable until
 * both are on the medium -- and the failure this guards is a machine
 * that does not boot.
 */
static int run_mksquashfs(const char *mksquashfs_bin, const char *image_root,
                           const char *out_path, const char *host_tools_dir)
{
	pid_t pid;
	int status;
	struct stat st;
	char partial_path[PATH_MAX];
	char out_dir[PATH_MAX];
	char *slash;
	int fd;
	/*
	 * partial_path is the buffer, not its contents: this initializer
	 * captures the address, and snprintf() below fills it long before
	 * the execve().
	 */
	char *argv[] = { (char *)mksquashfs_bin, (char *)image_root, partial_path,
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

	if (snprintf(partial_path, sizeof(partial_path), "%s.partial", out_path) >=
	    (int)sizeof(partial_path)) {
		fprintf(stderr, "path too long: %s.partial\n", out_path);
		return -1;
	}
	/* A fragment from a prior interrupted run must not be appended to
	 * or mistaken for this build's output. Note this unlinks the
	 * PARTIAL, never out_path: the root already there stays valid and
	 * bootable until this build has produced a whole one to replace
	 * it. */
	unlink(partial_path);

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
			ld_argv[5] = partial_path;
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

	fd = open(partial_path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", partial_path, strerror(errno));
		return -1;
	}
	if (fsync(fd) != 0) {
		fprintf(stderr, "fsync %s: %s\n", partial_path, strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	if (rename(partial_path, out_path) != 0) {
		fprintf(stderr, "rename %s -> %s: %s\n", partial_path, out_path, strerror(errno));
		return -1;
	}
	/*
	 * The directory entry itself, so the rename survives a power loss:
	 * without this the name can still point at the old inode after a
	 * crash, which is the one case where "atomic" is not enough.
	 */
	snprintf(out_dir, sizeof(out_dir), "%s", out_path);
	slash = strrchr(out_dir, '/');
	if (slash != NULL) {
		*slash = '\0';
		fd = open(out_dir[0] != '\0' ? out_dir : "/", O_RDONLY | O_DIRECTORY);
		if (fd < 0) {
			fprintf(stderr, "open %s: %s\n", out_dir, strerror(errno));
			return -1;
		}
		if (fsync(fd) != 0) {
			fprintf(stderr, "fsync %s: %s\n", out_dir, strerror(errno));
			close(fd);
			return -1;
		}
		close(fd);
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
 * cix#350: the libraries NOTHING IN THE ROOT DECLARES.
 *
 * Every other library the root carries is there because the
 * cix-hosttools image carries it, and every one it NEEDS is checked by
 * verify_root_closure() reading each object's own DT_NEEDED. That pair
 * has one blind spot, and it is structural rather than an oversight: a
 * dlopen() names its library at runtime, in a string, so no amount of
 * reading ELF headers can see it.
 *
 * So this is the whole of the hand-maintained set now -- one entry,
 * with the measurement that put it there and the install that fixes
 * it. An entry is added here only for a library that is loaded by name
 * at runtime and would therefore pass every other gate in this file.
 *
 * Checked with stat() rather than lstat() deliberately: the image
 * installs most of these as a symlink to a versioned file, and a
 * symlink whose target did not come along is exactly as broken as an
 * absence while looking exactly like a success.
 */
static int require_dlopened_libs(const char *image_root)
{
	static const struct {
		const char *soname;
		const char *pkg;
		const char *why;
	} libs[] = {
		/*
		 * squashfs-tools is the provider, and that is deliberate on
		 * its side rather than an accident worth correcting: its
		 * recipe stages this library into its own package with
		 * `stage library "libgcc_s.so.1"` and gates it with a
		 * `require file`, because a composed build environment does
		 * not carry a declared tool's runtime dependencies (cix#510)
		 * and mksquashfs is gcc-built and threads. gcc's own package
		 * ships the same path, so the two never share an image -- if
		 * they ever do, "one package owns a path" refuses the second
		 * and names both, which is that gate working rather than a
		 * problem here.
		 */
		{ "libgcc_s.so.1", "squashfs-tools",
		  "libpthread's own pthread_exit() and pthread_cancel() dlopen() it for stack "
		  "unwinding, so a program using threads starts, runs, and aborts at its own "
		  "normal exit with \"libgcc_s.so.1 must be installed for pthread_exit to work\" "
		  "-- measured the hard way on mksquashfs, which is how the root seals itself" },
	};
	static const char *const dirs[] = CIX_LIB_DIRS_SEARCH;
	size_t l, d;

	for (l = 0; l < sizeof(libs) / sizeof(libs[0]); l++) {
		int found = 0;

		for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]) && !found; d++) {
			char p[PATH_MAX];
			struct stat st;

			if (snprintf(p, sizeof(p), "%s/%s/%s", image_root, dirs[d], libs[l].soname) >=
			    (int)sizeof(p))
				continue;
			if (stat(p, &st) == 0 && S_ISREG(st.st_mode))
				found = 1;
		}
		if (!found) {
			fprintf(stderr,
			        "the assembled root has no %s, which nothing in it declares and "
			        "everything that threads needs: %s. Fix it with: pkg install "
			        "--image=cix-hosttools %s\n",
			        libs[l].soname, libs[l].why, libs[l].pkg);
			return 1;
		}
	}
	return 0;
}

/*
 * cix#569: every ELF object in the assembled root has its DT_NEEDED
 * closure inside the root, checked at the seal.
 *
 * shelled_bin_libs[] was a list kept beside the binaries by hand, and
 * nothing compared the two: a library could stay staged long after its
 * last consumer left (tar's libacl/libselinux/libpcre2/libattr, and
 * bzip2's libbz2, which no Cix package even builds), and a program
 * could be added whose library was never listed, which only shows as
 * a boot that cannot exec it. The binaries state their own needs, so
 * the root is read rather than the list trusted --
 * elfcheck_undeclared_links() with nothing provided from outside,
 * which reports the first soname no file in the tree is named.
 *
 * -1 refuses too: that is "could not tell", and a root is sealed on
 * evidence, never on ignorance (elfcheck.h).
 *
 * SINCE cix#350 THIS IS ALSO THE SUPPLY SIDE'S GATE. There is no list
 * of libraries any more: the root gets every shared object the
 * cix-hosttools image carries, and this walk says whether that was
 * enough. So a hit no longer means "someone forgot to add a line
 * here", it means the image is missing a package -- which is why the
 * message names the install rather than a table.
 */
static int verify_root_closure(const char *image_root)
{
	char file[PATH_MAX];
	char soname[ELFCHECK_SONAME_MAX];
	int rc = elfcheck_undeclared_links(image_root, NULL, 0, file, sizeof(file), soname,
	                                    sizeof(soname));

	if (rc == 0)
		return 0;
	if (rc < 0)
		fprintf(stderr, "could not read the assembled root %s to check its library closure\n",
		        image_root);
	else
		fprintf(stderr,
		        "the assembled root's /%s needs %s, which nothing in the root provides -- "
		        "the root carries the cix-hosttools image's shared objects and that image "
		        "has none named %s, so install the package that provides it there, or stop "
		        "staging /%s (cix#569, cix#350)\n",
		        file, soname, soname, file);
	return 1;
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
 * #554: refuses a root missing any program in
 * include/controlplane_programs.h, naming it. Each must be a regular
 * file, following symlinks, with an execute bit. A CP_KMOD program is
 * required only when the kmod image's usr/bin was given (have_kmod).
 *
 * Written after 0.2.57-414: 412 dropped `cp` from the root while
 * mkbootroot still forked it, and nothing noticed until an assembly
 * ran on that root. Returns 0, or 1 with the reason on stderr.
 */
static int verify_controlplane_programs(const char *image_root, int have_kmod)
{
	static const struct {
		const char *path;
		enum controlplane_program_source source;
		const char *pkg;
	} programs[] = {
#define CP_ENTRY_(id, path, source, pkg) { path, source, pkg },
		CONTROLPLANE_PROGRAMS(CP_ENTRY_)
#undef CP_ENTRY_
	};
	size_t i;
	int missing = 0;

	for (i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) {
		char p[PATH_MAX];
		struct stat st;

		if (programs[i].source == CP_KMOD && !have_kmod)
			continue;
		if (snprintf(p, sizeof(p), "%s%s", image_root, programs[i].path) >= (int)sizeof(p)) {
			fprintf(stderr, "path too long: %s%s\n", image_root, programs[i].path);
			return 1;
		}
		if (stat(p, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0111) == 0) {
			fprintf(stderr,
			        "the assembled root has no executable %s, which code running on the "
			        "control plane executes (include/controlplane_programs.h, #554)\n",
			        programs[i].path);
			missing++;
		}
	}
	return missing > 0 ? 1 : 0;
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
		        "<out.squashfs> <firmware-root-or-\"\"> <modules-dir-or-\"\"> "
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
	 * the cix-hosttools image -- when given, the host_tool_bins[] below
	 * (btrfs and mksquashfs) are copied from THIS tree instead
	 * of the build host. "" keeps the build-host source, the same
	 * tolerant default firmware_dir/modules_dir/kmod_bin_dir above
	 * already established. tar, gzip, xz and bzip2 are not staged at
	 * all (cix#569).
	 */
	host_tools_dir = argv[9];

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
	 */
	if (test_image_fixture_build(image_root, cixctl_bin, "cixctl") != 0)
		return 1;
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
			if (require_dlopened_libs(image_root) != 0)
				return 1;
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

	/*
	 * /etc/os-release, so the host can say what it is.
	 *
	 * The freedesktop spec's documented default when ID is absent is
	 * "linux", so without this file every reader identified a Cix host
	 * as generic Linux -- which is the one answer a platform built from
	 * source specifically to not be somebody else's distribution should
	 * never give. Found while packaging fastfetch, which reads exactly
	 * this file.
	 *
	 * osrelease_render() rather than a literal, because
	 * the image baseline staged (removed, ADR-0333) the same file into every
	 * container image. Two copies of this text would drift, and the
	 * shape of that drift is a host and the containers running on it
	 * disagreeing about what they are.
	 *
	 * BUILD_ID carries CIX_BUILD_VERSION, which is the version this
	 * root is being assembled AS -- the same string GET /v1/system/boot
	 * reports, so an operator reading os-release and an operator
	 * reading the API get the same answer rather than two that have to
	 * be reconciled.
	 */
	{
		char dst[PATH_MAX];
		char osr[OSRELEASE_MAX];
		FILE *f;

		if (osrelease_render(osr, sizeof(osr), CIX_BUILD_VERSION) != 0) {
			fprintf(stderr, "could not render /etc/os-release\n");
			return 1;
		}
		snprintf(dst, sizeof(dst), "%s/etc/os-release", image_root);
		f = fopen(dst, "w");
		if (f == NULL) {
			perror(dst);
			return 1;
		}
		if (fputs(osr, f) == EOF || fclose(f) != 0) {
			perror(dst);
			return 1;
		}
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
		/*
		 * #554: every program code on this root executes, checked at the
		 * seal rather than trusted from the step that staged it -- the
		 * reason verify_platform_libs_intact() above exists. This
		 * replaces a check for /usr/bin/cbs alone (ADR-0307 clause 6),
		 * which is one entry of the list.
		 */
		if (verify_controlplane_programs(image_root, kmod_bin_dir[0] != '\0') != 0)
			return 1;
		if (verify_root_closure(image_root) != 0)
			return 1;
		if (run_mksquashfs(use_mksquashfs, image_root, out_path, host_tools_dir) != 0)
			return 1;
	}

	printf("wrote %s\n", out_path);
	return 0;
}
