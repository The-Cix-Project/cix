# 0146 — LDAP server startup resync, and a break-glass recovery boot tool

## Status

Accepted

## Context

Activating host-auth write-gating for real on the production deployment (192.168.15.95, task #843) produced a genuine, total lockout: every `POST`/`PUT`/`DELETE` to `cixd`'s own REST API started failing authentication, for every user, with no way back in through the API itself (only `/v1/login`/`/v1/logout` are exempt from write-gating, and login itself was failing).

Root cause, confirmed by direct investigation (`ldapsearch` against `ldap-1` returning `Invalid credentials (49)`; `GET .../files` on `ldap-1`'s own `glauth.cfg` showing zero rendered `[[users]]` entries; direct reading of `daemon/src/hostauth.c`'s `try_ldap_login()`/`hostauth_login()`): `ldap_init()` loads registered LDAP server bindings from persisted state at daemon startup, but nothing re-pushes current user/group content into those servers' *live* config until the next explicit LDAP write happens. Every container — including the ones serving LDAP — is torn down and recreated on every host reboot, so a restart silently left the LDAP server's own managed config section empty until someone happened to make an LDAP mutation afterward. Write-gating's own login path (`hostauth_login()`) is deliberately strict by design: once *any* configured LDAP server gives an authoritative answer (a real bind success, or an explicit `LDAPCLIENT_ERR_LDAP_RESULT` rejection), that answer is final and the local password copy is never consulted — a real, intentional security choice (a revoked LDAP credential must not still work via a stale local fallback), not itself a bug. An LDAP server rendering zero users is exactly the "authoritative rejection" case, so every login failed, including the one admin account that would otherwise have worked locally.

This is the identical class of gap ADR-0091 already closed once, for DNS server bindings (`dns_server_sync_all()`, called at daemon startup right after `containerdef_autostart_all()`) — it was simply never mirrored to LDAP when the LDAP backend was built.

Immediate recovery for the live box required a full reinstall (no shell exists anywhere on this OS by design — the REST API is the entire control surface, so a lockout with no working login has no in-band unblock). The user asked for two things once unblocked: fix the actual bug, and "make sure we have a way to recover if this happens again... something safe and not trivial so we dont compromise cix, maybe boot with an iso that unlocks."

## Decision

**Part 1 — fix the root cause.** `daemon/src/main.c`'s startup sequence now calls `ldap_record_sync_all()` immediately after the existing `dns_server_sync_all()` call, mirroring that call's own placement and purpose exactly: every registered LDAP server binding gets its live user/group config re-pushed on every daemon startup, not just on the next explicit LDAP write. Verified with a new regression test in `test/test_ldap.c`: register an LDAP server, create a group, confirm it renders, do a real `stop_daemon()`+`start_daemon()`+`wait_for_daemon()` cycle with **zero** new LDAP writes in between, then confirm the same group still renders in the config file afterward.

**Part 2 — a genuine break-glass recovery mechanism**, `image/src/cix-recover.c`, a new, deliberately tiny (~230 line) standalone binary:

- Boots as its own `init=` target from a **second GRUB menu entry on the same installer media** as `cix-install` (`image/src/mkinstalleriso.c` now takes a `cix-recover-bin` argument and stages it as a second `/bin/` binary sharing the installer's already-staged runtime libs, and writes a `"Cix Recovery"` `menuentry` alongside `"Cix Install"`).
- Mounts the *already-installed* system's own real containers partition (`/dev/vda5`, the same hardcoded convention `daemon/src/main.c`'s `CONTAINERS_DEVICE` and `cix-install.c`'s `BOOT_TIME_DISK_PREFIX` already rely on) read-write.
- Resets **exactly one field** — `admin_groups`, back to empty, the same state a fresh install starts in, where every API write is open with no login required — in the persisted `state/hostauth_config.json`. Every other field (idle timeout, LDAP enable/servers/port/base DN) is preserved byte-for-byte from the parsed original.
- Touches nothing else: no container, no LDAP user/group record, no LDAP backend setting, no data of any kind.
- Requires a real typed `RESET` confirmation at the console before writing anything; aborts cleanly on anything else.
- Deliberately does **not** reuse `cix-install.c`'s own much larger partition-discovery/formatting machinery, even though that's a form of controlled logic duplication — a break-glass tool's real security property is how small and independently-auditable its own code is, and pulling in the full installer's disk-partitioning surface for a task that needs none of it would work against that, not for it.

**Real, deliberate friction**, matching the user's own "not trivial" bar: reaching this code at all requires hypervisor/physical console access to attach different boot media and force a reboot — something no network-side attacker manipulating `cixd`'s own REST API could ever do, gated or not. The typed confirmation on top means a stray or accidental boot into this entry can't silently disable write-gating either.

The user's own question — "if ldap is not available, or active, then we fall back to the user/group that's on the box itself without using ldap? right?" — describes real, already-existing behavior (`hostauth_login()`'s `answered == 0` fallback path), not a gap; what actually failed was that LDAP *was* reachable and answered authoritatively (with an empty, unsynced user list), which is a different case from LDAP being genuinely unavailable. Part 1 closes the gap that put the system into that state in the first place; Part 2 provides a real recovery path for if it (or anything else) locks every login out again regardless of cause.

## Consequences

- `image/src/mkinstalleriso.c`'s argv contract grew from 11 to 12 positional arguments (a new `cix-recover-bin` inserted after `cix-install-bin`) — both real call sites (`test/test_installer.c`'s own build-and-boot verification, and `daemon/src/main.c`'s `iso_build_start()` behind `POST /v1/system/iso`) updated to match.
- `pkg/recipes/cix/v1.4.0/build.sh` (the self-hosted ISO-build path, ADR-0057) is pinned to a git tag that predates `cix-recover.c`'s existence, so `POST /v1/system/iso` on a box whose "cix" hostbuild artifact still comes from that old pin will now correctly report a missing-artifact error (`cix-recover (from a "cix" hostbuild, ADR-0146)`) rather than silently omit the recovery entry — a real, pre-existing staleness this change surfaces but does not fix; re-pinning that recipe to a current tag is tracked separately (task #852), since it needs a live self-hosted build round-trip to verify, not a blind edit.
- The recovery tool only supports the standard, default state-storage layout (`/dev/vda5`) — a system whose state storage was relocated via the storage-migration mechanism (ADR-0141) needs manual recovery instead; documented directly in the tool's own console output, not silently assumed.
- This is the second break-glass-style mechanism this project has now built with a "small, independently-auditable, deliberately-not-DRY" posture relative to a larger sibling tool (`cix-install.c`) — the same justified-duplication precedent, applied to a new case.
