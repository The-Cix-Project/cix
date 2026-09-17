# 0299 — The control-plane root is written atomically, and its own bytes answer for its freshness

## Status

Accepted. Issue [#481](https://git.home.arpa/itdlabs/cix/issues/481). Refines [ADR-0057](0057-self-hosted-toolchain-and-control-plane-rebuild.md)'s server-side assembly and [ADR-0105](0105-bootroot-assembly-freshness.md)'s generation counters; both decisions stand, and the second is the one this completes.

## Context

`mkbootroot` was pointed straight at its final output path. `run_mksquashfs()` passed `out_path` — `<data-dir>/rebuildable/bootroot/cixd-root.squashfs`, the artifact every deploy and every installer ISO consumes — to `mksquashfs` as `argv[2]`, having first `unlink()`ed it. That path was therefore the *write target* for the whole multi-minute xz compression, and anything that interrupted it left a fragment sitting exactly where the platform's most consequential artifact belongs.

This is not hypothetical. On 2026-09-17 a reboot was issued while an assembly was running (`GET /system/assembly` reported `running: true, completed_generation: 0`). The `POST /v1/system/update` that would have staged the result failed with `401` on an expired session token — and that expired token is the only reason a possibly-half-written root was not written to the inactive slot and booted.

Three separate defects had to line up, and each is independently real:

1. **The write was not atomic.** A failed, killed or interrupted assembly destroyed the root that was already there (it had been `unlink()`ed) and left a fragment in its place.

2. **The freshness signal did not survive a reboot.** ADR-0105's `started_generation`/`completed_generation`/`running` are `static long`/`int` in the daemon's own memory. Deploying an assembled root *ends in a reboot*, so the normal state of a host that has just booted the thing it assembled is `{"running": false, "started_generation": 0, "completed_generation": 0}` — byte-identical to a host that has never assembled anything in its life. Both readings are honest about what that daemon process has done. Neither says anything about the file, which is the thing the operator's next command stages. `pkg hostbuild --deploy` is unaffected, because it captures a baseline within one invocation and fails loudly if the daemon restarts underneath it; every by-hand and post-reboot path is exactly where the answer is missing.

3. **The ISO builder checked less than the update path.** `POST /system/update` reads squashfs's on-disk magic and refuses a fragment with a `400`/`409`. The ISO builder's `required[]` pre-flight checked `access(path, R_OK)` — existence — for all ten of its inputs including this one. So the *media* path validated less than the live-staging path: a fragment could be baked into an installer image, Secure-Boot signed, minisign signed, published to the cache, and first noticed as a machine that does not boot. That check had also drifted into a third inline copy of "is this a squashfs", with `pkg_bootstrap_from_toolchain()`'s copy carrying a comment naming `do_system_update()`'s as its precedent — the shape "No Parallel Implementations" forbids, and the copy that was *not* the check at all is the one that shipped the bug.

## Decision

**The final path only ever holds a whole image.** `mkbootroot` writes to `<out_path>.partial`, and on a successful `mksquashfs` exit `fsync`s that file, `rename(2)`s it onto `out_path`, and `fsync`s the containing directory. `rename` within one directory is atomic, so the final name resolves either to this build's root or to the previous one. The `unlink()` targets the partial and never the final path: **the previous root surviving a failed assembly is the point, not a side effect** — it is the root the machine is currently running from.

The directory `fsync` is not ceremony. A rename is atomic in the namespace but not durable until both the file and the directory entry are on the medium, and the failure being guarded against is a machine that does not boot.

**The file answers for its own freshness.** `GET /v1/system/assembly` reports four fields `stat()`ed on every request: `image_present`, `image_bytes`, `image_mtime`, and `image_complete` (squashfs magic). `image_mtime` is the freshness fact that outlives a restart — an operator who knows when they started a build can identify this root without the daemon remembering anything. `image_complete` asks the same question `POST /system/update` will ask, so a client learns whether that call can succeed instead of discovering it from a `409`.

**Derived, never persisted.** `iso_recover_state()` already made this call for the ISO's `built_version`, in its own words: *"a stored claim about the ISO would be a second source of truth able to disagree with the filesystem, and the thing it describes is already on the filesystem."* The same reasoning applies unchanged here, and it is the reason this ADR adds no completion record beside the artifact.

**One definition of "is this a squashfs".** `squashfs_image_check()` in `daemon/include/squashfsimg.h`, a `static inline` header function for `namecheck.h`'s stated reason and in its shape: pure, stateless, and small enough that the copies were the duplication. Three answers, not two — 1 for the magic, 0 for a file without it, -1 for one that will not open — because the callers say different things about each. The ISO builder's `required[]` gained a per-entry `whole` flag and the control-plane root is the one entry that sets it.

## Alternatives rejected

**Verify at staging time instead, with `unsquashfs -stat`.** A real structural check, and it would have caught this particular fragment. But it validates the *reader's* copy of the problem rather than fixing the writer, it leaves every other consumer (the ISO builder above all) to remember to do the same thing, and it still destroys the previous root on a failed assembly. A correct write makes the deep check unnecessary; a deep check does not make an incorrect write safe.

**Persist a completion record beside the artifact** — the version and timestamp of the assembly that produced the root, written after the rename. This was the first design, and `iso_recover_state()`'s precedent is what ruled it out: it is a second source of truth about a file, able to disagree with the file, needing its own invalidation on assembly start so a leftover record cannot read as "complete". `st_mtime` is the same fact, already maintained by the kernel, and cannot disagree with the file it belongs to.

**Keep the counters as the only freshness answer and document the reboot gap.** The gap is not a documentation problem. The reading a client gets after a reboot is not vague, it is *wrong in a specific direction*: it says nothing has ever been assembled on a host whose entire running system came from an assembly minutes earlier.

**Leave the ISO builder alone as out of scope.** It consumes the same file, and it was checking existence where the update path checks magic — which means the path that produces *signed, published, publicly distributed* media was the laxer of the two. Splitting that into its own issue would have shipped the atomic write while leaving the higher-consequence consumer wrong.

## Consequences

A failed or interrupted assembly now leaves the previous root in place, whole and bootable, and says so through `image_mtime` — a client that mistakes it for a fresh one is reading a field that contradicts it. `image_present` implies `image_complete` on a current build; both stay reported, because a host upgraded from an older build can still be carrying a fragment an interrupted assembly left at that path, and because an operator-supplied `image_path` can be anything at all.

The atomic write needs one more `PATH_MAX` of stack in `run_mksquashfs()` and, transiently, room for two roots in `BOOTROOT_DIR` — 20,434,944 bytes each, measured on 192.168.15.95 for v2.57.200, on a partition sized for the platform's rebuildable state. (The ~10 MB figure `docs/api/README.md` quotes for #178 is an older, smaller root; it is not what this one measures.)

**The gate is split, and one half of it is not automated.** `test_system_update` (in `SELFTESTS`) asserts all three states of the file through the endpoint, and that `image_complete: false` predicts the update path's refusal of the same bytes. The rename itself lives in `mkbootroot`, which needs a real `mksquashfs` and therefore runs in no gate — `test_mkbootroot_firmware`, `test_bootroot_args` and `test_fresh_output_dir` are all outside `SELFTESTS` for exactly that reason (#224). That half is verified live on 192.168.15.95 by interrupting an assembly on purpose: the same accident that produced this ADR, run deliberately.
