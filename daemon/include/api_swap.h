#ifndef API_SWAP_H
#define API_SWAP_H

#include <stddef.h>

/*
 * api_swap -- GET/POST/DELETE /v1/system/swap: the host's own swap file.
 *
 * swap.c owns creation, swapon/swapoff and the on-disk state; this is
 * where it meets HTTP. See ADR-0249 for the boundary.
 */

void handle_swap_disable(int fd);
void handle_swap_enable(int fd, const char *body, size_t body_len);
void handle_swap_get(int fd);

#endif /* API_SWAP_H */
