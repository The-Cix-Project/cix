/*
 * Seals a control-plane root (ADR-0371 phase 3a): copies an image tree
 * into a staging directory, adds what ADR-0333 says is not image content
 * -- the mount points, an empty /etc/resolv.conf and /etc/os-release --
 * checks the result, and squashes it into one read-only image.
 *
 *   mkbootroot <image-rootfs> <out.squashfs> <staging-dir>
 *
 * In production the tree is the cix-root image's current version
 * (spawn_cix_bootroot_assembly()): every program, library and file the
 * root carries comes from that image's packages, and nothing else writes
 * into it. The seals are what stays of the old assembly, because #589 is
 * why they exist: verify_controlplane_files(), verify_controlplane_
 * programs(), require_dlopened_libs() and verify_root_closure() read the
 * assembled tree, never a list of what was meant to be in it.
 *
 * ONE RELEASE ONLY, it also takes the old ten arguments:
 *
 *   mkbootroot <staging-dir> <cixd> <cixctl> <web-dir> <out.squashfs>
 *              <firmware-root> <modules-dir> <kmod-bin-dir> <host-tools-rootfs>
 *
 * The daemon that assembles release K is always release K-1, so K's tool
 * must take K-1's call. That form builds a tree from the host-tools root
 * with test_root_tree_build() and seals it exactly as above. Release
 * K+1, assembled by a daemon that speaks the three-argument form, drops
 * it, and with it this file's link to test/test_root_tree.c and
 * test/test_image_fixture.c (ADR-0371; the same one-release rule as the
 * handover record's format, ADR-0375).
 */
#include "osrelease.h"
#include "persist.h"
#include "version.h"
#include "libdirs.h"
#include "btrfs.h"
#include "controlplane_programs.h"
#include "elfcheck.h"
#include "test_root_tree.h"

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

/*
 * ADR-0371 phase 3a: the files the control plane needs that are not
 * programs and not libraries, checked at the seal like the programs are.
 * Each comes from a package of the cix-root image, so an absence names
 * the install that fixes it. These used to be COPIED here, one by one,
 * from the cix-hosttools image (stage_controlplane_files(), ADR-0350,
 * cix#350) and from build outputs given as arguments; the image now
 * carries them at these exact paths.
 */
static int verify_controlplane_files(const char *image_root)
{
	static const struct {
		const char *path;
		const char *pkg;
		int exec;
		const char *why;
	} files[] = {
		{ "/bin/cixd", "cixd", 1, "pid 1 of the control plane" },
		{ "/bin/cix-init", "cixd", 1,
		  "pid 1 in every container (ADR-0260); cixd copies it into each container's tree, "
		  "so a root without it cannot create a container at all" },
		{ "/web/index.html", "cixd", 0, "the dashboard cixd serves from its web root" },
		{ "/usr/lib/ssl/openssl.cnf", "openssl", 0,
		  "the config the libcrypto cixd links reads at init; `pki ca bootstrap` fails "
		  "without it" },
		{ "/etc/ssl/certs/ca-certificates.crt", "ca-certificates", 0,
		  "the public roots every HTTPS fetch cixd makes verifies against, alone or with the "
		  "registered trust anchors (ADR-0350)" },
		{ "/usr/lib/locale/C.utf8/LC_CTYPE", "glibc", 0,
		  "cixd sets LC_CTYPE to C.UTF-8 so libarchive can extract a pax archive's UTF-8 "
		  "path; without it every source tarball with a non-ASCII name is refused (#518)" },
	};
	size_t i;
	int missing = 0;

	for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		char p[PATH_MAX];
		struct stat st;

		if (snprintf(p, sizeof(p), "%s%s", image_root, files[i].path) >= (int)sizeof(p)) {
			fprintf(stderr, "path too long: %s%s\n", image_root, files[i].path);
			return 1;
		}
		if (stat(p, &st) != 0 || !S_ISREG(st.st_mode) ||
		    (files[i].exec && (st.st_mode & 0111) == 0)) {
			fprintf(stderr,
			        "the assembled root has no %s%s -- %s. Fix it with: pkg install "
			        "--image=cix-root %s\n",
			        files[i].exec ? "executable " : "", files[i].path, files[i].why, files[i].pkg);
			missing++;
		}
	}
	return missing > 0 ? 1 : 0;
}

/*
 * The assembly itself: tree -> staging (image_root) -> out_path. The
 * staging parameter is called image_root because the code below is the
 * old assembly's own, moved here unchanged, and that is the name it used.
 */
static int assemble_from_tree(const char *tree, const char *out_path, const char *image_root)
{
	struct stat tst;
	char modules[PATH_MAX];
	int have_modules;

	/* A NULL or empty path is a path relative to the working directory,
	 * which is never the tree meant (0.2.57-612's "(null)/sbin/cix-init"). */
	if (tree == NULL || tree[0] == '\0' || out_path == NULL || out_path[0] == '\0' ||
	    image_root == NULL || image_root[0] == '\0') {
		fprintf(stderr, "mkbootroot: an empty path was given for the tree, the output or the "
		                "staging directory\n");
		return 2;
	}
	if (stat(tree, &tst) != 0 || !S_ISDIR(tst.st_mode)) {
		fprintf(stderr, "the image tree %s is not a directory: %s\n", tree, strerror(errno));
		return 1;
	}
	/* ADR-0253: the staging tree starts empty every time, so nothing from
	 * a previous assembly rides along. */
	if (persist_fresh_output_dir(image_root) != 0) {
		fprintf(stderr, "could not start the staging directory %s empty\n", image_root);
		return 1;
	}
	if (stage_tree(tree, image_root) != 0)
		return 1;
	fprintf(stderr, "copied the image tree %s\n", tree);

	/*
	 * An empty /etc/resolv.conf placeholder (ADR-0076). It is not image
	 * content (ADR-0333): no package ships it, and the tree may have no
	 * /etc at all, so the directory is made here if it is missing. A real
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
	/* ADR-0375 phase 4: where a live root switch mounts the staged slot
	 * before it moves the running root's mounts into it
	 * (ROOTSWITCH_DIR, daemon/include/rootswitch.h). It has to be in
	 * the root that runs the switch, and the root is read-only, so it
	 * ships empty in every root. */
	if (ensure_dir_under(image_root, "run") != 0)
		return 1;
	if (ensure_dir_under(image_root, "run/nextroot") != 0)
		return 1;

	/* The seals, on the assembled tree. A tree that ships kernel modules
	 * ships the tools that load them: modules without modprobe would be
	 * a root that boots and cannot load a NIC driver (#347). */
	if (snprintf(modules, sizeof(modules), "%s/lib/modules", image_root) >= (int)sizeof(modules)) {
		fprintf(stderr, "path too long: %s/lib/modules\n", image_root);
		return 1;
	}
	have_modules = stat(modules, &tst) == 0 && S_ISDIR(tst.st_mode);
	if (verify_controlplane_files(image_root) != 0)
		return 1;
	if (verify_controlplane_programs(image_root, have_modules) != 0)
		return 1;
	if (require_dlopened_libs(image_root) != 0)
		return 1;
	if (verify_root_closure(image_root) != 0)
		return 1;

	/* The tree's own mksquashfs, run through the tree's own loader and
	 * libraries (run_mksquashfs(), ADR-0154): the root seals itself with
	 * the squashfs-tools it ships. */
	{
		char mksquashfs_bin[PATH_MAX];

		if (snprintf(mksquashfs_bin, sizeof(mksquashfs_bin), "%s/usr/bin/mksquashfs", tree) >=
		    (int)sizeof(mksquashfs_bin)) {
			fprintf(stderr, "path too long: %s/usr/bin/mksquashfs\n", tree);
			return 1;
		}
		if (run_mksquashfs(mksquashfs_bin, image_root, out_path, tree) != 0)
			return 1;
	}
	printf("wrote %s\n", out_path);
	return 0;
}

/* The ten-argument form, for one release (see the top of this file). */
static int assemble_legacy(char **argv)
{
	char tree[PATH_MAX];

	if (snprintf(tree, sizeof(tree), "%s.tree", argv[1]) >= (int)sizeof(tree)) {
		fprintf(stderr, "path too long: %s.tree\n", argv[1]);
		return 1;
	}
	if (test_root_tree_build(tree, argv[2], argv[3], argv[4], argv[6], argv[7], argv[8],
	                         argv[9]) != 0)
		return 1;
	return assemble_from_tree(tree, argv[5], argv[1]);
}

int main(int argc, char **argv)
{
	if (argc == 4)
		return assemble_from_tree(argv[1], argv[2], argv[3]);
	if (argc == 10)
		return assemble_legacy(argv);
	fprintf(stderr,
	        "usage: %s <image-rootfs> <out.squashfs> <staging-dir>\n"
	        "       (the ten-argument form is accepted for one release only, for the daemon "
	        "that builds this release -- ADR-0371 phase 3a)\n",
	        argv[0]);
	return 2;
}
