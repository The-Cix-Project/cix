/*
 * Writing the running configuration back (ADR-0292).
 *
 * `GET /v1/config` (ADR-0206) renders one ordered document describing
 * everything this host is configured to do. These two endpoints are
 * the other direction: `POST /v1/config/diff` says what a supplied
 * document would change, and `POST /v1/config` changes it.
 *
 * ONE COMPUTATION, TWO ENDPOINTS. Both build the same plan through
 * build_plan(); the diff endpoint renders it and stops, the apply
 * endpoint renders it and runs it. Every applier takes a `dry_run`
 * flag and is called with it set while the plan is being built, so the
 * appliability an operator is shown comes from the same code that will
 * do the work, not from a second opinion about it.
 *
 * WHAT dry_run ACTUALLY PROVES, precisely, because overstating it
 * would be worse than not having it: it proves the document's SHAPE is
 * acceptable -- every field the section renders is present, no field
 * that is not, every type right, and no attempt to change a derived
 * value or a redacted secret. It does NOT prove the subsystem will
 * accept the VALUES: `siteconfig_set()` validates a domain suffix,
 * `resolv_set()` validates a dotted quad, `zswap_set()` writes kernel
 * parameters the kernel may refuse, and none of them offer a way to
 * ask without doing. So an apply can still fail after its plan
 * validated, and the response says per section exactly where it
 * stopped rather than claiming the whole thing was atomic.
 *
 * ONLY `replace` SECTIONS APPLY. The ConfigDocument schema declares,
 * per section, whether one setter replaces it outright (`replace`) or
 * whether applying it means creating, updating and deleting individual
 * live resources (`reconcile`). Reconcile is not implemented: a
 * supplied section in that mode with real changes refuses the WHOLE
 * request. Applying the appliable half of a document and reporting the
 * rest as skipped would leave a host in a state matching neither the
 * document nor what it had before.
 *
 * SECRETS ARE NOT SETTABLE HERE. The document renders a token or
 * password as a set/not-set boolean, never a value, so there is no
 * spelling of "set this secret" for it to carry. Every applier that
 * touches a setter with a secret parameter passes NULL, which those
 * setters define as "leave it alone" -- so changing a repo URL through
 * this document cannot silently clear the token that reaches it.
 */
#include "api_config.h"

#include "apiresp.h"
#include "backupconfig.h"
#include "dhcp.h"
#include "diskrole.h"
#include "kmod.h"
#include "kmodconfig.h"
#include "pkgpolicy.h"
#include "registry.h"
#include "syslogfwd.h"
#include "sysctlconfig.h"
#include "config.h"
#include "daemon_config.h"
#include "dns.h"
#include "json.h"
#include "jsondiff.h"
#include "ldap.h"
#include "ntp.h"
#include "pkg.h"
#include "resolv.h"
#include "siteconfig.h"
#include "swap.h"
#include "zswap.h"

#include "generated/config_sections.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG_ERR_MAX 320

/*
 * diskrole needs to know where containers live, which is main.c's to
 * know and arrives with the request. Same split config.c already has
 * for the renderers.
 */
static const char *g_containers_dir;

/* ---- reading fields out of a supplied section ---- */

static int need_object(const struct json_value *v, const char *what, char *err, size_t errsz)
{
	if (v == NULL || v->type != JSON_OBJECT) {
		snprintf(err, errsz, "%s must be a JSON object", what);
		return -1;
	}
	return 0;
}

static int need_string(const struct json_value *o, const char *k, const char **out, char *err,
                       size_t errsz)
{
	const struct json_value *v = json_object_get(o, k);

	if (v == NULL || v->type != JSON_STRING) {
		snprintf(err, errsz, "\"%s\" must be a string", k);
		return -1;
	}
	*out = v->u.string;
	return 0;
}

/*
 * A field the document renders as null when it is unset. NULL comes
 * back for null, which each caller maps onto whatever its own setter
 * spells "unset" -- they do not agree on that, so this does not guess.
 */
static int need_string_or_null(const struct json_value *o, const char *k, const char **out,
                               char *err, size_t errsz)
{
	const struct json_value *v = json_object_get(o, k);

	if (v != NULL && v->type == JSON_NULL) {
		*out = NULL;
		return 0;
	}
	return need_string(o, k, out, err, errsz);
}

static int need_bool(const struct json_value *o, const char *k, int *out, char *err, size_t errsz)
{
	const struct json_value *v = json_object_get(o, k);

	if (v == NULL || v->type != JSON_BOOL) {
		snprintf(err, errsz, "\"%s\" must be true or false", k);
		return -1;
	}
	*out = v->u.boolean;
	return 0;
}

static int need_int(const struct json_value *o, const char *k, long long *out, char *err,
                    size_t errsz)
{
	const struct json_value *v = json_object_get(o, k);

	if (v == NULL || v->type != JSON_NUMBER || v->u.number != (double)(long long)v->u.number) {
		snprintf(err, errsz, "\"%s\" must be a whole number", k);
		return -1;
	}
	*out = (long long)v->u.number;
	return 0;
}

/*
 * Reads an array of strings, borrowing the pointers from the parsed
 * tree (which outlives every use of them here).
 */
static int need_string_list(const struct json_value *arr, const char **out, int max, int *count,
                            const char *what, char *err, size_t errsz)
{
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY) {
		snprintf(err, errsz, "%s must be an array of strings", what);
		return -1;
	}
	if ((int)arr->u.array.count > max) {
		snprintf(err, errsz, "%s holds at most %d entries, %d supplied", what, max,
		         (int)arr->u.array.count);
		return -1;
	}
	for (i = 0; i < arr->u.array.count; i++) {
		if (arr->u.array.items[i]->type != JSON_STRING) {
			snprintf(err, errsz, "%s[%d] must be a string", what, (int)i);
			return -1;
		}
		out[i] = arr->u.array.items[i]->u.string;
	}
	*count = (int)arr->u.array.count;
	return 0;
}

/*
 * A `replace` section is replaced, so it is supplied whole: every
 * field the document renders must be there, and nothing else may be.
 * The alternative -- treating an absent field as "leave it alone" --
 * would mean the document you fetch and the document you apply are
 * different languages, and a typo in a field name would silently
 * become "no change" instead of an error.
 */
static int require_whole_section(const struct json_value *live, const struct json_value *sup,
                                 const char *state_fields, char *err, size_t errsz)
{
	size_t i;

	if (need_object(sup, "a replace section", err, errsz) != 0)
		return -1;
	for (i = 0; i < live->u.object.count; i++) {
		/* An observed member is not something the sender chose, so
		 * requiring it back would make a hand-written document
		 * impossible and a stale one wrong. */
		if (jsondiff_field_listed(state_fields, live->u.object.keys[i]))
			continue;
		if (json_object_get(sup, live->u.object.keys[i]) == NULL) {
			snprintf(err, errsz,
			         "\"%s\" is missing -- this section is replaced whole, so send it "
			         "back with every field it renders", live->u.object.keys[i]);
			return -1;
		}
	}
	for (i = 0; i < sup->u.object.count; i++) {
		if (json_object_get(live, sup->u.object.keys[i]) == NULL) {
			snprintf(err, errsz, "\"%s\" is not a field of this section",
			         sup->u.object.keys[i]);
			return -1;
		}
	}
	return 0;
}

/*
 * A redaction marker: a field standing in for a secret this document
 * never carried, so there is no spelling of "set it" for it to be.
 *
 * Refused rather than ignored, which is the opposite of how an
 * OBSERVED field is treated a few lines below -- and the difference is
 * deliberate. Sending back a stale observation is an accident of
 * timing and means nothing, so it is ignored. Sending back
 * `auth_token_set: false` against a host that has a token looks like
 * an instruction to clear it, and quietly doing nothing about that
 * would be the worst of the three available behaviours.
 */
static int require_unchanged(const struct json_value *live, const struct json_value *sup,
                             const char *field, const char *why, char *err, size_t errsz)
{
	if (jsondiff_equal(json_object_get(live, field), json_object_get(sup, field)))
		return 0;
	snprintf(err, errsz, "\"%s\" %s", field, why);
	return -1;
}

static int changed(const struct json_value *live, const struct json_value *sup, const char *field)
{
	return !jsondiff_equal(json_object_get(live, field), json_object_get(sup, field));
}

/* ---- the appliers, one per `replace` section ----
 *
 * Declared from the generated list, so a section the schema calls
 * `replace` with no function here does not link, and a function here
 * for a section the schema no longer calls `replace` is an unused
 * static -- an error under -Werror. The schema and this file cannot
 * drift apart in either direction.
 */
#define X(name)                                                                                \
	static int cfg_apply_##name(const struct json_value *live, const struct json_value *sup,   \
	                            const char *state_fields, int dry_run, char *err,              \
	                            size_t errsz);
CIX_CONFIG_SECTIONS_REPLACE(X)
#undef X

static int cfg_apply_site(const struct json_value *live, const struct json_value *sup,
                          const char *state_fields, int dry_run, char *err,
                          size_t errsz)
{
	const char *instance, *site, *domain;

	if (require_whole_section(live, sup, state_fields, err, errsz) != 0)
		return -1;
	if (need_string(sup, "instance_name", &instance, err, errsz) != 0 ||
	    need_string(sup, "site_name", &site, err, errsz) != 0 ||
	    need_string(sup, "domain_suffix", &domain, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	switch (siteconfig_set(instance, site, domain)) {
	case SITECONFIG_OK:
		return 0;
	case SITECONFIG_ERR_INVALID_INSTANCE_NAME:
		snprintf(err, errsz, "invalid instance_name");
		return -1;
	case SITECONFIG_ERR_INVALID_SITE_NAME:
		snprintf(err, errsz, "invalid site_name");
		return -1;
	case SITECONFIG_ERR_INVALID_DOMAIN_SUFFIX:
		snprintf(err, errsz, "invalid domain_suffix");
		return -1;
	default:
		snprintf(err, errsz, "the site configuration could not be persisted");
		return -1;
	}
}

static int daemon_set_err(enum daemon_config_error e, const char *field, char *err, size_t errsz)
{
	switch (e) {
	case DAEMON_CONFIG_OK:
		return 0;
	case DAEMON_CONFIG_ERR_INVALID_PORT:
		snprintf(err, errsz, "\"%s\": not a usable port", field);
		return -1;
	case DAEMON_CONFIG_ERR_REBIND_FAILED:
		snprintf(err, errsz, "\"%s\": the daemon could not bind the new listener", field);
		return -1;
	case DAEMON_CONFIG_ERR_WOULD_HAVE_NO_LISTENER:
		snprintf(err, errsz, "\"%s\": this would leave the daemon with no listener at all",
		         field);
		return -1;
	case DAEMON_CONFIG_ERR_HTTPS_NOT_AVAILABLE:
		snprintf(err, errsz, "\"%s\": there is no host certificate to serve TLS with yet",
		         field);
		return -1;
	default:
		snprintf(err, errsz, "\"%s\": the daemon configuration could not be persisted",
		         field);
		return -1;
	}
}

/*
 * The one section whose apply can cut the connection carrying it --
 * changing a port or disabling a listener rebinds immediately, exactly
 * as PUT /v1/system/daemon already does. Enables run before disables
 * so that swapping which listener is in use is not refused halfway
 * through for momentarily having none.
 */
static int cfg_apply_daemon(const struct json_value *live, const struct json_value *sup,
                            const char *state_fields, int dry_run, char *err,
                            size_t errsz)
{
	long long port, https_port;
	int http_enabled, https_enabled;

	if (require_whole_section(live, sup, state_fields, err, errsz) != 0)
		return -1;
	if (need_int(sup, "port", &port, err, errsz) != 0 ||
	    need_int(sup, "https_port", &https_port, err, errsz) != 0 ||
	    need_bool(sup, "http_enabled", &http_enabled, err, errsz) != 0 ||
	    need_bool(sup, "https_enabled", &https_enabled, err, errsz) != 0)
		return -1;
	if (port < 0 || port > 65535 || https_port < 0 || https_port > 65535) {
		snprintf(err, errsz, "a port must be between 0 and 65535");
		return -1;
	}
	if (dry_run)
		return 0;
	if (changed(live, sup, "http_enabled") && http_enabled &&
	    daemon_set_err(daemon_config_set_http_enabled(1), "http_enabled", err, errsz) != 0)
		return -1;
	if (changed(live, sup, "https_enabled") && https_enabled &&
	    daemon_set_err(daemon_config_set_https_enabled(1), "https_enabled", err, errsz) != 0)
		return -1;
	if (changed(live, sup, "port") &&
	    daemon_set_err(daemon_config_set_port((int)port), "port", err, errsz) != 0)
		return -1;
	if (changed(live, sup, "https_port") &&
	    daemon_set_err(daemon_config_set_https_port((int)https_port), "https_port", err,
	                   errsz) != 0)
		return -1;
	if (changed(live, sup, "http_enabled") && !http_enabled &&
	    daemon_set_err(daemon_config_set_http_enabled(0), "http_enabled", err, errsz) != 0)
		return -1;
	if (changed(live, sup, "https_enabled") && !https_enabled &&
	    daemon_set_err(daemon_config_set_https_enabled(0), "https_enabled", err, errsz) != 0)
		return -1;
	return 0;
}

static int cfg_apply_resolver(const struct json_value *live, const struct json_value *sup,
                              const char *state_fields, int dry_run, char *err,
                              size_t errsz)
{
	const char *ns[RESOLV_MAX_NAMESERVERS];
	int count = 0;

	if (require_whole_section(live, sup, state_fields, err, errsz) != 0)
		return -1;
	if (need_string_list(json_object_get(sup, "nameservers"), ns, RESOLV_MAX_NAMESERVERS, &count,
	                     "\"nameservers\"", err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	switch (resolv_set(ns, count)) {
	case RESOLV_OK:
		return 0;
	case RESOLV_ERR_INVALID_IP:
		snprintf(err, errsz, "a nameserver is not a dotted-quad IPv4 address");
		return -1;
	case RESOLV_ERR_TOO_MANY:
		snprintf(err, errsz, "at most %d nameservers", RESOLV_MAX_NAMESERVERS);
		return -1;
	default:
		snprintf(err, errsz, "/etc/resolv.conf could not be written");
		return -1;
	}
}

static int cfg_apply_time(const struct json_value *live, const struct json_value *sup,
                          const char *state_fields, int dry_run, char *err,
                          size_t errsz)
{
	const char *addrs[NTP_MAX_UPSTREAM];
	int count = 0;

	if (require_whole_section(live, sup, state_fields, err, errsz) != 0)
		return -1;
	if (need_string_list(json_object_get(sup, "upstream"), addrs, NTP_MAX_UPSTREAM, &count,
	                     "\"upstream\"", err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	switch (ntp_set_upstream(addrs, count)) {
	case NTP_OK:
		return 0;
	case NTP_ERR_INVALID_IP:
		snprintf(err, errsz, "an upstream address is not a dotted-quad IPv4 address");
		return -1;
	case NTP_ERR_TOO_MANY:
		snprintf(err, errsz, "at most %d upstream addresses", NTP_MAX_UPSTREAM);
		return -1;
	default:
		snprintf(err, errsz, "the upstream list could not be persisted");
		return -1;
	}
}

static int cfg_apply_zswap(const struct json_value *live, const struct json_value *sup,
                           const char *state_fields, int dry_run, char *err,
                           size_t errsz)
{
	struct zswap_config next;
	const char *compressor;
	long long pct;
	int enabled;

	if (require_whole_section(live, sup, state_fields, err, errsz) != 0)
		return -1;
	/*
	 * `supported`, `kernel` and `available_compressors` are observed,
	 * declared as such in the schema, and therefore ignored rather
	 * than refused: this section used to reject a document fetched
	 * before its own last change, because the `kernel` mirror in it
	 * had gone stale (#471).
	 */
	if (need_bool(sup, "enabled", &enabled, err, errsz) != 0 ||
	    need_int(sup, "max_pool_percent", &pct, err, errsz) != 0 ||
	    need_string(sup, "compressor", &compressor, err, errsz) != 0)
		return -1;
	if (strlen(compressor) >= ZSWAP_COMPRESSOR_MAX) {
		snprintf(err, errsz, "\"compressor\" is too long");
		return -1;
	}
	if (dry_run)
		return 0;
	memset(&next, 0, sizeof(next));
	next.enabled = enabled;
	next.max_pool_percent = (int)pct;
	snprintf(next.compressor, sizeof(next.compressor), "%s", compressor);
	switch (zswap_set(&next)) {
	case ZSWAP_OK:
		return 0;
	case ZSWAP_ERR_INVALID:
		snprintf(err, errsz, "max_pool_percent must be 1-100 and compressor one this "
		                     "kernel offers");
		return -1;
	case ZSWAP_ERR_UNSUPPORTED:
		snprintf(err, errsz, "this kernel has no zswap");
		return -1;
	case ZSWAP_ERR_APPLY_FAILED:
		snprintf(err, errsz, "the kernel refused the zswap parameters");
		return -1;
	default:
		snprintf(err, errsz, "the zswap configuration could not be persisted");
		return -1;
	}
}

static int cfg_apply_swap(const struct json_value *live, const struct json_value *sup,
                          const char *state_fields, int dry_run, char *err,
                          size_t errsz)
{
	enum swap_error e;
	long long size_mb;
	int enabled;

	if (require_whole_section(live, sup, state_fields, err, errsz) != 0)
		return -1;
	/* `path` and `disk` are observed (the file's location, and the
	 * disk carrying it, which POST /v1/storage-roles decides). */
	if (need_bool(sup, "enabled", &enabled, err, errsz) != 0 ||
	    need_int(sup, "size_mb", &size_mb, err, errsz) != 0)
		return -1;
	/*
	 * There are only two operations behind this section, swap_enable()
	 * and swap_disable(), and neither resizes. So a size change with
	 * `enabled` staying put is refused HERE, by shape, where it can be
	 * described accurately -- rather than reaching swap_enable() on
	 * already-enabled swap and coming back ALREADY_ENABLED, which this
	 * function would then have reported as "no longer in the state
	 * this plan was computed from": a false diagnosis of a document
	 * that was perfectly current.
	 */
	/*
	 * Both branches measured on 192.168.15.95, 2026-09-15. The
	 * enabled branch was unreachable at first -- host swap could not
	 * be enabled on that host at all, because btrfs refuses a
	 * copy-on-write swapfile (#472) -- and was verified once that was
	 * fixed: a `size_mb` change against enabled swap is refused here
	 * with the message above, and the disable-then-enable it points at
	 * works through this same endpoint.
	 */
	if (!changed(live, sup, "enabled") && changed(live, sup, "size_mb")) {
		if (enabled)
			snprintf(err, errsz,
			         "\"size_mb\" cannot be changed while swap stays enabled -- "
			         "disable it and enable it again at the new size");
		else
			snprintf(err, errsz, "\"size_mb\" means nothing while swap is disabled");
		return -1;
	}
	if (dry_run)
		return 0;
	e = enabled ? swap_enable(size_mb) : swap_disable();
	switch (e) {
	case SWAP_OK:
		return 0;
	case SWAP_ERR_ALREADY_ENABLED:
	case SWAP_ERR_NOT_ENABLED:
		/* The plan said `enabled` was flipping and the shape check
		 * above passed, so reaching here means something else moved
		 * swap between the plan and the apply. That is now the only
		 * way to get this message, which is what makes it true. */
		snprintf(err, errsz, "swap is no longer in the state this plan was computed from");
		return -1;
	case SWAP_ERR_INVALID_SIZE:
		snprintf(err, errsz, "\"size_mb\" is not a usable swap size");
		return -1;
	case SWAP_ERR_IO:
		snprintf(err, errsz, "the swap file could not be written or activated");
		return -1;
	default:
		snprintf(err, errsz, "the swap configuration could not be persisted");
		return -1;
	}
}

static int cfg_apply_backup(const struct json_value *live, const struct json_value *sup,
                            const char *state_fields, int dry_run, char *err,
                            size_t errsz)
{
	const char *disk;
	int enabled;

	if (require_whole_section(live, sup, state_fields, err, errsz) != 0)
		return -1;
	if (need_string_or_null(sup, "disk", &disk, err, errsz) != 0 ||
	    need_bool(sup, "enabled", &enabled, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (backupconfig_set(disk, enabled) != BACKUPCONFIG_OK) {
		snprintf(err, errsz, "the backup configuration could not be persisted");
		return -1;
	}
	return 0;
}

static int ldap_set_err(enum ldap_record_error e, char *err, size_t errsz)
{
	if (e == LDAP_RECORD_OK)
		return 0;
	if (e == LDAP_RECORD_ERR_INVALID_FIELD) {
		snprintf(err, errsz, "the directory refused these values -- a port outside "
		                     "1..65535, both ports equal, both listeners disabled, or "
		                     "client_tls naming a disabled listener");
		return -1;
	}
	if (e == LDAP_RECORD_ERR_NO_SERVER_CERT) {
		snprintf(err, errsz, "server_tls cannot be turned on until the registered server "
		                     "has a delivered certificate");
		return -1;
	}
	snprintf(err, errsz, "the directory configuration could not be persisted");
	return -1;
}

/*
 * Three setters behind one section, each called only when its own
 * fields moved. That matters beyond tidiness: ldap_config_set_
 * listeners() reloads a running directory server, so calling it for a
 * document that did not change a listener would restart a service for
 * no reason.
 */
static int cfg_apply_ldap(const struct json_value *live, const struct json_value *sup,
                          const char *state_fields, int dry_run, char *err,
                          size_t errsz)
{
	const char *client_uri, *base_dn, *bind_dn;
	long long start_uid, start_gid, plain_port, tls_port;
	int client_tls, server_plaintext, server_tls;
	char server_container[64];

	if (require_whole_section(live, sup, state_fields, err, errsz) != 0)
		return -1;
	if (require_unchanged(live, sup, "bind_password_set",
	                      "is a redaction marker: this document never carried the password, "
	                      "so it cannot set one -- use PUT /v1/ldap/config", err, errsz) != 0)
		return -1;
	if (need_int(sup, "start_uid", &start_uid, err, errsz) != 0 ||
	    need_int(sup, "start_gid", &start_gid, err, errsz) != 0 ||
	    need_string_or_null(sup, "client_uri", &client_uri, err, errsz) != 0 ||
	    need_string_or_null(sup, "base_dn", &base_dn, err, errsz) != 0 ||
	    need_string_or_null(sup, "bind_dn", &bind_dn, err, errsz) != 0 ||
	    need_bool(sup, "client_tls", &client_tls, err, errsz) != 0 ||
	    need_bool(sup, "server_plaintext", &server_plaintext, err, errsz) != 0 ||
	    need_int(sup, "server_plaintext_port", &plain_port, err, errsz) != 0 ||
	    need_bool(sup, "server_tls", &server_tls, err, errsz) != 0 ||
	    need_int(sup, "server_tls_port", &tls_port, err, errsz) != 0)
		return -1;
	if (start_uid <= 0 || start_gid <= 0) {
		snprintf(err, errsz, "\"start_uid\" and \"start_gid\" must be above zero");
		return -1;
	}
	if (dry_run)
		return 0;
	if ((changed(live, sup, "start_uid") || changed(live, sup, "start_gid")) &&
	    ldap_set_err(ldap_config_set((int)start_uid, (int)start_gid), err, errsz) != 0)
		return -1;
	if (changed(live, sup, "client_uri") || changed(live, sup, "base_dn") ||
	    changed(live, sup, "bind_dn")) {
		/*
		 * "" clears a field where NULL would mean "leave it alone",
		 * and the document spells an unset field as null -- so a null
		 * here is a request to clear. The bind password is always
		 * NULL: it has no spelling in this document at all.
		 */
		if (ldap_set_err(ldap_config_set_client(client_uri == NULL ? "" : client_uri,
		                                        base_dn == NULL ? "" : base_dn,
		                                        bind_dn == NULL ? "" : bind_dn, NULL),
		                 err, errsz) != 0)
			return -1;
	}
	if (changed(live, sup, "client_tls") || changed(live, sup, "server_plaintext") ||
	    changed(live, sup, "server_plaintext_port") || changed(live, sup, "server_tls") ||
	    changed(live, sup, "server_tls_port")) {
		server_container[0] = '\0';
		if (ldap_set_err(ldap_config_set_listeners(client_tls, server_plaintext,
		                                           (int)plain_port, server_tls,
		                                           (int)tls_port, server_container,
		                                           sizeof(server_container)),
		                 err, errsz) != 0)
			return -1;
	}
	return 0;
}

static int cfg_apply_package_repo(const struct json_value *live, const struct json_value *sup,
                                  const char *state_fields, int dry_run, char *err,
                                  size_t errsz)
{
	const char *url, *kind, *ref;

	if (require_whole_section(live, sup, state_fields, err, errsz) != 0)
		return -1;
	if (require_unchanged(live, sup, "auth_token_set",
	                      "is a redaction marker: this document never carried the token, so "
	                      "it cannot set one -- use PUT /v1/pkg/repo-config", err, errsz) != 0)
		return -1;
	if (need_string(sup, "repo_url", &url, err, errsz) != 0 ||
	    need_string(sup, "repo_kind", &kind, err, errsz) != 0 ||
	    need_string(sup, "ref", &ref, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	/* NULL for the token: the document never carried it, and passing
	 * anything else here would clear a working one. */
	switch (pkg_repo_set_config(url, kind, ref, NULL)) {
	case PKG_OK:
		return 0;
	case PKG_ERR_INVALID_NAME:
		snprintf(err, errsz, "\"repo_kind\" must be gitea, github or gitlab");
		return -1;
	default:
		snprintf(err, errsz, "the repository configuration could not be persisted");
		return -1;
	}
}

static int cfg_apply_package_artifacts(const struct json_value *live, const struct json_value *sup,
                                       const char *state_fields, int dry_run, char *err,
                                       size_t errsz)
{
	const char *base_url;
	int push_enabled;

	if (require_whole_section(live, sup, state_fields, err, errsz) != 0)
		return -1;
	if (require_unchanged(live, sup, "auth_token_set",
	                      "is a redaction marker: this document never carried the token, so it "
	                      "cannot set one -- use PUT /v1/pkg/artifact-config", err, errsz) != 0)
		return -1;
	if (need_string(sup, "base_url", &base_url, err, errsz) != 0 ||
	    need_bool(sup, "push_enabled", &push_enabled, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (pkg_artifact_set_config(base_url, NULL, &push_enabled) != PKG_OK) {
		snprintf(err, errsz, "the artifact-server configuration was refused");
		return -1;
	}
	return 0;
}

/*
 * The one `replace` section that is an array rather than an object: an
 * ordered list of addresses with a single setter behind it, which is
 * what `replace` means -- the object/array distinction is about shape,
 * not about how a section is written.
 */
static int cfg_apply_dns_forwarders(const struct json_value *live, const struct json_value *sup,
                                    const char *state_fields, int dry_run, char *err,
                                    size_t errsz)
{
	char list[DNS_FORWARDERS_MAX][DNS_FORWARDER_LEN];
	const char *ptrs[DNS_FORWARDERS_MAX];
	int count = 0;
	int i;

	(void)live;
	(void)state_fields; /* a list of addresses has no observed members */
	if (need_string_list(sup, ptrs, DNS_FORWARDERS_MAX, &count, "dns_forwarders", err, errsz) !=
	    0)
		return -1;
	for (i = 0; i < count; i++) {
		if (strlen(ptrs[i]) >= DNS_FORWARDER_LEN) {
			snprintf(err, errsz, "dns_forwarders[%d] is too long", i);
			return -1;
		}
	}
	if (dry_run)
		return 0;
	memset(list, 0, sizeof(list));
	for (i = 0; i < count; i++)
		snprintf(list[i], DNS_FORWARDER_LEN, "%s", ptrs[i]);
	/*
	 * One message for the one failure: read before writing it,
	 * dns_forwarders_set() returns non-OK only for a count out of
	 * range or an address inet_pton() rejects (both spelled
	 * DNS_SERVER_ERR_INVALID_PATH, reusing an existing code), and the
	 * count is already bounded above. It does not report a persist
	 * failure at all. So naming the address is accurate here, where
	 * for the other setters a single message would not have been.
	 */
	if (dns_forwarders_set(list, count) != DNS_SERVER_OK) {
		snprintf(err, errsz, "a forwarder is not a dotted-quad IPv4 address");
		return -1;
	}
	return 0;
}


/* ---- reconcile: sections applied element by element ----
 *
 * ONE walk, and per section only the three things that differ: what
 * creating, updating and deleting ONE element means. Writing the walk
 * eleven times would be eleven chances to order it differently.
 *
 * WHAT THE DOCUMENT NAMES IS CREATED OR UPDATED; WHAT IT DOES NOT NAME
 * IS REMOVED. That is what `replace` already means for the sections a
 * single setter owns -- sending a shorter `resolver.nameservers` list
 * removes one -- and having element-wise sections mean something
 * different would make one endpoint speak two languages. The safety is
 * that `POST /v1/config/diff` shows every removal before anything runs,
 * not that removals are quietly skipped.
 *
 * Elements are paired by the identity key the schema declares, through
 * jsondiff_element_name(), and compared with jsondiff_equal_ignoring()
 * -- both the differ's own functions, so the pairing an operator saw in
 * the plan is the pairing that happens here.
 *
 * Creates and updates run BEFORE removals. A document that renames an
 * element is a removal and an addition; doing the addition first means
 * the service it describes is never absent in between.
 *
 * `dry_run` runs the same code with every mutating call skipped, the
 * same discipline the replace appliers use -- so what the plan reports
 * as appliable is what this function will attempt.
 */

typedef int (*cfg_elem_fn)(const struct json_value *el, int dry_run, char *err, size_t errsz);
typedef int (*cfg_elem_upd)(const struct json_value *live_el, const struct json_value *sup_el,
                            int dry_run, char *err, size_t errsz);

struct cfg_elem_ops {
	cfg_elem_fn create;
	/* NULL when the subsystem has no in-place update: a changed
	 * element is then removed and recreated, which is visible in the
	 * plan as exactly that. */
	cfg_elem_upd update;
	cfg_elem_fn remove;
};

static const struct json_value *find_named(const struct json_value *arr, const char *key,
                                           const char *name)
{
	char other[JSONDIFF_PATH_MAX];
	size_t i;

	for (i = 0; i < arr->u.array.count; i++) {
		if (jsondiff_element_name(arr->u.array.items[i], key, other, sizeof(other)) != 0)
			continue;
		if (strcmp(other, name) == 0)
			return arr->u.array.items[i];
	}
	return NULL;
}

static int reconcile_list(const struct json_value *live, const struct json_value *sup,
                          const char *key, const char *state_fields,
                          const struct cfg_elem_ops *ops, int dry_run, char *err, size_t errsz)
{
	char name[JSONDIFF_PATH_MAX];
	char other[JSONDIFF_PATH_MAX];
	size_t i, j;

	if (sup->type != JSON_ARRAY) {
		snprintf(err, errsz, "this section is a list");
		return -1;
	}
	/*
	 * Every element must be nameable, and no two may share a name --
	 * checked before anything runs, because a document that names the
	 * same element twice says two things about it and there is no
	 * defensible order in which to do both.
	 */
	for (i = 0; i < sup->u.array.count; i++) {
		if (jsondiff_element_name(sup->u.array.items[i], key, name, sizeof(name)) != 0) {
			snprintf(err, errsz, "element %d has no usable \"%s\"", (int)i, key);
			return -1;
		}
		for (j = 0; j < i; j++) {
			if (jsondiff_element_name(sup->u.array.items[j], key, other,
			                          sizeof(other)) == 0 &&
			    strcmp(name, other) == 0) {
				snprintf(err, errsz, "\"%s\" appears twice", name);
				return -1;
			}
		}
	}

	for (i = 0; i < sup->u.array.count; i++) {
		const struct json_value *el = sup->u.array.items[i];
		const struct json_value *cur;
		int same;

		(void)jsondiff_element_name(el, key, name, sizeof(name));
		cur = find_named(live, key, name);
		if (cur == NULL) {
			if (ops->create(el, dry_run, err, errsz) != 0)
				return -1;
			continue;
		}
		same = jsondiff_equal_ignoring(cur, el, state_fields);
		if (same < 0) {
			snprintf(err, errsz, "out of memory comparing \"%s\"", name);
			return -1;
		}
		if (same)
			continue;
		if (ops->update != NULL) {
			if (ops->update(cur, el, dry_run, err, errsz) != 0)
				return -1;
			continue;
		}
		if (ops->remove(cur, dry_run, err, errsz) != 0)
			return -1;
		if (ops->create(el, dry_run, err, errsz) != 0)
			return -1;
	}

	for (i = 0; i < live->u.array.count; i++) {
		const struct json_value *el = live->u.array.items[i];

		if (jsondiff_element_name(el, key, name, sizeof(name)) != 0)
			continue; /* nothing live that cannot be named can be matched */
		if (find_named(sup, key, name) != NULL)
			continue;
		if (ops->remove(el, dry_run, err, errsz) != 0)
			return -1;
	}
	return 0;
}

/* --- the per-section element operations --- */

/*
 * The five "this container provides this service" registrations. An
 * element is the container's name and nothing else the operator
 * chooses, so there is no update: a difference can only be in an
 * observed member, which is never compared.
 */
#define CFG_REGISTRATION_OPS(section, field, reg_call, unreg_call, what)                        \
	static int cfg_##section##_create(const struct json_value *el, int dry_run, char *err,      \
	                                  size_t errsz)                                             \
	{                                                                                           \
		const char *name;                                                                      \
                                                                                                \
		if (need_string(el, field, &name, err, errsz) != 0)                                    \
			return -1;                                                                         \
		if (dry_run)                                                                           \
			return 0;                                                                          \
		if (reg_call != 0) {                                                                   \
			snprintf(err, errsz, "could not register %s as " what, name);                      \
			return -1;                                                                         \
		}                                                                                       \
		return 0;                                                                               \
	}                                                                                           \
	static int cfg_##section##_remove(const struct json_value *el, int dry_run, char *err,      \
	                                  size_t errsz)                                             \
	{                                                                                           \
		const char *name;                                                                      \
                                                                                                \
		if (need_string(el, field, &name, err, errsz) != 0)                                    \
			return -1;                                                                         \
		if (dry_run)                                                                           \
			return 0;                                                                          \
		if (unreg_call != 0) {                                                                 \
			snprintf(err, errsz, "could not unregister %s as " what, name);                    \
			return -1;                                                                         \
		}                                                                                       \
		return 0;                                                                               \
	}

CFG_REGISTRATION_OPS(ntp_servers, "container", ntp_server_register(name),
                     ntp_server_unregister(name), "an NTP server")
CFG_REGISTRATION_OPS(syslog_targets, "container", syslogfwd_target_register(name),
                     syslogfwd_target_unregister(name), "a syslog target")
CFG_REGISTRATION_OPS(dhcp_servers, "container", dhcp_server_register(name),
                     dhcp_server_unregister(name), "a DHCP server")

static const struct cfg_elem_ops cfg_ops_ntp_servers = { cfg_ntp_servers_create, NULL,
                                                         cfg_ntp_servers_remove };
static const struct cfg_elem_ops cfg_ops_syslog_targets = { cfg_syslog_targets_create, NULL,
                                                            cfg_syslog_targets_remove };
static const struct cfg_elem_ops cfg_ops_dhcp_servers = { cfg_dhcp_servers_create, NULL,
                                                          cfg_dhcp_servers_remove };

/* An LDAP server carries the path to the config it serves, so it has a
 * settable member and a difference is real. No in-place update exists,
 * so a changed one is re-registered. */
static int cfg_ldap_servers_create(const struct json_value *el, int dry_run, char *err,
                                   size_t errsz)
{
	const char *name, *config_path;

	if (need_string(el, "container", &name, err, errsz) != 0 ||
	    need_string(el, "config_path", &config_path, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (ldap_server_register(name, config_path) != LDAP_SERVER_OK) {
		snprintf(err, errsz, "could not register %s as an LDAP server", name);
		return -1;
	}
	return 0;
}

static int cfg_ldap_servers_remove(const struct json_value *el, int dry_run, char *err,
                                   size_t errsz)
{
	const char *name;

	if (need_string(el, "container", &name, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (ldap_server_unregister(name) != LDAP_SERVER_OK) {
		snprintf(err, errsz, "could not unregister the LDAP server %s", name);
		return -1;
	}
	return 0;
}

static const struct cfg_elem_ops cfg_ops_ldap_servers = { cfg_ldap_servers_create, NULL,
                                                          cfg_ldap_servers_remove };

/*
 * A DNS server registration binds to the container's RUNNING process
 * (the daemon writes the hosts file through /proc/<pid>/root and
 * signals it), so this resolves the same pid/pidfd the REST handler
 * does and refuses a container that is not running -- the same refusal,
 * for the same reason.
 */
static int cfg_dns_servers_create(const struct json_value *el, int dry_run, char *err,
                                  size_t errsz)
{
	struct registry_entry *entry;
	const char *name, *hosts_path;

	if (need_string(el, "container", &name, err, errsz) != 0 ||
	    need_string(el, "hosts_path", &hosts_path, err, errsz) != 0)
		return -1;
	entry = registry_find(name);
	if (entry == NULL || !entry->running) {
		snprintf(err, errsz, "%s is not a running container", name);
		return -1;
	}
	if (dry_run)
		return 0;
	if (dns_server_register(name, entry->handle.pid, entry->handle.pidfd, hosts_path) !=
	    DNS_SERVER_OK) {
		snprintf(err, errsz, "could not register %s as a DNS server", name);
		return -1;
	}
	return 0;
}

static int cfg_dns_servers_remove(const struct json_value *el, int dry_run, char *err,
                                  size_t errsz)
{
	const char *name;

	if (need_string(el, "container", &name, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (dns_server_unregister(name) != DNS_SERVER_OK) {
		snprintf(err, errsz, "could not unregister the DNS server %s", name);
		return -1;
	}
	return 0;
}

static const struct cfg_elem_ops cfg_ops_dns_servers = { cfg_dns_servers_create, NULL,
                                                         cfg_dns_servers_remove };

/* A persisted kernel parameter: setting is the same call for a new one
 * and a changed one. */
/*
 * A sysctl value renders as a STRING when it is one token and as an
 * ARRAY when it is a tuple -- `sysctl_value_write_json()` splits on
 * whitespace, so a real tuple-shaped parameter like
 * net.ipv4.tcp_wmem comes back as three strings. Both shapes have to
 * be accepted here or a document fetched from this very host would be
 * refused for a sysctl nobody edited. Joined back with single spaces,
 * which is what the subsystem stores.
 */
static int sysctl_value_text(const struct json_value *el, char *out, size_t out_size, char *err,
                             size_t errsz)
{
	const struct json_value *v = json_object_get(el, "value");
	size_t used = 0;
	size_t i;

	if (v != NULL && v->type == JSON_STRING) {
		if (snprintf(out, out_size, "%s", v->u.string) >= (int)out_size) {
			snprintf(err, errsz, "\"value\" is too long");
			return -1;
		}
		return 0;
	}
	if (v == NULL || v->type != JSON_ARRAY) {
		snprintf(err, errsz, "\"value\" must be a string, or an array of them for a "
		                     "tuple-shaped parameter");
		return -1;
	}
	out[0] = '\0';
	for (i = 0; i < v->u.array.count; i++) {
		int written;

		if (v->u.array.items[i]->type != JSON_STRING) {
			snprintf(err, errsz, "\"value\" must hold strings");
			return -1;
		}
		written = snprintf(out + used, out_size - used, "%s%s", used > 0 ? " " : "",
		                   v->u.array.items[i]->u.string);
		if (written < 0 || (size_t)written >= out_size - used) {
			snprintf(err, errsz, "\"value\" is too long");
			return -1;
		}
		used += (size_t)written;
	}
	return 0;
}

static int cfg_sysctl_create(const struct json_value *el, int dry_run, char *err, size_t errsz)
{
	char value[SYSCTL_VALUE_MAX];
	const char *key;

	if (need_string(el, "key", &key, err, errsz) != 0 ||
	    sysctl_value_text(el, value, sizeof(value), err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (sysctlconfig_set(key, value) != SYSCTLCONFIG_OK) {
		snprintf(err, errsz, "%s could not be set to \"%s\"", key, value);
		return -1;
	}
	return 0;
}

static int cfg_sysctl_update(const struct json_value *live_el, const struct json_value *sup_el,
                             int dry_run, char *err, size_t errsz)
{
	(void)live_el;
	return cfg_sysctl_create(sup_el, dry_run, err, errsz);
}

static int cfg_sysctl_remove(const struct json_value *el, int dry_run, char *err, size_t errsz)
{
	const char *key;

	if (need_string(el, "key", &key, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (sysctlconfig_delete(key) != SYSCTLCONFIG_OK) {
		snprintf(err, errsz, "%s could not be removed", key);
		return -1;
	}
	return 0;
}

static const struct cfg_elem_ops cfg_ops_sysctl = { cfg_sysctl_create, cfg_sysctl_update,
                                                    cfg_sysctl_remove };

/*
 * `default_options` renders as an OBJECT of parameter=value pairs
 * (`kmod_options_write_json()` splits the stored string on whitespace
 * and again on '='), so it has to be folded back into the "k=v k=v"
 * form the subsystem stores. Reading the renderer rather than assuming
 * a string is what caught this: a document fetched from this host
 * would otherwise have been refused for a module nobody edited.
 */
static int kmod_options_text(const struct json_value *el, char *out, size_t out_size, char *err,
                             size_t errsz)
{
	const struct json_value *v = json_object_get(el, "default_options");
	size_t used = 0;
	size_t i;

	out[0] = '\0';
	if (v == NULL || v->type != JSON_OBJECT) {
		snprintf(err, errsz, "\"default_options\" must be an object of parameter/value "
		                     "pairs");
		return -1;
	}
	for (i = 0; i < v->u.object.count; i++) {
		const char *val = json_as_string(v->u.object.values[i]);
		int written;

		if (val == NULL) {
			snprintf(err, errsz, "\"default_options.%s\" must be a string",
			         v->u.object.keys[i]);
			return -1;
		}
		written = snprintf(out + used, out_size - used, "%s%s=%s", used > 0 ? " " : "",
		                   v->u.object.keys[i], val);
		if (written < 0 || (size_t)written >= out_size - used) {
			snprintf(err, errsz, "\"default_options\" is too long");
			return -1;
		}
		used += (size_t)written;
	}
	return 0;
}

static int cfg_kernel_modules_create(const struct json_value *el, int dry_run, char *err,
                                     size_t errsz)
{
	char options[KMOD_OPTIONS_MAX];
	const struct json_value *jauto;
	const char *name;
	int autoload = 0;

	if (need_string(el, "name", &name, err, errsz) != 0 ||
	    kmod_options_text(el, options, sizeof(options), err, errsz) != 0)
		return -1;
	jauto = json_object_get(el, "autoload");
	if (jauto == NULL || jauto->type != JSON_BOOL) {
		snprintf(err, errsz, "\"autoload\" must be true or false");
		return -1;
	}
	autoload = jauto->u.boolean;
	if (dry_run)
		return 0;
	if (kmodconfig_set(name, options, 1, autoload) != KMODCONFIG_OK) {
		snprintf(err, errsz, "the configuration for module %s was refused", name);
		return -1;
	}
	return 0;
}

static int cfg_kernel_modules_update(const struct json_value *live_el,
                                     const struct json_value *sup_el, int dry_run, char *err,
                                     size_t errsz)
{
	(void)live_el;
	return cfg_kernel_modules_create(sup_el, dry_run, err, errsz);
}

static int cfg_kernel_modules_remove(const struct json_value *el, int dry_run, char *err,
                                     size_t errsz)
{
	const char *name;

	if (need_string(el, "name", &name, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (kmodconfig_delete(name) != KMODCONFIG_OK) {
		snprintf(err, errsz, "the configuration for module %s could not be removed", name);
		return -1;
	}
	return 0;
}

static const struct cfg_elem_ops cfg_ops_kernel_modules = { cfg_kernel_modules_create,
                                                            cfg_kernel_modules_update,
                                                            cfg_kernel_modules_remove };

static int cfg_package_policies_create(const struct json_value *el, int dry_run, char *err,
                                       size_t errsz)
{
	enum pkg_policy_kind kind;
	const char *name, *policy, *version;

	if (need_string(el, "name", &name, err, errsz) != 0 ||
	    need_string(el, "policy", &policy, err, errsz) != 0 ||
	    need_string_or_null(el, "version", &version, err, errsz) != 0)
		return -1;
	if (!pkgpolicy_kind_parse(policy, &kind)) {
		snprintf(err, errsz, "\"%s\" is not a package policy", policy);
		return -1;
	}
	if (dry_run)
		return 0;
	if (pkgpolicy_set(name, kind, version) != 0) {
		snprintf(err, errsz, "the policy for %s was refused", name);
		return -1;
	}
	return 0;
}

static int cfg_package_policies_update(const struct json_value *live_el,
                                       const struct json_value *sup_el, int dry_run, char *err,
                                       size_t errsz)
{
	(void)live_el;
	return cfg_package_policies_create(sup_el, dry_run, err, errsz);
}

static int cfg_package_policies_remove(const struct json_value *el, int dry_run, char *err,
                                       size_t errsz)
{
	const char *name;

	if (need_string(el, "name", &name, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (pkgpolicy_clear(name) != 0) {
		snprintf(err, errsz, "the policy for %s could not be removed", name);
		return -1;
	}
	return 0;
}

static const struct cfg_elem_ops cfg_ops_package_policies = { cfg_package_policies_create,
                                                              cfg_package_policies_update,
                                                              cfg_package_policies_remove };

static int cfg_disk_roles_create(const struct json_value *el, int dry_run, char *err, size_t errsz)
{
	const char *disk, *role;

	if (need_string(el, "disk_name", &disk, err, errsz) != 0 ||
	    need_string(el, "role", &role, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (diskrole_create(disk, role, g_containers_dir) != DISKROLE_OK) {
		snprintf(err, errsz, "the role \"%s\" was refused for %s", role, disk);
		return -1;
	}
	return 0;
}

static int cfg_disk_roles_remove(const struct json_value *el, int dry_run, char *err, size_t errsz)
{
	const char *disk;

	if (need_string(el, "disk_name", &disk, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (diskrole_delete(disk) != DISKROLE_OK) {
		snprintf(err, errsz, "the role assignment for %s could not be removed", disk);
		return -1;
	}
	return 0;
}

static const struct cfg_elem_ops cfg_ops_disk_roles = { cfg_disk_roles_create, NULL,
                                                        cfg_disk_roles_remove };

static int record_ip(const struct json_value *el, uint32_t *out, char *err, size_t errsz)
{
	struct in_addr a;
	const char *ip;

	if (need_string(el, "ip", &ip, err, errsz) != 0)
		return -1;
	if (inet_pton(AF_INET, ip, &a) != 1) {
		snprintf(err, errsz, "\"%s\" is not a dotted-quad IPv4 address", ip);
		return -1;
	}
	*out = (uint32_t)a.s_addr;
	return 0;
}

/*
 * `owner` is observed -- it records which container's lifecycle a
 * record is tied to, which only the creating container can establish.
 * A record created from this document belongs to the operator, so the
 * owner is NULL rather than anything copied out of the document.
 */
static int cfg_dns_records_create(const struct json_value *el, int dry_run, char *err,
                                  size_t errsz)
{
	const char *name;
	uint32_t ip;

	if (need_string(el, "name", &name, err, errsz) != 0 ||
	    record_ip(el, &ip, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (dns_record_create(name, ip, NULL, NULL) != DNS_OK) {
		snprintf(err, errsz, "the record %s was refused", name);
		return -1;
	}
	return 0;
}

static int cfg_dns_records_update(const struct json_value *live_el,
                                  const struct json_value *sup_el, int dry_run, char *err,
                                  size_t errsz)
{
	const char *name;
	uint32_t ip;

	(void)live_el;
	if (need_string(sup_el, "name", &name, err, errsz) != 0 ||
	    record_ip(sup_el, &ip, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (dns_record_update(name, ip, NULL) != DNS_OK) {
		snprintf(err, errsz, "the record %s could not be updated", name);
		return -1;
	}
	return 0;
}

static int cfg_dns_records_remove(const struct json_value *el, int dry_run, char *err,
                                  size_t errsz)
{
	const char *name;

	if (need_string(el, "name", &name, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (dns_record_delete(name) != DNS_OK) {
		snprintf(err, errsz, "the record %s could not be removed", name);
		return -1;
	}
	return 0;
}

static const struct cfg_elem_ops cfg_ops_dns_records = { cfg_dns_records_create,
                                                         cfg_dns_records_update,
                                                         cfg_dns_records_remove };

static int cfg_ldap_groups_create(const struct json_value *el, int dry_run, char *err,
                                  size_t errsz)
{
	const char *name;
	long long gid;

	if (need_string(el, "name", &name, err, errsz) != 0 ||
	    need_int(el, "gidnumber", &gid, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (ldap_group_create(name, (int)gid, NULL) != LDAP_RECORD_OK) {
		snprintf(err, errsz, "the group %s was refused", name);
		return -1;
	}
	return 0;
}

static int cfg_ldap_groups_update(const struct json_value *live_el,
                                  const struct json_value *sup_el, int dry_run, char *err,
                                  size_t errsz)
{
	const char *name;
	long long gid;

	(void)live_el;
	if (need_string(sup_el, "name", &name, err, errsz) != 0 ||
	    need_int(sup_el, "gidnumber", &gid, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	/* The name is the identity this element was matched by, so it is
	 * never the thing being changed here -- a renamed group is an
	 * addition and a removal, which is what the plan shows. */
	if (ldap_group_update(name, name, (int)gid, NULL) != LDAP_RECORD_OK) {
		snprintf(err, errsz, "the group %s could not be updated", name);
		return -1;
	}
	return 0;
}

static int cfg_ldap_groups_remove(const struct json_value *el, int dry_run, char *err,
                                  size_t errsz)
{
	const char *name;

	if (need_string(el, "name", &name, err, errsz) != 0)
		return -1;
	if (dry_run)
		return 0;
	if (ldap_group_delete(name) != LDAP_RECORD_OK) {
		snprintf(err, errsz, "the group %s could not be removed", name);
		return -1;
	}
	return 0;
}

static const struct cfg_elem_ops cfg_ops_ldap_groups = { cfg_ldap_groups_create,
                                                         cfg_ldap_groups_update,
                                                         cfg_ldap_groups_remove };

/*
 * The table, generated. Same guard the replace appliers have: a
 * section the schema calls `reconcile` with no operations here does
 * not compile, and operations for a section that is no longer
 * `reconcile` are an unused static, which -Werror rejects.
 */
struct config_reconciler {
	const char *name;
	const struct cfg_elem_ops *ops;
};

#define X(name) { #name, &cfg_ops_##name },
static const struct config_reconciler g_reconcilers[] = { CIX_CONFIG_SECTIONS_RECONCILE(X) };
#undef X

static const struct cfg_elem_ops *reconciler_for(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(g_reconcilers) / sizeof(g_reconcilers[0]); i++) {
		if (strcmp(g_reconcilers[i].name, name) == 0)
			return g_reconcilers[i].ops;
	}
	return NULL;
}

struct config_applier {
	const char *name;
	int (*fn)(const struct json_value *live, const struct json_value *sup,
	          const char *state_fields, int dry_run, char *err, size_t errsz);
};

#define X(name) { #name, cfg_apply_##name },
static const struct config_applier g_appliers[] = { CIX_CONFIG_SECTIONS_REPLACE(X) };
#undef X

static const struct config_applier *applier_for(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(g_appliers) / sizeof(g_appliers[0]); i++) {
		if (strcmp(g_appliers[i].name, name) == 0)
			return &g_appliers[i];
	}
	return NULL;
}

/* ---- the plan ---- */

struct section_plan {
	int index;                        /* into the generated section table */
	struct json_value *live;          /* owned */
	const struct json_value *sup;     /* borrowed from the request body */
	struct jsondiff diff;
	int appliable;
	char reason[CFG_ERR_MAX];
	int applied;
	char error[CFG_ERR_MAX];
};

static void plans_free(struct section_plan *plans, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		json_free(plans[i].live);
		jsondiff_free(&plans[i].diff);
	}
	free(plans);
}

static struct json_value *render_live(int index, const char *containers_dir)
{
	struct json_writer w;
	struct json_value *v;

	jw_init(&w);
	config_render_section(index, containers_dir, &w);
	v = json_parse(w.buf, w.len);
	jw_free(&w);
	return v;
}

/*
 * Builds one plan entry per supplied section, in schema order rather
 * than body order -- a section never depends on one below it, so
 * applying them in the order the document defines is the only order
 * that is meaningful.
 */
static int build_plan(const struct json_value *doc, const char *containers_dir,
                      struct section_plan **out, int *out_n, char *err, size_t errsz)
{
	struct section_plan *plans;
	size_t i;
	int n = 0;
	int idx;

	if (doc->type != JSON_OBJECT) {
		snprintf(err, errsz, "the body must be a configuration document -- a JSON object "
		                     "whose keys are section names");
		return -1;
	}
	for (i = 0; i < doc->u.object.count; i++) {
		if (config_section_index(doc->u.object.keys[i]) < 0) {
			snprintf(err, errsz, "\"%s\" is not a configuration section",
			         doc->u.object.keys[i]);
			return -1;
		}
	}
	g_containers_dir = containers_dir;
	plans = calloc((size_t)config_section_count(), sizeof(*plans));
	if (plans == NULL) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	for (idx = 0; idx < config_section_count(); idx++) {
		const struct json_value *sup = json_object_get(doc, config_section_name(idx));
		struct section_plan *p;
		const struct config_applier *ap;

		if (sup == NULL)
			continue;
		p = &plans[n];
		p->index = idx;
		p->sup = sup;
		p->live = render_live(idx, containers_dir);
		if (p->live == NULL) {
			snprintf(err, errsz, "section \"%s\" could not be rendered",
			         config_section_name(idx));
			plans_free(plans, n);
			return -1;
		}
		n++;
		if (jsondiff_compute(p->live, sup, config_section_key(idx),
		                     config_section_state_fields(idx), &p->diff) != 0) {
			snprintf(err, errsz, "out of memory computing the difference for \"%s\"",
			         config_section_name(idx));
			plans_free(plans, n);
			return -1;
		}
		if (p->diff.count == 0) {
			p->appliable = 1;
			continue;
		}
		if (config_section_apply_mode(idx) == CONFIG_APPLY_MANUAL) {
			/*
			 * One sentence, because the reason differs per section
			 * and belongs where it can be argued: removing an
			 * element destroys state this document cannot describe
			 * well enough to recreate (`volumes`, `containers`),
			 * or the section is an observation with no setter
			 * (`routes`), or the thing already has a better
			 * distribution mechanism than this one
			 * (`image_recipes`, `container_recipes`, which
			 * `pkg sync` brings from git). The message used to
			 * assert the first two reasons for all eleven, which
			 * was simply untrue for the recipe sections.
			 */
			snprintf(p->reason, sizeof(p->reason),
			         "\"%s\" is changed through its own endpoints, not through this "
			         "document -- see ADR-0292 for why this section in particular",
			         config_section_name(idx));
			continue;
		}
		if (config_section_apply_mode(idx) == CONFIG_APPLY_RECONCILE) {
			const struct cfg_elem_ops *ops = reconciler_for(config_section_name(idx));

			/*
			 * Both come from the same schema pass, so this cannot
			 * disagree -- except it did: a generator edit silently
			 * failed to apply, every non-replace section was
			 * emitted as `reconcile`, and the eleven with no
			 * operations reached this call with a NULL to
			 * dereference. Caught before it ran on a real host, and
			 * the lesson is that "cannot happen by construction" is
			 * worth one branch when the alternative is a segfault
			 * in the control plane.
			 */
			if (ops == NULL) {
				snprintf(p->reason, sizeof(p->reason),
				         "\"%s\" has no element operations -- the generated section "
				         "table and the generated reconcile list disagree, which is a "
				         "build fault, not something this document did",
				         config_section_name(idx));
				continue;
			}
			if (reconcile_list(p->live, sup, config_section_key(idx),
			                   config_section_state_fields(idx), ops, 1, p->reason,
			                   sizeof(p->reason)) != 0)
				continue;
			p->appliable = 1;
			continue;
		}
		ap = applier_for(config_section_name(idx));
		if (ap->fn(p->live, sup, config_section_state_fields(idx), 1, p->reason,
		           sizeof(p->reason)) != 0)
			continue;
		p->appliable = 1;
	}
	*out = plans;
	*out_n = n;
	return 0;
}

static void write_change(struct json_writer *w, const struct jsondiff_change *c)
{
	jw_obj_open(w);
	jw_key(w, "path");
	jw_str(w, c->path[0] == '\0' ? "(section)" : c->path);
	jw_key(w, "op");
	jw_str(w, jsondiff_op_name(c->op));
	if (c->from != NULL) {
		jw_key(w, "from");
		jw_value(w, c->from);
	}
	if (c->to != NULL) {
		jw_key(w, "to");
		jw_value(w, c->to);
	}
	jw_obj_close(w);
}

static void write_plan(struct json_writer *w, const struct section_plan *plans, int n, int applying,
                       int applied_ok)
{
	int changed_count = 0, appliable_count = 0, blocked_count = 0;
	int i, j;

	for (i = 0; i < n; i++) {
		if (plans[i].diff.count == 0)
			continue;
		changed_count++;
		if (plans[i].appliable)
			appliable_count++;
		else
			blocked_count++;
	}

	jw_obj_open(w);
	jw_key(w, "sections");
	jw_arr_open(w);
	for (i = 0; i < n; i++) {
		const struct section_plan *p = &plans[i];

		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, config_section_name(p->index));
		jw_key(w, "kind");
		jw_str(w, config_section_kind(p->index) == CONFIG_KIND_ARRAY ? "array" : "object");
		jw_key(w, "apply");
		jw_str(w, config_section_apply_mode(p->index) == CONFIG_APPLY_REPLACE ? "replace"
		       : config_section_apply_mode(p->index) == CONFIG_APPLY_RECONCILE ? "reconcile"
		                                                                       : "manual");
		jw_key(w, "status");
		jw_str(w, p->diff.count == 0 ? "unchanged" : "changed");
		jw_key(w, "appliable");
		jw_bool(w, p->appliable);
		if (p->reason[0] != '\0') {
			jw_key(w, "reason");
			jw_str(w, p->reason);
		}
		if (config_section_state_fields(p->index)[0] != '\0') {
			/* Named so "no changes" is never mistaken for "nothing
			 * about this section moved" -- these members were not
			 * compared at all. */
			jw_key(w, "observed_fields");
			jw_str(w, config_section_state_fields(p->index));
		}
		if (p->diff.truncated) {
			jw_key(w, "truncated");
			jw_bool(w, 1);
		}
		if (p->diff.unkeyed_path[0] != '\0') {
			/* Said out loud because the diff below is then
			 * positional, and a positional diff of a list that was
			 * reordered reads as a change to everything. */
			jw_key(w, "compared_by_position");
			jw_str(w, p->diff.unkeyed_path);
		}
		if (applying) {
			jw_key(w, "applied");
			jw_bool(w, p->applied);
			if (p->error[0] != '\0') {
				jw_key(w, "error");
				jw_str(w, p->error);
			}
		}
		jw_key(w, "changes");
		jw_arr_open(w);
		for (j = 0; j < p->diff.count; j++)
			write_change(w, &p->diff.changes[j]);
		jw_arr_close(w);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_key(w, "summary");
	jw_obj_open(w);
	jw_key(w, "sections_supplied");
	jw_int(w, n);
	jw_key(w, "sections_changed");
	jw_int(w, changed_count);
	jw_key(w, "appliable");
	jw_int(w, appliable_count);
	jw_key(w, "blocked");
	jw_int(w, blocked_count);
	if (applying) {
		jw_key(w, "applied");
		jw_bool(w, applied_ok);
	}
	jw_obj_close(w);
	jw_obj_close(w);
}

static struct json_value *parse_body(int fd, const char *body, size_t body_len)
{
	struct json_value *doc;

	if (body == NULL || body_len == 0) {
		respond_error(fd, 400, "Bad Request",
		              "the body must be a configuration document, the same shape GET "
		              "/v1/config returns");
		return NULL;
	}
	doc = json_parse(body, body_len);
	if (doc == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return NULL;
	}
	return doc;
}

void handle_config_diff(int fd, const char *containers_dir, const char *body, size_t body_len)
{
	struct section_plan *plans = NULL;
	struct json_writer w;
	struct json_value *doc;
	char err[CFG_ERR_MAX];
	int n = 0;

	doc = parse_body(fd, body, body_len);
	if (doc == NULL)
		return;
	if (build_plan(doc, containers_dir, &plans, &n, err, sizeof(err)) != 0) {
		respond_error(fd, 400, "Bad Request", err);
		json_free(doc);
		return;
	}
	jw_init(&w);
	write_plan(&w, plans, n, 0, 0);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
	plans_free(plans, n);
	json_free(doc);
}

void handle_config_apply(int fd, const char *containers_dir, const char *body, size_t body_len)
{
	struct section_plan *plans = NULL;
	struct json_writer w;
	struct json_value *doc;
	char err[CFG_ERR_MAX];
	int n = 0;
	int i;
	int blocked = 0;
	int applied_ok = 1;

	doc = parse_body(fd, body, body_len);
	if (doc == NULL)
		return;
	if (build_plan(doc, containers_dir, &plans, &n, err, sizeof(err)) != 0) {
		respond_error(fd, 400, "Bad Request", err);
		json_free(doc);
		return;
	}
	for (i = 0; i < n; i++) {
		if (plans[i].diff.count != 0 && !plans[i].appliable)
			blocked = 1;
	}
	if (blocked) {
		/*
		 * Nothing has been touched. The plan goes back with the
		 * refusal so the reason is in the same response as the
		 * evidence for it, rather than an operator having to ask the
		 * diff endpoint what the apply meant.
		 */
		jw_init(&w);
		write_plan(&w, plans, n, 1, 0);
		respond_json(fd, 400, "Bad Request", &w);
		jw_free(&w);
		plans_free(plans, n);
		json_free(doc);
		return;
	}
	for (i = 0; i < n; i++) {
		struct section_plan *p = &plans[i];
		const struct config_applier *ap;

		if (p->diff.count == 0)
			continue;
		if (config_section_apply_mode(p->index) == CONFIG_APPLY_RECONCILE) {
			const struct cfg_elem_ops *ops = reconciler_for(config_section_name(p->index));

			/* Unreachable: build_plan() refuses such a section
			 * above, so an apply never gets here with one. */
			if (ops == NULL) {
				snprintf(p->error, sizeof(p->error),
				         "\"%s\" has no element operations",
				         config_section_name(p->index));
				applied_ok = 0;
				break;
			}
			if (reconcile_list(p->live, p->sup, config_section_key(p->index),
			                   config_section_state_fields(p->index), ops, 0,
			                   p->error, sizeof(p->error)) != 0) {
				applied_ok = 0;
				break;
			}
			p->applied = 1;
			continue;
		}
		ap = applier_for(config_section_name(p->index));
		if (ap->fn(p->live, p->sup, config_section_state_fields(p->index), 0, p->error,
		           sizeof(p->error)) != 0) {
			applied_ok = 0;
			break;
		}
		p->applied = 1;
	}
	jw_init(&w);
	write_plan(&w, plans, n, 1, applied_ok);
	respond_json(fd, applied_ok ? 200 : 500, applied_ok ? "OK" : "Internal Server Error", &w);
	jw_free(&w);
	plans_free(plans, n);
	json_free(doc);
}
