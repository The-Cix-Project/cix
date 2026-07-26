# 0011 — rtnetlink control plane over the kernel's own bridge/veth, not a userspace switch

## Status

Accepted

## Context

The mission calls for "a 100% custom virtual switching and routing data plane... no Open vSwitch or eBPF." That sentence supports two very different readings: (a) our own C code drives the kernel's native bridge/veth/routing directly via rtnetlink, with the kernel still doing the actual packet forwarding, or (b) we reimplement L2 switching itself in userspace (raw sockets/tap devices, our own MAC-learning table), bypassing the kernel bridge entirely. Confirmed directly with the user before writing any code, since guessing wrong here would have meant a large amount of wasted, high-risk work: (a).

Separately, while implementing and testing `netplane/src/rtnetlink.c`, a real design pitfall surfaced: the first draft assigned the container-facing "gateway" IP address to the host-side veth end (`vt-a`) directly. That's wrong once that same interface is also enslaved to a bridge (`rtnl_link_set_master`) — a bridge port stops behaving like a normal, independently-addressable host interface once attached; the IP belongs on the *bridge device itself*, with ports carrying no address of their own. This isn't obvious from the individual rtnetlink calls succeeding (`rtnl_addr_add_ipv4` on a bridge port returns success — the kernel doesn't reject it), only from thinking through the actual topology.

## Decision

"Custom" means the **control plane**: our own hand-built rtnetlink messages (`netplane/src/rtnetlink.c`) creating bridges/veth pairs, moving links between network namespaces, assigning addresses, and installing routes — never `ip`/iproute2 shelled out to, never an OVS daemon, never eBPF programs. The kernel's own bridge and routing table still do the actual forwarding; reimplementing that in userspace would mean building a slower, less-tested version of something the kernel already does well, which is not what "Bar-Raising" means here.

Topology convention going forward: an addressable "gateway" IP is assigned to the **bridge device**, never to a port that's enslaved to it. Bridge ports get brought up (`IFF_UP`) but stay unaddressed.

Links are addressed by **name** (`ifi_index = 0` + `IFLA_IFNAME`) for every operation on an existing link (set-master, set-up, set-netns, delete) rather than resolving a name to an ifindex first and addressing by index — one fewer round trip, and the kernel already supports this resolution natively for `RTM_SETLINK`/`RTM_NEWLINK`/`RTM_DELLINK`. (`if_nametoindex()`, an ordinary glibc call, is still used where an attribute specifically requires a numeric ifindex value, e.g. `IFLA_MASTER`.)

## Consequences

- Every future networking capability (Phase 6's container wiring, Phase 7's routing protocols) builds on this same rtnetlink-primitives layer and the same bridge/veth topology convention — a container's veth port never carries its own address; whatever bridge it's attached to does.
- We take on the burden of getting netlink wire-format details right ourselves (message/attribute alignment, the veth driver's specific embedded-`ifinfomsg`-inside-`VETH_INFO_PEER` nesting requirement) rather than depending on a battle-tested library like `libnl` — consistent with this project's existing "hand-roll it" pattern (custom HTTP/JSON in the daemon, ADR-0007), but a real, ongoing cost: this is exactly the kind of low-level protocol code most prone to subtle, hard-to-spot bugs (see ADR-0008, ADR-0009 for two other examples of that risk materializing elsewhere in this project). `test/test_rtnetlink.c` verifies real end-to-end TCP connectivity through the constructed topology, not just that individual syscalls returned success, specifically because of that risk.
