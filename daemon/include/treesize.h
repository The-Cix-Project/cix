#ifndef TREESIZE_H
#define TREESIZE_H

/*
 * The apparent size of a directory tree: the sum of st_size over every
 * regular file and symlink beneath root, directories themselves
 * excluded, symlinks not followed (nftw FTW_PHYS). "How much did
 * something write here", not "how many blocks does the filesystem
 * charge for the tree".
 *
 * It is a walk, O(files) and unbounded. Container stats run it in a
 * helper child (#474); GET /volumes/{name}/usage runs it inline, only
 * when the volume has no qgroup to ask. test_blocking_waits counts
 * every call site, so a new one is a number someone has to raise.
 *
 * This was overlay_upperdir_size() in src/overlay.c, named for the
 * overlay upper layer it first measured. ADR-0360 removed the overlay
 * and the walk outlived it, for trees that are not btrfs subvolumes
 * with a qgroup to ask (cix#658).
 *
 * Returns 0 with *out_bytes set, or -1 with errno set by nftw() when
 * root cannot be walked (a tree mid-teardown, say).
 */
int treesize_bytes(const char *root, long long *out_bytes);

#endif
