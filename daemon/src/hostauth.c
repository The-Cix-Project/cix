#include "hostauth.h"
#include "ldap.h"
#include "ldapclient.h"
#include "persist.h"
#include "pki.h"
#include "logstore.h"

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
	g_config.ldap_tls = ldap_tls ? 1 : 0;
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
	jw_obj_close(w);
}

/*
 * The invariant this file protects, in one place (#370).
 *
 * "Gating is active" means: an admin group is configured AND at least
 * one enabled user is in one of them. Both halves matter, and the
 * second is what made this dangerous -- with an admin group configured
 * and nobody in it, every mutating request on the API is permitted,
 * silently, with the configuration still looking correct.
 *
 * Five ordinary operations could reach that state: deleting the admin
 * group, renaming it or changing its gidnumber (membership is by gid
 * and the config names it by name, so either orphans it), deleting the
 * last admin user, disabling or de-admining them, and pointing
 * admin_groups at a group with no members. None of them looks like
 * "turn authentication off", and all five did exactly that.
 *
 * These three are the predicates the guards ask. They exist so the
 * guards do not each grow their own copy of the rule -- and
 * hostauth_gating_active() below is now written in terms of the first
 * of them, so there is one definition of "an admin user" rather than
 * two that can drift.
 */
static int count_one_admin(const char *username, void *ctx)
{
	if (hostauth_user_is_admin(username))
		(*(int *)ctx)++;
	/*
	 * Always 0: ldap_user_for_each() stops at the first callback that
	 * returns true, and a count needs the whole walk. Reused rather
	 * than open-coding a second loop over the user table.
	 */
	return 0;
}

int hostauth_admin_user_count(void)
{
	int n = 0;

	ldap_user_for_each(count_one_admin, &n);
	return n;
}

/*
 * Would gating be active if admin_groups were exactly this list?
 *
 * Asked by the hostauth-config PUT guard, which must judge the config
 * a request PROPOSES. Deliberately NOT "refuse any config with no
 * members": setting admin_groups before the admin users exist is the
 * ordinary way an operator turns gating on for the first time, and
 * refusing it would obstruct enabling authentication in order to
 * protect authentication. The guard only refuses a change that would
 * turn ACTIVE gating off -- see its call site.
 */
struct proposed_groups {
	const char *const *names;
	int count;
};

static int user_in_proposed(const char *username, void *ctx)
{
	const struct proposed_groups *p = ctx;
	int j;

	for (j = 0; j < p->count; j++) {
		if (ldap_user_is_in_group(username, p->names[j]))
			return 1; /* short-circuits the walk: one is enough */
	}
	return 0;
}

int hostauth_would_gate(const char *const *admin_groups, int admin_group_count)
{
	struct proposed_groups p;

	if (admin_groups == NULL || admin_group_count == 0)
		return 0;
	p.names = admin_groups;
	p.count = admin_group_count;
	return ldap_user_for_each(user_in_proposed, &p);
}

int hostauth_group_is_admin_group(const char *group_name)
{
	int i;

	if (group_name == NULL)
		return 0;
	for (i = 0; i < g_config.admin_group_count; i++) {
		if (strcmp(g_config.admin_groups[i], group_name) == 0)
			return 1;
	}
	return 0;
}

int hostauth_user_is_admin(const char *username)
{
	int i;

	if (username == NULL)
		return 0;
	for (i = 0; i < g_config.admin_group_count; i++) {
		if (ldap_user_is_in_group(username, g_config.admin_groups[i]))
			return 1;
	}
	return 0;
}

/*
 * Would a user carrying exactly these gids be an admin? Asked by the
 * user-PUT guard, which has to judge the record the request PROPOSES,
 * before it is applied -- the existing record is no help there, since
 * the whole question is whether the change removes the last admin.
 *
 * Resolves each configured admin group NAME to its gid rather than the
 * other way round: the config names groups by name, a user carries
 * gids, and going name -> gid needs no reverse lookup and no second
 * copy of the membership rule.
 */
int hostauth_gids_are_admin(int primarygroup, const int *secondary_groups, int secondary_count,
                             int disabled)
{
	int i, j;

	if (disabled)
		return 0;
	for (i = 0; i < g_config.admin_group_count; i++) {
		const struct ldap_group *g = ldap_group_find(g_config.admin_groups[i]);

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
	 *
	 * #370: the walk now goes through hostauth_admin_user_count(), so
	 * "is there an admin" and "how many admins are there" cannot give
	 * different answers -- the guards that protect this invariant need
	 * the count, and two loops meaning the same thing is how they
	 * would drift.
	 */
	if (g_config.admin_group_count == 0)
		return 0;
	return hostauth_admin_user_count() > 0;
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
