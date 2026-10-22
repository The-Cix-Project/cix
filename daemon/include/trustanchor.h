#ifndef TRUSTANCHOR_H
#define TRUSTANCHOR_H

#include <stddef.h>

#include "json.h"
#include "pkicrypto.h"

/* The root's own trust store, where the ca-certificates package puts
 * its public roots and where libcurl looks by default (curl's
 * --with-ca-bundle, cix#589). */
#define TRUSTANCHOR_PUBLIC_BUNDLE "/etc/ssl/certs/ca-certificates.crt"

/*
 * ADR-0350: the CAs this host's operator registers, beyond the public
 * roots the root carries from the `ca-certificates` package.
 *
 * Persisted in the state directory, so an anchor survives updates and
 * leaves only with a factory reset. Never written into the
 * control-plane root, because that root is published inside the
 * installer ISO. cixd's one libcurl handle trusts the public roots and
 * these together, through a combined bundle this module writes beside
 * the store (trustanchor_bundle()).
 *
 * An anchor is supplied by an operator and never fetched: fetching is
 * what needs it.
 */

#define TRUSTANCHOR_MAX 16
#define TRUSTANCHOR_PEM_MAX 16384

struct trustanchor {
	struct pkicrypto_anchor info;
	long long added_at; /* unix seconds */
	size_t pem_len;
	char pem[TRUSTANCHOR_PEM_MAX];
};

enum trustanchor_result {
	TRUSTANCHOR_OK = 0,
	TRUSTANCHOR_INVALID,   /* the reason is in err */
	TRUSTANCHOR_DUPLICATE, /* already registered */
	TRUSTANCHOR_FULL,      /* TRUSTANCHOR_MAX reached */
	TRUSTANCHOR_NOT_FOUND,
	TRUSTANCHOR_PERSIST_FAILED
};

/*
 * Loads the store from store_path and writes the combined bundle.
 * public_bundle_path is the root's own trust store (the package's
 * public roots); bundle_path is where the combined one goes. Returns 0,
 * or -1 when the store exists and cannot be read -- a store silently
 * read as empty would drop the operator's anchors at the next write.
 */
int trustanchor_init(const char *store_path, const char *bundle_path,
                     const char *public_bundle_path);

int trustanchor_count(void);
const struct trustanchor *trustanchor_get(int i);
const struct trustanchor *trustanchor_find(const char *fingerprint);

/* Adds one anchor from an operator's PEM. fingerprint_out (65 bytes)
 * names it on success. */
enum trustanchor_result trustanchor_add(const char *pem, size_t pem_len, char *fingerprint_out,
                                        char *err, size_t err_size);
enum trustanchor_result trustanchor_remove(const char *fingerprint);

/*
 * The combined bundle in force: the root's public roots followed by
 * every anchor. NULL when there are no anchors, or when the combined
 * file could not be written, and then a fetch uses libcurl's own
 * default -- the root's public bundle, which is today's behaviour.
 */
const char *trustanchor_bundle(void);

/*
 * Called with trustanchor_bundle() whenever it may have changed: at
 * init, and after every add and remove. cixd passes
 * curlfetch_set_ca_bundle, so the store says what is in force without
 * linking libcurl. Set it before trustanchor_init().
 */
void trustanchor_on_bundle(void (*fn)(const char *bundle));

/*
 * One anchor as a JSON object: fingerprint, subject, issuer, not_after,
 * root, expired, added_at -- and pem when with_pem is set, which the
 * config document needs to recreate the anchor and the REST list does
 * not.
 */
void trustanchor_write_json(struct json_writer *w, const struct trustanchor *a, int with_pem);

#endif /* TRUSTANCHOR_H */
