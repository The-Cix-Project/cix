# 0119 — `/run` is a fresh tmpfs on every container start, not overlay-persisted storage

## Status

Accepted

## Context

`ntp-1`/`ntp-2` (`chrony.recipe`, task #763) never reliably survived a real reboot of 192.168.15.95 -- every reboot this session required manually deleting and recreating them. Investigating on the user's direct request ("why do ntp not survive a reboot, fix that") found the crash-loop was live and reproducible on demand, not reboot-specific: any respawn of a container named `ntp-1`/`ntp-2` failed within milliseconds, every time, while an identical container under a fresh name (`ntp-debug`, same image, same files, same command) started and stayed up cleanly.

The only difference between the two was on-disk state: `ntp-1`/`ntp-2` had been created and crash-looped many times across earlier sessions without ever going through an explicit `DELETE` (only process death, never disk cleanup) -- and `src/overlay.c`'s own `overlay_create()` mounts a container's upperdir `EEXIST`-tolerant by design (see that file's own comment: "the container is being recreated after a crash/restart before cleanup"), meaning a same-named container's writable layer, including `/run`, is deliberately reused rather than cleared, so genuine crash-restart state (a workload's own in-progress writes) survives a respawn as intended.

`GET /v1/containers/{name}/files --path=/run/chronyd.pid` on the (temporarily stabilized, post-`DELETE`) `ntp-1` confirmed the mechanism directly: it contained the literal byte `1`. `chronyd -d` (foreground/debug mode, `chrony.recipe`'s own invocation) still writes its compiled-in pidfile (`--with-pidfile=/run/chronyd.pid`, Part 72) and checks it on startup. Because every container gets its own fresh PID namespace (`CLONE_NEWPID`), chronyd is *always* PID 1 inside it -- so on any respawn, the leftover pidfile from its own previous life names a PID (`1`) that trivially, always exists (chronyd's own new incarnation, or any process at all in a fresh PID namespace), and chronyd refuses to start ("another instance may already be running"), exiting 1 immediately, forever, with no self-recovery possible short of deleting the container's on-disk state outright.

This is not a chrony-specific bug or a chrony.recipe misconfiguration -- `/run` being cleared on every process-manager-level start is a near-universal Linux/systemd convention that plenty of real, unmodified upstream daemons assume without stating it (the same general class of "assumes a convention this project's minimal images/lifecycle don't provide" gap CLAUDE.md already documents repeatedly for pidfile paths and `/etc/passwd`). `src/mountns.c`'s `mountns_pivot()` already mounts fresh `/proc` and `/sys` on every single container start, unconditionally -- `/run` had simply never been given the same treatment, because nothing had hit the gap loudly enough to surface it until chronyd's own respawn-time self-collision did.

## Decision

`mountns_pivot()` gains a third fresh mount, immediately after the existing `/sys` mount: `mkdir("/run", 0755)` (`EEXIST`-tolerant, matching the `/proc`/`/sys` pattern exactly) then `mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755")`. This shadows whatever `/run` content the overlay's upperdir carried over from a prior life (or the image's own lowerdir baseline) with an empty tmpfs, on every container start, every time -- unconditionally, not just for `restart:"always"` definitions, matching `/proc`/`/sys`'s own unconditional treatment. `MS_NOEXEC` is deliberately *not* set (unlike `/proc`/`/sys`, which never need to execute anything) since a real workload may reasonably stage or execute a helper script under `/run`; `MS_NOSUID`/`MS_NODEV` are kept for the same reason every other in-container mount already carries them.

Fixed at the mount-namespace level, not in `chrony.recipe` (e.g. by trying to suppress chronyd's pidfile) -- a per-recipe patch would only fix this one daemon, leaving the same class of bug waiting for the next upstream binary that assumes `/run` is ephemeral (`glauth`/`sshd`/anything else this project ever wraps). One general fix at the container-runtime level closes the whole class at once, consistent with "One Source of Truth" and "No Parallel Implementations."

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. Full regression sweep (24 tests) all pass, including `test_overlay`/`test_container_net`/`test_devices`/`test_container_lifecycle`/`test_container_restart` (every test that actually exercises `mountns_pivot()` via a real `clone3()`+`pivot_root()`).

Live on 192.168.15.95: after explicitly `DELETE`ing the pre-existing, poisoned `ntp-1`/`ntp-2` disk state (one-time cleanup -- the fix prevents the *next* poisoning, it doesn't retroactively clean up the one this session already left behind), redeployed as `v1.8.7`, recreated `ntp-1`/`ntp-2`, and confirmed stable. Rebooted the box twice in succession: both times `ntp-1`/`ntp-2` autostarted via `containerdef_autostart_all()` and stayed up, with zero manual intervention -- the exact failure mode this ADR exists to fix.

## Consequences

- `/run` now behaves the same way inside a Cix container as it does on every real Linux host: empty at start, safe for any daemon's pidfile/socket/lock conventions, never a source of stale cross-restart state.
- Any recipe that was relying (even accidentally) on `/run` content surviving a restart needs to move that state under a path that's actually meant to be persistent (anywhere else in the container's own rootfs) -- no current recipe in this repository does this, confirmed by inspection of every `files[]` block and every recipe's own runtime paths.
- This does not retroactively fix any container whose on-disk upperdir was already poisoned by a stale `/run/chronyd.pid`-shaped file before this fix was deployed -- those still need one explicit `DELETE` to clear the old state; only *new* poisoning is prevented going forward.
