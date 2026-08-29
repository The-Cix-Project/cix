# 0213 — Scripted partitioning is the install default, and the interactive editor stays as an escape hatch

## Status

Superseded by [ADR-0214](0214-no-interactive-partitioning.md).

Its first decision stands: scripted partitioning is the install default
and `--auto-partition` is gone. Its second — keeping the interactive
`fdisk` session behind `--interactive` — was reversed almost immediately,
once the two arguments below for keeping it were actually checked against
the code. ADR-0214 records why neither held. The reasoning here is left
intact rather than edited, since the mistake is the useful part.

## Context

`cix-install` has had three partitioning paths since Phase 11: an
interactive `fdisk` session, a scripted `sfdisk` layout behind
`--auto-partition`, and `--skip-partition` for a disk already prepared by
other means. **The interactive session was the default** — you got it by
passing no flag at all.

That default was chosen when the scripted path was new and unproven, and
when the fixed layout still claimed the whole disk. Neither is true now.
[ADR-0190](0190-install-leaves-the-disk-mostly-unallocated.md) made the
scripted layout adaptive and conservative: it reads the disk's real size
from sysfs, sizes `cix-containers` at the smaller of 16 GiB and half the
remainder, and deliberately leaves the rest unallocated. A running host
then partitions that remainder through the REST disk API. So the common
reason to reach for a manual editor — "I want the space arranged
differently" — is now answered after install, by the API, rather than at
install time by hand.

The question that prompted this was simply "why are we using `fdisk`,
isn't `sfdisk` more suitable?" — and the answer is that they are not
alternatives. `sfdisk` is script-driven and has no interactive mode; it
already does all of this installer's programmatic partition work, both
writing the layout and dumping the table back. `fdisk` exists solely for
the one path where a human drives it. So the real question is not which
tool, but whether that human-driven path should be the default, or exist
at all.

## Decision

**Scripted partitioning becomes the default.** `--auto-partition` is
removed rather than kept as an accepted no-op, since a flag retained only
so old invocations keep working is exactly the legacy shim this project
does not carry; an invocation that still passes it now fails loudly with a
usage error rather than silently meaning something different.

**The interactive path stays, behind an explicit `--interactive`.**

Two reasons the default flipped, and neither is convenience:

1. **The interactive path was never the safer one.** `cix-install` reads
   partition *roles* back from GPT partition **names**
   (`find_partition_device()`). An operator driving `fdisk` by hand must
   therefore reproduce `cix-esp`, `cix-root-a`, `cix-root-b`, `cix-config`
   and `cix-containers` byte-for-byte, in order, with the right types — an
   exacting contract that `fdisk`'s own interface says nothing whatsoever
   about, and whose violation surfaces much later, as a partition role
   that cannot be found. The scripted path removes the one step where a
   typo is both easy to make and expensive to discover. Making the
   error-prone path the one you fall into by default had it backwards.

2. **It matches how the platform is actually driven.** Every other
   operation here is an API call. An interactive console session as the
   default first act of a new host is the odd one out.

## Alternatives considered

**Delete the interactive path entirely.** Tempting: it is the only reason
this project packages `fdisk` at all, and dropping it would remove a
package, an `isotools` harvest step, and a staged binary. Rejected on one
fact — **the installer media carries no shell.** `mkinstalleriso` stages
`cix-install`, `cix-recover` and a handful of tools; there is no
`/bin/sh`, no busybox, nothing. Remove `fdisk` and a machine the standard
layout does not suit cannot be partitioned from this ISO at all; the
operator's only recourse is to go and find a different live medium. For a
platform whose entire purpose is installing on real hardware, a 500 KiB
statically-linked partition editor that is already built, already tested,
and costs nothing at runtime is cheap insurance.

There is also real coverage to preserve: `test/test_installer.c` drives a
full scripted `fdisk` session under QEMU, and its own comment records that
this coverage caught a genuine shipped bug (the earlier `cfdisk` never had
its terminfo database staged, so it failed outright with `Error opening
terminal: linux.` on a real install — invisible precisely because the path
had no test). Deleting the path would delete that test with it.

**Keep `--auto-partition` as a no-op alias.** Rejected: see above.

## Consequences

- A bare `cix-install` with neither flag now partitions the disk itself.
  The ISO's own boot entry and `docs/guides/installing.md` drop the flag.
- `--interactive` selects the `fdisk` session; `--skip-partition` is
  unchanged; the two remain mutually exclusive.
- `fdisk` stays a package (`fdisk 2.42.2-4`) harvested into the `isotools`
  artifact, and `sfdisk` continues to come off the control-plane root,
  where it already lives for `cixd`'s own partition management.
- Any script or boot entry still passing `--auto-partition` fails with a
  usage error. That is deliberate and visible, not silent.

Making that last point *true* turned up a worse bug than the one this ADR
set out to fix. The argument loop silently ignored anything it did not
recognise, so `--auto-partition` would simply have been dropped — and so
would a mistyped `--skip-partiton`, which means the installer would have
partitioned and destroyed a disk the operator was explicitly trying to
preserve, with no diagnostic anywhere. The flag whose typo is by far the
most expensive was the one least protected. Unrecognised arguments are now
fatal, and print the offending argument; a bare `--` is tolerated, since
whether the kernel passes the separator through to init's argv is the
bootloader's business and not worth being strict about.
