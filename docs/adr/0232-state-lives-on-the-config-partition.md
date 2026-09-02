# 0232 — State lives on the config partition, and state-storage is retired

## Status

Accepted. Retires the `state-storage` role and the
`STORAGE_KIND_STATE` placement introduced by
[ADR-0141](0141-multi-disk-storage-placement.md); that ADR's other two
placements, `rebuildable-storage` and `log-storage`, are unaffected and
stand exactly as written.

Decided by the owner, after asking what the role was actually for and
getting no usable answer:

> *"retire state-storage, and do the /config move with the migration"*

## Context

Two separate problems, and the same decision resolves both.

**The config partition held one file.** `cix-config` is 64 MiB and
contained `net.conf` — the host's boot-time IP, prefix, gateway and
interface — using 15360 bytes of it. Meanwhile the platform's own
definition of itself lived on `cix-containers` alongside container data,
images and packages: networks, DNS/DHCP/NTP/LDAP configuration, syslog
targets, the CA and every issued certificate, container definitions,
device maps, site identity, daemon configuration.

The OS disk layout implies three tiers — immutable roots, host
configuration, container data. Only two were being used, and the
configuration tier was the empty one.

**`state-storage` could not do the one thing that justified it.** The
role moved that state onto another disk. State is the smallest thing on
the box, so capacity and performance were never reasons; the only good
one is surviving the loss of the OS disk, since state is, in ADR-0141's
own words, *"the one thing that can never be regenerated if lost"*.

That scenario was broken three independent ways, each read out of the
code rather than inferred:

- `storagemigrate_start(STORAGE_KIND_STATE, STATE_DIR, target_dir, ...)`
  then `treecopy_recursive(source, target)` — the source is the
  *current* state. It copies onto the disk and never adopts state
  already there.
- `STORAGE_PLACEMENT_PATH` is `<data-dir>/storage_placement.json`, on
  the OS disk — the disk being protected against.
- Boot resolves placement from that record alone and never inspects an
  attached disk for existing state.

So after an OS disk failure the reinstalled box does not know the state
disk exists, and the natural recovery — assign the role, migrate —
copies fresh empty state **over** the real one. The role could not
survive the failure it existed for, and attempting recovery destroyed
what was being recovered.

It existed because ADR-0141 introduced its three placements together, at
a point when nothing but container data could be moved off the OS disk
at all. For rebuildable and log content that motivation stands on its
own: both grow without bound, and both are regenerable or expendable.
The symmetry does not carry to the one whose value was durability.

## Decision

**1. `STATE_DIR` defaults to `CONFIG_DIR/state`.** State lives on the
config partition, which is what that partition is for.

**2. The `state-storage` role and the `STORAGE_KIND_STATE` placement are
removed**, along with `GET`/`POST /v1/system/state-storage(/migrate)`,
`cixctl storage state`, and the dashboard's state-storage panel. State
is no longer relocatable. Durability is `GET /system/backup`.

**3. An existing box migrates once, by copying, and nothing is
deleted.** `migrate_state_to_config()` runs at boot when the destination
does not exist and the legacy `<data-dir>/state` does. It copies, and
only after the copy reports success renames the source to
`state.migrated-<timestamp>`. A failure at any point leaves the original
exactly where the previous build expects it, so booting the other slot
recovers the machine. Failure is not fatal: the daemon falls back to the
old location and continues, because refusing to boot over a migration is
worse than running from where the data already is.

**4. `cix-config` is sized 512 MiB on new installs**, up from 64. PKI is
the one part with real growth — a file per issued certificate — and the
partition is sized once at install and cannot be enlarged afterwards
without repartitioning the OS disk.

**5. A retired role on an upgraded box is skipped, not fatal.**
`diskrole_init()` rejects an unknown role by returning `-1`, which would
refuse the boot. A persisted `state-storage` entry is dropped with a log
line instead; the disk keeps its filesystem and its data. Turning the
retirement of an unused feature into an unbootable machine would be a
far worse outcome than the feature ever was.

## Consequences

- **What survives what is now coherent.** An A/B upgrade replaces a root
  slot and touches neither `/config` nor `cix-containers`. A full
  reinstall reformats the OS disk and is meant to. A factory reset
  removes operator state without reformatting anything. State sits in
  the tier that survives an upgrade, which is where configuration
  belongs.
- **A second consumer of the migration path exists**, so the copy is
  written to be re-runnable and idempotent: a second boot finds the
  destination present and does nothing, which also means it cannot
  overwrite state that has since diverged.
- **The old state directory is left on disk** as
  `state.migrated-<timestamp>`. It is never read again. Removing it is
  an operator decision, deliberately — automatically deleting the only
  copy of a CA immediately after moving it is exactly the risk this
  design refuses to take.
- **`storage_placement.json` may still carry a `"state"` key** on an
  upgraded box. Nothing reads it; the loader starts at the first live
  kind and the next save drops it.
- **The operator-assignable role vocabulary is five, not six.** The
  storage guide documents those alongside the five system partitions,
  because "what is this partition for" is the same question.
- **This does not by itself free `cix-containers`.** The data directory
  root still lives there; that is
  [#249](https://git.home.arpa/itdlabs/cix/issues/249). What changes is
  that the platform's own identity is no longer tied to the same
  filesystem as its rebuildable bulk.
