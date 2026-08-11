# Writing a package recipe

This is the canonical, complete reference for Kanxeo's recipe format — the source of truth this content lives in exactly once (`daemon/include/pkg.h`'s own header comment points here rather than repeating it). For what the REST endpoints that consume a recipe actually do, see [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs); for the underlying design rationale, see ADR-0036 (multi-source recipes) and ADR-0056 (the hostbuild variant).

## What a recipe is

A recipe is a POSIX shell script, one per package, matching the same well-proven format Gentoo ebuilds, Arch PKGBUILDs, and CRUX Pkgfiles all use. `kanxeod` treats it two completely different ways depending on which part is being read:

- **Metadata** (`pkg_name=`, `pkg_version=`, `pkg_source=`, `pkg_sha256=`, `pkg_depends=`) is read by a strict, non-executing line scanner (`parse_recipe()` in `daemon/src/pkg.c`) — the daemon never runs a shell interpreter over your recipe to extract these values.
- **Build logic** (`pkg_build()`/`pkg_install()`, real shell functions) is only ever invoked inside an isolated, network-less build container, `". /build/recipe.sh"` sourced by a tiny driver script. This is the *only* place a recipe's own shell code ever actually runs — never on the host, never outside a container.

This split is deliberate and load-bearing: a malicious or buggy recipe's shell code can corrupt its own build container's filesystem, but it can never touch the host, and it has no network access to exfiltrate anything even if it tried (this project's networking plane has no outbound NAT — see ADR-0007 and `daemon/include/pkg.h`'s own header comment).

## Required metadata fields

```sh
pkg_name="hello"
pkg_version="2.12.1"
pkg_source="https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db8"
pkg_depends=""
```

- **`pkg_name`** — must exactly match the `{name}` this recipe is uploaded as (`POST /pkg/recipes {"name": ...}`) — a mismatch is a `400`, so a bad upload can never silently attach to the wrong name. Same charset as every other simple name in this platform: `[A-Za-z0-9_-]+`.
- **`pkg_version`** — a plain string, compared byte-for-byte against what's installed to detect drift (`GET /pkg/{name}`'s `available_version` field, and what `POST /pkg/update-all` scans for). Bump it whenever the recipe changes in a way that should trigger an upgrade — there's no separate "recipe revision" concept, `pkg_version` *is* the revision.
- **`pkg_source`** — where to fetch from. Almost always an `https://` URL; a local self-hosted git remote's own archive-download endpoint works identically (see [`building-kanxeo.md`](building-kanxeo.md) for a real example). Fetched host-side by the daemon's own `curl` subprocess, before the build container ever starts — the container itself has no network access at all, so anything a build needs must already be named here.
- **`pkg_sha256`** — the fetched source's checksum, verified before extraction. A mismatch fails the job outright (`PKG_STATE_FAILED`, no partial state).
- **`pkg_depends`** — a space-separated list of other recipe names to install first (empty string if none). See [Dependencies](#dependencies) below. Must be exactly empty for a hostbuild recipe (see [The hostbuild variant](#the-hostbuild-variant) below) — dependency resolution has no meaning for a one-shot artifact harvest.

## Multi-source recipes

`pkg_source`/`pkg_sha256` are each really a space-separated *list*, positionally paired (ADR-0036) — a single URL is just the one-element case, which is why every simple recipe above still works unchanged. Real need: a package whose build fetches a few small external assets alongside its main tarball (lldap's own web frontend pulls CSS/JS/font files from a CDN), and the network-less build container can't fetch them itself.

```sh
pkg_source="https://example.org/pkg-1.0.tar.gz https://cdn.example.org/font.woff2 https://cdn.example.org/icons.css"
pkg_sha256="<tarball's sha256> <font.woff2's sha256> <icons.css's sha256>"
```

Index 0 is treated as "the" source: fetched, verified, and extracted into `/build/src` exactly like a single-source recipe. Every entry after that is fetched and verified the same way, but never extracted — each lands as a plain file at `/build/extra/<basename-of-its-own-URL>` for `pkg_build()`/`pkg_install()` to reference directly (e.g. `/build/extra/font.woff2`). Up to 16 entries (`PKG_MAX_SOURCES`). Any single entry's fetch failure or checksum mismatch fails the whole job — no partial-success state, matching the single-source case's own all-or-nothing guarantee.

**A real trap this bit in practice**: `/build/extra/`'s filename comes from `url_basename()` of the URL *as written in `pkg_source`*, not from the original file's name at its ultimate origin. If a source needs re-hosting — e.g. mirroring a CDN asset over the LAN (the [remote-development](remote-development.md) trick) for a box with no outbound DNS — the mirror URL's own path must still end in the exact basename `pkg_build()` expects, not a generic renamed pattern (`asset-1.src`, `pkg-name-version-N.src`, etc.). A real `lldap.recipe` LAN-mirror workaround once re-served all 8 of its CDN assets under a generic `lldap-0.6.3-N.src` naming scheme; every checksum passed, the whole build ran to completion, and only the final `cp /build/extra/bootstrap-nightshade.min.css ...` step failed with "No such file or directory" — silent and easy to miss until the log store's own tail-capture fix (ADR-0072) made the real error visible instead of truncated build-progress noise.

## The `pkg_build()`/`pkg_install()` contract

```sh
pkg_build() {
	./configure --prefix=/usr
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
}
```

Both run inside the isolated build container, in this order, with:

- **CWD already at `/build/src`** — the extracted source tree, one leading path component already stripped (so a tarball extracting to `hello-2.12.1/` still leaves you at its own root, not a level above it).
- **`PATH=/usr/bin:/bin`**, **`HOME=/build`** — nothing else in the environment. Whatever your build needs (a compiler, `make`, `autoconf`, ...) must already be present in the build image (see [Build images](#build-images) below) — there is no implicit toolchain.
- **No network access at all** — every source `pkg_build()`/`pkg_install()` could possibly need was already fetched host-side per [Multi-source recipes](#multi-source-recipes) above.
- **`$PKG_DESTDIR`** — set by the daemon to a real, empty staging directory (currently `/build/pkg-dest`). Everything `pkg_install()` writes under it is exactly what gets merged into the target image's rootfs once the build container exits successfully (an ordinary install) or harvested as a standalone artifact (a hostbuild — see below). The overwhelmingly common pattern is `make install DESTDIR="$PKG_DESTDIR"`, which every reasonably well-behaved upstream `Makefile`/`configure` script supports natively.

A failure at any point (`pkg_build()`/`pkg_install()` returning nonzero, the container itself failing to start) lands the package in `PKG_STATE_FAILED` with a diagnosable `error` field — nothing is silently dropped, and nothing partial gets merged into the target image.

### Real gotchas, found the hard way this project's own recipe catalog was built up against

These are genuine, confirmed environment facts about this project's own minimal images — not Kanxeo bugs, just what a from-scratch, `/usr/bin`-only, no-`/tmp` image actually looks like to a build script that assumes a normal Linux distro underneath it:

- **No `/bin`, only `/usr/bin`.** This project's images stage everything under `/usr/bin/` — `/bin/sh` doesn't exist unless something explicitly creates it (`bash.recipe`'s own `pkg_install()` symlinks it, see the real recipe below). glibc's `popen()`/`system()` hardcode `/bin/sh` with no override, so any build step that shells out (`make`'s own recipe lines, `configure`'s `$(shell ...)`-style macros) needs it present in the *build image*, not just the target.
- **No `/tmp`.** Use `/run` instead for any scratch path a build step needs.
- **Absolute tool paths inside a container, not bare names.** `gcc`/`ld` resolve their own installation prefix differently depending on how they're invoked (see `CLAUDE.md`'s own environment notes) — prefer `/usr/bin/gcc` over a bare `gcc` if a recipe's own build step execs a compiler directly rather than through `make`'s normal `$(CC)` indirection.
- **A recipe only ever sees what its build image already has.** If `pkg_build()` needs `bc`, `sed`, `flex`, or any other tool beyond the base toolchain, that tool needs its own recipe installed onto the build image first (ordinary `pkg install --image=<build-image> --name=<tool>`) — nothing is auto-detected or auto-installed on demand.

## Dependencies

`pkg_depends` is resolved automatically and recursively, before your own recipe's `pkg_build()` ever runs:

```sh
pkg_name="top"
pkg_depends="leaf1 leaf2"
```

Installing `top` installs `leaf1` and `leaf2` first (skipping any already installed), then `top` — one `POST /pkg/install {"name": "top"}` call, three packages end up tracked individually. A dependency diamond (two packages both depending on the same third one) installs the shared dependency exactly once. A missing recipe or a circular dependency (`A` needs `B` needs `A`) is rejected outright (`400`) before anything is fetched.

## Build images

Every install targets one image's rootfs — `pkg_build()`/`pkg_install()` write into a build *container* whose own toolchain comes from wherever that job's `build_image` (ordinary install) or `--build-image=` (hostbuild) rootfs already has installed. There's no single global toolchain; a build image is just an ordinary image built up the same way any other one is — `pkg install --image=my-builder --name=tcc`, `pkg install --image=my-builder --name=make`, and so on, one real dependency at a time, confirmed by a real build failure if something's still missing (the same discipline this project's own recipe catalog was built up with).

`POST /pkg/bootstrap` is a separate, one-time convenience specifically for the *shared, default* build sandbox ordinary installs use — see [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs) for that specific mechanism; it doesn't apply to a custom `build_image` used for a hostbuild.

## A complete, real worked example

`pkg/recipes/bash/5.2.37/build.sh`, verbatim, annotated with why each line is there:

```sh
pkg_name="bash"
pkg_version="5.2.37"
pkg_source="https://ftp.gnu.org/gnu/bash/bash-5.2.37.tar.gz"
pkg_sha256="9599b22ecd1d5787ad7d3b7bf0c59f312b3396d1e281175dd1f8a4014da621ff"
pkg_depends=""

pkg_build() {
	./configure --prefix=/usr
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	mkdir -p "$PKG_DESTDIR/bin"
	ln -sf /usr/bin/bash "$PKG_DESTDIR/bin/sh"
}
```

The extra `mkdir`/`ln` in `pkg_install()` is a real, deliberate fix, not boilerplate: any image that installs `bash` also gets a standard `/bin/sh -> /usr/bin/bash` symlink, the one path every real Linux distribution guarantees exists — closing the "no `/bin`" gotcha above for every future recipe that needs a working `/bin/sh` in its own build image.

## Adding, updating, and installing a recipe

```
POST /v1/pkg/recipes
{"name": "hello", "content": "pkg_name=hello\npkg_version=2.12.1\n..."}
```

Publishes a new `(name, version)` recipe (ADR-0107) — validated (parses, and its own `pkg_name=`/`pkg_version=` match `name` and the version this call actually publishes) before anything on disk changes. Recipe versions are immutable once published: an already-published `(name, version)` pair is rejected (`409 Conflict`), not silently overwritten — fixing a mistake means bumping `pkg_version=` and publishing again, not re-uploading under the same version. `kanxeoctl pkg recipe add --name=hello --file=./hello.recipe` is the CLI equivalent. A bare `pkg install`/`pkg hostbuild` (no explicit `version`) always resolves to the highest published version for that name. Then:

```
POST /v1/pkg/install
{"name": "hello"}
```

starts the actual build — see [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs) for the full async install/poll/upgrade contract, which this guide doesn't repeat.

## The hostbuild variant

A recipe can also be built as a standalone, host-side artifact instead of merging into a container image's rootfs — used for building the Linux kernel and for [self-hosted rebuilds of Kanxeo's own control plane](building-kanxeo.md) (ADR-0056). The recipe format is identical; the only hard requirement is `pkg_depends=""` (empty), since dependency resolution targets "merge into an image," a concept with no meaning for a one-shot harvest — every prerequisite the build needs must already be installed onto the named `--build-image=` beforehand, via ordinary `pkg install` calls against that image, exactly as described in [Build images](#build-images) above.

```sh
pkg_install() {
	cp build/mything "$PKG_DESTDIR/mything"
}
```

`$PKG_DESTDIR`'s contents are copied verbatim to `BASE_DIR/artifacts/<name>/` on the host instead of being merged anywhere — see [`docs/guides/kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) and [`docs/guides/building-kanxeo.md`](building-kanxeo.md) for the two real, complete operator runbooks built on this mechanism.
