#ifndef SITECONFIG_H
#define SITECONFIG_H

#include "json.h"

/*
 * A minimal, real, operator-configurable identity for this
 * install/site -- raised directly by the user rejecting a hardcoded
 * ".internal" default: "this should be configurable... this has an
 * effect on both DNS and PKI." Two fields: site_name (an operator-
 * chosen label, e.g. "lab1" -- empty is valid, meaning "no site
 * tier," a single-site deployment) and domain_suffix (the private TLD
 * this site's names live under; defaults to "internal" -- IANA-
 * reserved for exactly this use, RFC 9476 -- but is a real, settable
 * value, not a compile-time constant).
 *
 * Deliberately a *convenience*, not enforcement (see ADR-0046): DNS
 * records and PKI SANs remain plain operator-supplied strings exactly
 * as before this module existed -- nothing here validates or rejects
 * an unqualified name. This module's only job is to hold the site's
 * declared identity and expose it over a real API, so client tooling
 * (the web dashboard's own DNS-record/cert-issue forms) can suggest a
 * default FQDN (`<name>.<site_name>.<domain_suffix>`, or
 * `<name>.<domain_suffix>` when site_name is empty) instead of every
 * operator having to type or remember one. No daemon-side code
 * qualifies a name automatically -- that composition happens entirely
 * client-side, in whatever UI chooses to offer it.
 */

#define SITECONFIG_NAME_MAX 64

enum siteconfig_error {
	SITECONFIG_OK = 0,
	SITECONFIG_ERR_INVALID_SITE_NAME,
	SITECONFIG_ERR_INVALID_DOMAIN_SUFFIX,
	SITECONFIG_ERR_PERSIST_FAILED
};

/*
 * Loads state_path (the persisted site config, if any) at startup.
 * Absent file (first-ever boot) is not an error -- defaults apply:
 * site_name "" (no site tier), domain_suffix "internal". Returns 0,
 * or -1 if the file exists but is malformed.
 */
int siteconfig_init(const char *state_path);

/*
 * Sets both fields (PUT semantics -- always both together, so there
 * is never a moment where a persisted domain_suffix silently
 * disappears because only site_name was meant to change). site_name
 * may be "" (valid -- clears any site tier); domain_suffix must be a
 * valid, non-empty DNS name (dns_name_is_valid()'s own label rules,
 * reused rather than reimplemented -- this project's one real
 * hostname-label validator). Persists atomically; returns
 * SITECONFIG_ERR_PERSIST_FAILED on a disk failure, leaving the
 * in-memory value unchanged from before the call.
 */
enum siteconfig_error siteconfig_set(const char *site_name, const char *domain_suffix);

/* Writes {"site_name": "...", "domain_suffix": "..."} into w. */
void siteconfig_write_json(struct json_writer *w);

#endif /* SITECONFIG_H */
