#include "hostauth.h"
#include "ldap.h"
#include "ldapclient.h"
#include "persist.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Bounds one hostauth_login() call's worth of live-LDAP work (connect +
 * bind, per configured server) -- long enough for a real LAN round trip,
 * short enough that a down server doesn't stall a login request. */
#define HOSTAUTH_LDAP_TIMEOUT_MS 3000

struct hostauth_config {
	char admin_groups[HOSTAUTH_ADMIN_GROUPS_MAX][HOSTAUTH_GROUP_NAME_MAX];
	int admin_group_count;
	int idle_timeout_seconds; /* 0: no session reuse, every write re-authenticates */
	int ldap_enabled;
	char ldap_servers[HOSTAUTH_LDAP_MAX_SERVERS][HOSTAUTH_LDAP_HOST_MAX];
	int ldap_server_count;
	int ldap_port;
	char ldap_base_dn[HOSTAUTH_LDAP_BASE_DN_MAX];
};

static struct hostauth_config g_config = { { { 0 } }, 0, 900, 0, { { 0 } }, 0, HOSTAUTH_LDAP_DEFAULT_PORT, { 0 } };
static char g_config_path[PATH_MAX];

struct hostauth_session {
	char token[HOSTAUTH_TOKEN_LEN + 1];
	char username[HOSTAUTH_USERNAME_MAX];
	time_t expires_at; /* meaningless (never consulted) when idle_timeout_seconds == 0 */
	int in_use;
};

static struct hostauth_session g_sessions[HOSTAUTH_SESSION_MAX];

static int save_config(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	hostauth_write_config_json(&w);
	rc = persist_atomic_write(g_config_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int load_config(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *jgroups, *jidle, *jldapen, *jldapservers, *jldapport, *jldapbasedn;

	if (persist_read_file(g_config_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no file yet -- real, documented defaults (no admin groups, 900s idle) stand */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		fprintf(stderr, "%s: malformed persisted host-auth config\n", g_config_path);
		return -1;
	}

	jgroups = json_object_get(root, "admin_groups");
	if (jgroups != NULL && jgroups->type == JSON_ARRAY) {
		size_t i;

		g_config.admin_group_count = 0;
		for (i = 0; i < jgroups->u.array.count && (int)i < HOSTAUTH_ADMIN_GROUPS_MAX; i++) {
			const char *name = json_as_string(jgroups->u.array.items[i]);

			if (name != NULL) {
				snprintf(g_config.admin_groups[g_config.admin_group_count],
				         sizeof(g_config.admin_groups[0]), "%s", name);
				g_config.admin_group_count++;
			}
		}
	}
	jidle = json_object_get(root, "idle_timeout_seconds");
	if (jidle != NULL)
		g_config.idle_timeout_seconds = (int)json_as_number(jidle);

	jldapen = json_object_get(root, "ldap_enabled");
	if (jldapen != NULL && jldapen->type == JSON_BOOL)
		g_config.ldap_enabled = jldapen->u.boolean ? 1 : 0;

	jldapservers = json_object_get(root, "ldap_servers");
	if (jldapservers != NULL && jldapservers->type == JSON_ARRAY) {
		size_t i;

		g_config.ldap_server_count = 0;
		for (i = 0; i < jldapservers->u.array.count && (int)i < HOSTAUTH_LDAP_MAX_SERVERS; i++) {
			const char *host = json_as_string(jldapservers->u.array.items[i]);

			if (host != NULL) {
				snprintf(g_config.ldap_servers[g_config.ldap_server_count],
				         sizeof(g_config.ldap_servers[0]), "%s", host);
				g_config.ldap_server_count++;
			}
		}
	}
	jldapport = json_object_get(root, "ldap_port");
	if (jldapport != NULL)
		g_config.ldap_port = (int)json_as_number(jldapport);
	jldapbasedn = json_object_get(root, "ldap_base_dn");
	if (jldapbasedn != NULL && json_as_string(jldapbasedn) != NULL)
		snprintf(g_config.ldap_base_dn, sizeof(g_config.ldap_base_dn), "%s", json_as_string(jldapbasedn));

	json_free(root);
	return 0;
}

int hostauth_init(const char *config_path)
{
	snprintf(g_config_path, sizeof(g_config_path), "%s", config_path);
	memset(g_sessions, 0, sizeof(g_sessions));
	return load_config();
}

void hostauth_repoint(const char *new_config_path)
{
	snprintf(g_config_path, sizeof(g_config_path), "%s", new_config_path);
}

int hostauth_admin_group_count(void)
{
	return g_config.admin_group_count;
}

const char *hostauth_admin_group(int index)
{
	if (index < 0 || index >= g_config.admin_group_count)
		return NULL;
	return g_config.admin_groups[index];
}

int hostauth_idle_timeout_seconds(void)
{
	return g_config.idle_timeout_seconds;
}

enum hostauth_config_error hostauth_set_config(const char *const *admin_groups, int admin_group_count,
                                                int idle_timeout_seconds, int ldap_enabled,
                                                const char *const *ldap_servers, int ldap_server_count,
                                                int ldap_port, const char *ldap_base_dn)
{
	struct hostauth_config old = g_config;
	int i;

	if (admin_group_count < 0 || admin_group_count > HOSTAUTH_ADMIN_GROUPS_MAX)
		return HOSTAUTH_CONFIG_ERR_INVALID_FIELD;
	if (idle_timeout_seconds < 0)
		return HOSTAUTH_CONFIG_ERR_INVALID_FIELD;
	if (ldap_server_count < 0 || ldap_server_count > HOSTAUTH_LDAP_MAX_SERVERS)
		return HOSTAUTH_CONFIG_ERR_INVALID_FIELD;
	if (ldap_port < 1 || ldap_port > 65535)
		return HOSTAUTH_CONFIG_ERR_INVALID_FIELD;
	if (ldap_enabled && (ldap_server_count == 0 || ldap_base_dn == NULL || ldap_base_dn[0] == '\0'))
		return HOSTAUTH_CONFIG_ERR_INVALID_FIELD;

	memset(g_config.admin_groups, 0, sizeof(g_config.admin_groups));
	for (i = 0; i < admin_group_count; i++)
		snprintf(g_config.admin_groups[i], sizeof(g_config.admin_groups[0]), "%s", admin_groups[i]);
	g_config.admin_group_count = admin_group_count;
	g_config.idle_timeout_seconds = idle_timeout_seconds;

	g_config.ldap_enabled = ldap_enabled ? 1 : 0;
	memset(g_config.ldap_servers, 0, sizeof(g_config.ldap_servers));
	for (i = 0; i < ldap_server_count; i++)
		snprintf(g_config.ldap_servers[i], sizeof(g_config.ldap_servers[0]), "%s", ldap_servers[i]);
	g_config.ldap_server_count = ldap_server_count;
	g_config.ldap_port = ldap_port;
	snprintf(g_config.ldap_base_dn, sizeof(g_config.ldap_base_dn), "%s", ldap_base_dn != NULL ? ldap_base_dn : "");

	if (save_config() != 0) {
		g_config = old;
		return HOSTAUTH_CONFIG_ERR_PERSIST_FAILED;
	}
	return HOSTAUTH_CONFIG_OK;
}

int hostauth_rename_admin_group(const char *old_name, const char *new_name)
{
	struct hostauth_config old = g_config;
	int i, changed = 0;

	for (i = 0; i < g_config.admin_group_count; i++) {
		if (strcmp(g_config.admin_groups[i], old_name) == 0) {
			snprintf(g_config.admin_groups[i], sizeof(g_config.admin_groups[0]), "%s", new_name);
			changed = 1;
		}
	}
	if (!changed)
		return 1;

	if (save_config() != 0) {
		g_config = old;
		return 0;
	}
	return 1;
}

void hostauth_write_config_json(struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "admin_groups");
	jw_arr_open(w);
	for (i = 0; i < g_config.admin_group_count; i++)
		jw_str(w, g_config.admin_groups[i]);
	jw_arr_close(w);
	jw_key(w, "idle_timeout_seconds");
	jw_int(w, g_config.idle_timeout_seconds);
	jw_key(w, "ldap_enabled");
	jw_bool(w, g_config.ldap_enabled);
	jw_key(w, "ldap_servers");
	jw_arr_open(w);
	for (i = 0; i < g_config.ldap_server_count; i++)
		jw_str(w, g_config.ldap_servers[i]);
	jw_arr_close(w);
	jw_key(w, "ldap_port");
	jw_int(w, g_config.ldap_port);
	jw_key(w, "ldap_base_dn");
	jw_str(w, g_config.ldap_base_dn);
	jw_obj_close(w);
}

static int check_one_user(const char *username, void *ctx)
{
	int i;
	(void)ctx;

	for (i = 0; i < g_config.admin_group_count; i++) {
		if (ldap_user_is_in_group(username, g_config.admin_groups[i]))
			return 1;
	}
	return 0;
}

int hostauth_gating_active(void)
{
	/*
	 * A real, direct scan every call -- no cached "does an admin
	 * exist" flag to keep in sync with every possible LDAP user/group
	 * mutation (a disable, a group deletion, a group-membership
	 * change). Cheap (LDAP_USER_MAX is 256, this runs once per
	 * mutating request at most) and always correct, the same
	 * "recompute, don't cache" posture this project's own live
	 * /proc/mounts disk checks already established (ADR-0099).
	 * ldap_user_for_each() is the one real enumeration primitive this
	 * needs (daemon/src/ldap.c, added alongside it) -- reused as-is,
	 * not a second "walk every user" loop invented here.
	 */
	if (g_config.admin_group_count == 0)
		return 0;
	return ldap_user_for_each(check_one_user, NULL);
}

static void generate_token(char out[HOSTAUTH_TOKEN_LEN + 1])
{
	unsigned char raw[HOSTAUTH_TOKEN_LEN / 2];
	int fd;
	ssize_t n;
	size_t total = 0, i;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) {
		out[0] = '\0';
		return;
	}
	while (total < sizeof(raw)) {
		n = read(fd, raw + total, sizeof(raw) - total);
		if (n <= 0) {
			close(fd);
			out[0] = '\0';
			return;
		}
		total += (size_t)n;
	}
	close(fd);
	for (i = 0; i < sizeof(raw); i++)
		snprintf(out + i * 2, 3, "%02x", raw[i]);
}

/*
 * Builds the confirmed-working short-form bind DN ("cn=<username>,
 * ou=<primary-group-name>,<base_dn>" -- see ADR-0144's own comment on
 * why the short form binds correctly even though search results
 * report a longer canonical DN) from this daemon's own local ldap_user/
 * ldap_group records. Returns 0 and fills out_dn on success; -1 if
 * username names no local user, or that user's primarygroup names no
 * local group -- either way there's no DN to even attempt a bind
 * with, and the caller falls back to the local backend instead of
 * treating this as a network/protocol failure. */
static int build_login_bind_dn(const char *username, char *out_dn, size_t out_dn_size)
{
	const struct ldap_user *u = ldap_user_find(username);
	const struct ldap_group *g;

	if (u == NULL)
		return -1;
	g = ldap_group_find_by_gid(u->primarygroup);
	if (g == NULL)
		return -1;
	snprintf(out_dn, out_dn_size, "cn=%s,ou=%s,%s", username, g->name, g_config.ldap_base_dn);
	return 0;
}

/* Tries every configured LDAP server in order for one login attempt.
 * *out_answered is set to 1 iff some server gave an authoritative
 * bind-succeeded or bind-explicitly-rejected answer (in which case the
 * return value IS that answer); left at 0 (return value meaningless)
 * if every server was unreachable/erroring, or none are configured. */
static int try_ldap_login(const char *username, const char *password, int *out_answered)
{
	char dn[HOSTAUTH_USERNAME_MAX + HOSTAUTH_GROUP_NAME_MAX + HOSTAUTH_LDAP_BASE_DN_MAX + 8];
	int i;

	*out_answered = 0;
	if (build_login_bind_dn(username, dn, sizeof(dn)) != 0)
		return 0;

	for (i = 0; i < g_config.ldap_server_count; i++) {
		int result_code = -1;
		enum ldapclient_error err = ldapclient_bind(g_config.ldap_servers[i], g_config.ldap_port, dn,
		                                             password, HOSTAUTH_LDAP_TIMEOUT_MS, &result_code);

		if (err == LDAPCLIENT_OK) {
			*out_answered = 1;
			return 1;
		}
		if (err == LDAPCLIENT_ERR_LDAP_RESULT) {
			/* a real, well-formed rejection (invalidCredentials or
			 * similar) from a directory that IS reachable -- this is
			 * authoritative, not "try the next server" */
			*out_answered = 1;
			return 0;
		}
		/* LDAPCLIENT_ERR_CONNECT/PROTOCOL: this server didn't give a
		 * usable answer at all -- fall through and try the next one */
	}
	return 0;
}

enum hostauth_login_result hostauth_login(const char *username, const char *password,
                                           char out_token[HOSTAUTH_TOKEN_LEN + 1],
                                           int *out_expires_in_seconds)
{
	int i, slot = -1;
	time_t now = time(NULL);
	int authenticated;

	if (g_config.ldap_enabled) {
		int answered = 0;
		int ldap_result = try_ldap_login(username, password, &answered);

		authenticated = answered ? ldap_result : ldap_user_check_password(username, password);
	} else {
		authenticated = ldap_user_check_password(username, password);
	}
	if (!authenticated)
		return HOSTAUTH_LOGIN_INVALID_CREDENTIALS;

	/* Reap expired sessions opportunistically while looking for a free
	 * slot -- no separate timer/sweep needed for a table this small. */
	for (i = 0; i < HOSTAUTH_SESSION_MAX; i++) {
		if (g_sessions[i].in_use && g_config.idle_timeout_seconds > 0 &&
		    g_sessions[i].expires_at <= now)
			g_sessions[i].in_use = 0;
		if (!g_sessions[i].in_use && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return HOSTAUTH_LOGIN_TABLE_FULL;

	generate_token(g_sessions[slot].token);
	if (g_sessions[slot].token[0] == '\0')
		return HOSTAUTH_LOGIN_TABLE_FULL; /* /dev/urandom failure -- vanishingly rare, no
		                                    * dedicated error code for it, same posture
		                                    * ldap_generate_secret()'s own callers have */
	snprintf(g_sessions[slot].username, sizeof(g_sessions[slot].username), "%s", username);
	g_sessions[slot].expires_at = now + (time_t)g_config.idle_timeout_seconds;
	g_sessions[slot].in_use = 1;

	snprintf(out_token, HOSTAUTH_TOKEN_LEN + 1, "%s", g_sessions[slot].token);
	*out_expires_in_seconds = g_config.idle_timeout_seconds > 0 ? g_config.idle_timeout_seconds : 0;
	return HOSTAUTH_LOGIN_OK;
}

void hostauth_logout(const char *token)
{
	int i;

	if (token == NULL)
		return;
	for (i = 0; i < HOSTAUTH_SESSION_MAX; i++) {
		if (g_sessions[i].in_use && strcmp(g_sessions[i].token, token) == 0) {
			memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
			return;
		}
	}
}

int hostauth_check_token(const char *token, char *out_username, size_t out_username_size)
{
	int i;
	time_t now = time(NULL);

	if (token == NULL || token[0] == '\0')
		return 0;
	for (i = 0; i < HOSTAUTH_SESSION_MAX; i++) {
		if (!g_sessions[i].in_use || strcmp(g_sessions[i].token, token) != 0)
			continue;
		if (g_config.idle_timeout_seconds == 0) {
			/* Single-use: consumed here regardless of outcome, matching
			 * the documented "0 means every write re-authenticates"
			 * contract -- a token is never valid for a second request. */
			snprintf(out_username, out_username_size, "%s", g_sessions[i].username);
			memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
			return 1;
		}
		if (g_sessions[i].expires_at <= now) {
			memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
			return 0;
		}
		g_sessions[i].expires_at = now + (time_t)g_config.idle_timeout_seconds; /* sliding window */
		snprintf(out_username, out_username_size, "%s", g_sessions[i].username);
		return 1;
	}
	return 0;
}

int hostauth_authorize_write(const char *token)
{
	char username[HOSTAUTH_USERNAME_MAX];
	int i;

	if (!hostauth_gating_active())
		return 1;
	if (!hostauth_check_token(token, username, sizeof(username)))
		return 0;
	for (i = 0; i < g_config.admin_group_count; i++) {
		if (ldap_user_is_in_group(username, g_config.admin_groups[i]))
			return 1;
	}
	return 0;
}
