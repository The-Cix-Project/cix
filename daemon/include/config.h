#ifndef CIX_CONFIG_H
#define CIX_CONFIG_H

#include "json.h"

/*
 * The running configuration as one document (ADR-0206).
 *
 * A VIEW, derived fresh on every call from the daemon's own live state.
 * Nothing here is stored: a stored rendering would be a second source of
 * truth for every setting, and would drift -- the exact failure `One
 * Source of Truth` exists to prevent, and one this project has already
 * been bitten by in its own documentation.
 *
 * The section list is NOT defined here. It is generated from the
 * ConfigDocument schema in docs/api/openapi.yaml (ADR-0206's fifth
 * point: the schema generates the vocabulary, nothing hand-maintains a
 * second copy of it), and config.c expands that generated X-macro to
 * build its renderer table. A section with no renderer, or a renderer
 * with no section, is a build error.
 *
 * Secrets are never rendered. Where a subsystem's own writer would emit
 * a token, key or password, config.c renders the redacted form instead.
 */
void config_write_document(struct json_writer *w, const char *containers_dir);

/* ---- The section table, for callers that need one section at a time ----
 *
 * POST /v1/config and POST /v1/config/diff (ADR-0292) work section by
 * section: each supplied section is rendered here, compared against
 * what the caller sent, and -- for a `replace` section -- applied.
 * They read the same table `config_write_document()` renders from, so
 * the document an operator fetches and the document they can send back
 * are the same vocabulary by construction, not by agreement.
 */

enum config_kind {
	CONFIG_KIND_OBJECT,
	CONFIG_KIND_ARRAY
};

enum config_apply_mode {
	/* One setter replaces the whole section. */
	CONFIG_APPLY_REPLACE,
	/* Applying means creating, updating and deleting individual live
	 * resources in dependency order -- not implemented (ADR-0292). */
	CONFIG_APPLY_RECONCILE
};

int config_section_count(void);
/* -1 when no section has that name. */
int config_section_index(const char *name);
const char *config_section_name(int i);
enum config_kind config_section_kind(int i);
/* The identity field(s) of an array section, comma-separated; "" for
 * an object section or an array compared by position. */
const char *config_section_key(int i);
enum config_apply_mode config_section_apply_mode(int i);

/*
 * The section's OBSERVED members, comma-separated, or "" -- what the
 * host reports rather than what it was told: a container's `pid`, a
 * volume's `created_at`, what the kernel currently has under
 * `zswap.kernel`. They are left out of the comparison entirely and may
 * be omitted from a supplied section, because a document fetched
 * before something else moved is otherwise unusable: it differs in a
 * field nobody can set, and a section that cannot be applied refuses
 * the whole request.
 */
const char *config_section_state_fields(int i);

/* Writes just section `i`'s value -- the same bytes
 * config_write_document() would put under that key. */
void config_render_section(int i, const char *containers_dir, struct json_writer *w);

#endif
