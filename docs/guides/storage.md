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

`cix-install` lays down five fixed partitions on the OS disk, in this
order:

| # | Label | Holds | Yours to change |
|---|---|---|---|
| 1 | `cix-esp` | The EFI system partition — the bootloader and both slots' kernels | No — protected |
| 2 | `cix-root-a` | Root slot A: one complete operating system | No — protected; replaced by an A/B upgrade |
| 3 | `cix-root-b` | Root slot B: the other one | No — protected; replaced by an A/B upgrade |
| 4 | `cix-config` | `net.conf`, and the platform's own state under `/config/state` (networks, DNS/DHCP/NTP/LDAP/syslog, PKI, container definitions, device maps, site identity) | No — protected, but its *contents* are what every configuration endpoint writes |
| 5 | `cix-containers` | The data directory, `/var/lib/cix` — container data, and anything not placed on another disk | **Partly** — it can be grown, never shrunk, moved or unmounted; see below |

**The first four are protected.** They can never be given a role,
formatted, unmounted or deleted — destroying one makes the machine
unbootable, so the API refuses regardless of what you type. The rule is
keyed on the partition *number*, not the label, because a label can be
briefly empty just after boot and a protection a rename can lift is not
a protection.

**`cix-containers` is not protected, and can be grown but never
shrunk, moved or reclaimed.** It grows into contiguous free space
immediately after it — online, in place, because `cix-install` formats
it btrfs — which is the supported way to give the data directory more
room (`POST /storage/{name}/partitions/{partition_name}/resize`, issue
#94). What it cannot do is move or go away: it carries the data
directory itself, fixed when the control plane is assembled
(`cixd --data-dir=`), and no endpoint relocates it. So
`POST /storage/{name}/unmount` naming this partition (its device name,
e.g. `vda5`) is refused with a `409` saying why
([#249](https://git.home.arpa/itdlabs/cix/issues/249)).

Moving everything *out* of it does not change that. `rebuildable-storage`
and `log-storage` each move with their own call, and each container's
storage moves with `container migrate-storage`; do all of that and the
partition is nearly empty, but `/var/lib/cix` is still the data
directory and still lives there. Emptying its **contents** changes
nothing about its **mount**.

The practical consequence is that **the size you give this partition at
install time is a decision you keep**. If you intend to put container
storage on a dedicated disk later, size `cix-containers` small at
install rather than large. The free space you leave after it is
ordinary space: `cixctl storage add-partition vda ...` appends into it
without touching the four structural partitions
([#140](https://git.home.arpa/itdlabs/cix/issues/140)). The space you
put inside it does not come back.

**Attached disks mount at `/mnt/cix/<device>`**, on a tmpfs, outside the
data directory, so they never pin `cix-containers` in place
([ADR-0231](../adr/0231-attached-disks-do-not-mount-inside-the-data-directory.md)).

---

## 2. Two steps, always: assign a role, then format

```
cixctl storage-role create --disk=sdb --role=container-storage
cixctl storage format sdb
cixctl storage format-status sdb
```

They are separate because they are not comparably dangerous.

- **Assigning a role** writes one line of metadata. It is reversible
  (`cixctl storage-role rm sdb`) and touches no bytes on the disk. It
  says what the disk is *for*.
- **Formatting** destroys everything on the device. It requires a role
  to already be assigned, and double-confirms by making you type the
  disk name back. It is asynchronous — poll `format-status`
  (`none` / `running` / `ready` / `failed`).

A disk with no role cannot be formatted. That ordering is the point:
you have to say what a disk is for before you are allowed to wipe it.

---

## 3. The roles you assign

The five OS-disk partitions have fixed jobs (section 1). Every other
disk or partition takes one of these roles:

| Role | One per host? | What lives there | What puts it there |
|---|---|---|---|
| `container-storage` | No — many disks can have it | A container's own rootfs and data | Per container, at creation: `cixctl container run --disk=NAME` ([ADR-0102](../adr/0102-per-container-disk-selection.md)); an existing container moves with `cixctl container migrate-storage NAME --disk=NAME` |
| `rebuildable-storage` | Yes | Anything regenerable from recipes and sources: container images, installed packages, build artifacts, staged ISOs | `cixctl storage rebuildable migrate --disk=NAME` |
| `log-storage` | Yes | The consolidated log store | `cixctl storage logs migrate --disk=NAME` |
| `swap` | Yes | Backs a host swap file | `cixctl swap enable --size-mb=N --disk=NAME` ([ADR-0069](../adr/0069-host-swap-file.md)) |
| `backup` | Yes | Scheduled configuration snapshots | `cixctl backup-config set --disk=NAME --enable` |

**Assigning a role never moves anything.** For the four singleton roles,
the role marks a disk as an *eligible candidate*. A separate, explicit
migrate call is what makes one of them the active placement. You can
have three disks carrying `backup` and none of them in use.

**The platform's own state has no role.** It lives on the config
partition, and its durability is a backup (`cixctl backup`,
`backup-config`), not a disk placement. A `state-storage` role left on
an upgraded box is dropped at boot with a log line; the disk and its
contents are untouched (`daemon/src/diskrole.c`,
[#251](https://git.home.arpa/itdlabs/cix/issues/251)).

**The split between state and rebuildable content matters.** State is
what you cannot get back — lose it and your CA, your networks and your
container definitions are gone. Rebuildable content is everything the
platform can reconstruct from recipes given time and bandwidth. They
live in different places for that reason: state on the config
partition, which is small and survives an upgrade, and rebuildable
content wherever you point `rebuildable-storage`, which is usually the
largest thing on the box. A backup only has to cover the first.

---

## 4. So what should I create a disk for?

In rough order of how often it is worth doing:

- **Container data outgrowing the OS disk** → `container-storage`. The
  most common reason. Place individual containers on it with
  `container run --disk=`.
- **Package and image content filling the box** → `rebuildable-storage`.
  This is usually the largest consumer: images, every installed
  package's files, build artifacts, staged ISOs. Moving it off the OS
  disk is often the single biggest win, and it is the safest thing to
  lose.
- **Logs you want to keep, or keep off the OS disk** → `log-storage`.
- **Swap** → `swap`, then `cixctl swap enable --disk=`. The role only
  makes the disk eligible; the swap file is still created on demand.
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
are btrfs, and that is what makes containers cheap — see snapshots
below. It also gives per-subvolume quota accounting (qgroups),
copy-on-write, and checksummed metadata.

**ext4 is the older option** and is on a retirement path for platform
storage. It has no snapshots and no CoW, so a container placed on an
ext4 disk cannot share extents with its image.

**btrfs is the default.** `format` with no `--fs-type` gives you btrfs,
which is what you want in almost every case:

```
cixctl storage format sdb                  # btrfs
cixctl storage format sdb --fs-type=ext4   # only if you have a reason
```

Reach for `--fs-type=ext4` only when something outside Cix needs to read
the disk and cannot read btrfs.

---

## 6. Snapshots, and why container storage is cheap

This is the part that explains the disk-usage numbers you will see.

An image version is a **btrfs subvolume**. Creating a container on btrfs
is a `BTRFS_IOC_SNAP_CREATE_V2` of that subvolume — an O(1),
copy-on-write, writable clone. The container gets a full, real, writable
root filesystem, and on disk it costs only the extents it actually
changes. Ten containers from one image cost one image plus ten sets of
differences.

Where a snapshot is not possible, the rootfs is built another way
(`daemon/src/main.c`, container create):

| Container storage | User-namespaced container (the default) | Non-userns container |
|---|---|---|
| btrfs, snapshot succeeds | Snapshot, presented through an id-mapped mount | Snapshot |
| btrfs, snapshot fails (e.g. image store on another filesystem) | Full copy | OverlayFS over the image version, logged as a warning |
| not btrfs (e.g. an ext4 `container-storage` disk) | Full copy (`cp --reflink=auto -a`) | OverlayFS over the image version |

Consequences worth knowing:

- **A container on btrfs has a real subvolume as its rootfs**, not a
  union mount.
- **Snapshots are per-filesystem.** A container placed on an ext4
  `container-storage` disk costs a full copy of its image (or an
  overlay, for a non-userns container).
- **`used_bytes` on a btrfs disk counts shared extents once.** Adding up
  per-container usage will exceed the disk's own figure. That is
  correct, not a bug.
- **The rootfs is derived from the image version.** When a container's
  image pin moves, its rootfs is discarded and seeded again, so data
  that must outlive an image update belongs on a volume
  ([ADR-0277](../adr/0277-a-container-rootfs-is-derived-from-its-image.md),
  [`containers-and-services.md`](containers-and-services.md#volumes)).

---

## 7. Disk quotas

A container can be given a real, kernel-enforced limit on its own
storage:

```
cixctl container run --name=web --image=base --disk-quota=2147483648 \
    --service="web=/usr/bin/some-server --port=8080"
```

The mechanism is chosen automatically from the backing filesystem —
btrfs qgroups or ext4 project quotas. You do not select it, and the disk
role and format choice do not change whether quotas are available, only
which implementation is used. Volumes have their own size limit
(`cixctl volume quota NAME BYTES`, see
[`containers-and-services.md`](containers-and-services.md#volumes)).

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
last one to take the remaining space. `cixctl storage rm-partition sdc
sdc2` removes one — `409` if it still has a role, so
`storage-role rm` it first.

---

## 9. Moving something after the fact

Every singleton placement has a migrate call, and they are the supported
way to change your mind. Each is asynchronous and has a status to poll:

```
cixctl storage rebuildable migrate --disk=sdb
cixctl storage rebuildable migrate-status
cixctl storage logs migrate --disk=sdb
cixctl storage logs migrate-status
cixctl container migrate-storage web --disk=sdb
cixctl container migrate-storage-status web
```

Omit `--disk=` to move a placement back to the OS disk. The REST calls
are `POST /v1/system/rebuildable-storage/migrate`,
`POST /v1/system/log-storage/migrate` and
`POST /v1/containers/{name}/migrate-storage`, each with `{"disk": ...}`.

A disk that is the active placement for any of these cannot be
unmounted — the daemon refuses with a message naming the reason, because
unmounting it would break whatever relies on it. Migrate away first,
then `cixctl storage unmount NAME`.

The data directory itself cannot be moved; see section 1.

---

## 10. What survives what

The OS disk has three tiers, and they exist so that different things
survive different events:

| | A/B upgrade | Full reinstall | Factory reset |
|---|---|---|---|
| **Root slots** (`cix-root-a`/`b`) | Replaced — that *is* the upgrade | Replaced | Not touched |
| **`/config`** (`cix-config`) | Survives | Reformatted, unless it holds a CA (below) | Not touched by the wipe — see below |
| **`cix-containers`** (`/var/lib/cix`) | Survives | Reformatted | Its `state`, `containers`, `rebuildable`, `logs` and `volumes` directories are removed |
| **Attached disks** | Survive | Survive (not touched by the installer) | Their mountpoints under `/mnt/cix` are removed; the disks themselves are never reformatted |

- **An upgrade replaces the operating system and nothing else.**
  `POST /system/update` writes the inactive root slot and the ESP. It
  does not touch `/config` or `cix-containers`.
- **A reinstall is a clean slate for the OS disk.** `cix-install`
  rewrites the partition table and makes new filesystems. The one
  exception: if `cix-config` already carries a CA, the installer keeps
  that partition instead of formatting it, unless given `--wipe-config`
  ([`security.md`](security.md#carrying-the-ca-across-a-reinstall)).
  Anything else you need to keep across a reinstall must be on another
  disk or in a backup.
- **A factory reset** (`cixctl factory-reset --confirm=<instance
  name>`) arms a sentinel and reboots. On the way back up, before any
  subsystem loads, `factory_reset_apply_if_pending()`
  (`daemon/src/main.c`) removes `state`, `containers`, `rebuildable`,
  `logs` and `volumes` under the data directory, and the attached-disk
  mountpoint directory. That is the whole list. On an installed host
  the platform's state directory is `/config/state` (`STATE_DIR`,
  `daemon/src/main.c`), which is not among the paths the reset removes.
  The host keeps its IP either way, because `net.conf` is on
  `cix-config`.
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
| A container's rootfs is derived from its image | [ADR-0277](../adr/0277-a-container-rootfs-is-derived-from-its-image.md) |
| Where attached disks mount | [ADR-0231](../adr/0231-attached-disks-do-not-mount-inside-the-data-directory.md) |
| Host swap | [ADR-0069](../adr/0069-host-swap-file.md) |
