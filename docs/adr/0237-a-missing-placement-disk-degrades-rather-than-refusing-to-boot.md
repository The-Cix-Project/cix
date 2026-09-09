# 0237 — A missing placement disk degrades the daemon, it does not stop it

## Status

Accepted

Issue [#256](https://git.home.arpa/itdlabs/cix/issues/256). Decided 2026-09-02.

## Context

When a configured storage placement's disk could not be used, boot resolution
returned a failure and `cixd` exited:

```
resolve_rebuildable_storage_placement: rebuildable-storage's configured disk
  'sda' is present but not currently mounted -- refusing to start
```

Running as PID 1, it then parked at its banner (ADR/issue #131 — a PID 1 that
returns is a kernel panic, so parking is right and that part worked). The
machine was up, the kernel was fine, and there was no way in.

**On this platform that is a worse outcome than it looks.** The host has no
shell. The REST API is the only way to administer it — including the only way
to correct the placement record that is wrong. Refusing to start removes the
means of fixing the thing that caused the refusal. Recovery required console
access and the other A/B slot, on a machine that is otherwise managed entirely
over the network.

It happened for real: a kernel config change renamed a disk (#255), the
placement still named the old letter, and the box went dark.

The behaviour was also inconsistent with itself. `resolve_swap_placement()`
already degraded gracefully — *"using the default location instead this boot"* —
while log-storage and rebuildable-storage refused. Three placements, two
policies, no stated reason for the difference.

## Decision

**A placement whose disk cannot be used degrades to the default OS-disk
location for that boot. The daemon starts.**

It is reported three ways, because a silent degradation would be worse than the
refusal it replaces:

- on the console at resolution time (`stderr`),
- in the log store, emitted immediately after `logstore_init()` — resolution
  runs *before* the log store exists, and stderr is never mirrored into it
  (#132), so without this the reason would be invisible to every API client,
- as `"degraded": true` with a `degraded_reason` on the placement's own `GET`,
  which is what makes the condition discoverable without a console.

This also makes all three placements follow one policy, which is what swap was
already doing.

## The trade, stated plainly

Running on the default location means the images and packages on the configured
disk are **not visible for that boot**. That is a real cost and it is not
hidden: nothing is deleted, the other disk is simply not consulted, and an
operator can see exactly why and correct it.

The judgement is that "some content is temporarily not visible, and the API
says so" beats "the machine cannot be reached at all". The role in question is
literally named `rebuildable-storage`; refusing to boot over content that is
rebuildable by definition is disproportionate.

## Alternatives considered

**Keep refusing, but only for rebuildable-storage.** Rejected: it is the one
whose content is most explicitly reconstructible, so if any placement could
justify a refusal it is not this one.

**Refuse, but start an emergency API.** A second, smaller control plane is a
parallel implementation of the thing that already exists, with its own
authentication and its own bugs, existing only for a case the ordinary daemon
can handle by starting.

**Auto-repair by picking any disk with the right role.** Guessing which disk an
operator meant is how the wrong disk gets written to. Degrading and reporting
leaves the decision where it belongs.

## Consequences

A wrong or stale placement record is now a recoverable, visible condition
rather than an unbootable machine.

Anything that assumes `REBUILDABLE_DIR` is populated must tolerate it being
empty — which was already true on a fresh install.

Callers reading a placement must now distinguish *configured* from *in use*.
That is why `degraded` is a required field rather than an optional one: a
client that ignores it would report a placement that is not actually in effect.
