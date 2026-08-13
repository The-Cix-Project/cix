#include "hostauth.h"
#include "ldap.h"
#include "persist.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct hostauth_config {
	char admin_groups[HOSTAUTH_ADMIN_GROUPS_MAX][HOSTAUTH_GROUP_NAME_MAX];
	int admin_group_count;
	int idle_timeout_seconds; /* 0: no session reuse, every write re-authenticates */
};

static struct hostauth_config g_config = { { { 0 } }, 0, 900 };
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
	const struct json_value *jgroups, *jidle;

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
                                                int idle_timeout_seconds)
{
	struct hostauth_config old = g_config;
	int i;

	if (admin_group_count < 0 || admin_group_count > HOSTAUTH_ADMIN_GROUPS_MAX)
		return HOSTAUTH_CONFIG_ERR_INVALID_FIELD;
	if (idle_timeout_seconds < 0)
		return HOSTAUTH_CONFIG_ERR_INVALID_FIELD;

	memset(g_config.admin_groups, 0, sizeof(g_config.admin_groups));
	for (i = 0; i < admin_group_count; i++)
		snprintf(g_config.admin_groups[i], sizeof(g_config.admin_groups[0]), "%s", admin_groups[i]);
	g_config.admin_group_count = admin_group_count;
	g_config.idle_timeout_seconds = idle_timeout_seconds;

	if (save_config() != 0) {
		g_config = old;
		return HOSTAUTH_CONFIG_ERR_PERSIST_FAILED;
	}
	return HOSTAUTH_CONFIG_OK;
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

enum hostauth_login_result hostauth_login(const char *username, const char *password,
                                           char out_token[HOSTAUTH_TOKEN_LEN + 1],
                                           int *out_expires_in_seconds)
{
	int i, slot = -1;
	time_t now = time(NULL);

	if (!ldap_user_check_password(username, password))
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
