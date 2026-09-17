# 0293 — An unbounded measurement is served from a cache, never walked inside a request

## Status

Accepted. Applies [ADR-0247](0247-the-reactor-does-not-block.md)'s rule to work that is neither a request nor a subprocess, using [ADR-0278](0278-a-helper-process-for-filesystem-work.md)'s helper, and records why the obvious alternative is not available. Fixes [#474](https://git.home.arpa/itdlabs/cix/issues/474); contains [#402](https://git.home.arpa/itdlabs/cix/issues/402). The field this ADR names as `disk.upper_bytes` is `disk.usage.bytes` since [ADR-0301](0301-a-containers-disk-usage-is-named-for-the-question-it-answers.md), which renamed it once ADR-0207's subvolume substrate meant there was no upperdir to name; the decision recorded here is unchanged, and the old spelling is kept below as the record of what it was at the time.

## Context

`overlay_upperdir_size()` is an `nftw()` walk: O(files) and unbounded. Three request paths called it inline, and reading them in order is the whole argument:

- **`GET /containers/{name}/stats`** walked the container's entire writable tree **on every request, with no shortcut** — and the dashboard polls it. This is the one that matters.
- **`GET /volumes/{name}/usage`** walks only as a *fallback*, when the btrfs qgroup query fails. On the platform's own substrate it does not run at all, and [#161](https://git.home.arpa/itdlabs/cix/issues/161) is retiring the substrate where it would.
- **`image gc --measure`** walks every unreferenced image version in one request — operator-initiated and deliberately expensive, rather than polled.

`cixd` is pid 1 on an installed host with no shell. Its one event loop is the only way in. For as long as a pass is inside a synchronous walk, nothing is served — the failure ADR-0247 exists to prevent, and the one #399 measured at 366 seconds.

**The obvious fix is not available, and finding that out is most of this decision.** #402 proposed running the walk through `helper_run()` and said the mechanism "is already proven". It is — for *background* work. `helper_run()`'s existing user is `pkg.sync`'s extract, which has no client waiting. A request-path walk needs the response written when the helper finishes, and this daemon has no way to defer one:

```c
dispatch(cc->fd, &req);
...
client_conn_finish(cc);   /* immediately after, every time */
```

`client_conn_finish()` tears the connection down when nothing was buffered, or drains what was and closes. A handler that returns without writing loses its connection. There is no pending-response state anywhere — the WebSocket upgrades return early and *repurpose* the conn, which is a different mechanism.

Adding one is a new connection state in the core of a process where a crash is a kernel panic. That may still be worth doing one day; it is not the price of fixing a stats field.

## Decision

**Decouple the measurement from the request. A request serves the last figure and starts a new measurement when the one it served has aged out.**

The walk runs in a `helper_run()` child, which is exactly what ADR-0278 is for: filesystem-effecting work, with the in-memory consequence in the completion callback. The one number comes back through a pipe, because a fork sees a copy of every global and can change none the parent will read.

**`upper_bytes` is `null` until the first measurement lands, and `upper_measured_at` says when the figure is from.** Reporting `0` for "not measured yet" was the alternative, and it is a lie a caller cannot detect — it reads as an empty container. The nullable field is a contract change, taken deliberately.

Staleness is not new to this API: `GET /volumes/{name}/usage` on btrfs reads a qgroup that settles at transaction commit, roughly 30 seconds behind, and that endpoint already reports which source answered. What changes here is that the age is *stated* rather than assumed.

**The freshness window is proportional to what the walk cost, not a constant.** A fixed window has to be wrong in one direction or the other: long enough to protect a huge tree means a small container's size visibly lags its own growth, and short enough to track growth means a huge tree is walked over and over. So each measurement is timed and the next is allowed no sooner than ten times that, with a one-second floor. A trivial tree refreshes at the floor; an expensive one backs itself off without anyone choosing a number for it. "Polling harder must not mean walking harder" becomes a bound on **cost** rather than on frequency. One measurement per container is in flight at a time.

That is not what was written first. The first version used a flat 60 seconds, and `test_container_stats` failed it — the test appends to a file and requires the reported size to *advance*, which a minute-long window makes impossible. The failure is exactly what a user watching a container fill up would have seen, and it was worth more than the reasoning that produced the constant.

**`GET /volumes/{name}/usage` and `image gc --measure` are contained rather than converted.** The first is unreachable on the platform's own substrate; the second is an operator asking for an expensive thing once. Both are now counted by `test_blocking_waits`, alongside the blocking waits it already counts, so a fourth caller is a deliberate act that shows up in review rather than an accident.

## What the walk actually costs here, measured

Stated because the byte figures in this ADR invite the wrong conclusion. On 192.168.15.95 (2026-09-15), a stats request that triggers a measurement sees it land in **6 ms for `jump` and 3 ms for `syslog-1`** — despite `jump`'s tree holding 1.25 GB. `nftw()` walks inodes, not bytes, and these images have few files with a warm page cache.

So on this host this was a **latent** risk rather than an active stall: the inline walk cost single-digit milliseconds per request, not seconds. What makes it worth removing anyway is that the cost is decided by the data and not by the code — a container holding a real dataset, a build tree, a mail spool, is millions of inodes and seconds of walking, on the one event loop of a pid-1 daemon with no shell behind it. The fix removes a class of failure, and it should not be read as having removed one that was firing.

The same measurement explains something else honestly: the millisecond timing described above changes **nothing observable on this host**, because a 6 ms walk lands under the floor either way. It matters for the middle of the range — a walk of a few hundred milliseconds, which `time()` rounded to a cost of zero and which therefore re-ran continuously. That case is real and was reachable; it simply is not what this box does today.

## Consequences

- The stats endpoint no longer walks anything. Its cost is now bounded by the cache lookup, whatever the container holds.
- A caller sees `null` on the first request for a container the daemon has not measured yet — normally one poll. Clients must handle it; the dashboard shows "measuring…" rather than a fabricated number.
- **The figure is eventually consistent, and a test that needed it to be immediate had to change.** `test_container_stats` now polls for the first measurement and for growth, with a bounded timeout, rather than demanding both from the next response. That is a weaker assertion in exactly the way the API is weaker — and writing the test to pass by measuring inline would have been the bug back again.
- **The daemon still cannot defer an HTTP response**, and two request paths still walk. That is stated rather than fixed, and it is the honest position: the walk that was actually being hit is gone, and the remaining two are counted so they cannot quietly become three.
- A deleted container leaves a cache entry behind, reclaimed oldest-first when the table fills. Evicting on delete would mean another cleanup path to keep correct, for 80 bytes.
- The general rule this sets, and the reason it is an ADR rather than a comment: **a measurement whose cost is decided by the data, not by the code, does not belong inside a request on this daemon.** The next one gets a cache and a timestamp too.
