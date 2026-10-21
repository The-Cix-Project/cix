# 0347 — A container whose interface leaves the host has failed

## Status

Proposed, 2026-10-21. It is #623's Part B: what happens to `ar-1` when its radio is unplugged while hostapd keeps running. The owner chose this remedy on 2026-10-21, over measuring hostapd first and over a live re-attach. It builds on [ADR-0346](0346-a-restart-that-cannot-be-made-is-a-failure-and-is-retried.md) (a refused restart is retried) and on [ADR-0344](0344-a-device-is-one-record-with-one-state-and-every-grant-asks-it.md)'s interface records.

## Context

Read from the code at `3d4bf51d` unless an item says it was measured.

**Nothing reacts when a moved interface's hardware leaves.** An `interfaces[]` grant moves the interface into the container's netns, and a radio moves as its whole PHY (ADR-0265). `reconcile_live_device_revocations()` runs on a USB whole-device remove and walks `devices[]` grants only. So when a USB radio is unplugged from a running container:
- `registry_interface_record` keeps the grant, and `append_moved()` keeps listing `phy:phyN` and `net:<name>` as held by the container, under a parent that no longer exists. `GET /v1/devices` shows a radio that is not there.
- The container keeps running without its interface. Whether hostapd exits when its interface disappears is not measured: nothing on .95 can be unplugged without someone at the Proxmox host.

**Two outcomes, depending on that unmeasured behaviour.** If hostapd exits, `ar-1`'s `on_exit: fail-container` ends the container. Its restart is refused while `radio-ap` matches nothing, and since ADR-0346 it is retried until the radio returns. If hostapd keeps running, nothing happens: the AP is down, the record says held, and a returning radio appears on the host as a new PHY that nobody moves back.

**A live re-attach would not restore the service.** Moving a returning radio into the running container needs a contract change, since `interfaces[]` is "set once at creation; not modifiable on an already-running container". It also needs a new path that moves a PHY into a running netns. And a process still holding the old interface does not adopt a new one unless it reopens it.

**The record that disappears is the anchor.** Each interface record carries `anchor`: the host record its grant hangs off (`usb:2357:012e@0000:07:1b.0-usb2-1` for `ar-1`'s `phy:phy0`, measured with `GET /v1/devices` on .95 at 0.2.57-564). The anchor stays on the host when the PHY moves, and leaves the host's walk when the hardware does. The `phy:` and `net:` records are synthesized from the grant, so they stay in the snapshot after an unplug and cannot answer the question.

**An exited container still holds what it was granted** (read from the code). `devicegrant_snapshot()` walks every registry entry, not only running ones. So a container kept as exited (ADR-0181: `restart: "no"`, or `on-failure` after a clean exit) still adds its moved interface records and claims its grants as held, though its cgroup and netns are gone and its interfaces are back on the host. The loop that applies ADR-0344's reservation to stopped definitions skips any name with a registry entry, so a kept-exited container is not treated as stopped either. Its comment says "Running containers".

**A DELETE during a restart teardown is lost** (read from the code). `DELETE` on a container whose teardown is in flight upgrades a STOP to a DELETE and removes the definition. For any other kind it returns 204 and changes nothing, so a DELETE landing during a rolling restart's stop leaves the definition, and the replay brings the container back.

## Decision

**An `interfaces[]` grant is part of what the container is. When the hardware under one leaves the host, the container has failed, as if a service with `on_exit: fail-container` had exited. Its restart policy then applies, and ADR-0346 brings it back when the hardware returns.**

1. **Detection.** The reconcile that revokes unplugged `devices[]` grants also asks, of the same snapshot, whether each running container's interface anchors are still there. It uses the same `device_grants_gone()`, which from now on never reports an empty id as gone, so a record with no anchor is skipped by the one function rather than by a second one. The check runs on a USB whole-device remove, as today, and also on a PCI remove, because an interface's hardware sits on one of those two buses. Not on a block or SCSI remove.
2. **Failure, stopped gracefully.** The daemon stops the container with a new teardown kind, `REGISTRY_TEARDOWN_FAIL`, so cix-init stops the services in order, as for any stop. When the incarnation ends, the exit goes through the unprompted-exit path, not the teardown branch, and counts as a failure whatever its exit status. So `on-failure` restarts it, `no` does not, and the crash path's backoff applies.
   - It does not count against a roll's probation. An unplug is not the image's fault, so `roll_failures` is not incremented for this exit.
   - The status reads `stopping` while it shuts down, as for a STOP.
   - That change is the daemon's own, made from a uevent, so no request's `x-cix-invalidates` covers it. `registry_begin_kill()` announces the container and device reads (ADR-0339), for every kind. Before this it announced nothing, so a rolling restart's stop was not announced either.
3. **Said once.** The log carries one error line per failure: the container, the interface, and the record that left. The exit that follows is logged at info, as a STOP's is, even if the shutdown escalated to SIGKILL, so one failure is one error line. When the restart is then refused because the interface names nothing present, ADR-0346's `restart_refused` says so, and its retry brings the container back within 30 s of the hardware returning, renamed inside as before.
4. **A DELETE during any teardown that would bring the container back becomes a DELETE.** STOP, RESTART and FAIL are all upgraded and the definition is removed, so the finished teardown does not replay it. MIGRATE is not touched here: a migration's stop has its own completion and abort path.
5. **Only a running container holds.** The snapshot takes moved records and grant claims from running entries only. A kept-exited container is a stopped definition, as ADR-0344 means: it appears in `reserved_by`, and binds only with `reserve_devices`. Without this, a `restart: "no"` container failed by an unplug would keep listing the radio it lost. The contract already says `held_by` lists "every RUNNING container holding this record", so this brings the code to it.

## Verification

1. **In the gate.** `test_devicestate`, which is in `SELFTESTS`, checks `device_grants_gone()` on the new case: an empty id is never reported as gone, beside a present and an absent one. Nothing in a build container can be unplugged, so the failure path itself is not gated, and neither is rule 5.
2. **Live, on .95, which needs the owner.** `ar-1`'s TP-Link dongle is USB passthrough from Proxmox, so `device_del` and `device_add` from `qm monitor` are a real unplug and re-plug. Expected:
   - one log line naming `radio-ap`;
   - `ar-1` stopping, then `restart_refused` naming the absent interface;
   - `GET /v1/devices` no longer listing `phy:phyN` as held;
   - after the re-plug, `ar-1` running again within about 30 s with `radio-ap` inside.

   The same run measures what #623 has carried as unknown since it was filed: whether the container exits by itself, through hostapd, before the daemon fails it. Both the log order and that answer go into #623. The AP is down from the unplug until about 30 s after the re-plug.

## Consequences

- A container whose interface is unplugged goes down at once and comes back when the hardware returns, whether or not its own process noticed. The record stops showing a held radio that does not exist.
- The restart goes through the ordinary create, so the returning radio is found by its `mac` mapping in any port and named inside as before. No second path moves an interface into a running container.
- **A USB reset is an unplug.** Some USB wifi drivers reset their device after an error, and the kernel reports that as a remove and an add. The container is failed and back within about 30 s of the add. That is the one false positive this design accepts: from the host's side a reset and an unplug are the same event, and a process inside loses its interface either way.
- A `restart: "no"` container whose interface leaves stays exited, and the log line is the record of why. That is what `no` means.
- A container is failed for an unplug even when it could have carried on without that interface. `interfaces[]` has no optional form, so its interfaces are all required by definition. An optional interface would be a contract change, and is not decided here.
- A kept-exited container no longer blocks another container from its devices unless it set `reserve_devices`, which is what that setting already says about a stopped one.
- A PCI remove now also runs the `devices[]` revocation, which only USB triggered before. A PCI hot-remove of a passed-through GPU is revoked like a USB unplug.

## Alternatives considered

- **Measure hostapd first.** If hostapd exits on its own, ADR-0346 already covers `ar-1`. Rejected as the remedy because it makes the outcome depend on one program's behaviour; every other process that holds a moved interface would need measuring too. The measurement still happens, as part of the live verification.
- **Live re-attach** of the returning interface into the running container. Rejected for the three reasons in the context: a contract change, a second move path, and no effect on a process that keeps its old interface.
- **Revoke the grant and leave the container running**, as `devices[]` does. Rejected: a device node can be re-granted into a running container, but a process cannot be made to use a new interface, so the container would run on with no interface and a record that says nothing is wrong.
