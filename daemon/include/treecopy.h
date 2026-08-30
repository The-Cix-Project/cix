#ifndef TREECOPY_H
#define TREECOPY_H

/*
 * A real, permission-preserving recursive directory copy -- shared by
 * every ADR-0141 storage-placement migration job (state/rebuildable/
 * log-storage, and ADR-0142's container-storage-after-creation
 * migration), each of which needs to move a real directory tree across
 * a filesystem boundary (rename(2) can't -- EXDIST across mounted
 * disks) while preserving real file modes: state-storage in particular
 * carries pki.c's own 0600 private keys, and a copy that silently
 * flattened every file to some fixed mode (the way pkg.c's own
 * existing copy_file_simple() intentionally does, for its own,
 * different callers where a fixed mode is already correct) would be a
 * real security regression there.
 *
 * Deliberately a NEW, separate primitive rather than a modification of
 * pkg.c's own merge_tree()/copy_file_simple() -- those are already
 * used at several existing call sites (runtime library staging, recipe
 * file copies, artifact promotion) this project has not individually
 * audited for whether their own fixed-mode behavior is already
 * intentional and correct. Changing shared, already-working,
 * already-tested code as a side effect of adding a new caller would
 * risk a regression nothing here asked for; a new, purpose-built
 * function used only by the new migration callers carries none of
 * that risk (No Regressions).
 */

/*
 * Recursively copies every entry under src_root into dst_root
 * (dst_root itself must already exist), preserving each regular
 * file's own real permission bits (fstat() the source, then chmod()
 * the destination to match) and recreating symlinks verbatim
 * (readlink() + symlink(), never followed/copied-as-a-file -- the
 * same real gap merge_tree()'s own doc comment already found the hard
 * way with iptables' own symlinked binaries, correctly avoided here
 * from the start). Directories are created as needed with mode 0755
 * (a directory's own permission bits are a host-layout concern, not
 * sensitive content the way a file's contents might be -- no real
 * caller here has a directory that needs anything stricter).
 *
 * A source root that does not exist, or cannot be opened, is a
 * FAILURE -- not an empty success. Every caller is a migration, a
 * backup or a restore, and for those a copy that moved nothing must
 * never be indistinguishable from one that moved everything (#194).
 *
 * Returns 0 on success, -1 (errno set to the first failure
 * encountered) otherwise -- a partial copy may be left behind on
 * failure, which is fine: every caller here only ever proceeds to
 * repoint anything live once this returns 0, so a failed copy simply
 * leaves the old, still-fully-functional location in charge, the
 * same "job failed, state left as whatever the last completed step
 * produced" posture diskformat.c's own FAILED state already has.
 */
int treecopy_recursive(const char *src_root, const char *dst_root);

/*
 * Why the last treecopy_recursive() failed: "<what>: <path>: <strerror>".
 * errno alone is useless to an operator -- "No such file or directory"
 * out of a multi-gigabyte migration names neither the entry nor the
 * operation. Issue #172: a real rebuildable-storage migration failed
 * twice with nothing but "bulk copy failed" recorded anywhere.
 * Valid until the next treecopy_recursive() call.
 */
const char *treecopy_last_error(void);

#endif /* TREECOPY_H */
