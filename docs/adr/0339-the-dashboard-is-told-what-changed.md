# 0339 — The dashboard is told what changed, instead of asking every few seconds

## Status

Proposed, 2026-10-17. Issue [#599](https://git.home.arpa/itdlabs/cix/issues/599). Adds one operation and two extensions to the contract, so it is the owner's decision ([ADR-0317](0317-permissions-are-declared-by-the-api-contract.md) declares the operation's permission; [ADR-0338](0338-the-dashboard-renders-from-the-contract.md) is what the dashboard's renderers already are, and is unchanged). Nothing below is implemented.

## Context

The dashboard learns about the box by asking. `poll()` (`web/app.js`) runs every 2 s (`POLL_INTERVAL_MS`). It reads health on every tick, the core and current-view refreshers every 5 s (`VIEW_INTERVAL_MS`), and every refresher on every page every 30 s (`SWEEP_INTERVAL_MS`). Container stats, host stats and network ports keep their own 2 s timers while their page is open. Nothing is pushed except the two WebSocket streams that exist, the container console and the build log.

**Measured, 2026-10-17**, in Chromium, running the box's own served `app.js`, `form.js` and `api.js` from 0.2.57-518 against reads captured from 192.168.15.95 (47 GET paths, 12 containers), with the real `poll()` left to run:

- **181 requests a minute** from one tab sitting on the Containers view. Two tabs are twice that, and a person changing nothing generates all of it.
- **Up to 5 s before a change shows on the page being looked at, and up to 30 s for everything else the dashboard holds**: the tree's other categories, the session's grants and every other page's cache. A container that crashes while you look at the Storage page is still "running" in the tree for up to half a minute.
- **Re-rendering does NOT lose the operator's place**, which #599 listed as unmeasured. Over 60 s of polling with unchanged data, a held table row stayed connected, an open context menu stayed open and scroll was untouched: the list renderers already skip a render whose signature is unchanged, and #592's `syncForm()` does the same for forms. One run did lose a row and close the menu, and that run had scrolled the pane first, and `document.addEventListener("scroll", hideContextMenu, true)` closes the menu on any scroll by design. So the cost argued here is staleness and load, and not a lost place.

A timer cannot make both better at once: asking more often multiplies the load, and asking less often makes it more stale. **The box owns every fact the dashboard shows, so it should be the one that says when a fact changed.** Proxmox's interface polls. This decision beats it only if three things hold, and each is decided below rather than left to the implementation:

- **Nothing displayed is polled once someone is signed in.**
- **What a change invalidates is declared in one place and tested for correctness**, not merely for delivery.
- **A change the daemon fails to announce is caught and reported**, never silently covered.

## Decision

**One WebSocket, `GET /v1/events`. On it the daemon names the reads a change has made stale, including the facts that change by themselves, and carries the heartbeat that says the host is alive. The dashboard re-runs stale reads through the ordinary gated GETs.**

### The stream

1. **A WebSocket, not Server-Sent Events.** `EventSource` has no way to set a header, and the dashboard authenticates with a bearer token. The two existing WebSockets already carry it as a subprotocol (`["cix", "cix.bearer." + authToken]`, `web/app.js:4202`, #541), and this one does the same.

2. **It is answered before `dispatch()`, so its handler calls `authorize_upgrade_route()` itself.** This is the trap CLAUDE.md names: the console was unauthenticated on every gated release before 0.2.57-392 because an upgrade bypassed `dispatch()`. The operation is `x-cix-permission: authenticated`, meaning any signed-in session, as the three own-app-password operations already are. That is enough because the stream carries no data at all, only invalidations (point 4) and the heartbeat. Even the facts that change by themselves (point 7) arrive as invalidations and are re-read through their own gated GETs.

3. **What a change invalidates is declared by the contract: `x-cix-invalidates` on every mutating operation** (198 of the contract's 340), listing the `operationId`s of the GETs whose answers it can change. Deriving it from the path cannot work. `POST /v1/pkg/install` derives to `/v1/pkg/install`, which no GET answers, while what it changes is `listPkg` now and `listImages` once the image moves. Every `/{name}/stop`, `/migrate` and `/apply` has the same shape. `apigen` seeds the list from the path where the path is right, refuses a list naming a GET that does not exist, and refuses a mutating operation with no list, as it already refuses one with no `x-cix-permission` (#539).

4. **An invalidation names reads, never content, and never an instance.** It is `{"seq": N, "stale": ["listContainers"]}`. A stream carrying changed objects would have to filter each one against each subscriber's grants, which would be a second enforcement point beside `authorize_route()`, and the RBAC section forbids one. A stream carrying `/v1/containers/web` would tell a session without `containers:read` that a container called `web` exists. An operation id says only which kind of read to repeat, and the repeat goes through `apiRequest()`, gated exactly as today.

5. **A slow tab cannot stall the daemon.** `build_log_ws_broadcast()` calls `ws_write_frame()` directly on the socket, which a wedged browser can block, and it holds at most `PKG_BUILD_LOG_WS_MAX` (4) clients in a fixed array. This stream is the daemon's first non-blocking WebSocket writer: a bounded per-connection queue drained on `EPOLLOUT`. When a queue is full the daemon closes that connection; the client reconnects and re-reads everything, which is correct anyway. There is no fixed small subscriber cap. The gate goes in `test_slow_client`.

### What is pushed instead of polled

6. **Liveness is the stream's heartbeat once signed in.** The daemon sends a ping frame every 10 s. The TX/RX LEDs show the stream's own traffic, and a health change, such as a subsystem degrading, arrives as an invalidation of `getHealth`. **Signed out, health is still read on a timer**, because there is no session and so no stream: `GET /v1/health` is `public`, and it is how the lock screen says whether the host is answering (`refreshLockFacts()`, #597). The timer stops when the stream opens and resumes when the session ends.

7. **Facts that change by themselves are announced by the code that owns them, as invalidations like any other.** None of them is asked for on a timer:
   - **The clock** is read once on connect, and again whenever the daemon steps it (`PUT /v1/system/time`, or an SNTP correction in `ntp.c`), which invalidates `getSystemTime`. The browser ticks it locally from the last read.
   - **NTP status** is invalidated when `ntp.c`'s sync completes with a changed result.
   - **TLS-throttle counters** (`connthrottle.c`) change on every connection attempt, so announcing each change would flood the stream. They are coalesced to at most one announcement every 5 s, and only when a field the dashboard displays has changed.
   - **DHCP leases are the one exception in phase 1.** dnsmasq writes them inside its container with no daemon involvement, and watching that file from the host is new machinery for one read. So `getDhcpLeases` carries `x-cix-sampled: true` and is read on a 30 s timer while its page is open. Removing that sample is its own decision and its own ticket.

8. **Graphs subscribe instead of polling (phase 2).** A page that shows container stats, host stats or network ports subscribes to them on the stream. The daemon pushes each sample at its sampling rate while that subscription is open, and stops when the page unsubscribes or the socket closes. These are the only frames that carry data. A subscription names a GET operation, and the daemon authorizes it with `authorize_route()` on that operation's route when the subscription is made, so the one enforcement point decides it; nothing on the stream is filtered per message. Until phase 2 lands, those three 2 s timers stay as they are.

### Announcing internal changes, and proving it

9. **Two emit sources.** Every change made through the API is announced from one place: after `dispatch()` answers 2xx to a non-GET, it emits that route's `x-cix-invalidates`. That covers the dashboard, cixctl, other tabs and other operators. Every change the daemon makes by itself is announced at named sites. These were found by grepping, on `master` at 4f31f28e, for state assignments in `main.c`, `pkg.c` and `registry.c`, and for files that persist or log state outside a request. That is **a first inventory, not a proof of completeness**, which is why points 10 and 11 exist:
   - **Container lifecycle**, `daemon/src/registry.c`: `registry_create()` (`e->running = 1`, :174), `registry_mark_exited_with()` (:402), `registry_set_paused()` (:467), `registry_remove()` (:484), and the network and device attach/detach functions.
   - **Package job state**, `daemon/src/pkg.c`. The state is assigned at six sites (:1720, :5805, :9295, :10316, :12376, :14345). They become one setter, `pkg_job_set_state()`, which announces.
   - **Bootroot assembly**, `daemon/src/main.c`: start (:9628) and finish (:9642, :32514).
   - **Device hotplug**: the `NETLINK_KOBJECT_UEVENT` listener (`CONN_UEVENT`, `main.c:4810`, ADR-0161 Phase C) already sees every add and remove.
   - **Asynchronous jobs and probes**: container-storage and volume migrations (`containerstoragemigrate.c`, `volume.c`), volume backups (`volumebackup.c`), server-health probe results (`serverhealth.c`), scheduler runs (`scheduler.c`), image versions produced by `image_produce_new_version()` (`pkg.c:5147`), and the service states `cix-init` reports.

   `persist_atomic_write()` (about 100 call sites in 48 files) was rejected as a single hook. It knows a file path, not a read, and it misses in-memory state, which is exactly what package jobs and container running state are.

10. **Three gates, each testing something the others cannot.**
    - **Declared** (`test_apigen`): every mutating operation carries `x-cix-invalidates`, and every operation id it names exists as a GET.
    - **Correct** (a new events test, in SELFTESTS): for a representative set of changes, the test reads every GET it can run before and after and asserts that **the declared set contains every read whose answer actually changed.** The set is create, stop and remove a container, install a package, a build reaching each of its states, attach a network, and an injected uevent if the test harness can inject one. That is what catches a wrong annotation. Asserting only that "the declared event arrived" would prove the plumbing, which `dispatch()` makes true by construction. The set is representative rather than all 198, because factory reset, reboot, update and the disk operations cannot run against a test daemon, and the ADR says so rather than claiming coverage it does not have.
    - **Constructed** (a lint gate in the `test_naming` pattern): it fails on `->state =` in `pkg.c` outside `pkg_job_set_state()`, and on `->running =` in `registry.c` outside the announcing functions. So a new state change that bypasses its announcing setter fails a release, and the protection is a number in a diff rather than a sentence here.

11. **The drift check stays in the field, and says what it finds.** Every 5 minutes, on reconnect and when a hidden tab becomes visible, the dashboard re-reads everything it holds. When a value differs from the cached one and no invalidation named that read since the previous check, it writes `"<operationId> changed without an event"` to its own log panel and counts it. This is verification, not display: the page never waits for it to be current. It is the only instrument that can see an internal change the inventory missed or a declaration the gate's representative set did not exercise. Dropping it for a cleaner design would trade a visible defect for a silent stale page, which this project ranks as a product bug of the first rank. Its cost is about 9 requests a minute.

### The page tells the truth

12. **About the stream.** If the client hears nothing for 25 s, or the socket closes, it shows **"Live updates lost — reconnecting"** and retries with backoff. It never shows a page that looks current while it is not. On reconnect it does one full re-read, because events sent while it was away are gone; `seq` lets it notice a gap without one.
    - **A daemon restart drops every socket and must reconnect silently with the stored session**, never via the login screen. ADR-0325 and 0.2.57-518 each removed one route to that screen, and this must not add a third.
    - **A session can expire while the socket sits open with no request to check it**, so the daemon keeps a timer per stream and closes it at the session's expiry with code 4401. The client takes the same path as a 401 today.
    - **A hidden tab marks itself stale on an invalidation and re-reads once when it becomes visible.**

13. **What the client does with an invalidation, in two steps.**
    - **Phase 1:** any invalidation re-runs the core refreshers and the current view's, debounced by 150 ms so a burst is one render. That removes the 5 s view timer and the 30 s sweep, and delivers the latency.
    - **Phase 2**, with point 8: each refresher declares the operation ids it reads, so an invalidation re-runs only the refreshers that read something named stale. Until then the dashboard re-reads more than it needs to, never less, and only when something changed.

## Consequences

- **Contract:** one operation (`GET /v1/events`, `authenticated`), `x-cix-invalidates` on the 198 mutating operations (mostly seeded from the path), and `x-cix-sampled` on `getDhcpLeases`. `apigen` gates both extensions, and the README is updated in the same change.
- **Daemon:** one announce primitive, called from `dispatch()` and the sites in point 9; `pkg_job_set_state()`; the coalesced throttle announcement; clock and NTP announcements; and the first non-blocking WebSocket writer, with the per-stream session timer.
- **Dashboard:** the 5 s view timer and the 30 s sweep go; the stream and its truth states arrive; health polls only while signed out; the clock ticks locally; and the drift check arrives.
- **Gates:** `test_apigen` extended, an events test that compares before and after, a construction lint, and `test_slow_client` extended.
- **Load, by arithmetic rather than measurement.** Signed in and idle, phase 1 makes about 9 requests a minute: the drift check's roughly 45 reads every 5 minutes. To that add 2 a minute for leases while the DHCP page is open, the three stats timers while their pages are open until phase 2, and reads only when something changed. Today's figure is 181. Latency goes from up to 30 s to one debounce. Both are re-measured on the box once built, with the same Chromium harness that produced the 181.
- **Cost: six build cycles is the floor, not the estimate.**
  - One for the contract and `apigen`.
  - Two for the daemon: the stream, the non-blocking writer and the announcements, then the gates.
  - One for the dashboard.
  - One for the before/after events test.
  - One reserved for the non-blocking writer, the part with no precedent in this daemon.
  - Phase 2 (stats subscription and per-refresher operation ids) is priced separately when phase 1 is done.

## Alternatives considered

- **Poll less often.** It cuts the load and makes staleness worse, and staleness is the product defect.
- **Conditional GETs (`ETag` / `If-None-Match`).** They cut bytes and not requests, and fix neither staleness nor the rate.
- **Server-Sent Events.** Rejected by point 1.
- **Invalidations carrying the changed objects.** Rejected by point 4. They would make the stream a second authorization surface.
- **Deriving the invalidated read from the path.** Rejected by point 3. It is wrong for every action endpoint, and a guess cannot be gated.
- **One hook in `persist_atomic_write()`.** Rejected in point 9. It knows files rather than reads and misses in-memory state.
- **Keep the full 30 s sweep as a safety net.** It holds load near today's (about 120 a minute) and hides every missing announcement. Point 11 keeps what a safety net is for, at a twentieth of the rate, and makes it report rather than cover.
- **No field verification at all, relying on the gates.** Rejected by point 11. The gates cannot run every operation, and the inventory of internal changes is not complete by construction. Without the check, a missed announcement is a silently stale page.
- **Health over the stream for everyone.** A signed-out page has no session and so no stream, and the lock screen must still say whether the host answers (point 6).
