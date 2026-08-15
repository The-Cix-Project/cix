# 0150 — every container gets its own devpts mount and a `/dev/ptmx`

## Status

Accepted

## Context

User-reported, real: an interactive `ssh osakka@192.168.15.109` (jumpbox1) failed with `PTY allocation request failed on channel 0` (tracked as task #865, alongside a second, separate `pam_mkhomedir` gap reported in the same session). Every prior test of this platform's SSH support used non-interactive `ssh ... id` calls, which never request a pty at all — this was the first real interactive session ever exercised against a live container.

Root-caused directly, not guessed: `sshd`'s pty allocation opens `/dev/ptmx`, then opens the specific slave path `ptsname_r()` returns. Two things were missing, both confirmed by reading the actual code before touching anything:

1. **No `/dev/ptmx` at all.** `pkg_seed_image_baseline()` (`daemon/src/pkg.c`) only ever `mknod()`s five static char devices (`null`, `zero`, `full`, `random`, `urandom`) into a fresh image's `/dev`. This project's containers use a plain overlay `/dev` (no `devtmpfs`, unlike the host), so nothing auto-provides `/dev/ptmx` the way a normal Linux boot does.
2. **No `devpts` mounted anywhere inside a container's own mount namespace.** `mountns_pivot()` (`src/mountns.c`) already gives every container a fresh `/proc`, `/sys`, and `/run` on each start, but never touched `/dev/pts`. Even a static `/dev/ptmx` node would only work with a real `devpts` filesystem mounted somewhere reachable — and since each container gets its own `CLONE_NEWNS`, the host's own `/dev/pts` (mounted at boot by `main.c`'s `boot_init()`) is never inherited into it.

This is the identical bug **class** task #764 (ADR referenced there as "pending #426") already found and fixed once, for the daemon's own host-namespace console-exec feature (`exec.c`'s `posix_openpt()`+`ptsname_r()` pty allocation) — `boot_init()` gained a `/dev/pts` mount for exactly this reason. This ADR closes the same gap one namespace layer deeper, where it had never been addressed before: inside each container's own namespace, not just the host's.

## Decision

**Two changes, one per missing piece, both minimal and mirroring already-proven code:**

1. `src/mountns.c`'s `mountns_pivot()`: after the existing `/run` tmpfs mount, `mkdir("/dev", ...)` (guaranteed-present, the same treatment `/proc`/`/sys`/`/run` already get — a minimal rootfs isn't guaranteed to ship a `/dev` directory of its own) then `mkdir("/dev/pts", ...)` + `mount("devpts", "/dev/pts", "devpts", MS_NOSUID|MS_NOEXEC, "mode=0620,ptmxmode=0666")` — identical options to `boot_init()`'s own host-side mount, for the identical reason (`ptmxmode=0666` so a non-root process can still allocate a pty). Fresh on every container start, same as `/proc`/`/sys`/`/run` — mount points never persist across a restart, only rootfs *content* does.
2. `daemon/src/pkg.c`'s `pkg_seed_image_baseline()`: a `/dev/ptmx` **symlink** to `pts/ptmx` (devpts's own multiplexor device, the standard convention every real container runtime uses), seeded once alongside the five existing static device nodes. A symlink is static file content, unlike the mount itself — it belongs in the one-time image-baseline step, not the per-start mount sequence.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. New permanent regression test, `test/test_container_pty.c` + `test/pty_child.c` (mirrors `test_overlay.c`'s own minimal-lowerdir pattern exactly): a real container is created with a hand-built lowerdir reproducing `pkg_seed_image_baseline()`'s five device nodes plus the new `/dev/ptmx` symlink; its exec target allocates a pty exactly the way `sshd`'s own interactive session does (`posix_openpt`/`grantpt`/`unlockpt`/`ptsname_r`/open the slave/round-trip a byte) and reports results via its own upperdir. Confirms `/dev/pts/ptmx` exists post-mount, `posix_openpt()` succeeds, the slave opens, and a real byte round-trips through the pty.

**A real regression was found and fixed during this same verification pass, not shipped separately**: the first version of the `mountns_pivot()` change assumed `/dev` already existed before `mkdir("/dev/pts", ...)` — true for any real thinC-managed image (`pkg_seed_image_baseline()` always creates `/dev`) but false for `test_overlay.c`/`test_container_net.c`'s own minimal hand-built lowerdirs (no `/dev` at all), which broke outright (`mkdir: No such file or directory`) the moment this change landed. Fixed by giving `/dev` itself the same guaranteed-present treatment as `/proc`/`/sys`/`/run`. Full regression sweep re-run clean after the fix: `test_overlay`, `test_harness`, `test_devices`, `test_container_net`, `test_daemon_devices`, `test_images`, `test_container_lifecycle`, `test_container_restart`, `test_daemon`, `test_pkg`, `test_console_exec`, `test_daemon_net`, `test_networks`, `test_hostauth`, `test_ldap` all pass.

## Consequences

- Every container, on every platform this daemon runs on, now gets a real, isolated `devpts` instance and a working `/dev/ptmx` — not just jumpbox-shaped SSH containers. Any future workload needing a pty (an interactive shell, a terminal multiplexer, anything using `openpty()`) now just works, with no per-recipe opt-in needed.
- `container_dev_bpf_attach()`'s own cgroup device policy is unaffected: it's only ever loaded when a container specifies explicit `--device=` entries (`device_count > 0`); a container with none (every container in this fleet so far) gets no thinC-authored `BPF_CGROUP_DEVICE` program at all, so nothing here needed a device-cgroup allowlist change — confirmed by reading `container_dev.c` before assuming otherwise.
- Existing images (already-built rootfs content predating this change) self-heal automatically: `pkg_seed_image_baseline()` runs on *every* `pkg install`/upgrade against an image, not just its first version (confirmed via `install_mutate()`'s own doc comment — deliberately idempotent and re-run every time so an image that "missed baseline seeding" picks it up on its next install), so the very next package installed into any pre-existing image gains the `/dev/ptmx` symlink with no manual step. An image with genuinely no further installs ever planned would need one triggered by hand (or the image rebuilt) to pick this up — a real, narrow edge case, not the common one.
