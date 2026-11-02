/*
 * ADR-0368: the MCP gateway end to end, against a real cixd.
 *
 * test_mcp drives the protocol against stubs; what only a daemon can
 * show is what a tool call does once it reaches a handler. A write
 * made through POST /v1/mcp runs its operation's handler in-process,
 * not through dispatch(), so it must still announce what its contract
 * makes stale (ADR-0339) -- this test ends with test_assert_no_drift(),
 * which is the check that would see a missing announcement -- and
 * still write its audit line.
 *
 * DNS records are the write used: create, update and delete through
 * the gateway, each read back over REST. No user exists, so host auth
 * is not gating and every permission is held.
 */
#include "httpclient.h"
#include "json.h"
#include "test_drift.h"
#include "test_image_fixture.h"
#include "generated/mcp_tools.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7692
#define PORT_ARG "--port=7692"

static char g_data_dir[PATH_MAX];
static int g_failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "  FAIL: %s\n", what);
		g_failures++;
	}
}

static int wait_for_daemon(const struct cix_client *c, int max_attempts)
{
	int i;
	struct cix_response r;

	for (i = 0; i < max_attempts; i++) {
		if (cix_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			cix_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/cixd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;
	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/cixd", dargv, environ);
		perror("execve build/cixd");
		_exit(127);
	}
	return pid;
}

static int stop_daemon(pid_t pid)
{
	int status;

	kill(pid, SIGTERM);
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* POSTs one JSON-RPC message; the status, and the body in *r. */
static int mcp(const struct cix_client *c, const char *message, struct cix_response *r)
{
	memset(r, 0, sizeof(*r));
	if (cix_client_request(c, "POST", "/v1/mcp", message, r) != 0)
		return -1;
	return r->status;
}

/* Whether a tools/call response is a result, and isError is `want`. */
static int call_result_is(const struct cix_response *r, int want)
{
	const struct json_value *v =
	    json_object_get(json_object_get(r->json, "result"), "isError");

	return v != NULL && v->type == JSON_BOOL && v->u.boolean == want;
}

static int body_has(const struct cix_response *r, const char *needle)
{
	return r->body != NULL && strstr(r->body, needle) != NULL;
}

static void get_status(const struct cix_client *c, const char *path, int want,
                       const char *needle, const char *what)
{
	struct cix_response r;

	memset(&r, 0, sizeof(r));
	check(cix_client_request(c, "GET", path, NULL, &r) == 0 && r.status == want &&
	          (needle == NULL || body_has(&r, needle)),
	      what);
	cix_response_free(&r);
}

/* Whether two JSON documents are objects with the same top-level keys,
 * or are both something other than an object. */
static int same_shape(const struct json_value *a, const struct json_value *b)
{
	size_t i;

	if (a == NULL || b == NULL)
		return a == b;
	if (a->type != b->type)
		return 0;
	if (a->type != JSON_OBJECT)
		return 1;
	if (a->u.object.count != b->u.object.count)
		return 0;
	for (i = 0; i < a->u.object.count; i++)
		if (json_object_get(b, a->u.object.keys[i]) == NULL)
			return 0;
	return 1;
}

/*
 * ADR-0368: every read that is a tool answers through the gateway as
 * it answers over REST, from the same daemon. A tool call runs the same
 * handler in-process, so what this catches is a handler that behaves
 * differently there -- one that parks (the gateway is told busy), or
 * reads the connection -- which test_mcp_exposure's scan of the source
 * could miss one call further down. A path parameter is given a name
 * nothing has, so the comparison is of two not-founds and nothing is
 * created. Success must agree; a success must carry the same top-level
 * keys (a value may change between two reads of a live host); a
 * failure must carry the same error body.
 */
static void check_read_equivalence(const struct cix_client *c)
{
	int i, compared = 0;

	for (i = 0; i < CIX_MCP_TOOL_COUNT; i++) {
		const struct cix_mcp_tool *t = &cix_mcp_tools[i];
		char path[512], args[512], msg[1024], what[600];
		struct cix_response rest, gw;
		const struct json_value *result, *text, *is_error;
		struct json_value *mcp_body;
		size_t pl = 0, al = 0;
		const char *p;
		int rest_ok, ok;

		if (strcmp(t->method, "GET") != 0)
			continue;
		al += (size_t)snprintf(args + al, sizeof(args) - al, "{");
		for (p = t->path; *p != '\0' && pl + 1 < sizeof(path); p++) {
			if (*p == '{') {
				const char *e = strchr(p, '}');

				if (e == NULL)
					break;
				pl += (size_t)snprintf(path + pl, sizeof(path) - pl, "mcp-equiv-none");
				al += (size_t)snprintf(args + al, sizeof(args) - al, "%s\"%.*s\":\"mcp-equiv-none\"",
				                       al > 1 ? "," : "", (int)(e - p - 1), p + 1);
				p = e;
				continue;
			}
			path[pl++] = *p;
		}
		path[pl] = '\0';
		snprintf(args + al, sizeof(args) - al, "}");

		memset(&rest, 0, sizeof(rest));
		memset(&gw, 0, sizeof(gw));
		snprintf(msg, sizeof(msg),
		         "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"%s\","
		         "\"arguments\":%s}}",
		         t->name, args);
		if (cix_client_request(c, "GET", path, NULL, &rest) != 0 ||
		    cix_client_request(c, "POST", "/v1/mcp", msg, &gw) != 0 || gw.status != 200) {
			snprintf(what, sizeof(what), "read equivalence: %s could not be asked both ways",
			         t->name);
			check(0, what);
			cix_response_free(&rest);
			cix_response_free(&gw);
			continue;
		}
		result = json_object_get(gw.json, "result");
		is_error = json_object_get(result, "isError");
		text = json_object_get(json_object_get(result, "content") != NULL &&
		                               json_object_get(result, "content")->u.array.count > 0
		                           ? json_object_get(result, "content")->u.array.items[0]
		                           : NULL,
		                       "text");
		rest_ok = rest.status >= 200 && rest.status < 300;
		mcp_body = json_as_string(text) != NULL
		               ? json_parse(json_as_string(text), strlen(json_as_string(text)))
		               : NULL;
		if (is_error == NULL || is_error->type != JSON_BOOL || is_error->u.boolean == rest_ok)
			ok = 0;
		else if (rest_ok)
			ok = same_shape(rest.json, mcp_body);
		else
			ok = rest.body != NULL && json_as_string(text) != NULL &&
			     strcmp(rest.body, json_as_string(text)) == 0;
		snprintf(what, sizeof(what), "read equivalence: %s (REST %d): %.200s / %.200s", t->name,
		         rest.status, rest.body != NULL ? rest.body : "",
		         json_as_string(text) != NULL ? json_as_string(text) : "");
		check(ok, what);
		compared++;
		json_free(mcp_body);
		cix_response_free(&rest);
		cix_response_free(&gw);
	}
	printf("  read equivalence: %d reads compared\n", compared);
	check(compared >= 100, "read equivalence compared every read tool");
}

/*
 * ADR-0368 step 5: a filtered log read runs in a helper (ADR-0342), so
 * through the gateway it parks the MCP request itself, which is served
 * again when the read is done -- and must then answer what REST does.
 */
static void check_parked_read(const struct cix_client *c)
{
	struct cix_response rest, gw;
	const struct json_value *result, *text;
	struct json_value *mcp_body = NULL;

	memset(&rest, 0, sizeof(rest));
	memset(&gw, 0, sizeof(gw));
	if (cix_client_request(c, "GET", "/v1/system/logs?source=cixd&tail=5", NULL, &rest) != 0 ||
	    cix_client_request(c, "POST", "/v1/mcp",
	                       "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":"
	                       "{\"name\":\"getSystemLogs\",\"arguments\":{\"source\":\"cixd\","
	                       "\"tail\":\"5\"}}}",
	                       &gw) != 0) {
		check(0, "step 5: a filtered log read could not be asked both ways");
		cix_response_free(&rest);
		cix_response_free(&gw);
		return;
	}
	result = json_object_get(gw.json, "result");
	text = json_object_get(json_object_get(result, "content") != NULL &&
	                               json_object_get(result, "content")->u.array.count > 0
	                           ? json_object_get(result, "content")->u.array.items[0]
	                           : NULL,
	                       "text");
	if (json_as_string(text) != NULL)
		mcp_body = json_parse(json_as_string(text), strlen(json_as_string(text)));
	check(rest.status == 200 && gw.status == 200 &&
	          json_object_get(result, "isError") != NULL &&
	          json_object_get(result, "isError")->u.boolean == 0 &&
	          same_shape(rest.json, mcp_body),
	      "step 5: a filtered log read parks the MCP request and answers as REST does");
	if (gw.body != NULL && json_object_get(result, "isError") == NULL)
		fprintf(stderr, "    %.300s\n", gw.body);
	json_free(mcp_body);
	cix_response_free(&rest);
	cix_response_free(&gw);
}

int main(void)
{
	struct cix_client c;
	struct cix_response r;
	pid_t pid;

	printf("test_mcp_gateway:\n");
	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	pid = start_daemon();
	cix_client_init(&c, "127.0.0.1", TEST_PORT);
	if (pid < 0 || wait_for_daemon(&c, 50) != 0) {
		fprintf(stderr, "  FAIL: daemon never accepted connections\n");
		if (pid > 0) {
			kill(pid, SIGKILL);
			waitpid(pid, NULL, 0);
		}
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	check(mcp(&c, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}", &r) == 503,
	      "the gateway answers 503 while MCP is off");
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(cix_client_request(&c, "PUT", "/v1/system/mcp", "{\"enabled\":true}", &r) == 0 &&
	          r.status == 200,
	      "PUT /v1/system/mcp turns it on");
	cix_response_free(&r);

	check(mcp(&c,
	          "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
	          "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
	          "\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}",
	          &r) == 200 &&
	          body_has(&r, "\"protocolVersion\":\"2025-06-18\""),
	      "initialize is answered");
	cix_response_free(&r);

	check_read_equivalence(&c);
	check_parked_read(&c);

	check(mcp(&c,
	          "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":"
	          "\"createDnsRecord\",\"arguments\":{\"body\":{\"name\":\"mcp-gw.lan\",\"ip\":\"10.77.0.1\"}}}}",
	          &r) == 200 &&
	          call_result_is(&r, 0),
	      "createDnsRecord through the gateway succeeds");
	if (!call_result_is(&r, 0))
		fprintf(stderr, "    %.400s\n", r.body != NULL ? r.body : "");
	cix_response_free(&r);
	get_status(&c, "/v1/dns/records/mcp-gw.lan", 200, "10.77.0.1",
	           "the record the gateway created reads back over REST");

	check(mcp(&c,
	          "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":"
	          "\"updateDnsRecord\",\"arguments\":{\"name\":\"mcp-gw.lan\",\"body\":{\"ip\":\"10.77.0.2\"}}}}",
	          &r) == 200 &&
	          call_result_is(&r, 0),
	      "updateDnsRecord through the gateway succeeds");
	cix_response_free(&r);
	get_status(&c, "/v1/dns/records/mcp-gw.lan", 200, "10.77.0.2",
	           "the update reads back over REST");

	check(mcp(&c,
	          "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":"
	          "\"createDnsRecord\",\"arguments\":{\"body\":{\"name\":\"bad name!\",\"ip\":\"10.77.0.3\"}}}}",
	          &r) == 200 &&
	          call_result_is(&r, 1),
	      "a body the handler refuses is an isError result");
	cix_response_free(&r);

	check(mcp(&c,
	          "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":"
	          "\"deleteDnsRecord\",\"arguments\":{\"name\":\"mcp-gw.lan\"}}}",
	          &r) == 200 &&
	          call_result_is(&r, 0),
	      "deleteDnsRecord through the gateway succeeds");
	cix_response_free(&r);
	get_status(&c, "/v1/dns/records/mcp-gw.lan", 404, NULL, "the deleted record is gone");

	get_status(&c, "/v1/system/logs?source=audit&tail=50", 200,
	           "called createDnsRecord (POST /v1/dns/records) -> 201",
	           "a write through the gateway is audited with its status");

	/* ADR-0339: every write above announced what it made stale. */
	check(test_assert_no_drift(&c, NULL) == 0, "the gateway's writes caused no drift");

	check(stop_daemon(pid) == 0, "cixd exits cleanly");
	test_data_dir_cleanup(g_data_dir);
	if (g_failures != 0) {
		printf("test_mcp_gateway: %d FAILED\n", g_failures);
		return 1;
	}
	printf("test_mcp_gateway: PASS\n");
	return 0;
}
