#ifndef SRCTRUST_H
#define SRCTRUST_H

#include <stddef.h>

#include "json.h"

/*
 * ADR-0323, rung 4: the origins this host trusts to authenticate a
 * release by where it comes from -- TLS to that origin, the archive's
 * sha256 recorded when it is fetched.
 *
 * The owner's answer: origin trust is for the owner's own forge, and a
 * trusted origin changes only by an operator's API call, never as a
 * side effect of discovery. So a fresh host trusts nothing, and a
 * recipe's `verify origin` is honoured only when the origin of its
 * upstream source template is on this list.
 *
 * An origin is exactly "scheme://host[:port]": lowercase scheme and
 * host, no credentials, no path. An operator who lists an http origin
 * has chosen to trust that path's integrity too; the list does not
 * second-guess it, and a test's loopback forge is an origin like any
 * other.
 */

#define SRCTRUST_MAX 16
#define SRCTRUST_ORIGIN_MAX 256

int srctrust_init(const char *path);
void srctrust_repoint(const char *path);

/*
 * The origin of `url` -- scheme and authority with any userinfo
 * removed and scheme and host lowercased -- in `out`. 0, or -1 when
 * `url` is not an absolute http(s) url or the origin does not fit.
 */
int srctrust_origin_of(const char *url, char *out, size_t out_size);

/* 1 when the origin of `url` is on the list, 0 otherwise. */
int srctrust_trusts(const char *url);

/*
 * Replaces the whole list. Each entry must already be an origin in the
 * form above (srctrust_origin_of() of itself, unchanged); duplicates
 * and anything else are refused with the offending entry in err. 0, or
 * -1 with nothing changed.
 */
int srctrust_set(const char *const *origins, int count, char *err, size_t err_size);

int srctrust_count(void);
const char *srctrust_at(int i);

/* {"origins":["https://git.home.arpa", ...]} */
void srctrust_write_json(struct json_writer *w);

#endif /* SRCTRUST_H */
