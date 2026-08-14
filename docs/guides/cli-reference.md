# `kanxeoctl` CLI reference

`kanxeoctl` is a pure REST client (`docs/api/openapi.yaml`) — every subcommand below is exactly one HTTP call, per the API-First Mandate (ADR-0005). This page is the CLI's own command surface; for what each call actually does, its request/response fields, and its error conditions, see [`docs/api/README.md`](../api/README.md) and `openapi.yaml` — this page deliberately doesn't repeat that.

## Global flags and invocation

```
kanxeoctl [--host=ADDR] [--port=N] [--json] <command> [args...]
```

- `--host=`/`--port=` — default `127.0.0.1:7620`.
- `--json` — print the raw API response instead of the default formatted text. Every subcommand supports it except `console` (an interactive terminal session, not a per-call response) and `files get` (its own raw file bytes are the CLI's one non-JSON response body) — `--json` is silently ignored on those two.
- Running `kanxeoctl` with no command at all, from a real terminal (`isatty(stdin)`), drops into an **interactive shell**: one line, one command, reusing the same connection — useful for a session of several related calls without re-establishing a TCP connection each time (`kanxeoctl --json` plus a piped/redirected stdin skips the shell and falls through to the usual usage-error path instead, so scripting is unaffected). The prompt is the connected daemon's own `instance_name` (`GET /system/site`, e.g. `myhost> `), not a fixed string — useful the moment more than one Kanxeo install is reachable (ADR-0132).
- **Exit codes**: `0` success, `1` the API call itself failed (a non-2xx response, or a transport-level failure reaching the daemon), `2` a usage error (bad flags, unknown subcommand) — checked before any network call is made.
- **Authentication (ADR-0144)**: once a daemon has write-gating active (see [`docs/api/README.md`'s own "Host authentication" section](../api/README.md#host-authentication-adr-0144)), every mutating command needs a session — run `login` once and every subsequent `kanxeoctl` invocation on this machine authenticates automatically via the persisted token, until `logout` or the session's own idle timeout expires it. `GET`-only commands (`health`, `ps`, every `... ls`/`... show`) never need one.

## System

| Command | |
|---|---|
| `login [--username=NAME] [--password=PASS]` | Authenticate (ADR-0144) -- prompts for whichever of username/password isn't given as a flag, with terminal echo off for the password; on success persists the session token to `~/.kanxeoctl_token` (mode `0600`) so every subsequent invocation authenticates automatically. A no-op-equivalent (succeeds, but nothing enforces it) on a daemon where write-gating was never activated (no admin-group user exists yet) |
| `logout` | Invalidate the current session (if any) and remove the persisted token; always succeeds, even when not currently logged in |
| `health` | Liveness check -- minimal, no build/slot identity |
| `boot` | Build version/time, A/B slot, kernel version (ADR-0077) |
| `shutdown` | Stop `kanxeod`; powers off the host too when running as real PID 1 |
| `reboot` | Stop `kanxeod`; restarts the host too when running as real PID 1 |
| `update [--image=PATH] [--kernel=PATH]` | Write a fresh control-plane squashfs and/or kernel to the inactive A/B slot; does not reboot |
| `backup [--output=PATH]` | Bundle platform config state; prints it (or `--json`) by default, `--output=` saves verbatim for `restore --input=` |
| `restore --input=PATH` | Write a previously-saved bundle back; does not reboot or hot-reload |
| `backup-config show` | Which disk (if any) automatic backup snapshots write to, whether enabled, interval (ADR-0141) |
| `backup-config set [--disk=NAME\|--clear-disk] [--enable\|--disable] [--interval-hours=N]` | Only the fields given are changed; target disk must carry the `backup` role |
| `backup-config status` | Outcome of the most recent backup-snapshot attempt (manual or automatic) |
| `backup-config snapshot-now` | Write the same bundle `backup` produces to the configured disk right now |
| `site show` | This install's `instance_name`/`site_name`/`domain_suffix` |
| `site set [--instance-name=NAME] [--site-name=NAME] [--domain-suffix=NAME]` | Set them |
| `daemon-config show` | `kanxeod`'s own listen port, HTTP/HTTPS exposure, and which network is currently its management one |
| `daemon-config set [--port=N] [--https-port=N] [--enable-http] [--disable-http] [--enable-https] [--disable-https] [--management-network=NAME] [--bind-ip=A.B.C.D \| --clear-bind-ip]` | Live, no-restart change — only the fields given are touched. `bind_ip` (ADR-0068) is a dedicated second address on the management network's own bridge; `--clear-bind-ip` reverts to that network's own address |
| `hostauth-config show` | Current `admin_groups`/`idle_timeout_seconds`/live-LDAP backend settings (ADR-0144) |
| `hostauth-config set [--admin-group=NAME ...] [--idle-timeout-seconds=N] [--ldap-enable \| --ldap-disable] [--ldap-server=HOST ...] [--ldap-port=N] [--ldap-base-dn=NAME]` | Read-modify-write (the underlying `PUT` is full-replacement, but this command fetches the current config first so only the flags given actually change) -- write-gating activates the instant a real user is a member of one of `admin_groups` |
| `rolling-config show` | The configured rolling-restart jitter window (`jitter_window_seconds`) used by `run --follow-rolling` (ADR-0124) |
| `rolling-config set --jitter-window-seconds=N` | Set the jitter window — `0` disables jitter (restart happens immediately on every rolling reconcile) |
| `tls-throttle show` | Per-source-IP throttling config for repeated failed HTTPS handshakes (ADR-0134) |
| `tls-throttle set [--enabled \| --disabled] [--threshold=N] [--window-seconds=N] [--block-seconds=N] [--log-interval-seconds=N]` | Partial update — only the fields given are touched. `threshold` failures within `window-seconds` blocks a source, on both listeners, for `block-seconds`; loopback is never throttled. `log-interval-seconds` separately caps how often a repeatedly-failing source's own log line is written — `0` logs every failure |
| `tls-throttle status` | Every source currently tracked for failed handshakes, live (in-memory, not persisted) |
| `iso status` | Status of the most recent server-side installer ISO build |
| `iso build [--disk=DEV] [--ip=A.B.C.D] [--prefix=N] [--gateway=A.B.C.D] [--interface=IFNAME] [--wait]` | Assemble a fresh installer ISO server-side; all flags optional (unset fields fall back to the daemon's own defaults) — `--wait` polls until the build finishes instead of returning immediately |
| `routes` | The box's own real kernel IPv4 routing table (ADR-0066) — the only way to see this on a real install, no SSH/general shell |
| `host-stats` | Host-wide load/CPU/memory/disk/network snapshot, including cpu/memory/io pressure-stall (PSI) figures (ADR-0073, ADR-0074) — the host-level counterpart to `stats NAME` below |
| `process ls` | Every real process on the box (a direct `/proc` scan), each correlated to a container by its own real host ppid chain, if any (ADR-0131) |
| `process kill PID` | A real, immediate SIGKILL; refuses pid 1 and this daemon's own pid |
| `ping HOST` | Real ICMP echo against a literal IPv4 address (ADR-0075) — waits ~2s max, exits nonzero if unreachable |
| `resolv [show]` | The host's own outbound DNS resolver config (ADR-0076) |
| `resolv set [--nameserver=A.B.C.D ...]` | Replace it (repeatable flag, up to 3) — takes effect immediately, no reboot; no flags clears it |
| `time [show]` | The host's current date/time (ADR-0110) |
| `time set --unixtime=N` | Manually set the host clock (real `clock_settime()`, immediate, no reboot) |
| `ntp config [show]` | Upstream NTP server address list used to sync the host clock (ADR-0110) |
| `ntp config set [--server=A.B.C.D ...]` | Replace it (repeatable flag, up to 3); no flags clears it |
| `ntp status` | Most recent sync attempt's outcome/source/time |
| `ntp sync` | Trigger a sync attempt now, rather than waiting for the next hourly automatic one |
| `ntp server register --container=NAME` | Register a running container as an available internal NTP time source, mirrors `dns server register`/`ldap server register` |
| `ntp server ls` | List registered NTP server bindings |
| `ntp server unregister CONTAINER` | Unregister one (does not touch the container itself) |
| `syslog target register --container=NAME` | Register a running container (e.g. `syslog-1`/`syslog-2` running `sysklogd`) as an optional, redundant syslog forward target — every container-sourced log line is also sent to it as a real RFC 3164 UDP datagram, alongside the consolidated log store (ADR-0127) |
| `syslog target ls` | List registered syslog forward targets |
| `syslog target unregister CONTAINER` | Unregister one (does not touch the container itself) |
| `routes add --dest=A.B.C.D --prefix=N [--gateway=A.B.C.D]` | Add a real kernel route (ADR-0067 Part 3); or `--default --gateway=A.B.C.D` for the default route |
| `routes rm --dest=A.B.C.D --prefix=N` | Remove one; or `--default` for the default route |
| `swap` | Whether the host swap file is enabled (ADR-0069) |
| `swap enable --size-mb=N` | Create and activate a swap file of this size |
| `swap disable` | Deactivate and remove it |
| `logs [--source=kernel\|kanxeod\|audit\|container] [--level=...] [--container=NAME] [--regex=PATTERN] [--tail=N] [--since=UNIXTS]` | The consolidated log — kernel dmesg, kanxeod diagnostics, a per-request audit trail, and every container's own stdout/stderr, transparently (ADR-0070, ADR-0126) — `--container=` filters to one container's own lines, `--regex=` is a POSIX extended regex (case-insensitive) matched against the message text |
| `logs config [--max-bytes=N] [--min-level=LEVEL]` | Show or set the log's total size cap and/or minimum severity floor (`emerg`/`alert`/`crit`/`err`\|`error`/`warning`\|`warn`/`notice`/`info`/`debug`, default `debug`) -- either flag alone is fine, both are independent |

See [`docs/guides/kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) and [`docs/guides/staying-updated.md`](staying-updated.md) for `update`'s real operator runbooks, not just the flag syntax.

## Containers

| Command | |
|---|---|
| `ps` | List all containers |
| `container ls` | Same as `ps` — a noun-based synonym matching `dns`/`ldap`/`ntp`/etc.'s own `<noun> <verb>` shape (ADR-0132) |
| `run --name=NAME --image=IMAGE [flags...] -- CMD [ARGS...]` | Create and start a container — see below for the full flag list |
| `inspect NAME` | Show one container |
| `stop NAME` | Kill it now, keep its persisted definition (unlike `rm`) |
| `start NAME` | Bring a stopped-but-defined container back, no daemon restart needed |
| `pause NAME` / `unpause NAME` | Freeze/thaw via the real cgroup v2 freezer, not `SIGSTOP` |
| `stats NAME` | Real, host-side CPU/memory/disk/network usage, including this container's own cpu/memory/io pressure-stall (PSI) figures (ADR-0074), one point-in-time snapshot |
| `migrate-storage NAME [--disk=NAME]` | Move a container's own overlay storage to a disk carrying the `container-storage` role (or `--disk=` omitted for the default OS-disk placement) — briefly stops and automatically restarts the container for the final cutover; requires `restart` other than `"no"` (ADR-0142) |
| `migrate-storage-status NAME` | State/disk/error of the most recent (or running) container-storage migration |
| `console NAME [--cmd=PATH]` | Interactive shell inside a running container (`docker exec -it`-style); `--cmd=` overrides the default `/usr/bin/bash` |
| `files get NAME --path=/some/path [--output=PATH]` | Read one file's raw bytes back out of a container's rootfs; stdout if `--output=` omitted |
| `rm NAME` | Stop (if running), remove, and forget any persisted definition |

`run`'s full flag set:

```
run --name=NAME --image=IMAGE
    [--memory-max=BYTES] [--pids-max=N] [--cpu-max="QUOTA PERIOD"] [--cpuset=0-1,3]
    [--disk-quota=BYTES] [--disk=NAME]
    [--network=NAME[:IP] ...] [--ip-forward]
    [--dns-register]
    [--pki-issue] [--pki-cert-dir=PATH] [--pki-days=N]
    [--ldap-provision] [--ldap-user=NAME] [--ldap-group=NAME] [--ldap-uid=N] [--ldap-secret-dir=PATH]
    [--route=DEST/PREFIX:VIA ...]
    [--device=ID ...] [--interface=IFNAME ...]
    [--restart=always|on-failure|unless-stopped] [--restart-delay=N]
    [--follow-rolling] [--follow-rolling-jitter-seconds=N]
    [--depends-on=NAME ...]
    [--readiness-tcp-port=N [--readiness-timeout=N]]
    [--file=CONTAINER_PATH=LOCAL_PATH[:MODE] ...] [--file-owner=CONTAINER_PATH:UID:GID ...] [--sysctl=KEY=VALUE ...]
    [--dns-server=A.B.C.D ...]
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
| `image ls` / `image show NAME` / `image rm NAME` | List / inspect one (manifest, current version, and full version history) / remove (refused for `base`, in-use, or still has packages) |
| `image manifest set --image=NAME --package=NAME --mode=pinned\|rolling --version=VERSION` | Upsert one manifest entry (ADR-0107) -- `pinned` never auto-advances, `rolling` auto-rebuilds onto a newer recipe version as soon as one is published |
| `image manifest rm --image=NAME --package=NAME` | Remove one manifest entry |
| `image recipe add --name=NAME --file=PATH` | Publish a declarative image recipe (ADR-0123) -- recipe name and image name are 1:1; bulk-declares a manifest in one shot instead of one `image manifest set` per package |
| `image recipe show NAME` | Print a recipe's own raw content |
| `image recipe rm NAME` | Remove a stored image recipe |
| `image recipe ls` | List image recipes (metadata only) |
| `image apply-recipe NAME` | Apply `NAME`'s own stored recipe -- bulk-declares the manifest immediately (common case), or starts an async whole-rootfs artifact fetch for a fully-pinned recipe with a matching configured artifact server (poll `image recipe-apply-status`) |
| `image recipe-apply-status` | State/image/error of the most recent `image apply-recipe` artifact fetch |

## Devices

| Command | |
|---|---|
| `device ls` | Host PCI/USB/GPU devices from sysfs, with each one's `id` (pass to `run --device=`) and whether it's assignable |
| `devicemap create --name=NAME --kind=exact\|vendor_model --selector=SELECTOR` | A persisted, named device binding, usable in place of a raw id in `run --device=` |
| `devicemap ls` / `devicemap rm NAME` | List (shows whether each mapping currently resolves to real hardware) / remove |
| `disks [ls]` | Real host block devices (whole disks only), flagging which one is the fixed OS disk |
| `diskrole create --disk=NAME --role=container-storage\|backup\|state-storage\|rebuildable-storage\|log-storage` | Assign a persisted role to a disk (never the OS disk) |
| `diskrole ls` / `diskrole rm NAME` | List assigned roles (with whether each disk is currently present) / remove one (409 if the disk is the active state-storage placement) |
| `disks format NAME [--fs-type=ext4\|btrfs]` | Destructive: mkfs (ext4 by default, or btrfs, ADR-0104) + mount an already role-assigned, non-OS disk (409 against the active state-storage placement) |
| `disks format-status NAME` | State/mount_path/error of the most recent format job for this disk |
| `storage state [show]` | Which disk (if any) is the active placement for Kanxeo's own state (ADR-0141) |
| `storage state migrate [--disk=NAME]` | Move Kanxeo's own state to a disk already carrying the role and mounted; omit `--disk=` for the default OS-disk placement; live, no downtime |
| `storage state migrate-status` | State/disk/error of the most recent (or running) state-storage migration |
| `storage logs [show\|migrate [--disk=NAME]\|migrate-status]` | Same shape as `storage state`, for where the consolidated log store lives instead (ADR-0141 Phase 3); an independent job slot from `storage state migrate` |
| `storage rebuildable [show\|migrate [--disk=NAME]\|migrate-status]` | Same shape again, for where images/packages/artifacts live instead (ADR-0141 Phase 4); its own independent job slot |

## DNS

| Command | |
|---|---|
| `dns record create --name=NAME --ip=A.B.C.D` | Create a record |
| `dns record update --name=NAME --ip=A.B.C.D` | Edit an existing record's ip in place (task #749) |
| `dns record ls` / `dns record rm NAME` | List / remove |
| `dns server register --container=NAME --hosts-path=PATH` | Register a running container as a DNS-serving target |
| `dns server ls` / `dns server unregister CONTAINER` | List / unregister |
| `ldap server register --container=NAME --config-path=PATH` | Register a running container as the LDAP-serving target (task #725) -- `config_path` is its own absolute view of glauth's own config file |
| `ldap server ls` / `ldap server unregister CONTAINER` | List / unregister |
| `ldap group add --name=NAME [--gidnumber=N]` | Create a group (task #726) -- `--gidnumber=` optional, auto-allocated if omitted (task #748) |
| `ldap group update --name=NAME --gidnumber=N` | Edit an existing group's gidnumber in place (task #750) |
| `ldap group ls` / `ldap group rm NAME` | List / remove |
| `ldap user add --name=NAME [--uidnumber=N] --primarygroup=N [--secondary-groups=N,N,...] [--givenname=S] [--sn=S] [--mail=S] [--loginshell=S] [--homedirectory=S] [--password=S] [--disabled] [--ssh-key=S] [--can-search]` | Create a user -- `--uidnumber=` optional, auto-allocated if omitted (task #748); `--ssh-key=` optional, rendered as glauth's own `sshkeys` LDAP attribute, queried live by a container's own `AuthorizedKeysCommand` (task #731/ADR-0144 task #838); `--can-search` grants glauth's own minimal search capability, needed for a real bind/service account (`nslcd`, a live `AuthorizedKeysCommand`, ADR-0144 task #838), off by default |
| `ldap user update --name=NAME ...` | Edit an existing user in place -- full field replacement, same fields as `add` (task #731) |
| `ldap user ls` / `ldap user rm NAME` | List / remove |
| `ldap config show` | Show the current `start_uid`/`start_gid` auto-allocation floor (task #748) |
| `ldap config set --start-uid=N --start-gid=N` | Set the floor -- takes effect for future auto-allocations only, does not renumber existing users/groups |

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
| `pkg recipes` | List every published recipe version |
| `pkg recipe add --name=NAME --file=PATH` | Publish a new recipe version on this running system directly, no reinstall needed — immutable once published, rejected if this exact (name,version) already exists |
| `pkg recipe show NAME [--version=VERSION]` | Print a recipe version's own raw content; omitted version resolves to the highest available |
| `pkg recipe rm NAME [--version=VERSION]` | Remove recipe version(s); omitted removes every published version |
| `pkg repo-config show` | The currently configured recipe-sync source (empty if none) |
| `pkg repo-config set [--url=URL] [--kind=gitea\|github\|gitlab] [--ref=REF] [--token=TOKEN\|--clear-token] [--sync-interval=SECONDS]` | Partially update the configured repo; omitted flags leave that setting unchanged |
| `pkg sync [--wait]` | Fetch and merge the configured repo's recipes into this host's own catalog (additive — never overwrites an existing version) |
| `pkg sync-status` | The most recent (or currently running) sync's outcome |
| `pkg cache-config show` \| `set --max-bytes=N` | The local build-artifact cache's own size cap (always a real cap, no "unlimited" mode) |
| `pkg cache-status` | Current cache occupancy (max/current bytes, entry count) |
| `pkg cache-clear` | Remove every cached artifact — an explicit operator reset |
| `pkg artifact-config show` \| `set [--url=URL] [--token=TOKEN\|--clear-token]` | The configured plain-HTTP precompiled-artifact server — separate from `repo-config` above, never a git forge |
| `pkg install --name=NAME [--image=IMAGE] [--version=VERSION] [--upgrade]` | Start installing (or upgrading) a package; omitted version resolves to the highest available |
| `pkg ls` | List every known package (installed or in-flight) |
| `pkg rm NAME[@IMAGE]` | Uninstall |
| `pkg update-all` | Start an upgrade for the first installed package whose recipe has drifted; call again to drain the backlog |
| `pkg hostbuild NAME --build-image=IMAGE [--version=VERSION] [--wait] [--deploy] [--upgrade]` | Build a standalone host artifact (kernel, or Kanxeo's own control plane) instead of merging into an image — see [`docs/guides/writing-recipes.md#the-hostbuild-variant`](writing-recipes.md#the-hostbuild-variant). `--upgrade` re-runs a build already `state: "installed"` if the recipe's own version has moved on (otherwise a bare 409) |
| `pkg build-log` | Live-tail the currently in-flight install/hostbuild's own stdout/stderr (task #676, ADR-0101) — a one-way stream, not an interactive session; prints each chunk as it arrives and exits once the build finishes. 404 if nothing is currently building |

See [`docs/guides/writing-recipes.md`](writing-recipes.md) for the recipe format itself, and [`docs/guides/kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) / [`docs/guides/building-kanxeo.md`](building-kanxeo.md) for the two real operator runbooks built on `pkg hostbuild`.
