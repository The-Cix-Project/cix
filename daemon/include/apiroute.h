#ifndef APIROUTE_H
#define APIROUTE_H

#include <stddef.h>

/*
 * The generated-route dispatch layer (ADR-0218).
 *
 * One table, extracted from docs/api/openapi.yaml by tools/apigen.c at
 * build time, is the only way a /v1 request reaches a handler. The
 * table rows live in build/generated/api_routes.h (never committed --
 * build/ is gitignored, so a stale committed copy cannot exist); the
 * MATCHING lives here, as a pure function over a caller-supplied table,
 * so its semantics are testable against a tiny fixture table without
 * linking the daemon (test_apiroute.c does exactly that).
 *
 * Matching semantics, chosen to reproduce what the hand-written
 * dispatcher actually did -- fidelity first, elegance second:
 *
 *  - The query string ("?..." ) is not part of the route. The old chain
 *    was inconsistent here (most routes used a raw strcmp that a query
 *    string would break; a handful used a qlen trick to tolerate one);
 *    the matcher strips it uniformly, which widens tolerance and
 *    breaks nothing. Handlers that read query params still get the
 *    full original path via their ctx.
 *
 *  - A "{param}" matches exactly one non-empty segment, never '/'.
 *    Every path parameter in this API is a single name-like segment
 *    (container names, package names, tokens); a trailing slash or an
 *    empty segment therefore matches nothing, same as the old chain's
 *    name[0] != '\0' guards.
 *
 *  - Literal beats parameter, leftmost-first. "/containers/recipes"
 *    wins over "/containers/{name}" because at the second segment one
 *    row has a literal and the other a parameter -- the same
 *    reserved-word precedence the old chain got by ordering its ifs,
 *    now a property of the match instead of of the source order.
 *
 *  - No match is a 404 and a matched path with the wrong method is
 *    ALSO a 404 ("no such endpoint"), because that is what the old
 *    chain did -- it never distinguished a 405. Changing that would be
 *    a real (if arguably better) contract change, and this layer's job
 *    is to move routing, not to edit the API.
 *
 *  - A parameter longer than APIROUTE_PARAM_MAX-1 matches nothing.
 *    The old chain refused over-long names route by route (nlen < cap,
 *    else fall through to 404); one cap here replaces all of those.
 */

#define APIROUTE_MAX_SEGS 16
#define APIROUTE_MAX_PARAMS 2
#define APIROUTE_PARAM_MAX 256

struct http_request;

/* What an op handler receives: the connection, the parsed request
 * (full path with query string intact, body, headers), and the
 * path parameters in spec order, already extracted and NUL-terminated.
 * p[1] is "" for one-parameter routes, both are "" for none. */
struct api_ctx {
	int fd;
	const struct http_request *req;
	const char *p[APIROUTE_MAX_PARAMS];
};

typedef void (*api_op_fn)(const struct api_ctx *ctx);

struct api_route {
	const char *method;               /* "GET", "POST", ... */
	int n_segs;
	const char *segs[APIROUTE_MAX_SEGS]; /* literal text, or NULL for a {param} */
	api_op_fn fn;
	const char *op_id;                /* for diagnostics only */
	/*
	 * When set, the FINAL segment (which must be a {param}) captures
	 * the rest of the path, slashes included. Exists for exactly one
	 * contract shape: an operation whose parameter never legitimately
	 * contains '/', but whose handler owns the 400 that says so --
	 * sysctl keys and kmod names. Their specs promise "400: invalid
	 * key (e.g. contains '/')", which a single-segment parameter can
	 * never deliver (the request stops matching before the handler can
	 * speak). Declared per-operation in the spec (x-cix-rest-param),
	 * never inferred.
	 */
	int rest_param;
};

/*
 * Finds the best-matching route for (method, path) and extracts its
 * parameters. Returns the row index, or -1 when nothing matches (the
 * caller responds 404). params must be char[APIROUTE_MAX_PARAMS][APIROUTE_PARAM_MAX].
 */
int api_route_match(const struct api_route *routes, int n_routes, const char *method,
                    const char *path, char params[][APIROUTE_PARAM_MAX]);

#endif /* APIROUTE_H */
