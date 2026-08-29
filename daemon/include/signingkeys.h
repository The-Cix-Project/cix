#ifndef SIGNINGKEYS_H
#define SIGNINGKEYS_H

#include "json.h"

/*
 * GET/PUT/DELETE /v1/system/signing-keys (ADR-0212): the Secure Boot
 * signing key pair POST /v1/system/iso needs, at SIGNING_KEYS_DIR
 * (<data-dir>/keys/cix-signing.{key,crt,cer}).
 *
 * ADR-0064 made this directory an operator-populated-out-of-band
 * precondition and compared it to `pkg bootstrap --toolchain=`
 * requiring "a real pre-built artifact already scp'd onto the box".
 * That comparison is what turned out to be wrong: a real Cix host runs
 * no sshd, exposes no host-side exec, and has no console. There has
 * never been a way to put a file in this directory, so POST
 * /v1/system/iso has never been servable on any real installed box --
 * confirmed by walking every /v1/system/* route and by ssh to
 * 192.168.15.95 being refused outright.
 *
 * So the transport becomes REST, which is what every other operator
 * secret here already uses: pkg_repo_set_config() takes the git token
 * this same way and reports "auth_token_set" rather than the token.
 * This module holds to that: the private key goes in, and nothing --
 * no GET, no error message, no log line -- ever brings it back out.
 *
 * ADR-0064's actual security property is untouched. Its objection was
 * to cixd putting a release-signing key "onto every box that happens
 * to run it", because then any one box's compromise leaks the fleet's
 * Secure Boot key. Installing it is still a deliberate operator act
 * against one designated release host; cixd still never generates,
 * fetches, or copies it on its own initiative, and still stages only
 * the public DER cert onto an installed target for MOK enrolment.
 */

enum signingkeys_error {
	SIGNINGKEYS_OK = 0,
	SIGNINGKEYS_ERR_BAD_KEY,      /* private key is not parseable PEM */
	SIGNINGKEYS_ERR_BAD_CERT,     /* certificate is not parseable PEM */
	SIGNINGKEYS_ERR_MISMATCH,     /* cert's public key isn't this private key's */
	SIGNINGKEYS_ERR_PERSIST_FAILED
};

/*
 * Remembers keys_dir and builds the three real paths under it. Creates
 * the directory if it doesn't exist: a fresh install has never had one,
 * and the very first PUT would otherwise fail on a missing parent for
 * no reason an operator could act on. Reads nothing -- there is no
 * in-memory copy of any of this, deliberately; every query re-reads the
 * files, so what the API reports is what POST /system/iso will actually
 * find rather than a cache that can disagree with the disk.
 */
int signingkeys_init(const char *keys_dir);

/*
 * ADR-0141 Phase 2: repoints at a moved state directory without
 * touching content, the same shape resolv_repoint()/pki_repoint()
 * already have. SIGNING_KEYS_DIR lives under STATE_DIR precisely
 * because it is "this platform's own irreplaceable definition of
 * itself", so it moves when state-storage moves.
 */
void signingkeys_repoint(const char *new_keys_dir);

/*
 * Installs an operator-supplied pair. key_pem and cert_pem are the two
 * PEM blocks; the DER .cer mokutil needs is derived from cert_pem here
 * rather than being a third input to keep consistent by hand.
 *
 * Everything is validated before anything is written, and the write
 * itself is atomic (temporaries renamed into place), so a rejected or
 * half-finished PUT leaves the previous pair exactly as it was. The
 * mismatch check is not defensive padding: pasting a key and a cert
 * that don't belong together is the realistic failure mode of a
 * copy-paste interface, and it would otherwise be discovered as an
 * image that signs cleanly and then refuses to boot.
 */
enum signingkeys_error signingkeys_set(const char *key_pem, const char *cert_pem);

/*
 * Erases all three files. A release host being decommissioned or
 * repurposed shouldn't keep signing material it no longer needs, and
 * without this the only way to remove it would be to reinstall the box.
 * Absent files are not an error -- the post-state is what was asked for.
 */
enum signingkeys_error signingkeys_clear(void);

/*
 * {"key_set","cert_set","subject","not_after","fingerprint_sha256"} --
 * never the key itself. The three certificate fields are read live via
 * openssl and are null when no certificate is installed; they exist so
 * an operator can confirm which identity a host holds (and compare the
 * fingerprint against the cert they enrolled as a MOK) without needing
 * any way to read the material back.
 */
void signingkeys_write_json(struct json_writer *w);

#endif /* SIGNINGKEYS_H */
