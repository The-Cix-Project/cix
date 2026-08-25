# 0069 — A single, on-demand host swap file

## Status

Accepted

## Context

Raised directly by the user while `lldap.recipe`'s Rust/wasm build ran on a freshly-reinstalled box (192.168.15.95): the build ran the host out of RAM mid-package-build (`cargo build --release` with LTO/codegen for a WASM frontend is a real, substantial memory spike), and the user asked whether a swap partition or swap file could be created "on the fly," and whether that was acceptable for performance.

Two real constraints, both confirmed directly rather than assumed:

- **A swap partition is off the table for "on the fly."** `image/src/cix-install.c`'s `auto_partition()` lays out the disk once, at install time, with a fixed partition count/purpose (ESP, root A/B, config, containers — see `CLAUDE.md`'s own `ESP_DEVICE`/`ROOT_A_DEVICE`/`CONTAINERS_DEVICE` notes). There is no "spare unpartitioned space" concept anywhere in this project, and repartitioning a live, already-installed disk to carve out a new partition is exactly the kind of hard-to-reverse, high-blast-radius operation this project's own tooling has never been asked to do and shouldn't improvise under time pressure. A plain file on the already-mounted containers filesystem needs none of that.
- **File-backed swap is not a performance compromise.** On any kernel since the file-backed swap rewrite (long since default), the kernel resolves a swap file's on-disk extents once at `swapon(2)` time and thereafter does the same direct block I/O a raw partition would — especially true here, where the underlying storage is virtualized SSD/NVMe-backed QEMU/Proxmox storage, not spinning rust with real seek costs to avoid.

**No REST primitive existed for this at all.** Per the API-First Mandate, a capability has to exist as a REST endpoint before either the CLI or web dashboard can offer it — and per this project's own architecture, `cixd` has no host-level shell-exec surface for an operator (or for me) to improvise one from outside (deliberate: this is the same reasoning ADR-0034 already gives for having no SSH server). This had to be a real, if small, new daemon capability, not a one-off manual step.

## Decision

**A new `daemon/src/swap.c`/`swap.h` module**, following the exact persisted-module shape `devicemap.c` already established (its own `state_path` init parameter, a tiny JSON blob under the base data directory, `enabled`/`size_mb` fields). One swap file, not a general swap-device manager — this project has no notion of multiple swap targets to manage, and YAGNI applies.

**The on-disk swap-file format is written directly, not via a shelled-out `mkswap`.** The real kernel swap-file header (`mm/swapfile.c`'s `read_swap_header()`, the same fixed byte offsets `mkswap` itself has written unchanged for decades — `version` at byte 1024, `last_page` at 1028, `nr_badpages` at 1032, the `"SWAPSPACE2"` magic string in the final 10 bytes of the first page) is written via plain offset-based `memcpy`s into a flat, `calloc`'d page-sized buffer — never through a C struct. This sidesteps the exact class of bug ADR-0008 already burned this project on (TCC ignores `__attribute__((packed))`, and the real kernel `union swap_header` layout mixes a 1024-byte `bootbits` block with tightly-packed trailing fields that would need it). It also means `cixd` depends on no external `mkswap` binary — consistent with how it already shells out to `curl`/`tar` only for things it genuinely can't do more directly, never as a first resort.

**`fallocate(2)`, not `ftruncate(2)`, backs the file's actual blocks.** A swap file must have every block genuinely allocated on disk — a sparse file (what `ftruncate()` alone produces) is rejected by `swapon(2)`. `fallocate(fd, 0, 0, size_bytes)` guarantees real, contiguous-enough backing.

**`swapon(2)`/`swapoff(2)` are called directly** — ordinary glibc-wrapped syscalls (unlike `pivot_root`/`clone3`, which this project already knows need `syscall()` directly), no subprocess involved.

**`POST /v1/system/swap {"size_mb": N}` / `GET /v1/system/swap` / `DELETE /v1/system/swap`.** Enabling while already enabled is a `409` (resize means disable-then-enable, not an implicit resize — no hidden magic); size is bounded to `[64, 1048576]` MB as a sanity check, not a real limit. `cixctl swap [status] / enable --size-mb=N / disable`, matching the API 1:1 per the API-First Mandate.

**Enabled state is persisted and re-applied on every `swap_init()`** (daemon restart or a real reboot both call this) — `swapon(2)` is attempted again against the already-written file; failure is logged and treated as "not enabled" rather than blocking startup, the same "non-fatal reconciliation step" posture every other subsystem's own startup-time reconciliation already follows.

## Consequences

- Verified locally: enable/get/409-on-double-enable/disable/get all round-trip correctly against a real `cixd` (`/proc/swaps` inside this project's own dev LXC sandbox is itself `lxcfs`-virtualized and doesn't reflect real kernel swap state, so the authoritative verification is the `swapon(2)`/`swapoff(2)` return codes themselves).
- **A second real gap found only against the actual target, not by local verification**: the very first deployment attempt against a real installed box returned `500 swap file creation or activation failed` from a correct swap file and correct daemon code — the target kernel had never had `CONFIG_SWAP` built in at all (nothing in this from-scratch OS had ever needed it before this ADR), so `swapon(2)` returned a bare `ENOSYS` regardless of how correct the file itself was. This dev sandbox's own local verification could never have caught it, since this sandbox runs a normal Debian host kernel with swap already built in — the bug only exists in Cix's own custom kernel config (`image/kernel/qemu-part1.config`), a genuinely different artifact from the daemon binary being tested locally. Fixed by adding `CONFIG_SWAP=y` to that config (no `CONFIG_ZSWAP`/`CONFIG_FRONTSWAP` needed — this project uses neither compression nor backing-device tiering, just a plain file) and rebuilding the kernel.
- A box that has never called `POST /v1/system/swap` behaves exactly as before this ADR — purely additive, opt-in, no default swap anywhere.
- CLI and web dashboard both gained the identical capability the REST endpoint offers, no more and no less, per the API-First Mandate.
- This ADR does not address *why* the box ran out of RAM in the first place (a Rust/wasm release build's real memory footprint against this VM's allocated RAM) — swap is a mitigation, not a fix for undersized RAM; an operator building similarly heavy packages on a similarly small VM should expect to reach for this again, or increase the VM's RAM allocation instead.
