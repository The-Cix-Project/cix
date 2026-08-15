# Administration

Day-2 operations for an already-installed box: watching it, backing it up, and managing its storage. This guide is task-oriented — for field-by-field detail on any endpoint here, see [`docs/api/README.md`](../api/README.md); for the CLI's exact flag syntax, see [`cli-reference.md`](cli-reference.md).

## Monitoring

**Host-wide resource usage** — `kanxeoctl host-stats` (`GET /system/stats`, ADR-0073/ADR-0074): load average, CPU/memory/disk usage, per-interface network counters, and cgroup v2 pressure-stall (PSI) figures for CPU/memory/disk. Raw cumulative counters, no server-side history — call it again for a fresh point-in-time snapshot, or watch it live on the web dashboard's System > Monitoring > Host Stats page (four graphs, no charting library, polls only while that page is open). A single container's own usage is the equivalent `kanxeoctl stats NAME` (`GET /containers/{name}/stats`).

**Every real process on the box** — `kanxeoctl process ls` (`GET /system/processes`, ADR-0131): a direct `/proc` scan, each process correlated to a container if any (by walking its real host ppid chain against every running container's own root pid). `kanxeoctl process kill PID` sends a real, immediate `SIGKILL` — refuses pid 1 and this daemon's own pid outright, since either would crash or reboot the whole host on a real installed system. Killing a pid that happens to belong to a running container is exactly equivalent to that container crashing on its own; the container's own restart policy still applies. The web dashboard's System > Monitoring > Processes page is fetch-on-demand (a Refresh button, not folded into the poll loop) since a real process table churns too fast for a 2s auto-refresh to be anything but noisy.

**The consolidated log** — `kanxeoctl logs` (`GET /system/logs`, ADR-0070/ADR-0126): one chronologically-interleaved store covering real kernel `dmesg`, `kanxeod`'s own internal diagnostics, a per-request audit trail (every mutating `POST`/`PUT`/`DELETE` any client made — `GET` requests are excluded, a query is never an action), and every container's own stdout/stderr, captured transparently with no opt-in needed. Filter with `--source=`, `--level=`, `--container=`, `--regex=` (POSIX extended, case-insensitive), `--tail=`, `--since=`. `kanxeoctl logs config` shows or sets the store's size cap (`--max-bytes=`) and minimum severity floor (`--min-level=`) — the floor is checked at write time, the `--level=` filter above only ever filters what's already stored. The web dashboard's bottom log panel (every page, collapsible) shows this same stream live, merged with the dashboard's own client-side action log, filterable by source.

**Optional external syslog forwarding** — if you already run syslog tooling and want this platform's container logs to also reach it, register a running syslog-server container (e.g. `syslog-1` running `sysklogd`) as a forward target: `kanxeoctl syslog target register --container=NAME` (ADR-0127). Every container-sourced log line is then also sent as a real RFC 3164 UDP datagram — alongside, never instead of, the consolidated log store above, which stays the one source of truth this API and the web UI ever read from.

## Backup and restore

```sh
kanxeoctl backup --output=backup.json
```

Bundles container definitions, networks, DNS records, package install state + recipes, and site config into one file, saved byte-for-byte for later use with `kanxeoctl restore --input=backup.json`. Read the fine print before relying on this for disaster recovery — **does not** include container workload data (a database's own files, a git host's repos — back those up with their own native tooling), image rootfs content (reproducible by re-running `pkg install`, since everything here is compiled from source — the bundle is the "shopping list," not the built bytes), or anything PKI-related (a CA/leaf private key is never returned over this API anywhere, by design — back up `/var/lib/kanxeo/state/pki/` (ADR-0141) separately, directly on the host). Full detail: [`docs/api/README.md`](../api/README.md#backup-and-restore).

Restoring does not take effect immediately or reboot for you — a typical disaster-recovery sequence is: boot a fresh install → `kanxeoctl restore --input=backup.json` → `kanxeoctl reboot` → the second boot comes up with the restored state. A scheduled backup is just this same command run on a cron entry (or from a container with network reachability to `kanxeod`) — `kanxeoctl` is a plain REST client either way, nothing special about running it unattended.

## Disk management

`kanxeoctl disks` lists every real host block device, live-enumerated on every call, including any partitions already on it — flagging which one is the fixed OS disk (and, transitively, every one of its own partitions) — never a candidate for a role, formatting, or repartitioning. Every other disk (or partition — a partition is addressable everywhere a whole disk name is, once it exists) goes through two explicit, separate steps before it holds anything:

```sh
kanxeoctl diskrole create --disk=sdb --role=container-storage
kanxeoctl disks format sdb --fs-type=btrfs
kanxeoctl disks format-status sdb
```

1. **Assign a role** (`container-storage` or `backup` — a small, fixed vocabulary) — has no destructive side effect of its own.
2. **Format + mount** — destructive, requires an already-assigned role, and double-confirms the disk name before touching anything. Defaults to `ext4`; `--fs-type=btrfs` is the other supported option (ADR-0104). Async — poll `format-status` for completion (`state`: `none`/`running`/`ready`/`failed`).

A `container-storage`-role, formatted disk can then be selected per container at creation time (`run --disk=NAME`, ADR-0102) instead of the default location. Independently, a container can also be given a real, kernel-enforced disk quota on its own overlay storage (`run --disk-quota=BYTES`) — ext4 project quotas or btrfs qgroups, picked automatically from the backing filesystem, not something a disk role or format choice affects.

### Partitioning a data disk (task #844)

A whole disk with no role or existing partitions of its own can instead be split into several independently role-assignable partitions:

```sh
kanxeoctl disks partition-table sdc
kanxeoctl disks add-partition sdc --name=containers --size-mib=51200
kanxeoctl disks add-partition sdc --name=backups
kanxeoctl diskrole create --disk=sdc1 --role=container-storage
kanxeoctl diskrole create --disk=sdc2 --role=backup
kanxeoctl disks format sdc1
kanxeoctl disks format sdc2
```

`partition-table` writes a fresh, empty GPT table — destructive (wipes anything already on the disk), same double-confirmation posture as `format`. `add-partition` appends one partition at a time (never disturbs an existing one); omit `--size-mib` on the last one to consume all remaining space, the same convention the installer's own fixed OS-disk layout already uses internally. Each resulting partition (`sdc1`, `sdc2`, ...) is then just an ordinary disk name everywhere else in this API — `diskrole create`/`disks format` need no partition-specific syntax at all. `disks rm-partition sdc sdc2` removes one partition (`409` if it still has a role assigned — remove the role first, same as any other disk).

A disk is used in exactly one of two mutually-exclusive modes: role assigned directly to the whole disk, or partitioned with roles assigned to the individual partitions instead — `partition-table`/`add-partition` both refuse to touch a whole disk that already has a role of its own.

**Host swap**, if a package build (Rust/wasm builds are the confirmed real-world case) runs a box out of RAM: `kanxeoctl swap enable --size-mb=8192` / `kanxeoctl swap disable` / bare `kanxeoctl swap` to check current state (ADR-0069). A single on-demand file, off by default, persisted and re-applied automatically on every daemon start including a real reboot.

## Does installing a package onto an image reach containers already running from it?

**No — not on its own, by deliberate design (ADR-0107/0108).** Every install/upgrade/uninstall against an image produces a new, immutable, content-addressed rootfs version; nothing is ever mutated in place. A container pins the specific image version it was created against (`registry.json`'s own `image_version`) and keeps running against that exact rootfs forever, even after the image moves on to a newer version — its overlay lowerdir points at a different on-disk directory than the one the new version lives in, so there's no live content for it to pick up. This is the answer to a real, previously-surprising symptom: a package installed onto an image doesn't show up in an already-running container started from that image, only in one created (or recreated) afterward.

Two ways to actually get a running container onto new content, neither automatic unless you ask for it:
- **Recreate it** — `kanxeoctl rm NAME` + `kanxeoctl run ...` again (or re-`POST`/re-apply a [container recipe](../adr/0151-container-recipes.md)) re-resolves the image's current version at that moment.
- **`follow_rolling: true`** at creation time (`run --follow-rolling`, [ADR-0124](../adr/0124-pkg-redesign-part5-rolling-containers-and-restart-jitter.md)) — the daemon detects the pinned image's `current_version` advancing (a rolling auto-rebuild or a manual `pkg install`) and live-restarts the container onto the new pin on its own, spread out with jitter (`--follow-rolling-jitter-seconds=`, or the daemon-wide default via `kanxeoctl rolling-config`) so many containers following the same image don't all restart at once.

For patching a single file into an already-running container without a full recreate — a live config tweak, not a package install — see [`PUT /containers/{name}/files`](../api/README.md#writing-a-file-into-an-existing-container-live-without-a-recreate) ([ADR-0153](../adr/0153-container-file-live-update.md)); it's live and ephemeral, not a substitute for either option above.

## Keeping the box current

Backups and disk management are day-2 operations that don't change what's running on the box; updating the control plane or installed packages does — see [`staying-updated.md`](staying-updated.md) for that, and [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) for kernel updates specifically.
