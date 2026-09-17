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
#include "bootmodules.h"
#include "kmod.h"
#include "partlabel.h"
#include "netconf.h"
#include "nicreport.h"
#include "treecopy.h"

#include <dirent.h>
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
/* The package seed (#189), when the media carries one -- recipes and
 * artifacts copied onto the containers partition so the daemon has a
 * package source at first boot, before any network exists. Absent on
 * media built without a seed, which is a supported state, not an
 * error. */
#define SEED_SRC "/payload/seed"

#define SHIM_EFI_SRC "/payload/cix-shim.efi"
#define MOKMANAGER_EFI_SRC "/payload/cix-mm.efi"
/* The Cix-signed boot manager, staged by mkinstalleriso. "grubx64.efi"
 * is shim's own fixed name for the second stage it chainloads, not a
 * statement about which program that is -- it was systemd-boot and is
 * cix-boot now (ADR-0215). */
#define SIGNED_BOOT_MANAGER_SRC "/payload/cix-grubx64.efi"
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
/*
 * cix-config was 64 MiB, which was sized for what it held: one file,
 * net.conf, using 15 KB of it.
 *
 * That was always too small for what the partition is FOR. The layout
 * implies three tiers -- immutable roots, host configuration, container
 * data -- and the configuration tier ended up holding a single file
 * while the platform's own definition of itself (networks, DNS, DHCP,
 * NTP, LDAP, syslog targets, the CA and every issued certificate,
 * container definitions, device maps, site identity) lived on the
 * containers partition instead. Moving that where it belongs needs room
 * to be there.
 *
 * 512 MiB rather than 256: PKI is the one part with real growth, a file
 * per issued certificate, and this is sized once at install and can
 * never be enlarged afterwards without repartitioning the OS disk. The
 * extra 448 MiB is noise against any disk this platform installs on,
 * and it takes the question off the table permanently.
 *
 * Side effect worth knowing: at this size the filesystem no longer
 * needs btrfs --mixed (see mkfs_btrfs()), which existed only because
 * 64 MiB is under the ~109 MiB minimum for separate data/metadata
 * profiles.
 */
#define AUTO_CONFIG_SIZE_MIB 512
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
 * There is deliberately no compiled-in device prefix here any more
 * (#305). The loader entry names the root partition by PARTUUID, read
 * from the GPT this installer just wrote, and every other partition is
 * found by its GPT label. Nothing in the installed system assumes what
 * the kernel will call the disk next boot.
 */

/*
 * Relay a child's merged stdout+stderr onto both consoles.
 *
 * This exists because a child's output was invisible on real hardware.
 * The installer boots with `console=tty0 console=ttyS0`, and when more
 * than one console= is given the kernel points /dev/console at the LAST
 * one -- so every tool this installer runs wrote to the serial port.
 * A bare-metal machine plugged into a monitor has no serial port, and
 * the operator saw nothing at all: an sfdisk that failed reported only
 * our own "sfdisk failed (status 0x100)", with sfdisk's own explanation
 * of WHY discarded. Measured on a real install, 2026-09-12, where that
 * left the failure undiagnosable.
 *
 * dual_printf() is what makes our own messages visible, because it
 * writes to /dev/tty0 and /dev/ttyS0 explicitly rather than trusting
 * fd 1. A child cannot do that, so its output is piped back here and
 * put through the same function -- prefixed, so it is obvious which
 * lines are the tool's rather than the installer's.
 */
static void relay_child_output(int fd, const char *bin)
{
	const char *base = strrchr(bin, '/');
	char buf[512];
	size_t held = 0;
	ssize_t n;

	base = (base != NULL) ? base + 1 : bin;
	while ((n = read(fd, buf + held, sizeof(buf) - 1 - held)) != 0) {
		char *line;
		char *nl;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		held += (size_t)n;
		buf[held] = '\0';
		line = buf;
		while ((nl = strchr(line, '\n')) != NULL) {
			*nl = '\0';
			dual_printf("  [%s] %s\n", base, line);
			line = nl + 1;
		}
		/* Carry a partial last line over into the next read. */
		held = strlen(line);
		memmove(buf, line, held + 1);
		if (held == sizeof(buf) - 1) {
			dual_printf("  [%s] %s\n", base, buf);
			held = 0;
		}
	}
	if (held > 0)
		dual_printf("  [%s] %s\n", base, buf);
}

static int run_subprocess(const char *bin, char *const argv[])
{
	pid_t pid;
	int status;
	int out_pipe[2];

	if (pipe2(out_pipe, O_CLOEXEC) != 0) {
		dual_perror("pipe2");
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		dual_perror("fork");
		close(out_pipe[0]);
		close(out_pipe[1]);
		return -1;
	}
	if (pid == 0) {
		/* Both streams down the pipe: a tool's diagnosis is usually on
		 * stderr, and it is the half that matters most here. */
		dup2(out_pipe[1], STDOUT_FILENO);
		dup2(out_pipe[1], STDERR_FILENO);
		close(out_pipe[0]);
		close(out_pipe[1]);
		execve(bin, argv, environ);
		dual_perror(bin);
		_exit(127);
	}
	close(out_pipe[1]);
	relay_child_output(out_pipe[0], bin);
	close(out_pipe[0]);
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
	int out_pipe[2];
	pid_t pid;
	int status;
	size_t len = strlen(script);
	size_t written = 0;
	ssize_t n;

	if (pipe2(pipefd, O_CLOEXEC) != 0)
		return -1;
	if (pipe2(out_pipe, O_CLOEXEC) != 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[0], STDIN_FILENO);
		dup2(out_pipe[1], STDOUT_FILENO);
		dup2(out_pipe[1], STDERR_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		execve(bin, argv, environ);
		dual_perror(bin);
		_exit(127);
	}
	close(pipefd[0]);
	close(out_pipe[1]);
	/*
	 * The whole script first, then the output. Safe in this order only
	 * because the script is small -- a few hundred bytes against a
	 * 64 KiB pipe buffer -- so the write cannot block waiting for a
	 * child that is itself blocked writing output nobody is reading
	 * yet. A larger script would need both pipes polled together.
	 */
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
	relay_child_output(out_pipe[0], bin);
	close(out_pipe[0]);
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
 * idmapped-userns model runs on.
 *
 * FORCES (-f), and the comment that used to stand here is why it has
 * to. It claimed "No force flag: mkfs.btrfs always overwrites an
 * existing signature when run non-interactively (confirmed in
 * diskformat.c's own #103 investigation -- no \"-F\" equivalent
 * exists to pass)". Every part of that was false, and diskformat.c had
 * already found and corrected the same false claim in its own copy --
 * see its mkfs comment, which quotes btrfs-progs' own option table
 * (OPTLINE("-f, --force", ...)) and its test_dev_for_mkfs(file,
 * force_overwrite), which refuses outright when a filesystem is
 * present and force is false. The flag is lowercase -f; -F is ext4's
 * spelling, which is what made "no -F equivalent" read as true.
 *
 * Measured on real bare metal, 2026-09-12: installing onto an
 * nvme0n1 that already carried a Windows layout partitioned cleanly
 * and then died at "/dev/nvme0n1p4 appears to contain an existing
 * filesystem ... ERROR: use the -f option to force overwrite",
 * mkfs.btrfs exiting 0x100. sfdisk writes a new table but does not
 * erase filesystem signatures inside the extents it hands out, so any
 * install over a used disk hits this -- which is most of them. The
 * operator has already confirmed the partitioning by this point; that
 * confirmation is exactly what -f expresses.
 *
 * No quota flags: btrfs quota is
 * qgroups, enabled at runtime by cixd's own cix_btrfs_qgroup_limit_
 * excl() per container, not a mkfs-time feature bit like ext4's
 * prjquota was. with_mixed ("--mixed", data+metadata block groups
 * combined) is btrfs-progs' own documented answer for a filesystem
 * this small -- a filesystem below the ~109MiB minimum a
 * separate-profile filesystem needs. No caller passes it any more:
 * cix-config moved to 512 MiB, which is above that threshold, and the
 * big containers partition always used the normal separate profiles.
 * Kept because the parameter is the honest way to express "this
 * filesystem may be tiny", and a future small partition would need it.
 */
static int mkfs_btrfs(const char *device, const char *label, int with_mixed)
{
	if (with_mixed) {
		char *argv[] = { (char *)MKFS_BTRFS_BIN, "-q", "-f", "-L", (char *)label,
			          "--mixed", (char *)device, NULL };

		return run_subprocess(MKFS_BTRFS_BIN, argv);
	}
	{
		char *argv[] = { (char *)MKFS_BTRFS_BIN, "-q", "-f", "-L", (char *)label,
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
		dual_printf("cix-install: could not read the size of %s\n", disk);
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

	dual_printf("partitioning %s: %ld MiB total, %ld MiB system, %ld MiB data, %ld MiB left "
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

/* panic=10 on the kernel command line: a panicking kernel halts rather
 * than reboots, so without it a slot that dies during boot spends one
 * try and then waits for a human. On a host with no console and no BMC
 * that is an outage, not a recovery path -- ADR-0014's counted A/B
 * assessment only drains its counter across actual boot attempts.
 *
 * systemd-boot itself, the kernel, and root A's loader entry -- carrying
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
static int populate_esp(const char *esp_mount, const char *ip, const char *root_a_dev)
{
	char path[512];
	char loader_conf[512];
	char root_partuuid[64];

	/*
	 * The root= this writes is the single line that decides whether
	 * the installed machine ever boots, and it used to be built from
	 * a compiled-in "/dev/vda" (#305). The reasoning recorded beside
	 * that constant was that the target becomes /dev/vda once the
	 * installer's own media is gone -- true only while the disk is
	 * virtio. Move the same disk to SATA and it comes back as sda:
	 * the kernel finds sda1..sda5, the entry still says vda2, and a
	 * complete, correct install panics in a reboot loop.
	 *
	 * PARTUUID is the kernel's own answer and survives any renaming.
	 * It is what the kernel prints beside each partition when it
	 * cannot find a root, and root=PARTUUID= is understood by its
	 * built-in parser -- root=PARTLABEL= is not, which is why the GPT
	 * label that identifies every other partition here cannot be used
	 * for this one.
	 */
	if (partlabel_uuid_for_device(root_a_dev, root_partuuid, sizeof(root_partuuid)) != 0) {
		dual_printf("could not read the PARTUUID of %s -- refusing to write a boot entry "
		            "that names a device path\n",
		            root_a_dev);
		return -1;
	}
	dual_printf("root partition %s is PARTUUID=%s\n", root_a_dev, root_partuuid);

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
	if (copy_file(SIGNED_BOOT_MANAGER_SRC, path) != 0)
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
	/*
	 * No --bind= in the entry (#443). The management address belongs to
	 * net.conf, which the daemon reads at boot and which its listener
	 * binds (ADR-0284). This used to write the address here as well,
	 * and that second copy is what cost a real box: PUT /v1/system/
	 * management-network updated net.conf and the network registry, the
	 * loader entry kept the old address, and because the listener bound
	 * the argv value rather than net.conf's, the next boot tried an
	 * address that was no longer on the bridge -- EADDRNOTAVAIL, and as
	 * PID 1 that is issue #131's park banner with the console shell
	 * gone along with it.
	 *
	 * Nothing is lost by dropping it. It was never usable as a
	 * deliberate boot-time override, because cix-boot (ADR-0215) has no
	 * edit-at-menu; and a box whose net.conf names an address it cannot
	 * bind now comes up answering on 127.0.0.1 with a working console
	 * shell, so the recovery this copy might have served does not need
	 * it. The --bind= FLAG remains for test and dev invocations, which
	 * have no net.conf at all.
	 *
	 * Installing with no management address stays a deliberate,
	 * supported state: net.conf is simply not written (see the populate
	 * step), bootstrap_management_network() finds nothing to do, and
	 * cixd's own DEFAULT_BIND answers on loopback -- which boot_init()
	 * brings up precisely so that bind can succeed.
	 */
	snprintf(loader_conf, sizeof(loader_conf),
	         "title Cix (A)\n"
	         "sort-key cix\n"
	         "version 1\n"
	         "linux /cix-bzImage-a\n"
	         /* ADR-0246: cixd is init again until a restarted worker is
	          * proven to come up -- see the daemon's own entry writer
	          * for the measurement that reverted this. */
	         "options console=tty0 console=ttyS0 root=PARTUUID=%s rw panic=10 init=/bin/cixd "
	         "-- --init-mode --slot=a\n",
	         root_partuuid);
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
/*
 * Is the firmware actually enforcing Secure Boot?
 *
 * The EFI variable is SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c,
 * whose data is a single byte: 1 when enforcing. efivarfs prefixes every
 * variable's contents with a 4-byte attributes word, so the byte we want
 * is at offset 4 -- reading offset 0 gets the attributes and is the easy
 * mistake here.
 *
 * "Cannot tell" is deliberately reported as NOT enforcing. This gates
 * whether the installer demands a MOK password, and being unable to read
 * a variable is not evidence that Secure Boot is on; guessing "on" would
 * reintroduce exactly the prompt this is meant to avoid, on machines that
 * can never use it.
 */
static int secure_boot_enforcing(void)
{
	static const char *path =
	        "/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c";
	unsigned char buf[5];
	FILE *f = fopen(path, "rb");
	size_t n;

	if (f == NULL)
		return 0;
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if (n < 5)
		return 0;
	return buf[4] == 1;
}

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

/*
 * Ask the operator, rather than make them get it right at a GRUB prompt.
 *
 * Everything the installer needs used to be required on the kernel
 * command line -- disk, address, prefix, gateway, interface -- typed
 * blind, with no list of what the machine actually has and no feedback
 * until the installer refused to start. The flags all still work, and
 * are what an unattended install uses; what changes is that leaving one
 * out is now a question instead of a usage error.
 */
static void prompt_default(const char *question, const char *dflt, char *out, size_t out_size)
{
	char buf[256];
	int n;

	for (;;) {
		if (dflt != NULL && dflt[0] != '\0')
			dual_printf("%s [%s]: ", question, dflt);
		else
			dual_printf("%s: ", question);
		n = dual_console_readline(buf, sizeof(buf));
		if (n < 0) {
			/* No console left to ask on. Take the default if there is
			 * one; the caller validates either way. */
			snprintf(out, out_size, "%s", dflt != NULL ? dflt : "");
			return;
		}
		if (n == 0) {
			if (dflt != NULL && dflt[0] != '\0') {
				snprintf(out, out_size, "%s", dflt);
				return;
			}
			dual_printf("  (a value is needed here)\n");
			continue;
		}
		snprintf(out, out_size, "%s", buf);
		return;
	}
}

/*
 * The disks this machine actually has, listed for the operator to choose
 * from.
 *
 * Reads /sys/class/block directly rather than shelling out: this
 * environment carries three binaries and none of them lists disks. A
 * whole-disk entry is one with a "device" symlink (partitions have none)
 * -- the same distinction the daemon's own enumerator draws.
 */
static int pick_disk(char *out, size_t out_size)
{
	char names[32][64];
	long sizes[32];
	int count = 0;
	DIR *d = opendir("/sys/class/block");
	struct dirent *e;
	char answer[64];

	if (d == NULL) {
		dual_perror("/sys/class/block");
		return -1;
	}
	while ((e = readdir(d)) != NULL && count < 32) {
		char probe[256];
		struct stat pst;

		if (e->d_name[0] == '.')
			continue;
		snprintf(probe, sizeof(probe), "/sys/class/block/%s/device", e->d_name);
		if (stat(probe, &pst) != 0)
			continue; /* a partition, or not a real device */
		/*
		 * Never offer read-only media as an install target. The
		 * installer boots from a CD-ROM, which is a whole block device
		 * with a "device" link like any other -- so without this the
		 * media you are running from appears in the list of disks to
		 * erase. /sys/class/block/<name>/ro is the kernel's own answer.
		 */
		snprintf(probe, sizeof(probe), "/sys/class/block/%s/ro", e->d_name);
		{
			FILE *rf = fopen(probe, "r");
			char rb[8] = { 0 };

			if (rf != NULL) {
				if (fgets(rb, sizeof(rb), rf) != NULL && rb[0] == '1') {
					fclose(rf);
					continue;
				}
				fclose(rf);
			}
		}
		snprintf(names[count], sizeof(names[count]), "%s", e->d_name);
		snprintf(probe, sizeof(probe), "/dev/%s", e->d_name);
		sizes[count] = disk_size_mib(probe);
		count++;
	}
	closedir(d);

	if (count == 0) {
		dual_printf("cix-install: no disks found under /sys/class/block\n");
		return -1;
	}

	dual_printf("\nDisks on this machine:\n");
	for (int i = 0; i < count; i++)
		dual_printf("  %d) /dev/%-8s %6ld MiB (%.1f GiB)\n", i + 1, names[i], sizes[i],
		            (double)sizes[i] / 1024.0);
	dual_printf("\n");

	for (;;) {
		int choice;

		prompt_default("Install to which disk? (number)", count == 1 ? "1" : "", answer,
		               sizeof(answer));
		choice = atoi(answer);
		if (choice < 1 || choice > count) {
			dual_printf("  (choose 1 to %d)\n", count);
			continue;
		}
		snprintf(out, out_size, "/dev/%s", names[choice - 1]);

		dual_printf("\n  %s (%ld MiB) will be COMPLETELY ERASED -- every partition and all "
		            "data on it.\n",
		            out, sizes[choice - 1]);
		prompt_default("  Type ERASE to confirm", "", answer, sizeof(answer));
		if (strcmp(answer, "ERASE") == 0)
			return 0;
		dual_printf("  not confirmed -- choose again\n");
	}
}

/*
 * Load the NIC drivers, so the list below can show a real machine's
 * built-in Ethernet.
 *
 * The five chipsets are kernel modules rather than built in (see
 * image/kernel/qemu-part1.config), which is fine for the installed
 * system -- cixd modprobes exactly this list at boot, from the same
 * header -- and was fatal here: this installer used to carry no module
 * tree and no module tools, so the ONLY interfaces it could ever list
 * were the built-in drivers, meaning virtio_net. A real machine with a
 * built-in NIC and no virtio therefore showed "(none found)", and the
 * operator had nothing to choose. Measured on a bare-metal attempt,
 * 2026-09-12; the ISO has staged the drivers and modprobe since that
 * same day (#429).
 *
 * WHAT MODPROBE SAID IS NOW KEPT, and #442 is why (see
 * include/nicreport.h for the full account). This used to call the
 * failures "ordinary" and discard the text, on the reasoning that no
 * machine has all five chipsets so most loads do nothing useful. The
 * second half is true and the first does not follow from it. Measured
 * on 192.168.15.95, a virtio VM with no Broadcom hardware:
 * `cixctl kmod load tg3` returns 0 and the module goes Live with
 * used_by=0. modprobe SUCCEEDS on a machine that lacks the chipset --
 * it loads a driver, it does not require a device. So with a correct
 * module tree all five loads return 0 everywhere, and a non-zero is
 * always a real defect in the media: no tree, a tree for another
 * kernel release, or one built from another config. That text is the
 * only thing that tells those apart, and throwing it away is what left
 * #442 with no cause to name.
 *
 * The absence of module tools is reported rather than silently
 * tolerated: it is the difference between "this machine has no NIC the
 * platform supports" and "this media cannot look", and an operator
 * staring at an empty list deserves to know which.
 */
static void load_nic_modules(struct nic_load_result *res)
{
	static const char *const mods[] = CIX_NIC_MODULES;
	struct stat st;
	size_t i;

	memset(res, 0, sizeof(*res));
	res->attempted = (int)(sizeof(mods) / sizeof(mods[0]));
	if (stat(KMOD_MODPROBE_BIN, &st) != 0) {
		dual_printf("\n  (this media carries no module tools, so only NIC drivers built into\n");
		dual_printf("   the kernel can appear below -- see #429)\n");
		res->attempted = 0;
		return;
	}
	res->tools_present = 1;
	dual_printf("\nLoading NIC drivers");
	for (i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
		char out[256] = "";

		dual_printf(" %s", mods[i]);
		if (kmod_load(mods[i], NULL, out, sizeof(out)) == 0) {
			res->loaded++;
			continue;
		}
		res->failed++;
		/* The FIRST error, not the last: they are nearly always the
		 * same cause repeated five times, and the first one is the
		 * one the operator reads before the screen scrolls. */
		if (res->first_error[0] == '\0')
			snprintf(res->first_error, sizeof(res->first_error), "%s: %s", mods[i],
			         out[0] != '\0' ? out : "failed with no message");
	}
	dual_printf(" -- done\n");
	/*
	 * Printed here, next to the loads, rather than folded into the
	 * empty-list note below: a load can fail on a machine that then
	 * still lists a NIC (four chipsets absent, one built in), and that
	 * is a media defect worth seeing even when the operator has
	 * something to choose.
	 */
	if (res->failed > 0) {
		dual_printf("  %d of %d driver loads FAILED -- %s\n", res->failed, res->attempted,
		            res->first_error);
		dual_printf("  (this is a defect in the media, not in this machine)\n");
	}
}

/*
 * The NICs this machine has, so an operator does not have to guess a
 * name. Virtual interfaces are skipped: /sys/class/net/<if>/device only
 * exists for a real one, which is the same test used elsewhere.
 *
 * `lo` is listed too, and deliberately, even though it fails that test.
 * It is a legitimate answer -- "install this box now, commit to an
 * address later" -- and it is the ONLY answer available on a machine
 * whose real NIC is not listed here at all. Hiding the one choice that
 * always works, on the screen where the operator needs a choice that
 * works, was the gap.
 *
 * This comment used to give the reason an empty list happens as "the
 * NIC drivers are kernel modules and this installer carries no module
 * tree, so only built-in drivers (virtio_net) produce an interface".
 * That stopped being true the day it was written (#429 staged the
 * drivers and modprobe the same afternoon), and a near-identical
 * sentence was PRINTED ON SCREEN above this list -- which is how a
 * real bare-metal install that showed only `lo` came with a confident
 * explanation of a mechanism that no longer existed, and how #442
 * ended up filed as "cause not established". There are now three
 * distinguishable reasons and nicreport_no_nic_reason() picks between
 * them from what the loads actually did.
 *
 * lo is listed LAST and labelled, so it reads as the fallback it is
 * rather than as a candidate for a machine that has a real NIC here.
 */
static void list_interfaces(char *first, size_t first_size, const struct nic_load_result *loads)
{
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;
    int n = 0;

	first[0] = '\0';
	if (d == NULL)
		return;
	dual_printf("\nNetwork interfaces:\n");
	while ((e = readdir(d)) != NULL) {
		char probe[256];
		struct stat pst;

		if (e->d_name[0] == '.' || strcmp(e->d_name, "lo") == 0)
			continue; /* lo is printed after the loop, labelled */
		snprintf(probe, sizeof(probe), "/sys/class/net/%s/device", e->d_name);
		if (stat(probe, &pst) != 0)
			continue;
		dual_printf("  %s\n", e->d_name);
		if (first[0] == '\0')
			snprintf(first, first_size, "%s", e->d_name);
		n++;
	}
	closedir(d);
	if (n == 0) {
		char why[640];

		dual_printf("  %s\n", nicreport_no_nic_reason(loads, n, why, sizeof(why)));
	}
	dual_printf("  lo                (loopback, 127.0.0.1 -- install now, set the real\n");
	dual_printf("                     address later)\n");
	/*
	 * The default offered at the prompt stays the first REAL NIC when
	 * there is one: an operator with an e1000 in front of them should
	 * be able to press Enter. lo becomes the default only when nothing
	 * real was found, which is the case where it is also the only
	 * workable answer.
	 */
	if (first[0] == '\0')
		snprintf(first, first_size, "lo");
	dual_printf("\n");
}

/*
 * Does cix-config already carry a CA? (#415, ADR-0281)
 *
 * PKI state lives at /config/state/pki, and this installer formats the
 * partition it lives on -- so a reinstall destroyed the trust root and
 * every certificate under it. ADR-0281 gives an operator a way to carry
 * one across deliberately (cixctl pki export), and this covers the
 * operator who did not: if the partition already holds a CA, it is kept
 * rather than formatted.
 *
 * Mounted READ-ONLY, and the answer is a single stat. The installer is
 * the one component whose failure mode is "the box does not boot", so it
 * reads nothing it does not need and writes nothing here at all.
 *
 * A mount failure means NOT preserved, deliberately: a fresh disk has no
 * filesystem to mount, and a partition whose geometry moved has garbage
 * at the new offset. Both must be formatted, and both present as "mount
 * failed" -- so the safe answer and the common answer are the same one.
 *
 * Preservation is announced, never silent. That distinction is the whole
 * reason this is safe to default on: an installer that quietly keeps
 * state is a hazard of its own kind, because the operator cannot tell
 * which install they are looking at afterwards. --wipe-config is the
 * override for an operator who genuinely wants the disk clean.
 */
static int config_partition_has_pki(const char *config_dev)
{
	struct stat st;
	int found;

	if (mkdir(CONFIG_MOUNT, 0755) != 0 && errno != EEXIST)
		return 0;
	if (mount(config_dev, CONFIG_MOUNT, "btrfs", MS_RDONLY, NULL) != 0) {
		/* ext4 only because a box installed before cix-config became
		 * btrfs still has one, and that is exactly the install whose CA
		 * is most worth not destroying. */
		if (mount(config_dev, CONFIG_MOUNT, "ext4", MS_RDONLY, NULL) != 0)
			return 0;
	}
	found = stat(CONFIG_MOUNT "/state/pki/ca.key", &st) == 0 && st.st_size > 0;
	umount(CONFIG_MOUNT);
	return found;
}

static int install_main(int argc, char **argv)
{
	const char *disk = NULL;
	const char *ip = NULL;
	const char *gateway = NULL;
	const char *iface = NULL;
	int prefix = -1;
	int skip_partition = 0;
	/* #415: force cix-config to be formatted even when it already holds
	 * a CA. Opt-IN, because the destructive direction is the one that
	 * should need saying out loud. */
	int wipe_config = 0;
	/*
	 * Whether to stage the Secure Boot key enrolment: "auto" (default)
	 * enrols only when the firmware is enforcing, "always" regardless,
	 * "never" not at all.
	 *
	 * "always" is a real need rather than a test hook: an operator
	 * installing on a machine they intend to switch Secure Boot ON for
	 * afterwards wants the key enrolled now, while they are physically
	 * at the console, instead of discovering at the next boot that the
	 * chain is refused. It is also what lets the installer test cover
	 * enrolment on firmware it does not enforce on.
	 */
	const char *enroll_mode = "auto";
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
		if (strncmp(argv[i], "--enroll-key=", 13) == 0)
			enroll_mode = argv[i] + 13;
		else if (strncmp(argv[i], "--disk=", 7) == 0)
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
		else if (strcmp(argv[i], "--wipe-config") == 0)
			wipe_config = 1;
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

	/*
	 * Anything not given on the command line is asked for now (#271).
	 *
	 * Only a genuinely unusable invocation is still a usage error -- an
	 * unknown flag, which is a typo worth reporting rather than a
	 * question. Everything else has an answer the operator can give
	 * here, with the machine's own disks and NICs listed, which is a
	 * better place to get it right than a GRUB command line typed once,
	 * blind, with no feedback until the installer refuses to start.
	 */
	if (!unknown_arg) {
		static char disk_buf[64], ip_buf[64], gw_buf[64], iface_buf[64], prefix_buf[16];

		if (disk == NULL) {
			if (pick_disk(disk_buf, sizeof(disk_buf)) != 0)
				return 1;
			disk = disk_buf;
		}

		if (iface == NULL || ip == NULL || gateway == NULL || prefix <= 0) {
			char first_iface[64];
			struct nic_load_result nic_loads;

			dual_printf("\nThis address is how you reach the installed system: the REST API "
			            "is its only control surface, so it has to be right.\n");
			/*
			 * Blank is a real answer now, and the screen says so.
			 *
			 * Two reasons it has to be. On real hardware the built-in
			 * Ethernet is frequently NOT listed below at all: the NIC
			 * drivers are kernel modules and this installer carries no
			 * module tree, so only virtio NICs (built in) appear --
			 * measured on a real bare-metal attempt, 2026-09-12. And an
			 * operator may simply not have decided the address yet.
			 *
			 * Installing with none is a deliberate state, not a broken
			 * one: cixd's DEFAULT_BIND is 127.0.0.1 and boot_init()
			 * brings `lo` up for exactly that, so the box comes up
			 * running and answering on loopback. The installed system
			 * DOES load the NIC drivers -- load_boot_modules()
			 * modprobes e1000e, igb, ixgbe, r8169 and tg3 -- so the
			 * interface that is invisible here is present there, which
			 * is why deferring the decision works at all.
			 */
			dual_printf("\nPick the interface to manage this box on. `lo` (127.0.0.1) is a\n");
			dual_printf("real choice: it installs a box that comes up running, answering on\n");
			dual_printf("loopback, with no address committed to yet.\n");
			dual_printf("\nA gateway is optional -- leave it blank for a box reachable only on\n");
			dual_printf("its own subnet, which is the normal case for a LAN-local machine.\n");
			/*
			 * #442: three lines stood here telling the operator that a
			 * built-in Ethernet port may be missing from the list
			 * "because its driver is a kernel module and this installer
			 * carries no module tree". The media has carried the tree
			 * and modprobe since #429, the same afternoon that note was
			 * added -- so the one real bare-metal install that listed
			 * only `lo` was handed a printed explanation of a mechanism
			 * that did not exist, which is why the cause went
			 * unestablished. Nothing is claimed up front now: the
			 * drivers are loaded, and if the list still comes back
			 * empty, nicreport_no_nic_reason() says which of the three
			 * actual reasons it was.
			 */
			load_nic_modules(&nic_loads);
			list_interfaces(first_iface, sizeof(first_iface), &nic_loads);

			if (iface == NULL) {
				prompt_default("Management interface (blank for none)", first_iface, iface_buf,
				               sizeof(iface_buf));
				iface = iface_buf;
			}
			/*
			 * A blank interface means "no management network": stop
			 * asking, rather than walking the operator through three
			 * more questions whose answers would be discarded.
			 */
			if (iface[0] == '\0') {
				/* Nothing chosen at all -- same outcome as picking lo,
				 * and accepted rather than re-asked: the operator has
				 * already been told what loopback means. */
				ip = "";
				gateway = "";
				prefix = 0;
			} else {
				int is_lo = strcmp(iface, "lo") == 0;

				/*
				 * Loopback answers its own questions. 127.0.0.1/8 is
				 * the only sensible address on it and there is no
				 * gateway to reach, so offering the operator three
				 * prompts they cannot meaningfully answer differently
				 * would be theatre -- the defaults are the answers.
				 */
				if (ip == NULL) {
					prompt_default("Management IP address", is_lo ? "127.0.0.1" : "", ip_buf,
					               sizeof(ip_buf));
					ip = ip_buf;
				}
				if (prefix <= 0) {
					prompt_default("Prefix length", is_lo ? "8" : "24", prefix_buf,
					               sizeof(prefix_buf));
					prefix = atoi(prefix_buf);
				}
				/*
				 * Gateway is optional and skipped entirely for
				 * loopback. Blank means no default route -- correct
				 * for a LAN-local box, and the only correct answer for
				 * lo.
				 */
				if (gateway == NULL) {
					if (is_lo) {
						gateway = "";
					} else {
						prompt_default("Default gateway (blank for none)", "", gw_buf,
						               sizeof(gw_buf));
						gateway = gw_buf;
					}
				}
			}
		}
	}

	/*
	 * The disk is required; the management network is optional but
	 * ALL-OR-NOTHING. A half-given network -- an interface with no
	 * address, an address with no gateway -- is a typo rather than an
	 * intention, and bootstrap_management_network() would refuse it at
	 * first boot anyway (it validates all three and returns -1), which
	 * is a failure an operator would meet after the install rather than
	 * during it. So it is refused here, where they can still fix it.
	 *
	 * "Nothing" is the blank-interface answer, and is a real install:
	 * cixd comes up on 127.0.0.1 (DEFAULT_BIND) with `lo` brought up by
	 * boot_init(), and the address is set at first boot.
	 */
	{
		int net_given = (iface != NULL && iface[0] != '\0') || (ip != NULL && ip[0] != '\0') ||
		                (gateway != NULL && gateway[0] != '\0') || prefix > 0;
		/*
		 * A gateway is NOT part of completeness. A box reachable only
		 * on its own subnet is an ordinary, correct configuration --
		 * and it is the only correct one for lo, which has nowhere to
		 * route to. What is required is the interface, the address and
		 * the prefix: those three together are a working management
		 * network, and any one of them missing is a typo.
		 */
		int net_complete = iface != NULL && iface[0] != '\0' && ip != NULL && ip[0] != '\0' &&
		                   prefix > 0 && prefix <= 32;

		if (net_given && !net_complete) {
			dual_printf("\ncix-install: a management network needs --interface=, --ip= and "
			            "--prefix= together (--gateway= is optional), or none of them.\n");
			dual_printf("Given: interface=%s ip=%s prefix=%d gateway=%s\n",
			            (iface != NULL && iface[0] != '\0') ? iface : "(none)",
			            (ip != NULL && ip[0] != '\0') ? ip : "(none)", prefix,
			            (gateway != NULL && gateway[0] != '\0') ? gateway : "(none)");
			dual_printf("Leave all four out to install with no management network and set "
			            "the address at first boot.\n");
			return 1;
		}
	}

	if (unknown_arg || disk == NULL) {
		dual_printf("usage: %s [--disk=/dev/sdX] [--ip=A.B.C.D] [--prefix=N] "
		            "[--gateway=A.B.C.D] [--interface=IFNAME] [--skip-partition]\n"
		            "       [--enroll-key=auto|always|never] [--wipe-config]\n"
		            "  Every flag is optional: anything omitted is asked for on the console,\n"
		            "  with this machine's own disks and interfaces listed. Pass them all to\n"
		            "  install unattended.\n"
		            "  (--interface=: the physical NIC to bind the management IP to, e.g.\n"
		            "  eth0 -- see `ip link`/`ls /sys/class/net` from a rescue shell if\n"
		            "  unsure. The disk is partitioned here, with the standard layout,\n"
		            "  sized to the disk and leaving the remainder unallocated for you to\n"
		            "  use afterwards. --skip-partition: the disk is already partitioned\n"
		            "  by other means -- it must carry five GPT partitions named exactly\n"
		            "  cix-esp, cix-root-a, cix-root-b, cix-config and cix-containers,\n"
		            "  since roles are read back from those names.\n"
		            "  --wipe-config: format cix-config even if it already holds a CA. By\n"
		            "  default an existing CA on that partition is KEPT, not destroyed,\n"
		            "  and the installer says so when it does -- see cixctl pki export for\n"
		            "  carrying one across deliberately.)\n",
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
	/* #415: an existing CA on this partition is kept, not destroyed --
	 * loudly, so the operator knows which install they are looking at
	 * afterwards. --wipe-config forces the format. */
	if (!wipe_config && config_partition_has_pki(config_dev)) {
		dual_printf("cix-install: cix-config already holds a CA "
		            "(/state/pki/ca.key) -- KEEPING this partition, not formatting it.\n");
		dual_printf("cix-install: the existing PKI, DNS records, networks and container "
		            "definitions on it are preserved. Pass --wipe-config to format instead.\n");
	} else {
		/* with_mixed = 0: at AUTO_CONFIG_SIZE_MIB (512) this is comfortably
		 * above btrfs's ~109 MiB minimum for separate data/metadata
		 * profiles, so it gets the normal layout like every other
		 * filesystem here. --mixed was never desirable, only necessary. */
		if (mkfs_btrfs(config_dev, "cix-config", 0) != 0)
			return 1;
	}
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
	if (populate_esp(ESP_MOUNT, ip, root_a_dev) != 0) {
		umount(ESP_MOUNT);
		return 1;
	}
	if (umount(ESP_MOUNT) != 0) {
		dual_perror("umount esp");
		return 1;
	}

	/*
	 * Only ask for a MOK password when it can actually matter.
	 *
	 * enroll_signing_key() prompts for a one-time password and makes the
	 * operator type it again at the next boot, in MokManager. That is
	 * shim's physical-presence proof and cannot be skipped while
	 * enrolling -- but it is only ever consulted by firmware that
	 * ENFORCES Secure Boot. This ran unconditionally, and the SecureBoot
	 * variable was never read, so every install on a machine with Secure
	 * Boot off -- every QEMU test VM, most hardware in practice -- asked
	 * for a password protecting an enrollment nothing would ever check.
	 *
	 * And a failure no longer aborts the install. This used to
	 * `return 1` on any error, which contradicted the note directly
	 * above enroll_signing_key() itself: "this step doesn't gate
	 * anything about the install itself completing." The comment was
	 * right and the code was not. A machine that cannot enroll the key
	 * still boots -- Secure Boot simply refuses the chain until the key
	 * is enrolled by hand, which is a thing to report, not to fail an
	 * otherwise complete install over.
	 */
	if (strcmp(enroll_mode, "never") == 0) {
		dual_printf("cix-install: --enroll-key=never -- skipping MOK key enrollment\n");
	} else if (strcmp(enroll_mode, "auto") == 0 && !secure_boot_enforcing()) {
		dual_printf("cix-install: Secure Boot is not enforcing on this machine -- skipping "
		            "MOK key enrollment (no password needed)\n");
	} else if (enroll_signing_key() != 0) {
		dual_printf("cix-install: WARNING -- could not stage the Secure Boot key enrollment. "
		            "The install continues; this machine will refuse to boot the signed chain "
		            "until the key is enrolled manually.\n");
	}

	if (write_whole_file_to_device(ROOT_SQUASHFS_SRC, root_a_dev) != 0)
		return 1;

	if (ensure_dir(CONFIG_MOUNT) != 0)
		return 1;
	if (mount(config_dev, CONFIG_MOUNT, "btrfs", 0, NULL) != 0) {
		dual_perror("mount config");
		return 1;
	}
	/*
	 * net.conf only when there is a network to record. Its ABSENCE is
	 * how the installed system knows none was chosen:
	 * bootstrap_management_network() returns 0 -- a deliberate no-op,
	 * not a failure -- when it cannot parse one, so the daemon comes up
	 * on loopback and waits. Writing an empty or partial file instead
	 * would turn that clean "none" into a parse error on every boot.
	 */
	if (ip != NULL && ip[0] != '\0' && iface != NULL && iface[0] != '\0') {
		char net_conf_path[600];

		snprintf(net_conf_path, sizeof(net_conf_path), "%s/net.conf", CONFIG_MOUNT);
		/* netconf_write() rather than a local snprintf: cixd writes
		 * this same file now (PUT /v1/system/management-network), and
		 * two programs writing one format from two definitions of it
		 * is the drift this project's maxims refuse. */
		if (netconf_write(net_conf_path, ip, prefix, gateway, iface) != 0) {
			dual_perror("write net.conf");
			umount(CONFIG_MOUNT);
			return 1;
		}
	} else {
		dual_printf("cix-install: no management network configured -- cixd will answer on\n");
		dual_printf("  127.0.0.1. Set one on the booted box with:\n");
		dual_printf("    cixctl management-network set --interface=eth0 --ip=<addr> --prefix=24\n");
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

	/*
	 * The package seed, if this media carries one (#189).
	 *
	 * Copied to the exact paths the daemon already reads -- its recipe
	 * directory and its artifact cache -- so nothing new has to know
	 * about "a seed" after this moment. The daemon installs from them
	 * through its ordinary pipeline and verifies every artifact against
	 * its own recipe first, so what lands here is delivered, not
	 * trusted.
	 *
	 * Failing to copy is fatal rather than best-effort: an installer
	 * told to carry a seed and silently shipping a box without one
	 * produces exactly the "installed, boots, cannot run anything"
	 * state this exists to prevent.
	 */
	{
		struct stat sst;

		if (stat(SEED_SRC, &sst) == 0 && S_ISDIR(sst.st_mode)) {
			char dst[512];

			snprintf(dst, sizeof(dst), "%s/rebuildable", CONTAINERS_MOUNT);
			if (ensure_dir(dst) != 0)
				return 1;
			snprintf(dst, sizeof(dst), "%s/rebuildable/pkg", CONTAINERS_MOUNT);
			if (ensure_dir(dst) != 0)
				return 1;
			/*
			 * Both subdirectories must genuinely be there.
			 *
			 * treecopy_recursive() used to return SUCCESS for a source
			 * root that did not exist -- "nothing to copy" -- so an ISO
			 * whose seed was staged to the wrong path reported a clean
			 * install and produced a box with no recipes. That really
			 * happened. It is fixed at the source now (#194): a missing
			 * source root is a failure there, for every caller.
			 *
			 * This check stays anyway, and earns its place twice over.
			 * It names the actual problem -- media carrying a seed that
			 * is malformed -- rather than reporting a copy failure two
			 * layers down, and it verifies BOTH subdirectories before
			 * copying either, so a half-staged seed cannot leave a box
			 * with recipes and no artifacts.
			 *
			 * The copy itself is the daemon's own, linked here rather
			 * than reimplemented: it already reports WHICH entry
			 * stopped it, the difference between a diagnosable
			 * failure and "No such file or directory".
			 */
			if (stat(SEED_SRC "/recipes", &sst) != 0 ||
			    stat(SEED_SRC "/artifacts", &sst) != 0) {
				dual_printf("cix-install: the media carries a seed but it has no "
				            "recipes/ and artifacts/ -- refusing to install a box that "
				            "would come up unable to run anything\n");
				return 1;
			}
			if (treecopy_recursive(SEED_SRC "/recipes",
			                        CONTAINERS_MOUNT "/rebuildable/pkg/recipes") != 0) {
				dual_printf("cix-install: staging the seed recipes failed: %s\n",
				            treecopy_last_error());
				return 1;
			}
			if (treecopy_recursive(SEED_SRC "/artifacts",
			                        CONTAINERS_MOUNT "/rebuildable/pkg/cache") != 0) {
				dual_printf("cix-install: staging the seed artifacts failed: %s\n",
				            treecopy_last_error());
				return 1;
			}
			dual_printf("cix-install: staged the package seed\n");
		}
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
	/*
	 * An operator who chose loopback must be told what they have, on
	 * the last screen before this box becomes shell-less and reachable
	 * only over an API. Said here as well as at the prompt, because
	 * the prompt was several minutes and a disk format ago.
	 */
	if (ip == NULL || ip[0] == '\0' || strcmp(ip, "127.0.0.1") == 0) {
		dual_printf("\n");
		dual_printf("  This box has NO external management address: cixd will answer on\n");
		dual_printf("  127.0.0.1 only. It will boot, run, and be healthy -- but nothing on\n");
		dual_printf("  the network can reach it until it is given an address.\n");
	}
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

/*
 * PID 1 must never return, not even on success (#131).
 *
 * install_main() above has 31 paths that report a real problem and
 * return non-zero. As PID 1 every one of them was an instant
 * `Attempted to kill init!` kernel panic with a full stack trace --
 * which scrolls the installer's own message, the one saying what
 * actually went wrong, off the top of the screen. An operator on real
 * hardware with a monitor and no serial cable therefore saw the
 * installer "crash" with nothing to report, which is exactly how this
 * was found: reported from a real bare-metal attempt, 2026-09-12.
 *
 * The success path already knew this and parked on
 * dual_console_wait_for_key() before rebooting. Only the failures did
 * not, so the one outcome that most needs to be readable was the one
 * outcome that destroyed its own evidence.
 *
 * Same shape as cix-recover's main(), deliberately: two PID-1 tools,
 * one convention. Parks forever rather than rebooting, because a
 * reboot would take the error off the screen just as surely as the
 * panic did -- and there is nothing to lose by waiting, the install
 * did not complete.
 */
int main(int argc, char **argv)
{
	int rc = install_main(argc, argv);

	/*
	 * Only reached on failure: the success path reboots and never
	 * comes back here.
	 */
	dual_printf("\n=====================================================================\n");
	dual_printf("  cix-install FAILED (rc=%d). The reason is printed above this banner --\n", rc);
	dual_printf("  scroll up if you need it, or photograph the screen.\n");
	dual_printf("\n");
	dual_printf("  Nothing further will happen: this machine is parked deliberately so\n");
	dual_printf("  the error stays readable. It is safe to power off or reset.\n");
	dual_printf("=====================================================================\n");
	sync();
	for (;;)
		pause();
}
