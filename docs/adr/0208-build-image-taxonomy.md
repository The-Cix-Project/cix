# 0208 — one job per build image

## Status

Accepted. Refines the image-recipe mechanism (ADR-0122/ADR-0123) by
saying what the build images *are*, which no document has ever stated.
Prompted by [issue #167](https://git.home.arpa/itdlabs/cix/issues/167)
and the drift found alongside it.

## Context

This project has five images that exist to build things, and until now
none of them had a written purpose. That absence had consequences, all
of them observed rather than imagined:

**The image recipes are captures, not designs.** Every one of them says
so in its own header — *"The package list is exactly what the source
host had installed... Captured from 192.168.15.95."* The manifest is
whatever that box happened to contain at the moment it was snapshotted,
frozen into git. Nobody chose those package lists; a host did.

That is how `cix-builder` — the image whose entire purpose is building
Cix, which is TCC-only by Immutable Maxim — came to carry
`gcc:pinned:9.5.0-6`. Not a decision: gcc was present on the box during
the gcc-bootstrap work, so the capture took it. And it is not inert. The
hazard is already documented in `CLAUDE.md`: once a real gcc is
reachable in a build sandbox, a `configure` that does not explicitly pin
`CC=tcc` silently selects it, producing binaries nobody intended — which
is exactly how an `sshd` shipped with PAM on its link line and no PAM
actually recorded in the ELF.

**`dev` is used, but not for what its name or the docs suggest.** Its
real job is the ISO toolchain: `grub`, `sbsigntools`, `xorriso` and
`mtools` are installed into it, then `pkg hostbuild isotools` harvests
them into the artifact `cixd` uses to assemble installer ISOs
(ADR-0063/0064). Meanwhile
`docs/guides/kernel-build-and-ab-updates.md` told operators to build
kernels with `--build-image=dev` — an image whose manifest contains
neither `gcc` nor `kmod`, both of which `kernel.recipe` requires. The
documented self-hosted kernel build could not work as written, and the
name `dev` is precisely what made that mistake easy: a name that means
"general" invites being used for anything.

**The drift is real and it compounds.** On 192.168.15.95, in one
session: `cix-builder` was missing the `gcc` its own recipe lists; no
`dev` image existed at all; `binutils` was at a version predating its
own `pkg_depends="zlib"` fix, so a composed build environment held an
`ar` that could not start; and `libc-dev` predated the `libdl.a` fix, so
`tcc` could not link itself. Each was already solved in the repository.
The box simply did not have the answers. Untitled images make that state
impossible to check, because there is nothing to check *against*.

## Decision

**Every build image has exactly one job, stated in its own recipe, and
carries only what that job needs.**

| Image | Its one job | Why it holds what it holds |
|---|---|---|
| `cix-builder` | Build Cix itself — `cixd`, `cixctl`, `web/`, `mkbootroot` | TCC only. **`gcc` is removed.** ADR-0001 makes TCC the exclusive toolchain for this project's own code; an available gcc here is not a convenience, it is a way to violate that silently. |
| `kernel-builder` | Build the kernel and its modules | The one image that legitimately holds a real GCC toolchain, plus `kmod` for the `depmod` the recipe ends with, and `bc`/`bison`/`flex`/`elfutils`, which the kernel's own build genuinely requires. |
| `iso-builder` (was `dev`) | Produce the `isotools` artifact — `grub-mkrescue`, `sbsign`, `xorriso`, `mtools` | Renamed. "dev" describes no job, and a name that means everything gets used for anything — which is exactly what happened. |
| `gcc-tcc-bootstrap` | Build GCC under TCC — the bootstrap rung | Already single-purpose; named for its job. Left alone. |
| `cix-hosttools` | Supply host-side binaries staged into the control-plane squashfs | **Not a build image at all**, despite sitting alongside them: nothing compiles here. Listed so the distinction is written down rather than inferred. |

Three rules follow, and they are the part that actually prevents
recurrence:

1. **A build image's manifest is a declaration, not an observation.** It
   states what the image is *for*. A capture may be how the bytes get
   produced, but the package list is reviewed as a design, and anything
   in it that no job needs comes out.
2. **A recipe that needs a tool declares it.** The composed build
   environment (ADR-0199) is built from declared tools alone, so an
   undeclared dependency is not a small omission — it is a build
   environment holding a tool that cannot start. Both real failures
   above (`ar` without `zlib`, `tcc` without `libdl.a`) were exactly
   this.
3. **The guides name an image by its job.** No document tells an
   operator to build a kernel in the ISO image again.

## Consequences

**These are not text edits.** An image recipe's version *is* the hash of
its package list, and the artifact URL derives from that hash
(ADR-0123). Changing one entry changes the version and invalidates the
pinned artifact, so every change here means building the image for real
on a Cix host and re-capturing it. That cost is the reason to decide the
shape once, deliberately, rather than adjusting manifests one package at
a time.

**Artifacts come from Cix hosts, always.** An image artifact is
published from a real host that built it. Nothing compiled anywhere else
enters the cache — that rule predates this ADR but is restated because
the whole taxonomy is worthless if its contents can come from elsewhere.

**Removing gcc from `cix-builder` is a real change with a real risk**, and
it is the right one. Anything that turns out to need gcc there was, by
definition, not building with TCC — which is the bug, not the fix. If
something breaks, it breaks loudly at build time and names itself,
rather than silently producing a binary built by the wrong compiler.

**`dev` → `iso-builder` breaks references.** Every `--build-image=dev`
and `--image=dev` in the guides has to move with it. That is the point:
those references are what encoded the confusion, and leaving one behind
recreates it.

**Naming a job does not make a host comply.** This ADR gives the fleet
something to be checked against; it does not perform the check. Whether
a running host still matches its own image recipes is a separate,
worthwhile question — the drift catalogued above went unnoticed for
months precisely because nothing ever asked.
