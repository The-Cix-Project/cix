# 0351 — A backup carries only the recipes no source gives back

## Status

Accepted by the owner, 2026-10-23, for #632. Asked whether a system backup should keep carrying every stored recipe, the owner answered: *"I don't think so, right? Unless we're needing them?"* Nothing needed them (measured below). This narrows what ADR-0033 and ADR-0120 put in `pkg_recipes`; everything else in those records stands.

## Context

**The recipes were most of the bundle.** On 192.168.15.95, 0.2.57-580, 2026-10-23, `GET /v1/system/backup` carried 1620 stored recipes, 13.2 MB of an 18.5 MB bundle. The size is why #630 moved the backup off the event loop: it had held the loop for 0.45–0.5 s idle and 1168 ms under a build.

**Git already holds almost all of them.** Since ADR-0324, git is authoritative for a package its source owns. A recipe made on the box goes git-first through `pkg recipe commit`, and a sync refreshes or writes back any copy that differs. The store was compared with the cix-recipes clone, matching both `<name>@<version>-<release>.cbs` and the release-less `<name>@<version>.cbs` that older corpus files use:

- 1558 of the 1620 stored versions are in git.
- The other 62 are the box's alone: 50 probes, plus cbs v0.1.67-1, v0.1.69-1 and v0.1.70-1 (withdrawn), cix 0.2.57-387 to -423 (six superseded releases), hibr 0.49.1-1, linux-pam 1.6.1-12 and cix-aggressive-test 1.0-6.
- None of the 62 is installed. No recipe or image in git pins any of them; the two that mention one do so only in a changelog line.
- 9 stored package names are offered by no source, all of them probes.

A first count, comparing store keys against filenames, said 355 versions existed only on the box. That count was wrong: it read `rtw88-firmware@3.cbs` (`version "3"`, `release 1`) as a different version from the store's `3-1`.

**A copy in the backup is a second writer to git.** A restore writes the bundle's recipe text into the store. If that copy is older than git's, for example missing an `artifact_sha256` approval written back since, the next sync sees a divergence and acts on it (ADR-0324). A backup that holds a recipe its source also holds is a second source of truth for it, and the older of the two.

**What the box knows.** `pkgsource_owner_of()` answers, per package name, whether one source, several or none offers it, or which source the operator chose. That answer is per name and not per version. Nothing records which versions a sync last saw.

## Decision

**The backup carries every stored version of a package no source owns, and nothing of a package a source owns. A restore asks the sources for the rest at once.**

1. **The rule is ownership, per package name.** `do_system_backup()` skips a name for which `pkgsource_owner_of()` answers anything other than `PKGSOURCE_OWNER_NONE`. With one owner or several, a source gives that package back. What is carried is what nothing else can restore: a package published here with `pkg recipe add`, or anything on a host with no source.
2. **A version only the box holds, of a package a source owns, is not carried.** That is the 62 on .95, and losing them at a restore is the owner's decision. Such a version is either a divergence git already overrules or a probe that has done its job. Recording which versions each sync saw, to carry exactly those git lacks, would be new state kept only to preserve things nothing uses.
3. **The bundle says what it leaves out.** `source_recipes_included` is always `false`, on #415's pattern for `pki_included`, so a bundle with few recipes does not read as complete when it is not.
4. **A restore starts a recipe sync** through the scheduler's own `action_pkg_sync()`, and reports the outcome in the response's `recipe_sync`: started, already running, or no source configured. Without it, a restored host's installed packages would name recipes the store does not hold until the next scheduled sync, six hours later on .95. The recipe store is read from disk on demand, so these recipes need no reboot. The outcome is never a failure of the restore, whose files are already written.
5. **The backup stays in a helper child** (#630). It now reads far fewer recipes, but it still walks the whole store, and only the operator bounds how many unowned recipes there are. ADR-0342's rule is bounded against unbounded, not fast against slow.

## Alternatives

- **Keep every recipe.** Costs 13.2 MB per backup on .95 and keeps a second, older copy of what git is authoritative for.
- **Carry no recipes at all.** Loses every recipe a source cannot give back, including all of them on a host with no source. A restore would then lose something nothing else can restore.
- **Carry the versions git lacks, per version.** Would need a record of what each sync saw, kept only to preserve versions nothing installed or referenced needs (rule 2).
- **Leave the rest to the schedule.** Leaves a restored host with installed packages and no recipes for up to six hours, and nothing on the dashboard would say so.

## Consequences

- On .95 the bundle carries the 9 unowned probe names and none of the rest.
- The source list is not in the bundle. It holds tokens, so ADR-0033's boundary keeps it out. A restore onto a fresh install therefore syncs from that install's sources, the public catalogue by default (ADR-0315), until the operator adds their own. The public catalogue is the owner's forge, push-mirrored, so it carries the same recipes.
- A backup taken before this change still restores, recipes included. Restoring it brings back old copies of owned recipes, and the sync it starts treats any that differ from git the way ADR-0324 treats a divergence: a copy differing only in comments or in an approval git lacks is refreshed or written back, and a real divergence keeps the box's bytes and restores git to them, on a source this host may write. That now happens at the restore rather than at the next scheduled sync. Since a version is immutable, the expected difference is an approval, which is the ordinary write-back.
- `test_system_backup` asserts `source_recipes_included` and `recipe_sync`. Its daemon has no source, so it still proves that every recipe of an unowned package is carried.
