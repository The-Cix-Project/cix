#ifndef SRCGITEA_H
#define SRCGITEA_H

#include <stddef.h>

#include "srcupstream.h"

/*
 * ADR-0323: the gitea-tags discovery kind -- a project's releases are
 * the tags of its Gitea repository.
 *
 * Unlike kernel.org, whose one feed serves every package that follows
 * it, a gitea-tags feed belongs to ONE package: the recipe names its
 * repository through the CPDL upstream block's `source` template, and
 * its tag spelling through `tag`:
 *
 *   upstream "gitea-tags" {
 *       tag "v{version}"
 *       source "https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/hibr/archive/v{version}.tar.gz"
 *       verify "origin"
 *   }
 *
 * (That example is the shape an own-forge recipe needs, and cbs
 * v0.1.102 refuses it: its template check reads {{REPO_TOKEN}} as an
 * unknown placeholder -- cix-build-system#280. Until that ships, only a
 * source that needs no token can be declared.)
 *
 * cbs checks that `source`, expanded with the recipe's own version, is
 * exactly its main source url (cix-build-system v0.1.102, validate.c).
 * So the source template is both where the repository is read from and
 * how the author stage writes the next revision's url: one fact, never
 * a url string-edited from the previous one.
 *
 * THE ARCHIVE URL SHAPE IS THIS KIND'S CONTRACT. A gitea-tags source is
 * Gitea's API archive route, <base>/api/v1/repos/<owner>/<repo>/archive/
 * <tag>.tar.gz -- the one route that serves a tag archive to a token
 * (measured on 192.168.15.95, 2026-10-02: the web route answers 404
 * without a token, and so does the API route). The tags listing lives
 * beside it at <base>/api/v1/repos/<owner>/<repo>/tags. A source of any
 * other shape is refused by name rather than guessed at.
 *
 * Pure apart from the cache: no network. The fetch is the refresh
 * path's, in a helper process (main.c), which hands the listing to
 * srcgitea_store_listing().
 */

/*
 * The tags listing URL for a source template: everything up to and
 * including /api/v1/repos/<owner>/<repo>, then "/tags?limit=50&page=1".
 * Gitea caps a page at 50 and lists the newest tag first (measured on
 * 192.168.15.95, 2026-10-02: limit=100 returned 50 of 86, v0.99.5
 * first), so one page covers SRCUPSTREAM_MAX_CANDIDATES. 0, or -1 when
 * the template is not a Gitea API archive url.
 */
int srcgitea_tags_url(const char *source, char *out, size_t out_size);

/*
 * The version a tag names under template `tmpl` ("v{version}" maps
 * "v0.99.4" to "0.99.4"). 0, or -1 when the tag does not match the
 * template's fixed text, or the version would be empty or contain a
 * '/', a space or a quote.
 */
int srcgitea_version_of_tag(const char *tag, const char *tmpl, char *out, size_t out_size);

/*
 * Maps a Gitea tags listing (the JSON array GET .../tags returns) to
 * versions through `tmpl`, skipping tags the template does not match.
 * Writes at most `max` and returns how many, or -1 when the document
 * is not a JSON array.
 */
int srcgitea_versions_from_listing(const char *json, size_t len, const char *tmpl,
                                   char out[][SRCUPSTREAM_VERSION_MAX], size_t max);

/* The directory the per-package cache lives in (<dir>/<package>.json). */
int srcgitea_init(const char *dir);

/*
 * Stores what a refresh found for `package`: the listing mapped through
 * `tmpl`, and the time. 0, or -1 when the listing is not a tags array
 * or the cache cannot be written.
 */
int srcgitea_store_listing(const char *package, const char *json, size_t len, const char *tmpl,
                           long now);

/* The cached versions for `package`; 0 when it was never fetched. */
size_t srcgitea_candidates(const char *package, char out[][SRCUPSTREAM_VERSION_MAX], size_t max);

/* When `package`'s listing was last stored; 0 means never. */
long srcgitea_fetched_at(const char *package);

/*
 * Records why `package`'s listing could not be read this time -- no
 * source template, a fetch that failed, a document that is not a
 * listing -- keeping what an earlier refresh stored. A later successful
 * store clears it. 0, or -1.
 */
int srcgitea_store_error(const char *package, const char *why, long now);

/* The last recorded error for `package`, "" when none. 0, or -1. */
int srcgitea_error(const char *package, char *out, size_t out_size);

#endif /* SRCGITEA_H */
