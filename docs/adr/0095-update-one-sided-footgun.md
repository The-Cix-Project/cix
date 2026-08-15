# 0095 — POST /system/update's one-sided image_path/kernel_path footgun

## Status

Accepted

## Context

`POST /system/update` (ADR-0031, ADR-0032) has always let `image_path` and `kernel_path` be supplied independently — write just the root, just the kernel, or both. In practice this was a real, previously-worked-around footgun rather than a genuine convenience: whichever of the two was omitted left the inactive slot's own file exactly as it was from whenever that slot was last written, sometimes updates ago. A root-only update could land in a slot whose kernel predated a since-fixed config, and the next boot into that slot regressed to a bug already fixed elsewhere in the fleet — reproduced directly this project's own history (tracked as task #671, worked around by manually resupplying both `--image=`/`--kernel=` on every call from `docs/guides/remote-development.md`'s own documented deploy loop, never fixed at the source until now).

## Decision

`do_system_update()` now auto-fills whichever of `image_path`/`kernel_path` is omitted from the **active** slot's own currently-running copy — the root squashfs device this daemon actually booted from, and `<ESP>/thinc-bzImage-<active-slot>` — rather than leaving the inactive slot's prior content in place. Both are always written to the inactive slot on every successful call as a result; the response's own `updated` field is simplified to always report `["root", "kernel"]`, since both genuinely are fresh afterward regardless of which the caller explicitly supplied. Explicitly supplying both continues to work exactly as before — this only changes what happens when one is omitted, and reuses the exact same `write_file_to_device()`/`write_file_to_esp()` primitives already used for an explicit path (a block device's own `read()` returns 0 at its real capacity the same way a plain file's does at EOF, so no new copy primitive was needed for the active-device case).

## Consequences

- The manual "always resupply both, even the unchanged one" discipline this project's own guides have documented and required since early in the project is no longer necessary — kept documented as history/context, not as an ongoing requirement, in `docs/guides/remote-development.md` and `docs/guides/kernel-build-and-ab-updates.md`.
- Live-verified the unaffected explicit-both-paths case end to end via the existing QEMU-based `test_boot_update` (real device/ESP writes, a second real power-on confirming the freshly-written slot boots) — 3 consecutive clean runs, matching this project's own established verification bar for this test. The new auto-fill fallback path itself (only one of the two paths supplied) was verified by code review and the existing `test_system_update` sweep, not by a new dedicated QEMU scenario — adding one would need a real second-partition fixture beyond what this pass covers; a real, explicitly acknowledged gap, not silently glossed over.
