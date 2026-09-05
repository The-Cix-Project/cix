#ifndef API_KEYS_H
#define API_KEYS_H

#include <stddef.h>

/*
 * api_keys -- the two key pairs this platform holds: the Secure Boot
 * signing keys POST /v1/system/iso requires (ADR-0212) and the release
 * key artifacts are signed with.
 *
 * One module because they are one operator concern and share a rule
 * that matters more than either: a PUT body carries a PRIVATE key, so
 * it is never echoed back, never logged and never quoted in an error.
 * The responses are the same set/not-set summary GET returns.
 */

void handle_release_key_delete(int fd);
void handle_release_key_get(int fd);
void handle_release_key_put(int fd, const char *body, size_t body_len);
void handle_signing_keys_delete(int fd);
void handle_signing_keys_get(int fd);
void handle_signing_keys_put(int fd, const char *body, size_t body_len);

#endif /* API_KEYS_H */
