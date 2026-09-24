# 0251 — A package artifact carries what the platform runs, and nothing else

## Status

Accepted, with clause 4 withdrawn by [ADR-0306](0306-a-package-keeps-its-documentation-and-its-licence.md). Clauses 1, 2, 3 and the enforcement rule stand as written; the finalize phase no longer removes `usr/share/{man,info,doc,locale,i18n}`.

## Context

The `glibc` artifact in the cache is 56.8 MiB compressed, 146.4 MiB
unpacked across 2114 entries. It was measured after the owner asked why
an installer ISO is 218 MiB when the whole thing should be "kernel + cix
+ some decoration". The breakdown:

| bucket | size unpacked | share |
|---|---|---|
| static archives (24 files) | 78.39 MiB | 54% |
| shared objects | 37.48 MiB | 26% |
| i18n locale **sources** (607 files) | 15.04 MiB | 10% |
| everything else | 15.51 MiB | 10% |

Three separate findings, each measured:

1. **The artifact is unstripped.** `libc.so.6` carries 11.42 MiB of
   sections, of which `.debug_*` plus `.symtab` are **9.46 MiB — 83%**.
2. **`libc.a` is shipped three times**, 22.43 MiB each. `pkg_install()`
   copies a fixed list of link-time files into both multiarch directories
   in addition to `usr/lib`, and `libc.a` is on that list.
3. **Nothing consumes the locale sources.** `usr/share/i18n` ships 607
   files; `usr/lib/locale`, where compiled locales would live, ships
   **zero entries**. `localedef` is packaged but never runs.

Finding 1 and finding 3 are not glibc's alone. Fleet-wide the cache holds
112 packages totalling 1004 MiB, led by gcc at 261.6 MiB and
cix-installer at 217.9 MiB.

### Why this happened

Nothing in this platform defines what a package artifact *is*. Every
recipe runs `make install DESTDIR=$PKG_DESTDIR` and ships whatever a
general-purpose distribution would want, and then each recipe
individually tries to clean up after it. Measured across the recipe tree:

- **37 of 115** latest revisions prune anything at all. **78 do not.**
- The prune is written in at least **twenty-one different spellings**, no two
  agreeing on scope. Counted across all revisions: `man` appears 280
  times, `info` 135, `doc` 123, `locale` 77, `i18n` **13**.

`glibc` is the worked example, and it is instructive precisely because
its recipe is not ignorant. It prunes — `man`, `info`, `doc` — and misses
`locale` and `i18n`. It strips — `strip --strip-debug` on `*crt*.o` — and
misses every shared object and executable in the package. The recipe
author knew about both tools and applied each to a hand-picked subset.

That is the actual defect: **the scope of a universal rule is being
re-guessed, per recipe, 115 times.** It is the same shape as three
problems this project has already fixed the same way — per-recipe tool
declarations with no gate (#302, ADR-0250), per-recipe library paths
(#184), per-recipe workarounds for one compiler bug (#220) — and in every
case the fix was to define the rule once and enforce it centrally.

## Decision

**A package artifact carries what this platform runs, and nothing else.**

The rule is defined once, in `daemon/src/pkg.c`, and enforced as a
`pkg_finalize` phase that runs in the build container after
`pkg_install()` returns. It is not a recipe convention, because a recipe
convention is precisely what produced the inconsistency above.

Three clauses, each derived from a tenet this platform already holds
rather than from taste:

### 1. ELF outputs are stripped

Debug information is not something this platform runs. Applied by output
kind, because the correct flag differs and getting it wrong breaks
linking or module loading silently:

| kind | flag | why |
|---|---|---|
| `.so*`, executables | `--strip-unneeded` | preserves `.dynsym`, which is all the linker and `elfcheck` read |
| relocatable objects (ELF `ET_REL`: `.o`, `.ko`, Go's `.syso`) | `--strip-debug` | `.symtab` is load-bearing for linking and module loading |
| archives | *not stripped* | `!<arch>` does not imply an archive of ELF — `go-bootstrap` ships Go 1.4's `pkg/linux_amd64/*.a`, which `strip` rejects outright |

*Corrected 2026-09-24:* this row said `.o`, `.ko`, and the policy matched those file names. A relocatable object named otherwise got `--strip-unneeded` — Go's race runtime, `race/internal/amd64v1/race_linux.syso`, was stripped that way in `go@1.24.9-3`, and every `go build -race` then failed to link with `hole in findfunctab` (192.168.15.95, `probe-go-race@1`). The kind is now read from the ELF header's `e_type`, which is what the reasoning above always meant by "kind".

### 2. Static archives are dropped where a shared library supersedes them

CLAUDE.md's toolchain rule is "Dynamic linking against system glibc
always — never `-static`". An archive whose shared counterpart ships
beside it is therefore, on this platform, dead weight by charter.

The rule is: **drop `libfoo.a` if and only if `libfoo.so*` ships in the
same DESTDIR.**

This is deliberately a rule and not an allowlist. An allowlist is a
second thing to maintain and the "someone forgets" failure returns the
first time a package adds an archive. The rule is safe by construction,
and it protects exactly the archives that must survive without anyone
naming them: `libtcc1.a` (no `libtcc1.so` — dropping it kills the
platform's compiler), `libgcc.a` and `libgcc_eh.a` (`libgcc_s.so.1` is a
different basename), `libc_nonshared.a` (no shared counterpart; gcc's
dynamic links need it), and `crt*.o`, which is not an archive at all.
Equally it drops, without an entry anywhere: `libc.a`, `libm.a`,
`libpthread.a`, `librt.a`, `libdl.a`.

This clause was checked against the fleet before adoption rather than
reasoned about, because a wrong drop breaks a downstream build:

- **No recipe references `libc.a` by path.** Across every recipe revision
  in the tree the only mention of it is glibc's own copy loop — the loop
  that triplicates it.
- **The one `-static` link in the tree is disabled on purpose.**
  `gcc/16.2.0-16/build.sh` describes libtool's "can a statically linked
  program dlopen itself" probe, which executes a `-static` conftest; that
  recipe exports `lt_cv_dlopen_self_static=no` precisely so it never runs
  (it deadlocked in `__futex_wait` for 94 minutes in the 16.2.0-6 build).
  **That is not the same as proving gcc's three-stage bootstrap never
  needs `libc.a` anywhere else, and that is not proven.** glibc 2.44-14 is
  therefore staged and unpublished until a gcc rebuild confirms it.
- **Every absolute-path reference to an archive is a package referencing
  its own** (`liblzma.a` in xz, `libproc2.a` in procps, `libpkgconf.a` in
  pkgconf, `libfl.a` in flex, `libltdl.a` in libtool, `libfreetype.a` in
  freetype), and those references are consumed during that package's own
  build, which completes before finalize runs. There is no cross-package
  consumption of a `.a` by path.
- **`freetype` ships an archive with no shared counterpart**, and the
  rule keeps it — the case the rule exists to protect, arrived at without
  anyone naming freetype.
- The remaining failure mode is loud, not silent: with the `.so` still
  present, `-lfoo` continues to resolve. Only an absolute-path reference
  could break, and there are none.

The clause is not even new behaviour, though the numbers are smaller than
the prune's: counting latest revisions, **16 already delete `.la` files by
hand and 6 already delete an archive** — `xz` removes `liblzma.a` and
`liblzma.la`, `flex` removes `libfl.a` and `libfl.la`. As with the prune,
the intent was already there; only its scope was inconsistent.

### 3. Libtool metadata is not shipped

`*.la` files describe how to link something that is no longer being
linked. 16 of the 115 latest revisions already delete them; the other 99
ship them.

### 4. Documentation and locale trees are not shipped

`usr/share/{man,info,doc,locale,i18n}`. There is no man reader on this
platform, and no compiled locale for the sources to feed. This clause is
what the 37 hand-written prunes were each approximating.

### Enforcement, and what happens when the tool is missing

If the package produced ELF output and `strip` is not on `PATH`, the
build **fails**, naming `binutils` as the dependency to declare. It does
not skip silently. This is ADR-0250's finding applied to the finalize
phase itself, and it is consistent with ADR-0199: a build container is
composed from exactly `pkg_build_depends`, so a build that needs `strip`
declares `binutils`. Measured before adopting this: **103 of 115 recipes
already declare it**, and of the twelve that do not, seven are probes and
two ship no ELF at all.

The scan and the ELF test use bash builtins only — `shopt -s globstar`
and a four-byte `read` of the magic — so the finalize phase adds no tool
dependency of its own beyond `strip`, and only when there is ELF to
strip. A package that ships no ELF needs nothing new.

## Consequences

- Recipes stop hand-pruning. Existing `rm -rf .../share/man` lines become
  redundant rather than wrong, and are removed as each recipe next
  revises — not in a sweep, because published revisions are immutable
  (ADR-0107).
- **Existing published artifacts do not shrink.** They are immutable. The
  fleet shrinks as packages next rebuild, so gcc's 261.6 MiB waits for
  gcc's next bootstrap. This is a property of the decision, not a defect
  in it.
- Three recipes that ship ELF without declaring `binutils` — `htop`,
  `lldap`, `thinc-bridge` — need that declaration added at their next
  revision.
- `strip` was verified against TCC-produced ELF before adoption: a shared
  library and executable built by TCC survive `--strip-unneeded` with
  `.dynsym` intact and still run. TCC emits no debug information to begin
  with, so the saving falls almost entirely on the gcc-built packages —
  which are precisely the large ones.
- The policy is daemon code, so it takes effect on deploy, and only for
  builds run afterwards.

## Alternatives considered

**Fix the glibc recipe.** It is the largest single win and it is one
line. Rejected as the whole answer: it addresses one of 115 recipes and
leaves the mechanism that produced the inconsistency fully intact. It is
still done, as the first consumer of the policy rather than instead of
it.

**Enforce host-side, after the artifact is extracted.** Needs no recipe
to declare anything and cannot be forgotten. Rejected because it moves
the definition of a package's contents outside the build that produces
them, and because ADR-0078's host-tools set would grow a `strip` whose
own library resolution is the trap ADR-0154 documents.

**A recipe-level opt-out for the static-archive clause.** Rejected as a
knob with no current caller — the rule's construction already protects
every archive this platform links against. If a real case appears it gets
added then, with the case recorded.

---

## Addendum (2026-09-07): clause 2 does not reach an archive with no members

Clause 2 drops an archive whose shared counterpart ships beside it, because on a
platform that links dynamically always such an archive is a duplicate of bytes
already present. The rule was written against `libc.a` — 22.43 MiB, copied into
three directories, deleted again by this phase.

It matched on the stem alone, and that is wider than its own reason. An archive
with **no members** is eight bytes: the `!<arch>` magic and nothing else. It
duplicates nothing, so the justification above does not apply to it — and for
glibc's folded-in stubs it is not dead weight at all, it is the link-time
contract.

glibc ≥ 2.34 folds `pthread_create`, and the `rt` and `dl` entry points, into
`libc.so.6`. What it still installs is `libpthread.a` / `librt.a` / `libdl.a` at
eight bytes each, `libpthread.so.0` as a runtime stub, and **no `libpthread.so`**.
So `ld -lpthread` — which every gcc driver's `-pthread` expands to — has exactly
one file it can resolve against, and clause 2 was deleting it on a stem match
with the runtime stub. Every gcc-toolchain package passing `-pthread` then failed
to link with `cannot find -lpthread` (#324, found on `btop` while restoring the
fleet — the first such package rebuilt from source since this clause landed).

The scan now classifies a member-less archive as "keep" before clause 2 ever sees
it. `libc.a` is unaffected: it has real members and is still dropped.

**The fix is not a `libpthread.so` symlink.** That would work, and it would be
wrong: it makes `-lpthread` record a `DT_NEEDED libpthread.so.0` in every binary
that passes `-pthread`, a runtime dependency edge upstream deliberately does not
create. Keeping the empty archive is what glibc itself is built to expect, and it
adds eight bytes.

Emptiness is measured with `wc -c`, not with a `read` builtin, and that is a
correction rather than a preference. **The shell cannot measure binary at all:** a
NUL byte cannot be held in a variable, and `read` stops at one regardless of `-N`
or `-d ''`. Both spellings were written, and both reported eight bytes for a
64-byte NUL-padded archive — which would have kept every archive on the system
rather than only the empty ones. `test_pkg_finalize` caught exactly that, because
its own `wr_ar()` helper pads with NUL, and it failed the build with `libc.a
survived and must be removed`.

`wc` comes from coreutils, which the prune already required for `rm`, so this asks
for no package it did not already need; it is named in the tool check beside `rm`,
per ADR-0250.

`test_pkg_finalize` asserts an empty `libpthread.a` beside a `libpthread.so.0`
survives, with `libc.a` beside `libc.so.6` as the control.
