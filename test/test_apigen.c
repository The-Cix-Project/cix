/*
 * test_apigen -- proves the route extractor (ADR-0218) both reads the
 * real spec correctly AND refuses malformed input.
 *
 * The second half is the point. A generator that silently mis-parses is
 * a tool confidently wrong about its own subject, and everything
 * downstream -- the daemon's dispatcher, every channel's entry points --
 * trusts its output completely. So each way it could quietly lose a
 * route is exercised here as a case it must REJECT, with a message that
 * names the problem.
 *
 * This project has learned the same lesson three times in one session
 * from gates that could never pass (m4's spawn.h, glibc's -B CRT check,
 * glibc's symlink sweep): a check is only worth something once it has
 * been run against a case where it should pass and a case where it
 * should fail. This test does both before anything depends on apigen.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void fail(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "FAIL: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	failures++;
}

/* Runs apigen, capturing stdout+stderr. Returns the exit status. */
static int run_apigen(const char *spec_path, const char *extra, char *out, size_t out_size)
{
	char cmd[1024];
	FILE *p;
	size_t n = 0;

	snprintf(cmd, sizeof(cmd), "./build/apigen '%s' %s 2>&1", spec_path,
	         extra != NULL ? extra : "");
	p = popen(cmd, "r");
	if (p == NULL)
		return -1;
	while (n + 1 < out_size) {
		size_t got = fread(out + n, 1, out_size - n - 1, p);

		if (got == 0)
			break;
		n += got;
	}
	out[n] = '\0';
	return pclose(p);
}

/* Writes a minimal spec and returns its path in `path`. */
static int write_spec(char *path, size_t path_size, const char *body)
{
	FILE *f;

	snprintf(path, path_size, "/tmp/apigen_test_%d_%p.yaml", (int)getpid(), (void *)body);
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fputs(body, f);
	fclose(f);
	return 0;
}

/* A malformed spec must be REFUSED, and the refusal must name the cause
 * -- "it failed" is not enough for anyone to act on. */
static void expect_refusal(const char *what, const char *body, const char *must_mention)
{
	char path[256];
	char out[4096];
	int status;

	if (write_spec(path, sizeof(path), body) != 0) {
		fail("could not write the %s fixture", what);
		return;
	}
	status = run_apigen(path, NULL, out, sizeof(out));
	unlink(path);
	if (status == 0) {
		fail("%s was ACCEPTED -- apigen must refuse what it cannot parse, never skip it "
		     "(output: %.200s)",
		     what, out);
		return;
	}
	if (strstr(out, must_mention) == NULL)
		fail("%s was refused but the message does not mention \"%s\": %.300s", what,
		     must_mention, out);
}

/*
 * The contract must be valid YAML, and apigen is not the judge of that.
 *
 * apigen has its own parser, which tolerated two descriptions carrying
 * an unquoted colon -- `description: The daemon's own settings: bind
 * address...` -- for as long as they existed. Every standard YAML
 * reader rejects that file outright, so the contract was unparseable by
 * anything but us and nobody noticed, because the only thing that read
 * it was the tool that did not mind.
 *
 * A full YAML parser in C is not the answer to that. This checks the
 * one construct that actually broke it: a plain (unquoted) scalar
 * containing ": ", which YAML reads as a nested mapping. Cheap,
 * targeted, and it fails on the line that would fail elsewhere.
 */
static void check_scalars_are_quoted(void)
{
	FILE *f = fopen("docs/api/openapi.yaml", "r");
	char line[4096];
	int lineno = 0;
	int indent = 0;
	int block_indent = -1; /* inside a > or | block opened at this indent */

	if (f == NULL) {
		fail("could not open the spec to check its scalars");
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p = line;
		const char *colon;
		const char *val;

		lineno++;
		while (*p == ' ' || *p == '\t')
			p++;
		indent = (int)(p - line);
		if (*p == '\0' || *p == '\n')
			continue;
		/*
		 * Text inside a folded (>) or literal (|) block is prose, not
		 * a mapping, and colons in it are ordinary punctuation. This
		 * file is mostly such blocks, so failing to track them makes
		 * the check useless -- it flagged an error message quoted in a
		 * description on its first run.
		 */
		if (block_indent >= 0) {
			if (indent > block_indent)
				continue;
			block_indent = -1;
		}
		/* Only "key: value" lines; a list item or a comment is not one. */
		if (*p == '#' || *p == '-')
			continue;
		colon = strstr(p, ": ");
		if (colon == NULL)
			continue;
		val = colon + 2;
		while (*val == ' ')
			val++;
		/* Quoted, folded, literal or empty values are all fine -- the
		 * only hazard is a PLAIN scalar with a colon-space in it. */
		if (*val == '>' || *val == '|') {
			block_indent = indent;
			continue;
		}
		if (*val == '"' || *val == '\'' || *val == '\0' || *val == '\n' || *val == '[' ||
		    *val == '{' || *val == '&' || *val == '*')
			continue;
		if (strstr(val, ": ") != NULL)
			fail("openapi.yaml:%d has an unquoted scalar containing \": \", which no standard "
			     "YAML reader will parse -- quote it: %.80s",
			     lineno, p);
	}
	fclose(f);
}

int main(void)
{
	char out[65536];
	int status;

	check_scalars_are_quoted();

	/*
	 * 1. The real spec. Not a smoke test: 263 is cross-checked against
	 * an independent count, so a parser that quietly dropped a section
	 * would show up here rather than as a missing route much later.
	 */
	status = run_apigen("docs/api/openapi.yaml", NULL, out, sizeof(out));
	if (status != 0)
		fail("apigen rejected the real spec: %.300s", out);
	else if (atoi(out) != 276)
		fail("apigen found %d operations in the real spec, expected 276 -- if the spec "
		     "genuinely changed, update this number deliberately; a silently different "
		     "count is how a lost route hides",
		     atoi(out));

	/* Every operation must carry an operationId and every id must be
	 * unique -- both are what make it usable as a join key. */
	status = run_apigen("docs/api/openapi.yaml", "--list", out, sizeof(out));
	if (status != 0)
		fail("apigen --list rejected the real spec: %.300s", out);

	/* 2. A method with no operationId: cannot be dispatched to. */
	expect_refusal("an operation with no operationId",
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      summary: no id here\n",
	               "operationId");

	/* 3. A duplicate id: the join key stops being a key. */
	expect_refusal("a duplicate operationId",
	               "paths:\n"
	               "  /a:\n"
	               "    get:\n"
	               "      operationId: sameName\n"
	               "  /b:\n"
	               "    get:\n"
	               "      operationId: sameName\n",
	               "duplicate");

	/* 4. Something that is not an HTTP method directly under a path.
	 * OpenAPI allows path-level `parameters` here; this spec does not
	 * use it, and apigen must stop rather than guess -- if the spec
	 * starts using it, that is a deliberate change to this tool. */
	expect_refusal("a non-method key under a path",
	               "paths:\n"
	               "  /thing:\n"
	               "    parameters:\n"
	               "      - name: x\n",
	               "not an HTTP method");

	/* 5. A key under paths: that is not a path at all. */
	expect_refusal("a non-path key under paths:",
	               "paths:\n"
	               "  notapath:\n"
	               "    get:\n"
	               "      operationId: x\n",
	               "not a path");

	/* 6. An empty paths section. Emitting an empty table would unroute
	 * the entire API, and a build that succeeds while doing so is worse
	 * than one that stops. */
	expect_refusal("a spec with no operations", "paths:\ncomponents:\n  schemas: {}\n",
	               "no operations");

	/*
	 * The CLI header (ADR-0218 layer 1). Its job is that a channel
	 * cannot invent or misspell a path, so what matters is that every
	 * operation is present and every path parameter really became a
	 * %s -- a define that still contained "{name}" would compile
	 * fine at the call site and produce a literal brace in the URL.
	 */
	{
		char hdr_path[256];
		char buf[512];
		FILE *f;
		int defines = 0, methods = 0, braces = 0;
		int saw_health = 0, saw_one_param = 0, saw_two_param = 0;

		snprintf(hdr_path, sizeof(hdr_path), "/tmp/apigen_cli_%d.h", (int)getpid());
		snprintf(buf, sizeof(buf), "--emit-cli %s", hdr_path);
		status = run_apigen("docs/api/openapi.yaml", buf, out, sizeof(out));
		if (status != 0)
			fail("apigen --emit-cli failed: %.200s", out);
		f = fopen(hdr_path, "r");
		if (f == NULL) {
			fail("apigen --emit-cli wrote no header");
		} else {
			while (fgets(buf, sizeof(buf), f) != NULL) {
				if (strncmp(buf, "#define CIX_API_", 16) != 0)
					continue;
				if (strstr(buf, "_METHOD ") != NULL) {
					methods++;
					continue;
				}
				defines++;
				if (strchr(buf, '{') != NULL)
					braces++;
				if (strcmp(buf, "#define CIX_API_getHealth \"/v1/health\"\n") == 0)
					saw_health = 1;
				if (strcmp(buf, "#define CIX_API_getContainer \"/v1/containers/%s\"\n") == 0)
					saw_one_param = 1;
				if (strcmp(buf,
				           "#define CIX_API_detachContainerNetwork "
				           "\"/v1/containers/%s/networks/%s\"\n") == 0)
					saw_two_param = 1;
			}
			fclose(f);
			unlink(hdr_path);
		}
		if (defines != 276)
			fail("CLI header has %d path defines, expected one per operation (276)", defines);
		if (methods != 276)
			fail("CLI header has %d method defines, expected one per operation (276)", methods);
		if (braces != 0)
			fail("%d CLI path define(s) still contain '{' -- a parameter was not converted "
			     "to %%s and would put a literal brace in the URL",
			     braces);
		if (!saw_health)
			fail("a no-parameter path is not emitted verbatim");
		if (!saw_one_param)
			fail("a one-parameter path does not become a single %%s");
		if (!saw_two_param)
			fail("a two-parameter path does not become two %%s in order");
	}

	if (failures == 0)
		printf("APIGEN RESULT: PASS\n");
	else
		printf("APIGEN RESULT: FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
