# 0339 — The dashboard is told what changed, and the daemon proves it told

## Status

Proposed, 2026-10-17. Issue [#599](https://git.home.arpa/itdlabs/cix/issues/599). Adds one operation and two extensions to the contract, so it is the owner's decision ([ADR-0317](0317-permissions-are-declared-by-the-api-contract.md) declares the operation's permission; [ADR-0338](0338-the-dashboard-renders-from-the-contract.md) is what the dashboard's renderers already are, and is unchanged; [ADR-0070](0070-consolidated-log-store.md) is where drift is reported). Nothing below is implemented.

## Context

The dashboard learns about the box by asking. `poll()` (`web/app.js`) runs every 2 s (`POLL_INTERVAL_MS`). It reads health on every tick, the core and current-view refreshers every 5 s (`VIEW_INTERVAL_MS`), and every refresher on every page every 30 s (`SWEEP_INTERVAL_MS`). Container stats, host stats and network ports keep their own 2 s timers while their page is open. Nothing is pushed except the two WebSocket streams that exist, the container console and the build log.

**Measured on 2026-10-17, before anything was designed.** The first set was taken in Chromium, running the box's own served `app.js`, `form.js` and `api.js` from 0.2.57-518 against reads captured from 192.168.15.95 (47 GET paths, 12 containers), with the real `poll()` left to run:

- **181 requests a minute** from one tab sitting on the Containers view. Two tabs are twice that, and a person changing nothing generates all of it.
- **Up to 5 s before a change shows on the page being looked at, and up to 30 s for everything else the dashboard holds.** A container that crashes while you look at the Storage page is still "running" in the tree for up to half a minute.
- **Re-rendering does NOT lose the operator's place**, which #599 listed as unmeasured. Over 60 s with unchanged data, a held table row stayed connected, an open context menu stayed open and scroll was untouched. The one run that lost the menu had scrolled first, and `hideContextMenu` is bound to every scroll by design. So the cost is staleness and load, not a lost place.

The second set was taken directly against the box's API, timing and hashing each of the contract's **108 GET operations that take no path parameter**:

- **106 answer** (one is a 400 without its query parameter, one a 404 for a key not set). All 106 together take **9.5 s**, and two of them take nearly all of it: `/v1/pkg/verify` takes 8.2 s because it is a verification run, and `/v1/system/backup` takes 0.85 s because it builds a 15.9 MB bundle. Those two are computations a person asks for, not reads of state. **The other 104 take about 0.4 s together.**
- **Read twice 15 s apart with nobody doing anything, 9 of those 104 answered differently**: `/v1/system/time`, `stats`, `processes`, `stalls`, `logs`, `kmsg` and `server-health`; `/v1/system/hostauth/sessions`, which changes because reading it touches the reader's own session; and `/v1/storage`, which mixes slowly-changing state with live disk counters in one answer. So the contract's reads are not all one kind, and **any design that treats them alike will either miss changes or report false ones.**
- **Every response leaves through one function.** `http_emit()` (`daemon/src/http.c`) hands each chunk to the sink `main.c` installs with `http_set_response_sink()` (#237, `main.c:34263`). The only direct writes are the two WebSocket upgrade replies (`main.c:29922`, `:30237`). So the daemon can run any of its own GET handlers into memory without refactoring a handler.

A timer cannot make staleness and load better at once. **The box owns every fact the dashboard shows, so it should say when a fact changed, and it should prove, to itself and to every test, that it did.** Proxmox's interface polls. This decision beats it because nothing displayed is polled once someone is signed in, and because a change the daemon fails to announce is caught on the box itself and fails the tests that would have made it.

## Decision

### Every read says what kind it is

1. **Every GET operation declares `x-cix-read`, and `apigen` refuses one without it**, exactly as it refuses one with no `x-cix-permission` (#539). There are three kinds:
   - **`state`**: the answer changes only when something changes it, an API call or a daemon event. A state read is announced when it changes (point 3) and verified by the daemon (point 7). Most of the contract is this.
   - **`sample: <seconds>`**: the answer moves by itself, and the daemon re-reads it on that period and announces when it changed (point 6). Health, `server-health`, `ntp/status`, `tls-throttle/status` and `dhcp/leases` are samples.
   - **`derived`**: a computation run on request, never displayed as live state, never announced and never verified. `pkg/verify` and `system/backup` are this.

   Logs, kmsg, stats, processes and stalls are **streams**: they are read through the log reader and the phase-2 subscriptions (point 10), never by invalidation. The clock is read once and ticked locally (point 6). **`/v1/storage` is declared `sample` until its live counters move to a read of their own**, and that split is a ticket filed with this ADR, because a state read that embeds a sample can be neither announced nor verified honestly. `hostauth/sessions` is `state` with its `last_seen` field excluded from the verification hash, and the ADR that implements this names that exclusion in the contract (`x-cix-volatile-fields`) rather than in code.

2. **Every mutating operation declares `x-cix-invalidates`** (198 of the contract's 340), listing the `operationId`s of the state reads whose answers it can change. Deriving it from the path cannot work. `POST /v1/pkg/install` derives to `/v1/pkg/install`, which no GET answers, while what it changes is `listPkg` now and `listImages` once the image moves. `apigen` seeds the list from the path where the path is right, refuses a mutating operation without one, and refuses a name that is not a `state` read.

### The primitive: a generation per read

3. **The daemon keeps one monotonic generation counter per state and sample read** (`gen[op]`, at most 142 integers), and one announce primitive that increments the counters it is given. Everything else derives from it:
   - **An invalidation frame** is `{"gen": {"listContainers": 418}}`, and several coalesce into one frame.
   - **The heartbeat**, every 10 s, carries the whole vector, which is about 600 bytes.
   - **Every GET response carries `X-Cix-Generation: <gen[op]>`**, so a client knows which generation its copy is.
   - **Reconnect is a vector compare.** The client re-reads only the reads whose generation moved while it was away. A daemon restart starts every counter again, every generation then differs, and a full re-read follows with no special case. There is no sequence number, no replay buffer and no "events sent while away are lost".
   - **A lost frame is noticed within 10 s at no cost.** A heartbeat generation newer than the client's copy, with no invalidation seen for it, triggers that one re-read and a drift report (point 7), without a single extra request.

4. **The announce primitive is called from two places, both enumerated.**
   - **Every change made through the API**, from one place: after `dispatch()` answers 2xx to a non-GET, it announces that route's `x-cix-invalidates`. That covers the dashboard, cixctl, other tabs and other operators.
   - **Every change the daemon makes by itself.** These sites were found on `master` at 4f31f28e by grepping for state assignments in `main.c`, `pkg.c` and `registry.c`, and for files that persist or log state outside a request:
     - **Container lifecycle**, `registry.c`: `registry_create()` (:174), `registry_mark_exited_with()` (:402), `registry_set_paused()` (:467), `registry_remove()` (:484), and the network and device attach/detach functions.
     - **Package job state**, `pkg.c`: six sites (:1720, :5805, :9295, :10316, :12376, :14345) become one setter, `pkg_job_set_state()`.
     - **Bootroot assembly**, `main.c`: start (:9628) and finish (:9642, :32514).
     - **Device hotplug**: the `NETLINK_KOBJECT_UEVENT` listener (`CONN_UEVENT`, `main.c:4810`).
     - **Asynchronous jobs**: storage and volume migrations, volume backups, scheduler runs, `image_produce_new_version()` (`pkg.c:5147`), and the service states `cix-init` reports.

     This is a first inventory and not a proof. Point 7 is what proves it.

### The stream

5. **`GET /v1/events`, a WebSocket.**
   - **Why a WebSocket:** `EventSource` cannot set a header, and the dashboard authenticates with a bearer token, carried as the `cix.bearer.<token>` subprotocol as the console and build log already do (`web/app.js:4202`, #541).
   - **Authorization at the upgrade:** it is answered before `dispatch()`, so its handler calls `authorize_upgrade_route()` itself. That is the trap CLAUDE.md names, the console's before 0.2.57-392. The operation is `x-cix-permission: authenticated`.
   - **No data:** the stream carries generations, never content and never an instance name. Clients re-read through `apiRequest()`, gated as today.
   - **What a session may learn is decided once, by the one enforcement point.** Even "`listContainers` changed" tells a session without `containers:read` that container activity is happening. So at the upgrade the daemon computes `allowed[op]` by running `authorize_route()` against each read's own GET route, once per read. It recomputes the mask when the session's grants change (#548). A stream never receives a generation for a read its session may not perform. There is no per-message check and no new logic: one precomputed mask from the existing function. A change to groups or permissions announces `whoami`, so `applySessionEligibility()` re-gates the page at once.
   - **Bounded by construction.** Each stream holds a pending set of the reads whose generation moved, a bitmap of at most 142 bits, not a queue. When the socket is writable (`EPOLLOUT`) the daemon sends one coalesced frame and clears the set. A stale read is stale once however many times it changed, so the set cannot overflow, a slow tab costs a few bytes rather than a growing buffer, and there is no "close on full" path and no reconnect storm. This is the daemon's first non-blocking WebSocket writer; `build_log_ws_broadcast()` writes blocking and caps viewers at four (`PKG_BUILD_LOG_WS_MAX`), and neither property carries over. The gate goes in `test_slow_client`.
   - **Session expiry is enforced on the open stream.** A timer per stream closes it with code 4401 at the session's expiry, because an open socket makes no requests that would notice.
   - **`cixctl events [--follow]`** prints the stream. Every capability has a CLI surface (API-First), and this one is also the instrument for checking announcements on a real box.

### Nothing displayed is polled once signed in

6. **The daemon owns every timer the dashboard used to own.**
   - **Liveness is the heartbeat.** The TX/RX LEDs show the stream's own traffic.
   - **Health is a sample the daemon re-reads.** It announces when health changed, so a degraded subsystem reaches every open tab without any tab asking.
   - **Signed out, health is still read on a timer**, because there is no session and so no stream. `GET /v1/health` is `public` and is how the lock screen says whether the host is answering (`refreshLockFacts()`, #597). The timer stops when the stream opens and resumes when the session ends.
   - **The clock is read once on connect**, and again whenever the daemon steps it (`PUT /v1/system/time`, or an SNTP correction in `ntp.c`), which announces `getSystemTime`. The browser ticks it locally.
   - **Samples are the daemon's loop, not the client's.** For each `sample: N` read, the daemon re-runs the handler into memory every N seconds through the response sink, hashes the answer, and announces only when the hash changed. That covers NTP status, the TLS-throttle counters (coalesced, since they change on every connection attempt), server health, DHCP leases (re-reading the lease file, so no inotify watcher is needed) and, until it is split, storage. **The client never sets a timer for any of them.**

### The daemon verifies what it announced

7. **The drift check runs in the daemon, once for every client, into the one log store.**
   - **How it reads:** every `state` read is re-run into memory through the response sink, bypassing authorization because it is the daemon reading itself. Its body, less any `x-cix-volatile-fields`, is hashed and compared with the previous hash. Reads are spread across 5 minutes, one per event-loop turn, because the measured cost of the whole set (about 0.4 s) would otherwise stall the single-threaded loop at once.
   - **What it reports:** a changed hash with an unmoved generation is a missed announcement. The daemon writes `"<operationId> changed without an announcement"` to the log store with source `events`, so cixctl, the dashboard and `GET /v1/system/logs` all see it. It also counts it in a `drift` field on `GET /v1/health`.
   - **Why the daemon and not the browser:** deciding that the system's truth drifted is daemon logic (API-First), it should happen once rather than per tab, and the finding belongs in ADR-0070's store rather than one browser's panel.
   - **What it cannot see:** parameterized reads (`getContainer`, `getStorageFreeSpace`) are not enumerated, since there is no instance list to walk. Their collection reads are, and a changed instance changes its collection.
   - **It makes every test an events test.** Every daemon-linked test asserts at exit that the drift counter is zero, after a final drift pass over every state read. Every existing test that changes anything then also proves that the change was announced, with no new fixtures. That is the exhaustiveness a representative test set cannot give.

### Gates

8. **Four gates, each testing something the others cannot.**
   - **Declared** (`test_apigen`): every GET has `x-cix-read`, and every mutation has `x-cix-invalidates` naming only state reads.
   - **Correct** (a new events test in SELFTESTS): for a representative set of changes, it reads every runnable state read before and after and asserts that the declared set contains every read whose answer changed. That catches a wrong annotation, which a test that only checks "the event arrived" cannot. The set is create, stop and remove a container, install a package, a build through its states, attach a network, and an injected uevent if the harness can inject one.
   - **Exhaustive** (every daemon-linked test): the drift counter is zero at exit (point 7).
   - **Constructed**: the package job's state becomes reachable only through `pkg_job_set_state()`, enforced by the compiler where the struct can be made module-private. A lint in the `test_naming` pattern fails on `->running =` in `registry.c` outside its announcing functions, since `registry_entry` is read everywhere.
   - **The client logic** is gated in `test_web_form`'s quickjs harness: the debounce, the vector compare, the three truth states and the hidden-tab rule.

### The page tells the truth

9. **About the stream, the session and the tab.**
   - **No heartbeat for 25 s, or a closed socket**, shows **"Live updates lost — reconnecting"** and retries with backoff. The page never looks current while it is not.
   - **A daemon restart reconnects with the stored session**, never via the login screen. ADR-0325 and 0.2.57-518 each removed one route to that screen, and this must not add a third.
   - **Code 4401** takes the same path as a 401 today.
   - **A hidden tab** marks itself stale on an invalidation and compares generations once when it becomes visible.
   - **Any invalidation** re-runs the refreshers for what it names, debounced by 150 ms so a burst is one render.

### Phase 2

10. **Streams and per-refresher reads.** Container stats, host stats, network ports, processes and stalls become subscriptions on the stream. A subscription names a GET operation, is authorized by `authorize_route()` when it is made, and receives the latest sample only; replacing an unsent one bounds it as point 5 bounds invalidations. The log panel's tail joins the same mechanism. Each refresher then declares the operation ids it reads, so an invalidation re-runs exactly the refreshers that need it. Until phase 2, the three stats timers stay, and an invalidation re-runs the core and current view's refreshers. That is more than needed, never less, and only when something changed.

## Consequences

- **Contract:** one operation (`GET /v1/events`), `x-cix-read` on all 142 GETs, `x-cix-invalidates` on all 198 mutations, and `x-cix-volatile-fields` where a state read carries a field it does not own. `apigen` gates all three, and the README is updated in the same change. Filed with this ADR: the split of `/v1/storage`'s counters from its state.
- **Daemon:**
  - **The core:** the generation vector, the announce primitive and the announcing sites, with `pkg_job_set_state()`.
  - **The stream:** the non-blocking stream with its pending sets and permission masks, and the session timer.
  - **The loop:** the sample loop and the drift check, both running handlers through the existing response sink.
  - **Reporting:** the `drift` health field.
- **cixctl:** `events [--follow]`.
- **Dashboard:** every timer goes except signed-out health and the phase-2 stats timers; the stream, the vector compare, the truth states and the local clock arrive.
- **Load, by arithmetic, re-measured on the box once built** with the same Chromium harness that produced the 181: signed in and idle, **zero requests a minute** from the dashboard. The daemon's own self-reads are about 104 a 5-minute cycle plus the samples, all in memory, with no socket and no TLS. Latency goes from up to 30 s to one debounce.
- **Cost, priced from the measurements above: eight build cycles is the floor.**
  - One for the contract and `apigen`: 340 annotations, mostly seeded, and three gates.
  - Two for the generation vector, the announcements and the stream with its non-blocking writer.
  - One for the sample loop and the drift check through the sink.
  - One for the gates: the before/after test, the drift-at-exit assertion across the daemon-linked tests, and the construction lint.
  - One for the dashboard and cixctl.
  - Two reserved: one for the non-blocking writer, which has no precedent here, and one for what the first drift run on .95 finds. False drift there is a misclassified read or a missing announcement, and each is fixed rather than suppressed.
  - Phase 2 is priced separately.

## Alternatives considered

- **Poll less often.** It cuts the load and makes staleness worse, and staleness is the product defect.
- **Conditional GETs (`ETag` / `If-None-Match`).** They cut bytes and not requests, and fix neither staleness nor the rate. `X-Cix-Generation` is what they would have been, made useful by the stream.
- **Server-Sent Events.** They cannot carry the bearer token (point 5).
- **Invalidations carrying the changed objects.** That needs per-message permission filtering, a second enforcement point. The permission mask (point 5) decides what a session may learn once, with the existing check.
- **Deriving the invalidated read from the path.** It is wrong for every action endpoint (point 2).
- **One hook in `persist_atomic_write()`.** It knows files rather than reads, and misses in-memory state (point 4).
- **A per-connection queue that closes on overflow.** It is replaced by a pending set that cannot overflow (point 5).
- **Sequence numbers and a replay buffer for reconnect.** They are replaced by the generation vector, which also detects lost frames and needs no buffer (point 3).
- **The drift check in the browser.** It runs per tab, reports where cixctl never looks, and is daemon logic in a client (point 7).
- **No drift check at all, relying on representative tests.** Factory reset, reboot, update and the disk operations cannot run against a test daemon, and the internal inventory is not complete by construction. The daemon's own check, asserted at the exit of every test, is what makes coverage exhaustive.
- **Treating every GET alike.** 9 of 104 change by themselves and 2 are computations (measured above). A design that ignores that either misses changes or cries wolf.
