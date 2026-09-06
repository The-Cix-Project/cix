#ifndef SRCRESOLVE_H
#define SRCRESOLVE_H

#include "json.h"
#include "pipeline.h"
#include "srcpolicy.h"

#include <stddef.h>

/*
 * ADR-0255 stage 3, made visible: the SOURCE CATALOGUE.
 *
 * One row per package, answering the only question that matters before
 * anything is built: given the policy in force for this package, has
 * upstream published something we do not have a recipe for?
 *
 * WHY THIS IS COMPUTED, NEVER STORED.
 *
 * A row is a pure function of three inputs that all already have a home:
 * the kind's cached release list (srcupstream.h), the effective policy
 * (srcpolicy.h) and the recipes on disk (pkg.h). Persisting the result
 * would create a fourth copy that goes stale the instant a recipe is
 * published or a policy changes, with no event to invalidate it -- the
 * same shape as ADR-0155's image dedup, where a correct answer was
 * discarded because something else had already been recorded. So the
 * catalogue is recomputed on every read. That is also what makes
 * "manual invocation" free: reading it IS running it.
 *
 * The one thing a read does not do is fetch. Refreshing a kind's
 * release list is a network operation on the daemon's reactor and
 * belongs to that kind's own refresh endpoint (kernel.org:
 * POST /v1/system/kernel-releases). Every row reports the age of the
 * data it was resolved from, so stale is visible rather than implied.
 *
 * WHY THE STATE IS "DO WE HAVE IT", NOT "IS UPSTREAM NEWER".
 *
 * Newer/older is the wrong axis. The policy says which release we WANT;
 * the useful question is whether we HAVE it. Those come apart in a case
 * that is live on this platform today: with recipes for both 6.18.40-24
 * and 7.2.3-2 present, a longterm policy resolving to 6.18.46 needs a
 * recipe written even though the highest recipe on disk (7.2.3) is
 * numerically greater than the resolution. A "newer available" boolean
 * would say no. `missing` says yes, and both versions are reported so
 * the direction is never hidden behind the verdict.
 */

/*
 * THERE IS NO SEPARATE CATALOGUE VOCABULARY. ADR-0256 folded the four
 * states this module used to define -- pinned/current/missing/
 * unresolved -- into the platform's one (stage, status) pair, because
 * two names for one fact is precisely what that ADR exists to end. The
 * four map exactly, and the mapping is worth stating since the words
 * still describe real situations:
 *
 *   pinned      discover / not-implemented
 *               No pkg_upstream. Finding a new release of this project
 *               is something a person does, which is what
 *               `not-implemented` means, and it is a permanent and
 *               correct answer rather than a gap.
 *   current     author / ok
 *               A recipe exists for the resolved release.
 *   missing     author / blocked
 *               Resolution succeeded and nobody has written the recipe.
 *               Blocked on a person, not failed.
 *   unresolved  discover / failed  -- the release list was never fetched
 *               resolve  / failed  -- policy could not pick from it
 *               Two different causes needing opposite responses, which
 *               the old single `unresolved` could not tell apart.
 */

#define SRCRESOLVE_VERSION_MAX 64
#define SRCRESOLVE_REASON_MAX 256

struct srcresolve_entry {
	char name[64];
	char kind[32];                                 /* "" when pinned */
	char channel[SRCPOLICY_CHANNEL_MAX];
	char depth[SRCPOLICY_DEPTH_MAX];
	char resolved_version[SRCRESOLVE_VERSION_MAX];      /* "" unless resolved */
	char newest_recipe_version[SRCRESOLVE_VERSION_MAX]; /* "" if no recipe */
	long fetched_at;                               /* of the kind's data; 0 = never */
	enum pipeline_stage stage;
	enum pipeline_status status;
	char reason[SRCRESOLVE_REASON_MAX];
};

/*
 * The upstream half of a recipe version: "7.2.3-2" -> "7.2.3".
 *
 * ADR-0107 gives a recipe version the shape <upstream>-<revision>,
 * where the revision is this platform's own count of how many times we
 * have rebuilt that upstream release. Only the upstream half is
 * comparable with anything a project publishes, and only a TRAILING RUN
 * OF DIGITS after the last '-' is treated as a revision -- a version
 * with no such suffix ("1.0.8", "v2.53.82") is entirely upstream's.
 */
void srcresolve_upstream_of(const char *recipe_version, char *out, size_t out_size);

/*
 * Resolves one package. The recipe versions are passed in rather than
 * read here, which keeps this a pure function: a test can drive every
 * state without a recipe tree on disk, and the I/O lives in exactly one
 * place (srcresolve_write_json).
 *
 * `kind` NULL or "" means the recipe declares no upstream, which is
 * SRCRESOLVE_PINNED and not an error. Always fills *out.
 */
void srcresolve_one(const char *name, const char *kind,
                     const char *const *recipe_versions, size_t recipe_count,
                     struct srcresolve_entry *out);

/* The whole catalogue, recomputed from the recipes on disk. */
void srcresolve_write_json(struct json_writer *w);

#endif /* SRCRESOLVE_H */
