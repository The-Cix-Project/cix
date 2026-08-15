# 0060 — expose cpu.max and cpuset.cpus through the API/CLI, no reinterpretation layer

## Status

Accepted

## Context

Parts 1 and 2 of the bare-metal-readiness plan (the smallest, highest-confidence parts, sequenced first once Part 0.5 landed): `struct cgroup_limits` already had a `cpu_max` field, and `src/cgroup.c` already wrote it to the real `cpu.max` cgroup v2 file — but the daemon hardcoded it to `NULL` at every container-creation call site, with zero API/CLI surface. CPU affinity (`cpuset.cpus`) didn't exist at all: no field, no controller enablement, no kernel config. Both are the same family of gap — a resource-control mechanism partially or fully built at the runtime-library layer with no way for an operator to actually reach it — so this one ADR covers both, rather than splitting into two near-identical records.

## Decision

**Raw cgroup-native values, passed straight through, never reinterpreted.** `cpu_max` is the literal `"<quota> <period>"` string cgroup v2's own `cpu.max` file expects (microseconds, e.g. `"50000 100000"` = 50% of one CPU); `cpuset_cpus` is the literal `cpuset.cpus` range-list string (e.g. `"0-1,3"`). Neither gets an invented percentage/unit-conversion layer — this matches `memory_max`'s bytes and `pids_max`'s raw count, both already direct pass-throughs with no reinterpretation of their own. An operator who knows cgroup v2 syntax needs no translation layer to reason about; this project's own `docs/api/README.md` documents the raw syntax rather than abstracting it away.

**`cpuset_cpus` needed a new controller-enablement call, `cpu_max` didn't.** `cpu.max` already worked once written (the `cpu`/`cgroup_sched` controller was already enabled via `CONFIG_CGROUP_SCHED`/`CONFIG_FAIR_GROUP_SCHED`) — Part 1 was pure plumbing, no new mechanism. `cpuset.cpus` needed both a new kernel config option (`CONFIG_CPUSETS=y` — a distinct controller from `CONFIG_CGROUP_SCHED`, which only covers bandwidth limiting, not which physical CPUs a cgroup's tasks may run on at all) and a new `cgroup_enable_cpuset()` function, mirroring `cgroup_enable_io_accounting()` (ADR-0054) line for line: best-effort, called once at daemon startup, writes `+cpuset` to the cgroup v2 root's own `cgroup.subtree_control`. Not fatal on failure, same "degrade to unrestricted, never fail container creation over it" posture the io controller already established.

**Both fields skip the response entirely, matching existing convention.** Neither `GET /containers/{name}` nor the `201` create response echoes `memory_max`/`pids_max` back today; `cpu_max`/`cpuset_cpus` follow the same shape rather than introducing an inconsistent echo-back for only the two newest fields. `GET .../stats` (ADR-0054) already reports live `cpu.usage_usec` and `memory.max`; it does not gain a `cpuset.cpus`-reporting field here — that's a real, deliberate scope boundary, not an oversight (the stats endpoint reports usage and enforced limits it can read live, not every creation-time input verbatim).

## Consequences

- Verified for both: `test/test_container_lifecycle.c` creates a container with a real value over HTTP, then reads the value directly back from `/sys/fs/cgroup/<name>/{cpu.max,cpuset.cpus}` — the kernel-authoritative value, not a claim the API response makes about itself. Also manually verified through the real, compiled `thincctl --cpu-max=`/`--cpuset=` flags against a live daemon.
- `image/kernel/qemu-part1.config` gains `CONFIG_CPUSETS=y` — not yet verified by an actual kernel rebuild + boot test at the time this ADR is written; deferred and batched with Part 3's (kernel modules) and Part 4's (disk quota) own kernel config additions, per this plan's own kernel-config-consolidation decision (one rebuild+boot cycle covering all three, not three separate multi-hour cycles) rather than assumed working from the config line alone.
- `docs/api/openapi.yaml`/`docs/api/README.md`/`docs/guides/cli-reference.md` document both fields/flags.
- Parts 3-5 (kernel modules, disk quotas, ISO self-build) remain the larger, less-precedented remainder of the bare-metal-readiness plan.
