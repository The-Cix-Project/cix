#include "ldap.h"
#include "persist.h"
#include "registry.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/sha.h>

static struct ldap_server_binding g_bindings[LDAP_SERVER_MAX];
static char g_state_path[PATH_MAX];

static struct ldap_user g_users[LDAP_USER_MAX];
static char g_users_state_path[PATH_MAX];
static struct ldap_group g_groups[LDAP_GROUP_MAX];
static char g_groups_state_path[PATH_MAX];

static struct ldap_config g_config = { LDAP_CONFIG_DEFAULT_START_UID, LDAP_CONFIG_DEFAULT_START_GID };
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

static void hash_password(const char *password, char out[LDAP_PASSSHA256_LEN + 1])
{
	unsigned char digest[SHA256_DIGEST_LENGTH];
	size_t i;

	SHA256((const unsigned char *)password, strlen(password), digest);
	for (i = 0; i < SHA256_DIGEST_LENGTH; i++)
		snprintf(out + i * 2, 3, "%02x", digest[i]);
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
	jw_key(w, "passsha256");
	jw_str(w, u->passsha256);
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
		const char *passsha256 = json_as_string(json_object_get(item, "passsha256"));
		const char *owner = json_as_string(json_object_get(item, "owner"));
		const char *ssh_public_key = json_as_string(json_object_get(item, "ssh_public_key"));
		struct ldap_user *u;

		if (!ldap_username_is_valid(name))
			continue; /* skip a corrupt entry rather than fail the whole load */

		u = &g_users[count];
		memset(u, 0, sizeof(*u));
		strncpy(u->name, name, sizeof(u->name) - 1);
		u->uidnumber = (int)json_as_number(json_object_get(item, "uidnumber"));
		u->primarygroup = (int)json_as_number(json_object_get(item, "primarygroup"));
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
		if (passsha256 != NULL)
			strncpy(u->passsha256, passsha256, sizeof(u->passsha256) - 1);
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
	json_free(root);
	return 0;
}

const struct ldap_config *ldap_config_get(void)
{
	return &g_config;
}

enum ldap_record_error ldap_config_set(int start_uid, int start_gid)
{
	struct json_writer w;
	int rc;

	if (start_uid <= 0 || start_gid <= 0)
		return LDAP_RECORD_ERR_INVALID_FIELD;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "start_uid");
	jw_int(&w, start_uid);
	jw_key(&w, "start_gid");
	jw_int(&w, start_gid);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_config_state_path, w.buf, w.len);
	jw_free(&w);
	if (rc != 0)
		return LDAP_RECORD_ERR_PERSIST_FAILED;

	g_config.start_uid = start_uid;
	g_config.start_gid = start_gid;
	return LDAP_RECORD_OK;
}

void ldap_config_write_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "start_uid");
	jw_int(w, g_config.start_uid);
	jw_key(w, "start_gid");
	jw_int(w, g_config.start_gid);
	jw_obj_close(w);
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
		if (u->passsha256[0] != '\0') {
			toml_append_raw(buf, bufsize, &off, "\npasssha256 = ");
			toml_append_string(buf, bufsize, &off, u->passsha256);
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

	rc = persist_atomic_write(full_path, final_buf, final_len);
	free(final_buf);
	return rc;
}

/* Write-through: pushes the full current user/group set to every
 * currently-registered, currently-running LDAP server (mirrors dns_
 * server_sync_all()'s own "every binding, full re-render" behavior --
 * normally there's just one, but nothing here assumes that). Best-
 * effort per binding: a server that's unreachable right now is simply
 * skipped and stays stale until it next registers (ldap_server_
 * register() calls this too, so a fresh/replacement instance always
 * starts current). */
void ldap_record_sync_all(void)
{
	int i;

	for (i = 0; i < LDAP_SERVER_MAX; i++) {
		struct registry_entry *entry;
		char full_path[LDAP_SERVER_PATH_MAX + 32];

		if (g_bindings[i].container_name[0] == '\0')
			continue;
		entry = registry_find(g_bindings[i].container_name);
		if (entry == NULL || !entry->running)
			continue;
		if (snprintf(full_path, sizeof(full_path), "/proc/%d/root%s", (int)entry->handle.pid,
		             g_bindings[i].config_path) >= (int)sizeof(full_path))
			continue;
		ldap_write_config_file(full_path);
	}

	ldap_ssh_sync_all();
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

enum ldap_record_error ldap_group_update(const char *name, int gidnumber, struct ldap_group **out)
{
	struct ldap_group *g = ldap_group_find(name);
	struct ldap_group *collision;
	int old_gidnumber;

	if (g == NULL)
		return LDAP_RECORD_ERR_NOT_FOUND;
	if (gidnumber <= 0)
		return LDAP_RECORD_ERR_INVALID_FIELD;

	collision = ldap_group_find_by_gid(gidnumber);
	if (collision != NULL && collision != g)
		return LDAP_RECORD_ERR_DUPLICATE;

	old_gidnumber = g->gidnumber;
	g->gidnumber = gidnumber;

	if (save_groups_state() != 0) {
		g->gidnumber = old_gidnumber;
		return LDAP_RECORD_ERR_PERSIST_FAILED;
	}

	ldap_record_sync_all();
	*out = g;
	return LDAP_RECORD_OK;
}

static enum ldap_record_error validate_user_fields(int uidnumber, int primarygroup)
{
	if (uidnumber <= 0)
		return LDAP_RECORD_ERR_INVALID_FIELD;
	if (ldap_group_find_by_gid(primarygroup) == NULL)
		return LDAP_RECORD_ERR_GROUP_NOT_FOUND;
	return LDAP_RECORD_OK;
}

static void fill_user_fields(struct ldap_user *u, int uidnumber, int primarygroup,
                              const char *givenname, const char *sn, const char *mail,
                              const char *loginshell, const char *homedirectory,
                              const char *password, int disabled, const char *ssh_public_key)
{
	u->uidnumber = uidnumber;
	u->primarygroup = primarygroup;
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
		hash_password(password, u->passsha256);
	u->disabled = disabled ? 1 : 0;
	memset(u->ssh_public_key, 0, sizeof(u->ssh_public_key));
	if (ssh_public_key != NULL)
		strncpy(u->ssh_public_key, ssh_public_key, sizeof(u->ssh_public_key) - 1);
}

enum ldap_record_error ldap_user_create(const char *name, int uidnumber, int primarygroup,
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
	verr = validate_user_fields(uidnumber, primarygroup);
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
	fill_user_fields(u, uidnumber, primarygroup, givenname, sn, mail, loginshell, homedirectory,
	                  password, disabled, ssh_public_key);
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

enum ldap_record_error ldap_user_update(const char *name, int uidnumber, int primarygroup,
                                         const char *givenname, const char *sn, const char *mail,
                                         const char *loginshell, const char *homedirectory,
                                         const char *password, int disabled,
                                         const char *ssh_public_key, struct ldap_user **out)
{
	struct ldap_user *u = ldap_user_find(name);
	enum ldap_record_error verr;

	if (u == NULL)
		return LDAP_RECORD_ERR_NOT_FOUND;
	verr = validate_user_fields(uidnumber, primarygroup);
	if (verr != LDAP_RECORD_OK)
		return verr;

	/* password == NULL means "keep the existing hash" -- fill_user_
	 * fields() only overwrites passsha256 when password is non-NULL
	 * and non-empty, so u->passsha256 is left untouched otherwise. */
	fill_user_fields(u, uidnumber, primarygroup, givenname, sn, mail, loginshell, homedirectory,
	                  password, disabled, ssh_public_key);

	if (save_users_state() != 0)
		return LDAP_RECORD_ERR_PERSIST_FAILED;

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
	jw_bool(w, u->passsha256[0] != '\0');
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

/* ---- SSH-backed login sync for LDAP users (task #731) ---- */

static struct ldap_ssh_target g_ssh_targets[LDAP_SSH_TARGET_MAX];
static char g_ssh_state_path[PATH_MAX];

static int save_ssh_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	ldap_ssh_target_write_json_list(&w);
	rc = persist_atomic_write(g_ssh_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int load_ssh_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int count = 0;

	if (persist_read_file(g_ssh_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no persisted state yet -- first-ever startup */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted SSH target state\n", g_ssh_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count && count < LDAP_SSH_TARGET_MAX; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *container = json_as_string(json_object_get(item, "container"));

		if (container == NULL || container[0] == '\0')
			continue; /* skip a corrupt entry rather than fail the whole load */

		memset(&g_ssh_targets[count], 0, sizeof(g_ssh_targets[count]));
		strncpy(g_ssh_targets[count].container_name, container,
		        sizeof(g_ssh_targets[count].container_name) - 1);
		count++;
	}
	json_free(root);
	return 0;
}

void ldap_ssh_repoint(const char *new_state_path)
{
	snprintf(g_ssh_state_path, sizeof(g_ssh_state_path), "%s", new_state_path);
}

int ldap_ssh_init(const char *state_path)
{
	if (snprintf(g_ssh_state_path, sizeof(g_ssh_state_path), "%s", state_path) >=
	    (int)sizeof(g_ssh_state_path))
		return -1;

	memset(g_ssh_targets, 0, sizeof(g_ssh_targets));
	return load_ssh_state();
}

static struct ldap_ssh_target *ssh_target_find(const char *container_name)
{
	int i;

	for (i = 0; i < LDAP_SSH_TARGET_MAX; i++) {
		if (g_ssh_targets[i].container_name[0] != '\0' &&
		    strcmp(g_ssh_targets[i].container_name, container_name) == 0)
			return &g_ssh_targets[i];
	}
	return NULL;
}

enum ldap_ssh_error ldap_ssh_target_register(const char *container_name)
{
	int i, slot = -1;
	struct registry_entry *entry;

	entry = registry_find(container_name);
	if (entry == NULL)
		return LDAP_SSH_ERR_CONTAINER_NOT_FOUND;
	if (!entry->running)
		return LDAP_SSH_ERR_CONTAINER_NOT_RUNNING;
	if (ssh_target_find(container_name) != NULL)
		return LDAP_SSH_ERR_DUPLICATE;

	for (i = 0; i < LDAP_SSH_TARGET_MAX; i++) {
		if (g_ssh_targets[i].container_name[0] == '\0') {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return LDAP_SSH_ERR_FULL;

	memset(&g_ssh_targets[slot], 0, sizeof(g_ssh_targets[slot]));
	strncpy(g_ssh_targets[slot].container_name, container_name,
	        sizeof(g_ssh_targets[slot].container_name) - 1);

	if (save_ssh_state() != 0) {
		memset(&g_ssh_targets[slot], 0, sizeof(g_ssh_targets[slot]));
		return LDAP_SSH_ERR_PERSIST_FAILED;
	}

	ldap_ssh_sync_all();
	return LDAP_SSH_OK;
}

enum ldap_ssh_error ldap_ssh_target_unregister(const char *container_name)
{
	struct ldap_ssh_target *t = ssh_target_find(container_name);

	if (t == NULL)
		return LDAP_SSH_ERR_NOT_FOUND;

	memset(t, 0, sizeof(*t));
	if (save_ssh_state() != 0)
		return LDAP_SSH_ERR_PERSIST_FAILED;
	return LDAP_SSH_OK;
}

void ldap_ssh_target_forget(const char *container_name)
{
	struct ldap_ssh_target *t = ssh_target_find(container_name);

	if (t == NULL)
		return;
	memset(t, 0, sizeof(*t));
	save_ssh_state();
}

void ldap_ssh_target_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < LDAP_SSH_TARGET_MAX; i++) {
		if (g_ssh_targets[i].container_name[0] != '\0') {
			jw_obj_open(w);
			jw_key(w, "container");
			jw_str(w, g_ssh_targets[i].container_name);
			jw_obj_close(w);
		}
	}
	jw_arr_close(w);
}

/*
 * Writes rendered below a fixed marker line in full_path, preserving
 * everything above it byte-for-byte -- same "find the managed
 * section, truncate there, append fresh" discipline ldap_write_
 * config_file() already established for glauth's own config file,
 * simplified for a plain marker line instead of a TOML-syntax
 * boundary (this isn't TOML, so there's no equivalent unambiguous
 * structural marker to reuse). Never touches root/sshd/any other
 * pre-provisioned account sitting above the marker in a real
 * container's /etc/passwd,/etc/group,/etc/shadow.
 */
#define SSH_ACCOUNTS_MARKER \
	"\n# --- Kanxeo-managed accounts below (task #731) -- do not edit by hand ---\n"

static int write_managed_tail(const char *full_path, const char *rendered, size_t rendered_len)
{
	char *current = NULL;
	size_t current_len = 0;
	size_t prefix_len;
	const char *marker;
	char *final_buf;
	size_t final_len;
	int rc;

	if (persist_read_file(full_path, &current, &current_len) != 0) {
		current = NULL;
		current_len = 0;
	}

	prefix_len = current_len;
	if (current != NULL) {
		marker = memmem(current, current_len, SSH_ACCOUNTS_MARKER, strlen(SSH_ACCOUNTS_MARKER));
		if (marker != NULL)
			prefix_len = (size_t)(marker - current);
	}

	final_len = prefix_len + strlen(SSH_ACCOUNTS_MARKER) + rendered_len;
	final_buf = malloc(final_len > 0 ? final_len : 1);
	if (final_buf == NULL) {
		free(current);
		return -1;
	}
	if (prefix_len > 0)
		memcpy(final_buf, current, prefix_len);
	memcpy(final_buf + prefix_len, SSH_ACCOUNTS_MARKER, strlen(SSH_ACCOUNTS_MARKER));
	memcpy(final_buf + prefix_len + strlen(SSH_ACCOUNTS_MARKER), rendered, rendered_len);
	free(current);

	{
		char parent[PATH_MAX];
		char *slash;

		if (snprintf(parent, sizeof(parent), "%s", full_path) < (int)sizeof(parent)) {
			slash = strrchr(parent, '/');
			if (slash != NULL && slash != parent) {
				*slash = '\0';
				persist_mkdir_p(parent);
			}
		}
	}

	rc = persist_atomic_write(full_path, final_buf, final_len);
	free(final_buf);
	return rc;
}

/*
 * Renders every current LDAP user with a non-empty ssh_public_key
 * (and not disabled) into pid's own /etc/passwd,/etc/group,
 * /etc/shadow (managed-tail sections) plus a dedicated ~/.ssh/
 * authorized_keys per user. A locked (`*`) shadow password, never
 * `!` -- see ldap.h's own header comment on why that distinction
 * matters to openssh specifically. One /etc/group line per distinct
 * gid actually referenced (multiple users can share a primarygroup),
 * not one per user.
 */
static void write_accounts_for_pid(pid_t pid)
{
	static char passwd_rendered[65536];
	static char shadow_rendered[65536];
	static char group_rendered[8192];
	size_t passwd_off = 0, shadow_off = 0, group_off = 0;
	int gid_written[LDAP_GROUP_MAX];
	int i;
	char path[PATH_MAX];

	memset(gid_written, 0, sizeof(gid_written));

	for (i = 0; i < LDAP_USER_MAX; i++) {
		const struct ldap_user *u = &g_users[i];
		int j, gi = -1;
		int n;

		if (u->name[0] == '\0' || u->ssh_public_key[0] == '\0' || u->disabled)
			continue;

		n = snprintf(passwd_rendered + passwd_off, sizeof(passwd_rendered) - passwd_off,
		             "%s:x:%d:%d:%s:%s:%s\n", u->name, u->uidnumber, u->primarygroup,
		             u->givenname[0] != '\0' ? u->givenname : u->name,
		             u->homedirectory[0] != '\0' ? u->homedirectory : "/",
		             u->loginshell[0] != '\0' ? u->loginshell : "/usr/bin/bash");
		if (n > 0 && (size_t)n < sizeof(passwd_rendered) - passwd_off)
			passwd_off += (size_t)n;

		n = snprintf(shadow_rendered + shadow_off, sizeof(shadow_rendered) - shadow_off,
		             "%s:*:1:0:99999:7:::\n", u->name);
		if (n > 0 && (size_t)n < sizeof(shadow_rendered) - shadow_off)
			shadow_off += (size_t)n;

		for (j = 0; j < LDAP_GROUP_MAX; j++) {
			if (g_groups[j].name[0] != '\0' && g_groups[j].gidnumber == u->primarygroup) {
				gi = j;
				break;
			}
		}
		if (gi >= 0 && !gid_written[gi]) {
			gid_written[gi] = 1;
			n = snprintf(group_rendered + group_off, sizeof(group_rendered) - group_off,
			             "%s:x:%d:\n", g_groups[gi].name, g_groups[gi].gidnumber);
			if (n > 0 && (size_t)n < sizeof(group_rendered) - group_off)
				group_off += (size_t)n;
		}
	}

	if (snprintf(path, sizeof(path), "/proc/%d/root/etc/passwd", (int)pid) < (int)sizeof(path))
		write_managed_tail(path, passwd_rendered, passwd_off);
	if (snprintf(path, sizeof(path), "/proc/%d/root/etc/group", (int)pid) < (int)sizeof(path))
		write_managed_tail(path, group_rendered, group_off);
	if (snprintf(path, sizeof(path), "/proc/%d/root/etc/shadow", (int)pid) < (int)sizeof(path)) {
		if (write_managed_tail(path, shadow_rendered, shadow_off) == 0)
			chmod(path, 0640);
	}

	/* authorized_keys: one dedicated file per user, not a managed-tail
	 * section -- a whole file Kanxeo owns entirely, no risk of
	 * clobbering anything else the way /etc/passwd's other entries
	 * (root, sshd) need protecting from. */
	for (i = 0; i < LDAP_USER_MAX; i++) {
		const struct ldap_user *u = &g_users[i];
		char ssh_dir[PATH_MAX];
		char key_path[PATH_MAX];
		char key_line[LDAP_SSH_KEY_MAX + 2];
		int n;

		if (u->name[0] == '\0' || u->ssh_public_key[0] == '\0' || u->disabled)
			continue;

		if (snprintf(ssh_dir, sizeof(ssh_dir), "/proc/%d/root%s/.ssh", (int)pid,
		             u->homedirectory[0] != '\0' ? u->homedirectory : "/") >= (int)sizeof(ssh_dir))
			continue;
		if (persist_mkdir_p(ssh_dir) != 0)
			continue;

		n = snprintf(key_line, sizeof(key_line), "%s\n", u->ssh_public_key);
		if (n < 0 || (size_t)n >= sizeof(key_line))
			continue;
		if (snprintf(key_path, sizeof(key_path), "%s/authorized_keys", ssh_dir) >=
		    (int)sizeof(key_path))
			continue;
		if (persist_atomic_write(key_path, key_line, (size_t)n) == 0) {
			chmod(ssh_dir, 0700);
			chmod(key_path, 0600);
			chown(ssh_dir, (uid_t)u->uidnumber, (gid_t)u->primarygroup);
			chown(key_path, (uid_t)u->uidnumber, (gid_t)u->primarygroup);
		}
	}
}

void ldap_ssh_sync_all(void)
{
	int i;

	for (i = 0; i < LDAP_SSH_TARGET_MAX; i++) {
		struct registry_entry *entry;

		if (g_ssh_targets[i].container_name[0] == '\0')
			continue;
		entry = registry_find(g_ssh_targets[i].container_name);
		if (entry == NULL || !entry->running)
			continue;
		write_accounts_for_pid(entry->handle.pid);
	}
}
