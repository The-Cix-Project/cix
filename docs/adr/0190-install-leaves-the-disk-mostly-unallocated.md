# 0190 — A fresh install claims what it needs and leaves the rest of the disk alone

## Status

Accepted

Refines the layout described in [ADR-0018](0018-containers-partition-for-base-dir-persistence.md). That ADR's decision — partition 5 backs `BASE_DIR` instead of a tmpfs — is unchanged; what changes is how much of the disk that partition takes.

## Context

`thinc-install --auto-partition` created five partitions: ESP, root-a, root-b, config, and then `thinc-containers` sized as **the rest of the disk**. On a 2 TB machine that is 2 TB of one filesystem, chosen by the installer, before the machine's owner has said anything about how they want their storage arranged.

Raised directly by the operator, ahead of the first bare-metal install:

> i don't want on install (fresh) to have a container storage assigned role disk … It's up to the admin/user to partition the rest of the disk, and use it as needed. this retains flexibility right?

It does, and there is a stronger reason than flexibility: an installer that consumes everything makes its own guess irreversible without destroying data. Undoing "the rest of the disk" means shrinking a filesystem that already holds the system's state.

Two things were checked rather than assumed while scoping this:

- **The `config` partition is genuinely used.** It carries `net.conf` — the address, prefix, gateway and interface the installer was given — which `thincd` reads on its very first boot to bootstrap the management network. Not vestigial.
- **No disk role is auto-assigned.** `diskrole_create()` is only ever reached from its REST handler; nothing assigns one at boot. The operator's concern about an auto-assigned container-storage role was already satisfied — the disk-consumption half was the real gap.

## Decision

**The data partition is bounded, and the remainder of the disk is left unallocated.**

Its size is the smaller of a 16 GiB cap and half of what remains after the system partitions. The cap alone would be wrong on a small disk — 16 GiB of a 20 GiB disk is "the rest of the disk" wearing a number — and the half-share alone would be wrong on a large one, where 1 TB of unasked-for filesystem is no better than 2. Both together give a partition that is useful immediately and modest relative to the machine, on any disk.

**The installer states what it did**, in the same terms the operator thinks in: total, system, data, and how much it left for them.

**It refuses rather than improvises when the disk is too small.** Under about 512 MiB there is no honest layout, and it says the size it found and the size it needs instead of producing something that boots and then fills.

Growing into the free space later is a real, supported operation — `POST /disks/{disk}/partitions/{part}/resize` (issue #94) grows the partition and the filesystem inside it. That is what makes a conservative default safe: the reversible direction is available, and the irreversible one is not taken on the operator's behalf.

## Alternatives considered

**Keep "rest of disk", and let the operator shrink it.** Rejected: shrinking a filesystem holding live system state is the risky direction, and offering it as the remedy for a default nobody chose is backwards.

**Only ESP/root-a/root-b/config, with no data partition at all.** This is what the request literally described, and it cannot work as stated: both root slots are read-only squashfs, so with no writable partition the daemon has nowhere to persist containers, images, volumes or its own state — it would boot and lose everything at every write. Put to the operator with that consequence stated; they chose the bounded partition.

**A `--data-size=` flag with no default policy.** Rejected as the primary answer: every unattended install would then need one more decision made correctly at the console, and the failure mode of getting it wrong is discovering it after the machine is in service. A sound default that can be grown afterwards is better than a required choice.

## Consequences

A fresh install now leaves most of a large disk unallocated. That is the point, and it is visible: the installer prints it, and `GET /disks/{name}/free-space` (issue #95) reports it afterwards.

`test_installer`'s target disk grew from 512 MiB to 1 GiB, because a layout that deliberately leaves a remainder needs a disk with a remainder worth leaving. The test now asserts the unallocated space is really there rather than trusting the layout, and the file is sparse so the extra size costs nothing.

Existing installed systems are unaffected — this changes what a *new* install lays down, and nothing repartitions an existing machine.
