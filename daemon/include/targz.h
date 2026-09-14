#ifndef TARGZ_H
#define TARGZ_H

/*
 * Creating a gzip-compressed tar without a shell anywhere in the chain
 * (issue #164).
 *
 * GNU tar spawns `--use-compress-program=` (and `-z`) THROUGH /bin/sh.
 * This platform's own control-plane root has no shell at all -- and
 * deliberately so: nothing in cixd uses popen()/system(), every
 * subprocess it runs is an explicit absolute-path execve(). So every
 * compressed archive the daemon tried to create on a real installed
 * host failed, while decompression (`-xzf`/`-xf`, which tar handles
 * without a shell) kept working perfectly. That asymmetry is why this
 * went unnoticed for so long: packages installed fine, but no image or
 * package artifact could ever be exported, and pkg_cache_save() never
 * cached a single build.
 *
 * Worse, the failure named the wrong thing. tar reports the failed
 * exec against the COMPRESS PROGRAM, so a missing shell surfaced as
 *   tar (child): /usr/bin/gzip: Cannot exec: No such file or directory
 * on a host where /usr/bin/gzip was present, executable, and ran
 * correctly on its own -- confirmed by running it there. Reproduced
 * exactly in a local shell-less chroot, and confirmed fixed by adding
 * a shell to that same chroot; the shell, not gzip, was always the
 * missing piece.
 *
 * The fix removes the dependency rather than satisfying it: we build
 * the pipeline ourselves -- `tar -cf -` writing into a pipe that
 * `gzip -c` reads -- with both halves exec'd directly by absolute
 * path. No shell, and no temporary uncompressed copy either (an image
 * export is gigabytes; writing a plain .tar first and compressing it
 * afterwards would double the peak disk cost on the very partition
 * being exported).
 *
 * The bytes produced are IDENTICAL to what `--use-compress-program`
 * produced before, which matters: these archives are published to a
 * content-addressed artifact store whose contract is that one name
 * means one byte sequence forever. gzip reading a pipe records no
 * original filename and a zero mtime -- exactly as it did when tar
 * fed it -- so already-published artifacts stay valid and a rebuild
 * of the same tree still hashes the same. The same normalizing tar
 * flags (--sort=name/--mtime=@0/--owner=0/--group=0/--numeric-owner,
 * issue #129) are applied here, in one place, rather than repeated at
 * each call site.
 */

/*
 * The one place these two binary paths are spelled (One Source of
 * Truth) -- daemon/src/main.c's own export path never hardcodes them.
 * daemon/src/pkg.c no longer aliases either: extraction moved to
 * libarchive in-process (#411), so only targz.c's own creation
 * pipeline (tar -cf - | gzip -c, both above) still execve()s them.
 */
#define TARGZ_TAR_BIN "/usr/bin/tar"
#define TARGZ_GZIP_BIN "/usr/bin/gzip"

/*
 * Exit codes targz_run() returns, chosen so a caller can tell a
 * missing binary apart from a tool that ran and failed -- the same
 * distinction daemon/src/diskformat.c already draws, and for the same
 * reason: "it exited nonzero" is not a diagnosis.
 */
#define TARGZ_EXIT_OK 0
#define TARGZ_EXIT_FAILED 1       /* tar or gzip ran and failed */
#define TARGZ_EXIT_SETUP 2        /* could not open the output file, or pipe()/fork() failed */
#define TARGZ_EXIT_NO_BINARY 127  /* tar or gzip could not be exec'd at all */

/*
 * Archives src_dir's own CONTENTS (tar -C src_dir ... ".") into
 * out_path as a gzip-compressed tar.
 *
 * Call this INSIDE an already-forked child and _exit() with what it
 * returns: it forks two grandchildren of its own and waits for both,
 * so it must not run in a process whose other children are being
 * waited on elsewhere. Returns one of the TARGZ_EXIT_* codes above.
 * The caller's own stderr is inherited by both halves, so redirecting
 * fd 2 before calling captures whatever tar or gzip says.
 */
int targz_run(const char *src_dir, const char *out_path);

/*
 * Synchronous convenience wrapper: forks a child, runs targz_run() in
 * it, waits. Returns 0 on success, or -1 with the child's own
 * TARGZ_EXIT_* code written to *out_code when out_code is non-NULL
 * (pass NULL if the distinction doesn't matter to the caller).
 */
int targz_create(const char *src_dir, const char *out_path, int *out_code);

#endif /* TARGZ_H */
