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
 * image/src/cix-install.c's own auto_partition()), so a plain file
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
 * means cixd depends on no external `mkswap` binary at all --
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
 * issue #28: repoints the file swap_enable()/swap_disable() act on --
 * the default g_base_dir/swap/swapfile location if new_file_path names
 * it, or a real operator-chosen disk's own mount path otherwise
 * (POST /system/swap's own optional "disk" field, validated and
 * resolved by the caller -- this function itself has no notion of
 * disks/roles, pure path bookkeeping only, matching every other
 * *_repoint() in this codebase). Only meaningful while swap is
 * currently disabled -- calling it while enabled would desync this
 * path from the file swapon(2) is actually holding open; the caller
 * (main.c's REST handler) only ever calls this on the same code path
 * as swap_enable(), which itself refuses a second call while already
 * enabled, so this never needs to defensively re-check that itself.
 */
void swap_repoint(const char *new_file_path);

/*
 * True if swap is currently on. issue #28: the REST handler must check
 * this *before* calling swap_repoint() -- repointing while enabled
 * would desync g_file_path from the file swapon(2) is actually holding
 * open, corrupting swap_write_json()'s own "path" field, for a call
 * that's about to fail with SWAP_ERR_ALREADY_ENABLED anyway.
 */
int swap_is_enabled(void);

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

/*
 * {"enabled": bool, "size_mb": N, "path": "...", "disk": "sdc"|null} --
 * path is "" when not enabled. disk_name (issue #28) is the caller's
 * already-resolved storageplacement_get(STORAGE_KIND_SWAP) -- this
 * module has no notion of disks/roles itself, it just echoes back
 * whatever the caller already knows to be true, same layering every
 * other disk-placement-aware subsystem's own write_json already keeps.
 */
void swap_write_json(struct json_writer *w, const char *disk_name);

#endif /* SWAP_H */
