#ifndef PKGPOLICY_H
#define PKGPOLICY_H

#include "json.h"

/*
 * Issue #64: per-package rolling policy.
 *
 * Every omitted-version resolution in this daemon -- a plain `pkg
 * install`, dependency resolution, `update-all`, a follow_rolling
 * image's auto-rebuild, the "available" column -- has always used one
 * fixed rule: dpkg-style HIGHEST version wins (ADR-0107). That is the
 * right default for a rolling-release platform and stays the default.
 *
 * It is not always the right answer, and this project has the scar to
 * prove it: the Part 201 toolchain work published gcc 4.7.4 and 6.4.0
 * *after* 16.2.0 already existed, so for gcc the highest version and
 * the newest published diverge -- and neither is what an operator
 * walking a bootstrap chain actually wants installed. They want one
 * specific link of the chain, held.
 *
 * Three policies, per package name:
 *   highest  -- the default, unchanged behaviour.
 *   newest   -- the most recently PUBLISHED recipe wins, for names
 *               where publication order is the real intent. A recipe
 *               version's file is written exactly once and never
 *               touched again (ADR-0107 immutability), so its mtime is
 *               a real first-published timestamp, not an approximation.
 *   pinned   -- an explicit version that nothing may bump. A real hold:
 *               update-all and follow_rolling resolve to the pinned
 *               version, so there is never an update to apply.
 *
 * Policy is operator state, not recipe content -- a recipe cannot know
 * which link of a chain a particular box is meant to sit on.
 */

enum pkg_policy_kind {
	PKG_POLICY_HIGHEST = 0,
	PKG_POLICY_NEWEST,
	PKG_POLICY_PINNED
};

/* Loads from path; a missing or malformed file leaves an empty set
 * (every package on the default policy) rather than failing to boot. */
int pkgpolicy_init(const char *path);
void pkgpolicy_repoint(const char *path);

/*
 * The policy for name, and (for pinned) the version it is held at.
 * out_version may be NULL. Returns PKG_POLICY_HIGHEST for any name
 * with no policy set, which is what makes "no policy" and "the default
 * policy" the same thing rather than two states to keep in step.
 */
enum pkg_policy_kind pkgpolicy_get(const char *name, char *out_version, size_t out_version_size);

/* version is required for pinned, ignored otherwise. Returns -1 on an
 * invalid combination, 0 on success (persisted immediately). */
int pkgpolicy_set(const char *name, enum pkg_policy_kind kind, const char *version);

/* Removes any policy for name -- i.e. back to the default. */
int pkgpolicy_clear(const char *name);

const char *pkgpolicy_kind_name(enum pkg_policy_kind kind);
int pkgpolicy_kind_parse(const char *s, enum pkg_policy_kind *out);

void pkgpolicy_write_json(struct json_writer *w);

#endif /* PKGPOLICY_H */
