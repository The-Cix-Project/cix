# `kanxeoctl` CLI reference

`kanxeoctl` is a pure REST client (`docs/api/openapi.yaml`) — every subcommand below is exactly one HTTP call, per the API-First Mandate (ADR-0005). This page is the CLI's own command surface; for what each call actually does, its request/response fields, and its error conditions, see [`docs/api/README.md`](../api/README.md) and `openapi.yaml` — this page deliberately doesn't repeat that.

## Global flags and invocation

```
kanxeoctl [--host=ADDR] [--port=N] [--json] <command> [args...]
```

- `--host=`/`--port=` — default `127.0.0.1:7620`.
- `--json` — print the raw API response instead of the default formatted text. Every subcommand supports it.
- Running `kanxeoctl` with no command at all, from a real terminal (`isatty(stdin)`), drops into an **interactive shell**: one line, one command, reusing the same connection — useful for a session of several related calls without re-establishing a TCP connection each time (`kanxeoctl --json` plus a piped/redirected stdin skips the shell and falls through to the usual usage-error path instead, so scripting is unaffected).
- **Exit codes**: `0` success, `1` the API call itself failed (a non-2xx response, or a transport-level failure reaching the daemon), `2` a usage error (bad flags, unknown subcommand) — checked before any network call is made.

## System

| Command | |
|---|---|
| `health` | Liveness check |
| `shutdown` | Stop `kanxeod`; powers off the host too when running as real PID 1 |
| `reboot` | Stop `kanxeod`; restarts the host too when running as real PID 1 |
| `update [--image=PATH] [--kernel=PATH]` | Write a fresh control-plane squashfs and/or kernel to the inactive A/B slot; does not reboot |
| `backup [--output=PATH]` | Bundle platform config state; prints it (or `--json`) by default, `--output=` saves verbatim for `restore --input=` |
| `restore --input=PATH` | Write a previously-saved bundle back; does not reboot or hot-reload |
| `site show` | This install's `instance_name`/`site_name`/`domain_suffix` |
| `site set [--instance-name=NAME] [--site-name=NAME] [--domain-suffix=NAME]` | Set them |
| `daemon-config show` | `kanxeod`'s own listen port, HTTP/HTTPS exposure, and which network is currently its management one |
| `daemon-config set [--port=N] [--https-port=N] [--enable-http] [--disable-http] [--enable-https] [--disable-https] [--management-network=NAME] [--bind-ip=A.B.C.D \| --clear-bind-ip]` | Live, no-restart change — only the fields given are touched. `bind_ip` (ADR-0068) is a dedicated second address on the management network's own bridge; `--clear-bind-ip` reverts to that network's own address |
| `routes` | The box's own real kernel IPv4 routing table (ADR-0066) — the only way to see this on a real install, no SSH/general shell |
| `routes add --dest=A.B.C.D --prefix=N [--gateway=A.B.C.D]` | Add a real kernel route (ADR-0067 Part 3); or `--default --gateway=A.B.C.D` for the default route |
| `routes rm --dest=A.B.C.D --prefix=N` | Remove one; or `--default` for the default route |
| `swap` | Whether the host swap file is enabled (ADR-0069) |
| `swap enable --size-mb=N` | Create and activate a swap file of this size |
| `swap disable` | Deactivate and remove it |
| `logs [--source=kernel\|kanxeod\|audit] [--level=...] [--tail=N] [--since=UNIXTS]` | The consolidated log (kernel dmesg + kanxeod diagnostics + per-request audit trail, ADR-0070) |
| `logs config [--max-bytes=N]` | Show or set the log's total size cap |

See [`docs/guides/kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) and [`docs/guides/staying-updated.md`](staying-updated.md) for `update`'s real operator runbooks, not just the flag syntax.

## Containers

| Command | |
|---|---|
| `ps` | List all containers |
| `run --name=NAME --image=IMAGE [flags...] -- CMD [ARGS...]` | Create and start a container — see below for the full flag list |
| `inspect NAME` | Show one container |
| `stop NAME` | Kill it now, keep its persisted definition (unlike `rm`) |
| `start NAME` | Bring a stopped-but-defined container back, no daemon restart needed |
| `pause NAME` / `unpause NAME` | Freeze/thaw via the real cgroup v2 freezer, not `SIGSTOP` |
| `stats NAME` | Real, host-side CPU/memory/disk/network usage, one point-in-time snapshot |
| `console NAME [--cmd=PATH]` | Interactive shell inside a running container (`docker exec -it`-style); `--cmd=` overrides the default `/usr/bin/bash` |
| `files get NAME --path=/some/path [--output=PATH]` | Read one file's raw bytes back out of a container's rootfs; stdout if `--output=` omitted |
| `rm NAME` | Stop (if running), remove, and forget any persisted definition |

`run`'s full flag set:

```
run --name=NAME --image=IMAGE
    [--memory-max=BYTES] [--pids-max=N] [--cpu-max="QUOTA PERIOD"] [--cpuset=0-1,3]
    [--disk-quota=BYTES]
    [--network=NAME[:IP] ...] [--ip-forward]
    [--dns-register]
    [--pki-issue] [--pki-cert-dir=PATH] [--pki-days=N]
    [--route=DEST/PREFIX:VIA ...]
    [--device=ID ...] [--interface=IFNAME ...]
    [--restart=always|on-failure|unless-stopped] [--restart-delay=N]
    [--depends-on=NAME ...]
    [--readiness-tcp-port=N [--readiness-timeout=N]]
    -- CMD [ARGS...]
```

Each flag maps directly to the matching `ContainerCreateRequest` field — see [`docs/api/README.md`](../api/README.md#creating-a-container) for what each one actually means and its validation rules (network membership, route format, restart-policy semantics, readiness checks, and so on); this reference only lists the CLI surface, not the payload contract behind it.

## Networks

| Command | |
|---|---|
| `network create --name=NAME --subnet=A.B.C.D --prefix=N [--address=A.B.C.D]` | Create a network — no `--address=` means pure L2, no host-owned address (the default) |
| `network ls` / `network rm NAME` | List / remove |
| `network attach-interface NAME --interface=IFNAME [--vlan=N]` | Enslave a real host interface to this network's bridge; `--vlan=` creates an 802.1q sub-interface instead |
| `network detach-interface NAME --interface=IFNAME` | Detach |

## Images

| Command | |
|---|---|
| `image create --name=NAME` | An empty image, C runtime pre-seeded, ready for `pkg install --image=NAME` |
| `image ls` / `image rm NAME` | List / remove (refused for `base`, in-use, or still has packages) |

## Devices

| Command | |
|---|---|
| `device ls` | Host PCI/USB/GPU devices from sysfs, with each one's `id` (pass to `run --device=`) and whether it's assignable |
| `devicemap create --name=NAME --kind=exact\|vendor_model --selector=SELECTOR` | A persisted, named device binding, usable in place of a raw id in `run --device=` |
| `devicemap ls` / `devicemap rm NAME` | List (shows whether each mapping currently resolves to real hardware) / remove |
| `disks [ls]` | Real host block devices (whole disks only), flagging which one is the fixed OS disk |
| `diskrole create --disk=NAME --role=container-storage\|backup` | Assign a persisted role to a disk (never the OS disk) |
| `diskrole ls` / `diskrole rm NAME` | List assigned roles (with whether each disk is currently present) / remove one |

## DNS

| Command | |
|---|---|
| `dns record create --name=NAME --ip=A.B.C.D` | Create a record |
| `dns record ls` / `dns record rm NAME` | List / remove |
| `dns server register --container=NAME --hosts-path=PATH` | Register a running container as a DNS-serving target |
| `dns server ls` / `dns server unregister CONTAINER` | List / unregister |

## PKI

| Command | |
|---|---|
| `pki ca bootstrap [--common-name=NAME] [--days=N]` / `pki ca show` | Bootstrap / inspect the root CA |
| `pki intermediate bootstrap [--common-name=NAME] [--days=N]` / `pki intermediate show` | Bootstrap / inspect a second CA tier — root must already be bootstrapped; once done, every future `pki cert create` is signed by it instead |
| `pki cert create --name=NAME [--sans=a,b,c] [--days=N]` | Issue a leaf certificate |
| `pki cert ls` / `pki cert rm NAME` | List / remove |
| `pki reset [--root-common-name=NAME] [--intermediate-common-name=NAME] [--root-days=N] [--intermediate-days=N] [--leaf-days=N]` | Destructive: wipe and regenerate the entire chain, reissuing every tracked leaf |

## Packages

| Command | |
|---|---|
| `pkg bootstrap [--toolchain=PATH]` | Stage a build toolchain into the shared build sandbox — see [`docs/guides/writing-recipes.md`](writing-recipes.md#build-images) |
| `pkg bootstrap --toolchain-url=URL --toolchain-sha256=SHA256 [--wait]` | The daemon fetches the toolchain itself, host-side — for a real minimal install with no SSH server (ADR-0065) |
| `pkg bootstrap-status` | State/error of the most recent `--toolchain-url=` fetch |
| `pkg recipes` | List recipes |
| `pkg recipe add --name=NAME --file=PATH` | Add or update a recipe on this running system directly, no reinstall needed |
| `pkg recipe show NAME` | Print a recipe's own raw content |
| `pkg recipe rm NAME` | Remove a recipe |
| `pkg install --name=NAME [--image=IMAGE] [--upgrade]` | Start installing (or upgrading) a package |
| `pkg ls` | List every known package (installed or in-flight) |
| `pkg rm NAME[@IMAGE]` | Uninstall |
| `pkg update-all` | Start an upgrade for the first installed package whose recipe has drifted; call again to drain the backlog |
| `pkg hostbuild NAME --build-image=IMAGE [--wait] [--deploy]` | Build a standalone host artifact (kernel, or Kanxeo's own control plane) instead of merging into an image — see [`docs/guides/writing-recipes.md#the-hostbuild-variant`](writing-recipes.md#the-hostbuild-variant) |

See [`docs/guides/writing-recipes.md`](writing-recipes.md) for the recipe format itself, and [`docs/guides/kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) / [`docs/guides/building-kanxeo.md`](building-kanxeo.md) for the two real operator runbooks built on `pkg hostbuild`.
