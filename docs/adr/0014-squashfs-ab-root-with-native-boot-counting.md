# 0014 — Read-only squashfs A/B root with systemd-boot's native boot counting, not a writable root or a hand-rolled rollback mechanism

## Status

Accepted

## Context

Every phase through Phase 10 built and verified `cixd` as a guest process on top of an already-booted Linux system — this project's dev environment has, since Phase 1, always been a privileged LXC container on someone else's kernel. Phase 11 makes Cix the operating system itself, which means for the first time this project owns the boot chain, and the consequences of a corrupted or bad root filesystem stop being "restart the container" and start being "the machine won't boot." The user's own framing when scoping this phase was explicit: two root partitions carrying the same image, specifically "in case of corruption" — a real resilience requirement, not just a preference for having a spare copy sitting around unused.

Three designs were considered for what "two root partitions" actually means in practice:

1. A writable root, manually duplicated to the second partition as a backup an operator restores from by hand if something breaks.
2. A writable root with an application-level (in `cixd` itself) integrity-checking and rollback mechanism — e.g. checksums recorded per-file, verified at boot, falling back to the other slot on mismatch.
3. A read-only root image per slot, with the *bootloader* — not `cixd` — owning boot-attempt counting and automatic fallback.

Option 1 gives no automatic protection at all — an operator has to notice something is wrong and intervene, which fails exactly the "rock solid" bar this phase was scoped against. Option 2 is real, buildable work, but it means inventing and maintaining a bespoke integrity/rollback protocol inside `cixd`, competing with something the bootloader ecosystem has already solved correctly: `systemd-boot` has shipped a "Automatic Boot Assessment" mechanism (a `+LEFT-DONE` attempt-counter suffix on a loader entry's filename, decremented each boot, entry treated as failed and skipped once exhausted) for years, specifically for this exact A/B-rollback use case. Building a second implementation of the same idea, worse, would be exactly the "No Parallel Implementations" maxim this project holds everywhere else in the codebase, just applied to boot infrastructure instead of application code.

A writable root (options 1 and 2) also leaves a whole class of failure on the table that a read-only root closes outright: corruption introduced by the system's own normal operation — a crash mid-write, a disk error during a routine log/state update, anything that touches the root filesystem while the system is simply running. If root can't be written to at all during normal operation, that entire failure class doesn't exist.

## Decision

Root A and root B are each a single, immutable **squashfs** image, mounted read-only. `cixd` and its direct runtime dependencies (`openssl`, `curl`, `tar`, `sha256sum`, `dnsmasq`, `cp`, `rm`, ...) are the *only* things on it — no per-container or package-manager state, which lives on the separate config (4) and container-storage (5) partitions instead (untouched by anything in this decision).

Rollback is owned entirely by `systemd-boot`'s existing boot-counter convention, not by any code this project writes:

- The installer (and, later, the update path — see `docs/roadmap/ROADMAP.md` Phase 11) writes a loader entry for the newly-written slot with a fresh attempt budget.
- On every boot, `systemd-boot` decrements the counter before handing off to the kernel.
- `cixd`, once it reaches a genuinely healthy running state (detailed in Phase 11 part 2's design), marks the boot as successful with a plain `rename(2)` on the ESP — removing the attempt-counter suffix from its own loader entry's filename. This is deliberately a direct filesystem operation, not a subprocess call to `bootctl`, consistent with ADR-0007's hand-rolled-daemon posture for anything this simple.
- If `cixd` never gets the chance to confirm success (crash, hang, a kernel that doesn't even reach userspace) the counter reaches zero on some future boot attempt, and `systemd-boot` — entirely on its own, no daemon involvement — stops offering that entry and falls back to the other slot.

Writing a slot (whether the installer's initial write or a future update) is a single atomic operation: write the whole new squashfs image to the inactive partition, write its loader entry, done. There is no partial-update state a corrupted write could leave behind on the *active* slot, because the active slot is never touched by this process at all.

## Consequences

- `cixd` gains real, non-optional boot-time responsibilities it has never had before (this is why Phase 11 part 1 exists as its own step): recognizing `--init-mode`, and — once genuinely healthy — performing the loader-entry rename that tells the bootloader "this boot counts as good." Getting the definition of "healthy" wrong (confirming too early, before something that actually matters has succeeded) would silently defeat the entire rollback mechanism, so that definition needs its own explicit design pass in part 2, not an assumption carried over from this ADR.
- Any future OS-level state that legitimately needs to survive across a boot but isn't container/package workload data (a runtime log, a crash marker, anything) cannot live on root at all — it has to go on the config partition (4), the same boundary Phase 11's design already draws for `cixd`'s own persisted JSON state. A read-only root makes this a hard constraint, not a style preference.
- This is now the reference pattern for *any* future "the OS itself needs to change" story (kernel updates, `cixd` binary updates) — write a new image to the inactive slot, let the existing boot-counter mechanism gate whether it's trusted, never patch the active slot in place. A design that reached for in-place modification of a running root would be reintroducing exactly the corruption exposure this ADR exists to close.
- Choosing `systemd-boot` was already effectively locked in by this decision, not a separate, independently-reversible choice — its native boot-counting convention is the specific reason it was picked over GRUB or a hand-rolled boot menu in the first place.
