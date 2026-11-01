#ifndef MCP_H
#define MCP_H

/*
 * ADR-0368: cixd serves its API to MCP clients as tools.
 *
 * The switch is persisted and off by default: a new surface that an
 * LLM speaks to does not arrive enabled. While it is off, POST /v1/mcp
 * answers 503.
 */

#include <stddef.h>

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

/*
 * The gateway (POST /v1/mcp). The protocol lives here; the daemon is
 * reached only through the caller's two callbacks, so the same handler
 * and the same authorization serve an MCP call as serve REST.
 */
struct mcp_caller {
	void *ctx;
	const char *user;           /* for the audit line; "" when none */
	const char *server_version; /* initialize's serverInfo.version */
	/* 1 when the caller holds `permission`. */
	int (*may)(void *ctx, const char *permission);
	/* Runs `method` `path` (query included) in-process. 0 with
	 * *status and a malloc'd *body (NULL when none) when a route
	 * answered; -1 when none matches. */
	int (*run)(void *ctx, const char *method, const char *path, int *status, char **body,
	           size_t *body_len);
};

/*
 * Answers one JSON-RPC message. Returns 200 with the response in `w`
 * (jw_init()ed by the caller), or 202 for a notification, when `w` is
 * left empty and nothing is to be sent.
 */
int mcp_handle(const char *request, size_t len, const struct mcp_caller *c,
               struct json_writer *w);

/* The number of tools the contract marks `x-cix-mcp: tool`. */
int mcp_tool_count(void);

#endif
