# 0251 — A package artifact carries what the platform runs, and nothing else

## Status

Accepted

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
- The prune is written in at least **twelve different spellings**, no two
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
| `.o`, `.a`, `.ko` | `--strip-debug` | `.symtab` is load-bearing for linking and module loading |

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

- **`libc.a` has no consumer.** Across every recipe revision in the tree
  the only reference to it is glibc's own copy loop — the loop that
  triplicates it. Nothing links it.
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

The clause is not even new behaviour. **Nineteen recipes already delete
archives and libtool metadata by hand** — `xz` removes `liblzma.a` and
`liblzma.la`, `flex` removes `libfl.a` and `libfl.la`, and so on. As with
the prune, the intent was already unanimous; only its scope was
inconsistent.

### 3. Libtool metadata is not shipped

`*.la` files describe how to link something that is no longer being
linked. Nineteen recipes already delete them; the other ninety-six ship
them.

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
