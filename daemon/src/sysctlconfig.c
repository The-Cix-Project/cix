#include "sysctlconfig.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sysctlconfig_entry {
	char key[SYSCTL_KEY_MAX];
	char value[SYSCTL_VALUE_MAX];
	int in_use;
};

static char g_state_path[512];
static struct sysctlconfig_entry g_entries[SYSCTL_CONFIG_MAX];

static struct sysctlconfig_entry *entry_find(const char *key)
{
	int i;

	for (i = 0; i < SYSCTL_CONFIG_MAX; i++) {
		if (g_entries[i].in_use && strcmp(g_entries[i].key, key) == 0)
			return &g_entries[i];
	}
	return NULL;
}

int sysctl_key_is_valid(const char *key)
{
	size_t len;

	if (key == NULL || key[0] == '\0')
		return 0;
	len = strlen(key);
	if (len >= SYSCTL_KEY_MAX)
		return 0;
	/* A dotted sysctl key never legitimately contains '/' -- the live
	 * handler's own dots-to-slashes translation would otherwise
	 * produce a nonsensical path. No further restriction: this
	 * project's own "fully open passthrough, host-auth write-gating
	 * is the access control" decision applies to the key namespace
	 * exactly as much as it does to values. */
	return strchr(key, '/') == NULL;
}

static int save_state(void)
{
	struct json_writer w;
	int rc, i;

	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < SYSCTL_CONFIG_MAX; i++) {
		if (!g_entries[i].in_use)
			continue;
		jw_obj_open(&w);
		jw_key(&w, "key");
		jw_str(&w, g_entries[i].key);
		jw_key(&w, "value");
		jw_str(&w, g_entries[i].value);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
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
		return 0;

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted sysctl config\n", g_state_path);
		return -1;
	}
	if (root->u.array.count > SYSCTL_CONFIG_MAX) {
		json_free(root);
		fprintf(stderr, "%s: more entries persisted than SYSCTL_CONFIG_MAX\n", g_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *key = json_as_string(json_object_get(item, "key"));
		const char *value = json_as_string(json_object_get(item, "value"));

		if (!sysctl_key_is_valid(key) || value == NULL || strlen(value) >= SYSCTL_VALUE_MAX) {
			json_free(root);
			fprintf(stderr, "%s: invalid entry at index %zu\n", g_state_path, i);
			return -1;
		}

		memset(&g_entries[count], 0, sizeof(g_entries[count]));
		snprintf(g_entries[count].key, sizeof(g_entries[count].key), "%s", key);
		snprintf(g_entries[count].value, sizeof(g_entries[count].value), "%s", value);
		g_entries[count].in_use = 1;
		count++;
	}
	json_free(root);
	return 0;
}

int sysctlconfig_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;
	memset(g_entries, 0, sizeof(g_entries));
	return load_state();
}

void sysctlconfig_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

enum sysctlconfig_error sysctlconfig_set(const char *key, const char *value)
{
	struct sysctlconfig_entry *e;
	int slot = -1;
	int i;

	if (!sysctl_key_is_valid(key) || value == NULL || strlen(value) >= SYSCTL_VALUE_MAX)
		return SYSCTLCONFIG_ERR_INVALID_KEY;

	e = entry_find(key);
	if (e == NULL) {
		for (i = 0; i < SYSCTL_CONFIG_MAX; i++) {
			if (!g_entries[i].in_use) {
				slot = i;
				break;
			}
		}
		if (slot < 0)
			return SYSCTLCONFIG_ERR_FULL;
		e = &g_entries[slot];
		memset(e, 0, sizeof(*e));
		snprintf(e->key, sizeof(e->key), "%s", key);
		e->in_use = 1;
	}
	snprintf(e->value, sizeof(e->value), "%s", value);

	if (save_state() != 0)
		return SYSCTLCONFIG_ERR_PERSIST_FAILED;
	return SYSCTLCONFIG_OK;
}

enum sysctlconfig_error sysctlconfig_delete(const char *key)
{
	struct sysctlconfig_entry *e = entry_find(key);

	if (e == NULL)
		return SYSCTLCONFIG_ERR_NOT_FOUND;
	memset(e, 0, sizeof(*e));
	if (save_state() != 0)
		return SYSCTLCONFIG_ERR_PERSIST_FAILED;
	return SYSCTLCONFIG_OK;
}

void sysctl_value_write_json(const char *value, struct json_writer *w)
{
	char buf[SYSCTL_VALUE_MAX];
	char *tok, *save = NULL;
	int count = 0;
	char *tokens[SYSCTL_VALUE_MAX / 2 + 1];

	snprintf(buf, sizeof(buf), "%s", value);
	tok = strtok_r(buf, " \t\n\r", &save);
	while (tok != NULL && count < (int)(sizeof(tokens) / sizeof(tokens[0]))) {
		tokens[count++] = tok;
		tok = strtok_r(NULL, " \t\n\r", &save);
	}

	if (count <= 1) {
		jw_str(w, value);
		return;
	}
	jw_arr_open(w);
	{
		int i;

		for (i = 0; i < count; i++)
			jw_str(w, tokens[i]);
	}
	jw_arr_close(w);
}

static void write_one(const struct sysctlconfig_entry *e, struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "key");
	jw_str(w, e->key);
	jw_key(w, "value");
	sysctl_value_write_json(e->value, w);
	jw_obj_close(w);
}

int sysctlconfig_write_json_one(const char *key, struct json_writer *w)
{
	struct sysctlconfig_entry *e = entry_find(key);

	if (e == NULL)
		return 0;
	write_one(e, w);
	return 1;
}

void sysctlconfig_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < SYSCTL_CONFIG_MAX; i++) {
		if (g_entries[i].in_use)
			write_one(&g_entries[i], w);
	}
	jw_arr_close(w);
}

void sysctlconfig_foreach(void (*fn)(const char *key, const char *value, void *ctx), void *ctx)
{
	int i;

	for (i = 0; i < SYSCTL_CONFIG_MAX; i++) {
		if (g_entries[i].in_use)
			fn(g_entries[i].key, g_entries[i].value, ctx);
	}
}
