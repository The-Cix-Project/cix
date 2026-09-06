/*
 * See srcresolve.h for why the catalogue is computed on every read and
 * why its state answers "do we have it" rather than "is upstream
 * newer".
 */
#include "srcresolve.h"

#include "pkg.h"
#include "srcdepth.h"
#include "srcpolicy.h"
#include "srcupstream.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Enough for every package this platform has recipes for, with room. */
#define SRCRESOLVE_MAX_PACKAGES 512
#define SRCRESOLVE_MAX_RECIPE_VERSIONS 64

const char *srcresolve_state_name(enum srcresolve_state s)
{
	switch (s) {
	case SRCRESOLVE_PINNED:
		return "pinned";
	case SRCRESOLVE_CURRENT:
		return "current";
	case SRCRESOLVE_MISSING:
		return "missing";
	case SRCRESOLVE_UNRESOLVED:
	default:
		return "unresolved";
	}
}

void srcresolve_upstream_of(const char *recipe_version, char *out, size_t out_size)
{
	const char *dash;
	const char *p;
	size_t keep;

	if (out == NULL || out_size == 0)
		return;
	out[0] = '\0';
	if (recipe_version == NULL || recipe_version[0] == '\0')
		return;

	dash = strrchr(recipe_version, '-');
	/*
	 * A revision is a trailing run of digits and nothing else. "-2" is
	 * one; "-rc1" is upstream's own and stays, which is why this checks
	 * every character after the dash rather than just the first.
	 */
	if (dash != NULL && dash[1] != '\0') {
		for (p = dash + 1; *p != '\0'; p++) {
			if (!isdigit((unsigned char)*p))
				break;
		}
		if (*p == '\0') {
			keep = (size_t)(dash - recipe_version);
			if (keep >= out_size)
				keep = out_size - 1;
			memcpy(out, recipe_version, keep);
			out[keep] = '\0';
			return;
		}
	}
	snprintf(out, out_size, "%s", recipe_version);
}

/* The highest of the versions a package has recipes for. */
static void newest_recipe(const char *const *versions, size_t count, char *out, size_t out_size)
{
	size_t i;
	const char *best = NULL;

	out[0] = '\0';
	for (i = 0; i < count; i++) {
		if (versions[i] == NULL || versions[i][0] == '\0')
			continue;
		if (best == NULL || pkg_version_compare(versions[i], best) > 0)
			best = versions[i];
	}
	if (best != NULL)
		snprintf(out, out_size, "%s", best);
}

static void fail(struct srcresolve_entry *e, const char *fmt, ...)
{
	va_list ap;

	e->state = SRCRESOLVE_UNRESOLVED;
	va_start(ap, fmt);
	vsnprintf(e->reason, sizeof(e->reason), fmt, ap);
	va_end(ap);
}

void srcresolve_one(const char *name, const char *kind,
                     const char *const *recipe_versions, size_t recipe_count,
                     struct srcresolve_entry *out)
{
	const struct srcupstream_kind *k;
	struct srcpolicy pol;
	char candidates[SRCUPSTREAM_MAX_CANDIDATES][SRCUPSTREAM_VERSION_MAX];
	const char *candp[SRCUPSTREAM_MAX_CANDIDATES];
	char err[SRCRESOLVE_REASON_MAX];
	char upstream[SRCRESOLVE_VERSION_MAX];
	enum srcdepth_error de;
	size_t count, i;
	int lines, releases;

	memset(out, 0, sizeof(*out));
	snprintf(out->name, sizeof(out->name), "%s", name != NULL ? name : "");
	newest_recipe(recipe_versions, recipe_count, out->newest_recipe_version,
	              sizeof(out->newest_recipe_version));

	if (kind == NULL || kind[0] == '\0') {
		out->state = SRCRESOLVE_PINNED;
		snprintf(out->reason, sizeof(out->reason),
		         "no pkg_upstream declared -- this package is pinned and rolls only "
		         "when someone publishes a new recipe for it");
		return;
	}
	snprintf(out->kind, sizeof(out->kind), "%s", kind);

	k = srcupstream_find(kind);
	if (k == NULL) {
		fail(out, "recipe declares upstream kind \"%s\", which this platform does not "
		          "implement -- nothing can enumerate that project's releases", kind);
		return;
	}
	out->fetched_at = srcupstream_fetched_at(k);

	if (srcpolicy_effective_for_kind(name, kind, &pol, err, sizeof(err)) != 0) {
		snprintf(out->channel, sizeof(out->channel), "%s", pol.channel);
		snprintf(out->depth, sizeof(out->depth), "%s", pol.depth);
		fail(out, "%s", err);
		return;
	}
	snprintf(out->channel, sizeof(out->channel), "%s", pol.channel);
	snprintf(out->depth, sizeof(out->depth), "%s", pol.depth);

	if (srcdepth_parse(pol.depth, &lines, &releases) != SRCDEPTH_OK) {
		fail(out, "depth \"%s\" is not a depth expression", pol.depth);
		return;
	}

	count = srcupstream_candidates(k, pol.channel, candidates, SRCUPSTREAM_MAX_CANDIDATES);
	if (count == 0) {
		/*
		 * "never fetched" and "the channel publishes nothing" both
		 * arrive here as zero candidates and need opposite responses,
		 * so they are never reported as the same thing.
		 */
		if (out->fetched_at == 0)
			fail(out, "%s release data has never been fetched on this host -- "
			          "refresh it, then read this catalogue again", kind);
		else
			fail(out, "%s currently publishes nothing in the \"%s\" channel", kind,
			     pol.channel);
		return;
	}
	for (i = 0; i < count; i++)
		candp[i] = candidates[i];

	de = srcdepth_resolve(candp, count, lines, releases, out->resolved_version,
	                      sizeof(out->resolved_version), err, sizeof(err));
	if (de != SRCDEPTH_OK) {
		out->resolved_version[0] = '\0';
		/*
		 * kernel.org's releases.json lists only the newest release of
		 * each line, so a depth reaching back WITHIN a line has nothing
		 * to reach for. That is a property of the feed, not a mistake
		 * by whoever set the policy, and saying so is the difference
		 * between a fixable message and a dead end.
		 */
		if (de == SRCDEPTH_ERR_NO_SUCH_RELEASE)
			fail(out, "%s -- %s publishes only the newest release of each line, so "
			          "a depth reaching back within a line cannot resolve against it",
			     err, kind);
		else
			fail(out, "%s", err);
		return;
	}

	for (i = 0; i < recipe_count; i++) {
		srcresolve_upstream_of(recipe_versions[i], upstream, sizeof(upstream));
		if (strcmp(upstream, out->resolved_version) == 0) {
			out->state = SRCRESOLVE_CURRENT;
			snprintf(out->reason, sizeof(out->reason),
			         "recipe %s already builds %s", recipe_versions[i],
			         out->resolved_version);
			return;
		}
	}
	out->state = SRCRESOLVE_MISSING;
	if (out->newest_recipe_version[0] == '\0')
		snprintf(out->reason, sizeof(out->reason),
		         "%s resolves to %s and this platform has no recipe for it at all",
		         out->channel[0] != '\0' ? out->channel : kind, out->resolved_version);
	else
		snprintf(out->reason, sizeof(out->reason),
		         "%s resolves to %s and no recipe builds it (newest recipe is %s)",
		         out->channel[0] != '\0' ? out->channel : kind, out->resolved_version,
		         out->newest_recipe_version);
}

static void write_entry(struct json_writer *w, const struct srcresolve_entry *e)
{
	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, e->name);
	jw_key(w, "state");
	jw_str(w, srcresolve_state_name(e->state));
	jw_key(w, "upstream");
	if (e->kind[0] == '\0')
		jw_null(w);
	else
		jw_str(w, e->kind);
	jw_key(w, "channel");
	if (e->channel[0] == '\0')
		jw_null(w);
	else
		jw_str(w, e->channel);
	jw_key(w, "depth");
	if (e->depth[0] == '\0')
		jw_null(w);
	else
		jw_str(w, e->depth);
	/*
	 * Both versions, always, so the direction between them is visible
	 * without a caller having to infer it from the state.
	 */
	jw_key(w, "resolved_version");
	if (e->resolved_version[0] == '\0')
		jw_null(w);
	else
		jw_str(w, e->resolved_version);
	jw_key(w, "newest_recipe_version");
	if (e->newest_recipe_version[0] == '\0')
		jw_null(w);
	else
		jw_str(w, e->newest_recipe_version);
	jw_key(w, "upstream_fetched_at");
	if (e->fetched_at == 0)
		jw_null(w);
	else
		jw_num(w, (double)e->fetched_at);
	jw_key(w, "reason");
	jw_str(w, e->reason);
	jw_obj_close(w);
}

void srcresolve_write_json(struct json_writer *w)
{
	static char names[SRCRESOLVE_MAX_PACKAGES][PKG_IMAGE_NAME_MAX];
	static char versions[SRCRESOLVE_MAX_RECIPE_VERSIONS][PKG_VERSION_MAX];
	const char *vp[SRCRESOLVE_MAX_RECIPE_VERSIONS];
	struct srcresolve_entry e;
	int counts[4];
	int n, i, v, nv;

	memset(counts, 0, sizeof(counts));
	n = pkg_recipe_list_names(names, SRCRESOLVE_MAX_PACKAGES);

	jw_obj_open(w);
	jw_key(w, "packages");
	jw_arr_open(w);
	for (i = 0; i < n; i++) {
		char kind[32];

		if (pkg_recipe_upstream(names[i], kind, sizeof(kind)) != 0)
			kind[0] = '\0';
		nv = pkg_recipe_list_versions(names[i], versions, SRCRESOLVE_MAX_RECIPE_VERSIONS);
		for (v = 0; v < nv; v++)
			vp[v] = versions[v];
		srcresolve_one(names[i], kind, vp, (size_t)nv, &e);
		counts[e.state]++;
		write_entry(w, &e);
	}
	jw_arr_close(w);
	/*
	 * The summary a catalogue page reads before it renders a single
	 * row. Computed here rather than by each client, so two clients
	 * cannot disagree about how many packages need attention.
	 */
	jw_key(w, "total");
	jw_num(w, (double)n);
	jw_key(w, "pinned");
	jw_num(w, (double)counts[SRCRESOLVE_PINNED]);
	jw_key(w, "current");
	jw_num(w, (double)counts[SRCRESOLVE_CURRENT]);
	jw_key(w, "missing");
	jw_num(w, (double)counts[SRCRESOLVE_MISSING]);
	jw_key(w, "unresolved");
	jw_num(w, (double)counts[SRCRESOLVE_UNRESOLVED]);
	jw_obj_close(w);
}
