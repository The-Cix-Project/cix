#ifndef SRCPOLICY_H
#define SRCPOLICY_H

#include "json.h"

#include <stddef.h>

/*
 * ADR-0255: which upstream release a package builds -- the SOURCE
 * policy axis.
 *
 * Deliberately not the same axis as [ADR-0188](pkgpolicy.h), and the
 * two must never be merged into one setting even though both use the
 * word "pinned":
 *
 *   source policy   (here)        which upstream RELEASE do we build?
 *   artifact policy (pkgpolicy.h) which BUILT artifact does an image take?
 *
 * Source decides what gets built; artifact decides what gets consumed.
 * A package can roll its source while an image holds an older artifact,
 * which is exactly the control a staged rollout needs -- collapsing
 * them would make that unexpressible.
 *
 * A policy is (channel, depth):
 *   channel  which stream, when the upstream publishes several. The
 *            valid values come from the discovery kind the recipe
 *            declares (srcupstream.h), never from free text.
 *   depth    how far back in that stream (srcdepth.h), "n" by default.
 *
 * THE DEFAULT ACTS. ADR-0255 settles this: a rollable recipe rolls
 * without being named package by package, because a default that has
 * to be set 116 times is a feature that exists and is off. What makes
 * that safe is that rolling stops short of the running host -- it moves
 * artifacts and images, which are cheap to rebuild and discard, and
 * never a booted machine.
 *
 * The platform-wide default supplies a channel PREFERENCE, not a
 * mandate. It is applied where the package's own kind actually
 * publishes that channel, and ignored where it does not -- a kind with
 * a single linear release sequence has no channel to set, and one whose
 * channels do not include the preferred name must be told explicitly.
 * That keeps a global setting from silently meaning different things to
 * different upstreams.
 */

#define SRCPOLICY_CHANNEL_MAX 32
#define SRCPOLICY_DEPTH_MAX 16

struct srcpolicy {
	char channel[SRCPOLICY_CHANNEL_MAX]; /* "" = not chosen */
	char depth[SRCPOLICY_DEPTH_MAX];     /* always set; "n" when unspecified */
	int explicit_entry;                  /* 1 if this package has its own, 0 if inherited */
};

int srcpolicy_init(const char *path);
void srcpolicy_repoint(const char *path);

/* The platform-wide default. depth defaults to "n", channel to "". */
void srcpolicy_default_get(struct srcpolicy *out);
int srcpolicy_default_set(const char *channel, const char *depth, char *err, size_t err_size);

/*
 * The effective policy for a package: its own entry if it has one, else
 * the platform default. Always fills *out, so callers never have to
 * distinguish "no policy" from "the default policy" -- they are the
 * same thing, which is what keeps them from drifting apart.
 */
void srcpolicy_get(const char *name, struct srcpolicy *out);

/*
 * Sets a package's own policy. `kind_name` is the discovery kind from
 * its recipe and is what the channel is validated against; pass NULL to
 * skip that check only when the recipe genuinely declares none, in
 * which case a channel is refused outright.
 *
 * Returns 0 on success (persisted immediately), -1 with a reason in
 * `err` otherwise.
 */
int srcpolicy_set(const char *name, const char *kind_name, const char *channel,
                   const char *depth, char *err, size_t err_size);

int srcpolicy_clear(const char *name);

/*
 * Resolves the effective policy against a package's declared kind, the
 * way the pipeline's Resolve stage will.
 *
 * This is where a global default meets a specific upstream, and it is
 * the one place that can honestly answer "can this package roll right
 * now, and if not, why not". Returns 0 when the package has a usable
 * (channel, depth) for that kind, -1 with a reason otherwise.
 */
int srcpolicy_effective_for_kind(const char *name, const char *kind_name,
                                  struct srcpolicy *out, char *err, size_t err_size);

void srcpolicy_write_json(struct json_writer *w);
void srcpolicy_write_json_one(const char *name, struct json_writer *w);

#endif /* SRCPOLICY_H */
