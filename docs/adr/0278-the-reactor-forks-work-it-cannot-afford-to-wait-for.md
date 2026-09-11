# 0278 — The reactor forks work it cannot afford to wait for

## Status

Accepted

Issues [#367](https://git.home.arpa/itdlabs/cix/issues/367) and
[#368](https://git.home.arpa/itdlabs/cix/issues/368). Applies
[ADR-0247](0247-the-reactor-does-not-block-and-that-is-the-defence.md)'s rule to work that is neither a request nor
a subprocess. Does not change it.

## Context

ADR-0247 says the reactor does not block, and `test_blocking_waits` holds the line for
waits on children. Neither covers work the daemon simply *does* — an `nftw()` walk, a
tarball extraction — synchronously, inside one pass of the loop.

That gap is not theoretical. `pkg.sync` extracts the whole recipe repository on the event
loop every six hours. Measured on 192.168.15.95 across four days, taking the first
appearance of each new high-water mark:

```
09-07 03:23:15   6116 ms
09-08 21:23:14   5842 ms
09-09 15:23:13   5062 ms
09-09 21:23:15   5421 ms
09-10 03:23:14   5235 ms
```

`03:23`, `09:23`, `15:23`, `21:23` — a six-hour period at the same minute, aligned with the
`recipe-sync` schedule, and every record carrying an empty `activity`, which is what
identifies it as scheduled work rather than a request handler. Seven times the 750 ms the
daemon itself calls slow.

Five seconds is not a blip here. `cixd` is pid 1 on a host with no shell; for that window
nothing is served — no API, no dashboard, no console, no container lifecycle event. It is
also perfectly regular and entirely self-inflicted, unlike a load-induced stall.

`GET /v1/volumes/{name}/usage` is the same shape and has not bitten yet only because the
one volume on that host holds 27 KB: the walk is O(files) and unbounded.

## Decision

**Work that can take unbounded time runs in a forked helper, and the reactor learns it
finished from a pidfd like every other child.**

One primitive — `helper_run(work, arg, done, ctx, what)` — forks, runs `work` in the child,
and calls `done` on the loop when it exits. `CONN_HELPER` and `handle_helper_event()` are
its two halves in the reactor.

**A process, not a thread.** Recorded so it is not re-litigated: threading `cixd` would
have to contend with 215 mutable global tables, 21 places that document a dependence on
being single-threaded, and ~74 fork/clone sites whose children do non-async-signal-safe
work before `execve`. `exec.c` states the dependency outright — "setenv() after fork() is
safe here specifically because cixd is single-threaded". There is no small version of that
change, in a process where a crash is a kernel panic.

**Extracted rather than hand-rolled.** The daemon already had this shape in three separate
places: `procfuse_start()`, the build containers, and `diskformat.c`, each with its own
fork, conn kind and completion handler. A fourth copy by hand is how a fifth happens.

### What may move into a helper

**Filesystem-effecting work only.** A fork sees a copy of every global and can change none
the parent will read. Work moved in must have its whole effect on disk; any in-memory
consequence belongs in `done`, which runs in the parent.

Getting that split wrong fails silently — the child updates its copy, exits 0, and the
parent carries on with the old value. So the split is stated per call site rather than left
to be inferred. For `pkg.sync`: the extraction moves, the recipe merge does not, because it
calls `pkg_recipe_add()` and reads `g_sync_refetch_*` — and it is the cheap half anyway,
a few hundred small files.

### A failed fork is not lost work

`helper_run()` returns -1 without calling `done`, and the caller runs the work inline
exactly as before. Worse for latency, correct for the result — the right way round. A
caller that cannot say that about itself has no business using this.

## Alternatives considered

**Make request handling concurrent.** Would not have helped: every one of these stalls has
an empty `activity`, meaning no request was in flight. The work is scheduled, not served.

**Make the extraction faster.** Treats one symptom. The walk in `/volumes/{name}/usage` is
unbounded by nature and cannot be made fast enough to be safe on the loop.

**Leave it and raise the stall threshold.** Would silence the measurement that found this.

## Consequences

- A six-hourly five-second outage on every Cix host stops.
- The loop's worst pass stops being dominated by scheduled work, so the figure starts
  meaning what it says.
- `helper_run()` is the one place to add the next offload, and the volume usage walk (#368)
  is its next intended user.
- Anything moved into a helper must be audited for the memory/disk split. That is a real
  ongoing cost, paid per call site, and it is why the rule is written here rather than in a
  comment at one of them.
