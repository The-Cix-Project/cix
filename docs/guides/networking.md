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

`cixctl network ls` / `network rm NAME` list and remove (refused if any container is still attached, or if it's the management network — see [The management network](#the-management-network)).

## Attaching real hardware

```sh
cixctl network attach-interface lan1 --interface=eth1
cixctl network attach-interface lan1 --interface=eth1 --vlan=100
```

Enslaves a real host interface into a network's bridge (ADR-0038) — the "physical ethernet on a host-managed switch" mechanism. The interface must be currently assignable (`cixctl device ls`'s own `net:` entries — not already inside a container's netns, not already attached elsewhere via this command). Without `--vlan=`, `eth1` itself is enslaved untagged. With `--vlan=100`, an 802.1q sub-interface `eth1.100` is created and enslaved instead, leaving bare `eth1` free to attach to a *different* network (with a different, or no, VLAN tag) at the same time — one physical NIC can back several isolated networks this way. `network detach-interface lan1 --interface=eth1` releases it — the bridge attachment or the VLAN sub-interface, whichever this command originally created.

This is a different mechanism from a NIC passed straight into one container's own network namespace at creation time (`container run --interface=IFNAME`, ADR-0022) — that's exclusive, dedicated hardware for one container; attaching to a network's bridge is shared L2 connectivity any number of containers on that network can use.

## Recovering a stuck NIC

```sh
cixctl network flap-interface eth0
```

Brings a host interface administratively **down and then back up** (`POST /system/interfaces/{name}/flap`), which renegotiates the carrier — the fix for a NIC that has a link but no connectivity, or a bridge port wedged in `blocking`. It is *flap-only*: there is deliberately no "set the interface down" on its own, because a down-only command is how you strand a shell-less box on the very interface you are trying to fix; a flap always ends with the interface up.

Run it **from the console** (which talks to `127.0.0.1` and is unaffected). If you run it against the off-box address that rides the interface being flapped, the reply may not come back even though the flap succeeded — the interface is momentarily down — so reconnect and re-check rather than treating a dropped reply as failure. Loopback (`lo`) is refused outright.

**First check whether a flap is even the right tool.** If the symptom is "eth0 exists but can't reach the LAN" *and* the driver loaded late at boot, the interface may simply never have been enslaved to the management bridge (its uplink attach failed and the boot continued — the address is on the bridge, the bridge has no port). `cixctl network ls` shows whether `eth0` is in the management network's interface list; if it is not, the fix is `cixctl network attach-interface <mgmt-net> --interface=eth0`, not a flap. Flap when the interface is attached and the link itself is stuck.

An interface enslaved to a bridge this way stays visible on the host but reports `assignable: false` — read from the kernel's own `master` symlink, not from a bookkeeping table Cix maintains. An interface *moved* into a container disappears from the listing entirely. Both are ground truth rather than record-keeping, which is why neither can drift. (`--device=` grants are not like this: the same USB, PCI or GPU device can currently be granted to several containers with nothing reporting it — issue #356.)

See [Giving a container a radio](#giving-a-container-a-radio) below for the wireless case, which is a third thing again.

## Giving a container a radio

```sh
cixctl container run --name=ar-1 --image=wifi_router --interface=wlan0 \
    --network=access:192.168.151.110 \
    --service="hostapd=/usr/bin/hostapd /etc/hostapd/hostapd.conf"
```

A wireless adapter is passed as an **interface**, never as a device — and that catches people out, because a USB dongle looks like a USB device.

By the time a container exists, the adapter's USB endpoint has already been claimed by its driver on the host (`rtw88_8822bu`, say) and turned into `wlan0`. Granting the container the raw USB device with `--device=usb:...` would hand it something the host driver already owns, and `hostapd` would still have nothing to drive: what it wants is the interface. So `--interface=wlan0` is the whole grant, and no device passthrough is involved.

**A radio moves differently from a network card, and Cix handles that for you.** A wireless netdev belongs to a *wiphy* — one radio, which can own several interfaces — and the kernel refuses to let one of them leave the namespace alone (`EINVAL`). Cix detects a wireless interface over netlink and moves the entire PHY instead, with everything on it. See [ADR-0265](../adr/0265-a-radio-moves-as-a-phy-not-as-an-interface.md) for why, and issue #341 for what it cost to find out.

**One container owns the radio, exclusively, for as long as it exists.** This is not a Cix policy — it is what moving a PHY into a namespace means:

- while the container runs, `cixctl device ls` shows no `net:wlan0` on the host, and the host cannot use that radio for anything
- a second container asking for it is refused with `unknown or unassignable interface`, because the ordinary lookup finds nothing to give
- deleting the container moves the PHY back, and the host has it again in about a second
- if you want both an AP and a client interface on one adapter, both must live in the *same* container — a wiphy cannot be split across namespaces

**Driver loading is separate and explicit.** A radio driver is a kernel module (see [administration.md](administration.md#kernel-modules)), so the interface only exists once the module is loaded. Mark it to load on every boot rather than loading it by hand:

```sh
cixctl kmod-config set rtw88_8822bu --autoload
```

The driver is a module on purpose: built into the kernel, it probes the USB device at an unpredictable point relative to the root mount and can lose the race to its own firmware (issue #346).

**Bridging the radio to a wired segment** needs `container_bridge`, because the two halves arrive from opposite directions: `hostapd` puts the radio into whatever bridge its `bridge=` names and will create that bridge itself, but it knows nothing about the container's other interfaces and will never enslave one. Cix builds the bridge first with the wired attachment already in it, and `hostapd` then adds the radio to the bridge it finds. See [ADR-0264](../adr/0264-a-container-may-bridge-its-own-interfaces.md).

`container_bridge` is a field of a network attachment in the `POST /v1/containers` body and has no `cixctl container run` flag, so a bridged access point is created from a deployment (see [`containers-and-services.md`](containers-and-services.md#deployments)) or a direct API call. The attachment from the `ar-1` deployment:

```json
"interfaces": ["wlan0"],
"networks": [
  {"name": "access", "ip": "192.168.151.110", "ifname": "access", "container_bridge": "br0"}
]
```

The address goes on the bridge `br0` inside the container, never on the port. The field contract is in [`docs/api/README.md`](../api/README.md#creating-a-container).

## The management network

`cixd` always listens on `127.0.0.1`, plus **one off-box address**: the management address ([ADR-0287](../adr/0287-the-management-address-is-the-single-truth.md)). Install time creates an ordinary network from the interface and prefix given to `cix-install` and sets the management address into it (see [`installing.md`](installing.md)). From then on it is a network like any other in `GET /networks`.

Which network is "the management network" is **derived, not stored**: a network's `management` field is `true` when its subnet contains the current management address. While it is `true`, deleting that network or detaching one of its interfaces is refused with `409`, because either would cut the connection you manage the box through. There is no force flag.

```sh
cixctl management-address show
cixctl management-address set 172.31.0.5
cixctl management-address reset
```

`set` takes an address, never a network name. The network is whichever existing network's subnet contains the address, so create that network and attach its interface first; an address in no network's subnet is refused with `400`. The change applies live and then persists: the new listeners are bound before the old ones are torn down, and the superseded address is removed from its bridge a couple of seconds later so the reply to this call still gets out. `reset` drops the box to loopback-only, which is also how you free the management network for deletion.

Since `cixd` runs as PID 1 on an installed system, check that the new address is reachable before relying on it. The physical console always works, because `127.0.0.1` is always bound. Ports and HTTP/HTTPS exposure are a separate resource, `cixctl daemon-config`. See [`docs/api/README.md`](../api/README.md#the-management-address-and-cixds-own-listeners) for both contracts.

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
cixctl container run --name=router --image=frr --network=internal --network=dmz --ip-forward --service=main=/usr/sbin/some-router-daemon
```

Enough for a container running a real dynamic routing protocol (BIRD, FRR) to do the routing itself, or for pure static routing. Other containers then need their own static route pointing at the router's address to actually reach the far side:

```sh
cixctl container run --name=internal-host --image=base --network=internal --route=172.32.0.0/24:172.31.0.2 --service=main=/usr/bin/some-binary
```

Up to 8 routes, set once at container creation — not modifiable on an already-running container. `--ip-forward` is per-netns and defaults off; it never affects the host or any other container. That's a different, container-scoped mechanism from the host's own `net.ipv4.ip_forward` — see [`cixctl sysctl`](administration.md#host-sysctl-tuning) to tune the host's own kernel directly.
