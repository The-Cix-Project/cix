#ifndef SQUASHFSIMG_H
#define SQUASHFSIMG_H

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/*
 * Does this path hold a whole squashfs image?
 *
 * 1 if the file's first four bytes are squashfs's own on-disk magic
 * ("hsqs", the little-endian bytes of 0x73717368), 0 if it opened and
 * they are not, -1 if it could not be opened at all (errno set).
 * Three answers rather than two because the callers say different
 * things about each: a path that will not open is usually a typo, and
 * one that opens without the magic is usually an assembly still
 * writing it (#435).
 *
 * open()+read(), deliberately never stat()+S_ISREG: a squashfs image's
 * bytes are equally valid backing a regular file or a raw block
 * device, and both pkg.c's toolchain install and
 * test_boot_update.c's --test-update-image= rely on the device case.
 *
 * A static inline header function rather than a new .c/.o, for
 * namecheck.h's reason and in its shape: pure, stateless, and small
 * enough that separate copies of it -- do_system_update()'s, the ISO
 * builder's, and pkg_toolchain_install()'s, the last of which already
 * carried a comment naming the first as its precedent -- are the
 * duplication "No Parallel Implementations" forbids. The ISO
 * builder's copy was the one that mattered, because it was not this
 * check at all: it tested access(R_OK) and nothing else, so a
 * half-written control-plane root could be baked into signed
 * installer media that POST /system/update would have refused (#481).
 */
static inline int squashfs_image_check(const char *path)
{
	int fd, ok;
	char magic[4];

	if (path == NULL || path[0] == '\0') {
		errno = EINVAL;
		return -1;
	}
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ok = (read(fd, magic, sizeof(magic)) == (ssize_t)sizeof(magic) &&
	      memcmp(magic, "hsqs", sizeof(magic)) == 0);
	close(fd);
	return ok;
}

#endif /* SQUASHFSIMG_H */
