#include "signingkeys.h"

#include "logstore.h"
#include "pki.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The three real filenames POST /v1/system/iso already looks for --
 * main.c builds the identical paths from SIGNING_KEYS_DIR, and
 * image/src/mkinstalleriso.c takes them as arguments. They are spelled
 * once here and once there; changing either without the other breaks
 * the ISO build, so both sites name the same three basenames
 * deliberately rather than inventing a shared constant that would then
 * have to be reachable from the image/ tree too.
 */
#define KEY_BASENAME "cix-signing.key"
#define CRT_BASENAME "cix-signing.crt"
#define CER_BASENAME "cix-signing.cer"

#define OPENSSL_BIN "/usr/bin/openssl"

static char g_key_path[PATH_MAX];
static char g_crt_path[PATH_MAX];
static char g_cer_path[PATH_MAX];
static char g_keys_dir[PATH_MAX];

static void build_paths(const char *keys_dir)
{
	snprintf(g_keys_dir, sizeof(g_keys_dir), "%s", keys_dir);
	snprintf(g_key_path, sizeof(g_key_path), "%s/" KEY_BASENAME, keys_dir);
	snprintf(g_crt_path, sizeof(g_crt_path), "%s/" CRT_BASENAME, keys_dir);
	snprintf(g_cer_path, sizeof(g_cer_path), "%s/" CER_BASENAME, keys_dir);
}

int signingkeys_init(const char *keys_dir)
{
	build_paths(keys_dir);
	/*
	 * A fresh install has never had this directory -- nothing else
	 * creates it, because until now nothing could ever write here.
	 * Creating it up front means the first PUT fails only for reasons
	 * an operator can act on, rather than ENOENT on a parent nobody
	 * told them to make. 0700: the private key lives here.
	 */
	if (mkdir(keys_dir, 0700) != 0 && errno != EEXIST) {
		logstore_write("cixd", "error", "signing keys: cannot create %s: %s", keys_dir,
		               strerror(errno));
		return -1;
	}
	return 0;
}

void signingkeys_repoint(const char *new_keys_dir)
{
	build_paths(new_keys_dir);
}

/* Writes buf to path with mode, via "<path>.tmp" -- caller renames. */
static int write_temp(const char *path, const char *buf, size_t len, mode_t mode, char *tmp_out,
                      size_t tmp_out_size)
{
	int fd;
	ssize_t written;

	if ((size_t)snprintf(tmp_out, tmp_out_size, "%s.tmp", path) >= tmp_out_size)
		return -1;
	/*
	 * O_TRUNC, not O_EXCL: a temporary left behind by a killed daemon
	 * would otherwise block every future PUT with no way for an
	 * operator to clear it. The mode is set at creation rather than
	 * chmod'ed afterwards so the private key is never briefly readable.
	 */
	fd = open(tmp_out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0)
		return -1;
	written = write(fd, buf, len);
	if (written < 0 || (size_t)written != len) {
		close(fd);
		unlink(tmp_out);
		return -1;
	}
	if (close(fd) != 0) {
		unlink(tmp_out);
		return -1;
	}
	return 0;
}

/*
 * Both public keys, in PEM, from the two temporaries. Comparing these
 * is how a mismatched paste is caught: `openssl pkey -pubout` and
 * `openssl x509 -pubkey -noout` emit byte-identical PEM for the same
 * key, so an exact string compare is the whole test and needs no
 * parsing of either side.
 */
static int pubkey_of_privkey(const char *key_path, char *out, size_t out_size)
{
	char *argv[6];

	argv[0] = (char *)OPENSSL_BIN;
	argv[1] = (char *)"pkey";
	argv[2] = (char *)"-in";
	argv[3] = (char *)key_path;
	argv[4] = (char *)"-pubout";
	argv[5] = NULL;
	return pki_run_openssl(argv, out, out_size);
}

static int pubkey_of_cert(const char *crt_path, char *out, size_t out_size)
{
	char *argv[7];

	argv[0] = (char *)OPENSSL_BIN;
	argv[1] = (char *)"x509";
	argv[2] = (char *)"-in";
	argv[3] = (char *)crt_path;
	argv[4] = (char *)"-pubkey";
	argv[5] = (char *)"-noout";
	argv[6] = NULL;
	return pki_run_openssl(argv, out, out_size);
}

enum signingkeys_error signingkeys_set(const char *key_pem, const char *cert_pem)
{
	char key_tmp[PATH_MAX];
	char crt_tmp[PATH_MAX];
	char cer_tmp[PATH_MAX];
	char key_pub[4096];
	char crt_pub[4096];
	enum signingkeys_error rc = SIGNINGKEYS_ERR_PERSIST_FAILED;
	char *argv[9];

	if (key_pem == NULL || cert_pem == NULL)
		return SIGNINGKEYS_ERR_BAD_KEY;

	/*
	 * 0600 on the key from the instant it exists; 0644 on the cert,
	 * which is public material an operator legitimately reads back off
	 * the box to enrol as a MOK.
	 */
	if (write_temp(g_key_path, key_pem, strlen(key_pem), 0600, key_tmp, sizeof(key_tmp)) != 0)
		return SIGNINGKEYS_ERR_PERSIST_FAILED;
	if (write_temp(g_crt_path, cert_pem, strlen(cert_pem), 0644, crt_tmp, sizeof(crt_tmp)) != 0) {
		unlink(key_tmp);
		return SIGNINGKEYS_ERR_PERSIST_FAILED;
	}
	cer_tmp[0] = '\0';

	/*
	 * Validate before anything is renamed into place. Each check runs
	 * against the temporary, so the previously installed pair -- which
	 * a release host may be actively using -- is still intact if any
	 * of this fails.
	 */
	if (pubkey_of_privkey(key_tmp, key_pub, sizeof(key_pub)) != 0) {
		rc = SIGNINGKEYS_ERR_BAD_KEY;
		goto out;
	}
	if (pubkey_of_cert(crt_tmp, crt_pub, sizeof(crt_pub)) != 0) {
		rc = SIGNINGKEYS_ERR_BAD_CERT;
		goto out;
	}
	if (key_pub[0] == '\0' || strcmp(key_pub, crt_pub) != 0) {
		rc = SIGNINGKEYS_ERR_MISMATCH;
		goto out;
	}

	/*
	 * Derive the DER .cer rather than asking the operator for a third
	 * blob. It is the same certificate in a different encoding, so
	 * accepting it separately would only create a way for the two to
	 * disagree -- and mokutil, which is the only consumer, would then
	 * enrol an identity that doesn't match what actually signed the
	 * image.
	 */
	if ((size_t)snprintf(cer_tmp, sizeof(cer_tmp), "%s.tmp", g_cer_path) >= sizeof(cer_tmp)) {
		cer_tmp[0] = '\0';
		goto out;
	}
	argv[0] = (char *)OPENSSL_BIN;
	argv[1] = (char *)"x509";
	argv[2] = (char *)"-in";
	argv[3] = crt_tmp;
	argv[4] = (char *)"-outform";
	argv[5] = (char *)"DER";
	argv[6] = (char *)"-out";
	argv[7] = cer_tmp;
	argv[8] = NULL;
	if (pki_run_openssl(argv, NULL, 0) != 0) {
		rc = SIGNINGKEYS_ERR_BAD_CERT;
		goto out;
	}

	if (rename(key_tmp, g_key_path) != 0)
		goto out;
	if (rename(crt_tmp, g_crt_path) != 0)
		goto out;
	if (rename(cer_tmp, g_cer_path) != 0)
		goto out;

	logstore_write("cixd", "info",
	               "signing keys: operator installed a Secure Boot signing key pair");
	return SIGNINGKEYS_OK;

out:
	unlink(key_tmp);
	unlink(crt_tmp);
	if (cer_tmp[0] != '\0')
		unlink(cer_tmp);
	/*
	 * Deliberately no openssl output in this log line. It can quote the
	 * material it was given, and the private key is exactly what must
	 * never reach the log store.
	 */
	logstore_write("cixd", "error", "signing keys: rejected an installed pair (reason %d)",
	               (int)rc);
	return rc;
}

enum signingkeys_error signingkeys_clear(void)
{
	int failed = 0;

	if (unlink(g_key_path) != 0 && errno != ENOENT)
		failed = 1;
	if (unlink(g_crt_path) != 0 && errno != ENOENT)
		failed = 1;
	if (unlink(g_cer_path) != 0 && errno != ENOENT)
		failed = 1;
	if (failed) {
		logstore_write("cixd", "error", "signing keys: could not remove the key pair");
		return SIGNINGKEYS_ERR_PERSIST_FAILED;
	}
	logstore_write("cixd", "info", "signing keys: operator removed the Secure Boot key pair");
	return SIGNINGKEYS_OK;
}

/* "prefix=value\n" out of openssl's own -noout output. */
static int field_after(const char *output, const char *prefix, char *out, size_t out_size)
{
	const char *p = strstr(output, prefix);
	const char *end;
	size_t len;

	if (p == NULL)
		return -1;
	p += strlen(prefix);
	end = strchr(p, '\n');
	len = end != NULL ? (size_t)(end - p) : strlen(p);
	while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' '))
		len--;
	if (len == 0 || len >= out_size)
		return -1;
	memcpy(out, p, len);
	out[len] = '\0';
	return 0;
}

static int file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

void signingkeys_write_json(struct json_writer *w)
{
	int key_set = file_exists(g_key_path);
	int cert_set = file_exists(g_crt_path) && file_exists(g_cer_path);
	char out[8192];
	char subject[512];
	char not_after[128];
	char fingerprint[256];
	int have_subject = 0;
	int have_not_after = 0;
	int have_fingerprint = 0;

	if (cert_set) {
		char *argv[9];

		argv[0] = (char *)OPENSSL_BIN;
		argv[1] = (char *)"x509";
		argv[2] = (char *)"-in";
		argv[3] = g_crt_path;
		argv[4] = (char *)"-noout";
		argv[5] = (char *)"-subject";
		argv[6] = (char *)"-enddate";
		argv[7] = (char *)"-fingerprint";
		argv[8] = NULL;
		/*
		 * -sha256 is not passed: openssl 3's default fingerprint digest
		 * is already SHA-256, and older builds that default to SHA-1
		 * would report a differently-labelled value rather than a
		 * wrong one -- the label openssl prints is kept verbatim below
		 * for exactly that reason.
		 */
		if (pki_run_openssl(argv, out, sizeof(out)) == 0) {
			have_subject = field_after(out, "subject=", subject, sizeof(subject)) == 0;
			have_not_after = field_after(out, "notAfter=", not_after, sizeof(not_after)) == 0;
			have_fingerprint =
			    field_after(out, "Fingerprint=", fingerprint, sizeof(fingerprint)) == 0;
		}
	}

	jw_obj_open(w);
	jw_key(w, "key_set");
	jw_bool(w, key_set);
	jw_key(w, "cert_set");
	jw_bool(w, cert_set);
	jw_key(w, "subject");
	if (have_subject)
		jw_str(w, subject);
	else
		jw_null(w);
	jw_key(w, "not_after");
	if (have_not_after)
		jw_str(w, not_after);
	else
		jw_null(w);
	jw_key(w, "fingerprint_sha256");
	if (have_fingerprint)
		jw_str(w, fingerprint);
	else
		jw_null(w);
	jw_obj_close(w);
}
