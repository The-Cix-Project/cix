/*
 * ADR-0368: cixd serves its API to MCP clients as tools.
 *
 * mcp.c owns the protocol and reaches the daemon only through two
 * callbacks -- may the caller hold this permission, and run this
 * request -- so the protocol is tested here against stubs of both, over
 * the real tool table apigen generates from docs/api/openapi.yaml. What
 * a tool call does once it reaches a handler is the handler's own
 * behaviour, verified on a real host. The audit line is captured by a
 * logstore_write() defined here in place of the store's.
 *
 * In SELFTESTS: it needs no daemon.
 */
#include "mcp.h"
#include "json.h"
#include "logstore.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "  FAIL: %s\n", what);
		g_failures++;
	}
}

/* ---------- stubs ---------- */

static char g_log[1024];
static char g_log_level[16];

void logstore_write(const char *source, const char *level, const char *fmt, ...)
{
	va_list ap;

	(void)source;
	snprintf(g_log_level, sizeof(g_log_level), "%s", level);
	va_start(ap, fmt);
	vsnprintf(g_log, sizeof(g_log), fmt, ap);
	va_end(ap);
}

/* The caller holds every permission but the one named here. */
static const char *g_denied = "";
static char g_ran_method[16];
static char g_ran_path[2048];
static int g_runs;
static int g_run_status = 200;
static const char *g_run_body = "{\"name\":\"dns-1\"}";

static int stub_may(void *ctx, const char *permission)
{
	(void)ctx;
	return strcmp(permission, g_denied) != 0;
}

static int stub_run(void *ctx, const char *method, const char *path, int *status, char **body,
                    size_t *body_len)
{
	(void)ctx;
	g_runs++;
	snprintf(g_ran_method, sizeof(g_ran_method), "%s", method);
	snprintf(g_ran_path, sizeof(g_ran_path), "%s", path);
	*status = g_run_status;
	*body = g_run_body != NULL ? strdup(g_run_body) : NULL;
	*body_len = g_run_body != NULL ? strlen(g_run_body) : 0;
	return 0;
}

static const struct mcp_caller g_caller = { NULL, "agent", "0.2.57-test", stub_may, stub_run };

/* Answers `request`; returns the status and leaves the response text in
 * `out` (empty for a 202). */
static int ask(const char *request, char *out, size_t out_size)
{
	struct json_writer w;
	int status;

	jw_init(&w);
	status = mcp_handle(request, strlen(request), &g_caller, &w);
	snprintf(out, out_size, "%.*s", (int)w.len, w.buf != NULL ? w.buf : "");
	jw_free(&w);
	return status;
}

static int has(const char *text, const char *needle)
{
	return strstr(text, needle) != NULL;
}

/* ---------- the protocol ---------- */

static void test_initialize(void)
{
	char out[4096];

	check(ask("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
	          "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
	          "\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}",
	          out, sizeof(out)) == 200,
	      "initialize is answered 200");
	check(has(out, "\"protocolVersion\":\"2025-06-18\""), "initialize answers 2025-06-18");
	check(has(out, "\"tools\":{\"listChanged\":false}"), "initialize offers tools");
	check(has(out, "\"version\":\"0.2.57-test\""), "serverInfo carries the build version");
	check(has(out, "\"id\":1"), "the response echoes a numeric id");

	ask("{\"jsonrpc\":\"2.0\",\"id\":\"a\",\"method\":\"initialize\",\"params\":"
	    "{\"protocolVersion\":\"2025-03-26\"}}",
	    out, sizeof(out));
	check(has(out, "\"protocolVersion\":\"2025-03-26\""), "an older revision is answered in kind");
	check(has(out, "\"id\":\"a\""), "the response echoes a string id");

	ask("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\",\"params\":"
	    "{\"protocolVersion\":\"1999-01-01\"}}",
	    out, sizeof(out));
	check(has(out, "\"protocolVersion\":\"2025-06-18\""),
	      "an unknown revision is answered with ours");
}

static void test_messages(void)
{
	char out[4096];

	check(ask("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", out,
	          sizeof(out)) == 202 &&
	          out[0] == '\0',
	      "a notification is a 202 with no body");
	check(ask("{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{}}", out, sizeof(out)) == 202,
	      "a client's response is a 202");
	ask("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}", out, sizeof(out));
	check(has(out, "\"result\":{}"), "ping answers an empty result");
	ask("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"resources/list\"}", out, sizeof(out));
	check(has(out, "\"code\":-32601"), "an unimplemented method is -32601");
	ask("{not json", out, sizeof(out));
	check(has(out, "\"code\":-32700") && has(out, "\"id\":null"), "bad JSON is -32700, id null");
	ask("[1,2]", out, sizeof(out));
	check(has(out, "\"code\":-32600"), "a batch is not a request (-32600)");
	ask("{\"jsonrpc\":\"1.0\",\"id\":5,\"method\":\"ping\"}", out, sizeof(out));
	check(has(out, "\"code\":-32600"), "a jsonrpc other than 2.0 is -32600");
}

static void test_tools_list(void)
{
	char out[65536];

	check(mcp_tool_count() > 0, "the contract marks at least one tool");
	g_denied = "";
	ask("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", out, sizeof(out));
	check(has(out, "\"name\":\"getContainer\""), "tools/list offers getContainer");
	check(has(out, "\"required\":[\"name\"]"), "a path parameter is required");
	check(has(out, "\"include_internal\":{\"type\":\"string\"}"),
	      "a query parameter is a property");
	check(has(out, "\"additionalProperties\":false"), "no other argument is accepted");
	check(has(out, "\"readOnlyHint\":true"), "a GET is read-only");
	check(!has(out, "\"name\":\"callMcp\""), "the gateway is not a tool of itself");

	g_denied = "containers:read";
	ask("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", out, sizeof(out));
	check(!has(out, "\"name\":\"getContainer\"") && !has(out, "\"name\":\"listContainers\""),
	      "tools/list leaves out what the caller may not call");
	check(has(out, "\"name\":\"getHealth\""), "and still offers what it may");
	g_denied = "";
}

static void test_tools_call(void)
{
	char out[8192];

	g_runs = 0;
	g_log[0] = '\0';
	ask("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":"
	    "\"getContainer\",\"arguments\":{\"name\":\"dns-1\"}}}",
	    out, sizeof(out));
	check(g_runs == 1 && strcmp(g_ran_method, "GET") == 0 &&
	          strcmp(g_ran_path, "/v1/containers/dns-1") == 0,
	      "a call runs its operation's request");
	check(has(out, "\"isError\":false") && has(out, "dns-1"), "a 200 is the tool's text result");
	check(has(g_log, "agent called getContainer (GET /v1/containers/dns-1) -> 200") &&
	          strcmp(g_log_level, "info") == 0,
	      "a call writes its audit line");

	ask("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":"
	    "\"getContainer\",\"arguments\":{\"name\":\"a/b c?\"}}}",
	    out, sizeof(out));
	check(strcmp(g_ran_path, "/v1/containers/a%2Fb%20c%3F") == 0,
	      "a path argument is percent-encoded, so it stays one segment");

	ask("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":"
	    "\"listContainers\",\"arguments\":{\"include_internal\":true}}}",
	    out, sizeof(out));
	check(strcmp(g_ran_path, "/v1/containers?include_internal=true") == 0,
	      "a query argument becomes the query, a boolean as its word");

	g_runs = 0;
	ask("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":"
	    "\"getContainer\",\"arguments\":{\"name\":\"x\",\"evil\":\"1\"}}}",
	    out, sizeof(out));
	check(g_runs == 0 && has(out, "\"code\":-32602") && has(out, "evil"),
	      "an argument the operation does not take is -32602, and nothing runs");

	ask("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":"
	    "\"getContainer\",\"arguments\":{}}}",
	    out, sizeof(out));
	check(g_runs == 0 && has(out, "\"code\":-32602"), "a missing path argument is -32602");

	ask("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":"
	    "\"deleteContainer\",\"arguments\":{\"name\":\"x\"}}}",
	    out, sizeof(out));
	check(g_runs == 0 && has(out, "\"code\":-32602"),
	      "an operation not marked tool cannot be called");

	g_denied = "containers:read";
	g_log[0] = '\0';
	ask("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":"
	    "\"getContainer\",\"arguments\":{\"name\":\"x\"}}}",
	    out, sizeof(out));
	check(g_runs == 0 && has(out, "\"isError\":true") && has(out, "containers:read"),
	      "a refused call runs nothing and names the permission");
	check(has(g_log, "agent refused getContainer -- needs containers:read") &&
	          strcmp(g_log_level, "warn") == 0,
	      "a refused call is audited as a warning");
	g_denied = "";

	g_run_status = 404;
	g_run_body = "{\"error\":\"no such container\"}";
	ask("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":"
	    "\"getContainer\",\"arguments\":{\"name\":\"x\"}}}",
	    out, sizeof(out));
	check(has(out, "\"isError\":true") && has(out, "no such container"),
	      "a non-2xx answer is an isError result carrying the API's error");

	g_run_status = 0;
	g_run_body = NULL;
	ask("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":"
	    "\"getContainer\",\"arguments\":{\"name\":\"x\"}}}",
	    out, sizeof(out));
	check(has(out, "\"isError\":true") && has(out, "background work"),
	      "a handler that parks is an isError result, not a hang");
	g_run_status = 200;
	g_run_body = "{\"name\":\"dns-1\"}";
}

/* The switch: off when never saved, persisted, read back. */
static void test_switch(void)
{
	char dir[] = "/tmp/test_mcp_XXXXXX";
	char path[256];
	FILE *f;

	if (mkdtemp(dir) == NULL) {
		check(0, "mkdtemp");
		return;
	}
	snprintf(path, sizeof(path), "%s/mcp.json", dir);
	mcp_init(path);
	check(!mcp_enabled(), "a host that never saved the switch is off");
	check(mcp_set_enabled(1) == MCP_OK && mcp_enabled(), "turning it on takes effect");
	mcp_init(path);
	check(mcp_enabled(), "and survives a restart");
	f = fopen(path, "w");
	if (f != NULL) {
		fputs("{\"enabled\":\"yes\"}", f);
		fclose(f);
	}
	mcp_init(path);
	check(!mcp_enabled(), "an invalid saved setting leaves it off");
	unlink(path);
	rmdir(dir);
}

int main(void)
{
	printf("test_mcp:\n");
	test_initialize();
	test_messages();
	test_tools_list();
	test_tools_call();
	test_switch();
	if (g_failures != 0) {
		printf("test_mcp: %d FAILED\n", g_failures);
		return 1;
	}
	printf("test_mcp: PASS\n");
	return 0;
}
