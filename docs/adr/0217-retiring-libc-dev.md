# 0217 — Retiring libc-dev

## Status

Accepted. Completes the work [ADR-0216](0216-the-glibc-floor-is-closed.md)
began: that ADR gave every image a C library it *built*; this one removes the
last package that supplied C library **headers and link objects** it did not.

## Context

`libc-dev` staged a development environment for a C library — 106 named glibc
headers, the kernel UAPI trees, and the link objects (`crt1.o`, `crti.o`,
`crtn.o`, `Scrt1.o`, `libc.a`) — by copying them out of whatever machine ran
the original bootstrap. Its own recipe said so plainly:

> *"It does NOT launder the input. These headers still originate from a
> distribution glibc via the original bootstrap; that is the last unclosed
> link in this project's self-hosting."*

It was, by its own description, temporary. The condition for removing it was
a real glibc built from source, which ADR-0216 delivered.

### What made it urgent rather than tidy

`libc-dev.recipe` is pinned to glibc **2.36**. The runtime has been glibc
**2.44** since ADR-0216. Every package in the catalogue was therefore
compiling against headers eight releases behind the library it linked, and
`libc-dev` hid that completely — it reached every build environment
containing a compiler through `gcc`'s own `pkg_depends`, and GCC searches
`/usr/include/x86_64-linux-gnu` (which libc-dev owned) *before*
`/usr/include` (which glibc owned).

That is not a theoretical mismatch. It produced:

- `binutils` failing its own configure with `expected declaration specifiers
  before '__COLD'` — a macro glibc 2.44 defines and 2.36 does not, three
  steps downstream of the cause (#187);
- `m4` failing on a `posix_spawn_file_actions_addchdir` redeclaration that
  only exists against 2.44's headers, invisible for as long as 2.36's were
  winning (#198);
- and, silently, every other package built during that period.

## Decision

**Delete `libc-dev`.** Its two jobs are split between packages this platform
builds:

| what libc-dev supplied | now supplied by |
|---|---|
| glibc headers (`stdio.h`, `sys/*`, ...) | `glibc`, at both `/usr/include` and `/usr/include/x86_64-linux-gnu` (2.44-8) |
| CRT objects and `libc.a` | `glibc` (2.44-9 onward, with a build-time gate that links *and runs* a TCC-built binary against them) |
| kernel UAPI headers (`linux/`, `asm/`) | `linux-headers` |

Every current recipe already declares that pair; no recipe but `libc-dev`
itself still names `libc-dev`.

The artifact cache's `libc-dev` bytes are **purged**, not merely orphaned. A
package whose whole purpose was to carry another distribution's headers into
this one should not remain installable by accident.

## Consequences

**Accepted cost: pre-retirement recipe revisions become unbuildable.** 354
historical revisions name `libc-dev` in their declared tools. Recipe versions
are immutable (ADR-0107) and stay exactly as they are — but with the package
gone, they can no longer be built from source. This is deliberate and is the
price of the retirement:

- an installed package does **not** need its build dependencies present, so
  nothing currently deployed is affected;
- every one of those packages has a current revision that declares
  `glibc` + `linux-headers` instead, which is what a rebuild would use;
- rebuilding all of them pre-emptively was considered and rejected — 30 of
  the 39 target versions have never been built anywhere, so it is many hours
  of building to preserve the rebuildability of revisions that would never be
  rebuilt. In a rolling-release system each package picks up its current
  recipe the next time it is genuinely built.

**A prerequisite, not an afterthought.** `kernel-builder` had `libc-dev` and
**no `linux-headers`**, so libc-dev was the only source of kernel UAPI
headers in that image. `linux-headers` is installed there *before* libc-dev
is removed. `iso-builder`, the other image carrying it, already had both.

**Verification is doing the job, not reporting success.** Each affected image
is checked by building the thing it exists to build — a kernel from
`kernel-builder`, an ISO from `iso-builder`. This project has been taught
twice that assembly reporting success is not evidence the output works
(CLAUDE.md's own note on the two glibc-mismatch panics).

**The test floor was carrying the same defect and is fixed first.** It pinned
`libc-dev 2.36-3` *and* `glibc 2.44-6` together, so every local fixture build
reproduced the exact 2.36-headers/2.44-library mismatch inside the one place
meant to catch it. The floor is now `glibc 2.44-12` + `linux-headers
6.18.40-4` — the same pair every real recipe declares.

## Alternatives considered

**Keep it, pinned to 2.44.** Rejected: it would still be a package that
copies headers off the build host, which the Build Provenance Mandate
forbids, and the pin would drift again the moment glibc moved.

**Leave the cache artifacts in place.** Rejected by the owner, and correctly:
unreferenced bytes that install a foreign distribution's headers are exactly
what should not survive a retirement.

**Leave it installed but undeclared.** Rejected: an image carrying a package
nothing declares is precisely the "orphaned config" state the One Source of
Truth maxim exists to prevent, and it would mask the next instance of this
same problem the way this one was masked.
