#include "mcp.h"
#include "base64.h"
#include "json.h"
#include "logstore.h"
#include "persist.h"
#include "generated/mcp_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* ---------- the gateway ---------- */

/* The revision implemented, and the older ones whose tool methods are
 * the same, which a client asking for one of them is answered in. */
#define MCP_PROTOCOL "2025-06-18"
static const char *const mcp_older[] = { "2025-03-26", "2024-11-05" };

int mcp_tool_count(void)
{
	return CIX_MCP_TOOL_COUNT;
}

static const struct cix_mcp_tool *tool_by_name(const char *name)
{
	int i;

	for (i = 0; name != NULL && i < CIX_MCP_TOOL_COUNT; i++)
		if (strcmp(cix_mcp_tools[i].name, name) == 0)
			return &cix_mcp_tools[i];
	return NULL;
}

/* Whether `name` is one of a comma-separated list. */
static int in_list(const char *list, const char *name)
{
	size_t n = strlen(name);
	const char *p = list;

	while (*p != '\0') {
		const char *comma = strchr(p, ',');
		size_t len = comma != NULL ? (size_t)(comma - p) : strlen(p);

		if (len == n && strncmp(p, name, n) == 0)
			return 1;
		if (comma == NULL)
			break;
		p = comma + 1;
	}
	return 0;
}

/* Echoes the request's id: a string, a number or null. */
static void write_id(struct json_writer *w, const struct json_value *id)
{
	jw_key(w, "id");
	if (id != NULL && id->type == JSON_STRING)
		jw_str(w, id->u.string);
	else if (id != NULL && id->type == JSON_NUMBER)
		jw_int(w, (long long)id->u.number);
	else
		jw_null(w);
}

static void write_error(struct json_writer *w, const struct json_value *id, int code,
                        const char *message)
{
	jw_obj_open(w);
	jw_key(w, "jsonrpc");
	jw_str(w, "2.0");
	write_id(w, id);
	jw_key(w, "error");
	jw_obj_open(w);
	jw_key(w, "code");
	jw_int(w, code);
	jw_key(w, "message");
	jw_str(w, message);
	jw_obj_close(w);
	jw_obj_close(w);
}

/* Opens {"jsonrpc":"2.0","id":...,"result": -- the caller writes the
 * result value and closes with jw_obj_close(). */
static void open_result(struct json_writer *w, const struct json_value *id)
{
	jw_obj_open(w);
	jw_key(w, "jsonrpc");
	jw_str(w, "2.0");
	write_id(w, id);
	jw_key(w, "result");
}

static void do_initialize(struct json_writer *w, const struct json_value *id,
                          const struct json_value *params, const struct mcp_caller *c)
{
	const char *asked = json_as_string(json_object_get(params, "protocolVersion"));
	const char *answer = MCP_PROTOCOL;
	size_t i;

	for (i = 0; asked != NULL && i < sizeof(mcp_older) / sizeof(mcp_older[0]); i++)
		if (strcmp(asked, mcp_older[i]) == 0)
			answer = mcp_older[i];
	open_result(w, id);
	jw_obj_open(w);
	jw_key(w, "protocolVersion");
	jw_str(w, answer);
	jw_key(w, "capabilities");
	jw_obj_open(w);
	jw_key(w, "tools");
	jw_obj_open(w);
	jw_key(w, "listChanged");
	jw_bool(w, 0);
	jw_obj_close(w);
	jw_obj_close(w);
	jw_key(w, "serverInfo");
	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, "cixd");
	jw_key(w, "version");
	jw_str(w, c->server_version != NULL ? c->server_version : "");
	jw_obj_close(w);
	jw_key(w, "instructions");
	jw_str(w, "Each tool is one operation of the Cix host API (docs/api/openapi.yaml), named "
	          "by its operationId; its arguments are the operation's path and query "
	          "parameters, and `body` holds the request body when the operation takes one. A "
	          "call does exactly what the REST request would, under your own permissions.");
	jw_obj_close(w);
	jw_obj_close(w);
}

/* tools/list: every tool the caller's permissions allow. */
static void do_tools_list(struct json_writer *w, const struct json_value *id,
                          const struct mcp_caller *c)
{
	int i;

	open_result(w, id);
	jw_obj_open(w);
	jw_key(w, "tools");
	jw_arr_open(w);
	for (i = 0; i < CIX_MCP_TOOL_COUNT; i++) {
		const struct cix_mcp_tool *t = &cix_mcp_tools[i];

		if (!c->may(c->ctx, t->permission))
			continue;
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, t->name);
		jw_key(w, "description");
		jw_str(w, t->summary);
		/* Rendered by apigen from the contract: the path parameters,
		 * the query parameters and the body's properties. */
		jw_key(w, "inputSchema");
		jw_raw_text(w, t->input_schema, strlen(t->input_schema));
		jw_key(w, "annotations");
		jw_obj_open(w);
		jw_key(w, "readOnlyHint");
		jw_bool(w, t->read_only);
		jw_key(w, "destructiveHint");
		jw_bool(w, t->destructive);
		jw_obj_close(w);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
	jw_obj_close(w);
}

/* An argument's value as URL text: a string, a number or a boolean. */
static int arg_text(const struct json_value *v, char *out, size_t out_size)
{
	if (v == NULL)
		return -1;
	if (v->type == JSON_STRING)
		return snprintf(out, out_size, "%s", v->u.string) >= (int)out_size ? -1 : 0;
	if (v->type == JSON_NUMBER)
		return snprintf(out, out_size, "%.17g", v->u.number) >= (int)out_size ? -1 : 0;
	if (v->type == JSON_BOOL)
		return snprintf(out, out_size, "%s", v->u.boolean ? "true" : "false") >= (int)out_size
		               ? -1
		               : 0;
	return -1;
}

/* Appends s percent-encoded (RFC 3986 unreserved kept) at out[*at]. */
static int append_encoded(char *out, size_t out_size, size_t *at, const char *s)
{
	static const char hex[] = "0123456789ABCDEF";

	for (; *s != '\0'; s++) {
		unsigned char ch = (unsigned char)*s;
		int plain = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
		            (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~';

		if (*at + (plain ? 1 : 3) >= out_size)
			return -1;
		if (plain) {
			out[(*at)++] = (char)ch;
		} else {
			out[(*at)++] = '%';
			out[(*at)++] = hex[ch >> 4];
			out[(*at)++] = hex[ch & 15];
		}
	}
	out[*at] = '\0';
	return 0;
}

/*
 * The request path for a tool call: the contract's path with each
 * {name} replaced by its argument, then the query arguments given.
 * Returns 0, or -1 with `why` set.
 */
static int build_path(const struct cix_mcp_tool *t, const struct json_value *args, char *out,
                      size_t out_size, char *why, size_t why_size)
{
	const char *p = t->path;
	size_t at = 0, i;
	int first_query = 1;

	if (args != NULL && args->type != JSON_OBJECT) {
		snprintf(why, why_size, "arguments must be an object");
		return -1;
	}
	for (i = 0; args != NULL && i < args->u.object.count; i++)
		if (!in_list(t->path_params, args->u.object.keys[i]) &&
		    !in_list(t->query_params, args->u.object.keys[i]) &&
		    !(t->has_body && strcmp(args->u.object.keys[i], "body") == 0)) {
			snprintf(why, why_size, "%s takes no argument \"%s\"", t->name,
			         args->u.object.keys[i]);
			return -1;
		}
	out[0] = '\0';
	while (*p != '\0') {
		if (*p == '{') {
			const char *e = strchr(p, '}');
			char name[128], text[1024];

			if (e == NULL)
				break;
			snprintf(name, sizeof(name), "%.*s", (int)(e - p - 1), p + 1);
			if (arg_text(json_object_get(args, name), text, sizeof(text)) != 0 ||
			    text[0] == '\0') {
				snprintf(why, why_size, "%s needs \"%s\"", t->name, name);
				return -1;
			}
			if (append_encoded(out, out_size, &at, text) != 0)
				goto too_long;
			p = e + 1;
			continue;
		}
		if (at + 1 >= out_size)
			goto too_long;
		out[at++] = *p++;
		out[at] = '\0';
	}
	for (i = 0; args != NULL && i < args->u.object.count; i++) {
		char text[1024];

		if (!in_list(t->query_params, args->u.object.keys[i]))
			continue;
		if (arg_text(args->u.object.values[i], text, sizeof(text)) != 0) {
			snprintf(why, why_size, "\"%s\" must be a string, a number or a boolean",
			         args->u.object.keys[i]);
			return -1;
		}
		if (at + 2 >= out_size)
			goto too_long;
		out[at++] = first_query ? '?' : '&';
		out[at] = '\0';
		first_query = 0;
		if (append_encoded(out, out_size, &at, args->u.object.keys[i]) != 0 || at + 2 >= out_size)
			goto too_long;
		out[at++] = '=';
		out[at] = '\0';
		if (append_encoded(out, out_size, &at, text) != 0)
			goto too_long;
	}
	return 0;
too_long:
	snprintf(why, why_size, "the arguments make a path longer than %zu bytes", out_size - 1);
	return -1;
}

/*
 * The request body for a tool call: the `body` argument, as the JSON
 * the client sent, or none when it sent none. The handler validates it
 * as it validates any request, so nothing is checked twice here.
 */
static int build_body(const struct json_value *args, struct json_writer *body)
{
	const struct json_value *v = json_object_get(args, "body");

	if (v == NULL)
		return 0;
	jw_value(body, v);
	return 1;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A tools/call result: one text item, isError when it did not succeed. */
static void write_call_result(struct json_writer *w, const struct json_value *id, const char *text,
                              int is_error)
{
	open_result(w, id);
	jw_obj_open(w);
	jw_key(w, "content");
	jw_arr_open(w);
	jw_obj_open(w);
	jw_key(w, "type");
	jw_str(w, "text");
	jw_key(w, "text");
	jw_str(w, text);
	jw_obj_close(w);
	jw_arr_close(w);
	jw_key(w, "isError");
	jw_bool(w, is_error);
	jw_obj_close(w);
	jw_obj_close(w);
}

/*
 * A successful answer that is not JSON -- a file's bytes, a build log,
 * an artifact -- as an embedded resource whose `blob` is the bytes in
 * base64 (the owner, 2026-11-02), so nothing in them is altered or
 * lost on the way to the client. Its URI is the request it answers.
 */
static void write_resource_result(struct json_writer *w, const struct json_value *id,
                                  const char *path, const char *type, const char *body,
                                  size_t body_len)
{
	size_t b64_size = ((body_len + 2) / 3) * 4 + 1;
	char *b64 = malloc(b64_size);
	char uri[2100];

	if (b64 == NULL ||
	    base64_encode((const unsigned char *)body, body_len, b64, b64_size) != 0) {
		free(b64);
		write_call_result(w, id, "the answer could not be encoded", 1);
		return;
	}
	snprintf(uri, sizeof(uri), "cix:%s", path);
	open_result(w, id);
	jw_obj_open(w);
	jw_key(w, "content");
	jw_arr_open(w);
	jw_obj_open(w);
	jw_key(w, "type");
	jw_str(w, "resource");
	jw_key(w, "resource");
	jw_obj_open(w);
	jw_key(w, "uri");
	jw_str(w, uri);
	jw_key(w, "mimeType");
	jw_str(w, type);
	jw_key(w, "blob");
	jw_str(w, b64);
	jw_obj_close(w);
	jw_obj_close(w);
	jw_arr_close(w);
	jw_key(w, "isError");
	jw_bool(w, 0);
	jw_obj_close(w);
	jw_obj_close(w);
	free(b64);
}

/* 1 when the call parked this request (nothing written), else 0. */
static int do_tools_call(struct json_writer *w, const struct json_value *id,
                         const struct json_value *params, const struct mcp_caller *c)
{
	const char *name = json_as_string(json_object_get(params, "name"));
	const struct json_value *args = json_object_get(params, "arguments");
	const struct cix_mcp_tool *t = tool_by_name(name);
	char path[2048], why[256], msg[384], type[128];
	struct json_writer req;
	char *body = NULL;
	size_t body_len = 0;
	int status = 0, sent, ran;
	long long started;
	const char *user = c->user != NULL && c->user[0] != '\0' ? c->user : "-";

	if (t == NULL) {
		snprintf(msg, sizeof(msg), "no tool named \"%s\"", name != NULL ? name : "");
		write_error(w, id, -32602, msg);
		return 0;
	}
	if (!c->may(c->ctx, t->permission)) {
		logstore_write("audit", "warn", "mcp: %s refused %s -- needs %s (ADR-0368)", user,
		               t->name, t->permission);
		snprintf(msg, sizeof(msg), "%s needs the permission %s, which you do not hold", t->name,
		         t->permission);
		write_call_result(w, id, msg, 1);
		return 0;
	}
	if (build_path(t, args, path, sizeof(path), why, sizeof(why)) != 0) {
		write_error(w, id, -32602, why);
		return 0;
	}
	jw_init(&req);
	sent = t->has_body && build_body(args, &req);
	started = now_ms();
	ran = c->run(c->ctx, t->method, path, sent ? req.buf : NULL, sent ? req.len : 0, &status,
	             &body, &body_len, type, sizeof(type));
	jw_free(&req);
	if (ran < 0) {
		snprintf(msg, sizeof(msg), "%s %s matched no route", t->method, path);
		write_call_result(w, id, msg, 1);
		return 0;
	}
	if (ran == 1) {
		/* Step 5: the work parked this request. It is audited on the
		 * pass that answers, when the request is served again. */
		free(body);
		return 1;
	}
	logstore_write("audit", "info", "mcp: %s called %s (%s %s) -> %d in %lld ms (ADR-0368)", user,
	               t->name, t->method, path, status, now_ms() - started);
	if (status == 0) {
		snprintf(msg, sizeof(msg), "%s wrote no answer", t->name);
		write_call_result(w, id, msg, 1);
	} else if (status >= 200 && status < 300 && body != NULL &&
	           strncmp(type, "application/json", 16) != 0) {
		write_resource_result(w, id, path, type, body, body_len);
	} else {
		write_call_result(w, id, body != NULL ? body : "", status < 200 || status >= 300);
	}
	free(body);
	return 0;
}

int mcp_handle(const char *request, size_t len, const struct mcp_caller *c,
               struct json_writer *w)
{
	struct json_value *root = json_parse(request, len);
	const struct json_value *id, *params;
	const char *version, *method;

	if (root == NULL || root->type != JSON_OBJECT) {
		write_error(w, NULL, root == NULL ? -32700 : -32600,
		            root == NULL ? "parse error" : "a request must be a JSON object");
		json_free(root);
		return 200;
	}
	id = json_object_get(root, "id");
	params = json_object_get(root, "params");
	version = json_as_string(json_object_get(root, "jsonrpc"));
	method = json_as_string(json_object_get(root, "method"));
	/* A client's response to a request of ours: we send none, so
	 * there is nothing to match it to. Accepted, as the transport says. */
	if (method == NULL && version != NULL && strcmp(version, "2.0") == 0 &&
	    (json_object_get(root, "result") != NULL || json_object_get(root, "error") != NULL)) {
		json_free(root);
		return 202;
	}
	if (version == NULL || strcmp(version, "2.0") != 0 || method == NULL) {
		write_error(w, id, -32600, "jsonrpc must be \"2.0\" and method a string");
		json_free(root);
		return 200;
	}
	/* A notification: accepted, nothing sent. */
	if (id == NULL) {
		json_free(root);
		return 202;
	}
	if (strcmp(method, "initialize") == 0) {
		do_initialize(w, id, params, c);
	} else if (strcmp(method, "ping") == 0) {
		open_result(w, id);
		jw_obj_open(w);
		jw_obj_close(w);
		jw_obj_close(w);
	} else if (strcmp(method, "tools/list") == 0) {
		do_tools_list(w, id, c);
	} else if (strcmp(method, "tools/call") == 0) {
		if (do_tools_call(w, id, params, c)) {
			json_free(root);
			return MCP_PARKED;
		}
	} else {
		char msg[160];

		snprintf(msg, sizeof(msg), "no method \"%.100s\"", method);
		write_error(w, id, -32601, msg);
	}
	json_free(root);
	return 200;
}
