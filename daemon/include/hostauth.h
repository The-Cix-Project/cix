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
	/* ADR-0317 section 6 (#541): the admin_groups given would leave no
	 * enabled user holding identity:write while one does now. */
	HOSTAUTH_CONFIG_ERR_LOCKOUT,
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
 * group rename. ADR-0317 (#540): the permission mapping is keyed by
 * group name, so if old_name has an entry -- any grant at all, not only
 * every permission -- it is renamed in place, admin_groups is
 * re-derived, and both are persisted. A renamed group therefore never
 * silently loses its grants, and a renamed admin group never drops out
 * of write-gating (the class of incident ADR-0146 closed once already).
 * A no-op, returning 1, if old_name has no entry. Returns 0 on a real
 * persist failure -- the caller treats that as reason to refuse the
 * rename outright rather than leave the mapping and the group's own
 * name inconsistent. The name is historical: it predates the mapping. */
int hostauth_rename_admin_group(const char *old_name, const char *new_name);

/*
 * ---- ADR-0317 (#540): the group -> permission mapping ----
 *
 * The one stored statement of who may do what: a group name maps to a
 * set of words from the closed vocabulary the API contract declares
 * (x-cix-permissions, generated into build/generated/permissions.h).
 * A user holds the union of the permissions of every group they are
 * in, primary and secondary; there is no deny.
 *
 * admin_groups is DERIVED from this -- the groups holding every
 * permission -- so every admin-group function above answers from the
 * mapping and #370's guards (last admin, disable, de-admin) protect the
 * groups that hold everything. hostauth_set_config() still takes an
 * admin_groups list, read as: a named group is granted every
 * permission; a group that held every permission and is no longer
 * named loses its grants. Nothing enforces the mapping per operation
 * until #541.
 */
#define HOSTAUTH_PERM_GROUPS_MAX 32
#define HOSTAUTH_PERMISSION_MAX 48
/* How many words one PUT may name: the whole vocabulary fits with room. */
#define HOSTAUTH_PERMISSIONS_PER_REQUEST_MAX 64

enum hostauth_perm_error {
	HOSTAUTH_PERM_OK = 0,
	HOSTAUTH_PERM_ERR_INVALID_GROUP,
	HOSTAUTH_PERM_ERR_UNKNOWN_PERMISSION, /* the offending word is written to detail */
	HOSTAUTH_PERM_ERR_FULL,               /* HOSTAUTH_PERM_GROUPS_MAX groups already mapped */
	/* More groups would hold every permission than admin_groups can
	 * list (HOSTAUTH_ADMIN_GROUPS_MAX) -- the derived list must stay
	 * whole, or a group holding everything would silently not be an
	 * admin under today's enforcement. */
	HOSTAUTH_PERM_ERR_TOO_MANY_ADMIN_GROUPS,
	/* Would leave no enabled user holding identity:write while one
	 * does now (ADR-0317 section 6). */
	HOSTAUTH_PERM_ERR_LOCKOUT,
	HOSTAUTH_PERM_ERR_PERSIST_FAILED,
};

/* Replaces the permissions group grants with exactly perms[0..count);
 * count == 0 removes the group from the mapping. Nothing is changed
 * unless the whole request is accepted. */
enum hostauth_perm_error hostauth_set_group_permissions(const char *group, const char *const *perms,
                                                         int count, char *detail,
                                                         size_t detail_size);
/* Whether username holds permission through any of their groups. */
int hostauth_user_has_permission(const char *username, const char *permission);
/* Whether permission is a word in the contract's vocabulary. */
int hostauth_permission_is_known(const char *permission);
/* {"vocabulary": [...], "groups": {"<group>": [...]}} */
void hostauth_write_permissions_json(struct json_writer *w);
/* Called by ldap_group_delete() once the group is gone: removes the
 * group from the mapping, so a later group created with the same name
 * does not inherit what the deleted one was granted. */
void hostauth_forget_group(const char *name);
/* The permissions username holds (the union over their groups), as a
 * JSON array in vocabulary order; [] for NULL. */
void hostauth_write_user_permissions_json(struct json_writer *w, const char *username);

/*
 * ADR-0317 section 7 (#542): gives a host the standard groups --
 * cix-admins (every permission), cix-operators (`public`, every read,
 * every operate), cix-readers (every read) -- creating each in the
 * directory and in the mapping. Once per host: a marker persisted with
 * the mapping keeps a group the operator deleted from coming back. A
 * group that already exists, in the directory or the mapping, is never
 * re-granted. Called at boot after ldap_init() and hostauth_init();
 * returns -1 (logged, and retried at the next start) if any could not
 * be made. New groups take gids from ldap_gid_alloc(), which skips any
 * gid a user still carries.
 */
int hostauth_provision_standard_groups(void);

/*
 * Gating, and the one invariant that protects it (ADR-0317 section 6,
 * #541; #370 before it).
 *
 * Gating is active while at least one enabled user holds
 * identity:write -- the permission that changes the mapping, and so
 * the one that can grant every other. Before anyone holds it, on a
 * fresh install or in the test suite, every operation answers without
 * a session; that is what keeps a new box from locking itself out of
 * the API that would explain why.
 *
 * Once someone holds it, no ordinary operation may leave nobody
 * holding it: that would turn authentication off for the whole API
 * while the configuration still looked correct -- #370's five
 * incidents, restated for permissions. The guards refuse such a change
 * with 409 rather than making gating fail closed, which would trade a
 * silent hole for an API lockout recoverable only through ADR-0146's
 * cix-recover boot entry. The predicates below are the whole rule;
 * the mapping's own writers (hostauth_set_config() and
 * hostauth_set_group_permissions()) apply the same test to the mapping
 * they propose.
 *
 * State arriving from outside the API -- a restored or hand-edited
 * config file -- cannot be guarded, so gating_active is reported by
 * GET /system/hostauth-config and GET /health, logged at boot when
 * inactive, and shown by the dashboard.
 */
int hostauth_gating_active(void);

/*
 * Would a user change still leave someone holding identity:write?
 * Judges the record the request PROPOSES -- these gids, this disabled
 * flag, or deletion -- since the stored record cannot answer what the
 * change does. 1 (allowed) while gating is inactive, or while any
 * OTHER user holds it.
 */
int hostauth_user_change_keeps_grantor(const char *username, int primarygroup,
                                       const int *secondary_groups, int secondary_count,
                                       int disabled, int deleting);

/*
 * Would deleting this group, or changing its gidnumber (which orphans
 * every member: users carry gids), still leave someone holding
 * identity:write through some OTHER group? 1 while gating is inactive
 * or the group grants no identity:write.
 */
int hostauth_group_change_keeps_grantor(const char *group);

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
 * The one authorization decision (ADR-0317 section 4, #541): may the
 * caller presenting token run an operation that requires permission?
 * dispatch() asks it once per request, after the generated route table
 * has matched and before the handler runs, with the permission the
 * contract declares for that operation.
 *
 *   public            OK, and the token is not looked at -- so a
 *                     single-use session is never consumed by it.
 *   gating inactive   OK (see hostauth_gating_active()).
 *   no valid session  NO_SESSION (the caller answers 401). The token
 *                     IS checked here, which refreshes its idle window
 *                     and consumes a single-use one: this is the one
 *                     real check per request.
 *   authenticated     OK for any valid session.
 *   anything else     OK when the session's user holds it through one
 *                     of their groups, read live -- a membership
 *                     change takes effect on the next request, not the
 *                     next login -- and FORBIDDEN (403) otherwise.
 *
 * A NULL permission is FORBIDDEN: apigen refuses a spec without one,
 * so it cannot happen, and if it ever did it must not fail open.
 */
enum hostauth_authz {
	HOSTAUTH_AUTHZ_OK = 0,
	HOSTAUTH_AUTHZ_NO_SESSION,
	HOSTAUTH_AUTHZ_FORBIDDEN,
};
enum hostauth_authz hostauth_authorize(const char *token, const char *permission);

/*
 * ADR-0317 section 8 (#543): hostauth_authorize() for a request that
 * presented an app password (HTTP Basic) instead of a session. username
 * is the user the app password verified as (ldap_app_password_check()),
 * or NULL when it did not verify -- NO_SESSION, a 401, like any missing
 * credential. The permissions are exactly the user's, with one
 * exception: `authenticated` is refused. It means "any valid session",
 * and it is what guards a user managing their OWN app passwords; if an
 * app password satisfied it, a leaked one could mint successors that
 * outlive its own revocation. Managing app passwords with one needs
 * identity:write, like managing anyone's.
 */
enum hostauth_authz hostauth_authorize_app_password(const char *username, const char *permission);

#endif /* HOSTAUTH_H */
