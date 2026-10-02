#ifndef PKGREPO_H
#define PKGREPO_H

#include "json.h"
#include "pkgsource.h"

#include <stddef.h>

/*
 * ADR-0324 step B: where a host's built packages come from -- any
 * number of package repositories, as recipes come from any number of
 * sources (pkgsource.h).
 *
 * A repository is an artifact server: a base url the published
 * artifact names are appended to, an optional token, and whether this
 * host PUSHES what it builds there (#129). They are MIRRORS: a built
 * package is accepted on its recipe's artifact_sha256 and its signature
 * (ADR-0279), never on where it came from, so the list is tried in
 * order for speed and a copy that fails verification is refused and
 * the next one tried. Order decides which copy arrives first; it can
 * never change what is accepted.
 *
 * Edits share pkgsource's error type and name rule: one way for a list
 * edit to fail, one rule for what a name may be.
 */

#define PKG_REPOSITORIES_MAX 32
#define PKG_REPOSITORY_URL_MAX 512
#define PKG_REPOSITORY_TOKEN_MAX 256

/* ADR-0315: the repository a host that never saved one starts on. */
#define PKG_REPOSITORY_DEFAULT_NAME "cix-public"
#define PKG_REPOSITORY_DEFAULT_URL "https://cache.cix.world"

struct pkg_repository {
	char name[PKG_SOURCE_NAME_MAX];
	char url[PKG_REPOSITORY_URL_MAX];
	char token[PKG_REPOSITORY_TOKEN_MAX]; /* never reported, never logged */
	int push;
};

/*
 * Loads the list from `path`. When it does not exist yet, the single
 * artifact server ADR-0122 kept at `legacy_path` becomes the first
 * entry -- named after its host, `:` written `-`, with its token and
 * push switch -- and the legacy file is removed; a legacy config whose
 * url was cleared becomes an empty list. A host with neither starts on
 * ADR-0315's public cache, pull-only. A saved empty list stays empty:
 * clearing is permanent.
 */
int pkgrepo_init(const char *path, const char *legacy_path);
/* Path only, when rebuildable storage moves (ADR-0141). */
void pkgrepo_repoint(const char *path);

int pkgrepo_count(void);
const struct pkg_repository *pkgrepo_at(int i);
const struct pkg_repository *pkgrepo_find(const char *name);
/* How many repositories this host pushes to. */
int pkgrepo_push_count(void);

/* Every refusal writes its reason to err: a bad name, a duplicate, a url without a scheme. */
enum pkgsource_error pkgrepo_add(const struct pkg_repository *r, char *err, size_t err_size);

/*
 * Partial update: NULL, or -1 for push, leaves a field as it is; ""
 * for token clears it. The name never changes.
 */
enum pkgsource_error pkgrepo_update(const char *name, const char *url, const char *token,
                                    int push, char *err, size_t err_size);

enum pkgsource_error pkgrepo_remove(const char *name, char *err, size_t err_size);

/* GET /v1/pkg/repositories: {"repositories":[...]}, token_set and never token. */
void pkgrepo_write_json(struct json_writer *w);
/* The bare list, as the config document's package_repositories section (ADR-0292). */
void pkgrepo_write_json_list(struct json_writer *w);

#endif /* PKGREPO_H */
