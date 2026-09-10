# Administration

Day-2 operations for an already-installed box: watching it, backing it up, and managing its storage. This guide is task-oriented — for field-by-field detail on any endpoint here, see [`docs/api/README.md`](../api/README.md); for the CLI's exact flag syntax, see [`cli-reference.md`](cli-reference.md).

## Monitoring

**Host-wide resource usage** — `cixctl host-stats` (`GET /system/stats`, ADR-0073/ADR-0074): load average, CPU/memory/disk usage, per-interface network counters, and cgroup v2 pressure-stall (PSI) figures for CPU/memory/disk. Raw cumulative counters, no server-side history — call it again for a fresh point-in-time snapshot, or watch it live on the web dashboard's System > Monitoring > Host Stats page (four graphs, no charting library, polls only while that page is open). A single container's own usage is the equivalent `cixctl container stats NAME` (`GET /containers/{name}/stats`).

**Every real process on the box** — `cixctl process ls` (`GET /system/processes`, ADR-0131): a direct `/proc` scan, each process correlated to a container if any (by walking its real host ppid chain against every running container's own root pid). `cixctl process kill PID` sends a real, immediate `SIGKILL` — refuses pid 1 and this daemon's own pid outright, since either would crash or reboot the whole host on a real installed system. Killing a pid that happens to belong to a running container is exactly equivalent to that container crashing on its own; the container's own restart policy still applies. The web dashboard's System > Monitoring > Processes page is fetch-on-demand (a Refresh button, not folded into the poll loop) since a real process table churns too fast for a 2s auto-refresh to be anything but noisy.

**The consolidated log** — `cixctl logs` (`GET /system/logs`, ADR-0070/ADR-0126): one chronologically-interleaved store covering real kernel `dmesg`, `cixd`'s own internal diagnostics, a per-request audit trail (every mutating `POST`/`PUT`/`DELETE` any client made — `GET` requests are excluded, a query is never an action), and every container's own stdout/stderr, captured transparently with no opt-in needed. Filter with `--source=`, `--level=`, `--container=`, `--regex=` (POSIX extended, case-insensitive), `--tail=`, `--since=`. `cixctl logs config` shows or sets the store's size cap (`--max-bytes=`) and minimum severity floor (`--min-level=`) — the floor is checked at write time, the `--level=` filter above only ever filters what's already stored. The web dashboard's bottom log panel (every page, collapsible) shows this same stream live, merged with the dashboard's own client-side action log, filterable by source.

**What has happened to a package** — `cixctl pipeline runs` (`GET /pipeline/runs`, ADR-0272). `cixctl pipeline` tells you where a package stands *now*; this tells you what has happened to it: one line per run, newest first, naming the image, how it ended, how long it took, and whether it was requested or caused by a recipe publish. Narrow it with `--name=`, `--image=` and `--limit=`.

Reach for it before the build log, not after. Build logs are capped at forty files — on a real host that was two days' worth — while runs are kept for a thousand, so a run whose log has been pruned still tells you it happened and how it ended, and the CLI prints `(pruned)` rather than a filename that is not there. Retention is a count and is yours to set: `PUT /system/pipeline-config {"run_retention": N}`, 1 to 2000, stored with the runs so it survives a restart.

**Optional external syslog forwarding** — if you already run syslog tooling and want this platform's container logs to also reach it, register a running syslog-server container (e.g. `syslog-1` running `sysklogd`) as a forward target: `cixctl syslog target register --container=NAME` (ADR-0127). Every container-sourced log line is then also sent as a real RFC 3164 UDP datagram — alongside, never instead of, the consolidated log store above, which stays the one source of truth this API and the web UI ever read from.

## Host sysctl tuning

```sh
cixctl sysctl set net.ipv4.ip_forward --value=1
cixctl sysctl set net.ipv4.ip_local_port_range --value=32768 --value=60999
cixctl sysctl show
```

Direct, live tuning of the host's own `/proc/sys` — no key allowlist (fully open, matching this platform's own "no curated sysctl schema" scope decision), and distinct from the per-container `--sysctl=` flag at `run`/`POST /v1/containers` time, which is `net.*`-only and scoped to that container's own netns. Repeat `--value=` for a tuple-shaped key (`ip_local_port_range` above); a single-token key takes one `--value=`. Persists by default for reapply at every boot, right after configured kernel modules load and before the management network comes up — pass `--no-persist` for a one-shot change that shouldn't survive a reboot. `cixctl sysctl rm KEY` drops a key from the boot-apply list only; it never touches the live value. Full detail: [`docs/api/README.md`](../api/README.md#host-level-sysctl-adr-0160).

## Kernel modules

```sh
cixctl kmod ls
cixctl kmod show e1000e
cixctl kmod-config set e1000e --autoload
cixctl kmod load e1000e
cixctl kmod unload e1000e
```

Wraps real `modprobe`/`modinfo` (ADR-0159 Phase A) -- `kmod ls` reads the kernel's own live `/proc/modules`; `kmod show NAME` is real `modinfo` output (description, module parameters, dependencies, in-tree vs. out-of-tree) for a module that's built and available, whether or not it's currently loaded. `kmod load NAME [--option=KEY=VALUE ...]` is real `modprobe`, with real dependency resolution -- omit `--option=` and it falls back to that module's own persisted `kmod-config` default, if one exists. `kmod-config set NAME --autoload` marks a module for automatic reload on every future boot (its own small, REST-managed list -- distinct from this platform's separate, fixed hardware-detection module list, which needs no configuration at all); `kmod-config set NAME --option=KEY=VALUE` sets the persisted default options a bare `kmod load NAME` falls back to. Building an *additional* module not already present in this platform's own curated kernel build -- rather than just loading one that's already there -- goes through the kernel rebuild + A/B cutover path in [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md), not this command.

`kmod unload NAME` is real `modprobe -r` -- reverse-dependency-aware, not a bare `rmmod` that would leave now-unused dependencies loaded. **It needs a kernel built with `CONFIG_MODULE_UNLOAD`, which this platform's kernel did not have before 7.2.3-9** (issue #353): on an older kernel `delete_module(2)` is not compiled in at all, and the unload fails with `Function not implemented` no matter how idle the module is. If you see that, the kernel is the answer, not the module.

A failed load or unload reports **modprobe's own words**, not a fixed sentence -- `is builtin.`, `not found in directory /lib/modules/...`, `FATAL: Module X is in use.` Each points at a different fix, and each used to be discarded before the message reached you.

**Changing a loaded module's parameters means unloading it first.** Module parameters are set when a module is inserted, so `kmod load NAME --option=...` against a module that is already loaded is refused with `409` rather than reporting a success that changed nothing -- `modprobe` on a loaded module exits 0 and does nothing at all. Unload it, then load it with the options you want. A bare `kmod load NAME` of something already loaded is still fine and does nothing: it means "make sure this is loaded", and it is.

Which drivers are modules at all is a deliberate split (#347): a driver for a *particular device* -- a GPU, a wireless chipset -- is a module, while everything needed to reach the root filesystem or the network is built in. That is not a stylistic preference. This platform's kernel is an EFI stub with **no initramfs**, so a storage controller or NIC driver that is not built in cannot be loaded from a disk the kernel cannot yet see.

## Device hotplug

```sh
cixctl container run --name=printer --image=base --optional-device=usb:04b8:0202:12345--service=print-daemon=/usr/bin/print-daemon
cixctl container device attach printer usb:04b8:0202:12345
cixctl container device detach printer usb:04b8:0202:12345
cixctl device ls
```

Extends the existing device-passthrough model (`run --device=ID`, `devicemap`) rather than replacing it (ADR-0161) -- the passthrough unit stays the *whole device*, never an individual USB interface, even for a composite device (a combo HID+storage device, say): `device ls`/`GET /v1/devices` now reports every real interface such a device exposes (class/subclass/protocol) so it's no longer opaque, but granting it still means granting the entire thing.

**`--optional-device=ID`** is the one real behavior addition here: unlike `--device=ID`, a currently-unresolvable optional reference does not fail container creation -- the container is created without that grant, and the reference itself is remembered. From that point on, two things keep it moving toward being granted without any further operator action needed:

- **Real hotplug reaction** -- this daemon listens for real kernel `NETLINK_KOBJECT_UEVENT` USB add/remove events. When a device matching a running container's own pending reference actually appears, it's live-attached automatically (BPF grant + `/dev` node, no recreate); when a currently-granted device's hardware disappears, its grant is actively revoked the same way, whether it was originally attached at creation or hotplugged in later. If more than one running container's own pending reference would match the same newly-appeared device, it's granted to **neither** (logged, not silently arbitrated) -- resolve the ambiguity by giving the devices distinct devicemap names instead of matching the same raw vendor:product pair.
- **`cixctl container device attach NAME ID`** -- the same live-attach primitive the hotplug listener itself calls, available to run by hand at any time (matches `container network attach`'s own two-layer shape: a real, callable primitive first, automation is just another caller of it). `container device detach NAME ID` is the reverse -- refuses (409) a device that was granted at container creation, live or not; recreate the container to remove one of those.

Every hotplug-driven grant or revocation is written to the consolidated log (`cixctl logs`) -- see [Monitoring](#monitoring) above.

## Backup and restore

```sh
cixctl backup --output=backup.json
```

Bundles container definitions, networks, DNS records, package install state + recipes, and site config into one file, saved byte-for-byte for later use with `cixctl restore --input=backup.json`. Read the fine print before relying on this for disaster recovery — **does not** include container workload data (a database's own files, a git host's repos — back those up with their own native tooling), image rootfs content (reproducible by re-running `pkg install`, since everything here is compiled from source — the bundle is the "shopping list," not the built bytes), or anything PKI-related (a CA/leaf private key is never returned over this API anywhere, by design — back up `/var/lib/cix/state/pki/` (ADR-0141) separately, directly on the host). Full detail: [`docs/api/README.md`](../api/README.md#backup-and-restore).

Restoring does not take effect immediately or reboot for you — a typical disaster-recovery sequence is: boot a fresh install → `cixctl restore --input=backup.json` → `cixctl reboot` → the second boot comes up with the restored state. A scheduled backup is just this same command run on a cron entry (or from a container with network reachability to `cixd`) — `cixctl` is a plain REST client either way, nothing special about running it unattended.

## Disk management

`cixctl storage` lists every real host block device, live-enumerated on every call, including any partitions already on it, and flags which ones are protected — the OS disk itself and its first four structural partitions (the ESP, both root slots and `/config`), which are never given a role, formatted or unmounted.

Everything else about storage — the six disk roles and what each is for, when to add a disk at all, btrfs versus ext4 and what snapshots buy you, per-container quotas, splitting a disk into several role-assigned partitions, and moving a placement afterwards — has its own guide: **[`storage.md`](storage.md)**. It is the single place that describes them, so this section deliberately does not repeat it.

The two-step shape is worth knowing here, because it is what stops an accident:

```sh
cixctl storage-role create --disk=sdb --role=container-storage   # metadata only, reversible
cixctl storage format sdb                                        # destructive, needs a role first
cixctl storage format-status sdb                                 # async: none/running/ready/failed
```

A disk is used in exactly one of two mutually-exclusive modes: role assigned directly to the whole disk, or partitioned with roles assigned to the individual partitions instead — `partition-table`/`add-partition` both refuse to touch a whole disk that already has a role of its own.

**Host swap**, if a package build (Rust/wasm builds are the confirmed real-world case) runs a box out of RAM: `cixctl swap enable --size-mb=8192` / `cixctl swap disable` / bare `cixctl swap` to check current state (ADR-0069). A single on-demand file, off by default, persisted and re-applied automatically on every daemon start including a real reboot.

## Does installing a package onto an image reach containers already running from it?

**No — not on its own, by deliberate design (ADR-0107/0108).** Every install/upgrade/uninstall against an image produces a new, immutable, content-addressed rootfs version; nothing is ever mutated in place. A container pins the specific image version it was created against (`registry.json`'s own `image_version`) and keeps running against that exact rootfs forever, even after the image moves on to a newer version — its overlay lowerdir points at a different on-disk directory than the one the new version lives in, so there's no live content for it to pick up. This is the answer to a real, previously-surprising symptom: a package installed onto an image doesn't show up in an already-running container started from that image, only in one created (or recreated) afterward.

Two ways to actually get a running container onto new content, neither automatic unless you ask for it:
- **Recreate it** — `cixctl container rm NAME` + `cixctl container run ...` again (or re-`POST`/re-apply a [container recipe](../adr/0151-container-recipes.md)) re-resolves the image's current version at that moment.
- **`follow_rolling: true`** at creation time (`run --follow-rolling`, [ADR-0124](../adr/0124-pkg-redesign-part5-rolling-containers-and-restart-jitter.md)) — the daemon detects the pinned image's `current_version` advancing (a rolling auto-rebuild or a manual `pkg install`) and live-restarts the container onto the new pin on its own, spread out with jitter (`--follow-rolling-jitter-seconds=`, or the daemon-wide default via `cixctl rolling-config`) so many containers following the same image don't all restart at once.

For patching a single file into an already-running container without a full recreate — a live config tweak, not a package install — see [`PUT /containers/{name}/files`](../api/README.md#writing-a-file-into-an-existing-container-live-without-a-recreate) ([ADR-0153](../adr/0153-container-file-live-update.md)); it's live and ephemeral, not a substitute for either option above.

## Deploying against an image that has not been built yet

Applying a deployment whose image exists but is still empty does not fail. It returns **`202 Accepted`**, queues the image for a build, and creates the container by itself once the image is realized:

```
$ cixctl deployment apply dns-1
{"name": "dns-1", "state": "awaiting-image", "awaiting_image": "dnsimg"}
```

This closes an asymmetry rather than adding a feature ([ADR-0270](../adr/0270-a-deployment-waits-for-its-image-rather-than-being-refused.md)). Installing a package into an image has always built it from its recipe when no artifact existed — the composition edge from image down to package forks on its own. The edge from deployment down to image did not, so the operator's next move was always the same manual round trip: go realize the image, come back, apply again.

Watch it with `cixctl pipeline` or `GET /v1/pipeline` — a waiting deployment reports stage `acquire`, status `blocked`, and names the image it is blocked on. It does not block your terminal, and it does not forget across a reboot: the wait is persisted, and the daemon re-queues the image on its way back up.

Three limits worth knowing before you rely on it:

- **`cixctl container run` against an unbuilt image still fails immediately.** That is the same code path the daemon replays at boot and after a crash, and a wait there would stall a reboot rather than help anyone. Waiting is a property of *applying a deployment*, which is a declaration of intent, not of creating a container, which is an instruction.
- **An empty image with neither a manifest nor an image recipe is refused.** Nothing declares what belongs in it, so there is nothing to wait for. Give it a manifest (`cixctl image manifest set`) or an image recipe first.
- **If the image builds and the container then fails to create for some other reason** — a name taken in the meantime, a network deleted — the deployment stays pending and reports `failed` at the same stage with that error. It is retried when that image next changes, or immediately if you apply again.

## Keeping the box current

Backups and disk management are day-2 operations that don't change what's running on the box; updating the control plane or installed packages does — see [`staying-updated.md`](staying-updated.md) for that, and [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) for kernel updates specifically.
