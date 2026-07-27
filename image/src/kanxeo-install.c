/*
 * kanxeo-install: takes a raw disk (already partitioned by the operator
 * via a real, interactive cfdisk session -- or pre-partitioned by other
 * tooling, see --skip-partition) and produces the real Phase 11 layout:
 * ESP, root A, root B, config, containers (docs/ROADMAP.md's Phase 11
 * design). Boots as its own init= target, in its own squashfs image
 * bundling the payload it installs (a control-plane squashfs, the
 * kernel, systemd-boot) -- see test/test_installer.c for how that image
 * gets assembled and driven end to end.
 *
 * Runs on a real target disk, never a disk *image file* -- unlike
 * test/test_disk_image.c's test-only helpers (built to assemble a disk
 * image file for QEMU's benefit on a dev LXC with no loop devices),
 * this uses ordinary mount()/mkfs subprocess calls throughout. No
 * mtools, no raw byte-offset math against a file -- partitions here are
 * real block devices.
 *
 * Partition roles are read back from GPT partition *names* (sfdisk -d),
 * not just types -- root A/B/config/containers would otherwise all
 * share a generic "Linux filesystem" type with nothing to tell them
 * apart: kanxeo-esp, kanxeo-root-a, kanxeo-root-b, kanxeo-config,
 * kanxeo-containers.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define CFDISK_BIN "/usr/sbin/cfdisk"
#define SFDISK_BIN "/usr/sbin/sfdisk"
#define MKFS_VFAT_BIN "/usr/sbin/mkfs.vfat"
#define MKFS_EXT4_BIN "/usr/sbin/mkfs.ext4"

/* Bundled inside this installer's own bootable environment -- see
 * image/src/mkinstalleriso.c for how they get staged there. The kernel
 * lives under /boot/ rather than /payload/ since it serves double duty:
 * the same file GRUB itself boots as this installer's own kernel, and
 * the file copied here onto the target disk's ESP -- one copy, not two. */
#define SYSTEMD_BOOT_EFI_SRC "/payload/systemd-bootx64.efi"
#define BZIMAGE_SRC "/boot/kanxeo-bzImage"
#define ROOT_SQUASHFS_SRC "/payload/kanxeo-root.squashfs"

#define ESP_MOUNT "/mnt/esp"
#define CONFIG_MOUNT "/mnt/config"
#define CONTAINERS_MOUNT "/mnt/containers"

#define ROOT_A_TRIES 3

/*
 * The loader entry's root= and every future boot's own partition access
 * (daemon/src/main.c's ESP_DEVICE/CONFIG_DEVICE) are about the INSTALLED
 * system's own future boot, once this installer's disk is gone and the
 * target is the only disk present -- fixed convention, not derived from
 * whatever device path the installer itself saw the target disk at
 * (e.g. /dev/vdb during install, becoming /dev/vda once it's the only
 * disk left). Partition numbers match this file's own partitioning
 * scheme: 1=ESP, 2=root-a, 3=root-b, 4=config, 5=containers.
 */
#define BOOT_TIME_DISK_PREFIX "/dev/vda"

static int run_subprocess(const char *bin, char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve(bin, argv, environ);
		perror(bin);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid) {
		perror("waitpid");
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "%s failed (status 0x%x)\n", bin, (unsigned)status);
		return -1;
	}
	return 0;
}

static int run_subprocess_capture(const char *bin, char *const argv[], char *out, size_t out_size)
{
	int pipefd[2];
	pid_t pid;
	int status;
	size_t total = 0;
	ssize_t n;

	out[0] = '\0';
	if (pipe2(pipefd, O_CLOEXEC) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execve(bin, argv, environ);
		_exit(127);
	}
	close(pipefd[1]);
	while (total + 1 < out_size) {
		n = read(pipefd[0], out + total, out_size - total - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		total += (size_t)n;
	}
	out[total] = '\0';
	close(pipefd[0]);
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

static int ensure_dir(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	return 0;
}

static int write_text_file(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");

	if (f == NULL) {
		perror(path);
		return -1;
	}
	if (fputs(content, f) < 0) {
		perror(path);
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static int copy_file(const char *src_path, const char *dst_path)
{
	int src, dst;
	char buf[65536];
	ssize_t n;

	src = open(src_path, O_RDONLY);
	if (src < 0) {
		perror(src_path);
		return -1;
	}
	dst = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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

/* Writes the whole content of src_path onto dst device_path -- root A's
 * squashfs, raw, no filesystem/offset math needed since this is a real
 * partition device, not a byte range inside a larger disk image file. */
static int write_whole_file_to_device(const char *src_path, const char *device_path)
{
	return copy_file(src_path, device_path);
}

/*
 * Finds the device path for the partition whose GPT name matches
 * want_name in `sfdisk -d`'s dump output -- lines look like
 * "/dev/vdb2 : start=..., size=..., type=..., name=\"kanxeo-root-a\"".
 */
static int find_partition_device(const char *dump, const char *want_name, char *out_device,
                                  size_t out_size)
{
	const char *line = dump;
	char name_pat[64];

	snprintf(name_pat, sizeof(name_pat), "name=\"%s\"", want_name);

	while (line != NULL && *line != '\0') {
		const char *eol = strchr(line, '\n');
		size_t linelen = eol != NULL ? (size_t)(eol - line) : strlen(line);
		char linebuf[512];
		const char *colon;

		if (linelen >= sizeof(linebuf))
			linelen = sizeof(linebuf) - 1;
		memcpy(linebuf, line, linelen);
		linebuf[linelen] = '\0';

		if (strstr(linebuf, name_pat) != NULL) {
			colon = strstr(linebuf, " : ");
			if (colon != NULL) {
				size_t devlen = (size_t)(colon - linebuf);

				if (devlen >= out_size)
					devlen = out_size - 1;
				memcpy(out_device, linebuf, devlen);
				out_device[devlen] = '\0';
				return 0;
			}
		}
		line = eol != NULL ? eol + 1 : NULL;
	}
	fprintf(stderr, "no partition named \"%s\" found on this disk\n", want_name);
	return -1;
}

static int mkfs_vfat(const char *device)
{
	char *argv[] = { (char *)MKFS_VFAT_BIN, "-n", "ESP", (char *)device, NULL };

	return run_subprocess(MKFS_VFAT_BIN, argv);
}

static int mkfs_ext4(const char *device, const char *label)
{
	char *argv[] = { (char *)MKFS_EXT4_BIN, "-q", "-F", "-L", (char *)label, (char *)device, NULL };

	return run_subprocess(MKFS_EXT4_BIN, argv);
}

/* systemd-boot itself, the kernel, and root A's loader entry -- carrying
 * an initial tries-left counter, the same Automatic Boot Assessment
 * convention part 2 already proved, applied uniformly to the very first
 * install rather than treating it as a special pre-trusted case. */
static int populate_esp(const char *esp_mount)
{
	char path[512];
	char loader_conf[512];

	snprintf(path, sizeof(path), "%s/EFI", esp_mount);
	if (ensure_dir(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/EFI/BOOT", esp_mount);
	if (ensure_dir(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/EFI/BOOT/BOOTX64.EFI", esp_mount);
	if (copy_file(SYSTEMD_BOOT_EFI_SRC, path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/kanxeo-bzImage", esp_mount);
	if (copy_file(BZIMAGE_SRC, path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/loader", esp_mount);
	if (ensure_dir(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/loader/loader.conf", esp_mount);
	if (write_text_file(path, "default kanxeo-*\ntimeout 0\n") != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/loader/entries", esp_mount);
	if (ensure_dir(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/loader/entries/kanxeo-a+%d.conf", esp_mount, ROOT_A_TRIES);
	snprintf(loader_conf, sizeof(loader_conf),
	         "title Kanxeo (A)\n"
	         "sort-key kanxeo\n"
	         "version 1\n"
	         "linux /kanxeo-bzImage\n"
	         "options console=ttyS0 root=%s2 rw init=/bin/kanxeod -- --init-mode --slot=a\n",
	         BOOT_TIME_DISK_PREFIX);
	if (write_text_file(path, loader_conf) != 0)
		return -1;

	return 0;
}

/*
 * kanxeo-install runs as its own init= target, the same PID-1 boot shape
 * kanxeod's own --init-mode uses (daemon/src/main.c's boot_init()) --
 * needs the same minimal proc/sysfs mounts before cfdisk/sfdisk/mkfs.*
 * can be trusted to work at all. devtmpfs auto-populates /dev before
 * init ever runs (CONFIG_DEVTMPFS_MOUNT), so no /dev mount here either,
 * same as kanxeod's own boot_init().
 */
static int early_mounts(void)
{
	if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
		perror("/proc");
		return -1;
	}
	if (mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
		perror("/sys");
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *disk = NULL;
	const char *ip = NULL;
	const char *gateway = NULL;
	int prefix = -1;
	int skip_partition = 0;
	int i;
	struct stat st;
	char sfdisk_dump[16384];
	char esp_dev[64], root_a_dev[64], root_b_dev[64], config_dev[64], containers_dev[64];

	if (early_mounts() != 0)
		return 1;

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--disk=", 7) == 0)
			disk = argv[i] + 7;
		else if (strncmp(argv[i], "--ip=", 5) == 0)
			ip = argv[i] + 5;
		else if (strncmp(argv[i], "--gateway=", 10) == 0)
			gateway = argv[i] + 10;
		else if (strncmp(argv[i], "--prefix=", 9) == 0)
			prefix = atoi(argv[i] + 9);
		else if (strcmp(argv[i], "--skip-partition") == 0)
			skip_partition = 1;
	}

	if (disk == NULL || ip == NULL || gateway == NULL || prefix <= 0 || prefix > 32) {
		fprintf(stderr,
		        "usage: %s --disk=/dev/sdX --ip=A.B.C.D --prefix=N --gateway=A.B.C.D "
		        "[--skip-partition]\n",
		        argv[0]);
		return 2;
	}

	if (stat(disk, &st) != 0) {
		perror(disk);
		return 1;
	}

	printf("kanxeo-install: target disk %s -- ALL DATA ON THIS DISK WILL BE DESTROYED\n", disk);
	fflush(stdout);

	if (!skip_partition) {
		char *cfdisk_argv[] = { (char *)CFDISK_BIN, (char *)disk, NULL };

		if (run_subprocess(CFDISK_BIN, cfdisk_argv) != 0) {
			fprintf(stderr, "cfdisk did not complete successfully -- aborting\n");
			return 1;
		}
	}

	{
		char *dump_argv[] = { (char *)SFDISK_BIN, "-d", (char *)disk, NULL };

		if (run_subprocess_capture(SFDISK_BIN, dump_argv, sfdisk_dump, sizeof(sfdisk_dump)) != 0) {
			fprintf(stderr, "failed to read the partition table from %s\n", disk);
			return 1;
		}
	}

	if (find_partition_device(sfdisk_dump, "kanxeo-esp", esp_dev, sizeof(esp_dev)) != 0)
		return 1;
	if (find_partition_device(sfdisk_dump, "kanxeo-root-a", root_a_dev, sizeof(root_a_dev)) != 0)
		return 1;
	if (find_partition_device(sfdisk_dump, "kanxeo-root-b", root_b_dev, sizeof(root_b_dev)) != 0)
		return 1;
	if (find_partition_device(sfdisk_dump, "kanxeo-config", config_dev, sizeof(config_dev)) != 0)
		return 1;
	if (find_partition_device(sfdisk_dump, "kanxeo-containers", containers_dev,
	                           sizeof(containers_dev)) != 0)
		return 1;
	/* root_b_dev is intentionally never used beyond this existence
	 * check -- root B stays genuinely empty until the first real
	 * update ever writes there (this part's own confirmed scope). */
	(void)root_b_dev;

	printf("kanxeo-install: found all 5 partitions (esp=%s root-a=%s root-b=%s config=%s "
	       "containers=%s)\n",
	       esp_dev, root_a_dev, root_b_dev, config_dev, containers_dev);
	fflush(stdout);

	if (mkfs_vfat(esp_dev) != 0)
		return 1;
	if (mkfs_ext4(config_dev, "kanxeo-config") != 0)
		return 1;
	if (mkfs_ext4(containers_dev, "kanxeo-containers") != 0)
		return 1;

	if (ensure_dir(ESP_MOUNT) != 0)
		return 1;
	if (mount(esp_dev, ESP_MOUNT, "vfat", 0, NULL) != 0) {
		perror("mount esp");
		return 1;
	}
	if (populate_esp(ESP_MOUNT) != 0) {
		umount(ESP_MOUNT);
		return 1;
	}
	if (umount(ESP_MOUNT) != 0) {
		perror("umount esp");
		return 1;
	}

	if (write_whole_file_to_device(ROOT_SQUASHFS_SRC, root_a_dev) != 0)
		return 1;

	if (ensure_dir(CONFIG_MOUNT) != 0)
		return 1;
	if (mount(config_dev, CONFIG_MOUNT, "ext4", 0, NULL) != 0) {
		perror("mount config");
		return 1;
	}
	{
		char net_conf_path[600];
		char net_conf[256];

		snprintf(net_conf_path, sizeof(net_conf_path), "%s/net.conf", CONFIG_MOUNT);
		snprintf(net_conf, sizeof(net_conf), "ip=%s\nprefix=%d\ngateway=%s\n", ip, prefix, gateway);
		if (write_text_file(net_conf_path, net_conf) != 0) {
			umount(CONFIG_MOUNT);
			return 1;
		}
	}
	if (umount(CONFIG_MOUNT) != 0) {
		perror("umount config");
		return 1;
	}

	/* Containers partition: format-check only -- kanxeod's own existing
	 * ensure_dir()/pkg_init() machinery populates it at first real boot,
	 * the same "fall through to unmodified existing logic" precedent
	 * parts 1-2 already established for BASE_DIR's own subdirectories. */
	if (ensure_dir(CONTAINERS_MOUNT) != 0)
		return 1;
	if (mount(containers_dev, CONTAINERS_MOUNT, "ext4", 0, NULL) != 0) {
		perror("mount containers");
		return 1;
	}
	if (umount(CONTAINERS_MOUNT) != 0) {
		perror("umount containers");
		return 1;
	}

	printf("kanxeo-install: install complete\n");
	fflush(stdout);
	return 0;
}
