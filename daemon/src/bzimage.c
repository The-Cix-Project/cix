#include "bzimage.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* The setup header's own offsets (boot protocol, x86). */
#define BZ_BOOT_FLAG 0x1FE    /* 0x55 0xAA */
#define BZ_HEADER_MAGIC 0x202 /* "HdrS" */
#define BZ_VERSION 0x206      /* boot protocol version, u16 */
#define BZ_KERNEL_VERSION 0x20E /* offset of the version string, less 0x200, u16 */

static int read_at(int fd, off_t at, void *buf, size_t len)
{
	return pread(fd, buf, len, at) == (ssize_t)len ? 0 : -1;
}

static int is_bzimage_fd(int fd)
{
	unsigned char flag[2], magic[4];

	return read_at(fd, BZ_BOOT_FLAG, flag, sizeof(flag)) == 0 && flag[0] == 0x55 &&
	       flag[1] == 0xAA && read_at(fd, BZ_HEADER_MAGIC, magic, sizeof(magic)) == 0 &&
	       memcmp(magic, "HdrS", 4) == 0;
}

int bzimage_check(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	int ok;

	if (fd < 0)
		return -1;
	ok = is_bzimage_fd(fd);
	close(fd);
	return ok;
}

int bzimage_release(const char *path, char *out, size_t out_size)
{
	unsigned char le[2];
	unsigned version, pointer;
	char text[BZIMAGE_RELEASE_MAX];
	size_t i;
	int fd;

	if (out_size == 0)
		return -1;
	out[0] = '\0';
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (!is_bzimage_fd(fd) || read_at(fd, BZ_VERSION, le, 2) != 0) {
		close(fd);
		return -1;
	}
	version = (unsigned)le[0] | ((unsigned)le[1] << 8);
	if (version < 0x0200 || read_at(fd, BZ_KERNEL_VERSION, le, 2) != 0) {
		close(fd);
		return -1;
	}
	pointer = (unsigned)le[0] | ((unsigned)le[1] << 8);
	if (pointer == 0) {
		close(fd);
		return -1;
	}
	/* A short read is fine as long as the release word ends inside it. */
	memset(text, 0, sizeof(text));
	if (pread(fd, text, sizeof(text) - 1, (off_t)pointer + 0x200) <= 0) {
		close(fd);
		return -1;
	}
	close(fd);
	for (i = 0; i < sizeof(text) - 1 && text[i] != '\0' && text[i] != ' '; i++)
		if (text[i] < 0x21 || text[i] > 0x7E)
			return -1;
	if (i == 0 || i >= out_size || (text[i] != ' ' && text[i] != '\0'))
		return -1;
	memcpy(out, text, i);
	out[i] = '\0';
	return 0;
}

int bzimage_release_listed(const char *release, const char *list)
{
	size_t rlen;
	const char *p;

	if (release == NULL || list == NULL || release[0] == '\0')
		return 0;
	rlen = strlen(release);
	for (p = list; *p != '\0';) {
		const char *end = strchr(p, '\n');
		size_t len = end != NULL ? (size_t)(end - p) : strlen(p);

		if (len == rlen && memcmp(p, release, rlen) == 0)
			return 1;
		if (end == NULL)
			break;
		p = end + 1;
	}
	return 0;
}
