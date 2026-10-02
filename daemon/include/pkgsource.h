#ifndef PKGSOURCE_H
#define PKGSOURCE_H

#include "json.h"

#include <stddef.h>

/*
 * ADR-0324: where a host's recipes come from -- any number of sources,
 * as a Debian machine lists any number of archives.
 *
 * A source is one git repository on a forge. Each has a ROLE: read (the
 * default) or write, where write means this host may commit recipes it
 * writes into it (ADR-0323). And each says whether it may hand this host
 * signing keys (trust_keys), off by default: a source that may supply
 * keys is a source that may vouch for packages.
 *
 * OWNERSHIP IS COMPUTED FROM WHAT SOURCES OFFER, PLUS THE OPERATOR'S
 * CHOICES. Each sync records what every source OFFERS -- the package,
 * image and deployment names it carries -- in a file per source. A
 * choice the operator made decides; without one, a name offered by one
 * source belongs to it, and a name offered by several belongs to none:
 * the sync merges it from no source and says so. List order never
 * decides, because "first wins" is a choice nobody made. Publishing a
 * recipe here with a source named (or the only source implied) records
 * that as a choice, so a recipe no sync has seen yet still has an owner
 * -- and with it the token its fetch needs. The choices are the only
 * stored ownership; a source that stops offering an unchosen name
 * simply stops owning it.
 */

#define PKG_SOURCES_MAX 32
#define PKG_SOURCE_NAME_MAX 64
#define PKG_SOURCE_URL_MAX 512
#define PKG_SOURCE_KIND_MAX 16 /* "gitea" / "github" / "gitlab" */
#define PKG_SOURCE_REF_MAX 128
#define PKG_SOURCE_TOKEN_MAX 256
/* A minisign public key file: a comment line and one base64 line. */
#define PKG_SOURCE_CATALOGUE_KEY_MAX 256
/* "<kind>:<name>", kind being package, image or deployment. */
#define PKG_SOURCE_ITEM_MAX 160

/* ADR-0315: the source a host that never saved one starts on. */
#define PKG_SOURCE_DEFAULT_NAME "cix-public"
#define PKG_SOURCE_DEFAULT_URL "https://github.com/The-Cix-Project/cix-recipes"
#define PKG_SOURCE_DEFAULT_KIND "github"
#define PKG_SOURCE_DEFAULT_REF "main"

struct pkg_source {
	char name[PKG_SOURCE_NAME_MAX];
	char url[PKG_SOURCE_URL_MAX];
	char kind[PKG_SOURCE_KIND_MAX];
	char ref[PKG_SOURCE_REF_MAX];
	char token[PKG_SOURCE_TOKEN_MAX]; /* never reported, never logged */
	int write;
	int trust_keys;
	/* ADR-0324 step C: the minisign public key this source's recipe index
	 * must verify against, or "" -- unsigned, as every source was before. */
	char catalogue_key[PKG_SOURCE_CATALOGUE_KEY_MAX];
};

enum pkgsource_error {
	PKGSOURCE_OK = 0,
	PKGSOURCE_ERR_INVALID, /* a field is malformed; the message says which */
	PKGSOURCE_ERR_EXISTS,
	PKGSOURCE_ERR_NOT_FOUND,
	PKGSOURCE_ERR_FULL,
	PKGSOURCE_ERR_PERSIST
};

/*
 * Loads the list from `path`. When it does not exist yet, the single
 * repository configuration ADR-0121 kept at `legacy_path` becomes the
 * first entry -- named after its repository, read or write as its commit
 * switch said, and trusted for keys, because it is where this host has
 * always taken its keys from -- and the legacy file is removed. A host
 * with neither starts on ADR-0315's public catalogue, trusted for keys.
 * A saved empty list stays empty: clearing is permanent.
 * `offers_dir` holds what each source last offered.
 */
int pkgsource_init(const char *path, const char *legacy_path, const char *offers_dir);
/* Paths only, when rebuildable storage moves (ADR-0141). */
void pkgsource_repoint(const char *path, const char *offers_dir);

/*
 * ADR-0257's one-time migration of a sync interval into a schedule: the
 * value a migrated legacy repo config carried, or 0. Cleared once the
 * schedule exists.
 */
int pkgsource_legacy_sync_interval_seconds(void);
void pkgsource_clear_legacy_sync_interval(void);

/*
 * The source a legacy repo config was migrated into by this boot's
 * pkgsource_init(), or NULL. The caller seeds that source's offers from
 * the recipe store (everything there came from or was added for the one
 * source the host had), so ownership -- and the token a fetch needs --
 * holds from the first boot instead of from the first sync. That sync
 * then replaces the seed with what git really carries.
 */
const char *pkgsource_migrated(void);

int pkgsource_count(void);
const struct pkg_source *pkgsource_at(int i);
const struct pkg_source *pkgsource_find(const char *name);

/*
 * Adds a source. Every refusal writes its reason to err: a name that is
 * not [a-z0-9][a-z0-9._-]*, a duplicate, an empty url, a kind other than
 * gitea/github/gitlab, or write on a kind with no commit client (gitea
 * is the only one).
 */
enum pkgsource_error pkgsource_add(const struct pkg_source *s, char *err, size_t err_size);

/*
 * Partial update of one source: a NULL string, or -1 for write and
 * trust_keys, leaves that field as it is; "" for token clears it. The
 * name never changes. Validated as add is.
 */
enum pkgsource_error pkgsource_update(const char *name, const char *url, const char *kind,
                                      const char *ref, const char *token, int write,
                                      int trust_keys, char *err, size_t err_size);

/* Removes a source, its offers and every choice that named it. */
enum pkgsource_error pkgsource_remove(const char *name, char *err, size_t err_size);

/* The redacted list, for GET /v1/pkg/sources: token_set, never token. */
void pkgsource_write_json(struct json_writer *w);
/* The bare list, as the config document's package_sources section (ADR-0292). */
void pkgsource_write_json_list(struct json_writer *w);

/* [a-z0-9][a-z0-9._-]*, under PKG_SOURCE_NAME_MAX: the rule for every
 * name in a list ADR-0324 keeps -- recipe sources and package
 * repositories alike. */
int pkgsource_name_is_valid(const char *name);

/* Whether `kind` has a commit client (ADR-0323). */
int pkgsource_kind_can_write(const char *kind);

/*
 * ADR-0324 step C: sets the public key a source's signed index must
 * verify against; "" clears it, and the source syncs unsigned. A key
 * that is not a minisign public key file is refused. Changing the key
 * forgets the last index time this host accepted from the source, so a
 * rotated key starts fresh rather than stranding the host at the old
 * key's newest timestamp.
 */
enum pkgsource_error pkgsource_set_catalogue_key(const char *name, const char *key, char *err,
                                                 size_t err_size);

/*
 * The time of the newest signed index this host has accepted from
 * `source` (0 for none), and recording a newer one. A sync refuses an
 * index older than this: an attacker replaying an old, genuinely signed
 * tree would otherwise withhold every newer recipe.
 */
long long pkgsource_catalogue_time(const char *source);
int pkgsource_catalogue_time_set(const char *source, long long t);

/* 1 when this boot created the default public source (a host that never
 * saved a list), so the caller can give it its built-in catalogue key. */
int pkgsource_created_default(void);

/* ---- what each source offers, and who owns a name ---- */

/*
 * Records what `source` offered in its last successful sync: one
 * "<kind>:<name>" per item. A source whose fetch failed keeps what it
 * offered before, so a network failure never moves ownership.
 */
int pkgsource_offers_write(const char *source, const char *const *items, int count);

/*
 * Adds one item to what `source` offers, as a commit to that source
 * makes true before the next sync can see it (ADR-0323). No-op when it
 * is already offered.
 */
int pkgsource_offers_add(const char *source, const char *item);

/* Re-reads every source's offers file, after a sync. */
int pkgsource_offers_load(void);

enum pkgsource_owner {
	PKGSOURCE_OWNER_NONE = 0, /* no source offers it */
	PKGSOURCE_OWNER_ONE,      /* exactly one source, or the operator's choice among several */
	PKGSOURCE_OWNER_CONFLICT  /* several offer it and the operator has not chosen */
};

/*
 * Who owns `item` ("<kind>:<name>"). For OWNER_ONE the source's name is
 * written to out; for OWNER_CONFLICT the offering sources are, joined by
 * ", ", so a caller can name them.
 */
enum pkgsource_owner pkgsource_owner_of(const char *item, char *out, size_t out_size);

/*
 * The source `item` is published or committed under, for every path
 * that does so -- one resolution, so add and commit cannot disagree.
 * An owned item stays with its owner, and naming another is refused. An
 * unowned or contested item takes `requested` when given; otherwise
 * the one candidate source (the one writable source, with
 * writable_only), and a host with no source at all publishes with no
 * owner (out ""). *choose is set when the answer is not yet stored
 * ownership, so the caller records it with pkgsource_choose() once the
 * recipe has landed. Every refusal writes its reason to err.
 */
int pkgsource_resolve(const char *item, const char *requested, int writable_only, char *out,
                      size_t out_size, int *choose, char *err, size_t err_size);

/*
 * The operator's choice of source for an item: it decides ownership
 * whatever the sources offer (see above). source "" or
 * NULL clears it. Refused (with err) for a source that does not exist.
 */
enum pkgsource_error pkgsource_choose(const char *item, const char *source, char *err,
                                      size_t err_size);

/* GET /v1/pkg/source-ownership: every conflict and every choice. */
void pkgsource_write_ownership_json(struct json_writer *w);

#endif /* PKGSOURCE_H */
