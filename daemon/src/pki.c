#include "pki.h"
#include "dns.h"
#include "persist.h"

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

/*
 * Forks and execve()s openssl with argv (argv[0] is conventionally
 * PKI_OPENSSL_BIN; the array must be NULL-terminated), redirecting
 * the child's stdout AND stderr into a pipe read back into out (if
 * non-NULL) -- used both for error diagnostics on failure (logged
 * server-side, never echoed raw into an HTTP response) and, for
 * `-noout` query invocations, as the actual result to parse on
 * success. All daemon-owned fds are already CLOEXEC from creation
 * (ADR-0009), so the child only ever inherits the one pipe fd it's
 * meant to.
 */
static int run_openssl(char *const argv[], char *out, size_t out_size)
{
	int pipefd[2];
	pid_t pid;
	int status;
	size_t total = 0;
	ssize_t n;

	if (out != NULL && out_size > 0)
		out[0] = '\0';

	if (pipe2(pipefd, O_CLOEXEC) != 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execve(PKI_OPENSSL_BIN, argv, environ);
		_exit(127);
	}

	close(pipefd[1]);
	if (out != NULL && out_size > 0) {
		while (total + 1 < out_size) {
			n = read(pipefd[0], out + total, out_size - total - 1);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (n == 0)
				break;
			total += (size_t)n;
		}
		out[total] = '\0';
	} else {
		char discard[256];

		while (read(pipefd[0], discard, sizeof(discard)) > 0)
			;
	}
	close(pipefd[0]);

	if (waitpid(pid, &status, 0) != pid)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

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
 * A CA's common_name is a free-form display label ("Kanxeo Root CA"),
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

int pki_init(const char *pki_dir, const char *certs_state_path)
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
	if (run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
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
	if (run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
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
	if (run_openssl(argv, output, sizeof(output)) != 0)
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
	if (run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
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
	if (run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
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
	if (run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
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
	if (run_openssl(argv, output, sizeof(output)) != 0)
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

	off = (size_t)snprintf(sanbuf, sizeof(sanbuf), "subjectAltName=DNS:%s", sans[0]);
	for (i = 1; i < san_count && off < sizeof(sanbuf); i++)
		off += (size_t)snprintf(sanbuf + off, sizeof(sanbuf) - off, ",DNS:%s", sans[i]);

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
	if (run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
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
	if (run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
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
	if (run_openssl(argv, errbuf, sizeof(errbuf)) != 0) {
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
	if (run_openssl(argv, output, sizeof(output)) != 0 ||
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

enum pki_error pki_cert_deliver(const char *name, pid_t pid, const char *dest_dir)
{
	char src_key[PATH_MAX], src_crt[PATH_MAX];
	char dst_key[PATH_MAX], dst_crt[PATH_MAX];
	char parent[PATH_MAX + 32];
	char *key_pem = NULL, *cert_pem = NULL, *intermediate_pem = NULL, *chain_pem = NULL;
	size_t key_pem_len, cert_pem_len, intermediate_pem_len = 0, chain_len;
	enum pki_error result = PKI_OK;

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
	 * has been bootstrapped -- the leaf alone otherwise, unchanged from
	 * before this existed. */
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

	if (snprintf(parent, sizeof(parent), "/proc/%d/root%s", (int)pid, dest_dir) >=
	        (int)sizeof(parent) ||
	    snprintf(dst_crt, sizeof(dst_crt), "%s/tls.crt", parent) >= (int)sizeof(dst_crt) ||
	    snprintf(dst_key, sizeof(dst_key), "%s/tls.key", parent) >= (int)sizeof(dst_key)) {
		free(key_pem);
		free(cert_pem);
		free(intermediate_pem);
		free(chain_pem);
		return PKI_ERR_PERSIST_FAILED;
	}

	if (persist_mkdir_p(parent) != 0 || persist_atomic_write(dst_crt, chain_pem, chain_len) != 0 ||
	    persist_atomic_write(dst_key, key_pem, key_pem_len) != 0) {
		result = PKI_ERR_PERSIST_FAILED;
	} else {
		chmod(dst_key, 0600);
	}

	free(key_pem);
	free(cert_pem);
	free(intermediate_pem);
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

enum pki_error pki_write_trust_bundle_file(const char *dest_path)
{
	char *root_pem = NULL, *intermediate_pem = NULL, *bundle = NULL;
	size_t root_len, intermediate_len = 0, bundle_len;
	enum pki_error result = PKI_OK;

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

	if (persist_atomic_write(dest_path, bundle, bundle_len) != 0)
		result = PKI_ERR_PERSIST_FAILED;

	free(root_pem);
	free(intermediate_pem);
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
