# 0041 — container images get a real, fixed baseline FHS layout at seed time

## Status

Accepted

## Context

Phase 23 and Phase 24 independently hit the same class of bug: `iptables`'s own `/run/xtables.lock` needs `/run` to exist (Phase 23); `bird` hard-crashes with no `/dev/null` at all, and `birdc`'s own control socket needs a directory that didn't exist either (Phase 24). Both were fixed by hand, directly on the already-built `router` image's on-disk files during live verification, explicitly flagged both times as "not reproducible from a fresh `pkg install` on a clean image." No image this platform ever builds — `base`, `router`, or any future one — ships any baseline filesystem layout beyond whatever `pkg install` itself happens to produce.

`daemon/src/pkg.c`'s `pkg_seed_image_runtime()` (ADR-0019, ADR-0023) already solves the identically-shaped problem for one specific case — a dynamically-linked package needs `ld.so`/`libc.so.6`/etc. that no recipe provides itself — via a fixed, hardcoded set staged into every image, both at creation (`image_create()`) and after every install (`pkg_build_completed()`), idempotently. The dev-node and `/run` gap is the same shape: content every container implicitly needs that no recipe should have to provide for itself.

## Decision

Extend `pkg_seed_image_runtime()` in place — renamed to `pkg_seed_image_baseline()`, since its scope is now genuinely a baseline FHS layout, not just C-runtime libs — with two more fixed, hardcoded pieces, added to the same function so both call sites (`image_create()`, `pkg_build_completed()`) cover them automatically, with the same idempotent-and-self-healing property the runtime libs already have (a pre-existing image gets the baseline the next time anyone installs anything into it, no migration step needed):

1. **Standard char device nodes** `/dev/{null,zero,full,random,urandom}` — the exact `dev_nodes[]` table and `mknod(S_IFCHR|0666, makedev(major,minor))` pattern `test/test_image_fixture.c`'s `test_image_fixture_stage_toolchain()` already proves safe in this exact sandbox (its own ancestor cgroup's `BPF_CGROUP_DEVICE` policy permits precisely this set).
2. **A plain, empty `/run` directory**, mode 0755. No tmpfs mount — confirmed no `MS_TMPFS`/tmpfs mounting exists anywhere in the container runtime (`src/container.c`/`mountns.c`/`overlay.c`), and OverlayFS already makes a plain shared-lowerdir directory safe: anything a container writes under `/run` at runtime copies up into that container's own private upperdir, never touching the shared image or other containers.

Unlike the runtime-lib loop (which tolerates a missing *host source* — nothing to copy from is a legitimate, non-fatal case), a device node or a plain directory has no such excuse: any real `mknod`/`mkdir` failure here is a genuine I/O/permission problem and is **fatal** (`PKG_ERR_PERSIST_FAILED`), matching the existing loop's own fatal handling of a real copy failure, not its tolerant handling of an absent source.

**`pkg/recipes/bird.recipe` fixed at its actual root, not just papered over**: confirmed directly (fetched bird's real source, `autoreconf -fi && ./configure --help`, then grepped `configure.ac`/`Makefile.in`) that `birdc`'s control socket path is hardcoded as `CONTROL_SOCKET="$(runstatedir)/bird.ctl"`, and bird's own bare `--prefix=/usr` (no `--runstatedir=` override) resolves `runstatedir` to autoconf's default `${localstatedir}/run` = `/usr/var/run` — a real, confirmed root cause, not the generic missing-`/run` problem. `bird.recipe`'s `pkg_build()` now passes `--runstatedir=/run` explicitly; verified via a real local `./configure && make` that the resulting binary's compiled-in `PATH_CONTROL_SOCKET` is genuinely `/run/bird.ctl`, not assumed from `configure`'s own reported variable value alone.

## Consequences

- A from-scratch `pkg install` onto a clean image now produces a container that can actually run `bird`/`iptables`/any future package needing these same basics — the real, generic fix Phase 23 and Phase 24 both explicitly named as still open.
- **Still a fixed, hardcoded baseline set, not an extensible mechanism** — same shape ADR-0019's runtime-lib set already is, not a "package declares what extra baseline dirs/devices it needs" system. A future package needing something outside this set (another device node, another well-known directory) is a known, accepted future gap, found and fixed the same way this one was: live, when something actually breaks.
- **`/tmp` deliberately not staged into container images**, and this is a considered decision, not an oversight: `test_image_fixture_stage_toolchain()` stages a `01777 /tmp` for the *pkgbuild toolchain* image, a different, separate-purpose artifact — no container-image failure has ever named a missing `/tmp`.
- **No `/var/run → /run` compat symlink**, also deliberate: nothing has hit a missing `/var/run`, and it wouldn't even have fixed bird's own real gap (`/usr/var/run`, a different, `--prefix`-relative path). Speculative infrastructure for an unconfirmed failure mode, against this project's own "verify empirically, don't speculate" discipline.
- Scoped to container images only — the control-plane root itself needs none of this (`CONFIG_DEVTMPFS=y`/`CONFIG_DEVTMPFS_MOUNT=y` already auto-populates `/dev` there at boot, and `cixd`'s own state lives entirely under `BASE_DIR`, never `/run`) — `image/src/mkbootroot.c`/`cix-install.c` are unaffected, the same "two genuinely separate artifacts" framing ADR-0023 already established.
