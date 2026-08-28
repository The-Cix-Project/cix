# 0209 — images are derived, not stored; one mechanism creates every container

## Status

Accepted. Supersedes [ADR-0123](0123-image-artifact-fast-path.md)'s
whole-rootfs artifact fast path. Refines
[ADR-0199](0199-recipes-declare-their-build-tools.md) by removing the
fallback it left open, and [ADR-0208](0208-build-image-taxonomy.md) by
making an image's manifest true by construction rather than by
convention. Directly caused by
[issue #168](https://git.home.arpa/itdlabs/cix/issues/168) and
[issue #169](https://git.home.arpa/itdlabs/cix/issues/169).

## Context

Four mechanisms, each defensible when written, combined into a system
where nothing could be said about what an image actually contained.

**Measured, on a real host, before any of this was decided.** The
`cix-builder` image was 2.2 GB compressed, ~8 GB extracted, **88,798
entries**. The packages in its manifest accounted for **6,378 files** —
so **93% of the image belonged to no package it declared**. The excess
was a developer workstation's `/usr`: rustup (1330 MiB), cargo (850
MiB), gcc (939 MiB across two trees), chromium (337 MiB), go (241 MiB),
node (365 MiB across three), valgrind, gdb, qemu, sudo, systemd,
erlang, python3.

How it got there, all four confirmed by reading the code rather than
inferred:

1. **`PKG_BUILD_SANDBOX_IMAGE` is `cix-builder`.** The shared build
   sandbox was not a hidden internal thing; it was the image operators
   name on a command line. Issue #40 chose this deliberately and its
   own comment predicted the consequence: *"this image is now grown by
   EVERY successful ordinary install."*
2. **Every successful install folded into it**, forever, so it became
   the union of everything ever installed on that host.
3. **`pkg bootstrap` copied the build host's whole `/usr`** into it.
4. **`libc-dev` copied the build container's entire `/usr/include`**
   (#169), carrying Debian's Erlang, valgrind and gcc-12 C++ headers
   into *any* image installing it — a second, independent path that
   would have survived fixing the first three.

And because every one of those merges produced a new immutable image
version, with **no garbage collection anywhere in the API**, the result
was 66 versions of an 8 GB rootfs on a 16 GiB partition. The disk
filled during this investigation, which is how it was found.

The common shape is worth naming, because it is what this ADR exists to
prevent: **content arrived in images by accumulation rather than by
declaration.** No one chose to ship chromium in a build image. A
mechanism imported it, another mechanism propagated it, and a manifest
described something else entirely. The manifest was not lying — it was
never connected to the content in the first place.

## Decision

**1. An image is derived from its manifest, materialized while
referenced, and garbage-collected when not.**

An image's content is exactly: the baseline, plus each declared
package's own recorded file list. Nothing else can enter, because
nothing else has a path in. It is materialized on disk because a
running container must overlay or snapshot a real rootfs — but it is a
*cache of a computation*, not a source of truth, and it is collected
once no container, container definition, or build references its
version. Immutability while referenced (ADR-0107/0108) is unchanged and
still required.

The consequence that matters: **an image's manifest becomes true by
construction.** Drift is not detected, it is impossible.

**2. Package artifacts are the only published binaries. Whole-rootfs
image artifacts are dropped.**

ADR-0123's fast path — fetch one tarball instead of installing N
packages — is retired. It was the mechanism that took one contaminated
capture and shipped it to every host, and it is a second
representation of something already fully described by packages plus a
manifest.

Provisioning now installs the manifest's packages, each independently
checksummed and each traceable to a recipe and a Cix host that built
it. This is slower and it is worth it: the alternative is a binary
blob whose relationship to any recipe is a matter of trust.

**3. One mechanism creates every container.**

A build container becomes an ordinary container created from an
ordinary spec — same creation path, same teardown, same code — whose
image is the composed build environment and whose `cmd`/env come from
the recipe. `pkg.c` stops having a private way to make a container.

This is No Parallel Implementations applied to the thing the whole
platform is *for*. It also makes builds observable through the same
surfaces as everything else, rather than through a parallel set of
package-specific states.

**4. The baseline is composed from packages, not copied from the host.**

`pkg_seed_image_baseline()`'s seven hardcoded host paths become a
declared package set, composed exactly like a build environment.
`libtinfo` and `libgcc_s` come from this project's own `ncurses` and
`gcc` packages immediately — we build both already and were copying
Debian's copies of them anyway.

`glibc` and `ld.so` remain host-sourced for now and are **explicitly
recorded as the last unclosed link in the bootstrap**. That is not a
compromise hidden in a footnote: it is the one remaining foreign
dependency in the entire system, it is named, and closing it means
building glibc from source on a Cix host. Everything above it is
honest; this is not yet.

## Consequences

**Determinism becomes a property, not an aspiration.** Given a manifest
and the package artifacts it names, an image is reproducible byte for
byte on any host. Today it is reproducible only in the sense that
running the same history in the same order on the same machine gets you
the same pile.

**The no-fallback rule is what makes all of it hold.** A recipe that
does not declare its build tools cannot be built (#168's fix, already
landed). Without that, every other guarantee here leaks: one
undeclared build against an accumulated environment reintroduces
exactly the drift this removes. There is no test-only exemption
either — see below.

**The test suite has to bootstrap like a real box.** Five package tests
depended on the fungible sandbox, and the harness filled it with the
same wholesale `/usr` copy. They will install a fixture toolchain
package through the artifact/cache path — which needs no build
environment at all, exactly as a fresh host's first packages do — and
then declare it. A test-only escape hatch would be the fallback we just
deleted, wearing a different name.

**Provisioning gets slower and the code gets simpler.** Removing the
image-artifact path removes a whole tier: capture, publish, fetch,
verify, extract. What replaces it already exists and is already
exercised on every install.

**Sixty recipes must declare their build tools.** That is the real cost
of this decision and it is not avoidable — it is the same cost as
saying what our software actually needs. The mechanism makes each one
verifiable: an under-declaration fails the build naming the missing
tool, so a wrong declaration cannot produce a wrong binary.

**What this does not fix.** The `glibc`/`ld.so` floor above. And an
image's *reproducibility* still rests on package artifacts being
reproducible — which they are (issue #129's normalizing tar flags,
now in `targz.c`), but that property is now load-bearing in a way it
was not before.
