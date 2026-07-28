#include "test_disk_image.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
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

#define MKFS_VFAT_BIN "/usr/sbin/mkfs.vfat"
#define MCOPY_BIN "/usr/bin/mcopy"
#define MMD_BIN "/usr/bin/mmd"
#define MREN_BIN "/usr/bin/mren"
#define MKSQUASHFS_BIN "/usr/bin/mksquashfs"
#define QEMU_BIN "/usr/bin/qemu-system-x86_64"
#define OVMF_CODE "/usr/share/OVMF/OVMF_CODE_4M.fd"
#define OVMF_CODE_SECURE "/usr/share/OVMF/OVMF_CODE_4M.ms.fd"

int run_subprocess(const char *bin, char *const argv[])
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

int run_subprocess_capture(const char *bin, char *const argv[], char *out, size_t out_size)
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

int run_subprocess_stdin(const char *bin, char *const argv[], const char *script)
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

int ensure_dir(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	return 0;
}

int write_text_file(const char *path, const char *content)
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

static int rm_tree_visitor(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
	(void)sb;
	(void)ftwbuf;
	if (typeflag == FTW_DP)
		return rmdir(path);
	return unlink(path);
}

void rm_tree(const char *path)
{
	nftw(path, rm_tree_visitor, 16, FTW_DEPTH | FTW_PHYS);
}

int sfdisk_dump_offset(const char *dump, int partition_index, long *out_start_sectors,
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

int write_at_offset(const char *dst_path, long offset_bytes, const char *src_path)
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

int extract_partition(const char *src_path, long offset_bytes, long size_bytes, const char *out_path)
{
	FILE *src = fopen(src_path, "rb");
	FILE *dst;
	char buf[65536];
	long remaining = size_bytes;
	int ok = 1;

	if (src == NULL) {
		perror(src_path);
		return -1;
	}
	dst = fopen(out_path, "wb");
	if (dst == NULL) {
		perror(out_path);
		fclose(src);
		return -1;
	}
	if (fseek(src, offset_bytes, SEEK_SET) != 0) {
		perror("fseek");
		ok = 0;
	} else {
		while (remaining > 0) {
			size_t want = (size_t)(remaining < (long)sizeof(buf) ? remaining : (long)sizeof(buf));
			size_t n = fread(buf, 1, want, src);

			if (n == 0)
				break;
			if (fwrite(buf, 1, n, dst) != n) {
				perror("fwrite");
				ok = 0;
				break;
			}
			remaining -= (long)n;
		}
	}
	fclose(src);
	fclose(dst);
	return ok ? 0 : -1;
}

int esp_mkfs(const char *esp_img)
{
	char *argv[] = { (char *)MKFS_VFAT_BIN, "-n", "ESP", (char *)esp_img, NULL };

	return run_subprocess(MKFS_VFAT_BIN, argv);
}

int esp_mmd(const char *esp_img, const char *esp_dir_path)
{
	char *argv[] = { (char *)MMD_BIN, "-i", (char *)esp_img, (char *)esp_dir_path, NULL };

	return run_subprocess(MMD_BIN, argv);
}

int esp_mcopy_in(const char *esp_img, const char *host_src_path, const char *esp_dest_path)
{
	char *argv[] = { (char *)MCOPY_BIN, "-i", (char *)esp_img, (char *)host_src_path,
		          (char *)esp_dest_path, NULL };

	return run_subprocess(MCOPY_BIN, argv);
}

int esp_mren(const char *esp_img, const char *esp_old_path, const char *esp_new_path)
{
	char *argv[] = { (char *)MREN_BIN, "-i", (char *)esp_img, (char *)esp_old_path,
		          (char *)esp_new_path, NULL };

	return run_subprocess(MREN_BIN, argv);
}

int build_squashfs(const char *image_root, const char *out_path)
{
	char *argv[] = { (char *)MKSQUASHFS_BIN, (char *)image_root, (char *)out_path,
		          "-noappend", "-comp", "xz", "-quiet", NULL };

	unlink(out_path);
	return run_subprocess(MKSQUASHFS_BIN, argv);
}

enum qemu_boot_outcome qemu_boot_capture(const struct qemu_boot_opts *opts, char *out, size_t out_size)
{
	char code_arg[600], vars_arg[600], disk_arg[600], disk2_arg[600];
	int pipefd[2], in_pipefd[2];
	pid_t pid;
	enum qemu_boot_outcome outcome;
	size_t total = 0;
	struct timespec deadline, now;
	char *qemu_argv[36];
	int argc = 0;
	int next_input = 0;

	out[0] = '\0';

	snprintf(code_arg, sizeof(code_arg), "if=pflash,format=raw,readonly=on,file=%s",
	         opts->secure_boot ? OVMF_CODE_SECURE : OVMF_CODE);
	snprintf(vars_arg, sizeof(vars_arg), "if=pflash,format=raw,file=%s", opts->ovmf_vars);
	if (opts->disk_img_is_cdrom)
		snprintf(disk_arg, sizeof(disk_arg), "%s", opts->disk_img);
	else
		snprintf(disk_arg, sizeof(disk_arg), "file=%s,if=virtio,format=raw", opts->disk_img);

	qemu_argv[argc++] = (char *)QEMU_BIN;
	qemu_argv[argc++] = "-machine";
	qemu_argv[argc++] = "q35";
	qemu_argv[argc++] = "-m";
	qemu_argv[argc++] = "512";
	qemu_argv[argc++] = "-cpu";
	qemu_argv[argc++] = "qemu64";
	qemu_argv[argc++] = "-display";
	qemu_argv[argc++] = "none";
	qemu_argv[argc++] = "-serial";
	qemu_argv[argc++] = "stdio";
	qemu_argv[argc++] = "-monitor";
	qemu_argv[argc++] = "none";
	qemu_argv[argc++] = "-no-reboot";
	if (opts->secure_boot) {
		qemu_argv[argc++] = "-global";
		qemu_argv[argc++] = "driver=cfi.pflash01,property=secure,value=on";
	}
	qemu_argv[argc++] = "-drive";
	qemu_argv[argc++] = code_arg;
	qemu_argv[argc++] = "-drive";
	qemu_argv[argc++] = vars_arg;
	if (opts->disk_img_is_cdrom) {
		qemu_argv[argc++] = "-cdrom";
		qemu_argv[argc++] = disk_arg;
	} else {
		qemu_argv[argc++] = "-drive";
		qemu_argv[argc++] = disk_arg;
	}
	if (opts->disk_img2 != NULL) {
		snprintf(disk2_arg, sizeof(disk2_arg), "file=%s,if=virtio,format=raw", opts->disk_img2);
		qemu_argv[argc++] = "-drive";
		qemu_argv[argc++] = disk2_arg;
	}
	if (opts->with_nic) {
		qemu_argv[argc++] = "-netdev";
		qemu_argv[argc++] = "user,id=n0";
		qemu_argv[argc++] = "-device";
		qemu_argv[argc++] = "virtio-net-pci,netdev=n0";
	}
	if (opts->direct_kernel != NULL) {
		qemu_argv[argc++] = "-kernel";
		qemu_argv[argc++] = (char *)opts->direct_kernel;
		qemu_argv[argc++] = "-append";
		qemu_argv[argc++] = (char *)opts->direct_kernel_args;
	}
	qemu_argv[argc] = NULL;

	if (pipe2(pipefd, O_CLOEXEC) != 0) {
		perror("pipe2");
		return QEMU_BOOT_ERROR;
	}
	if (pipe2(in_pipefd, O_CLOEXEC) != 0) {
		perror("pipe2");
		close(pipefd[0]);
		close(pipefd[1]);
		return QEMU_BOOT_ERROR;
	}
	pid = fork();
	if (pid < 0) {
		perror("fork");
		close(pipefd[0]);
		close(pipefd[1]);
		close(in_pipefd[0]);
		close(in_pipefd[1]);
		return QEMU_BOOT_ERROR;
	}
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(in_pipefd[0], STDIN_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		close(in_pipefd[0]);
		close(in_pipefd[1]);
		execve(QEMU_BIN, qemu_argv, environ);
		perror(QEMU_BIN);
		_exit(127);
	}
	close(pipefd[1]);
	close(in_pipefd[0]);

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += opts->timeout_seconds;
	outcome = QEMU_BOOT_TIMEOUT;

	for (;;) {
		struct pollfd pfd;
		int remaining_ms;
		int rc;

		clock_gettime(CLOCK_MONOTONIC, &now);
		remaining_ms = (int)((deadline.tv_sec - now.tv_sec) * 1000 +
		                      (deadline.tv_nsec - now.tv_nsec) / 1000000);
		if (remaining_ms <= 0) {
			fprintf(stderr, "timed out after %ds waiting for boot\n", opts->timeout_seconds);
			break;
		}

		pfd.fd = pipefd[0];
		pfd.events = POLLIN;
		rc = poll(&pfd, 1, remaining_ms);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			outcome = QEMU_BOOT_ERROR;
			break;
		}
		if (rc == 0)
			continue; /* re-check the deadline at the top of the loop */

		if (pfd.revents & POLLIN) {
			char buf[8192];
			ssize_t n;

			n = read(pipefd[0], buf, sizeof(buf));
			if (n <= 0) {
				outcome = QEMU_BOOT_EOF;
				break;
			}
			fwrite(buf, 1, (size_t)n, stdout);

			if (total + (size_t)n < out_size) {
				memcpy(out + total, buf, (size_t)n);
				total += (size_t)n;
				out[total] = '\0';
			} else if (total + 1 < out_size) {
				size_t room = out_size - total - 1;

				memcpy(out + total, buf, room);
				total += room;
				out[total] = '\0';
			}

			if (opts->panic_marker != NULL && strstr(out, opts->panic_marker) != NULL) {
				fprintf(stderr, "\npanic marker detected\n");
				outcome = QEMU_BOOT_PANIC;
				break;
			}
			if (opts->success_marker != NULL && strstr(out, opts->success_marker) != NULL) {
				outcome = QEMU_BOOT_SUCCESS;
				break;
			}
			while (next_input < opts->n_scripted_input &&
			       strstr(out, opts->scripted_input[next_input].wait_for) != NULL) {
				const char *send = opts->scripted_input[next_input].send;
				size_t send_len = strlen(send);
				size_t sent = 0;
				ssize_t wn;

				while (sent < send_len) {
					wn = write(in_pipefd[1], send + sent, send_len - sent);
					if (wn < 0) {
						if (errno == EINTR)
							continue;
						perror("write scripted input");
						break;
					}
					sent += (size_t)wn;
				}
				next_input++;
			}
		}
		if (pfd.revents & (POLLHUP | POLLERR)) {
			outcome = QEMU_BOOT_EOF;
			break;
		}
	}

	close(pipefd[0]);
	close(in_pipefd[1]);
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

	return outcome;
}
