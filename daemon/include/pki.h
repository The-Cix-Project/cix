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
int pki_init(const char *pki_dir, const char *certs_state_path);

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
 * tls.crt is the real, complete chain a TLS server needs to present
 * (leaf + intermediate, in that order -- the standard fullchain.pem
 * convention) if an intermediate has been bootstrapped, the leaf
 * alone otherwise -- transparent to every existing caller.
 */
enum pki_error pki_cert_deliver(const char *name, pid_t pid, const char *dest_dir);

/* Metadata only (name/serial/not_after/sans/owner) -- no cert_pem, no key_pem. */
void pki_write_json_list(struct json_writer *w);

/* Metadata + cert_pem (read fresh from the .crt file on disk) -- still no key_pem. */
enum pki_error pki_cert_get_one(const char *name, struct json_writer *w);

#endif /* PKI_H */
