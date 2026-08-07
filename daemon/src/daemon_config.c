#include "daemon_config.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_HTTPS_PORT 8443

static char g_state_path[512];
static int g_port;          /* 0: no persisted override */
static int g_http_enabled;  /* defaults to 1 -- every install starts HTTP-only */
static int g_https_enabled; /* defaults to 0 */
static int g_https_port;    /* 0: no persisted override, main.c falls back to DEFAULT_HTTPS_PORT */
static char g_bind_ip[DAEMON_CONFIG_BIND_IP_MAX]; /* empty: no dedicated bind_ip set (ADR-0068) */

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *jport, *jhttp, *jhttps, *jhttps_port, *jbind_ip;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no persisted state yet -- first-ever startup */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		fprintf(stderr, "%s: malformed persisted daemon config\n", g_state_path);
		return -1;
	}

	jport = json_object_get(root, "port");
	if (jport != NULL) {
		int port = (int)json_as_number(jport);

		if (port < 1 || port > 65535) {
			json_free(root);
			fprintf(stderr, "%s: invalid persisted port\n", g_state_path);
			return -1;
		}
		g_port = port;
	}

	jhttp = json_object_get(root, "http_enabled");
	if (jhttp != NULL && jhttp->type == JSON_BOOL)
		g_http_enabled = jhttp->u.boolean;

	jhttps = json_object_get(root, "https_enabled");
	if (jhttps != NULL && jhttps->type == JSON_BOOL)
		g_https_enabled = jhttps->u.boolean;

	jhttps_port = json_object_get(root, "https_port");
	if (jhttps_port != NULL) {
		int port = (int)json_as_number(jhttps_port);

		if (port < 1 || port > 65535) {
			json_free(root);
			fprintf(stderr, "%s: invalid persisted https_port\n", g_state_path);
			return -1;
		}
		g_https_port = port;
	}

	jbind_ip = json_object_get(root, "bind_ip");
	if (jbind_ip != NULL && jbind_ip->type == JSON_STRING) {
		const char *ip = json_as_string(jbind_ip);

		if (ip != NULL && snprintf(g_bind_ip, sizeof(g_bind_ip), "%s", ip) >= (int)sizeof(g_bind_ip)) {
			json_free(root);
			fprintf(stderr, "%s: persisted bind_ip too long\n", g_state_path);
			return -1;
		}
	}

	json_free(root);
	return 0;
}

static int save_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "port");
	jw_int(&w, g_port);
	jw_key(&w, "http_enabled");
	jw_bool(&w, g_http_enabled);
	jw_key(&w, "https_enabled");
	jw_bool(&w, g_https_enabled);
	jw_key(&w, "https_port");
	jw_int(&w, g_https_port);
	jw_key(&w, "bind_ip");
	if (g_bind_ip[0] != '\0')
		jw_str(&w, g_bind_ip);
	else
		jw_null(&w);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

int daemon_config_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;
	g_port = 0;
	g_http_enabled = 1;
	g_https_enabled = 0;
	g_https_port = 0;
	g_bind_ip[0] = '\0';
	return load_state();
}

int daemon_config_port(void)
{
	return g_port;
}

int daemon_config_http_enabled(void)
{
	return g_http_enabled;
}

int daemon_config_https_enabled(void)
{
	return g_https_enabled;
}

int daemon_config_https_port(void)
{
	return g_https_port != 0 ? g_https_port : DEFAULT_HTTPS_PORT;
}

enum daemon_config_error daemon_config_set_port(int port)
{
	int prev_port;

	if (port < 1 || port > 65535)
		return DAEMON_CONFIG_ERR_INVALID_PORT;

	prev_port = g_port;
	g_port = port;
	if (save_state() != 0) {
		g_port = prev_port;
		return DAEMON_CONFIG_ERR_PERSIST_FAILED;
	}
	return DAEMON_CONFIG_OK;
}

enum daemon_config_error daemon_config_set_http_enabled(int enabled)
{
	int prev;

	if (!enabled && !g_https_enabled)
		return DAEMON_CONFIG_ERR_WOULD_HAVE_NO_LISTENER;

	prev = g_http_enabled;
	g_http_enabled = enabled ? 1 : 0;
	if (save_state() != 0) {
		g_http_enabled = prev;
		return DAEMON_CONFIG_ERR_PERSIST_FAILED;
	}
	return DAEMON_CONFIG_OK;
}

enum daemon_config_error daemon_config_set_https_enabled(int enabled)
{
	int prev;

	if (!enabled && !g_http_enabled)
		return DAEMON_CONFIG_ERR_WOULD_HAVE_NO_LISTENER;

	prev = g_https_enabled;
	g_https_enabled = enabled ? 1 : 0;
	if (save_state() != 0) {
		g_https_enabled = prev;
		return DAEMON_CONFIG_ERR_PERSIST_FAILED;
	}
	return DAEMON_CONFIG_OK;
}

enum daemon_config_error daemon_config_set_https_port(int port)
{
	int prev_port;

	if (port < 1 || port > 65535)
		return DAEMON_CONFIG_ERR_INVALID_PORT;

	prev_port = g_https_port;
	g_https_port = port;
	if (save_state() != 0) {
		g_https_port = prev_port;
		return DAEMON_CONFIG_ERR_PERSIST_FAILED;
	}
	return DAEMON_CONFIG_OK;
}

const char *daemon_config_bind_ip(void)
{
	return g_bind_ip[0] != '\0' ? g_bind_ip : NULL;
}

enum daemon_config_error daemon_config_set_bind_ip(const char *ip)
{
	char prev[DAEMON_CONFIG_BIND_IP_MAX];

	memcpy(prev, g_bind_ip, sizeof(prev));
	if (ip == NULL) {
		g_bind_ip[0] = '\0';
	} else if (snprintf(g_bind_ip, sizeof(g_bind_ip), "%s", ip) >= (int)sizeof(g_bind_ip)) {
		memcpy(g_bind_ip, prev, sizeof(g_bind_ip));
		return DAEMON_CONFIG_ERR_INVALID_BIND_IP;
	}
	if (save_state() != 0) {
		memcpy(g_bind_ip, prev, sizeof(g_bind_ip));
		return DAEMON_CONFIG_ERR_PERSIST_FAILED;
	}
	return DAEMON_CONFIG_OK;
}

void daemon_config_write_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "port");
	jw_int(w, g_port);
	jw_key(w, "http_enabled");
	jw_bool(w, g_http_enabled);
	jw_key(w, "https_enabled");
	jw_bool(w, g_https_enabled);
	jw_key(w, "https_port");
	jw_int(w, daemon_config_https_port());
	jw_key(w, "bind_ip");
	if (g_bind_ip[0] != '\0')
		jw_str(w, g_bind_ip);
	else
		jw_null(w);
	jw_obj_close(w);
}
