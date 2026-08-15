# ADR-0104: fs_type on the disk-format REST endpoint -- real btrfs disk formatting alongside ext4

## Status

Accepted

## Context

ADR-0103 (task #678) closed the *quota enforcement* half of the ext4-vs-btrfs
gap: a container placed on a btrfs-backed disk now gets a real qgroup limit.
But `POST /v1/disks/{name}/format` (Phase C, ADR unnumbered-at-the-time /
tasks #656-666) was still hardcoded end to end for ext4 -- it shells out to a
staged `mkfs.ext4` binary and mounts the result with `mount(dev, path,
"ext4", ...)`, with no way to ask for anything else. ADR-0103 explicitly
scoped this out and named it as the deliberately-deferred remaining piece,
re-tracked as task #732 (a pre-existing near-duplicate of #678, re-purposed
rather than left open as a duplicate). This ADR closes that piece: a disk can
now be formatted btrfs by the same REST endpoint, operator-chosen at format
time via a new `fs_type` field.

## Decision

### `fs_type` on `POST /v1/disks/{name}/format`, defaulting to `ext4`

`{"confirm_disk_name": "...", "fs_type": "ext4"|"btrfs"}` -- optional, and
omitting it entirely keeps every pre-existing request's exact prior
behavior (ext4). An unrecognized value is a `400`, validated in
`main.c`'s `handle_disk_format_post()` before `diskformat_start()` is ever
called, rather than threading a "does this daemon actually know this
string" question down into `diskformat.c` itself -- the string-to-enum
mapping only exists in one place (`daemon/src/main.c`), and
`diskformat_start()`'s own signature takes a real `enum diskformat_fs_type`
it can switch on directly, not a string it would have to re-validate.

### `diskformat_start()` branches on `enum diskformat_fs_type`, not a second code path

`daemon/src/diskformat.c`'s forked child now picks its mkfs binary
(`DISKFORMAT_MKFS_EXT4_BIN` or the new `DISKFORMAT_MKFS_BTRFS_BIN`) and its
`mount(2)` fstype string (`"ext4"`/`"btrfs"`) from the same `fs_type`
parameter, inside the one existing fork+exec+mount sequence -- not a
parallel `diskformat_start_btrfs()` function. `mkfs.btrfs` takes no `-F`
(force) flag of its own (confirmed via a real local build+`--help`: no
equivalent exists, and btrfs-progs' own documented default is to overwrite
an existing signature non-interactively without prompting), so the
ext4 branch's own `-F` is simply omitted on the btrfs branch rather than
passed as a no-op flag that doesn't exist.

### `mkfs.btrfs` staging is genuinely optional, unlike `mkfs.ext4`

`mkfs.ext4` is staged into every assembled boot image unconditionally
(`image/src/mkbootroot.c`'s `shelled_bins[]`, Phase C) -- every real thinC
install can always format ext4. `mkfs.btrfs` cannot follow the same
unconditional pattern: this dev sandbox has no `mkfs.btrfs` of its own at
all (confirmed directly -- unlike `mke2fs`, Debian doesn't ship
btrfs-progs by default), and an already-deployed real box may not have
built `btrfs-progs.recipe` onto its own `thinc-hosttools` image yet
either. Adding it to `mkbootroot.c`'s existing `host_tool_bins[]` (every
entry there is unconditionally required, with a dev-host fallback that
doesn't exist for this binary) would have made every local test build
fail outright.

Staged instead the same tolerant way `firmware_dir`/`modules_dir`/
`kmod_bin_dir` already are: only attempted when `host_tools_dir` is given
*and* that tree's own copy actually exists (`stat()`-gated) -- reusing the
existing `host_tools_dir` argument threaded through `mkbootroot.c` already
(no new positional argv slot needed, so no test call site needed updating).
A box whose `thinc-hosttools` image predates `btrfs-progs.recipe` simply
can't format a disk btrfs yet; `POST .../format` with `fs_type=btrfs` on
such a box starts the job (this daemon has no cheap way to distinguish
"binary genuinely missing" from every other reason `execve()` could fail
without also handling every other such reason the same way) and fails
loud once the forked child's own `execve()` returns `ENOENT` --
`DISKFORMAT_STATE_FAILED`/`"mkfs.btrfs failed"`, the same shape `mkfs.ext4`
failing for any other reason already produces, not a new or confusing
failure mode.

### `btrfs-progs.recipe` -- from source, one binary, matching `e2fsprogs.recipe`'s economy

New `pkg/recipes/btrfs-progs.recipe` (v7.1, kernel.org's own canonical
release host) builds only `mkfs.btrfs`, not the full suite (`btrfs`,
`btrfsck`, `btrfs-image`, ~15 more tools nothing in this project calls) --
the same "don't build unused tools speculatively" precedent
`e2fsprogs.recipe` already established for `mke2fs`.

Real, confirmed build-time dependencies (via a real local build in this
sandbox -- `./configure`'s own summary output plus a real `ldd` on the
resulting binary): `blkid`, `uuid`, and `zlib` via pkg-config
(`PKG_CHECK_MODULES` in btrfs-progs' own `configure.ac`, unconditional --
no `--disable` flag exists to skip any of the three). `--disable-zstd
--disable-lzo` avoid needing zstd/lzo (neither has a recipe in this
catalog, and neither is needed for the one thing this project uses
`mkfs.btrfs` for -- formatting a container-storage disk, not
restore/receive compression). `--disable-convert` avoids needing
e2fsprogs' `libext2fs` (`btrfs-convert`, an in-place ext-to-btrfs migration
tool, not a capability this project offers for any filesystem).
`--disable-documentation --disable-python` avoid needing python3/sphinx.
`--disable-libudev` avoids needing libudev (multipath device dedup,
irrelevant to this project's own disk model, ADR-0099). `--with-crypto=
builtin` avoids needing an external crypto library for btrfs's own
checksum tree.

New `pkg/recipes/libblkid.recipe` reuses `libuuid.recipe`'s own already-
staged libuuid (via `pkg_depends="libuuid pkgconf"` and pkg-config)
rather than rebuilding it a second time from the same util-linux source
tree -- confirmed via a real local build: `./configure
--disable-all-programs --enable-libblkid` (no `--enable-libuuid`) resolves
`uuid.h`/`libuuid.so` entirely through the already-installed
`libuuid.recipe` image content, producing just `libblkid.so` + `blkid.pc`.

The resulting `mkfs.btrfs`'s own real `NEEDED` list (confirmed via `ldd`
on a real local build): `libuuid.so.1`, `libblkid.so.1`, `libz.so.1`,
`libc.so.6` -- the identical non-libc set `mke2fs` already needs (already
staged into every assembled boot image by `mkbootroot.c`'s existing
`shelled_bin_libs[]`, added for `mke2fs`'s own sake) plus `libz.so.1`
(already staged there for curl/unsquashfs). **No new runtime library
staging was needed in `mkbootroot.c` for this binary at all** -- only the
binary copy itself, per the tolerant block above.

## Consequences

- `POST /v1/disks/{name}/format` can now format a disk btrfs, operator-
  chosen via `fs_type`, on any box whose `thinc-hosttools` image includes
  `btrfs-progs.recipe`. A box without one keeps today's ext4-only
  behavior exactly, and a `btrfs` request there fails loud and
  specifically rather than silently no-op'ing.
- Combined with ADR-0103, `--disk-quota=` now has a complete, closed loop
  on btrfs: a disk can be *formatted* btrfs (this ADR) and a container
  placed on it gets a real *quota* (ADR-0103) -- the gap the user's
  2026-08-08 directive named ("CLOSE it") is now closed end to end, not
  just the enforcement half.
- Two new library recipes (`libblkid.recipe`, reusing `libuuid.recipe`)
  and one new tool recipe (`btrfs-progs.recipe`) join the recipe catalog,
  each verified via a real local build in this sandbox (configure summary
  output, real `make`, real `ldd`/`--version` on the resulting binary) --
  not yet verified through a live `thincd` `pkg install` round-trip or
  against a real, mounted btrfs filesystem (this sandbox has kernel btrfs
  support but no mounted btrfs filesystem, no loop devices, and no disk
  safe to reformat -- the same acknowledged gap ADR-0099/ADR-0102/ADR-0103
  already documented). Live end-to-end verification (build the recipes
  through a real daemon, format a real disk btrfs, confirm the mount and
  the qgroup limit both take effect) remains an open follow-up against
  real hardware.
