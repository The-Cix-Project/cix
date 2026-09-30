#include "hostauth.h"
#include "ldap.h"
#include "ldapclient.h"
#include "persist.h"
#include "pki.h"
#include "logstore.h"
#include "namecheck.h"
#include "connthrottle.h"

/* ADR-0317: the contract's permission vocabulary (apigen --emit-permissions). */
#include "generated/permissions.h"

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
	char token[HOSTAUTH_TOKEN_LEN + 1];
	char username[HOSTAUTH_USERNAME_MAX];
	time_t expires_at; /* meaningless (never consulted) when idle_timeout_seconds == 0 */
	int in_use;
};

static struct hostauth_session g_sessions[HOSTAUTH_SESSION_MAX];

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

/* Tries every configured LDAP server in order for one login attempt.
 * *out_answered is set to 1 iff some server gave an authoritative
 * bind-succeeded or bind-explicitly-rejected answer (in which case the
 * return value IS that answer); left at 0 (return value meaningless)
 * if every server was unreachable/erroring, or none are configured. */
static int try_ldap_login(const char *username, const char *password, int *out_answered)
{
	char dn[HOSTAUTH_USERNAME_MAX + HOSTAUTH_GROUP_NAME_MAX + HOSTAUTH_LDAP_BASE_DN_MAX + 8];
	char *ca_pem = NULL;
	size_t ca_pem_len = 0;
	int authenticated = 0;
	int i;

	*out_answered = 0;
	if (build_login_bind_dn(username, dn, sizeof(dn)) != 0)
		return 0;

	/*
	 * #416: the trust chain is read ONCE per login, not once per
	 * server -- and here rather than at startup, because the CA can
	 * change under a running daemon (pki_ca_reset(), an intermediate
	 * bootstrap, ADR-0281's import) and a stale anchor fails as a
	 * refused connection against a perfectly correct certificate. A
	 * login is rare enough (this module's own comment says so about
	 * connection pooling) that the read costs nothing worth saving.
	 *
	 * No chain while ldap_tls is on means this function does NOT bind
	 * at all: it returns with *out_answered still 0, which is the
	 * established "no server gave a usable answer" outcome, so
	 * hostauth_login() falls back to the local user records. It
	 * deliberately does not fall back to a plaintext bind -- that
	 * would send the credential in clear on the strength of a
	 * configuration that asked for the opposite. hostauth_set_config()
	 * already refuses this combination; a CA reset under an
	 * already-saved config is how it can still be reached.
	 */
	if (g_config.ldap_tls &&
	    (pki_trust_bundle_pem(&ca_pem, &ca_pem_len) != PKI_OK || ca_pem == NULL)) {
		logstore_write("hostauth", "error",
		                "ldap_tls is on but no CA trust chain is available -- not binding at "
		                "all rather than binding in clear; bootstrap the CA, or set "
		                "ldap_tls false if this directory really is plaintext");
		free(ca_pem);
		return 0;
	}

	for (i = 0; i < g_config.ldap_server_count; i++) {
		int result_code = -1;
		enum ldapclient_error err =
		    ldapclient_bind(g_config.ldap_servers[i], g_config.ldap_port, dn, password,
		                     HOSTAUTH_LDAP_TIMEOUT_MS, ca_pem, ca_pem_len, &result_code);

		if (err == LDAPCLIENT_OK) {
			*out_answered = 1;
			authenticated = 1;
			break;
		}
		if (err == LDAPCLIENT_ERR_LDAP_RESULT) {
			/* a real, well-formed rejection (invalidCredentials or
			 * similar) from a directory that IS reachable -- this is
			 * authoritative, not "try the next server" */
			*out_answered = 1;
			break;
		}
		/* LDAPCLIENT_ERR_CONNECT/PROTOCOL: this server didn't give a
		 * usable answer at all -- fall through and try the next one */
	}
	free(ca_pem);
	return authenticated;
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

/*
 * {"sessions":[{"username":...,"expires_in_seconds":<int or null>}, ...]}
 * -- every currently active session, real admin visibility into a
 * table that previously had none at all (no endpoint, no CLI, no web
 * panel). The raw token is never included, before or after issuance --
 * only hostauth_login()'s own one-time response ever carries it. An
 * expired-but-not-yet-reaped entry (idle_timeout_seconds > 0, past its
 * own expires_at) is skipped -- it would report a negative
 * expires_in_seconds otherwise, and it's not really "active" by the
 * same definition hostauth_check_token()'s own opportunistic reaping
 * already uses. idle_timeout_seconds == 0 sessions (single-use,
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
		if (g_config.idle_timeout_seconds > 0 && g_sessions[i].expires_at <= now)
			continue;

		jw_obj_open(w);
		jw_key(w, "username");
		jw_str(w, g_sessions[i].username);
		jw_key(w, "expires_in_seconds");
		if (g_config.idle_timeout_seconds > 0)
			jw_int(w, (long long)(g_sessions[i].expires_at - now));
		else
			jw_null(w);
		jw_obj_close(w);
	}
	jw_arr_close(w);
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
			memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
			revoked++;
		}
	}
	return revoked;
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
	for (i = 0; i < HOSTAUTH_SESSION_MAX; i++) {
		if (!g_sessions[i].in_use || strcmp(g_sessions[i].token, token) != 0)
			continue;
		if (g_sessions[i].expires_at <= now)
			return; /* already lapsed -- reaping stays with the real check */
		g_sessions[i].expires_at = now + (time_t)g_config.idle_timeout_seconds;
		return;
	}
}

int hostauth_peek_token(const char *token, char *out_username, size_t out_username_size)
{
	int i;
	time_t now = time(NULL);

	if (token == NULL || token[0] == '\0')
		return 0;
	for (i = 0; i < HOSTAUTH_SESSION_MAX; i++) {
		if (!g_sessions[i].in_use || strcmp(g_sessions[i].token, token) != 0)
			continue;
		if (g_config.idle_timeout_seconds != 0 && g_sessions[i].expires_at <= now)
			return 0; /* expired -- leave reaping it to the next real check */
		snprintf(out_username, out_username_size, "%s", g_sessions[i].username);
		return 1;
	}
	return 0;
}

enum hostauth_authz hostauth_authorize(const char *token, const char *permission)
{
	char username[HOSTAUTH_USERNAME_MAX];

	if (permission == NULL)
		return HOSTAUTH_AUTHZ_FORBIDDEN;
	if (strcmp(permission, "public") == 0 || !hostauth_gating_active())
		return HOSTAUTH_AUTHZ_OK;
	if (!hostauth_check_token(token, username, sizeof(username)))
		return HOSTAUTH_AUTHZ_NO_SESSION;
	if (strcmp(permission, "authenticated") == 0 ||
	    hostauth_user_has_permission(username, permission))
		return HOSTAUTH_AUTHZ_OK;
	return HOSTAUTH_AUTHZ_FORBIDDEN;
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
