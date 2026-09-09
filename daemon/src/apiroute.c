#include "apiroute.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * See apiroute.h for the semantics this implements and why each was
 * chosen. This file deliberately knows nothing about the table's
 * contents -- the table is generated, the matching is not, and keeping
 * the matching table-agnostic is what lets test_apiroute.c prove these
 * semantics against a fixture table without linking the daemon.
 */

/* Splits path (query already excluded by `len`) into segments.
 * Returns the count, or -1 when it cannot be a route match: too many
 * segments, an empty segment ("//" or trailing "/"), or an over-long
 * one. Each of those matched nothing in the old chain either. */
static int split_segments(const char *path, size_t len, const char *starts[APIROUTE_MAX_SEGS],
                          size_t seg_lens[APIROUTE_MAX_SEGS])
{
	int n = 0;
	size_t i = 0;

	if (len == 0 || path[0] != '/')
		return -1;
	while (i < len) {
		size_t start, seg_len;

		i++; /* the '/' */
		start = i;
		while (i < len && path[i] != '/')
			i++;
		seg_len = i - start;
		if (seg_len == 0)
			return -1; /* "//" or trailing "/" */
		if (seg_len >= APIROUTE_PARAM_MAX)
			return -1;
		if (n >= APIROUTE_MAX_SEGS)
			return -1;
		starts[n] = path + start;
		seg_lens[n] = seg_len;
		n++;
	}
	return n;
}

/*
 * Whether `a` beats `b` for the same request: at the leftmost segment
 * where they differ in kind, the literal wins. Rows can never tie --
 * two rows with identical method and identical kind at every segment
 * would be the same operation twice, which apigen refuses.
 */
static int route_beats(const struct api_route *a, const struct api_route *b)
{
	int i;

	for (i = 0; i < a->n_segs; i++) {
		int a_lit = a->segs[i] != NULL;
		int b_lit = b->segs[i] != NULL;

		if (a_lit != b_lit)
			return a_lit;
	}
	return 0;
}

/*
 * Percent-decoding for a path parameter, in place.
 *
 * Query strings have been decoded since url_query_param() below; path
 * segments never were, and a correct client cannot tell. `@` is legal
 * unencoded in a path segment but encoding it is equally legal, and
 * every general-purpose client library does: Python's
 * urllib.parse.quote() escapes it by default. So a package addressed
 * the documented way, `name@image`, resolved perfectly as
 * /v1/pkg/cix@__hostbuild and answered "no such package" as
 * /v1/pkg/cix%40__hostbuild -- the same package, the same request, a
 * 200 or a 404 depending on which client wrote the URL. Measured
 * against a real entry on 192.168.15.95.
 *
 * TWO ESCAPES ARE REFUSED RATHER THAN DECODED, and this is the part
 * that matters. Routing has already happened by the time this runs:
 * the segments were split on '/' and matched against the table. A
 * %2F decoded now would insert a separator into a segment AFTER the
 * decision about what that path meant was taken, which is exactly how
 * path-confusion bugs are built. %00 is refused for the same reason in
 * the other direction -- it would truncate the parameter and hide
 * whatever followed from every check downstream. Neither has a
 * legitimate use in any parameter this API defines, so both are a
 * refusal (-1, a 404 from the caller) rather than a silent
 * substitution.
 *
 * A malformed escape (`%zz`, or a `%` at the end) is left alone rather
 * than rejected, matching url_query_param()'s own behaviour: a literal
 * percent is an ordinary character and this is not the place to
 * invent a stricter rule than the query parser applies.
 */
static int decode_path_param(char *sv)
{
	size_t r = 0, w = 0;

	while (sv[r] != '\0') {
		if (sv[r] == '%' && isxdigit((unsigned char)sv[r + 1]) &&
		    isxdigit((unsigned char)sv[r + 2])) {
			char hex[3] = { sv[r + 1], sv[r + 2], '\0' };
			int c = (int)strtol(hex, NULL, 16);

			if (c == '/' || c == '\0')
				return -1;
			sv[w++] = (char)c;
			r += 3;
			continue;
		}
		sv[w++] = sv[r++];
	}
	sv[w] = '\0';
	return 0;
}

int api_route_match(const struct api_route *routes, int n_routes, const char *method,
                    const char *path, char params[][APIROUTE_PARAM_MAX])
{
	const char *starts[APIROUTE_MAX_SEGS];
	size_t seg_lens[APIROUTE_MAX_SEGS];
	int n_segs, i, best = -1;

	n_segs = split_segments(path, strcspn(path, "?"), starts, seg_lens);
	if (n_segs < 0)
		return -1;

	for (i = 0; i < n_routes; i++) {
		const struct api_route *r = &routes[i];
		int s, ok = 1;

		if (strcmp(r->method, method) != 0)
			continue;
		if (r->rest_param ? n_segs < r->n_segs : n_segs != r->n_segs)
			continue;
		for (s = 0; s < (r->rest_param ? r->n_segs - 1 : n_segs); s++) {
			if (r->segs[s] != NULL &&
			    (strlen(r->segs[s]) != seg_lens[s] ||
			     memcmp(r->segs[s], starts[s], seg_lens[s]) != 0)) {
				ok = 0;
				break;
			}
		}
		if (!ok)
			continue;
		if (best < 0 || route_beats(r, &routes[best]))
			best = i;
	}
	if (best < 0)
		return -1;

	{
		const struct api_route *r = &routes[best];
		int s, p = 0;
		int fixed = r->rest_param ? r->n_segs - 1 : r->n_segs;

		for (s = 0; s < fixed && p < APIROUTE_MAX_PARAMS; s++) {
			if (r->segs[s] == NULL) {
				memcpy(params[p], starts[s], seg_lens[s]);
				params[p][seg_lens[s]] = '\0';
				if (decode_path_param(params[p]) != 0)
					return -1;
				p++;
			}
		}
		if (r->rest_param && p < APIROUTE_MAX_PARAMS) {
			/* The remainder verbatim, slashes and all -- taken from
			 * the original path so nothing is re-joined. Length was
			 * bounded per segment by split_segments(); the whole
			 * remainder gets the same cap here. */
			const char *rest = starts[r->n_segs - 1];
			size_t rest_len = (size_t)((path + strcspn(path, "?")) - rest);

			if (rest_len >= APIROUTE_PARAM_MAX)
				return -1;
			memcpy(params[p], rest, rest_len);
			params[p][rest_len] = '\0';
			p++;
		}
		for (; p < APIROUTE_MAX_PARAMS; p++)
			params[p][0] = '\0';
	}
	return best;
}

/*
 * Refuses a query parameter the spec never gave this operation (#282).
 *
 * The matcher above deliberately ignores everything from '?' onward,
 * because a query string has nothing to do with which route a request
 * belongs to. What was missing is the second half: once the route IS
 * known, its declared parameters are known too, and anything else in
 * the query string is a caller asking for something this operation
 * cannot do. Accepting it and carrying on is the dangerous answer --
 * DELETE /v1/pkg/{name} takes its image as part of the path, so a
 * caller who wrote ?image=jumpbox had it dropped and destroyed the
 * package in the default image instead, with a 204 saying it worked.
 *
 * Empty parameters are tolerated ("?", "?&", "?x=1&&y=2"): they carry
 * no instruction, so there is nothing to misread. A bare key with no
 * '=' is still a parameter and is still checked, since "?list" means
 * the same thing to a reader as "?list=1".
 */
int api_route_query_unknown(const struct api_route *r, const char *path, char *out,
                            size_t out_size)
{
	const char *q = strchr(path, '?');

	if (out != NULL && out_size > 0)
		out[0] = '\0';
	if (q == NULL)
		return 0;
	q++;
	while (*q != '\0') {
		const char *end = q;
		size_t klen;
		int i, known = 0;

		while (*end != '\0' && *end != '&')
			end++;
		klen = strcspn(q, "=&");
		if (klen > (size_t)(end - q))
			klen = (size_t)(end - q);
		if (klen == 0) {
			q = (*end == '&') ? end + 1 : end;
			continue;
		}
		for (i = 0; i < r->n_query_params; i++) {
			const char *d = r->query_params[i];

			if (strlen(d) == klen && memcmp(d, q, klen) == 0) {
				known = 1;
				break;
			}
		}
		if (!known) {
			if (out != NULL && out_size > 0) {
				size_t n = klen < out_size - 1 ? klen : out_size - 1;

				memcpy(out, q, n);
				out[n] = '\0';
			}
			return -1;
		}
		q = (*end == '&') ? end + 1 : end;
	}
	return 0;
}

/*
 * This daemon's first (and, deliberately, narrowest-possible) query
 * string parser: GET .../files?path=... is the first route that ever
 * needs one. One key only, no repeated-key/array semantics, %XX
 * percent-decoding only (no "+" -> space -- this project has never had
 * a form-encoded body, no reason to invent that convention here).
 * full_path is the request's own req->path, "?"-and-all; key is looked
 * up among the "&"-separated pairs after the first "?". Returns 0 and
 * fills out[] on a match, -1 if the key is absent or the value doesn't
 * fit in out_size.
 */
int url_query_param(const char *full_path, const char *key, char *out, size_t out_size)
{
	const char *q = strchr(full_path, '?');
	size_t key_len = strlen(key);

	if (q == NULL)
		return -1;
	q++;
	while (*q != '\0') {
		const char *amp = strchr(q, '&');
		size_t pair_len = amp != NULL ? (size_t)(amp - q) : strlen(q);

		if (pair_len > key_len && q[key_len] == '=' && strncmp(q, key, key_len) == 0) {
			const char *v = q + key_len + 1;
			size_t vlen = pair_len - key_len - 1;
			size_t oi = 0;
			size_t vi = 0;

			while (vi < vlen) {
				char c = v[vi];

				if (c == '%' && vi + 2 < vlen && isxdigit((unsigned char)v[vi + 1]) &&
				    isxdigit((unsigned char)v[vi + 2])) {
					char hex[3] = { v[vi + 1], v[vi + 2], '\0' };

					c = (char)strtol(hex, NULL, 16);
					vi += 3;
				} else {
					vi++;
				}
				if (oi + 1 >= out_size)
					return -1;
				out[oi++] = c;
			}
			out[oi] = '\0';
			return 0;
		}
		q = amp != NULL ? amp + 1 : q + pair_len;
	}
	return -1;
}
