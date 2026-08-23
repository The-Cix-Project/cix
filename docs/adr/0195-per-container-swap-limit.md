# 0195 — A per-container swap limit, where zero is a setting rather than "off"

## Status

Accepted

Completes the per-container resource picture ADR-0165 established for memory, CPU and PIDs.

## Context

A container's `memory.max` caps the anonymous and page-cache memory it may hold. It does not cap what happens next: under pressure, that memory goes to swap, and swap is a host-wide resource with no per-container accounting of any kind here. One container could take the whole 8 GiB swapfile and leave every other container unable to swap at all — with the container's own memory limit still perfectly respected the whole time.

This mattered for a real incident: 192.168.15.95 went fully unresponsive during a heavy `-j6` gcc bootstrap that drove the box into disk thrashing.

**The issue that asked for this said it needed `CONFIG_MEMCG_SWAP`, and that turned out to be stale.** Checked against the actual 6.18 source rather than assumed: the symbol does not appear anywhere in `mm/Kconfig` or `init/Kconfig` — it was removed upstream before 6.1 — and `mem_cgroup_swap_init()` registers `memory.swap.max` unconditionally, inside a block guarded only by `CONFIG_SWAP`. This platform's kernel has had `CONFIG_MEMCG=y` and `CONFIG_SWAP=y` all along. There was nothing to enable; the capability was already there and nothing exposed it.

## Decision

**`memory_swap_max` on `POST /v1/containers`, written to the container's own `memory.swap.max`.**

The whole design turns on one point: **0 is a real setting.** `memory.swap.max = 0` forbids the container from swapping at all — under pressure it is reclaimed or OOM-killed rather than pushed to disk, which is a genuine choice for a latency-sensitive workload on a box that does have swap. So this limit cannot use the convention every other limit in `struct cgroup_limits` uses, where 0 means "not set".

That has consequences at every layer, and each one is a place this could have silently done the wrong thing:

- **In the struct**, the unset sentinel is `-1`. Every caller `memset`s `struct cgroup_limits` to zero, so each of the three initialisers now sets it back to `-1` explicitly. That is the price of 0 meaning something, and it is paid where it can be seen rather than by choosing a sentinel that makes the trap invisible.
- **In the API**, an absent field means unlimited; a negative one is **refused**, not treated as unset — omitting the field is what means that, and accepting two spellings of one intent invites the caller to believe the wrong one.
- **On read-back**, `0` stays `0` and `null` means no limit. Folding zero into null would report the strictest possible setting as the absence of a setting: not a rounding error, the opposite answer.
- **In the CLI**, the "flag not given" sentinel is `-2`, because `-1` is already spoken for by the daemon's own refusal.

**`memory.swap.current` is reported in `GET /containers/{name}/stats`.** A limit an operator cannot see usage against is half a feature — the number worth acting on is how close a container is to its ceiling, not the ceiling.

## Consequences

The limit is enforced by the kernel, so it holds whether or not this daemon is running — the same property the memory limit already had.

It is only meaningful on a host that actually has swap. On one that does not, writing `memory.swap.max` succeeds and constrains nothing, which is the kernel's own behaviour and is not worth a second code path to detect; `GET /system/swap` is where an operator sees whether the host has any.

Zero-versus-null is now load-bearing in four places, and the test asserts the distinction directly rather than picking a round number nobody would confuse: a container created with `0` must read back `0`, and one created without the field must read back `null`.
