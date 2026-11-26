#ifndef CIX_DEPSECRET_H
#define CIX_DEPSECRET_H

#include "json.h"

/*
 * depsecret -- the secrets each deployment was applied with, kept so a
 * re-apply needs none typed again (ADR-0392, #724).
 *
 * One file, `{"<deployment>": {"KEY": "value", ...}, ...}`, written
 * 0600 through persist_atomic_write_private(). Read on demand: an apply
 * is rare, and keeping the values out of the daemon's memory between
 * applies is no loss.
 *
 * No read of the API returns a value from here. The values are already
 * at rest in the container definitions a deployment renders into
 * (containerdef), so this file adds no new exposure; it is the record of
 * which value belongs to which {{SECRET:KEY}}, which a rendered body
 * cannot give back.
 */

/*
 * Writes into w one JSON object: every string member of `given`, then
 * every kept secret of `deployment` that `given` does not name. `given`
 * may be NULL. The result is what deployment_render() takes as
 * its secrets. Returns the number of kept secrets it added.
 */
int depsecret_merge(const char *deployment, const struct json_value *given,
                    struct json_writer *w);

/* Keeps every string member of `given` for `deployment`, replacing any
 * kept value of the same key. Returns 0, or -1 if the file could not be
 * written (the apply goes on; a later one then asks for the secret). */
int depsecret_keep(const char *deployment, const struct json_value *given);

/* Forgets every kept secret of `deployment`, when the deployment itself
 * is removed. 0 when there was nothing to forget. */
int depsecret_forget(const char *deployment);

#endif
