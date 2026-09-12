#include "ldap.h"
#include "containerpath.h"
#include "logstore.h"
#include "subid.h"
#include "hostauth.h"
#include "persist.h"
#include "pki.h"
#include "registry.h"
#include "serverhealth.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static struct ldap_server_binding g_bindings[LDAP_SERVER_MAX];
static char g_state_path[PATH_MAX];

static struct ldap_user g_users[LDAP_USER_MAX];
static char g_users_state_path[PATH_MAX];
static struct ldap_group g_groups[LDAP_GROUP_MAX];
static char g_groups_state_path[PATH_MAX];

/* The client-login fields (URI, base DN, bind DN, bind password) are
 * deliberately left zeroed: until an operator PUTs them there is no LDAP
 * server to point a container at, and an empty value is what the recipe
 * renderer treats as unset. Designated initialisers say that on purpose
 * rather than leaving it to the reader to count fields. */
static struct ldap_config g_config = {
	.start_uid = LDAP_CONFIG_DEFAULT_START_UID,
	.start_gid = LDAP_CONFIG_DEFAULT_START_GID,
	/* #414: plaintext until an operator turns TLS on -- the honest
	 * default, since that is what every existing box is doing and a
	 * flag that silently changed the port clients dial would break
	 * them on upgrade. The port is glauth's own sample default. */
	.client_tls = 0,
	/* #419: unmanaged until an operator PUTs a server_* field -- the
	 * ports still carry real defaults, because ldap_client_port()
	 * reads them whether or not the render is allowed to write. */
	.listeners_managed = 0,
	.server_plaintext = 1,
	.server_plaintext_port = HOSTAUTH_LDAP_DEFAULT_PORT,
	.server_tls = 0,
	.server_tls_port = LDAP_CONFIG_DEFAULT_TLS_PORT,
};
static char g_config_state_path[PATH_MAX];

static int config_path_is_valid(const char *path)
{
	return path != NULL && path[0] == '/' && strstr(path, "..") == NULL;
}

static int save_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	ldap_server_write_json_list(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int count = 0;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no persisted state yet -- first-ever startup */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted LDAP server binding state\n", g_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count && count < LDAP_SERVER_MAX; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *container = json_as_string(json_object_get(item, "container"));
		const char *config_path = json_as_string(json_object_get(item, "config_path"));

		if (container == NULL || container[0] == '\0' || !config_path_is_valid(config_path))
			continue; /* skip a corrupt entry rather than fail the whole load */

		memset(&g_bindings[count], 0, sizeof(g_bindings[count]));
		strncpy(g_bindings[count].container_name, container,
		        sizeof(g_bindings[count].container_name) - 1);
		strncpy(g_bindings[count].config_path, config_path,
		        sizeof(g_bindings[count].config_path) - 1);
		count++;
	}
	json_free(root);
	return 0;
}

void ldap_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

int ldap_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;

	memset(g_bindings, 0, sizeof(g_bindings));
	return load_state();
}

static struct ldap_server_binding *binding_find(const char *container_name)
{
	int i;

	for (i = 0; i < LDAP_SERVER_MAX; i++) {
		if (g_bindings[i].container_name[0] != '\0' &&
		    strcmp(g_bindings[i].container_name, container_name) == 0)
			return &g_bindings[i];
	}
	return NULL;
}

int ldap_server_list_containers(char out[][LDAP_SERVER_NAME_MAX], int max)
{
	int i, n = 0;

	for (i = 0; i < LDAP_SERVER_MAX && n < max; i++) {
		if (g_bindings[i].container_name[0] != '\0') {
			snprintf(out[n], LDAP_SERVER_NAME_MAX, "%s", g_bindings[i].container_name);
			n++;
		}
	}
	return n;
}

const struct ldap_server_binding *ldap_server_find(const char *container_name)
{
	return binding_find(container_name);
}

enum ldap_server_error ldap_server_register(const char *container_name, const char *config_path)
{
	int i, slot = -1;

	if (!config_path_is_valid(config_path))
		return LDAP_SERVER_ERR_INVALID_PATH;
	if (binding_find(container_name) != NULL)
		return LDAP_SERVER_ERR_DUPLICATE;

	for (i = 0; i < LDAP_SERVER_MAX; i++) {
		if (g_bindings[i].container_name[0] == '\0') {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return LDAP_SERVER_ERR_FULL;

	memset(&g_bindings[slot], 0, sizeof(g_bindings[slot]));
	strncpy(g_bindings[slot].container_name, container_name,
	        sizeof(g_bindings[slot].container_name) - 1);
	strncpy(g_bindings[slot].config_path, config_path, sizeof(g_bindings[slot].config_path) - 1);

	if (save_state() != 0) {
		memset(&g_bindings[slot], 0, sizeof(g_bindings[slot]));
		return LDAP_SERVER_ERR_PERSIST_FAILED;
	}

	/*
	 * #418: render the record set into the newly-registered server
	 * before returning, the way dns_server_register() has always
	 * written its own hosts file. A registered glauth whose config
	 * carries no [[users]] stanzas refuses every bind with an ordinary
	 * invalidCredentials, while reporting "running" with a valid
	 * certificate -- so the failure is authentication rejections, not
	 * a container that visibly fails to start.
	 *
	 * This lived in handle_ldap_server_create() instead, so only the
	 * POST /ldap/servers path got it. A container whose own DEFINITION
	 * declares the role (ADR-0151's register_declared_server_roles(),
	 * which is how a deployment applies one) registered and rendered
	 * nothing. Measured on 192.168.15.95, 2026-09-12: after recreating
	 * ldap-2 from its recipe, its config had 0 [[users]] stanzas
	 * against untouched ldap-1's 3, and an ldapsearch bind with the
	 * correct password returned 49 there and 0 here. Both containers
	 * carry follow_rolling, so an ordinary image update silently
	 * emptied the directory it rebuilt.
	 *
	 * It is here rather than at the two call sites because
	 * ldap_record_sync_all()'s own comment ALREADY claimed
	 * "ldap_server_register() calls this too, so a fresh/replacement
	 * instance always starts current". That sentence was false for as
	 * long as it existed, and it is why nobody checked -- it read as
	 * documentation of a guarantee. Putting the call where the comment
	 * says it is makes the claim true instead of deleting it.
	 *
	 * Best-effort, unlike DNS's own DNS_SERVER_ERR_WRITE_FAILED: this
	 * renders EVERY binding, so failing the registration on another
	 * server's write would refuse a correct request for an unrelated
	 * reason. A per-binding failure already warns to both stderr and
	 * the log store (see ldap_record_sync_all()).
	 */
	ldap_record_sync_all();
	return LDAP_SERVER_OK;
}

enum ldap_server_error ldap_server_unregister(const char *container_name)
{
	struct ldap_server_binding *b = binding_find(container_name);

	if (b == NULL)
		return LDAP_SERVER_ERR_NOT_FOUND;
	memset(b, 0, sizeof(*b));
	save_state();
	return LDAP_SERVER_OK;
}

void ldap_server_forget(const char *container_name)
{
	struct ldap_server_binding *b = binding_find(container_name);

	if (b != NULL) {
		memset(b, 0, sizeof(*b));
		save_state();
	}
}

void ldap_server_write_json_one(const struct ldap_server_binding *binding, struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "container");
	jw_str(w, binding->container_name);
	jw_key(w, "config_path");
	jw_str(w, binding->config_path);
	jw_obj_close(w);
}

void ldap_server_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < LDAP_SERVER_MAX; i++) {
		if (g_bindings[i].container_name[0] != '\0')
			ldap_server_write_json_one(&g_bindings[i], w);
	}
	jw_arr_close(w);
}

/* ---- LDAP user/group record store (task #726) ---- */

/* Issue #83: counts used to detect managed-but-undeliverable state (users
 * and groups exist, but no LDAP server is registered to serve them). */
int ldap_user_count(void)
{
	int i, n = 0;

	for (i = 0; i < LDAP_USER_MAX; i++) {
		if (g_users[i].name[0] != '\0')
			n++;
	}
	return n;
}

int ldap_group_count(void)
{
	int i, n = 0;

	for (i = 0; i < LDAP_GROUP_MAX; i++) {
		if (g_groups[i].name[0] != '\0')
			n++;
	}
	return n;
}

int ldap_username_is_valid(const char *name)
{
	size_t i, len;

	if (name == NULL || name[0] == '\0')
		return 0;
	len = strlen(name);
	if (len >= LDAP_USER_NAME_MAX)
		return 0;
	if (!((name[0] >= 'a' && name[0] <= 'z') || name[0] == '_'))
		return 0;
	for (i = 1; i < len; i++) {
		char c = name[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return 0;
	}
	return 1;
}

int ldap_groupname_is_valid(const char *name)
{
	/* Same charset rules as a username -- glauth's own config "groups"
	 * stanza has no separate convention of its own. */
	return ldap_username_is_valid(name);
}

/* ADR-0144: real bcrypt hashing (daemon/include/pwhash.h) -- see
 * struct ldap_user's own doc comment in ldap.h for why this replaced
 * the original unsalted-SHA256 passsha256 field. Failure (only
 * possible from a too-small output buffer, which out's own fixed
 * PWHASH_BCRYPT_LEN+1 size here never triggers) leaves out untouched;
 * callers already only invoke this when a real password was given. */
static void hash_password(const char *password, char out[PWHASH_BCRYPT_LEN + 1])
{
	pwhash_bcrypt_new(password, out, PWHASH_BCRYPT_LEN + 1);
}

/*
 * Persisted-state serialization for users -- deliberately distinct
 * from ldap_user_write_json_one() (the public REST-response writer,
 * which omits passsha256 entirely and reports only "has_password").
 * The persisted copy is never sent over the API and needs the real
 * hash so a restart doesn't silently forget every credential.
 */
static void write_user_persist_one(const struct ldap_user *u, struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, u->name);
	jw_key(w, "uidnumber");
	jw_int(w, u->uidnumber);
	jw_key(w, "primarygroup");
	jw_int(w, u->primarygroup);
	jw_key(w, "secondary_groups");
	jw_arr_open(w);
	{
		int i;

		for (i = 0; i < u->secondary_group_count; i++)
			jw_int(w, u->secondary_groups[i]);
	}
	jw_arr_close(w);
	jw_key(w, "givenname");
	jw_str(w, u->givenname);
	jw_key(w, "sn");
	jw_str(w, u->sn);
	jw_key(w, "mail");
	jw_str(w, u->mail);
	jw_key(w, "loginshell");
	jw_str(w, u->loginshell);
	jw_key(w, "homedirectory");
	jw_str(w, u->homedirectory);
	jw_key(w, "passbcrypt");
	jw_str(w, u->passbcrypt);
	jw_key(w, "disabled");
	jw_bool(w, u->disabled != 0);
	jw_key(w, "owner");
	if (u->owner_container[0] != '\0')
		jw_str(w, u->owner_container);
	else
		jw_null(w);
	jw_key(w, "can_search");
	jw_bool(w, u->can_search != 0);
	jw_key(w, "ssh_public_key");
	jw_str(w, u->ssh_public_key);
	jw_obj_close(w);
}

static int save_users_state(void)
{
	struct json_writer w;
	int rc, i;

	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < LDAP_USER_MAX; i++) {
		if (g_users[i].name[0] != '\0')
			write_user_persist_one(&g_users[i], &w);
	}
	jw_arr_close(&w);
	rc = persist_atomic_write(g_users_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int save_groups_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	ldap_group_write_json_list(&w);
	rc = persist_atomic_write(g_groups_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int load_users_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int count = 0;

	if (persist_read_file(g_users_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0;

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted LDAP user state\n", g_users_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count && count < LDAP_USER_MAX; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *name = json_as_string(json_object_get(item, "name"));
		const char *givenname = json_as_string(json_object_get(item, "givenname"));
		const char *sn = json_as_string(json_object_get(item, "sn"));
		const char *mail = json_as_string(json_object_get(item, "mail"));
		const char *loginshell = json_as_string(json_object_get(item, "loginshell"));
		const char *homedirectory = json_as_string(json_object_get(item, "homedirectory"));
		const char *passbcrypt = json_as_string(json_object_get(item, "passbcrypt"));
		const char *owner = json_as_string(json_object_get(item, "owner"));
		const struct json_value *jsecondary = json_object_get(item, "secondary_groups");
		const char *ssh_public_key = json_as_string(json_object_get(item, "ssh_public_key"));
		struct ldap_user *u;

		if (!ldap_username_is_valid(name))
			continue; /* skip a corrupt entry rather than fail the whole load */

		u = &g_users[count];
		memset(u, 0, sizeof(*u));
		strncpy(u->name, name, sizeof(u->name) - 1);
		u->uidnumber = (int)json_as_number(json_object_get(item, "uidnumber"));
		u->primarygroup = (int)json_as_number(json_object_get(item, "primarygroup"));
		if (jsecondary != NULL && jsecondary->type == JSON_ARRAY) {
			size_t j;

			for (j = 0; j < jsecondary->u.array.count && (int)j < LDAP_USER_MAX_SECONDARY_GROUPS;
			     j++)
				u->secondary_groups[j] = (int)json_as_number(jsecondary->u.array.items[j]);
			u->secondary_group_count = (int)j;
		}
		if (givenname != NULL)
			strncpy(u->givenname, givenname, sizeof(u->givenname) - 1);
		if (sn != NULL)
			strncpy(u->sn, sn, sizeof(u->sn) - 1);
		if (mail != NULL)
			strncpy(u->mail, mail, sizeof(u->mail) - 1);
		if (loginshell != NULL)
			strncpy(u->loginshell, loginshell, sizeof(u->loginshell) - 1);
		if (homedirectory != NULL)
			strncpy(u->homedirectory, homedirectory, sizeof(u->homedirectory) - 1);
		if (passbcrypt != NULL)
			strncpy(u->passbcrypt, passbcrypt, sizeof(u->passbcrypt) - 1);
		if (owner != NULL)
			strncpy(u->owner_container, owner, sizeof(u->owner_container) - 1);
		u->disabled = json_as_number(json_object_get(item, "disabled")) != 0;
		u->can_search = json_as_number(json_object_get(item, "can_search")) != 0;
		if (ssh_public_key != NULL)
			strncpy(u->ssh_public_key, ssh_public_key, sizeof(u->ssh_public_key) - 1);
		count++;
	}
	json_free(root);
	return 0;
}

static int load_groups_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int count = 0;

	if (persist_read_file(g_groups_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0;

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted LDAP group state\n", g_groups_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count && count < LDAP_GROUP_MAX; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *name = json_as_string(json_object_get(item, "name"));
		struct ldap_group *g;

		if (!ldap_groupname_is_valid(name))
			continue;

		g = &g_groups[count];
		memset(g, 0, sizeof(*g));
		strncpy(g->name, name, sizeof(g->name) - 1);
		g->gidnumber = (int)json_as_number(json_object_get(item, "gidnumber"));
		count++;
	}
	json_free(root);
	return 0;
}

void ldap_record_repoint(const char *new_users_state_path, const char *new_groups_state_path)
{
	snprintf(g_users_state_path, sizeof(g_users_state_path), "%s", new_users_state_path);
	snprintf(g_groups_state_path, sizeof(g_groups_state_path), "%s", new_groups_state_path);
}

int ldap_record_init(const char *users_state_path, const char *groups_state_path)
{
	if (snprintf(g_users_state_path, sizeof(g_users_state_path), "%s", users_state_path) >=
	    (int)sizeof(g_users_state_path))
		return -1;
	if (snprintf(g_groups_state_path, sizeof(g_groups_state_path), "%s", groups_state_path) >=
	    (int)sizeof(g_groups_state_path))
		return -1;

	memset(g_users, 0, sizeof(g_users));
	memset(g_groups, 0, sizeof(g_groups));
	if (load_groups_state() != 0)
		return -1;
	return load_users_state();
}

void ldap_config_repoint(const char *new_state_path)
{
	snprintf(g_config_state_path, sizeof(g_config_state_path), "%s", new_state_path);
}

int ldap_config_init(const char *state_path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *jstart_uid, *jstart_gid;

	if (snprintf(g_config_state_path, sizeof(g_config_state_path), "%s", state_path) >=
	    (int)sizeof(g_config_state_path))
		return -1;

	g_config.start_uid = LDAP_CONFIG_DEFAULT_START_UID;
	g_config.start_gid = LDAP_CONFIG_DEFAULT_START_GID;

	if (persist_read_file(g_config_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no persisted config yet -- defaults stand */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		fprintf(stderr, "%s: malformed persisted LDAP config\n", g_config_state_path);
		return -1;
	}

	jstart_uid = json_object_get(root, "start_uid");
	jstart_gid = json_object_get(root, "start_gid");
	if (jstart_uid != NULL)
		g_config.start_uid = (int)json_as_number(jstart_uid);
	if (jstart_gid != NULL)
		g_config.start_gid = (int)json_as_number(jstart_gid);
	/* Issue #66: client-login fields (absent in older state files --
	 * empty/unset then, no migration needed). */
	{
		const char *v;

		if ((v = json_as_string(json_object_get(root, "client_uri"))) != NULL)
			snprintf(g_config.client_uri, sizeof(g_config.client_uri), "%s", v);
		if ((v = json_as_string(json_object_get(root, "base_dn"))) != NULL)
			snprintf(g_config.base_dn, sizeof(g_config.base_dn), "%s", v);
		if ((v = json_as_string(json_object_get(root, "bind_dn"))) != NULL)
			snprintf(g_config.bind_dn, sizeof(g_config.bind_dn), "%s", v);
		if ((v = json_as_string(json_object_get(root, "bind_password"))) != NULL)
			snprintf(g_config.bind_password, sizeof(g_config.bind_password), "%s", v);
	}
	/* #414: absent in older state files -- plaintext then, which is
	 * what those boxes were actually doing, so no migration. */
	{
		const struct json_value *jv = json_object_get(root, "client_tls");

		if (jv != NULL && jv->type == JSON_BOOL)
			g_config.client_tls = jv->u.boolean;
		/*
		 * #419: absent in state written before this existed, which is
		 * the state every already-installed host is in -- so
		 * listeners_managed stays 0 and the render leaves both
		 * sections exactly as that host's own configs already have
		 * them. A client_tls_port key may also be present from before;
		 * it is deliberately not read, because the port clients get is
		 * the server's own now. The one value that could be lost is a
		 * non-default client_tls_port, and it is reported in the
		 * changelog rather than silently migrated -- there is nothing
		 * honest to migrate it INTO until the server's real port is
		 * known, which is the whole point of this change.
		 */
		jv = json_object_get(root, "listeners_managed");
		if (jv != NULL && jv->type == JSON_BOOL)
			g_config.listeners_managed = jv->u.boolean;
		jv = json_object_get(root, "server_plaintext");
		if (jv != NULL && jv->type == JSON_BOOL)
			g_config.server_plaintext = jv->u.boolean;
		jv = json_object_get(root, "server_plaintext_port");
		if (jv != NULL && jv->type == JSON_NUMBER)
			g_config.server_plaintext_port = (int)jv->u.number;
		jv = json_object_get(root, "server_tls");
		if (jv != NULL && jv->type == JSON_BOOL)
			g_config.server_tls = jv->u.boolean;
		jv = json_object_get(root, "server_tls_port");
		if (jv != NULL && jv->type == JSON_NUMBER)
			g_config.server_tls_port = (int)jv->u.number;
	}
	json_free(root);
	return 0;
}

const struct ldap_config *ldap_config_get(void)
{
	/*
	 * base_dn is NOT independently authoritative (issue #80). The one
	 * canonical base DN is hostauth-config's ldap_base_dn
	 * (hostauth_ldap_base_dn(), ADR-0148) -- the same value glauth's own
	 * backend baseDN is kept in sync with on every write. Overlay it here
	 * so every reader (the nslcd/nsswitch ldap_login auto-staging, the
	 * {{LDAP:BASE_DN}} recipe token, GET /v1/ldap/config) sees that one
	 * true value and can never drift from what the registered servers
	 * actually serve -- exactly the drift that broke jump login. Empty
	 * (never configured) leaves whatever was stored untouched, matching
	 * ADR-0148's own "don't blank out a working value" posture.
	 */
	const char *canonical = hostauth_ldap_base_dn();

	if (canonical[0] != '\0')
		snprintf(g_config.base_dn, sizeof(g_config.base_dn), "%s", canonical);
	return &g_config;
}

/* Serializes the WHOLE config (allocation floors + issue #66's client
 * fields, bind_password included -- this file is the module's own
 * root-only state, the one place the credential legitimately lives)
 * and writes it atomically. Both setters below funnel through here so
 * neither can ever clobber the other's fields. */
static enum ldap_record_error ldap_config_persist(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "start_uid");
	jw_int(&w, g_config.start_uid);
	jw_key(&w, "start_gid");
	jw_int(&w, g_config.start_gid);
	jw_key(&w, "client_uri");
	jw_str(&w, g_config.client_uri);
	jw_key(&w, "base_dn");
	jw_str(&w, g_config.base_dn);
	jw_key(&w, "bind_dn");
	jw_str(&w, g_config.bind_dn);
	jw_key(&w, "bind_password");
	jw_str(&w, g_config.bind_password);
	jw_key(&w, "client_tls");
	jw_bool(&w, g_config.client_tls);
	jw_key(&w, "listeners_managed");
	jw_bool(&w, g_config.listeners_managed);
	jw_key(&w, "server_plaintext");
	jw_bool(&w, g_config.server_plaintext);
	jw_key(&w, "server_plaintext_port");
	jw_int(&w, g_config.server_plaintext_port);
	jw_key(&w, "server_tls");
	jw_bool(&w, g_config.server_tls);
	jw_key(&w, "server_tls_port");
	jw_int(&w, g_config.server_tls_port);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_config_state_path, w.buf, w.len);
	jw_free(&w);
	return rc == 0 ? LDAP_RECORD_OK : LDAP_RECORD_ERR_PERSIST_FAILED;
}

enum ldap_record_error ldap_config_set(int start_uid, int start_gid)
{
	int old_uid = g_config.start_uid, old_gid = g_config.start_gid;
	enum ldap_record_error rc;

	if (start_uid <= 0 || start_gid <= 0)
		return LDAP_RECORD_ERR_INVALID_FIELD;

	g_config.start_uid = start_uid;
	g_config.start_gid = start_gid;
	rc = ldap_config_persist();
	if (rc != LDAP_RECORD_OK) {
		g_config.start_uid = old_uid;
		g_config.start_gid = old_gid;
	}
	return rc;
}

/*
 * Every registered, running server must already hold a delivered leaf
 * before server_tls can be turned on. glauth told to serve TLS with no
 * certificate exits on its next config reload, so accepting the PUT
 * would take down a working server -- strictly worse than refusing a
 * setting. Checked once here rather than again at render time: one
 * gate, and the operator gets a named container instead of a fleet-wide
 * "something is missing".
 *
 * Returns 0 when every server is ready, or -1 with out_container filled.
 */
static int servers_have_tls_certs(char *out_container, size_t out_container_size)
{
	int i;

	for (i = 0; i < LDAP_SERVER_MAX; i++) {
		struct registry_entry *entry;
		char cert_path[PATH_MAX];
		struct stat st;

		if (g_bindings[i].container_name[0] == '\0')
			continue;
		entry = registry_find(g_bindings[i].container_name);
		if (entry == NULL || !entry->running)
			continue; /* already warned about loudly by the render */
		container_file_host_path(g_bindings[i].container_name, entry->disk_name,
		                          PKI_CONTAINER_CERT_DIR "/tls.crt", cert_path,
		                          sizeof(cert_path));
		if (stat(cert_path, &st) != 0 || st.st_size == 0) {
			if (out_container != NULL)
				snprintf(out_container, out_container_size, "%s",
				          g_bindings[i].container_name);
			return -1;
		}
	}
	return 0;
}

enum ldap_record_error ldap_config_set_listeners(int client_tls, int server_plaintext,
                                                  int server_plaintext_port, int server_tls,
                                                  int server_tls_port, char *out_container,
                                                  size_t out_container_size)
{
	struct ldap_config saved = g_config;
	enum ldap_record_error rc;
	int touched_server = 0;

	if (client_tls >= 0)
		g_config.client_tls = client_tls != 0;
	if (server_plaintext >= 0) {
		g_config.server_plaintext = server_plaintext != 0;
		touched_server = 1;
	}
	if (server_tls >= 0) {
		g_config.server_tls = server_tls != 0;
		touched_server = 1;
	}
	if (server_plaintext_port >= 0) {
		g_config.server_plaintext_port = server_plaintext_port;
		touched_server = 1;
	}
	if (server_tls_port >= 0) {
		g_config.server_tls_port = server_tls_port;
		touched_server = 1;
	}

	if (g_config.server_plaintext_port < 1 || g_config.server_plaintext_port > 65535 ||
	    g_config.server_tls_port < 1 || g_config.server_tls_port > 65535 ||
	    g_config.server_plaintext_port == g_config.server_tls_port) {
		g_config = saved;
		return LDAP_RECORD_ERR_INVALID_FIELD;
	}
	/* "Serving nothing" and "clients pointed at a disabled listener"
	 * are the same rule hostauth_set_config() applies to ldap_enabled:
	 * a saved state that cannot work is never accepted, because it
	 * reads as correct afterwards. */
	if (!g_config.server_plaintext && !g_config.server_tls) {
		g_config = saved;
		return LDAP_RECORD_ERR_INVALID_FIELD;
	}
	if ((g_config.client_tls && !g_config.server_tls) ||
	    (!g_config.client_tls && !g_config.server_plaintext)) {
		g_config = saved;
		return LDAP_RECORD_ERR_INVALID_FIELD;
	}
	/* Only when TURNING it on: a fleet that is already serving TLS has
	 * certificates by construction, and re-checking would refuse an
	 * unrelated PUT because some other container is momentarily
	 * mid-restart. */
	if (g_config.server_tls && !saved.server_tls &&
	    servers_have_tls_certs(out_container, out_container_size) != 0) {
		g_config = saved;
		return LDAP_RECORD_ERR_NO_SERVER_CERT;
	}

	if (touched_server)
		g_config.listeners_managed = 1;

	rc = ldap_config_persist();
	if (rc != LDAP_RECORD_OK) {
		g_config = saved;
		return rc;
	}
	/* Push the new listener state into every registered server now,
	 * rather than waiting for the next user/group write to carry it --
	 * the whole deliverable of #419 is that flipping a listener is one
	 * PUT, not a recipe edit and a recreate. glauth's own config
	 * watcher reloads on the write; no signal is sent (its
	 * watchconfig = true is confirmed against upstream's own
	 * startConfigWatcher()). */
	if (touched_server)
		ldap_record_sync_all();
	return LDAP_RECORD_OK;
}

enum ldap_record_error ldap_config_set_client(const char *client_uri, const char *base_dn,
                                               const char *bind_dn, const char *bind_password)
{
	struct ldap_config saved = g_config;
	enum ldap_record_error rc;

	if (client_uri != NULL)
		snprintf(g_config.client_uri, sizeof(g_config.client_uri), "%s", client_uri);
	if (base_dn != NULL)
		snprintf(g_config.base_dn, sizeof(g_config.base_dn), "%s", base_dn);
	if (bind_dn != NULL)
		snprintf(g_config.bind_dn, sizeof(g_config.bind_dn), "%s", bind_dn);
	if (bind_password != NULL)
		snprintf(g_config.bind_password, sizeof(g_config.bind_password), "%s", bind_password);
	rc = ldap_config_persist();
	if (rc != LDAP_RECORD_OK)
		g_config = saved;
	return rc;
}

/*
 * The open half: object opened, every stored field written, NOT closed
 * -- so a caller can append a field of its own before closing. The one
 * caller that does is GET /ldap/config, which appends
 * effective_client_uri (issue #84): what clients are actually handed
 * right now is a different question from what is configured, and
 * deriving it needs the registry and health state, neither of which
 * this module can see.
 */
void ldap_config_write_json_open(struct json_writer *w)
{
	/*
	 * Sync g_config.base_dn to the canonical hostauth value before
	 * reading it below (issue #80) -- so the reported base_dn matches
	 * what the nslcd auto-staging and {{LDAP:BASE_DN}} token actually
	 * use, never a stale stored copy. ldap_config_get() does the overlay.
	 */
	ldap_config_get();

	jw_obj_open(w);
	jw_key(w, "start_uid");
	jw_int(w, g_config.start_uid);
	jw_key(w, "start_gid");
	jw_int(w, g_config.start_gid);
	/* Issue #66: client-login fields. The bind credential is NEVER
	 * echoed -- only whether one is set (the PKI-private-key posture). */
	jw_key(w, "client_uri");
	if (g_config.client_uri[0] != '\0')
		jw_str(w, g_config.client_uri);
	else
		jw_null(w);
	jw_key(w, "base_dn");
	if (g_config.base_dn[0] != '\0')
		jw_str(w, g_config.base_dn);
	else
		jw_null(w);
	jw_key(w, "bind_dn");
	if (g_config.bind_dn[0] != '\0')
		jw_str(w, g_config.bind_dn);
	else
		jw_null(w);
	jw_key(w, "client_tls");
	jw_bool(w, g_config.client_tls);
	jw_key(w, "listeners_managed");
	jw_bool(w, g_config.listeners_managed);
	jw_key(w, "server_plaintext");
	jw_bool(w, g_config.server_plaintext);
	jw_key(w, "server_plaintext_port");
	jw_int(w, g_config.server_plaintext_port);
	jw_key(w, "server_tls");
	jw_bool(w, g_config.server_tls);
	jw_key(w, "server_tls_port");
	jw_int(w, g_config.server_tls_port);
	jw_key(w, "bind_password_set");
	jw_bool(w, g_config.bind_password[0] != '\0');
}

void ldap_config_write_json(struct json_writer *w)
{
	ldap_config_write_json_open(w);
	jw_obj_close(w);
}

int ldap_user_uidnumber_in_range(long long lo, long long hi)
{
	int i;

	for (i = 0; i < LDAP_USER_MAX; i++) {
		if (g_users[i].name[0] != '\0' && (long long)g_users[i].uidnumber >= lo &&
		    (long long)g_users[i].uidnumber <= hi)
			return 1;
	}
	return 0;
}

struct ldap_group *ldap_group_find(const char *name)
{
	int i;

	for (i = 0; i < LDAP_GROUP_MAX; i++) {
		if (g_groups[i].name[0] != '\0' && strcmp(g_groups[i].name, name) == 0)
			return &g_groups[i];
	}
	return NULL;
}

struct ldap_group *ldap_group_find_by_gid(int gidnumber)
{
	int i;

	for (i = 0; i < LDAP_GROUP_MAX; i++) {
		if (g_groups[i].name[0] != '\0' && g_groups[i].gidnumber == gidnumber)
			return &g_groups[i];
	}
	return NULL;
}

struct ldap_user *ldap_user_find(const char *name)
{
	int i;

	for (i = 0; i < LDAP_USER_MAX; i++) {
		if (g_users[i].name[0] != '\0' && strcmp(g_users[i].name, name) == 0)
			return &g_users[i];
	}
	return NULL;
}

/*
 * Appends a TOML basic (double-quoted) string literal for s onto
 * buf[*off..bufsize), escaping backslash/quote/control characters --
 * these fields (givenname/sn/mail/loginshell/homedirectory) are
 * REST-supplied free text, so this is real injection prevention, not
 * defensive-for-its-own-sake: an unescaped '"' in, say, a submitted
 * "sn" value could otherwise break out of the string and let the
 * request forge additional TOML keys/stanzas in glauth's own config.
 * *off is left at bufsize (a sentinel) once truncated, so every
 * subsequent append becomes a cheap no-op instead of corrupting
 * already-written content -- checked once at the end by the caller.
 */
static void toml_append_string(char *buf, size_t bufsize, size_t *off, const char *s)
{
	size_t i;

	if (*off >= bufsize)
		return;
	buf[(*off)++] = '"';
	for (i = 0; s[i] != '\0' && *off < bufsize; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c == '\\' || c == '"') {
			if (*off + 2 > bufsize) {
				*off = bufsize;
				return;
			}
			buf[(*off)++] = '\\';
			buf[(*off)++] = (char)c;
		} else if (c == '\n' || c == '\r' || c == '\t' || c < 0x20) {
			char esc[7];
			int n;

			if (c == '\n')
				n = snprintf(esc, sizeof(esc), "\\n");
			else if (c == '\r')
				n = snprintf(esc, sizeof(esc), "\\r");
			else if (c == '\t')
				n = snprintf(esc, sizeof(esc), "\\t");
			else
				n = snprintf(esc, sizeof(esc), "\\u%04x", c);
			if (n < 0 || *off + (size_t)n > bufsize) {
				*off = bufsize;
				return;
			}
			memcpy(buf + *off, esc, (size_t)n);
			*off += (size_t)n;
		} else {
			buf[(*off)++] = (char)c;
		}
	}
	if (*off >= bufsize) {
		*off = bufsize;
		return;
	}
	buf[(*off)++] = '"';
}

static void toml_append_raw(char *buf, size_t bufsize, size_t *off, const char *s)
{
	size_t len;

	if (*off >= bufsize)
		return;
	len = strlen(s);
	if (*off + len >= bufsize) {
		*off = bufsize;
		return;
	}
	memcpy(buf + *off, s, len);
	*off += len;
}

static void toml_append_int(char *buf, size_t bufsize, size_t *off, int v)
{
	char num[16];

	snprintf(num, sizeof(num), "%d", v);
	toml_append_raw(buf, bufsize, off, num);
}

/*
 * Renders every currently-defined user/group as glauth "config"
 * datastore TOML array-of-tables stanzas (field names/casing verified
 * directly against glauth's own v2/pkg/config/config.go User/Group
 * structs -- e.g. "homedir", not "homedirectory", a real difference
 * from the SQLite backend's own column name this project briefly
 * considered) into buf. Returns the number of bytes written, or (size_t)-1
 * if the content didn't fit (matching dns_write_hosts_file()'s own
 * "truncate gracefully, never overrun" discipline).
 */
static size_t render_users_groups_toml(char *buf, size_t bufsize)
{
	size_t off = 0;
	int i;

	for (i = 0; i < LDAP_GROUP_MAX; i++) {
		const struct ldap_group *g = &g_groups[i];

		if (g->name[0] == '\0')
			continue;
		toml_append_raw(buf, bufsize, &off, "\n[[groups]]\nname = ");
		toml_append_string(buf, bufsize, &off, g->name);
		toml_append_raw(buf, bufsize, &off, "\ngidnumber = ");
		toml_append_int(buf, bufsize, &off, g->gidnumber);
		toml_append_raw(buf, bufsize, &off, "\n");
	}
	for (i = 0; i < LDAP_USER_MAX; i++) {
		const struct ldap_user *u = &g_users[i];

		if (u->name[0] == '\0')
			continue;
		toml_append_raw(buf, bufsize, &off, "\n[[users]]\nname = ");
		toml_append_string(buf, bufsize, &off, u->name);
		toml_append_raw(buf, bufsize, &off, "\nuidnumber = ");
		toml_append_int(buf, bufsize, &off, u->uidnumber);
		toml_append_raw(buf, bufsize, &off, "\nprimarygroup = ");
		toml_append_int(buf, bufsize, &off, u->primarygroup);
		if (u->secondary_group_count > 0) {
			int j;

			/* ADR-0144: glauth's own User.OtherGroups []int, field name
			 * lowercased with no toml tag -- same "verified directly
			 * against glauth's real struct, no tag means lowercase the
			 * exact Go field name" convention every other field in this
			 * render function already follows (see this function's own
			 * top comment). */
			toml_append_raw(buf, bufsize, &off, "\nothergroups = [");
			for (j = 0; j < u->secondary_group_count; j++) {
				if (j > 0)
					toml_append_raw(buf, bufsize, &off, ", ");
				toml_append_int(buf, bufsize, &off, u->secondary_groups[j]);
			}
			toml_append_raw(buf, bufsize, &off, "]");
		}
		toml_append_raw(buf, bufsize, &off, "\ngivenname = ");
		toml_append_string(buf, bufsize, &off, u->givenname);
		toml_append_raw(buf, bufsize, &off, "\nsn = ");
		toml_append_string(buf, bufsize, &off, u->sn);
		toml_append_raw(buf, bufsize, &off, "\nmail = ");
		toml_append_string(buf, bufsize, &off, u->mail);
		toml_append_raw(buf, bufsize, &off, "\nloginshell = ");
		toml_append_string(buf, bufsize, &off, u->loginshell);
		toml_append_raw(buf, bufsize, &off, "\nhomedir = ");
		toml_append_string(buf, bufsize, &off, u->homedirectory);
		toml_append_raw(buf, bufsize, &off, "\ndisabled = ");
		toml_append_raw(buf, bufsize, &off, u->disabled ? "true" : "false");
		if (u->passbcrypt[0] != '\0') {
			/*
			 * ADR-0144: glauth's own config-backend bind path (pkg/handler/
			 * ldapopshelper.go) calls hex.DecodeString(user.PassBcrypt)
			 * BEFORE treating the result as the actual bcrypt hash bytes
			 * to compare against -- confirmed directly against glauth's
			 * own real source, and against live behavior: writing the
			 * literal bcrypt string here (an earlier version of this
			 * function did exactly that) parses fine as TOML, but every
			 * single bind then failed with glauth's own "invalid
			 * credentials, incorrect stored hash", since hex-decoding an
			 * already-ASCII "$2b$12$..." string does not recover that
			 * same string. The bcrypt hash is ASCII-only (base64 alphabet
			 * plus "$"), so this is a plain byte-for-byte hex encoding of
			 * the string's own bytes, not a real transcoding -- glauth
			 * hex-decodes it right back before ever touching bcrypt.
			 */
			char passbcrypt_hex[PWHASH_BCRYPT_LEN * 2 + 1];
			size_t k;

			for (k = 0; u->passbcrypt[k] != '\0' && k < sizeof(u->passbcrypt) - 1; k++)
				snprintf(passbcrypt_hex + k * 2, 3, "%02x", (unsigned char)u->passbcrypt[k]);
			passbcrypt_hex[k * 2] = '\0';

			toml_append_raw(buf, bufsize, &off, "\npassbcrypt = ");
			toml_append_string(buf, bufsize, &off, passbcrypt_hex);
		}
		if (u->ssh_public_key[0] != '\0') {
			/*
			 * ADR-0144 task #838: glauth's own User.SSHKeys []string
			 * (pkg/config/config.go), TOML tag "sshkeys" -- the real LDAP
			 * Public Key (LPK) convention, exposed as the `sshPublicKey`
			 * attribute by default (glauth's own backend-level SSHKeyAttr,
			 * left at its default here since this project stages no
			 * override). This project's own data model holds exactly one
			 * key per user (ldap_user's own single ssh_public_key field,
			 * not a list), so this always renders a single-element array
			 * -- glauth's own field is a real []string regardless of how
			 * many entries are populated, confirmed directly against its
			 * source (same "verified against glauth's real struct"
			 * convention every other field in this function follows).
			 * This is what a live AuthorizedKeysCommand ldapsearch (task
			 * #838's own next part) queries for -- replacing the
			 * file-rendered authorized_keys mechanism this whole ADR set
			 * out to retire.
			 */
			toml_append_raw(buf, bufsize, &off, "\nsshkeys = [");
			toml_append_string(buf, bufsize, &off, u->ssh_public_key);
			toml_append_raw(buf, bufsize, &off, "]");
		}
		toml_append_raw(buf, bufsize, &off, "\n");
		if (u->can_search) {
			/* Task #727: a minimal, self-only capability grant for an
			 * auto-provisioned service account -- syntax verified
			 * directly against glauth's own real sample config
			 * (v2/sample-simple.cfg's own "[[users.capabilities]]"
			 * nested array-of-tables under a [[users]] entry). */
			toml_append_raw(buf, bufsize, &off,
			                 "  [[users.capabilities]]\n  action = \"search\"\n  object = \"*\"\n");
		}
	}
	return off >= bufsize ? (size_t)-1 : off;
}

/*
 * Writes the current user/group set into full_path -- the container's
 * own glauth config file, reached via /proc/<pid>/root/<config_path>
 * exactly like dns_write_hosts_file() reaches a DNS server's own
 * hosts file. Unlike that file, this one also carries the operator's
 * own bootstrap settings (backend/listen/TLS, staged once at container
 * creation via the same --file= convention lldap.recipe already
 * established), which must survive untouched: this function reads the
 * CURRENT file content first, finds the first "\n[[users]]" or
 * "\n[[groups]]" byte offset (an unambiguous TOML array-of-tables
 * boundary -- these headers can only start a line at column 0) and
 * truncates there before appending the freshly-rendered tail. No
 * marker found yet (a fresh base config with no managed section):
 * the fresh content is simply appended at the existing EOF. No SIGHUP
 * or other signal is sent -- glauth's own fsnotify config watcher
 * (v2/glauth.go's startConfigWatcher(), active whenever the operator's
 * base config sets `watchconfig = true`) notices the write and
 * reloads on its own, confirmed directly against glauth's real source.
 */
/*
 * ADR-0148: finds a "baseDN = "..."" line anywhere within [prefix,
 * prefix+prefix_len) -- the operator-authored portion of a registered
 * glauth server's own config, never the managed [[users]]/[[groups]]
 * tail this file's own render already owns -- and rewrites just its
 * quoted value to new_basedn, leaving every other byte (the line's
 * own indentation, everything else in [backend]/[ldap]/[behaviors])
 * untouched. Matches glauth's own real, confirmed convention
 * (double-quoted TOML basic strings, e.g. `baseDN = "dc=glauth,dc=com"`
 * -- see sample-simple.cfg) -- a config spelling it with single quotes,
 * or omitting it entirely, is a legitimate no-op (*out_buf untouched,
 * returns 0): this function only ever edits a value already present in
 * the exact form it recognizes, it never invents structure the
 * operator's own file doesn't already have. On a real match, *out_buf
 * is a fresh malloc'd copy of the full rewritten prefix (caller frees)
 * and *out_len its length; returns 1. Returns -1 only on malloc
 * failure. */
static int rewrite_basedn(const char *prefix, size_t prefix_len, const char *new_basedn,
                           char **out_buf, size_t *out_len)
{
	const char *end = prefix + prefix_len;
	const char *line_start = prefix;
	const char *key = NULL;
	const char *eq, *quote_open, *quote_close;
	size_t head_len, new_len, tail_len;
	char *buf;

	for (;;) {
		const char *t = line_start;
		const char *nl;

		while (t < end && (*t == ' ' || *t == '\t'))
			t++;
		if ((size_t)(end - t) >= 6 && strncmp(t, "baseDN", 6) == 0) {
			key = t;
			break;
		}
		nl = memchr(line_start, '\n', (size_t)(end - line_start));
		if (nl == NULL)
			return 0; /* no baseDN line at all -- legitimate no-op */
		line_start = nl + 1;
	}

	eq = key + 6;
	while (eq < end && (*eq == ' ' || *eq == '\t'))
		eq++;
	if (eq >= end || *eq != '=')
		return 0;
	eq++;
	while (eq < end && (*eq == ' ' || *eq == '\t'))
		eq++;
	if (eq >= end || *eq != '"')
		return 0; /* not a double-quoted value -- see this function's own comment */
	quote_open = eq;
	quote_close = memchr(quote_open + 1, '"', (size_t)(end - (quote_open + 1)));
	if (quote_close == NULL)
		return 0;

	head_len = (size_t)(quote_open + 1 - prefix);
	new_len = strlen(new_basedn);
	tail_len = (size_t)(end - quote_close);

	buf = malloc(head_len + new_len + tail_len);
	if (buf == NULL)
		return -1;
	memcpy(buf, prefix, head_len);
	memcpy(buf + head_len, new_basedn, new_len);
	memcpy(buf + head_len + new_len, quote_close, tail_len);
	*out_buf = buf;
	*out_len = head_len + new_len + tail_len;
	return 1;
}

/*
 * #419: rewrites one TOML section's own key/value lines inside the
 * operator-authored prefix, so glauth's [ldap]/[ldaps] listeners are
 * configuration rather than literal recipe text. Same ownership terms
 * ADR-0148 set for baseDN, one step further: the operator's file
 * supplies the initial value and the API owns it from then on.
 *
 * Deliberately less conservative than rewrite_basedn(), which is a
 * no-op when the line is absent because inventing a baseDN would be
 * inventing policy. Here an absent section IS the normal state of a
 * hand-written config, and leaving it alone would mean the switch
 * silently does nothing -- the #418 failure shape. So: a key present
 * in the section is rewritten in place, a key absent is inserted at the
 * section's end, and an absent section is appended whole.
 *
 * THE SECTION HEADER MUST MATCH EXACTLY, not by prefix. "[ldap]" is a
 * prefix of "[ldaps]", so a strncmp here would rewrite the wrong
 * section's `enabled` and produce the exact inverse of what was asked
 * -- on every registered server at once, on one PUT. The header is
 * therefore matched at column 0 with its closing bracket and only
 * horizontal whitespace to end of line.
 *
 * Returns 0 with *out_buf/*out_len set to a fresh malloc'd prefix
 * (caller frees), or -1 on allocation failure.
 */
static const char *section_bound(const char *p, const char *end, const char *name)
{
	size_t nlen = strlen(name);
	const char *line = p;

	while (line < end) {
		const char *nl = memchr(line, '\n', (size_t)(end - line));
		const char *eol = nl != NULL ? nl : end;

		if ((size_t)(eol - line) >= nlen + 2 && line[0] == '[' &&
		    strncmp(line + 1, name, nlen) == 0 && line[1 + nlen] == ']') {
			const char *t = line + 2 + nlen;

			while (t < eol && (*t == ' ' || *t == '\t' || *t == '\r'))
				t++;
			if (t == eol)
				return line;
		}
		if (nl == NULL)
			break;
		line = nl + 1;
	}
	return NULL;
}

/* End of the section starting at `hdr`: the next line that begins a new
 * table at column 0 ('[' in the first column), or `end`. */
static const char *section_end(const char *hdr, const char *end)
{
	const char *nl = memchr(hdr, '\n', (size_t)(end - hdr));
	const char *line = nl != NULL ? nl + 1 : end;

	while (line < end) {
		if (*line == '[')
			return line;
		nl = memchr(line, '\n', (size_t)(end - line));
		if (nl == NULL)
			break;
		line = nl + 1;
	}
	return end;
}

/* The "  key = ..." line for `key` within [sec_start, sec_end), or NULL.
 * Leading whitespace is skipped, so glauth's own two-space indentation
 * and an unindented spelling both match. */
static const char *section_key_line(const char *sec_start, const char *sec_end, const char *key,
                                     const char **out_line_end)
{
	size_t klen = strlen(key);
	const char *nl = memchr(sec_start, '\n', (size_t)(sec_end - sec_start));
	const char *line = nl != NULL ? nl + 1 : sec_end;

	while (line < sec_end) {
		const char *le = memchr(line, '\n', (size_t)(sec_end - line));
		const char *eol = le != NULL ? le : sec_end;
		const char *t = line;

		while (t < eol && (*t == ' ' || *t == '\t'))
			t++;
		if ((size_t)(eol - t) > klen && strncmp(t, key, klen) == 0) {
			const char *a = t + klen;

			while (a < eol && (*a == ' ' || *a == '\t'))
				a++;
			if (a < eol && *a == '=') {
				*out_line_end = le != NULL ? le + 1 : eol;
				return line;
			}
		}
		if (le == NULL)
			break;
		line = le + 1;
	}
	return NULL;
}

struct kv {
	const char *key;
	char value[128];
};

static int rewrite_section(const char *prefix, size_t prefix_len, const char *section,
                            const struct kv *kvs, int kv_count, char **out_buf, size_t *out_len)
{
	const char *end = prefix + prefix_len;
	const char *hdr = section_bound(prefix, end, section);
	char *buf;
	size_t cap = prefix_len + 512 + (size_t)kv_count * 160;
	size_t n = 0;
	int i;

	buf = malloc(cap);
	if (buf == NULL)
		return -1;

	if (hdr == NULL) {
		/* No such section: append it whole, with a blank line before
		 * it so the result stays readable TOML rather than merely
		 * valid. A trailing newline is added first if the file lacks
		 * one, so the header cannot land mid-line. */
		memcpy(buf, prefix, prefix_len);
		n = prefix_len;
		if (n > 0 && buf[n - 1] != '\n')
			buf[n++] = '\n';
		n += (size_t)snprintf(buf + n, cap - n, "\n[%s]\n", section);
		for (i = 0; i < kv_count; i++)
			n += (size_t)snprintf(buf + n, cap - n, "  %s = %s\n", kvs[i].key,
			                       kvs[i].value);
		*out_buf = buf;
		*out_len = n;
		return 0;
	}

	{
		const char *sec_end = section_end(hdr, end);
		const char *hdr_nl = memchr(hdr, '\n', (size_t)(end - hdr));
		const char *body = hdr_nl != NULL ? hdr_nl + 1 : sec_end;

		/* everything up to and including the header line */
		memcpy(buf, prefix, (size_t)(body - prefix));
		n = (size_t)(body - prefix);

		/* the section body, with matched keys replaced in place */
		{
			const char *line = body;

			while (line < sec_end) {
				const char *le = memchr(line, '\n', (size_t)(sec_end - line));
				const char *next = le != NULL ? le + 1 : sec_end;
				const char *t = line;
				int replaced = 0;

				while (t < (le != NULL ? le : sec_end) && (*t == ' ' || *t == '\t'))
					t++;
				for (i = 0; i < kv_count; i++) {
					size_t klen = strlen(kvs[i].key);
					const char *a = t + klen;
					const char *eol = le != NULL ? le : sec_end;

					if ((size_t)(eol - t) <= klen || strncmp(t, kvs[i].key, klen) != 0)
						continue;
					while (a < eol && (*a == ' ' || *a == '\t'))
						a++;
					if (a >= eol || *a != '=')
						continue;
					n += (size_t)snprintf(buf + n, cap - n, "  %s = %s\n", kvs[i].key,
					                       kvs[i].value);
					replaced = 1;
					break;
				}
				if (!replaced) {
					memcpy(buf + n, line, (size_t)(next - line));
					n += (size_t)(next - line);
				}
				line = next;
			}
		}

		/* keys the section did not already carry */
		for (i = 0; i < kv_count; i++) {
			const char *le = NULL;

			if (section_key_line(hdr, sec_end, kvs[i].key, &le) == NULL)
				n += (size_t)snprintf(buf + n, cap - n, "  %s = %s\n", kvs[i].key,
				                       kvs[i].value);
		}

		/* everything from the next table onward, untouched */
		memcpy(buf + n, sec_end, (size_t)(end - sec_end));
		n += (size_t)(end - sec_end);
	}
	*out_buf = buf;
	*out_len = n;
	return 0;
}

/*
 * Renders both listener sections into the prefix. A no-op -- byte for
 * byte -- while listeners_managed is false, which is the state of every
 * install that predates #419: a default derived from nothing would
 * otherwise rewrite two live, working listeners on the first boot after
 * upgrade, and glauth's config watcher would apply it within seconds.
 * Same skip ldap_write_config_file() already performs for baseDN when
 * ldap_base_dn has never been set.
 *
 * cert/key are written only when [ldaps] is being created: an existing
 * section's own paths are whatever that container was built with, and
 * this has no per-container knowledge of them (pki_cert_dir is a create
 * parameter the registry does not record). The default is the one
 * PKI_CONTAINER_CERT_DIR names, which is where pki_issue delivers.
 *
 * The listen value is rewritten whole to "0.0.0.0:<port>". Preserving a
 * custom bind address was considered and rejected: parsing it back out
 * to keep the host half would make this function the authority on a
 * value the API cannot express, and a half-owned field is how baseDN
 * drifted in the first place (ADR-0148).
 */
static int rewrite_listeners(const char *prefix, size_t prefix_len, char **out_buf,
                             size_t *out_len)
{
	struct kv plain[2], tls[4];
	char *stage = NULL;
	size_t stage_len = 0;

	if (!g_config.listeners_managed || prefix_len == 0)
		return 0;

	plain[0].key = "enabled";
	snprintf(plain[0].value, sizeof(plain[0].value), "%s",
	          g_config.server_plaintext ? "true" : "false");
	plain[1].key = "listen";
	snprintf(plain[1].value, sizeof(plain[1].value), "\"0.0.0.0:%d\"",
	          g_config.server_plaintext_port);

	tls[0].key = "enabled";
	snprintf(tls[0].value, sizeof(tls[0].value), "%s", g_config.server_tls ? "true" : "false");
	tls[1].key = "listen";
	snprintf(tls[1].value, sizeof(tls[1].value), "\"0.0.0.0:%d\"", g_config.server_tls_port);
	tls[2].key = "cert";
	snprintf(tls[2].value, sizeof(tls[2].value), "\"%s/tls.crt\"", PKI_CONTAINER_CERT_DIR);
	tls[3].key = "key";
	snprintf(tls[3].value, sizeof(tls[3].value), "\"%s/tls.key\"", PKI_CONTAINER_CERT_DIR);

	if (rewrite_section(prefix, prefix_len, "ldap", plain, 2, &stage, &stage_len) != 0)
		return -1;
	{
		char *final = NULL;
		size_t final_len = 0;
		/* cert/key only when creating the section -- see above. */
		int tls_kvs = section_bound(stage, stage + stage_len, "ldaps") != NULL ? 2 : 4;

		if (rewrite_section(stage, stage_len, "ldaps", tls, tls_kvs, &final, &final_len) != 0) {
			free(stage);
			return -1;
		}
		free(stage);
		*out_buf = final;
		*out_len = final_len;
	}
	return 1;
}

/*
 * Public form of rewrite_listeners(), for staging (#419).
 *
 * The render below reaches a container that is already running, and
 * that is not enough on its own: glauth binds its listeners at startup
 * and its config watcher reloads only the record datastore, so a
 * listener written into a live config is not adopted. Worse, a restart
 * re-stages files[] from the persisted definition -- the recipe's own
 * value -- and resync_managed_services() runs after the container is
 * already up, so the re-render always lands after glauth has read its
 * config. Both measured on 192.168.15.95, v2.57.114: `[ldap] enabled`
 * read true in both live configs while 3893 stayed refused, and a real
 * stop/start of ldap-1 did not change that.
 *
 * So the values are rendered into the staged content too, before
 * clone3(), which is the same ordering fix pki_cert_deliver() needed in
 * #414. Returns 1 with *out_buf/*out_len set (caller frees), 0 when
 * there is nothing to do (listeners unmanaged, or empty content), -1 on
 * allocation failure.
 */
int ldap_render_listeners(const char *content, size_t content_len, char **out_buf,
                          size_t *out_len)
{
	return rewrite_listeners(content, content_len, out_buf, out_len);
}

static int ldap_write_config_file(const char *full_path)
{
	char *current = NULL;
	size_t current_len = 0;
	size_t prefix_len;
	const char *users_marker, *groups_marker;
	/*
	 * Sized comfortably above LDAP_USER_MAX * (worst-case escaped
	 * field lengths) + LDAP_GROUP_MAX * (worst case) -- see
	 * render_users_groups_toml()'s own field list; ~1.6KB/user *
	 * 256 + ~150B/group * 64 is under 420KB, so this is a defensive
	 * backstop that should never actually trigger, not a real,
	 * silent record-dropping path (same posture as dns_write_hosts_
	 * file()'s own sizing comment).
	 */
	static char rendered[524288];
	size_t rendered_len;
	char *final_buf;
	size_t final_len;
	int rc;

	rendered_len = render_users_groups_toml(rendered, sizeof(rendered));
	if (rendered_len == (size_t)-1)
		return -1;

	/* A missing/unreadable current file is not fatal -- persist_read_
	 * file() itself distinguishes "doesn't exist" from a real error;
	 * either way, falling back to prefix_len == 0 just means the
	 * fresh managed section becomes the entire file, which is the
	 * correct behavior for the very first write. */
	if (persist_read_file(full_path, &current, &current_len) != 0) {
		current = NULL;
		current_len = 0;
	}

	prefix_len = (current != NULL) ? current_len : 0;
	if (current != NULL) {
		users_marker = memmem(current, current_len, "\n[[users]]", 10);
		groups_marker = memmem(current, current_len, "\n[[groups]]", 11);
		if (users_marker != NULL && (groups_marker == NULL || users_marker < groups_marker))
			prefix_len = (size_t)(users_marker - current);
		else if (groups_marker != NULL)
			prefix_len = (size_t)(groups_marker - current);
	}

	/* ADR-0148: the prefix's own baseDN, if it has one in the form
	 * recognized, is kept in sync with hostauth-config's real,
	 * canonical ldap_base_dn -- an empty ldap_base_dn (never yet
	 * configured) intentionally skips this rather than blanking out
	 * whatever the operator already has working. */
	{
		const char *canonical_basedn = hostauth_ldap_base_dn();
		char *rewritten_prefix = NULL;
		size_t rewritten_len = 0;

		if (prefix_len > 0 && canonical_basedn[0] != '\0') {
			int rrc = rewrite_basedn(current, prefix_len, canonical_basedn, &rewritten_prefix,
			                          &rewritten_len);

			if (rrc < 0) {
				free(current);
				return -1;
			}
			if (rrc == 1) {
				free(current);
				current = rewritten_prefix;
				prefix_len = rewritten_len; /* current is now exactly the (rewritten) prefix */
			}
		}
	}

	/* #419: and the listener sections, on the same terms -- after
	 * baseDN so both operate on the same prefix, and a no-op while
	 * listeners_managed is false. */
	{
		char *relisten = NULL;
		size_t relisten_len = 0;
		int lrc = rewrite_listeners(current, prefix_len, &relisten, &relisten_len);

		if (lrc < 0) {
			free(current);
			return -1;
		}
		if (lrc == 1) {
			free(current);
			current = relisten;
			prefix_len = relisten_len;
		}
	}

	final_len = prefix_len + rendered_len;
	final_buf = malloc(final_len > 0 ? final_len : 1);
	if (final_buf == NULL) {
		free(current);
		return -1;
	}
	if (prefix_len > 0)
		memcpy(final_buf, current, prefix_len);
	memcpy(final_buf + prefix_len, rendered, rendered_len);
	free(current);

	/* full_path's parent directories aren't guaranteed to exist -- a
	 * minimal container image may have no /etc at all. mkdir -p them
	 * first, matching dns_write_hosts_file()'s own identical need. */
	{
		char parent[LDAP_SERVER_PATH_MAX + 64];
		char *slash;

		if (snprintf(parent, sizeof(parent), "%s", full_path) < (int)sizeof(parent)) {
			slash = strrchr(parent, '/');
			if (slash != NULL && slash != parent) {
				*slash = '\0';
				persist_mkdir_p(parent);
			}
		}
	}

	/* In place, not a rename: this file lives inside a running
	 * container, and a rename into an overlay upper layer is invisible
	 * through the merged mount the container reads (#276). The records
	 * themselves are still written atomically -- see save_users_state()
	 * -- and this config is regenerated from them, so it is derived
	 * state and safe to write this way. */
	rc = persist_write_file_inplace(full_path, final_buf, final_len);
	free(final_buf);
	return rc;
}

/* Write-through: pushes the full current user/group set to every
 * currently-registered, currently-running LDAP server (mirrors dns_
 * server_sync_all()'s own "every binding, full re-render" behavior --
 * normally there's just one, but nothing here assumes that). Best-
 * effort per binding: a server that's unreachable right now is simply
 * skipped and stays stale until it next registers -- ldap_server_
 * register() calls this, so a fresh/replacement instance always starts
 * current.
 *
 * That last sentence was FALSE from when it was written until #418
 * (2026-09-12): the call lived in handle_ldap_server_create(), so it
 * held for POST /ldap/servers and not for a container declaring the
 * role in its own definition. It is true now because the call was moved
 * into ldap_server_register() rather than the comment being softened --
 * recorded here because a comment asserting a guarantee is what the
 * next reader believes instead of reading the code, and this one cost a
 * live directory. */
void ldap_record_sync_all(void)
{
	int i;

	for (i = 0; i < LDAP_SERVER_MAX; i++) {
		struct registry_entry *entry;
		char full_path[LDAP_SERVER_PATH_MAX + 32];

		if (g_bindings[i].container_name[0] == '\0')
			continue;
		entry = registry_find(g_bindings[i].container_name);
		if (entry == NULL || !entry->running) {
			/*
			 * A registered LDAP server that does not get its config
			 * re-rendered is serving whatever it last received, which
			 * is exactly the situation ADR-0146 was written after --
			 * so say so rather than continuing in silence.
			 *
			 * This used to be a bare `continue`, and that silence cost
			 * real time: five separate hypotheses about #276 had to be
			 * eliminated one build at a time precisely because the one
			 * path that does nothing said nothing when it did.
			 *
			 * Both stderr and the log store on purpose. stderr is not
			 * mirrored into the log store, and a test harness capturing
			 * a forked cixd sees only stderr, so a log-store-only line
			 * would be invisible exactly where this matters most.
			 */
			fprintf(stderr, "ldap: %s registered but %s -- config NOT re-rendered\n",
			        g_bindings[i].container_name,
			        entry == NULL ? "no such container" : "not running");
			logstore_write("cixd", "warn",
			                "ldap: %s is registered but %s -- its config was not re-rendered, so "
			                "it is serving whatever it last received",
			                g_bindings[i].container_name,
			                entry == NULL ? "no such container" : "not running");
			continue;
		}
		/*
		 * The container's tree on the HOST side, not through its own
		 * /proc/<pid>/root view (#269). A btrfs-backed userns
		 * container's rootfs is an id-mapped mount, and the daemon has
		 * no mapped identity there, so writing through it is refused.
		 * Measured: glauth on such a container served an empty user
		 * list and rejected every bind, while this loop reported
		 * nothing at all.
		 */
		container_file_host_path(g_bindings[i].container_name, entry->disk_name,
		                          g_bindings[i].config_path, full_path, sizeof(full_path));
		/* #276: which file, for which container, on every render. The
		 * bug being chased is that creates appear and updates do not,
		 * and the first thing that distinguishes those is whether the
		 * second write targets the same path as the first. */
		fprintf(stderr, "ldap: rendering %s -> %s\n", g_bindings[i].container_name, full_path);
		if (ldap_write_config_file(full_path) != 0)
			logstore_write("cixd", "error",
			                "ldap: could not write %s config to %s -- this server is now "
			                "serving stale or empty user data",
			                g_bindings[i].container_name, full_path);
	}
}

enum ldap_record_error ldap_group_create(const char *name, int gidnumber, struct ldap_group **out)
{
	int i, slot = -1;
	struct ldap_group *g;

	if (!ldap_groupname_is_valid(name))
		return LDAP_RECORD_ERR_INVALID_NAME;
	if (gidnumber <= 0)
		return LDAP_RECORD_ERR_INVALID_FIELD;
	if (ldap_group_find(name) != NULL)
		return LDAP_RECORD_ERR_DUPLICATE;

	for (i = 0; i < LDAP_GROUP_MAX; i++) {
		if (g_groups[i].name[0] == '\0') {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return LDAP_RECORD_ERR_FULL;

	g = &g_groups[slot];
	memset(g, 0, sizeof(*g));
	strncpy(g->name, name, sizeof(g->name) - 1);
	g->gidnumber = gidnumber;

	if (save_groups_state() != 0) {
		memset(g, 0, sizeof(*g));
		return LDAP_RECORD_ERR_PERSIST_FAILED;
	}

	ldap_record_sync_all();
	*out = g;
	return LDAP_RECORD_OK;
}

enum ldap_record_error ldap_group_delete(const char *name)
{
	struct ldap_group *g = ldap_group_find(name);

	if (g == NULL)
		return LDAP_RECORD_ERR_NOT_FOUND;

	memset(g, 0, sizeof(*g));
	if (save_groups_state() != 0)
		return LDAP_RECORD_ERR_PERSIST_FAILED;

	ldap_record_sync_all();
	return LDAP_RECORD_OK;
}

enum ldap_record_error ldap_group_update(const char *name, const char *new_name, int gidnumber,
                                          struct ldap_group **out)
{
	struct ldap_group *g = ldap_group_find(name);
	struct ldap_group *collision;
	int old_gidnumber;
	int renaming = new_name != NULL && new_name[0] != '\0' && strcmp(new_name, name) != 0;
	char old_name[LDAP_GROUP_NAME_MAX];

	if (g == NULL)
		return LDAP_RECORD_ERR_NOT_FOUND;
	if (gidnumber <= 0)
		return LDAP_RECORD_ERR_INVALID_FIELD;
	if (renaming) {
		if (!ldap_groupname_is_valid(new_name))
			return LDAP_RECORD_ERR_INVALID_NAME;
		if (ldap_group_find(new_name) != NULL)
			return LDAP_RECORD_ERR_DUPLICATE;
	}

	collision = ldap_group_find_by_gid(gidnumber);
	if (collision != NULL && collision != g)
		return LDAP_RECORD_ERR_DUPLICATE;

	old_gidnumber = g->gidnumber;
	g->gidnumber = gidnumber;
	if (renaming) {
		snprintf(old_name, sizeof(old_name), "%s", g->name);
		snprintf(g->name, sizeof(g->name), "%s", new_name);
	}

	if (save_groups_state() != 0) {
		g->gidnumber = old_gidnumber;
		if (renaming)
			snprintf(g->name, sizeof(g->name), "%s", old_name);
		return LDAP_RECORD_ERR_PERSIST_FAILED;
	}

	/* admin_groups propagation happens after groups_state is durably
	 * saved (so a persist failure there never leaves the group's own
	 * record renamed with no way to reconcile) but before this function
	 * returns success -- a caller seeing LDAP_RECORD_OK must never
	 * observe a state where the rename landed but admin_groups still
	 * names the old, now-nonexistent group. */
	if (renaming && !hostauth_rename_admin_group(old_name, new_name)) {
		snprintf(g->name, sizeof(g->name), "%s", old_name);
		g->gidnumber = old_gidnumber;
		save_groups_state(); /* best-effort revert of the on-disk group record too */
		return LDAP_RECORD_ERR_PERSIST_FAILED;
	}

	ldap_record_sync_all();
	*out = g;
	return LDAP_RECORD_OK;
}

static enum ldap_record_error validate_user_fields(int uidnumber, int primarygroup,
                                                     const int *secondary_groups,
                                                     int secondary_group_count)
{
	int i;

	if (uidnumber <= 0)
		return LDAP_RECORD_ERR_INVALID_FIELD;
	/* ADR-0179: symmetric half of the subid allocator's own collision
	 * check -- a managed user's uidnumber must never land inside an
	 * already-committed subordinate range (which would let a
	 * user-namespaced container's mapped identity collide with this
	 * real user's own host-side file ownership). Floor 100000 makes
	 * this practically unreachable for conventional uidnumbers; this
	 * is the belt-and-suspenders guarantee, not the sole defense. */
	if (subid_overlaps_uidnumber((long long)uidnumber))
		return LDAP_RECORD_ERR_INVALID_FIELD;
	if (ldap_group_find_by_gid(primarygroup) == NULL)
		return LDAP_RECORD_ERR_GROUP_NOT_FOUND;
	if (secondary_group_count < 0 || secondary_group_count > LDAP_USER_MAX_SECONDARY_GROUPS)
		return LDAP_RECORD_ERR_INVALID_FIELD;
	for (i = 0; i < secondary_group_count; i++) {
		if (ldap_group_find_by_gid(secondary_groups[i]) == NULL)
			return LDAP_RECORD_ERR_GROUP_NOT_FOUND;
	}
	return LDAP_RECORD_OK;
}

static void fill_user_fields(struct ldap_user *u, int uidnumber, int primarygroup,
                              const int *secondary_groups, int secondary_group_count,
                              const char *givenname, const char *sn, const char *mail,
                              const char *loginshell, const char *homedirectory,
                              const char *password, int disabled, const char *ssh_public_key)
{
	u->uidnumber = uidnumber;
	u->primarygroup = primarygroup;
	memset(u->secondary_groups, 0, sizeof(u->secondary_groups));
	if (secondary_group_count > 0)
		memcpy(u->secondary_groups, secondary_groups,
		       (size_t)secondary_group_count * sizeof(*secondary_groups));
	u->secondary_group_count = secondary_group_count;
	memset(u->givenname, 0, sizeof(u->givenname));
	if (givenname != NULL)
		strncpy(u->givenname, givenname, sizeof(u->givenname) - 1);
	memset(u->sn, 0, sizeof(u->sn));
	if (sn != NULL)
		strncpy(u->sn, sn, sizeof(u->sn) - 1);
	memset(u->mail, 0, sizeof(u->mail));
	if (mail != NULL)
		strncpy(u->mail, mail, sizeof(u->mail) - 1);
	memset(u->loginshell, 0, sizeof(u->loginshell));
	if (loginshell != NULL)
		strncpy(u->loginshell, loginshell, sizeof(u->loginshell) - 1);
	memset(u->homedirectory, 0, sizeof(u->homedirectory));
	if (homedirectory != NULL)
		strncpy(u->homedirectory, homedirectory, sizeof(u->homedirectory) - 1);
	if (password != NULL && password[0] != '\0')
		hash_password(password, u->passbcrypt);
	u->disabled = disabled ? 1 : 0;
	memset(u->ssh_public_key, 0, sizeof(u->ssh_public_key));
	if (ssh_public_key != NULL)
		strncpy(u->ssh_public_key, ssh_public_key, sizeof(u->ssh_public_key) - 1);
}

enum ldap_record_error ldap_user_create(const char *name, int uidnumber, int primarygroup,
                                         const int *secondary_groups, int secondary_group_count,
                                         const char *givenname, const char *sn, const char *mail,
                                         const char *loginshell, const char *homedirectory,
                                         const char *password, int disabled,
                                         const char *owner_container, int can_search,
                                         const char *ssh_public_key, struct ldap_user **out)
{
	int i, slot = -1;
	struct ldap_user *u;
	enum ldap_record_error verr;

	if (!ldap_username_is_valid(name))
		return LDAP_RECORD_ERR_INVALID_NAME;
	verr = validate_user_fields(uidnumber, primarygroup, secondary_groups, secondary_group_count);
	if (verr != LDAP_RECORD_OK)
		return verr;
	if (ldap_user_find(name) != NULL)
		return LDAP_RECORD_ERR_DUPLICATE;

	for (i = 0; i < LDAP_USER_MAX; i++) {
		if (g_users[i].name[0] == '\0') {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return LDAP_RECORD_ERR_FULL;

	u = &g_users[slot];
	memset(u, 0, sizeof(*u));
	strncpy(u->name, name, sizeof(u->name) - 1);
	fill_user_fields(u, uidnumber, primarygroup, secondary_groups, secondary_group_count, givenname,
	                  sn, mail, loginshell, homedirectory, password, disabled, ssh_public_key);
	if (owner_container != NULL)
		strncpy(u->owner_container, owner_container, sizeof(u->owner_container) - 1);
	u->can_search = can_search ? 1 : 0;

	if (save_users_state() != 0) {
		memset(u, 0, sizeof(*u));
		return LDAP_RECORD_ERR_PERSIST_FAILED;
	}

	ldap_record_sync_all();
	*out = u;
	return LDAP_RECORD_OK;
}

enum ldap_record_error ldap_user_update(const char *name, const char *new_name, int uidnumber,
                                         int primarygroup, const int *secondary_groups,
                                         int secondary_group_count, const char *givenname,
                                         const char *sn, const char *mail, const char *loginshell,
                                         const char *homedirectory, const char *password, int disabled,
                                         const char *ssh_public_key, int can_search,
                                         struct ldap_user **out)
{
	struct ldap_user *u = ldap_user_find(name);
	enum ldap_record_error verr;
	int renaming = new_name != NULL && new_name[0] != '\0' && strcmp(new_name, name) != 0;
	char old_name[LDAP_USER_NAME_MAX];

	if (u == NULL)
		return LDAP_RECORD_ERR_NOT_FOUND;
	verr = validate_user_fields(uidnumber, primarygroup, secondary_groups, secondary_group_count);
	if (verr != LDAP_RECORD_OK)
		return verr;
	if (renaming) {
		if (!ldap_username_is_valid(new_name))
			return LDAP_RECORD_ERR_INVALID_NAME;
		if (ldap_user_find(new_name) != NULL)
			return LDAP_RECORD_ERR_DUPLICATE;
	}

	/* password == NULL means "keep the existing hash" -- fill_user_
	 * fields() only overwrites passbcrypt when password is non-NULL
	 * and non-empty, so u->passbcrypt is left untouched otherwise. */
	fill_user_fields(u, uidnumber, primarygroup, secondary_groups, secondary_group_count, givenname,
	                  sn, mail, loginshell, homedirectory, password, disabled, ssh_public_key);
	/* ADR-0144 task #838: can_search, like every other PUT field, is
	 * full-field-replacement -- an operator managing a real bind/
	 * service account's search grant through this same endpoint
	 * expects PUT to set it exactly as given, not silently preserve
	 * whatever it was before. */
	u->can_search = can_search ? 1 : 0;
	if (renaming) {
		snprintf(old_name, sizeof(old_name), "%s", u->name);
		snprintf(u->name, sizeof(u->name), "%s", new_name);
	}

	if (save_users_state() != 0) {
		/* Unlike the other fields above (a pre-existing, unfixed gap
		 * this function has always had -- not this change's to fix),
		 * the name is a real lookup key: leaving it renamed in memory
		 * but not on disk would make this user briefly unfindable
		 * under either name after a restart, worth reverting on its
		 * own even though nothing else here rolls back. */
		if (renaming)
			snprintf(u->name, sizeof(u->name), "%s", old_name);
		return LDAP_RECORD_ERR_PERSIST_FAILED;
	}

	ldap_record_sync_all();
	*out = u;
	return LDAP_RECORD_OK;
}

enum ldap_record_error ldap_user_delete(const char *name)
{
	struct ldap_user *u = ldap_user_find(name);

	if (u == NULL)
		return LDAP_RECORD_ERR_NOT_FOUND;

	memset(u, 0, sizeof(*u));
	if (save_users_state() != 0)
		return LDAP_RECORD_ERR_PERSIST_FAILED;

	ldap_record_sync_all();
	return LDAP_RECORD_OK;
}

int ldap_user_is_in_group(const char *user_name, const char *group_name)
{
	struct ldap_user *u = ldap_user_find(user_name);
	struct ldap_group *g;
	int i;

	if (u == NULL || u->disabled)
		return 0;
	g = ldap_group_find(group_name);
	if (g == NULL)
		return 0;
	if (u->primarygroup == g->gidnumber)
		return 1;
	for (i = 0; i < u->secondary_group_count; i++) {
		if (u->secondary_groups[i] == g->gidnumber)
			return 1;
	}
	return 0;
}

int ldap_user_check_password(const char *user_name, const char *password)
{
	struct ldap_user *u = ldap_user_find(user_name);

	if (u == NULL || u->disabled || u->passbcrypt[0] == '\0' || password == NULL)
		return 0;
	return pwhash_bcrypt_check(password, u->passbcrypt);
}

int ldap_user_for_each(int (*fn)(const char *name, void *ctx), void *ctx)
{
	int i;

	for (i = 0; i < LDAP_USER_MAX; i++) {
		if (g_users[i].name[0] == '\0')
			continue;
		if (fn(g_users[i].name, ctx))
			return 1;
	}
	return 0;
}

void ldap_group_write_json_one(const struct ldap_group *g, struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, g->name);
	jw_key(w, "gidnumber");
	jw_int(w, g->gidnumber);
	jw_obj_close(w);
}

void ldap_group_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < LDAP_GROUP_MAX; i++) {
		if (g_groups[i].name[0] != '\0')
			ldap_group_write_json_one(&g_groups[i], w);
	}
	jw_arr_close(w);
}

void ldap_user_write_json_one(const struct ldap_user *u, struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, u->name);
	jw_key(w, "uidnumber");
	jw_int(w, u->uidnumber);
	jw_key(w, "primarygroup");
	jw_int(w, u->primarygroup);
	jw_key(w, "secondary_groups");
	jw_arr_open(w);
	{
		int i;

		for (i = 0; i < u->secondary_group_count; i++)
			jw_int(w, u->secondary_groups[i]);
	}
	jw_arr_close(w);
	jw_key(w, "givenname");
	jw_str(w, u->givenname);
	jw_key(w, "sn");
	jw_str(w, u->sn);
	jw_key(w, "mail");
	jw_str(w, u->mail);
	jw_key(w, "loginshell");
	jw_str(w, u->loginshell);
	jw_key(w, "homedirectory");
	jw_str(w, u->homedirectory);
	jw_key(w, "has_password");
	jw_bool(w, u->passbcrypt[0] != '\0');
	jw_key(w, "disabled");
	jw_bool(w, u->disabled != 0);
	jw_key(w, "owner");
	if (u->owner_container[0] != '\0')
		jw_str(w, u->owner_container);
	else
		jw_null(w);
	jw_key(w, "can_search");
	jw_bool(w, u->can_search != 0);
	jw_key(w, "ssh_public_key");
	jw_str(w, u->ssh_public_key);
	jw_obj_close(w);
}

void ldap_user_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < LDAP_USER_MAX; i++) {
		if (g_users[i].name[0] != '\0')
			ldap_user_write_json_one(&g_users[i], w);
	}
	jw_arr_close(w);
}

void ldap_user_forget_owner(const char *container_name)
{
	int i;

	for (i = 0; i < LDAP_USER_MAX; i++) {
		if (g_users[i].name[0] != '\0' &&
		    strcmp(g_users[i].owner_container, container_name) == 0) {
			memset(&g_users[i], 0, sizeof(g_users[i]));
			save_users_state();
			ldap_record_sync_all();
			return;
		}
	}
}

int ldap_uid_alloc(void)
{
	int uid = g_config.start_uid;
	int i;

	for (;;) {
		int in_use = 0;

		for (i = 0; i < LDAP_USER_MAX; i++) {
			if (g_users[i].name[0] != '\0' && g_users[i].uidnumber == uid) {
				in_use = 1;
				break;
			}
		}
		if (!in_use)
			return uid;
		uid++;
	}
}

int ldap_gid_alloc(void)
{
	int gid = g_config.start_gid;
	int i;

	for (;;) {
		int in_use = 0;

		for (i = 0; i < LDAP_GROUP_MAX; i++) {
			if (g_groups[i].name[0] != '\0' && g_groups[i].gidnumber == gid) {
				in_use = 1;
				break;
			}
		}
		if (!in_use)
			return gid;
		gid++;
	}
}

int ldap_generate_secret(char out[LDAP_PROVISION_SECRET_LEN + 1])
{
	unsigned char raw[LDAP_PROVISION_SECRET_LEN / 2];
	int fd;
	ssize_t n;
	size_t total = 0;
	size_t i;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -1;
	while (total < sizeof(raw)) {
		n = read(fd, raw + total, sizeof(raw) - total);
		if (n <= 0) {
			close(fd);
			return -1;
		}
		total += (size_t)n;
	}
	close(fd);

	for (i = 0; i < sizeof(raw); i++)
		snprintf(out + i * 2, 3, "%02x", raw[i]);
	return 0;
}

#define LDAP_SERVICE_BIND_USER  "svc-nslcd"
#define LDAP_SERVICE_BIND_GROUP "svc-accounts"

void ldap_ensure_service_bind_account(void)
{
	const char *base = hostauth_ldap_base_dn();
	struct ldap_user *u;
	struct ldap_group *g;
	char secret[LDAP_PROVISION_SECRET_LEN + 1];
	char dn[sizeof(g_config.bind_dn)];

	/* LDAP not configured yet (no canonical base DN) -- nothing to bind
	 * against, so nothing to provision. Self-heals on the next call once
	 * hostauth-config's ldap_base_dn is set. */
	if (base[0] == '\0')
		return;

	/*
	 * Respect a deliberately operator-chosen bind account: only ever
	 * manage the well-known svc-nslcd identity. If bind_dn is set and
	 * names something else, the operator wired a custom bind -- leave it
	 * completely alone.
	 */
	if (g_config.bind_dn[0] != '\0' &&
	    strncmp(g_config.bind_dn, "cn=" LDAP_SERVICE_BIND_USER ",",
	            strlen("cn=" LDAP_SERVICE_BIND_USER ",")) != 0)
		return;

	u = ldap_user_find(LDAP_SERVICE_BIND_USER);
	if (u != NULL && g_config.bind_password[0] != '\0') {
		/*
		 * Already provisioned and we hold a working password -- just keep
		 * bind_dn aligned with the account's real group + the canonical
		 * base (which could have moved), without disturbing the password.
		 */
		g = ldap_group_find_by_gid(u->primarygroup);
		if (g != NULL) {
			snprintf(dn, sizeof(dn), "cn=%s,ou=%s,%s", LDAP_SERVICE_BIND_USER, g->name, base);
			if (strcmp(dn, g_config.bind_dn) != 0) {
				snprintf(g_config.bind_dn, sizeof(g_config.bind_dn), "%s", dn);
				ldap_config_persist();
			}
		}
		return;
	}

	/*
	 * (Re)provision: either the account doesn't exist, or it does but we
	 * hold no usable password for it (empty bind_password -- e.g. a fresh
	 * state file, or an account created by hand). Ensure a group, then
	 * create-or-reset the account with a fresh random secret + can_search,
	 * and record its real DN + that secret as the client bind identity.
	 * ldap_user_create()/ldap_group_create() push to every running glauth
	 * server themselves (ldap_record_sync_all()); a server not up yet
	 * self-heals on its next register.
	 */
	g = ldap_group_find(LDAP_SERVICE_BIND_GROUP);
	if (g == NULL && ldap_group_create(LDAP_SERVICE_BIND_GROUP, ldap_gid_alloc(), &g) != LDAP_RECORD_OK)
		return;
	if (g == NULL || ldap_generate_secret(secret) != 0)
		return;

	if (u == NULL) {
		if (ldap_user_create(LDAP_SERVICE_BIND_USER, ldap_uid_alloc(), g->gidnumber, NULL, 0,
		                     "Service", "nslcd", NULL, "/usr/bin/nologin", "/nonexistent",
		                     secret, 0, NULL, 1, NULL, &u) != LDAP_RECORD_OK || u == NULL)
			return;
	} else {
		if (ldap_user_update(LDAP_SERVICE_BIND_USER, NULL, u->uidnumber, u->primarygroup,
		                     u->secondary_groups, u->secondary_group_count, u->givenname, u->sn,
		                     u->mail, u->loginshell, u->homedirectory, secret, u->disabled,
		                     u->ssh_public_key, 1, &u) != LDAP_RECORD_OK || u == NULL)
			return;
		g = ldap_group_find_by_gid(u->primarygroup);
		if (g == NULL)
			return;
	}

	snprintf(g_config.bind_dn, sizeof(g_config.bind_dn), "cn=%s,ou=%s,%s",
	         LDAP_SERVICE_BIND_USER, g->name, base);
	snprintf(g_config.bind_password, sizeof(g_config.bind_password), "%s", secret);
	ldap_config_persist();
}

/*
 * Issue #84: the same health filtering the derived list gets, applied to
 * an explicitly-configured client_uri. Everything #81 built was inert on
 * any box with client_uri set -- which included the real one -- because
 * the explicit list short-circuited before any filtering ran.
 *
 * Three rules, and the two conservative ones matter more than the
 * filtering itself: a URI that maps to no registered server is kept
 * untouched (it may be a directory Cix does not manage), and if
 * filtering would leave nothing, the original list stands. Handing a
 * client a server that might be down beats handing it none -- the
 * client retries; an empty list turns a partial outage into a total one.
 */
/*
 * Issue #84: which registered LDAP server, if any, a single URI from an
 * explicitly-configured client_uri list refers to.
 *
 * Health is keyed by container name; client_uri is free-form URIs, so
 * the two only meet by resolving each registered server's live IP and
 * comparing. Returns the matching registered container name, or NULL
 * for a URI that maps to nothing Cix manages -- which is a real,
 * legitimate case (an operator may point at a directory this platform
 * knows nothing about) and must be left strictly alone rather than
 * filtered on evidence that does not exist.
 *
 * A port is only allowed to match when it is the port health actually
 * probes. Same IP on a different port is a different service, and
 * dropping it on the strength of a probe that never touched it would be
 * a guess dressed up as a health decision.
 */
static const char *ldap_uri_registered_server(const char *uri, char names[][LDAP_SERVER_NAME_MAX],
                                               int count)
{
	const char *authority, *p;
	char host[128];
	size_t hlen;
	int port = HOSTAUTH_LDAP_DEFAULT_PORT;
	int i;

	authority = strstr(uri, "://");
	authority = authority != NULL ? authority + 3 : uri;
	if (*authority == '[') /* IPv6 literal -- nothing here is IPv6, so never ours */
		return NULL;
	for (p = authority; *p != '\0' && *p != '/'; p++)
		;
	hlen = (size_t)(p - authority);
	if (hlen == 0 || hlen >= sizeof(host))
		return NULL;
	memcpy(host, authority, hlen);
	host[hlen] = '\0';
	{
		char *colon = strrchr(host, ':');

		if (colon != NULL) {
			*colon = '\0';
			port = atoi(colon + 1);
		}
	}
	/* #414: with TLS on, an explicitly-configured list names the TLS
	 * port, and rejecting it here would silently drop every entry --
	 * health filtering would then pass nothing through and the list
	 * would look empty rather than wrong. #419: both accepted ports
	 * come from the server config now, so a URI naming the port the
	 * server really listens on is recognised whichever listener it is
	 * -- previously the plaintext arm was the hardcoded 3893. */
	if (port != g_config.server_plaintext_port && port != g_config.server_tls_port)
		return NULL;

	for (i = 0; i < count; i++) {
		struct registry_entry *se = registry_find(names[i]);
		struct in_addr a;
		char ipbuf[INET_ADDRSTRLEN];

		if (se == NULL || se->net_count == 0 || se->nets[0].ip_be == 0)
			continue;
		a.s_addr = se->nets[0].ip_be;
		if (inet_ntop(AF_INET, &a, ipbuf, sizeof(ipbuf)) == NULL)
			continue;
		if (strcmp(ipbuf, host) == 0)
			return names[i];
	}
	return NULL;
}

void ldap_filter_configured_client_uri(const char *configured, char *out, size_t out_size)
{
	char names[LDAP_SERVER_MAX][LDAP_SERVER_NAME_MAX];
	char work[sizeof(((struct ldap_config *)0)->client_uri)];
	char *tok, *save;
	size_t off = 0;
	int count;

	snprintf(out, out_size, "%s", configured);
	count = ldap_server_list_containers(names, LDAP_SERVER_MAX);
	if (count == 0)
		return;

	snprintf(work, sizeof(work), "%s", configured);
	for (tok = strtok_r(work, " \t", &save); tok != NULL; tok = strtok_r(NULL, " \t", &save)) {
		const char *server = ldap_uri_registered_server(tok, names, count);
		int written;

		if (server != NULL && !serverhealth_in_service("ldap", server))
			continue;
		written = snprintf(out + off, out_size - off, "%s%s", off > 0 ? " " : "", tok);
		if (written > 0 && (size_t)written < out_size - off)
			off += (size_t)written;
	}
	if (off == 0)
		snprintf(out, out_size, "%s", configured);
}

/*
 * The LDAP URI list handed to client containers. Built from the live IPs
 * of registered LDAP servers when no explicit client_uri is configured.
 *
 * Issue #81: servers that are drained or confirmed unhealthy are dropped
 * -- the direct fix for #80's shape, where a registered-but-not-serving
 * pair silently broke every login. Two deliberate safety rules keep that
 * filtering from ever becoming its own outage:
 *   - a never-yet-probed server counts as in service, so turning health
 *     tracking on can't black-hole a working deployment during the very
 *     first sweep;
 *   - if filtering would leave NOTHING, the unfiltered list is used
 *     instead. Handing a client a server that might be down is strictly
 *     better than handing it nothing at all -- the client retries, and
 *     an empty URI list would turn a partial outage into a total one.
 */
/*
 * The one port the LDAP service is reachable on right now. Extracted
 * (#416) because two places need the same answer and a second copy of
 * the conditional is a second source of truth: the client URI below,
 * and cixd's own server-health probe, which opens a TCP connection to
 * decide whether a server is in service.
 *
 * That probe used to be hardcoded to HOSTAUTH_LDAP_DEFAULT_PORT, which
 * was correct only while the plaintext listener was always up. When
 * ldap-1/ldap-2 1.6.0 turned it off, both servers immediately reported
 * unhealthy with "connect: Connection refused" against a port nothing
 * was serving any more -- measured on 192.168.15.95, 2026-09-12. The
 * unfiltered-list fallback below meant clients kept working, so the
 * only visible symptom was a health view that was simply wrong, which
 * is the kind of thing an operator acts on.
 *
 * #419 then made the answer honest rather than merely shared: it read
 * client_tls_port and HOSTAUTH_LDAP_DEFAULT_PORT, neither of which was
 * the port the server was actually listening on -- nothing checked, and
 * nothing could, while the listener config was literal TOML inside a
 * recipe. It reads the server configuration now, which the daemon owns.
 */
int ldap_client_port(void)
{
	const struct ldap_config *lc = ldap_config_get();

	return lc->client_tls ? lc->server_tls_port : lc->server_plaintext_port;
}

int ldap_effective_client_uri(char *out, size_t out_size)
{
	const struct ldap_config *lc = ldap_config_get();
	char names[LDAP_SERVER_MAX][LDAP_SERVER_NAME_MAX];
	int count, i, pass;
	size_t off = 0;

	if (lc->client_uri[0] != '\0') {
		/* Issue #84: filtered, not passed through -- an explicit list
		 * used to skip every health rule below it. */
		ldap_filter_configured_client_uri(lc->client_uri, out, out_size);
		return 1;
	}
	count = ldap_server_list_containers(names, LDAP_SERVER_MAX);

	/* pass 0: in-service servers only. pass 1 (only if that produced an
	 * empty list): every reachable server, health ignored. */
	for (pass = 0; pass < 2 && off == 0; pass++) {
		out[0] = '\0';
		for (i = 0; i < count; i++) {
			struct registry_entry *se = registry_find(names[i]);
			struct in_addr a;
			char ipbuf[INET_ADDRSTRLEN];
			int written;

			if (se == NULL || se->net_count == 0 || se->nets[0].ip_be == 0)
				continue;
			if (pass == 0 && !serverhealth_in_service("ldap", names[i]))
				continue;
			a.s_addr = se->nets[0].ip_be;
			if (inet_ntop(AF_INET, &a, ipbuf, sizeof(ipbuf)) == NULL)
				continue;
			/*
			 * Scheme and port together (#414) -- they are one
			 * decision, and splitting them produces a URI that
			 * names TLS on a plaintext port or the reverse. The
			 * port comes from ldap_client_port() so cixd's own
			 * health probe cannot disagree with what clients are
			 * told (#416).
			 *
			 * This is the client population configured FROM here:
			 * nslcd.conf and {{LDAP:URI}}. The daemon's OWN bind
			 * does not come through this function -- hostauth
			 * dials ldapclient.c, which has its own ldap_tls
			 * switch on PUT /system/hostauth-config (#416), for a
			 * client that may be pointed at a different port
			 * entirely. Both being on LDAPS is what allowed
			 * ldap-1/ldap-2 1.6.0 to turn the plaintext listener
			 * off at all.
			 */
			written = snprintf(out + off, out_size - off, "%s%s://%s:%d/",
			                    off > 0 ? " " : "",
			                    g_config.client_tls ? "ldaps" : "ldap", ipbuf,
			                    ldap_client_port());
			if (written > 0 && (size_t)written < out_size - off)
				off += (size_t)written;
		}
	}
	return off > 0;
}
