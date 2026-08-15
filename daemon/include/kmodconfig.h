#ifndef KMODCONFIG_H
#define KMODCONFIG_H

#include "json.h"
#include "kmod.h"

/*
 * ADR-0159 Phase A: persisted per-module state -- a default `options`
 * value a bare POST /v1/system/kmod/{name} with no body falls back to,
 * and an `autoload` flag for load_configured_modules()'s own boot-time
 * pass. Same small, atomic-JSON-persisted-table shape devicemap.c and
 * sysctlconfig.c already established (X_init/X_repoint, linear-scan
 * entries) -- distinct from and unrelated to ADR-0061's own hardcoded,
 * hardware-detection module list (load_boot_modules()).
 */

#define KMODCONFIG_MAX 128

enum kmodconfig_error {
	KMODCONFIG_OK,
	KMODCONFIG_ERR_INVALID_NAME,
	KMODCONFIG_ERR_FULL,
	KMODCONFIG_ERR_NOT_FOUND,
	KMODCONFIG_ERR_PERSIST_FAILED,
};

int kmodconfig_init(const char *state_path);
void kmodconfig_repoint(const char *new_state_path);

/*
 * Read-modify-write, matching daemon-config's own established PUT
 * shape: options (a canonical "key=value ..." string, kmod_options_
 * from_json()'s own output) is applied only if options != NULL;
 * autoload is applied only if has_autoload is non-zero. Creates the
 * entry if it doesn't exist yet. Passing options == NULL and
 * has_autoload == 0 is a no-op update (still succeeds).
 */
enum kmodconfig_error kmodconfig_set(const char *name, const char *options, int has_autoload,
                                      int autoload_value);

/* Clears this module's persisted config entirely (both fields). */
enum kmodconfig_error kmodconfig_delete(const char *name);

/* NULL if name has no persisted entry. Never NULL for a module with an
 * entry but no default_options set -- "" (empty) in that case. */
const char *kmodconfig_get_options(const char *name);

/* 0 if name has no persisted entry or autoload is false. */
int kmodconfig_get_autoload(const char *name);

/* Writes {"name","default_options":{...},"autoload":bool}. Returns 0
 * if name has no persisted entry (writes nothing). */
int kmodconfig_write_json_one(const char *name, struct json_writer *w);

void kmodconfig_write_json_list(struct json_writer *w);

/* load_configured_modules()'s own boot-time iteration: fn(name,
 * options, ctx) for every entry with autoload=true. options is never
 * NULL (empty string for "no default options"). */
void kmodconfig_foreach_autoload(void (*fn)(const char *name, const char *options, void *ctx),
                                  void *ctx);

#endif /* KMODCONFIG_H */
