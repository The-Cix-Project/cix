# 0346 — A restart that cannot be made is a failure, and is retried

## Status

Accepted by the owner, 2026-10-21. It came out of #623: what happens to `ar-1` when its radio is unplugged. It amends [ADR-0027](0027-restart-policy-expansion-and-backoff.md) (backoff covers a refused restart, not only a crash) and follows [ADR-0270](0270-a-deployment-waits-for-its-image-rather-than-being-refused.md)'s shape for saying why a defined container is not running.

## Context

Measured on 192.168.15.95 at 0.2.57-563, or read from the code at `f6a00110`. Each item says which.

**A refused automatic restart is permanent** (read from the code). `handle_restart_timer_event()` recreates a crashed container from its definition. When `create_container_from_body()` refuses, it writes the reason to stderr and returns. Nothing re-arms the timer, except for a rolled image, which ADR-0323 rolls back. `containerdef_autostart_all()` does the same at boot.

**And silent** (measured). A scratch container held the USB tablet through a mapping, with `restart: always` and a service that exits after five seconds. With the mapping deleted, its restart was refused. Thirty seconds later:
- `container ls` and `GET /v1/containers/restartprobe` both read `stopped`, with `stopped=no`, so nobody had stopped it.
- `GET /v1/system/logs` had no line about a restart or a refusal; stderr reaches no log on a shell-less host (#132).
- With the mapping recreated and the tablet present, it stayed down twenty seconds later, with nothing retrying.

**What that does to `ar-1`** (read from its recipe). `ar-1` is `restart: always`, its hostapd service is `on_exit: fail-container`, and it names its radio by the `radio-ap` mapping. If the radio is unplugged and hostapd exits, the restart is refused because `radio-ap` matches no interface. The AP then stays down for good, even after the radio returns, and nothing says why. A box that boots with the radio unplugged ends the same way, through autostart. Whether hostapd does exit when its interface disappears is not measured, because nothing on .95 can be unplugged. Either way, the restart path is wrong for every refusal, not only a radio's: an image deleted, a network gone, an address taken.

**A retry multiplies whatever a refusal leaves behind** (read from the code at `331533ef`). A refused create runs once today; retried, it would run every 30 seconds indefinitely. So what a refused `create_container_from_body()` has allocated or written by the time it refuses decides whether retrying is safe:
- An address is never reserved. `network_alloc_ip()` derives a free one from the registry on each call, so a refusal leaks none.
- An absent device or interface mapping, an unknown network and a taken address (`"ip is held by container …"`) are all refused **before** anything is written: before the allocation, the container base directory and the rootfs snapshot.
- A missing volume (`"no such volume"`) and a missing LDAP group are refused **after** the rootfs snapshot. Each attempt sets the previous snapshot aside and takes a new one. On btrfs that is two ioctls; where btrfs is absent it is a full copy of the image (ADR-0341). Retried, that would be repeated every 30 seconds.

**Another mechanism already waits for one cause** (read from the code). ADR-0270 sets `awaiting_image` when a deployment is applied against an image that cannot run, queues the build, and creates the container when it lands. A crash restart whose image can no longer run does not reach that wait today. ADR-0345's give-back of unplugged devices serves running containers only, so a stopped one has no event to wait on.

## Decision

**A restart that cannot be made is a failure like a crash. It goes to the wait that already exists for its cause, and otherwise is retried on ADR-0027's backoff until it succeeds or someone decides otherwise.**

1. **One wait per cause.** A refusal because the image cannot run (`deployment_image_can_run()`) joins ADR-0270's wait: `awaiting_image` is set, the build is queued, and it is not retried on the timer. The image arriving is the event, as it is for a deployment. Every other refusal is retried here: a device or mapping absent, a network gone, an address taken, a volume deleted. No event exists for those.
2. **A refused create leaves nothing behind, and refuses as early as its cause allows.** The volume and LDAP-group checks move ahead of the rootfs snapshot and the container base directory, beside the other references a body makes. They depend only on the body and on registries the create already reads, so this changes no outcome and only moves the refusal earlier. After that, every refusal this ADR retries is stateless.

3. **One remedy, two entry points.** The failure branch of `handle_restart_timer_event()` and the failure branch of `containerdef_autostart_all()` both call one function. It records why the restart was refused, says so, and arms the retry. A rolled image keeps its own branch (ADR-0323 answer 4: roll back, re-arm at once).
4. **The retry is ADR-0027's backoff, computed by the one function that computes it.** `consecutive_failures` increments and the delay doubles from the definition's `restart_delay_seconds` up to the 30-second cap. That calculation is inline in `handle_container_event()` today, and moves into a function both paths call; a second copy would be a parallel implementation, and the first-failure doubling is exactly what a copy gets wrong. A refusal has no uptime, so it climbs to the cap and stays there. So once what a container needs returns, it is running again within about 30 seconds. For `ar-1` that holds if hostapd exits when its radio goes, which is not measured.
   - **The cap is ADR-0027's 30 seconds, and that is a decision, not an oversight.** It was chosen for a crash-loop, which the container controls. For a refusal it is right for a returning radio and eager for a typo. A second cap, or a knob, would be a second answer to "how often is a failing container retried", which ADR-0027 already owns. After rule 2, an attempt costs only the references a body makes, with nothing written.
   - **Which policies retry:** every policy whose crash already arms the timer, because the timer is policy-blind once armed: `always`, `on-failure` after a non-zero exit, and `unless-stopped`. Boot autostart already skips a `restart: "no"` definition (ADR-0181), so `"no"` never reaches the retry.
5. **What stops it is what already stops a crash-loop:** `stop`, which sets the definition's `stopped` flag that the timer handler checks unconditionally, and `DELETE`, which removes the definition. A new guard joins them: **a timer that fires for a name already running does nothing.** An operator who re-created the container during the wait would otherwise be refused "already exists" every 30 seconds for ever.
6. **It is said once, and shown always.**
   - The log gets an error line when a refusal first appears or its reason changes, and an info line on the first successful restart after a refusal: "restarted after its restart was refused". It gives no count, because `consecutive_failures` counts crashes too, and a number would be wrong whenever a refusal followed a crash-loop. A definition retried every 30 seconds would otherwise write 2,880 lines a day.
   - The container's record says why it is not running, in a new field `restart_refused`: the sentence the create was refused with, `""` otherwise. As with ADR-0270's `awaiting_image`, its status stays `stopped`, and like `awaiting_image` the field is optional in the contract. Like `awaiting_image`, only the stopped-definition writer emits it, since a running record is never refused. (This sentence said both writers did until implementation; `registry_write_json_one()` carries neither field.) A `stop` clears it, since the retries it describes have ended. It is set through a setter that announces it (ADR-0339) **only when the sentence changes**: a retry refused for the same reason announces nothing, or the dashboard would refetch every 30 seconds for each such definition. `container ls` and the dashboard show it beside `awaiting_image`.
   - Like `consecutive_failures`, it is not persisted, and `containerdef_add()` clears it on every redefine, as it already clears `stopped` and `consecutive_failures`, so a re-created container does not keep a stale reason. After a daemon restart, boot autostart tries again and sets it again if the restart is still refused.

## Verification

1. **In the gate.** `test_container_restart` is in `DAEMON_SELFTESTS`. Two `restart: always` definitions each have a one-second service, an explicit IP, and `restart_delay_seconds` of 5, so the first delay is 10 seconds (ADR-0027 doubles on the first failure) and the window is deliberate rather than a race. During it a second container takes each one's IP, and the restart is refused with `ip is held by container "<squatter>"`, the sentence `create_container_from_body()` writes for that collision. The test asserts:
   - each definition's `restart_refused` names the squatter;
   - after the second attempt, 20 seconds after the first, the log carries the first definition's refusal exactly once;
   - once its squatter is deleted, it is running again within 45 seconds (the 30-second cap plus margin), and the log says it restarted after its restart was refused;
   - the second definition, stopped while refused and its squatter deleted at once, reads `restart_refused` as empty after the stop and has not restarted when the first has.

   **What the gate does not show is rule 2**, because an address collision is a validation refusal and returns before any write. The moved existence checks are verified by reading: `body_reference_gone()` runs before `persist_mkdir_p(container_base)` and the rootfs snapshot, and checks only `volume_find()` and `ldap_group_find()` against the body.
2. **Live, on .95.** The tablet case above is repeated: delete the mapping, let the service exit, then read the refusal in the log and in `restart_refused`. Recreate the mapping, and the container is running within 30 seconds.

## Consequences

- A container that cannot be restarted keeps trying, and says why. That covers a radio that returns, a network recreated, a volume restored and an address freed. An image that cannot run waits for its build instead, as a deployment already does.
- **A refused boot autostart comes back as a crash-restart does: without its dependency wait.** The timer handler never acts on `depends_on` or readiness (its own comment says so). The 30-second cap usually gives a dependency time to come up, but nothing guarantees it.
- **`stop` ends the retries until the next reboot, for `restart: always`.** ADR-0027 honours `stopped` at boot only for `unless-stopped`, so a stopped `always` definition is autostarted again at boot, and if still refused, it retries again. This is ADR-0027's meaning of `always`, unchanged. The lasting way out is `DELETE`, or `unless-stopped`.
- A definition with a permanent mistake, such as a volume that was deleted or an address another container now owns, retries every 30 seconds until it is stopped or deleted. The log names it once, and the record names it for as long as it lasts. That is the intended trade: refusing to try again cannot tell a mistake from a device that is coming back.
- **Not decided here: a radio unplugged from a container that keeps running.** If hostapd survives the loss of its interface, the container stays up, its grant record keeps listing the radio as held, and a returning radio is not moved back in, because no live interface attach exists. That needs hostapd's behaviour measured first, and stays open on #623.

## Alternatives considered

- **Retry on a hardware add as well as on the timer.** Rejected. It would tie the restart path to the uevent listener to save at most 30 seconds, and ADR-0027 already owns how often a failing container is retried.
- **A new status such as `restart-refused`.** Rejected in favour of ADR-0270's precedent: a defined container that is not running is `stopped`, and a field says why. One status vocabulary for every client.
- **Log every refusal.** Rejected for volume. The record carries the current state, and the log carries the changes.
