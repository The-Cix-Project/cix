# 0226 — gcc is an ordinary choice for a third-party package, not a debt

## Status

Accepted. Amends the bar in [ADR-0224](0224-the-toolchain-tenet.md) §3
for third-party packages only. ADR-0224 stands unchanged in every other
respect, and §1 in particular is untouched.

Requested by the owner: *"outside the core system itself, anything
moving to gcc because tcc is too difficult, then it should be the
natural change, right?"*

## Context

ADR-0224 drew one line correctly and a second line too tightly.

The correct line is §1: **Cix's own code is TCC, always, with no bar and
no exception.** That is where the self-hosting claim actually lives, and
nothing here touches it.

The line drawn too tightly is §3, which made a third-party package's
move to gcc an *exception* requiring four conditions and a tracked
issue, described as "a debt with a ticket". That framing has three
costs, and this project has now paid all three:

**It misdescribes what is happening.** Cix's gcc is Cix-built,
three-stage bootstrapped, with stage 2 byte-identical to stage 3. A
package built by it is exactly as self-hosted as one built by TCC. The
compiler used is an implementation detail of that package's build, not a
statement about the platform's sovereignty. ADR-0224 says this itself
and then treats the choice as a concession anyway.

**It produces effort with no product in it.** The bar's real effect is
to make someone fight a third-party build system until TCC wins — work
that ships nothing, on code this project does not own and did not write.
The owner's instruction on this is direct: *"we should not be
battling."*

**It makes the record less honest, not more.** A bar that must be
cleared invites reasons written to clear it. The #222 audit found the
predictable result: every one of the 21 grandfathered recipes had a
justification, one of them (`btrfs-progs`, "TCC cannot parse
`__thread`") was measurably false against the current compiler, and the
real blocker turned out to be an unimplemented `-MG` flag. The reason
was written once, to satisfy a bar, and then rotted.

## Decision

**For a third-party package, choosing gcc because TCC is difficult is
an ordinary engineering decision and needs no justification beyond
saying so.**

Concretely, amending ADR-0224 §3:

- The **four-part bar no longer applies** to third-party packages. TCC
  remains the default and the first thing tried, because it usually
  works and the result is smaller; but when it does not, gcc is the
  natural next step rather than a last resort.

- **`pkg_toolchain="gcc"` and `pkg_toolchain_reason="..."` are still
  required**, and their purpose changes. They exist so a reader can see
  at a glance which compiler built a package and why — auditability, not
  permission. A reason may be as ordinary as "TCC needs `-MG`, which it
  does not implement".

- **A tracked issue is no longer required.** File one when the TCC gap
  is worth fixing for its own sake, not as a toll for the exception.

- **The asserted count in `test_toolchain_policy` stays**, and its
  meaning changes with it. It is no longer a ceiling on debt. It is a
  visibility device: the set cannot change without someone editing a
  number in a diff, so a package moving compilers is always a deliberate,
  reviewable act. That was always the part that worked.

**ADR-0224 §1 is unaffected and remains absolute.** Cix's own code is
TCC, and a TCC bug that blocks it is fixed rather than routed around.

## Consequences

The gcc set will probably grow, and that is the intended outcome rather
than a regression. What must not happen is a package moving compilers
silently — the count assertion is what prevents that, and it is kept for
exactly that reason.

Reasons recorded under this ADR should be re-measured when the compiler
changes, not treated as permanent. `btrfs-progs` is the worked example:
its stated reason became false when the compiler was replaced, and
nothing noticed until someone re-ran it. A reason is a measurement with
a date on it, not a verdict.

The #222 backfill is therefore about writing down what is true now, in a
field a reader can find, rather than about shrinking a list.
