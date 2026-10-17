# 0339 — The dashboard is told what changed, instead of asking every few seconds

## Status

Proposed, 2026-10-17. Issue [#599](https://git.home.arpa/itdlabs/cix/issues/599). Adds one operation to the contract, so it is the owner's decision ([ADR-0317](0317-permissions-are-declared-by-the-api-contract.md) declares its permission; [ADR-0338](0338-the-dashboard-renders-from-the-contract.md) is what the dashboard's renderers already are, and is unchanged). Nothing below is implemented.

## Context

The dashboard learns about the box by asking. `poll()` (`web/app.js`) runs every 2 s (`POLL_INTERVAL_MS`). It reads health on every tick, the core and current-view refreshers every 5 s (`VIEW_INTERVAL_MS`), and every refresher on every page every 30 s (`SWEEP_INTERVAL_MS`). Container stats, host stats and network ports keep their own 2 s timers while their page is open. Nothing is pushed except the two WebSocket streams that exist, the container console and the build log.

**Measured, 2026-10-17**, in Chromium, running the box's own served `app.js`, `form.js` and `api.js` from 0.2.57-518 against reads captured from 192.168.15.95 (47 GET paths, 12 containers), with the real `poll()` left to run:

- **181 requests a minute** from one tab sitting on the Containers view. Two tabs are twice that, and a person changing nothing generates all of it.
- **Up to 5 s before a change shows on the page being looked at, and up to 30 s for everything else the dashboard holds** — the tree's other categories, the session's grants, every other page's cache. A container that crashes while you look at the Storage page is still "running" in the tree for up to half a minute.
- **Re-rendering does NOT lose the operator's place**, which #599 listed as unmeasured. Over 60 s of polling with unchanged data, a held table row stayed connected, an open context menu stayed open and scroll was untouched: the list renderers already skip a render whose signature is unchanged, and #592's `syncForm()` does the same for forms. One run did lose a row and close the menu, and that run had scrolled the pane first, and `document.addEventListener("scroll", hideContextMenu, true)` closes the menu on any scroll by design. So the cost argued here is staleness and load, and not a lost place.

The tree's state is therefore up to 30 s behind the box, and the only way to make it current is to ask more often, which multiplies the load. Asking less often makes it more stale. A timer cannot make both better at once; only being told can.

## Decision

**One WebSocket, `GET /v1/events`, on which the daemon says which resources changed. The dashboard re-reads what is affected through the ordinary gated GETs.** Each point below is decided by a constraint and is not a preference:

1. **A WebSocket, not Server-Sent Events.** `EventSource` has no way to set a header, and the dashboard authenticates with a bearer token. The two WebSockets already carry it as a subprotocol (`["cix", "cix.bearer." + authToken]`, `web/app.js:4202`, #541), and this one does the same.

2. **It is answered before `dispatch()`, so its handler calls `authorize_upgrade_route()` itself.** This is the trap CLAUDE.md names: the console was unauthenticated on every gated release before 0.2.57-392 because an upgrade bypassed `dispatch()`. The operation is `x-cix-permission: authenticated` (any signed-in session, as the three own-app-password operations already are), since the stream carries no data a session could not already read (point 3).

3. **An event names a resource, never its content, and never an instance name.** An event is `{"seq": N, "resource": "/v1/containers"}`: the contract path up to its first parameter. A stream that carried objects would have to filter each one against each subscriber's grants, which would be a second enforcement point beside `authorize_route()`, and the RBAC section forbids one. A stream that carried `/v1/containers/web` would tell a session without `containers:read` that a container called `web` exists. A collection path tells it only that something somewhere changed, which it could infer from the clock. The client re-reads that resource through `apiRequest()`, which is gated exactly as today.

4. **Two emit sources, both enumerated, and no catch-all.**
   - **Every change made through the API**, from one place: after `dispatch()` answers 2xx to a non-GET, it emits the route's resource. That covers the dashboard, cixctl, other tabs and other operators with one line.
   - **Every change the daemon makes by itself**, at named sites. Measured on `master` at 4f31f28e:
     - Container lifecycle in `daemon/src/registry.c`: `registry_create()` (`e->running = 1`, :174), `registry_mark_exited_with()` (:402), `registry_set_paused()` (:467), `registry_remove()` (:484), and the network and device attach/detach functions.
     - Package job state in `daemon/src/pkg.c`. The state is assigned at six sites (:1720, :5805, :9295, :10316, :12376, :14345). They become one setter, `pkg_job_set_state()`, which emits. Six scattered assignments with an emit beside each is the hand-kept list this project keeps having to delete.
     - Bootroot assembly in `daemon/src/main.c`: start (:9628) and finish (:9642, :32514).
     - Scheduler runs (`daemon/src/scheduler.c`), image versions produced by `image_produce_new_version()` (`pkg.c:5147`), and the service states `cix-init` reports.

     `persist_atomic_write()` (about 100 call sites in 48 files) was considered as the one hook and rejected. It knows a file path, not a resource, so mapping one to the other would be a second registry. It also misses in-memory state, and package jobs and container running state are exactly that.

5. **Samples are not changes.** CPU, memory and network graphs are time series, and an event that fires every second is a poll with extra steps. Those three per-page timers stay. So does the 2 s health read: it is what lights the TX/RX LEDs, and it costs one request.

6. **A slow tab cannot stall the daemon.** `build_log_ws_broadcast()` calls `ws_write_frame()` directly on the socket, which a wedged browser can block, and it holds at most `PKG_BUILD_LOG_WS_MAX` (4) clients in a fixed array. The event stream sends non-blocking from a bounded per-connection queue. When a client's queue is full, the daemon closes that connection, and the client reconnects and re-reads everything, which is correct anyway. There is no fixed small subscriber cap. `test_slow_client` is where the gate goes.

7. **The page tells the truth about the stream.** The daemon sends a ping frame every 10 s. If the client hears nothing for 25 s, or the socket closes, it shows **"Live updates lost — reconnecting"** and retries with backoff. It never shows a page that looks current while it is not. On reconnect the client does one full refresh, because events sent while it was away are gone; `seq` lets it notice a gap without a reconnect. A daemon restart drops every socket. That must reconnect silently with the stored session, and never send the operator to the login screen. ADR-0325 and 0.2.57-518 each removed one route to that screen, and this must not add a third. When the session the stream was opened with ends, the daemon closes it with code 4401, and the client takes the same path as a 401 today.

8. **What the timers become.** The 5 s view refresh is removed. An event runs the current view's refreshers and those of the resources it names, debounced by 150 ms so a burst of changes is one render. The 30 s sweep stays, for the facts no emit point can see because nothing in the daemon changes them: the clock, the NTP offset and the hotplug state of a disk. The ADR that implements this lists those facts by name, and anything else found to need the sweep is a missing emit point, filed as a bug.

## Consequences

- One operation in `openapi.yaml` (`GET /v1/events`, `authenticated`), with its README entry in the same change.
- One emit primitive in the daemon, called from `dispatch()` and from the sites above. The package job's state gets one setter.
- `app.js`: the 5 s view timer goes, an event handler and a stream-state indicator arrive, and the sweep stays at 30 s.
- Cost: on the order of three build cycles. One for the daemon side and its gates (`test_slow_client`, plus an events test that makes a change and asserts the event), one for the dashboard, and a margin for the gate that finds something.
- At rest a tab costs about 30 health reads a minute plus one 30 s sweep instead of 181 requests, and a change made anywhere appears in every open tab within one debounce rather than up to 30 s later. Neither number is measured until it is built; the gate that proves the second is the events test above.

## Alternatives considered

- **Poll less often.** It cuts the load and makes staleness worse, and staleness is the product defect.
- **Conditional GETs (`ETag` / `If-None-Match`).** They cut bytes and not requests, and fix neither staleness nor the rate.
- **Server-Sent Events.** Rejected by point 1.
- **Events carrying the changed objects.** Rejected by point 3. They would make the stream a second authorization surface.
- **One hook in `persist_atomic_write()`.** Rejected in point 4. It knows files rather than resources and misses in-memory state.
