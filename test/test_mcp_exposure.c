/*
 * test_mcp_exposure -- an operation the contract marks `x-cix-mcp: tool`
 * has a handler that answers in-process (ADR-0368).
 *
 * A tool call runs the operation's own handler through api_run(),
 * which differs from a client's request in three ways: no socket (fd
 * -1), no request headers, and a sentinel connection that cannot be
 * served again. So whether an operation can be a tool is a property of
 * its handler's TEXT, and this reads it, in the test_announce pattern:
 * for every tool, its `op_<operationId>` function and each `handle_*`
 * that function calls, one level down, must contain none of
 *
 *   - a park: `_ask(`, `park_body_take(`, `bootmgr_submit(`,
 *     `update_submit(`. A parked handler is answered busy in-process
 *     (park_body_take()'s and credcheck_ask()'s own sentinel checks),
 *     so it is safe, but it can never succeed until step 5 serves it.
 *   - a password hash: `hash_request_password(`, `credcheck_hash(`,
 *     `credcheck_new_secret(`, which park too.
 *   - a header read: `http_find_header(`, `->headers`. In-process there
 *     are none, so the answer would be about nobody.
 *   - an in-process run of its own: `api_run(`, `events_run_get(`,
 *     which would reuse the one sentinel the outer call is using.
 *
 * What the contract alone decides (a WebSocket, `authenticated`, a body
 * that is not JSON) apigen refuses; this is the half only the source
 * can answer. When written (2026-11-02), 29 of 356 operations stayed
 * `none`: 3 WebSockets, 4 more `authenticated`, and 3 non-JSON bodies,
 * which are apigen's to refuse; and 15 that park, 2 that read a header
 * (whoami, logout) and 2 that run operations in-process themselves
 * (verifyEvents, callMcp), which are this test's.
 *
 * In SELFTESTS: it reads files and runs nothing.
 */
#include "generated/mcp_tools.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static struct {
	char name[128];
	char *text;
} g_files[256];
static int g_file_count;

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "r");
	char *buf;
	long n;

	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n + 2);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	buf[0] = '\n'; /* so a definition on line 1 still follows a newline */
	if (fread(buf + 1, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[n + 1] = '\0';
	fclose(f);
	return buf;
}

static void load_sources(void)
{
	DIR *d = opendir("daemon/src");
	struct dirent *de;

	if (d == NULL) {
		fprintf(stderr, "  FAIL: daemon/src is unreadable\n");
		g_failures++;
		return;
	}
	while ((de = readdir(d)) != NULL) {
		size_t n = strlen(de->d_name);
		char path[300];

		if (n < 3 || strcmp(de->d_name + n - 2, ".c") != 0)
			continue;
		/* Every file, or the scan is of a part of the daemon: a
		 * 64-file cap once dropped main.c (130 files, 2026-11-02). */
		if (g_file_count >= (int)(sizeof(g_files) / sizeof(g_files[0]))) {
			fprintf(stderr, "  FAIL: daemon/src holds more sources than this test reads; "
			                "raise g_files\n");
			g_failures++;
			break;
		}
		snprintf(path, sizeof(path), "daemon/src/%s", de->d_name);
		g_files[g_file_count].text = slurp(path);
		if (g_files[g_file_count].text == NULL)
			continue;
		snprintf(g_files[g_file_count].name, sizeof(g_files[0].name), "%s", de->d_name);
		g_file_count++;
	}
	closedir(d);
}

/*
 * The body of the function NAME's definition, from its `{` to its
 * closing `}` at the start of a line, copied into a new buffer; NULL
 * when no file defines it. A definition starts at a line's beginning
 * with `NAME(` after a return type, and reaches a line that is `{`
 * before any `;` -- which is what tells it from a prototype.
 */
static char *function_body(const char *name)
{
	char needle[160];
	int i;

	snprintf(needle, sizeof(needle), " %s(", name);
	for (i = 0; i < g_file_count; i++) {
		const char *p = g_files[i].text;

		while ((p = strstr(p, needle)) != NULL) {
			const char *line = p;
			const char *open, *semi, *close;

			while (line > g_files[i].text && line[-1] != '\n')
				line--;
			p += strlen(needle);
			/* The name must be on a declaration line, not a call. */
			if (*line == '\t' || *line == ' ' || *line == '\n' || *line == '#' ||
			    *line == '*' || *line == '/')
				continue;
			open = strstr(p, "\n{\n");
			semi = strchr(p, ';');
			if (open == NULL || (semi != NULL && semi < open))
				continue;
			close = strstr(open, "\n}\n");
			if (close == NULL)
				continue;
			{
				size_t n = (size_t)(close - open) + 3;
				char *out = malloc(n + 1);

				if (out == NULL)
					return NULL;
				memcpy(out, open, n);
				out[n] = '\0';
				return out;
			}
		}
	}
	return NULL;
}

static const char *const g_refused[] = {
	"_ask(",          "park_body_take(",        "bootmgr_submit(",
	"update_submit(", "hash_request_password(", "credcheck_hash(",
	"credcheck_new_secret(", "http_find_header(", "->headers",
	"api_run(",       "events_run_get(",
};

/* The first refused text in body, or NULL. */
static const char *refused_in(const char *body)
{
	size_t i;

	for (i = 0; i < sizeof(g_refused) / sizeof(g_refused[0]); i++)
		if (strstr(body, g_refused[i]) != NULL)
			return g_refused[i];
	return NULL;
}

static void check_tool(const char *op_id)
{
	char fn[160];
	char *body;
	const char *bad, *p;

	snprintf(fn, sizeof(fn), "op_%s", op_id);
	body = function_body(fn);
	if (body == NULL) {
		fprintf(stderr, "  FAIL: tool %s has no %s() in daemon/src\n", op_id, fn);
		g_failures++;
		return;
	}
	bad = refused_in(body);
	if (bad != NULL) {
		fprintf(stderr, "  FAIL: tool %s: %s() contains `%s`, so it cannot answer in-process "
		                "-- mark it x-cix-mcp: none (ADR-0368)\n",
		        op_id, fn, bad);
		g_failures++;
	}
	/* One level down: every handle_* it calls. */
	for (p = strstr(body, "handle_"); p != NULL; p = strstr(p + 1, "handle_")) {
		char callee[128];
		size_t n = 0;
		char *cbody;

		if (p > body && (p[-1] == '_' || (p[-1] >= 'a' && p[-1] <= 'z')))
			continue;
		while ((p[n] >= 'a' && p[n] <= 'z') || p[n] == '_' || (p[n] >= '0' && p[n] <= '9'))
			n++;
		if (p[n] != '(' || n >= sizeof(callee))
			continue;
		memcpy(callee, p, n);
		callee[n] = '\0';
		cbody = function_body(callee);
		if (cbody == NULL)
			continue;
		bad = refused_in(cbody);
		if (bad != NULL) {
			fprintf(stderr, "  FAIL: tool %s: %s(), which %s() calls, contains `%s`, so it "
			                "cannot answer in-process -- mark it x-cix-mcp: none (ADR-0368)\n",
			        op_id, callee, fn, bad);
			g_failures++;
		}
		free(cbody);
	}
	free(body);
}

int main(void)
{
	int i;

	printf("test_mcp_exposure:\n");
	load_sources();
	if (g_file_count < 10) {
		fprintf(stderr, "  FAIL: read %d daemon sources, expected the whole tree\n",
		        g_file_count);
		return 1;
	}
	/* Controls: the scan must see a park and a header read where they
	 * are, or a clean result means nothing. */
	{
		char *b = function_body("handle_logs_get");

		if (b == NULL || refused_in(b) == NULL) {
			fprintf(stderr, "  FAIL: control: handle_logs_get() was not found to park\n");
			g_failures++;
		}
		free(b);
		b = function_body("op_getHealth");
		if (b == NULL) {
			fprintf(stderr, "  FAIL: control: op_getHealth() was not found -- the scan "
			                "did not read main.c\n");
			g_failures++;
		}
		free(b);
		b = function_body("handle_whoami");
		if (b == NULL || refused_in(b) == NULL) {
			fprintf(stderr, "  FAIL: control: handle_whoami() was not found to read a "
			                "header\n");
			g_failures++;
		}
		free(b);
	}
	for (i = 0; i < CIX_MCP_TOOL_COUNT; i++)
		check_tool(cix_mcp_tools[i].name);
	printf("  %d tools checked\n", CIX_MCP_TOOL_COUNT);
	if (g_failures != 0) {
		printf("test_mcp_exposure: %d FAILED\n", g_failures);
		return 1;
	}
	printf("test_mcp_exposure: PASS\n");
	return 0;
}
