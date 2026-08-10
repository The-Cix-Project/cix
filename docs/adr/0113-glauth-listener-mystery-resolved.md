# 0113 — glauth's "listener mystery" resolved: two real, unrelated causes, neither a Kanxeo or glauth defect (closes task #747 for real)

## Status

Accepted

## Context

Task #747 (Part 69 of `ROADMAP.md`) closed the "listener mystery" investigation without a code change, on the grounds that every layer Kanxeo owns had been independently proven correct and the remaining unknown lived inside glauth's own stdout/stderr — unreachable without the `capture_output` feature (ADR-0112) that session explicitly deferred. That investigation's own diagnosis was **half right and half wrong** in a way only became visible once the missing tool actually existed:

**Half right**: something genuinely internal to the running glauth process was the remaining unknown, and reading its real stdout/stderr was the correct next step.

**Half wrong**: the investigation's own diagnostic container (`glauth-diag`) was never actually talking to glauth at all. `POST /v1/containers` with no explicit per-network IP auto-allocates one, and on this box's `management` network that auto-allocation landed on `192.168.15.1` — an address never reserved for Kanxeo's own use (the project's own standing convention, confirmed with the user directly, restricts scratch/diagnostic containers to `192.168.15.101`-`109`) and, it turns out, a real device already present on the physical LAN. Every TCP SYN sent to `192.168.15.1:3893` was answered — correctly, per TCP semantics — by *that* device's own kernel with an immediate RST, since it has nothing listening on port 3893. glauth itself, the entire time, was running perfectly healthy and genuinely `listen()`ing, completely unaware any of this was happening: not a namespace, bridging, or Kanxeo-side problem at all, just a diagnostic test pointed at the wrong physical machine.

## Decision

### Root cause 1 — diagnostic tooling discipline, not a defect

Confirmed directly, this session: recreating the identical container (same image, same command, same config) with an explicit `{"name": "management", "ip": "192.168.15.103"}` network attachment (inside the reserved range) connects on the very first attempt, no code changed anywhere. `capture_output` (ADR-0112) is what made this fixable at all — without it, there was no way to see glauth's own successful `LDAP server listening address=0.0.0.0:3893` log line and realize the *process* was fine, only the *test* was wrong.

No code fix for this half — it's a discipline gap (see [`feedback_lan_ip_range`](../../CLAUDE.md) convention, now reinforced by this ADR as a concrete cautionary example), not a bug. Any future scratch/diagnostic container on this box's `management` network must pin an explicit IP inside `192.168.15.101`-`109`, never rely on auto-allocation, which has no knowledge of what else lives on the physical LAN outside Kanxeo's own bridge.

### Root cause 2 — a real, previously-undiscovered kernel gap: no inotify support

With the IP fixed, a second, genuinely real defect surfaced immediately via the same `capture_output` diagnostic: `could not start config-watcher error="function not implemented"` — `ENOSYS` from `inotify_init()`. `CONFIG_INOTIFY_USER` had never been enabled anywhere in `image/kernel/qemu-part1.config`'s history (confirmed via `git log -p` and a direct grep), since nothing built before glauth ever needed live filesystem-change notification inside a container. glauth's own fsnotify watcher (`v2/glauth.go`'s `startConfigWatcher()`, active whenever the config sets `watchconfig = true`, per ADR-0109's own design) silently failed to start and glauth fell back to a one-time config read at its own startup — meaning every one of Kanxeo's own live `POST`/`PUT /v1/ldap/users`|`/groups` writes (`ldap_write_config_file()`) landed on disk correctly but never reached an *already-running* glauth process. Only a fresh container start (which reads the file once at boot, by which point it already reflects Kanxeo's current durable record) ever showed correct data — a subtle, intermittent-looking symptom depending entirely on container creation order relative to CRUD calls, exactly the kind of thing that makes a "listener mystery" drag across sessions.

Fixed by adding `CONFIG_INOTIFY_USER=y` to `image/kernel/qemu-part1.config` (its own real Kconfig dependency, `select FSNOTIFY`, resolved automatically by `make olddefconfig`) and rebuilding the kernel from the project's own documented, reproducible recipe (the config file's own header comment). Deployed to 192.168.15.95 via the established LAN-serve `system/update --kernel_path=` + reboot round-trip (`docs/guides/remote-development.md`), no ISO reinstall needed.

## Verification

Live, end to end, against the real box: recreated `ldap-1` (pinned to `.103`) post-reboot, confirmed `captured_output` shows a clean `LDAP server listening` with no config-watcher error, then issued a real `POST /v1/ldap/users` (`testuser2`) with **no container restart** — `captured_output` showed glauth's own `watcher got event`/`Config was reloaded` lines firing live, and a genuine `ldapsearch` bind against `192.168.15.103:3893` with the new user's real password authenticated successfully (`result: 50 Insufficient access` is glauth's own real, expected authorization response for a user with no granted search capability — proof the bind itself succeeded, not a failure).

## Consequences

- Every container on `management` created by an operator or by future diagnostic work must use an explicit, reserved-range IP — the auto-allocation path remains correct and useful for isolated/non-uplinked networks, but is unsafe on a network with a real physical uplink where Kanxeo has no visibility into what else occupies the subnet.
- `container_decode_exit_status()`'s [141,255] ambiguity (noted in ADR-0112's own Consequences section) remains a real, separate, still-open observability gap — unrelated to this ADR's own findings, not fixed here.
- Any future glauth-adjacent (or generally fsnotify-dependent) container feature can now rely on `CONFIG_INOTIFY_USER` being present in every subsequently-built kernel — this is a permanent kernel-config addition, not a one-off patch.
