# 0082 — CONFIG_SMP was never enabled: uniprocessor kernel, and the real root cause of the cpuset gap

## Status

Accepted

## Context

Raised directly by the user reporting real, live-observed memory growth on 192.168.15.95 ("thinc's real host") — investigating that surfaced this, a second and more foundational finding, along the way.

The memory question itself: guest-internal accounting (`GET /v1/system/stats`, sourced from `/proc/meminfo`) showed a flat, tiny ~62MB "used" across repeated polls minutes apart, while the user's own outer view (Proxmox's own reported VM memory) kept climbing from ~460MB toward ~546MB with nothing running. There is no internal leak — `image/kernel/qemu-part1.config` never enabled `CONFIG_VIRTIO_BALLOON`, so this KVM guest has no way to hand physical pages back to the host once touched; QEMU's own host-side RSS for the VM can only grow, regardless of what the guest kernel itself frees internally. Fixed by adding `CONFIG_VIRTIO_BALLOON=y` to the kernel config (see this ADR's own Decision section; not itself the more significant finding this ADR is really about).

While fixing the live-open `--cpuset=` regression (ADR-0081's own atomic-write fix did NOT actually fix `--cpuset=` — memory/pids started working, cpuset kept failing with the identical `cgroup_create: No such file or directory`), reading `init/Kconfig` directly settled it: `config CPUSETS` has `depends on SMP`. `CONFIG_SMP` was never present anywhere in `image/kernel/qemu-part1.config`, and `make allnoconfig` (this project's own documented starting point for every kernel build) defaults it to disabled. Every kernel this project has ever built and deployed, across every phase, has therefore been **strictly uniprocessor** — regardless of how many vCPUs the target VM was actually given. `--cpuset=` could never have worked on any of them, no matter what daemon-side cgroup delegation code ADR-0079/ADR-0081 wrote — there was never a cpuset controller compiled into the kernel at all for it to delegate.

This is not a cosmetic gap. A uniprocessor kernel on a multi-vCPU host means every container, and the daemon itself, has been confined to a single core this entire time — real, silently wasted capacity on every deployment, not just a missing optional feature.

## Decision

Add `CONFIG_SMP=y` to `image/kernel/qemu-part1.config`, with a comment naming both consequences (uniprocessor waste, and the cpuset dependency) so a future reader doesn't have to re-derive either. Also add `CONFIG_VIRTIO_BALLOON=y` in the same pass, for the memory-accounting question this same investigation started from.

Both require a full kernel rebuild from `make allnoconfig` forward (not an incremental `olddefconfig` layered on the previous uniprocessor `.config`) — enabling `CONFIG_SMP` makes a large number of previously-hidden Kconfig symbols reachable for the first time (per-CPU infrastructure, SMP-only scheduler/locking code paths, etc.), and layering onto a stale non-SMP `.config` risks `olddefconfig` making different, less-deliberate choices for those newly-visible options than a clean pass would.

## Consequences

- Every kernel deployment from this point forward is genuinely SMP, using every vCPU the host VM is actually given — a real capacity fix, not just the `--cpuset=` bug it was found investigating.
- `--cpuset=` container creation should now work now that the controller genuinely exists to be delegated (ADR-0081's own fix, which was already correct in what it does -- request only genuinely-available controllers -- was blocked the whole time by cpuset never being available in the first place).
- `CONFIG_VIRTIO_BALLOON=y` alone fixes the reported host-memory-growth question -- but only takes effect once the VM's own QEMU/Proxmox-side configuration actually attaches a virtio-balloon device (a hypervisor-side setting, outside this repository's own scope; the guest-side driver being present is the necessary and now-satisfied precondition, not the whole story if ballooning still isn't visibly reclaiming after this kernel is deployed).
- This finding, once fixed, retroactively explains why raw single-core CPU throughput was never something this project's own testing had reason to question — every load-bearing workload tested so far (lldap builds, kernel hostbuilds, container churn) ran fine on one core, so a uniprocessor kernel never produced an obviously-broken symptom the way the cpuset failure eventually did. Worth remembering as a general lesson: an unused capability failing loudly (cpuset) is how a silent, unrelated capacity gap (SMP) got found at all.
