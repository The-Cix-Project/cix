/*
 * test_web_syntax -- the dashboard's JavaScript must parse (#340).
 *
 * Every other surface this project ships is read by a compiler before
 * it reaches a host: cixd, cixctl, the tests and the recipes all go
 * through tcc. web/*.js went through nothing. On 2026-09-08 a dangling
 * `else`, left behind when a refactor removed one arm of an if/else,
 * shipped and took the ENTIRE web UI down on a deployed host:
 *
 *     app.js:2967 Uncaught SyntaxError: Unexpected token 'else'
 *
 * A file that does not parse runs no script at all, so that is a blank
 * dashboard rather than one broken feature -- and it was found by the
 * owner opening the page, which is the wrong way to find it.
 *
 * WHY A REAL PARSER. A textual check was considered and rejected
 * (#340): the broken shape was an `else` whose preceding token is `;`,
 * perfectly legal after a braceless `if`, and this file uses braceless
 * `if` freely. Telling those apart needs a parser, not a heuristic, and
 * a gate with false positives is worse than no gate. So this links
 * quickjs and asks it to COMPILE each file without running it --
 * JS_EVAL_FLAG_COMPILE_ONLY, which parses and generates bytecode and
 * executes none of it. Nothing in the dashboard runs here.
 *
 * This is the gate that runs in SELFTESTS, inside a build container on
 * the box, where node is not packaged and building it would be a V8
 * compile. `make web-syntax` is a weaker dev-sandbox convenience: node
 * over web/*.js, so it misses index.html's inline block, which this
 * covers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

static int g_fail;

static char *read_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) {
		fclose(f);
		return NULL;
	}
	rewind(f);
	buf = malloc((size_t)n + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[n] = '\0';
	*out_len = (size_t)n;
	return buf;
}

/*
 * 0 when the source parses. The message is the engine's own, including
 * its line number -- the point of using a parser is that its complaint
 * names the place.
 */
static int parses(JSContext *ctx, const char *src, size_t len, const char *name, char *err,
                  size_t err_size)
{
	JSValue v = JS_Eval(ctx, src, len, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
	JSValue exc;
	const char *msg;

	err[0] = '\0';
	if (!JS_IsException(v)) {
		JS_FreeValue(ctx, v);
		return 0;
	}
	exc = JS_GetException(ctx);
	msg = JS_ToCString(ctx, exc);
	snprintf(err, err_size, "%s", msg != NULL ? msg : "(no message)");
	if (msg != NULL)
		JS_FreeCString(ctx, msg);
	JS_FreeValue(ctx, exc);
	JS_FreeValue(ctx, v);
	return -1;
}

/*
 * A gate that has never rejected anything is a gate nobody has tested.
 * These two run before the real files: the broken one is the SHAPE that
 * caused the outage -- an `else` with no `if` to attach to -- and the
 * good one is there so a parser that rejected everything would fail
 * here rather than look like a pass.
 */
static void self_check(JSContext *ctx)
{
	/*
	 * strlen, never a literal count: a hand-written length that is
	 * one short truncates the snippet's closing brace, and the good
	 * case then fails as a syntax error for a reason that has
	 * nothing to do with the gate. Both counts here were off by one
	 * when first written.
	 */
	static const char GOOD[] = "function f(a) { if (a) g(); else h(); }";
	static const char BAD[] = "function f(a) { if (a) g(); else h(); else }";
	char err[512];

	if (parses(ctx, GOOD, strlen(GOOD), "<good>", err, sizeof(err)) != 0) {
		fprintf(stderr, "FAIL: self-check: valid JavaScript was rejected: %s\n", err);
		g_fail = 1;
	}
	if (parses(ctx, BAD, strlen(BAD), "<bad>", err, sizeof(err)) == 0) {
		fprintf(stderr, "FAIL: self-check: a dangling `else` was ACCEPTED -- this gate "
		                "would not have caught the outage it was written for\n");
		g_fail = 1;
	}
}

/*
 * An inline <script> in index.html is JavaScript the browser runs too,
 * so it belongs in the gate. Blocks carrying src= are skipped: their
 * body is a file of its own and is checked as one.
 *
 * JS_Eval requires a zero-terminated input, so each block is
 * terminated in place and restored -- the buffer is ours and mutable.
 */
static int parse_html_scripts(JSContext *ctx, const char *path, char *src)
{
	char *cur = src;
	int blocks = 0;

	while ((cur = strstr(cur, "<script")) != NULL) {
		char *open_end = strchr(cur, '>');
		char *body, *close;
		char saved;
		char err[512];
		char name[256];
		int has_src;

		if (open_end == NULL)
			break;

		/*
		 * Look for src= within the open tag only, so an occurrence in
		 * the script body cannot be mistaken for an attribute.
		 */
		saved = *open_end;
		*open_end = '\0';
		has_src = strstr(cur, "src=") != NULL;
		*open_end = saved;

		body = open_end + 1;
		close = strstr(body, "</script>");
		if (close == NULL)
			break;
		cur = close + 9;

		/* A src= block's body is a file of its own, checked as one. */
		if (has_src)
			continue;

		blocks++;
		snprintf(name, sizeof(name), "%s inline block %d", path, blocks);
		saved = *close;
		*close = '\0';
		if (parses(ctx, body, strlen(body), name, err, sizeof(err)) != 0) {
			fprintf(stderr, "FAIL: %s does not parse: %s\n", name, err);
			g_fail = 1;
		}
		*close = saved;
	}
	return blocks;
}

int main(int argc, char **argv)
{
	/*
	 * Every line of JavaScript the dashboard serves. index.html loads
	 * api.js, vt.js and app.js and carries one inline block of its own
	 * (the pre-paint theme apply). api.js is GENERATED by apigen and is
	 * a prerequisite of build/cixd, so by the time selftest runs it
	 * exists -- which is why an absent file here is a failure and not a
	 * note. It was a note once; that made a build which had somehow not
	 * generated it report a pass having checked half the surface.
	 */
	static const char *const files[] = { "web/api.js", "web/vt.js", "web/app.js",
		                             "web/index.html" };
	JSRuntime *rt = JS_NewRuntime();
	JSContext *ctx;
	int i, n;

	if (rt == NULL) {
		fprintf(stderr, "FAIL: could not create a JS runtime\n");
		return 1;
	}
	ctx = JS_NewContext(rt);
	if (ctx == NULL) {
		fprintf(stderr, "FAIL: could not create a JS context\n");
		JS_FreeRuntime(rt);
		return 1;
	}

	self_check(ctx);

	n = argc > 1 ? argc - 1 : (int)(sizeof(files) / sizeof(files[0]));
	for (i = 0; i < n; i++) {
		const char *path = argc > 1 ? argv[i + 1] : files[i];
		size_t len = 0;
		char *src = read_file(path, &len);
		size_t plen = strlen(path);
		int is_html = plen > 5 && strcmp(path + plen - 5, ".html") == 0;

		if (src == NULL) {
			fprintf(stderr, "FAIL: %s could not be read -- every file in this "
			                "list is either tracked or generated before "
			                "selftest runs, so this is not a skip\n",
			        path);
			g_fail = 1;
			continue;
		}
		if (is_html) {
			int blocks = parse_html_scripts(ctx, path, src);

			if (blocks == 0) {
				fprintf(stderr, "FAIL: %s has no inline script block -- this "
				                "gate found one when it was written, so either "
				                "the page changed or the extraction broke\n",
				        path);
				g_fail = 1;
			} else {
				printf("  %-24s %d inline block(s) parse\n", path, blocks);
			}
		} else {
			char err[512];

			if (parses(ctx, src, len, path, err, sizeof(err)) != 0) {
				fprintf(stderr, "FAIL: %s does not parse: %s\n", path, err);
				fprintf(stderr,
				        "      A file that does not parse runs NO script at "
				        "all, so this would ship a blank dashboard, not one "
				        "broken feature.\n");
				g_fail = 1;
			} else {
				printf("  %-24s parses\n", path);
			}
		}
		free(src);
	}

	JS_FreeContext(ctx);
	JS_FreeRuntime(rt);
	if (g_fail) {
		fprintf(stderr, "test_web_syntax: FAILED\n");
		return 1;
	}
	printf("test_web_syntax: the dashboard parses\n");
	return 0;
}
