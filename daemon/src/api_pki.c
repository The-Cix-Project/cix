#include "api_pki.h"

#include "apiresp.h"
#include "apiroute.h"
#include "daemonpaths.h"
#include "http.h"
#include "json.h"
#include "pki.h"
#include "logstore.h"
#include "siteconfig.h"
#include "dns.h"
#include "registry.h"
#include "containerdef.h"
#include "namecheck.h"

#include <stdio.h>
#include <string.h>

static void respond_pki_error(int fd, enum pki_error err)
{
	switch (err) {
	case PKI_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid name/common_name/sans");
		break;
	case PKI_ERR_NOT_BOOTSTRAPPED:
		respond_error(fd, 400, "Bad Request", "CA not bootstrapped -- POST /v1/pki/ca first");
		break;
	case PKI_ERR_ALREADY_BOOTSTRAPPED:
		respond_error(fd, 409, "Conflict", "CA is already bootstrapped");
		break;
	case PKI_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "a cert with this name already exists");
		break;
	case PKI_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "PKI cert table full");
		break;
	case PKI_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such CA / cert");
		break;
	case PKI_ERR_OPENSSL_FAILED:
	case PKI_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "PKI operation failed");
		break;
	}
}

/*
 * Reissues this install's own well-known "host" leaf so its SAN
 * always matches the current site config (ADR-0050) -- a fixed
 * record name ("host"), never the FQDN itself, so a rename (any of
 * instance_name/site_name/domain_suffix changing) is a clean
 * delete-then-recreate under the identical stable identity rather
 * than orphaning one differently-named cert per rename. CN ends up
 * being "host" (pki_cert_create()'s own `name` doubles as CN), which
 * is fine and arguably correct -- modern TLS verification uses SAN,
 * not CN, for hostname matching; the SAN is the real, current FQDN.
 *
 * Best-effort, never fails the caller: no root CA bootstrapped yet is
 * the common, expected case on a fresh install with site config set
 * before PKI ever is -- logged, not surfaced as an error on whatever
 * unrelated operation (a site PUT, a CA bootstrap, a reset) triggered
 * this call.
 */
void reissue_host_pki_cert(void)
{
	char fqdn[SITECONFIG_NAME_MAX * 3];
	const char *sans[1];
	struct json_writer scratch;
	enum pki_error perr;

	if (!pki_ca_bootstrapped())
		return;

	siteconfig_host_fqdn(fqdn, sizeof(fqdn));
	if (!dns_name_is_valid(fqdn)) {
		fprintf(stderr, "host: composed FQDN '%s' is not a valid DNS name, skipping "
		                 "PKI (re)issue\n",
		        fqdn);
		return;
	}

	pki_cert_delete("host"); /* PKI_ERR_NOT_FOUND (first time) is fine, ignored */

	sans[0] = fqdn;
	jw_init(&scratch);
	/* "__host" (not "host"): a reserved owner sentinel distinct from
	 * any real container name, so this cert's own ownership can never
	 * collide with (and be wrongly wiped by) a real container someone
	 * happens to name "host" -- same reserved-pseudo-name convention
	 * PKG_BUILD_CONTAINER_NAME ("__pkgbuild") already established. */
	perr = pki_cert_create("host", sans, 1, 365, "__host", &scratch);
	jw_free(&scratch);
	if (perr != PKI_OK)
		fprintf(stderr, "host: could not (re)issue PKI cert for %s (err=%d)\n", fqdn,
		        (int)perr);
}

void handle_pki_ca_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	const char *common_name = "Cix Root CA";
	int days = 3650;
	enum pki_error perr;

	if (body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		if (json_as_string(json_object_get(root, "common_name")) != NULL)
			common_name = json_as_string(json_object_get(root, "common_name"));
		if (json_object_get(root, "days") != NULL)
			days = (int)json_as_number(json_object_get(root, "days"));
	}

	perr = pki_ca_create(common_name, days);
	json_free(root);

	if (perr != PKI_OK) {
		respond_pki_error(fd, perr);
		return;
	}

	reissue_host_pki_cert();

	{
		struct json_writer w;

		jw_init(&w);
		if (pki_ca_get(&w) != PKI_OK) {
			jw_free(&w);
			respond_error(fd, 500, "Internal Server Error", "CA created but could not be read back");
			return;
		}
		respond_json(fd, 201, "Created", &w);
		jw_free(&w);
	}
}

void handle_pki_ca_get(int fd)
{
	struct json_writer w;
	enum pki_error perr;

	jw_init(&w);
	perr = pki_ca_get(&w);
	if (perr != PKI_OK) {
		jw_free(&w);
		/*
		 * PKI_ERR_NOT_BOOTSTRAPPED means two different things
		 * depending on the endpoint: for POST /v1/pki/certs it's a
		 * genuine "you can't do this yet" precondition (400, via
		 * respond_pki_error below). Here, GET-ing a CA that doesn't
		 * exist yet is exactly the same shape as GET
		 * /v1/dns/records/{name} or /v1/networks/{name} on a
		 * missing resource -- 404, matching every other single-
		 * resource GET in this API, not respond_pki_error's generic
		 * (POST-precondition-oriented) 400 mapping.
		 */
		if (perr == PKI_ERR_NOT_BOOTSTRAPPED)
			respond_error(fd, 404, "Not Found", "CA not bootstrapped yet");
		else
			respond_pki_error(fd, perr);
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_pki_intermediate_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	const char *common_name = "Cix Intermediate CA";
	int days = 1825;
	enum pki_error perr;

	if (body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		if (json_as_string(json_object_get(root, "common_name")) != NULL)
			common_name = json_as_string(json_object_get(root, "common_name"));
		if (json_object_get(root, "days") != NULL)
			days = (int)json_as_number(json_object_get(root, "days"));
	}

	perr = pki_intermediate_create(common_name, days);
	json_free(root);

	if (perr != PKI_OK) {
		/*
		 * respond_pki_error()'s own NOT_BOOTSTRAPPED/ALREADY_BOOTSTRAPPED
		 * wording is root-CA-specific ("POST /v1/pki/ca first" / "CA is
		 * already bootstrapped") -- both real but wrong words for this
		 * endpoint, so handled here instead of falling through to it.
		 */
		if (perr == PKI_ERR_NOT_BOOTSTRAPPED)
			respond_error(fd, 400, "Bad Request", "root CA not bootstrapped -- POST /v1/pki/ca first");
		else if (perr == PKI_ERR_ALREADY_BOOTSTRAPPED)
			respond_error(fd, 409, "Conflict", "intermediate is already bootstrapped");
		else
			respond_pki_error(fd, perr);
		return;
	}

	reissue_host_pki_cert(); /* now signed by the intermediate instead of the root */

	{
		struct json_writer w;

		jw_init(&w);
		if (pki_intermediate_get(&w) != PKI_OK) {
			jw_free(&w);
			respond_error(fd, 500, "Internal Server Error",
			              "intermediate created but could not be read back");
			return;
		}
		respond_json(fd, 201, "Created", &w);
		jw_free(&w);
	}
}

void handle_pki_intermediate_get(int fd)
{
	struct json_writer w;
	enum pki_error perr;

	jw_init(&w);
	perr = pki_intermediate_get(&w);
	if (perr != PKI_OK) {
		jw_free(&w);
		/* Same 404-not-400 reasoning as handle_pki_ca_get() above --
		 * GET-ing an intermediate that doesn't exist yet is a missing
		 * resource, not a POST-precondition failure. */
		if (perr == PKI_ERR_NOT_BOOTSTRAPPED)
			respond_error(fd, 404, "Not Found", "intermediate not bootstrapped yet");
		else
			respond_pki_error(fd, perr);
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_pki_cert_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name;
	char qualified_name[DNS_NAME_MAX];
	const struct json_value *jsans, *jdays;
	const char *sans_buf[PKI_MAX_SANS];
	int san_count;
	int days = 365;
	enum pki_error perr;
	struct json_writer w;
	size_t i;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	name = json_as_string(json_object_get(root, "name"));
	jsans = json_object_get(root, "sans");
	jdays = json_object_get(root, "days");
	if (jdays != NULL)
		days = (int)json_as_number(jdays);

	if (name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name missing");
		return;
	}

	/* ADR-0052: same server-side default-qualification rule as DNS
	 * records above -- only the CN/default-SAN name, never an
	 * explicitly-supplied sans[] entry (an operator who lists exact
	 * SANs has already opted into precise control there). */
	siteconfig_qualify(name, qualified_name, sizeof(qualified_name));
	name = qualified_name;

	if (jsans != NULL) {
		if (jsans->type != JSON_ARRAY || jsans->u.array.count == 0 ||
		    jsans->u.array.count > PKI_MAX_SANS) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "sans must be a non-empty array of at most 8 entries");
			return;
		}
		san_count = (int)jsans->u.array.count;
		for (i = 0; i < (size_t)san_count; i++) {
			sans_buf[i] = json_as_string(jsans->u.array.items[i]);
			if (sans_buf[i] == NULL) {
				json_free(root);
				respond_error(fd, 400, "Bad Request", "sans must be an array of strings");
				return;
			}
		}
	} else {
		sans_buf[0] = name;
		san_count = 1;
	}

	jw_init(&w);
	perr = pki_cert_create(name, sans_buf, san_count, days, NULL, &w);
	json_free(root);

	if (perr != PKI_OK) {
		jw_free(&w);
		respond_pki_error(fd, perr);
		return;
	}

	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

void handle_pki_cert_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "certs");
	pki_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_pki_cert_get_one(int fd, const char *name)
{
	struct json_writer w;
	enum pki_error perr;

	jw_init(&w);
	perr = pki_cert_get_one(name, &w);
	if (perr != PKI_OK) {
		jw_free(&w);
		respond_pki_error(fd, perr);
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_pki_cert_delete(int fd, const char *name)
{
	enum pki_error perr = pki_cert_delete(name);

	if (perr != PKI_OK) {
		respond_pki_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * For every currently-live container that owns a just-reissued leaf
 * (name == the container's own name, owner_container == the
 * container's own name -- the exact convention create_container_from_
 * body()'s own pki_issue block always uses, near line 2319),
 * redeliver it so a running service's tls.crt/tls.key don't go stale
 * after a CA reset.
 *
 * Deliberately enumerated via registry_list_names(), not
 * containerdef_resolve_order() -- containerdef_add() only persists a
 * definition for restart != "no" (main.c's own POST /v1/containers
 * handler, a few hundred lines up); a plain, unpersisted restart:"no"
 * container is still a real, live pki_issue owner and must not be
 * silently skipped just because it has no containerdef entry to
 * enumerate through. pki_cert_owned_by() is checked against the PKI
 * index itself (the real source of truth for "does this container own
 * a cert"), not the original create request's own pki_issue flag --
 * robust regardless of whether a persisted definition happens to
 * exist to recover that flag from.
 *
 * pki_cert_dir is recovered from the persisted definition when one
 * exists (same field the original pki_issue block reads); a
 * restart:"no" container that used a non-default --pki-cert-dir= has
 * no persisted body to recover it from, so the default applies --
 * a known, narrow gap (documented in ADR-0049) rather than a silent
 * wrong-path write.
 */
static void redeliver_pki_certs_after_reset(void)
{
	char names[REGISTRY_MAX_CONTAINERS][REGISTRY_NAME_MAX];
	int count = registry_list_names(names, REGISTRY_MAX_CONTAINERS);
	int i;

	for (i = 0; i < count; i++) {
		struct registry_entry *entry = registry_find(names[i]);
		struct container_def *def;
		char cert_dir_buf[PATH_MAX];
		enum pki_error derr;

		/*
		 * Both skips below were bare continues, and that silence cost a
		 * probe cycle (#417): test_pki's own reset case failed with
		 * "resetlive's delivered tls.crt was not redelivered after
		 * reset" while nothing anywhere said which of the two branches
		 * had declined to deliver, or that any had. A CA reset destroys
		 * the signer of every cert already delivered into a container,
		 * so a container skipped here keeps serving a certificate
		 * signed by a CA that no longer exists -- which is exactly the
		 * situation an operator needs told about rather than left to
		 * infer from a TLS failure later.
		 */
		if (entry == NULL || !entry->running) {
			/* Both streams, for the reason ldap_record_sync_all()'s own
			 * comment gives: stderr is not mirrored into the log store,
			 * and a test harness capturing a forked cixd sees ONLY
			 * stderr -- so a log-store-only line is invisible exactly
			 * where it matters most. Writing to the store alone here
			 * cost a probe cycle on #417, which is that comment being
			 * right and me not having read it. */
			fprintf(stderr, "pki reset: %s not running -- certificate NOT refreshed\n",
			        names[i]);
			logstore_write("cixd", "warn",
			                "pki reset: %s was not running, so its delivered certificate "
			                "was NOT refreshed -- it holds one signed by the CA this reset "
			                "destroyed until it is recreated",
			                names[i]);
			continue;
		}
		if (!pki_cert_owned_by(names[i], names[i])) {
			/* Ordinary for most containers -- only one that owns a cert
			 * under its own name has anything to redeliver -- so info,
			 * not warn. It is logged at all because "nothing happened"
			 * and "nothing was meant to happen" are indistinguishable
			 * without it. */
			fprintf(stderr, "pki reset: %s owns no cert of its own -- nothing to redeliver\n",
			        names[i]);
			logstore_write("cixd", "info",
			                "pki reset: %s owns no certificate under its own name, nothing "
			                "to redeliver",
			                names[i]);
			continue;
		}

		snprintf(cert_dir_buf, sizeof(cert_dir_buf), PKI_CONTAINER_CERT_DIR);
		def = containerdef_find(names[i]);
		if (def != NULL) {
			struct json_value *body_root = json_parse(def->body, def->body_len);

			if (body_root != NULL) {
				const char *dir = json_as_string(json_object_get(body_root, "pki_cert_dir"));

				if (dir != NULL)
					snprintf(cert_dir_buf, sizeof(cert_dir_buf), "%s", dir);
				json_free(body_root);
			}
		}

		derr = pki_cert_deliver(names[i], names[i], cert_dir_buf);
		fprintf(stderr, "pki reset: redeliver %s -> %s: %s\n", names[i], cert_dir_buf,
		        derr == PKI_OK ? "ok" : "FAILED");
		if (derr != PKI_OK)
			logstore_write("cixd", "error",
			                "pki reset: redelivering %s's certificate into %s failed "
			                "(err=%d) -- it is still holding one signed by the destroyed CA",
			                names[i], cert_dir_buf, (int)derr);
		else
			logstore_write("cixd", "info", "pki reset: redelivered %s's certificate into %s",
			                names[i], cert_dir_buf);
	}
}

/*
 * #415 / ADR-0281. Both take their passphrase in the request BODY, never
 * a query parameter: a query string is logged by every intermediary that
 * logs a request line, and this one opens the trust root.
 */
void handle_pki_export(int fd, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const char *passphrase;
	struct json_writer w;
	enum pki_error perr;

	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	passphrase = json_as_string(json_object_get(root, "passphrase"));
	if (passphrase == NULL || passphrase[0] == '\0') {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "passphrase is required and may not be empty -- it is the only thing protecting the CA private key in the bundle");
		return;
	}

	jw_init(&w);
	perr = pki_export(passphrase, &w);
	json_free(root);
	if (perr != PKI_OK) {
		jw_free(&w);
		respond_pki_error(fd, perr);
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_pki_import(int fd, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const char *passphrase, *bundle;
	struct json_writer w;
	enum pki_error perr;

	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	passphrase = json_as_string(json_object_get(root, "passphrase"));
	bundle = json_as_string(json_object_get(root, "bundle"));
	if (passphrase == NULL || passphrase[0] == '\0' || bundle == NULL || bundle[0] == '\0') {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "passphrase and bundle are both required");
		return;
	}

	perr = pki_import(passphrase, bundle);
	json_free(root);
	if (perr != PKI_OK) {
		/*
		 * Not respond_pki_error(): that maps OPENSSL_FAILED to 500, and
		 * here the overwhelming cause is a wrong passphrase, which is
		 * the caller's input and not a server fault. The two are not
		 * distinguishable from outside -- AES-256-CBC has no integrity
		 * tag, so a wrong passphrase and a corrupted bundle both present
		 * as "decoded to something that is not a bundle" -- so the
		 * message says what the operator can actually check rather than
		 * asserting a cause that was never established.
		 */
		if (perr == PKI_ERR_OPENSSL_FAILED)
			respond_error(fd, 400, "Bad Request",
			              "the bundle did not decrypt and validate -- check the passphrase, and that the bundle is the complete single-line string POST /v1/pki/export returned");
		else
			respond_pki_error(fd, perr);
		return;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "certs_restored");
	jw_int(&w, pki_cert_count());
	jw_key(&w, "intermediate_restored");
	jw_bool(&w, pki_intermediate_bootstrapped());
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_pki_reset(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	char root_cn[PKI_SUBJECT_MAX];
	char intermediate_cn[PKI_SUBJECT_MAX];
	int root_days = 3650;
	int intermediate_days = 1825;
	int leaf_days = 365;
	enum pki_error perr;
	struct json_writer w;

	snprintf(root_cn, sizeof(root_cn), "Cix Root CA - %s", siteconfig_domain_suffix());
	snprintf(intermediate_cn, sizeof(intermediate_cn), "Cix Intermediate CA - %s",
	         siteconfig_domain_suffix());

	if (body_len > 0) {
		const char *s;

		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		s = json_as_string(json_object_get(root, "root_common_name"));
		if (s != NULL)
			snprintf(root_cn, sizeof(root_cn), "%s", s);
		s = json_as_string(json_object_get(root, "intermediate_common_name"));
		if (s != NULL)
			snprintf(intermediate_cn, sizeof(intermediate_cn), "%s", s);
		if (json_object_get(root, "root_days") != NULL)
			root_days = (int)json_as_number(json_object_get(root, "root_days"));
		if (json_object_get(root, "intermediate_days") != NULL)
			intermediate_days = (int)json_as_number(json_object_get(root, "intermediate_days"));
		if (json_object_get(root, "leaf_days") != NULL)
			leaf_days = (int)json_as_number(json_object_get(root, "leaf_days"));
	}

	jw_init(&w);
	perr = pki_ca_reset(root_cn, intermediate_cn, root_days, intermediate_days, leaf_days, &w);
	json_free(root);

	if (perr != PKI_OK) {
		jw_free(&w);
		respond_pki_error(fd, perr);
		return;
	}

	redeliver_pki_certs_after_reset();

	/*
	 * Already reissued once above if "host" was tracked before this
	 * reset (pki_ca_reset()'s own generic per-leaf reissue loop) --
	 * called again here anyway, for the case where it wasn't (an
	 * existing chain that predates this feature, or one that was
	 * bootstrapped without ever going through a site config PUT). The
	 * redundant reissue in the already-tracked case is harmless (same
	 * FQDN, freshly signed either way) and keeps this guarantee simple:
	 * "host" always exists and is current after any PKI-affecting
	 * operation, full stop.
	 */
	reissue_host_pki_cert();

	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}
