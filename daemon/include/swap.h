#ifndef SWAP_H
#define SWAP_H

#include "json.h"

#include <stdint.h>

/*
 * A single, host-level swap file, enabled/disabled on demand
 * (ADR-0069) -- raised directly by the user after watching a real
 * Rust/wasm package build (lldap.recipe) run a freshly-installed box
 * out of RAM. Deliberately one file, not a general swap-device
 * manager: this project has no notion of "spare disk space to turn
 * into a partition" (the install-time partition layout is fixed,
 * image/src/thinc-install.c's own auto_partition()), so a plain file
 * on the already-mounted containers filesystem is the only shape that
 * can be created "on the fly" without repartitioning a live disk --
 * and on any modern kernel with SSD/NVMe-backed storage, file-backed
 * swap performs identically to a raw partition (the kernel resolves
 * the file's extents once at swapon(2) time and does direct block
 * I/O against them afterward).
 *
 * The on-disk swap-file format itself (mkswap's job everywhere else)
 * is written directly by swap_enable() via plain offset-based writes
 * into a page-sized buffer, not a C struct -- the same lesson ADR-0008
 * already established for `struct epoll_event` (TCC ignores
 * `__attribute__((packed))`) applies with extra force here, since the
 * real kernel `union swap_header` layout mixes a 1024-byte
 * `bootbits` field with unaligned trailing members; sidestepping any
 * struct layout entirely removes the whole class of bug. This also
 * means thincd depends on no external `mkswap` binary at all --
 * consistent with how it already shells out to `curl`/`tar` only for
 * things it can't do more directly, never for something a few
 * documented byte offsets can replace.
 */

enum swap_error {
	SWAP_OK = 0,
	SWAP_ERR_ALREADY_ENABLED,
	SWAP_ERR_NOT_ENABLED,
	SWAP_ERR_INVALID_SIZE,
	SWAP_ERR_IO,
	SWAP_ERR_PERSIST_FAILED
};

#define SWAP_MIN_MB 64
#define SWAP_MAX_MB (1024 * 1024) /* 1 TiB -- a sanity bound, not a real limit */

/*
 * Loads state_path (if any persisted state exists) and, if it says
 * swap was last left enabled, re-activates file_path via swapon(2) --
 * best-effort, matching every other subsystem's "never fail daemon
 * startup over a non-fatal reconciliation step" posture (e.g.
 * cgroup_enable_controllers()): a daemon restart or a real reboot
 * both land here, and swap coming back automatically either time is
 * the whole point of persisting it at all.
 */
int swap_init(const char *state_path, const char *file_path);

/*
 * Creates (or overwrites) the swap file at exactly size_mb megabytes,
 * writes a real kernel swap-file header into it, and activates it via
 * swapon(2). SWAP_ERR_ALREADY_ENABLED if swap is already on -- call
 * swap_disable() first to resize. size_mb must be within
 * [SWAP_MIN_MB, SWAP_MAX_MB].
 */
enum swap_error swap_enable(int64_t size_mb);

/* swapoff(2) the file and unlink it. SWAP_ERR_NOT_ENABLED if it
 * wasn't on to begin with. */
enum swap_error swap_disable(void);

/* {"enabled": bool, "size_mb": N, "path": "..."} -- path is "" when
 * not enabled. */
void swap_write_json(struct json_writer *w);

#endif /* SWAP_H */
