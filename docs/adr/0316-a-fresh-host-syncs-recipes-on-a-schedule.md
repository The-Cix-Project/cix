# 0316 — a fresh host syncs recipes on a schedule

## Status

Accepted. Directed by the owner on 2026-09-29: *"add a default recipe-sync schedule too"*, straight after [ADR-0315](0315-the-public-catalogue-and-cache-are-the-defaults.md). ADR-0315 gave a fresh host a recipe catalogue but left it to fetch only when asked, and named the schedule as a separate decision. This ADR makes it.

## Context

Since [ADR-0257](0257-one-scheduler-structured-schedules.md), when anything runs on a clock is a schedule, and a host with no schedules runs nothing periodically. 192.168.15.95 syncs because it has a `recipe-sync` schedule (`pkg.sync` every 6 hours); a fresh install had none and therefore never refreshed its catalogue.

In the scheduler, an every-N job that has never run is due at once (`scheduler_run_due()`: `due = last_run_at == 0 ? now : ...`). So a job created at first boot runs on the first scheduler tick after startup.

## Decision

**A host that has never saved a schedule file starts with `recipe-sync`: action `pkg.sync`, every 21,600 seconds (6 hours), enabled.** It is created at startup by `seed_default_schedules()` in `daemon/src/main.c`, after the ADR-0257 legacy migration, so an interval migrated from an old config takes the name first and wins.

**The first sync happens at first boot**, because a job that has never run is due at once, and then every 6 hours. The schedule records only that it started a sync (`last_reason` "started a recipe sync"); how the sync itself went is `cixctl pkg sync-status` (`GET /v1/pkg/sync`). If that first sync fails — for example because DNS was not set yet — the next attempt is 6 hours later. `cixctl pkg sync --wait` runs one straight away.

**Only when no schedule file was ever saved.** `scheduler_init()` notes whether it read a file (`scheduler_state_was_saved()`), including an empty one. Creating the default saves the file, so after first boot the host is an ordinary host with a saved schedule. An operator who deletes `recipe-sync` or changes its interval keeps that choice across restarts and upgrades. This is the rule ADR-0315 applies to the package sources.

**The schedule does not depend on the sources.** It exists whether or not a repo is set. If an operator clears the repo config, each run fails with `no repo is configured` and records it, and deleting the schedule stops it. The two are independent settings, and each keeps what the operator chose.

### Consequences

- **An existing host with no schedule file gains `recipe-sync` when it upgrades**, and syncs straight away. That is intended. A host that has any saved schedules, like 192.168.15.95, is unchanged.
- **Tests start with an empty saved schedule list.** `test_data_dir_create()` writes `state/schedules.json` as `{"schedules":[]}`, so no test daemon runs anything on a clock unless the test creates it. `test_pkg` (a floor selftest) checks the default against a data directory with no schedule file, with the repo config still cleared so the sync the job fires cannot reach a network. It checks that the job exists with the right action and interval, deletes it, restarts, and checks it stays deleted.

### What this rejects

- **A built-in sync timer outside the scheduler.** ADR-0257 removed exactly that. A default is one ordinary schedule an operator can see, change and delete like any other.
- **Creating the schedule on the first `PUT` of a repo config.** A fresh host never makes that `PUT` under ADR-0315, which is the case this exists for.
- **`catch_up`.** It governs missed wall-clock occurrences and has no effect on an every-N job.
