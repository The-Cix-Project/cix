#include "pki.h"
#include "containerpath.h"
#include "registry.h"
#include "dns.h"
#include "persist.h"
#include "logstore.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define PKI_OPENSSL_BIN "/usr/bin/openssl"

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



/* Finds "<prefix><value>\n" (or end-of-string) inside output and
 * copies value into out. Used to pull e.g. "serial=..."/"notAfter=..."
 * out of `openssl x509 -noout ...`'s stdout. */
static int extract_field(const char *output, const char *prefix, char *out, size_t out_size)
{
	const char *p = strstr(output, prefix);
	const char *end;
	size_t len;

	if (p == NULL)
		return -1;
	p += strlen(prefix);
	end = strchr(p, '\n');
	len = end != NULL ? (size_t)(end - p) : strlen(p);
	if (len > 0 && p[len - 1] == '\r')
		len--;
	if (len >= out_size)
		return -1;
	memcpy(out, p, len);
	out[len] = '\0';
	return 0;
}

/*
 * A CA's common_name is a free-form display label ("Cix Root CA"),
 * not a hostname -- dns_name_is_valid() doesn't apply. It is however
 * embedded verbatim into an openssl `-subj "/CN=<common_name>"`
 * argument, so an unvalidated '/' would let a caller inject
 * additional, unintended DN fields (e.g. "Foo/O=EvilOrg" becomes
 * CN=Foo, O=EvilOrg). Reject '/' and control characters; otherwise
 * unrestricted.
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
	char subj[PKI_SUBJECT_MAX + 8];
	char days_str[16];
	char errbuf[512];
	char *argv[24];

	if (!common_name_is_valid(common_name))
		return PKI_ERR_INVALID_NAME;
	if (pki_ca_bootstrapped())
		return PKI_ERR_ALREADY_BOOTSTRAPPED;

	if (persist_mkdir_p(g_pki_dir) != 0 || persist_mkdir_p(g_certs_dir) != 0)
		return PKI_ERR_PERSIST_FAILED;

	snprintf(subj, sizeof(subj), "/CN=%s", common_name);
	snprintf(days_str, sizeof(days_str), "%d", days);

	argv[0] = (char *)PKI_OPENSSL_BIN;
	argv[1] = "genpkey";
	argv[2] = "-algorithm";
	argv[3] = "RSA";
	argv[4] = "-pkeyopt";
	argv[5] = "rsa_keygen_bits:2048";
	argv[6] = "-out";
	argv[7] = g_ca_key_path;
	argv[8] = NULL;
	if (pki_run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "pki: CA genpkey failed: %s\n", errbuf);
		unlink(g_ca_key_path);
		return PKI_ERR_OPENSSL_FAILED;
	}
	chmod(g_ca_key_path, 0600);

	argv[0] = (char *)PKI_OPENSSL_BIN;
	argv[1] = "req";
	argv[2] = "-x509";
	argv[3] = "-new";
	argv[4] = "-key";
	argv[5] = g_ca_key_path;
	argv[6] = "-days";
	argv[7] = days_str;
	argv[8] = "-subj";
	argv[9] = subj;
	argv[10] = "-out";
	argv[11] = g_ca_cert_path;
	argv[12] = NULL;
	if (pki_run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "pki: CA req -x509 failed: %s\n", errbuf);
		unlink(g_ca_key_path);
		unlink(g_ca_cert_path);
		return PKI_ERR_OPENSSL_FAILED;
	}
	chmod(g_ca_cert_path, 0644);

	return PKI_OK;
}

enum pki_error pki_ca_get(struct json_writer *w)
{
	char output[1024];
	char subject[PKI_SUBJECT_MAX + 8];
	char serial[PKI_SERIAL_MAX];
	char not_before[PKI_DATE_MAX];
	char not_after[PKI_DATE_MAX];
	char *cert_pem;
	size_t cert_pem_len;
	char *argv[24];

	if (!pki_ca_bootstrapped())
		return PKI_ERR_NOT_BOOTSTRAPPED;

	argv[0] = (char *)PKI_OPENSSL_BIN;
	argv[1] = "x509";
	argv[2] = "-in";
	argv[3] = g_ca_cert_path;
	argv[4] = "-noout";
	argv[5] = "-subject";
	argv[6] = "-serial";
	argv[7] = "-startdate";
	argv[8] = "-enddate";
	argv[9] = NULL;
	if (pki_run_openssl(argv, output, sizeof(output)) != 0)
		return PKI_ERR_OPENSSL_FAILED;

	if (extract_field(output, "subject=", subject, sizeof(subject)) != 0 ||
	    extract_field(output, "serial=", serial, sizeof(serial)) != 0 ||
	    extract_field(output, "notBefore=", not_before, sizeof(not_before)) != 0 ||
	    extract_field(output, "notAfter=", not_after, sizeof(not_after)) != 0)
		return PKI_ERR_OPENSSL_FAILED;

	if (persist_read_file(g_ca_cert_path, &cert_pem, &cert_pem_len) != 0 || cert_pem == NULL)
		return PKI_ERR_PERSIST_FAILED;

	jw_obj_open(w);
	jw_key(w, "subject");
	jw_str(w, subject);
	jw_key(w, "serial");
	jw_str(w, serial);
	jw_key(w, "not_before");
	jw_str(w, not_before);
	jw_key(w, "not_after");
	jw_str(w, not_after);
	jw_key(w, "cert_pem");
	jw_str(w, cert_pem);
	jw_obj_close(w);
	free(cert_pem);
	return PKI_OK;
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
	char csr_path[PATH_MAX];
	char subj[PKI_SUBJECT_MAX + 8];
	char days_str[16];
	char errbuf[512];
	char *argv[24];

	if (!common_name_is_valid(common_name))
		return PKI_ERR_INVALID_NAME;
	if (!pki_ca_bootstrapped())
		return PKI_ERR_NOT_BOOTSTRAPPED;
	if (pki_intermediate_bootstrapped())
		return PKI_ERR_ALREADY_BOOTSTRAPPED;
	if (snprintf(csr_path, sizeof(csr_path), "%s/intermediate.csr", g_pki_dir) >=
	    (int)sizeof(csr_path))
		return PKI_ERR_PERSIST_FAILED;

	snprintf(subj, sizeof(subj), "/CN=%s", common_name);
	snprintf(days_str, sizeof(days_str), "%d", days);

	/* 1. intermediate keypair */
	argv[0] = (char *)PKI_OPENSSL_BIN;
	argv[1] = "genpkey";
	argv[2] = "-algorithm";
	argv[3] = "RSA";
	argv[4] = "-pkeyopt";
	argv[5] = "rsa_keygen_bits:2048";
	argv[6] = "-out";
	argv[7] = g_intermediate_key_path;
	argv[8] = NULL;
	if (pki_run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "pki: intermediate genpkey failed: %s\n", errbuf);
		unlink(g_intermediate_key_path);
		return PKI_ERR_OPENSSL_FAILED;
	}
	chmod(g_intermediate_key_path, 0600);

	/* 2. CSR, with CA:TRUE + keyCertSign/cRLSign baked in via -addext
	 * (the same pattern pki_cert_create()'s own leaf CSR already uses
	 * for its SAN extension) -- this is what makes the signed result a
	 * real intermediate CA, not just another leaf. */
	argv[0] = (char *)PKI_OPENSSL_BIN;
	argv[1] = "req";
	argv[2] = "-new";
	argv[3] = "-key";
	argv[4] = g_intermediate_key_path;
	argv[5] = "-subj";
	argv[6] = subj;
	argv[7] = "-addext";
	argv[8] = "basicConstraints=critical,CA:TRUE,pathlen:0";
	argv[9] = "-addext";
	argv[10] = "keyUsage=critical,keyCertSign,cRLSign";
	argv[11] = "-out";
	argv[12] = csr_path;
	argv[13] = NULL;
	if (pki_run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "pki: intermediate req failed: %s\n", errbuf);
		unlink(g_intermediate_key_path);
		unlink(csr_path);
		return PKI_ERR_OPENSSL_FAILED;
	}

	/* 3. sign with the ROOT (not self-signed -- this is the whole
	 * point: the intermediate's trust derives from the root). */
	argv[0] = (char *)PKI_OPENSSL_BIN;
	argv[1] = "x509";
	argv[2] = "-req";
	argv[3] = "-in";
	argv[4] = csr_path;
	argv[5] = "-CA";
	argv[6] = g_ca_cert_path;
	argv[7] = "-CAkey";
	argv[8] = g_ca_key_path;
	argv[9] = "-CAcreateserial";
	argv[10] = "-days";
	argv[11] = days_str;
	argv[12] = "-copy_extensions";
	argv[13] = "copy";
	argv[14] = "-out";
	argv[15] = g_intermediate_cert_path;
	argv[16] = NULL;
	if (pki_run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "pki: intermediate x509 sign failed: %s\n", errbuf);
		unlink(g_intermediate_key_path);
		unlink(csr_path);
		unlink(g_intermediate_cert_path);
		return PKI_ERR_OPENSSL_FAILED;
	}
	chmod(g_intermediate_cert_path, 0644);
	unlink(csr_path);

	return PKI_OK;
}

enum pki_error pki_intermediate_get(struct json_writer *w)
{
	char output[1024];
	char subject[PKI_SUBJECT_MAX + 8];
	char serial[PKI_SERIAL_MAX];
	char not_before[PKI_DATE_MAX];
	char not_after[PKI_DATE_MAX];
	char *cert_pem;
	size_t cert_pem_len;
	char *argv[24];

	if (!pki_intermediate_bootstrapped())
		return PKI_ERR_NOT_BOOTSTRAPPED;

	argv[0] = (char *)PKI_OPENSSL_BIN;
	argv[1] = "x509";
	argv[2] = "-in";
	argv[3] = g_intermediate_cert_path;
	argv[4] = "-noout";
	argv[5] = "-subject";
	argv[6] = "-serial";
	argv[7] = "-startdate";
	argv[8] = "-enddate";
	argv[9] = NULL;
	if (pki_run_openssl(argv, output, sizeof(output)) != 0)
		return PKI_ERR_OPENSSL_FAILED;

	if (extract_field(output, "subject=", subject, sizeof(subject)) != 0 ||
	    extract_field(output, "serial=", serial, sizeof(serial)) != 0 ||
	    extract_field(output, "notBefore=", not_before, sizeof(not_before)) != 0 ||
	    extract_field(output, "notAfter=", not_after, sizeof(not_after)) != 0)
		return PKI_ERR_OPENSSL_FAILED;

	if (persist_read_file(g_intermediate_cert_path, &cert_pem, &cert_pem_len) != 0 ||
	    cert_pem == NULL)
		return PKI_ERR_PERSIST_FAILED;

	jw_obj_open(w);
	jw_key(w, "subject");
	jw_str(w, subject);
	jw_key(w, "serial");
	jw_str(w, serial);
	jw_key(w, "not_before");
	jw_str(w, not_before);
	jw_key(w, "not_after");
	jw_str(w, not_after);
	jw_key(w, "cert_pem");
	jw_str(w, cert_pem);
	jw_obj_close(w);
	free(cert_pem);
	return PKI_OK;
}

enum pki_error pki_cert_create(const char *name, const char *const *sans, int san_count, int days,
                                const char *owner_container, struct json_writer *w)
{
	char key_path[PATH_MAX], csr_path[PATH_MAX], crt_path[PATH_MAX];
	char subj[DNS_NAME_MAX + 8];
	char days_str[16];
	char sanbuf[PKI_MAX_SANS * (DNS_NAME_MAX + 8) + 32];
	char output[1024];
	char errbuf[512];
	char serial[PKI_SERIAL_MAX];
	char not_after[PKI_DATE_MAX];
	char *key_pem = NULL, *cert_pem = NULL;
	size_t key_pem_len, cert_pem_len;
	size_t off;
	int i, slot = -1;
	char *argv[24];
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
	    snprintf(csr_path, sizeof(csr_path), "%s/%s.csr", g_certs_dir, name) >=
	        (int)sizeof(csr_path) ||
	    snprintf(crt_path, sizeof(crt_path), "%s/%s.crt", g_certs_dir, name) >=
	        (int)sizeof(crt_path))
		return PKI_ERR_INVALID_NAME;
	snprintf(subj, sizeof(subj), "/CN=%s", name);
	snprintf(days_str, sizeof(days_str), "%d", days);

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
	for (i = 0; i < san_count && off < sizeof(sanbuf); i++) {
		struct in_addr probe;
		const char *kind = inet_pton(AF_INET, sans[i], &probe) == 1 ? "IP" : "DNS";

		if (i == 0)
			off = (size_t)snprintf(sanbuf, sizeof(sanbuf), "subjectAltName=%s:%s", kind,
			                        sans[i]);
		else
			off += (size_t)snprintf(sanbuf + off, sizeof(sanbuf) - off, ",%s:%s", kind,
			                         sans[i]);
	}

	/* 1. leaf keypair */
	argv[0] = (char *)PKI_OPENSSL_BIN;
	argv[1] = "genpkey";
	argv[2] = "-algorithm";
	argv[3] = "RSA";
	argv[4] = "-pkeyopt";
	argv[5] = "rsa_keygen_bits:2048";
	argv[6] = "-out";
	argv[7] = key_path;
	argv[8] = NULL;
	if (pki_run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "pki: genpkey (leaf %s) failed: %s\n", name, errbuf);
		unlink(key_path);
		return PKI_ERR_OPENSSL_FAILED;
	}
	chmod(key_path, 0600);

	/* 2. CSR with the SAN extension baked in (no shell means no
	 * -extfile process substitution -- -addext at CSR-creation time
	 * plus -copy_extensions copy at signing time is the clean
	 * equivalent, verified live before this was written). */
	argv[0] = (char *)PKI_OPENSSL_BIN;
	argv[1] = "req";
	argv[2] = "-new";
	argv[3] = "-key";
	argv[4] = key_path;
	argv[5] = "-subj";
	argv[6] = subj;
	argv[7] = "-addext";
	argv[8] = sanbuf;
	argv[9] = "-out";
	argv[10] = csr_path;
	argv[11] = NULL;
	if (pki_run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "pki: req (leaf %s) failed: %s\n", name, errbuf);
		unlink(key_path);
		unlink(csr_path);
		return PKI_ERR_OPENSSL_FAILED;
	}

	/* 3. sign with the CA -- the intermediate if one has been
	 * bootstrapped (pki_intermediate_create()), the root directly
	 * otherwise. Transparent to every existing caller: no signature
	 * change here, just which key/cert pair actually signs. */
	argv[0] = (char *)PKI_OPENSSL_BIN;
	argv[1] = "x509";
	argv[2] = "-req";
	argv[3] = "-in";
	argv[4] = csr_path;
	argv[5] = "-CA";
	argv[6] = pki_intermediate_bootstrapped() ? g_intermediate_cert_path : g_ca_cert_path;
	argv[7] = "-CAkey";
	argv[8] = pki_intermediate_bootstrapped() ? g_intermediate_key_path : g_ca_key_path;
	argv[9] = "-CAcreateserial";
	argv[10] = "-days";
	argv[11] = days_str;
	argv[12] = "-copy_extensions";
	argv[13] = "copy";
	argv[14] = "-out";
	argv[15] = crt_path;
	argv[16] = NULL;
	if (pki_run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "pki: x509 sign (leaf %s) failed: %s\n", name, errbuf);
		unlink(key_path);
		unlink(csr_path);
		unlink(crt_path);
		return PKI_ERR_OPENSSL_FAILED;
	}
	chmod(crt_path, 0644);
	unlink(csr_path);

	/* 4. read back serial + expiry for the metadata index */
	argv[0] = (char *)PKI_OPENSSL_BIN;
	argv[1] = "x509";
	argv[2] = "-in";
	argv[3] = crt_path;
	argv[4] = "-noout";
	argv[5] = "-serial";
	argv[6] = "-enddate";
	argv[7] = NULL;
	if (pki_run_openssl(argv, output, sizeof(output)) != 0 ||
	    extract_field(output, "serial=", serial, sizeof(serial)) != 0 ||
	    extract_field(output, "notAfter=", not_after, sizeof(not_after)) != 0) {
		fprintf(stderr, "pki: could not read back metadata for leaf %s\n", name);
		unlink(key_path);
		unlink(crt_path);
		return PKI_ERR_OPENSSL_FAILED;
	}

	/* 5. persist the metadata index entry */
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

	/* 6. the one-time response: cert + key PEM */
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

	if (persist_mkdir_p(parent) != 0 ||
	    persist_atomic_write(dst_crt, chain_pem, strlen(chain_pem)) != 0 ||
	    persist_atomic_write(dst_key, key_pem, strlen(key_pem)) != 0) {
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
 * The passphrase reaches openssl through a 0600 file, never argv:
 * -pass pass:<secret> puts it in /proc/<pid>/cmdline, readable for as
 * long as the child lives. The file is in PKI_DIR, which already holds
 * every private key on the box, so it adds no exposure that partition
 * does not already carry, and it is unlinked on every exit path.
 *
 * -aes-256-cbc with pbkdf2 at 600000 iterations, sha256, salted. Every
 * one of those is stated explicitly because `openssl enc` defaults
 * have changed across versions and a bundle that cannot be decrypted
 * by the next release is not a backup. CBC rather than an AEAD mode
 * because `openssh enc` refuses AEAD outright -- so the bundle has no
 * integrity tag, and import compensates by validating what it decodes
 * (see pki_import()) rather than trusting that it decrypted.
 */

#define PKI_EXPORT_CIPHER "-aes-256-cbc"
#define PKI_EXPORT_ITER "600000"

/* Capacity of an argv array, in slots, for argv_push() below. */
#define ARGV_CAP(a) ((int)(sizeof(a) / sizeof((a)[0])))

/*
 * Appends one argument to an incrementally-built openssl command line,
 * bounds-checked against the array's own capacity and always leaving
 * room for the NULL terminator. Returns 0, or -1 when the array is
 * full -- callers OR the results together and check once, then abandon
 * the invocation rather than writing past their own stack frame.
 *
 * This exists because of a measured bug, not a hypothetical one: the
 * first cut of pki_export() and pki_import() (#415) built a
 * 17-argument command line into `char *argv[16]`, so the 17th argument
 * and the NULL terminator landed two pointers past the end of the
 * frame. -Wall -Werror cannot see a runtime index, so it compiled
 * clean, shipped, and presented as an opaque 500 from
 * POST /v1/pki/export with nothing in the log store -- measured on
 * 192.168.15.95 running v2.57.109, 2026-09-12. The five older openssl
 * invocations in this file assign fixed indices into `argv[24]` and
 * were all verified in range (max index 16) while fixing this; they
 * are left as they are, and anything built incrementally goes through
 * here.
 */
static int argv_push(char **argv, int *n, int cap, const char *val)
{
	if (*n >= cap - 1)
		return -1;
	argv[(*n)++] = (char *)val;
	return 0;
}

/* One temp file under PKI_DIR, created O_EXCL at 0600. Returns 0 on
 * success. suffix distinguishes the three this module needs open at
 * once, so a concurrent export cannot collide with an import.
 *
 * O_EXCL is load-bearing rather than decorative: it refuses to write
 * through a symlink or into a file something else planted, and these
 * files hold a passphrase and the decrypted CA key. That means the
 * open must be allowed to FAIL, so the only stale file it can trip
 * over -- one left by a previous daemon whose pid this process reuses
 * -- is cleared and the create retried exactly once, rather than
 * unlinking unconditionally first (which would make O_EXCL dead code
 * and this comment false). */
static int pki_tmp_create(const char *suffix, char *out_path, size_t out_size, const char *data,
                           size_t data_len)
{
	int fd;

	if ((size_t)snprintf(out_path, out_size, "%s/.export-%s.%d", g_pki_dir, suffix,
	                     (int)getpid()) >= out_size)
		return -1;
	fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0 && errno == EEXIST) {
		unlink(out_path);
		fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	}
	if (fd < 0)
		return -1;
	if (data != NULL && data_len > 0) {
		ssize_t n = write(fd, data, data_len);

		if (n < 0 || (size_t)n != data_len) {
			close(fd);
			unlink(out_path);
			return -1;
		}
	}
	close(fd);
	return 0;
}

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
	char plain_path[PATH_MAX], enc_path[PATH_MAX], pass_path[PATH_MAX];
	char pass_arg[PATH_MAX + 16];
	/* 24, matching every other openssl invocation in this file. The
	 * command line below is 17 arguments plus a NULL; argv_push()
	 * enforces the bound rather than this number being trusted. */
	char *argv[24];
	char errbuf[4096];
	char *cipher = NULL;
	size_t cipher_len;
	struct json_writer plain;
	enum pki_error result = PKI_OK;
	int i, j, pushed;

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

	if (pki_tmp_create("pass", pass_path, sizeof(pass_path), passphrase, strlen(passphrase)) != 0 ||
	    pki_tmp_create("plain", plain_path, sizeof(plain_path), plain.buf, plain.len) != 0 ||
	    pki_tmp_create("enc", enc_path, sizeof(enc_path), NULL, 0) != 0) {
		jw_free(&plain);
		unlink(pass_path);
		unlink(plain_path);
		return PKI_ERR_PERSIST_FAILED;
	}
	jw_free(&plain);

	snprintf(pass_arg, sizeof(pass_arg), "file:%s", pass_path);
	i = 0;
	pushed = 0;
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), PKI_OPENSSL_BIN);
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "enc");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), PKI_EXPORT_CIPHER);
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-pbkdf2");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-iter");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), PKI_EXPORT_ITER);
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-md");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "sha256");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-salt");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-a");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-A");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-in");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), plain_path);
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-out");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), enc_path);
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-pass");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), pass_arg);
	if (pushed != 0) {
		logstore_write("pki", "error",
		                "export: openssl command line does not fit argv[%d]",
		                ARGV_CAP(argv));
		result = PKI_ERR_PERSIST_FAILED;
	} else {
		argv[i] = NULL;
		if (pki_run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
			/* logstore, not just stderr: cixd's stderr is never
			 * mirrored into the log store, so an openssl failure
			 * reported only there leaves POST /v1/pki/export as a
			 * 500 with no recorded cause anywhere -- which is
			 * exactly how the argv overflow above hid. */
			logstore_write("pki", "error", "export encryption failed: %s", errbuf);
			result = PKI_ERR_OPENSSL_FAILED;
		} else if (persist_read_file(enc_path, &cipher, &cipher_len) != 0 ||
		            cipher == NULL) {
			logstore_write("pki", "error",
			                "export: could not read back the encrypted bundle");
			result = PKI_ERR_PERSIST_FAILED;
		}
	}

	unlink(pass_path);
	unlink(plain_path);
	unlink(enc_path);
	if (result != PKI_OK) {
		free(cipher);
		return result;
	}

	/* -A gives one base64 line, but openssl still terminates it. */
	while (cipher_len > 0 &&
	       (cipher[cipher_len - 1] == '\n' || cipher[cipher_len - 1] == '\r'))
		cipher[--cipher_len] = '\0';

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
 * the one that actually pins it -- openssl derives a public key from
 * each and they must be byte-identical, which no corrupted input
 * produces by accident.
 */
static int pki_import_pair_matches(const char *key_path, const char *cert_path)
{
	char from_key[8192] = { 0 }, from_cert[8192] = { 0 };
	char *argv[8];
	int i;

	i = 0;
	argv[i++] = (char *)PKI_OPENSSL_BIN;
	argv[i++] = "pkey";
	argv[i++] = "-in";
	argv[i++] = (char *)key_path;
	argv[i++] = "-pubout";
	argv[i] = NULL;
	if (pki_run_openssl(argv, from_key, sizeof(from_key)) != 0 || from_key[0] == '\0')
		return 0;

	i = 0;
	argv[i++] = (char *)PKI_OPENSSL_BIN;
	argv[i++] = "x509";
	argv[i++] = "-in";
	argv[i++] = (char *)cert_path;
	argv[i++] = "-noout";
	argv[i++] = "-pubkey";
	argv[i] = NULL;
	if (pki_run_openssl(argv, from_cert, sizeof(from_cert)) != 0 || from_cert[0] == '\0')
		return 0;

	return strcmp(from_key, from_cert) == 0;
}

enum pki_error pki_import(const char *passphrase, const char *bundle)
{
	char enc_path[PATH_MAX], plain_path[PATH_MAX], pass_path[PATH_MAX];
	char pass_arg[PATH_MAX + 16];
	/* 24, see pki_export()'s own note on this number. */
	char *argv[24];
	char errbuf[4096];
	char *plain = NULL;
	size_t plain_len;
	struct json_value *root = NULL;
	const struct json_value *jcerts;
	enum pki_error result = PKI_OK;
	size_t ci;
	int i, pushed;

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

	if (pki_tmp_create("pass", pass_path, sizeof(pass_path), passphrase, strlen(passphrase)) != 0 ||
	    pki_tmp_create("enc", enc_path, sizeof(enc_path), bundle, strlen(bundle)) != 0 ||
	    pki_tmp_create("plain", plain_path, sizeof(plain_path), NULL, 0) != 0) {
		unlink(pass_path);
		unlink(enc_path);
		return PKI_ERR_PERSIST_FAILED;
	}

	snprintf(pass_arg, sizeof(pass_arg), "file:%s", pass_path);
	i = 0;
	pushed = 0;
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), PKI_OPENSSL_BIN);
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "enc");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-d");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), PKI_EXPORT_CIPHER);
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-pbkdf2");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-iter");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), PKI_EXPORT_ITER);
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-md");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "sha256");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-a");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-A");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-in");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), enc_path);
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-out");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), plain_path);
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), "-pass");
	pushed |= argv_push(argv, &i, ARGV_CAP(argv), pass_arg);
	if (pushed != 0) {
		logstore_write("pki", "error",
		                "import: openssl command line does not fit argv[%d]",
		                ARGV_CAP(argv));
		unlink(pass_path);
		unlink(enc_path);
		unlink(plain_path);
		return PKI_ERR_PERSIST_FAILED;
	}
	argv[i] = NULL;
	if (pki_run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
		/* Overwhelmingly the wrong passphrase. Not logged with the
		 * openssl text, which says "bad decrypt" and nothing an
		 * operator can act on beyond what the status code says. */
		unlink(pass_path);
		unlink(enc_path);
		unlink(plain_path);
		return PKI_ERR_OPENSSL_FAILED;
	}
	if (persist_read_file(plain_path, &plain, &plain_len) != 0 || plain == NULL)
		result = PKI_ERR_PERSIST_FAILED;
	unlink(pass_path);
	unlink(enc_path);
	unlink(plain_path);
	if (result != PKI_OK)
		return result;

	root = json_parse(plain, plain_len);
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
