#ifndef KERNELRECIPE_H
#define KERNELRECIPE_H

#include <stddef.h>

/*
 * Generating the kernel recipe revision a channel proposes (#65).
 *
 * This is the last link of the chain ADR-0254 unblocked: a channel
 * resolves to an upstream version, kernel.org's clearsigned
 * sha256sums.asc supplies that version's checksum, and this turns the
 * pair into a publishable recipe revision. Publishing is what the
 * EXISTING rolling machinery already reacts to -- an image carrying
 * `kernel` as `mode: rolling` is rebuilt by queue_rolling_rebuilds_for()
 * with no further help.
 *
 * Deliberately a pure text transform, taking the current recipe and
 * returning the new one. No fetching, no verification, no writing: the
 * caller has already verified the checksum before it gets here, and
 * keeping this side-effect-free is what makes it testable without a
 * network, a key, or a daemon.
 *
 * WHAT IT MUST NOT BREAK, and why each is a real hazard:
 *
 *   - the kernel recipe has TWO sources -- the upstream tarball and
 *     this project's own config fragment, fetched from the forge with
 *     a {{REPO_TOKEN}} placeholder -- and two space-separated
 *     checksums. Only the FIRST of each may move. Rewriting the second
 *     would either drop the config or invalidate its hash.
 *   - the config source is pinned to a git ref. That ref is this
 *     platform's own decision about which config to build with, and an
 *     upstream version bump is not a reason to change it.
 *   - everything else in the file is carried forward byte for byte.
 *     A generator that reformats a recipe makes every future diff
 *     unreadable, and the recipe body is where the build actually
 *     lives.
 */

/*
 * current_recipe   the newest kernel recipe's build.sh text
 * new_version      the recipe version to write, e.g. "7.2.3-1"
 * tarball_url      the upstream tarball URL for that version
 * tarball_sha256   its checksum, ALREADY verified against a signature
 * out              malloc'd new recipe text; caller frees. Only set on
 *                  success.
 *
 * Returns 0 on success, -1 with err filled in otherwise.
 */
int kernel_recipe_generate(const char *current_recipe, const char *new_version,
                           const char *tarball_url, const char *tarball_sha256, char **out,
                           char *err, size_t err_size);

/*
 * "7.2.3" -> "https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz"
 * and the directory that version's sha256sums.asc lives in.
 * Returns 0 on success, -1 if version is not a kernel version.
 */
int kernel_upstream_urls(const char *version, char *out_tarball, size_t tarball_size,
                         char *out_sums, size_t sums_size);

#endif /* KERNELRECIPE_H */
