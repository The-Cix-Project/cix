#ifndef KMOD_H
#define KMOD_H

#include "json.h"

/*
 * ADR-0159 Phase A: real modprobe(8)/modinfo(8) (kmod.recipe, staged
 * at /usr/bin/<tool> on every real image -- see that recipe's own
 * --sbindir=/usr/bin comment), wrapped exactly the way this project
 * already wraps sfdisk/openssl/curl elsewhere -- fork+execve, never a
 * raw init_module(2)/finit_module(2) reimplementation, since modprobe
 * already does real dependency resolution (via the harvested
 * modules.dep) and real module-parameter passing. Removal goes
 * through `modprobe -r`, not a bare rmmod, for the same
 * reverse-dependency-aware reason.
 */

#define KMOD_NAME_MAX 64
#define KMOD_OPTIONS_MAX 256

/* Non-empty, fits KMOD_NAME_MAX, no '/', no whitespace: the one shape
 * every one of modprobe/modinfo's own <name> arguments takes. */
int kmod_name_is_valid(const char *name);

/* modprobe <name> [key=value ...] -- options is a space-joined
 * "key=value key2=value2" string in modprobe's own native syntax (NULL
 * or "" for none). 0 on success, -1 if modprobe itself failed (module
 * not found/buildable, a rejected option, or the real binary isn't
 * present at all -- this project's own dev sandbox has no staged
 * modprobe, only a real installed box does). */
int kmod_load(const char *name, const char *options);

/* modprobe -r <name>. Same success/failure convention as kmod_load(). */
int kmod_unload(const char *name);

/* Reads /proc/modules directly (a plain, always-present kernel text
 * interface -- no fork needed at all, unlike load/unload/info) and
 * writes a JSON array of {"name","size","used_by_count","used_by":
 * [...],"state"} for every currently-loaded module, live, never
 * cached -- matching disk.c's own "re-enumerate fresh on every call"
 * convention. */
void kmod_write_json_loaded(struct json_writer *w);

/* Real `modinfo <name>` output, parsed into a JSON object -- the one
 * place modinfo's own output can't be replaced by /proc/modules, which
 * only ever shows what's currently *loaded*, not what's available to
 * load. Writes {"name","filename","description","version","license",
 * "author","depends":[...],"in_tree":bool,"params":[{"name","type",
 * "description"},...]} (any field modinfo didn't report is simply
 * omitted, except depends/params which are always present, possibly
 * empty). Returns 0 on success, -1 if modinfo itself failed (module
 * not built/found, or modinfo isn't present at all). */
int kmod_write_json_info(const char *name, struct json_writer *w);

/* Converts a JSON object of string values ({"key":"value",...}) into
 * the canonical space-joined "key=value key2=value2" form kmod_load()
 * and the persisted kmod-config default_options both use. An empty or
 * NULL object yields an empty string. -1 if jval is non-NULL but isn't
 * a JSON_OBJECT, or any value isn't a plain string, or the result
 * would not fit out_size. */
int kmod_options_from_json(const struct json_value *jval, char *out, size_t out_size);

/* The inverse: parses a canonical "key=value key2=value2" string
 * (empty/NULL for none) and writes a JSON object. A malformed token
 * (no '=') is written with an empty string value rather than dropped,
 * so round-tripping never silently loses data. */
void kmod_options_write_json(const char *options, struct json_writer *w);

#endif /* KMOD_H */
