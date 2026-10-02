#ifndef CATALOGUE_H
#define CATALOGUE_H

#include <stddef.h>

/*
 * ADR-0324 step C: the signed catalogue index, as Debian signs its
 * archive. A source's tree carries recipes/INDEX -- the sha256 and path
 * of every file a sync takes from it -- and recipes/INDEX.minisig, a
 * minisign signature over the index by that source's catalogue key. A
 * host configured with the key takes from the tree only what the
 * signed index vouches for; a host without one syncs as it always has.
 *
 * Covered: every regular file in recipes/package, recipes/image,
 * recipes/deployment and docs/keys -- docs/keys because a key a sync
 * adopts grants artifact trust, the most sensitive thing a tree holds.
 * The index and its signature are not covered, and the header says so:
 * an index that listed itself could never be written, and one that
 * changed when re-written would be re-signed on every sync.
 *
 * Pure: no network, no keys. Hashing is the caller's function, so the
 * daemon passes its own (pkg_run_capture_sha256) and this module has no
 * second implementation of it.
 */

#define CATALOGUE_INDEX_PATH "recipes/INDEX"
#define CATALOGUE_SIG_PATH "recipes/INDEX.minisig"
#define CATALOGUE_SHA256_HEX 65

/* 0 and a 64-hex digest in out, or -1. */
typedef int (*catalogue_hash_fn)(const char *path, char *out, size_t out_size);

/*
 * The index of the tree at `root` (the repository's top directory):
 * a two-line header, then "<sha256>  <path>\n" per covered file, sorted
 * by path. malloc'd and NUL-terminated, *out_len its length; NULL when a
 * covered file cannot be hashed. A directory that does not exist
 * contributes nothing, so a tree without images still has an index.
 */
char *catalogue_build(const char *root, catalogue_hash_fn hash, size_t *out_len);

/* The signature's trusted comment for the index whose sha256 is index_sha:
 * "cix catalogue sha256=<hex> time=<t>". 0, or -1 when it does not fit. */
int catalogue_comment(const char *index_sha, long long t, char *out, size_t out_size);

/*
 * Reads a trusted comment back. 0 with *out_t set when it names exactly
 * this index (`index_sha`) in the expected shape; -1 for anything else.
 */
int catalogue_comment_parse(const char *comment, const char *index_sha, long long *out_t);

/* A parsed index: path -> sha256. */
struct catalogue;

struct catalogue *catalogue_parse(const char *text, size_t len);
void catalogue_free(struct catalogue *c);
int catalogue_count(const struct catalogue *c);

/*
 * Whether the index vouches for `path` (relative to the tree, e.g.
 * "recipes/package/zlib@1.3.2-12.cbs") holding exactly `sha`.
 */
int catalogue_vouches(const struct catalogue *c, const char *path, const char *sha);

#endif /* CATALOGUE_H */
