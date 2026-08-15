# 0118 — `boot_init()` mounts `/dev/pts`: the real root cause of every live console/exec failure on an installed box

## Status

Accepted

## Context

Task #764 (a follow-up from task #760's sweep): `thincctl console` returned a bare `HTTP/1.1 500 Internal Server Error` against every running container on 192.168.15.95, while the identical `exec_into_container()` code path (`daemon/src/exec.c`) passed cleanly in the local `test_console_exec` regression test.

The real reason was invisible without host shell access — `exec_into_container()`'s own diagnostics were plain `fprintf(stderr, ...)` calls that go to `thincd`'s own stderr, never captured by the logstore and never returned to a REST client. A first diagnostic pass (surfacing `strerror(errno)` in the HTTP response body and the console client's own error output — see the companion commit just before this one) made the real reason visible for the first time: `"failed to start console session: No such file or directory"`.

Tracing that ENOENT to its source: `exec_into_container()` calls `posix_openpt()` (always succeeds — `/dev/ptmx` is populated unconditionally by `devtmpfs`, `CONFIG_DEVTMPFS_MOUNT`), then `ptsname_r()` to get the slave device's path (e.g. `/dev/pts/3`), then `open()`s that path directly. That slave path only resolves to a real device node once the **devpts filesystem** is actually mounted at `/dev/pts` — a completely separate step from `/dev/ptmx` existing. `image/src/thinc-install.c`'s own `early_mounts()` has mounted devpts since ADR-0042/task #406 — but that binary only ever runs once, during disk installation. `daemon/src/main.c`'s `boot_init()` — the function that actually runs every time `thincd` boots as real PID 1 on an already-installed box (`--init-mode`, a bare kernel with no initramfs) — mounts `/proc`, `/sys`, `cgroup2`, the containers device, binds `/etc/resolv.conf`, and mounts the ESP, but never mounted `/dev/pts` at all. This gap has existed since the console/exec feature itself was built (tasks #424-434), which happened *after* task #406's devpts fix — the installer's own devpts mount was never carried over to the daemon's own separate boot path, because nothing exercised the gap until now.

This also explains why the bug was invisible in every prior test run: the local dev-sandbox and every QEMU test harness invocation of `thincd` inherits an already-mounted `/dev/pts` from its own outer environment (the sandbox container, or — for the QEMU tests — a normal Linux userspace booting the test image, not `thincd` itself running as bare-metal PID 1). Only a real, from-scratch `--init-mode` boot on genuine hardware/QEMU-as-PID-1 (exactly what 192.168.15.95 is) ever exercises `boot_init()` at all — and this ADR's own verification (below) is the first time this project's own QEMU boot tests confirm that path explicitly for devpts.

## Decision

`boot_init()` gains the same devpts mount `thinc-install.c`'s `early_mounts()` already has, immediately after the existing `proc`/`sysfs`/`cgroup2` mounts and before anything container-related: `mkdir("/dev/pts", 0755)` (EEXIST-tolerant) then `mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "mode=0620,ptmxmode=0666")` — identical options, for the identical reason (`ptmxmode=0666` keeps devpts's own `/dev/pts/ptmx` alias world-writable, matching what any non-root `posix_openpt()` caller would expect even though everything in this project currently execs as root). Made fatal (`return -1` on failure), matching every other mount in `boot_init()` except the two already explicitly documented as non-fatal (the ESP config partition and the resolv.conf bind) — a host that can't provide `/dev/pts` can't offer the console/exec feature at all, and failing loudly at boot is strictly better than a silent, only-discoverable-live 500 on first use.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. Full regression sweep (24 daemon-linked/host-side tests) all pass. Critically, this change touches the real PID-1 boot path, so the two QEMU tests that actually exercise it end to end were run explicitly, not just the fast in-process tests: `test_boot` (a from-scratch disk image, real `thincd` as PID 1, confirms `boot_init()` itself doesn't regress) and `test_installer` (a full install + reboot cycle) — both pass.

Live-verified on 192.168.15.95 after deploying as `v1.8.6`: `thincctl console jumpbox1 --cmd=/usr/bin/id` returned `uid=0(root) gid=0(root) groups=0(root)` cleanly, confirming the console/exec path now works end to end on a real, from-scratch `boot_init()` boot.

## Consequences

- Every future real install (or reboot of an already-installed box) now has a working console/exec feature from first boot — this was never something a fresh install could offer at all, since the gap predates any box this project has ever actually deployed to with the feature enabled.
- `thinc-install.c`'s own `early_mounts()` devpts mount is now duplicated logic (two copies of the identical mkdir+mount, one per binary) rather than shared — accepted deliberately: the two binaries have no shared "host bootstrap" module to put it in, and introducing one for two lines of genuinely identical code would be a larger, riskier refactor than this fix's own scope justifies. A future cleanup could extract a shared helper if a third caller ever needs the same mount.
- This is a strong argument for extending this project's own QEMU boot tests to more of the daemon's real runtime surface (not just startup + a health check) going forward — `test_boot`/`test_installer` confirm `boot_init()` itself still succeeds with the new mount in place, but neither actually exercises the console/exec feature end to end over a real PID-1 boot; a future test doing so would have caught this exact gap directly rather than needing a live investigation to surface it. Not added here — out of scope for a same-day root-cause fix, but a real, concrete follow-up worth tracking.
