# 0257 — One scheduler, and a schedule is structured, not a string

## Status

Accepted

Finishes what issue #96 started (the volume sweep riding the backup timer). Supplies the cyclical half of [ADR-0255](0255-a-recipe-is-a-rule-not-a-version.md)'s Discover stage and, later, the update window that [ADR-0256](0256-the-pipeline-is-the-model.md) named as not built.

## Context

Six periodic timers run in this daemon, each armed and re-armed by its own pair of functions:

```
start_serverhealth_timer      start_build_stall_timer     start_ntp_periodic_timer
start_pkg_sync_periodic_timer start_backup_periodic_timer start_boot_confirm_timer
```

Four of them exist because an operator set an interval, and each of those four owns its own copy of the same three ideas — *is it on*, *how often*, *when did it last run*: `backupconfig.interval_hours`, `volumebackup.interval_hours`, `pkg_repo.sync_interval_seconds`, and NTP's own.

**Two of them back up things to a disk on a schedule, and the platform already noticed.** `main.c` says so, in a comment that is really the first half of this ADR:

> *Issue #96: the volume sweep rides this same tick rather than arming a second timer. Both are "copy something to the backup disk on a schedule", the intervals are independently configured and each is checked against its own last-run time, and one timer is one thing to reason about when backups do not run.*

That is a scheduler stopped one step short: one timer, two schedules, two sets of bookkeeping, and no way for an operator to see either of them as a thing that has a next run time. Add a third scheduled job — refreshing upstream release lists, which ADR-0255 needs and does not have — and the pattern repeats a fifth time.

Nothing here is broken. That is precisely why it is worth naming now: every one of these was a reasonable local decision, and the cost only shows up as the fifth copy.

## Decision

**One scheduler. A schedule is a resource. An action is from a registry. The wire format is structured, and the string form is display-only.**

### The wire format is structured JSON

```json
{ "name": "nightly-backup",
  "action": "system.backup",
  "params": { "disk": "sdc" },
  "schedule": { "daily": { "at": "02:00" } },
  "window_minutes": 180,
  "catch_up": true,
  "enabled": true }
```

Exactly one of three schedule forms is present:

```json
{ "every":  { "seconds": 30 } }
{ "daily":  { "at": "02:00" } }
{ "weekly": { "on": "sun", "at": "03:00" } }
```

**Why not a string grammar, cron-like or otherwise.** The decisive property of a schedule syntax is its failure mode, not its expressiveness. Cron's real defect is that a mistyped expression still *parses* and means something else — `*/5` against `5`, day-of-month against day-of-week. Every positional glob syntax inherits that, systemd's `OnCalendar` included.

A structured body has no syntax to mistype: a wrong field is a missing field, and a wrong value is out of range. It also serves the two surfaces that are not a shell far better than a string does. **The web form *is* the schedule** — a select, a time input, a number — where a string grammar forces the dashboard to encode one itself, which is a second implementation of the daemon's parser that can drift from it. And the CLI's flags map one-to-one onto the fields, so no sugar layer is needed either.

Honesty about what this does *not* buy, because it was claimed and is false: **`apigen` generates routes, CLI constants, web constants and config sections, and no request-body validation at all.** The schema documents the shape; the daemon validates it. What a structured body actually saves is the tokenizer, not the range checks.

### The string form exists, and is never parsed back

`schedule_describe()` renders `daily at 02:00 for 3h` for a CLI column, a log line, a status field. **One direction only.** Nothing anywhere reads it back, so there is no syntax for anyone to mistype — which is the whole objection to cron, answered by construction rather than by a better grammar.

If a schedule ever needs to be a single pasteable token (in a recipe, say), a parser can be added *over* a stable structure later. That direction is easy. Adding structure underneath a shipped string grammar is the hard one, and this ADR deliberately takes the reversible order.

### Actions are a registry, never free text

A job names an action the daemon implements. The registry holds a name, a summary, and a function pointer — the same shape as `srcupstream`'s discovery kinds, chosen for the same reason: the valid values are knowable, so they are published (`GET /schedule-actions`) rather than invited as free text and rejected later.

**This is the security boundary and it is not a style preference.** A free-text command field on a host with no shell is a shell-exec endpoint wearing a friendly name. There is no configuration that makes it acceptable, so the field does not exist.

**Actions enqueue; they never do the work inline.** Every action calls an entry point that already exists and already respects its own queue — `do_backup_snapshot_now()`, `volumebackup_sweep()`, `pkg_sync_start()`. That is what keeps a scheduler from being a way around "never two heavy builds at once".

### Missed runs: one rule, per-job flag

A box that was off at 02:00 runs the job once at startup if `catch_up` is true, otherwise waits for the next occurrence. Default **false**. A backup usually should catch up; a build sweep should not — it would start heavy work at the least predictable moment, immediately after a boot.

### The window is a duration on the job, not a second concept

`window_minutes` says how long after the fire time an action may keep *starting* work. A schedule fires an instant; an update window is an interval, and modelling it as two jobs (open, close) would put a state machine in the operator's hands. A queue-draining action asks `scheduler_in_window()` before each item.

### What migrates, and what deliberately does not

**Migrates** (operator-facing policy): system backup, volume backup, pkg sync, NTP.

**Does not migrate:** `build-stall` and `boot-confirm`. These are internal watchdogs with no policy in them, and putting a watchdog under an operator-editable scheduler means an operator can switch off the thing that reports wedged builds, or the thing that falls a bad boot back to the other slot. A scheduler that can disable the safety net is worse than five timers. This is stated here so that nobody later "finishes the job" by absorbing them.

## Consequences

**A contract change on four endpoints, when the migration lands.** `backup-config`, `volume-backup-config`, `repo-config` and `ntp` each lose their `interval_*` field; the schedule lives at `/v1/schedules/<name>`. Clean cut-over with no fallback field, per this project's standing no-backward-compatibility rule. On first boot after the upgrade, an old config still carrying an interval creates the equivalent job once and drops the field — a one-time upgrade step of the kind `test_layout_upgrade` already covers, not a permanent shim.

**Shipped in parts, and the first part registers exactly one action.** The scheduler, its CRUD surface and `pkg.refresh-upstreams` land first. That action has no existing timer, so nothing can double-fire while both mechanisms are alive, and it closes the first gap ADR-0255 left open: release lists that only ever refreshed when someone asked by hand. The four migrating actions arrive with the cut-over, together, so no window exists in which a job and a legacy timer both drive the same work.

**`schedule` is a noun here.** `GET /schedules` lists; `POST /schedules/{name}/run` runs one now. Manual invocation must never become a second way to *define* a schedule.

## Alternatives considered

**Five-field cron.** Rejected on failure mode: a typo parses. Also carries semantics (`@reboot`, day-of-week/day-of-month OR-ing) that would have to be either implemented or documented as absent, and both are worse than not offering the syntax.

**systemd `OnCalendar`.** Real and specified, and some operators know it — but it is positional globbing with cron's exact silent-typo flaw, and borrowing the syntax of an init system this project deliberately does not run invites people to expect its semantics too.

**ISO 8601 durations plus RFC 5545 `RRULE`.** The most expressive option and the least readable: `PT6H` against `P6D` against `PT6M` is a misreading waiting to happen, and implementing a correct subset of `RRULE` means documenting which subset, which is inventing a grammar with extra steps.

**Leave the timers alone and add a sixth.** Rejected: that is the decision that produced this ADR's context section, taken five times.
