#include "daemon_config.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_HTTPS_PORT 443

static char g_state_path[512];
static int g_port;          /* 0: no persisted override */
static int g_http_enabled;  /* defaults to 1 */
static int g_https_enabled;
/*
 * ADR-0207 phase 3: whether a container created WITHOUT an explicit
 * "userns" field gets a user namespace. Defaults to 1 -- secure by
 * default, the owner's stated posture: every workload runs as a mapped
 * unprivileged uid unless it (or the platform, for its own trusted
 * build containers) opts out. An operator can flip the platform-wide
 * default off here; the per-container "userns" field always wins over
 * either default. The test fixture seeds this to 0 because the dev
 * sandbox's own LSM forbids uid_map writes entirely (documented in
 * CLAUDE.md) -- through this same ordinary config channel, not a
 * test-only code path.
 */
static int g_userns_default = 1; /* defaults to 1 too (ADR-0171) -- every fresh install
                              * starts both listeners on, matching this project's
                              * own already-correct port defaults (80/443, see
                              * DEFAULT_PORT/DEFAULT_HTTPS_PORT). A pre-PKI-
                              * bootstrap install just has HTTPS silently
                              * unavailable until a host cert exists (see the
                              * boot-time soft-fail in main.c) -- not a hazard,
                              * the same non-fatal "wanted but not yet available"
                              * posture ADR-0163/ADR-0169 already established
                              * elsewhere in this codebase. */
static int g_https_port;    /* 0: no persisted override, main.c falls back to DEFAULT_HTTPS_PORT */
static char g_management_address[DAEMON_CONFIG_MANAGEMENT_ADDRESS_MAX]; /* empty: loopback-only (ADR-0287) */

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *jport, *jhttp, *jhttps, *jhttps_port, *jmgmt;

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
	{
		const struct json_value *jud = json_object_get(root, "userns_default");

		if (jud != NULL && jud->type == JSON_BOOL)
			g_userns_default = jud->u.boolean;
	}

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

	/*
	 * The single truth (ADR-0287). On a pre-ADR-0287 box the key is
	 * absent; migrate the one legacy input this module can see -- an
	 * explicit bind_ip -- into it. The other legacy case (bind_ip was
	 * null and the address came from whichever network was flagged
	 * management) is migrated in main.c's boot path, which alone can
	 * resolve that network's address; it calls
	 * daemon_config_set_management_address() there. The legacy bind_ip
	 * key is never written again once this box saves state.
	 */
	jmgmt = json_object_get(root, "management_address");
	if (jmgmt == NULL || jmgmt->type != JSON_STRING)
		jmgmt = json_object_get(root, "bind_ip"); /* legacy, migrate once */
	if (jmgmt != NULL && jmgmt->type == JSON_STRING) {
		const char *ip = json_as_string(jmgmt);

		if (ip != NULL && ip[0] != '\0' &&
		    snprintf(g_management_address, sizeof(g_management_address), "%s", ip) >=
		            (int)sizeof(g_management_address)) {
			json_free(root);
			fprintf(stderr, "%s: persisted management_address too long\n", g_state_path);
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
	jw_key(&w, "userns_default");
	jw_bool(&w, g_userns_default);
	jw_key(&w, "https_port");
	jw_int(&w, g_https_port);
	jw_key(&w, "management_address");
	if (g_management_address[0] != '\0')
		jw_str(&w, g_management_address);
	else
		jw_null(&w);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

void daemon_config_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

int daemon_config_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;
	g_port = 0;
	g_http_enabled = 1;
	g_https_enabled = 1;
	g_https_port = 0;
	g_management_address[0] = '\0';
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

const char *daemon_config_management_address(void)
{
	return g_management_address[0] != '\0' ? g_management_address : NULL;
}

enum daemon_config_error daemon_config_set_management_address(const char *address)
{
	char prev[DAEMON_CONFIG_MANAGEMENT_ADDRESS_MAX];

	memcpy(prev, g_management_address, sizeof(prev));
	if (address == NULL) {
		g_management_address[0] = '\0';
	} else if (snprintf(g_management_address, sizeof(g_management_address), "%s", address) >=
	           (int)sizeof(g_management_address)) {
		memcpy(g_management_address, prev, sizeof(g_management_address));
		return DAEMON_CONFIG_ERR_INVALID_MANAGEMENT_ADDRESS;
	}
	if (save_state() != 0) {
		memcpy(g_management_address, prev, sizeof(g_management_address));
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
	jw_obj_close(w);
}

int daemon_config_userns_default(void)
{
	return g_userns_default;
}

enum daemon_config_error daemon_config_set_userns_default(int enabled)
{
	g_userns_default = enabled ? 1 : 0;
	return save_state() == 0 ? DAEMON_CONFIG_OK : DAEMON_CONFIG_ERR_PERSIST_FAILED;
}
