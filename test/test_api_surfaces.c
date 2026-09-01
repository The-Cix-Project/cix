/*
 * test_api_surfaces -- ADR-0218 layer 1, the negative check.
 *
 * A presentation channel must not build API paths of its own. This does
 * NOT try to enumerate which paths a channel uses: that was attempted
 * three times during the work that produced ADR-0218 and gave three
 * different answers, one of them supporting the confident and false
 * conclusion that the dashboard could not stop a container (it builds
 * that URL by string concatenation, which the extraction did not see).
 *
 * So the question is inverted. Enumerating what a channel builds is
 * unreliable; asserting that it builds NOTHING is trivial, because it
 * needs no understanding of the code at all -- just the absence of a
 * literal. That asymmetry is the whole design: an unreliable check
 * would be worth less than no check, and a reliable one is available
 * for the asking.
 *
 * What a violation looks like: someone types "/v1/containers/" + name
 * instead of using the generated constant. It compiles, it works today,
 * and it silently stops working the day the contract moves -- which is
 * exactly the drift #182's five-domain regrouping would cause at scale.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>

static int failures;

/*
 * Scans one file for `/v1/` inside a string literal. Deliberately crude
 * -- it looks for the text, not for syntax -- because crudeness is what
 * makes it trustworthy here: there is no parse to get wrong. Comments
 * are the one place the text may legitimately appear (describing the
 * API, or recording a path that used to be built by hand), so a line
 * whose first non-space characters are a comment marker is skipped.
 */
static void scan(const char *path, const char *channel, const char *how)
{
	FILE *f = fopen(path, "r");
	char line[8192];
	int lineno = 0, hits = 0;

	if (f == NULL) {
		fprintf(stderr, "FAIL: cannot open %s\n", path);
		failures++;
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p = line;
		const char *found;

		lineno++;
		while (*p == ' ' || *p == '\t')
			p++;
		if (strncmp(p, "/*", 2) == 0 || strncmp(p, "*", 1) == 0 ||
		    strncmp(p, "//", 2) == 0)
			continue;
		/*
		 * A path being BUILT is a string literal that STARTS with
		 * /v1/. A path merely mentioned -- "POST /v1/system/iso needs
		 * both" in a help message -- appears mid-sentence and is not
		 * a path this code constructs. Checked structurally (does the
		 * literal begin here?) rather than by guessing at intent.
		 *
		 * The precision was not academic: the first version of this
		 * check flagged exactly such a help string, and the right
		 * response was to sharpen the rule rather than add an
		 * exception -- a check with a growing exception list stops
		 * being one.
		 */
		found = strstr(line, "\"/v1/");
		if (found == NULL)
			found = strstr(line, "`/v1/");
		if (found == NULL)
			continue;
		if (hits == 0)
			fprintf(stderr,
			        "FAIL: %s builds API paths by hand. Use the generated %s instead --\n"
			        "      a hand-typed path compiles and fails at runtime, and silently\n"
			        "      breaks the day the contract moves (ADR-0218).\n",
			        channel, how);
		fprintf(stderr, "      %s:%d: %.100s", path, lineno, line);
		hits++;
	}
	fclose(f);
	if (hits > 0)
		failures++;
}

/*
 * ADR-0218 layer 2: the declaration check.
 *
 * `x-cix-expose` in the spec says which presentation channels must
 * expose an operation. The API decides that -- in one place -- rather
 * than each channel deciding for itself and drifting, which is the
 * situation that produced 32 CLI-only and 5 web-only operations with
 * nothing recording whether any of them was intended.
 *
 * Enforced BOTH ways, because each direction catches a different
 * mistake:
 *   declared but absent  -- the contract promises a capability through
 *                           a channel that does not offer it
 *   present but undeclared -- a channel grew a capability the contract
 *                           never sanctioned
 *
 * This is only checkable at all because of layer 1. Enumerating which
 * operations a channel implements used to mean reconstructing URLs
 * from string concatenation, which gave three different answers in
 * three attempts. Now every call site names its operation --
 * CIX_API_getContainer / CIX_API.getContainer -- so this is an
 * identifier scan, and identifiers are not built by concatenation.
 *
 * What it does NOT prove: that a person can reach the capability. A
 * generated helper referenced from dead code passes this happily.
 * That is layer 3, and the distinction is #192's lesson -- detecting
 * is not preventing, and existing is not reachable.
 */
/*
 * Whether `pattern` occurs as a WHOLE identifier, not merely as a
 * substring. The distinction is load-bearing: CIX_API.getVolume occurs
 * inside CIX_API.getVolumeBackups, getNetwork inside getNetworkPorts,
 * getPkg inside getPkgRecipe. A plain strstr() therefore credits a
 * channel with operations it never calls -- this check reported six
 * such phantoms on its first run, every one of them a longer name
 * swallowing a shorter one.
 *
 * Only the trailing edge needs testing: every caller passes a pattern
 * that already begins with the CIX_API_ / CIX_API. prefix, so the
 * leading edge is fixed by construction.
 */
static int mentions(const char *haystack, const char *pattern)
{
	size_t n = strlen(pattern);
	const char *p = haystack;

	while ((p = strstr(p, pattern)) != NULL) {
		char after = p[n];

		if (!((after >= 'A' && after <= 'Z') || (after >= 'a' && after <= 'z') ||
		      (after >= '0' && after <= '9') || after == '_'))
			return 1;
		p += n;
	}
	return 0;
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
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
	buf[n] = '\0';
	fclose(f);
	return buf;
}

/*
 * Blanks whole comment LINES in place, so the exposure check below sees
 * the same file the path check does (#234).
 *
 * The two halves of this test disagreed: the path check skips a line
 * whose first non-space characters open a comment, while the exposure
 * check substring-searched the file whole. So a comment NAMING an
 * operation's generated constant was indistinguishable from a call to
 * it, and a comment written to explain why the CLI must not call an
 * unexposed operation failed the build for calling it. An explanation
 * had to be reworded around the checker, which is the wrong way round.
 *
 * Deliberately line-leading rather than a real comment parser, for the
 * reason the path check gives for the same choice: a parser has to know
 * that "/*" inside a string literal is not a comment, and that the
 * "//" in an https:// URL -- which web/app.js contains -- is not one
 * either. Looking only at what a line STARTS with cannot be fooled by
 * either, at the cost of missing a trailing comment on a code line.
 * That cost is the right one: a trailing comment sits beside real code,
 * so treating it as code is conservative, and this check's job is to
 * avoid FALSE alarms without inventing permission to miss true ones.
 *
 * In place, and blanking rather than deleting, so offsets and line
 * numbers are unchanged for anything that reads the buffer afterwards.
 */
static void blank_comment_lines(char *buf)
{
	char *line = buf;

	while (line != NULL && *line != '\0') {
		char *eol = strchr(line, '\n');
		char *p = line;

		while (*p == ' ' || *p == '\t')
			p++;
		if (strncmp(p, "/*", 2) == 0 || strncmp(p, "*", 1) == 0 ||
		    strncmp(p, "//", 2) == 0) {
			char *q = line;

			while (q != eol && *q != '\0')
				*q++ = ' ';
		}
		if (eol == NULL)
			break;
		line = eol + 1;
	}
}

static void check_declared_exposure(void)
{
	/* Both halves of the CLI, concatenated: an operation used by either
	 * file is exposed by the channel. */
	char *cli_main = slurp("cli/src/main.c");
	char *cli_console = slurp("client/src/console.c");
	char *cli = NULL;
	char *web = slurp("web/app.js");
	FILE *p;
	char line[1024];
	int checked = 0;

	if (cli_main != NULL && cli_console != NULL) {
		size_t n = strlen(cli_main) + strlen(cli_console) + 1;

		cli = malloc(n);
		if (cli != NULL) {
			snprintf(cli, n, "%s%s", cli_main, cli_console);
		}
	}
	free(cli_main);
	free(cli_console);
	if (cli == NULL || web == NULL) {
		fprintf(stderr, "FAIL: cannot read a channel source\n");
		failures++;
		free(cli);
		free(web);
		return;
	}
	/* #234: a comment naming an operation is not a call to it. */
	blank_comment_lines(cli);
	blank_comment_lines(web);
	p = popen("./build/apigen docs/api/openapi.yaml --list", "r");
	if (p == NULL) {
		fprintf(stderr, "FAIL: cannot run apigen\n");
		failures++;
		free(cli);
		free(web);
		return;
	}
	while (fgets(line, sizeof(line), p) != NULL) {
		char method[16], path[256], op_id[128], expose[64];
		char want_cli[160], want_web[160];
		int decl_cli, decl_web, has_cli, has_web;

		if (sscanf(line, "%15s %255s %127s %63s", method, path, op_id, expose) != 4)
			continue;
		checked++;
		decl_cli = mentions(expose, "cli");
		decl_web = mentions(expose, "web");
		snprintf(want_cli, sizeof(want_cli), "CIX_API_%s", op_id);
		snprintf(want_web, sizeof(want_web), "CIX_API.%s", op_id);
		has_cli = mentions(cli, want_cli);
		has_web = mentions(web, want_web);

		if (decl_cli && !has_cli) {
			fprintf(stderr,
			        "FAIL: %s is declared x-cix-expose [cli] but the CLI never calls it\n",
			        op_id);
			failures++;
		}
		if (!decl_cli && has_cli) {
			fprintf(stderr,
			        "FAIL: the CLI calls %s but the contract does not expose it to cli --\n"
			        "      the API decides which channel offers a capability, not the channel\n",
			        op_id);
			failures++;
		}
		if (decl_web && !has_web) {
			fprintf(stderr,
			        "FAIL: %s is declared x-cix-expose [web] but the dashboard never calls it\n",
			        op_id);
			failures++;
		}
		if (!decl_web && has_web) {
			fprintf(stderr,
			        "FAIL: the dashboard calls %s but the contract does not expose it to web\n",
			        op_id);
			failures++;
		}
	}
	pclose(p);
	free(cli);
	free(web);
	if (checked == 0) {
		fprintf(stderr, "FAIL: no operations checked -- apigen produced nothing, so a green "
		                "result here would mean nothing\n");
		failures++;
	}
}


/*
 * ADR-0218 layer 3: reachability.
 *
 * Layers 1 and 2 prove an operation's helper EXISTS and is referenced.
 * Neither proves a person can reach it: a generated helper called from
 * a function nothing ever calls satisfies both perfectly while the
 * operator still cannot do the thing. That gap is #192's lesson in
 * another costume -- detecting is not preventing, existing is not
 * reachable -- so it is checked rather than assumed.
 *
 * The check walks the CLI's call graph from main() and fails if any
 * API call sits in a function that graph cannot reach.
 *
 * Two things learned building it, both worth keeping:
 *
 * 1. A first version treated every "type name(" line as a function
 *    DEFINITION, including forward declarations. A prototype then
 *    claimed the byte range of the code after it, every later boundary
 *    shifted, and the walk reported six live commands as dead --
 *    cmd_files_get among them, which is called two lines from where
 *    the checker said nothing called it. A checker that cries wolf on
 *    working code is worse than none, so a definition here must have a
 *    body: '{' ending the signature, or alone on the next line.
 *
 * 2. It is deliberately conservative. An unknown construct makes a
 *    function look REACHABLE, never dead -- a false pass is a missed
 *    warning, a false failure is a broken build for correct code.
 */
#define MAXFN 1024
#define FNNAME 128

struct fn {
	char name[FNNAME];
	size_t start, end;
	int reachable;
};

static struct fn g_fn[MAXFN];
static int g_fn_count;

static int is_ident_char(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
	       c == '_';
}

/* The next non-blank line's first character. */
static char peek_next_nonblank(const char *s, size_t from)
{
	size_t i = from;

	while (s[i] != '\0') {
		if (s[i] == '\n') {
			size_t j = i + 1;

			while (s[j] == ' ' || s[j] == '\t' || s[j] == '\n')
				j++;
			return s[j];
		}
		i++;
	}
	return '\0';
}

static void collect_functions(const char *s)
{
	size_t i = 0;
	int at_line_start = 1;

	g_fn_count = 0;
	while (s[i] != '\0' && g_fn_count < MAXFN) {
		if (at_line_start && (is_ident_char(s[i]))) {
			size_t j = i, last_ident_start = i, paren = 0;

			/* scan the candidate signature to its '(' */
			while (s[j] != '\0' && s[j] != '\n' && s[j] != '(') {
				if (!is_ident_char(s[j - 1 < j ? j - 1 : j]) && is_ident_char(s[j]))
					last_ident_start = j;
				j++;
			}
			if (s[j] == '(') {
				size_t name_end = j;
				size_t k = j;
				int depth = 0;

				/* balance the parameter list, possibly across lines */
				while (s[k] != '\0') {
					if (s[k] == '(')
						depth++;
					else if (s[k] == ')') {
						depth--;
						if (depth == 0)
							break;
					}
					k++;
				}
				if (s[k] == ')') {
					size_t t = k + 1;

					while (s[t] == ' ' || s[t] == '\t')
						t++;
					/* a DEFINITION has a body; a prototype ends in ';' */
					if (s[t] == '{' || (s[t] == '\n' && peek_next_nonblank(s, k) == '{')) {
						size_t n = name_end - last_ident_start;

						if (n > 0 && n < FNNAME) {
							memcpy(g_fn[g_fn_count].name, s + last_ident_start, n);
							g_fn[g_fn_count].name[n] = '\0';
							g_fn[g_fn_count].start = i;
							g_fn[g_fn_count].reachable = 0;
							g_fn_count++;
						}
					}
				}
			}
		}
		at_line_start = (s[i] == '\n');
		i++;
	}
	for (i = 0; (int)i < g_fn_count; i++)
		g_fn[i].end = ((int)i + 1 < g_fn_count) ? g_fn[i + 1].start : strlen(s);
}

static int fn_index(const char *name)
{
	int i;

	for (i = 0; i < g_fn_count; i++)
		if (strcmp(g_fn[i].name, name) == 0)
			return i;
	return -1;
}

/* Marks f and everything it calls. Depth-bounded rather than
 * unbounded recursion: a cycle is normal in C and must not blow the
 * stack of a test. */
static void mark_reachable(const char *s, int f, int depth)
{
	size_t i;

	if (f < 0 || f >= g_fn_count || g_fn[f].reachable || depth > 64)
		return;
	g_fn[f].reachable = 1;
	for (i = g_fn[f].start; i < g_fn[f].end; i++) {
		if (!is_ident_char(s[i]) || (i > 0 && is_ident_char(s[i - 1])))
			continue;
		{
			size_t j = i;
			char word[FNNAME];
			size_t n = 0;

			while (is_ident_char(s[j]) && n + 1 < sizeof(word))
				word[n++] = s[j++];
			word[n] = '\0';
			while (s[j] == ' ' || s[j] == '\t')
				j++;
			if (s[j] == '(')
				mark_reachable(s, fn_index(word), depth + 1);
			i = j > i ? j - 1 : i;
		}
	}
}

static void check_reachability(void)
{
	char *s = slurp("cli/src/main.c");
	int i, dead = 0;

	if (s == NULL) {
		fprintf(stderr, "FAIL: cannot read cli/src/main.c for reachability\n");
		failures++;
		return;
	}
	collect_functions(s);
	if (g_fn_count < 100) {
		/* The parser found implausibly few functions, so a clean result
		 * would mean nothing. Say so instead of passing. */
		fprintf(stderr,
		        "FAIL: reachability found only %d function definitions in cli/src/main.c -- "
		        "the parser is wrong, and a pass here would be meaningless\n",
		        g_fn_count);
		failures++;
		free(s);
		return;
	}
	mark_reachable(s, fn_index("main"), 0);
	for (i = 0; i < g_fn_count; i++) {
		size_t j;

		if (g_fn[i].reachable)
			continue;
		for (j = g_fn[i].start; j + 8 < g_fn[i].end; j++) {
			if (strncmp(s + j, "CIX_API_", 8) != 0)
				continue;
			if (dead == 0)
				fprintf(stderr,
				        "FAIL: the CLI calls the API from code nothing reaches. A generated\n"
				        "      helper referenced from a function main() cannot reach is a\n"
				        "      capability the contract promises and no operator can use\n"
				        "      (ADR-0218 layer 3).\n");
			fprintf(stderr, "      %s() is unreachable but calls the API\n", g_fn[i].name);
			dead++;
			break;
		}
	}
	if (dead > 0)
		failures++;
	free(s);
}

int main(void)
{
	/*
	 * A channel is not one file. client/src/console.c builds the two
	 * WebSocket paths (container console, pkg build-log) and was
	 * missed by the first version of this check -- which is how an
	 * operation can look "exposed by neither channel" while the CLI
	 * has used it all along.
	 */
	scan("cli/src/main.c", "the CLI",
	     "CIX_API_<operationId> constants from build/generated/cix_api.h");
	scan("client/src/console.c", "the CLI's client library",
	     "CIX_API_<operationId> constants from build/generated/cix_api.h");
	scan("web/app.js", "the dashboard", "CIX_API.<operationId>() helpers from the generated web/api.js");

	check_declared_exposure();
	check_reachability();

	if (failures == 0)
		printf("API SURFACES RESULT: PASS\n");
	else
		printf("API SURFACES RESULT: FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
