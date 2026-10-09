/*
 * cix-xorriso -- rewrites the xorriso invocation grub-mkrescue builds,
 * then runs the real xorriso.
 *
 * Two jobs, both forced on us by arguments grub-mkrescue pushes
 * unconditionally and offers no option to suppress.
 *
 * JOB ONE: STRIP APPLE HFS+ METADATA
 *
 * grub-mkrescue asks xorriso to write HFS+ metadata alongside the ISO,
 * so the media is also bootable on Apple hardware. libisofs converts
 * filenames to UTF-16 for it, glibc resolves that through gconv
 * modules, and no Cix image has any -- the directory
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
 * JOB TWO: GIVE THE ISO9660 FILESYSTEM A PARTITION OF ITS OWN (cix#430)
 *
 * The installer's GRUB entries have to tell the kernel which device
 * holds this filesystem. A device name is assigned by enumeration
 * order, so `root=/dev/sda` is a coin flip on exactly the machines the
 * USB entries exist for -- an installer's target machine usually has a
 * disk of its own, and it is just as likely to be sda as the stick is.
 * `root=PARTUUID=` is the fix the kernel's own root= parser already
 * understands, and it needs a GPT partition that covers the filesystem
 * and whose GUID is known at the time grub.cfg is written.
 *
 * As grub-mkrescue builds it, the media has neither. Measured on
 * 192.168.15.95, 2026-10-09 (probe-isogrub@6-1 through @9-1,
 * probe-isopoff@1-1 and @4-1, probe-grubsrc@1-1, all recorded in
 * cix#430):
 *
 *   - grub-mkrescue creates efi.img INSIDE the staged tree that becomes
 *     the ISO (grub-mkrescue.c:857) and then pushes `--efi-boot
 *     efi.img -efi-boot-part --efi-boot-image` (:865-868), which asks
 *     xorriso to describe that in-filesystem copy as the ESP.
 *   - So the ESP's blocks sit in the middle of the filesystem, and a
 *     partition spanning the filesystem would contain them. GPT
 *     partitions do not overlap, so xorriso describes the same space as
 *     three non-overlapping entries instead: Gap0 (LBA 64 to just
 *     before the ESP), the ESP, and Gap1. No entry covers the
 *     filesystem.
 *   - Asking for the overlapping isohybrid layout anyway is REFUSED,
 *     not ignored: `-boot_image any part_like_isohybrid=on` fails the
 *     build with "libisofs: FAILURE : Image write error / Caused by:
 *     Overlapping MBR partition entries requested". So the published
 *     layout is not a flag nobody passed; it is the only layout
 *     libisofs will write for an embedded ESP.
 *   - And grub-mkrescue has no switch for it: the three pushes are
 *     gated only on an EFI platform being present, under none of the
 *     system_area guards at :634, :744 and :898.
 *
 * So the ESP is moved out of the filesystem and appended past the end
 * of the image. `--efi-boot efi.img` STAYS, so the El Torito catalogue
 * still points at the in-filesystem copy and optical EFI boot is
 * unchanged; only the pair that describes that copy as a partition is
 * removed, and the same file is appended as partition 2 of type 0xef.
 * grub-mkrescue keeps that copy on purpose -- its own comment at :870
 * says "so that we have a duplicate on the ISO 9660 file system" -- so
 * there is always a file to append.
 *
 * Measured result, with grub-mkrescue's real arguments in play:
 *
 *   1  ISO9660     basic data   LBA 64..19207    <- the filesystem
 *   2  Appended2   ESP type     LBA 19208..24967
 *   3  Gap1        basic data   LBA 24968..25567
 *
 * with byte 65537 reading CD001 -- the partition-relative volume
 * descriptor `partition_offset=16` writes, which is what lets a
 * partition starting at LBA 64 be mounted as iso9660 from its own
 * first block.
 *
 * The three native settings are prepended rather than appended because
 * they are parameters of xorriso's NATIVE -boot_image command, not
 * options of the mkisofs emulation, and `-as mkisofs` consumes its own
 * argument list to the end. `-append_partition` is an emulation option
 * and so goes at the tail, inside that list.
 *
 * WHY A SEPARATE PROGRAM
 *
 * grub-mkrescue decides these arguments itself, and there is no way to
 * ask it not to:
 *
 *   - --arcs-boot does set system_area away from SYS_AREA_COMMON (the
 *     condition guarding the HFS+ blocks), but grub-mkrescue re-runs its
 *     own auto-detection whenever a source directory is given with -d
 *     (grub-mkrescue.c:560 tests grub_install_source_directory and
 *     :572 resets it), which overrides the flag straight back. -d is
 *     not optional for us: grub_util_get_pkglibdir() reads no
 *     environment variable (unlike pkgdatadir, which is how the font
 *     lookup was redirected), so -d is the only way to point it at the
 *     isotools artifact.
 *   - Patching grub's source would mean carrying a modification to
 *     unmodified upstream code across every future version.
 *
 * It does support --xorriso=PATH, so this sits in that slot. Same
 * "wrapper in front of a tool we do not control" shape procps.recipe
 * and chrony.recipe already use, as a compiled binary rather than a
 * script because a Cix control-plane root has no shell at all.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

/*
 * The arguments grub-mkrescue pushes for HFS+, with how many values
 * follow each. Taken from grub-mkrescue.c's own push sequence rather
 * than from observed output, so a reordering upstream cannot silently
 * leave a stray value behind to be read as a filename -- and since
 * confirmed term for term against a real argv (cix#430, 2026-10-09):
 *
 *   -hfsplus -apm-block-size 2048
 *   -hfsplus-file-creator-type chrp tbxj /System/.../.disk_label
 *   -hfs-bless-by i /System/Library/CoreServices/boot.efi
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

/*
 * The pair that describes the in-filesystem efi.img as the ESP. Removed
 * together: `-efi-boot-part` takes `--efi-boot-image` as its value, so
 * neither is meaningful alone.
 */
static const char *const esp_args[] = {
	"-efi-boot-part",
	"--efi-boot-image",
};

/*
 * 16 blocks of 2048 bytes, i.e. byte 32768, i.e. 512-byte LBA 64 --
 * where xorriso starts the partition it describes and where the
 * measured ISO9660 entry begins. Changing this moves the partition's
 * first block away from the descriptor written for it.
 */
#define PARTITION_OFFSET_BLOCKS "16"

static int is_dir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file_under(const char *dir, const char *rel, char *out, size_t out_size)
{
	struct stat st;

	if ((size_t)snprintf(out, out_size, "%s/%s", dir, rel) >= out_size)
		return 0;
	return stat(out, &st) == 0 && S_ISREG(st.st_mode);
}

int main(int argc, char **argv)
{
	const char *real = getenv("CIX_REAL_XORRISO");
	const char *guid = getenv("CIX_ISO_DISK_GUID");
	const char *efi_rel = NULL;
	char gpt_setting[128];
	char efi_abs[PATH_MAX];
	char candidate[PATH_MAX];
	char **out;
	int i, n = 0;
	int writing = 0;
	int found_efi = 0;

	if (real == NULL || real[0] == '\0') {
		fprintf(stderr, "cix-xorriso: CIX_REAL_XORRISO is not set -- "
		                "nothing to hand the filtered arguments to\n");
		return 1;
	}

	/*
	 * grub-mkrescue runs the xorriso it was given TWICE, and the first
	 * call must be left alone. check_xorriso() forks
	 * `xorriso -as mkisofs -help` through a pipe and greps the output
	 * for "graft-points" to decide the binary is usable
	 * (grub-mkrescue.c:337, :509). That argv carries none of the
	 * image-writing arguments, so a wrapper that rewrites or refuses it
	 * fails the check -- and its complaint goes into grub's pipe, where
	 * nothing can see it. Measured on 192.168.15.95, 2026-10-09:
	 * probe-isogrub@8-1 refused that invocation for having no
	 * --efi-boot and grub-mkrescue stopped 5 ms later with
	 * `error: xorriso not found.`, for a binary that was present and
	 * executable. Three earlier revisions of that probe died the same
	 * way with the cause unknown at the time.
	 *
	 * `-o` is the discriminator rather than the absence of the EFI
	 * arguments, deliberately. Keying on those would mean that if
	 * grub-mkrescue ever stopped pushing them, a REAL image write would
	 * be passed through unmodified -- producing media with no ISO9660
	 * partition while grub.cfg still named its PARTUUID, i.e. an
	 * unbootable ISO from a silent success. With `-o` as the test, such
	 * an invocation reaches the rewrite below and fails loudly instead.
	 */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0) {
			writing = 1;
			break;
		}
	}
	if (!writing) {
		argv[0] = (char *)real;
		execv(real, argv);
		perror(real);
		return 127;
	}

	if (guid == NULL || guid[0] == '\0') {
		fprintf(stderr, "cix-xorriso: CIX_ISO_DISK_GUID is not set -- "
		                "the GPT disk GUID decides the ISO9660 partition's "
		                "PARTUUID, which grub.cfg names, so writing media "
		                "without it would produce an unbootable ISO "
		                "(cix#430)\n");
		return 1;
	}
	if ((size_t)snprintf(gpt_setting, sizeof(gpt_setting), "gpt_disk_guid=%s", guid) >=
	    sizeof(gpt_setting)) {
		fprintf(stderr, "cix-xorriso: CIX_ISO_DISK_GUID is too long\n");
		return 1;
	}

	/*
	 * Room for: the real path, the three native -boot_image settings
	 * (three tokens each), everything from argv, the four tokens of
	 * -append_partition, and the NULL. Nothing is ever added per input
	 * argument, so this cannot be exceeded.
	 */
	out = calloc((size_t)argc + 9 + 4 + 2, sizeof(*out));
	if (out == NULL) {
		perror("calloc");
		return 1;
	}

	out[n++] = (char *)real;
	out[n++] = (char *)"-boot_image";
	out[n++] = (char *)"any";
	out[n++] = (char *)"partition_offset=" PARTITION_OFFSET_BLOCKS;
	out[n++] = (char *)"-boot_image";
	out[n++] = (char *)"any";
	out[n++] = gpt_setting;
	out[n++] = (char *)"-boot_image";
	out[n++] = (char *)"any";
	out[n++] = (char *)"appended_part_as=gpt";

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
		if (skipped)
			continue;
		for (k = 0; k < sizeof(esp_args) / sizeof(esp_args[0]); k++) {
			if (strcmp(argv[i], esp_args[k]) == 0) {
				skipped = 1;
				break;
			}
		}
		if (skipped)
			continue;
		/*
		 * `--efi-boot` is KEPT along with its value: the El Torito
		 * catalogue it builds is what boots this medium in an optical
		 * drive, and the value is the path of the copy inside the
		 * filesystem, which is also the file appended below.
		 */
		if (strcmp(argv[i], "--efi-boot") == 0 && i + 1 < argc) {
			efi_rel = argv[i + 1];
			out[n++] = argv[i];
			out[n++] = argv[i + 1];
			i++;
			continue;
		}
		out[n++] = argv[i];
	}

	if (efi_rel == NULL) {
		fprintf(stderr, "cix-xorriso: grub-mkrescue passed no --efi-boot, so "
		                "there is no EFI image to append as the ESP; this "
		                "medium would carry no EFI system partition and no "
		                "ISO9660 partition for grub.cfg's root=PARTUUID= to "
		                "name (cix#430)\n");
		free(out);
		return 1;
	}

	/*
	 * `--efi-boot`'s value is relative to the ISO's root, and the
	 * staged directory holding it arrives as one of the source
	 * arguments -- grub-mkrescue's own temporary tree, whose name
	 * changes every run. So it is found by asking which of the
	 * arguments is a directory that actually contains that file, rather
	 * than by pattern-matching a path shape. Later matches win, since
	 * the content directories come after grub's own staged tree and a
	 * medium's own file should take precedence over grub's if both ever
	 * carried one.
	 */
	efi_abs[0] = '\0';
	for (i = 1; i < n; i++) {
		if (!is_dir(out[i]))
			continue;
		if (!is_file_under(out[i], efi_rel, candidate, sizeof(candidate)))
			continue;
		memcpy(efi_abs, candidate, strlen(candidate) + 1);
		found_efi = 1;
	}
	if (!found_efi) {
		fprintf(stderr, "cix-xorriso: none of the directories grub-mkrescue "
		                "passed contains \"%s\", so the ESP cannot be "
		                "appended (cix#430)\n", efi_rel);
		free(out);
		return 1;
	}

	/*
	 * An emulation option, so it belongs inside the list `-as mkisofs`
	 * consumes -- i.e. at the tail, not with the native settings at the
	 * front. Type 0xef is the EFI system partition; xorriso names the
	 * entry "Appended2" and gives it the ESP type GUID, measured.
	 */
	out[n++] = (char *)"-append_partition";
	out[n++] = (char *)"2";
	out[n++] = (char *)"0xef";
	out[n++] = efi_abs;
	out[n] = NULL;

	execve(real, out, environ);
	perror(real);
	free(out);
	return 127;
}
