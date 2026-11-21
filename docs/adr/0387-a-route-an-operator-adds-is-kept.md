# 0387 — A route an operator adds is kept, and an interface a network records is attached when it appears

## Status

Accepted, 2026-11-21 (#715). The owner asked for the fix after gibsson-1 came back from two reboots off the network: *"this journey is getting more sucky with every step"*, and *"why did it lose it's IP address?"*. Amends [ADR-0067](0067-network-address-field-rename.md)'s route endpoints, which kept nothing.

## Context

Two pieces of boot-time network state were applied once and never again, both measured on gibsson-1 on 2026-11-21:

- **A network's recorded interface.** `network_init()` re-attaches each network's recorded interfaces to its bridge, and runs before the boot's module loads. The box's NIC is r8169, which this kernel builds as a module (`CONFIG_R8169=m`), so `eth0` did not exist yet. The attach failed, the record still said `eth0` was on `border`, and nothing tried again. `network attach-interface border --interface=eth0` then answered `409 interface is already attached`, and the box was unreachable until it was detached and re-attached at the console. Uevents for `SUBSYSTEM=net` were ignored.
- **A route.** `POST /v1/system/routes` put a route in the kernel and kept nothing: its contract said so ("nothing added here survives a reboot"). The box's default route is via the cores' VRRP address, 192.168.253.254, so after every reboot it had none, and it answered nothing outside its own subnets until the route was added again by hand. A route whose gateway is a container's address cannot be applied at boot anyway: the containers start later.

## Decision

1. **A recorded interface that is absent is waiting, not lost.** `network_init()` marks it waiting and logs that once. `network_reattach_waiting()` attaches every waiting interface that is present: after the boot's kmod sweep and configured module loads, and on every `SUBSYSTEM=net` add uevent for that name. `attach-interface` on a waiting interface attaches it instead of answering 409. The waiting flag lives in memory only; the record is unchanged.
2. **An added route is kept.** `routeconfig.c` saves each route `POST /v1/system/routes` adds to `STATE_DIR/routes.json`. The main loop's `route_tick()` puts every kept route back: every 5 s while one waits for its gateway, then once a minute, which also restores a route the kernel dropped when an interface went down. A route the kernel already has is kept and answered 204; it was a 400 that read like a failure. Any other refusal names the kernel's reason and keeps nothing. `DELETE` forgets the route as well, and with no gateway in the body it matches a kept route whatever its gateway, as the kernel's delete does. `GET` lists the kept routes under `saved`, each `applied` or `waiting`.
3. **The daemon announces what it changes.** `route_tick()` announces `getSystemRoutes` when a route goes in or drops out, and an attach from a uevent announces `listNetworks` (ADR-0339).

## Alternatives considered

- **Retry the interface on a timer.** Rejected: the kernel announces a new interface as a uevent, and the module-load step is a known point at boot. Polling would add a periodic cost for an event that has a signal.
- **Keep routes in `net.conf`.** `net.conf` is the installer's file and holds one gateway. Routes are an API resource and belong in the state directory with the rest.
- **Order the boot so modules load before `network_init()`.** It would fix the NIC at boot and nothing else: a USB adapter plugged in later, or a driver loaded by a hotplug, would still be missed.

## Consequences

- A reboot of a box whose uplink NIC is a module, or whose default route goes through its own containers, no longer needs anyone at the console.
- Existing hosts have no `routes.json`; routes added before this release are still in the kernel and not kept. Adding them once more keeps them, and answers 204.
- `test_routes` (DAEMON_SELFTESTS) asserts that a duplicate add is 204, that the route is listed as kept and applied and written to `routes.json`, and that a delete without its gateway forgets it.
