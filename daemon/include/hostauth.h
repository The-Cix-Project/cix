#ifndef HOSTAUTH_H
#define HOSTAUTH_H

#include "json.h"

#include <stddef.h>

/*
 * ADR-0144: real authentication for cixd's own REST API, which has
 * had none at all until this. One rule -- write operations (POST/PUT/
 * DELETE, plus the container console, judged by intent rather than
 * HTTP verb) require a valid session belonging to a user in one of the
 * configured admin groups; GET stays open, always. "Write-gating" only
 * ever actually activates once at least one such user exists
 * (hostauth_gating_active()) -- a fresh install can never lock itself
 * out of its own API, no separate bootstrap/break-glass credential
 * needed.
 *
 * This module owns sessions and the admin-group/idle-timeout/LDAP-
 * backend config; it does NOT own credential verification itself --
 * that's ldap_user_check_password() (daemon/src/ldap.c, ADR-0144's
 * own data-model part, the local in-process backend) and
 * ldapclient_bind() (daemon/src/ldapclient.c, a live LDAP bind against
 * a real running glauth server, tried first when ldap_enabled). One
 * directory (Cix's own persisted ldap_user/ldap_group records --
 * glauth is just a rendered, running view of that same data), two
 * interchangeable ways to ask it "is this password right": hostauth_
 * login() itself decides which one answered authoritatively, see its
 * own doc comment below.
 *
 * Sessions are in-memory only, wiped on every daemon restart --
 * matches this project's own established "ephemeral unless there's a
 * real reason to persist" precedent (the container registry itself is
 * the same way). A sliding idle timeout (config, 0 means no session
 * reuse at all -- every write re-authenticates) refreshes on every
 * successful hostauth_check_token() call.
 */

#define HOSTAUTH_TOKEN_LEN 48 /* hex-encoded, real /dev/urandom bytes -- see hostauth.c */
#define HOSTAUTH_SESSION_MAX 64
#define HOSTAUTH_ADMIN_GROUPS_MAX 8
#define HOSTAUTH_GROUP_NAME_MAX 32 /* matches LDAP_GROUP_NAME_MAX, no header dependency */
#define HOSTAUTH_USERNAME_MAX 32   /* matches LDAP_USER_NAME_MAX, no header dependency */

/* ADR-0144: live-LDAP backend config -- a try-in-order server list, the
 * same "list of servers, try in order" shape resolv.c's own RESOLV_MAX_
 * NAMESERVERS/nameservers[] already established for GET/PUT /v1/system/
 * resolv, reused here rather than inventing a second list convention. */
#define HOSTAUTH_LDAP_MAX_SERVERS 3
#define HOSTAUTH_LDAP_HOST_MAX 64      /* an IPv4 dotted-quad or short hostname */
#define HOSTAUTH_LDAP_BASE_DN_MAX 256
#define HOSTAUTH_LDAP_DEFAULT_PORT 3893 /* glauth's own real default listen port, see ADR-0109/0113 */

int hostauth_init(const char *config_path);
void hostauth_repoint(const char *new_config_path);

/* Current admin-group list (0 configured means gating can never
 * activate -- see hostauth_gating_active()) and idle timeout. */
int hostauth_admin_group_count(void);
const char *hostauth_admin_group(int index);
int hostauth_idle_timeout_seconds(void);
const char *hostauth_ldap_base_dn(void); /* ADR-0148: real, canonical, empty ("") when unset */

enum hostauth_config_error {
	HOSTAUTH_CONFIG_OK = 0,
	HOSTAUTH_CONFIG_ERR_INVALID_FIELD,
	HOSTAUTH_CONFIG_ERR_PERSIST_FAILED,
	/* #416: ldap_tls asked for with ldap_enabled, on a host with no
	 * root CA. Distinct from INVALID_FIELD because no single field is
	 * wrong -- the combination is, and the operator's fix is to
	 * bootstrap the CA, which a generic "invalid field" would not
	 * name. */
	HOSTAUTH_CONFIG_ERR_NO_CA,
};

/* Full replacement, matching daemon-config's own "only fields given
 * are changed" PUT convention at the REST layer (main.c) -- this
 * function itself always takes the complete resulting set, same shape
 * backupconfig_set() already established. admin_group_count == 0 is
 * valid (no admin groups configured yet -- gating stays inactive).
 * idle_timeout_seconds: 0 means no session reuse (every write
 * re-authenticates); must be >= 0.
 *
 * ADR-0144's own live-LDAP backend config: ldap_enabled, a try-in-order
 * ldap_servers list (ldap_server_count long, each a host or IP glauth
 * is reachable on), the shared ldap_port every one of them is queried
 * on, and ldap_base_dn (e.g. "dc=glauth,dc=com") -- this value is the
 * one real, canonical source of truth for the base DN as of ADR-0148
 * (hostauth_ldap_base_dn()): ldap.c's own config-file renderer keeps a
 * registered server's own glauth.cfg baseDN line in sync with it on
 * every write, closing what used to be a fourth independently-typed
 * copy of the same value. The rest of that file (listener/TLS/
 * behaviors) stays genuinely operator-authored -- see ldap.h's own
 * header comment. Rejected
 * (HOSTAUTH_CONFIG_ERR_INVALID_FIELD) if: ldap_server_count is outside
 * 0..HOSTAUTH_LDAP_MAX_SERVERS; ldap_port is outside 1..65535; or
 * ldap_enabled is true while ldap_server_count == 0 or ldap_base_dn is
 * empty -- "enabled with nothing to bind against" can never be a valid
 * saved state.
 *
 * ldap_tls (#416) runs the daemon's own bind over LDAPS, verified
 * against this host's own CA. HOSTAUTH_CONFIG_ERR_NO_CA when asked for
 * together with ldap_enabled on a host that has not bootstrapped one,
 * by the same rule -- it is settable ahead of enabling the backend, so
 * the ordering an operator prefers is theirs to choose. Note this is
 * NOT ldap.c's client_tls: that configures the LDAP clients inside
 * containers, this configures cixd. */
enum hostauth_config_error hostauth_set_config(const char *const *admin_groups, int admin_group_count,
                                                int idle_timeout_seconds, int ldap_enabled,
                                                const char *const *ldap_servers, int ldap_server_count,
                                                int ldap_port, int ldap_tls, const char *ldap_base_dn);
void hostauth_write_config_json(struct json_writer *w);

/* Called by ldap_group_rename() (ADR-0147) before it commits an LDAP
 * group rename -- if old_name currently appears in admin_groups, it's
 * rewritten to new_name in place and persisted, so a renamed admin
 * group never silently drops out of write-gating (the exact class of
 * incident ADR-0146 already closed once for a different cause; a
 * rename that orphaned admin_groups would be a self-inflicted repeat
 * of it). A no-op, returning 1, if old_name isn't currently an admin
 * group at all. Returns 0 on a real persist failure -- the caller
 * (ldap_group_rename()) treats that as reason to refuse the rename
 * outright rather than leave admin_groups and the group's own name
 * inconsistent. */
int hostauth_rename_admin_group(const char *old_name, const char *new_name);

/* True once at least one user is a member of a configured admin group
 * -- the bootstrap-safety check every write-gating decision starts
 * from. False (writes stay open) if no admin group is configured, or
 * none has a member yet. */
/*
 * The predicates that protect gating's own preconditions (#370).
 *
 * Gating is active only while an admin group is configured AND an
 * enabled user is in one. Five ordinary operations could quietly break
 * the second half -- deleting the admin group, renaming it or changing
 * its gidnumber, deleting the last admin, disabling or de-admining
 * them, and pointing admin_groups at an empty group. Each one turns
 * authentication off for the whole API while leaving the configuration
 * looking correct, which is exactly what nobody would look for.
 *
 * The chosen fix is to make those transitions UNREACHABLE through the
 * API rather than to make hostauth_gating_active() fail closed. Failing
 * closed would turn an orphaned config into an API lockout whose only
 * recovery is ADR-0146's cix-recover boot entry, on a host with no
 * shell -- trading a silent hole for a new way to brick the box. So the
 * guards refuse the operation, with 409 and a message naming the fix,
 * and gating's own definition is left alone.
 *
 * That leaves one residual case this cannot guard: state arriving from
 * outside the API, such as a restored or hand-edited config file. That
 * is what the visibility half of #370 is for -- gating_active is
 * reported by GET /system/hostauth-config and GET /health, logged at
 * boot when inactive, and banner-ed in the dashboard, so an open
 * control plane is never silent even when it was not reached through a
 * guarded path.
 */
int hostauth_admin_user_count(void);
int hostauth_group_is_admin_group(const char *group_name);
int hostauth_user_is_admin(const char *username);

/*
 * Would a user carrying exactly these gids be an admin? The user-PUT
 * guard needs to judge the record a request PROPOSES, before it is
 * applied -- the stored record cannot answer that, since the question
 * is precisely whether the change removes the last admin.
 */
int hostauth_gids_are_admin(int primarygroup, const int *secondary_groups, int secondary_count,
                             int disabled);

/*
 * Would gating be active if admin_groups were exactly this list? The
 * hostauth-config PUT guard's own predicate -- it must judge the config
 * a request PROPOSES rather than the one in force.
 */
int hostauth_would_gate(const char *const *admin_groups, int admin_group_count);

int hostauth_gating_active(void);

enum hostauth_login_result {
	HOSTAUTH_LOGIN_OK = 0,
	HOSTAUTH_LOGIN_INVALID_CREDENTIALS,
	HOSTAUTH_LOGIN_TABLE_FULL,
};

/*
 * Verifies username/password, trying the live LDAP backend first when
 * ldap_enabled: builds the confirmed-working short-form bind DN
 * ("cn=<username>,ou=<primary-group-name>,<ldap_base_dn>", from this
 * daemon's own local ldap_user/ldap_group records -- never from a
 * search) and attempts a real ldapclient_bind() against each
 * configured server in order. The first server to answer
 * authoritatively (bind succeeded, or explicitly rejected the
 * credentials) decides the outcome -- an unreachable/erroring server
 * is skipped in favor of the next one, never treated as "wrong
 * password". Only when EVERY configured server was unreachable (or
 * LDAP is disabled, or no local user/group record exists yet to build
 * a DN from) does this fall back to the local, in-process check
 * (ldap_user_check_password(), no network call) -- the same
 * underlying passbcrypt data either way, so this fallback changes
 * availability, never which password is actually correct.
 *
 * On success, issues a real session token into out_token
 * (HOSTAUTH_TOKEN_LEN + 1 bytes) and reports its idle-timeout-based
 * initial expiry in *out_expires_in_seconds (-1 means "never expires
 * on idle," i.e. idle_timeout_seconds == 0 is handled the other way:
 * see hostauth_check_token()'s own doc comment). */
enum hostauth_login_result hostauth_login(const char *username, const char *password,
                                           char out_token[HOSTAUTH_TOKEN_LEN + 1],
                                           int *out_expires_in_seconds);

/* No-op if token doesn't name a live session (logout is always
 * idempotent from the caller's point of view). */
void hostauth_logout(const char *token);

/*
 * Validates token, refreshing its idle expiry on success (sliding
 * window) -- unless idle_timeout_seconds is 0, in which case every
 * token is single-use and consumed (removed) here regardless of
 * outcome, matching the documented "0 means every write re-
 * authenticates" contract. Returns 1 and fills out_username (real
 * data, HOSTAUTH_USERNAME_MAX bytes) on a valid, unexpired session; 0
 * otherwise (unknown token, expired token, or the single-use-then-
 * gone case above).
 */
int hostauth_check_token(const char *token, char *out_username, size_t out_username_size);

/* Read-only sibling of hostauth_check_token() -- same validity check
 * (unknown/expired token both return 0), but never mutates a session:
 * no sliding-window expiry refresh, and critically, never consumes a
 * single-use (idle_timeout_seconds == 0) token the way the real check
 * does. For introspection only (GET /v1/hostauth/whoami) -- calling
 * this can never itself invalidate a session, which repeatedly calling
 * hostauth_check_token() for the same purpose would, under a single-
 * use config. */
/*
 * Slides a live session's idle window without consuming or validating
 * it (#379). Called once per served request, after a successful peek,
 * so that a client which only ever reads is not treated as idle. No-op
 * under idle_timeout_seconds == 0 (single-use sessions).
 */
void hostauth_touch_token(const char *token);

int hostauth_peek_token(const char *token, char *out_username, size_t out_username_size);

/* {"sessions":[{"username":...,"expires_in_seconds":<int or null>}, ...]}
 * -- every currently active session. Never includes a raw token, an
 * opaque session ID, or anything else that would let a caller target
 * one specific session; see hostauth_revoke_sessions_for_user()'s own
 * doc comment for why revocation is per-username, not per-session. */
void hostauth_write_sessions_json(struct json_writer *w);

/* Revokes every active session for username ("log this account out
 * everywhere"). Returns the count actually revoked (0 if none were
 * active). */
int hostauth_revoke_sessions_for_user(const char *username);

/*
 * The one real authorization decision dispatch() consults for every
 * mutating request: returns 1 if the write should proceed. That's
 * true when gating isn't active at all (hostauth_gating_active() ==
 * 0), OR token names a live session whose user is currently a member
 * of at least one configured admin group (checked live via ldap_user_
 * is_in_group(), never cached in the session -- a membership change
 * takes effect on this request, not at next login). token may be
 * NULL (no Authorization header at all), always false in that case
 * once gating is active.
 */
int hostauth_authorize_write(const char *token);

#endif /* HOSTAUTH_H */
