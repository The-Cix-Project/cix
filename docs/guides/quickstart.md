# Quickstart

From an installed Cix host to a running container you can open a shell in. Every command is `cixctl` against the host's REST API; the dashboard at `http://<host>/` can do all of it too ([web-dashboard.md](web-dashboard.md)). Each step links to the guide that covers it in depth.

## 1. Install a host

Build installer media and boot it: [installing.md](installing.md). A Cix host has no shell and no SSH; everything after this point is the API.

## 2. Reach it

```sh
cixctl --host=<host-ip> health
```

`{"status": "ok"}` means it's up. Every later command takes the same `--host=`; it is left out below for brevity.

If an operator has turned on write-gating ([security.md](security.md)), log in once. The token is saved in `~/.cixctl_token` and sent automatically from then on:

```sh
cixctl login --username=<user>      # prompts for the password
```

## 3. Put a program in the default image

Every host has an image called `base` from its first boot. An image gets even its C library as a package, and a container on an image with no C library is refused with an error naming what to install. At startup the daemon installs `glibc` into `base` by itself when it can do that from an already-built artifact, so check first:

```sh
cixctl pkg ls                       # is glibc listed for base?
cixctl pkg install --name=glibc     # only if it is not
```

Then install `bash` (the shell) and `coreutils` (`sleep`, `ls` and the rest). Neither brings `glibc` with it:

```sh
cixctl pkg install --name=bash
cixctl pkg install --name=coreutils
cixctl pkg ls                       # repeat until both show "installed"
```

`pkg install` uses an already-built artifact when one is available and builds from the recipe otherwise. Where recipes and artifacts come from on a fresh box (the installer's package seed, a configured artifact cache, a recipe repository) is in [installing.md](installing.md#building-the-iso) and [writing-recipes.md](writing-recipes.md).

## 4. Create a network

```sh
cixctl network create --name=lan1 --subnet=172.31.0.0 --prefix=24
```

The bridge stays pure layer 2 unless you also pass `--address=`. More in [networking.md](networking.md).

## 5. Run a container

A container declares the **services** it runs, not a single command ([ADR-0260](../adr/0260-a-container-declares-services-not-a-command.md)), and the **consoles** it offers. This one runs one long-lived service and offers a bash console:

```sh
cixctl container run --name=hello --image=base --network=lan1 \
    --service="main=/usr/bin/sleep infinity" \
    --console="shell=/usr/bin/bash"
cixctl container inspect hello
```

Quote a `--service=` or `--console=` value that contains spaces. The CLI splits the command on spaces and does no shell-style quoting inside it, so a command that needs a quoted argument (`bash -c "…"`) belongs in a script inside the image, not on this line.

## 6. Look around

```sh
cixctl container console hello      # an interactive shell inside it
cixctl process ls                   # every process on the box, with its container
```

## Where to go next

- [cli-reference.md](cli-reference.md): the full command surface
- [networking.md](networking.md), [storage.md](storage.md), [security.md](security.md): the host's subsystems
- [staying-updated.md](staying-updated.md): keeping an install current
- [docs/api/README.md](../api/README.md): the REST API the CLI and the dashboard are both built on
