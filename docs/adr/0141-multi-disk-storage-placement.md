# 0141 — Multi-disk storage placement: state, rebuildable, and log storage become relocatable; backup gets a real destination

## Status

Accepted

## Context

Direct follow-up to the disk management web UI (ADR-0140): the user asked where packages/images, thinC's own config, and logs actually live today, and whether another disk role was needed. Investigated the real code rather than guessing, and found every one of the following lives under one single directory tree, `g_base_dir` (`/var/lib/thinc`, the OS disk's fixed `containers` partition) -- the *only* exception being `/boot` (the ESP) and one tiny file needed before the daemon itself starts (`/config/net.conf`):

- `containers/` -- per-container overlay data. Already relocatable per-container via the `container-storage` disk role and `POST /containers`' own `disk` field (ADR-0102, Phase D).
- `images/` -- every image's shared rootfs layers, which every container's own overlay reads from regardless of where that container's own `disk=` points. Never relocatable today.
- `pkg/`, `artifacts/` -- package build cache and precompiled-artifact cache. Can grow large; fully regenerable from recipes/sources if lost.
- `iso/` -- installer ISO build output.
- A set of small, individually-atomic-written JSON files (`networks.json`, `dns_records.json`, `ldap_*.json`, `diskroles.json`, `daemon_config.json`, `container_defs.json`, `ntp*.json`, `tls_throttle.json`, `rolling_config.json`, `devicemaps.json`, `quota_projids.json`, `syslog_targets.json`, `site_config.json`) -- this platform's own complete definition of itself. Small, but the one thing that can never be regenerated if lost.
- `pki/`, `keys/` -- the CA, every issued certificate/key, and signing keys.
- `logs/` -- the consolidated log store (ADR-0070/ADR-0126), continuously appended to.
- `swap/` -- the on-demand host swap file (ADR-0069, its own separate mechanism, not disk-role-based, out of scope here).
- `disks/` -- mount points for other role-assigned disks; not data of its own.

None of the above except `containers/` can be moved anywhere. The `diskrole` vocabulary itself is only two values, `container-storage` and `backup` -- and `backup` was found to be a pure label: nothing in the daemon writes to a `backup`-role disk. `GET /system/backup` only ever produces a downloadable bundle for the caller to save client-side; there is no server-side backup destination at all today.

## Decision

### New role vocabulary

`diskrole.h`'s `enum diskrole_kind` gains three values, extending the existing small closed vocabulary (`DISKROLE_ERR_INVALID_ROLE` already rejects anything outside it -- this is additive, no existing behavior changes):

```c
enum diskrole_kind {
	DISKROLE_CONTAINER_STORAGE,
	DISKROLE_BACKUP,
	DISKROLE_STATE_STORAGE,       /* new */
	DISKROLE_REBUILDABLE_STORAGE, /* new */
	DISKROLE_LOG_STORAGE,         /* new */
};
```

Named `state-storage`, not `config-storage` -- the OS disk already has a real, separate, much smaller fixed `config` partition (`/config/net.conf`, early-boot networking only); reusing "config" for this would collide with that existing, unrelated concept. `rebuildable-storage` covers `images/`, `pkg/`, `artifacts/`, `iso/` together, not four separate roles -- they share the same defining property (regenerable from recipes/sources, not irreplaceable) and the same operational motivation (can grow large, no reason to force them onto the OS disk), and splitting them further isn't justified by anything found in this investigation. `container-storage` is completely unchanged -- still chosen per-container at creation time via `disk=`, still supports multiple disks the way it always has, since it was never a daemon-wide singleton to begin with.

### A necessary prerequisite: today's layout is completely flat, not already grouped

Checked directly, and this changes the shape of the work: `init_base_dir_paths()` in `main.c` computes every single one of the paths listed in Context above as a **direct child of `g_base_dir`** -- `networks.json`, `dns_records.json`, `pki/`, `keys/`, `images/`, `pkg/`, `artifacts/`, `iso/`, all flat siblings, no existing intermediate grouping directory for "the state files" or "the rebuildable files" to migrate as one unit. `logs/` is the one exception -- already its own clean subdirectory (`LOG_DIR`), nothing to restructure there.

So `state-storage` and `rebuildable-storage` each need a real grouping directory introduced first, not just a migration mechanism layered on the existing flat paths:

- **`STATE_DIR` (`g_base_dir/state`)**: every JSON state file moves under it (`STATE_DIR/networks.json`, etc.), and `PKI_DIR`/`SIGNING_KEYS_DIR` move under it too (`STATE_DIR/pki`, `STATE_DIR/keys`) -- they're conceptually the same "this platform's own irreplaceable definition of itself" category the rest of `state-storage` already is. Migrating `state-storage` becomes "move this one directory," not a bespoke list of a dozen unrelated paths.
- **`REBUILDABLE_DIR` (`g_base_dir/rebuildable`)**: `IMAGES_DIR`, `PKG_DIR`, `ARTIFACTS_DIR`, `ISO_DIR` become `REBUILDABLE_DIR/images`, `REBUILDABLE_DIR/pkg`, `REBUILDABLE_DIR/artifacts`, `REBUILDABLE_DIR/iso` -- same reasoning, one directory to migrate instead of four. Found along the way: `PKGBUILD_TOOLCHAIN_FETCH_PATH` is the one path in this whole file that reconstructs `"pkg/bootstrap_toolchain.squashfs"` directly from `g_base_dir` instead of from the already-existing `PKG_DIR` the way every other `PKG_DIR`-relative path correctly does -- harmless today (the two happen to agree), but would silently break the moment `PKG_DIR` moves. Needs fixing to derive from `PKG_DIR` regardless of this ADR, and definitely as part of it.

**This is a real, breaking layout change that every already-installed box (including 192.168.15.95) needs to survive automatically.** A box running today has its real state sitting at the old flat paths; a daemon built with the new `init_base_dir_paths()` would compute `STATE_DIR/networks.json` and find nothing there on next boot -- silent, total state loss, unacceptable under No Regressions. The fix: a one-time startup step, before anything else runs, that detects "the new grouped directories don't exist yet, but the old flat files do" and moves each known old path into its new nested home via a plain `rename(2)` -- genuinely low-risk, since source and destination are both still on the *same* filesystem at this point (this runs before any disk-role/migration logic is ever reachable), so no `EXDEV` cross-disk copy is involved, just an ordinary same-filesystem rename. This step is themselves worth its own real test (a fixture with the old flat layout, confirming a clean upgrade to the grouped one, run before any of the rest of this ADR's own work begins) rather than trusted to work by inspection alone.

### Multiple disks per role, but one *active* placement per singleton concern

Unlike `container-storage` (inherently multi-instance -- each container independently names its own disk), `state-storage`/`rebuildable-storage`/`log-storage` are each a **daemon-wide singleton**: there is exactly one live `images/` directory, one live log store, one live state directory at any moment, the same way there is exactly one active management network today. Multiple disks *can* carry the same role (eligible candidates, exactly like today's role assignment already permits multiple disks sharing a role with no code change needed there), but only one is ever the *active* placement for that concern. Switching which one is active is a real data migration, not an instant pointer flip -- modeled as three new small resources, one per concern, each mirroring `GET /disks/{name}/format`'s own already-established GET-current-state / POST-start-a-job / poll-until-done shape:

```
GET  /v1/system/state-storage              -> {"disk": "sdc"|null}            (null = default OS-disk placement)
POST /v1/system/state-storage/migrate      -> {"disk": "sdc"|null}             body; 202 + initial job status
GET  /v1/system/state-storage/migrate      -> {"state":"none|running|ready|failed", "disk":..., "error":...}

GET  /v1/system/rebuildable-storage        (same shape)
POST /v1/system/rebuildable-storage/migrate
GET  /v1/system/rebuildable-storage/migrate

GET  /v1/system/log-storage                (same shape)
POST /v1/system/log-storage/migrate
GET  /v1/system/log-storage/migrate
```

`disk` in the migrate request must already carry the matching role (`state-storage` for `/system/state-storage/migrate`, etc.) and currently be mounted -- the same `DISKFORMAT_ERR_NO_ROLE`/not-mounted class of rejection `diskformat_start()` already enforces for format, reused directly rather than reinvented. `disk: null` migrates *back* to the default OS-disk placement -- a real, symmetric operation, not a dead end once you've moved something off the OS disk once.

### Migration mechanics -- safe under this daemon's own single-threaded reactor (ADR-0009), no new concurrency primitive needed

A cross-disk move can never be a plain `rename(2)` (`EXDEV` -- source and destination are different mounted filesystems by definition here), so this needs a real recursive copy. `pkg.c`'s own `merge_tree()` is the right tree-walking shape to generalize (real, tested, handles files/dirs/symlinks correctly -- found the hard way per its own comment, iptables' own symlinked binaries) -- but **not a verbatim reuse**: its own per-file step, `copy_file_simple()`, hardcodes the destination file's mode to `0755` regardless of the source's real permissions. That's harmless for `merge_tree()`'s existing caller (files being installed into an image rootfs, where 0755 is already the right shape for installed program content) but would be a real security regression here -- `pki.c` deliberately writes private keys `0600`/certs `0644` (`chmod()` calls confirmed directly), and a migration that flattened every copied file to `0755` would make every private key world-readable at the new location. The generalized shared copy primitive `fstat()`s each source file and `open()`s/`fchmod()`s the destination with that same real mode -- a genuine correctness fix surfaced by this review, worth folding back into `pkg.c`'s own call site too (its files happening to already want something 0755-shaped is coincidence, not a guarantee, and shouldn't stay silently mode-flattening by accident).

1. **Forked child, mirroring `diskformat_start()`'s own fork+pidfd+epoll async-job shape exactly** (same one-job-at-a-time-per-concern constraint, same `DISKFORMAT_STATE_RUNNING`/`READY`/`FAILED` state machine renamed for this job type) -- the child does the bulk of the copy (the generalized, permission-preserving tree-copy above, shared by both this module and `pkg.c` rather than duplicated -- No Parallel Implementations) from the *current* active location to the new disk's own mount path, while the daemon keeps reading/writing the *old* location completely normally the entire time -- no pause, no readonly window, ordinary operation continues.
2. Once the child reports the bulk copy finished successfully, the **parent (main reactor thread, not the forked child) does one final, synchronous step**: a second, fast incremental copy (only entries changed since the bulk copy started -- cheap, since nothing changes much even under normal load) followed immediately by repointing the live in-memory path (`LOG_DIR`, `STATE_DIR`, or `REBUILDABLE_DIR` -- each now a single real directory per the grouping introduced above, not a scattered list) to the new location. Because this daemon is single-threaded and non-blocking (ADR-0009 -- confirmed directly, no worker-thread pool exists anywhere in this codebase), this final step is inherently atomic from every other request's point of view: nothing else can run concurrently and observe a half-repointed state, the same guarantee every other synchronous handler in this daemon already relies on. No new locking primitive is needed *because* of this existing architecture, not despite it.

   `log-storage` needs one extra piece the other two don't: `logstore.c` holds a **persistently-open `FILE *g_current_fp`** across writes (confirmed directly -- `ensure_current_segment_open()` only reopens when `g_current_fp` is `NULL`, not per write), so changing `g_dir` alone would leave already-open writes still landing on the *old* disk. The repoint step needs a real `logstore_repoint(new_dir)` (or equivalent) that `fclose()`s the current segment, sets `g_current_fp = NULL`, and only then updates `g_dir` -- the next `logstore_write()` call naturally reopens (or creates) a fresh segment under the new location via the existing `ensure_current_segment_open()` path, no other change to that function needed. `state-storage`'s own files, by contrast, are safe as a bare string repoint: `persist.c` writes are already one atomic `open`+`write`+`rename(2)` per save with no long-lived fd held between calls. `rebuildable-storage` is expected to be the same (no long-lived fd pattern found in `image.c`/`pkg.c` for the reactor thread itself -- builds run in forked children, passed a path, not a shared descriptor) but this needs confirming directly during implementation, not assumed from this review alone.
3. Only once the repoint succeeds is the *old* location's data removed (`persist.c`'s own `remove_tree_cb()`/`nftw()`, already used elsewhere for exactly this) -- deliberately not left behind as an ambiguous stale copy, matching One Source of Truth. A failure at any point before the repoint step leaves the daemon still using the old location untouched, with the partially-copied new-location data left for the operator to inspect or the next migration attempt to overwrite -- the same "job failed, state left as whatever the last completed step produced" posture `diskformat`'s own FAILED state already has.

`state-storage` in particular is expected to complete near-instantly in the overwhelming majority of real installs (a handful of small JSON files plus a modest PKI key/cert count) -- modeled as the same async job for consistency with the other two anyway, not because it's expected to be slow.

### Backup gets a real, scoped destination

`backup`-role disks stay scoped to `state-storage`'s own data (the same JSON+PKI bundle `GET /system/backup` already knows how to produce) -- explicitly never workload/container data (already an existing, explicit non-goal) and never `rebuildable-storage` (no point; it's rebuildable by definition, that's the entire reason it isn't `state-storage`). New resource, mirroring every other `*-config` partial-update convention already established (`daemon-config`, `pkg/repo-config`):

```
GET/PUT /v1/system/backup-config   -> {"disk": "sdc"|null, "enabled": bool, "interval_hours": N}
GET     /v1/system/backup-config/status  -> {"state":..., "last_attempt":..., "error":...}  (mirrors pkgSyncStatus's own shape)
POST    /v1/system/backup-config/snapshot-now  -> triggers one immediate snapshot regardless of schedule, same "Sync now" pattern NTP/pkg-repo already have
```

When `enabled` and a `backup`-role, mounted disk is configured, the daemon writes the same bundle `GET /system/backup` already generates to that disk on the configured interval (an in-process timer, the same shape `poll()`'s own `setInterval` equivalent already uses server-side for NTP's hourly auto-sync) -- reusing the existing bundle-generation code path directly, not a second implementation of it.

### Safety: this can't be layered on top of disk role/format without teaching them about it

Found during review, not incidental: `DELETE /diskroles/{name}` and `POST /disks/{name}/format` both currently have zero awareness that a disk might be silently load-bearing beyond its own role label. Both need a new check, or this feature would let an operator pull live data out from under the daemon:

- **`DELETE /diskroles/{disk_name}`** must be refused (409) if `disk_name` is the *currently active* placement for `state-storage`, `rebuildable-storage`, or `log-storage`, or the currently configured `backup-config` disk -- removing the role out from under an in-use placement would leave the daemon's own live-location tracking pointing at a disk that, per its own role table, doesn't do that anymore. Migrate away first (`disk: null` back to the OS-disk default, or to a different role-eligible disk), the same "repoint before you disconnect" discipline `PUT /system/daemon-config`'s own management-network field already established for a conceptually identical hazard.
- **`POST /disks/{disk_name}/format`** must be refused (409, extending the existing "must already have a role" check with one more condition) under the same circumstances -- formatting a disk currently serving as someone's active placement would destroy live data mid-flight, not just fail to find a role.

Neither check applies to `container-storage` -- a container's own `disk=` placement is the *operator's* explicit, per-container choice to track and protect, not something the daemon guesses at globally the way it must for its own singleton concerns.

### What stays explicitly unchanged / out of scope

- `container-storage`: identical to today, no change.
- Container workload data is never covered by any of the above -- `state-storage` is thinC's own definition of itself, not what containers are doing.
- The host swap file (ADR-0069) stays its own separate, disk-role-independent mechanism.
- A container's own storage disk is still chosen only at creation time; making it migratable *after* creation was not asked for here and is a real separate question for later if it comes up.

## Consequences

- Every category the user asked about (packages/images, thinC's own config, logs) becomes genuinely relocatable, closing the gap found during this investigation.
- `backup` goes from an inert label to an actual, scoped, working mechanism -- automatic snapshots of exactly the platform's own state, on the box's own schedule, no operator action required once configured.
- Three new small daemon-wide job types share one migration primitive (the generalized `merge_tree()`) rather than three separate copy implementations -- No Parallel Implementations honored even while adding three new concerns at once.
- Real, non-trivial new surface: a real (if small) breaking layout change with a required automatic upgrade path for every already-installed box, a new module (migration job management, mirroring `diskformat.c`'s own shape), `main.c` path-repoint logic for three different subsystems (`LOG_DIR`, `STATE_DIR`, `REBUILDABLE_DIR`), a new scheduled-job mechanism for backups. Comparable in size to the original swap-file feature (ADR-0069), larger than any single round shipped so far this session -- phased delivery is the right approach, not one giant change (see the implementation plan that follows this ADR).
- CLI: `thincctl storage state|rebuildable|logs show|migrate --disk=NAME` (omit `--disk=` to migrate back to the OS-disk default), `thincctl backup-config show|set|snapshot-now|status`.
- Web UI: the existing Disks page (ADR-0140) gains a "Storage Placement" section (current disk + Migrate action, per concern); the existing Maintenance > Backup page gains an "Automatic backups" section (disk/enabled/interval config + last-snapshot status + a Snapshot now button) -- no new tree leaf needed for either, both extend pages that already exist for exactly this kind of thing.
