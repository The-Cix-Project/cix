# 0328 — Cix produces no tarball: a hostbuild exports as a CIXPKG, and an image is not exported

## Status

Accepted, 2026-10-03, the owner's decision on cix#411: *"An image export should not produce a tarball. Go with 2."* It amends [ADR-0201](0201-artifacts-are-retrievable-and-self-publishing.md) (the export tarred its subject), removes the image export issue #126 added, and applies the One Build System Mandate (a package artifact is `.cixpkg`) to the last code in cixd that wrote a tarball.

## Context

Two paths in `cixd` still produced a `.tar.gz`, both through `daemon/src/targz.c` (a forked `tar | gzip`):

- **A hostbuild export** (`POST /v1/pkg/{name}/artifact/export`, and the automatic publish after a hostbuild) ran `cbs package` when the recipe declared `cixpkg`, and tarred otherwise. The only recipes that declare anything else are stored shell recipes, which `parse_recipe()` gives `tar.gz` by construction (ADR-0307 clause 1).
- **An image export** (`POST /v1/images/{name}/export`) tarred an image's whole rootfs, named for ADR-0123's image-artifact fetch. ADR-0209 retired that fetch, so nothing has consumed the export since. It had no CLI or dashboard surface (`x-cix-expose: []`).

## Decision

**Nothing cixd produces is a tarball.**

- **A hostbuild exports as a CIXPKG, always.** The export runs `cbs package` over the installed tree. It packages the installed bytes, so the language of the recipe that built them does not matter, and it never asks for a format. `pkg_hostbuild_package_info()` no longer returns one.
- **An image is not exported.** `/images/{name}/export` and `/images/{name}/export/download` are removed from the contract. A `.cixpkg` of a whole rootfs would bring back, in a new format, the second representation of a package set that ADR-0209 retired. An image is its package set plus its manifest, and `GET /images/{name}/recipe/export` already writes that out (ADR-0320).
- **`targz.c` is deleted**, with `test_targz` and `targz_probe`. Nothing in cixd forks `tar` or `gzip` to write an artifact any more.

## Not decided here

**Reading an existing `.tar.gz`** (an artifact in the cache, built from a shell recipe before the conversion) is unchanged, and so is ADR-0307 clause 1, which gives a stored shell recipe the `tar.gz` format so that artifact can be fetched by name.

Removing that is the next step, and it is not free. Measured 2026-10-03:

- 192.168.15.95 stores 991 shell recipe versions.
- One installed version comes from one: `gcc@16.2.0-13`, in `cix-builder` and `kernel-builder`.
- The latest image recipes in cix-recipes pin 55 versions that resolve to shell recipes: 38 in `iso-builder`, 8 in `gcc-tcc-bootstrap`, 6 in `herdr`, and 1 each in `chrony`, `cix-builder` and `kernel-builder`.
- The LAN artifact cache (192.168.15.31:8080) holds 452 `.tar.gz` artifacts.

A shell recipe with no format cannot be fetched, so those images could not be composed on a host without a local copy. The pins move first, then the format goes. That needs its own decision, and is tracked as cix#569.

## Consequences

- A host that exported images loses that endpoint. No consumer had existed since ADR-0209, and no CLI command or dashboard view used it.
- The manual publish of a hostbuild names its destination as the `.cixpkg` path. It used the existing-file lookup, whose not-found answer is spelled `.tar.gz`, the same mistake cix#528 fixed on the automatic path.
- Whether a `.cixpkg` is byte-reproducible across hosts is a property of `cbs package`. The tar flags that made a cache tarball reproducible (ADR-0201) went with the tarball.
- `mkbootroot` still stages `/usr/bin/tar` and `gzip` into the control-plane root. Their cixd consumer is gone: no exec of either remains in `daemon/src` (measured with grep, 2026-10-03). Whether anything else in that root runs them, cbs included, is not established, so they leave with cix#569 rather than here.
