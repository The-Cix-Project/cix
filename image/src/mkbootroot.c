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
 */
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

/* Copies every regular file directly inside src_dir into dst_dir (flat,
 * not recursive -- web/'s own three files, index.html/app.js/style.css,
 * have no subdirectories, and this project's own "no framework, no
 * build step" dashboard design (ADR-0010) means that's not expected to
 * change). Needed because kanxeod's DEFAULT_WEB_ROOT ("web", relative --
 * daemon/src/main.c) previously only resolved correctly when running
 * from a repo checkout during development; the installed system's own
 * squashfs root never had a web/ directory at all, so every dashboard
 * request 404'd even though the REST API (a separate routing path)
 * worked fine -- found live, after a real install. */
static int copy_dir_files(const char *src_dir, const char *dst_dir)
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
	const char *web_dir;
	const char *out_path;

	if (argc != 5) {
		fprintf(stderr, "usage: %s <staging-dir> <build/kanxeod> <web-dir> <out.squashfs>\n",
		        argv[0]);
		return 2;
	}
	image_root = argv[1];
	kanxeod_bin = argv[2];
	web_dir = argv[3];
	out_path = argv[4];

	if (ensure_dir(image_root) != 0)
		return 1;
	if (test_image_fixture_build(image_root, kanxeod_bin, "kanxeod") != 0)
		return 1;

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
		if (copy_dir_files(web_dir, web_dst) != 0)
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
