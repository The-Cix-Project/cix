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

#endif
