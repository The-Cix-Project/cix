# ADR-0224: The Toolchain Tenet — TCC by right, gcc by evidence

Date: 2026-09-01

## Status

Accepted. Supersedes the informal "3-tier TCC policy" that lived only as
a sentence in `CLAUDE.md` and an aside in ADR-0222.

**Amended for third-party packages by [ADR-0226](0226-gcc-is-an-ordinary-choice-for-third-party-packages.md).** Section 3's four-part bar and its required tracking issue no longer apply to third-party packages, where choosing gcc because TCC is difficult is an ordinary engineering decision. The declared `pkg_toolchain=` / `pkg_toolchain_reason=` fields and the asserted count remain, for auditability rather than permission. **Section 1 is untouched and remains absolute**: Cix's own code is TCC, always.

## Context

Two things have been treated as one thing, and the conflation has been
expensive.

**Self-hosting** is the property that matters: Cix builds Cix, with
Cix's own toolchain, on a Cix host. Nothing foreign enters a binary, a
library, an image or the artifact cache. That is the Build Provenance
Mandate and it is not negotiable.

**TCC-for-everything** is a different thing: small, comprehensible,
ours. It is a real good, and it is *not* what makes the platform
sovereign.

Cix's gcc is built by Cix — a three-stage bootstrap whose stage 2 is
proven byte-identical to stage 3, seeded from a discarded ambient
compiler that contributes no file to the result. **Using it costs
nothing in self-hosting.** Every hour spent making TCC swallow gnulib,
perl, bird or libcap buys simplicity, not independence.

That cost has been real. In one session: `perl` (a codegen fault),
`bird` (configure refuses any compiler not identifying as GCC), and
`libcap` (an attribute TCC cannot parse) each consumed a full
investigation. Meanwhile the actual backlog — a daemon that can be
wedged, images carrying a developer's `/usr`, a bootstrap that is
circular — waited.

The failure mode of *not* having a rule is not that gcc gets used. It
is that gcc gets used **without anyone noticing**, one reasonable
decision at a time, until "TCC by default" is folklore. This project has
already watched that happen with drift: 34 of 139 installed packages
were behind their recipes and nobody knew, because nothing counted them.
A rule nobody can count is a rule that has already been broken.

## Decision

### 1. Cix's own code is TCC. Always. There is no bar and no exception.

The daemon, the CLI, the netplane data plane, the runtime library, the
host tools, the installer, the test suite — everything in this
repository — is compiled by TCC. If TCC cannot compile Cix's own code,
that is a bug in our code or a bug in TCC, and it gets fixed. It is
never routed around.

This is where sovereignty actually lives, and it is not subject to
convenience.

### 2. Third-party packages are TCC by default.

A recipe reaches for TCC first and is expected to do real work to make
that succeed: declare the tools it needs, supply a missing builtin
through a shim, correct a flag, patch a build system's assumption.
Nearly all of them do.

### 3. A package moves to gcc only by clearing the bar, and the bar is
   four things, all of which must hold.

**(a) Measured, not assumed.** The TCC failure has been reproduced and
its exact error text is recorded in the recipe. "It probably will not
work" is not evidence. `bird` was re-tested against a new compiler
before its exception was granted, precisely because two sibling packages
had just been fixed by that upgrade.

**(b) Not fixable at recipe level.** Not a missing declared tool, not an
unsupported flag, not a shim for a missing builtin, not a `sed` against
a build system. These are the common causes, they are cheap, and they
must be exhausted first.

**(c) In a recognised class.** One of:
   - a **compiler identity check** (the build refuses anything not
     identifying as GCC — no capability can satisfy it);
   - a **missing language feature** TCC does not implement;
   - a **codegen fault** in TCC;
   - a build system requiring **GCC internals** (its own specs,
     plugins, or private search paths).

**(d) Tracked, not accepted.** An issue exists against the TCC gap and
is linked from the recipe. An exception is a debt with a ticket, not a
verdict.

### 4. Every gcc-built package declares itself, and the number is
   asserted.

A recipe that builds with gcc sets:

```sh
pkg_toolchain="gcc"
pkg_toolchain_reason="<class>: <what fails, in one line> (#<issue>)"
```

`test_toolchain_policy` asserts three things: that every recipe invoking
gcc declares it, that every declaration carries a reason and an issue
reference, and that **the total count matches a number written in the
test**. Adding an exception therefore requires editing that number, in
a diff, deliberately — the same mechanism that keeps the REST surface
honest (`test_apigen`'s route count).

This is the whole anti-drift device. Not a principle: a number that
changes visibly.

## Consequences

**Battling stops being a virtue.** A recipe that has cleared the bar
moves to gcc and the session moves on. The judgement of "is this worth
more hours" is replaced by four checkable conditions.

**The exception list stays small and legible**, because growing it costs
a visible edit to an asserted count, and because condition (d) means
each entry names the compiler gap it is waiting on. When TCC gains that
feature, the exception can be retired — and there is a list to retire
from.

**Existing exceptions are grandfathered but not exempt.** `kernel`,
`openssl`, `gcc`, `perl` and `bird` must each carry the declaration and
reason. Where the class is "codegen fault" or "missing feature", the
linked issue is the route back.

**Nothing changes about provenance.** gcc here is Cix's own. A package
built with it is as self-hosted as one built with TCC. Anyone tempted to
re-litigate that should read the Build Provenance Mandate, which is
about *where bytes come from*, not which of our two compilers produced
them.

## Alternatives considered

**Keep TCC absolutely everywhere.** Rejected: it costs real weeks, it is
not what makes the platform sovereign, and it was never a decision — it
was a default that hardened.

**Let recipes choose freely.** Rejected: that is the drift this ADR
exists to prevent, and this project has already demonstrated it cannot
see drift it does not count.

**A written guideline with no mechanism.** Rejected on the same
evidence. The informal three-tier policy already existed, as prose, and
did not prevent a full investigation being spent on each of three
packages that plainly met its own criteria for an exception.
