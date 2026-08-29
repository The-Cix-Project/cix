/*
 * cix-xorriso -- strips Apple HFS+ metadata arguments out of the
 * xorriso invocation grub-mkrescue builds, then runs the real xorriso.
 *
 * WHY THIS EXISTS
 *
 * grub-mkrescue unconditionally asks xorriso to write HFS+ metadata
 * alongside the ISO, so the media is also bootable on Apple hardware.
 * libisofs converts filenames to UTF-16 for it, glibc resolves that
 * through gconv modules, and no Cix image has any -- the directory
 * /usr/lib/x86_64-linux-gnu/gconv does not exist at all. The ISO build
 * therefore died at its very last step, after every input had been
 * staged and signed:
 *
 *   libisofs: FAILURE : Charset conversion error
 *   xorriso : FAILURE : Failed to prepare session write run
 *
 * Copying glibc's gconv modules in from a Debian host would have
 * fixed it and would have been the wrong fix: it imports foreign
 * binaries to satisfy a feature this platform does not want. Cix
 * installs on x86_64 UEFI machines; HFS+ is dead weight here.
 *
 * WHY A SEPARATE PROGRAM
 *
 * grub-mkrescue decides these arguments itself, and there is no way to
 * ask it not to:
 *
 *   - --arcs-boot does set system_area away from SYS_AREA_COMMON (the
 *     condition guarding the HFS+ block), but grub-mkrescue re-runs its
 *     own auto-detection whenever a source directory is given with -d,
 *     which overrides the flag straight back. -d is not optional for
 *     us: grub_util_get_pkglibdir() reads no environment variable
 *     (unlike pkgdatadir, which is how the font lookup was redirected),
 *     so -d is the only way to point it at the isotools artifact.
 *   - Patching grub's source would mean carrying a modification to
 *     unmodified upstream code across every future version.
 *
 * It does support --xorriso=PATH, so this sits in that slot. Same
 * "wrapper in front of a tool we do not control" shape procps.recipe
 * and chrony.recipe already use, as a compiled binary rather than a
 * script because a Cix control-plane root has no shell at all.
 *
 * For an EFI-only build this removes exactly the HFS+ arguments and
 * nothing else: the other two SYS_AREA_COMMON branches in
 * grub-mkrescue sit inside i386-pc and powerpc guards that never fire
 * here, and --protective-msdos-label is pushed unconditionally.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

/*
 * The arguments grub-mkrescue pushes for HFS+, with how many values
 * follow each. Taken from grub-mkrescue.c's own push sequence rather
 * than from observed output, so a reordering upstream cannot silently
 * leave a stray value behind to be read as a filename.
 */
static const struct {
	const char *opt;
	int values;
} hfs_args[] = {
	{ "-hfsplus", 0 },
	{ "-apm-block-size", 1 },              /* 2048 */
	{ "-hfsplus-file-creator-type", 3 },   /* chrp tbxj <path> */
	{ "-hfs-bless-by", 2 },                /* i <path> */
};

int main(int argc, char **argv)
{
	const char *real = getenv("CIX_REAL_XORRISO");
	char **out;
	int i, n = 0;

	if (real == NULL || real[0] == '\0') {
		fprintf(stderr, "cix-xorriso: CIX_REAL_XORRISO is not set -- "
		                "nothing to hand the filtered arguments to\n");
		return 1;
	}

	out = calloc((size_t)argc + 1, sizeof(*out));
	if (out == NULL) {
		perror("calloc");
		return 1;
	}

	out[n++] = (char *)real;
	for (i = 1; i < argc; i++) {
		size_t k;
		int skipped = 0;

		for (k = 0; k < sizeof(hfs_args) / sizeof(hfs_args[0]); k++) {
			if (strcmp(argv[i], hfs_args[k].opt) != 0)
				continue;
			/* Step over the option and the values belonging to it.
			 * Bounded by argc so a truncated tail cannot run off
			 * the end. */
			i += hfs_args[k].values;
			skipped = 1;
			break;
		}
		if (!skipped && i < argc)
			out[n++] = argv[i];
	}
	out[n] = NULL;

	execve(real, out, environ);
	perror(real);
	free(out);
	return 127;
}
