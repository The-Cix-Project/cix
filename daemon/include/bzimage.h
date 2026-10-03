#ifndef BZIMAGE_H
#define BZIMAGE_H

#include <stddef.h>

/*
 * What an x86 bzImage says about itself, read from its own setup
 * header (the Linux x86 boot protocol, Documentation/arch/x86/boot.rst).
 *
 * cix#568: the control-plane root carries the kernel's modules, and a
 * kernel only loads modules built for its own release. Staging a root
 * next to a kernel of another release boots a host whose drivers
 * cannot load -- and the boot can still confirm, because cixd itself
 * loads no module -- so the release a bzImage will report as
 * `uname -r` has to be readable before the two are paired.
 */

#define BZIMAGE_RELEASE_MAX 128

/*
 * 1 when path is a bzImage (the boot-sector signature 0x55 0xAA at
 * 0x1FE and "HdrS" at 0x202, the check bootloaders use), 0 when it is
 * a readable file that is not one, -1 when it cannot be opened.
 */
int bzimage_check(const char *path);

/*
 * The kernel release the image will report as `uname -r`: the first
 * word of the version string its setup header points at (kernel_version
 * at 0x20E, the string at that value + 0x200; boot protocol 2.00 and
 * later). 0 with out filled, -1 when path is not a bzImage or carries
 * no readable version string.
 */
int bzimage_release(const char *path, char *out, size_t out_size);

/*
 * Whether release is one of the lines of list (newline-separated, as a
 * root's kernel-releases record is written). An empty list contains
 * nothing. 1 or 0.
 */
int bzimage_release_listed(const char *release, const char *list);

#endif
