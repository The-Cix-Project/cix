# 0375 — The control plane upgrades in place

## Status

Accepted by the owner on 2026-11-06: *"Accept ADR-0375, go ahead"*. Proposed 2026-11-05. Asked for by the owner the same day, after a day of releases had restarted vpn-1 at every reboot: *"it's senseless for this to happen every upgrade?"*, and then *"Continue with what's bar raising"*. It builds on [ADR-0247](0247-the-reactor-does-not-block-and-that-is-the-defence.md), which removed the last attempt to keep containers across a cixd restart and named the prerequisite this ADR takes as its first phase. It changes how [ADR-0327](0327-the-host-follows-its-rolling-packages-in-a-nightly-window.md)'s host roll ends. It changes nothing about the A/B slots or the boot counter.

## Context

**Every cix release reboots the host.** The control-plane root is an image in an A/B slot, and the only way onto a new one today is to reboot into it. A reboot stops every container. So an update to cixd, which runs no workload, takes down every workload: vpn-1, which the owner reaches their network through; dns-1 and dns-2; ldap-1; the agents. Kernel updates are rarer, and cannot avoid it.

**It was tried once and removed, and the reason still holds.** [ADR-0246](0246-the-control-plane-is-one-process-doing-five-jobs.md) built a supervisor that restarted a cixd worker and re-adopted its containers. ADR-0247 removed it, `container_adopt()` included, because *"the whole `--init-mode` startup path assumes a fresh kernel"*: `boot_init()`, the disk remounts, `network_init()` and a dozen `boot_subsystem_init()` calls had never run twice in one kernel's life. Its closing words are this ADR's starting point: *"If a restartable control plane is wanted again, the prerequisite is unchanged and now unencumbered: `--init-mode` startup has to become idempotent first."*

**What a running container is tied to, read from the code on 2026-11-05:**

- **Held only as an open descriptor, with no path to reopen:**
  - each container's cix-init control and report sockets, an anonymous socketpair (`main.c:9007`). cix-init drops the control socket for good on EOF (`init/src/cix_init.c:1428`);
  - the service output pipes, dup2'd onto every service's stdout and stderr (`main.c:9020`);
  - the capture pipe (`main.c:17895`);
  - the device-policy BPF program's descriptor (`container_dev.c:177`).

  Losing any of these breaks the container, or its control.
- **Recoverable by path from a recorded pid:** the cgroup (`/sys/fs/cgroup/cix-workload/<name>`), the pidfd (`pidfd_open`), the network namespace (`/proc/<pid>/ns/net`), and the veth names, which encode the pid (`container_net.c:68`).
- **Held only in memory:** the registry (`registry.c:54`), so the IP allocations and service states as well, and the package chains (`pkg.c:1071`).
- **Inconsistent with an empty registry at start:** autostart *creates* every defined container (`containerdef_autostart_all()`, `main.c:38003`). Run beside live containers, it would clone duplicates into their populated cgroups. It would also hand out their IP addresses (`registry_alloc_ip`).
- **Children that own shared anonymous memory:** procfuse's server (its load page, `procfuse.c:1160`) and stallwatch (its heartbeat page, `stallwatch.c:1016`). Neither page survives an exec.

**What the kernel gives for free:** `execve` keeps the PID. cixd's children stay its children, and a container armed with `PR_SET_PDEATHSIG` sees no parent die. A descriptor survives an exec when `FD_CLOEXEC` is cleared on it, and the repository already passes descriptors that way to cix-init (`keep_fds`, `container.h:593`). cixd is single-threaded: no `pthread_create` in the daemon.

## Decision

**When the staged root carries the kernel that is running, the host roll switches to it in place, and every container keeps running.** When the kernel changes, it reboots in the window as today.

1. **Startup becomes idempotent first** (ADR-0247's prerequisite). Every `--init-mode` step is classified, and the classification is a table a gate reads, not a comment:
   - **once per kernel**, skipped on a handover: kernel mounts, module loads, sysctls, the management network, the uevent socket, console shells, boot confirmation;
   - **idempotent**, run again: bridges, which already tolerate `EEXIST`;
   - **per process**, rebuilt: the reactor, the listeners.
2. **Handover, not adoption.** Before it execs, the running cixd drains:
   - no package job, helper child, park or teardown is in flight, and the roll waits for that;
   - stallwatch is stopped;
   - procfuse's page moves to a memfd, so it can be handed over.

   It then writes one handover record, `STATE_DIR/handover.json`: each container's registry entry with its pid, IP allocations and service states, and the descriptor numbers it is passing, with `FD_CLOEXEC` cleared on each. The new cixd reads the record and takes each container over. It reacquires the cgroup, pidfd and namespace by path, takes the passed descriptors as they are, and then deletes the record. Autostart runs only for definitions the record does not name. **A container is never created twice and never adopted blind:** a record that disagrees with the kernel (no such pid, a cgroup without it) is a failed handover, and the host reboots instead.
3. **The root switch.** The new cixd lives in the staged slot's image, built against that image's glibc, and mixing two glibc builds panics a root (CLAUDE.md records it twice). So the old cixd mounts the staged slot, moves the live mounts into it (data, config, ESP, cgroup, proc, sys, disks), switches root into it, and execs `/bin/cixd --handover` from there. Containers have their own mount namespaces and rootfs, so they are not disturbed. The old root stays referenced until nothing holds it.
4. **The rollback stays the boot counter.** A live switch does not change the default boot entry until the new cixd has run its probation and confirms, as a booted one does today (`confirm_boot()`). If the new PID 1 dies, the kernel panics and reboots into the default entry, which is still the old slot. **A failed switch costs one reboot, which is today's cost for every release.** It can never cost more.
5. **One path.** The host roll is the only caller (ADR-0327, and CLAUDE.md since 2026-11-05: releases reach a host through the window). Its last step becomes "switch when the kernel is unchanged, otherwise reboot". There is no second endpoint and no hand route.

**Phase 2's design, read from the code on 2026-11-06.**

1. **ADR-0247's objection does not apply.** It removed `container_adopt()` because a process that is not a container's parent cannot see that container exit. A handover keeps the PID across `execve`, so the containers stay children of the same process. The new cixd reaps them as the old one did.
2. **A container is rebuilt through the create path, not deserialized.** A hand-written dump of the ~300-line `registry_entry` would be a second definition of that struct, and it would drift the first time a field is added. So:
   - `registry_create()` (`registry.c:105`), which finds a slot, clones through `container_create()` and fills the entry, gets an adopt twin that takes the existing `struct container_handle` instead of cloning. Both share one fill.
   - `create_container_from_body()` (`main.c:15412`, ~2,900 lines) is split into three parts: computing the spec from the body, the host side effects (rootfs seed and snapshot, file staging, PKI, LDAP, the cix-init transport, the clone), and recording. Takeover runs the first and third. Seeding again would discard a live rootfs, so it must never run.
   - That split is phase 2a. It changes no behaviour, is held by the existing daemon tests, and gets its own probe before any handover code.
3. **The record carries only what neither the definition nor the kernel can give**, as a cross-build contract: JSON, a `format` number, the writing build's version, and unknown keys ignored. Per container:
   - its pid and whether it is running or exited (exited-but-kept entries too);
   - `started_at`;
   - the auto-allocated IPs, which exist only in the entry;
   - each service's state, pid and restart count, because cix-init has no status query;
   - a stop in progress (`teardown_kind`, with the escalation re-armed for the grace that remains);
   - the live and pending device grants;
   - the captured output;
   - the descriptor number of each passed role: cix-init control and report, each service pipe, the capture pipe, the BPF program.

   The cgroup, pidfd and network namespace are reacquired by path. The init table is recomputed from the body and not sent again. Two more items go in the record, refined while extracting the plan (2026-11-06):
   - **the staged file paths**, the body's `files[]` plus whichever LDAP client files staging chose to write;
   - **the DNS servers staged into `resolv.conf`**, which default from the DNS servers registered at the time.

   Both are facts about what the old process staged. Recomputing them would copy staging's conditions into a second place. What the plan does recompute is `plan_*()` in `main.c`: capabilities, consoles, sysctls, environment, network attachments without their addresses, interface names, and from those the init table.
4. **Descriptor order:** wait for quiescence, clear `FD_CLOEXEC` on each passed descriptor, write the record last, then exec. On takeover, check each descriptor's type before trusting it (a SEQPACKET socket, a FIFO), and set `FD_CLOEXEC` again.
5. **Where takeover runs:** the record is validated first: every pid alive, each pid in its cgroup, every descriptor typed. A record that fails validation is a refusal. It is applied right after `registry_init()`, before the orphan-base and rootfs sweeps, image retention and autostart, each of which treats an empty registry as the truth. Allocated IPs are marked taken before anything can allocate, and autostart skips the recorded names.
6. **The handover path runs for any `--handover`, not only `--init-mode`.** A daemon-linked test then proves it with a real container: same pid, stats, service output written after the handover, a stop, a crash observed and restarted, and a non-colliding IP for a new container. A record naming a dead pid refuses to start. Outside init mode, that refusal is a nonzero exit, not a reboot.

**Phase 2's build plan, validated 2026-11-06.**

*The rule that settles the open questions:* **a handover changes nothing about which containers run.**
- A running container stays running with the same pid.
- An exited container that is kept stays exited, with its exit status.
- A container the operator stopped stays stopped.
- A crash-restart that was pending still happens.

So `containerdef_autostart_all()` joins `include/startup_steps.h` as once per kernel, and the record carries every registry entry, running or not. `rearm_roll_watches()` stays per process, because the roll state is in the persisted definition.

*Corrections to phase 1:*
1. The handover checks run for any `--handover`, not only `--init-mode`. Outside init mode a refusal is exit 1.
2. **Quiescence means every entry is steady.** No teardown in flight, no helper, no package job, assembly or host roll. Every `fork()` that is not `helper_run()` is checked too: the artifact push, the write-back, discovery, the discard queue's delete, the ISO build, the backup. A stop in progress is excluded by quiescence, not carried.
3. **The record is always written**, even with no containers. On the new side, `--handover` without a record is a refusal, and so is a populated `cix-workload` cgroup without one.
4. A first start in a kernel deletes a record left over from an interrupted handover, and says so.
5. The record lives at `STATE_DIR/handover.json`.

*The record (format 1)*, schema in `daemon/include/handover.h`:
- **Top level:** `format`, `written_by`, `written_at`, `procfuse_pid`, and `containers[]`.
- **Per container:**
  - identity: `name`, `image`, `image_version`;
  - run state: `running`, `pid` (0 when exited), `started_at`, `paused`, and the exit fields when it exited;
  - `restart_at`, the deadline of an armed crash restart (0 for none);
  - flags: `userns_enabled`, `disk_quota_bytes`, `reserve_devices`, `ip_forward`, `capture_requested`, `procfuse_expected`, `init_up`, `ready`;
  - every field of each network attachment, device, pending device and interface record;
  - `file_paths[]` and `dns_servers[]`;
  - each service's runtime state and output descriptor;
  - the descriptors `init_control`, `init_report`, `capture` and `bpf_prog`;
  - `captured_output`.
- **Reader rules:** unknown keys are ignored. A missing required key, or a format newer than this build's, is a refusal.

*The sequence:*

```mermaid
sequenceDiagram
    participant R as host roll / test trigger
    participant O as cixd (old image)
    participant K as kernel
    participant N as cixd (new image, same PID)
    R->>O: hand over
    O->>O: quiescent? (entries steady, no job/helper/fork in flight)
    O->>O: stallwatch_stop()
    O->>K: clear FD_CLOEXEC on cix-init sockets, service and capture pipes, BPF fd
    O->>O: write STATE_DIR/handover.json (always, even empty)
    O->>K: execv(cixd --handover)
    K-->>N: same PID; children, passed fds and mounts intact
    N->>N: E1 validate: format, keys; pidfd_open(pid); pid in its cgroup; each fd typed; FD_CLOEXEC set back
    N->>N: skip once-per-kernel steps (startup_steps.h), autostart included
    N->>N: E2 after registry_init(): per container, plan_*() from the body + record values, registry_adopt(), registry_restore_runtime()
    N->>N: orphan/rootfs sweeps, image retention (registry now populated)
    N->>K: E3 after the epoll fd exists: watch pidfd, cix-init report, output pipes; re-arm pending restarts
    N->>N: unlink the record; log "took over N containers"
```

*If the exec fails*, the old cixd sets `FD_CLOEXEC` back, removes the record, restarts stallwatch, and carries on.

*Files:*
- `daemon/src/handover.c` and `.h`: the record as plain fields, with its writer, reader and kernel validation. It is unit-tested by `test_handover_record`, and the architecture diagram names it.
- `registry.c`: `registry_adopt()`, sharing `registry_fill()`, and `registry_restore_runtime()`. Every write stays in `registry.c`.
- `main.c`: builds the record from the registry, `handover_takeover_one()`, the three call sites, the autostart gate and quiescence.
- `test/test_handover.c`, daemon-linked. Before the handover it sets up:
  - A, running, with a service printing every 500 ms, capture on and a `/dev/null` device;
  - B, an exited oneshot;
  - C, `restart:"always"` with its service killed just before the handover.

  After the handover it asserts A has the same pid, its stats answer, its capture contains a line written after the handover, a stop works, and a new container gets a different IP from A's. B is still exited with its status, and C is restarted. It ends with `test_assert_no_drift()`. Negative case: a record naming a dead pid makes the daemon exit 1.

*Order:*
1. `handover.c` and its unit test.
2. The adoption, the glue and the daemon-linked test.
3. The docs.

Each is one probe; the second is budgeted for two.

*What phase 2 leaves for phase 3:*
- no per-container load figure until procfuse's page is a memfd;
- console shells that survive the exec but are unwatched;
- a partial output line in flight is lost;
- event streams and open consoles reconnect.

## Alternatives

- **Keep rebooting, in the window.** That is today's state, and it is the floor this improves on. It also stays the path for every kernel change.
- **A supervisor above cixd (ADR-0246).** Removed by ADR-0247, and it needed the same idempotent startup. The handover needs no second process at PID 1.
- **Exec the staged cixd without switching root, with its own loader.** Rejected: cbs, curl, mkbootroot and every tool cixd runs would stay the old root's versions. That is CLAUDE.md's four-places-cbs trap applied to the whole root.
- **kexec.** Rejected: a kexec restarts the kernel, which ends every container. It saves firmware time, not workloads.

## Consequences and cost

- **Phases, each one release verified on .95** through the window:
  1. The startup table, and a test that starts a test daemon twice in one data directory with containers running.
  2. The handover record, descriptor passing and takeover, with a test that re-execs a test daemon under a running container and finds it alive and controllable.
  3. The drain: helpers, stallwatch re-forked, procfuse's page as a memfd.
  4. The root switch and the deferred confirmation, verified live by a real roll on .95.
  5. The host roll's last step chooses.
- **All of it is PID 1 code**, where a fault is a kernel panic. That is why rule 4's floor matters: the worst outcome of each phase is the reboot every release costs today.
- **A kernel update still reboots,** in the window. A box whose kernel rolls as often as cix gains less.
- **vpn-1 still restarts when the `vpn` image itself rolls.** A second instance with staggered rolls is the answer to that, and it is a separate decision.
