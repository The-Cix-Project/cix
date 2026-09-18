# 0306 — A package keeps its documentation, and therefore its licence

## Status

Accepted. Amends [ADR-0251](0251-a-package-artifact-carries-what-the-platform-runs.md),
whose clause 4 is withdrawn; its clauses 1, 2, 3 and its enforcement rule stand
unchanged. Issue [#168](https://git.home.arpa/itdlabs/cix-build-system/issues/168)
in the build-system tracker carries the follow-on.

## Context

ADR-0251 gave the finalize phase four rules. Clause 4 removed
`usr/share/{man,info,doc,locale,i18n}` from every staged tree, on the
reasoning that "there is no man reader on this platform, and no compiled
locale for the sources to feed".

That reasoning is sound for `man` and for `locale`. It was never stated
for `doc`, and the size argument the whole ADR rests on was never
measured for any of the three. ADR-0251's own table accounts for `glibc`
as:

| bucket | size unpacked | share |
|---|---:|---|
| static archives (24 files) | 78.39 MiB | 54% |
| shared objects | 37.48 MiB | 26% |
| i18n locale **sources** (607 files) | 15.04 MiB | 10% |
| everything else | 15.51 MiB | 10% |

`man`, `info` and `doc` are inside that last row and were never separated
from it. Clauses 1 and 2 recovered ~80% of the artifact. Clause 4 rode
along on their argument.

The owner put it plainly, reviewing the decision on 2026-09-18:

> I dislike the deleting, I'm not sure it's a healthy practice, even if
> we don't use the files, the penalty size wise of having them is not
> worth the practice.

**And it cost something specific.** `usr/share/doc/<package>/COPYING` is
where GNU packages install their licence text. Clause 4 deleted it from
every artifact it touched. Measured across the 171 installed package
entries on 192.168.15.95 on 2026-09-18, **25 licence files survive
anywhere at all**, and every one of them survives by living somewhere
clause 4 did not look — `usr/share/automake-1.16/COPYING`,
`usr/doc/cmake-4.4/LICENSE.rst`, `usr/share/licenses/fastfetch/LICENSE`,
`usr/lib/python3.13/LICENSE.txt`. The one package still carrying
`usr/share/doc/bison/COPYING` has it because it was built before the
policy reached it.

A platform that redistributes GPL binaries and deletes their licence
texts on the way into the artifact has a problem that is not about bytes.

## Decision

**The finalize phase deletes no directory because nothing reads it.**
Clause 4 is withdrawn in full — `man`, `info`, `doc`, `locale` and
`i18n` all reach the artifact as the build left them.

The other three clauses stand, because each removes something the
platform **cannot use** rather than something nobody reads, and each has
a functional consequence rather than a housekeeping one:

1. **Strip debug sections.** 9.46 MiB of `libc.so.6`'s 11.42 MiB is
   `.debug_*` plus `.symtab`, and this platform does not run a debugger
   against a shipped artifact.
2. **Drop a static archive superseded by a shared object beside it.**
   This is the rule that makes a stray `-ldl` fail loudly at link
   instead of silently resolving against a `.a` — see the environment
   note in `CLAUDE.md` and ADR-0251's own reasoning.
3. **Drop `*.la`.** Libtool metadata describing how to link something
   that is no longer being linked.

The distinction is the decision: **remove what the platform cannot use,
never what it merely does not read.**

## Consequences

- **`usr/share/i18n` comes back, and it is 15.04 MiB per `glibc`
  artifact — 10% of it.** That is the one part of clause 4 with a
  measured cost, and it is recorded here rather than buried, because it
  is the number that would justify reversing this decision if the fleet
  ever needs those bytes. It was accepted knowingly: the rule against
  deleting what is merely unread does not get an exception for the one
  case where the bytes are noticeable, or it is not a rule.
- **Licence texts return to new artifacts, not to old ones** — and not
  to all of them. Published artifacts are immutable (ADR-0107), so a
  package regains its `COPYING` at its next rebuild. But **57 of the
  147 current recipes delete something under `$PKG_DESTDIR/usr/share`
  themselves** (measured 2026-09-18): 37 name `man`, `doc` or `info`
  directly, and some, `xz` among them, remove the whole tree in one
  line. For those, this decision changes nothing until the recipe
  does. That is the half of the fix this ADR does not deliver, and it
  is tracked rather than assumed.
- **This was measured wrong once, which is why the number is here.**
  The first version of this change asserted that no current recipe
  hand-prunes. The regex behind it anchored `rm` at the start of a
  line and every real one is tab-indented, so it found zero where
  there are 57. The claim reached a guide and a commit message before
  a real build contradicted it.
- **This does not by itself give the platform a licence policy.** Most
  upstream `make install` runs never install a licence file at all, so
  preserving what lands on disk cannot answer "what is this package
  licensed under". That wants the package format to record it as a fact,
  which is filed as cix-build-system#168 and is not settled here.
- **Pruning becomes something a build declares and reports, not
  something a shell function does silently.** The mechanism is filed as
  cix-build-system#169: the objection recorded above is not to recovering
  bytes, it is to an unaccountable deletion, and an invisible `rm -rf` is
  how a licence went missing with nothing in any build log naming it.
- `test_pkg_finalize`'s five assertions for these trees are **inverted
  rather than deleted**, and the fixture still stages all five plus a
  `usr/share/doc/zlib/COPYING`. A reintroduced prune fails the gate
  instead of shipping quietly.
