#include "mcp.h"
#include "json.h"
#include "logstore.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>

static int g_enabled;
static char g_state_path[512];

int mcp_enabled(void)
{
	return g_enabled;
}

static int save_state(void)
{
	char buf[64];
	int n;

	if (g_state_path[0] == '\0')
		return -1;
	n = snprintf(buf, sizeof(buf), "{\"enabled\":%s}\n", g_enabled ? "true" : "false");
	if (n < 0 || (size_t)n >= sizeof(buf))
		return -1;
	return persist_atomic_write(g_state_path, buf, (size_t)n);
}

void mcp_init(const char *path)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;

	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
	g_enabled = 0;
	if (persist_read_file(path, &buf, &len) == 0 && buf != NULL) {
		root = json_parse(buf, len);
		if (root != NULL) {
			const struct json_value *v = json_object_get(root, "enabled");

			/* A file gone bad leaves the switch off rather than
			 * refusing to boot, and says so. */
			if (v != NULL && v->type == JSON_BOOL)
				g_enabled = v->u.boolean;
			else
				logstore_write("cixd", "warn",
				               "mcp: ignoring an invalid setting in %s -- off (ADR-0368)",
				               path);
			json_free(root);
		}
	}
	free(buf);
}

enum mcp_error mcp_set_enabled(int enabled)
{
	int prev = g_enabled;

	g_enabled = enabled != 0;
	if (save_state() != 0) {
		g_enabled = prev;
		return MCP_ERR_PERSIST_FAILED;
	}
	return MCP_OK;
}

void mcp_write_config_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "enabled");
	jw_bool(w, g_enabled);
	jw_obj_close(w);
}
