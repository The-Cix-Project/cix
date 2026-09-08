# 0264 — A container may bridge its own interfaces

## Status

Accepted

Issue [#30](https://git.home.arpa/itdlabs/cix/issues/30). Builds on [ADR-0259](0259-an-interface-may-be-named.md)
(an attachment may be named) and the `interfaces[]` passthrough that moves a real NIC into a
container's namespace.

## Context

An access point is a bridge between a radio and a wired segment. On this platform the two halves of
that bridge arrive from opposite directions and nothing joined them.

**hostapd owns the radio.** Given `bridge=br0` it puts the wireless interface into a bridge of that
name, creating the bridge if it does not exist. It knows nothing about the container's other
interfaces and will never enslave one — that is not a gap in hostapd, it is simply not its job.

**Cix owns the wired half.** It makes the veth pair, moves one end into the container's network
namespace, renames it and addresses it. It has never had any notion of what happens to that
interface afterwards.

So an AP container could be given a radio and a network attachment, and traffic would reach neither
across the other: hostapd's bridge held the radio alone, and the veth sat beside it, addressed and
unconnected. Wireless clients could talk to the AP and to nothing behind it.

The obvious workaround is a service inside the container that runs `ip link add br0` and
`ip link set eth1 master br0`. That needs iproute2 in the image, a shell to drive it, and a startup
ordering between that service and hostapd — three things this platform deliberately does not have.
It also puts network construction inside a container, which is precisely the boundary the API-First
Mandate draws: the daemon owns namespaces, cgroups, mounts and rtnetlink, and containers declare
what they want.

## Decision

**A network attachment may declare a bridge inside the container that it becomes a port of.**

```json
{"name": "access", "ip": "192.168.151.1", "container_bridge": "br0"}
```

At container creation, in the child that already holds the new network namespace, Cix creates the
named bridge and enslaves the veth to it. This happens **before any service starts**, so hostapd
finds an existing `br0` and adds the radio to it. That ordering is the whole design — it is what
makes the two halves meet without either side knowing about the other.

Three rules follow from what a bridge port is:

**The address goes on the bridge, not the port.** A bridge port with an address of its own does not
receive on it. An attachment that names a bridge assigns its address to the bridge instead, which
is both correct and what an operator means.

**Several attachments may name one bridge.** The first creates it, the rest join. In the child's
netns `EEXIST` can only mean an earlier attachment in the same loop already created it, so it is
success rather than tolerance; every other errno still fails the container.

**Creation-time only.** `POST /containers/{name}/networks` refuses `container_bridge` with a 400.
Building a bridge means acting inside the container's network namespace, and the live-attach path
has no primitive for that — the same reason `route_spec` has always been creation-time. The field is
refused explicitly rather than accepted and ignored, because a silently-dropped field is a lie the
operator finds out about later.

## Consequences

The primitives already existed — `rtnl_bridge_create()` and `rtnl_link_set_master()` are both
generic and both already used to build host bridges. This is a new arrangement of them inside a
namespace, not new networking code, which is why it is small.

It is not AP-specific. Any container that needs several segments joined at layer 2 — a transparent
filter, a soft switch, a router with a bridged pair — now declares it. That generality is a
by-product of describing the mechanism honestly rather than naming it `hostapd_bridge`.

**The AP still needs a second thing this does not provide:** the radio itself, which arrives through
`interfaces[]` (a real netdev moved into the namespace), not through a network attachment. The two
meet in the bridge and neither knows about the other. That is the intended shape.

## Alternatives considered

**Bridge on the host and pass the bridge into the container.** A bridge is not a movable netdev in
the sense a NIC is, and it would put the container's layer-2 topology outside the container while
hostapd's half stayed inside — the same split, moved.

**Have hostapd's `bridge=` name a bridge Cix built on the host.** hostapd would have to enslave an
interface across a namespace boundary, which it cannot do, and the radio has to be in the container
for hostapd to drive it at all.

**Run hostapd on the host.** It would work and it discards the container model for one service, with
no isolation, no declared image, and no restart policy — the platform would have a special case at
its centre for a workload that is not special.
