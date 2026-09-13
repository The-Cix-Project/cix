# 0284 — cixd always binds loopback, and the box's chosen address is a second listener

## Status

Accepted

## Context

[ADR-0283](0283-the-management-address-is-changeable-on-a-running-box.md) added `PUT /v1/system/management-network`. Verifying it on a live host destroyed that host's reachability, and the way it did is the whole reason this ADR exists.

The address the box answers on was recorded in **three** places, not one: `net.conf` on the config partition, the network registry, and the boot loader entry's `--bind=<ip>`. The endpoint updated the first two. At the next boot `cixd` bound the third, because `bootstrap_management_network()` assigns the global `g_bind_addr` from `net.conf` while `main()` passed its *own* argv-derived local, `bind_addr`, to `start_http_listener()` — and the flow between them is one-way (`g_bind_addr = bind_addr`, never back). So `net.conf` decided what `g_bind_addr` *said* and argv decided what was actually *bound*.

The result was `create_listen_socket: bind: cannot assign requested address`, a non-zero return from `cixd_main()`, and — because `cixd` is PID 1 — issue #131's park banner. Parking is the right floor for a genuinely unrecoverable start, but `cixd` also forks the interactive `cixctl` onto `/dev/tty0` and `/dev/ttyS0`, so parking removed the only local surface the machine has. There is no other shell on a Cix host. Recovery took a live ISO and an edit to the ESP.

A first attempt at fixing this added a *fallback*: try the chosen address, and if that fails, bind loopback instead. The owner rejected the shape:

> "the simple fix is to always bind to 127.0.0.1 and to bind to another address based on what was chosen for the box. this way a failure of this sort can never happen. your logic of us having a shell or otherwise is not ok, you know that cixd is pid 1 and we never have a shell on the box"

That is the correct criticism, and it is not about robustness. The console shell is a real `cixctl` speaking TCP to `cixd`, and its target was `g_bind_addr` — the management address. So *every* console session was one address change away from breaking, whether or not any bind ever failed. A fallback only helps in the case where the bind fails; it leaves the coupling in place in the case where it succeeds, which is every other boot.

## Decision

**`cixd` always binds `127.0.0.1`. The box's chosen address is a second listener, never an alternative to the first.** Skipped only when the chosen address already covers loopback: `DEFAULT_BIND` itself (an install with no management network), or the `0.0.0.0` wildcard some test invocations use — binding `127.0.0.1:P` while `0.0.0.0:P` is held fails `EADDRINUSE`, and `SO_REUSEADDR` does not change that.

**The loopback pair is never rebound and never stopped.** `PUT /system/daemon-config` and `PUT /system/management-network` move the management listeners only. An operator cannot switch off the local surface by reconfiguring the remote one, which is the point.

**The console shell and the stall watchdog talk to loopback**, not to `g_bind_addr`. Both ask a purely local question — "give me a shell", "is it serving?" — and neither should be able to answer "no" because the management address moved.

**The listener binds `g_bind_addr`, not `main()`'s local.** This is the one-line half of the fix, and it makes `net.conf` genuinely authoritative while demoting the loader entry's `--bind=` to a harmless echo.

**A chosen address that cannot be bound is logged, not fatal.** The box comes up serving on loopback with a working console and a `management-network set` to fix itself with. `net.conf` is deliberately *not* rewritten: the chosen address is still what the operator asked for, so the next boot retries it rather than quietly settling for loopback for ever. `maybe_confirm_boot()` treats this exactly as it treats a missing uplink (ADR-0256) — a boot that is running and reachable from nowhere must not be confirmed as a good A/B slot on the strength of having bound a socket.

**One implementation, explicitly targeted.** `start_http_listener()`/`start_https_listener()` take the `struct conn` to fill, and `start_listeners()` starts whichever protocols the config enables into a given pair. The loopback listeners use the same functions as the management ones rather than a second copy — the first draft of this change had grown a wrapper *and* an `_on` variant of each, which is two names for one behaviour and exactly what the maxims forbid.

## Consequences

The failure class is gone rather than handled: there is no decision about which address to run on, so there is no wrong answer to it. A mistyped or stale management address costs remote reachability and nothing else.

Four listeners exist where there were two. The accept path already dispatched on `conn->kind` rather than on which global a pointer matched, so it needed no change — but its comment named the two globals and has been corrected, since a comment that enumerates is a comment that goes stale.

The three-way duplication of the management address is reduced, not eliminated: the loader entry still carries `--bind=`, now unread at boot when `net.conf` parses, and regenerated from `g_bind_addr` on every A/B update. Recorded as a known duplication rather than left implicit.
