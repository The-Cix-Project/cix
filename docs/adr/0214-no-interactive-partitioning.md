# 0214 — There is no interactive partitioning; the installer writes the layout, and that is the only way

## Status

Accepted. Supersedes [ADR-0213](0213-scripted-partitioning-is-the-install-default.md),
which made scripted partitioning the default but kept the interactive
`fdisk` session behind `--interactive`. That half was wrong, and this ADR
exists mostly to record *why* the argument for keeping it did not hold —
because it was a reasonable-sounding argument, and someone will make it
again.

## Context

ADR-0213 flipped the default and its reasoning still stands: `cix-install`
identifies partitions by **GPT partition name**, so hand-partitioning
means reproducing `cix-esp`, `cix-root-a`, `cix-root-b`, `cix-config` and
`cix-containers` byte-for-byte, in order, with the right types — a
contract `fdisk`'s interface says nothing about, and whose violation
surfaces much later as a role that cannot be found.

It then kept the interactive path on two arguments. Both were tested
against the code, and neither survives.

**"The installer media carries no shell, so `fdisk` is the only escape
hatch."** The premise is true — `mkinstalleriso` stages `cix-install`,
`cix-recover` and a handful of tools, and there is no `/bin/sh`,
no busybox, nothing; `cix-recover` contains no `execve` at all. The
conclusion does not follow. The scenario being protected against is "the
standard layout does not suit this machine", and because
`find_partition_device()` requires exactly those five names, the
*structure* is fixed no matter who writes the table. An interactive
session could only ever vary the four **sizes**. So the question is not
"what if the layout is wrong", it is "which of these four numbers might an
operator need to change, and is `fdisk` the right way to change it":

| partition | size | can an operator need it different? |
|---|---|---|
| `cix-containers` | min(16 GiB, half the remainder) | Yes — and it is already handled. [ADR-0190](0190-install-leaves-the-disk-mostly-unallocated.md) deliberately leaves the rest of the disk unallocated, and a running host grows the partition into that free space through the REST disk API. That is the reversible direction and an ordinary API call. |
| `cix-root-a`/`-b` | 160 MiB each | Not in practice: the control-plane squashfs is around 10 MB, so the slot has ~16× headroom. If that ever stopped being true it would stop being true for *every* install, and the fix is the constant in `cix-install.c`, not an operator's typing. |
| `cix-esp` | 64 MiB | Same: a platform constant, wrong for everyone or for no one. |
| `cix-config` | 64 MiB | Same. |

Nothing in that table is a job for a manual partition editor on a console.

**"Removing it deletes `test_installer`'s QEMU-driven `fdisk` session,
which already caught a real shipped bug."** The bug was real — `cfdisk`'s
terminfo database was never staged, so the path failed outright with
`Error opening terminal: linux.` on a real install, and switching to
`fdisk` was what made it scriptable enough to test at all. But this
argument is circular: it keeps a feature in order to keep the test *of
that feature*. The test was never evidence that the path was needed, only
that it worked. When the path goes, its test goes with it, and that is the
point rather than the cost.

## Decision

The interactive path is removed. `cix-install` writes the layout itself,
or accepts one prepared elsewhere via `--skip-partition`. There is no
`--interactive`, and `fdisk` is no longer packaged, harvested into the
`isotools` artifact, or staged into the installer image.

`--skip-partition` remains the answer for a genuinely bespoke layout: the
disk is prepared by whatever tooling the operator likes, carrying all five
correctly-named partitions, and `cix-install` reads the table back.

## Consequences

- `recipes/package/fdisk/` is deleted. It existed for one reason and that
  reason is gone; `sfdisk`, which does all of this installer's actual
  partition work — writing the layout on stdin and dumping the table back
  — was never in question and comes off the control-plane root, where it
  already lives for `cixd`'s own partition management.
- `isotools` no longer harvests `fdisk`; `mkfs.fat` still does, since the
  ESP still has to be formatted and no Cix root carries a FAT formatter.
- `test_installer` loses its interactive session and keeps the scripted
  one, which now covers the only partitioning path there is.
- The interactive walkthrough is removed from
  [`installing.md`](../guides/installing.md).
- An operator who genuinely needs a different *system* layout has no
  in-ISO route and must prepare the disk from other media. That is
  accepted deliberately: it is a rare case, the sizes involved are
  platform constants rather than site preferences, and the common case it
  was conflated with — wanting more container storage — is served better
  after install, by the API, growing into space the installer deliberately
  left free.

## The general lesson

Both arguments for keeping it shared a shape worth naming: they defended
the *mechanism* without checking what the mechanism could actually change.
Once the five-name read-back was traced, "custom layout" collapsed into
"four constants", and three of the four turned out not to be operator
decisions at all. Ask what a flexible path can vary before arguing for its
flexibility.
