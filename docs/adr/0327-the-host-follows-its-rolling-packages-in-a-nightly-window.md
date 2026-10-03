# 0327 — The host follows its rolling packages, in a nightly window

## Status

Accepted, 2026-10-03, the owner's answer: *"nightly window at 03:00"*. It extends ADR-0323 (every package can roll) from containers to the host itself, and uses ADR-0257's scheduler, ADR-0031's A/B update and the boot counter as they are.

## Context

ADR-0323 made a release roll from discovery to an authored recipe, and on into a container, because a container follows its image. The host does not: its two packages, the kernel and cix (the control-plane root), are hostbuilt into `__hostbuild`. On 192.168.15.95 no image manifest lists either (measured 2026-10-03). A newer kernel recipe therefore waited for someone to run `pkg hostbuild`, assemble, stage and reboot. The owner's question, *"how will this be a rolling release?"*, had no answer for the host.

Two facts measured in the code shape the answer. The control-plane root carries the kernel's modules, which the assembly takes from the kernel hostbuild's artifact (`ARTIFACTS_DIR/kernel/lib/modules`). And `POST /v1/system/update` with only a root copies the running kernel. So a kernel roll is: build the kernel, assemble a root with its modules, and stage both together.

## Decision

**A scheduled action, `system.roll`, brings the host to its newest packages and reboots into them within the job's window.**

- **What is behind:** for the kernel and cix, the same comparison `available_version` and `update-all` use (`pkg_entry_drift()`). That respects package policy, so a version pin holds the kernel. Also, whether what runs (`uname -r`, the build version) differs from what is installed.
- **The steps:**
  1. Hostbuild the kernel if it is behind, then cix if it is behind. One at a time, kernel first, because the root's assembly reads the kernel's modules.
  2. Assemble the root. A cix hostbuild starts this itself.
  3. Stage the root, and the kernel if it moved, on the inactive slot through `do_system_update()`, the one way a slot is staged.
  4. Reboot.
- **The window:** the action keeps working after it returns, so the scheduler tells a running action when its window ends (`scheduler_running_window_end()`). A roll that finishes inside the window reboots. One that finishes after stays staged and reboots nothing, and the next night finds the host not running what it built, and stages and reboots.
- **The rollback is the boot counter.** A staged slot that does not confirm its boot falls back to the slot that ran. Nothing new is needed for it.
- **A failure stops the roll and stages nothing.** A failed hostbuild, assembly or staging is logged with its cause, and the host keeps running what it ran.
- **The default:** a host that never saved a schedule file starts with `host-roll`, `system.roll` daily at 03:00 for 3h, beside ADR-0316's `recipe-sync`. A host with a saved schedule file, like 192.168.15.95, gets it by an explicit `PUT /v1/schedules/host-roll`. An operator who deletes it keeps that choice.
- **Heavy builds stay serial:** the roll starts only when no hostbuild is running, and runs its own builds one after another (#362).
  A 03:00 kernel hostbuild can still coincide with a queued rolling rebuild of an image, exactly as a manual hostbuild can today; the shared build budget (ADR-0165) is what bounds that.

## Consequences

- A kernel.org stable release reaches the host with no person involved: discovered, authenticated, authored, built, staged, rebooted in the window, and rolled back by the boot counter if it does not boot.
- The host reboots on its own. That is the owner's choice of when, and an operator who wants to choose the moment edits or deletes `host-roll`.
- The roll's state is not persisted: a daemon restart mid-roll ends it, and the next window starts again from what is installed and running.
