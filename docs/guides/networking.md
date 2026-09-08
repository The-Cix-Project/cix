# Networking

Networks, physical/VLAN interface attachment, the host's own kernel routing table, and container-to-container routing — task-oriented walkthroughs of capability that's fully documented endpoint-by-endpoint in [`docs/api/README.md`](../api/README.md), gathered here into one place. For exact CLI flag syntax, see [`cli-reference.md`](cli-reference.md).

## Creating a network

```sh
cixctl network create --name=lan1 --subnet=172.31.0.0 --prefix=24 --address=172.31.0.1
```

Creates a real Linux bridge immediately (via rtnetlink) and persists the definition — it survives a daemon restart or reboot, unlike a container. `name` becomes the bridge's own interface name, so it's limited to 15 characters. `subnet` must be the exact network address for the given `prefix` (host bits zero).

`--address=` is optional and changes what kind of network you get:

- **Given** — the bridge itself owns that address, and it's assigned as the automatic default route for every container's *primary* attachment to this network. The normal case for a network the host itself routes for.
- **Omitted** — pure L2, no host-owned address at all. For a network whose routing is owned by whatever's attached to it instead (a router-pair container running a routing protocol, a shared VRRP address) — containers on it get no default route from this network.

`cixctl network ls` / `network rm NAME` list and remove (refused if any container is still attached, or if it's the management network — see below).

## Attaching real hardware

```sh
cixctl network attach-interface lan1 --interface=eth1
cixctl network attach-interface lan1 --interface=eth1 --vlan=100
```

Enslaves a real host interface into a network's bridge (ADR-0038) — the "physical ethernet on a host-managed switch" mechanism. The interface must be currently assignable (`cixctl device ls`'s own `net:` entries — not already inside a container's netns, not already attached elsewhere via this command). Without `--vlan=`, `eth1` itself is enslaved untagged. With `--vlan=100`, an 802.1q sub-interface `eth1.100` is created and enslaved instead, leaving bare `eth1` free to attach to a *different* network (with a different, or no, VLAN tag) at the same time — one physical NIC can back several isolated networks this way. `network detach-interface lan1 --interface=eth1` releases it — the bridge attachment or the VLAN sub-interface, whichever this command originally created.

This is a different mechanism from a NIC passed straight into one container's own network namespace at creation time (`run --interface=IFNAME`, ADR-0022) — that's exclusive, dedicated hardware for one container; attaching to a network's bridge is shared L2 connectivity any number of containers on that network can use.

## The management network

Exactly one network is `is_management` at a time — the one `cixd` itself binds to. It's bootstrapped automatically at install time from the physical interface and address given to `cix-install` (see [`installing.md`](installing.md)), but from then on it's an ordinary, API-visible network like any other (`GET /networks`), not a special case hidden from the API.

Because deleting or detaching the management network's own interface would sever the connection you're managing the box through, both are unconditionally refused (`409`) while `is_management` is set — deliberately, with no override flag. To move it:

```sh
cixctl daemon-config set --management-network=lan1
```

`lan1` must already have its own address (create it with `--address=`, or attach a physical interface to it first). This performs a live listen-socket rebind — the new socket is created and bound *before* the old one is torn down, so a failure leaves the previous listener intact rather than dropping connectivity. Since `cixd` runs as real PID 1 on an installed system, double-check reachability of the new network before relying on it — physical console access is the only fallback if it's wrong. See [`docs/api/README.md`](../api/README.md#the-management-network-and-cixds-own-listeners) for the full daemon-config contract (port, HTTP/HTTPS, a dedicated `bind_ip` decoupled from the management network's own address).

## The host's own kernel routing table

```sh
cixctl routes
cixctl routes add --dest=172.40.0.0 --prefix=24 --gateway=172.30.1.1
cixctl routes rm --dest=172.40.0.0 --prefix=24
```

A real, live `RTM_GETROUTE` dump plus thin add/remove wrappers (ADR-0066/ADR-0067) — this exists specifically because a real installed box has no SSH and no general shell, so it's otherwise impossible to confirm what the kernel actually did with a route. Nothing here is a persisted Cix resource: a route added this way is gone on the next reboot unless something else (a network's own address, a container's `ip_forward`) re-applies it every boot. `--default --gateway=` targets the default route specifically, the same convention the underlying route-add primitive itself uses.

## Container-to-container routing

A container attached to two networks with IP forwarding on will actually forward packets between them:

```sh
cixctl container run --name=router --image=frr --network=internal --network=dmz --ip-forward--service=main=/usr/sbin/some-router-daemon
```

Enough for a container running a real dynamic routing protocol (BIRD, FRR) to do the routing itself, or for pure static routing. Other containers then need their own static route pointing at the router's address to actually reach the far side:

```sh
cixctl container run --name=internal-host --image=base --network=internal --route=172.32.0.0/24:172.31.0.2--service=main=/usr/bin/some-binary
```

Up to 8 routes, set once at container creation — not modifiable on an already-running container. `--ip-forward` is per-netns and defaults off; it never affects the host or any other container. That's a different, container-scoped mechanism from the host's own `net.ipv4.ip_forward` — see [`cixctl sysctl`](administration.md#host-sysctl-tuning) to tune the host's own kernel directly.
