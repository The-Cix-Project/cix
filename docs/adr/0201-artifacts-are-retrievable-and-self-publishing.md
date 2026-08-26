# 0201 — A built artifact is retrievable, and publishes itself

## Status

Accepted

Builds on [ADR-0122](0122-pkg-redesign-part3-artifact-cache-and-server.md) (the package artifact tier) and [ADR-0123](0123-pkg-redesign-part4-image-recipes-and-artifact.md) (the image artifact tier), which established *pulling* precompiled artifacts. This ADR is the other half: getting bytes *out* of the host that built them.

## Context

Found while push-testing a real artifact cache. `kmod@34.2-2` pushed fine. `kernel@6.18.40-14` could not be extracted from the box at all.

`GET /v1/pkg` reported the kernel as `state: installed` with 34 recorded files (`bzImage` plus 33 under `lib/modules/6.18.40/`). Every one of them was unreachable:

- Booting a container from `__hostbuild` and reading `/bzImage` through `GET /v1/containers/{n}/files` returned `no such file` for all 34.
- `GET /v1/images/__hostbuild` showed `manifest: []` and two versions both predating the build; `e3b0c442…` is the sha256 of the empty string.

None of that is a bug. Per [ADR-0056](0056-hostbuild-artifact-mechanism.md) a hostbuild's harvested output lands in `artifacts_dir/<name>/`, a plain host directory that is deliberately never container-visible. The recorded file list describes that directory, not any image rootfs. The gap was elsewhere: **nothing could read it back out.**

`artifact_path` was exposed in the package JSON, but as a host path *string*. The only consumers — `spawn_cix_bootroot_assembly()` and the ISO route — `access()` the files directly off local disk. And `artifacts` is classified REBUILDABLE, so `POST /v1/system/backup` excludes it and factory reset wipes it.

That classification was fair when a hostbuild artifact was cheap to reproduce. It is not fair now. The self-hosted kernel is roughly a 50-minute build at the end of a four-stage toolchain chain (TCC → gcc 4.7.4 → 9.5 → 16.2). It existed as bytes in exactly one directory on one machine, unbackupable, unpushable, and one factory reset from gone.

The same shape applied to ordinary packages from the other direction: a built package landed in the local cache and stayed there. Nothing published it, so every other host rebuilt from source. **An artifact's default fate was to be forgotten.**

## Decision

Three changes, each addressing one part of that.

### 1. One export mechanism, two subjects

The image export state machine (issue #126) already tarred a tree, reaped the child, reported status, and served bytes in bounded chunks. A hostbuild artifact needs all of that and differs only in *where the bytes come from* and *what the file is called*.

So it was generalized rather than duplicated — `artifact_export_*` with an `enum artifact_export_kind`, and a new route pair:

```
POST /v1/pkg/{name}/artifact/export
GET  /v1/pkg/{name}/artifact/export
GET  /v1/pkg/{name}/artifact/export/download?offset=&length=
```

Growing a second, near-identical state machine would have been a direct No Parallel Implementations violation. The two kinds share everything and are kept apart only by the discriminator, which the handlers match on alongside the name so an image and a package sharing a name can never be confused.

The output filename is `<name>-<version>.tar.gz` for both kinds — deliberately the exact name the artifact cache serves at, so an export can be pushed verbatim with no renaming step that could drift from what a puller later asks for.

### 2. A fresh build publishes itself

`POST/PUT /v1/pkg/artifact-config` gains `push_enabled`. When it is on and a token is configured, a **genuine fresh build** enqueues its own cache tarball for upload to the configured server.

Four properties this holds to:

- **It never publishes what it did not build.** A cache or artifact hit's bytes already came from somewhere; only the fresh-build branch enqueues. (Tested by reinstalling a cached package and asserting nothing is republished.)
- **It never becomes a trust boundary.** The consumer still verifies bytes against the recipe's own git-tracked `pkg_artifact_sha256`. The `X-Cix-Sha256` header is a corruption check at the door, not a claim taken on faith. The three-way validation — recipe from git, binary from the server, checksum from the recipe — is untouched.
- **It never blocks the event loop.** The upload runs in a forked child that also computes the digest, so a large push costs one fork rather than a stalled control plane ([ADR-0180](0180-async-container-teardown.md)).
- **It is never silent.** Every reason to skip is logged, and the reaper reports what the server actually said. A host that silently publishes nothing looks exactly like a host with nothing to publish — the trap issue #125 set.

Push is **off by default**. Publishing is outward-facing, so it is opted into deliberately, never inherited from merely having a `base_url` set for pulling.

Pushes are serialized through a queue rather than run concurrently: parallel builds ([ADR-0157](0157-parallel-package-builds-design.md)) can finish together, and dropping the overflow would recreate the exact problem this exists to fix.

### 3. Cache tarballs are reproducible

Publishing to a shared server whose contract is *one name means one byte sequence forever* only works if two hosts building the same content produce the same bytes.

They did not. Confirmed by direct experiment rather than assumed: with the previous flags, two builds of an identical tree differ, because per-file mtimes ride in the tar headers. Adding `--sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner` makes them byte-identical.

(The gzip header timestamp was already zero, for an unrelated reason: `--use-compress-program` pipes through gzip's stdin, which has no filename or mtime to record. That fell out of issue #125's PATH fix and is worth knowing before anyone "simplifies" it back to `-z`.)

Without this, every host would produce a different tarball for the same recipe version, the first push would win, and every later one would be refused as a conflict — turning a real integrity rule into permanent noise. With it, a rejected push means what it should: **two builds genuinely diverged.**

## Consequences

- The kernel, and every other hostbuild artifact, can be retrieved, backed up, and published.
- A host that builds something contributes it, instead of hoarding it.
- A `409` from the artifact server becomes a meaningful alarm rather than expected background noise.
- **`pkg_artifact_sha256` values recorded before this change describe non-canonical tarballs.** Existing pulls are unaffected (the server still holds the objects those checksums describe), but a checksum regenerated from a daemon-built tarball will differ from one produced any other way. Recorded as its own issue rather than silently reconciled.
- `artifacts` remains in the REBUILDABLE class. Now that the contents are retrievable, whether that is still the right classification is a real question, and is deliberately left open rather than settled as a side effect of this change.

## Alternatives considered

**A second export state machine for hostbuild artifacts.** Rejected: identical in every respect that matters, and duplicating it is precisely the maxim violation.

**Serving the artifact directory as a static file tree.** Rejected: it would expose a host path namespace directly over the API, and there is already a chunked-download mechanism that solves the single-event-loop problem correctly.

**Pushing every install, not just fresh builds.** Rejected: re-uploading bytes that were just downloaded is pure noise, and would make a `409` meaningless by generating conflicts from ordinary operation.

**Gating push on the built tarball matching the recipe's declared `pkg_artifact_sha256`.** Attractive — it would mean only recipe-blessed bytes are ever published. Rejected because it inverts the workflow: a checksum can only be recorded *after* a canonical artifact exists, so this would have made the feature unable to publish anything new. Reproducibility (change 3) addresses the same risk without the deadlock.
