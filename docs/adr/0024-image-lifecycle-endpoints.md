# 0024 — image lifecycle as a filesystem-backed REST resource, no separate persistence

## Status

Accepted

## Context

Before this change, an image was a purely implicit concept: a directory under `images_dir` that came into existence either at real system install time (`base`, via `kanxeo-install.c`) or as a side effect of the first `pkg install` targeting a name never seen before (ADR-0020). `GET /v1/devices` and `GET /v1/pkg` were the only two places an image name was even visible in the API. There was no way to list what images exist, create one explicitly (before installing anything into it), or delete one no longer needed — confirmed directly by grepping the API surface, not assumed.

## Decision

New resource, `/images`, mirroring `/networks`' own four-endpoint shape (`GET`/`POST /images`, `GET`/`DELETE /images/{name}`) — deliberately **not** persisted the way `/networks` is (`daemon/src/network.c`'s own `networks.json`): an image genuinely *is* its directory, nothing more to remember across a restart, the same "host state, not daemon-owned state" posture `device.c` already has for PCI/USB/net hardware. `daemon/src/image.c`/`daemon/include/image.h` re-enumerate `images_dir` fresh on every list/get call.

`POST /images` creates an empty `rootfs/` directory and immediately calls `pkg_seed_image_runtime()` (ADR-0023; renamed `pkg_seed_image_baseline()` by ADR-0041, same call site) so the image is usable right away, before any `pkg install` ever targets it — matching the intuition that a freshly created image should already be capable of running a dynamically-linked binary the moment something lands in it.

`DELETE /images/{name}` needed two new safety checks beyond simple existence, both mirroring an existing pattern rather than inventing a new one:
- `registry_image_in_use()` (`daemon/src/registry.c`), mirroring `registry_network_in_use()` exactly — `registry_entry` gained a `char image[]` field, populated via a new explicit `image` parameter on `registry_create()` (the same shape `nets`/`devices` are already passed in, not derived from `container_spec`). `GET /v1/containers` responses gain an `"image"` field as a direct, natural side effect — closing a small pre-existing gap (a container's own image was previously never echoed back at all).
- `pkg_image_has_packages()` (`daemon/src/pkg.c`), a new query mirroring the same shape — refuses deleting an image with any package still tracked against it, avoiding orphaned `pkg.c` state referencing a rootfs that no longer exists.
- `"base"` is hard-protected from deletion by name, unconditionally — every container's own default image, deleting it out from under the platform is never a reasonable v1 operation.

Recursive removal uses a hand-rolled `nftw()`-based walk (`FTW_DEPTH | FTW_PHYS` — post-order so children are removed before their parent directory, physical so a symlink inside a rootfs is unlinked itself, never followed and traversed into) rather than shelling out to `rm -rf`. This is a deliberate, narrow exception to precedent, not an oversight: `pkg.c`'s own subprocess use (`curl`, `cp`, `sha256sum`, a package's real build toolchain) is already established and justified for executing *untrusted build scripts* and real upstream software, a fundamentally different problem from removing a directory tree the daemon itself fully owns — for that, this project's own broader posture (hand-rolled C, direct syscalls, no shelling out for host/daemon-owned operations — namespaces, cgroups, rtnetlink all follow this already) is the right one to follow.

Name-validation note: `pkg_find()`/`network_find()` are safe to call with an unvalidated raw name because they only ever compare it against already-validated in-memory table entries, never construct a filesystem path from it directly. `image.c` has no such table — every entry point (`image_create`, `image_delete`, `image_write_json_one`) validates the name's charset (`simple_name_is_valid()`, the same shared validator every other simple resource name in this project already uses) *before* any path is built from it, since a directory scan alone offers no equivalent protection against a name like `../../etc` being used to construct a path outside `images_dir`.

## Consequences

- Verified over real HTTP against a live daemon (`test/test_images.c`, new): create, list, get, 409 on duplicate create; 409 deleting an image a running container references; 409 deleting an image with tracked packages; 400 deleting `base`; successful delete of an empty, unused image with the directory genuinely gone afterward. 3 consecutive clean runs. Full regression sweep re-run clean.
- `registry_create()`'s signature changed (new `image` parameter) — both existing call sites (`handle_create()`'s real container path, and the internal `__pkgbuild` build-container path, which reports `"pkgbuild"` as its own image, an honest reflection of the real directory it actually uses) were updated; no behavior change for either beyond the new field now being visible in `GET /v1/containers`.
- No image *rename* or *clone* operation — creating a `router` image still means starting empty and `pkg install`ing into it, not forking from `base`'s own contents. Not a v1 need identified yet; a real, deliberate scope boundary, not an oversight.
