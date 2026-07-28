/*
 * Exec target for test_devices.c. Each argv triple after argv[0] is
 * <path> <0|1: open must fail/succeed> <"major:minor" to verify via
 * fstat(), or "-" to skip> -- the real proof that a live container
 * process is actually gated by container_dev_bpf_attach()'s
 * BPF_CGROUP_DEVICE program, not just that container_create() didn't
 * error out setting it up.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int i;

	if (argc < 4 || (argc - 1) % 3 != 0) {
		fprintf(stderr, "usage: dev_child <path> <0|1> <major:minor|-> ...\n");
		return 126;
	}

	for (i = 1; i < argc; i += 3) {
		const char *path = argv[i];
		int expect_ok = atoi(argv[i + 1]);
		const char *expect_rdev = argv[i + 2];
		int fd = open(path, O_RDONLY);

		if (!expect_ok) {
			if (fd >= 0) {
				fprintf(stderr, "FAIL: expected open(%s) to be denied, but it succeeded\n",
				        path);
				close(fd);
				return 2;
			}
			if (errno != EPERM) {
				fprintf(stderr,
				        "FAIL: expected open(%s) to fail with EPERM, got errno=%d (%s)\n",
				        path, errno, strerror(errno));
				return 3;
			}
			continue;
		}

		if (fd < 0) {
			fprintf(stderr, "FAIL: expected open(%s) to succeed, got errno=%d (%s)\n", path,
			        errno, strerror(errno));
			return 1;
		}

		if (strcmp(expect_rdev, "-") != 0) {
			struct stat st;
			unsigned int want_major, want_minor;

			if (fstat(fd, &st) != 0) {
				perror("fstat");
				close(fd);
				return 4;
			}
			if (sscanf(expect_rdev, "%u:%u", &want_major, &want_minor) != 2) {
				fprintf(stderr, "bad expect_rdev arg %s\n", expect_rdev);
				close(fd);
				return 126;
			}
			if (major(st.st_rdev) != want_major || minor(st.st_rdev) != want_minor) {
				fprintf(stderr, "FAIL: %s has rdev %u:%u, expected %u:%u\n", path,
				        major(st.st_rdev), minor(st.st_rdev), want_major, want_minor);
				close(fd);
				return 5;
			}
		}
		close(fd);
	}

	return 0;
}
