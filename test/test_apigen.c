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

/* The most FIELDS schemas (and request names) this check will hold. It
 * was 256 while the contract had 121 request schemas (41 components
 * plus 80 synthesized from inline bodies, #603) -- but the list it
 * caps is EVERY FIELDS block, and 0.2.57-512's 132 synthesized `<op>Args`
 * schemas took it past 256 and 513's four more were then cut off and read
 * as missing. 392 on 2026-10-17, so 1024; and reaching it now fails. */
#define APIGEN_TEST_MAX_REQ 1024

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

/*
 * AN INSTANT DECLARES ITSELF, so a reader does not have to recognise
 * the name.
 *
 * `format: unix-seconds` on 35 integer properties is what lets the
 * dashboard render a moment from the contract instead of from twenty
 * hand-rolled `new Date(x * 1000)` call sites. The convention is only
 * worth having if the NEXT instant carries it too, and nothing fails
 * when a convention is merely forgotten -- so a property whose name
 * says it is a moment must say so in its format.
 *
 * Checked on the NAME, which is the one signal available: `_at`,
 * `_until` and `_unixtime`. That is Stripe's convention too (the `_at`
 * suffix is documented there as meaning a Unix timestamp), and it is
 * exactly the signal this change exists to stop relying on -- a gate
 * may read a name, a renderer may not.
 *
 * A STRING property is skipped rather than refused: `created_at` with
 * `format: date-time` is an RFC 3339 string and perfectly correct.
 * This is about integers pretending to be plain numbers.
 */
static void check_instants_declare_their_format(void)
{
	FILE *f = fopen("docs/api/openapi.yaml", "r");
	char line[4096];
	char name[256] = "";
	int lineno = 0, name_line = 0;
	int want = 0, checked = 0;

	if (f == NULL) {
		fail("could not open the spec to check its instants");
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p = line;
		size_t n;

		lineno++;
		while (*p == ' ' || *p == '\t')
			p++;
		/*
		 * The format may sit anywhere in the property's own block --
		 * `description` legitimately comes first in places -- so what
		 * ends the search is the NEXT property, not the next line.
		 */
		if (want && strncmp(p, "format:", 7) == 0) {
			if (strstr(p, "unix-seconds") == NULL)
				fail("openapi.yaml:%d: %s is an integer instant declaring "
				     "format: %.40s -- an instant is format: unix-seconds, "
				     "which is what lets the dashboard render it from the "
				     "contract rather than from its own new Date(x * 1000)",
				     name_line, name, p + 8);
			checked++;
			want = 0;
			continue;
		}
		/* `  <name>:` with nothing after the colon opens a property. */
		n = strcspn(p, ":");
		if (p[n] == ':' && (p[n + 1] == '\n' || p[n + 1] == '\0') && n > 0 &&
		    n + 1 < sizeof(name)) {
			if (want) {
				fail("openapi.yaml:%d: %s is an integer whose name says it is a "
				     "moment, and its block declares no format -- add "
				     "`format: unix-seconds`, or the dashboard renders it as a "
				     "bare number",
				     name_line, name);
				want = 0;
			}
			memcpy(name, p, n);
			name[n] = '\0';
			name_line = lineno;
			continue;
		}
		if (strncmp(p, "type: integer", 13) == 0 ||
		    strncmp(p, "type: [integer", 14) == 0) {
			size_t ln = strlen(name);

			if ((ln > 3 && strcmp(name + ln - 3, "_at") == 0) ||
			    (ln > 6 && strcmp(name + ln - 6, "_until") == 0) ||
			    (ln > 9 && strcmp(name + ln - 9, "_unixtime") == 0))
				want = 1;
		}
	}
	if (want)
		fail("openapi.yaml:%d: %s is an integer instant at the end of the file with no "
		     "format declared", name_line, name);
	fclose(f);
	if (checked < 15)
		/* The floor is 15 against 29 such properties, measured
		 * 2026-10-16 -- about half, so a contract that legitimately
		 * shrinks does not trip it and a reader that silently matches
		 * nothing does. */
		fail("only %d integer instants were checked for format: unix-seconds -- the "
		     "floor is 15 and the contract has about twice that named _at, _until "
		     "or _unixtime, so this gate is not reading what it thinks it is",
		     checked);
}

/*
 * AN allOf SCHEMA CARRIES ITS BASE'S FIELDS AND ITS OWN (#592).
 *
 * Until apigen read `allOf`, the eight schemas in this contract that
 * compose another emitted no fields at all -- their own properties sit
 * four columns deeper than a plain schema's, under a list item -- so
 * nothing generated could render a schedule, a pipeline row or a
 * recipe detail. Asserted on the real contract rather than a fixture,
 * on the schema that motivated it: `Schedule` must carry `action` (from
 * ScheduleWrite, its base) BEFORE `next_run_at` (its own, an instant),
 * which is base-first order and both halves present at once.
 */
static void check_allof_is_merged(void)
{
	char js[256];
	char out[4096];
	char extra[320];
	FILE *f;
	static char buf[4 * 1024 * 1024];
	size_t n;
	const char *s, *e, *a, *nr;

	snprintf(js, sizeof(js), "/tmp/apigen_allof_%d.js", (int)getpid());
	snprintf(extra, sizeof(extra), "--emit-web %s", js);
	if (run_apigen("docs/api/openapi.yaml", extra, out, sizeof(out)) != 0) {
		fail("apigen --emit-web failed for the allOf check: %.200s", out);
		return;
	}
	f = fopen(js, "r");
	if (f == NULL) {
		fail("apigen --emit-web wrote nothing for the allOf check");
		return;
	}
	n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = '\0';
	fclose(f);
	unlink(js);
	s = strstr(buf, "\n\t\tSchedule: [");
	e = s != NULL ? strstr(s + 1, "\n\t\t]") : NULL;
	if (s == NULL || e == NULL) {
		fail("the emitted fields have no Schedule block -- an allOf schema is "
		     "emitting nothing again");
		return;
	}
	a = strstr(s, "name: \"action\"");
	nr = strstr(s, "name: \"next_run_at\"");
	if (a == NULL || a > e)
		fail("Schedule's fields lack `action`, which comes from its allOf base "
		     "ScheduleWrite -- the $ref member was not merged");
	if (nr == NULL || nr > e)
		fail("Schedule's fields lack `next_run_at`, its own property -- the allOf "
		     "inline member was not read");
	if (a != NULL && nr != NULL && a > nr)
		fail("Schedule lists its own fields before its base's -- an allOf merges "
		     "the referenced schema first");
	if (nr != NULL && nr < e) {
		const char *u = strstr(nr, "unix-seconds");
		const char *eol = strchr(nr, '\n');

		/* On next_run_at's own line, and NULL counts as missing --
		 * `strstr() > e` alone would pass when it is absent. */
		if (u == NULL || (eol != NULL && u > eol))
			fail("Schedule's next_run_at lost its format: unix-seconds in the merge");
	}
}

int main(void)
{
	char out[65536];
	int status;

	check_scalars_are_quoted();
	check_instants_declare_their_format();
	check_allof_is_merged();

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
	 * 341 as of ADR-0339: GET /events/status, the box's boot epoch and the
	 *     generation of every read it announces.
	 * 342 as of ADR-0339 step three: GET /events, the WebSocket that
	 *     carries those generations live.
	 * 343 as of ADR-0339 step four: POST /events/verify, the daemon's check
	 *     of its own announcements, on demand.
	 */
	status = run_apigen("docs/api/openapi.yaml", NULL, out, sizeof(out));
	if (status != 0)
		fail("apigen rejected the real spec: %.300s", out);
	else if (atoi(out) != 343)
		fail("apigen found %d operations in the real spec, expected 343 -- if the spec "
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

	/*
	 * 5b. A path parameter must be the declared one, in the path's own
	 * order (#592). A form now asks for each one the caller does not
	 * supply and puts it in the URL by position, so an undeclared
	 * parameter would be a field nobody described and a swapped pair
	 * would put one value where the other belongs.
	 */
	expect_refusal("an undeclared path parameter",
	               VOCAB
	               "paths:\n"
	               "  /thing/{name}:\n"
	               "    get:\n"
	               "      operationId: getThing\n"
	               "      x-cix-permission: public\n",
	               "not the declared parameter");
	expect_refusal("path parameters declared out of the path's order",
	               VOCAB
	               "paths:\n"
	               "  /thing/{a}/{b}:\n"
	               "    get:\n"
	               "      operationId: getThing\n"
	               "      x-cix-permission: public\n"
	               "      parameters:\n"
	               "        - name: b\n"
	               "          in: path\n"
	               "          required: true\n"
	               "          schema:\n"
	               "            type: string\n"
	               "        - name: a\n"
	               "          in: path\n"
	               "          required: true\n"
	               "          schema:\n"
	               "            type: string\n",
	               "not the declared parameter");

	/*
	 * 5c. A list whose items are a `oneOf` naming TWO schemas (#592).
	 * A form draws a list's rows from the one schema its items name;
	 * with two, which row to draw would be a guess.
	 */
	expect_refusal("an items oneOf naming two schemas",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: getThing\n"
	               "      x-cix-permission: public\n"
	               "components:\n"
	               "  schemas:\n"
	               "    A:\n"
	               "      type: object\n"
	               "      properties:\n"
	               "        x:\n"
	               "          type: string\n"
	               "    B:\n"
	               "      type: object\n"
	               "      properties:\n"
	               "        y:\n"
	               "          type: string\n"
	               "    Holder:\n"
	               "      type: object\n"
	               "      properties:\n"
	               "        list:\n"
	               "          type: array\n"
	               "          items:\n"
	               "            oneOf:\n"
	               "              - $ref: \"#/components/schemas/A\"\n"
	               "              - $ref: \"#/components/schemas/B\"\n",
	               "second $ref");

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
	/*
	 * ADR-0339 (#599): every read states its kind and every change the
	 * reads it makes stale. The events stream announces from these, so
	 * a gap is a page that stays stale with nothing saying so -- a
	 * build failure, as a missing permission is. The real spec passing
	 * above is the positive control: all 144 reads and 199 changes.
	 */
	expect_refusal("a read with no x-cix-read",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: getThing\n"
	               "      x-cix-permission: public\n",
	               "has no x-cix-read");
	expect_refusal("a read of an unknown kind",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: getThing\n"
	               "      x-cix-read: live\n"
	               "      x-cix-permission: public\n",
	               "must be state, sample");
	expect_refusal("a sample with no period",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: getThing\n"
	               "      x-cix-read: sample\n"
	               "      x-cix-permission: public\n",
	               "x-cix-sample-period goes with");
	expect_refusal("a change with no x-cix-invalidates",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: getThing\n"
	               "      x-cix-read: state\n"
	               "      x-cix-permission: public\n"
	               "    put:\n"
	               "      operationId: putThing\n"
	               "      x-cix-permission: public\n",
	               "has no x-cix-invalidates");
	expect_refusal("a change invalidating a read that does not exist",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    put:\n"
	               "      operationId: putThing\n"
	               "      x-cix-invalidates: [getNothing]\n"
	               "      x-cix-permission: public\n",
	               "not a GET operation");
	expect_refusal("a change invalidating a stream",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: tailThing\n"
	               "      x-cix-read: stream\n"
	               "      x-cix-permission: public\n"
	               "    put:\n"
	               "      operationId: putThing\n"
	               "      x-cix-invalidates: [tailThing]\n"
	               "      x-cix-permission: public\n",
	               "a stream read");
	expect_refusal("a view composed of itself",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: getThing\n"
	               "      x-cix-read: state\n"
	               "      x-cix-composed-of: [getThing]\n"
	               "      x-cix-permission: public\n",
	               "composed of itself");
	expect_refusal("a view composed of a view",
	               VOCAB
	               "paths:\n"
	               "  /a:\n"
	               "    get:\n"
	               "      operationId: getA\n"
	               "      x-cix-read: state\n"
	               "      x-cix-permission: public\n"
	               "  /b:\n"
	               "    get:\n"
	               "      operationId: getB\n"
	               "      x-cix-read: state\n"
	               "      x-cix-composed-of: [getA]\n"
	               "      x-cix-permission: public\n"
	               "  /c:\n"
	               "    get:\n"
	               "      operationId: getC\n"
	               "      x-cix-read: state\n"
	               "      x-cix-composed-of: [getB]\n"
	               "      x-cix-permission: public\n",
	               "itself a view");
	expect_refusal("an invalidation list mixing * with names",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: getThing\n"
	               "      x-cix-read: state\n"
	               "      x-cix-permission: public\n"
	               "    put:\n"
	               "      operationId: putThing\n"
	               "      x-cix-invalidates: [*, getThing]\n"
	               "      x-cix-permission: public\n",
	               "stands alone");
	expect_refusal("an invalidation list with an empty entry",
	               VOCAB
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: getThing\n"
	               "      x-cix-read: state\n"
	               "      x-cix-permission: public\n"
	               "    put:\n"
	               "      operationId: putThing\n"
	               "      x-cix-invalidates: [getThing, , getThing]\n"
	               "      x-cix-permission: public\n",
	               "empty entry");
	expect_refusal("a malformed vocabulary word",
	               "x-cix-permissions:\n  - Things:Read\n"
	               "paths:\n"
	               "  /thing:\n"
	               "    get:\n"
	               "      operationId: badWord\n"
	               "      x-cix-permission: Things:Read\n",
	               "is not a permission");

	/*
	 * A property written as a YAML FLOW MAPPING. Valid YAML, and apigen
	 * reads a property's keys one per line below its name, so
	 * `count: {type: integer}` used to be recorded as a field with no
	 * type -- which every consumer then read as a string. Fifteen of
	 * the contract's properties were written that way, all in the
	 * service schemas, and Service's `restarts` and `pid` reached the
	 * dashboard as strings while `on_exit` lost its enum (2026-10-16).
	 * Refused now, at both depths: the property itself, and an array's
	 * `items`.
	 *
	 * The real spec passing above is this pair's positive control, and
	 * a pointed one: it still carries `example:` blocks written in flow
	 * form (`ready: {tcp_port: 22}`), which are VALUES rather than
	 * property definitions and must stay legal.
	 */
#define FLOW_SPEC(props)                                                       \
	VOCAB                                                                  \
	"paths:\n"                                                             \
	"  /thing:\n"                                                          \
	"    get:\n"                                                           \
	"      operationId: getThing\n"                                        \
	"      x-cix-permission: public\n"                                     \
	"components:\n"                                                        \
	"  schemas:\n"                                                         \
	"    Thing:\n"                                                         \
	"      type: object\n"                                                 \
	"      properties:\n" props
	expect_refusal("a property written as a flow mapping",
	               FLOW_SPEC("        count: {type: integer}\n"),
	               "flow mapping");
	expect_refusal("an array whose items are a flow mapping",
	               FLOW_SPEC("        tags:\n"
	                         "          type: array\n"
	                         "          items: {type: string}\n"),
	               "flow mapping");
#undef FLOW_SPEC

	/* allOf in any shape but the one apigen merges is refused, not
	 * half-read: a second $ref, and a member that is neither. */
#define ALLOF_SPEC(members)                                                    \
	VOCAB                                                                  \
	"paths:\n"                                                             \
	"  /thing:\n"                                                          \
	"    get:\n"                                                           \
	"      operationId: getThing\n"                                        \
	"      x-cix-permission: public\n"                                     \
	"components:\n"                                                        \
	"  schemas:\n"                                                         \
	"    Base:\n"                                                          \
	"      type: object\n"                                                 \
	"      properties:\n"                                                  \
	"        a:\n"                                                         \
	"          type: string\n"                                             \
	"    Thing:\n"                                                         \
	"      allOf:\n" members
	expect_refusal("an allOf with a second $ref",
	               ALLOF_SPEC("        - $ref: \"#/components/schemas/Base\"\n"
	                          "        - $ref: \"#/components/schemas/Base\"\n"),
	               "second $ref");
	expect_refusal("an allOf member that is neither a $ref nor an object",
	               ALLOF_SPEC("        - $ref: \"#/components/schemas/Base\"\n"
	                          "        - oneOf: []\n"),
	               "allOf member apigen cannot read");
	expect_refusal("an allOf naming a schema that does not exist",
	               ALLOF_SPEC("        - $ref: \"#/components/schemas/Nowhere\"\n"),
	               "which is not a schema");
	/* One level only: resolving a chain in file order would make the
	 * merged field list depend on which schema is written first. */
	expect_refusal("an allOf whose base is itself an allOf",
	               ALLOF_SPEC("        - $ref: \"#/components/schemas/Base\"\n"
	                          "        - type: object\n"
	                          "          properties:\n"
	                          "            b:\n"
	                          "              type: string\n"
	                          "    Outer:\n"
	                          "      allOf:\n"
	                          "        - $ref: \"#/components/schemas/Thing\"\n"),
	               "itself an allOf");
#undef ALLOF_SPEC
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
			if (rows != 343 || with_permission != rows)
				fail("the generated route table has %d rows and %d carry a permission; "
				     "expected 343 and 343 (ADR-0317)",
				     rows, with_permission);
		}
	}

	{
		/*
		 * ADR-0339: the generated events headers. The daemon indexes
		 * its generation vector by these slots and raises them from
		 * these lists, so a slot out of range would write past the
		 * vector and a missing change row would announce nothing --
		 * both silently. Counted here against the contract itself:
		 * one slot per announced read (144 reads, less 10 streams and
		 * 11 derived reads, is 123), one row per change (199).
		 */
		char slots_path[256], table_path[256];
		char buf[4096];
		FILE *f;
		int count = -1, defines = 0, read_rows = 0, change_rows = 0, change_count = -1;
		int out_of_range = 0;

		snprintf(slots_path, sizeof(slots_path), "/tmp/apigen_evslots_%d.h", (int)getpid());
		snprintf(table_path, sizeof(table_path), "/tmp/apigen_evtable_%d.h", (int)getpid());
		snprintf(buf, sizeof(buf), "--emit-events-slots %s --emit-events-table %s", slots_path,
		         table_path);
		status = run_apigen("docs/api/openapi.yaml", buf, out, sizeof(out));
		if (status != 0)
			fail("apigen --emit-events-slots/--emit-events-table failed: %.200s", out);
		f = fopen(slots_path, "r");
		if (f != NULL) {
			while (fgets(buf, sizeof(buf), f) != NULL) {
				if (strncmp(buf, "#define EV_", 11) == 0)
					defines++;
				else if (strncmp(buf, "#define EVENTS_READ_COUNT ", 26) == 0)
					count = atoi(buf + 26);
			}
			fclose(f);
		}
		f = fopen(table_path, "r");
		if (f != NULL) {
			while (fgets(buf, sizeof(buf), f) != NULL) {
				if (strstr(buf, ", EVENTS_READ_") != NULL)
					read_rows++;
				else if (strncmp(buf, "\t{ \"", 4) == 0)
					change_rows++;
				else if (strncmp(buf, "#define EVENTS_CHANGE_COUNT ", 28) == 0)
					change_count = atoi(buf + 28);
				else if (strncmp(buf, "static const unsigned short ev_inv_", 35) == 0) {
					const char *p = strchr(buf, '{');

					while (p != NULL && *p != '\0' && *p != '}') {
						char *end;
						long v = strtol(p + 1, &end, 10);

						if (end == p + 1) {
							p++;
							continue;
						}
						if (v < 0 || (count >= 0 && v >= count))
							out_of_range++;
						p = end;
					}
				}
			}
			fclose(f);
		}
		unlink(slots_path);
		unlink(table_path);
		if (count != 123 || defines != count || read_rows != count)
			fail("the events slots say %d reads, define %d EV_ constants and the table has "
			     "%d read rows; all three should be 123 (ADR-0339)",
			     count, defines, read_rows);
		if (change_count != 199 || change_rows != change_count)
			fail("the events table declares %d changes and has %d change rows; both should "
			     "be 199, one per non-GET operation (ADR-0339)",
			     change_count, change_rows);
		if (out_of_range > 0)
			fail("%d invalidation slot(s) in the events table fall outside the generation "
			     "vector%s",
			     out_of_range, "");
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
		if (defines != 343)
			fail("CLI header has %d path defines, expected one per operation (343)", defines);
		if (methods != 343)
			fail("CLI header has %d method defines, expected one per operation (343)", methods);
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
		/*
		 * The two `title` gates, and both are SELF-SCOPING (#596).
		 *
		 * A schema nobody has adopted yet has no titles at all and is
		 * not judged; one that has started gets held to finishing.
		 * That matters because the contract has 385 request-schema
		 * fields and the dashboard renders 35 of them today -- a gate
		 * that demanded all 385 at once would simply be switched off.
		 */
		int titled_block = 0, untitled_in_block = 0, half_titled = 0;
		int long_first = 0, long_first_line = 0;
		char half_titled_text[160] = "", long_first_text[200] = "";
		char block_name[96] = "";
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
				{
					/* Its own local rather than the shared `tp`
					 * below: two readers sharing one cursor is an
					 * ordering trap waiting for the next check to be
					 * inserted between them. */
					const char *pat = strstr(buf, "pattern: \"");
					const char *q;

					if (pat != NULL)
						for (q = pat + 10; *q != '\0' && *q != '"'; q++)
							if (*q == ']' && q > pat + 10 && q[-1] == '-' &&
							    (q - 2 < pat + 10 || q[-2] != '\\')) {
								if (vmode_pattern++ == 0) {
									vmode_line = lno;
									snprintf(vmode_text,
									         sizeof(vmode_text),
									         "%.150s", pat);
								}
								break;
							}
				}
				/*
				 * A FIELDS block that has STARTED carrying `title` must
				 * finish, and its titled fields must lead their
				 * descriptions with a sentence a form can show.
				 *
				 * `title` is the operator's label (#596). Without it the
				 * renderer de-cases the schema key, which produced "Sn",
				 * "Ip" and "Uidnumber" on real forms -- the dashboard
				 * speaking its own schema's vocabulary at a person. A
				 * half-titled schema is the bad state: some fields read
				 * as English and the rest as JSON keys, in one modal.
				 */
				if (buf[0] == '\t' && buf[1] == '\t' && strstr(buf, ": [") != NULL) {
					size_t k = 2;

					while (buf[k] != '\0' && buf[k] != ':' && k - 2 < sizeof(block_name) - 1) {
						block_name[k - 2] = buf[k];
						k++;
					}
					block_name[k - 2] = '\0';
					titled_block = 0;
					untitled_in_block = 0;
				} else if (buf[0] == '\t' && buf[1] == '\t' && buf[2] == ']') {
					if (titled_block && untitled_in_block > 0 && half_titled++ == 0)
						snprintf(half_titled_text, sizeof(half_titled_text),
						         "%s (%d of its fields have no title)", block_name,
						         untitled_in_block);
					block_name[0] = '\0';
				} else if (strstr(buf, "{ name: \"") != NULL) {
					const char *ti = strstr(buf, ", title: \"");

					if (ti == NULL)
						untitled_in_block++;
					else
						titled_block = 1;
					/*
					 * The first sentence is the hint the form shows, so
					 * on a titled field it has to BE one. 160 characters
					 * is the measured ceiling: across this contract's
					 * 684 property descriptions the mean first sentence
					 * is 81 characters, and the ones past 160 are
					 * reference paragraphs with no sentence break in
					 * them rather than long sentences.
					 */
					if (ti != NULL) {
						const char *ds = strstr(buf, ", description: \"");

						if (ds != NULL) {
							const char *stop = strstr(ds + 16, ". ");
							size_t n = stop != NULL ? (size_t)(stop - (ds + 16))
							                        : strlen(ds + 16);

							if (n > 160 && long_first++ == 0) {
								long_first_line = lno;
								snprintf(long_first_text, sizeof(long_first_text),
								         "%.150s", ds + 16);
							}
						}
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
		if (shapes != 343)
			fail("web api.js has %d _SHAPE entries, expected one per operation (343)",
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
		if (half_titled > 0)
			fail("%d request schema(s) carry `title` on some fields and not others, the "
			     "first being %s.\n"
			     "      A title is the operator's label (#596) and the fallback de-cases "
			     "the schema key, so a half-titled schema renders half English and half "
			     "JSON in one modal -- \"Surname\" beside \"Sn\". Finish the schema or "
			     "start it later; this gate only judges one that has begun.",
			     half_titled, half_titled_text);
		if (long_first > 0)
			fail("%d titled field(s) open their description with more than 160 characters "
			     "before the first sentence break, the first at line %d: %s...\n"
			     "      The form shows that first sentence as the field's hint and keeps "
			     "the whole description on hover, so it has to be a sentence an operator "
			     "can read. Lead with what they need and let the reference detail follow.",
			     long_first, long_first_line, long_first_text);
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
		int n_names = 0, n_have = 0, i, dangling = 0, overflow = 0;
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
					if (n_have >= APIGEN_TEST_MAX_REQ) {
						overflow = 1;
					} else {
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
				if (n_names >= APIGEN_TEST_MAX_REQ) {
					overflow = 1;
				} else {
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
		/* A full list is a refusal, never a truncation: dropping the
		 * names past the cap is what reported four real request
		 * schemas as dangling in the 0.2.57-513 gate (#592). */
		if (overflow)
			fail("api.js has more FIELDS schemas or request names than %s holds; "
			     "raise it rather than check a prefix of them%s",
			     "APIGEN_TEST_MAX_REQ", "");
		if (n_names == 0)
			fail("no operation in api.js names a request schema -- 124 of the "
			     "contract's 343 carry a requestBody, so this is a reader that "
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
