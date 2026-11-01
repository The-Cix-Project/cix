#ifndef MCP_H
#define MCP_H

/*
 * ADR-0368: cixd serves its API to MCP clients as tools.
 *
 * This is the switch, persisted and off by default: a new surface that
 * an LLM speaks to does not arrive enabled. The gateway itself
 * (POST /v1/mcp) follows in its own release.
 */

enum mcp_error {
	MCP_OK = 0,
	MCP_ERR_PERSIST_FAILED
};

/* Loads the persisted switch. A host that never saved one is off. */
void mcp_init(const char *path);

int mcp_enabled(void);

/* Persists the switch; in force at once. */
enum mcp_error mcp_set_enabled(int enabled);

struct json_writer;

/* GET /v1/system/mcp's body: {"enabled": bool}. */
void mcp_write_config_json(struct json_writer *w);

#endif
