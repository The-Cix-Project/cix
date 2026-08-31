# 0221 — Composed build environments are reclaimed by last use, not by reachability

## Status

Accepted

## Context

Composed build environments (ADR-0199) are cached as images named
`__buildenv-<hash of the declared tool set>`. That naming is right:
recipes sharing a tool set share one environment, and it is built once.

Nothing ever removed them (issue #112). Each revision of a declared set
mints a new environment and abandons the old one, and because a recipe
version is immutable, *correcting* a build tool list is the normal way
to work — so abandoned environments are the common case rather than the
exception. Each is a full rootfs. The growth is unbounded.

The obvious question is "when is an environment dead?", and the obvious
answer is reachability: no recipe version currently in the catalogue
names this tool set. Issue #112 raised that itself and then correctly
noted it is not quite right — a package installed from an *older* recipe
version can still be rebuilt (`pkg resume`), so "unreferenced by the
current catalogue" is not the same as "unreachable".

Computing real reachability means walking every version of every recipe,
which is expensive, and it is still only an approximation of what some
future operator might ask for.

## Decision

**Reclaim by last use. Do not compute reachability.**

The reframing that settles it: look at what being wrong actually costs.

Composition is cheap and deterministic. Delete an environment that turns
out to be wanted, and the next build **recomposes it**. There is no
failure mode — only a slower build. This is not a correctness question
at all; it is a cost question with automatic recovery.

So the rule is:

- every build **stamps** the environment it used
- an environment an in-flight build is using is **never** touched — this
  is the one hard rule, and it is about a live process rather than about
  prediction
- anything else unused beyond a retention window is reclaimed

An age-based rule also resolves the case reachability cannot see at all:
an environment that no recipe names *and* that nothing will ever want
again is indistinguishable from one that is merely idle. Under
last-use both resolve correctly without ever having to tell them apart.

**Not folded into the source/artifact cache pruning.** Issue #112 named
the distinction and it is worth keeping visible: prune the cache and a
build gets *slower*; prune an environment and a build gets
*recomposed*. Same-looking outcome, different meaning — and one number
covering both would hide which had happened. Build environments get
their own listing and their own delete.

## Consequences

- `GET /v1/pkg/buildenv` lists each environment with its last use and
  whether a build currently holds it; `DELETE /v1/pkg/buildenv/{name}`
  removes one and refuses (409) while it is in use.
- The stamp is a marker file's mtime inside the environment's own image
  directory — derived from the filesystem rather than stored in a second
  registry that could disagree with it, the same posture the rest of
  this daemon takes about state it can observe directly.
- A missing stamp (an environment composed by an older daemon) is
  treated as "used now" rather than "never used". The alternative would
  make the first daemon carrying this change delete every pre-existing
  environment at once, which is exactly the kind of surprise an
  upgrade should not spring — and the cost of being conservative here
  is one retention window's delay.
- Reclamation being wrong is survivable by construction. That is the
  property the whole decision rests on, and it is why no attempt is
  made to be precise.

## Alternatives considered

**Reachability analysis over every recipe version.** Rejected: expensive,
still approximate, and it buys precision that has no value when the
penalty for imprecision is a recomposition.

**Fold into `pkg cache-config`/`cache-status`.** Rejected: it would put
two different meanings behind one number, and an operator pruning "the
cache" would not know they had also discarded build environments.

**Do nothing until it is a real disk problem.** Rejected, though it was
tempting — 192.168.15.95 had four environments when #112 was filed and
one when it was fixed, so this has never actually hurt. But the growth
is unbounded and the fix is small; waiting for it to hurt means fixing
it on a box that is already full.
