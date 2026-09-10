#include "logstore.h"
#include "api_hostauth.h"

#include "apiresp.h"
#include "daemonpaths.h"
#include "http.h"
#include "json.h"
#include "hostauth.h"
#include "ldap.h"

#include <stdio.h>
#include <string.h>

/*
 * ADR-0144: POST /v1/login -- the one endpoint that always works
 * regardless of write-gating (dispatch() exempts this exact path, see
 * its own comment). Real bcrypt verification via hostauth_login()
 * (which itself calls ldap_user_check_password()) -- no LDAP bind in
 * this part of the ADR yet, that's a later part's own addition to
 * this same function's internals, not a new endpoint.
 */
void handle_login(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *username, *password;
	char token[HOSTAUTH_TOKEN_LEN + 1];
	char who[HOSTAUTH_USERNAME_MAX];
	int expires_in_seconds;
	enum hostauth_login_result lerr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	username = json_as_string(json_object_get(root, "username"));
	password = json_as_string(json_object_get(root, "password"));
	if (username == NULL || password == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "username and password are both required");
		return;
	}

	/*
	 * The audit lines below outlive the parse tree, and json_as_string()
	 * hands back a pointer INTO it -- so the name is copied out before
	 * json_free() rather than read from freed memory afterwards. That
	 * really happened: the refusal line survived it (the test matches on
	 * "REFUSED", not on the name) while the success line printed garbage
	 * and test_hostauth failed on "a successful login is not audited by
	 * name" with no hint that the cause was a use-after-free.
	 */
	snprintf(who, sizeof(who), "%s", username);

	lerr = hostauth_login(username, password, token, &expires_in_seconds);
	json_free(root);
	if (lerr == HOSTAUTH_LOGIN_INVALID_CREDENTIALS) {
		/*
		 * Audited by NAME, which the generic audit line at dispatch
		 * cannot do: a login is the one request that has no token yet,
		 * so it records itself as "-" there. A failed login attempt
		 * naming the account it was made against is among the more
		 * useful lines this trail can carry -- never the password, and
		 * never a hint about which half was wrong.
		 */
		logstore_write("audit", "warn", "%s login REFUSED (invalid username or password)",
		                who);
		respond_error(fd, 401, "Unauthorized", "invalid username or password");
		return;
	}
	if (lerr == HOSTAUTH_LOGIN_TABLE_FULL) {
		respond_error(fd, 500, "Internal Server Error",
		              "too many active sessions -- try again shortly");
		return;
	}

	logstore_write("audit", "info", "%s logged in", who);

	{
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "token");
		jw_str(&w, token);
		jw_key(&w, "expires_in_seconds");
		if (expires_in_seconds > 0)
			jw_int(&w, expires_in_seconds);
		else
			jw_null(&w);
		jw_obj_close(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

/*
 * POST /v1/logout -- always 204, even for an unknown/already-expired
 * token (hostauth_logout()'s own idempotent contract) -- a client
 * logging out never needs to know or care whether its session had
 * already lapsed server-side.
 */
void handle_logout(int fd, const char *req_headers, size_t req_headers_len)
{
	/* Must fit "Bearer " (7) + the real token (HOSTAUTH_TOKEN_LEN) + NUL
	 * -- a too-small buffer here previously made http_find_header()
	 * silently report "doesn't fit" (a real, live bug: logout always
	 * responded 204 per its own idempotent contract, but never actually
	 * called hostauth_logout() at all, leaving the session valid). */
	char token[HOSTAUTH_TOKEN_LEN + 16];

	if (http_find_header(req_headers, req_headers_len, "Authorization", token, sizeof(token)) >= 0) {
		const char *bearer = strncmp(token, "Bearer ", 7) == 0 ? token + 7 : token;

		hostauth_logout(bearer);
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * GET /v1/whoami -- introspection only, never mutates a session
 * (hostauth_peek_token(), not hostauth_check_token() -- see that
 * function's own doc comment for why the distinction matters under a
 * single-use/idle_timeout_seconds==0 config). Exists specifically so a
 * client can answer "is my current bearer token actually still valid"
 * without write-gating's own GETs-are-always-open rule making that
 * otherwise undeterminable (every ordinary GET succeeds whether or not
 * a token is supplied, by design) -- cixctl's own interactive shell
 * prompt (ADR-0164) is the first real caller. No Authorization header
 * at all, or one naming an unknown/expired/absent-session token, both
 * report the same authenticated:false -- this endpoint doesn't
 * distinguish "never logged in" from "session lapsed," the same way
 * GET /v1/health doesn't distinguish flavors of "not ok."
 */
void handle_whoami(int fd, const char *req_headers, size_t req_headers_len)
{
	char token_hdr[HOSTAUTH_TOKEN_LEN + 16];
	char username[HOSTAUTH_USERNAME_MAX];
	struct json_writer w;
	int authenticated = 0;

	if (http_find_header(req_headers, req_headers_len, "Authorization", token_hdr,
	                      sizeof(token_hdr)) >= 0) {
		const char *bearer = strncmp(token_hdr, "Bearer ", 7) == 0 ? token_hdr + 7 : token_hdr;

		authenticated = hostauth_peek_token(bearer, username, sizeof(username));
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "authenticated");
	jw_bool(&w, authenticated);
	jw_key(&w, "username");
	if (authenticated)
		jw_str(&w, username);
	else
		jw_null(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_hostauth_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	hostauth_write_config_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * PUT /v1/system/hostauth-config -- full replacement (admin_groups is
 * a list, not a single field with an obvious "partial update" meaning
 * the way backup-config's own disk/enabled/interval_hours are each
 * independent) -- the request always supplies both fields, mirroring
 * daemon-config's own full-object PUT shape for a config resource
 * whose fields are this tightly coupled (a lone idle_timeout_seconds
 * change makes little sense to send without knowing what admin_groups
 * currently is, unlike backup-config's own genuinely-independent
 * fields).
 */
void handle_hostauth_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jgroups, *jidle, *jldapen, *jldapservers, *jldapport, *jldapbasedn;
	const char *admin_groups[HOSTAUTH_ADMIN_GROUPS_MAX];
	const char *ldap_servers[HOSTAUTH_LDAP_MAX_SERVERS];
	int admin_group_count = 0;
	int idle_timeout_seconds;
	int ldap_enabled = 0;
	int ldap_server_count = 0;
	int ldap_port = HOSTAUTH_LDAP_DEFAULT_PORT;
	const char *ldap_base_dn = "";
	enum hostauth_config_error err;
	size_t i;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jgroups = json_object_get(root, "admin_groups");
	jidle = json_object_get(root, "idle_timeout_seconds");
	if (jgroups == NULL || jgroups->type != JSON_ARRAY || jidle == NULL ||
	    jidle->type != JSON_NUMBER) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "admin_groups (array) and idle_timeout_seconds "
		                                       "(number) are both required");
		return;
	}
	if (jgroups->u.array.count > HOSTAUTH_ADMIN_GROUPS_MAX) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "admin_groups must have at most 8 entries");
		return;
	}
	for (i = 0; i < jgroups->u.array.count; i++) {
		admin_groups[i] = json_as_string(jgroups->u.array.items[i]);
		if (admin_groups[i] == NULL) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "admin_groups entries must be strings");
			return;
		}
	}
	admin_group_count = (int)jgroups->u.array.count;
	idle_timeout_seconds = (int)json_as_number(jidle);

	/*
	 * ADR-0144's own live-LDAP backend fields: all optional, defaulting
	 * to disabled/empty -- an older-shaped PUT body (just admin_groups/
	 * idle_timeout_seconds, this endpoint's original contract) still
	 * works exactly as before rather than being rejected outright.
	 */
	jldapen = json_object_get(root, "ldap_enabled");
	if (jldapen != NULL && jldapen->type == JSON_BOOL)
		ldap_enabled = jldapen->u.boolean ? 1 : 0;
	jldapservers = json_object_get(root, "ldap_servers");
	if (jldapservers != NULL) {
		if (jldapservers->type != JSON_ARRAY || jldapservers->u.array.count > HOSTAUTH_LDAP_MAX_SERVERS) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "ldap_servers must be an array of at most 3 entries");
			return;
		}
		for (i = 0; i < jldapservers->u.array.count; i++) {
			ldap_servers[i] = json_as_string(jldapservers->u.array.items[i]);
			if (ldap_servers[i] == NULL) {
				json_free(root);
				respond_error(fd, 400, "Bad Request", "ldap_servers entries must be strings");
				return;
			}
		}
		ldap_server_count = (int)jldapservers->u.array.count;
	}
	jldapport = json_object_get(root, "ldap_port");
	if (jldapport != NULL) {
		if (jldapport->type != JSON_NUMBER) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "ldap_port must be a number");
			return;
		}
		ldap_port = (int)json_as_number(jldapport);
	}
	jldapbasedn = json_object_get(root, "ldap_base_dn");
	if (jldapbasedn != NULL) {
		ldap_base_dn = json_as_string(jldapbasedn);
		if (ldap_base_dn == NULL) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "ldap_base_dn must be a string");
			return;
		}
	}

	/*
	 * #370: refuse a config change that would turn ACTIVE gating off --
	 * pointing admin_groups at a group nobody is in, or clearing it
	 * entirely, opens every mutating request on the API while the
	 * response still looks like an ordinary successful update.
	 *
	 * Only that direction is refused. Setting admin_groups BEFORE the
	 * admin users exist is the ordinary way gating gets turned on for
	 * the first time, and gating is inactive throughout that, so a
	 * blanket "admin_groups must have a member" rule would obstruct
	 * enabling authentication in order to protect it. The test is
	 * therefore active-now-and-inactive-after, not merely inactive-after.
	 */
	if (hostauth_gating_active() && !hostauth_would_gate(admin_groups, admin_group_count)) {
		json_free(root);
		respond_error(fd, 409, "Conflict",
		              "this would turn off write authentication for the whole API: no enabled "
		              "user is in any of the admin_groups given. Add an admin to the new group "
		              "first, or remove the last admin deliberately via /v1/ldap");
		return;
	}

	err = hostauth_set_config(admin_groups, admin_group_count, idle_timeout_seconds, ldap_enabled,
	                           ldap_servers, ldap_server_count, ldap_port, ldap_base_dn);
	json_free(root);
	if (err != HOSTAUTH_CONFIG_OK) {
		if (err == HOSTAUTH_CONFIG_ERR_INVALID_FIELD)
			respond_error(fd, 400, "Bad Request",
			              "idle_timeout_seconds must be >= 0, admin_groups at most 8 entries, "
			              "ldap_servers at most 3 entries, ldap_port in 1..65535, and "
			              "ldap_enabled requires at least one ldap_servers entry plus a "
			              "non-empty ldap_base_dn");
		else
			respond_error(fd, 500, "Internal Server Error", "could not persist host-auth config");
		return;
	}

	{
		struct json_writer w;

		jw_init(&w);
		hostauth_write_config_json(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

void handle_hostauth_sessions_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "sessions");
	hostauth_write_sessions_json(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* DELETE /v1/system/hostauth/sessions/{username} -- revokes every
 * active session for that user ("log out everywhere"). Always 204,
 * even if the user had no active session (the same idempotent-logout
 * posture handle_logout() already has), since the end state (no
 * active session for this user) is identical either way. */
void handle_hostauth_sessions_revoke(int fd, const char *username)
{
	hostauth_revoke_sessions_for_user(username);
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}



/*
 * Reaps bootstrap_fetch_start()'s own curl child. On a real, clean
 * exit, verifies the fetched artifact's checksum (pkg_run_capture_
 * sha256(), the exact same real check every recipe source already
 * gets, ADR-0036) before ever handing it to pkg_bootstrap_from_
 * toolchain() -- a corrupt or wrong-URL fetch must never get
 * unsquashfs'd into the build sandbox silently.
 */
