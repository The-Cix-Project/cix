# 0382 — A verified source is kept by its checksum

## Status

Accepted by the owner on 2026-11-19 (*"Source cache first"*), offered as: make cixd keep checksum-verified sources by sha256 and reuse them, then build ADR-0381's `linux-firmware` recipe.

## Context

A package's sources are fetched host-side into `<pkg>/sources`, one file per chain slot and source, and verified there against the recipe's `sha256` (`pkg_fetch_completed()`). The slot's next fetch removes them. So every build of a recipe downloads its sources again, whoever built it before.

Measured on 192.168.15.95, 2026-11-19, by `probe-linuxfw@1-1`: kernel.org's `linux-firmware-20261019.tar.xz`, 672,694,092 bytes, took about 70 minutes to fetch (06:57 to 08:07 UTC), about 160 KB/s. ADR-0381's release 2 needs it twice (a probe that reads `WHENCE`, then the build), and every later rebuild once more. Meanwhile the package job holds every roll (#688). The kernel's tarball and gcc's are the same shape at a smaller scale.

## Decision

**A source whose bytes have verified against a recipe's `sha256` is kept in `<pkg>/source-cache/<sha256>`, and a later fetch of any source declaring that `sha256` takes it from there instead of the network.**

1. **Only verified bytes enter.**
   - The parent stores a source after `pkg_fetch_completed()` has hashed it equal to the recipe's `sha256`, as a hard link to the slot's file. That is one metadata operation on the loop; both directories are under `<pkg>`, on one filesystem.
   - A recipe with no `sha256` has nothing to key by, and is never cached.
2. **The key is the content.** Any recipe, version or mirror declaring that `sha256` is served the same file, since a `sha256` names one byte sequence (ADR-0307 clause 5). The url is not part of the key.
3. **A hit is still verified, and nothing is hashed on the loop.**
   - The fetch child links the cached file to the slot's path and skips the network.
   - The fetch child hashes every source it put in place, fetched or linked, and leaves the result beside it (`<file>.sha256`). The parent compares that string with the recipe's. Before this the parent hashed on the loop: measured on 192.168.15.95 (`GET /v1/system/stalls`, ts 1795075672), `event pkg-fetch (3150 ms)` for `linux-firmware`'s 672 MB, a stall every cache hit would have repeated.
   - A cached file that no longer verifies is removed and the source fails like a bad fetch, so the next attempt downloads it.
   - A hit sets the file's mtime, for eviction.
4. **Bounded, and evicted off the loop.**
   - `source_max_bytes` in `/pkg/cache-config`, beside the artifact cache's `max_bytes`, default 8 GiB.
   - The disk snapshot's helper child evicts the least recently used sources past it, in the same pass that walks the artifact cache (ADR-0377), and reports the total and count. `GET /v1/pkg/cache` carries them as `source_total_bytes` and `source_entry_count`.
   - Never walk or evict it anywhere else.
   - **An operator empties it with the artifact cache:** `DELETE /v1/pkg/cache` clears both, the one reset an operator has.
5. **Said in the build's log:** a source served from the cache is logged as such, once per source, so a build that downloaded nothing says so.

## Consequences

- **The first fetch of a source costs what it costs; later builds of it cost a link.** For `linux-firmware` that is one 70-minute fetch per upstream release, not one per build.
- **A source cache is not an artifact cache.** It holds inputs, keyed by content, never published, never installed. It is lost with `<pkg>` like the sources directory, and refilled by fetching.
- **The budget is a second knob,** because the two caches answer different questions: what was built, and what was fetched to build it.
