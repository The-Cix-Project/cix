/*
 * Assembles a minimal Phase 11 boot root at a staging directory and
 * squashes it into a single read-only image: kanxeod + the ld.so/libc.so.6
 * it dynamically links against (per the project's TCC-wide "never -static"
 * rule), plus empty /proc, /sys, and BASE_DIR mountpoints for
 * kanxeod --init-mode's own boot-time mounts (daemon/src/main.c's
 * boot_init()) to mount onto. Reuses test_image_fixture_build() (test/)
 * rather than re-implementing the same ld.so/libc staging a second time --
 * that function's own contract ("shared by every test that needs a real,
 * working image") already covers exactly this need; nothing about it is
 * test-specific.
 *
 * The base container image (base/rootfs) and any container/package data
 * are deliberately never part of this image -- Phase 11's root A/B slots
 * are scoped to the control plane only (docs/ROADMAP.md).
 *
 * Phase 14 part 2 (ADR-0029): optionally also stages GPU firmware
 * (amdgpu) into this same root, since the kernel's own request_firmware()
 * calls happen at driver-probe time, on the host root, before any
 * container exists -- nowhere else this could live. The firmware itself
 * isn't fetched by this tool or vendored into the repo; it's an
 * operator-supplied directory (see README.md's own fetch recipe --
 * upstream linux-firmware's amdgpu/ subtree, via a sparse clone).
 */
#include "test_image_fixture.h"

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

static int run_mksquashfs(const char *image_root, const char *out_path)
{
	pid_t pid;
	int status;
	char *argv[] = { (char *)MKSQUASHFS_BIN, (char *)image_root, (char *)out_path,
		          "-noappend", "-comp", "xz", "-quiet", NULL };

	/* A stale image from a prior run must not silently linger under a
	 * new build -- mksquashfs itself refuses to overwrite without
	 * -noappend, so any leftover has to go first. */
	unlink(out_path);

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve(MKSQUASHFS_BIN, argv, environ);
		perror("execve mksquashfs");
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

int main(int argc, char **argv)
{
	const char *image_root;
	const char *kanxeod_bin;
	const char *kanxeoctl_bin;
	const char *web_dir;
	const char *out_path;
	const char *firmware_dir;

	if (argc != 7) {
		fprintf(stderr,
		        "usage: %s <staging-dir> <build/kanxeod> <build/kanxeoctl> <web-dir> "
		        "<out.squashfs> <amdgpu-firmware-dir-or-\"\">\n",
		        argv[0]);
		return 2;
	}
	image_root = argv[1];
	kanxeod_bin = argv[2];
	kanxeoctl_bin = argv[3];
	web_dir = argv[4];
	out_path = argv[5];
	firmware_dir = argv[6];

	if (ensure_dir(image_root) != 0)
		return 1;
	if (test_image_fixture_build(image_root, kanxeod_bin, "kanxeod") != 0)
		return 1;
	/*
	 * kanxeoctl itself: staged so kanxeod --init-mode (Phase 19) has
	 * something to execve() when it spawns a managed shell on each
	 * console -- previously absent from this image entirely, meaning
	 * even a fully working console-input path would have had nothing to
	 * launch. Needs no extra runtime libs of its own beyond what's
	 * already staged above (ld.so/libc.so.6, the same TCC dynamic-link
	 * dependency kanxeod itself has).
	 */
	if (test_image_fixture_build(image_root, kanxeoctl_bin, "kanxeoctl") != 0)
		return 1;
	/*
	 * kanxeod itself never needs libtinfo -- this is staged purely so
	 * the RUNNING system's own root reliably has it available at its
	 * real, well-known host path, the source pkg_seed_image_baseline()
	 * (daemon/src/pkg.c) copies from when seeding a container image's
	 * own C runtime. Without this, that mechanism would only ever find
	 * ld.so/libc.so.6 (already staged above) on a real installed
	 * system -- this dev sandbox's own rich /usr made that gap easy to
	 * miss (see ADR-0023).
	 */
	if (test_image_fixture_add_lib(image_root, "/usr/lib/x86_64-linux-gnu/libtinfo.so.6") != 0)
		return 1;

	/*
	 * The real binaries kanxeod itself shells out to at runtime --
	 * grep-confirmed against daemon/src/pki.c's/daemon/src/pkg.c's own
	 * hardcoded absolute-path _BIN macros, the authoritative list, not
	 * docs/ROADMAP.md's own partly-stale "dnsmasq" mention (dnsmasq
	 * runs inside operator-created containers; kanxeod itself never
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
	 * unconditionally required for kanxeod's own core PKI/pkg
	 * functionality, not an operator-opt-in extra -- silently
	 * tolerating their absence is exactly the bug this closes.
	 */
	if (ensure_dir_under(image_root, "usr") != 0)
		return 1;
	if (ensure_dir_under(image_root, "usr/bin") != 0)
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
			{ "/usr/bin/sha256sum", "usr/bin/sha256sum" }, /* PKG_SHA256SUM_BIN */
			{ "/usr/bin/cp", "usr/bin/cp" },               /* PKG_CP_BIN */
			{ "/usr/bin/rm", "bin/rm" },                   /* PKG_RM_BIN is "/bin/rm", no /usr prefix */
			{ "/usr/bin/unsquashfs", "usr/bin/unsquashfs" }, /* PKG_UNSQUASHFS_BIN -- the
			                                                   * pkg_bootstrap_from_toolchain()
			                                                   * import path, no mount/loop-device
			                                                   * needed (this dev sandbox's own
			                                                   * documented "no /dev/loop* at all"
			                                                   * constraint; real hardware
			                                                   * shouldn't need one for this either). */
		};
		static const char *const shelled_bin_libs[] = {
			/* openssl */
			"/lib/x86_64-linux-gnu/libssl.so.3",
			"/lib/x86_64-linux-gnu/libcrypto.so.3",
			/* curl */
			"/lib/x86_64-linux-gnu/libcurl.so.4",
			"/lib/x86_64-linux-gnu/libz.so.1",
			"/lib/x86_64-linux-gnu/libnghttp2.so.14",
			"/lib/x86_64-linux-gnu/libidn2.so.0",
			"/lib/x86_64-linux-gnu/librtmp.so.1",
			"/lib/x86_64-linux-gnu/libssh2.so.1",
			"/lib/x86_64-linux-gnu/libpsl.so.5",
			"/lib/x86_64-linux-gnu/libgssapi_krb5.so.2",
			"/lib/x86_64-linux-gnu/libldap-2.5.so.0",
			"/lib/x86_64-linux-gnu/liblber-2.5.so.0",
			"/lib/x86_64-linux-gnu/libzstd.so.1",
			"/lib/x86_64-linux-gnu/libbrotlidec.so.1",
			"/lib/x86_64-linux-gnu/libunistring.so.2",
			"/lib/x86_64-linux-gnu/libgnutls.so.30",
			"/lib/x86_64-linux-gnu/libhogweed.so.6",
			"/lib/x86_64-linux-gnu/libnettle.so.8",
			"/lib/x86_64-linux-gnu/libgmp.so.10",
			"/lib/x86_64-linux-gnu/libkrb5.so.3",
			"/lib/x86_64-linux-gnu/libk5crypto.so.3",
			"/lib/x86_64-linux-gnu/libcom_err.so.2",
			"/lib/x86_64-linux-gnu/libkrb5support.so.0",
			"/lib/x86_64-linux-gnu/libsasl2.so.2",
			"/lib/x86_64-linux-gnu/libbrotlicommon.so.1",
			"/lib/x86_64-linux-gnu/libp11-kit.so.0",
			"/lib/x86_64-linux-gnu/libtasn1.so.6",
			"/lib/x86_64-linux-gnu/libkeyutils.so.1",
			"/lib/x86_64-linux-gnu/libresolv.so.2",
			"/lib/x86_64-linux-gnu/libffi.so.8",
			/* tar + cp */
			"/lib/x86_64-linux-gnu/libacl.so.1",
			"/lib/x86_64-linux-gnu/libselinux.so.1",
			"/lib/x86_64-linux-gnu/libpcre2-8.so.0",
			"/lib/x86_64-linux-gnu/libattr.so.1",
			/* unsquashfs -- libz.so.1/libzstd.so.1 already listed above (curl) */
			"/lib/x86_64-linux-gnu/libpthread.so.0",
			"/lib/x86_64-linux-gnu/libm.so.6",
			"/lib/x86_64-linux-gnu/liblzma.so.5",
			"/lib/x86_64-linux-gnu/liblzo2.so.2",
			"/lib/x86_64-linux-gnu/liblz4.so.1",
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
		for (i = 0; i < sizeof(shelled_bin_libs) / sizeof(shelled_bin_libs[0]); i++) {
			if (test_image_fixture_add_lib(image_root, shelled_bin_libs[i]) != 0)
				return 1;
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
	}

	/* kanxeod's DEFAULT_WEB_ROOT is "web", resolved relative to its own
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

	if (ensure_dir_under(image_root, "proc") != 0)
		return 1;
	if (ensure_dir_under(image_root, "sys") != 0)
		return 1;
	/* Kernel's own devtmpfs auto-mount (CONFIG_DEVTMPFS_MOUNT) needs an
	 * existing /dev to mount onto -- confirmed empirically (a real QEMU
	 * boot logged "devtmpfs: error mounting -2" without this). */
	if (ensure_dir_under(image_root, "dev") != 0)
		return 1;
	/* Phase 11 part 2: kanxeod --init-mode mounts the ESP here to reach
	 * the loader entry confirm_boot() renames once a boot proves
	 * healthy (daemon/src/main.c). */
	if (ensure_dir_under(image_root, "boot") != 0)
		return 1;
	/* Phase 11 part 3: kanxeod --init-mode mounts the config partition
	 * here for its static-IP net.conf (daemon/src/main.c's
	 * apply_static_ip()) -- absent (and harmlessly so) on parts 1/2's
	 * own throwaway disks, which have no partition 4 at all. */
	if (ensure_dir_under(image_root, "config") != 0)
		return 1;
	if (ensure_dir_under(image_root, "var") != 0)
		return 1;
	if (ensure_dir_under(image_root, "var/lib") != 0)
		return 1;
	if (ensure_dir_under(image_root, "var/lib/kanxeo") != 0)
		return 1;

	if (run_mksquashfs(image_root, out_path) != 0)
		return 1;

	printf("wrote %s\n", out_path);
	return 0;
}
