# 0235 — Build capacity is derived from package state, not from a field someone has to clear

## Status

Accepted

Issue [#246](https://git.home.arpa/itdlabs/cix/issues/246). Decided 2026-09-02.

## Context

The daemon runs up to `max_concurrent_jobs` package jobs at once. A job owns
one entry in `g_chains[]`, and the whole notion of "is there room for another
job" is derived from one field:

```c
static int chain_alloc(void)
{
	for (i = 0; i < max_jobs; i++)
		if (g_chains[i].name[0] == '\0')
			return i;
	return -1;
}
```

A slot is free when its `name` is the empty string. Nothing else is consulted.

That makes releasing a slot an action someone has to remember to perform, and
the number of places that must remember is not small: better than twenty
separate `g_chains[chain_idx].name[0] = '\0';` assignments, spread across the
fetch path, the build-environment composition path, the build path and the
resume path, several of them nested three branches deep inside an exit-status
decode. Each is a place the slot can be lost. There is no counterpart to
`chain_alloc()` — no `chain_free()` — so the compiler cannot help, and a path
that returns without clearing is indistinguishable from one that intends to
hold its slot. Two paths legitimately do intend that: a forked
build-environment composition (`return 2`), and a chain advancing to its next
dependency.

A missed clear is permanent. The slot is never revisited, so it is held for
the lifetime of the process, and the capacity lost is never recovered. This
was measured rather than theorised: repeated failed builds of one package took
a host from zero to nine of ten slots held with no job running anywhere —
`active_jobs: 9`, an empty rebuild queue, no fetching or building rows and no
build containers. One more would have refused every package operation on the
box with `409` until a reboot, on a host with no shell to recover through.

The failure is also silent in the worst way. Capacity degrades one slot at a
time, and the box keeps working until it abruptly cannot build anything at
all, with nothing in any log connecting the refusal to the builds that caused
it.

## Decision

**A chain slot is busy if and only if the package that owns it is in a
transient state. Capacity is computed from that, not from whether a string was
cleared.**

`chain_reap_stale()` runs at the two points where the answer is consumed —
`chain_alloc()`, which hands out capacity, and `pkg_active_chain_names()`,
which reports it — and releases any slot whose owner is not currently running.

The predicate is the package's own state, of which there are exactly four.
`PKG_STATE_FETCHING` is set when a fetch starts and held across a forked
build-environment composition; `PKG_STATE_BUILDING` is set when the build
container starts. Those two are a running job. `PKG_STATE_INSTALLED` and
`PKG_STATE_FAILED` are terminal: `pkg_fail()` sets one or the other on every
failure, including an upgrade failure, which deliberately leaves the old
version `INSTALLED`. An entry that has disappeared entirely is stale for the
same reason.

Reclaiming is logged at `warn`, naming the slot and its holder. A slot that
needed reclaiming means a clear site was missed, and a silent self-heal would
conceal the very defect it is compensating for.

## Why this predicate, and not a new one

This is not a new invariant introduced to paper over the old one. It is the
invariant the file already relies on. Both `pkg_fetch_completed()` and
`pkg_buildenv_completed()` discard a late child-exit event with exactly this
test — issue #98's stale-slot discriminator — on the stated grounds that *an
entry that is no longer FETCHING cannot be the one whose child just exited*.
The same reasoning gives the same answer about the slot itself. Using it in
both places makes one fact have one source rather than two representations
that can disagree, which is the condition the leak lived in.

## Alternatives considered

**Audit the clear sites and fix the one that leaks.** Rejected as the whole
fix, though it remains worth doing: it restores the ten slots without removing
the property that made them losable, and the next path added to the exit-status
decode has the same exposure. The warn-level log preserves the ability to find
the specific offender, so this is deferred rather than abandoned.

**A `chain_free()` counterpart to `chain_alloc()`.** Better than scattered
assignments, and still an action someone must remember at every return. It
narrows the class without closing it.

**Reference counting or an owning pid per slot.** Genuinely closes it, but
introduces a second thing to keep consistent with package state — precisely
the duplicate-state problem being removed here. The build path has no single
pid to own a slot across fetch, compose and build.

## Consequences

Capacity becomes self-correcting: a missed clear costs a log line rather than
a slot, and a host cannot be walked into a state where it refuses all package
work until rebooted.

The load-bearing requirement is that every legitimately-running job keeps its
entry in a transient state for as long as it holds a slot. That holds today at
all three transitions and is asserted by the reap log — a reclaim that happens
while a job really is running would announce itself rather than corrupt
quietly. Any future path that wants to hold a slot without a running package
would have to say so explicitly, which is the right thing to have to justify.

The reap is O(slots) on a ten-element array at two call sites and costs
nothing measurable.
