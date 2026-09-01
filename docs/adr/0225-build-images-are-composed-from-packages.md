# ADR-0225: Build images are composed from packages, not copied from a host

Date: 2026-09-01

## Status

Accepted. The direction was chosen by the owner; the staging below is
the implementation plan, and each step is separately verifiable.

Supersedes the live-copy half of `pkg_bootstrap_build_image()`. Answers
issue #168, and is the real answer to #141.

## Context

`cix-builder` is 2.2 GB compressed, ~8 GB extracted, 88,798 entries.
The packages in its manifest account for 6,378 files. **Roughly 93% of
the image belongs to no package.** What that 93% is, measured:

```
1330 MiB  usr/local/rustup      337 MiB  usr/lib/chromium
 850 MiB  usr/local/cargo       241 MiB  usr/local/go
 822 MiB  usr/lib/x86_64-...    198 MiB  usr/lib/node_modules
 811 MiB  usr/libexec/gcc       119 MiB  usr/bin/node
```

That is a developer workstation's `/usr`, verbatim, inside a build
image, downloaded by every host that installs it.

It arrives through one function.
`pkg_bootstrap_build_image()` stages a toolchain "from THIS HOST ... by
copying whole `/usr/{include,lib,lib64,bin,libexec}` directories" — its
own doc comment. On a rich host that is everything installed on it.

**The important finding is that the obvious fix does not work.** There
is already a "correct production path": `image/src/mktoolchainimage.c`
builds a portable toolchain artifact, imported by
`pkg_bootstrap_from_toolchain()` via `unsquashfs`, with an HTTP fetch
path too (ADR-0065). Retiring the live copy in favour of it looks like
the whole answer.

It is not, because both call the *same* staging function —
`test_image_fixture_stage_toolchain()` — and that function performs the
wholesale `/usr` copy. Moving to the artifact would relocate the 8 GB,
not remove it. Its own comments already argue against staging the whole
of `/usr/share` "for no real benefit" while copying `/usr/lib` and
`/usr/bin` entire.

**What has changed since that code was written is decisive.** It exists
because Cix could not build its own toolchain, so the toolchain had to
come from somewhere else. That is no longer true. `gcc`, `binutils`,
`make`, `m4`, `bison`, `flex`, `perl`, `coreutils`, `bash`, `sed`,
`grep`, `gawk`, `findutils`, `diffutils`, `glibc` and `linux-headers`
are all real Cix packages, built from source on a Cix host. Everything
a build image needs is already packaged.

And the mechanism to assemble one exists twice over: image recipes
(`recipes/image/*`) describe an image *by its package manifest*, and
ADR-0199's composer already builds an environment containing exactly a
declared set of packages, on every single package build.

## Decision

**A build image is its declared packages. Nothing is copied from any
host's filesystem into an image, ever.**

Concretely:

1. **`pkg_bootstrap_build_image()`'s live `/usr` copy is retired.** It
   is the sole source of the unowned 93%, and its reason for existing
   expired when the toolchain became packaged.

2. **The steady-state path is the one that already works**: an image
   recipe names packages, the ordinary install machinery puts them
   there. This is not new code; `cix-builder`'s own recipe already
   does it, on top of a seed that should not be there.

3. **The bootstrap path is a published toolchain artifact built the
   same way** — composed from Cix packages on a Cix host, published to
   the artifact cache like any other artifact, fetched over the
   existing ADR-0065 path. Not staged from a developer machine.

4. **"The manifest describes the image" becomes assertable, and then
   asserted.** The daemon knows both the manifest and the rootfs
   contents. A file in an image owned by no package in its manifest is
   a defect, and once (1) lands it is a rare one.

## Consequences

**#141 is answered by the same change.** "There is no product path to
seed a fresh box's build toolchain" was true because the only path was
copying from a machine that happened to have one. A published artifact
built from packages is that product path, and it is reproducible in a
way a filesystem copy never was.

**Image size collapses**, and the number is the point: from ~8 GB of
which 6,378 files are accounted for, to an image that is its manifest.
Every host that installs it stops downloading a stranger's Chromium.

**The build-provenance mandate stops being partly aspirational.** It
already says "no host `/usr` content is ever copied into an image" and
names this function. Today the code contradicts the rule; after this it
does not.

**A real cost, stated.** The wholesale copy was tolerant: it swept up
whatever a build happened to need, including things nobody enumerated.
Composing from packages makes every dependency explicit, so the first
builds after this will surface missing tools one at a time. That is the
same trade ADR-0199 already made deliberately for build environments,
and the same discipline: a build that fails naming what it lacks is
better than one that succeeds because a developer's laptop happened to
have it.

**Ordering matters and is not optional.** Retiring the copy before the
artifact path is proven would leave no way to bootstrap a fresh host.
The staging is: build a toolchain artifact from packages on a Cix host;
prove a fresh image can be seeded from it; only then retire the copy.

## Alternatives considered

**Copy a named list of files instead of whole directories.** Rejected
as the primary answer. It shrinks the image and keeps the fundamental
problem — the image's contents are still whatever some host had, now
filtered by a list somebody has to maintain, whose omissions surface as
confusing build failures. It is a reasonable interim step if the
staging above proves slow, and nothing here forbids it.

**Refuse to publish images containing unowned files.** Rejected as a
fix, adopted as a check. It attacks the symptom — the artifact other
machines download — and leaves the cause. As point (4) it is valuable
precisely because it prevents regression after the cause is gone.

**Leave it.** Rejected. It is not merely size: while 93% of an image is
undeclared, an image manifest does not describe an image, which makes
ADR-0208's "each image has one job" unenforceable and every
reproducibility claim about images untrue.
