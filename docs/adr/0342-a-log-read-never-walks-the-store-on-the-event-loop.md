# 0342 — A log read never walks the store on the event loop

## Status

Accepted by the owner, 2026-10-19 ("Yes!"), as proposed the day before. Issue [#612](https://git.home.arpa/itdlabs/cix/issues/612); phase 2 is [#616](https://git.home.arpa/itdlabs/cix/issues/616). It changes how `GET /v1/system/logs` is served, adds a documented `503` to it, and refuses one kind of event-stream subscription, so it is the owner's decision. It applies [ADR-0247](0247-the-reactor-does-not-block-and-that-is-the-defence.md) (the loop's blocking budget) and [ADR-0278](0278-the-reactor-forks-work-it-cannot-afford-to-wait-for.md) (`helper_run()`), generalizes [ADR-0340](0340-a-credential-is-checked-off-the-loop-and-the-request-waits-for-it.md)'s parked request rather than adding a second one, and leaves [ADR-0070](0070-consolidated-log-store.md)'s store format and [ADR-0339](0339-the-dashboard-is-told-what-changed.md)'s sampling model unchanged.

## Context

cixd is one event loop, and the log store is read inside it. `logstore_tail_ex()` (`daemon/src/logstore.c`) reads the store's eight segments newest first and stops once it holds `tail` matches. But it reads each segment in full from its start, and `json_parse()`s every line before any filter is applied.

**Measured on 192.168.15.95, 0.2.57-540, 2026-10-18:**

| request | time | stall record |
|---|---|---|
| `GET /v1/system/logs?tail=1` | 76 ms | none |
| `GET /v1/system/logs?regex=zzznomatchzzz&tail=1` | 1533 ms | 1533 ms |

- **A filtered read that finds fewer than `tail` matches reads every segment.** `source`, `container`, `level`, `regex` and `since` are all filters. Every slow read in #612's table carried one, at 1234 to 1800 ms, and for that long nothing else on the box is served. "What happened in the last minute, `tail=40`" is a whole-store scan whenever that minute held fewer than 40 lines.
- **The cost grows with the store, and the store's size is the operator's.** `max_bytes` is 100 MB on .95 (`GET /v1/system/logs/config`) and may be set to 100 GB (`LOGSTORE_MAX_MAX_BYTES`). So this is not a slow endpoint to tune. It is loop work with no bound.
- **An unfiltered tail is bounded only by the newest segment's size.** It still parses that whole segment, which is up to `max_bytes / 8`: 12.5 MB today, 12.5 GB at the cap.
- **The dashboard's own log panel is read inside the loop, every 2 s.** It subscribes to `GET /v1/system/logs?since=0&tail=300` (`x-cix-read: stream`, `x-cix-sample-period: 2`), and `events_subs_tick()` runs that GET in-process with `events_run_get()`, once per tick for however many tabs asked. An in-process read has no connection, so it cannot be parked or moved to a child.
- **Who issues the filtered reads:** `cixctl logs --source/--container/--regex/--since`, tests, and operators. With hundreds of containers, "this container's last 40 lines" is the most common question an operator asks, and for a quiet container it reads everything.

Which of reading, parsing and matching dominates the 1533 ms is not measured, and nothing below depends on it.

## Decision

### The rule is bounded versus unbounded, decided by the query's shape

1. **An unfiltered tail is served on the loop, and costs O(`tail`).** `logstore_tail_ex()` reads each segment backwards from its end in blocks, newest line first. It stops as soon as it holds `tail` entries, and goes on into the previous segment only when the newest one runs out. The result is the same entries, in the same chronological order, as today's reader returns. "Unfiltered" means no `source`, `container`, `level` or `regex`, and `since` absent or 0. This is what keeps the dashboard's in-process sample legal under ADR-0247 at any `max_bytes`, and it needs no new state.

2. **Every other read runs in a `helper_run()` child, always.** There is one rule, with no size heuristic: a query with any filter may have to read the whole store, so it never runs on the loop. Only two paths reach the reader: the unfiltered path in item 1, and the child's work function (see [What implementing it settled](#what-implementing-it-settled) for how that is counted).

3. **`since` is a filter, never a stop condition.** `ts` is `time(NULL)` at write, and this daemon sets the clock (`ntp.c`, and an unsynced clock at boot). So a segment can hold a step backwards, and "the first `ts < since` ends the walk" would silently drop the entries after it.

4. **A filtered stream subscription is refused.** A subscriber names its path verbatim (`s->path`), so a filtered one would put an unbounded read in the loop every period. The subscribe reply is `refused` with the reason "a filtered history is a one-shot GET, not a stream". The dashboard subscribes only to the unfiltered tail.

### The request waits, through ADR-0340's park, generalized

5. **One parked-work record per connection, with a kind.** ADR-0340's `cred_park` becomes that record: kind credential check or log read. A filtered `GET /v1/system/logs` parks, `dispatch()` returns without a response, and when the child finishes the request is served again from the start through `serve_request()`. The handler then finds its result attached and answers from it. There is one park, one resume and one teardown, not a second copy for logs.

6. **When the client leaves, a log read is killed.** A credential check finishes its bookkeeping (ADR-0340). A log read has none, and a scan nobody will read must not hold a slot, so the child gets `SIGKILL` and its slot is released when it is reaped.

7. **Log reads have their own budget.** At most 2 run at once and 16 more wait, in arrival order; beyond that the answer is `503` with `Retry-After: 1`, the same shape as a login. The budget is separate from credential checks', so a login burst and a log scan cannot starve each other.

### The result comes back in a memory file

8. **The parent creates a `memfd_create()` file before the fork; the child writes the JSON array into it; the parent reads it after the child exits.** A pipe cannot carry it: a result can reach 5000 × 4 KB, about 21 MB, and the child would block on a full pipe that the parent does not read until the exit. A memfd needs no path and no cleanup on a crash, and it is freed with its last descriptor; a file under `STATE_DIR` would need both. The response is then written by the ordinary non-blocking writer (#237).

9. **The child sees a consistent store.** Three facts, read from the code:
   - `write_entry()` flushes after every entry, so the child sees everything written before the fork.
   - A line half-written after the fork is skipped, because `json_parse()` refuses it.
   - A segment evicted while the child reads it stays readable through the child's open `FILE *`, and one evicted before the child opens it is skipped.

   The child writes nothing to the log store. `emit_write_failure_entry()` reads the parent's failure counters as they were at the fork, which is the right answer for that moment.

### Gates

10. **`test_blocking_waits` counts the call sites** of the reader and of the two paths that reach it.
11. **The backward reader is checked against the old one.** The test compares their output entry for entry on a wrapped full segment, a tail spanning segments, and each filter with `since`.
12. **A live test in the shape of `test_hostauth`'s slow-login case.** It fills a large store and keeps a no-match `regex` read in flight. Meanwhile health must answer within 50 ms, a client that leaves must have its child killed, and the read past the queue must get the 503.

## What implementing it settled

- **The reader has one caller.** `logread_run()` (`daemon/src/logread.c`) is the only call to `logstore_tail_ex()`. The handler calls it for a bounded read, and the child reaches it through `logread_write_to()`. `test_blocking_waits` budgets `logstore_tail_ex(` to `logread.c`, `logread_run(` to `api_logs.c` once and `logread.c`, and `logread_write_to(` to `main.c` once. So a new direct read on the loop is a number someone has to raise.
- **The backward reader serves every query,** filtered or not. There is one reader, not one per path, and the forward one is gone.
- **`logstore_tail()` is removed.** It had no caller.
- **The query is parsed once, in `logread_query_parse()`.** The handler and the subscribe check both use it, so what the stream refuses and what the handler sends to a child are the same decision.
- **The 503 is written in `api_logs.c`, not `logread.c`,** so the reader and its query rules link into `test_logread` without the HTTP layer.
- **`helper_run_pid()`** is `helper_run()` that reports the child's pid. A log read whose client left is killed through that pid; `helper_run()` itself is unchanged for every other caller.

## Phase 2, tracked separately

A closed segment gets a summary at rotation: its largest `ts`, the set of sources, and the set of containers. It is stored beside the segment, so a filtered read skips any segment that cannot match. The skip is exact whatever the clock did, because a segment whose largest `ts` is below `since` holds no entry at or after it. That makes `since`, `source` and `container` reads proportional to the segments that can hold an answer. `regex` stays a full scan, because an operator asking for a regex over the store is asking for one. This is #616, not part of this decision.

## Consequences

- **Contract:** `GET /v1/system/logs` gains a documented `503`, and `openapi.yaml` and `docs/api/README.md` change in the same commit. A filtered stream subscription is refused. No client changes: `cixctl logs` and the dashboard already make one request and wait for its answer.
- **Daemon:**
  - `logstore_tail_ex()` reads backwards;
  - the park record gains a kind;
  - the log-read child, its memfd and its budget are added.
- **What stays as it is:** the segment format, `max_bytes`, ADR-0339's sampling, and every query parameter.
- **Cost:** about two build cycles. It is verified live on .95 by repeating the measurement above: the no-match `regex` read must leave no slow pass in the stall record, and health must answer within 50 ms while it runs.

## Alternatives considered

- **Every log read in a child, unfiltered included.** Rejected: the dashboard's sample runs in-process with no connection to park, and a fork every 2 s per host is the wrong cost for a read that can be made O(`tail`).
- **An in-memory ring of recent entries.** Rejected: it is a second copy of the newest entries. Seeding it at startup is itself a read of a segment, which cannot be bounded without the backward reader. The backward reader alone gives the same bound with no new state.
- **Stop at the first `ts < since`.** Rejected: unsound across a clock step (item 3). The phase 2 summary gives the same skip exactly.
- **A per-container index or per-container files.** Not rejected, and not needed yet. Phase 2's summaries answer the container case per segment; an index is a store-format change and its own decision, if measurement shows phase 2 is not enough.
- **A pipe for the result.** Rejected (item 8).
