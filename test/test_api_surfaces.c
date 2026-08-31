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

	if (failures == 0)
		printf("API SURFACES RESULT: PASS\n");
	else
		printf("API SURFACES RESULT: FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
