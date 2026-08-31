# 0222 — A declared capability loss beats a foreign toolchain

## Status

Accepted

## Context

Converting the last of #207's recipes to build from source hit a real
compiler bug rather than a packaging gap. `iproute2`'s `ip/ipvrf.c`
builds a BPF program from an array of designated compound literals, and
this platform's TCC miscompiles that construct — it uses a struct
field's byte offset as an array index and stops with `index too large`
(#211, `tccgen.c:6389`). A twelve-line reduction reproduces it on a Cix
host.

Everything else about that build had been made correct: it configures
with ELF, netlink and capability support all on, and compiles every
object but one.

That left three ways forward, and they are not interchangeable:

1. **Fix TCC.** Correct, and it benefits everything else using the
   idiom — the kernel's own BPF headers use it everywhere. But it is
   real compiler work, and it blocks a package that is otherwise ready.
2. **Build without the affected command**, and say so.
3. **Add `iproute2` to the Tier-3 TCC exception list**, building it with
   GCC.

Option 3 is the one that looks cheapest and is not. The exception list
exists for genuinely infeasible cases (`kernel`, `openssl`, `gcc`
itself). Reaching for it because a compiler bug is inconvenient would
turn a short list of understood exceptions into the ordinary way to get
past a TCC gap, and each addition is quiet: the package still builds,
still installs, still works. Nothing about the artifact says it was
built by a different compiler than the platform claims to use.

The unstated assumption behind preferring option 3 is that shipping a
*complete* package matters more than shipping a *self-hosted* one. For
this project that is backwards. The self-hosting property is the thing
being built; a missing subcommand is a gap in a tool.

## Decision

**A capability loss is acceptable when it is declared. It is not
acceptable when it is silent.**

Concretely, a recipe may ship less than upstream provides, if and only
if:

- **The loss is named in the recipe**, in the header comment and in
  `pkg_changelog`, in terms of what an operator can no longer do —
  not in terms of which object failed to compile.
- **The reason is a real, identified defect**, linked to its issue.
  "It did not build" is not a reason; "TCC miscompiles this construct,
  #211, here is the reduction" is.
- **What is *kept* is stated too.** `iproute2` loses `ip vrf`
  (exec/identify/pids) and keeps `ip link ... type vrf`, because those
  live in different files. Saying only what is lost invites the reader
  to assume the whole feature area is gone.
- **The removal is asserted in both directions.** The recipe fails if
  the removed symbol is still present, *and* fails if the retained one
  is missing. A capability loss that is not verified is a hope, and a
  half-removed command that still appears in `--help` is worse than
  either clean outcome.
- **It is reversible, and marked as such.** The removal is a
  consequence of a compiler bug, not a judgement about what the package
  ought to contain, so it is reverted when the bug is fixed.

This is deliberately narrower than "ship whatever builds". A recipe that
quietly loses a feature because a `configure` probe failed is exactly
what #113 is about, and this ADR does not license it — the difference is
between a loss someone chose, wrote down and verified, and a loss nobody
noticed.

## Consequences

The Tier-3 exception list stays short and keeps meaning what it says.
The alternative — treating it as the escape hatch for compiler bugs —
would have made it grow whenever TCC was inconvenient, and the whole
point of the 3-tier policy is that the exceptions are few and argued.

A declared loss also leaves a trail that a foreign-toolchain build does
not. `keepalived` is the worked example: 2.3.4 shipped with iptables,
nftables and ipset support by copying eight of Debian's libraries into
the package, which looked complete and was not self-hosted. Rebuilding
it from source lost all three, and revisions -3 through -5 restored two
of them as their dependencies became real Cix packages. That sequence is
only possible because each loss was recorded; a build that had silently
kept the features by using another distribution's libraries would have
had nothing to pay back and no record that anything was owed.

The cost is real: an operator can hit a missing subcommand. That is
mitigated by the loss being in the changelog rather than discovered at
the command line, and it is bounded by the requirement that every such
loss carry an open issue that would remove it.

Supersedes nothing. Applies alongside the Build Provenance Mandate,
which is the constraint that makes this trade necessary in the first
place: if foreign binaries were acceptable, none of these choices would
arise.
