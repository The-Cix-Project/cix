# `thincctl` CLI reference

`thincctl` is a pure REST client (`docs/api/openapi.yaml`) — every subcommand below is exactly one HTTP call, per the API-First Mandate (ADR-0005). This page is the CLI's own command surface; for what each call actually does, its request/response fields, and its error conditions, see [`docs/api/README.md`](../api/README.md) and `openapi.yaml` — this page deliberately doesn't repeat that.

## Global flags and invocation

```
thincctl [--host=ADDR] [--port=N] [--json] <command> [args...]
```

- `--host=`/`--port=` — default `127.0.0.1:80`.
- `--json` — print the raw API response instead of the default formatted text. Every subcommand supports it except `console` (an interactive terminal session, not a per-call response) and `files get`/`files put` (`get`'s own raw file bytes are the CLI's one non-JSON response body, and `put`'s own success response is a real `204 No Content` with nothing to render as JSON) — `--json` is silently ignored on all three.
- Running `thincctl` with no command at all, from a real terminal (`isatty(stdin)`), drops into an **interactive shell**: one line, one command, reusing the same connection — useful for a session of several related calls without re-establishing a TCP connection each time (`thincctl --json` plus a piped/redirected stdin skips the shell and falls through to the usual usage-error path instead, so scripting is unaffected). The prompt is the connected daemon's own full site identity (`GET /system/site`, `instance.site.domain` or `instance.domain` with no site tier set, e.g. `lab.uk.home.arpa> `), not a fixed string — useful the moment more than one thinC install is reachable (ADR-0132). The trailing character follows real shell/network-device convention: `>` normally, `#` once `login` succeeds (`GET /whoami`, ADR-0164) -- refreshed immediately after every `login`/`logout` command, not just at shell startup.
- **Exit codes**: `0` success, `1` the API call itself failed (a non-2xx response, or a transport-level failure reaching the daemon), `2` a usage error (bad flags, unknown subcommand) — checked before any network call is made.
- **Authentication (ADR-0144)**: once a daemon has write-gating active (see [`docs/api/README.md`'s own "Host authentication" section](../api/README.md#host-authentication-adr-0144)), every mutating command needs a session — run `login` once and every subsequent `thincctl` invocation on this machine authenticates automatically via the persisted token, until `logout` or the session's own idle timeout expires it. `GET`-only commands (`health`, `container ls`, every `... ls`/`... show`) never need one.

## System

| Command | |
|---|---|
| `login [--username=NAME] [--password=PASS]` | Authenticate (ADR-0144) -- prompts for whichever of username/password isn't given as a flag, with terminal echo off for the password; on success persists the session token to `~/.thincctl_token` (mode `0600`) so every subsequent invocation authenticates automatically. A no-op-equivalent (succeeds, but nothing enforces it) on a daemon where write-gating was never activated (no admin-group user exists yet) |
| `logout` | Invalidate the current session (if any) and remove the persisted token; always succeeds, even when not currently logged in |
| `health` | Liveness check -- minimal, no build/slot identity |
| `boot` | Build version/time, A/B slot, kernel version (ADR-0077) |
| `shutdown` | Stop `thincd`; powers off the host too when running as real PID 1 |
| `reboot` | Stop `thincd`; restarts the host too when running as real PID 1 |
| `update [--image=PATH] [--kernel=PATH]` | Write a fresh control-plane squashfs and/or kernel to the inactive A/B slot; does not reboot |
| `backup [--output=PATH]` | Bundle platform config state; prints it (or `--json`) by default, `--output=` saves verbatim for `restore --input=` |
| `restore --input=PATH` | Write a previously-saved bundle back; does not reboot or hot-reload |
| `backup-config show` | Which disk (if any) automatic backup snapshots write to, whether enabled, interval (ADR-0141) |
| `backup-config set [--disk=NAME\|--clear-disk] [--enable\|--disable] [--interval-hours=N]` | Only the fields given are changed; target disk must carry the `backup` role |
| `backup-config status` | Outcome of the most recent backup-snapshot attempt (manual or automatic) |
| `backup-config snapshot-now` | Write the same bundle `backup` produces to the configured disk right now |
| `site show` | This install's `instance_name`/`site_name`/`domain_suffix` |
| `site set [--instance-name=NAME] [--site-name=NAME] [--domain-suffix=NAME]` | Set them |
| `daemon-config show` | `thincd`'s own listen port, HTTP/HTTPS exposure, and which network is currently its management one |
| `daemon-config set [--port=N] [--https-port=N] [--enable-http] [--disable-http] [--enable-https] [--disable-https] [--management-network=NAME] [--bind-ip=A.B.C.D \| --clear-bind-ip]` | Live, no-restart change — only the fields given are touched. `bind_ip` (ADR-0068) is a dedicated second address on the management network's own bridge; `--clear-bind-ip` reverts to that network's own address |
| `hostauth-config show` | Current `admin_groups`/`idle_timeout_seconds`/live-LDAP backend settings (ADR-0144) |
| `hostauth-config set [--admin-group=NAME ...] [--idle-timeout-seconds=N] [--ldap-enable \| --ldap-disable] [--ldap-server=HOST ...] [--ldap-port=N] [--ldap-base-dn=NAME]` | Read-modify-write (the underlying `PUT` is full-replacement, but this command fetches the current config first so only the flags given actually change) -- write-gating activates the instant a real user is a member of one of `admin_groups` |
| `hostauth-sessions ls` | Every active session (username, expires-in) -- never a raw token, before or after issuance (ADR-0152) |
| `hostauth-sessions revoke USERNAME` | Log that user out everywhere -- revokes every active session for it at once |
| `rolling-config show` | The configured rolling-restart jitter window (`jitter_window_seconds`) used by `container run --follow-rolling` (ADR-0124) |
| `rolling-config set --jitter-window-seconds=N` | Set the jitter window — `0` disables jitter (restart happens immediately on every rolling reconcile) |
| `pkg-build-config show` | The configured pkg install/hostbuild concurrency ceiling (`max_concurrent_jobs`, default 10, ADR-0157) |
| `pkg-build-config set --max-concurrent-jobs=N` | Set it — 1-10; lowering it doesn't disrupt jobs already in flight, only future ones |
| `tls-throttle show` | Per-source-IP throttling config for repeated failed HTTPS handshakes (ADR-0134) |
| `tls-throttle set [--enabled \| --disabled] [--threshold=N] [--window-seconds=N] [--block-seconds=N] [--log-interval-seconds=N]` | Partial update — only the fields given are touched. `threshold` failures within `window-seconds` blocks a source, on both listeners, for `block-seconds`; loopback is never throttled. `log-interval-seconds` separately caps how often a repeatedly-failing source's own log line is written — `0` logs every failure |
| `tls-throttle status` | Every source currently tracked for failed handshakes, live (in-memory, not persisted) |
| `iso status` | Status of the most recent server-side installer ISO build |
| `iso build [--disk=DEV] [--ip=A.B.C.D] [--prefix=N] [--gateway=A.B.C.D] [--interface=IFNAME] [--wait]` | Assemble a fresh installer ISO server-side; all flags optional (unset fields fall back to the daemon's own defaults) — `--wait` polls until the build finishes instead of returning immediately |
| `routes` | The box's own real kernel IPv4 routing table (ADR-0066) — the only way to see this on a real install, no SSH/general shell |
| `host-stats` | Host-wide uptime/load/CPU/memory/disk/network snapshot, including cpu/memory/io pressure-stall (PSI) figures (ADR-0073, ADR-0074) — the host-level counterpart to `stats NAME` below. `uptime.host` is seconds since boot, `uptime.daemon` seconds since `thincd` itself started; they diverge after a control-plane restart that was not a reboot |
| `kmsg [--tail=N]` | The kernel's own ring buffer (`/dev/kmsg`) — dmesg over REST (issue #77). Mount failures, driver probes and OOM kills surface here, and on a real installed host with no shell this is the only way to read them. Distinct from the log store, which carries `thincd`'s own diagnostics and the audit trail |
| `server-health [ls]` | Health of every registered LDAP/DNS/NTP/syslog server (issue #81) — state, whether it's in service, how it was probed (`tcp:PORT` is a real service check; `process` only means the container is running), and the last error |
| `server-health drain KIND NAME` / `server-health undrain KIND NAME` | Take one deliberately out of / back into service (maintenance). Persisted across a daemon restart, unlike the observed health state. A drained or unhealthy server is withheld from the client config thinC generates |
| `process ls` | Every real process on the box (a direct `/proc` scan), each correlated to a container by its own real host ppid chain, if any (ADR-0131) |
| `process kill PID` | A real, immediate SIGKILL; refuses pid 1 and this daemon's own pid |
| `ping HOST` | Real ICMP echo against a literal IPv4 address (ADR-0075) — waits ~2s max, exits nonzero if unreachable |
| `resolv [show]` | The host's own outbound DNS resolver config (ADR-0076) |
| `resolv set [--nameserver=A.B.C.D ...]` | Replace it (repeatable flag, up to 3) — takes effect immediately, no reboot; no flags clears it |
| `sysctl [show]` | Every host-level sysctl currently persisted for reapply at boot (ADR-0160) — not the full kernel sysctl tree |
| `sysctl get KEY` | The true current live value of one host-level sysctl (persisted or not), e.g. `net.ipv4.ip_forward` |
| `sysctl set KEY --value=V [--value=V ...] [--no-persist]` | Live write against the host's real `/proc/sys` — no key allowlist, dots translate to slashes. Repeat `--value=` for a tuple-shaped key (e.g. `net.ipv4.ip_local_port_range --value=32768 --value=60999`). Persists for reapply at every boot unless `--no-persist` is given |
| `sysctl rm KEY` | Remove a key from the persisted boot-apply list only — never touches the live value |
| `kmod [ls]` | Every currently-loaded kernel module (live `/proc/modules`, ADR-0159) |
| `kmod show NAME` | Real `modinfo`: description, params, depends, in-tree vs. out-of-tree — for a module that's built/available whether loaded or not |
| `kmod load NAME [--option=KEY=VALUE ...]` | Real `modprobe`; no `--option=` falls back to this module's own persisted `kmod-config` default options |
| `kmod unload NAME` | Real `modprobe -r` (reverse-dependency-aware) |
| `kmod-config [ls]` | Every module with a persisted default-options and/or autoload entry |
| `kmod-config set NAME [--option=KEY=VALUE ...] [--autoload \| --no-autoload]` | Read-modify-write — only the fields given are touched |
| `kmod-config rm NAME` | Clear a module's persisted config entirely — never touches whether it's currently loaded |
| `kmod-build --build-image=IMAGE [--version=VERSION] [--symbol=CONFIG_FOO ...] [--upgrade] [--wait] [--keep-on-failure]` | An ordinary `pkg hostbuild kernel` under the hood, gaining extra `=m` module symbols merged into the same curated kernel config (ADR-0159 Phase B) — needs a reboot onto the new `bzImage` (A/B cutover) to actually take effect. `--keep-on-failure` (ADR-0175) preserves a crashing kernel/toolchain build container for real debugging instead of losing it on failure |
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
| `swap` | Whether the host swap file is enabled (ADR-0069) and which disk (if any) it's placed on |
| `swap enable --size-mb=N [--disk=NAME]` | Create and activate a swap file of this size; `--disk=` places it on a disk carrying the `swap` role (issue #28) instead of the default OS-disk location |
| `swap disable` | Deactivate and remove it |
| `dhcp show` | Every DHCP-configured network and every static reservation (ADR-0197) |
| `dhcp leases` | Current leases, read from the serving container's own lease file. A lease is not a DNS record -- it resolves anyway, because the same dnsmasq issues it and answers for it |
| `dhcp server ls \| add CONTAINER \| rm CONTAINER` | Registered DHCP servers. `rm` also drops the server from every range that named it, and disables any range left with none |
| `dhcp enable --network=NAME --range=START-END --server=CONTAINER [--server=CONTAINER ...] [--lease-seconds=N] [--router=IP]` | Enable DHCP on a network. `--server=` is repeatable: dnsmasq has no failover protocol, so a range named to more than one server is split into disjoint slices -- all answer, none share an address. **Changes to a range restart the servers that serve it**, since dnsmasq reads ranges only at startup |
| `dhcp disable --network=NAME` | Turn it off |
| `dhcp static add --mac=M --ip=IP [--hostname=NAME]` | Reserve an address for a MAC. Live -- no restart |
| `dhcp static rm MAC` | Remove a reservation |
| `zswap show` | The compressed swap cache (issue #51): configured intent, what the kernel actually reports, and the compressors this kernel was built with. A disagreement between the first two means the setting did not take |
| `zswap set [--enable \| --disable] [--max-pool-percent=N] [--compressor=NAME]` | Configure it. On by default -- it costs nothing until the box is actually swapping |
| `logs [--source=kernel\|thincd\|audit\|container] [--level=...] [--container=NAME] [--regex=PATTERN] [--tail=N] [--since=UNIXTS]` | The consolidated log — kernel dmesg, thincd diagnostics, a per-request audit trail, and every container's own stdout/stderr, transparently (ADR-0070, ADR-0126) — `--container=` filters to one container's own lines, `--regex=` is a POSIX extended regex (case-insensitive) matched against the message text |
| `logs config [--max-bytes=N] [--min-level=LEVEL]` | Show or set the log's total size cap and/or minimum severity floor (`emerg`/`alert`/`crit`/`err`\|`error`/`warning`\|`warn`/`notice`/`info`/`debug`, default `debug`) -- either flag alone is fine, both are independent |

See [`docs/guides/kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) and [`docs/guides/staying-updated.md`](staying-updated.md) for `update`'s real operator runbooks, not just the flag syntax.

## Containers

Every container operation is a subcommand of `container` — one noun-based namespace matching `dns`/`ldap`/`ntp`/etc.'s own `<noun> <verb>` shape (ADR-0132, issue #74). There are no bare top-level container verbs (no plain `ps`/`run`/`rm`/…).

| Command | |
|---|---|
| `container ls` | List all containers (running, stopped, and exited) |
| `container run --name=NAME --image=IMAGE [flags...] -- CMD [ARGS...]` | Create and start a container — see below for the full flag list |
| `container inspect NAME` | Show one container |
| `container edit NAME --json='{...}'` | Edit the stored definition in place — cmd, env, files, limits, volumes (issue #11). Applies at the container's next start; the output says so, and says when a restart is needed. `name` and the index fields (`restart`/`depends_on`/`readiness`/`follow_rolling`) are refused with a 400 naming them |
| `container stop NAME` | Kill it now, keep its persisted definition — it reappears as `stopped` (only `container rm` removes it; ADR-0181) |
| `container start NAME` | Bring a stopped or exited container back, no daemon restart needed |
| `container pause NAME` / `container unpause NAME` | Freeze/thaw via the real cgroup v2 freezer, not `SIGSTOP` |
| `container stats NAME` | Real, host-side CPU/memory/disk/network usage, including this container's own cpu/memory/io pressure-stall (PSI) figures (ADR-0074), one point-in-time snapshot |
| `container migrate-storage NAME [--disk=NAME]` | Move a container's own overlay storage to a disk carrying the `container-storage` role (or `--disk=` omitted for the default OS-disk placement) — briefly stops and automatically restarts the container for the final cutover; requires `restart` other than `"no"` (ADR-0142) |
| `container migrate-storage-status NAME` | State/disk/error of the most recent (or running) container-storage migration |
| `container console NAME [--cmd=PATH]` | Interactive shell inside a running container (`docker exec -it`-style); `--cmd=` overrides the default `/usr/bin/bash` |
| `container files get NAME --path=/some/path [--output=PATH]` | Read one file's raw bytes back out of a container's rootfs; stdout if `--output=` omitted |
| `container files put NAME --path=/some/path --file=LOCAL_PATH [--mode=0644]` | Write/overwrite one file inside an already-existing container, live and ephemeral, without a recreate (ADR-0153) |
| `container rm NAME` | Stop (if running), remove, and forget the persisted definition — the only way to make a container truly gone (ADR-0181) |
| `container recipe add --name=NAME --file=PATH` | Publish a container recipe (ADR-0151) -- content must already be a full `POST /containers` body, its own `"name"` matching NAME |
| `container recipe show NAME` | Print a recipe's own raw, unsubstituted content |
| `container recipe rm NAME` | Remove a stored container recipe |
| `container recipe ls` | List container recipes (metadata only) |
| `container apply-recipe NAME [--secret=KEY=VALUE ...]` | Render `NAME`'s own stored recipe (substituting `{{SECRET:KEY}}` tokens, plus `{{LDAP:*}}` tokens from `ldap config`'s stored client settings) and create the container -- always synchronous, real `POST /containers` under the hood |
| `container network attach NAME --network=NETWORK [--ip=A.B.C.D]` | Attach a network to an already-running container, live, without a recreate (ADR-0156) |
| `container network detach NAME NETWORK` | Detach a live-attached network; refuses (409) a network attached at container creation |
| `container device attach NAME ID` | Live-grant one more device to an already-running container, no recreate (ADR-0161 Phase D) — `ID` is a real device id or devicemap name, resolved fresh |
| `container device detach NAME ID` | Detach a live-attached device; refuses (409) a device granted at container creation |

`container run`'s full flag set:

```
container run --name=NAME --image=IMAGE
    [--memory-max=BYTES] [--pids-max=N] [--cpu-max="QUOTA PERIOD"] [--cpuset=0-1,3]
    [--memory-swap-max=BYTES]   -- 0 = may not swap at all; omit = swap unlimited
    [--disk-quota=BYTES] [--disk=NAME]
    [--network=NAME[:IP] ...] [--ip-forward]
    [--userns] [--ldap-client] [--ldap-allow-group=NAME ...] [--capture-output]
    [--dns-register]
    [--pki-issue] [--pki-cert-dir=PATH] [--pki-days=N]
    [--ldap-provision] [--ldap-user=NAME] [--ldap-group=NAME] [--ldap-uid=N] [--ldap-secret-dir=PATH]
    [--route=DEST/PREFIX:VIA ...]
    [--device=ID ...] [--optional-device=ID ...] [--interface=IFNAME ...]
    [--cap-add=CAP_NAME ...]
    [--restart=always|on-failure|unless-stopped] [--restart-delay=N]
    [--follow-rolling] [--follow-rolling-jitter-seconds=N]
    [--depends-on=NAME ...]
    [--readiness-tcp-port=N [--readiness-timeout=N]]
    [--file=CONTAINER_PATH=LOCAL_PATH[:MODE] ...] [--file-owner=CONTAINER_PATH:UID:GID ...] [--sysctl=KEY=VALUE ...]
    [--env=KEY=VALUE ...]
    [--dns-server=A.B.C.D ...]
    -- CMD [ARGS...]
```

Each flag maps directly to the matching `ContainerCreateRequest` field — see [`docs/api/README.md`](../api/README.md#creating-a-container) for what each one actually means and its validation rules (network membership, route format, restart-policy semantics, readiness checks, and so on); this reference only lists the CLI surface, not the payload contract behind it. `--optional-device=ID` (ADR-0161 Phase B) is the one exception to "a bad device id fails creation": unlike `--device=ID`, a currently-unresolvable `--optional-device=` still creates the container, without that grant — the reference itself is remembered and matched against real hardware as it appears, see [`administration.md`](administration.md#device-hotplug) for the operator-facing walkthrough.


| Command | |
|---|---|
| `exec NAME -- COMMAND [ARGS...]` | Run a command inside a **running** container's own namespaces and print what it wrote. No shell (nothing reinterprets your arguments) and no pty (nothing echoes or edits the output) &mdash; which is what makes it a usable diagnostic where the interactive console is not. Exits with the command's own status |

## Networks

| Command | |
|---|---|
| `network create --name=NAME --subnet=A.B.C.D --prefix=N [--address=A.B.C.D] [--alloc-start=IP --alloc-end=IP]` | Create a network — no `--address=` means pure L2 (the default); `--alloc-start/--alloc-end` bound the auto-IP window (issue #70; a management network skips `.1` by default regardless) |
| `network ls` / `network rm NAME` | List / remove |
| `network ports NAME` | What is plugged into this network's bridge right now, per port, with each port's own counters (issue #26). The list is the kernel's, so a port nothing can account for prints as `unattributed` rather than being left out |
| `network attach-interface NAME --interface=IFNAME [--vlan=N]` | Enslave a real host interface to this network's bridge; `--vlan=` creates an 802.1q sub-interface instead |
| `network detach-interface NAME --interface=IFNAME` | Detach |



| Command | |
|---|---|
| `stalls` | Times the control plane stopped going round its own loop, with the kernel function it was sleeping in and the request it was serving (issue #100) — written by a watchdog process, because the loop cannot record its own silence |
| `kernel-policy show` | Which kernel line this box tracks, what that channel is at, and whether the running kernel is behind it (issue #65) |
| `kernel-policy set --channel=pinned\|longterm\|stable\|mainline` | Set the channel — kernel.org's own monikers. `pinned` (the default) proposes no version at all; the pin stays in the kernel recipe |
| `kernel-policy refresh` | Re-ask kernel.org's `releases.json` what each channel is at. Async — the answer lands a moment after the command returns |
| `boot-console show` | The installed system's own boot console parameters, plus the options line each loader entry currently carries (issue #24) |
| `boot-console set [--console=NAME ...] [--extra="..."]` | Set them — `--console` is repeatable and ordered. Rewrites the loader entries on the ESP; takes effect at the next boot. Everything from `root=` onward is left alone |
| `control-plane-reservation show` | How much CPU/memory is held back for the daemon itself, the host's totals, and the derived ceiling actually applied to the `thinc-workload` cgroup every container and build lives under (issue #86) |
| `control-plane-reservation set [--enabled\|--disabled] [--cpu-percent=N] [--memory-bytes=N]` | Change it — applied to the live cgroup immediately. `cpu_percent` 1-50; a larger reservation would be a second workload budget, not a safety margin |
| `factory-reset --confirm=<instance name>` | Return the box to its just-installed state and reboot. Destroys every container, image, network, registration, package state, the log store, and **every volume and all data in them**. Keeps the installed OS; forgets disk roles without reformatting the disks |

## Software

| Command | |
|---|---|
| `software` | What is declared (has a recipe) against what is actually installed. Flags anything installed with **no recipe** — it cannot be rebuilt from source control, so either capture one or it is debris |

## Volumes

Storage whose lifetime is independent of any container using it — deleting a container never removes its volumes. That is what makes it the right home for a jump host's `/home`, a database's data directory, or anything else worth keeping across the recreates that `follow_rolling` and recipe edits perform routinely. See [ADR-0183](../adr/0183-persistent-volumes.md) for the reasoning and [`docs/api/README.md`](../api/README.md#persistent-volumes-issue-88-adr-0183) for the payload contract.

| Command | |
|---|---|
| `volume create --name=NAME [--disk=DISK] [--owner-uid=N --owner-gid=N]` | Create a persistent volume. Without an owner it belongs to root, which a non-root workload cannot write to (issue #102) |
| `volume owner NAME --uid=N --gid=N [--recursive]` | Hand a volume to the account that will use it. `--recursive` also rewrites what is already inside; off by default, since a volume in use holds files whose ownership may have been set deliberately |
| `volume owner NAME --root` | Hand it back to root |
| `volume ls` / `volume show NAME` | List / inspect one (disk, resolved host path, creation time) |
| `volume backups NAME [--enable\|--disable] [--retain=N] [--while-running=refuse\|pause\|allow]` | Show or set a volume's backup policy. Opt-in. `--while-running` decides what happens when a container is using it: `refuse` skips (and an always-on container means never), `pause` freezes every container using it for the copy then resumes them (a genuinely consistent snapshot, at the cost of real downtime), `allow` copies live and accepts a crash-consistent snapshot |
| `volume backup NAME` | Take one snapshot now |
| `volume restore NAME SNAPSHOT` | **Replace** the volume's contents with that snapshot |
| `volume quota NAME BYTES` | Set a real, kernel-enforced size limit (0 removes it). Without one a volume can grow until its disk is full |
| `volume migrate NAME [--disk=DISK]` | Move its data to another disk or partition; omit `--disk` to move it back to the default OS-disk placement. Refused while a container mounting it is running |
| `volume rm NAME` | **Deletes the volume's data**, permanently — refused while any container *definition* references it (the error names which one) |

Attach or detach one on a container that already exists:

| Command | |
|---|---|
| `container volume attach NAME --volume=VOLUME --path=/mount/point [--read-only]` | Adds it to the container's definition |
| `container volume detach NAME VOLUME` | Removes it from the definition; the volume and its data are untouched |

An attach to a **running** container takes effect immediately as well as being recorded; otherwise it applies on the container's next start. The command prints which. Unlike `container network attach`, which is live *and ephemeral*, the definition here is the source of truth — the live mount is it taking effect early, not instead.

A volume is never *owned* by a container: containers reference volumes by name, never the reverse, so several containers may mount the same volume and a volume outlives every one of them. `volume ls`'s own listing plus `container inspect NAME` (which echoes a container's `volumes` back by name) are the two ends of that mapping.

Attach one at container creation with `--volume=NAME:/path[:ro]`. The volume must already exist: an unknown name fails creation rather than quietly making a fresh empty one. A volume that can't be mounted — or a `:ro` one that can't be remounted read-only — fails the container's start instead of coming up without the storage, or with a guarantee that isn't real.

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
| `device ls` | Host PCI/USB/GPU devices from sysfs, with each one's `id` (pass to `run --device=`), whether it's assignable, and (for a composite USB device) every real interface it exposes (ADR-0161 Phase A) |
| `devicemap create --name=NAME --kind=exact\|vendor_model --selector=SELECTOR` | A persisted, named device binding, usable in place of a raw id in `run --device=` |
| `devicemap ls` / `devicemap rm NAME` | List (shows whether each mapping currently resolves to real hardware) / remove |
| `disks [ls]` | Real host block devices, including their partitions (task #844), flagging which one is the fixed OS disk |
| `diskrole create --disk=NAME --role=container-storage\|backup\|state-storage\|rebuildable-storage\|log-storage\|swap` | Assign a persisted role to a disk or partition (never the OS disk) |
| `diskrole ls` / `diskrole rm NAME` | List assigned roles (with whether each disk is currently present) / remove one (409 if the disk is the active state-storage placement) |
| `disks format NAME [--fs-type=ext4\|btrfs]` | Destructive: mkfs (ext4 by default, or btrfs, ADR-0104) + mount an already role-assigned, non-OS disk (409 against the active state-storage placement) |
| `disks format-status NAME` | State/mount_path/error of the most recent format job for this disk |
| `disks unmount NAME` | Real, synchronous `umount2(2)` of an already-mounted, non-OS disk (issue #34) — data untouched, only its attachment to the running system is removed; 409 against the same active-placement/container-storage-in-use checks `format` has |
| `disks grow-partition DISK PARTITION [--size-mib=N]` | Grow a partition and the filesystem in it; omit the size to take all free space immediately after it. Grow only — shrinking would need the filesystem shrunk first, and cutting the table entry before that destroys live data. Partition must be unmounted, and ext4 or unformatted |
| `disks free-space NAME` | How much room is left in this disk's table — total, and the largest single gap, which is what actually bounds one new partition. Asked of sfdisk, not computed by subtracting sizes (that misses alignment, GPT reserved areas, and gaps from an earlier delete) |
| `disks partition-table NAME` | Destructive: writes a fresh, empty GPT partition table to a non-OS whole disk with no role or partitions of its own in use |
| `disks add-partition NAME --name=PART_NAME [--size-mib=N]` | Appends one new partition to a disk's existing table; omit `--size-mib` for "rest of the disk" |
| `disks rm-partition DISK_NAME PARTITION_NAME` | Removes one partition (409 if it still has a role assigned) |
| `storage state [show]` | Which disk (if any) is the active placement for thinC's own state (ADR-0141) |
| `storage state migrate [--disk=NAME]` | Move thinC's own state to a disk already carrying the role and mounted; omit `--disk=` for the default OS-disk placement; live, no downtime |
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
| `ldap group update --name=NAME --gidnumber=N [--new-name=NEWNAME]` | Edit an existing group's gidnumber in place (task #750); `--new-name=` renames it (ADR-0147) -- if it's currently an admin group, `hostauth-config`'s own `admin_groups` follows the rename automatically |
| `ldap group ls` / `ldap group rm NAME` | List / remove |
| `ldap user add --name=NAME [--uidnumber=N] --primarygroup=N [--secondary-groups=N,N,...] [--givenname=S] [--sn=S] [--mail=S] [--loginshell=S] [--homedirectory=S] [--password=S] [--disabled] [--ssh-key=S] [--can-search]` | Create a user -- `--uidnumber=` optional, auto-allocated if omitted (task #748); `--ssh-key=` optional, rendered as glauth's own `sshkeys` LDAP attribute, queried live by a container's own `AuthorizedKeysCommand` (task #731/ADR-0144 task #838); `--can-search` grants glauth's own minimal search capability, needed for a real bind/service account (`nslcd`, a live `AuthorizedKeysCommand`, ADR-0144 task #838), off by default |
| `ldap user update --name=NAME [--new-name=NEWNAME] ...` | Edit an existing user in place -- full field replacement, same fields as `add` (task #731); `--new-name=` renames it (ADR-0147) |
| `ldap user ls` / `ldap user rm NAME` | List / remove |
| `ldap config show` | Show the current `start_uid`/`start_gid` auto-allocation floor (task #748), the client-login settings, and `effective_client_uri` — what `ldap_client` containers are actually handed right now, after derivation from registered servers and after dropping any that are drained or unhealthy (issue #84) |
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
| `pkg sync [--wait] [--refetch=NAME@VERSION]` | Fetch and merge the configured repo's recipes into this host's own catalog (additive — never overwrites an existing version). `--refetch=` lets this one sync replace exactly one already-seen version, for a recipe under active development (issue #59) — one-shot, and never inherited by the periodic background sync |
| `pkg sync-status` | The most recent (or currently running) sync's outcome |
| `pkg cache-config show` \| `set --max-bytes=N` | The local build-artifact cache's own size cap (always a real cap, no "unlimited" mode) |
| `pkg cache-status` | Current cache occupancy (max/current bytes, entry count) |
| `pkg cache-clear` | Remove every cached artifact — an explicit operator reset |
| `pkg artifact-config show` \| `set [--url=URL] [--token=TOKEN\|--clear-token]` | The configured plain-HTTP precompiled-artifact server — separate from `repo-config` above, never a git forge |
| `pkg install --name=NAME [--image=IMAGE] [--version=VERSION] [--upgrade] [--keep-on-failure]` | Start installing (or upgrading) a package; omitted version resolves to the highest available. `--keep-on-failure` (ADR-0175) preserves a failed build's own container instead of tearing it down — see [Debugging a failed build](../api/README.md#debugging-a-failed-build-keep_on_failure-adr-0175-issue-35) |
| `pkg ls` | List every known package (installed or in-flight). State reads `failed:fetch` / `failed:build` / `failed:recipe` / `failed:install` (issue #101), so a source that could not be reached is distinguishable from a build that genuinely broke |
| `pkg rm NAME[@IMAGE]` | Uninstall |
| `pkg update-all` | Start an upgrade for the first installed package whose recipe has drifted; call again to drain the backlog |
| `pkg hostbuild NAME --build-image=IMAGE [--version=VERSION] [--wait] [--deploy] [--upgrade] [--keep-on-failure]` | Build a standalone host artifact (kernel, or thinC's own control plane) instead of merging into an image — see [`docs/guides/writing-recipes.md#the-hostbuild-variant`](writing-recipes.md#the-hostbuild-variant). `--upgrade` re-runs a build already `state: "installed"` if the recipe's own version has moved on (otherwise a bare 409). `--keep-on-failure` (ADR-0175) preserves a failed build container for real debugging |
| `pkg resume --name=NAME [--image=IMAGE] [--version=VERSION] [--keep-on-failure]` | Continue a `--keep-on-failure`-preserved build container in place (ADR-0177) — its already-extracted source tree is kept, only the recipe (optionally a newly-fixed version) and install destination are refreshed, skipping a full fetch+extract+build restart — see [Continuing a kept build in place](../api/README.md#continuing-a-kept-build-in-place-post-pkgresume-adr-0177-issue-46) |
| `pkg build-log` | Live-tail the currently in-flight install/hostbuild's own stdout/stderr (task #676, ADR-0101) — a one-way stream, not an interactive session; prints each chunk as it arrives and exits once the build finishes. 404 if nothing is currently building |
| `pkg build-logs [--last \| --file=NAME]` | The **complete** persisted output of recent builds, kept on disk as each build streams (issue #57) — as opposed to `pkg build-log`'s live-only stream and the log store's ~4KB tail. `--last` prints the most recent build's whole log; with no arguments, lists what is kept |
| `pkg policy ls` | Per-package rolling policy — which version an omitted version resolves to (issue #64). Only packages with a policy set are listed; everything else is on `highest` |
| `pkg policy set NAME --policy=highest\|newest\|pinned [--version=V]` | `highest` is the default; `newest` picks the most recently published recipe; `pinned` holds an explicit version that `update-all`/`follow_rolling` cannot bump |
| `pkg policy clear NAME` | Back to the default |

See [`docs/guides/writing-recipes.md`](writing-recipes.md) for the recipe format itself, and [`docs/guides/kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) / [`docs/guides/building-thinc.md`](building-thinc.md) for the two real operator runbooks built on `pkg hostbuild`.
