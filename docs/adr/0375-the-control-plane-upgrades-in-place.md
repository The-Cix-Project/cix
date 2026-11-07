# 0375 — The control plane upgrades in place

## Status

Accepted by the owner on 2026-11-06: *"Accept ADR-0375, go ahead"*. All five phases built and released in 0.2.57-664, live on 192.168.15.95 since 2026-11-06. That release reached the host by reboot. The first live switch, from 664 to 665 with 14 containers running, was on 2026-11-07 (ROADMAP Part 360). Proposed 2026-11-05. Asked for by the owner the same day, after a day of releases had restarted vpn-1 at every reboot: *"it's senseless for this to happen every upgrade?"*, and then *"Continue with what's bar raising"*. It builds on [ADR-0247](0247-the-reactor-does-not-block-and-that-is-the-defence.md), which removed the last attempt to keep containers across a cixd restart and named the prerequisite this ADR takes as its first phase. It changes how [ADR-0327](0327-the-host-follows-its-rolling-packages-in-a-nightly-window.md)'s host roll ends. It changes nothing about the A/B slots or the boot counter.

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

*Two refinements made while building it, both simpler than the plan above:*
- **A pending crash restart is re-armed, not carried.** The record's `restart_at` is always 0. In E3 the new cixd re-arms every definition that has no entry, is not stopped, and whose policy would restart it (`always`, or `on-failure` after a failure), each with its own backoff. That is the definitions' own answer, with no timer deadline to carry.
- **A stop in progress is waited out, not carried.** Quiescence requires every entry's `teardown_kind` to be none. A stop's grace is seconds, and carrying it would mean re-arming an escalation timer across the exec.

*Built and proven, 2026-11-06 (probe-cix-compile@0.2.57-713):*
- **`test_handover_record`:** writing and reading back, and every refusal.
- **`test_handover`:** a daemon-linked test, with the handover triggered after 12 s. A running container keeps its pid, its address and its stats. A new container on the same network gets a different address. A stop through the API reaches cix-init over the passed socket, and the exit is reaped through the reacquired pidfd.

*What phase 2 leaves for phase 3:*
- no per-container load figure until procfuse's page is a memfd;
- console shells that survive the exec but are unwatched;
- a partial output line in flight is lost;
- event streams and open consoles reconnect.

**Phase 3, the drain completed (2026-11-06).** Two of the three items the phase list below names were already done by phase 2: quiescence waits for every helper child, and stallwatch is stopped before the exec and started again by the new cixd. What remained were the phase 2 limits above, and each is now carried in the record. Format 1 had not shipped, so these are changes to format 1, not a format 2.
- **procfuse.** The load page is a memfd (`memfd_create`, then `MAP_SHARED` of it) rather than anonymous memory, which an exec drops. The record names the server's pid and the memfd, with `FD_CLOEXEC` cleared on the memfd. In E1 the new cixd checks four things: the server is its child, `STATE_DIR/procfuse` is a FUSE mount, the descriptor answers `F_GET_SEALS` (only a memfd does), and its size is the page's. It then maps the page and records the server as its own. A handed-over cixd reports load figures again, gives new containers their binds, and stops the server at shutdown. Before this it leaked the server. A record with no server makes the new cixd start one, as a boot does.
- **Console shells.** They were reachable only through their epoll conns, so cixd now keeps a table of them by tty. A pid of 0 there means a respawn is pending. The record carries the table. The new cixd checks each pid is its child, watches it again, and arms a pending respawn on its usual delay. They exist only in init mode, so `test_boot` is their gate: its success marker is now the takeover line, which reports the shells taken over.
- **A line half read.** Output conns are kept on a list, so the record can carry, per service and for cix-init's own stdio, the bytes read since the last newline. Those bytes seed the new conn's buffer. The conn stays the buffer's owner. Moving the buffer into the registry entry would have cost 1 KiB for every service of every container, for a state that matters for the one instant of an exec.
- **Every pid the record names must be a child of the new process**: containers, console shells and procfuse's server. `pidfd_open` succeeding proves only that the pid is alive. Parentage is what makes it ours to reap, and the exec keeps it.
- **A definition with no entry is brought up by autostart's own rule**, `def_bringup_decision()`, with one difference: a stop holds across a handover, because a handover is not a reboot. Before this, phase 2 had its own test. That test missed two cases. An `on-failure` container whose crash came after a stable run has `consecutive_failures` 0, and it was not re-armed. A definition waiting for its image had the image's place in the in-memory rebuild queue dropped.

*Built and proven:* `test_handover_record` round-trips the new fields, including bytes that are not UTF-8, and refuses a pid that is not a child. `test_handover` now covers the three containers phase 2's plan named (A running with capture, B exited, C with its restart pending), a line split by the handover, the load figure where procfuse runs (not in a build container), and the negative case. A record naming a dead pid makes `cixd --handover` exit 1. `test_boot` asserts a console shell was carried.

*Not yet shown: procfuse's resume.* It runs only when the old cixd had a FUSE server. A build container has no `/dev/fuse` (`CIX_BASELINE_DEVICES`), so `test_handover`'s daemons had none and that test checks no load figure. `test_boot`'s kernel carries `CONFIG_FUSE_FS=y`, so its init-mode cixd most likely had a server. A failed resume refuses the start, so its pass suggests the resume worked. But no line on the console says a server was handed over, so this is inferred, not measured. Phase 4's first probe adds the procfuse state to the printed takeover line and asserts it in `test_boot`.

**Phase 4's build plan: the root switch and the deferred confirmation (2026-11-06).** Read from the code before writing any.

*What there is to switch to.* A slot is a raw squashfs on its own partition (`ROOT_A_DEVICE`, `ROOT_B_DEVICE`). `do_system_update()` writes the inactive one, its kernel to `/boot/cix-bzImage-<slot>`, and a counted entry `cix-<slot>+3.conf` with the newest version. So after an update the next boot tries the new slot three times before falling back. `confirm_boot()` renames the counted entry to `cix-<slot>.conf` once a boot is serving. cix-boot passes over an entry whose tries-left is 0 unless nothing else can boot (`pick_entry()`).

*Preconditions*, all checked before anything is touched. A refusal leaves this cixd running as it was:
- init mode, with `--slot=` set, and the target is the other slot;
- the target has a counted entry, so an update staged it and nothing confirmed it since;
- the staged kernel is the running one: `bzimage_release()` of `/boot/cix-bzImage-<target>` equals `uname -r`. Phase 5 makes this choice, and the mechanism refuses anything else itself;
- quiescence, as phase 2.

*The switch, in a new module `rootswitch.c`.* It is unit-tested where it can be, since PID 1 is where a fault is a panic.
1. **Plan the moves** from `/proc/self/mountinfo`: every mount whose parent is the root mount. Today those are `/dev`, `/proc`, `/sys`, `/config`, `/boot`, `/var/lib/cix`, `/mnt/cix` and the `/etc/resolv.conf` bind. Each carries its submounts, such as devpts, cgroup2, efivarfs, procfuse and volumes. Two mounts on one mount point are refused: a move would take only the top one. The list is read from the kernel, not written down, so a mount added later moves without anyone remembering it.
2. **Mount the target** read-only at `/run/nextroot`. `mkbootroot` stages that empty directory, so every root carrying this code has it. Then check that the new root has `/bin/cixd`, that every planned mount point exists in it as a directory or a file, and that it has `/lib/modules/<release>` whenever the old root had them.
3. **Hold the staged entry**: rename `cix-<target>+N[-M].conf` to `cix-<target>+0-<N+M>.conf` (`esp_entry_hold()`). Until the new cixd confirms, a reboot boots the old slot, so rule 4 holds as written: a failed switch costs exactly one reboot, into the slot that was running. Today's counted reboot would instead retry the new slot three times.
4. **Write the record**, which now also carries the writer's `slot`. It lives in `/config/state`, which is the same path before and after the moves.
5. **Move each planned mount** with `MS_MOVE` under `/run/nextroot`. Host mounts are private (only container namespaces are made private, and nothing marks a host mount shared), so the moves are allowed. If a move fails, the ones already made are moved back, the target is unmounted, the entry is renamed back and the record removed, and this cixd carries on.
6. **Pivot**: `chdir("/run/nextroot")`, `pivot_root(".", ".")`, `umount2(".", MNT_DETACH)`, `chdir("/")`. `pivot_root` moves every process in the namespace whose root or working directory was the old root, which covers procfuse's server and the console shells. A container's processes have their own root and are untouched. The old root stays referenced by whatever still maps a binary from it and goes when they do.
7. **Exec `/bin/cixd`** from the new root, with `--slot=<target>` and `--handover`. The one-shot test triggers (`--test-handover-after=`, `--test-switch-after=`, `--test-update-image=`, `--test-update-kernel=`) are dropped, because each belongs to the process it was given to. A test's configuration, such as `--test-esp-entries-dir=`, passes on. An exec that fails after the pivot cannot be undone, so it syncs and reboots, which boots the old slot (step 3).

*The new side.* A handover whose record names a different slot than this cixd's is a switch. A switched cixd runs `maybe_confirm_boot()` at the point a boot does, with the same definition of healthy: about to serve, management address bound. `esp_entry_to_confirm()` already ranks a counted entry first, so the held `+0` entry is the one renamed to `cix-<target>.conf`. If confirmation never comes, the existing retry timer ends in a reboot, and the held entry makes that a reboot into the old slot.

*What review found before any code, and what it changed:*
- **A handed-over cixd lost three facts the boot computes once**, from phase 1 on:
  - The platform's partitions. `resolve_platform_devices()` runs inside `boot_init()`, so after a handover `ROOT_A_DEVICE`, `ROOT_B_DEVICE` and the others were empty. The next update would have had no slot to write.
  - The listener's address. Only `bootstrap_management_network()` sets `g_bind_addr`, so the successor would have served on loopback alone, unreachable from the network.
  - Whether the uplink attached.

  No handover had run on a host, so none of this showed. The partitions are now read again from `/sys` on every handover. The address and the uplink travel in the record (`bind_addr`, `uplink_unattached`), because only the old process knows them. The kernel module scan's report, which only the boot's sweep fills, now says it ran in the process before, rather than that no sweep ran.
- **A rename on the ESP is made durable** (`esp_rename_durable()`), for the hold, its undo and `confirm_boot()`'s rename. Rule 4's "one reboot, into the old slot" holds only if the hold is on disk before anything can panic. The first cut fsynced only the directory, and probe 715 showed that was not enough. On vfat, a rename writes the new directory entry with size 0 and no first cluster. The renamed file's own inode writeback fills them in later, and a directory fsync does not trigger it. A guest stopped right after the confirmation left `cix-b.conf` at 0 bytes. So the renamed file is fsynced too. Every confirmed boot had the same window before this, and a power cut in it would have left an empty entry.
- **The target must have exactly one entry, and it must be counted.** A stale `cix-<target>.conf` beside a held entry would be booted instead of the slot that was running.
- **The first release that carries this code cannot switch.** The switch runs in the root that is running, and only a root built by this release's `mkbootroot` has `/run/nextroot`. A missing one is a stated refusal. So the first release with phase 4 reaches a host by the window's reboot, and the release after it is the first that can switch.

*Accepted limits.* procfuse's server and the console shells run the old build's code until they next exit. The server's page layout is checked by its size and by a format number now kept at the start of the page. A console shell is a REST client, and its next respawn runs the new `cixctl`.

*Tests.* A new, gated `test_rootswitch` covers `esp_entry_hold()` and the mount planner against crafted `mountinfo` text. `test_esp` is outside the gate. In `test_boot_update`, slot A updates slot B and is then told `--test-switch-after=5`. It switches into B in place, under the same kernel, and B confirms. The console must show each of these once: the update, the switch, the second start with `slot=b`, and the confirmation. The ESP must then hold `cix-b.conf` and no counted B entry. A cold power-on boots B, as before. The phase 3 leftovers ride along: the takeover line prints procfuse's state and `test_boot` asserts it, `container_output_line()` sizes its buffer from `HANDOVER_OUTPUT_LINE_MAX`, and the dead-pid case asserts its reason.

*Built and proven, 2026-11-06 (probe-cix-compile@0.2.57-716, commits 24c4eabf and 0d3c4f56).* `test_rootswitch`, `test_handover`, `test_boot`, `test_boot_ab` and `test_boot_update` all pass. Under QEMU, slot A updated slot B, switched its root into B in place under the same kernel, and B's cixd started with `slot=b`. It took over two console shells, resumed procfuse with its load page, and confirmed B: the ESP held `cix-b.conf` with its content and no counted B entry. A cold power-on then booted B. Probe 715 had shown everything up to the confirmation and an empty `cix-b.conf`, which is the vfat finding above. `test_boot` now asserts that procfuse is resumed with its load page, so phase 3's resume, which no probe had shown, is measured.

**Phase 5: the host roll chooses (2026-11-06).** `hostroll_staged()` rebooted after every stage. When the roll staged the running kernel (`stage_kernel` 0, so `do_system_update()` copied the active kernel forward), it now enters `HOSTROLL_SWITCH`. `hostroll_switch_tick()`, run from the loop where the test trigger runs, waits for quiescence and calls `handover_exec(slot)`. Four outcomes, and none costs more than a release did before:
- **switched:** the containers keep running, and the new cixd confirms its slot;
- **refused or failed before the pivot:** everything is undone, and the roll reboots into the staged slot as before;
- **failed after the pivot:** a reboot into the slot that was running (phase 4);
- **work still in flight at the bound:** the roll reboots as before. The bound is the window's end less a minute, or `HOSTROLL_SWITCH_WAIT_SECONDS` with no window.

**A handed-over cixd that cannot start reboots; it does not park.** Until phase 5, a non-zero return from `cixd_main()` as PID 1 parked behind its banner (#131), whether the start was a boot or a handover. For a handover that broke rule 4's floor: after a refused record or takeover, containers would still be running with no API and no way back. `main()` now syncs and reboots after a failed `--handover` start. After a switch, the held entry boots the slot that was running. After an in-place handover, the slot is unchanged.

A kernel change still reboots. The roll is the only caller, as rule 5 says. The decision has no QEMU test of its own, because the roll needs a hostbuild. Its parts are tested: the switch by `test_boot_update`, the refusals by `test_rootswitch`. Its live proof needs two releases. The first release carrying phase 4 reaches .95 by reboot, because the root running there has no `/run/nextroot`. The release after it is the first the roll can switch into.

## Alternatives

- **Keep rebooting, in the window.** That is today's state, and it is the floor this improves on. It also stays the path for every kernel change.
- **A supervisor above cixd (ADR-0246).** Removed by ADR-0247, and it needed the same idempotent startup. The handover needs no second process at PID 1.
- **Exec the staged cixd without switching root, with its own loader.** Rejected: cbs, curl, mkbootroot and every tool cixd runs would stay the old root's versions. That is CLAUDE.md's four-places-cbs trap applied to the whole root.
- **kexec.** Rejected: a kexec restarts the kernel, which ends every container. It saves firmware time, not workloads.

## Consequences and cost

- **Phases, each one release verified on .95** through the window:
  1. The startup table, and a test that starts a test daemon twice in one data directory with containers running.
  2. The handover record, descriptor passing and takeover, with a test that re-execs a test daemon under a running container and finds it alive and controllable.
  3. The drain: helpers, stallwatch re-forked, procfuse's page as a memfd. Done with phase 3 above; the first two had come with phase 2.
  4. The root switch and the deferred confirmation, verified live by a real roll on .95.
  5. The host roll's last step chooses.
- **All of it is PID 1 code**, where a fault is a kernel panic. That is why rule 4's floor matters: the worst outcome of each phase is the reboot every release costs today.
- **Connections to cixd do not survive the exec.** An event stream or an open container console drops and its client reconnects; the containers behind them carry on. A console shell on the host's own console survives, because it is a process cixd watches, not a connection.
- **A kernel update still reboots,** in the window. A box whose kernel rolls as often as cix gains less.
- **vpn-1 still restarts when the `vpn` image itself rolls.** A second instance with staggered rolls is the answer to that, and it is a separate decision.
