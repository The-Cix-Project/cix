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

Bundles container definitions, networks, DNS records, package install state + recipes, and site config into one file, saved byte-for-byte for later use with `kanxeoctl restore --input=backup.json`. Read the fine print before relying on this for disaster recovery — **does not** include container workload data (a database's own files, a git host's repos — back those up with their own native tooling), image rootfs content (reproducible by re-running `pkg install`, since everything here is compiled from source — the bundle is the "shopping list," not the built bytes), or anything PKI-related (a CA/leaf private key is never returned over this API anywhere, by design — back up `/var/lib/kanxeo/pki/` separately, directly on the host). Full detail: [`docs/api/README.md`](../api/README.md#backup-and-restore).

Restoring does not take effect immediately or reboot for you — a typical disaster-recovery sequence is: boot a fresh install → `kanxeoctl restore --input=backup.json` → `kanxeoctl reboot` → the second boot comes up with the restored state. A scheduled backup is just this same command run on a cron entry (or from a container with network reachability to `kanxeod`) — `kanxeoctl` is a plain REST client either way, nothing special about running it unattended.

## Disk management

`kanxeoctl disks` lists every real host block device (whole disks only), live-enumerated on every call, flagging which one is the fixed OS disk — never a candidate for a role or for formatting. Every other disk goes through two explicit, separate steps before it holds anything:

```sh
kanxeoctl diskrole create --disk=sdb --role=container-storage
kanxeoctl disks format sdb --fs-type=btrfs
kanxeoctl disks format-status sdb
```

1. **Assign a role** (`container-storage` or `backup` — a small, fixed vocabulary) — has no destructive side effect of its own.
2. **Format + mount** — destructive, requires an already-assigned role, and double-confirms the disk name before touching anything. Defaults to `ext4`; `--fs-type=btrfs` is the other supported option (ADR-0104). Async — poll `format-status` for completion (`state`: `none`/`running`/`ready`/`failed`).

A `container-storage`-role, formatted disk can then be selected per container at creation time (`run --disk=NAME`, ADR-0102) instead of the default location. Independently, a container can also be given a real, kernel-enforced disk quota on its own overlay storage (`run --disk-quota=BYTES`) — ext4 project quotas or btrfs qgroups, picked automatically from the backing filesystem, not something a disk role or format choice affects.

**Host swap**, if a package build (Rust/wasm builds are the confirmed real-world case) runs a box out of RAM: `kanxeoctl swap enable --size-mb=8192` / `kanxeoctl swap disable` / bare `kanxeoctl swap` to check current state (ADR-0069). A single on-demand file, off by default, persisted and re-applied automatically on every daemon start including a real reboot.

## Keeping the box current

Backups and disk management are day-2 operations that don't change what's running on the box; updating the control plane or installed packages does — see [`staying-updated.md`](staying-updated.md) for that, and [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) for kernel updates specifically.
