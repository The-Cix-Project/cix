#ifndef HOSTAUTH_H
#define HOSTAUTH_H

#include "json.h"

#include <stddef.h>

/*
 * ADR-0144: real authentication for kanxeod's own REST API, which has
 * had none at all until this. One rule -- write operations (POST/PUT/
 * DELETE, plus the container console, judged by intent rather than
 * HTTP verb) require a valid session belonging to a user in one of the
 * configured admin groups; GET stays open, always. "Write-gating" only
 * ever actually activates once at least one such user exists
 * (hostauth_gating_active()) -- a fresh install can never lock itself
 * out of its own API, no separate bootstrap/break-glass credential
 * needed.
 *
 * This module owns sessions and the admin-group/idle-timeout config;
 * it does NOT own credential verification itself -- that's
 * ldap_user_check_password()/ldap_user_is_in_group() (daemon/src/
 * ldap.c, ADR-0144's own data-model part), the local backend, and (a
 * later part of this same ADR) a live LDAP bind+search backend tried
 * first when enabled. One directory, two interchangeable ways to ask
 * it a question -- this module doesn't care which one answered.
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

int hostauth_init(const char *config_path);
void hostauth_repoint(const char *new_config_path);

/* Current admin-group list (0 configured means gating can never
 * activate -- see hostauth_gating_active()) and idle timeout. */
int hostauth_admin_group_count(void);
const char *hostauth_admin_group(int index);
int hostauth_idle_timeout_seconds(void);

enum hostauth_config_error {
	HOSTAUTH_CONFIG_OK = 0,
	HOSTAUTH_CONFIG_ERR_INVALID_FIELD,
	HOSTAUTH_CONFIG_ERR_PERSIST_FAILED,
};

/* Full replacement, matching daemon-config's own "only fields given
 * are changed" PUT convention at the REST layer (main.c) -- this
 * function itself always takes the complete resulting set, same shape
 * backupconfig_set() already established. admin_group_count == 0 is
 * valid (no admin groups configured yet -- gating stays inactive).
 * idle_timeout_seconds: 0 means no session reuse (every write
 * re-authenticates); must be >= 0. */
enum hostauth_config_error hostauth_set_config(const char *const *admin_groups, int admin_group_count,
                                                int idle_timeout_seconds);
void hostauth_write_config_json(struct json_writer *w);

/* True once at least one user is a member of a configured admin group
 * -- the bootstrap-safety check every write-gating decision starts
 * from. False (writes stay open) if no admin group is configured, or
 * none has a member yet. */
int hostauth_gating_active(void);

enum hostauth_login_result {
	HOSTAUTH_LOGIN_OK = 0,
	HOSTAUTH_LOGIN_INVALID_CREDENTIALS,
	HOSTAUTH_LOGIN_TABLE_FULL,
};

/* Local-backend login: real bcrypt verification against this
 * daemon's own already-persisted LDAP user record (ldap_user_check_
 * password()), no network call. (A later ADR-0144 part adds a live
 * LDAP bind attempted first when enabled -- this function's own
 * contract doesn't change, only what it tries before falling back to
 * this.) On success, issues a real session token into out_token
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
