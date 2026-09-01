# ADR-0223: The compiler is a pinned upstream snapshot, not a patched release

Date: 2026-09-01

## Status

Accepted. Supersedes nothing; the three-tier TCC policy (ADR-0222 and
`CLAUDE.md`) is unchanged, and this decides *which* TCC.

## Context

Every line of Cix's own code, and every third-party package outside the
short Tier 3 exception list, is compiled by TCC. Until now that meant
the **0.9.27 release of December 2017**, carrying six local patches this
project wrote.

Issue #216 made that untenable. The 0.9.27 release assigns two
simultaneously-live locals the same stack slot when a `case` label of an
enclosing `switch` sits inside the block that declares them. In perl's
`regexec.c` that put `SV *ret` and `REGEXP *re_sv` at the same address,
so `re_sv = NULL` destroyed a value assigned three lines earlier and
`miniperl` segfaulted on every build.

The severity is not the crash. Where two aliased locals are only read
and written rather than dereferenced, the fault corrupts values and the
program exits 0. This project has shipped that twice already — a
TCC-built `tar` that listed one member per archive and reported success
(#122), and `squashfs-tools` byte-swapping every on-disk field while
reporting success. perl crashed only because one aliased variable
happened to be dereferenced immediately after the other was zeroed.
Every package this compiler had ever built was in scope.

Two options existed.

**Patch 0.9.27.** No patch exists to take: Debian carries four patches
for tcc and none touches code generation. Writing one means maintaining
a code-generation patch by hand, forever, against a compiler with no
maintainer and no release since 2017.

**Pin an upstream snapshot.** Upstream development continues on the
`mob` branch, where `tccgen.c` has since gained a real scope structure —
`cur_scope`/`prev_scope`, `vla.locorig`, cleanup lists, `pending_gotos`
— a rewrite of exactly the area at fault. This is also what Debian
itself does: it ships `0.9.27+git20200814.62c30a4a`, not the release.

## Decision

**The compiler is a pinned upstream commit.** `tcc.recipe` builds
`2ba12e83b3599ca8f5d50c179fe5138fe956f0c9` (2026-08-08), reporting
itself as 0.9.28rc.

Pinned to a **commit**, never to `mob`: a moving branch is not a
reproducible source, and a recipe whose output depends on when it ran
cannot be reasoned about. The checksum was verified against two
independent fetches of that exact commit.

## Consequences

**Five of six local patches are gone,** each verified individually
against the new compiler rather than dropped hopefully: `lib/atomic.c`,
`dso_handle.o`, the `bcheck.c` malloc-hooks edit, the #122 do-while fix,
and our `__has_include` implementation. Only the `__ATOMIC_*`
predefines remain, because upstream still does not provide them.

**Four issues move at once.** #216 is fixed; #212 gains real C11
(`_Atomic`, `_Static_assert`, `stdatomic.h`, `stdalign.h`,
`stdnoreturn.h`); #209's `-pthread` is accepted rather than silently
dropping the source file; #208 goes from 15 missing builtins to 3.
`bswap16/32/64` are still missing and still matter, because they are
byte-order macros.

**Every gate is kept, including for bugs this compiler no longer has.**
Their value is proving those stay fixed across future snapshots, and one
is added for #216. That judgement paid immediately: dropping
`lib/atomic.c` was wrong — upstream defines only the size-suffixed
`__atomic_test_and_set_N` and no `__atomic_clear` — and the gate caught
it on the first build rather than a package doing so later.

**Two latent bugs in Cix's own code surfaced,** because the new compiler
is stricter. An `enum` was used 350 lines before its definition (C has
no forward-declared enums; GCC and old TCC both tolerated it for five
releases), and four files used fixed-width integer types with no
`<stdint.h>` in scope. Both are ours and both are now fixed.

**One behavioural reversal to plan for.** Inline linkage flips: bare
`inline` starts working — which is what gnulib needs, since `_GL_INLINE`
expands to it — and `extern inline` starts failing. Upstream is
arguably the more correct of the two under C99. `static inline` works on
both and is the spelling recipes should use.

**The library search path had to be widened.** Upstream searches
`/usr/lib` and `/usr/lib/<triplet>`; this platform installs shared
libraries under `/lib/<triplet>`, so `-lcrypto` was not found. The
recipe derives the triplet from where libc actually is and passes the
paths to **all three** bootstrap configures.

**The bootstrap itself had to be corrected,** and this is the finding
most likely to matter again. It compared two builds that had been
compiled by *different* compilers — the installed release and the new
source — so it only ever passed because previous revisions barely
changed code generation. Across a 2017-to-2026 jump it failed and
reported that the compiler does not reproduce itself. It does; the
comparison was wrong. It is now a real three-stage, where stages 2 and 3
are both built by compilers built from this source, and the seed
compiler drops out of the result entirely.

**Self-hosting is preserved.** The new compiler builds under the old
one, so it is seeded exactly as every previous revision was. Nothing
foreign enters.

**perl remains on gcc.** #216 is fixed and its `miniperl` no longer
crashes, but a TCC build now fails later, linking an XS module, with tcc
itself segfaulting. `-shared` was verified to work in general before
trusting the upgrade — five cases including eight objects at `-O2` —
so that is a narrow issue, tracked separately, and perl's Tier 3
exception stands with its reason updated rather than left stale.

## Alternatives considered

**Move affected packages to gcc as they break.** Rejected: it converts
"TCC by default" into "gcc by default" one package at a time, and it
cannot work at all for a fault whose full trigger set is unknown. You
cannot audit call sites for a bug you can only partly characterise.

**Stay on 0.9.27 and accept the risk.** Rejected: the failure mode is
silent corruption, and the project has already shipped that twice.
ADR-0222 permits a *declared* capability loss; it does not permit an
undeclared correctness one.
