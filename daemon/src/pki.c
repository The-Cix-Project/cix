#include "pki.h"
#include "containerpath.h"
#include "registry.h"
#include "dns.h"
#include "persist.h"
#include "logstore.h"
#include "pkicrypto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct pki_cert_record {
	char name[DNS_NAME_MAX];
	char serial[PKI_SERIAL_MAX];
	char not_after[PKI_DATE_MAX];
	char sans[PKI_MAX_SANS][DNS_NAME_MAX];
	int san_count;
	/* Empty: created directly via POST /v1/pki/certs, not tied to any
	 * container's lifecycle. Non-empty: this container's name -- the
	 * cert is auto-deleted when it's deleted (pki_cert_forget_owner()). */
	char owner_container[DNS_NAME_MAX];
};

static char g_pki_dir[PATH_MAX];
static char g_ca_key_path[PATH_MAX];
static char g_ca_cert_path[PATH_MAX];
static char g_intermediate_key_path[PATH_MAX];
static char g_intermediate_cert_path[PATH_MAX];
static char g_certs_dir[PATH_MAX];
static char g_certs_state_path[PATH_MAX];
static struct pki_cert_record g_certs[PKI_MAX_CERTS];

/*
 * A CA's common_name is a free-form display label ("Cix Root CA"),
 * not a hostname -- dns_name_is_valid() doesn't apply. It used to be
 * embedded verbatim into an openssl `-subj "/CN=<common_name>"`
 * argument, where an unvalidated '/' would have injected further DN
 * fields ("Foo/O=EvilOrg" became CN=Foo, O=EvilOrg). Since #351 it is
 * one CN value set through libcrypto and cannot, but the rule stays:
 * relaxing it would change which names the API accepts, and a CA name
 * with a '/' or a control character has no legitimate use. Reject both;
 * otherwise unrestricted.
 */
static int common_name_is_valid(const char *cn)
{
	size_t i;

	if (cn == NULL || cn[0] == '\0' || strlen(cn) >= PKI_SUBJECT_MAX)
		return 0;
	for (i = 0; cn[i] != '\0'; i++) {
		unsigned char c = (unsigned char)cn[i];

		if (c < 0x20 || c == 0x7f || c == '/')
			return 0;
	}
	return 1;
}

int pki_ca_bootstrapped(void)
{
	struct stat st;

	return stat(g_ca_cert_path, &st) == 0 && stat(g_ca_key_path, &st) == 0;
}

static struct pki_cert_record *cert_find(const char *name)
{
	int i;

	for (i = 0; i < PKI_MAX_CERTS; i++) {
		if (g_certs[i].name[0] != '\0' && strcmp(g_certs[i].name, name) == 0)
			return &g_certs[i];
	}
	return NULL;
}

static int save_state(void)
{
	struct json_writer w;
	int rc;
	int i, j;

	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < PKI_MAX_CERTS; i++) {
		struct pki_cert_record *rec = &g_certs[i];

		if (rec->name[0] == '\0')
			continue;
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, rec->name);
		jw_key(&w, "serial");
		jw_str(&w, rec->serial);
		jw_key(&w, "not_after");
		jw_str(&w, rec->not_after);
		jw_key(&w, "sans");
		jw_arr_open(&w);
		for (j = 0; j < rec->san_count; j++)
			jw_str(&w, rec->sans[j]);
		jw_arr_close(&w);
		jw_key(&w, "owner");
		jw_str(&w, rec->owner_container);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	rc = persist_atomic_write(g_certs_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int parse_persisted_entry(const struct json_value *item, struct pki_cert_record *slot)
{
	const char *name = json_as_string(json_object_get(item, "name"));
	const char *serial = json_as_string(json_object_get(item, "serial"));
	const char *not_after = json_as_string(json_object_get(item, "not_after"));
	const struct json_value *jsans = json_object_get(item, "sans");
	/* "owner" is optional for backward compatibility with state
	 * persisted by part 1, before this field existed. */
	const char *owner = json_as_string(json_object_get(item, "owner"));
	size_t i;

	if (!dns_name_is_valid(name) || serial == NULL || not_after == NULL || jsans == NULL ||
	    jsans->type != JSON_ARRAY || jsans->u.array.count == 0 ||
	    jsans->u.array.count > PKI_MAX_SANS)
		return -1;

	memset(slot, 0, sizeof(*slot));
	strncpy(slot->name, name, sizeof(slot->name) - 1);
	strncpy(slot->serial, serial, sizeof(slot->serial) - 1);
	strncpy(slot->not_after, not_after, sizeof(slot->not_after) - 1);
	slot->san_count = (int)jsans->u.array.count;
	for (i = 0; i < jsans->u.array.count; i++) {
		const char *s = json_as_string(jsans->u.array.items[i]);

		if (!dns_name_is_valid(s))
			return -1;
		strncpy(slot->sans[i], s, sizeof(slot->sans[i]) - 1);
	}
	if (owner != NULL)
		strncpy(slot->owner_container, owner, sizeof(slot->owner_container) - 1);
	return 0;
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int rc = 0;
	int count = 0;

	if (persist_read_file(g_certs_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no persisted state yet -- first-ever startup */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted PKI cert state\n", g_certs_state_path);
		return -1;
	}
	if (root->u.array.count > PKI_MAX_CERTS) {
		json_free(root);
		fprintf(stderr, "%s: more certs persisted than PKI_MAX_CERTS\n", g_certs_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count; i++) {
		int j, dup = 0;

		if (parse_persisted_entry(root->u.array.items[i], &g_certs[count]) != 0) {
			fprintf(stderr, "%s: invalid entry at index %zu\n", g_certs_state_path, i);
			rc = -1;
			break;
		}
		for (j = 0; j < count; j++) {
			if (strcmp(g_certs[j].name, g_certs[count].name) == 0) {
				dup = 1;
				break;
			}
		}
		if (dup) {
			fprintf(stderr, "%s: duplicate name at index %zu\n", g_certs_state_path, i);
			rc = -1;
			break;
		}
		count++;
	}
	json_free(root);
	return rc;
}

static int set_paths(const char *pki_dir, const char *certs_state_path)
{
	if (snprintf(g_pki_dir, sizeof(g_pki_dir), "%s", pki_dir) >= (int)sizeof(g_pki_dir))
		return -1;
	if (snprintf(g_ca_key_path, sizeof(g_ca_key_path), "%s/ca.key", pki_dir) >=
	    (int)sizeof(g_ca_key_path))
		return -1;
	if (snprintf(g_ca_cert_path, sizeof(g_ca_cert_path), "%s/ca.crt", pki_dir) >=
	    (int)sizeof(g_ca_cert_path))
		return -1;
	if (snprintf(g_intermediate_key_path, sizeof(g_intermediate_key_path), "%s/intermediate.key",
	             pki_dir) >= (int)sizeof(g_intermediate_key_path))
		return -1;
	if (snprintf(g_intermediate_cert_path, sizeof(g_intermediate_cert_path), "%s/intermediate.crt",
	             pki_dir) >= (int)sizeof(g_intermediate_cert_path))
		return -1;
	if (snprintf(g_certs_dir, sizeof(g_certs_dir), "%s/certs", pki_dir) >=
	    (int)sizeof(g_certs_dir))
		return -1;
	if (snprintf(g_certs_state_path, sizeof(g_certs_state_path), "%s", certs_state_path) >=
	    (int)sizeof(g_certs_state_path))
		return -1;
	return 0;
}

/* ADR-0141 Phase 2: repoints every one of this module's own derived
 * paths without reloading g_certs[] -- see network_repoint()'s own doc
 * comment for the shared reasoning. Returns -1 on a path-too-long
 * error (same bound set_paths() itself already enforces), 0 otherwise. */
int pki_repoint(const char *new_pki_dir, const char *new_certs_state_path)
{
	return set_paths(new_pki_dir, new_certs_state_path);
}

int pki_init(const char *pki_dir, const char *certs_state_path)
{
	if (set_paths(pki_dir, certs_state_path) != 0)
		return -1;

	memset(g_certs, 0, sizeof(g_certs));
	return load_state();
}

enum pki_error pki_ca_create(const char *common_name, int days)
{
	char errbuf[512];

	if (!common_name_is_valid(common_name))
		return PKI_ERR_INVALID_NAME;
	if (pki_ca_bootstrapped())
		return PKI_ERR_ALREADY_BOOTSTRAPPED;

	if (persist_mkdir_p(g_pki_dir) != 0 || persist_mkdir_p(g_certs_dir) != 0)
		return PKI_ERR_PERSIST_FAILED;

	/* #351: in-process; it was `openssl genpkey` then `req -x509 -new`. */
	if (pkicrypto_rsa_key_create(g_ca_key_path, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "pki: CA key generation failed: %s\n", errbuf);
		unlink(g_ca_key_path);
		return PKI_ERR_OPENSSL_FAILED;
	}
	if (pkicrypto_ca_create(g_ca_key_path, common_name, days, g_ca_cert_path, errbuf,
	                        sizeof(errbuf)) != 0) {
		fprintf(stderr, "pki: CA certificate failed: %s\n", errbuf);
		unlink(g_ca_key_path);
		unlink(g_ca_cert_path);
		return PKI_ERR_OPENSSL_FAILED;
	}
	return PKI_OK;
}

/* The fields GET /pki/ca and /pki/intermediate report, then the PEM. */
static enum pki_error write_ca_json(struct json_writer *w, const char *cert_path)
{
	struct pkicrypto_cert_fields fields;
	char *cert_pem;
	size_t cert_pem_len;

	if (pkicrypto_cert_fields(cert_path, &fields, NULL, 0) != 0)
		return PKI_ERR_OPENSSL_FAILED;
	if (persist_read_file(cert_path, &cert_pem, &cert_pem_len) != 0 || cert_pem == NULL)
		return PKI_ERR_PERSIST_FAILED;

	jw_obj_open(w);
	jw_key(w, "subject");
	jw_str(w, fields.subject);
	jw_key(w, "serial");
	jw_str(w, fields.serial);
	jw_key(w, "not_before");
	jw_str(w, fields.not_before);
	jw_key(w, "not_after");
	jw_str(w, fields.not_after);
	jw_key(w, "cert_pem");
	jw_str(w, cert_pem);
	jw_obj_close(w);
	free(cert_pem);
	return PKI_OK;
}

enum pki_error pki_ca_get(struct json_writer *w)
{
	if (!pki_ca_bootstrapped())
		return PKI_ERR_NOT_BOOTSTRAPPED;
	return write_ca_json(w, g_ca_cert_path);
}

int pki_intermediate_bootstrapped(void)
{
	struct stat st;

	return stat(g_intermediate_cert_path, &st) == 0 && stat(g_intermediate_key_path, &st) == 0;
}

int pki_intermediate_cert_pem(char **out_pem, size_t *out_len)
{
	if (!pki_intermediate_bootstrapped())
		return 0;
	if (persist_read_file(g_intermediate_cert_path, out_pem, out_len) != 0 || *out_pem == NULL)
		return -1;
	return 1;
}

enum pki_error pki_intermediate_create(const char *common_name, int days)
{
	/* CA:TRUE with pathlen 0, and keyCertSign/cRLSign -- what makes the
	 * result a real intermediate CA rather than another leaf. These are
	 * the -addext strings the forked `openssl req` carried before #351,
	 * now parsed by the same libcrypto routine in-process. */
	static const char *const ext[] = {
		"basicConstraints=critical,CA:TRUE,pathlen:0",
		"keyUsage=critical,keyCertSign,cRLSign",
	};
	char errbuf[512];

	if (!common_name_is_valid(common_name))
		return PKI_ERR_INVALID_NAME;
	if (!pki_ca_bootstrapped())
		return PKI_ERR_NOT_BOOTSTRAPPED;
	if (pki_intermediate_bootstrapped())
		return PKI_ERR_ALREADY_BOOTSTRAPPED;

	if (pkicrypto_rsa_key_create(g_intermediate_key_path, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "pki: intermediate key generation failed: %s\n", errbuf);
		unlink(g_intermediate_key_path);
		return PKI_ERR_OPENSSL_FAILED;
	}
	/* Signed by the ROOT, not self-signed: the intermediate's trust
	 * derives from the root, which is the whole point. */
	if (pkicrypto_cert_issue(g_ca_cert_path, g_ca_key_path, g_intermediate_key_path,
	                         common_name, ext, (int)(sizeof(ext) / sizeof(ext[0])), days,
	                         g_intermediate_cert_path, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "pki: intermediate certificate failed: %s\n", errbuf);
		unlink(g_intermediate_key_path);
		unlink(g_intermediate_cert_path);
		return PKI_ERR_OPENSSL_FAILED;
	}
	return PKI_OK;
}

enum pki_error pki_intermediate_get(struct json_writer *w)
{
	if (!pki_intermediate_bootstrapped())
		return PKI_ERR_NOT_BOOTSTRAPPED;
	return write_ca_json(w, g_intermediate_cert_path);
}

enum pki_error pki_cert_create(const char *name, const char *const *sans, int san_count, int days,
                                const char *owner_container, struct json_writer *w)
{
	char key_path[PATH_MAX], crt_path[PATH_MAX];
	char sanbuf[PKI_MAX_SANS * (DNS_NAME_MAX + 8) + 32];
	char errbuf[512];
	char serial[PKI_SERIAL_MAX];
	char not_after[PKI_DATE_MAX];
	char *key_pem = NULL, *cert_pem = NULL;
	size_t key_pem_len, cert_pem_len;
	size_t off;
	int i, slot = -1;
	struct pkicrypto_cert_fields fields;
	struct pki_cert_record *rec;

	if (!dns_name_is_valid(name) || san_count < 1 || san_count > PKI_MAX_SANS)
		return PKI_ERR_INVALID_NAME;
	for (i = 0; i < san_count; i++) {
		if (!dns_name_is_valid(sans[i]))
			return PKI_ERR_INVALID_NAME;
	}

	if (!pki_ca_bootstrapped())
		return PKI_ERR_NOT_BOOTSTRAPPED;
	if (cert_find(name) != NULL)
		return PKI_ERR_DUPLICATE;

	for (i = 0; i < PKI_MAX_CERTS; i++) {
		if (g_certs[i].name[0] == '\0') {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return PKI_ERR_FULL;

	if (snprintf(key_path, sizeof(key_path), "%s/%s.key", g_certs_dir, name) >=
	        (int)sizeof(key_path) ||
	    snprintf(crt_path, sizeof(crt_path), "%s/%s.crt", g_certs_dir, name) >=
	        (int)sizeof(crt_path))
		return PKI_ERR_INVALID_NAME;

	/*
	 * An IPv4 literal becomes an IP: SAN, everything else a DNS: SAN
	 * (#414). Not cosmetic: a TLS client checks the name it DIALLED
	 * against the matching SAN type, and an address dialled as an
	 * address is never matched against a DNS SAN no matter what the
	 * string says. This whole function emitted DNS: unconditionally,
	 * which was invisible while every cert was verified by hostname --
	 * and is exactly what stops LDAPS working here, because
	 * ldap_effective_client_uri() hands clients the registered
	 * servers' live IPs rather than names.
	 *
	 * inet_pton() is the test rather than a character scan: it accepts
	 * precisely what an IP SAN may contain and rejects "10.0.0.1.local",
	 * which a "digits and dots" heuristic would wrongly promote.
	 */
	/*
	 * off is initialised HERE, and that is the whole point of the line.
	 * #414 folded what had been an assignment before the loop plus an
	 * append inside it into one loop whose guard reads `off` -- so the
	 * first read happened before any write. When the garbage on the
	 * stack was >= sizeof(sanbuf) the loop body never ran at all and
	 * an UNINITIALISED sanbuf went to openssl as -addext, which then
	 * parsed whatever was there as an extension name.
	 *
	 * Measured in a build container on 192.168.15.95, 2026-09-12
	 * (probe-test-pki/1, against v2.57.116): four consecutive
	 * issuances failed with "Duplicate extension: p.internal",
	 * "Duplicate extension: ernal" and "Duplicate extension: " --
	 * three different tails of an unrelated domain string left on the
	 * stack by an earlier call. Cert issuance therefore worked or
	 * failed depending on stack contents, and it happens to work on
	 * the live host, which is why this shipped and stood.
	 *
	 * One snprintf rather than two branches: the separator is the only
	 * thing that differs per iteration, and a single write is one less
	 * place for the offset to be got wrong.
	 */
	off = 0;
	for (i = 0; i < san_count && off < sizeof(sanbuf); i++) {
		struct in_addr probe;
		const char *kind = inet_pton(AF_INET, sans[i], &probe) == 1 ? "IP" : "DNS";
		int n = snprintf(sanbuf + off, sizeof(sanbuf) - off, "%s%s:%s",
		                  i == 0 ? "subjectAltName=" : ",", kind, sans[i]);

		/* snprintf returns what it WOULD have written, so a truncated
		 * SAN list must stop here rather than carry an offset past the
		 * end into the next iteration. sanbuf is sized for
		 * PKI_MAX_SANS full-length names, so this is a backstop. */
		if (n < 0 || (size_t)n >= sizeof(sanbuf) - off) {
			fprintf(stderr, "pki: SAN list for leaf %s does not fit\n", name);
			return PKI_ERR_INVALID_NAME;
		}
		off += (size_t)n;
	}

	/* 1. leaf keypair */
	if (pkicrypto_rsa_key_create(key_path, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "pki: key generation (leaf %s) failed: %s\n", name, errbuf);
		unlink(key_path);
		return PKI_ERR_OPENSSL_FAILED;
	}

	/* 2. sign with the CA -- the intermediate if one has been
	 * bootstrapped (pki_intermediate_create()), the root directly
	 * otherwise -- carrying the SAN extension. In-process since #351;
	 * the forked route was a CSR with -addext, then `x509 -req
	 * -copy_extensions copy`, which produced the same certificate. */
	{
		const char *ext[1];

		ext[0] = sanbuf;
		if (pkicrypto_cert_issue(
		        pki_intermediate_bootstrapped() ? g_intermediate_cert_path : g_ca_cert_path,
		        pki_intermediate_bootstrapped() ? g_intermediate_key_path : g_ca_key_path,
		        key_path, name, ext, 1, days, crt_path, errbuf, sizeof(errbuf)) != 0) {
			fprintf(stderr, "pki: signing (leaf %s) failed: %s\n", name, errbuf);
			unlink(key_path);
			unlink(crt_path);
			return PKI_ERR_OPENSSL_FAILED;
		}
	}

	/* 3. read back serial + expiry for the metadata index */
	if (pkicrypto_cert_fields(crt_path, &fields, errbuf, sizeof(errbuf)) != 0 ||
	    strlen(fields.serial) >= sizeof(serial) || strlen(fields.not_after) >= sizeof(not_after)) {
		fprintf(stderr, "pki: could not read back metadata for leaf %s\n", name);
		unlink(key_path);
		unlink(crt_path);
		return PKI_ERR_OPENSSL_FAILED;
	}
	snprintf(serial, sizeof(serial), "%s", fields.serial);
	snprintf(not_after, sizeof(not_after), "%s", fields.not_after);

	/* 4. persist the metadata index entry */
	rec = &g_certs[slot];
	memset(rec, 0, sizeof(*rec));
	strncpy(rec->name, name, sizeof(rec->name) - 1);
	strncpy(rec->serial, serial, sizeof(rec->serial) - 1);
	strncpy(rec->not_after, not_after, sizeof(rec->not_after) - 1);
	rec->san_count = san_count;
	for (i = 0; i < san_count; i++)
		strncpy(rec->sans[i], sans[i], sizeof(rec->sans[i]) - 1);
	if (owner_container != NULL)
		strncpy(rec->owner_container, owner_container, sizeof(rec->owner_container) - 1);

	if (save_state() != 0) {
		memset(rec, 0, sizeof(*rec));
		unlink(key_path);
		unlink(crt_path);
		return PKI_ERR_PERSIST_FAILED;
	}

	/* 5. the one-time response: cert + key PEM */
	if (persist_read_file(key_path, &key_pem, &key_pem_len) != 0 || key_pem == NULL ||
	    persist_read_file(crt_path, &cert_pem, &cert_pem_len) != 0 || cert_pem == NULL) {
		free(key_pem);
		free(cert_pem);
		return PKI_ERR_PERSIST_FAILED;
	}

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, rec->name);
	jw_key(w, "serial");
	jw_str(w, rec->serial);
	jw_key(w, "not_after");
	jw_str(w, rec->not_after);
	jw_key(w, "sans");
	jw_arr_open(w);
	for (i = 0; i < rec->san_count; i++)
		jw_str(w, rec->sans[i]);
	jw_arr_close(w);
	jw_key(w, "owner");
	if (rec->owner_container[0] != '\0')
		jw_str(w, rec->owner_container);
	else
		jw_null(w);
	jw_key(w, "cert_pem");
	jw_str(w, cert_pem);
	jw_key(w, "key_pem");
	jw_str(w, key_pem);
	jw_obj_close(w);

	free(key_pem);
	free(cert_pem);
	return PKI_OK;
}

enum pki_error pki_cert_read_pem(const char *name, char **out_key_pem, char **out_chain_pem)
{
	char src_key[PATH_MAX], src_crt[PATH_MAX];
	char *key_pem = NULL, *cert_pem = NULL, *intermediate_pem = NULL, *chain_pem = NULL;
	size_t key_pem_len, cert_pem_len, intermediate_pem_len = 0, chain_len;

	*out_key_pem = NULL;
	*out_chain_pem = NULL;
	if (cert_find(name) == NULL)
		return PKI_ERR_NOT_FOUND;

	snprintf(src_key, sizeof(src_key), "%s/%s.key", g_certs_dir, name);
	snprintf(src_crt, sizeof(src_crt), "%s/%s.crt", g_certs_dir, name);
	if (persist_read_file(src_key, &key_pem, &key_pem_len) != 0 || key_pem == NULL ||
	    persist_read_file(src_crt, &cert_pem, &cert_pem_len) != 0 || cert_pem == NULL) {
		free(key_pem);
		free(cert_pem);
		return PKI_ERR_PERSIST_FAILED;
	}

	/* tls.crt is the real, complete chain a TLS server needs (leaf +
	 * intermediate, standard fullchain.pem order) when an intermediate
	 * has been bootstrapped -- the leaf alone otherwise. */
	if (pki_intermediate_bootstrapped() &&
	    (persist_read_file(g_intermediate_cert_path, &intermediate_pem, &intermediate_pem_len) !=
	         0 ||
	     intermediate_pem == NULL)) {
		free(key_pem);
		free(cert_pem);
		free(intermediate_pem);
		return PKI_ERR_PERSIST_FAILED;
	}
	chain_len = cert_pem_len + intermediate_pem_len;
	chain_pem = malloc(chain_len + 1);
	if (chain_pem == NULL) {
		free(key_pem);
		free(cert_pem);
		free(intermediate_pem);
		return PKI_ERR_PERSIST_FAILED;
	}
	memcpy(chain_pem, cert_pem, cert_pem_len);
	if (intermediate_pem != NULL)
		memcpy(chain_pem + cert_pem_len, intermediate_pem, intermediate_pem_len);
	chain_pem[chain_len] = '\0';

	free(cert_pem);
	free(intermediate_pem);
	*out_key_pem = key_pem;
	*out_chain_pem = chain_pem;
	return PKI_OK;
}

enum pki_error pki_cert_deliver(const char *name, const char *container_name,
                                 const char *dest_dir)
{
	char dst_key[PATH_MAX], dst_crt[PATH_MAX];
	char parent[PATH_MAX + 32];
	char *key_pem = NULL, *chain_pem = NULL;
	enum pki_error result;

	result = pki_cert_read_pem(name, &key_pem, &chain_pem);
	if (result != PKI_OK)
		return result;

	/*
	 * The container's tree on the HOST side (#269). Delivering through
	 * the container's own /proc/<pid>/root view fails on a btrfs-backed
	 * userns container, whose rootfs is an id-mapped mount the daemon
	 * has no mapped identity in -- so an issued certificate would simply
	 * never arrive. The entry is consulted for its disk, which is what
	 * decides where the tree is.
	 *
	 * container_name, NOT name (#397): `name` identifies the
	 * CERTIFICATE, the tree belongs to the CONTAINER. They were equal
	 * for every caller until ADR-0280 let a container be handed a cert
	 * named something else, and delivering "jump-ssh" into "jump" then
	 * computed the host path of a container that does not exist.
	 *
	 * This writes into a LIVE container, which is now the narrow case:
	 * container creation stages the cert before clone3() instead
	 * (#414), so the only caller left is the post-CA-reset redelivery,
	 * where the container is by definition already running and there is
	 * no staging directory to write into.
	 */
	{
		const struct registry_entry *re = registry_find(container_name);

		container_file_host_path(container_name, re != NULL ? re->disk_name : "", dest_dir,
		                          parent, sizeof(parent));
	}
	if (parent[0] == '\0' ||
	    snprintf(dst_crt, sizeof(dst_crt), "%s/tls.crt", parent) >= (int)sizeof(dst_crt) ||
	    snprintf(dst_key, sizeof(dst_key), "%s/tls.key", parent) >= (int)sizeof(dst_key)) {
		free(key_pem);
		free(chain_pem);
		return PKI_ERR_PERSIST_FAILED;
	}

	/*
	 * IN PLACE, not a rename (#276, #417). This is the one call site
	 * that writes into a container whose overlay is already mounted,
	 * and persist_atomic_write() renames a NEW inode over the path --
	 * which lands in the upper layer while the merged mount the
	 * container reads through keeps resolving the inode it already
	 * holds. The write succeeds, the API reports success, and the
	 * container goes on serving the certificate signed by the CA that
	 * was just destroyed. persist_write_file_inplace()'s own comment
	 * carries the measurement of that mechanism: the first write to a
	 * path is visible and every later one is not.
	 *
	 * That is exactly the shape here -- creation stages tls.crt before
	 * clone3() (#414), so by the time this runs the path has already
	 * been read at least once. test_pki's resetlive check compares the
	 * delivered bytes through /proc/<pid>/root before and after a CA
	 * reset and is what gates this; it failed on v2.57.121, whose
	 * delivery used an atomic write.
	 */
	if (persist_mkdir_p(parent) != 0 ||
	    persist_write_file_inplace(dst_crt, chain_pem, strlen(chain_pem)) != 0 ||
	    persist_write_file_inplace(dst_key, key_pem, strlen(key_pem)) != 0) {
		result = PKI_ERR_PERSIST_FAILED;
	} else {
		chmod(dst_key, 0600);
	}

	free(key_pem);
	free(chain_pem);
	return result;
}

enum pki_error pki_cert_delete(const char *name)
{
	struct pki_cert_record *rec = cert_find(name);
	char key_path[PATH_MAX], crt_path[PATH_MAX];

	if (rec == NULL)
		return PKI_ERR_NOT_FOUND;

	snprintf(key_path, sizeof(key_path), "%s/%s.key", g_certs_dir, name);
	snprintf(crt_path, sizeof(crt_path), "%s/%s.crt", g_certs_dir, name);
	unlink(key_path);
	unlink(crt_path);

	memset(rec, 0, sizeof(*rec));
	if (save_state() != 0)
		return PKI_ERR_PERSIST_FAILED;
	return PKI_OK;
}

int pki_cert_owned_by(const char *name, const char *owner)
{
	struct pki_cert_record *rec = cert_find(name);

	return rec != NULL && strcmp(rec->owner_container, owner) == 0;
}

int pki_cert_exists(const char *name)
{
	return cert_find(name) != NULL;
}

int pki_cert_count(void)
{
	int i, n = 0;

	for (i = 0; i < PKI_MAX_CERTS; i++) {
		if (g_certs[i].name[0] != '\0')
			n++;
	}
	return n;
}

void pki_cert_forget_owner(const char *container_name)
{
	struct pki_cert_record *rec = cert_find(container_name);

	if (rec == NULL || strcmp(rec->owner_container, container_name) != 0)
		return;
	pki_cert_delete(container_name);
}

enum pki_error pki_ca_reset(const char *root_common_name, const char *intermediate_common_name,
                             int root_days, int intermediate_days, int leaf_days,
                             struct json_writer *w)
{
	struct pki_cert_record *snapshot;
	int snapshot_count = 0;
	int had_intermediate;
	char path[PATH_MAX];
	enum pki_error perr;
	int i;

	if (!common_name_is_valid(root_common_name))
		return PKI_ERR_INVALID_NAME;
	if (!pki_ca_bootstrapped())
		return PKI_ERR_NOT_BOOTSTRAPPED;
	had_intermediate = pki_intermediate_bootstrapped();
	if (had_intermediate && !common_name_is_valid(intermediate_common_name))
		return PKI_ERR_INVALID_NAME;

	snapshot = malloc(sizeof(*snapshot) * PKI_MAX_CERTS);
	if (snapshot == NULL)
		return PKI_ERR_PERSIST_FAILED;
	for (i = 0; i < PKI_MAX_CERTS; i++) {
		if (g_certs[i].name[0] != '\0')
			snapshot[snapshot_count++] = g_certs[i];
	}

	/* Every leaf's signer is about to stop existing -- their on-disk
	 * key/cert are unrecoverable-by-design once this proceeds, so wipe
	 * them and persist the now-empty index before touching the CA
	 * itself. If reissue below fails partway, the index never points
	 * at files that no longer exist. */
	for (i = 0; i < snapshot_count; i++) {
		snprintf(path, sizeof(path), "%s/%s.key", g_certs_dir, snapshot[i].name);
		unlink(path);
		snprintf(path, sizeof(path), "%s/%s.crt", g_certs_dir, snapshot[i].name);
		unlink(path);
	}
	memset(g_certs, 0, sizeof(g_certs));
	if (save_state() != 0) {
		free(snapshot);
		return PKI_ERR_PERSIST_FAILED;
	}

	unlink(g_ca_key_path);
	unlink(g_ca_cert_path);
	unlink(g_intermediate_key_path);
	unlink(g_intermediate_cert_path);
	/* Serial counters the forked `x509 -CAcreateserial` kept beside
	 * each CA before #351. Nothing writes them now (every serial is
	 * random), but a host that issued certificates before still has
	 * them, and a reset clears this directory's state. */
	snprintf(path, sizeof(path), "%s/ca.srl", g_pki_dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/intermediate.srl", g_pki_dir);
	unlink(path);

	perr = pki_ca_create(root_common_name, root_days);
	if (perr != PKI_OK) {
		free(snapshot);
		return perr;
	}
	if (had_intermediate) {
		perr = pki_intermediate_create(intermediate_common_name, intermediate_days);
		if (perr != PKI_OK) {
			free(snapshot);
			return perr;
		}
	}

	jw_obj_open(w);
	jw_key(w, "root");
	pki_ca_get(w);
	jw_key(w, "intermediate");
	if (had_intermediate)
		pki_intermediate_get(w);
	else
		jw_null(w);

	jw_key(w, "reissued");
	jw_arr_open(w);
	for (i = 0; i < snapshot_count; i++) {
		const char *sans_ptrs[PKI_MAX_SANS];
		int j;
		enum pki_error rperr;

		for (j = 0; j < snapshot[i].san_count; j++)
			sans_ptrs[j] = snapshot[i].sans[j];
		rperr = pki_cert_create(snapshot[i].name, sans_ptrs, snapshot[i].san_count, leaf_days,
		                         snapshot[i].owner_container[0] != '\0' ?
		                             snapshot[i].owner_container :
		                             NULL,
		                         w);
		if (rperr != PKI_OK)
			fprintf(stderr, "pki: reset could not reissue leaf %s (err=%d)\n",
			        snapshot[i].name, (int)rperr);
	}
	jw_arr_close(w);
	jw_obj_close(w);

	free(snapshot);
	return PKI_OK;
}

/*
 * Taking the CA off the box, and putting it back (#415, ADR-0281).
 *
 * PKI_DIR is /config/state/pki, on the cix-config partition -- so the
 * CA survives a reboot, an A/B update and a rolling rebuild, and does
 * NOT survive a reinstall: cix-install.c mkfs's that partition. Before
 * this existed there was no way at all to carry a CA across one, and
 * "back it up at the host level" was not an answer on a host that is
 * shell-less by charter. A key in exactly this position was already
 * lost that way once, on 2026-09-06.
 *
 * A passphrase, not plain bytes. The bundle carries the root CA's
 * private key -- the one thing this API has never returned, on any
 * endpoint. ADR-0281 supersedes that narrowly rather than abandoning
 * it: the key leaves only through this one endpoint, only encrypted
 * under a passphrase the daemon never stores, so an exported bundle
 * sitting on an operator's laptop is not the trust root.
 *
 * The passphrase and the plaintext never reach a file or an argv:
 * since #351 the bundle is encrypted and decrypted in-process
 * (pkicrypto_export_encrypt/decrypt). Before that both went through
 * 0600 temp files in PKI_DIR to a forked `openssl enc`.
 *
 * -aes-256-cbc with pbkdf2 at 600000 iterations, sha256, salted: the
 * format `openssl enc` wrote, kept byte for byte so every bundle
 * exported by an earlier release still imports, and fixed in code
 * rather than left to any tool's defaults -- a bundle that cannot be
 * decrypted by the next release is not a backup. CBC rather than an
 * AEAD mode because that is the format `openssl enc` produced (it
 * refuses AEAD outright) -- so the bundle has no integrity tag, and
 * import compensates by validating what it decodes (see pki_import())
 * rather than trusting that it decrypted.
 */

#define PKI_EXPORT_CIPHER "-aes-256-cbc"
#define PKI_EXPORT_ITER "600000"

/* Reads a PEM file straight into a json_writer string value, or writes
 * JSON null when it is absent. Absent is legitimate: an install may
 * never have bootstrapped an intermediate. */
static int pki_export_file_field(struct json_writer *w, const char *key, const char *path)
{
	char *buf = NULL;
	size_t len;

	jw_key(w, key);
	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL) {
		jw_null(w);
		free(buf);
		return 0;
	}
	jw_str(w, buf);
	free(buf);
	return 0;
}

enum pki_error pki_export(const char *passphrase, struct json_writer *w)
{
	char errbuf[512];
	char *cipher = NULL;
	struct json_writer plain;
	int i, j;

	if (passphrase == NULL || passphrase[0] == '\0')
		return PKI_ERR_INVALID_NAME;
	if (!pki_ca_bootstrapped())
		return PKI_ERR_NOT_BOOTSTRAPPED;

	/* The whole store, not just the CA. A leaf is re-derivable from the
	 * CA only if something reissues it -- and the certs ADR-0280 exists
	 * for are precisely the ones nothing reissues: an unowned cert is a
	 * durable identity (jump's SSH host key), and restoring the CA
	 * without it would rotate exactly the key #397 was filed to stop
	 * rotating. Owned leaves are carried too, and that is deliberate:
	 * when autostart recreates ldap-1, its pki_issue hits DUPLICATE and
	 * keeps the restored cert rather than issuing a new one. */
	jw_init(&plain);
	jw_obj_open(&plain);
	jw_key(&plain, "version");
	jw_int(&plain, 1);
	pki_export_file_field(&plain, "ca_key", g_ca_key_path);
	pki_export_file_field(&plain, "ca_cert", g_ca_cert_path);
	pki_export_file_field(&plain, "intermediate_key", g_intermediate_key_path);
	pki_export_file_field(&plain, "intermediate_cert", g_intermediate_cert_path);
	jw_key(&plain, "certs");
	jw_arr_open(&plain);
	for (i = 0; i < PKI_MAX_CERTS; i++) {
		struct pki_cert_record *rec = &g_certs[i];
		char key_path[PATH_MAX], crt_path[PATH_MAX];

		if (rec->name[0] == '\0')
			continue;
		snprintf(key_path, sizeof(key_path), "%s/%s.key", g_certs_dir, rec->name);
		snprintf(crt_path, sizeof(crt_path), "%s/%s.crt", g_certs_dir, rec->name);
		jw_obj_open(&plain);
		jw_key(&plain, "name");
		jw_str(&plain, rec->name);
		jw_key(&plain, "serial");
		jw_str(&plain, rec->serial);
		jw_key(&plain, "not_after");
		jw_str(&plain, rec->not_after);
		jw_key(&plain, "sans");
		jw_arr_open(&plain);
		for (j = 0; j < rec->san_count; j++)
			jw_str(&plain, rec->sans[j]);
		jw_arr_close(&plain);
		jw_key(&plain, "owner");
		jw_str(&plain, rec->owner_container);
		pki_export_file_field(&plain, "key_pem", key_path);
		pki_export_file_field(&plain, "cert_pem", crt_path);
		jw_obj_close(&plain);
	}
	jw_arr_close(&plain);
	jw_obj_close(&plain);

	/*
	 * #351: encrypted in-process, byte-compatible with the
	 * `openssl enc -aes-256-cbc -pbkdf2 -iter 600000 -md sha256 -salt
	 * -a -A` it replaced, so every bundle exported before still imports.
	 * The plaintext -- every private key -- no longer passes through a
	 * file on disk, and is zeroed before its buffer is freed.
	 */
	if (pkicrypto_export_encrypt(passphrase, (const unsigned char *)plain.buf, plain.len,
	                             &cipher, errbuf, sizeof(errbuf)) != 0) {
		explicit_bzero(plain.buf, plain.len);
		jw_free(&plain);
		logstore_write("pki", "error", "export encryption failed: %s", errbuf);
		return PKI_ERR_OPENSSL_FAILED;
	}
	explicit_bzero(plain.buf, plain.len);
	jw_free(&plain);

	jw_obj_open(w);
	jw_key(w, "bundle");
	jw_str(w, cipher);
	jw_key(w, "cipher");
	jw_str(w, PKI_EXPORT_CIPHER " pbkdf2 iter=" PKI_EXPORT_ITER " md=sha256");
	jw_obj_close(w);
	free(cipher);
	return PKI_OK;
}

/* Writes one PEM field out of the decoded bundle. A null/absent field
 * is skipped, not an error -- an install with no intermediate exports
 * two JSON nulls and must import cleanly. */
static int pki_import_file_field(const struct json_value *obj, const char *key, const char *path,
                                  mode_t mode, int required)
{
	const char *pem = json_as_string(json_object_get(obj, key));

	if (pem == NULL)
		return required ? -1 : 0;
	if (persist_atomic_write(path, pem, strlen(pem)) != 0)
		return -1;
	chmod(path, mode);
	return 0;
}

/*
 * CBC gives no integrity tag, so a wrong passphrase does not fail --
 * it produces garbage. openssl's own padding check rejects most of it,
 * but "decrypted to something" is not "decrypted to our bundle", so
 * the decoded blob is validated before a single byte reaches the
 * store: it must parse as JSON, carry the version this code knows, and
 * its ca_key and ca_cert must be a matching pair. The pair check is
 * the one that actually pins it -- the key must be the one the
 * certificate certifies (compared in-process since #351), which no
 * corrupted input produces by accident.
 */
static int pki_import_pair_matches(const char *key_path, const char *cert_path)
{
	return pkicrypto_key_matches_cert(key_path, cert_path, NULL, 0) == 1;
}

enum pki_error pki_import(const char *passphrase, const char *bundle)
{
	char errbuf[512];
	unsigned char *plain = NULL;
	size_t plain_len;
	struct json_value *root = NULL;
	const struct json_value *jcerts;
	enum pki_error result = PKI_OK;
	size_t ci;

	if (passphrase == NULL || passphrase[0] == '\0' || bundle == NULL || bundle[0] == '\0')
		return PKI_ERR_INVALID_NAME;
	/*
	 * Refused outright when a CA already exists, matching
	 * pki_ca_create()'s own one-shot posture: silently replacing a live
	 * trust root would invalidate every certificate this install has
	 * issued, and nothing about "import a backup" says an operator
	 * meant that. The intended case is a freshly installed box, which
	 * has no CA at all -- pkg.c's own seeding already treats
	 * NOT_BOOTSTRAPPED as "the common state on a fresh install".
	 */
	if (pki_ca_bootstrapped())
		return PKI_ERR_ALREADY_BOOTSTRAPPED;

	/*
	 * #351: decrypted in-process. The cipher is the one
	 * `openssl enc -d` undid before, so a bundle exported by any
	 * earlier release imports unchanged; the plaintext never touches
	 * the disk, and is zeroed once parsed.
	 */
	if (pkicrypto_export_decrypt(passphrase, bundle, strlen(bundle), &plain, &plain_len, errbuf,
	                             sizeof(errbuf)) != 0) {
		logstore_write("pki", "error", "import: %s", errbuf);
		return PKI_ERR_OPENSSL_FAILED;
	}
	root = json_parse((const char *)plain, plain_len);
	explicit_bzero(plain, plain_len);
	free(plain);
	if (root == NULL || root->type != JSON_OBJECT ||
	    (int)json_as_number(json_object_get(root, "version")) != 1 ||
	    json_as_string(json_object_get(root, "ca_key")) == NULL ||
	    json_as_string(json_object_get(root, "ca_cert")) == NULL) {
		json_free(root);
		return PKI_ERR_OPENSSL_FAILED;
	}
	jcerts = json_object_get(root, "certs");
	if (jcerts == NULL || jcerts->type != JSON_ARRAY ||
	    jcerts->u.array.count > PKI_MAX_CERTS) {
		json_free(root);
		return PKI_ERR_OPENSSL_FAILED;
	}

	/*
	 * ORDER IS THE SAFETY PROPERTY HERE, and it is why this needs no
	 * staging directory. pki_ca_bootstrapped() is true only once BOTH
	 * ca.crt and ca.key exist, so everything else is written first and
	 * the CA key last. A failure at any point therefore leaves an
	 * install that still reads as not-bootstrapped -- so the guard
	 * above still lets a corrected retry through, rather than locking
	 * the operator out of their own import with a 409 over a
	 * half-written store.
	 */
	if (persist_mkdir_p(g_certs_dir) != 0) {
		json_free(root);
		return PKI_ERR_PERSIST_FAILED;
	}
	memset(g_certs, 0, sizeof(g_certs));
	for (ci = 0; ci < jcerts->u.array.count && result == PKI_OK; ci++) {
		const struct json_value *item = jcerts->u.array.items[ci];
		const char *cname = json_as_string(json_object_get(item, "name"));
		char key_path[PATH_MAX], crt_path[PATH_MAX];

		if (cname == NULL || cname[0] == '\0' || strchr(cname, '/') != NULL ||
		    strstr(cname, "..") != NULL) {
			result = PKI_ERR_INVALID_NAME;
			break;
		}
		if (snprintf(key_path, sizeof(key_path), "%s/%s.key", g_certs_dir, cname) >=
		        (int)sizeof(key_path) ||
		    snprintf(crt_path, sizeof(crt_path), "%s/%s.crt", g_certs_dir, cname) >=
		        (int)sizeof(crt_path)) {
			result = PKI_ERR_INVALID_NAME;
			break;
		}
		if (pki_import_file_field(item, "key_pem", key_path, 0600, 1) != 0 ||
		    pki_import_file_field(item, "cert_pem", crt_path, 0644, 1) != 0) {
			result = PKI_ERR_PERSIST_FAILED;
			break;
		}
		if (parse_persisted_entry(item, &g_certs[ci]) != 0) {
			result = PKI_ERR_OPENSSL_FAILED;
			break;
		}
	}
	if (result == PKI_OK && save_state() != 0)
		result = PKI_ERR_PERSIST_FAILED;

	if (result == PKI_OK &&
	    (pki_import_file_field(root, "intermediate_key", g_intermediate_key_path, 0600, 0) != 0 ||
	     pki_import_file_field(root, "intermediate_cert", g_intermediate_cert_path, 0644, 0) != 0))
		result = PKI_ERR_PERSIST_FAILED;

	/* ca.crt, then ca.key -- last, for the reason above. */
	if (result == PKI_OK &&
	    (pki_import_file_field(root, "ca_cert", g_ca_cert_path, 0644, 1) != 0 ||
	     pki_import_file_field(root, "ca_key", g_ca_key_path, 0600, 1) != 0))
		result = PKI_ERR_PERSIST_FAILED;
	json_free(root);

	if (result == PKI_OK && !pki_import_pair_matches(g_ca_key_path, g_ca_cert_path)) {
		/* Decrypted, parsed, and still not our bundle. Take the CA key
		 * back out so the install reads as not-bootstrapped again and
		 * a retry is possible. */
		unlink(g_ca_key_path);
		unlink(g_ca_cert_path);
		memset(g_certs, 0, sizeof(g_certs));
		save_state();
		return PKI_ERR_OPENSSL_FAILED;
	}
	if (result != PKI_OK) {
		unlink(g_ca_key_path);
		memset(g_certs, 0, sizeof(g_certs));
		save_state();
		return result;
	}

	/*
	 * g_certs[] was rebuilt in memory above rather than left for the
	 * next restart to read: pki_ca_bootstrapped() is stat-based and
	 * flips the instant the files land, so a stale in-memory index
	 * would make GET /v1/pki/certs answer "empty" on an install that
	 * had just successfully imported a dozen -- and it would look like
	 * a successful import, which is the dangerous shape. Re-read from
	 * the file that was just written, so what is served is what is
	 * persisted rather than what this function happened to build.
	 */
	memset(g_certs, 0, sizeof(g_certs));
	if (load_state() != 0)
		return PKI_ERR_PERSIST_FAILED;
	return PKI_OK;
}

enum pki_error pki_trust_bundle_pem(char **out_pem, size_t *out_len)
{
	char *root_pem = NULL, *intermediate_pem = NULL, *bundle = NULL;
	size_t root_len, intermediate_len = 0, bundle_len;

	if (out_pem == NULL)
		return PKI_ERR_INVALID_NAME;
	*out_pem = NULL;
	if (out_len != NULL)
		*out_len = 0;
	if (!pki_ca_bootstrapped())
		return PKI_ERR_NOT_BOOTSTRAPPED;

	if (persist_read_file(g_ca_cert_path, &root_pem, &root_len) != 0 || root_pem == NULL)
		return PKI_ERR_PERSIST_FAILED;

	if (pki_intermediate_bootstrapped() &&
	    (persist_read_file(g_intermediate_cert_path, &intermediate_pem, &intermediate_len) != 0 ||
	     intermediate_pem == NULL)) {
		free(root_pem);
		return PKI_ERR_PERSIST_FAILED;
	}

	bundle_len = root_len + intermediate_len;
	bundle = malloc(bundle_len + 1);
	if (bundle == NULL) {
		free(root_pem);
		free(intermediate_pem);
		return PKI_ERR_PERSIST_FAILED;
	}
	memcpy(bundle, root_pem, root_len);
	if (intermediate_pem != NULL)
		memcpy(bundle + root_len, intermediate_pem, intermediate_len);
	bundle[bundle_len] = '\0';

	free(root_pem);
	free(intermediate_pem);
	*out_pem = bundle;
	if (out_len != NULL)
		*out_len = bundle_len;
	return PKI_OK;
}

enum pki_error pki_write_trust_bundle_file(const char *dest_path)
{
	char *bundle = NULL;
	size_t bundle_len = 0;
	enum pki_error result = pki_trust_bundle_pem(&bundle, &bundle_len);

	if (result != PKI_OK)
		return result;
	if (persist_atomic_write(dest_path, bundle, bundle_len) != 0)
		result = PKI_ERR_PERSIST_FAILED;
	free(bundle);
	return result;
}

/* cert_pem may be NULL (list view: metadata only, never a key). */
static void write_cert_json(const struct pki_cert_record *rec, const char *cert_pem,
                             struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, rec->name);
	jw_key(w, "serial");
	jw_str(w, rec->serial);
	jw_key(w, "not_after");
	jw_str(w, rec->not_after);
	jw_key(w, "sans");
	jw_arr_open(w);
	for (i = 0; i < rec->san_count; i++)
		jw_str(w, rec->sans[i]);
	jw_arr_close(w);
	jw_key(w, "owner");
	if (rec->owner_container[0] != '\0')
		jw_str(w, rec->owner_container);
	else
		jw_null(w);
	if (cert_pem != NULL) {
		jw_key(w, "cert_pem");
		jw_str(w, cert_pem);
	}
	jw_obj_close(w);
}

void pki_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < PKI_MAX_CERTS; i++) {
		if (g_certs[i].name[0] != '\0')
			write_cert_json(&g_certs[i], NULL, w);
	}
	jw_arr_close(w);
}

enum pki_error pki_cert_get_one(const char *name, struct json_writer *w)
{
	struct pki_cert_record *rec = cert_find(name);
	char crt_path[PATH_MAX];
	char *cert_pem;
	size_t cert_pem_len;

	if (rec == NULL)
		return PKI_ERR_NOT_FOUND;

	snprintf(crt_path, sizeof(crt_path), "%s/%s.crt", g_certs_dir, name);
	if (persist_read_file(crt_path, &cert_pem, &cert_pem_len) != 0 || cert_pem == NULL)
		return PKI_ERR_PERSIST_FAILED;

	write_cert_json(rec, cert_pem, w);
	free(cert_pem);
	return PKI_OK;
}
