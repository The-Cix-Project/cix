# ADR-0228: Build-environment composition runs in a forked child, and the install resumes

- **Status:** Accepted
- **Date:** 2026-09-01
- **Issue:** [#238](https://git.home.arpa/itdlabs/cix/issues/238)
- **Related:** [ADR-0199](0199-composed-build-environments.md) (what composition is), [ADR-0227](0227-responses-are-buffered-and-drained-never-blocking.md) (the same principle for responses)

## Context

Composing a build environment (ADR-0199) walks and copies every file of
every declared build tool. For `toolchain` that is gcc, glibc and
binutils -- thousands of files. It ran inline in `pkg_fetch_completed()`,
which is to say inside the daemon's single event loop.

Measured on 192.168.15.95 running v2.14.4: publishing eleven recipe
revisions back to back, ten took 8-57 ms and one took **247,591 ms**.
The audit log brackets it exactly -- nine publishes at 23:27:45-23:28:02,
then four minutes of silence, then the next at 23:32:12.

The part worth recording is what the instrumentation said, which was
nothing:

**The stall store has no record for that window.** Not a stall, not a
`stall-continues`. The loop kept heartbeating the whole time, because it
kept going round -- doing several seconds of composition per pass. It was
alive by the only measure it has, and unusable by every other. That is
issue #229 with a real case attached rather than a hypothesis, and it is
why this was originally mis-attributed: the first write-up of #238
claimed nothing else was served during those four minutes, which was
inference, not measurement, and was corrected on the issue.

This is the same defect class as ADR-0227 -- blocking work inside a
single-threaded event loop -- reached from the other direction. ADR-0227
stopped a *client* holding the loop. This stops the daemon holding it
against itself.

## Decision

**Composition runs in a forked child, and the install resumes when the
child exits.**

`buildenv_image_for()` now returns 0 (ready), 1 (forked -- `out_pid` and
`out_pidfd` name the child), or -1 (failed). A composing return means the
chain keeps its slot and the entry keeps its state; `CONN_PKG_BUILDENV`
watches the pidfd exactly as `CONN_PKG_FETCH` already watches the fetch
child, and `pkg_buildenv_completed()` continues from there.

**The resume carries no state.** It re-derives the recipe and the upgrade
flag from the chain, calls the same `pkg_prepare_build_and_start()`, and
reaches `buildenv_image_for()`'s existing "already composed" fast path,
which costs nothing. A resumed call is indistinguishable from a first
call that happened to find the environment ready.

This works because **image state lives on disk, not in memory** --
`image_current_version()` re-reads `manifest.json` on every call, with no
cache. So everything the child produces is visible to the parent simply
by asking again. That property is load-bearing for this design; a future
in-memory image cache would break it silently, and should be read as
requiring a real handover instead.

## Details that cost something to get right

**The child closes every inherited descriptor.** Unlike the fetch child
it never `execve()`s, so `SOCK_CLOEXEC` does nothing for it, and it would
otherwise hold live client sockets open for the whole of composition --
reintroducing ADR-0227's exact symptom through the change meant to
prevent it.

**A failed composition deletes its image.** `image_produce_new_version()`
creates the image before it can fail, and the "already composed" check
deliberately skips an empty one -- so a leftover would be re-forked on
every subsequent attempt, forever. Removing it is what makes failure
terminal rather than a loop.

## Alternatives considered

**Make composition cheaper instead.** Already done as far as it goes:
#236 replaced a 4 KB read/write loop with `FICLONE` reflinks. It reduced
the constant and left the shape -- thousands of syscalls inline -- and
the 247 s measurement above is *after* that improvement.

**Threads.** Rejected for the same reason as in ADR-0227: it makes every
piece of daemon state shared mutable state to solve "do not block the
loop", which forking already solves with no shared state at all.

**Compose eagerly, ahead of need.** Does not help. Whoever triggers the
eager composition pays exactly the same cost in exactly the same loop.

## Consequences

- Publishing a recipe, or any other request, is no longer delayed by a
  rebuild starting.
- The install path gained a resumable step. `pkg_fetch_completed()`'s
  contract changed (a third return value, two new out-params), which is
  the hard-to-reverse part and why this is an ADR.
- Composition failures are now reported through the same path as any
  other build failure, with the image cleaned up.
- **#229 remains open and is now better specified**: the measurement this
  needs is per-request latency (accept to response), not loop liveness.
  A heartbeat cannot see a loop that is busy rather than stuck.
