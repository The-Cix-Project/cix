#ifndef API_CONFIG_H
#define API_CONFIG_H

#include <stddef.h>

/*
 * Writing the running configuration back (ADR-0292).
 *
 * Both take the document itself as the request body -- the same shape
 * `GET /v1/config` returns, so fetch, edit, send back is the whole
 * workflow and nothing has to be unwrapped or re-nested on the way.
 * A partial document is normal and expected: only the sections present
 * in the body are considered at all.
 *
 * handle_config_diff() computes the plan and touches nothing.
 * handle_config_apply() computes the same plan and runs it, refusing
 * the whole request if any part of it cannot be applied.
 */
void handle_config_diff(int fd, const char *containers_dir, const char *body, size_t body_len);
void handle_config_apply(int fd, const char *containers_dir, const char *body, size_t body_len);

#endif /* API_CONFIG_H */
