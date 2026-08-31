/*
 * test_apiroute -- proves the generated-route matcher's semantics
 * (ADR-0218) against a fixture table, before the daemon trusts it with
 * all 263 operations.
 *
 * The matcher is the one hand-written piece between the spec and every
 * handler; a subtle mis-match here would misroute requests while every
 * individual handler stayed correct. Each documented semantic from
 * apiroute.h is asserted here in both directions -- what must match and
 * what must not -- because a check only run against the case it was
 * written for is how this project produced three gates that could
 * never pass.
 */
#include <stdio.h>
#include <string.h>

#include "apiroute.h"

static int failures;

#define CHECK(cond, ...)                                                        \
	do {                                                                    \
		if (!(cond)) {                                                  \
			fprintf(stderr, "FAIL: ");                              \
			fprintf(stderr, __VA_ARGS__);                           \
			fprintf(stderr, "\n");                                  \
			failures++;                                             \
		}                                                               \
	} while (0)

/* Row indexes double as identity: the test asserts WHICH row matched,
 * not merely that something did. */
static const struct api_route T[] = {
	/* 0 */ { "GET", 2, { "v1", "health" }, NULL, "getHealth" },
	/* 1 */ { "GET", 2, { "v1", "containers" }, NULL, "listContainers" },
	/* 2 */ { "POST", 2, { "v1", "containers" }, NULL, "createContainer" },
	/* 3 */ { "GET", 3, { "v1", "containers", NULL }, NULL, "getContainer" },
	/* 4 */ { "GET", 3, { "v1", "containers", "recipes" }, NULL, "listRecipes" },
	/* 5 */ { "POST", 4, { "v1", "containers", NULL, "start" }, NULL, "startContainer" },
	/* 6 */ { "DELETE", 5, { "v1", "containers", NULL, "networks", NULL }, NULL, "detachNetwork" },
	/* 7 */ { "POST", 4, { "v1", "containers", "recipes", "apply-all" }, NULL, "applyAll" },
	/* 8 */ { "GET", 4, { "v1", "system", "sysctl", NULL }, NULL, "getSysctl", 1 },
};
#define NT ((int)(sizeof(T) / sizeof(T[0])))

static int match(const char *method, const char *path, char params[][APIROUTE_PARAM_MAX])
{
	return api_route_match(T, NT, method, path, params);
}

int main(void)
{
	char p[APIROUTE_MAX_PARAMS][APIROUTE_PARAM_MAX];

	/* Exact literal match, and the method is part of the key. */
	CHECK(match("GET", "/v1/health", p) == 0, "GET /v1/health must match row 0");
	CHECK(match("POST", "/v1/health", p) == -1,
	      "POST /v1/health must NOT match -- wrong method is a 404, same as the old chain");
	CHECK(match("GET", "/v1/containers", p) == 1 && match("POST", "/v1/containers", p) == 2,
	      "method selects between rows sharing a path");

	/* Parameter extraction, including names with the characters this
	 * API really uses (@ in pkg names, dots in versions). */
	CHECK(match("GET", "/v1/containers/dns-1", p) == 3 && strcmp(p[0], "dns-1") == 0,
	      "one-param route must extract the segment (got \"%s\")", p[0]);
	CHECK(match("GET", "/v1/containers/gcc@cix-builder", p) == 3 &&
	              strcmp(p[0], "gcc@cix-builder") == 0,
	      "'@' is an ordinary parameter character");
	CHECK(match("DELETE", "/v1/containers/web/networks/mgmt0", p) == 6 &&
	              strcmp(p[0], "web") == 0 && strcmp(p[1], "mgmt0") == 0,
	      "two-param route must extract both in order (got \"%s\", \"%s\")", p[0], p[1]);
	CHECK(match("GET", "/v1/containers/dns-1", p) == 3 && p[1][0] == '\0',
	      "unused params must come back empty, not stale");

	/* Literal beats parameter, leftmost-first -- the reserved-word rule. */
	CHECK(match("GET", "/v1/containers/recipes", p) == 4,
	      "\"recipes\" must hit the literal row, not become a container name");
	CHECK(match("GET", "/v1/containers/recipesX", p) == 3 && strcmp(p[0], "recipesX") == 0,
	      "a name merely PREFIXED by a reserved word is still a name");
	CHECK(match("POST", "/v1/containers/recipes/apply-all", p) == 7,
	      "literal wins at every depth, not only the first");

	/* The query string is not part of the route. */
	CHECK(match("GET", "/v1/health?x=1", p) == 0, "query string must be stripped");
	CHECK(match("GET", "/v1/containers/dns-1?verbose=1", p) == 3 && strcmp(p[0], "dns-1") == 0,
	      "a param segment must not swallow the query string");

	/* What must NOT match: the old chain's name[0] != '\\0' guards. */
	CHECK(match("GET", "/v1/containers/", p) == -1, "trailing slash is not an empty param");
	CHECK(match("GET", "/v1//health", p) == -1, "an empty segment matches nothing");
	CHECK(match("GET", "/v1/health/extra", p) == -1, "segment count is exact");
	CHECK(match("GET", "/v1/containers/a/b", p) == -1,
	      "a param is one segment -- it never swallows '/'");
	CHECK(match("GET", "nonsense", p) == -1, "a path must start with '/'");
	{
		/* Over-long segment: refused, same as the chain's nlen caps. */
		char long_path[APIROUTE_PARAM_MAX + 64];
		int i, n;

		n = snprintf(long_path, sizeof(long_path), "/v1/containers/");
		for (i = 0; i < APIROUTE_PARAM_MAX; i++)
			long_path[n + i] = 'a';
		long_path[n + i] = '\0';
		CHECK(match("GET", long_path, p) == -1,
		      "a segment at/over APIROUTE_PARAM_MAX must match nothing, never truncate");
	}

	/* Rest-parameter routes (x-cix-rest-param): the final {param}
	 * captures the remainder, slashes included, so a handler whose
	 * contract promises "400: invalid key (contains '/')" still gets
	 * to say so instead of the matcher answering 404 first. */
	CHECK(match("GET", "/v1/system/sysctl/net.ipv4.ip_forward", p) == 8 &&
	              strcmp(p[0], "net.ipv4.ip_forward") == 0,
	      "a rest param still matches a plain single segment");
	CHECK(match("GET", "/v1/system/sysctl/bad/key", p) == 8 && strcmp(p[0], "bad/key") == 0,
	      "a rest param captures slashes verbatim (got \"%s\")", p[0]);
	CHECK(match("GET", "/v1/system/sysctl/a/b/c/d", p) == 8 && strcmp(p[0], "a/b/c/d") == 0,
	      "a rest param captures arbitrarily many segments");
	CHECK(match("GET", "/v1/system/sysctl", p) == -1,
	      "a rest param still requires at least one segment");
	CHECK(match("GET", "/v1/containers/a/b", p) == -1,
	      "routes WITHOUT the rest flag stay strictly single-segment");

	if (failures == 0)
		printf("APIROUTE RESULT: PASS\n");
	else
		printf("APIROUTE RESULT: FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
