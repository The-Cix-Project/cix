# 0349 — A container's load average is sampled from its own threads

## Status

Accepted, 2026-10-22, for #625. The owner agreed the mechanism on 2026-10-21: sample every 5 s in procfuse's own process, count R and D threads, apply the kernel's averaging, serve the same figure in `/proc/loadavg` and in `GET /v1/containers/{name}/stats`, and leave the host's load in the status bar. Rules 2, 4 and 5 below were settled while writing this ADR. They were reported to the owner with the release that implements it.

## Context

Read from the code at `34ccbd94`.

ADR-0262 lists `/proc/loadavg` among the files synthesised "from `cpu.stat`, the container's own start time, and `memory.swap.*`". The code does not do that. `render_loadavg()` (`daemon/src/procfuse.c`) copies the host's `/proc/loadavg` through, and its comment explains why: the kernel keeps no load average per cgroup. So `uptime`, `top`, `w` and any monitoring agent inside a container report the whole host's load as the container's own. `make -l` and `ninja -l` inside a build container throttle on the host's load too.

Every file procfuse serves today is computed fresh at each read, from the reader's cgroup. A load average cannot be computed that way. It is an exponential average over time, so something has to sample at fixed intervals and keep the result.

LXCFS solves the same problem with an opt-in loadavg. Its sampler runs every 5 s, counts the cgroup's runnable and uninterruptible threads, and applies the kernel's own fixed-point formula.

## Decision

**procfuse samples every container cgroup's runnable and uninterruptible threads every 5 s, in its own process, and keeps the kernel's three averages for each cgroup in a page cixd can read. `/proc/loadavg` and the stats endpoint serve that one figure.**

1. **Where, and how often.** The sampler runs in the procfuse server process, which is already forked and already allowed to block. Its loop waits in `poll()` on the FUSE descriptor until the next 5 s deadline on `CLOCK_MONOTONIC`. When a pass falls behind, the next deadline counts from the moment the late pass ran, so passes never pile up. 5 s is the kernel's own `LOAD_FREQ`, which is the interval its exponents are computed for. cixd's event loop does none of this work.

2. **What is counted: threads, over the cgroup's whole subtree, by one function.** A pass walks `/sys/fs/cgroup/cix-workload` and reads each cgroup's `cgroup.threads`. For each thread it reads `/proc/<tid>/stat` and counts state `R` (running) and `D` (uninterruptible). Each cgroup's figure covers its own threads plus those of every cgroup beneath it, so a container that delegates sub-cgroups (`CLONE_NEWCGROUP`) is still counted whole at its root. The kernel's load average counts tasks, which are threads, so walking `cgroup.procs` would undercount every multi-threaded process.
   - The live scan behind `/proc/stat`'s `procs_running` and `procs_blocked`, and behind `/proc/uptime`'s start time, becomes the same thread scan over the same subtree. The two files therefore agree, and `/proc/stat` now counts threads, as the host's does.
   - Rejected: keeping that scan on `cgroup.procs`. `/proc/stat` would then count processes and `/proc/loadavg` threads, and the two would disagree inside one container.

3. **The kernel's arithmetic, exactly.** Each average uses `calc_load()`: `FSHIFT` 11, `FIXED_1` 2048, and `EXP_1`/`EXP_5`/`EXP_15` of 1884/2014/2037. The active count is `(R + D) * FIXED_1`, and `FIXED_1 - 1` is added when the active count is at least the old average. The figures are formatted the way `/proc/loadavg` formats the host's: add `FIXED_1/200`, then `LOAD_INT` and `LOAD_FRAC`. So a container's `0.63` means what `uptime` means on a host. The API serves the same two-decimal figure, never a recomputation in floating point.

4. **One page, shared the way stallwatch shares its page.** The table lives in a `MAP_SHARED | MAP_ANONYMOUS` mapping created before the fork, as `stallwatch.c` creates its own. It has 4096 entries.
   - Every field is `volatile`, and each entry carries a sequence counter that is odd while the child writes it. On x86-64, stores are not reordered with other stores and loads not with other loads, and the compiler keeps `volatile` accesses in program order. So a reader that sees the same even counter before and after its copy has a consistent entry. A reader that cannot get one after a bounded number of tries treats the entry as absent.
   - Rejected: a socket or a file between the two processes. Either would need a request and a reply on every read, where the page needs none.

5. **An entry is keyed by its cgroup path, and starts over when its occupant changes.** The key is the path below `/sys/fs/cgroup`, such as `cix-workload/ar-1`. Both readers can name it without asking anything: `render_loadavg()` already has the reader's cgroup directory, and the stats handler reads it from the container's own cgroup descriptor (`/proc/self/fd/<cgroup_fd>`), which finds a build container under `cix-pkgbuild` as surely as a service.
   - An entry restarts at zero when the oldest start time in its subtree changes. That is the rule `/proc/stat`'s idle floor already uses (`idle_monotonic()`), so a recreated or restarted container never inherits its predecessor's load.
   - A cgroup that is gone at the end of a pass frees its entry.
   - Rejected: keying by the cgroup directory's inode. It would be a second recycling rule beside the idle cache's, and both readers would need an extra `stat()`.

6. **What each surface serves.**
   - **`/proc/loadavg`:** the reader's own entry. Field 4 is that subtree's `running/threads`.
   - **Field 5, the last pid:** stays the host's. The honest value is `ns_last_pid`, which can only be read from inside the container's pid namespace, so serving it would cost a fork per read. Nothing that reads the file (`uptime`, `top`, `w`) uses that field.
   - **Readers with no entry:** a reader outside `cix-workload`, or one whose cgroup has no entry yet, reads the host's line, as before.
   - **`GET /v1/containers/{name}/stats`:** gains `load` with `load1`, `load5` and `load15` (the names `GET /v1/system/stats` already uses for the host), plus `running` and `threads`. It is present only when the container has an entry. Absent means not sampled yet, never zero.
   - **Dashboard:** the container page shows the same figure, or says it has not been sampled yet. The status bar's load stays the host's.

7. **Past capacity, said once.** A cgroup that finds no free entry, or whose path does not fit in one, is not recorded. Its readers get the host's line and its stats carry no `load`. The child counts such cgroups in the page's header, and the stats handler logs the count once per daemon run when it first sees it non-zero. The child does not write the log store itself, because it shares no writer with cixd's.

## Consequences

- procfuse is stateful for one file. Everything else it serves is still computed at read. ADR-0262's list is corrected in place to say where `/proc/loadavg` comes from, which is a factual correction, not a reversal.
- A pass costs one `/proc/<tid>/stat` read per thread across all containers, every 5 s, in a process that blocks nothing else. A cgroup nested *k* levels deep is scanned once per pass, not *k* times, because the walk sums bottom-up.
- A new container's averages start at 0 and climb over the first minutes, as a host's do after boot.
- Build containers get their own load too, so `make -l` inside one throttles on the build's own load rather than the box's.
- A container whose cgroup has no entry behaves exactly as every container did before this ADR.
