#ifndef UPSTREAMKEYS_H
#define UPSTREAMKEYS_H

#include <stddef.h>

#include "json.h"

/*
 * ADR-0318's upstream key store: the OpenPGP keys that may authenticate
 * an upstream release of ONE package -- the key that signs kernel.org's
 * checksum lists, say. It is its own store, apart from the artifact
 * trust directory and docs/keys: a key that approves the kernel's
 * sources must not be able to approve a Cix artifact, nor hibr's
 * sources (ADR-0318, kept by ADR-0323).
 *
 * A key enters only by an operator's call (POST /v1/pkg/{name}/upstream-
 * keys), never by discovery, and only when the fingerprint the operator
 * pins is the key's own (pgp_key_fingerprint()). There is no trust on
 * first use: nothing here fetches a key.
 *
 * A recipe names the key it is authenticated by -- `verify checksums
 * ... { key "<fingerprint>" }` -- and the lookup is always by package
 * AND fingerprint, so a key installed for one package answers for no
 * other.
 *
 * Persisted: a key is the operator's decision and must outlive a restart.
 */

#define UPSTREAMKEYS_MAX 64              /* across every package */
#define UPSTREAMKEYS_KEY_MAX (64 * 1024) /* armored bytes */

int upstreamkeys_init(const char *path);

/*
 * Adds `armored` for `package` under the pinned `fingerprint` (hex,
 * spaces allowed, any case). Refused, with a reason in err, when the
 * key is not one this platform can verify with, when its fingerprint
 * is not the pinned one, or when the store is full. Adding the same
 * fingerprint again replaces the key. 0, or -1.
 */
int upstreamkeys_add(const char *package, const char *fingerprint, const char *armored,
                     size_t armored_len, long long now, char *err, size_t err_size);

/* 0 removed, 1 when there was no such key, -1 when it could not be saved. */
int upstreamkeys_remove(const char *package, const char *fingerprint);

/* The armored key `package` has under `fingerprint`, or NULL. Valid
 * until the store next changes. */
const char *upstreamkeys_find(const char *package, const char *fingerprint, size_t *len);

/* {"package": "...", "keys": [{"fingerprint", "bytes", "added_at"}]} --
 * public keys, but the listing is what an operator reads, so the armor
 * itself stays out of it. */
void upstreamkeys_write_json(const char *package, struct json_writer *w);

/* "B886 8C80 ..." or "b8868c80..." -> "B8868C80...": 40 hex digits, or -1. */
int upstreamkeys_normalise(const char *in, char out[41]);

#endif /* UPSTREAMKEYS_H */
