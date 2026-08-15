# 0163 — A malformed persisted state file must never be fatal to boot

## Status

Accepted

## Context

Confirmed live, on 192.168.15.95, during the Phase 5 rebrand migration (ADR-0162): `POST /system/restore` wrote an empty string as the literal content of `site_config.json` (the backup bundle's own `site_config` field was empty — `do_system_restore()`'s own field validator, `json_string_field_is_valid()`, deliberately treats an empty string as valid, since for most fields "nothing configured yet" is a legitimate, common state to round-trip through backup/restore). On the next boot, `siteconfig_load()` tried to `json_parse()` that empty content, got `NULL`, logged "malformed persisted site config," and returned `-1`. `main()`'s own boot sequence treats every one of its ~20 subsystem `_init()` calls as fatal on a nonzero return — `return 1` straight out of `main`. On a real `--init-mode` boot, `thincd` **is** real PID 1, so `main()` returning at all is not an ordinary failed-daemon-start; it's the kernel logging `Kernel panic - not syncing: Attempted to kill init!` and the box going down, unrecoverable short of a full reinstall (confirmed: a plain reset reproduced the identical panic, since the bad file was still on disk).

A single corrupted state file — a bad restore, a truncated write, disk corruption, an operator hand-editing something — should never be able to do that. This is exactly the class of decision `docs/adr/0000-adr-process.md` says warrants a record: expensive to reverse (a full reinstall), and it constrains later work (every future subsystem's own `_init()` needs to keep respecting this guarantee, not just the ones fixed here).

## Decision

Two complementary fixes, at two different layers:

1. **`siteconfig_load()` itself** (the module actually involved in the incident): a malformed-JSON parse failure or an invalid field, post-parse, now logs a warning and returns success (falling back to the defaults `apply_defaults()` already applied before the load was attempted) instead of returning `-1`. This is the precise, best-case fix for this one module — real, sensible defaults, not just "skip and hope."
2. **`boot_subsystem_init()`**, a new gate in `daemon/src/main.c` that every one of the ~20 persisted-JSON-state `_init()` calls in the boot sequence now routes through: on a real `--init-mode` boot, a nonzero return is logged as a warning and treated as success (that subsystem keeps whatever defaults its own `_init()` already applied before attempting to load); a plain/test invocation (not real PID 1, an ordinary process exit is harmless there) keeps the original fail-fast behavior exactly as before, unchanged — deliberately, since fast-fail is genuinely useful for catching bugs in dev/test and carries none of the real-boot risk.

Layer 2 is the durable, general guarantee — every current persisted-JSON-state loader is covered, and the comment on `boot_subsystem_init()` itself is the marker future subsystem authors need to see before adding a new uncovered `_init()` call. Layer 1 is what makes site config specifically degrade to *useful* defaults rather than just "inert."

Deliberately **not** applied to the handful of boot steps with real mount/network side effects (`bootstrap_management_network()`, the `resolve_*_storage_placement()` family, `ensure_dir()`) — those fail in a materially different way than "one JSON file didn't parse" (a real disk/network problem, not stale data) and deserve their own dedicated review rather than a blanket fix bundled into this one.

## Consequences

- A future subsystem added to the boot sequence that loads its own persisted JSON state must route its `_init()` call through `boot_subsystem_init()`, or it silently reintroduces exactly this class of bug. There is no compiler check for this — it relies on the comment above `boot_subsystem_init()` and this ADR being found by whoever adds the next one.
- A malformed state file (any of the ~20 now covered) is now a *silent-ish* event on a real boot — logged to stderr/the console, not surfaced any louder than that. An operator who wants to know their site identity/DNS records/whatever silently reset to defaults needs to actually look at boot output; there's no dashboard alert for "a config file was ignored." Accepted as strictly better than the alternative (the box being down entirely), but a real, open gap if quieter degradation turns out to be its own problem later.
- `do_system_restore()`'s own `json_string_field_is_valid()` — the thing that let an empty string through in the first place — was deliberately left as-is, not tightened. It has a real, legitimate reason to accept empty content (most fields round-trip "nothing configured yet" correctly that way); the fix belongs at the loader/boot layer, which now can't be brought down by *any* malformed content regardless of source, not just restore's own empty-string case.
