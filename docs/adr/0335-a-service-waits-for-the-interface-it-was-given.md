# 0335 — A service waits for the interface it was given

## Status

Proposed, 2026-10-09. Issue [#344](https://git.home.arpa/itdlabs/cix/issues/344). Applies [ADR-0247](0247-the-reactor-does-not-block-and-that-is-the-defence.md) and [ADR-0278](0278-the-reactor-forks-work-it-cannot-afford-to-wait-for.md) to the interface-attach path, and extends what a container declares under [ADR-0260](0260-a-container-declares-services-not-a-command.md).

## Context

**Creating a container that carries a radio blocks the whole control plane for 1.66 seconds.** Measured on 192.168.15.95 under `0.2.57-475`, with per-step timing on the path:

```
container ar-1: interface attach 1676 ms total -- classify 0, down 0, move 16, bring-up 1660
                                  (of which setns 0, rtnl_open 0, set_up 1658)
```

One netlink call — `rtnl_link_set_up()` on the moved radio — is 1658 of 1676 ms. `NL80211_CMD_SET_WIPHY_NETNS`, which moves an entire PHY and every interface on it and which #344 named as a prime suspect, costs 16 ms. The `setns()` and the `rtnl_open()` are 0. Stallwatch had reported a stable ~1.72 s worst pass per `ar-1` apply across seven creations, and the remainder is the rest of the create in that same pass.

**The work is already forked. What blocks is the wait.** `container_net_host_attach_interfaces()` forks a short-lived helper because the bring-up must happen from inside the container's netns and a netlink socket can only address its own namespace — then `waitpid()`s for it, in the request handler, on the single-threaded reactor. That is the shape ADR-0247 exists to forbid and the shape that froze this control plane for 366 seconds in #399. ADR-0278's remedy is "fork what you cannot afford to wait for", and here the fork is already there; only the wait needs removing.

**Why removing just the wait is not enough.** `ar-1` declares `on_exit: "fail-container"` for `hostapd`, which is correct for a service whose whole purpose is the radio. If the create returns while the interface is still down, `hostapd` starts on a down `wlan0`, dies, the container fails, [ADR-0323](0323-every-package-can-roll-discovery-authentication-and-a-green-build.md)'s rollback marks that image version bad, and the access point — proven live with a real associated client under #30 — goes down on every reboot. Trading a 1.7 s stall for that is a regression, and a design whose failure has to "surface some other way" is a stop-gap by the No Stop-Gaps maxim's own definition.

**Three things are not available, and each is ruled out by an existing decision rather than by taste.**

- **Bringing the interface up from inside the container's own userspace.** That needs `iproute2` in the image and a shell to drive it. `src/container_net.c`'s own comment records this platform as deliberately having neither, and under [ADR-0333](0333-an-images-content-comes-only-from-packages.md) anything in an image is a package — so this would mean a package in every image that carries an interface.
- **Returning before the child execs.** [#549](https://git.home.arpa/itdlabs/cix/issues/549) made `container_create()` wait for the exec precisely so that "created" means the first process is running in its own root; two real races were fixed by it. Moving the bring-up into the child would simply relocate the 1.66 s into that wait.
- **Making the whole create asynchronous.** It reverses #549 for every container, not only the ones carrying an interface, to fix a cost only radios pay.

## Decision

**A service may not start before the interfaces its container was given are up, and the daemon stops waiting for them.**

1. **The daemon reaps the bring-up helper by its pidfd** instead of `waitpid()`ing for it, which is the convention every other child the daemon tracks already uses. `container_create()` returns after the move — 16 ms of netlink — and the reactor serves other requests while the radio comes up.

2. **`cix-init` holds a service until its container's interfaces have `IFF_UP`**, before `execve()`. It reads `/sys/class/net/<name>/flags` and waits for bit 0, bounded by **5000 ms** — the same ceiling `CONTAINER_EXEC_WAIT_MS` already uses for the child's exec, and three times the 1658 ms measured above. On timeout it starts the service anyway and logs that it did: a radio that never comes up is a hardware fault, and refusing to start is a worse answer than starting and letting `on_exit` decide.

3. **`flags`, not `operstate` — and that distinction is the difference between working and silently not working.** Measured on 192.168.15.95, 2026-10-09, from inside a container whose `lo` had just been brought up by `container_net_child_loopback_up()`, which calls the same `rtnl_link_set_up()` this decision waits on:

   ```
   /sys/class/net/lo/flags      0x9      IFF_UP (0x1) | IFF_LOOPBACK (0x8)
   /sys/class/net/lo/operstate  unknown
   ```

   An interface that is genuinely up reports `operstate` as `unknown`, not `up` — the kernel writes `up` there only for a driver that reports carrier, and a wireless interface that is administratively up but not yet associated reports `dormant`. An earlier draft of this decision waited for `operstate` to leave `down`; `unknown` satisfies that before *and* after the bring-up, so `cix-init` would never have waited at all and the race would have survived with the mechanism appearing to be in place. `IFF_UP` is set by `rtnl_link_set_up()` and by nothing else on this path, which makes it the one signal that answers the question being asked.

4. **The check is sysfs, not netlink, and that is a measurement rather than a preference.** `mountns_pivot()` mounts a *fresh* sysfs inside the container, deliberately after the netns is created — its own comment explains that a carried-in submount stays pinned to the netns it was set up in and "would keep showing devices from the wrong namespace". So the container's `/sys/class/net` reports the container's own netns, which is exactly the question being asked. And `cix-init` needs **no new syscall**: it already issues `openat`, `read` and `close`, and it already uses `poll` with zero file descriptors as a sleep (`init/src/cix_init.c:1558`, a 250 ms tick when there is no control fd), which is what bounds the retry. It has no `nanosleep` and does not need one. No netlink, no libc, nothing added to any image.

5. **The daemon derives the list; the recipe does not have to declare it.** A container given an interface is a container that wants it, so every service waits for every interface the container carries. Waiting for an interface a particular service does not need costs that service some milliseconds once; requiring each recipe to repeat what the container spec already says would be a second place to state one fact.

6. **The interface list travels in the spec `cix-init` already receives**, not over the control socket. The socket carries operations on a running container (`CIXINIT_OP_SHUTDOWN`); this is part of what the container *is*, known before PID 1 starts, and a value known at creation does not belong in a runtime message.

7. **Nothing changes for a container with no interfaces**, which is every container on this host but one, and every build container. No list, no poll, no new code path reached.

## Consequences

- **The control plane stops stalling on a radio.** The reactor is held for the 16 ms move instead of 1.66 s. The radio still takes 1.66 s to come up — no design makes hardware faster — but nobody waits on the reactor for it.
- **The race the simple fix would have introduced is impossible rather than unlikely.** A service cannot observe a down interface it was given, because it has not been exec'd yet.
- **"Created" keeps meaning what #549 made it mean.** The child's exec is still waited for; only the helper's completion is not.
- **A new failure mode, deliberately chosen:** an interface that never comes up now delays its services by the timeout instead of failing the create. That is the right trade — the old behaviour failed the create for a hardware fault, which an operator can do nothing about from the API — but it is a behaviour change and the timeout's value is a judgement, so it is logged when it fires rather than passing silently.
- **`cix-init` grows a reason to read the filesystem**, which it has not needed before. It is a bounded poll of one known path, with no parsing beyond a string compare, and it uses only syscalls already in its trampoline — but it is new surface in the one binary that must not depend on the image's userspace, and that is the cost being accepted.
- **The attach's own failure is no longer the create's failure.** A bring-up that fails after the create returned has to be reported, not swallowed: the helper's exit status is read when the pidfd fires and goes to the log store with the container's name, the same way its timings already do.

## Alternatives rejected

- **Reap the helper by pidfd and nothing else.** The whole of the benefit and none of the safety: it frees the reactor and lets `hostapd` race a down `wlan0`, which for `ar-1`'s `on_exit: "fail-container"` means the AP fails on every reboot. Rejected as a regression, not as a lesser option.
- **Keep the blocking wait.** Defensible, because the stall only costs other callers during an operator-initiated apply, and it is what ships today. Rejected because ADR-0247 is not a preference, and because a 1.7 s freeze is exactly the "software engineery, less actual product" quality the owner has already called out once.
- **A synthetic gate service the daemon injects, with `hostapd` `--after` it.** Uses `cix-init`'s existing `--after` vocabulary with no new concept — but a service that runs no program is a new concept of its own, and the ordering graph would then contain an entry no recipe wrote and no operator can see in `container ls`. Making the wait a property of each service is the same behaviour without inventing a ghost.
- **`--ready=NAME:interface:IFNAME` as a new readiness kind.** Readiness in ADR-0260 means "this service is now usable by its dependents", which an interface being up does not say about a service. Overloading it would make `--ready` mean two things.
- **Polling from the daemon and starting services over the control socket.** Keeps `cix-init` ignorant of interfaces, at the price of a round trip per service and a daemon that must know when each one may start — which moves the service graph's semantics out of the one process that owns it, the parallel implementation ADR-0260 exists to prevent.
- **Shortening the 1.66 s itself.** Not rejected, but not available: whether `rtnl_link_set_up()` blocks in the driver's `ndo_open` or in the kernel still settling the PHY migration is **not established**. Both predict a slow call, a second `set_up` is a no-op under either, and the experiments that would separate them need the radio out of `ar-1` and the access point down. It changes nothing here — under both mechanisms the cost has to leave the reactor — and if it later proves to be the settle, this decision stands and the wait simply gets shorter.
