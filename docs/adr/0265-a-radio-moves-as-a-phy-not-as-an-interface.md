# 0265 — A radio moves as a PHY, not as an interface

## Status

Accepted

Issue [#341](https://git.home.arpa/itdlabs/cix/issues/341). Makes [ADR-0264](0264-a-container-may-bridge-its-own-interfaces.md)
usable for its own motivating case, and extends the `interfaces[]` passthrough that ADR-0022
introduced.

## Context

An access point needs the radio inside the container, because hostapd drives it. ADR-0264 built the
bridge that lets that radio meet a wired segment. Both assumed the radio could get there, and it
could not.

`container_net_host_attach_interfaces()` moves a named interface into the new namespace with
`rtnl_link_set_netns_pid()`. For `wlan0` the kernel returns **EINVAL**, and container creation fails
with `container_net_host_attach_interfaces: Invalid argument`.

The call is not broken. `test_container_net` moves a veth through that exact path and passes, and
that file's own comment records the call as *"fully generic (confirmed via kernel source, not
veth-specific)"*. The failure is specific to wireless.

**A wireless netdev is not the thing that owns a namespace.** It belongs to a *wiphy*, and one wiphy
can own several netdevs — an AP interface, a monitor interface, a station interface, all on one
radio. The kernel will not let one of them leave on its own, because the hardware underneath cannot
be in two namespaces at once. The whole PHY moves or nothing does.

The operation that moves it is `NL80211_CMD_SET_WIPHY_NETNS` — what `iw phy <phy> set netns <pid>`
issues. That command lives in the **nl80211** family, which is *generic* netlink: a different
protocol (`NETLINK_GENERIC`), with a family id that is not a compile-time constant and must be
resolved at runtime by asking the controller for it by name. Everything else this platform says to
the kernel is rtnetlink, where the message types are fixed numbers.

## Decision

**`interfaces[]` keeps its meaning — "put this interface in the container" — and the platform picks
the mechanism the kernel requires for it.**

A recipe still says `"interfaces": ["wlan0"]`. Nothing about the container contract changes, and
nothing asks an operator to know that a radio is special. `container_net_host_attach_interfaces()`
tests whether the name is backed by a wiphy and takes the nl80211 path if so, the rtnetlink path
otherwise. Teardown mirrors it.

Three implementation choices worth recording:

**"Is this wireless?" is answered from sysfs**, not over netlink: `/sys/class/net/<name>/phy80211`.
The caller must decide which mechanism to use *before* opening a socket for either, and a missing
directory is a cheaper and less ambiguous answer than a netlink round trip that can fail for
unrelated reasons. The wiphy index comes from the same place — the kernel publishes it as a file,
so resolving it over nl80211 would mean a whole response-parsing path existing for one integer.

**The message-building helpers are now shared**, not copied. They were file-static in
`rtnetlink.c`, which was right while rtnetlink was the only family; they moved to
`netplane/src/nlmsg.h` when a second family appeared. They are `static` and deliberately not
`inline`: TCC emits a bare `inline` definition as a strong global in every translation unit, so two
includers collide at link with "defined twice" — the failure the m4 build found. One copy per
includer is the price, and for functions this small it is not worth engineering around.

**Both directions exist as separate calls**, by pid and by namespace fd, because the two directions
genuinely differ. Going in, the daemon has the container's pid and no fd for its namespace yet.
Coming back out, the teardown helper is already inside the container and holds an fd for the
host's — with no pid it could name instead without assuming something about its own parent.

## Consequences

**The host loses the radio entirely while the container exists**, along with every other interface
on that wiphy. For a dedicated access-point adapter that is exactly what is wanted. For a radio the
host is also using, it is not — and this platform offers no way to share one, because the kernel
does not. An operator who moves their only wireless card into a container has moved their only
wireless card into a container.

**Generic netlink is now something this platform speaks**, which it did not before. That is a real
capability rather than a workaround: `nl80211` is the door to everything else wireless — channel
survey, regulatory hints, station lists — and none of it was reachable through rtnetlink at any
price. Nothing beyond the namespace move is implemented, and nothing should be until something
needs it.

**What is not verifiable off the target.** The move itself needs a real wiphy and real privileges,
so neither this dev sandbox nor a build container can exercise it; `nl80211_is_wireless()` is
testable anywhere and is tested, and the move is verified on the box. That split is stated rather
than papered over — a test that mocked a wiphy would assert that the mock behaves, which is the one
thing already known.

## Alternatives considered

**Run hostapd on the host.** Discards the container model for one service — no isolation, no
declared image, no restart policy — and puts a permanent special case at the centre of a platform
whose whole point is that workloads are containers.

**Leave the radio on the host and bridge there.** hostapd has to be where the radio is, so this
relocates the problem rather than solving it, and splits the AP across the boundary ADR-0264 exists
to close.

**Shell out to `iw`.** It would work, and it would mean iproute2-class tooling in the host root and
a subprocess in the network path — against the standing rule that this platform talks to the kernel
through netlink directly, never through another project's CLI.
