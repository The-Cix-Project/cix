# 0180 — Asynchronous container teardown (delete/stop never block the daemon)

## Status

Accepted (2026-08-21). Implements the fix for issue #67.

## Context

`cixd` is a single-threaded epoll loop — one blocking call inside any
handler freezes the entire control plane: every endpoint, every client,
the dashboard. `DELETE /v1/containers/{name}` and `POST .../stop` on a
*running* container both funneled through `registry_remove()`, whose
kill sequence was synchronous: SIGKILL via pidfd, then
`waitid(P_PIDFD, …, WEXITED)` — an **unbounded** wait. "SIGKILL is
unblockable" is a statement about signal masks, not about task state: a
child stuck in uninterruptible D-state only ever *pends* the kill, and
the wait never returns.

This was not theoretical. It froze the whole daemon twice in real
production on 192.168.15.95 within one day (both required a hard host
reset to recover), and the user independently reported having hit the
same wedge before. The evidence trail (issue #67): the wedged delete
left container row, DNS record, and issued cert all intact — the
handler blocked before its first state mutation, exactly at the wait.
`registry_remove()`'s own original comment had explicitly deferred this:
"a fully async kill+reap would need a pending-removal state machine;
not warranted for a v1 skeleton." Production disagreed.

A second, smaller hazard rode along: the thaw-before-kill only ran when
`e->paused` said the cgroup was frozen. A stale flag (any path that
freezes without updating it) would mean SIGKILL pends into a frozen
cgroup forever — the same unbounded wait by another door.

## Decision

Teardown of a **running** container becomes asynchronous, completing on
the existing crash-detection machinery rather than a synchronous wait:

1. **Durable intent first, synchronously.** The handler writes
   everything that must survive a daemon restart *before* killing
   anything: for DELETE, the service-ownership forgets
   (DNS/LDAP/PKI/NTP/syslog) and `containerdef_remove()`; for stop,
   `containerdef_set_stopped(name, 1)`. No restart window can
   resurrect the container.
2. **Kill without waiting.** New primitive `registry_begin_kill(e,
   kind)`: unconditionally thaw the cgroup (a no-op write when already
   thawed — this also fixes the stale-`paused`-flag hazard everywhere,
   including the synchronous paths), send SIGKILL via the pidfd,
   record `teardown_kind` (`REGISTRY_TEARDOWN_STOP`/`DELETE`) on the
   entry, return. The response goes out immediately (DELETE: 204;
   stop: 200 with status `"stopping"`).
3. **Completion on the reactor.** The entry's pidfd epoll registration
   — the exact mechanism that already detects ordinary crashes — fires
   when the process is actually dead. `handle_container_event()` sees
   the recorded `teardown_kind` and runs the completion: registry slot
   release (safe now: `registry_remove()`'s kill/wait branch is
   skipped for a non-running entry), plus, for delete, the
   umount-then-rmtree disk cleanup, and for a pkgbuild container the
   `pkg_build_completed()` bookkeeping that needs the real exit
   status. The crash-restart logic is bypassed entirely for these
   exits.
4. **Visible transitional state.** Until reaped, the entry reports
   status `"deleting"`/`"stopping"` (never a misleading `"running"`),
   and a same-name create 409s off the still-in-use registry slot —
   the standard name-conflict path, no new mechanism. Repeat
   DELETE/stop during the window are idempotent; DELETE during a stop
   teardown upgrades the recorded intent in place (one pending pidfd
   event completes whichever kind it finds).

If a child genuinely never dies (kernel-level D-state hang), the
container remains visibly `"deleting"` forever and **the daemon stays
fully alive** — the failure is scoped to the one container and
surfaced, instead of taking the whole control plane down silently.

## Consequences

- **Client-visible semantics change.** A 204/200 means the intent is
  durably recorded and the kill sent — not that teardown has finished.
  Anything that previously chained "delete container → immediately act
  on what it released" must settle first: the container's own GET
  reaching 404 (delete) or status `"stopped"` (stop). The concrete
  case found while converting the test suite: DELETE container →
  DELETE its network can transiently 409 ("still in use") until the
  attachment is released at reap time. Documented in the API reference;
  every affected test now settle-polls (bounded at 5s — a SIGKILLed
  child taking longer *is* a regression).
- **Not converted (deliberately, tracked on #67):** the
  storage-migration finalize path keeps its synchronous
  stop-inside-a-job (its all-or-nothing semantics genuinely want one),
  and the rolling-restart timer path likewise still uses the
  synchronous primitive — both now benefit from the unconditional
  thaw, both remain candidates for phase-2 conversion via the same
  `teardown_kind` machinery (a `RESTART` kind whose completion replays
  the definition).
- **Daemon-restart edge:** intent is durable, so nothing resurrects;
  a child that was mid-teardown when the daemon itself exited is
  outside the registry on the next boot (on a real install cixd is
  PID 1 — a daemon exit is a host reboot; in dev harnesses the test
  data-dir isolation already covers it).

## Alternatives considered

**Bounded synchronous wait** (WNOHANG poll loop with a ~5s deadline):
much smaller patch, no contract change — rejected because the daemon
still freezes for up to the deadline on every slow teardown, and the
timeout case leaves a half-killed container with no visible state,
trading an honest async contract for a quieter version of the same
outage class.
