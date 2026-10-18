#include "hostauth.h"
#include "ldap.h"
#include "persist.h"
#include "pki.h"
#include "logstore.h"
#include "namecheck.h"
#include "connthrottle.h"
#include "pkicrypto.h"
#include "events.h"

/* ADR-0317: the contract's permission vocabulary (apigen --emit-permissions). */
#include "generated/permissions.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Bounds one login's live-LDAP work (connect + bind, per configured
 * server) -- long enough for a real LAN round trip, short enough that a
 * down server does not hold a login for long. Since ADR-0340 the wait is
 * in a helper child, so it holds that login and nothing else. */
#define HOSTAUTH_LDAP_TIMEOUT_MS 3000

struct hostauth_config {
	char admin_groups[HOSTAUTH_ADMIN_GROUPS_MAX][HOSTAUTH_GROUP_NAME_MAX];
	int admin_group_count;
	int idle_timeout_seconds; /* 0: no session reuse, every write re-authenticates */
	int ldap_enabled;
	char ldap_servers[HOSTAUTH_LDAP_MAX_SERVERS][HOSTAUTH_LDAP_HOST_MAX];
	int ldap_server_count;
	int ldap_port;
	/* #416: the daemon's OWN bind runs over TLS. Separate from
	 * ldap.c's client_tls, which configures the LDAP clients inside
	 * containers -- a different client population reaching a possibly
	 * different port, so one flag could never correctly serve both. */
	int ldap_tls;
	char ldap_base_dn[HOSTAUTH_LDAP_BASE_DN_MAX];
};

static struct hostauth_config g_config = { { { 0 } },      0, 900, 0, { { 0 } }, 0,
                                            HOSTAUTH_LDAP_DEFAULT_PORT, 0, { 0 } };
static char g_config_path[PATH_MAX];

/*
 * ADR-0317 (#540): the group -> permission mapping, the one stored
 * statement of who may do what. g_config.admin_groups above is a cache
 * DERIVED from it by derive_admin_groups() -- the groups holding every
 * permission -- so every existing admin-group function, and #370's
 * guards built on them, answer from the mapping unchanged.
 *
 * `has` is indexed by position in cix_permissions[], the vocabulary
 * apigen generates from the contract's x-cix-permissions: the same list
 * every operation's x-cix-permission was checked against, so "every
 * permission" means the same thing here as in the contract.
 */
struct perm_group {
	char name[HOSTAUTH_GROUP_NAME_MAX];
	unsigned char has[CIX_PERMISSION_COUNT];
};
static struct perm_group g_perm_groups[HOSTAUTH_PERM_GROUPS_MAX];
static int g_perm_group_count;

/*
 * ADR-0317 section 7 (#542): whether this host has already been given
 * the standard groups. Recorded so they are provisioned once, not on
 * every boot: a standard group the operator deleted stays deleted, the
 * same rule ADR-0315 and ADR-0316 apply to the other defaults a fresh
 * host is given. Persisted beside the mapping.
 */
static int g_standard_groups_provisioned;

static int perm_index(const char *word)
{
	int i;

	if (word == NULL)
		return -1;
	for (i = 0; i < CIX_PERMISSION_COUNT; i++) {
		if (strcmp(cix_permissions[i], word) == 0)
			return i;
	}
	return -1;
}

static int perm_group_holds_all(const struct perm_group *pg)
{
	int i;

	for (i = 0; i < CIX_PERMISSION_COUNT; i++) {
		if (!pg->has[i])
			return 0;
	}
	return 1;
}

static int perm_group_find(const struct perm_group *groups, int count, const char *name)
{
	int i;

	for (i = 0; i < count; i++) {
		if (strcmp(groups[i].name, name) == 0)
			return i;
	}
	return -1;
}

static int count_groups_holding_all(const struct perm_group *groups, int count)
{
	int i, n = 0;

	for (i = 0; i < count; i++)
		n += perm_group_holds_all(&groups[i]);
	return n;
}

/*
 * Rebuilds g_config.admin_groups from the mapping, in mapping order.
 * Every writer of the mapping calls this, so the cache cannot drift.
 * The writers refuse a change that would need more than
 * HOSTAUTH_ADMIN_GROUPS_MAX entries (HOSTAUTH_PERM_ERR_TOO_MANY_ADMIN_
 * GROUPS); a hand-edited state file is the only way past that, and is
 * truncated here with the reason on stderr.
 */
static void derive_admin_groups(void)
{
	int i;

	memset(g_config.admin_groups, 0, sizeof(g_config.admin_groups));
	g_config.admin_group_count = 0;
	for (i = 0; i < g_perm_group_count; i++) {
		if (!perm_group_holds_all(&g_perm_groups[i]))
			continue;
		if (g_config.admin_group_count >= HOSTAUTH_ADMIN_GROUPS_MAX) {
			fprintf(stderr,
			        "host auth: more than %d groups hold every permission; \"%s\" and any "
			        "after it are not admin groups until that is reduced\n",
			        HOSTAUTH_ADMIN_GROUPS_MAX, g_perm_groups[i].name);
			break;
		}
		snprintf(g_config.admin_groups[g_config.admin_group_count],
		         sizeof(g_config.admin_groups[0]), "%s", g_perm_groups[i].name);
		g_config.admin_group_count++;
	}
}

/* Defined with the mapping's own operations, at the end of this file. */
static int any_user_holds(const struct perm_group *groups, int count, int idx);

static void write_mapping_groups(struct json_writer *w)
{
	int i, j;

	jw_obj_open(w);
	for (i = 0; i < g_perm_group_count; i++) {
		jw_key(w, g_perm_groups[i].name);
		jw_arr_open(w);
		for (j = 0; j < CIX_PERMISSION_COUNT; j++) {
			if (g_perm_groups[i].has[j])
				jw_str(w, cix_permissions[j]);
		}
		jw_arr_close(w);
	}
	jw_obj_close(w);
}

struct hostauth_session {
	char token_hash[65]; /* sha256 of the token, hex -- the token itself is never kept (#562) */
	char username[HOSTAUTH_USERNAME_MAX];
	time_t expires_at; /* meaningless (never consulted) when idle_timeout_seconds == 0 */
	time_t logged_in_at;                     /* when hostauth_issue_session() issued it */
	char source_ip[HOSTAUTH_SOURCE_IP_MAX]; /* the client that logged in; "" if unknown */
	time_t saved_expires_at; /* expires_at as last written to the sessions file */
	int in_use;
};

static struct hostauth_session g_sessions[HOSTAUTH_SESSION_MAX];

/*
 * #562: a login survives a cixd restart. The table used to live in
 * memory only, so every restart -- every release deployed, every
 * reboot -- signed out every browser and every cixctl at once, and the
 * dashboard went on showing "logged in" over panels that were now
 * refused. Measured on 192.168.15.95, 2026-10-02: a token answering
 * whoami "authenticated":true before `cixctl reboot` answered false
 * after it.
 *
 * The file sits beside hostauth_config.json and holds, per session,
 * sha256(token) -- never the token, so reading the file does not give
 * anyone a session -- with the user, the times and the source address.
 * It is not part of a system backup: a restored host starts with no
 * sessions, as a new one does.
 *
 * Written on login, logout and revoke, and as a session's idle window
 * slides only once it has moved SESSION_SAVE_SLACK seconds past what
 * the file says. A crash can therefore cut a live session's window
 * short by at most that much; it never extends one.
 */
#define SESSION_SAVE_SLACK 60

static char g_sessions_path[PATH_MAX];

/* What load_sessions() found, for hostauth_log_carried_sessions(): the
 * number carried, or -1 for a malformed file. init runs before the log
 * store opens, so it cannot say this where it happens. */
static int g_sessions_carried;

static void sessions_path_from(const char *config_path)
{
	const char *slash = strrchr(config_path, '/');

	if (slash == NULL)
		snprintf(g_sessions_path, sizeof(g_sessions_path), "hostauth_sessions.json");
	else
		snprintf(g_sessions_path, sizeof(g_sessions_path), "%.*s/hostauth_sessions.json",
		         (int)(slash - config_path), config_path);
}

static int token_hash(const char *token, char out[65])
{
	if (token == NULL || token[0] == '\0')
		return -1;
	return pkicrypto_sha256_hex(token, strlen(token), out, 65);
}

/* The slot holding `token`, or -1. */
static int find_session(const char *token)
{
	char h[65];
	int i;

	if (token_hash(token, h) != 0)
		return -1;
	for (i = 0; i < HOSTAUTH_SESSION_MAX; i++)
		if (g_sessions[i].in_use && strcmp(g_sessions[i].token_hash, h) == 0)
			return i;
	return -1;
}

static void save_sessions(void)
{
	struct json_writer w;
	time_t now = time(NULL);
	int i;

	if (g_sessions_path[0] == '\0')
		return;
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "sessions");
	jw_arr_open(&w);
	for (i = 0; i < HOSTAUTH_SESSION_MAX; i++) {
		if (!g_sessions[i].in_use ||
		    (g_config.idle_timeout_seconds > 0 && g_sessions[i].expires_at <= now))
			continue;
		jw_obj_open(&w);
		jw_key(&w, "token_sha256");
		jw_str(&w, g_sessions[i].token_hash);
		jw_key(&w, "username");
		jw_str(&w, g_sessions[i].username);
		jw_key(&w, "expires_at");
		jw_int(&w, (long long)g_sessions[i].expires_at);
		jw_key(&w, "logged_in_at");
		jw_int(&w, (long long)g_sessions[i].logged_in_at);
		jw_key(&w, "source_ip");
		jw_str(&w, g_sessions[i].source_ip);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	if (persist_atomic_write(g_sessions_path, w.buf, w.len) == 0) {
		chmod(g_sessions_path, 0600);
		for (i = 0; i < HOSTAUTH_SESSION_MAX; i++)
			g_sessions[i].saved_expires_at = g_sessions[i].expires_at;
	} else {
		logstore_write("hostauth", "error",
		               "could not write %s -- sessions will not survive the next restart",
		               g_sessions_path);
	}
	jw_free(&w);
}

/*
 * ADR-0339: the one way a session leaves the table, so every departure
 * -- a logout, a revoke, a single-use token being spent, an expiry --
 * is announced. Saving stays with the caller, which may drop several
 * before writing once.
 */
static void drop_session(int i)
{
	memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
	events_announce(EV_listHostauthSessions);
}

static void expiry_may_be_sooner(void);

/* Slides slot i's idle window, writing the file only when it has moved enough. */
static void slide_session(int i, time_t now)
{
	g_sessions[i].expires_at = now + (time_t)g_config.idle_timeout_seconds;
	if (g_sessions[i].expires_at - g_sessions[i].saved_expires_at >= SESSION_SAVE_SLACK)
		save_sessions();
}

static void load_sessions(void)
{
	char *buf = NULL;
	size_t len = 0, k;
	struct json_value *root;
	const struct json_value *arr;
	time_t now = time(NULL);
	int n = 0;

	g_sessions_carried = 0;
	if (persist_read_file(g_sessions_path, &buf, &len) != 0 || buf == NULL)
		return;
	root = json_parse(buf, len);
	free(buf);
	arr = root != NULL ? json_object_get(root, "sessions") : NULL;
	if (arr == NULL || arr->type != JSON_ARRAY) {
		g_sessions_carried = -1;
		json_free(root);
		return;
	}
	for (k = 0; k < arr->u.array.count && n < HOSTAUTH_SESSION_MAX; k++) {
		const struct json_value *o = arr->u.array.items[k];
		const char *h = json_as_string(json_object_get(o, "token_sha256"));
		const char *user = json_as_string(json_object_get(o, "username"));
		const char *ip = json_as_string(json_object_get(o, "source_ip"));
		const struct json_value *jexp = json_object_get(o, "expires_at");
		const struct json_value *jin = json_object_get(o, "logged_in_at");
		struct hostauth_session *s = &g_sessions[n];

		if (h == NULL || strlen(h) != 64 || strspn(h, "0123456789abcdef") != 64 ||
		    user == NULL || user[0] == '\0' || strlen(user) >= sizeof(s->username) ||
		    jexp == NULL || jin == NULL)
			continue;
		memset(s, 0, sizeof(*s));
		snprintf(s->token_hash, sizeof(s->token_hash), "%s", h);
		snprintf(s->username, sizeof(s->username), "%s", user);
		snprintf(s->source_ip, sizeof(s->source_ip), "%s", ip != NULL ? ip : "");
		s->expires_at = (time_t)json_as_number(jexp);
		s->logged_in_at = (time_t)json_as_number(jin);
		s->saved_expires_at = s->expires_at;
		if (g_config.idle_timeout_seconds > 0 && s->expires_at <= now)
			continue; /* lapsed while cixd was down */
		s->in_use = 1;
		n++;
	}
	json_free(root);
	g_sessions_carried = n;
}

static void write_config_fields(struct json_writer *w);

/*
 * The state file holds the mapping ("permissions") AND the derived
 * admin_groups. The mapping is what this build reads back; admin_groups
 * is kept in the file for one reason: a box that rolls back to its
 * other A/B slot runs a build older than #540, which reads only
 * admin_groups -- without it, that build would see no admin groups,
 * and with no admin groups gating is off. This build never reads
 * admin_groups back while "permissions" is present.
 */
static int save_config(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	write_config_fields(&w);
	jw_key(&w, "permissions");
	write_mapping_groups(&w);
	jw_key(&w, "standard_groups_provisioned");
	jw_bool(&w, g_standard_groups_provisioned);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_config_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int load_config(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *jgroups, *jidle, *jldapen, *jldapservers, *jldapport, *jldaptls,
	    *jldapbasedn;

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
	/* #416: absent in older state files -- plaintext then, which is what
	 * those boxes were actually doing, so no migration. */
	jldaptls = json_object_get(root, "ldap_tls");
	if (jldaptls != NULL && jldaptls->type == JSON_BOOL)
		g_config.ldap_tls = jldaptls->u.boolean;
	jldapbasedn = json_object_get(root, "ldap_base_dn");
	if (jldapbasedn != NULL && json_as_string(jldapbasedn) != NULL)
		snprintf(g_config.ldap_base_dn, sizeof(g_config.ldap_base_dn), "%s", json_as_string(jldapbasedn));

	/* #542: absent in every file written before it, which is what makes
	 * the first boot of that build provision the standard groups. */
	{
		const struct json_value *jprov = json_object_get(root, "standard_groups_provisioned");

		g_standard_groups_provisioned = jprov != NULL && jprov->type == JSON_BOOL && jprov->u.boolean;
	}

	/*
	 * ADR-0317 (#540): the mapping, or the migration into it.
	 *
	 * With "permissions" present it is the truth and admin_groups is
	 * re-derived from it below; the file's own admin_groups is only
	 * there for an older build after a rollback (see save_config()).
	 *
	 * Without it, the file predates #540, and every group in its
	 * admin_groups is granted every permission -- nobody gains or
	 * loses anything, and no operator step is needed (ADR-0317
	 * section 7). Written back at once, so the migration happens once.
	 */
	g_perm_group_count = 0;
	memset(g_perm_groups, 0, sizeof(g_perm_groups));
	{
		const struct json_value *jperms = json_object_get(root, "permissions");
		int migrated = 0;
		size_t i, j;

		if (jperms != NULL && jperms->type == JSON_OBJECT) {
			for (i = 0; i < jperms->u.object.count &&
			            g_perm_group_count < HOSTAUTH_PERM_GROUPS_MAX;
			     i++) {
				const struct json_value *words = jperms->u.object.values[i];
				struct perm_group *pg = &g_perm_groups[g_perm_group_count];

				if (!simple_name_is_valid(jperms->u.object.keys[i], HOSTAUTH_GROUP_NAME_MAX) ||
				    words == NULL || words->type != JSON_ARRAY)
					continue;
				snprintf(pg->name, sizeof(pg->name), "%s", jperms->u.object.keys[i]);
				for (j = 0; j < words->u.array.count; j++) {
					int idx = perm_index(json_as_string(words->u.array.items[j]));

					/* A word this build's vocabulary lacks -- written
					 * by a newer build, or a hand edit -- grants
					 * nothing here and is dropped at the next save. */
					if (idx >= 0)
						pg->has[idx] = 1;
					else
						fprintf(stderr,
						        "host auth: group \"%s\" names \"%s\", which is not a "
						        "permission in this build; ignored\n",
						        pg->name,
						        json_as_string(words->u.array.items[j]) != NULL
						                ? json_as_string(words->u.array.items[j])
						                : "(not a string)");
				}
				g_perm_group_count++;
			}
		} else {
			int k;

			for (k = 0; k < g_config.admin_group_count; k++) {
				struct perm_group *pg = &g_perm_groups[g_perm_group_count++];

				snprintf(pg->name, sizeof(pg->name), "%s", g_config.admin_groups[k]);
				memset(pg->has, 1, sizeof(pg->has));
			}
			migrated = g_config.admin_group_count > 0;
		}
		derive_admin_groups();
		json_free(root);
		if (migrated) {
			fprintf(stderr,
			        "host auth: migrated %d admin group(s) into the permission mapping, each "
			        "granted every permission (ADR-0317)\n",
			        g_perm_group_count);
			if (save_config() != 0)
				fprintf(stderr, "host auth: could not write the migrated mapping back; it "
				                "will be derived again at the next start\n");
		}
	}
	return 0;
}

int hostauth_init(const char *config_path)
{
	snprintf(g_config_path, sizeof(g_config_path), "%s", config_path);
	sessions_path_from(config_path);
	memset(g_sessions, 0, sizeof(g_sessions));
	if (load_config() != 0)
		return -1;
	/* After the config: the idle timeout decides which sessions lapsed. */
	load_sessions();
	return 0;
}

void hostauth_repoint(const char *new_config_path)
{
	snprintf(g_config_path, sizeof(g_config_path), "%s", new_config_path);
	sessions_path_from(new_config_path);
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

/* ADR-0148: the one real, canonical source of truth for the LDAP base
 * DN -- ldap.c's own config-file renderer uses this to keep a
 * registered glauth server's own baseDN in sync, instead of it being
 * a fourth independently-typed copy (hostauth-config, the server's
 * own glauth.cfg, and every client container's own nslcd.conf/
 * ldap-authkeys.conf were the other three -- only the first of those
 * four is actually fixed by this change; see that ADR for why the
 * other two stay deliberately manual). Empty ("") when unset, same as
 * every other string field here -- never NULL. */
const char *hostauth_ldap_base_dn(void)
{
	return g_config.ldap_base_dn;
}

enum hostauth_config_error hostauth_set_config(const char *const *admin_groups, int admin_group_count,
                                                int idle_timeout_seconds, int ldap_enabled,
                                                const char *const *ldap_servers, int ldap_server_count,
                                                int ldap_port, int ldap_tls, const char *ldap_base_dn)
{
	struct hostauth_config old = g_config;
	struct perm_group old_groups[HOSTAUTH_PERM_GROUPS_MAX];
	int old_group_count;
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
	/* #416: the same "enabled with nothing to bind against can never be
	 * a valid saved state" rule as the line above. TLS verification
	 * against this host's own CA is impossible before that CA exists,
	 * and accepting the config would mean every login silently failing
	 * its handshake and falling back -- a saved state that looks
	 * correct and does nothing it says. Checked only when ldap_enabled,
	 * so the flag can be set ahead of enabling the backend. */
	if (ldap_enabled && ldap_tls && !pki_ca_bootstrapped())
		return HOSTAUTH_CONFIG_ERR_NO_CA;

	/*
	 * ADR-0317 (#540): admin_groups is read into the mapping, never
	 * stored beside it. A named group is granted every permission; a
	 * group that held every permission and is no longer named loses
	 * its grants entirely. A group granted only some permissions is
	 * neither, and is left as it is.
	 */
	memcpy(old_groups, g_perm_groups, sizeof(old_groups));
	old_group_count = g_perm_group_count;
	for (i = 0; i < g_perm_group_count;) {
		int named = 0;
		int k;

		for (k = 0; k < admin_group_count; k++)
			named |= strcmp(admin_groups[k], g_perm_groups[i].name) == 0;
		if (!named && perm_group_holds_all(&g_perm_groups[i])) {
			g_perm_groups[i] = g_perm_groups[--g_perm_group_count];
			memset(&g_perm_groups[g_perm_group_count], 0, sizeof(g_perm_groups[0]));
			continue;
		}
		i++;
	}
	for (i = 0; i < admin_group_count; i++) {
		int at = perm_group_find(g_perm_groups, g_perm_group_count, admin_groups[i]);

		if (at < 0) {
			if (g_perm_group_count >= HOSTAUTH_PERM_GROUPS_MAX ||
			    !simple_name_is_valid(admin_groups[i], HOSTAUTH_GROUP_NAME_MAX)) {
				memcpy(g_perm_groups, old_groups, sizeof(old_groups));
				g_perm_group_count = old_group_count;
				return HOSTAUTH_CONFIG_ERR_INVALID_FIELD;
			}
			at = g_perm_group_count++;
			snprintf(g_perm_groups[at].name, sizeof(g_perm_groups[at].name), "%s",
			         admin_groups[i]);
		}
		memset(g_perm_groups[at].has, 1, sizeof(g_perm_groups[at].has));
	}
	/*
	 * #541 (ADR-0317 section 6): the translation above can take
	 * identity:write away from everyone -- admin_groups [] on a box
	 * whose only holders are in a full group. Judged on the mapping
	 * just built, against the one in force, the same test
	 * hostauth_set_group_permissions() applies. It replaces #370's
	 * separate "would these admin_groups gate" pre-check in the
	 * handler, which asked about admin groups rather than about who
	 * can still grant permissions.
	 */
	{
		int idx = perm_index("identity:write");

		if (idx >= 0 && any_user_holds(old_groups, old_group_count, idx) &&
		    !any_user_holds(g_perm_groups, g_perm_group_count, idx)) {
			memcpy(g_perm_groups, old_groups, sizeof(old_groups));
			g_perm_group_count = old_group_count;
			return HOSTAUTH_CONFIG_ERR_LOCKOUT;
		}
	}
	derive_admin_groups();
	g_config.idle_timeout_seconds = idle_timeout_seconds;

	g_config.ldap_enabled = ldap_enabled ? 1 : 0;
	memset(g_config.ldap_servers, 0, sizeof(g_config.ldap_servers));
	for (i = 0; i < ldap_server_count; i++)
		snprintf(g_config.ldap_servers[i], sizeof(g_config.ldap_servers[0]), "%s", ldap_servers[i]);
	g_config.ldap_server_count = ldap_server_count;
	g_config.ldap_port = ldap_port;
	g_config.ldap_tls = ldap_tls ? 1 : 0;
	snprintf(g_config.ldap_base_dn, sizeof(g_config.ldap_base_dn), "%s", ldap_base_dn != NULL ? ldap_base_dn : "");

	if (save_config() != 0) {
		g_config = old;
		memcpy(g_perm_groups, old_groups, sizeof(old_groups));
		g_perm_group_count = old_group_count;
		return HOSTAUTH_CONFIG_ERR_PERSIST_FAILED;
	}
	if (g_config.idle_timeout_seconds != old.idle_timeout_seconds)
		expiry_may_be_sooner();
	return HOSTAUTH_CONFIG_OK;
}

int hostauth_rename_admin_group(const char *old_name, const char *new_name)
{
	/*
	 * ADR-0317 (#540): the mapping is keyed by group name, so a rename
	 * moves the group's grants with it -- every group's, not only an
	 * admin group's, or renaming a partly-privileged group would
	 * silently strip it. admin_groups is re-derived.
	 */
	struct hostauth_config old = g_config;
	struct perm_group old_entry;
	int at = perm_group_find(g_perm_groups, g_perm_group_count, old_name);

	if (at < 0)
		return 1;
	old_entry = g_perm_groups[at];
	snprintf(g_perm_groups[at].name, sizeof(g_perm_groups[at].name), "%s", new_name);
	derive_admin_groups();
	if (save_config() != 0) {
		g_perm_groups[at] = old_entry;
		g_config = old;
		return 0;
	}
	return 1;
}

void hostauth_write_config_json(struct json_writer *w)
{
	jw_obj_open(w);
	write_config_fields(w);
	jw_obj_close(w);
}

static void write_config_fields(struct json_writer *w)
{
	int i;

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
	jw_key(w, "ldap_tls");
	jw_bool(w, g_config.ldap_tls);
	jw_key(w, "ldap_base_dn");
	jw_str(w, g_config.ldap_base_dn);
	/*
	 * #370: the computed answer, not just the configuration. An admin
	 * group can be configured and still hold nobody, in which case
	 * every mutating request is permitted -- and the four fields above
	 * look entirely correct while that is true. This is the field that
	 * says whether authentication is actually in force.
	 */
	jw_key(w, "gating_active");
	jw_bool(w, hostauth_gating_active());
}

/*
 * Gating and its invariant (ADR-0317 section 6, #541) -- see the
 * header for the rule. Every predicate here is a direct scan of the
 * user table against the mapping, recomputed per call rather than
 * cached, for the reason #370 gave: nothing then has to be kept in
 * sync with every possible user, group or mapping change.
 *
 * #370 asked the same questions of admin groups. Since #540 an admin
 * group is only a group holding every permission, and what actually
 * decides whether anyone can still administer the box is whether
 * anyone holds identity:write -- the permission that grants the rest.
 * So that is what these ask.
 */
int hostauth_gating_active(void)
{
	int idx = perm_index("identity:write");

	return idx >= 0 && any_user_holds(g_perm_groups, g_perm_group_count, idx);
}

/* Holds permission idx through a group other than skip_group, as a
 * user other than skip_user (either may be NULL). */
struct other_holder {
	const char *skip_user;
	const char *skip_group;
	int idx;
};

static int holds_elsewhere(const char *username, void *ctx)
{
	const struct other_holder *q = ctx;
	int i;

	if (q->skip_user != NULL && strcmp(username, q->skip_user) == 0)
		return 0;
	for (i = 0; i < g_perm_group_count; i++) {
		if (!g_perm_groups[i].has[q->idx])
			continue;
		if (q->skip_group != NULL && strcmp(g_perm_groups[i].name, q->skip_group) == 0)
			continue;
		if (ldap_user_is_in_group(username, g_perm_groups[i].name))
			return 1; /* stops the walk: one is enough */
	}
	return 0;
}

/*
 * Would a user carrying exactly these gids hold permission idx? Goes
 * group name -> gid, since the mapping names groups and a user carries
 * gids; that needs no reverse lookup and no second copy of the
 * membership rule.
 */
static int gids_hold(int idx, int primarygroup, const int *secondary_groups, int secondary_count,
                     int disabled)
{
	int i, j;

	if (disabled)
		return 0;
	for (i = 0; i < g_perm_group_count; i++) {
		const struct ldap_group *g;

		if (!g_perm_groups[i].has[idx])
			continue;
		g = ldap_group_find(g_perm_groups[i].name);
		if (g == NULL)
			continue;
		if (primarygroup == g->gidnumber)
			return 1;
		for (j = 0; j < secondary_count; j++) {
			if (secondary_groups[j] == g->gidnumber)
				return 1;
		}
	}
	return 0;
}

int hostauth_user_change_keeps_grantor(const char *username, int primarygroup,
                                       const int *secondary_groups, int secondary_count,
                                       int disabled, int deleting)
{
	struct other_holder q;
	int idx = perm_index("identity:write");

	if (idx < 0 || !hostauth_gating_active())
		return 1;
	q.skip_user = username;
	q.skip_group = NULL;
	q.idx = idx;
	if (ldap_user_for_each(holds_elsewhere, &q))
		return 1;
	return !deleting && gids_hold(idx, primarygroup, secondary_groups, secondary_count, disabled);
}

int hostauth_group_change_keeps_grantor(const char *group)
{
	struct other_holder q;
	int idx = perm_index("identity:write");

	if (idx < 0 || !hostauth_gating_active())
		return 1;
	q.skip_user = NULL;
	q.skip_group = group;
	q.idx = idx;
	return ldap_user_for_each(holds_elsewhere, &q);
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

int hostauth_ldap_plan(const char *username, struct hostauth_ldap_plan *out)
{
	int i;

	memset(out, 0, sizeof(*out));
	if (!g_config.ldap_enabled ||
	    build_login_bind_dn(username, out->bind_dn, sizeof(out->bind_dn)) != 0)
		return 0;
	for (i = 0; i < g_config.ldap_server_count && i < HOSTAUTH_LDAP_MAX_SERVERS; i++)
		snprintf(out->servers[i], sizeof(out->servers[i]), "%s", g_config.ldap_servers[i]);
	out->server_count = i;
	out->port = g_config.ldap_port;
	out->tls = g_config.ldap_tls;
	out->timeout_ms = HOSTAUTH_LDAP_TIMEOUT_MS;
	return 1;
}

enum hostauth_login_result hostauth_issue_session(const char *username,
                                                   char out_token[HOSTAUTH_TOKEN_LEN + 1],
                                                   int *out_expires_in_seconds,
                                                   const char *source_ip)
{
	int i, slot = -1;
	time_t now = time(NULL);

	/* Expired sessions are normally gone already (hostauth_reap_expired()
	 * runs at each expiry); one whose timer has not fired yet is reaped
	 * here rather than holding a slot a login needs. */
	for (i = 0; i < HOSTAUTH_SESSION_MAX; i++) {
		if (g_sessions[i].in_use && g_config.idle_timeout_seconds > 0 &&
		    g_sessions[i].expires_at <= now)
			drop_session(i);
		if (!g_sessions[i].in_use && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return HOSTAUTH_LOGIN_TABLE_FULL;

	generate_token(out_token);
	if (out_token[0] == '\0' || token_hash(out_token, g_sessions[slot].token_hash) != 0) {
		out_token[0] = '\0';
		return HOSTAUTH_LOGIN_TABLE_FULL; /* /dev/urandom failure -- vanishingly rare, no
		                                    * dedicated error code for it, same posture
		                                    * ldap_generate_secret()'s own callers have */
	}
	snprintf(g_sessions[slot].username, sizeof(g_sessions[slot].username), "%s", username);
	g_sessions[slot].expires_at = now + (time_t)g_config.idle_timeout_seconds;
	g_sessions[slot].logged_in_at = now;
	snprintf(g_sessions[slot].source_ip, sizeof(g_sessions[slot].source_ip), "%s",
	         source_ip != NULL ? source_ip : "");
	g_sessions[slot].in_use = 1;
	save_sessions();
	events_announce(EV_listHostauthSessions);
	expiry_may_be_sooner();

	*out_expires_in_seconds = g_config.idle_timeout_seconds > 0 ? g_config.idle_timeout_seconds : 0;
	return HOSTAUTH_LOGIN_OK;
}

void hostauth_logout(const char *token)
{
	int i = find_session(token);

	if (i < 0)
		return;
	drop_session(i);
	save_sessions();
}

/*
 * {"sessions":[{"username":...,"expires_in_seconds":<int or null>,
 *   "logged_in_at":<unix seconds>,"source_ip":<string or null>}, ...]}
 * -- every currently active session, real admin visibility into a
 * table that previously had none at all (no endpoint, no CLI, no web
 * panel). The raw token is never included, before or after issuance --
 * only hostauth_issue_session()'s one-time login response carries it. Every
 * live entry is listed: an expiry is a change hostauth_reap_expired()
 * makes and announces (ADR-0339), not one this reader makes by
 * consulting the clock, which is what let the list change with no
 * announcement. An entry in the moment between its expiry and the
 * reap reports 0 rather than a negative number. idle_timeout_seconds
 * == 0 sessions (single-use,
 * consumed on next check) report null -- there is no meaningful
 * "expires in" for a session that's valid for exactly one more
 * request, whenever that happens to be.
 */
void hostauth_write_sessions_json(struct json_writer *w)
{
	int i;
	time_t now = time(NULL);

	jw_arr_open(w);
	for (i = 0; i < HOSTAUTH_SESSION_MAX; i++) {
		if (!g_sessions[i].in_use)
			continue;

		jw_obj_open(w);
		jw_key(w, "username");
		jw_str(w, g_sessions[i].username);
		jw_key(w, "expires_in_seconds");
		if (g_config.idle_timeout_seconds > 0)
			jw_int(w, g_sessions[i].expires_at > now ? (long long)(g_sessions[i].expires_at - now) : 0);
		else
			jw_null(w);
		jw_key(w, "logged_in_at");
		jw_int(w, (long long)g_sessions[i].logged_in_at);
		jw_key(w, "source_ip");
		if (g_sessions[i].source_ip[0] != '\0')
			jw_str(w, g_sessions[i].source_ip);
		else
			jw_null(w);
		jw_obj_close(w);
	}
	jw_arr_close(w);
}

static void (*g_expiry_listener)(void);

void hostauth_set_expiry_listener(void (*fn)(void))
{
	g_expiry_listener = fn;
}

static void expiry_may_be_sooner(void)
{
	if (g_expiry_listener != NULL)
		g_expiry_listener();
}

long long hostauth_next_expiry(void)
{
	long long soonest = 0;
	int i;

	if (g_config.idle_timeout_seconds <= 0)
		return 0;
	for (i = 0; i < HOSTAUTH_SESSION_MAX; i++)
		if (g_sessions[i].in_use &&
		    (soonest == 0 || (long long)g_sessions[i].expires_at < soonest))
			soonest = (long long)g_sessions[i].expires_at;
	return soonest;
}

int hostauth_reap_expired(void)
{
	time_t now = time(NULL);
	int i, dropped = 0;

	if (g_config.idle_timeout_seconds <= 0)
		return 0;
	for (i = 0; i < HOSTAUTH_SESSION_MAX; i++) {
		if (g_sessions[i].in_use && g_sessions[i].expires_at <= now) {
			drop_session(i);
			dropped++;
		}
	}
	if (dropped > 0)
		save_sessions();
	return dropped;
}

/*
 * Revokes every currently active session belonging to username --
 * "log this account out everywhere," the meaningful admin action for
 * a table that can hold more than one concurrent session per user
 * (no per-session opaque ID is exposed at all, so there is no
 * "revoke just this one" -- the raw token is the only real per-session
 * identifier, and that is never surfaced past its one-time login
 * response). Returns the number of sessions actually revoked.
 */
int hostauth_revoke_sessions_for_user(const char *username)
{
	int i, revoked = 0;

	if (username == NULL || username[0] == '\0')
		return 0;
	for (i = 0; i < HOSTAUTH_SESSION_MAX; i++) {
		if (g_sessions[i].in_use && strcmp(g_sessions[i].username, username) == 0) {
			drop_session(i);
			revoked++;
		}
	}
	if (revoked > 0)
		save_sessions();
	return revoked;
}

int hostauth_check_token(const char *token, char *out_username, size_t out_username_size)
{
	time_t now = time(NULL);
	int i = find_session(token);

	if (i < 0)
		return 0;
	if (g_config.idle_timeout_seconds == 0) {
		/* Single-use: consumed here regardless of outcome, matching
		 * the documented "0 means every write re-authenticates"
		 * contract -- a token is never valid for a second request. */
		snprintf(out_username, out_username_size, "%s", g_sessions[i].username);
		drop_session(i);
		save_sessions();
		return 1;
	}
	if (g_sessions[i].expires_at <= now) {
		drop_session(i);
		save_sessions();
		return 0;
	}
	slide_session(i, now); /* sliding window */
	snprintf(out_username, out_username_size, "%s", g_sessions[i].username);
	return 1;
}

/*
 * #379: a client that only reads is still a client.
 *
 * The idle window slid only in hostauth_check_token(), which runs on
 * the WRITE gate. A client that logged in, started a long job and then
 * polled `GET /v1/pkg/{name}` every ten seconds was therefore idle by
 * this module's reckoning the whole time, and was logged out mid-job --
 * measured on 192.168.15.95 at the fifteen-minute mark, which is
 * exactly the default timeout.
 *
 * "Idle" has to mean "not talking to the daemon", not "not writing".
 * So the per-request path touches the session after it has peeked a
 * valid token, for any method.
 *
 * Deliberately NOT folded into hostauth_peek_token(): that function is
 * also `GET /v1/whoami`'s introspection ("is my token still valid"),
 * and an answer to a question should not change the thing it answers
 * about. Separate function, called from the one place that knows a real
 * request is being served.
 *
 * A no-op under idle_timeout_seconds == 0, where sessions are
 * single-use and expires_at is meaningless by contract.
 */
void hostauth_touch_token(const char *token)
{
	time_t now = time(NULL);
	int i;

	if (token == NULL || token[0] == '\0' || g_config.idle_timeout_seconds == 0)
		return;
	i = find_session(token);
	if (i < 0 || g_sessions[i].expires_at <= now)
		return; /* unknown, or already lapsed -- reaping stays with the real check */
	slide_session(i, now);
}

int hostauth_peek_token(const char *token, char *out_username, size_t out_username_size)
{
	time_t now = time(NULL);
	int i = find_session(token);

	if (i < 0)
		return 0;
	if (g_config.idle_timeout_seconds != 0 && g_sessions[i].expires_at <= now)
		return 0; /* expired -- leave reaping it to the next real check */
	snprintf(out_username, out_username_size, "%s", g_sessions[i].username);
	return 1;
}

/*
 * The one session decision (ADR-0317), shared by the two entry points
 * below so they cannot disagree: they differ only in how the token is
 * looked up. `lookup` is hostauth_check_token() or hostauth_peek_token().
 */
static enum hostauth_authz authorize_session(const char *token, const char *permission,
                                             int (*lookup)(const char *, char *, size_t))
{
	char username[HOSTAUTH_USERNAME_MAX];

	if (permission == NULL)
		return HOSTAUTH_AUTHZ_FORBIDDEN;
	if (strcmp(permission, "public") == 0 || !hostauth_gating_active())
		return HOSTAUTH_AUTHZ_OK;
	if (!lookup(token, username, sizeof(username)))
		return HOSTAUTH_AUTHZ_NO_SESSION;
	if (strcmp(permission, "authenticated") == 0 ||
	    hostauth_user_has_permission(username, permission))
		return HOSTAUTH_AUTHZ_OK;
	return HOSTAUTH_AUTHZ_FORBIDDEN;
}

enum hostauth_authz hostauth_authorize(const char *token, const char *permission)
{
	return authorize_session(token, permission, hostauth_check_token);
}

/*
 * ADR-0339: the same decision, without spending the token. The events
 * stream asks it for every announced read when a stream opens, and
 * again at every heartbeat; under a single-use config
 * (idle_timeout_seconds: 0) hostauth_check_token() consumes the token
 * it checks, so asking through it would sign the operator out on the
 * first question.
 */
enum hostauth_authz hostauth_authorize_peek(const char *token, const char *permission)
{
	return authorize_session(token, permission, hostauth_peek_token);
}

/*
 * ---- ADR-0317 (#540): the permission mapping's own operations ----
 */

int hostauth_permission_is_known(const char *permission)
{
	return perm_index(permission) >= 0;
}

/* Whether username holds permission (by index) through any group in
 * the given mapping -- the current one, or a proposed one being judged
 * by a guard before it is applied. */
static int user_holds_under(const struct perm_group *groups, int count, const char *username,
                            int idx)
{
	int i;

	for (i = 0; i < count; i++) {
		if (groups[i].has[idx] && ldap_user_is_in_group(username, groups[i].name))
			return 1;
	}
	return 0;
}

int hostauth_user_has_permission(const char *username, const char *permission)
{
	int idx = perm_index(permission);

	if (username == NULL || idx < 0)
		return 0;
	return user_holds_under(g_perm_groups, g_perm_group_count, username, idx);
}

struct holder_query {
	const struct perm_group *groups;
	int count;
	int idx;
};

static int one_holder(const char *username, void *ctx)
{
	const struct holder_query *q = ctx;

	return user_holds_under(q->groups, q->count, username, q->idx); /* 1 stops the walk */
}

/* Whether any user holds the permission under the given mapping. */
static int any_user_holds(const struct perm_group *groups, int count, int idx)
{
	struct holder_query q;

	q.groups = groups;
	q.count = count;
	q.idx = idx;
	return ldap_user_for_each(one_holder, &q);
}

enum hostauth_perm_error hostauth_set_group_permissions(const char *group, const char *const *perms,
                                                         int count, char *detail,
                                                         size_t detail_size)
{
	struct perm_group proposed[HOSTAUTH_PERM_GROUPS_MAX];
	int proposed_count = g_perm_group_count;
	unsigned char has[CIX_PERMISSION_COUNT];
	int identity_write = perm_index("identity:write");
	int at, i, any = 0;

	if (detail != NULL && detail_size > 0)
		detail[0] = '\0';
	if (!simple_name_is_valid(group, HOSTAUTH_GROUP_NAME_MAX))
		return HOSTAUTH_PERM_ERR_INVALID_GROUP;
	memset(has, 0, sizeof(has));
	for (i = 0; i < count; i++) {
		int idx = perm_index(perms[i]);

		if (idx < 0) {
			if (detail != NULL && detail_size > 0)
				snprintf(detail, detail_size, "%s", perms[i] != NULL ? perms[i] : "");
			return HOSTAUTH_PERM_ERR_UNKNOWN_PERMISSION;
		}
		has[idx] = 1;
		any = 1;
	}

	/* Build the mapping the request proposes, then judge it whole. */
	memcpy(proposed, g_perm_groups, sizeof(proposed));
	at = perm_group_find(proposed, proposed_count, group);
	if (!any) {
		if (at >= 0) {
			proposed[at] = proposed[--proposed_count];
			memset(&proposed[proposed_count], 0, sizeof(proposed[0]));
		}
	} else {
		if (at < 0) {
			if (proposed_count >= HOSTAUTH_PERM_GROUPS_MAX)
				return HOSTAUTH_PERM_ERR_FULL;
			at = proposed_count++;
			memset(&proposed[at], 0, sizeof(proposed[at]));
			snprintf(proposed[at].name, sizeof(proposed[at].name), "%s", group);
		}
		memcpy(proposed[at].has, has, sizeof(has));
	}
	if (count_groups_holding_all(proposed, proposed_count) > HOSTAUTH_ADMIN_GROUPS_MAX)
		return HOSTAUTH_PERM_ERR_TOO_MANY_ADMIN_GROUPS;
	/*
	 * ADR-0317 section 6: once someone holds the permission that grants
	 * permissions, a change may not leave nobody holding it. Before
	 * anyone does -- a fresh box -- the mapping is freely editable,
	 * the same "never refuse turning protection ON" posture #370 took,
	 * and the same test as hostauth_set_config() and the user and group
	 * guards (hostauth_*_change_keeps_grantor()).
	 */
	if (identity_write >= 0 && any_user_holds(g_perm_groups, g_perm_group_count, identity_write) &&
	    !any_user_holds(proposed, proposed_count, identity_write))
		return HOSTAUTH_PERM_ERR_LOCKOUT;

	{
		struct perm_group old[HOSTAUTH_PERM_GROUPS_MAX];
		int old_count = g_perm_group_count;
		struct hostauth_config old_config = g_config;

		memcpy(old, g_perm_groups, sizeof(old));
		memcpy(g_perm_groups, proposed, sizeof(proposed));
		g_perm_group_count = proposed_count;
		derive_admin_groups();
		if (save_config() != 0) {
			memcpy(g_perm_groups, old, sizeof(old));
			g_perm_group_count = old_count;
			g_config = old_config;
			return HOSTAUTH_PERM_ERR_PERSIST_FAILED;
		}
	}
	return HOSTAUTH_PERM_OK;
}

void hostauth_forget_group(const char *name)
{
	struct hostauth_config old_config = g_config;
	struct perm_group old_entry;
	int at = perm_group_find(g_perm_groups, g_perm_group_count, name);

	if (at < 0)
		return;
	old_entry = g_perm_groups[at];
	g_perm_groups[at] = g_perm_groups[--g_perm_group_count];
	memset(&g_perm_groups[g_perm_group_count], 0, sizeof(g_perm_groups[0]));
	derive_admin_groups();
	if (save_config() != 0) {
		/* The group is gone either way; what failed is recording that
		 * its grants went with it. Kept in memory, and said, rather
		 * than quietly dropped. */
		g_perm_groups[g_perm_group_count++] = old_entry;
		g_config = old_config;
		fprintf(stderr, "host auth: could not persist removing deleted group \"%s\" from the "
		                "permission mapping\n", name);
	}
}

void hostauth_write_permissions_json(struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "vocabulary");
	jw_arr_open(w);
	for (i = 0; i < CIX_PERMISSION_COUNT; i++)
		jw_str(w, cix_permissions[i]);
	jw_arr_close(w);
	jw_key(w, "groups");
	write_mapping_groups(w);
	jw_obj_close(w);
}

/* The permissions username holds, as a JSON array in vocabulary order
 * -- the union over every group they are in. [] for NULL. */
void hostauth_write_user_permissions_json(struct json_writer *w, const char *username)
{
	int i;

	jw_arr_open(w);
	for (i = 0; username != NULL && i < CIX_PERMISSION_COUNT; i++) {
		if (user_holds_under(g_perm_groups, g_perm_group_count, username, i))
			jw_str(w, cix_permissions[i]);
	}
	jw_arr_close(w);
}

/*
 * ---- ADR-0317 section 7 (#542): the standard groups ----
 *
 * cix-admins holds every permission, cix-operators `public` plus every
 * read and every operate, cix-readers every read -- the sets section 7
 * names, derived from the vocabulary by suffix so a permission added to
 * the contract lands in the right group without an edit here.
 */
static int word_is_area(const char *word, const char *verb)
{
	const char *colon = strchr(word, ':');

	return colon != NULL && strcmp(colon + 1, verb) == 0;
}

static int grants_admin(const char *word)
{
	(void)word;
	return 1;
}

static int grants_operator(const char *word)
{
	return strcmp(word, "public") == 0 || word_is_area(word, "read") ||
	       word_is_area(word, "operate");
}

static int grants_reader(const char *word)
{
	return word_is_area(word, "read");
}

static const struct {
	const char *name;
	int (*grants)(const char *word);
} standard_groups[] = {
	{ "cix-admins", grants_admin },
	{ "cix-operators", grants_operator },
	{ "cix-readers", grants_reader },
};

int hostauth_provision_standard_groups(void)
{
	size_t s;
	int complete = 1, changed = 0;

	if (g_standard_groups_provisioned)
		return 0;
	for (s = 0; s < sizeof(standard_groups) / sizeof(standard_groups[0]); s++) {
		const char *name = standard_groups[s].name;
		struct perm_group *pg;
		struct ldap_group *g;
		enum ldap_record_error rerr;
		int i, gid;

		/* Section 7: "an existing group of the same name is never
		 * re-granted" -- existing in the directory, or already in
		 * the mapping. On 192.168.15.95 cix-admins is the directory's
		 * own admin group, and it keeps exactly what it has. */
		if (ldap_group_find(name) != NULL ||
		    perm_group_find(g_perm_groups, g_perm_group_count, name) >= 0)
			continue;
		if (g_perm_group_count >= HOSTAUTH_PERM_GROUPS_MAX ||
		    (standard_groups[s].grants == grants_admin &&
		     count_groups_holding_all(g_perm_groups, g_perm_group_count) >=
		         HOSTAUTH_ADMIN_GROUPS_MAX)) {
			logstore_write("hostauth", "warning",
			               "standard group %s not provisioned: the permission mapping is full",
			               name);
			complete = 0;
			continue;
		}
		gid = ldap_gid_alloc();
		rerr = ldap_group_create(name, gid, &g);
		if (rerr != LDAP_RECORD_OK) {
			logstore_write("hostauth", "warning",
			               "standard group %s not provisioned: creating it in the directory "
			               "failed (err=%d); retried at the next start",
			               name, (int)rerr);
			complete = 0;
			continue;
		}
		pg = &g_perm_groups[g_perm_group_count++];
		memset(pg, 0, sizeof(*pg));
		snprintf(pg->name, sizeof(pg->name), "%s", name);
		for (i = 0; i < CIX_PERMISSION_COUNT; i++)
			pg->has[i] = (unsigned char)standard_groups[s].grants(cix_permissions[i]);
		changed = 1;
		logstore_write("hostauth", "info", "standard group %s provisioned (gid %d)", name, gid);
	}
	if (changed)
		derive_admin_groups();
	/* Only once every standard group exists or was deliberately found
	 * already there; a failure is retried at the next start, and the
	 * groups already made are skipped then as existing. */
	g_standard_groups_provisioned = complete;
	if ((changed || complete) && save_config() != 0) {
		logstore_write("hostauth", "err",
		               "could not persist the standard groups' permissions; the groups "
		               "exist in the directory and will not be re-granted");
		return -1;
	}
	return complete ? 0 : -1;
}

enum hostauth_authz hostauth_authorize_app_password(const char *username, const char *permission)
{
	if (permission == NULL)
		return HOSTAUTH_AUTHZ_FORBIDDEN;
	if (strcmp(permission, "public") == 0 || !hostauth_gating_active())
		return HOSTAUTH_AUTHZ_OK;
	if (username == NULL || username[0] == '\0')
		return HOSTAUTH_AUTHZ_NO_SESSION;
	/* `authenticated` is "any valid session", and an app password is
	 * not one: see the header. */
	if (strcmp(permission, "authenticated") == 0)
		return HOSTAUTH_AUTHZ_FORBIDDEN;
	return hostauth_user_has_permission(username, permission) ? HOSTAUTH_AUTHZ_OK
	                                                          : HOSTAUTH_AUTHZ_FORBIDDEN;
}

void hostauth_note_auth_failure(const char *peer_ip)
{
	if (connthrottle_record_auth_failure(peer_ip)) {
		struct throttle_config cfg = connthrottle_config_get();

		logstore_write("audit", "warn",
		               "%s authentication blocked for %d seconds after %d failed attempts "
		               "within %d seconds",
		               peer_ip, cfg.block_seconds, cfg.threshold, cfg.window_seconds);
	}
}

void hostauth_log_carried_sessions(void)
{
	if (g_sessions_carried < 0)
		logstore_write("hostauth", "error", "%s is malformed -- started with no sessions",
		               g_sessions_path);
	else if (g_sessions_carried > 0)
		logstore_write("hostauth", "info", "%d login session%s carried across the restart",
		               g_sessions_carried, g_sessions_carried == 1 ? "" : "s");
}
