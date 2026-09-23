# Images

An image is what a container runs from: a root filesystem made of a small created baseline (device nodes, directories, `nsswitch.conf`) plus the files of every package declared for it. Nothing else enters an image — nothing is copied from any host ([ADR-0209](../adr/0209-derived-images-and-one-container-mechanism.md), [ADR-0225](../adr/0225-build-images-are-composed-from-packages.md)).

Three ideas carry the whole model:

- **The manifest** is what the image should contain: a list of `package` / `mode` / `version` entries, where `mode` is `pinned` (exactly that version) or `rolling` (the highest available version at or above that floor) ([ADR-0107](../adr/0107-package-image-versioning.md)).
- **A version** is one immutable build of that content. Its identifier is a hash of the resolved `name@version` set ([ADR-0108](../adr/0108-image-version-content-hash.md)). A container records the version it was created from and keeps running it.
- **An image recipe** is the manifest written down as a file, so the image can be rebuilt from text ([ADR-0123](../adr/0123-pkg-redesign-part4-image-recipes-and-artifact.md), [ADR-0252](../adr/0252-an-image-recipe-is-authoritative.md)).

Every command below is `cixctl` against a real host (`--host=` implied). The request and response shapes are in [`openapi.yaml`](../api/openapi.yaml) under `/images`.

## Building an image from its recipe

The normal path. Image recipes live in the [cix-recipes](https://git.home.arpa/itdlabs/cix-recipes) repository as `recipes/image/<name>@<version>.sh` and reach a host with the recipe sync (`cixctl pkg sync`); for each image name, the highest recipe version is the one used. Then:

```sh
cixctl image recipe ls
cixctl image materialize dns
```

`image materialize NAME` creates the image if it does not exist, declares its manifest from the recipe of the same name, and installs every package in it, waiting for each and reporting it as `installed` or `already present`. It is safe to re-run, and it exits nonzero if any package did not install.

The same thing in its separate steps:

```sh
cixctl image create --name=dns
cixctl image apply-recipe dns             # declares the manifest; builds nothing
cixctl pkg install --name=glibc --image=dns
cixctl pkg install --name=dnsmasq --image=dns
```

`apply-recipe` only declares: it is refused while a package job is running, and no package is installed until `pkg install` runs.

## Writing an image recipe

An image recipe is one line:

```sh
image_packages="glibc:rolling:2.44 dnsmasq:rolling:2.90"
```

Each entry is `package:mode:version`. An image that runs containers needs a C library in it — a freshly created image has none, and creating a container from one is refused with `image "NAME" has no C library -- install a libc package (glibc) into it before running a container from it`. Declare `glibc` like any other package.

Publish it to the host:

```sh
cixctl image recipe add --name=dns --file=dns@1.2.0.sh
cixctl image recipe show dns
cixctl image recipe rm dns
```

The recipe's name is the image's name. `--file=` is read on the machine running `cixctl`. `image recipe add` replaces any existing recipe for that name, and removing a recipe does not touch the image or anything already installed in it. In the repository, commit the recipe as a new version file instead, so every host that syncs gets it.

Under [ADR-0252](../adr/0252-an-image-recipe-is-authoritative.md), an image that has a recipe is changed by changing its recipe: adding a package to such an image is a recipe change. The daemon does not refuse a direct `pkg install --image=` into a recipe-backed image; ADR-0252 records that as not yet enforced.

## Reading an image

```sh
cixctl image ls
cixctl image show dns
cixctl image manifest show --image=dns --version=<hash>
```

`image show` prints the current version, the declared manifest, and every version newest first, with the current one marked. The declared manifest is live intent: for a `rolling` entry its version is a floor, not the version installed. To see what a version actually holds — the question that matters for a container, since a container runs a fixed version — use `image manifest show` with that version's hash.

The dashboard shows the same under **Software > Build > Local Images** and each image's own page (Containers using this image, Packages installed, Manifest, Recipe, Versions).

## Changing the manifest by hand

For an image with no recipe:

```sh
cixctl image manifest set --image=tools --package=curl --mode=pinned --version=8.21.0
cixctl image manifest set --image=tools --package=zstd --mode=rolling --version=1.5.7
cixctl image manifest rm --image=tools --package=curl
```

`manifest set` adds or updates one entry; `manifest rm` removes one and succeeds if it is already absent. Neither builds anything: follow with `pkg install` for a package that is not installed yet.

## How an image moves forward

- **Installing produces a new version.** Each install into an image produces a new immutable version and makes it current. New containers are created from the current version.
- **Rolling entries rebuild on their own.** Publishing a new recipe version of a package queues a rebuild of every image that tracks that package as `rolling` with a floor at or below it. `cixctl pkg rebuilds` lists rebuilds queued but not started; `cixctl pipeline` shows where each one stands. Which version counts as highest can be changed per package ([ADR-0188](../adr/0188-per-package-rolling-policy.md)).
- **Existing containers stay where they are** unless they were created with `--follow-rolling` (or `follow_rolling` in a deployment), in which case they are restarted onto the new version, spread over the jitter window from `cixctl rolling-config` ([ADR-0124](../adr/0124-pkg-redesign-part5-rolling-containers-and-restart-jitter.md)). When a container's pinned version moves, its root filesystem is seeded again from the new version ([ADR-0277](../adr/0277-a-container-rootfs-is-derived-from-its-image.md)).
- **The same manifest is the same version.** Because a version is a hash of the resolved package set, reinstalling a package at the version it already has reproduces an existing hash, and the image points back at that existing version instead of the tree just built. When content changed but the package set did not, a new version appears only once the set itself changes ([ADR-0155](../adr/0155-baseline-reseed-manifest-hash-dedup-gap.md)).

## Reclaiming old versions

Nothing removes a version automatically, and every install leaves one behind. Reclaim the ones nothing uses:

```sh
cixctl image gc --dry-run      # list what would be removed; changes nothing
cixctl image gc                # remove it
```

A version is kept if it is its image's current version, if a container in the registry pins it, or if a stored container definition names it — a stopped container is revived from its definition, so its version must survive. `image gc` is refused while any package job is running. Removal cannot be undone, so run `--dry-run` first.

`--measure` adds the size of each collectable version. It walks every one of them, and the daemon answers no other request while it does, which can take minutes on a host with many versions; leave it off unless the number is needed.

The dashboard has the same pair under **Software > Build > Local Images**: **Preview reclaimable…** and **Reclaim unused versions**.

## Removing an image

```sh
cixctl pkg rm dnsmasq@tools
cixctl image rm tools
```

`image rm` is refused for `base`, for an image a running container uses, and for an image that still has packages installed — remove those first with `pkg rm NAME@IMAGE`.

## Build images

A build container runs from an image too. A build image — `cix-builder` and the others in [ADR-0208](../adr/0208-build-image-taxonomy.md) — is an ordinary image whose recipe declares the toolchain packages. It is built the same way, with `cixctl image materialize cix-builder`, and nothing from a host's filesystem goes into it ([ADR-0225](../adr/0225-build-images-are-composed-from-packages.md)). Which tools a package build gets is covered in [`writing-recipes.md`](writing-recipes.md).

## Where the decisions live

| Topic | ADR |
|---|---|
| Manifests, pinned and rolling, immutable versions, container pinning | [ADR-0107](../adr/0107-package-image-versioning.md) |
| A version is a hash of the resolved package set | [ADR-0108](../adr/0108-image-version-content-hash.md) |
| Image recipes | [ADR-0123](../adr/0123-pkg-redesign-part4-image-recipes-and-artifact.md), [ADR-0252](../adr/0252-an-image-recipe-is-authoritative.md) |
| Images are derived from packages; reclaiming unused versions | [ADR-0209](../adr/0209-derived-images-and-one-container-mechanism.md) |
| Build images are composed from packages | [ADR-0225](../adr/0225-build-images-are-composed-from-packages.md), [ADR-0208](../adr/0208-build-image-taxonomy.md) |
| Rolling containers and restart jitter | [ADR-0124](../adr/0124-pkg-redesign-part5-rolling-containers-and-restart-jitter.md) |
| Per-package version policy | [ADR-0188](../adr/0188-per-package-rolling-policy.md) |
| A container's rootfs follows its pinned version | [ADR-0277](../adr/0277-a-container-rootfs-is-derived-from-its-image.md) |
| Same-manifest reinstall reuses the existing version | [ADR-0155](../adr/0155-baseline-reseed-manifest-hash-dedup-gap.md) |
