# 0376 — The announcement self-check reads in a helper child

## Status

Accepted 2026-11-08. The owner parked this proposal on 2026-11-02 ("Move then we'll get back to it?") to return to after other work. On 2026-11-08, asked what came next, this was the pick. Issue [#646](https://git.home.arpa/itdlabs/cix/issues/646). It changes how [ADR-0339](0339-the-dashboard-is-told-what-changed.md)'s self-check (point 7) runs, not what it checks. It documents a `503` and a `500` on `POST /v1/events/verify`, and makes it an MCP tool. It uses [ADR-0342](0342-a-log-read-never-walks-the-store-on-the-event-loop.md)'s park, one more kind, and [ADR-0278](0278-the-reactor-forks-work-it-cannot-afford-to-wait-for.md)'s `helper_run()`, and adds no mechanism beside either.

## Context

ADR-0339 point 7 has the daemon check its own announcements. It runs each announced read that takes no path parameter through `api_run()`, in-process, and compares the hash of the answer with the last one. A state read whose answer changed while its generation did not is drift. The rolling check runs one state read every 3 s, round-robin. A full pass runs every such read at daemon start, as the baseline, and on `POST /v1/events/verify`, which `test_assert_no_drift()` calls at the end of every daemon-linked test.

That runs the handler on the event loop, so the handler's cost is the loop's cost. `GET /v1/system/stalls` on 192.168.15.95 (read 2026-11-08) names the self-check as the slowest event in its most recent records:

| ts | pass | slowest step |
|---|---|---|
| 1793934028 | 9410 ms, with a `stall` record | `GET /v1/pkg/cache` |
| 1793915024 | 5310 ms | `GET /v1/storage` (5240 ms) |
| 1793896968 | 1173 ms | `GET /v1/volumes` |
| 1793760163 | 778 ms | `GET /v1/volumes` |
| 1793563276 to 1793626729 | 840 to 943 ms, four times | `POST /v1/events/verify` |
| 1792946762 | 5606 ms | `GET /v1/pkg/cache` |

`GET /v1/pkg/cache` is `cache_scan()`: one `readdir` and one `stat` per cached artifact. It took 2.2 to 2.6 ms over 1237 entries on an idle box (2026-10-27). The slow passes came during hostbuilds and assemblies. That fits the disk being busy, but it was not measured. For 9.4 s the control plane answered nothing, so that the daemon could confirm a number it had announced correctly.

The self-check is diagnosis. It serves no client, and nothing waits for its answer except `POST /v1/events/verify`. So it is the one caller of these handlers whose cost can be taken off the loop without any client noticing.

## Decision

1. **Every state read the self-check makes runs in a `helper_run()` child.** That covers the rolling check, the start-up baseline and `POST /v1/events/verify`. The child runs `api_run()` for each read in its list exactly as the loop did, hashes each 200 body, and writes one fixed-size result (slot, readable, hash) per read into a memory file the parent made before the fork. For a state read, `events_self_read()` is called from that child (`events_check_work()`) and from nowhere else. The one other caller is a sample's tick on the loop (item 3).

2. **The parent applies the results, and the generation decides what one means.** The parent records every read's generation at the fork, then applies each result when the child exits:
   - A read whose generation moved between the fork and the result is not observed. It is neither drift nor a new baseline. An announcement inside the window means the child's answer may be from either side of the change, and recording it as a baseline would make the next honest answer read as drift.
   - Every other result is `events_observe()`d exactly as before. The drift line, the count and the announcement are unchanged.

   So drift still means only one thing: the answer changed and nothing at all was announced for that read.

3. **Samples stay in-process.** A sample is what a subscription streams (ADR-0339 phase 2), and ADR-0342 item 4 already keeps that read bounded on the loop. The rolling check's sample ticks are the subscriptions' own reads and do not move. A full pass still observes the samples it has always observed, from the child, because a sample's observation compares hashes only.

4. **The child changes nothing the parent reads.** It runs handler code, so this is stated, and enforced where a mistake would be silent:
   - **Log lines are held**, with `logstore_hold()` into the result, and the parent replays them. This is the credential check's mechanism (ADR-0340). The log store never has two writers.
   - **A helper child never starts a helper.** `helper_run_pid()` refuses in any helper child. A helper registers its pidfd on the daemon's epoll set, which a fork shares, so a child doing it would put a pointer into its own memory into the parent's epoll set. Today no self-readable handler starts one. A handler that would park answers busy, because `api_run()` is given no park connection, and so the read counts as unreadable, as before. The refusal makes that hold for every helper, not only this one.
   - **No state file is written.** `persist_atomic_write()` refuses in the self-check child (`persist_set_readonly()`). A GET handler that saved state on its way would race the parent's own write of the same file. Under this rule it fails, and the read counts as unreadable, so it is visible in `GET /v1/events/status` rather than silent.
   - An announcement made in the child is lost with the child. `api_run()` announces only for non-GET operations, and the self-check runs only GETs.
   - The stall watcher already ignores any process but the daemon's own (`g_step_owner`).

5. **One park kind, `PARK_EVENTSCHECK`, one running at a time.** `POST /v1/events/verify` parks, through the same `struct park` and `park_body_submit()` as a verification (#643). The parent builds its answer when the child exits, writes it into the record's memory file, and `respond_json_from_fd()` answers. Up to four requests wait. Beyond that the answer is 503 with `Retry-After`, as for every other park kind, and the contract documents it.

   `verifyEvents` becomes an MCP tool (ADR-0368). It was `none` only because its handler ran `api_run()` itself, which `test_mcp_exposure` refuses for a tool. Its handler now parks, as a package verification does. Through the gateway, the fork happens inside the gateway's own `api_run()`, so the child clears the inherited sentinel before its reads. That sentinel is the child's own copy, and the call it belongs to never resumes there.

   The daemon's own checks (the baseline, then every 3 s) are daemon jobs of the same kind, and **they never queue.** A tick that finds a check running or waiting does nothing. A slow check therefore delays the next one rather than stacking up behind it.

6. **The baseline runs in the child too, and no longer before serving.** Start-up submits it as the first daemon job, where `events_full_pass(NULL)` used to run synchronously before the loop. Until its result arrives, the rolling check has nothing to compare against, and a read's first observation is its baseline, as it always was. ADR-0375's handover runs the same start-up, so a switched cixd no longer spends the baseline's time before serving.

7. **A check never holds up an in-place switch.** ADR-0375's quiescence counts live helpers, so a running check delays a switch until it ends. While a host roll waits to switch (`HOSTROLL_SWITCH`), the timer starts no check, so the wait is at most one check long.

## Not decided here

**A client's GET of the same handlers still runs on the loop.** `GET /v1/pkg/cache`, `GET /v1/storage` and `GET /v1/volumes` cost a client what they cost the self-check, and a dashboard showing one re-reads it on every announcement. This ADR takes the self-check off the loop because that covers the whole class with one mechanism: the self-check runs every self-readable handler, and nothing waits on it. Fixing a handler's own cost means looking at each one separately. `cache_scan()` could keep a running total, since every write to the cache already announces through `pkg_announce_cache()`. `/v1/storage` and `/v1/volumes` read block devices and filesystems, and need their own measurement. That work is [#675](https://git.home.arpa/itdlabs/cix/issues/675).

## Consequences

- The self-check costs the loop one fork every 3 s, plus applying one result. The fork copies page tables, not memory, and it is the same cost every other helper already pays.
- `POST /v1/events/verify` answers when its child finishes. A test's `test_assert_no_drift()` waits exactly as before, through an ordinary HTTP request.
- `test_blocking_waits` counts the lines that name `events_self_read()`: its definition, the sample tick and the child. It counts every other off-loop function the same way, so this rule has a gate. `events_full_pass()`, the in-process pass, is gone.
- The baseline is now taken shortly after start rather than before serving. A change nobody announced in the first moments after start, before each read's baseline, goes unseen. Before this ADR, the same gap was every change made before the daemon started, which no check ever saw either.
