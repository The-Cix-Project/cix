#include "api_keys.h"

#include "apiresp.h"
#include "http.h"
#include "json.h"
#include "signingkeys.h"
#include "releasekey.h"

#include <stdio.h>
#include <string.h>

/*
 * ADR-0212: GET/PUT/DELETE /v1/system/signing-keys -- the Secure Boot
 * signing key pair POST /v1/system/iso requires. See signingkeys.h for
 * why this is a REST surface at all rather than the out-of-band-only
 * precondition ADR-0064 originally specified.
 *
 * The PUT body carries a private key. It is therefore never echoed
 * back, never logged, and never quoted in an error -- the responses
 * below are the same key_set/cert_set summary GET returns, exactly as
 * pkg_repo_write_json_config() reports auth_token_set and not the git
 * token it was given.
 */
void handle_signing_keys_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	signingkeys_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void respond_signingkeys_error(int fd, enum signingkeys_error err)
{
	switch (err) {
	case SIGNINGKEYS_ERR_BAD_KEY:
		respond_error(fd, 400, "Bad Request",
		              "key is not a parseable PEM private key");
		return;
	case SIGNINGKEYS_ERR_BAD_CERT:
		respond_error(fd, 400, "Bad Request", "cert is not a parseable PEM certificate");
		return;
	case SIGNINGKEYS_ERR_MISMATCH:
		/*
		 * Worth its own message rather than a generic 400: both blobs
		 * are individually valid here, so "invalid PEM" would send an
		 * operator looking at the wrong thing. Pasting two halves that
		 * do not belong together is the realistic mistake this
		 * interface enables.
		 */
		respond_error(fd, 400, "Bad Request",
		              "the certificate's public key does not match the private key -- they are "
		              "not a pair");
		return;
	case SIGNINGKEYS_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "could not persist the signing key pair");
		return;
	}
}

void handle_signing_keys_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *key_pem;
	const char *cert_pem;
	enum signingkeys_error serr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	key_pem = json_as_string(json_object_get(root, "key"));
	cert_pem = json_as_string(json_object_get(root, "cert"));
	if (key_pem == NULL || cert_pem == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "key and cert (both PEM strings) are required");
		return;
	}

	serr = signingkeys_set(key_pem, cert_pem);
	json_free(root);
	if (serr != SIGNINGKEYS_OK) {
		respond_signingkeys_error(fd, serr);
		return;
	}

	jw_init(&w);
	signingkeys_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_signing_keys_delete(int fd)
{
	enum signingkeys_error serr = signingkeys_clear();
	struct json_writer w;

	if (serr != SIGNINGKEYS_OK) {
		respond_signingkeys_error(fd, serr);
		return;
	}
	jw_init(&w);
	signingkeys_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * ADR-0220: GET/PUT/DELETE /v1/system/release-key -- the Ed25519 key
 * that signs published artifacts. Deliberately not the Secure Boot pair
 * above: that one is RSA because UEFI mandates RSA and answers "may
 * this firmware boot this image?"; this one answers "did Cix publish
 * these bytes?". One key for both would mean whoever can sign a
 * download can sign a bootloader.
 *
 * Same discipline as the pair above: the PUT body carries a private
 * key, so it is never echoed, logged, or quoted in an error. The public
 * half is different -- it exists to be published, and comes back in
 * minisign's own format so an operator can hand it to a verifier
 * unchanged.
 */
static void write_release_key_json(struct json_writer *w)
{
	char pub[256];
	char id_hex[RELEASEKEY_ID_HEX_SIZE];

	jw_obj_open(w);
	jw_key(w, "key_set");
	jw_bool(w, releasekey_is_set());
	jw_key(w, "public_key");
	if (releasekey_is_set() && releasekey_public(pub, sizeof(pub)) == RELEASEKEY_OK)
		jw_str(w, pub);
	else
		jw_null(w);
	jw_key(w, "key_id");
	if (releasekey_is_set() && releasekey_key_id_hex(id_hex, sizeof(id_hex)) == RELEASEKEY_OK)
		jw_str(w, id_hex);
	else
		jw_null(w);
	jw_obj_close(w);
}

void handle_release_key_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	write_release_key_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_release_key_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *key_pem;
	enum releasekey_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	key_pem = json_as_string(json_object_get(root, "key"));
	if (key_pem == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "key (a PEM string) is required");
		return;
	}

	rerr = releasekey_set(key_pem, strlen(key_pem));
	json_free(root);
	if (rerr == RELEASEKEY_ERR_BAD_KEY) {
		/*
		 * Names the algorithm rather than saying "invalid key",
		 * because the realistic mistake here is pasting the RSA
		 * Secure Boot key sitting right beside this one -- a
		 * perfectly valid key that this endpoint cannot use.
		 */
		respond_error(fd, 400, "Bad Request",
		              "key is not a parseable Ed25519 private key -- generate one with "
		              "'openssl genpkey -algorithm ed25519'");
		return;
	}
	if (rerr != RELEASEKEY_OK) {
		respond_error(fd, 500, "Internal Server Error", "could not persist the release key");
		return;
	}

	jw_init(&w);
	write_release_key_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_release_key_delete(int fd)
{
	struct json_writer w;

	if (releasekey_clear() != RELEASEKEY_OK) {
		respond_error(fd, 500, "Internal Server Error", "could not remove the release key");
		return;
	}
	jw_init(&w);
	write_release_key_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}
