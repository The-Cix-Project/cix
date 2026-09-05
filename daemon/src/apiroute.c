#include "apiroute.h"

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
