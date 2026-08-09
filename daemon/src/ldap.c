#include "ldap.h"
#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct ldap_server_binding g_bindings[LDAP_SERVER_MAX];
static char g_state_path[PATH_MAX];

static int db_path_is_valid(const char *path)
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
		const char *db_path = json_as_string(json_object_get(item, "db_path"));

		if (container == NULL || container[0] == '\0' || !db_path_is_valid(db_path))
			continue; /* skip a corrupt entry rather than fail the whole load */

		memset(&g_bindings[count], 0, sizeof(g_bindings[count]));
		strncpy(g_bindings[count].container_name, container,
		        sizeof(g_bindings[count].container_name) - 1);
		strncpy(g_bindings[count].db_path, db_path, sizeof(g_bindings[count].db_path) - 1);
		count++;
	}
	json_free(root);
	return 0;
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

enum ldap_server_error ldap_server_register(const char *container_name, const char *db_path)
{
	int i, slot = -1;

	if (!db_path_is_valid(db_path))
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
	strncpy(g_bindings[slot].db_path, db_path, sizeof(g_bindings[slot].db_path) - 1);

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
	jw_key(w, "db_path");
	jw_str(w, binding->db_path);
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
