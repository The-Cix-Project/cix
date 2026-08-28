# 0210 — `base` is not special; every image is derived from its manifest

## Status

Accepted. Completes [ADR-0209](0209-derived-images-and-one-container-mechanism.md)
by removing the one image that ADR-0209's rule could not describe.
Retires the imperative seeding of `base` in `image/src/cix-install.c`.

## Context

`base` looks like a foundation. It is not one, and the difference
matters because a great deal of design has been arranged around the
assumption.

What `base` actually is, in the code:

- `PKG_DEFAULT_IMAGE` — where `pkg install` puts a package when no
  `--image` is named.
- `IMAGE_ERR_PROTECTED` — refused deletion, described as *"every
  container's own default, never removable"*.

That is the whole of it. **Nothing inherits from `base`.** It is not a
lower layer for other images; every image has its own independent
rootfs, and a container overlays (or, on btrfs, snapshots) the rootfs
of *its own* image. The name suggests a hierarchy the system does not
have.

The question worth asking, then, is what content is genuinely common
to every container and therefore worth sharing. The answer is: the
baseline — the glibc floor, the device nodes, `/etc/passwd`,
`/etc/nsswitch.conf`. A couple of megabytes. And
`pkg_seed_image_baseline()` already applies it to **every** image
independently, so `base` contributes nothing to it.

Beyond that there is no shared middle. `dns` needs dnsmasq. `ntp`
needs chrony. A build environment needs a toolchain and is composed
per build from declared tools. Two unrelated workloads have nothing
between them that a shared image could usefully hold.

Meanwhile `base` carries a real privilege that ADR-0209's rule cannot
express: `cix-install` seeds its rootfs imperatively at install time,
copying a C runtime in so dynamically-linked packages can `execve` at
all. That is content placed in an image by code rather than declared by
a manifest — the same pattern this project has spent
[#168](https://git.home.arpa/itdlabs/cix/issues/168) and
[#169](https://git.home.arpa/itdlabs/cix/issues/169) removing
everywhere else. ADR-0209 says an image's content is exactly its
baseline plus its declared packages. `base` is the one image for which
that is false.

## Decision

**`base` becomes an ordinary image, derived from a manifest like every
other.**

- Its content is the baseline plus whatever its manifest declares —
  nothing installer-side, nothing in C.
- The imperative seeding in `cix-install.c` is removed. What that
  seeding provided (a working C runtime) is what
  `pkg_seed_image_baseline()` already provides to every image, so the
  special case was redundant as well as inexpressible.
- The **name** survives as a default: `pkg install` with no `--image`
  and a container with no `image` both need somewhere to point, and a
  default that always exists is worth keeping. It stays protected from
  deletion for that reason alone — because removing the default would
  break those two paths, not because its contents are special.

An image with an empty manifest is then exactly "what a container needs
in order to exist at all", which is the honest meaning `base` should
have had all along.

## Consequences

**Every image becomes uniform.** There is one rule for what an image
contains and no exception to reason about. A reader who understands one
image understands all of them — including the one the installer
creates.

**Duplication is real, small, and now visible.** Each image carries its
own copy of the baseline, and two images that both contain bash each
hold a copy. Nothing deduplicates across images today. That was equally
true before this ADR — `base` never shared anything — the difference is
that it is now an explicit property rather than an assumption hidden
behind a reassuring name. If it ever matters, btrfs reflink at
composition time is the mechanism, and ADR-0207 already puts the
platform on btrfs. That is an optimisation to measure and decide, not a
reason to reinstate a privileged image.

**A fresh install has one less special step,** and one less way for a
box to differ from what its manifests say. The installer stops being a
place where image content is decided.

**What this does not change.** Containers still overlay or snapshot
their image's rootfs (ADR-0079/0080). Images are still immutable while
referenced (ADR-0107/0108). The default image still exists and is still
undeletable. Nothing about the runtime model moves — only the
justification for one image being different from the rest, which turned
out not to exist.
