# 0385 — A roll group restarts one member at a time

## Status

Accepted by the owner on 2026-11-20, choosing *"Declared roll groups"* over serialising every container that follows one image, and over leaving it.

## Context

The daemon restarts containers by itself for two reasons:
- an image they follow rolls (ADR-0124, ADR-0323);
- a DHCP range change, which dnsmasq reads only at start (ADR-0197).

Both arm `arm_rolling_restart_timer()` with a random delay per container (`rolling_jitter_seconds()`). Each draws its delay on its own, so nothing keeps two containers of a redundant pair from restarting together.

Measured on 192.168.15.95, 2026-11-19, when the router image gained dnsmasq: cr-1 and cr-2 restarted at 19:48:5x and 19:49:08 UTC. They were ten seconds apart by luck, and VRRP held the gateways. dns-1 and dns-2, which serve DNS and DHCP to every network, restarted together several times that day for range changes.

## Decision

**A container may declare `roll_group`. Members of one group never restart together for a restart the daemon chooses: when a member's restart is due, it waits until every other member is running and ready.**

1. **Declared, not derived.** `roll_group` is a container definition field (`^[A-Za-z0-9_-]{1,63}$`). Containers that share a value are a group: on .95, `cores` for cr-1 and cr-2, `dns` for dns-1 and dns-2. Containers that merely share an image are not grouped by it.
2. **One gate, where every daemon-chosen restart passes.** Both causes above arm `arm_rolling_restart_timer()`, and its handler stops and replays the container. That handler asks first. While another member is not running and ready, or is stopping, the restart is re-armed a few seconds later instead, and the log says once which peer it is waiting for. Timers fire one at a time on the event loop, and a stop marks its container as tearing down at once, so two members cannot both pass.
3. **A member that is down holds the group.** A peer that crashed, is crash-looping, or failed its roll is not ready, so the healthy member is never taken down after it. A rolled member that never becomes ready goes back to its previous image after its probation (ADR-0323), becomes ready, and releases the group; the version it failed on is marked bad, so the next member is not rolled onto it.
4. **What does not count as a peer:** a member an operator stopped (`stopped`), and one whose restart policy is `no` and that is not running. Neither will come back by itself.
5. **Operator actions are not gated.** A restart, stop or delete asked for through the API happens at once; the group governs only what the daemon decides by itself. A crash restart is not gated either, since there is nothing to wait for: the container is already down.

## Alternatives

- **Every follower of one image, one at a time.** No setting, but it also serialises unrelated containers that happen to share an image, and it misses a pair on different images. Not chosen by the owner.
- **A queue of pending restarts.** Exact ordering, but it is state that a handover must carry, and it duplicates what the registry already knows. The re-armed timer asks the registry instead.

## Consequences

- A group of two rolls in about twice the time of one, and a broken member holds its peers until it recovers. That is the point.
- The field is refused by an older cixd (unknown key), so deployments name it only after the release that adds it, and adding it means recreating the container. Do that one member at a time.
- A rolling-restart timer lost to an in-place switch is lost whether grouped or not, as before this ADR; an image roll is re-derived after the switch, a DHCP-driven one is not.

## Live checks

- Roll the image a group follows: the second member's restart waits until the first is ready, and the log says so.
- A DHCP range change restarts dns-1 and dns-2 one after the other.
