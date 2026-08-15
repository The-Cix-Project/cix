# Quickstart

The fastest real path from nothing to a running container. Every step here is a real command against a real daemon — nothing here is illustrative-only. Each step links to the guide that covers it in depth; this page stays deliberately thin.

## 1. Build and start the daemon

```sh
make
sudo build/thincd
```

Leave it running. In another shell:

```sh
build/thincctl health
```

`{"status": "ok"}` means it's up. Full detail: [`building-thinc.md`](building-thinc.md).

## 2. Give it something runnable

A freshly created image has no binaries in it at all — not even a shell — since everything is compiled from source, on demand. Stage a build toolchain once, then install a real, minimal package (`bash`) onto the default `base` image:

```sh
build/thincctl pkg bootstrap
build/thincctl pkg recipe add --name=bash --file=pkg/recipes/bash.recipe
build/thincctl pkg install --name=bash
```

Poll until it's done:

```sh
build/thincctl pkg ls
```

Full detail: [`writing-recipes.md`](writing-recipes.md).

## 3. Create a network

```sh
build/thincctl network create --name=lan1 --subnet=172.31.0.0 --prefix=24
```

Full detail: [`docs/api/README.md`](../api/README.md#creating-a-network).

## 4. Run a container

```sh
build/thincctl run --name=hello --image=base --network=lan1 -- /usr/bin/bash -c "echo hello from inside thinC"
build/thincctl inspect hello
```

Full detail: [`docs/api/README.md`](../api/README.md#creating-a-container).

## 5. Look around

```sh
build/thincctl ps
build/thincctl console hello   # a real interactive shell inside it, if it's still running
```

Or open `http://127.0.0.1/` in a browser for the same thing visually — see [`web-dashboard.md`](web-dashboard.md).

## Where to go next

- [`cli-reference.md`](cli-reference.md) — the full command surface
- [`installing.md`](installing.md) — putting this on real, booted hardware instead of a dev-machine daemon
- [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) / [`staying-updated.md`](staying-updated.md) — keeping an install current
- [`docs/api/README.md`](../api/README.md) — the full API surface this CLI and the dashboard are both built on
