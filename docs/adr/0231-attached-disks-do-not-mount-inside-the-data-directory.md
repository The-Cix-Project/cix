# 0231 — Attached disks do not mount inside the data directory

## Status

Accepted. Amends the mount *location* chosen in
[ADR-0071](0071-disk-format-mount.md); everything else in ADR-0071 —
that formatting and mounting is a separate, explicitly-confirmed action
requiring a role and a typed-back disk name — stands unchanged.

Reported by the owner, trying to delete `vda5`:

> *"claude, why are the mount points sitting under another mount point,
> vda5 says no role, I am very confused"*

## Context

An assigned-role disk was mounted at `<data-dir>/disks/<device>`. On a
real host the data directory is `/var/lib/cix`, which is the OS disk's
own `cix-containers` partition, so every attached disk's mountpoint was
a directory *inside that partition's filesystem*.

Mounting one filesystem beneath another couples them. The kernel
refuses to unmount a mountpoint that still carries other mounts, so on
192.168.15.95:

```
vda5   /var/lib/cix              cix-containers   16 GiB
sda    /var/lib/cix/disks/sda    rebuildable-storage
vdb1   /var/lib/cix/disks/vdb1   container-storage
```

`POST /v1/disks/vda5/unmount` could never succeed, and said only that
*"something on this disk may still be busy/in use"* — which reads like
an open file or a stray process rather than a structural fact. The
partition had been deliberately sized small so the rest of the disk
stayed usable; instead it became permanently unmountable, unresizable
and unreclaimable the moment any disk was attached.

Nothing decided this. `disks/` was created alongside `state/`,
`containers/`, `rebuildable/`, `logs/` and `volumes/` because that is
where subdirectories went. But those five hold **data**, and a
mountpoint is not data — it is an empty directory whose only purpose is
to have a filesystem grafted onto it. It was the odd one out in that
list, and that is the whole of the bug.

Two facts made the move cheap, and both were checked rather than
assumed:

- **Nothing persists an absolute path under it.** Storage placements
  record a disk *name* (`{"disk":"sda"}`), a container records a disk
  name, and the assembly squashfs path is rebuilt from `BOOTROOT_DIR`
  each boot. Every path derived from the mount directory is recomputed
  at startup.
- **A mountpoint is worthless across a reboot.** Nothing needs to
  survive, so nothing needs migrating.

## Decision

Attached disks mount at **`/mnt/cix/<device>`**, outside the data
directory, on a **tmpfs** that `boot_init()` mounts there.

The tmpfs is the part that matters. The root slot is currently mounted
`rw` (`cix-install` writes `root=<dev> rw`), so plain directories on
the root would work today — and would be wrong the day the A/B roots
become genuinely read-only, which is the property they exist to
provide. Runtime state does not belong on the root in either case. The
mountpoint parent `/mnt/cix` ships in the image (`mkbootroot`) so even
creating it never writes to the root.

`--disks-dir=PATH` overrides the location. When it is absent, one rule
resolves it: a **non-default `--data-dir`** keeps disks inside that
data directory, and anything else uses `/mnt/cix`. That middle case
preserves this project's own test isolation without editing every
daemon-linked test — a test that has moved its data directory is a
self-contained world, and its disks belong in that world rather than at
a fixed path several concurrent tests would share.

The tmpfs mount is deliberately **non-fatal**, like the config
partition's mount: if it fails, the daemon still starts and an ordinary
directory is used. A disk mountpoint is not worth refusing a boot over.

## Consequences

- `cix-containers` becomes an ordinary partition again. Nothing is
  mounted beneath it, so it can be unmounted, resized or reclaimed once
  whatever else pins it is dealt with.
- No migration and no data movement. On the next boot every disk mounts
  at the new path because every path is derived. The empty
  `<data-dir>/disks/` directories left behind on an upgraded host are
  inert.
- A factory reset removes the mountpoint directory by its real path
  rather than as a child of the data directory. It still removes only
  mountpoints, never contents: it runs before
  `diskformat_remount_present_role_disks()`, so every directory under
  it is empty at that moment.
- The unmount error now names what is actually holding a mount
  (`other filesystems are mounted beneath it (...) -- unmount those
  first`, 409) instead of a generic 500. That message is still reachable
  — nesting is legal, it is just no longer something this platform does
  to itself.
- **This does not by itself free `cix-containers`**, because the data
  directory still lives on it. That is [#249](https://git.home.arpa/itdlabs/cix/issues/249)
  and needs its own decision; what changes here is that nothing is
  *blocked* by the mount hierarchy any more.
