#ifndef SITECONFIG_H
#define SITECONFIG_H

#include "json.h"

/*
 * A minimal, real, operator-configurable identity for this
 * install/site -- raised directly by the user rejecting a hardcoded
 * ".internal" default: "this should be configurable... this has an
 * effect on both DNS and PKI." Three fields: instance_name (this
 * specific host's own label, e.g. "cix1" -- always non-empty,
 * defaults to "cix", the one field this module guarantees always
 * has *something* to display so the dashboard/backups always have an
 * identity to show), site_name (an operator-chosen group/location
 * label, e.g. "lab1" -- empty is valid, meaning "no site tier," a
 * single-site deployment), and domain_suffix (the private TLD this
 * site's names live under; defaults to "internal" -- IANA-reserved
 * for exactly this use, RFC 9476 -- but is a real, settable value,
 * not a compile-time constant).
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
 * client-side, in whatever UI chooses to offer it. instance_name
 * follows the identical convenience-only rule: it labels this
 * install for the operator's own benefit (dashboard header, backup
 * bundle) -- nothing daemon-side reads it back to make a decision.
 */

#define SITECONFIG_NAME_MAX 64

enum siteconfig_error {
	SITECONFIG_OK = 0,
	SITECONFIG_ERR_INVALID_INSTANCE_NAME,
	SITECONFIG_ERR_INVALID_SITE_NAME,
	SITECONFIG_ERR_INVALID_DOMAIN_SUFFIX,
	SITECONFIG_ERR_PERSIST_FAILED
};

/*
 * Loads state_path (the persisted site config, if any) at startup.
 * Absent file (first-ever boot) is not an error -- defaults apply:
 * instance_name "cix", site_name "" (no site tier), domain_suffix
 * "internal". A persisted file from before instance_name existed
 * (missing the field) is not an error either -- the default applies
 * to that one field exactly as it would on a fresh install. Returns
 * 0, or -1 if the file exists but is malformed.
 */
int siteconfig_init(const char *state_path);

/* ADR-0141 Phase 2: repoints without reloading -- see network_repoint()'s
 * own doc comment for the shared reasoning. */
void siteconfig_repoint(const char *new_state_path);

/*
 * Sets all three fields (PUT semantics -- always all together, so
 * there is never a moment where a persisted field silently
 * disappears because only one was meant to change). instance_name
 * must be a valid, non-empty DNS name; site_name may be "" (valid --
 * clears any site tier); domain_suffix must be a valid, non-empty DNS
 * name (dns_name_is_valid()'s own label rules, reused rather than
 * reimplemented -- this project's one real hostname-label validator,
 * applied identically to all three). Persists atomically; returns
 * SITECONFIG_ERR_PERSIST_FAILED on a disk failure, leaving the
 * in-memory value unchanged from before the call.
 */
enum siteconfig_error siteconfig_set(const char *instance_name, const char *site_name,
                                      const char *domain_suffix);

/* Writes {"instance_name": "...", "site_name": "...", "domain_suffix": "..."} into w. */
void siteconfig_write_json(struct json_writer *w);

/*
 * This install's own domain_suffix, read live from memory (no disk
 * I/O, never NULL). For callers that need to compose something from
 * the current site identity -- e.g. a default CA common name, or a
 * server-side name-qualification helper -- without going through the
 * JSON-writer shape siteconfig_write_json() targets.
 */
/* This install's own name -- what POST /system/factory-reset requires
 * typed back before it will do anything (issue #63). */
const char *siteconfig_instance_name(void);

const char *siteconfig_domain_suffix(void);

/*
 * Composes this install's own FQDN -- `<instance_name>.<site_name>.
 * <domain_suffix>`, or `<instance_name>.<domain_suffix>` when
 * site_name is "". This is this specific install's own identity (the
 * PKI host cert, the auto-maintained instance DNS record), distinct
 * from siteconfig_qualify()'s job of qualifying an arbitrary
 * operator-typed label -- instance_name is always the label here,
 * never a caller-supplied one. bufsize should be at least
 * 3 * SITECONFIG_NAME_MAX to never truncate.
 */
void siteconfig_host_fqdn(char *buf, size_t bufsize);

/*
 * Server-side counterpart to the identical rule the web dashboard's
 * own client-side qualifyOnBlur() already applies (ADR-0052): if
 * label contains no '.', composes <label>.<site_name>.<domain_suffix>
 * (or <label>.<domain_suffix> when site_name is ""); otherwise copies
 * label through unchanged. A default, never an enforcement -- an
 * operator who already typed a dot gets exactly what they typed, no
 * daemon-side override, ever. label may be NULL (writes "" to buf).
 */
void siteconfig_qualify(const char *label, char *buf, size_t bufsize);

#endif /* SITECONFIG_H */
