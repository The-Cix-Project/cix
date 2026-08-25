# 0187 — Bound everything else: a kernel-enforced reservation for the control plane

## Status

Accepted

## Context

On an installed Cix host the REST daemon is not one management path among several — it is the only one. There is no SSH and no general shell (ADR-0034). A `cixd` that stops answering is therefore not a degraded box; it is a box nobody can reach until someone walks to the hypervisor.

That is exactly what happened on 192.168.15.95. Four concurrent package builds oversubscribed a 2-CPU machine and the daemon stopped answering HTTP. The kernel was healthy throughout — ping 0% loss, 0.26ms — so nothing had crashed and nothing had run out of memory. The control plane had simply been pushed off the run queue by workloads it was supposed to be managing, and the machine needed a hypervisor-level reset.

Issue #85 fixed the specific trigger: `pkg-build-config`'s CPU and memory limits were applied per build container while `max_concurrent_jobs` defaulted to 10, so the configured ceiling silently multiplied by concurrency. That removed one way to starve the daemon. It did not remove the class — a runaway container, an unlucky mix of workloads, or any future subsystem that spawns work can reach the same state.

Two of issue #86's three parts were already done: `nice -20` and `oom_score_adj -1000`, set at startup, best-effort. Both are real and both help. Neither is a guarantee: nice is a hint about who wins a contended CPU, not a floor, and OOM protection says nothing about scheduling at all.

## Decision

**Bound everything that is not the control plane, rather than prioritising the control plane.**

Every container and every package build is created as a leaf under one `cix-workload` cgroup, whose ceiling is the machine minus a configured reservation. Builds nest one level deeper (`cix-workload/cix-pkgbuild`) so that the build budget sits *inside* the workload budget rather than beside it — beside it would be two ceilings that add up to more than the machine, which is the same mistake #85 was in a different place. What the control plane has left is then a kernel-enforced remainder rather than a hope.

**The reservation is expressed as what the control plane keeps** — `cpu_percent` and `memory_bytes` — not as what workloads may have. An operator reasons about "leave the daemon a tenth of the box", and that reasoning stays correct when the box is replaced by a bigger one. The ceiling is derived from live host totals on every apply, so the same configuration means the same thing on a 2-CPU VM and on a 32-core machine. This matters immediately: this project's next install is real hardware, and every number here was first chosen against a small VM.

**On by default** (10%, 512 MiB), with `cpu_percent` capped at 50 and `memory_bytes` floored at 64 MiB. A reservation that has to be discovered and switched on is a reservation nobody has when they need it, and the failure it prevents is the worst one this platform has. The caps keep it a safety margin: reserving most of the machine would be a second workload budget wearing a safety margin's name.

**Disabling it removes the ceilings, not the hierarchy.** Containers and builds stay exactly where they are in the cgroup tree; only the limits appear or disappear. Moving the topology with the setting would make turning the reservation off a migration, and would leave already-running containers accounted somewhere their successors are not.

Changes apply to the live cgroup immediately, not at the next container creation — an operator raising the reservation because the box is under strain needs it while it is under strain.

## Alternatives considered

**Real-time scheduling for the daemon (`SCHED_FIFO`).** A genuine floor rather than a hint, and it would have prevented this incident. Rejected because it inverts the failure: a control plane that can never be preempted and then spins, or blocks in a syscall while holding the CPU, takes the machine down harder than starvation did — and this daemon runs as PID 1 on an installed host, where there is nothing above it to recover from that. Reserving capacity leaves the scheduler's own fairness intact.

**Per-container limits, made mandatory.** Every container must declare CPU and memory, and the daemon refuses ones that do not. Rejected for the reason #85 exists: N containers each within its own limit still sum to more than the machine. A per-thing limit is not a budget, however strictly it is enforced.

**Leave it to the operator to configure.** Rejected as the default. The mechanism should be there before the incident, not after it — and the operator who most needs this is the one who has not thought about it yet.

## Consequences

A runaway workload can now make the box slow, and cannot make it unreachable. That is the property this platform actually needs, given that losing the daemon means losing every way in.

Container cgroups moved from `/sys/fs/cgroup/<name>` to `/sys/fs/cgroup/cix-workload/<name>`. Anything reading those paths directly had to move with them — two test helpers did. The daemon itself never rebuilds that path (it holds an open `cgroup_fd` from creation), which is why pause/unpause, stats and device policy needed no change at all.

Controllers are now delegated to a parent's subtree one at a time rather than in a single write. A single write is all-or-nothing: one controller the kernel does not offer at that level fails the whole line and silently delegates nothing. That is what made containers requesting a `cpuset` fail with a bare 500 the moment they moved under a parent — the child had no controller to write to.

`GET /v1/system/control-plane-reservation` reports the derived ceiling alongside the stored numbers, so "10 percent" can be read as the actual `cpu.max` the kernel is enforcing without anyone recomputing it.
