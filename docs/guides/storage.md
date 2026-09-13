# Disks, roles and filesystems

What the disks in a Cix host are for, what to create one for, and what
choosing btrfs or ext4 actually changes.

Task-oriented. For field-by-field endpoint detail see
[`docs/api/README.md`](../api/README.md); for exact CLI flags see
[`cli-reference.md`](cli-reference.md); for *why* a decision was made,
each section links the ADR that owns it.

**The short answer.** A Cix host works with no extra disks at all —
everything lives on the OS disk. You add a disk when you want one
particular kind of content to live somewhere bigger, faster or
separately failing: container data, package/image content, logs, swap,
or backups. You never add a disk to "install Cix on"; that is what the
OS disk already is.

---

## 1. What is already there

`cix-install` lays down five fixed partitions on the OS disk and they
are not yours to manage:

| Partition | Label | Holds |
|---|---|---|
| 1 | `cix-esp` | The EFI system partition — bootloader and kernels |
| 2 | `cix-root-a` | Root slot A |
| 3 | `cix-root-b` | Root slot B |
| 4 | `cix-config` | `net.conf`, the host's boot-time IP/gateway/interface |
| 5 | `cix-containers` | Everything mutable: the data directory, `/var/lib/cix` |

**The first four are protected.** They can never be given a role,
formatted, unmounted or deleted — destroying one makes the machine
unbootable, so the API refuses regardless of what you type. The rule is
keyed on the partition *number*, not the label, because a label can be
briefly empty just after boot and a protection a rename can lift is not
a protection.

**The fifth is not protected**, and deliberately so. If you sized it
smaller than the disk at install time, the remaining free space is
ordinary space: `cixctl storage add-partition vda ...` appends into it
without touching the four structural partitions
([#140](https://git.home.arpa/itdlabs/cix/issues/140)).

**Attached disks mount at `/mnt/cix/<device>`**, on a tmpfs, outside the
data directory ([ADR-0231](../adr/0231-attached-disks-do-not-mount-inside-the-data-directory.md)).
They used to mount *inside* it, which made `cix-containers` permanently
unmountable — the kernel will not unmount a mountpoint that still
carries other mounts.

---

## 2. Two steps, always: assign a role, then format

```
cixctl storage-role create --disk=sdb --role=container-storage
cixctl storage format sdb
cixctl storage format-status sdb
```

They are separate because they are not comparably dangerous.

- **Assigning a role** writes one line of metadata. It is reversible and
  touches no bytes on the disk. It says what the disk is *for*.
- **Formatting** destroys everything on the device. It requires a role
  to already be assigned, and double-confirms by making you type the
  disk name back. It is asynchronous — poll `format-status`
  (`none` / `running` / `ready` / `failed`).

A disk with no role cannot be formatted. That ordering is the point:
you have to say what a disk is for before you are allowed to wipe it.

---

## 3. The roles

### System roles — assigned at install, not by you

The five OS-disk partitions each have a fixed job. They are listed here
because "what is this partition for" is the same question the roles
below answer, and leaving them out made the role list look like the
whole story when it covers only the disks you add.

| Partition | Job | Yours to change |
|---|---|---|
| `cix-esp` | The EFI system partition — the bootloader and both slots' kernels | No — protected |
| `cix-root-a` | Root slot A: one complete operating system | No — protected; replaced by an A/B upgrade |
| `cix-root-b` | Root slot B: the other one | No — protected; replaced by an A/B upgrade |
| `cix-config` | Host configuration — `net.conf`, and the platform's own state (networks, DNS/DHCP/NTP/LDAP/syslog, PKI, container definitions, device maps, site identity) | No — protected, but its *contents* are what every configuration endpoint writes |
| `cix-containers` | The data directory, `/var/lib/cix` — container data, and anything not placed on another disk | **Partly** — not protected; it can be *grown* into free space after it, but not unmounted, shrunk or reclaimed; see below |

The first four are refused a role, a format and an unmount, keyed on
partition **number** rather than label — a label can be briefly empty
just after boot, and a protection a rename can lift is not a
protection. `cix-containers` is deliberately not protected: if you
sized it smaller than the disk, the free space after it is ordinary
space you can carve into new partitions.

**`cix-containers` can be grown but never shrunk, moved or reclaimed
(issue #249).** It *grows* into contiguous free space immediately after
it — online, in place, because it is btrfs — which is the supported way
to give the data directory more room
(`POST /storage/{disk}/partitions/{part}/resize`, issue #94). What it
cannot do is move or go away: it carries the data directory itself,
chosen once when the control plane is assembled (`cixd --data-dir=`),
and there is no endpoint that relocates it. So
`POST /storage/cix-containers/unmount` is refused by rule, with a `409`
saying why.

This catches people out because the obvious workaround looks like it
should work and does not. You can move everything *out* of it —
`rebuildable-storage` and `log-storage` each move with their own
endpoint, and each container's storage moves with
`POST /containers/{name}/migrate-storage`. Do all of that and the
partition is nearly empty, and still cannot be unmounted, shrunk
or deleted (though it can still be grown): emptying its **contents**
changes nothing about its **mount**. `/var/lib/cix` is still the data directory and still lives
there.

The practical consequence is that **the size you give this partition at
install time is a decision you keep**. If you intend to put container
storage on a dedicated disk later, size `cix-containers` small at
install rather than large — the space you leave after it stays usable
for new partitions, and the space you put inside it does not come
back.

### Roles you assign

| Role | One per host? | What lives there | What puts it there |
|---|---|---|---|
| `container-storage` | No — many disks can have it | A container's own rootfs and data | Per container, at creation: `cixctl run --disk=NAME` ([ADR-0102](../adr/0102-per-container-disk-selection.md)); an existing container moves with `POST /containers/{name}/migrate-storage` |
| `rebuildable-storage` | Yes | Anything regenerable from recipes and sources: container images, installed packages, build artifacts, staged ISOs | `POST /system/rebuildable-storage/migrate` |
| `log-storage` | Yes | The consolidated log store | `POST /system/log-storage/migrate` |
| `swap` | Yes | Backs a host swap file | `POST /system/swap` with a `disk` ([ADR-0069](../adr/0069-host-swap-file.md)) |
| `backup` | Yes | Scheduled configuration snapshots | `PUT /system/backup-config` |

Two things this table is saying that are easy to miss:

**Assigning a role never moves anything.** For the four singleton roles,
the role marks a disk as an *eligible candidate*. A separate, explicit
migrate call is what makes one of them the active placement. You can
have three disks carrying `backup` and none of them in use.

> **There used to be a sixth role, `state-storage`, and it is retired**
> ([#251](https://git.home.arpa/itdlabs/cix/issues/251)). It moved the
> platform's own state onto another disk. State is tiny, so capacity
> and performance were never reasons; the only good one was surviving
> the loss of the OS disk, and that never worked — the migration copied
> current state onto the disk rather than adopting state already there,
> the record of which disk held state lived on the OS disk itself, and
> boot read only that record. State lives on the **config partition**
> now, and durability is `GET /system/backup`. An upgraded box with the
> role assigned drops it on first boot, with a log line; the disk and
> its contents are untouched.

**The split between state and rebuildable content still matters, even
though only one of them is a role now.** State is what you cannot get
back — lose it and your CA, your networks and your container
definitions are gone. Rebuildable content is everything the platform
can reconstruct from recipes given time and bandwidth. They live in
different places for that reason: state on the config partition, which
is small and survives an upgrade, and rebuildable content wherever you
point `rebuildable-storage`, which is usually the largest thing on the
box. A backup only has to cover the first.

---

## 4. So what should I create a disk for?

In rough order of how often it is worth doing:

- **Container data outgrowing the OS disk** → `container-storage`. The
  most common reason. Place individual containers on it with
  `run --disk=`.
- **Package and image content filling the box** → `rebuildable-storage`.
  This is usually the largest consumer: images, every installed
  package's files, build artifacts, staged ISOs. Moving it off the OS
  disk is often the single biggest win, and it is the safest thing to
  lose.
- **Logs you want to keep, or keep off the OS disk** →  `log-storage`.
- **Swap** → `swap`, then point `POST /system/swap` at it. Note the role
  only makes the disk eligible; the swap file is still created on
  demand.
- **Backups landing somewhere other than the machine they protect** →
  `backup`.

You do **not** need a disk per role. One extra disk given
`rebuildable-storage` is a complete and sensible setup.

---

## 5. btrfs or ext4

Both are supported. They are not equivalent.

**btrfs is the platform's own substrate**
([ADR-0207](../adr/0207-btrfs-storage-substrate-userns-by-default.md)).
The image store, container rootfs and the platform's own writable data
are btrfs, and that is what makes the container model work at all — see
snapshots below. It also gives per-subvolume quota accounting
(qgroups), copy-on-write, and checksummed metadata.

**ext4 is the older option** and is being retired from platform storage
as btrfs proves itself. It has no snapshots and no CoW, so a container
placed on an ext4 disk cannot share extents with its image.

**btrfs is the default.** `format` with no `fs_type` gives you btrfs,
which is what you want in almost every case:

```
cixctl storage format sdb                  # btrfs
cixctl storage format sdb --fs-type=ext4   # only if you have a reason
```

This default was ext4 until recently, which was simply a default that
outlived its decision — ADR-0207 made btrfs the substrate and put ext4
on a retirement path, and nobody moved the default with it. The
practical effect was that a data disk you formatted yourself could not
snapshot, so containers placed on it got a full copy of their image
instead of a copy-on-write clone.

Reach for `--fs-type=ext4` only when something outside Cix needs to read
the disk and cannot read btrfs.

---

## 6. Snapshots, and why container storage is cheap

This is the part that explains the disk-usage numbers you will see.

An image version is a **btrfs subvolume**. Creating a container is a
`BTRFS_IOC_SNAP_CREATE_V2` of that subvolume — an O(1), copy-on-write,
writable clone. The container gets a full, real, writable root
filesystem, and on disk it costs only the extents it actually changes.
Ten containers from one image cost one image plus ten sets of
differences.

Consequences worth knowing:

- **A container's rootfs is a real subvolume**, not a union mount. There
  is no OverlayFS anywhere in this platform any more.
- **Snapshots are per-filesystem.** A container placed on an ext4
  `container-storage` disk gets a full copy instead of a snapshot, and
  costs accordingly.
- **`used_bytes` on a btrfs disk counts shared extents once.** Adding up
  per-container usage will exceed the disk's own figure. That is
  correct, not a bug.

---

## 7. Disk quotas

A container can be given a real, kernel-enforced limit on its own
storage:

```
cixctl run --name=web --image=base --disk-quota=2147483648
```

The mechanism is chosen automatically from the backing filesystem —
btrfs qgroups or ext4 project quotas. You do not select it, and the disk
role and format choice do not change whether quotas are available, only
which implementation is used.

---

## 8. Splitting one disk into several roles

A whole disk with no role and no partitions can be divided, and each
partition is then an ordinary disk name everywhere in the API:

```
cixctl storage partition-table sdc
cixctl storage add-partition sdc --name=containers --size-mib=51200
cixctl storage add-partition sdc --name=backups
cixctl storage-role create --disk=sdc1 --role=container-storage
cixctl storage-role create --disk=sdc2 --role=backup
cixctl storage format sdc1
cixctl storage format sdc2
```

`partition-table` writes a fresh, empty GPT and is destructive, with the
same double-confirmation as `format`. `add-partition` appends one at a
time and never disturbs an existing partition; omit `--size-mib` on the
last one to take the remaining space. `disks rm-partition sdc sdc2`
removes one — `409` if it still has a role, so remove the role first.

---

## 9. Moving something after the fact

Every singleton placement has a migrate call, and they are the supported
way to change your mind:

```
POST /v1/system/rebuildable-storage/migrate   {"disk": "sdb"}
POST /v1/system/log-storage/migrate           {"disk": "sdb"}
POST /v1/containers/{name}/migrate-storage    {"disk": "sdb"}
```

A disk that is the active placement for any of these cannot be
unmounted — the daemon refuses with a message naming the reason, because
unmounting it would break whatever relies on it. Migrate away first,
then unmount.

**One thing you cannot currently move:** the data directory itself,
`/var/lib/cix` on `cix-containers`. Every placement above can be moved
off it, which empties it — but the directory stays where it is, so that
partition cannot be fully reclaimed. Tracked as
[#249](https://git.home.arpa/itdlabs/cix/issues/249).

---

## 10. What survives what

The OS disk has three tiers, and they exist so that different things
survive different events. This is the whole point of the layout, so it
is worth stating exactly:

| | A/B upgrade | Full reinstall | Factory reset |
|---|---|---|---|
| **Root slots** (`cix-root-a`/`b`) | Replaced — that *is* the upgrade | Replaced | Untouched |
| **`/config`** (`cix-config`) | **Survives** | Reformatted | Untouched today — including the platform's own state, which lives here |
| **`cix-containers`** (`/var/lib/cix`) | Survives | Reformatted | State, containers, rebuildable content, logs and volumes are removed |
| **Attached disks** | Survive | Survive (not touched by the installer) | Their mountpoints are forgotten; the disks themselves are never reformatted |

Read that as three sentences:

- **An upgrade replaces the operating system and nothing else.**
  `POST /system/update` writes the inactive root slot and the ESP. It
  does not touch `/config` or `cix-containers`.
- **A reinstall is a clean slate for the OS disk**, and is meant to be.
  `cix-install` rewrites the partition table and makes new filesystems.
  Anything you need to keep across a reinstall must be on another disk
  or in a backup.
- **A factory reset forgets what the operator configured**, without
  reformatting hardware — it removes state, container data, rebuildable
  content, logs and volumes, and leaves the disks themselves alone.

Two things that follow, and are easy to get wrong:

- **A factory reset today leaves `/config` alone**, so the host keeps
  its IP and stays reachable. That is deliberate: a reset that made a
  shell-less box unreachable would be the worst outcome this platform
  has.
- **Attached disks are never reformatted by any of the three.** Losing a
  role assignment is not losing data; re-assign the role and the
  filesystem is still there.

## Where the decisions live

| Topic | ADR |
|---|---|
| Roles, format and mount as separate confirmed actions | [ADR-0071](../adr/0071-disk-format-mount.md) |
| Choosing a filesystem at format time | [ADR-0104](../adr/0104-btrfs-disk-format.md) |
| Per-container disk selection | [ADR-0102](../adr/0102-per-container-disk-selection.md) |
| State / rebuildable / log placement model | [ADR-0141](../adr/0141-multi-disk-storage-placement.md) |
| btrfs substrate and snapshot-backed containers | [ADR-0207](../adr/0207-btrfs-storage-substrate-userns-by-default.md) |
| Where attached disks mount | [ADR-0231](../adr/0231-attached-disks-do-not-mount-inside-the-data-directory.md) |
| Host swap | [ADR-0069](../adr/0069-host-swap-file.md) |
