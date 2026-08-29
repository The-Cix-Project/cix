/*
 * cix-install: takes a raw disk (partitioned here with a fixed,
 * scripted layout -- or pre-partitioned by other tooling, see
 * --skip-partition) and produces the real Phase 11 layout:
 * ESP, root A, root B, config, containers (docs/roadmap/ROADMAP.md's Phase 11
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
 * apart: cix-esp, cix-root-a, cix-root-b, cix-config,
 * cix-containers.
 */
#include "dual_console.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define SFDISK_BIN "/usr/sbin/sfdisk"
#define MKFS_VFAT_BIN "/usr/sbin/mkfs.vfat"
#define MKFS_EXT4_BIN "/usr/sbin/mkfs.ext4"
#define MKFS_BTRFS_BIN "/usr/sbin/mkfs.btrfs"
#define MOKUTIL_BIN "/usr/bin/mokutil"

/* Bundled inside this installer's own bootable environment -- see
 * image/src/mkinstalleriso.c for how they get staged there. The kernel
 * lives under /boot/ rather than /payload/ since it serves double duty:
 * the same file GRUB itself boots as this installer's own kernel, and
 * the file copied here onto the target disk's ESP -- one copy, not two.
 *
 * The target ESP's Secure Boot chain (see populate_esp() and ADR-0015):
 * shim (Microsoft-signed, trusted with zero enrollment) chain-loads
 * whatever it finds at its own hardcoded "grubx64.efi" lookup -- here,
 * systemd-boot itself, Cix-signed and deliberately kept under that
 * name. mmx64.efi is MokManager (Debian-signed), auto-invoked by shim
 * once enroll_signing_key() below stages a pending enrollment request. */
#define SHIM_EFI_SRC "/payload/cix-shim.efi"
#define MOKMANAGER_EFI_SRC "/payload/cix-mm.efi"
#define SIGNED_SYSTEMD_BOOT_SRC "/payload/cix-grubx64.efi"
#define SIGNING_CERT_SRC "/payload/cix-signing.cer"
#define BZIMAGE_SRC "/boot/cix-bzImage"
#define ROOT_SQUASHFS_SRC "/payload/cix-root.squashfs"

#define ESP_MOUNT "/mnt/esp"
#define CONFIG_MOUNT "/mnt/config"
#define CONTAINERS_MOUNT "/mnt/containers"

#define ROOT_A_TRIES 3

/* The automatic layout's fixed sizes -- the same ones test/test_installer.c's
 * own create_target_disk() already uses and has proven correct (byte-for-
 * byte identical GPT names/types to what a real interactive fdisk/cfdisk
 * session produces, per find_partition_device()'s own read-back below,
 * which doesn't care how the table was written). Not configurable here: an
 * operator who needs different sizing changes it here, and a
 * running host has the REST partition API for everything after install --
 * which is where layout changes belong, since ADR-0190 leaves the
 * remainder of the disk unallocated precisely so they can be made there.
 *
 * These four sizes are the only thing a hand-partitioning operator could
 * ever have varied -- the STRUCTURE is fixed by find_partition_device()'s
 * read-back, which requires exactly these five names -- and none of them
 * is something an operator is better placed to choose than this constant
 * is. That is why ADR-0214 removed the interactive path entirely. */
#define AUTO_ESP_SIZE_MIB 64
#define AUTO_ROOT_SIZE_MIB 160
#define AUTO_CONFIG_SIZE_MIB 64
/*
 * Issue #104: the data partition is BOUNDED, and the rest of the disk
 * is left unallocated.
 *
 * It used to be "rest of disk", which quietly decided how the whole
 * machine's storage was carved up before its owner had said anything --
 * and left them no room to decide differently without destroying and
 * re-partitioning. The installer's job is to make the system bootable
 * and persistent, not to claim every byte it can see.
 *
 * The cap is a ceiling, not a fixed size: on a small disk, taking 16
 * GiB of a 20 GiB disk would be "the rest of the disk" wearing a
 * number, so the actual size is the smaller of this cap and half of
 * what remains after the system partitions. Whatever is left over is
 * the admin's, and growing the data partition into it later is a real,
 * supported operation (POST /disks/{disk}/partitions/{part}/resize).
 */
#define AUTO_DATA_MAX_MIB 16384

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
		dual_perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve(bin, argv, environ);
		dual_perror(bin);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid) {
		dual_perror("waitpid");
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		dual_printf("%s failed (status 0x%x)\n", bin, (unsigned)status);
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

/* Like run_subprocess(), but feeds script to the child's stdin -- used by
 * auto_partition() to drive sfdisk's own scripted-partition-table mode
 * (the same mechanism test/test_disk_image.c's own run_subprocess_stdin()
 * already uses host-side to pre-partition test disks; needed here too
 * now that cix-install can do the same partitioning itself). */
static int run_subprocess_stdin(const char *bin, char *const argv[], const char *script)
{
	int pipefd[2];
	pid_t pid;
	int status;
	size_t len = strlen(script);
	size_t written = 0;
	ssize_t n;

	if (pipe2(pipefd, O_CLOEXEC) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[0], STDIN_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execve(bin, argv, environ);
		dual_perror(bin);
		_exit(127);
	}
	close(pipefd[0]);
	while (written < len) {
		n = write(pipefd[1], script + written, len - written);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			dual_perror("write");
			break;
		}
		written += (size_t)n;
	}
	close(pipefd[1]);
	if (waitpid(pid, &status, 0) != pid) {
		dual_perror("waitpid");
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		dual_printf("%s failed (status 0x%x)\n", bin, (unsigned)status);
		return -1;
	}
	return 0;
}

static int ensure_dir(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		dual_perror(path);
		return -1;
	}
	return 0;
}

static int write_text_file(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");

	if (f == NULL) {
		dual_perror(path);
		return -1;
	}
	if (fputs(content, f) < 0) {
		dual_perror(path);
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
		dual_perror(src_path);
		return -1;
	}
	dst = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (dst < 0) {
		dual_perror(dst_path);
		close(src);
		return -1;
	}
	while ((n = read(src, buf, sizeof(buf))) > 0) {
		if (write(dst, buf, (size_t)n) != n) {
			dual_perror("write");
			close(src);
			close(dst);
			return -1;
		}
	}
	if (n < 0)
		dual_perror(src_path);
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
 * "/dev/vdb2 : start=..., size=..., type=..., name=\"cix-root-a\"".
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
	dual_printf("no partition named \"%s\" found on this disk\n", want_name);
	return -1;
}

static int mkfs_vfat(const char *device)
{
	char *argv[] = { (char *)MKFS_VFAT_BIN, "-n", "ESP", (char *)device, NULL };

	return run_subprocess(MKFS_VFAT_BIN, argv);
}

/*
 * with_quota enables ext4's real project-quota feature at mkfs time
 * (Part 4, bare-metal-readiness plan, ADR-0062) -- "-O quota -E
 * quotatype=prjquota" bakes the hidden quota inode and RO_COMPAT_QUOTA
 * feature bit directly into the filesystem, which the kernel then
 * honors automatically on every mount with no mount-time option
 * needed at all ("grpquota|noquota|quota|usrquota" are real, but
 * ignored no-ops on ext4's own mount(8) man page -- confirmed, not
 * assumed -- this is the modern journaled-quota-via-feature-bit
 * mechanism, not the legacy mount-option-driven one). Only the
 * containers partition needs this -- the ESP and cix-config
 * partitions never hold container overlay data, so quota tracking on
 * them would be real, unexercised scope.
 */
static int mkfs_ext4(const char *device, const char *label, int with_quota)
{
	if (with_quota) {
		char *argv[] = { (char *)MKFS_EXT4_BIN, "-q", "-F", "-L", (char *)label,
			          "-O", "quota", "-E", "quotatype=prjquota", (char *)device, NULL };

		return run_subprocess(MKFS_EXT4_BIN, argv);
	}
	{
		char *argv[] = { (char *)MKFS_EXT4_BIN, "-q", "-F", "-L", (char *)label,
			          (char *)device, NULL };

		return run_subprocess(MKFS_EXT4_BIN, argv);
	}
}

/*
 * ADR-0207 phase 4: btrfs is the install default for the platform's
 * own writable partitions -- the substrate the whole snapshot-rootfs +
 * idmapped-userns model runs on. No force flag: mkfs.btrfs always
 * overwrites an existing signature when run non-interactively
 * (confirmed in diskformat.c's own #103 investigation -- no "-F"
 * equivalent exists to pass). No quota flags either: btrfs quota is
 * qgroups, enabled at runtime by cixd's own cix_btrfs_qgroup_limit_
 * excl() per container, not a mkfs-time feature bit like ext4's
 * prjquota was. with_mixed ("--mixed", data+metadata block groups
 * combined) is btrfs-progs' own documented answer for a filesystem
 * this small -- cix-config is 64MiB, below the ~109MiB minimum a
 * separate-profile filesystem needs; the big containers partition
 * uses the normal separate profiles.
 */
static int mkfs_btrfs(const char *device, const char *label, int with_mixed)
{
	if (with_mixed) {
		char *argv[] = { (char *)MKFS_BTRFS_BIN, "-q", "-L", (char *)label,
			          "--mixed", (char *)device, NULL };

		return run_subprocess(MKFS_BTRFS_BIN, argv);
	}
	{
		char *argv[] = { (char *)MKFS_BTRFS_BIN, "-q", "-L", (char *)label,
			          (char *)device, NULL };

		return run_subprocess(MKFS_BTRFS_BIN, argv);
	}
}

/* The default: sfdisk, scripted, no operator interaction -- the same
 * mechanism (and the same GPT names/types/order) a real interactive fdisk
 * session produces.
 *
 * This is now the only path that writes a partition table (ADR-0214;
 * --skip-partition still accepts one prepared elsewhere). It replaced an
 * interactive fdisk session that was never the safer of the two, despite
 * feeling like it. cix-install reads roles back
 * from GPT partition NAMES (find_partition_device()), so an operator
 * driving fdisk by hand has to reproduce "cix-esp", "cix-root-a",
 * "cix-root-b", "cix-config" and "cix-containers" byte-for-byte, in order,
 * with the right types -- an exacting contract the fdisk UI says nothing
 * about, and whose failure surfaces much later, as a partition role that
 * cannot be found. Scripting it removes the one step where a typo is both
 * easy to make and expensive to discover. */
/*
 * The disk's own size in MiB, read from sysfs rather than computed from
 * anything this installer was told: /sys/class/block/<name>/size is in
 * 512-byte sectors, and it is the kernel's own answer for the device
 * actually present. 0 means "could not tell", which the caller treats
 * as a reason to stop rather than to guess.
 */
static long disk_size_mib(const char *disk)
{
	const char *base = strrchr(disk, '/');
	char path[256];
	char buf[64];
	FILE *f;
	long long sectors = 0;

	base = base != NULL ? base + 1 : disk;
	snprintf(path, sizeof(path), "/sys/class/block/%s/size", base);
	f = fopen(path, "r");
	if (f == NULL)
		return 0;
	if (fgets(buf, sizeof(buf), f) == NULL) {
		fclose(f);
		return 0;
	}
	fclose(f);
	sectors = atoll(buf);
	return (long)(sectors / 2048); /* 512-byte sectors -> MiB */
}

static int auto_partition(const char *disk)
{
	char script[400];
	char *sfdisk_argv[] = { (char *)SFDISK_BIN, (char *)disk, NULL };
	long total_mib = disk_size_mib(disk);
	long system_mib = AUTO_ESP_SIZE_MIB + 2 * AUTO_ROOT_SIZE_MIB + AUTO_CONFIG_SIZE_MIB;
	long remaining_mib, data_mib;

	if (total_mib <= 0) {
		fprintf(stderr, "cix-install: could not read the size of %s\n", disk);
		return -1;
	}
	/* A few MiB for the GPT itself at both ends, plus alignment slack.
	 * Being conservative here costs nothing and stops a layout that is
	 * arithmetically fine from failing at sfdisk. */
	remaining_mib = total_mib - system_mib - 8;
	if (remaining_mib < 64) {
		fprintf(stderr,
		        "cix-install: %s is %ld MiB; this layout needs at least %ld MiB "
		        "(ESP + two root slots + config + a data partition)\n",
		        disk, total_mib, system_mib + 8 + 64);
		return -1;
	}

	/* Issue #104: bounded, and never more than half of what is left --
	 * see AUTO_DATA_MAX_MIB. The remainder stays unallocated, for the
	 * admin to partition as they choose. */
	data_mib = remaining_mib / 2;
	if (data_mib > AUTO_DATA_MAX_MIB)
		data_mib = AUTO_DATA_MAX_MIB;

	printf("partitioning %s: %ld MiB total, %ld MiB system, %ld MiB data, %ld MiB left "
	       "unallocated for you to use\n",
	       disk, total_mib, system_mib, data_mib, remaining_mib - data_mib);

	snprintf(script, sizeof(script),
	         "label: gpt\n"
	         "size=%dMiB, type=uefi, name=\"cix-esp\"\n"
	         "size=%dMiB, type=linux, name=\"cix-root-a\"\n"
	         "size=%dMiB, type=linux, name=\"cix-root-b\"\n"
	         "size=%dMiB, type=linux, name=\"cix-config\"\n"
	         "size=%ldMiB, type=linux, name=\"cix-containers\"\n",
	         AUTO_ESP_SIZE_MIB, AUTO_ROOT_SIZE_MIB, AUTO_ROOT_SIZE_MIB, AUTO_CONFIG_SIZE_MIB,
	         data_mib);
	return run_subprocess_stdin(SFDISK_BIN, sfdisk_argv, script);
}

/* systemd-boot itself, the kernel, and root A's loader entry -- carrying
 * an initial tries-left counter, the same Automatic Boot Assessment
 * convention part 2 already proved, applied uniformly to the very first
 * install rather than treating it as a special pre-trusted case.
 *
 * The loader entry's own "--bind=<ip>" (cixd's real, working flag,
 * previously never passed here at all -- cixd silently fell back to
 * its 127.0.0.1-only default despite apply_static_ip() already having
 * configured this exact address on eth0) binds the *specific* address
 * just configured, not 0.0.0.0 -- cixd already knows the one real
 * address this install is for; no reason to listen on every interface
 * when exactly one is correct. */
static int populate_esp(const char *esp_mount, const char *ip)
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
	if (copy_file(SHIM_EFI_SRC, path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/EFI/BOOT/grubx64.efi", esp_mount);
	if (copy_file(SIGNED_SYSTEMD_BOOT_SRC, path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/EFI/BOOT/mmx64.efi", esp_mount);
	if (copy_file(MOKMANAGER_EFI_SRC, path) != 0)
		return -1;

	/*
	 * Per-slot kernel files, not one shared /cix-bzImage -- a future
	 * kernel update (like a root update already does) has to be able to
	 * write the *inactive* slot's own kernel file without touching
	 * whatever the currently-booted slot references, the same reasoning
	 * root A/B are already two separate partitions instead of one
	 * shared one. Both slots get the identical initial kernel here
	 * (a cheap, small copy) rather than leaving slot B's kernel file
	 * genuinely absent the way root B's own *content* deliberately
	 * stays empty -- unlike root, there's nothing to duplicate for free
	 * that would make emptiness meaningful, and an absent kernel file
	 * would fail an operator's first root-only update to slot B for a
	 * reason that has nothing to do with what they actually asked to
	 * change. See ADR-0032.
	 */
	snprintf(path, sizeof(path), "%s/cix-bzImage-a", esp_mount);
	if (copy_file(BZIMAGE_SRC, path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/cix-bzImage-b", esp_mount);
	if (copy_file(BZIMAGE_SRC, path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/loader", esp_mount);
	if (ensure_dir(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/loader/loader.conf", esp_mount);
	if (write_text_file(path, "default cix-*\ntimeout 0\n") != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/loader/entries", esp_mount);
	if (ensure_dir(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/loader/entries/cix-a+%d.conf", esp_mount, ROOT_A_TRIES);
	snprintf(loader_conf, sizeof(loader_conf),
	         "title Cix (A)\n"
	         "sort-key cix\n"
	         "version 1\n"
	         "linux /cix-bzImage-a\n"
	         "options console=tty0 console=ttyS0 root=%s2 rw init=/bin/cixd -- --init-mode "
	         "--slot=a --bind=%s\n",
	         BOOT_TIME_DISK_PREFIX, ip);
	if (write_text_file(path, loader_conf) != 0)
		return -1;

	return 0;
}

/*
 * Stages a MOK (Machine Owner Key) enrollment request for the Cix
 * signing key -- shim can already run the signed systemd-boot/kernel
 * chain populate_esp() just wrote, but only once that key is actually
 * trusted. mokutil --import writes the pending request into the
 * firmware's own persistent NVRAM (via efivarfs) and prompts here for a
 * one-time password; at the *installed* system's very next boot, shim
 * detects the pending request and auto-invokes MokManager (already
 * trusted, no enrollment of its own needed), which prompts once more
 * for that same password to confirm. After that single confirmation,
 * Secure Boot needs no further exceptions on this machine (ADR-0015) --
 * this installer's own boot stays unsigned/unenforced regardless, so
 * this step doesn't gate anything about the install itself completing.
 */
static int enroll_signing_key(void)
{
	if (mkdir("/sys/firmware/efi/efivars", 0755) != 0 && errno != EEXIST) {
		dual_perror("/sys/firmware/efi/efivars");
		return -1;
	}
	if (mount("efivarfs", "/sys/firmware/efi/efivars", "efivarfs", 0, NULL) != 0) {
		dual_perror("mount efivarfs");
		return -1;
	}

	dual_printf("cix-install: enrolling the Cix Secure Boot signing key -- choose a "
	            "temporary password now; you'll need it once more at the very next reboot, in "
	            "the blue MokManager screen, to confirm it\n");
	dual_printf("(mokutil may print \"Can't open /proc/keys\"/\"Failed to access kernel "
	            "trusted keyring\" below -- harmless in this minimal environment, the "
	            "enrollment request itself is still written correctly)\n");

	{
		char *argv[] = { (char *)MOKUTIL_BIN, "--import", (char *)SIGNING_CERT_SRC, NULL };

		return run_subprocess_dual_console(MOKUTIL_BIN, argv);
	}
}

/*
 * cix-install runs as its own init= target, the same PID-1 boot shape
 * cixd's own --init-mode uses (daemon/src/main.c's boot_init()) --
 * needs the same minimal proc/sysfs mounts before fdisk/sfdisk/mkfs.*
 * can be trusted to work at all. devtmpfs auto-populates /dev before
 * init ever runs (CONFIG_DEVTMPFS_MOUNT), so no /dev mount here either,
 * same as cixd's own boot_init(). devpts (ADR-0042) is the one
 * genuinely new mount here -- posix_openpt()'s slave device only shows
 * up under /dev/pts once that filesystem is actually mounted; nothing
 * else in this from-scratch environment ever does it.
 */
static int early_mounts(void)
{
	if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
		dual_perror("/proc");
		return -1;
	}
	if (mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
		dual_perror("/sys");
		return -1;
	}
	if (mkdir("/dev/pts", 0755) != 0 && errno != EEXIST) {
		dual_perror("/dev/pts");
		return -1;
	}
	if (mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
	          "mode=0620,ptmxmode=0666") != 0) {
		dual_perror("mount devpts");
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *disk = NULL;
	const char *ip = NULL;
	const char *gateway = NULL;
	const char *iface = NULL;
	int prefix = -1;
	int skip_partition = 0;
	int unknown_arg = 0;
	int i;
	struct stat st;
	char sfdisk_dump[16384];
	char esp_dev[64], root_a_dev[64], root_b_dev[64], config_dev[64], containers_dev[64];

	/* Literal first statement (ADR-0042) -- has no dependency on
	 * early_mounts() (devtmpfs already auto-populates /dev before init
	 * ever runs), and every diagnostic below this line should reach
	 * whichever console the operator is actually watching. */
	dual_console_open("/dev/tty0", "/dev/ttyS0");

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
		else if (strncmp(argv[i], "--interface=", 12) == 0)
			iface = argv[i] + 12;
		else if (strcmp(argv[i], "--skip-partition") == 0)
			skip_partition = 1;
		else if (strcmp(argv[i], "--") != 0) {
			/*
			 * Anything unrecognised is fatal, and that is a safety
			 * property, not tidiness. This loop used to ignore what it
			 * did not know, which meant a mistyped "--skip-partiton"
			 * silently became "partition this disk" -- the installer
			 * destroying a disk the operator was explicitly trying to
			 * preserve, with no diagnostic anywhere. The one flag whose
			 * typo is most costly was the one least protected.
			 *
			 * A bare "--" is tolerated because the kernel command line
			 * carries one (init=/bin/cix-install -- --disk=...) and
			 * whether it reaches init's argv is the bootloader's
			 * business, not something to be strict about.
			 */
			dual_printf("cix-install: unrecognised argument: %s\n", argv[i]);
			unknown_arg = 1;
		}
	}

	if (unknown_arg || disk == NULL || ip == NULL || gateway == NULL || iface == NULL ||
	    prefix <= 0 || prefix > 32) {
		dual_printf("usage: %s --disk=/dev/sdX --ip=A.B.C.D --prefix=N --gateway=A.B.C.D "
		            "--interface=IFNAME [--skip-partition]\n"
		            "  (--interface=: the physical NIC to bind the management IP to, e.g.\n"
		            "  eth0 -- see `ip link`/`ls /sys/class/net` from a rescue shell if\n"
		            "  unsure. The disk is partitioned here, with the standard layout,\n"
		            "  sized to the disk and leaving the remainder unallocated for you to\n"
		            "  use afterwards. --skip-partition: the disk is already partitioned\n"
		            "  by other means -- it must carry five GPT partitions named exactly\n"
		            "  cix-esp, cix-root-a, cix-root-b, cix-config and cix-containers,\n"
		            "  since roles are read back from those names.)\n",
		            argv[0]);
		return 2;
	}

	if (stat(disk, &st) != 0) {
		dual_perror(disk);
		return 1;
	}

	dual_printf("cix-install: target disk %s -- ALL DATA ON THIS DISK WILL BE DESTROYED\n", disk);

	if (!skip_partition) {
		if (auto_partition(disk) != 0) {
			dual_printf("partitioning did not complete successfully -- aborting\n");
			return 1;
		}
	}

	{
		char *dump_argv[] = { (char *)SFDISK_BIN, "-d", (char *)disk, NULL };

		if (run_subprocess_capture(SFDISK_BIN, dump_argv, sfdisk_dump, sizeof(sfdisk_dump)) != 0) {
			dual_printf("failed to read the partition table from %s\n", disk);
			return 1;
		}
	}

	if (find_partition_device(sfdisk_dump, "cix-esp", esp_dev, sizeof(esp_dev)) != 0)
		return 1;
	if (find_partition_device(sfdisk_dump, "cix-root-a", root_a_dev, sizeof(root_a_dev)) != 0)
		return 1;
	if (find_partition_device(sfdisk_dump, "cix-root-b", root_b_dev, sizeof(root_b_dev)) != 0)
		return 1;
	if (find_partition_device(sfdisk_dump, "cix-config", config_dev, sizeof(config_dev)) != 0)
		return 1;
	if (find_partition_device(sfdisk_dump, "cix-containers", containers_dev,
	                           sizeof(containers_dev)) != 0)
		return 1;
	/* root_b_dev is intentionally never used beyond this existence
	 * check -- root B stays genuinely empty until the first real
	 * update ever writes there (this part's own confirmed scope). */
	(void)root_b_dev;

	dual_printf("cix-install: found all 5 partitions (esp=%s root-a=%s root-b=%s config=%s "
	            "containers=%s)\n",
	            esp_dev, root_a_dev, root_b_dev, config_dev, containers_dev);

	if (mkfs_vfat(esp_dev) != 0)
		return 1;
	if (mkfs_btrfs(config_dev, "cix-config", 1) != 0)
		return 1;
	/* Full "cix-containers" fits now -- the old mkfs_ext4() call here
	 * had to truncate to "cix-container" for EXT2_LABEL_LEN (16);
	 * btrfs labels carry 255. Cosmetic either way: partitions are
	 * always identified by GPT name (find_partition_device()), never
	 * this label. */
	if (mkfs_btrfs(containers_dev, "cix-containers", 0) != 0)
		return 1;

	if (ensure_dir(ESP_MOUNT) != 0)
		return 1;
	if (mount(esp_dev, ESP_MOUNT, "vfat", 0, NULL) != 0) {
		dual_perror("mount esp");
		return 1;
	}
	if (populate_esp(ESP_MOUNT, ip) != 0) {
		umount(ESP_MOUNT);
		return 1;
	}
	if (umount(ESP_MOUNT) != 0) {
		dual_perror("umount esp");
		return 1;
	}

	if (enroll_signing_key() != 0)
		return 1;

	if (write_whole_file_to_device(ROOT_SQUASHFS_SRC, root_a_dev) != 0)
		return 1;

	if (ensure_dir(CONFIG_MOUNT) != 0)
		return 1;
	if (mount(config_dev, CONFIG_MOUNT, "btrfs", 0, NULL) != 0) {
		dual_perror("mount config");
		return 1;
	}
	{
		char net_conf_path[600];
		char net_conf[320];

		snprintf(net_conf_path, sizeof(net_conf_path), "%s/net.conf", CONFIG_MOUNT);
		snprintf(net_conf, sizeof(net_conf), "ip=%s\nprefix=%d\ngateway=%s\ninterface=%s\n", ip, prefix,
		         gateway, iface);
		if (write_text_file(net_conf_path, net_conf) != 0) {
			umount(CONFIG_MOUNT);
			return 1;
		}
	}
	if (umount(CONFIG_MOUNT) != 0) {
		dual_perror("umount config");
		return 1;
	}

	/* Containers partition: cixd's own existing ensure_dir()/
	 * pkg_init()/image_create() machinery populates all of it at first
	 * real boot -- the same "fall through to unmodified existing logic"
	 * precedent parts 1-2 established for BASE_DIR's own subdirectories.
	 *
	 * ADR-0210: this block used to copy a C runtime (ld.so, libc.so.6,
	 * libtinfo.so.6) into images/base/rootfs, on the stated grounds
	 * that nothing else ever would. That stopped being true, and then
	 * stopped being read at all: ADR-0107/0108 made images versioned
	 * (<images>/<name>/<version>/rootfs), and container creation
	 * resolves strictly through image_current_version() -- so the flat
	 * path this wrote was consulted by nothing. Verified directly, not
	 * inferred: a fresh daemon reported no images at all and refused a
	 * container on "base" with "image rootfs does not exist", proving
	 * the seeding was not what made the default image work.
	 *
	 * ensure_default_image() in the daemon now creates it the same way
	 * every other image is created, with the same baseline. The
	 * partition itself still needs to exist and be mountable, which is
	 * all this does now. */
	if (ensure_dir(CONTAINERS_MOUNT) != 0)
		return 1;
	if (mount(containers_dev, CONTAINERS_MOUNT, "btrfs", 0, NULL) != 0) {
		dual_perror("mount containers");
		return 1;
	}
	if (umount(CONTAINERS_MOUNT) != 0) {
		dual_perror("umount containers");
		return 1;
	}

	dual_printf("cix-install: install complete\n");
	dual_printf("\n");
	dual_printf("=====================================================================\n");
	dual_printf("  Installation complete. Remove the installation media, then press\n");
	dual_printf("  Enter to reboot into the newly-installed system.\n");
	dual_printf("=====================================================================\n");

	/*
	 * Without this, cix-install (running as PID 1) simply returns
	 * from main() here -- the kernel's response to init exiting is an
	 * immediate, unprompted panic, which read to a real operator as the
	 * installer silently freezing rather than finishing (found live,
	 * reported directly: "libkeyutils..." warnings during MOK
	 * enrollment above, then this final silence, with nothing telling
	 * them it was actually done). dual_console_wait_for_key() blocks on
	 * whichever console the operator is actually watching; reboot(2)
	 * only returns on failure, in which case the panic above is at
	 * least no worse than the prior behavior.
	 */
	dual_console_wait_for_key();
	sync();
	reboot(RB_AUTOBOOT);
	dual_perror("reboot");
	return 1;
}
