/*
 * cix#568: bzimage_release() reads the release a kernel will report as
 * `uname -r` out of its own setup header, and do_system_update() pairs
 * a root with a kernel on that answer -- so a wrong read stages a root
 * beside a kernel that cannot load its modules, or refuses a correct
 * pair. Every case is a file built here byte by byte, at the offsets
 * the x86 boot protocol fixes: the boot flag at 0x1FE, "HdrS" at 0x202,
 * the protocol version at 0x206, kernel_version at 0x20E.
 */
#include "bzimage.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures;
static char g_dir[] = "/tmp/test_bzimage_XXXXXX";

static void fail(const char *what)
{
	fprintf(stderr, "FAIL: %s\n", what);
	g_failures++;
}

/* A setup header with the given protocol version and version string
 * (NULL for kernel_version 0), the string placed at 0x400 + 0x200. */
static void write_image(const char *name, int boot_flag, unsigned protocol, const char *version,
                        char *path, size_t path_size)
{
	unsigned char buf[0x700];
	int fd;

	memset(buf, 0, sizeof(buf));
	if (boot_flag) {
		buf[0x1FE] = 0x55;
		buf[0x1FF] = 0xAA;
		memcpy(buf + 0x202, "HdrS", 4);
	}
	buf[0x206] = (unsigned char)(protocol & 0xFF);
	buf[0x207] = (unsigned char)(protocol >> 8);
	if (version != NULL) {
		buf[0x20E] = 0x00;
		buf[0x20F] = 0x04; /* 0x400, so the string is at 0x600 */
		memcpy(buf + 0x600, version, strlen(version) + 1);
	}
	snprintf(path, path_size, "%s/%s", g_dir, name);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
		fail("could not write a fixture image");
	if (fd >= 0)
		close(fd);
}

int main(void)
{
	char path[512], release[BZIMAGE_RELEASE_MAX];

	if (mkdtemp(g_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}

	write_image("good", 1, 0x020F, "7.2.9 (builder@cix) #1 SMP PREEMPT_DYNAMIC", path,
	            sizeof(path));
	if (bzimage_check(path) != 1)
		fail("a bzImage with the boot flag and HdrS is a bzImage");
	if (bzimage_release(path, release, sizeof(release)) != 0 || strcmp(release, "7.2.9") != 0)
		fail("the release is the version string's first word: 7.2.9");

	write_image("bare", 1, 0x020F, "7.2.3", path, sizeof(path));
	if (bzimage_release(path, release, sizeof(release)) != 0 || strcmp(release, "7.2.3") != 0)
		fail("a version string that is only the release reads whole");

	write_image("old", 1, 0x01FF, "7.2.9 (x)", path, sizeof(path));
	if (bzimage_release(path, release, sizeof(release)) == 0)
		fail("boot protocol before 2.00 has no kernel_version field");

	write_image("nover", 1, 0x020F, NULL, path, sizeof(path));
	if (bzimage_release(path, release, sizeof(release)) == 0)
		fail("kernel_version 0 means no version string");

	write_image("plain", 0, 0x020F, "7.2.9 (x)", path, sizeof(path));
	if (bzimage_check(path) != 0)
		fail("a readable file without the boot flag is not a bzImage");
	if (bzimage_release(path, release, sizeof(release)) == 0)
		fail("no release is read from a file that is not a bzImage");

	write_image("ctrl", 1, 0x020F, "7.2\t9 (x)", path, sizeof(path));
	if (bzimage_release(path, release, sizeof(release)) == 0)
		fail("a control character in the release is refused, not returned");

	write_image("small", 1, 0x020F, "7.2.9 (x)", path, sizeof(path));
	if (bzimage_release(path, release, 4) == 0)
		fail("a release longer than the buffer is refused, not truncated");

	snprintf(path, sizeof(path), "%s/absent", g_dir);
	if (bzimage_check(path) != -1)
		fail("a missing file is -1");

	if (!bzimage_release_listed("7.2.9", "7.2.9\n"))
		fail("a release on the only line is listed");
	if (!bzimage_release_listed("7.2.9", "7.2.3\n7.2.9"))
		fail("a release on the last line, no trailing newline, is listed");
	if (bzimage_release_listed("7.2.9", "7.2.91\n7.2.3\n"))
		fail("7.2.9 is not 7.2.91");
	if (bzimage_release_listed("7.2.9", ""))
		fail("an empty list contains nothing");

	snprintf(path, sizeof(path), "rm -rf '%s'", g_dir);
	if (system(path) != 0)
		fail("could not remove the fixtures");
	if (g_failures > 0) {
		printf("test_bzimage: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_bzimage: all checks passed\n");
	return 0;
}
