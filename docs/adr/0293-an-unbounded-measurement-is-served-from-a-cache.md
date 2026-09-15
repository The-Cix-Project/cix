# 0293 — An unbounded measurement is served from a cache, never walked inside a request

## Status

Accepted. Applies [ADR-0247](0247-the-reactor-does-not-block.md)'s rule to work that is neither a request nor a subprocess, using [ADR-0278](0278-a-helper-process-for-filesystem-work.md)'s helper, and records why the obvious alternative is not available. Fixes [#474](https://git.home.arpa/itdlabs/cix/issues/474); contains [#402](https://git.home.arpa/itdlabs/cix/issues/402).

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

The freshness window (60 s) is deliberately longer than a dashboard poll: **polling harder must not mean walking harder.** One measurement per container is in flight at a time.

**`GET /volumes/{name}/usage` and `image gc --measure` are contained rather than converted.** The first is unreachable on the platform's own substrate; the second is an operator asking for an expensive thing once. Both are now counted by `test_blocking_waits`, alongside the blocking waits it already counts, so a fourth caller is a deliberate act that shows up in review rather than an accident.

## Consequences

- The stats endpoint no longer walks anything. Its cost is now bounded by the cache lookup, whatever the container holds.
- A caller sees `null` on the first request for a container the daemon has not measured yet — normally one poll. Clients must handle it; the dashboard shows "measuring…" rather than a fabricated number.
- **The daemon still cannot defer an HTTP response**, and two request paths still walk. That is stated rather than fixed, and it is the honest position: the walk that was actually being hit is gone, and the remaining two are counted so they cannot quietly become three.
- A deleted container leaves a cache entry behind, reclaimed oldest-first when the table fills. Evicting on delete would mean another cleanup path to keep correct, for 80 bytes.
- The general rule this sets, and the reason it is an ADR rather than a comment: **a measurement whose cost is decided by the data, not by the code, does not belong inside a request on this daemon.** The next one gets a cache and a timestamp too.
