# 0289 — A hostbuild package is consumed from the cache like any other

## Status

Accepted. Amends [ADR-0122](0122-pkg-redesign-part3-artifact-cache-and-server.md) (which excluded a hostbuild from the local build-artifact cache) and rests on [ADR-0279](0279-an-artifact-carries-its-own-approval.md) (a published signature is an approval in its own right) and [#129](https://git.home.arpa/itdlabs/cix/issues/129) (a hostbuild publishes its artifact like any other build).

## Context

`cix`, `kernel` and `isotools` are the three **hostbuild** packages — a host builds them for itself and, since #129, publishes them to the artifact cache like any other package. The question this ADR settles is the mirror of #129: can a host **consume** one of them from the cache instead of rebuilding it? Rebuilding `cix` or `kernel` is the most expensive thing this platform does, so "upgrade from the cache, not only build" is the whole point of having the cache at all.

The belief going in was that a hostbuild always rebuilds. That is **false**, and it was proven false on a real host. Measured on 192.168.15.95, 2026-09-13:

```
POST /v1/pkg/hostbuild {name:cix, version:v2.57.155, upgrade:true}
  → state fetching → installed in 3 s, artifact_cached=true, no build
```

`cix@v2.57.155` is signed in the cache; the box fetched it and skipped the build. The mechanism was already almost entirely in place, across two independent facts:

1. **The remote signed-artifact fetch is not hostbuild-gated.** `start_fetch_for()`'s fetch child tries the artifact tier whenever `pkg_artifact_is_configured()` and either the recipe carries a `pkg_artifact_sha256` **or** the host holds a trusted release key (ADR-0279). A hostbuild reaches that same child (`pkg_hostbuild_start()` → `start_fetch_for()`), so nothing about a hostbuild ever excluded it from the artifact tier. `cix`/`kernel`/`isotools` carry no `pkg_artifact_sha256`, so the trust anchor is the **release-key signature**: the fetch succeeding proves the box holds a trusted key (adopted from `docs/keys/` by `pkg sync`, ADR-0279).

2. **The push path signs whatever a key-holding host publishes.** After an artifact push returns `201`, a signature push is enqueued whenever `releasekey_is_set()` — not per-package (`pkg.c`, the `PKG_PUSH_SIGNATURE` chain). So every *new* version of a hostbuild package a key-holding host cuts is signed at publish and is therefore cache-consumable by every host that trusts the key. This is exactly how the signed `cix` versions came to exist.

The one thing that was genuinely inconsistent was the **local** build-artifact cache (Tier 1). ADR-0122's `start_fetch_for()` computed `cache_hit = !is_hostbuild && pkg_cache_has(...)`, excluding a hostbuild from the local cache with the stated reason that "a hostbuild job's own artifact never belongs in the shared package cache." That reasoning is about the **output** — a hostbuild's harvest goes to `ARTIFACTS_DIR`, never merged into an image — and it is still true of the output. But the guard sat on the **read**, and the read is a different question. Since #129 a hostbuild publishes its artifact, and a successful *remote* artifact fetch saves the fetched tarball into the local cache (`pkg_cache_save_from_file()`). So the local cache legitimately holds hostbuild packages, and the exclusion meant a hostbuild would re-download an artifact it had already fetched once. Measured: a hostbuild that *builds* still never writes the local cache (its harvest branch has no `pkg_cache_save()` — only the ordinary-install branch does), so a local hit for a hostbuild can only ever come from a prior fetch, whose bytes are that same version and therefore the same artifact.

### What was measured and rejected along the way

Two dead ends were investigated and are recorded so they are not re-walked:

- **"Retroactively sign the existing `kernel`/`isotools` cache bytes."** Considered because their *current* cache artifacts are unsigned (published before the box held the key). Rejected as unnecessary: a hostbuild fetch requests `<name>-<recipe.version>-<arch>.tar.gz`, and `kernel`/`isotools` recipe versions **include the release** (`kernel` `pkg_version="7.2.3-10"`, `isotools` `pkg_version="2.14-16"`) — so the daemon asks for the exact stamped stem, which exists. The next revision of either, when built by a key-holding host, signs itself and is cache-consumable from then on; a version already installed on a host is never re-fetched. No retroactive-sign endpoint is needed. (An earlier analysis mistakenly tested the release-less alias `<name>-<version>-x86_64.tar.gz`, which only `cix`-style versions — no release — ever request; that alias plays no part in the `kernel`/`isotools` path.)
- **"Rebuild `kernel`/`isotools` at the same version to sign them."** Rejected: the cache is immutable per-bytes, so re-pushing a non-reproducible build returns `409` and the `409` path does not sign — a rebuild-to-sign of an existing version cannot produce a signature at all.

## Decision

**A hostbuild package is consumed from the cache exactly like any other package.** No hostbuild-specific gate stands between a host and a cache artifact it is entitled to install, on either tier.

Concretely:

- **The local-cache read no longer excludes a hostbuild.** `start_fetch_for()` computes `cache_hit = pkg_cache_has(recipe.name, recipe.version)`. A hostbuild that finds its `(name, version)` in the local cache reuses those bytes — extracted to `dest_dir` and harvested to `ARTIFACTS_DIR`, the same path a remote-fetched hostbuild already took — instead of re-downloading. The output rule ADR-0122 actually cared about is untouched: a hostbuild's harvest still goes to `ARTIFACTS_DIR` and is never merged into an image.
- **The remote artifact tier is unchanged** — it was never hostbuild-gated. For a hostbuild package with no `pkg_artifact_sha256`, the release-key signature (ADR-0279) is the trust anchor, and a host consumes a signed hostbuild artifact iff it has adopted the signing key via `pkg sync`.
- **Nothing new signs, and nothing is signed retroactively.** Cache-consumption of a hostbuild package is a property that arrives for free on the next version a key-holding host publishes, because the push path already signs it.

The operator-facing shape does not change: the same `cixctl pkg hostbuild <name> --upgrade [--deploy]` now fetches a signed artifact when one is in the cache, and builds only when one is not.

## Consequences

- Upgrading the control plane (`cix`) or the `kernel` from the cache is now an ordinary fetch, not a rebuild, on any host that trusts the release key and has the artifact configured — the expensive operations become cheap for every host that is not the one that built them, which is the whole self-hosting-at-scale argument.
- A re-`hostbuild` of a version already fetched once reuses the local tarball rather than re-downloading it. Minor, but it removes the last hostbuild-shaped inconsistency in the two-tier cache.
- No trust is weakened: consumption still requires either a git-tracked `pkg_artifact_sha256` or a signature from an adopted key, exactly as ADR-0122 and ADR-0279 left it. Dropping the guard only lets a hostbuild reach the *local* cache, whose entries were themselves put there by a verified fetch.
- The current unsigned `kernel`/`isotools` artifacts stay build-only until their next revision is cut — a deliberate non-action, not a gap to close, since a running version is never re-fetched.
