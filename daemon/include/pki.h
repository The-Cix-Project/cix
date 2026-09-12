#ifndef PKI_H
#define PKI_H

#include "json.h"

#include <sys/types.h>

/*
 * Phase 9 part 1: a single internal root CA plus REST-managed leaf
 * certificate issuance. Actual cryptography (keypair generation, CSR
 * signing) is done by the daemon shelling out to the system's real,
 * unmodified `openssl` binary as a short-lived subprocess -- never
 * linked into the daemon (ADR-0007's "no third-party dependency
 * footprint anywhere in the daemon" stays intact: exec'ing isn't
 * linking) and never hand-rolled (the same "real software, not
 * hand-rolled" reasoning BIRD and dnsmasq were chosen under in
 * Phases 7 and 8).
 *
 * The CA private key is never returned over the API, in any endpoint,
 * ever -- it is the root of trust and must never leave the host. Leaf
 * certificate private keys ARE returned, but exactly once, in the
 * POST /v1/pki/certs response at the moment of issuance -- neither
 * the list nor the single-get endpoint ever includes one again
 * afterward, matching the "credential shown once" pattern most real
 * ACME-shaped systems use.
 */

#define PKI_MAX_CERTS 256
#define PKI_MAX_SANS 8
#define PKI_SERIAL_MAX 64
#define PKI_DATE_MAX 32
#define PKI_SUBJECT_MAX 128 /* CA common_name only -- a free-form display label, not a hostname */

enum pki_error {
	PKI_OK = 0,
	PKI_ERR_INVALID_NAME,
	PKI_ERR_NOT_BOOTSTRAPPED,
	PKI_ERR_ALREADY_BOOTSTRAPPED,
	PKI_ERR_DUPLICATE,
	PKI_ERR_FULL,
	PKI_ERR_NOT_FOUND,
	PKI_ERR_OPENSSL_FAILED,
	PKI_ERR_PERSIST_FAILED
};

/*
 * pki_dir is the root of the on-disk layout (ca.key, ca.crt, ca.srl,
 * certs/<name>.{key,crt}); certs_state_path is the persisted leaf-cert
 * metadata index (name/serial/not_after/sans -- a fast-listing cache,
 * not the source of truth for correctness; the key/cert files on disk
 * are authoritative). Loads that index, if any, at startup.
 */
/*
 * Forks and execve()s PKI_OPENSSL_BIN with argv (NULL-terminated,
 * argv[0] conventionally the binary path), capturing the child's
 * stdout AND stderr into out when non-NULL. Returns 0 only when the
 * child exited 0.
 *
 * Exported, rather than kept static here, so signingkeys.c can reuse
 * the one place this project talks to the openssl binary instead of
 * growing a second copy of the same fork/exec/capture logic. The
 * captured output is for server-side diagnostics and for parsing
 * `-noout` query results -- never echo it raw into an HTTP response,
 * since openssl's own errors can quote input material.
 */
/* Defined in opensslrun.c -- see opensslrun.h for why it lives there. */
#include "opensslrun.h"

int pki_init(const char *pki_dir, const char *certs_state_path);

/* ADR-0141 Phase 2: repoints without reloading g_certs[] -- see
 * network_repoint()'s own doc comment for the shared reasoning.
 * Returns -1 on a path-too-long error, 0 otherwise. */
int pki_repoint(const char *new_pki_dir, const char *new_certs_state_path);

int pki_ca_bootstrapped(void);

/* common_name must not contain '/' or control characters -- it is
 * embedded verbatim into an openssl `-subj "/CN=<common_name>"`
 * argument, and an unvalidated '/' would let a caller inject
 * additional, unintended DN fields (e.g. "Foo/O=EvilOrg"). */
enum pki_error pki_ca_create(const char *common_name, int days);

/* Writes CA info (subject, serial, not_before, not_after, cert_pem --
 * never the key) straight into w, read fresh from ca.crt on disk via
 * `openssl x509 -noout ...` every call -- no in-memory cache to keep
 * in sync, One Source of Truth, and this is a low-frequency
 * management call so the extra subprocess cost is a non-issue. */
enum pki_error pki_ca_get(struct json_writer *w);

int pki_intermediate_bootstrapped(void);

/*
 * Reads the intermediate CA's own cert PEM straight off disk, if one
 * is bootstrapped -- for a caller (main.c's TLS listener setup,
 * ADR-0136) that needs to add it as an *extra* chain certificate
 * alongside an already-loaded leaf, distinct from pki_cert_deliver()'s
 * own chain_pem (which builds a full leaf+intermediate file for a
 * container to receive) and pki_write_trust_bundle_file()'s own
 * root+intermediate trust bundle (a different concern -- what this
 * host trusts outbound, not what it presents inbound). Returns 1 and
 * fills *out_pem (malloc'd, caller frees)/*out_len if bootstrapped, 0
 * (leaving *out_pem untouched) if not, -1 on a real read failure.
 */
int pki_intermediate_cert_pem(char **out_pem, size_t *out_len);

/*
 * Creates a second CA tier (ADR-0046) -- a real intermediate keypair
 * and cert, signed BY the root (not self-signed), with
 * basicConstraints=CA:TRUE and keyUsage=keyCertSign,cRLSign baked in
 * via -addext at CSR-creation time, the same -addext-then-
 * -copy_extensions-at-signing pattern pki_cert_create()'s own leaf
 * issuance already established. Requires the root to already be
 * bootstrapped (PKI_ERR_NOT_BOOTSTRAPPED otherwise);
 * PKI_ERR_ALREADY_BOOTSTRAPPED if an intermediate already exists --
 * there is no rotate/replace operation in v1, matching pki_ca_create()'s
 * own one-shot-only precedent for the root. Once this succeeds,
 * pki_cert_create() signs every future leaf with the intermediate
 * instead of the root automatically -- no separate opt-in, and no
 * change to pki_cert_create()'s own call signature or callers. The
 * root key's own exposure is unchanged by any of this: it signs
 * exactly one thing, this intermediate, once.
 */
enum pki_error pki_intermediate_create(const char *common_name, int days);

/* Same shape as pki_ca_get(), for the intermediate instead of the
 * root -- PKI_ERR_NOT_BOOTSTRAPPED if no intermediate exists yet. */
enum pki_error pki_intermediate_get(struct json_writer *w);

/*
 * Issues a leaf certificate named after `name` (used verbatim as its
 * CN), with the given SANs (san_count must be >=1; every entry,
 * including name itself if the caller wants it in the SAN list too,
 * is validated with dns_name_is_valid() -- a leaf cert's identity is
 * conceptually a hostname, exactly like a DNS record's name).
 * Writes the full response -- including cert_pem AND key_pem, the
 * only place the leaf private key is ever returned -- straight into
 * w on success. Signed by the intermediate CA if one has been
 * bootstrapped (pki_intermediate_create()), by the root directly
 * otherwise -- transparent to every existing caller, no API change.
 */
enum pki_error pki_cert_create(const char *name, const char *const *sans, int san_count, int days,
                                const char *owner_container, struct json_writer *w);

enum pki_error pki_cert_delete(const char *name);

/* 1 if a cert named `name` exists and its owner_container is exactly
 * `owner`, 0 otherwise (including "no such cert" -- not itself an
 * error here, just "nothing owned"). For callers deciding whether to
 * redeliver into a specific live container without needing the full
 * JSON-serialized metadata shape. */
int pki_cert_owned_by(const char *name, const char *owner);

/* 1 if a cert named `name` is in the index, 0 otherwise. Says nothing
 * about ownership -- deliberately, since a cert's identity (its name
 * and SANs) and its lifetime (owner_container) are separate concerns
 * (ADR-0280). Exists so a caller can validate "deliver the cert called
 * X into this container" up-front and refuse with a 400 naming the
 * missing cert, rather than creating the container and discovering at
 * delivery time that there is nothing to deliver. */
int pki_cert_exists(const char *name);

/* Best-effort cleanup on container deletion: deletes name's cert iff
 * it exists and its owner_container is name itself (via
 * pki_cert_delete() -- one deletion code path, not two). Safe no-op
 * for every container that never had an auto-issued cert, so this is
 * called unconditionally from the container-delete handler. Mirrors
 * dns_record_forget_owner()'s exact shape. */
void pki_cert_forget_owner(const char *container_name);

/*
 * Delivers an already-issued cert (name must already exist, i.e.
 * pki_cert_create() succeeded first -- this is a separate step, not
 * part of issuance itself) into a running container's own filesystem:
 * writes <dest_dir>/tls.crt and <dest_dir>/tls.key (chmod 0600 on the
 * key) via /proc/<pid>/root/<dest_dir>/..., the same
 * /proc/<pid>/root/ pattern dns_server_register() already established
 * (ADR-0013) for reaching into a running container from outside it.
 * One-time: unlike DNS server bindings, there is no live-resync
 * mechanism here -- a cert doesn't change after a container starts.
 * `name` identifies the CERTIFICATE; `container_name` identifies the
 * container whose tree is written into. They are separate because
 * ADR-0280 lets a container be handed a cert named something else
 * entirely -- they were equal for every caller before that, which is
 * exactly why this function used to take one parameter for both and
 * silently wrote to a nonexistent container's path when they differed
 * (#397).
 *
 * tls.crt is the real, complete chain a TLS server needs to present
 * (leaf + intermediate, in that order -- the standard fullchain.pem
 * convention) if an intermediate has been bootstrapped, the leaf
 * alone otherwise -- transparent to every existing caller.
 */
enum pki_error pki_cert_deliver(const char *name, const char *container_name, pid_t pid,
                                 const char *dest_dir);

/*
 * Wipes and regenerates the entire CA chain (root, plus the
 * intermediate too if one already existed) with new common names,
 * then reissues every leaf cert currently tracked -- same name/SANs/
 * owner, a fresh keypair and validity period for each. This is the
 * only way to change an already-bootstrapped CA's subject:
 * pki_ca_create()/pki_intermediate_create() are one-shot by design and
 * refuse a second call outright (PKI_ERR_ALREADY_BOOTSTRAPPED) -- this
 * is the explicit, real "start over" operation that design
 * deliberately doesn't provide implicitly (see ADR for this).
 *
 * Requires the root to already be bootstrapped (PKI_ERR_NOT_BOOTSTRAPPED
 * otherwise -- use pki_ca_create() directly for a genuinely first-ever
 * bootstrap). Re-creates the intermediate only if one existed before
 * this call; an install that never bootstrapped one doesn't gain one
 * just by resetting the root.
 *
 * Writes {"root": <same shape as pki_ca_get()>, "intermediate":
 * <same shape as pki_intermediate_get(), or null>, "reissued": [
 * <same shape as pki_cert_create()'s own response, one per
 * successfully reissued leaf> ]} into w. Every reissued leaf's
 * cert_pem AND key_pem are included -- this is a genuine new issuance
 * moment for each (a brand-new keypair, signed by the new chain), so
 * it gets the identical "returned exactly once, right now" treatment
 * pki_cert_create()'s own response already gives a leaf at first
 * issuance; there is no second chance to retrieve a reissued leaf's
 * private key after this call returns. A leaf whose reissue itself
 * fails (logged to stderr, non-fatal to the rest of the batch) is
 * simply gone afterward, not left in its old state -- its old
 * key/cert are already unlinked before any reissue is attempted, since
 * they're signed by a CA that no longer exists the moment this
 * proceeds.
 *
 * Does NOT redeliver a reissued leaf into any live container that
 * owns it -- pki.c has no dependency on containerdef.c/registry.c to
 * look up a live pid or that container's own --pki-cert-dir. The REST
 * handler layer already does exactly this kind of orchestration for
 * --pki-issue at container-create time and is where it belongs here
 * too (replay the same check for every currently-live container after
 * a successful reset).
 */
enum pki_error pki_ca_reset(const char *root_common_name, const char *intermediate_common_name,
                             int root_days, int intermediate_days, int leaf_days,
                             struct json_writer *w);

/*
 * Writes the current CA trust chain (root alone, or root+intermediate
 * if one is bootstrapped -- order doesn't matter for a pure trust-
 * anchor bundle the way it does for pki_cert_deliver()'s own
 * leaf-first fullchain.pem) to dest_path. For staging into a
 * container image's own trust store (ADR-0051) -- a different job
 * from pki_cert_deliver()'s "hand a running container its own leaf
 * identity." PKI_ERR_NOT_BOOTSTRAPPED if no root exists yet.
 */
enum pki_error pki_write_trust_bundle_file(const char *dest_path);

/* Metadata only (name/serial/not_after/sans/owner) -- no cert_pem, no key_pem. */
void pki_write_json_list(struct json_writer *w);

/* Metadata + cert_pem (read fresh from the .crt file on disk) -- still no key_pem. */
enum pki_error pki_cert_get_one(const char *name, struct json_writer *w);

#endif /* PKI_H */
