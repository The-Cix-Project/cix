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
 * Returns 0 on success, -1 (errno set to the first failure
 * encountered) otherwise -- a partial copy may be left behind on
 * failure, which is fine: every caller here only ever proceeds to
 * repoint anything live once this returns 0, so a failed copy simply
 * leaves the old, still-fully-functional location in charge, the
 * same "job failed, state left as whatever the last completed step
 * produced" posture diskformat.c's own FAILED state already has.
 */
int treecopy_recursive(const char *src_root, const char *dst_root);

#endif /* TREECOPY_H */
