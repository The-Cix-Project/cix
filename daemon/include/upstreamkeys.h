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
 * A key enters from the signed catalogue of the source that owns the
 * package, when the host trusts that source for keys (ADR-0326 -- the
 * owner vouches once, in git, and every host follows), or by an
 * operator's call (POST /v1/pkg/{name}/upstream-keys). Never by
 * discovery, and only when the key's own fingerprint
 * (pgp_key_fingerprint()) is the pinned one. There is no trust on first
 * use: nothing here fetches a key.
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
 * spaces allowed, any case) -- an operator's own key, beside the ones the
 * catalogue supplies. Refused, with a reason in err, when the
 * key is not one this platform can verify with, when its fingerprint
 * is not the pinned one, when a source's catalogue already supplies that
 * key, or when the store is full. Adding the same
 * fingerprint again replaces the key. 0, or -1.
 */
int upstreamkeys_add(const char *package, const char *fingerprint, const char *armored,
                     size_t armored_len, long long now, char *err, size_t err_size);

/* 0 removed, 1 when there was no such key, 2 when it came from a source's
 * catalogue (remove it there, or the next sync restores it), -1 when it
 * could not be saved. */
int upstreamkeys_remove(const char *package, const char *fingerprint);

/* The armored key `package` has under `fingerprint`, or NULL. Valid
 * until the store next changes. */
const char *upstreamkeys_find(const char *package, const char *fingerprint, size_t *len);

/* {"package": "...", "keys": [{"fingerprint", "bytes", "added_at", "source"}]}
 * -- source is the catalogue that supplied it, or null for an operator's;
 * public keys, but the listing is what an operator reads, so the armor
 * itself stays out of it. */
void upstreamkeys_write_json(const char *package, struct json_writer *w);

/*
 * ADR-0326: the catalogue is where upstream keys come from. A source a
 * host trusts for keys carries recipes/keys/<package>@<FINGERPRINT>.asc
 * for the packages it owns, and each sync replaces that source's keys
 * with what it offers now -- so a key removed from git leaves every host
 * at its next sync. An offer is taken only when the key's own
 * fingerprint is the one in its file name.
 */
struct upstreamkeys_offer {
	const char *package;
	const char *fingerprint;
	const char *armored;
	size_t len;
};

/*
 * Replaces every key `source` supplied with `offers`. Re-reads the store
 * first, because a sync runs in a helper process holding an older copy.
 * Returns how many were adopted; *refused counts offers whose key could
 * not be used or is not the one its name pins, and `why` names the
 * first. -1 when the store could not be saved.
 */
int upstreamkeys_sync_source(const char *source, const struct upstreamkeys_offer *offers, int n,
                             long long now, int *refused, char *why, size_t why_size);

/* Drops the keys of every source not in `names` (a source an operator
 * removed). 0, or -1 when the store could not be saved. */
int upstreamkeys_retain_sources(const char *const *names, int n);

/* "B886 8C80 ..." or "b8868c80..." -> "B8868C80...": 40 hex digits, or -1. */
int upstreamkeys_normalise(const char *in, char out[41]);

#endif /* UPSTREAMKEYS_H */
