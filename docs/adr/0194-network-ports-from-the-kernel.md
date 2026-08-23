# 0194 — A network's ports are whatever the kernel says is on the bridge

## Status

Accepted

## Context

A thinC network is a real Linux bridge. Everything about what is attached to it was, until now, answered from this daemon's own registry: which containers hold an address on it, which host interfaces were enslaved to it. Both are records of what this daemon *did*.

That is a fine answer right up to the moment it is wrong, and it is exactly the answer that cannot tell you it is wrong. A veth left on a bridge by a container the registry has forgotten, an interface enslaved by something outside this daemon — neither exists as far as the registry is concerned, and the operator sees a tidy picture of a network that is not the one the kernel is switching.

The request that prompted this asked for a switch panel: numbered ports, each showing what is plugged into it and that port's own traffic. Building that from the registry would have produced a drawing of the daemon's beliefs with the visual authority of a hardware faceplate.

## Decision

**The port list comes from `/sys/class/net/<bridge>/brif` — the kernel's own answer — and the registry annotates it.**

Each port is classified: `uplink` when the interface is one this network was told to bridge onto, `container` when it matches a running container's own host-side veth, and **`unattributed`** when it is on the bridge and nothing here can say why. That third case is the reason for the whole design. It is reported, not dropped, and the dashboard draws it with a dashed border rather than silently omitting it.

Three consequences follow from taking the kernel as the source:

**A container that is not running has no port.** It has no veth on the bridge; nothing is plugged in. Drawing a slot for it would state the opposite. Which containers are *defined* on a network is a separate, already-answered question.

**Counters are read from the port's own interface, and named from the port's side.** `rx_bytes` is what reached the switch from whatever is plugged in — the inverse of what the container sees on its own interface. This is the kind of number that is worse than useless if the direction is left implicit, so the field meaning is stated in the API description, the CLI output, and the dashboard panel.

**There are no port numbers.** A real switch's port 3 is port 3 forever; here, ports appear and vanish with containers, so any number would be positional and would move for reasons having nothing to do with the port. `ifname` is what names a port. The order returned is deterministic — uplinks, then container ports by container name, then unattributed — purely so a rendered panel does not reshuffle between polls; the dashboard numbers what it draws and says the number is its own.

The host-side veth name is a convention (`vh<pid>-<idx>`, coined at creation in `src/container_net.c`), not something stored. It was already being re-spelled at two call sites when this needed a third, so it is now derived in one place — including the live-attach case (ADR-0156), whose veth carries a real recorded name because it was never coined from a pid.

## Consequences

The panel can now show something the rest of the dashboard cannot: a disagreement between what this daemon thinks it did and what the kernel is actually doing. That is a diagnostic that did not exist before, and it exists precisely because the data is not sourced from the thing being checked.

The aggregate traffic chart sums every port, which means a byte crossing the switch is counted twice — once on the port it arrived by, once on the port it left by. That is switch throughput, not the network's traffic with the outside world, and the label says so rather than letting a plausible-looking number be read as the other thing.

Reading `brif` and `statistics/` per request is a handful of small `/sys` reads and does no work when nobody is looking; the dashboard polls it only while a network's own page is open, on the same 2s tick the container and host stats views already use.
