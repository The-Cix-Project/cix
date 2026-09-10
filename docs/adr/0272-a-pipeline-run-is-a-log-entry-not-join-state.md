# 0272 — A pipeline run is a log entry, not join state

## Status

Accepted

Issue [#371](https://git.home.arpa/itdlabs/cix/issues/371) (Deploy 3 of four).
Extends [ADR-0256](0256-a-package-has-a-position-in-a-pipeline.md) and
[ADR-0269](0269-one-pipeline-model-for-four-kinds.md) along the time axis; stores its
records the way [ADR-0070](0070-consolidated-log-store.md) stores lines.

## Context

ADR-0256 gave a package a position — a stage and a status — and ADR-0269 generalised
that to four kinds. Both describe *now*. `GET /v1/pipeline` computes every position at
read time and stores nothing, which is what keeps the model honest: there is no second
copy of the truth to drift from the first.

What neither answers is *then*. An operator looking at `glibc` in `base` can see it is
installed; they cannot see that it failed twice this morning, how long the build that
succeeded took, or whether anyone asked for it at all. The only record of a past build is its log
file, and those are capped:

```c
#define PKG_BUILD_LOG_KEEP 40
```

`daemon/src/pkg.c`. Measured on 192.168.15.95 on 2026-09-10: 41 log files spanning two
days, 1.4 MB, mean 34 KB each. Thirty-seven of the forty-one were `cix` — this
session's own deploy iteration — so the two-day span is not a quiet box, it is a busy
one evicting its own history. **A build log on this platform lives under two days.**

That measurement is what decides the shape of this ADR. The instinct is to keep more
logs. The right answer is the opposite: a log is 34 KB of build output, and what an
operator actually needs from last week is forty bytes of it — that it ran, when, for
how long, and how it ended. Those are two different things with two different
lifetimes, and conflating them is why the cheap one is currently being thrown away to
make room for the expensive one.

## Decision

**A run is an event, appended to a log with retention. It is never read to compute a
position.**

`GET /v1/pipeline` continues to derive every position from live state at read time and
to store nothing. A run record is written once, when the daemon stops working on an
atom, and is never updated afterwards. Nothing joins the two: the pipeline view answers
"where is this now", the run store answers "what has happened to it", and if they ever
disagree that disagreement is a finding rather than something to reconcile.

This is not a reversal of ADR-0256. That ADR forbids storing a *position*, because a
stored position is a second source of truth about the present that drifts from the
first. A run is not a position; it is a record that something happened, in the same
class as the audit trail — which ADR-0271 extends without anyone calling it a second
source of truth about who the user is.

### The atom is the same atom

ADR-0269 established that the unit is `(package, image)`, and a run keeps it. A chain
that installs `openssh` and pulls `openssl` and `zlib` with it produces **three** run
records, not one, because three atoms moved. They carry the same `chain` value so the
view can say "part of the same install", and each records its own outcome — the chain
that fails on its second dependency has one success and one failure to show, which a
single per-chain record could not express.

### A run is closed once, at the real outcome

The daemon has exactly one funnel for a failure — `pkg_record_outcome()`, reached by
both `pkg_fail()` and `pkg_fail_cancelled()` — and one assignment for a success. The
success assignment is deliberately provisional; its own comment says both branches
below it may revert it to failed. A run is therefore closed at the point the outcome is
final, never at the provisional assignment, so that a package which is marked installed
and then fails its image-version step records the failure it actually had.

An in-flight run is not in the store at all. While a build is running it is live state,
already reported by `GET /v1/pkg/hostbuild/{name}` and by the pipeline view; writing a
half-record and amending it later would create exactly the mutable second copy this
ADR exists to avoid. The store therefore contains no run whose outcome is unknown.

### The log reference is allowed to dangle

A run record carries `log` — the build-log filename, when the run had one. Build logs
are pruned at 40 and runs are kept far longer, so most historical runs will point at a
file that no longer exists. That is correct and is surfaced as such: the view says the
log has been pruned rather than hiding the run. Tying run retention to log retention
would throw away the cheap, useful record to match the expensive one.

### Retention is a number an operator can see

Runs are kept as a count, not a duration, and the count lives in a new
`pipeline-config` resource rather than a `#define`. The default is 1000. At the
measured shape of this platform — 13 packages tracked rolling across 15 images, a
maximum fan-out of 6 images for `glibc` and a median of 1, against 84 pinned manifest
entries — a publish causes at most a handful of rebuilds, and 1000 runs is a season of
history for roughly 400 KB. The resource exists now because Deploy 4's approval gates
need a home of their own, and one config resource with one field is better than a
second one arriving later.

## Consequences

An operator can ask what has happened to a package in an image and get an answer that
outlives the build log by a factor of about thirty. The drawer in the dashboard gains a
history under the current position, so "it is installed" and "it failed four times
first" are visible together.

Two things get harder, deliberately. A run cannot be corrected after the fact, because
it is an event and events do not change — a wrong record is evidence of a bug, not
something to patch. And the run store cannot be used to make the pipeline view faster,
because the view is forbidden from reading it; if the read-time join ever becomes too
slow, the fix is a faster join, not a cache with a second truth in it.

## Alternatives considered

**Keep more build logs.** Raising `PKG_BUILD_LOG_KEEP` to 1000 would give the same
history for 34 MB instead of 400 KB, and would still lose everything on the day someone
builds 1001 times. It answers the question by paying eighty times as much for it.

**One record per chain.** Simpler to write, and unable to express a chain whose
dependencies had different outcomes — which is the case an operator most needs to see.

**Record the run when it starts, and update it when it ends.** This is the shape every
CI system uses, and it is wrong here for the reason ADR-0256 gives: an in-flight run
in the store is a mutable second copy of live state the daemon already reports
accurately. Appending only closed runs means the store is append-only in fact and not
merely by convention.

**Derive history from the audit trail.** ADR-0271 already records who asked for what,
and a build's start really is in there. But the audit trail records *requests*, and a
rolling rebuild is not a request — nobody asked for it, a publish caused it. The runs
that most need explaining are exactly the ones the audit trail cannot see.

The cause is carried on the **chain**, not on an ambient value consumed when a
run opens. That distinction is not cosmetic: `pkg_install_start()` has four early
returns before any run exists, so a consumed-at-open value survived a refused
rolling rebuild and mislabelled the next operator-requested install; and a chain
resolves its dependencies first, so only the first atom would have consumed it
and every dependency a rolling rebuild pulled would have recorded itself as
requested. A chain has one cause, and that is where it lives. Issue
[#375](https://git.home.arpa/itdlabs/cix/issues/375) records the remaining gap:
an open run is in memory only, so a daemon restarted mid-build records nothing
for the run it was in the middle of.

This is also why a run records a `trigger` and not an actor. "Requested" versus
"rolling" is the distinction that changes what an operator does next, and it is knowable
at the one place a job begins. *Which person* requested it is already recorded, by name,
in the audit trail alongside the request that started it — putting it on the run as well
would be a second copy of an answer that already exists, for the runs where it exists at
all.
