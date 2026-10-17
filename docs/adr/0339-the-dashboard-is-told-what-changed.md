# 0339 — The dashboard is told what changed, instead of asking every few seconds

## Status

Proposed, 2026-10-17. Issue [#599](https://git.home.arpa/itdlabs/cix/issues/599). Adds one operation and one extension to the contract, so it is the owner's decision ([ADR-0317](0317-permissions-are-declared-by-the-api-contract.md) declares the operation's permission; [ADR-0338](0338-the-dashboard-renders-from-the-contract.md) is what the dashboard's renderers already are, and is unchanged). Nothing below is implemented.

## Context

The dashboard learns about the box by asking. `poll()` (`web/app.js`) runs every 2 s (`POLL_INTERVAL_MS`). It reads health on every tick, the core and current-view refreshers every 5 s (`VIEW_INTERVAL_MS`), and every refresher on every page every 30 s (`SWEEP_INTERVAL_MS`). Container stats, host stats and network ports keep their own 2 s timers while their page is open. Nothing is pushed except the two WebSocket streams that exist, the container console and the build log.

**Measured, 2026-10-17**, in Chromium, running the box's own served `app.js`, `form.js` and `api.js` from 0.2.57-518 against reads captured from 192.168.15.95 (47 GET paths, 12 containers), with the real `poll()` left to run:

- **181 requests a minute** from one tab sitting on the Containers view. Two tabs are twice that, and a person changing nothing generates all of it.
- **Up to 5 s before a change shows on the page being looked at, and up to 30 s for everything else the dashboard holds**: the tree's other categories, the session's grants and every other page's cache. A container that crashes while you look at the Storage page is still "running" in the tree for up to half a minute.
- **Re-rendering does NOT lose the operator's place**, which #599 listed as unmeasured. Over 60 s of polling with unchanged data, a held table row stayed connected, an open context menu stayed open and scroll was untouched: the list renderers already skip a render whose signature is unchanged, and #592's `syncForm()` does the same for forms. One run did lose a row and close the menu, and that run had scrolled the pane first, and `document.addEventListener("scroll", hideContextMenu, true)` closes the menu on any scroll by design. So the cost argued here is staleness and load, and not a lost place.

A timer cannot make both better at once: asking more often multiplies the load, and asking less often makes it more stale. Only being told can.

Proxmox's interface polls too. This decision is only better than polling if two things hold, and both are decided below rather than left to the implementation. First, what a change invalidates is declared in one place (point 3). Second, a change the daemon fails to announce becomes visible instead of being silently covered by a fallback (point 8).

## Decision

**One WebSocket, `GET /v1/events`, on which the daemon names the reads a change has made stale. The dashboard re-runs them through the ordinary gated GETs.**

1. **A WebSocket, not Server-Sent Events.** `EventSource` has no way to set a header, and the dashboard authenticates with a bearer token. The two existing WebSockets already carry it as a subprotocol (`["cix", "cix.bearer." + authToken]`, `web/app.js:4202`, #541), and this one does the same.

2. **It is answered before `dispatch()`, so its handler calls `authorize_upgrade_route()` itself.** This is the trap CLAUDE.md names: the console was unauthenticated on every gated release before 0.2.57-392 because an upgrade bypassed `dispatch()`. The operation is `x-cix-permission: authenticated`, meaning any signed-in session, as the three own-app-password operations already are. That is enough because the stream carries no data (point 4).

3. **What a change invalidates is declared by the contract: `x-cix-invalidates` on every mutating operation**, listing the `operationId`s of the GETs whose answers it can change. Deriving it from the path cannot work. `POST /v1/pkg/install` derives to `/v1/pkg/install`, which no GET answers, while what it changes is `listPkg` now and `listImages` once the image moves. Every `/{name}/stop`, `/migrate` and `/apply` has the same shape. `apigen` seeds the list from the path where the path is right, refuses a list naming a GET that does not exist, and refuses a mutating operation with no list at all, as it already refuses one with no `x-cix-permission` (#539). It is one source of truth, enforced by a gate rather than by prose.

4. **An event names reads, never content, and never an instance.** An event is `{"seq": N, "stale": ["listContainers", "getHealth"]}`. A stream carrying objects would have to filter each one against each subscriber's grants, which would be a second enforcement point beside `authorize_route()`, and the RBAC section forbids one. A stream carrying `/v1/containers/web` would tell a session without `containers:read` that a container called `web` exists. An operation id tells it only which kind of read to repeat, and the repeat is gated by `apiRequest()` exactly as today.

5. **Two emit sources, enumerated, with the gap made visible by point 8 rather than assumed away.**
   - **Every change made through the API**, from one place: after `dispatch()` answers 2xx to a non-GET, it emits that route's `x-cix-invalidates`. That covers the dashboard, cixctl, other tabs and other operators.
   - **Every change the daemon makes by itself**, at named sites. These were found by grepping, on `master` at 4f31f28e, for state assignments in `main.c`, `pkg.c` and `registry.c`, and for files that persist or log state outside a request. That is a first inventory, not a proof of completeness:
     - **Container lifecycle**, `daemon/src/registry.c`: `registry_create()` (`e->running = 1`, :174), `registry_mark_exited_with()` (:402), `registry_set_paused()` (:467), `registry_remove()` (:484), and the network and device attach/detach functions.
     - **Package job state**, `daemon/src/pkg.c`. The state is assigned at six sites (:1720, :5805, :9295, :10316, :12376, :14345). They become one setter, `pkg_job_set_state()`, which emits. Six scattered assignments with an emit beside each is the hand-kept list this project keeps having to delete.
     - **Bootroot assembly**, `daemon/src/main.c`: start (:9628) and finish (:9642, :32514).
     - **Device hotplug**: the `NETLINK_KOBJECT_UEVENT` listener (`CONN_UEVENT`, `main.c:4810`, ADR-0161 Phase C) already sees every add and remove, so it emits.
     - **Asynchronous jobs and probes**: container-storage and volume migrations (`containerstoragemigrate.c`, `volume.c`), volume backups (`volumebackup.c`), server-health probe results (`serverhealth.c`), scheduler runs (`scheduler.c`), image versions produced by `image_produce_new_version()` (`pkg.c:5147`), and the service states `cix-init` reports.

     `persist_atomic_write()` (about 100 call sites in 48 files) was rejected as a single hook. It knows a file path, not a read, and it misses in-memory state, which is exactly what package jobs and container running state are.

6. **Samples are not changes.** CPU, memory and network graphs are time series, and an event that fires every second is a poll with extra steps. Those three per-page timers stay. So does the 2 s health read: it lights the TX/RX LEDs and costs one request.

7. **A slow tab cannot stall the daemon.** `build_log_ws_broadcast()` calls `ws_write_frame()` directly on the socket, which a wedged browser can block, and it holds at most `PKG_BUILD_LOG_WS_MAX` (4) clients in a fixed array. The event stream is the daemon's first non-blocking WebSocket writer: a bounded per-connection queue drained on `EPOLLOUT`. When a queue is full the daemon closes that connection; the client reconnects and re-reads everything, which is correct anyway. There is no fixed small subscriber cap. The gate goes in `test_slow_client`.

8. **The sweep becomes an instrument, not a crutch.** A fallback that silently re-reads everything would hide every missing emit point for ever, which is the "surface that looks fine while being wrong" this project ranks first among product bugs. So:
   - **Facts nothing in the daemon changes are read on a timer, by name, and only they are:** the clock (`getSystemTime`), NTP sync status, TLS-throttle counters, DHCP leases (dnsmasq writes them inside its container, with no daemon involvement), and the log tail, which already has its own reader. Their GETs carry `x-cix-sampled: true` in the contract, so this list is itself declared rather than kept here.
   - **A verification sweep re-reads everything every 5 minutes.** When it fetches a value that differs from the cached one, and no event named that read since the previous sweep, the dashboard writes `"<operationId> changed without an event"` to its own log panel and counts it. A missing emit point therefore appears in the field as a named defect, to be filed as a bug, instead of being covered up.

9. **The page tells the truth about the stream.** The daemon sends a ping frame every 10 s. If the client hears nothing for 25 s, or the socket closes, it shows **"Live updates lost — reconnecting"** and retries with backoff. It never shows a page that looks current while it is not. On reconnect it does one full refresh, because events sent while it was away are gone; `seq` lets it notice a gap without one.
   - **A restart does not sign anyone out.** A daemon restart drops every socket. That must reconnect silently with the stored session, never via the login screen. ADR-0325 and 0.2.57-518 each removed one route to that screen, and this must not add a third.
   - **A session's end is noticed without a request.** A session can expire while the socket sits open with no request to check it, so the daemon keeps a timer per stream and closes it at the session's expiry with code 4401. The client then takes the same path as a 401 today.
   - **A hidden tab does not re-read.** It marks itself stale on an event and refreshes once when it becomes visible again.

10. **What the client does with an event, in two steps.**
    - **First step:** any event re-runs the core refreshers and the current view's, debounced by 150 ms so a burst is one render. That alone removes the 5 s view timer and delivers the latency.
    - **Second step:** each refresher declares the `operationId`s it reads, so an event re-runs only the refreshers that read something named stale. Until that is done the dashboard re-reads more than it needs to, but never less, and the extra reads happen only when something actually changed.

## Consequences

- Contract: one operation (`GET /v1/events`, `authenticated`), `x-cix-invalidates` on every mutating operation (198 of the contract's 340), and `x-cix-sampled` on the sampled reads (`getSystemTime`, `getSystemNtpStatus`, `getSystemTlsThrottleStatus`, `getDhcpLeases`). `apigen` gates both extensions. The README is updated in the same change.
- Daemon: one emit primitive, called from `dispatch()` and from the sites in point 5; one package-job state setter; the first non-blocking WebSocket writer.
- Dashboard: the 5 s view timer goes; an event handler, the stream-state indicator and the drift instrument arrive; the 30 s sweep becomes a 5 minute one.
- Load, by arithmetic rather than measurement: health at 30 a minute, four sampled reads at 30 s intervals (8), and about 45 reads every 5 minutes (9). That is roughly **50 requests a minute at rest against 181 today**, plus reads only when something changed. Latency goes from up to 30 s to one debounce. Both are re-measured on the box once built, with the same Chromium harness that produced the 181.
- Cost: four build cycles is the honest estimate. One for the contract and `apigen` (the 198 annotations are mostly seeded from the path), one for the daemon and its gates (`test_slow_client`, and an events test that makes a change through the API and through the daemon and asserts each event), and one for the dashboard. The fourth is reserved for the non-blocking writer, the part with no precedent in this daemon and the one most likely to grow.

## Alternatives considered

- **Poll less often.** It cuts the load and makes staleness worse, and staleness is the product defect.
- **Conditional GETs (`ETag` / `If-None-Match`).** They cut bytes and not requests, and fix neither staleness nor the rate.
- **Server-Sent Events.** Rejected by point 1.
- **Events carrying the changed objects.** Rejected by point 4. They would make the stream a second authorization surface.
- **Deriving the invalidated read from the path.** Rejected by point 3. It is wrong for every action endpoint, and a guess cannot be gated.
- **One hook in `persist_atomic_write()`.** Rejected in point 5. It knows files rather than reads and misses in-memory state.
- **Keep the full 30 s sweep as a safety net.** It holds load near today's (about 120 a minute) and hides every missing emit point. Point 8 keeps what a safety net is for and makes it report rather than cover.
