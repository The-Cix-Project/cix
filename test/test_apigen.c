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

/* The most request schemas this check will hold; the contract names 121
 * today (41 components plus 80 synthesized from inline bodies, #603). */
#define APIGEN_TEST_MAX_REQ 256

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
	 * 1. The real spec. Not a smoke test: the number is cross-checked
	 * against an independent count, so a parser that quietly dropped a
	 * section would show up here rather than as a missing route much
	 * later. (The comment said 263 long after the assertion had moved
	 * past it -- a stale number in a comment about guarding a number.)
	 *
	 * 301 as of #415: POST /pki/export and POST /pki/import.
	 * 303 as of ADR-0283: GET and PUT /system/management-network.
	 * 304 as of ADR-0287: /system/management-network (GET+PUT) removed,
	 *     /system/management-address (GET+PUT+DELETE) added: -2 +3 = +1.
	 * 305 as of the interface flap: POST /system/interfaces/{name}/flap.
	 * 307 as of #469: GET and POST /system/boot-manager.
	 * 309 as of ADR-0292: POST /config and POST /config/diff.
	 * 312 as of #540: GET /system/hostauth/permissions, and PUT and
	 *     DELETE /system/hostauth/permissions/{group}.
	 * 318 as of #543: GET and POST /ldap/users/{name}/app-passwords,
	 *     DELETE .../{app}, and the same three under /whoami/app-passwords.
	 * 319 as of #381: POST /pipeline/revoke.
	 * 320 as of #447: PUT /containers/{name}/sysctls.
	 * 323 as of #535: GET and PUT /images/{name}/policy, GET
	 * /images/{name}/recipe/export.
	 * 325 as of ADR-0323: POST and GET /pkg/recipe-commit.
	 * 329 as of ADR-0324: GET and PUT /pkg/repo-config gone; GET and POST
	 *     /pkg/sources, PUT and DELETE /pkg/sources/{name}, GET and PUT
	 *     /pkg/source-ownership.
	 * 331 as of ADR-0324 step B: GET and PUT /pkg/artifact-config gone; GET
	 *     and POST /pkg/repositories, PUT and DELETE /pkg/repositories/{name}.
	 * 334 as of ADR-0324 step C: GET, PUT and DELETE /system/catalogue-key.
	 * 335 as of ADR-0323's author stage: POST /pkg/recipe-revise.
	 * 337 as of ADR-0323 rung 4: GET and PUT /pkg/trusted-origins.
	 * 339 as of ADR-0323 answer 4: GET /pkg/bad-versions, DELETE
	 *     /pkg/bad-versions/{name}/{version}.
	 * 342 as of ADR-0318's upstream key store: GET and POST
	 *     /pkg/{name}/upstream-keys, DELETE /pkg/{name}/upstream-keys/{fingerprint}.
	 * 339 as of ADR-0328: POST and GET /images/{name}/export and GET
	 *     /images/{name}/export/download gone.
	 * 340 as of #428: GET /system/iso/published, the log of installer
	 *     ISOs this host published -- deliberately its own endpoint and
	 *     not an array on GET /system/iso (ADR-0272's snapshot/log split).
	 */
	status = run_apigen("docs/api/openapi.yaml", NULL, out, sizeof(out));
	if (status != 0)
		fail("apigen rejected the real spec: %.300s", out);
	else if (atoi(out) != 340)
		fail("apigen found %d operations in the real spec, expected 340 -- if the spec "
		     "genuinely changed, update this number deliberately; a silently different "
		     "count is how a lost route hides",
		     atoi(out));

	/* Every operation must carry an operationId and every id must be
	 * unique -- both are what make it usable as a join key. */
	status = run_apigen("docs/api/openapi.yaml", "--list", out, sizeof(out));
	if (status != 0)
		fail("apigen --list rejected the real spec: %.300s", out);

	/*
	 * Every fixture carries a permission vocabulary (ADR-0317), because
	 * apigen refuses a spec without one before reading anything else --
	 * without it each case below would be refused for that instead of
	 * for the fault it exists to test, and pass for the wrong reason.
	 */
#define VOCAB "x-cix-permissions:\n  - public\n  - things:read\n"

	/* 2. A method with no operationId: cannot be dispatched to. */
	expect_refusal("an operation with no operationId",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      summary: no id here\n",
	               "operationId");

	/* 3. A duplicate id: the join key stops being a key. */
	expect_refusal("a duplicate operationId",
	               VOCAB
	               "paths:\n"
	               "  /a:\n"
	               "    get:\n"
	               "      operationId: sameName\n"
	               "      x-cix-permission: public\n"
	               "  /b:\n"
	               "    get:\n"
	               "      operationId: sameName\n"
	               "      x-cix-permission: public\n",
	               "duplicate");

	/* 4. Something that is not an HTTP method directly under a path.
	 * OpenAPI allows path-level `parameters` here; this spec does not
	 * use it, and apigen must stop rather than guess -- if the spec
	 * starts using it, that is a deliberate change to this tool. */
	expect_refusal("a non-method key under a path",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    parameters:\n"
	               "      - name: x\n",
	               "not an HTTP method");

	/* 5. A key under paths: that is not a path at all. */
	expect_refusal("a non-path key under paths:",
	               VOCAB
	               "paths:\n"
	               "  notapath:\n"
	               "    get:\n"
	               "      operationId: x\n",
	               "not a path");

	/* 6. An empty paths section. Emitting an empty table would unroute
	 * the entire API, and a build that succeeds while doing so is worse
	 * than one that stops. */
	expect_refusal("a spec with no operations", VOCAB "paths:\ncomponents:\n  schemas: {}\n",
	               "no operations");

	/*
	 * 7-11. ADR-0317 (#539): the permission annotation. An operation
	 * with no stated permission, or one outside the closed vocabulary,
	 * must not build -- that is what makes "an endpoint with no
	 * authorisation" unrepresentable rather than something to audit.
	 */
	expect_refusal("an operation with no x-cix-permission",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: noPermission\n",
	               "has no x-cix-permission");
	expect_refusal("an operation naming a permission outside the vocabulary",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: unknownPermission\n"
	               "      x-cix-permission: things:write\n",
	               "not in x-cix-permissions");
	expect_refusal("an operation declaring two permissions",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: twoPermissions\n"
	               "      x-cix-permission: public\n"
	               "      x-cix-permission: things:read\n",
	               "twice");
	expect_refusal("a spec with no permission vocabulary",
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: noVocabulary\n"
	               "      x-cix-permission: public\n",
	               "no x-cix-permissions");
	expect_refusal("a malformed vocabulary word",
	               "x-cix-permissions:\n  - Things:Read\n"
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: badWord\n"
	               "      x-cix-permission: Things:Read\n",
	               "is not a permission");
#undef VOCAB

	/*
	 * The CLI header (ADR-0218 layer 1). Its job is that a channel
	 * cannot invent or misspell a path, so what matters is that every
	 * operation is present and every path parameter really became a
	 * %s -- a define that still contained "{name}" would compile
	 * fine at the call site and produce a literal brace in the URL.
	 */
	{
		/*
		 * ADR-0317 (#539): the refusals above prove apigen will not
		 * generate without a permission; this proves the one it did
		 * generate carries each operation's into its route row. A
		 * refusal rule and a missing emit would otherwise pass together
		 * while the dispatcher read NULL from every route.
		 */
		char routes_path[256];
		char buf[1024];
		FILE *f;
		int rows = 0, with_permission = 0;

		snprintf(routes_path, sizeof(routes_path), "/tmp/apigen_routes_%d.h", (int)getpid());
		snprintf(buf, sizeof(buf), "--emit-routes %s", routes_path);
		status = run_apigen("docs/api/openapi.yaml", buf, out, sizeof(out));
		if (status != 0)
			fail("apigen --emit-routes failed: %.200s", out);
		f = fopen(routes_path, "r");
		if (f == NULL) {
			fail("apigen --emit-routes wrote no header");
		} else {
			while (fgets(buf, sizeof(buf), f) != NULL) {
				const char *open;
				size_t n;

				if (strstr(buf, "\t{ \"") != buf || strstr(buf, ", op_") == NULL)
					continue;
				rows++;
				/* A row ends `, "<permission>" },`: cut the `" },` and
				 * the permission is what follows the last quote. */
				n = strlen(buf);
				while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
					buf[--n] = '\0';
				if (n < 4 || strcmp(buf + n - 4, "\" },") != 0)
					continue;
				buf[n - 4] = '\0';
				open = strrchr(buf, '"');
				if (open != NULL && open[1] != '\0')
					with_permission++;
			}
			fclose(f);
			unlink(routes_path);
			if (rows != 340 || with_permission != rows)
				fail("the generated route table has %d rows and %d carry a permission; "
				     "expected 340 and 340 (ADR-0317)",
				     rows, with_permission);
		}
	}

	{
		/*
		 * The generated section table and the generated reconcile
		 * list must agree about which sections are applied element by
		 * element. They come from the same pass over the same schema,
		 * so they "cannot" disagree -- and they did: an edit to the
		 * mode the table emits silently failed to apply, every
		 * non-replace section came out as CONFIG_APPLY_RECONCILE, and
		 * the eleven with no element operations would have reached a
		 * NULL dereference in the daemon. A count nobody checks is a
		 * count that can drift; this is the check.
		 */
		char hdr_path[256];
		char buf[1024];
		FILE *f;
		int mode_reconcile = 0, listed = 0, in_list = 0;

		snprintf(hdr_path, sizeof(hdr_path), "/tmp/apigen_cfg_%d.h", (int)getpid());
		snprintf(buf, sizeof(buf), "--emit-config-sections %s", hdr_path);
		status = run_apigen("docs/api/openapi.yaml", buf, out, sizeof(out));
		if (status != 0)
			fail("apigen --emit-config-sections failed: %.200s", out);
		f = fopen(hdr_path, "r");
		if (f == NULL) {
			fail("apigen --emit-config-sections wrote no header");
		} else {
			while (fgets(buf, sizeof(buf), f) != NULL) {
				if (strstr(buf, "#define CIX_CONFIG_SECTIONS_RECONCILE") != NULL) {
					in_list = 1;
					continue;
				}
				if (in_list) {
					if (strstr(buf, "\tX(") == buf)
						listed++;
					else if (strstr(buf, "X(") == NULL)
						in_list = 0;
					continue;
				}
				if (strstr(buf, "CONFIG_APPLY_RECONCILE") != NULL)
					mode_reconcile++;
			}
			fclose(f);
			unlink(hdr_path);
			if (mode_reconcile != listed)
				fail("%d sections are CONFIG_APPLY_RECONCILE in the section table but "
				     "%d are in CIX_CONFIG_SECTIONS_RECONCILE -- a section applied "
				     "element by element with no element operations is a NULL "
				     "dereference in the daemon",
				     mode_reconcile, listed);
			if (listed == 0)
				fail("no section is applied element by element -- the reconcile list "
				     "is empty, which no schema in this repo should produce");
		}
	}

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
		if (defines != 340)
			fail("CLI header has %d path defines, expected one per operation (340)", defines);
		if (methods != 340)
			fail("CLI header has %d method defines, expected one per operation (340)", methods);
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

	/*
	 * ADR-0338 (#594): the dashboard's generated structure.
	 *
	 * Three assertions, and the third is the one that matters most.
	 * Counting shapes catches an operation the emitter skipped;
	 * finding one known field catches a constraint silently dropped.
	 * But an `enum` in this spec is an inline BARE list -- `enum: [ok,
	 * failed]` -- and writing it through verbatim produces a JS array
	 * of undefined identifiers, which is a ReferenceError at load:
	 * not one broken field, the whole dashboard. So every emitted
	 * option must be quoted, and that is checked rather than trusted,
	 * because the first draft of the emitter did exactly this.
	 */
	{
		char js_path[256];
		/*
		 * Wide enough for the longest line this emitter produces, and
		 * checked below rather than assumed.
		 *
		 * This was 1024, and once descriptions arrived (#594) 16 of
		 * 827 field lines were longer than that -- so every assertion
		 * here was silently reading half a line, and a pair of strstr()
		 * calls that must both land on one line could be split across
		 * two reads. Measured 2026-10-10: longest line 2,157
		 * characters, longest assertion-bearing line 761.
		 */
		char buf[16384];
		FILE *f;
		int long_line = 0, odd_quotes = 0, odd_quote_line = 0, lno = 0;
		int shapes = 0, saw_fields = 0, saw_prefix = 0, bare_option = 0, options = 0;
		int saw_spaced = 0, saw_spanned_enum = 0, saw_spanned_required = 0;
		int in_storage_device = 0, bad_type = 0;
		int vmode_pattern = 0, vmode_line = 0;
		char vmode_text[160] = "";
		char bad_type_text[80] = "";
		const char *tp;
		char *dsc;
		int desc_lines = 0, desc_not_last = 0;

		snprintf(js_path, sizeof(js_path), "/tmp/apigen_web_%d.js", (int)getpid());
		snprintf(buf, sizeof(buf), "--emit-web %s", js_path);
		status = run_apigen("docs/api/openapi.yaml", buf, out, sizeof(out));
		if (status != 0)
			fail("apigen --emit-web failed: %.200s", out);
		f = fopen(js_path, "r");
		if (f == NULL) {
			fail("apigen --emit-web wrote nothing");
		} else {
			while (fgets(buf, sizeof(buf), f) != NULL) {
				const char *op;
				const char *q;
				size_t len = strlen(buf);
				int quotes = 0;

				lno++;

				/*
				 * Two checks on the line ITSELF, before anything looks
				 * at what is in it.
				 *
				 * The first is honesty about this reader: a line that
				 * did not fit arrived as a fragment, and every
				 * assertion below would then be testing half a line --
				 * which is a passing test that proves nothing, the
				 * worst of the three outcomes.
				 *
				 * The second is the whole failure CLASS the two
				 * specific assertions below are instances of. An odd
				 * number of unescaped quotes on a line means a string
				 * literal that does not close, which is how both
				 * api.js defects presented: `expecting '}'` from
				 * test_web_syntax on the box, with no indication of
				 * where. This names the line, and it runs wherever
				 * this test runs rather than only where quickjs is.
				 */
				if (len > 0 && buf[len - 1] != '\n' && !feof(f))
					long_line = 1;
				for (q = buf; *q != '\0'; q++) {
					if (*q == '\\') {
						if (q[1] != '\0')
							q++;
						continue;
					}
					if (*q == '"')
						quotes++;
				}
				if (quotes % 2 != 0 && odd_quotes++ == 0)
					odd_quote_line = lno;
				/*
				 * Where this line's prose begins, so every check below
				 * can match STRUCTURE and not English.
				 *
				 * Since #594 a field carries its contract description,
				 * which is free text about this very API -- a sentence
				 * may well contain `options: [`, `minimum: 8` or
				 * `required: true`. apigen emits description LAST for
				 * exactly this reason, and the assertion after the
				 * loop proves it still does, so "before dsc" is an
				 * exact anchor rather than a hopeful one.
				 */
				dsc = strstr(buf, ", description: ");
				if (dsc != NULL) {
					desc_lines++;
					if (strstr(dsc, "\" },\n") == NULL &&
					    strstr(dsc, "\" },") == NULL)
						desc_not_last++;
					/* Cut the prose off. Everything below this point
					 * then matches structure only, which is exact
					 * rather than nearly-always-right -- and nothing
					 * after the loop reads the description text. */
					*dsc = '\0';
				}
				/*
				 * A HYPHEN AT THE END OF A CHARACTER CLASS, which a
				 * browser answers by IGNORING THE WHOLE PATTERN.
				 *
				 * Measured in Chromium on 2026-10-10: HTML compiles an
				 * input's `pattern` with the regex `v` flag, under
				 * which `[A-Za-z0-9_-]` is a SyntaxError -- and the
				 * HTML spec's answer to an uncompilable pattern is to
				 * skip the constraint entirely. So `checkValidity()`
				 * returned true for `db_1.arpa` against a pattern
				 * that excludes `_`, while a control pattern
				 * (`^[a-z]+$` vs `ABC`) correctly returned false.
				 *
				 * 27 of this contract's 29 patterns were written that
				 * way, which is every name field -- so the one thing
				 * moving a constraint into the schema was supposed to
				 * buy, a form that refuses what the daemon refuses,
				 * was silently not happening. `\-` compiles under
				 * `u`, under `v` and under no flag, and matches
				 * identically.
				 *
				 * Textual on purpose: this gate cannot compile a
				 * regex, and it does not need to -- the defect has an
				 * exact spelling.
				 */
				if ((tp = strstr(buf, "pattern: \"")) != NULL) {
					const char *q;

					for (q = tp + 10; *q != '\0' && *q != '"'; q++)
						if (*q == ']' && q > tp + 10 && q[-1] == '-' &&
						    (q - 2 < tp + 10 || q[-2] != '\\')) {
							if (vmode_pattern++ == 0) {
								vmode_line = lno;
								snprintf(vmode_text, sizeof(vmode_text),
								         "%.150s", tp);
							}
							break;
						}
				}
				if (strstr(buf, "_SHAPE: {") != NULL)
					shapes++;
				if (strncmp(buf, "\tFIELDS: {", 10) == 0)
					saw_fields = 1;
				/* NetworkCreateRequest's prefix_len carries
				 * minimum: 8 / maximum: 30 in the contract, and the
				 * hand-written form carried neither. */
				if (strstr(buf, "name: \"prefix_len\"") != NULL &&
				    strstr(buf, "minimum: 8") != NULL &&
				    strstr(buf, "maximum: 30") != NULL)
					saw_prefix = 1;
				if (strstr(buf, "options: [\"now\", \"on next start\"]") != NULL)
					saw_spaced = 1;
				/* #602: the last value of a flow list written across
				 * two lines, and the last name of one written across
				 * three. Both are the END of their list, which is
				 * exactly what a reader that stops at the first
				 * physical line drops.
				 *
				 * The required half is anchored to StorageDevice's own
				 * block rather than to the field name alone: a bare
				 * `parent_disk` + `required: true` match would be
				 * satisfied by any schema that happens to carry that
				 * field, which is a weaker claim than the one this
				 * asserts. */
				if (strstr(buf, "\t\tStorageDevice: [") != NULL)
					in_storage_device = 1;
				else if (in_storage_device && strstr(buf, "\t\t],") != NULL)
					in_storage_device = 0;
				if (strstr(buf, "name: \"stage\"") != NULL &&
				    strstr(buf, "\"assemble\", \"stage\", \"verify\"]") != NULL)
					saw_spanned_enum = 1;
				if (in_storage_device && strstr(buf, "name: \"parent_disk\"") != NULL &&
				    strstr(buf, "required: true") != NULL)
					saw_spanned_required = 1;
				op = strstr(buf, "options: [");
				if (op != NULL) {
					options++;
					if (op[10] != '"' && op[10] != ']')
						bare_option++;
				}
				/*
				 * Every type must be one of JSON Schema's own, and
				 * this is the assertion that was missing (#594).
				 *
				 * `type: [string, "null"]` -- a nullable type, which
				 * this contract writes 30 times -- was read by a
				 * scalar reader and truncated at 16 characters to
				 * `[string, "null`, then emitted verbatim inside
				 * quotes: `type: "[string, "null""`. JS reads that as
				 * a string, a bare `null`, and an unterminated string,
				 * so the whole file failed to parse and the dashboard
				 * ran no script at all. test_web_syntax caught it
				 * twice, on the box, a release apart; nothing here
				 * did, because every assertion in this block tested
				 * PRESENCE. A closed set of legal values is the cheap
				 * check that fails in the right place.
				 */
				tp = strstr(buf, " type: \"");
				if (tp != NULL) {
					const char *const ok[] = { "string", "integer", "boolean",
						                       "array",  "object",  "number" };
					size_t k;
					int good = 0;

					tp += 8;
					for (k = 0; k < sizeof(ok) / sizeof(ok[0]); k++) {
						size_t l = strlen(ok[k]);

						if (strncmp(tp, ok[k], l) == 0 && tp[l] == '"')
							good = 1;
					}
					if (!good) {
						bad_type++;
						if (bad_type == 1)
							snprintf(bad_type_text, sizeof(bad_type_text), "%.60s",
							         tp - 8);
					}
				}
			}
			fclose(f);
			unlink(js_path);
		}
		if (shapes != 340)
			fail("web api.js has %d _SHAPE entries, expected one per operation (340)",
			     shapes);
		if (!saw_fields)
			fail("web api.js has no FIELDS table -- the renderers have nothing to render "
			     "from, so every form would go back to being hand-written");
		if (!saw_prefix)
			fail("NetworkCreateRequest.prefix_len is emitted without its minimum/maximum -- "
			     "the constraint the daemon enforces did not reach the form, which is the "
			     "whole point of generating it");
		if (options == 0)
			fail("no enum reached api.js at all, though the contract declares 107 -- the "
			     "options reader is not running");
		if (bare_option != 0)
			fail("%d emitted enum(s) start with an unquoted value -- a YAML inline list "
			     "written through verbatim is a JS array of undefined identifiers, which "
			     "is a ReferenceError at load",
			     bare_option);
		/*
		 * The case that actually broke, and the reason this block is
		 * not the real gate.
		 *
		 * An enum value here may contain SPACES and may be quoted in
		 * the YAML, both forms in one spec: `[now, on next start]` and
		 * `[now, "on next start"]`. The first truncates to "on" if the
		 * reader stops at a space -- a select offering a value the
		 * daemon never accepts -- and the second yields a lone double
		 * quote that the JS writer escapes into an UNTERMINATED
		 * string, which swallowed 974 lines of api.js and made
		 * test_web_syntax refuse the whole file.
		 *
		 * Every check above passed while that was true, because they
		 * assert PRESENCE and the defect was PARSEABILITY. The real
		 * gate is test_web_syntax, which parses the file with a real
		 * engine and caught this; this assertion is the specific-case
		 * belt, so the truncating variant cannot come back quietly.
		 */
		if (!saw_spaced)
			fail("an enum value containing spaces did not survive into api.js -- expected "
			     "options: [\"now\", \"on next start\"], which is the pair of YAML forms "
			     "that broke this emitter once");
		/*
		 * #602: a YAML flow list may span physical lines, and this
		 * tool is a line-oriented scanner, so such a list used to be
		 * read as a list that ended at the first line -- silently, and
		 * only at its END, which is the half nothing looks at.
		 *
		 * Both of these are measured positions rather than invented
		 * ones: SourceCatalogueEntry.stage spans two lines and was
		 * emitting 7 of its 15 values, and StorageDevice's `required`
		 * spans three and was emitting 8 of its 17 names -- which
		 * api_shapes.h hands to the contract-vs-daemon gate, so nine
		 * fields of every storage response went unchecked. Asserting
		 * the LAST entry of each is what distinguishes a joined list
		 * from a truncated one; a count would also pass on a list
		 * truncated somewhere else.
		 */
		if (!saw_spanned_enum)
			fail("SourceCatalogueEntry.stage lost the tail of its enum -- its 15 values "
			     "are written across two lines in the contract, and \"verify\" is the "
			     "last of them");
		if (!saw_spanned_required)
			fail("StorageDevice lost the tail of its required list -- its 17 names are "
			     "written across three lines in the contract, and parent_disk is the "
			     "last of them");
		if (bad_type > 0)
			fail("%d field(s) in api.js carry a type that is not one of JSON Schema's "
			     "six -- the first is `%s`. A nullable `type: [string, \"null\"]` read "
			     "as a scalar lands here as a broken string literal, which costs the "
			     "whole file rather than one field",
			     bad_type, bad_type_text);
		if (vmode_pattern > 0)
			fail("%d emitted pattern(s) end a character class with a bare `-`, the first "
			     "at line %d: %s\n"
			     "      A browser compiles an <input pattern> with the regex `v` flag, "
			     "under which that is a SyntaxError -- and the HTML spec's answer to an "
			     "uncompilable pattern is to SKIP THE CONSTRAINT, silently. The form then "
			     "accepts everything, which is indistinguishable from a pattern that "
			     "passes. Write the hyphen as `\\-`: it compiles under `u`, under `v` and "
			     "under no flag, and matches identically. A LEADING `-` in the class is "
			     "not the fix -- that throws under `v` too.",
			     vmode_pattern, vmode_line, vmode_text);
		if (odd_quotes > 0)
			fail("%d line(s) of api.js carry an odd number of unescaped quotes, the first "
			     "at line %d -- a string literal that does not close, which is a "
			     "SyntaxError for the WHOLE file and therefore a blank dashboard",
			     odd_quotes, odd_quote_line);
		if (long_line)
			fail("a line of api.js is longer than this test's read buffer, so every "
			     "assertion above was reading a fragment -- raise it rather than let "
			     "these checks pass on half a line");
		/*
		 * The invariant the anchoring rests on: a field's description is
		 * the LAST thing in its object, so cutting the line there leaves
		 * exactly the structure. If a future key is emitted after it,
		 * every text match above silently starts searching English.
		 */
		if (desc_lines == 0)
			fail("no field in api.js carries a description -- the contract has 1,450 of "
			     "them at property depth and they are the authored meaning a generated "
			     "form renders as its hint (ADR-0338)");
		if (desc_not_last > 0)
			fail("%d field line(s) emit something AFTER the description, so it is no "
			     "longer last in the object -- every text match in this block then "
			     "searches free English, where `options: [` and `required: true` are "
			     "things a sentence about this API can say",
			     desc_not_last);
	}

	/*
	 * 7. Every request schema a shape NAMES must exist in FIELDS (#603).
	 *
	 * `FIELDS` omits a schema with no fields, so a `request:` naming
	 * one hands a renderer a key that resolves to undefined -- which
	 * fails at render time, where `request: null` would correctly have
	 * fallen back to an authored form. That is strictly worse than no
	 * schema, and the generator really did produce it while #603 was
	 * being written: `attachContainerNetwork_SHAPE` named
	 * `attachContainerNetworkRequest`, which appeared exactly once in
	 * the whole file, because its body is a `oneOf` that no generated
	 * form can render and so no fields were ever recorded under it.
	 * Three bodies in this contract are of that kind -- that `oneOf`,
	 * and two free-form objects (`type: object` with no properties, and
	 * one with `additionalProperties`) which are maps rather than
	 * forms.
	 *
	 * A dangling reference is exactly the kind of defect that passes
	 * every presence check, so it gets a check of its own.
	 */
	{
		char js_path[256];
		char buf[16384];
		char names[APIGEN_TEST_MAX_REQ][96];
		char have[APIGEN_TEST_MAX_REQ][96];
		FILE *f;
		int n_names = 0, n_have = 0, i, dangling = 0;
		char first[96] = "";

		snprintf(js_path, sizeof(js_path), "/tmp/apigen_req_%d.js", (int)getpid());
		snprintf(buf, sizeof(buf), "--emit-web %s", js_path);
		status = run_apigen("docs/api/openapi.yaml", buf, out, sizeof(out));
		if (status != 0)
			fail("apigen --emit-web failed: %.200s", out);
		f = fopen(js_path, "r");
		if (f == NULL) {
			fail("apigen --emit-web wrote nothing");
		} else {
			/*
			 * One pass, collecting both sides: every name a shape's
			 * `request:` gives, and every schema that opens a FIELDS
			 * block. A pass per name would be 121 reads of a 323 KB
			 * file to answer a question two lists already answer.
			 */
			while (fgets(buf, sizeof(buf), f) != NULL) {
				const char *r;

				if (strncmp(buf, "\t\t", 2) == 0 && buf[2] != '\t' &&
				    strstr(buf, ": [") != NULL) {
					if (n_have < APIGEN_TEST_MAX_REQ) {
						size_t k = 0;

						while (buf[2 + k] != ':' && buf[2 + k] != '\0' &&
						       k + 1 < sizeof(have[0]))
							k++;
						memcpy(have[n_have], buf + 2, k);
						have[n_have][k] = '\0';
						n_have++;
					}
					continue;
				}
				if (strstr(buf, "_SHAPE: {") == NULL)
					continue;
				r = strstr(buf, ", request: \"");
				if (r == NULL)
					continue;
				r += 12;
				if (n_names < APIGEN_TEST_MAX_REQ) {
					size_t k = 0;

					while (r[k] != '"' && r[k] != '\0' &&
					       k + 1 < sizeof(names[0]))
						k++;
					memcpy(names[n_names], r, k);
					names[n_names][k] = '\0';
					n_names++;
				}
			}
			for (i = 0; i < n_names; i++) {
				int j, found = 0;

				for (j = 0; j < n_have; j++)
					if (strcmp(names[i], have[j]) == 0)
						found = 1;
				if (!found && dangling++ == 0)
					snprintf(first, sizeof(first), "%s", names[i]);
			}
			fclose(f);
			unlink(js_path);
		}
		if (n_names == 0)
			fail("no operation in api.js names a request schema -- 124 of the "
			     "contract's 340 carry a requestBody, so this is a reader that "
			     "resolved none of them");
		if (dangling > 0)
			fail("%d operation(s) name a request schema with no FIELDS entry, the first "
			     "`%s` -- a renderer resolves that to undefined and fails at render "
			     "time, where request: null would have fallen back correctly",
			     dangling, first);
	}

	if (failures == 0)
		printf("APIGEN RESULT: PASS\n");
	else
		printf("APIGEN RESULT: FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
