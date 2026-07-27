/*
 * Phase 11 part 1 demonstrable test: proves the actual boot chain works --
 * kernel -> systemd-boot -> kanxeod --init-mode -- not just that each piece
 * builds. Assembles a throwaway 2-partition disk (ESP + one squashfs root
 * slot; the real 5-partition A/B layout is part 3's installer output, not
 * this test's job), boots it in QEMU (software-emulated -- no /dev/kvm in
 * this dev LXC), and scrapes the serial console for the exact same
 * "kanxeod listening on ..." line every other test in this suite already
 * treats as daemon-ready (daemon/src/main.c) -- proof this is the real
 * startup sequence running for real, not a special-cased boot-mode stub.
 *
 * No loop devices, no mount(): this LXC exposes no /dev/loop* at all (a
 * real environment constraint found empirically, the same class as the
 * missing /dev/kvm -- see docs/ROADMAP.md). Partitioning is done directly
 * on the disk image file (sfdisk operates on a plain file fine); the ESP
 * is built as a standalone FAT32 file populated with `mtools`
 * (mcopy/mmd -- a real userspace FAT implementation, no mount needed);
 * both the ESP and the root squashfs are then written into the disk image
 * at their exact partition byte offsets (parsed from `sfdisk -d`) with a
 * plain seek+write, no loop device required anywhere. QEMU reads the
 * resulting file directly as a virtio disk.
 */
#include "test_image_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define MKBOOTROOT_BIN "build/mkbootroot"
#define KANXEOD_BIN "build/kanxeod"
#define BZIMAGE_PATH "build/bzImage"
#define SFDISK_BIN "/usr/sbin/sfdisk"
#define MKFS_VFAT_BIN "/usr/sbin/mkfs.vfat"
#define MCOPY_BIN "/usr/bin/mcopy"
#define MMD_BIN "/usr/bin/mmd"
#define QEMU_BIN "/usr/bin/qemu-system-x86_64"
#define SYSTEMD_BOOT_EFI "/usr/lib/systemd/boot/efi/systemd-bootx64.efi"
#define OVMF_CODE "/usr/share/OVMF/OVMF_CODE_4M.fd"
#define OVMF_VARS_TEMPLATE "/usr/share/OVMF/OVMF_VARS_4M.fd"

#define DISK_SIZE_BYTES (256 * 1024 * 1024) /* 64MB ESP + ~192MB root -- root.squashfs measures well under 1MB */
#define ESP_SIZE_MIB 64
#define SECTOR_SIZE 512

#define BOOT_TIMEOUT_SECONDS 120
#define SUCCESS_MARKER "kanxeod listening on"
#define PANIC_MARKER "Kernel panic"

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

/* Like run_subprocess(), but captures stdout -- used for `sfdisk -d`. */
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

/* Like run_subprocess(), but feeds `script` to the child's stdin -- used
 * for `sfdisk`, which reads its partition table from stdin. */
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
		perror(bin);
		_exit(127);
	}
	close(pipefd[0]);
	while (written < len) {
		n = write(pipefd[1], script + written, len - written);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		written += (size_t)n;
	}
	close(pipefd[1]);
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "%s failed (status 0x%x)\n", bin, (unsigned)status);
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

/* Finds the Nth (1-based) "start=<sectors>, size=<sectors>" pair in
 * `sfdisk -d`'s dump output -- one per partition, in partition order. */
static int sfdisk_dump_offset(const char *dump, int partition_index, long *out_start_sectors,
                               long *out_size_sectors)
{
	const char *p = dump;
	int idx = 0;

	while ((p = strstr(p, " : start=")) != NULL) {
		idx++;
		if (idx == partition_index) {
			if (sscanf(p, " : start=%ld, size=%ld", out_start_sectors, out_size_sectors) == 2)
				return 0;
			return -1;
		}
		p += 1;
	}
	fprintf(stderr, "sfdisk -d: partition %d not found\n", partition_index);
	return -1;
}

static int write_at_offset(const char *dst_path, long offset_bytes, const char *src_path)
{
	FILE *dst = fopen(dst_path, "r+b");
	FILE *src;
	char buf[65536];
	size_t n;
	int ok = 1;

	if (dst == NULL) {
		perror(dst_path);
		return -1;
	}
	src = fopen(src_path, "rb");
	if (src == NULL) {
		perror(src_path);
		fclose(dst);
		return -1;
	}
	if (fseek(dst, offset_bytes, SEEK_SET) != 0) {
		perror("fseek");
		ok = 0;
	} else {
		while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
			if (fwrite(buf, 1, n, dst) != n) {
				perror("fwrite");
				ok = 0;
				break;
			}
		}
	}
	fclose(src);
	fclose(dst);
	return ok ? 0 : -1;
}

/* Builds the ESP as a standalone FAT32 file via mtools -- systemd-boot
 * itself, the kernel, and a loader entry pointing init at
 * kanxeod --init-mode on the raw root partition. No mount() anywhere:
 * mcopy/mmd are a real userspace FAT implementation operating on the
 * image file directly. */
static int build_esp_image(const char *esp_img, const char *workdir)
{
	char loader_conf_path[600];
	char loader_conf[512];
	char *mkfs_argv[] = { (char *)MKFS_VFAT_BIN, "-n", "ESP", (char *)esp_img, NULL };
	char *mmd_efi[] = { (char *)MMD_BIN, "-i", (char *)esp_img, "::/EFI", NULL };
	char *mmd_boot[] = { (char *)MMD_BIN, "-i", (char *)esp_img, "::/EFI/BOOT", NULL };
	char *mmd_loader[] = { (char *)MMD_BIN, "-i", (char *)esp_img, "::/loader", NULL };
	char *mmd_entries[] = { (char *)MMD_BIN, "-i", (char *)esp_img, "::/loader/entries", NULL };
	char *mcopy_boot_efi[] = { (char *)MCOPY_BIN, "-i", (char *)esp_img,
		                    (char *)SYSTEMD_BOOT_EFI, "::/EFI/BOOT/BOOTX64.EFI", NULL };
	char *mcopy_kernel[] = { (char *)MCOPY_BIN, "-i", (char *)esp_img,
		                  (char *)BZIMAGE_PATH, "::/kanxeo-bzImage", NULL };
	char *mcopy_loader_conf[] = { (char *)MCOPY_BIN, "-i", (char *)esp_img,
		                       loader_conf_path, "::/loader/loader.conf", NULL };
	char *mcopy_kanxeo_conf[] = { (char *)MCOPY_BIN, "-i", (char *)esp_img,
		                       loader_conf_path, "::/loader/entries/kanxeo.conf", NULL };

	if (run_subprocess(MKFS_VFAT_BIN, mkfs_argv) != 0)
		return -1;
	if (run_subprocess(MMD_BIN, mmd_efi) != 0)
		return -1;
	if (run_subprocess(MMD_BIN, mmd_boot) != 0)
		return -1;
	if (run_subprocess(MMD_BIN, mmd_loader) != 0)
		return -1;
	if (run_subprocess(MMD_BIN, mmd_entries) != 0)
		return -1;
	if (run_subprocess(MCOPY_BIN, mcopy_boot_efi) != 0)
		return -1;
	if (run_subprocess(MCOPY_BIN, mcopy_kernel) != 0)
		return -1;

	snprintf(loader_conf_path, sizeof(loader_conf_path), "%s/loader.conf", workdir);
	if (write_text_file(loader_conf_path, "default kanxeo\ntimeout 0\n") != 0)
		return -1;
	if (run_subprocess(MCOPY_BIN, mcopy_loader_conf) != 0)
		return -1;

	snprintf(loader_conf, sizeof(loader_conf),
	         "title Kanxeo\n"
	         "linux /kanxeo-bzImage\n"
	         "options console=ttyS0 root=/dev/vda2 rw init=/bin/kanxeod -- --init-mode\n");
	if (write_text_file(loader_conf_path, loader_conf) != 0)
		return -1;
	if (run_subprocess(MCOPY_BIN, mcopy_kanxeo_conf) != 0)
		return -1;

	return 0;
}

/* Reads from qemu_fd (the QEMU subprocess's captured stdout/serial
 * console) until SUCCESS_MARKER or PANIC_MARKER appears, or the deadline
 * passes -- poll()-based rather than blocking read(), since this has to
 * bound the wait, not just consume until EOF the way run_openssl() does. */
static int wait_for_boot(int qemu_fd)
{
	char buf[8192];
	size_t total = 0;
	struct timespec deadline;
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += BOOT_TIMEOUT_SECONDS;

	for (;;) {
		struct pollfd pfd;
		int remaining_ms;
		int rc;

		clock_gettime(CLOCK_MONOTONIC, &now);
		remaining_ms = (int)((deadline.tv_sec - now.tv_sec) * 1000 +
		                      (deadline.tv_nsec - now.tv_nsec) / 1000000);
		if (remaining_ms <= 0) {
			fprintf(stderr, "timed out after %ds waiting for boot\n", BOOT_TIMEOUT_SECONDS);
			return -1;
		}

		pfd.fd = qemu_fd;
		pfd.events = POLLIN;
		rc = poll(&pfd, 1, remaining_ms);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			return -1;
		}
		if (rc == 0)
			continue; /* re-check the deadline at the top of the loop */

		if (pfd.revents & POLLIN) {
			ssize_t n;

			if (total + 1 >= sizeof(buf))
				total = 0; /* ring-buffer-free: just keep scanning the tail */
			n = read(qemu_fd, buf + total, sizeof(buf) - total - 1);
			if (n <= 0)
				break;
			total += (size_t)n;
			buf[total] = '\0';
			fwrite(buf + (total - (size_t)n), 1, (size_t)n, stdout);

			if (strstr(buf, PANIC_MARKER) != NULL) {
				fprintf(stderr, "\nkernel panic detected\n");
				return -1;
			}
			if (strstr(buf, SUCCESS_MARKER) != NULL)
				return 0;
		}
		if (pfd.revents & (POLLHUP | POLLERR))
			break;
	}

	fprintf(stderr, "qemu exited before reaching a healthy boot\n");
	return -1;
}

int main(void)
{
	char workdir[] = "/tmp/kanxeo_test_boot_XXXXXX";
	char stage_dir[600];
	char root_squashfs[600];
	char disk_img[600];
	char esp_img[600];
	char ovmf_vars[600];
	char sfdisk_dump[8192];
	long esp_start_sec, esp_size_sec;
	long root_start_sec, root_size_sec;

	if (mkdtemp(workdir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(stage_dir, sizeof(stage_dir), "%s/stage", workdir);
	snprintf(root_squashfs, sizeof(root_squashfs), "%s/root.squashfs", workdir);
	snprintf(disk_img, sizeof(disk_img), "%s/disk.img", workdir);
	snprintf(esp_img, sizeof(esp_img), "%s/esp.img", workdir);
	snprintf(ovmf_vars, sizeof(ovmf_vars), "%s/OVMF_VARS.fd", workdir);

	/* 1. Build the minimal control-plane root (build/mkbootroot already
	 * reuses test_image_fixture_build() for the kanxeod+ld.so+libc
	 * staging -- not reimplemented here). */
	{
		char *mkbootroot_argv[] = { (char *)MKBOOTROOT_BIN, stage_dir,
			                     (char *)KANXEOD_BIN, root_squashfs, NULL };
		if (run_subprocess(MKBOOTROOT_BIN, mkbootroot_argv) != 0)
			return 1;
	}

	/* 2. Throwaway raw disk: GPT, one ESP + one root partition. Part 1's
	 * own scope boundary -- the real 5-partition A/B layout is part 3's
	 * installer output, not this test's job. sfdisk operates on the
	 * plain file directly, no loop device needed. */
	{
		int fd = open(disk_img, O_CREAT | O_WRONLY, 0644);

		if (fd < 0 || ftruncate(fd, DISK_SIZE_BYTES) != 0) {
			perror(disk_img);
			if (fd >= 0)
				close(fd);
			return 1;
		}
		close(fd);
	}
	{
		char script[256];
		char *sfdisk_argv[] = { (char *)SFDISK_BIN, disk_img, NULL };

		snprintf(script, sizeof(script),
		         "label: gpt\n"
		         "size=%dMiB, type=uefi, name=\"ESP\"\n"
		         "type=linux, name=\"root\"\n",
		         ESP_SIZE_MIB);
		if (run_subprocess_stdin(SFDISK_BIN, sfdisk_argv, script) != 0)
			return 1;
	}
	{
		char *sfdisk_dump_argv[] = { (char *)SFDISK_BIN, "-d", disk_img, NULL };

		if (run_subprocess_capture(SFDISK_BIN, sfdisk_dump_argv, sfdisk_dump, sizeof(sfdisk_dump)) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 1, &esp_start_sec, &esp_size_sec) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 2, &root_start_sec, &root_size_sec) != 0)
			return 1;
	}

	/* 3. ESP, built standalone via mtools, then dropped into the disk
	 * image at its real partition offset. */
	{
		int fd = open(esp_img, O_CREAT | O_WRONLY, 0644);

		if (fd < 0 || ftruncate(fd, esp_size_sec * SECTOR_SIZE) != 0) {
			perror(esp_img);
			if (fd >= 0)
				close(fd);
			return 1;
		}
		close(fd);
	}
	if (build_esp_image(esp_img, workdir) != 0)
		return 1;
	if (write_at_offset(disk_img, esp_start_sec * SECTOR_SIZE, esp_img) != 0)
		return 1;

	/* 4. Root partition: the whole squashfs image, written raw at its
	 * partition offset -- a single atomic block copy, exactly how
	 * ADR-0014 describes every future A/B slot write working too. */
	if (write_at_offset(disk_img, root_start_sec * SECTOR_SIZE, root_squashfs) != 0)
		return 1;

	/* 5. Boot it. Software-emulated (no /dev/kvm in this dev LXC) --
	 * correct but slow, acceptable for a boot-correctness smoke test. */
	if (test_image_fixture_copy_file(OVMF_VARS_TEMPLATE, ovmf_vars) != 0)
		return 1;

	{
		char code_arg[600], vars_arg[600], disk_arg[600];
		int pipefd[2];
		pid_t pid;
		int rc;

		snprintf(code_arg, sizeof(code_arg), "if=pflash,format=raw,readonly=on,file=%s", OVMF_CODE);
		snprintf(vars_arg, sizeof(vars_arg), "if=pflash,format=raw,file=%s", ovmf_vars);
		snprintf(disk_arg, sizeof(disk_arg), "file=%s,if=virtio,format=raw", disk_img);

		char *qemu_argv[] = {
			(char *)QEMU_BIN, "-machine", "q35", "-m", "512", "-cpu", "qemu64",
			"-display", "none", "-serial", "stdio", "-monitor", "none", "-no-reboot",
			"-drive", code_arg, "-drive", vars_arg, "-drive", disk_arg, NULL
		};

		if (pipe2(pipefd, O_CLOEXEC) != 0) {
			perror("pipe2");
			return 1;
		}
		pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid == 0) {
			dup2(pipefd[1], STDOUT_FILENO);
			close(pipefd[0]);
			close(pipefd[1]);
			execve(QEMU_BIN, qemu_argv, environ);
			perror(QEMU_BIN);
			_exit(127);
		}
		close(pipefd[1]);

		rc = wait_for_boot(pipefd[0]);
		close(pipefd[0]);

		kill(pid, SIGTERM);
		{
			int status;
			int waited;

			for (waited = 0; waited < 20; waited++) {
				if (waitpid(pid, &status, WNOHANG) == pid)
					break;
				usleep(100000);
			}
			if (waited >= 20) {
				kill(pid, SIGKILL);
				waitpid(pid, &status, 0);
			}
		}

		if (rc != 0)
			return 1;
	}

	printf("BOOT RESULT: PASS\n");
	return 0;
}
