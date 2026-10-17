# 0339 — The dashboard is told what changed, and the daemon proves it told

## Status

Accepted, 2026-10-17, with the owner's adjustments over four reviews. Issue [#599](https://git.home.arpa/itdlabs/cix/issues/599). Adds three operations and three extensions to the contract, so it is the owner's decision ([ADR-0317](0317-permissions-are-declared-by-the-api-contract.md) declares their permissions; [ADR-0338](0338-the-dashboard-renders-from-the-contract.md) is what the dashboard's renderers already are, and is unchanged; [ADR-0070](0070-consolidated-log-store.md) is where drift is reported). Nothing below is implemented.

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

1. **Every GET operation declares `x-cix-read`, and `apigen` refuses one without it**, exactly as it refuses one with no `x-cix-permission` (#539). There are five kinds, and each says what the stream and the drift check do with it:

   | Kind | The answer | Announced | Drift-checked | Examples |
   |---|---|---|---|---|
   | `state` | changes only when something changes it | yes, from the change (point 4) | yes (point 7) | most of the contract |
   | `sample: <seconds>` | moves by itself | yes, when the daemon's periodic re-read differs (point 6) | no, it is the check | health, `server-health`, `ntp/status`, `tls-throttle/status`, `dhcp/leases` |
   | `session` | differs per session | yes, when that session's grants change (point 5) | no, the check has no session | `whoami`, `whoami/app-passwords` |
   | `stream` | is a running tail or series | never; read by subscription (point 10) | no | `logs`, `kmsg`, `stats`, `processes`, `stalls`, `pkg/build/log` |
   | `derived` | is a computation run on request | never | no | `pkg/verify` (8.2 s), `system/backup` (15.9 MB) |

   The clock is a `sample` the client ticks locally (point 6). **`/v1/storage` is declared `sample` until its live counters move to a read of their own**, a split filed as a ticket with this ADR, because a state read that embeds a sample can be neither announced nor verified honestly. `hostauth/sessions` is `state` with its `last_seen` field named in `x-cix-volatile-fields`, which the drift hash leaves out. That exclusion is declared in the contract rather than coded.

2. **Every mutating operation declares `x-cix-invalidates`** (198 of the contract's 340), listing the `operationId`s of the state reads whose answers it can change. Deriving it from the path cannot work. `POST /v1/pkg/install` derives to `/v1/pkg/install`, which no GET answers, while what it changes is `listPkg` now and `listImages` once the image moves. `apigen` seeds the list from the path where the path is right, refuses a mutating operation without one, and refuses a name that is not a `state` read.

### The primitive: a generation per read

3. **The daemon keeps one monotonic generation counter per announced read** (`gen[op]`, at most 142 integers), under a **boot epoch**: a random 64-bit value chosen at daemon start. One announce primitive increments the counters it is given. Everything else derives from them:
   - **An invalidation frame** is `{"epoch": "9f2c...", "gen": {"listContainers": 418}}`, and several coalesce into one frame.
   - **The first frame on a new stream is the whole vector**, masked by point 5. The heartbeat, every 10 s, carries it again, at about 600 bytes.
   - **Every GET response carries `X-Cix-Generation: <epoch>.<gen>`**, so a client knows exactly which generation its copy is.
   - **The client's one rule:** re-read a read only when the announced generation is newer than the one in its last response's header, and keep the header's value after reading. A second change that lands while a re-read is in flight then costs exactly one more read, never a duplicate and never a miss.
   - **An epoch that differs from the client's is a full re-read.** Without the epoch, a client holding `listContainers: 3` that reconnects after a restart during which three changes happened would see 3 again and re-read nothing. That would be a stale page with every indicator green, on the path this design most needs to be right. Counters restart with the daemon; the epoch says so.
   - **Reconnect within one epoch is a vector compare.** The client re-reads only what moved while it was away. There is no sequence number, no replay buffer and no "events sent while away are lost".
   - **A lost frame is noticed within 10 s at no cost.** A heartbeat generation newer than the client's copy, with no invalidation seen for it, triggers that one re-read without an extra request.

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
   - **What a session may learn is decided once, by the one enforcement point.** Even "`listContainers` changed" tells a session without `containers:read` that container activity is happening. So at the upgrade the daemon computes `allowed[op]` for each read. It uses the decision `authorize_route()` itself makes (`main.c:28775`), `hostauth_authorize(bearer, route->permission)`, without the refusal `authorize_route()` writes to a request's fd. The implementing change factors that decision out so both call one function; a copy of the check would be the parallel implementation the RBAC section forbids. The mask is recomputed when the session's grants change (#548), and that recomputation also drops any phase-2 subscription the session may no longer make. A stream never receives a generation for a read its session may not perform. There is no per-message check and no new logic: one precomputed mask from the existing function. A change to groups or permissions announces `whoami`, so `applySessionEligibility()` re-gates the page at once.
   - **Bounded by construction.** Each stream holds a pending set of the reads whose generation moved, a bitmap of at most 142 bits, not a queue. When the socket is writable (`EPOLLOUT`) the daemon sends one coalesced frame and clears the set. A stale read is stale once however many times it changed, so the set cannot overflow, a slow tab costs a few bytes rather than a growing buffer, and there is no "close on full" path and no reconnect storm. This is the daemon's first non-blocking WebSocket writer; `build_log_ws_broadcast()` writes blocking and caps viewers at four (`PKG_BUILD_LOG_WS_MAX`), and neither property carries over. The gate goes in `test_slow_client`.
   - **Session expiry is enforced on the open stream.** A timer per stream closes it with code 4401 at the session's expiry, because an open socket makes no requests that would notice.
   - **`cixctl events [--follow]`** prints the stream, and `cixctl events verify` runs the drift check (point 7). Every capability has a CLI surface (API-First), and this one is also the instrument for checking announcements on a real box.

### Nothing displayed is polled once signed in

6. **The daemon owns every timer the dashboard used to own.**
   - **Liveness is the heartbeat.** The TX/RX LEDs show the stream's own traffic.
   - **Health is a sample the daemon re-reads.** It announces when health changed, so a degraded subsystem reaches every open tab without any tab asking.
   - **Signed out, health is still read on a timer**, because there is no session and so no stream. `GET /v1/health` is `public` and is how the lock screen says whether the host is answering (`refreshLockFacts()`, #597). The timer stops when the stream opens and resumes when the session ends.
   - **The clock is read once on connect**, and again whenever the daemon steps it (`PUT /v1/system/time`, or an SNTP correction in `ntp.c`), which announces `getSystemTime`. The browser ticks it locally.
   - **Samples are the daemon's loop, not the client's.** For each `sample: N` read, the daemon re-runs the handler into memory every N seconds through the response sink, hashes the answer, and announces only when the hash changed. That covers NTP status, the TLS-throttle counters (coalesced, since they change on every connection attempt), server health, DHCP leases (re-reading the lease file, so no inotify watcher is needed) and, until it is split, storage. **The client never sets a timer for any of them.**

### The daemon verifies what it announced

7. **The drift check runs in the daemon, once for every client, into the one log store.**
   - **How it reads:** every `state` read is re-run into memory through the response sink, bypassing authorization because it is the daemon reading itself. The handler is given a sentinel fd that the sink recognises and captures. The one fd operation on that path before the sink, `http_set_blocking()` in `respond_json()`, returns -1 on a bad fd and its result is ignored (`daemon/src/http.c`, read 2026-10-17), so the sentinel is tolerated. The body, less any `x-cix-volatile-fields`, is hashed and compared with the previous hash. Reads are spread across 5 minutes, one per event-loop turn, because the measured cost of the whole set (about 0.4 s) would otherwise stall the single-threaded loop at once.
   - **What it reports:** a changed hash with an unmoved generation is a missed announcement. The daemon writes `"<operationId> changed without an announcement"` to the log store with source `events`, so cixctl, the dashboard and `GET /v1/system/logs` all see it. It also counts it on `GET /v1/events/status` (`authenticated`). A defect count is not given to anyone who can reach the public health endpoint.
   - **It can be asked for.** `POST /v1/events/verify` runs a full pass immediately and answers 202; its findings go to the log store and the count. `cixctl events verify` is that call. It is how an operator checks a box, and how a test runs the final pass.
   - **Why the daemon and not the browser:** deciding that the system's truth drifted is daemon logic (API-First), it should happen once rather than per tab, and the finding belongs in ADR-0070's store rather than one browser's panel.
   - **What it cannot see:** parameterized reads (`getContainer`, `getStorageFreeSpace`) are not enumerated, since there is no instance list to walk. Their collection reads are, and a changed instance changes its collection.
   - **It makes every test an events test.** Every daemon-linked test, before it stops its daemon, calls `POST /v1/events/verify` and asserts that `GET /v1/events/status` reports zero drift. Every existing test that changes anything then also proves that the change was announced, with no new fixtures. That is the exhaustiveness a representative test set cannot give.
   - **A test that changes state behind the daemon's back declares it.** Writing a state file or deleting a bridge directly will rightly be reported as drift, because the daemon was not told. Such a test names the reads it expects to drift and why, in the test, and asserts exactly those. The assertion is never weakened to make it pass.

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

- **Contract:** three operations (`GET /v1/events`, `GET /v1/events/status` and `POST /v1/events/verify`, all `authenticated`), `x-cix-read` on all 142 GETs, `x-cix-invalidates` on all 198 mutations, and `x-cix-volatile-fields` where a state read carries a field it does not own. `apigen` gates all three, and the README is updated in the same change. Filed with this ADR: the split of `/v1/storage`'s counters from its state.
- **Daemon:**
  - **The core:** the generation vector, the announce primitive and the announcing sites, with `pkg_job_set_state()`.
  - **The stream:** the non-blocking stream with its pending sets and permission masks, and the session timer.
  - **The loop:** the sample loop and the drift check, both running handlers through the existing response sink.
  - **Reporting:** the drift count on `GET /v1/events/status`, and `POST /v1/events/verify`.
- **cixctl:** `events [--follow]` and `events verify`.
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

## Implementation notes

Recorded as each step lands, where the implementation is narrower than or departs from the text above.

- **Session end is checked at every heartbeat, not by a timer per stream** (step three, 0.2.57-521).
  Each heartbeat asks the session decision again and closes the stream with 4401 when it no longer
  stands. That covers revocation and logout as well as expiry, needs no bookkeeping per stream, and
  notices an ended session within 10 s.
- **The masks and the heartbeat ask without spending the token.** `hostauth_authorize()`'s decision
  became one function shared by a consuming entry point and a peeking one
  (`hostauth_authorize_peek()`). Under a single-use config the consuming check spends the token it
  checks, so asking it 124 times at a stream's upgrade would have signed the operator out at once.
- **An app password cannot open a stream.** The operation is `authenticated`, and `authenticated`
  means a session (ADR-0317), so `cixctl events --follow` needs `cixctl login` first.
- **A grant change is detected through the contract.** A change whose `x-cix-invalidates` names
  `getWhoami` marks every stream's mask for recomputation before the next frame, so the set of
  grant-changing operations is the contract's, not a hand-kept list.
- **`POST /v1/events/verify` answers 200 with its findings, not 202** (step four, 0.2.57-524). A full
  pass is about half a second, so it runs synchronously. The caller, usually a test, gets a deterministic
  `{"checked": n, "drift": [...], "unreadable": [...]}` instead of having to read the log store. It is
  `system:write`, because each call costs the loop that half second.
- **Checked against the box before shipping:** the 86 parameterless state reads were hashed over HTTP
  5 minutes apart at rest. Only `listHostauthSessions` differed, in its `expires_in_seconds`, which is
  declared volatile and left out of the daemon's hash. So the classification needed no change before
  the check could report.
- **The at-exit assertion is in `test_events` only, for now.** It rolls out to every daemon-linked test
  under #607, once `cixctl events status` on 192.168.15.95 has shown zero drift for 24 hours at rest.
  That is a measured condition rather than a promise, and it keeps one missing announcement from
  landing as forty failing tests.
- **The check's first catch was a misdeclared read** (step four's first build, 0.2.57-525).
  `test_events` reported drift on `getConfig` after a site change. `GET /v1/config` is, in its own
  description, *"a VIEW, derived fresh on every call"* over 33 sections, so it was declared `state` by
  mistake and is `derived`. The two bulk changes that listed it, `applyConfig` and `postSystemRestore`,
  can change any read, so they declare `x-cix-invalidates: [*]`, which announces every read. `apigen`
  accepts `*` only standing alone. 524's run had failed without naming the read; the test now prints
  the daemon's `events` log lines when drift is found, which is how 525 named it.
- **The first live check caught a view** (0.2.57-526 on 192.168.15.95). `cixctl events verify`
  compared 84 state reads in 0.44 s and named one: `getPipeline`, a read-time join (ADR-0256) whose
  inputs were announced while the view itself was not. Some of those inputs are daemon-made changes:
  containers becoming ready after boot, and the booted ESP entry being confirmed minutes later. Two
  changes answer it:
  - **`x-cix-composed-of`** declares a view's inputs once, and announcing any input announces the
    view. One level only, and gated by `apigen`.
  - **`confirm_boot()`** now announces `getEsp` and `getSystemBoot`.

  `getConfig`, made `derived` the step before, is the same kind of view and could take this mechanism
  instead, which would make Running Config live. It is left `derived` for now.
- **Step five: the dashboard on the stream** (0.2.57-528).
  - **What a change re-reads:** phase 1 is any change re-running the core refreshers and the current
    page's, debounced 150 ms. Response-header generations are not tracked yet, so a second change landing
    mid-refresh costs one extra refresh, never a miss.
  - **What stays on a timer:** the log panel keeps its timer until phase 2, as do the three stats pages.
    The once-a-minute status-bar refresh stays because load average is a sample.
  - **When the stream is down,** the page falls back to the polling it had and says "Live updates lost —
    reconnecting". That fallback is the "page tells the truth" clause, not a regression.
  - **Measured** in Chromium with the box's own files and the four frames 0.2.57-523 sent on the box,
    replayed. A heartbeat costs zero requests, a change frame one refresh of six reads, and an idle
    minute 32 requests against the 181 of the Context: 30 are the log panel's 2 s poll and 2 the status
    bar.
- **The first hour on 528 found ten reads changing unannounced**, and each was fixed where it changes
  (0.2.57-529). They came in four kinds:
  - **Clock-derived fields, declared volatile:** `idle_seconds` in `listBuildEnvironments`, a log's
    `size_bytes` and `modified_at` while it is written, and a package job's `stage_seconds`,
    `run_seconds` and `last_output_seconds_ago`. A log's final size is announced when it closes.
  - **A view, declared:** `getPkgDrift` is composed of `listPkg` and `listPkgRecipes`.
  - **Daemon-made changes with no setter:**
    - the 34 hand-written chain-slot releases became `chain_release()` (gated by `test_announce`);
    - recipe-store writes announce, and a sync announces in the daemon on completion, because its
      merge ran in a helper whose own announcements reach no listener;
    - the artifact cache's stores, evictions and clears;
    - a sync's state and its write-back queue;
    - the kernel release refresh at start and end, and a discovery run's end;
    - the build-memory raise and its return;
    - bad-version marks.
  - **An expiry made by a reader's clock:** a session left `listHostauthSessions` when the reader saw
    its `expires_at` pass, which no code did. A session now ends by code: a timer armed at the soonest
    expiry runs `hostauth_reap_expired()`, which drops, saves and announces, and the reader lists every
    live entry.

  A drift found later is also announced (`events_observe()`), as well as counted and logged. A page is
  not left stale for longer than one check, while the log still names the missing site for fixing.
- **The drift decision is pure and tested without a daemon.** `events_hash_body()` and
  `events_observe()` live in `events.c`, and `test_events_drift` drives them through the case no daemon
  test can produce cleanly: a state change with no announcement.
- **Hotplug announces only hardware subsystems** (`block`, `usb`, `pci`, `scsi`; step two). Every
  container start adds a veth (`SUBSYSTEM=net`), and announcing it would have re-read the device and
  storage reads on every container lifecycle event.

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
