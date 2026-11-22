#ifndef CIX_LDAPTOKEN_H
#define CIX_LDAPTOKEN_H

/*
 * A container's own provisioned LDAP account, filled into its files
 * (ADR-0389, #733).
 *
 * A container with ldap_provision gets a search-only account of its own,
 * with a fresh secret at every start. A service that reads its bind
 * credential from its own config file (lighttpd's mod_authn_ldap is the
 * first) names it there as a token, and the token is replaced when the
 * file is staged, before the container starts:
 *
 *   {{LDAP:PROVISIONED_DN}}       the account's DN
 *   {{LDAP:PROVISIONED_SECRET}}   its secret for this start
 *
 * These are not the deployment-apply tokens ({{LDAP:URI}} and the rest,
 * which pkg.c fills from the daemon's own client config): the secret
 * does not exist until the container is being created, so apply leaves
 * these verbatim and container creation fills them.
 */

/*
 * Replaces every {{LDAP:PROVISIONED_DN}} and {{LDAP:PROVISIONED_SECRET}}
 * in `in` with `dn` and `secret`. Returns 0 when `in` holds no
 * {{LDAP:PROVISIONED_ token (and sets *out to NULL), 1 when it did (and
 * *out is a newly allocated, NUL-terminated string the caller frees),
 * and -1 when a token names something else, or when `dn` or `secret` is
 * NULL because the container has no provisioned account, or when
 * allocation fails. A refusal is never a partial render: *out is NULL.
 */
int ldaptoken_render(const char *in, const char *dn, const char *secret, char **out);

#endif /* CIX_LDAPTOKEN_H */
