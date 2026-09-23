# `cixctl` CLI reference

`cixctl` is a pure REST client of [`docs/api/openapi.yaml`](../api/openapi.yaml): it holds no logic the API does not expose (the API-First Mandate, [ADR-0005](../adr/0005-api-first-mandate.md)). Most commands are one HTTP call. The exceptions are read-modify-write setters (`hostauth-config set`, `kmod-config set`), multi-step commands (`image materialize`, `pkg artifact-export`, `pkg hostbuild --deploy`), and the client-only `pager`, which makes no call at all. This page lists the command surface. For what each call does, its fields and its errors, see [`docs/api/README.md`](../api/README.md) and `openapi.yaml`.

The usage text printed by `cixctl <command>` with missing or wrong arguments is the authoritative syntax for any command below.

## Global flags and invocation

```
cixctl [--host=ADDR] [--port=N] [--json] <command> [args...]
```

- `--host=`/`--port=`: default `127.0.0.1:80`.
- `--json`: print the raw API response instead of the formatted text. It is a global flag and goes before the command (`cixctl --json show running-config`). `console`, `container files get` and `container files put` have no JSON form: a console is an interactive session, `get` returns raw file bytes, and `put` returns `204 No Content`.
- Running `cixctl` with no command from a terminal (`isatty(stdin)`) starts an **interactive shell**: one line per command, over one connection. With stdin piped or redirected it prints the usage error instead, so scripts are unaffected. The prompt is the connected daemon's site identity (`GET /system/site`: `instance.site.domain`, or `instance.domain` when no site tier is set, e.g. `lab.uk.home.arpa> `). The prompt ends in `>`, or `#` while logged in (`GET /whoami`, ADR-0164), refreshed after every `login`/`logout`. `exit`, `quit` and `help` work inside the shell only.
- **Exit codes**: `0` success, `1` the API call failed (a non-2xx response, or the daemon could not be reached), `2` a usage error (bad flags, unknown subcommand), detected before any network call.
- **Authentication ([ADR-0144](../adr/0144-host-authentication-and-real-ldap.md))**: once write-gating is active (see [Host authentication](../api/README.md#host-authentication-adr-0144)), every mutating command needs a session. Run `login` once; later invocations on the same machine authenticate with the saved token until `logout` or the session's idle timeout. Read-only commands never need one.

## Completion

Completion reaches the end of a command, not just the first word:

```
cixctl dns forwarders <TAB>      -> set  show  --forwarder=
cixctl container mig<TAB>        -> migrate-storage  migrate-storage-status
cixctl update --<TAB>            -> --image=  --image-sha256=  --image-url=  --kernel=
```

It works in the built-in interactive shell, and in your own shell once you install one of the scripts in [`cli/completion/`](../../cli/completion) (`cixctl.bash` or `cixctl.zsh`). Source it from your shell rc, or put the bash one in `/usr/share/bash-completion/completions/cixctl`.

Both get their candidates from `cixctl __complete`, a hidden helper that reads the CLI's own command tree:

- **Tab never blocks.** `__complete` makes no HTTP call, so completion works when the daemon is slow, unreachable or not running.
- **The completion scripts define no commands themselves**, so they stay in step with the CLI.

Flag *values* are not completed. The useful ones (container names, image names) can only come from the daemon, and a blocking request on every keypress is worse than none.

The command tree is data in `cli/src/cmdtree.h`. `test_clitree` re-derives the command surface from `cli/src/main.c` and fails the build when the two disagree.

## The pager

Long output (`container ls`, `logs`, `pkg ls`) goes through a pager. The setting is remembered:

```
cixctl pager status      # on | off
cixctl pager off
cixctl pager on
```

It engages only when all of these hold:

- **stdout is a terminal.** Piping to `grep`, `jq` or a file is never paged.
- **`--json` is not set.** That output is meant for another program.
- **It is turned on.** The setting is stored in `~/.cixctl_pager`.

`$PAGER` is respected. The fallback is `less -FRX`: `-F` exits at once when the output fits one screen, `-R` passes colour through, and `-X` leaves the output on screen after the pager exits. An explicitly empty `PAGER` means no pager.

`pager` configures this client only, so it has no endpoint.

## Seeding a fresh box

```
cixctl --host=NEWBOX image recipe add --name=toolchain --file=recipes/image/toolchain@1.0.0.sh
cixctl --host=NEWBOX image materialize toolchain
```

`--file=` is a local path. Image recipes live in the [cix-recipes](https://git.home.arpa/itdlabs/cix-recipes) repository as flat `<name>@<version>.sh` files ([ADR-0308](../adr/0308-recipes-are-their-own-repository-flat.md)); the path above is relative to a checkout of it. `image materialize` creates the image if it does not exist, declares its manifest from the image recipe of the same name, and installs every package in it (#141). An install that finds an approved prebuilt artifact verifies it and stages it without composing a build sandbox, so a host with no compiler can materialize a build image ([ADR-0225](../adr/0225-build-images-are-composed-from-packages.md)). A package that arrives as another's build dependency is reported as `already present`, not as an error.

## Configuration as a document

```
cixctl show running-config          # the whole configuration, Cisco-style
cixctl --json show running-config   # the document itself
```

One ordered, redacted view of every configurable subsystem ([ADR-0206](../adr/0206-configuration-as-a-first-class-document.md)): identity, storage, networks, DNS, DHCP, PKI, LDAP, packages, images, containers. Secrets are never printed. Tokens and passwords render as set/not-set, and PKI private keys never appear. It reads live state, and its section list comes from the API schema.

```
cixctl config diff  --file=c.json [--section=NAME ...]   # what it would change
cixctl config apply --file=c.json [--section=NAME ...]   # change it
```

Both take the document `cixctl --json show running-config` writes, so the workflow is fetch, edit, send back ([ADR-0292](../adr/0292-configuration-applies-section-by-section.md)). `diff` changes nothing.

**Send only the sections you changed.** The document carries running state alongside configuration (a container's `pid`, a volume's creation time), and one unappliable section refuses the whole request. `--section=NAME` sends only the named sections.

```
$ cixctl --json show running-config > c.json
$ vi c.json
$ cixctl config diff --file=c.json --section=resolver
resolver             changed    appliable
  replace  nameservers[1]               192.168.15.100 -> 9.9.9.9
1 supplied, 1 changed, 1 appliable, 0 blocked
$ cixctl config apply --file=c.json --section=resolver
```

Eleven sections can be applied, the ones a single setter owns: site, daemon, resolver, time, zswap, swap, backup, dns_forwarders, ldap, package_repo, package_artifacts. The rest show in a diff and refuse an apply; use their own commands (`container`, `network`, `dns`, `pkg`, …). A section is replaced whole, so send back every field it renders. Secrets cannot be set this way: the document never carries them, and applying a section leaves its secret untouched.

## Session and identity

| Command | |
|---|---|
| `login [--username=NAME] [--password=PASS]` | Authenticate. Prompts for whichever is not given, with echo off for the password. Saves the session token to `~/.cixctl_token` (mode `0600`). Succeeds with nothing enforced on a daemon where write-gating was never activated |
| `logout` | End the current session, if any, and remove the saved token. Always succeeds |
| `site show` | This install's `instance_name`/`site_name`/`domain_suffix` |
| `site set [--instance-name=NAME] [--site-name=NAME] [--domain-suffix=NAME]` | Set them |
| `hostauth-config show` | `admin_groups`, `idle_timeout_seconds` and the LDAP backend settings |
| `hostauth-config set [--admin-group=NAME ...] [--idle-timeout-seconds=N] [--ldap-enable \| --ldap-disable] [--ldap-server=HOST ...] [--ldap-port=N] [--ldap-tls \| --no-ldap-tls] [--ldap-base-dn=NAME]` | Change only the flags given (the command fetches the current config first). Write-gating activates as soon as a real user is a member of one of `admin_groups`. `--ldap-tls` (#416) is the daemon's own bind over LDAPS; `ldap config set --client-tls` is the separate switch for LDAP clients in containers. `--ldap-enable` is refused (409) when no root CA is bootstrapped |
| `hostauth-sessions ls` | Every active session (username, expires-in), never a token ([ADR-0152](../adr/0152-hostauth-session-introspection.md)) |
| `hostauth-sessions revoke USERNAME` | Revoke every session that user holds |

## Boot, updates and the control plane

| Command | |
|---|---|
| `health` | Liveness check |
| `boot` | Build version and time, A/B slot, kernel version |
| `shutdown` | Stop `cixd`; powers the host off too when `cixd` is PID 1 (an installed system) |
| `reboot` | Stop `cixd`; restarts the host too when `cixd` is PID 1 |
| `update [--image=PATH \| --image-url=URL --image-sha256=HEX] [--kernel=PATH]` | Write a control-plane squashfs and/or a kernel to the inactive A/B slot and stage a loader entry for it. Does not reboot. `--image=`/`--kernel=` are paths already on the box; on an installed host, which has no shell, use `--image-url=` with its required `--image-sha256=` |
| `boot-next [a\|b\|clear]` | Boot the given slot once on the next boot, then return to normal selection. No argument reports what is armed. This is the rollback tool: pinning the loader default instead is sticky and breaks the next update |
| `boot-manager [--path=FILE \| --url=URL --sha256=HEX]` | Report or replace `\EFI\BOOT\BOOTX64.EFI`, the boot manager both slots share (#469). Does not reboot |
| `assembly status` | What control-plane assembly is doing, and where the assembled root is (`image_path`, ready for `update --image=`) with its size, mtime and completeness |
| `assembly start` | Assemble a fresh control-plane root now (#308). Returns at once; poll `assembly status` |
| `esp show` | The ESP's boot configuration ([ADR-0202](../adr/0202-the-esp-is-reachable-over-rest.md)). Leads with **`will boot:`**, the entry the boot manager would choose, then every entry marked `[default]` or `[not matched]` |
| `esp set [--default=PATTERN] [--timeout=N]` | Set `loader.conf`'s default **glob pattern** (not an entry name, #128) and/or the menu timeout. A pattern matching no entry is refused; other directives are kept |
| `esp rm-entry NAME` | Remove one stale loader entry. The last entry for the running slot cannot be removed |
| `boot-console show` | The boot console parameters, and the options line each loader entry carries (#24) |
| `boot-console set [--console=NAME ...] [--extra="..."]` | Set them. `--console` is repeatable and ordered (`--console=tty0 --console=ttyS0,115200n8`). Rewrites the loader entries; takes effect at the next boot. Everything from `root=` onward is left alone |
| `kernel-policy show` | Which kernel line this box tracks, where that channel is, and whether the running kernel is behind (#65) |
| `kernel-policy set --channel=pinned\|longterm\|stable\|mainline` | Set the channel (kernel.org's own names). `pinned`, the default, proposes no version; the pin stays in the kernel recipe |
| `kernel-policy refresh` | Re-read kernel.org's `releases.json`. Asynchronous |
| `kmod-build [--version=VERSION] [--symbol=CONFIG_FOO ...] [--upgrade] [--wait] [--keep-on-failure]` | A `pkg hostbuild kernel` with extra `=m` module symbols merged into the kernel config (ADR-0159). Takes effect after a reboot onto the new kernel |
| `iso build [--disk=DEV] [--ip=A.B.C.D] [--prefix=N] [--gateway=A.B.C.D] [--interface=IFNAME] [--wait]` | Assemble an installer ISO on the host from the latest `cix`, `kernel` and `isotools` hostbuild artifacts ([ADR-0064](../adr/0064-rest-driven-iso-assembly.md)). Every flag is optional; with none, the installer asks for everything at install time |
| `iso status` | State, `iso_path` and error of the most recent ISO build |
| `iso publish [--wait]` | Put the ISO and its signature in the artifact cache |
| `signing-keys [show]` | Whether this host holds the Secure Boot signing key pair `iso build` needs, and the certificate's identity |
| `signing-keys set --key=PATH --cert=PATH` | Install the pair ([ADR-0212](../adr/0212-signing-keys-over-rest.md)). The private key is never returned by any endpoint |
| `signing-keys clear` | Remove it from this host |
| `release-key [show]` | Whether this host holds the Ed25519 release-signing key, and its public half |
| `release-key set --key=PATH` | Install it ([ADR-0220](../adr/0220-a-separate-release-signing-key.md)) |
| `release-key clear` | Remove it from this host |
| `factory-reset --confirm=<instance name>` | Return the box to its just-installed state and reboot. Destroys every container, image, network, registration, package state, the log store, and **every volume and all data in it**. Keeps the installed OS; forgets disk roles without reformatting the disks |

Runbooks: [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) and [`staying-updated.md`](staying-updated.md).

## Daemon settings

| Command | |
|---|---|
| `daemon-config show` | `cixd`'s listen ports and HTTP/HTTPS exposure |
| `daemon-config set [--port=N] [--https-port=N] [--enable-http] [--disable-http] [--enable-https] [--disable-https]` | Live, no restart; only the flags given change |
| `management-address show` | The one off-box address `cixd` answers on, whether it is bound, and the network derived from it ([ADR-0287](../adr/0287-the-management-address-is-the-single-truth.md)) |
| `management-address set A.B.C.D` | Set the off-box address; the network follows from it. Live and persistent. Point later commands at the new address with `--host=` |
| `management-address reset` | Unbind the off-box address. `127.0.0.1` stays bound |
| `tls-throttle show` | Per-source throttling of repeated failed HTTPS handshakes (ADR-0134) |
| `tls-throttle set [--enabled \| --disabled] [--threshold=N] [--window-seconds=N] [--block-seconds=N] [--log-interval-seconds=N]` | Change only the fields given. `threshold` failures within `window-seconds` block a source on both listeners for `block-seconds`; loopback is never throttled. `log-interval-seconds` limits how often one source's failures are logged; `0` logs every one |
| `tls-throttle status` | Every source currently tracked (in memory, not persisted) |
| `control-plane-reservation show` | CPU and memory held back for the daemon, the host totals, and the ceiling applied to the `cix-workload` cgroup that every container and build runs under (#86) |
| `control-plane-reservation set [--enabled \| --disabled] [--cpu-percent=N] [--memory-bytes=N]` | Change it; applied to the live cgroup immediately. `cpu_percent` is 1-50 |
| `rolling-config show` | The rolling-restart jitter window (`jitter_window_seconds`) used by `container run --follow-rolling` (ADR-0124) |
| `rolling-config set --jitter-window-seconds=N` | 0-3600; `0` restarts immediately on every rolling reconcile |
| `pkg-build-config show` | How many package jobs may run at once (`max_concurrent_jobs`, default 10) and the memory/CPU ceiling of each build sandbox (ADR-0157) |
| `pkg-build-config set [--max-concurrent-jobs=N] [--memory-max=BYTES] [--cpu-max="QUOTA PERIOD"]` | Change only the flags given. `max-concurrent-jobs` is 1-10; lowering it affects only future jobs. `--memory-max=0` or `--cpu-max=""` means unlimited; `cpu-max` is raw cgroup v2 `cpu.max` syntax, as for `container run --cpu-max=` |

## Backup and scheduling

| Command | |
|---|---|
| `backup [--output=PATH]` | The platform configuration bundle (container definitions, networks, DNS records, package state and recipes). No workload data, no image content, no keys. Prints it by default; `--output=` saves it byte-for-byte for `restore` |
| `restore --input=PATH` | Write a saved bundle back. Does not reboot or hot-reload |
| `backup-config show` | Which disk automatic backup snapshots go to, and whether they are enabled (ADR-0141). When they run is a schedule (below) |
| `backup-config set [--disk=NAME \| --clear-disk] [--enable \| --disable]` | Change only the fields given. The disk must carry the `backup` role |
| `backup-config status` | Outcome of the most recent snapshot attempt |
| `backup-config snapshot-now` | Write the `backup` bundle to the configured disk now |
| `schedule ls` | Everything this host does on a clock ([ADR-0257](../adr/0257-one-scheduler-structured-schedules.md)) |
| `schedule actions` | The actions a schedule may run |
| `schedule show NAME` / `schedule rm NAME` | Inspect / remove one |
| `schedule run NAME` | Run it now, even if disabled |
| `schedule set NAME --action=A <when> [--window-minutes=N] [--catch-up] [--disabled]` | Create or replace one. `<when>` is exactly one of `--every-seconds=N`, `--every-minutes=N`, `--every-hours=N`, `--every-days=N`, `--daily-at=HH:MM`, or `--weekly-on=sun\|mon\|… --weekly-at=HH:MM`. `--catch-up` runs once at startup if the time passed while the daemon was down; `--disabled` stores it without running it |

## Observing the host

| Command | |
|---|---|
| `host-stats` | Host-wide uptime, load, CPU, memory, disk and network, including pressure-stall (PSI) figures (ADR-0073, ADR-0074). `uptime.host` is seconds since boot, `uptime.daemon` seconds since `cixd` started |
| `stalls` | Times the control plane stopped going round its loop, with the kernel function it was sleeping in and the request it was serving (#100). Recorded by a separate watchdog process |
| `kmsg [--tail=N]` | The kernel ring buffer (`/dev/kmsg`) over REST (#77) |
| `logs [--source=kernel\|cixd\|audit\|container] [--level=...] [--container=NAME] [--regex=PATTERN] [--tail=N] [--since=UNIXTS]` | The consolidated log: kernel messages, `cixd` diagnostics, the per-request audit trail and every container's stdout/stderr (ADR-0070, ADR-0126). `--regex=` is a case-insensitive POSIX extended regex on the message text |
| `logs config [--max-bytes=N] [--min-level=LEVEL]` | Show or set the log's size cap and minimum severity (`emerg`/`alert`/`crit`/`err`\|`error`/`warning`\|`warn`/`notice`/`info`/`debug`, default `debug`) |
| `process ls` | Every process on the box (a `/proc` scan), each matched to a container through its host parent chain, if any (ADR-0131) |
| `process kill PID` | Immediate `SIGKILL`; refuses pid 1 and the daemon's own pid |
| `routes [ls]` | The kernel IPv4 routing table (ADR-0066) |
| `routes add --dest=A.B.C.D --prefix=N [--gateway=A.B.C.D]` / `routes add --default --gateway=A.B.C.D` | Add a route |
| `routes rm --dest=A.B.C.D --prefix=N` / `routes rm --default` | Remove one |
| `ping HOST` | ICMP echo to an IPv4 address (ADR-0075); waits about 2 s and exits nonzero if unreachable |
| `server-health [ls]` | Health of every registered LDAP/DNS/NTP/syslog server (#81): state, whether it is in service, how it was probed (`tcp:PORT` is a service check; `process` only means the container runs), and the last error |
| `server-health drain KIND CONTAINER` / `server-health undrain KIND CONTAINER` | Take one out of or back into service. `KIND` is `ldap`, `dns`, `ntp` or `syslog`. Persists across a daemon restart. A drained or unhealthy server is left out of the client configuration Cix generates |
| `software` | What is declared (has a recipe) against what is installed. Anything installed with **no recipe** cannot be rebuilt from source control |

## Tuning the host

| Command | |
|---|---|
| `sysctl [show]` | Every host-level sysctl persisted for reapply at boot (ADR-0160) |
| `sysctl get KEY` | The live value of one key, persisted or not |
| `sysctl set KEY --value=V [--value=V ...] [--no-persist]` | Write `/proc/sys` live. No allowlist; host-auth write-gating is the only access control. Repeat `--value=` for a tuple (`net.ipv4.ip_local_port_range --value=32768 --value=60999`). Persisted for boot unless `--no-persist` |
| `sysctl rm KEY` | Stop reapplying it at boot; the live value is untouched |
| `kmod [ls]` | Loaded kernel modules (`/proc/modules`, ADR-0159) |
| `kmod show NAME` | `modinfo`: description, parameters, dependencies, in-tree or not |
| `kmod load NAME [--option=KEY=VALUE ...]` | `modprobe`; with no `--option=`, the module's persisted `kmod-config` options apply |
| `kmod unload NAME` | `modprobe -r` |
| `kmod-config [ls]` | Modules with persisted default options and/or autoload |
| `kmod-config set NAME [--option=KEY=VALUE ...] [--autoload \| --no-autoload]` | Change only the fields given |
| `kmod-config rm NAME` | Clear both fields; the module's loaded state is untouched |
| `ksm show` | Kernel samepage merging (#50) |
| `ksm set [--enable \| --disable] [--pages-to-scan=N] [--sleep-millisecs=N]` | Configure the scanner. Nothing merges until a container opts in with `container run --ksm` |
| `zswap show` | The compressed swap cache (#51): configured intent, what the kernel reports, and the available compressors. A disagreement between the first two means the setting did not take |
| `zswap set [--enable \| --disable] [--max-pool-percent=N] [--compressor=NAME]` | Configure it |
| `swap [status]` | Whether the host swap file is enabled (ADR-0069) and which disk it is on |
| `swap enable --size-mb=N [--disk=NAME]` | Create and activate a swap file; `--disk=` puts it on a disk carrying the `swap` role (#28) |
| `swap disable` | Deactivate and remove it |
| `time [show]` | The host's date and time (ADR-0110) |
| `time set --unixtime=N` | Set the host clock (`clock_settime()`) |
| `ntp config [show]` | Upstream NTP servers used to set the host clock |
| `ntp config set [--server=A.B.C.D ...]` | Replace them (up to 3); no flags clears them |
| `ntp status` | Outcome, source and time of the most recent sync |
| `ntp sync` | Sync now |
| `ntp server register --container=NAME` / `ntp server ls` / `ntp server unregister CONTAINER` | Register a running container as an internal NTP source |
| `resolv [show]` | The host's outbound DNS resolvers (ADR-0076) |
| `resolv set [--nameserver=A.B.C.D ...]` | Replace them (up to 3), live; no flags clears them |
| `syslog target register --container=NAME` / `syslog target ls` / `syslog target unregister CONTAINER` | Forward every container log line to a running container as an RFC 3164 UDP datagram, in addition to the log store (ADR-0127) |

## Storage

See [`storage.md`](storage.md) for how disks, roles and filesystems fit together.

| Command | |
|---|---|
| `storage [ls]` | Block devices and their partitions, marking the OS disk |
| `storage free-space NAME` | Room left in the disk's partition table: the total, and the largest single gap, which bounds one new partition. Read by `sfdisk` from the disk itself |
| `storage partition-table NAME` | Destructive: write an empty GPT to a non-OS whole disk with no role or partition in use |
| `storage add-partition NAME --name=PART_NAME [--size-mib=N]` | Append one partition; omit `--size-mib` for the rest of the disk |
| `storage grow-partition DISK_NAME PARTITION_NAME [--size-mib=N]` | Grow a partition and its filesystem; omit the size to take all free space after it. Grow only. A mounted btrfs grows online; anything else must be unmounted first |
| `storage rm-partition DISK_NAME PARTITION_NAME` | Remove one partition; refused (409) while it has a role |
| `storage format NAME [--fs-type=ext4\|btrfs]` | Destructive: `mkfs` and mount a role-assigned, non-OS disk (btrfs by default). Refused (409) against an active placement |
| `storage format-status NAME` | State, mount path and error of the most recent format |
| `storage unmount NAME` | `umount2(2)` a mounted non-OS disk (#34); data untouched. Refused (409) if it is an active placement or holds a running container's storage |
| `storage-role create --disk=NAME --role=container-storage\|backup\|rebuildable-storage\|log-storage\|swap` | Assign a role to a disk or partition (never the OS disk) |
| `storage-role ls` / `storage-role rm NAME` | List roles (with whether each disk is present) / remove one; refused (409) while the disk is an active placement |
| `storage logs [show]` / `storage logs migrate [--disk=NAME]` / `storage logs migrate-status` | Where the log store lives; move it to a disk carrying `log-storage`, or back to the OS disk with no `--disk=` (ADR-0141) |
| `storage rebuildable [show]` / `storage rebuildable migrate [--disk=NAME]` / `storage rebuildable migrate-status` | The same for images, packages and artifacts (`rebuildable-storage`) |

## Containers

Every container operation is a subcommand of `container`. `console NAME` is also accepted as a shorthand for `container console NAME`.

| Command | |
|---|---|
| `container ls [--all]` | List containers (running, stopped and exited). `--all` includes the platform's own build containers (#426) |
| `container drift` | Containers that should be running and are not. **Exits 1** if any are listed, so a script can gate on it. A container an operator stopped is listed separately and does not set the exit status; a stopped `unless-stopped` container is not listed. Exit 2 means the daemon could not be asked (#268) |
| `container run --name=NAME --image=IMAGE --service=NAME=/path [args] [flags...]` | Create and start a container; full flag set below |
| `container inspect NAME` | Show one container |
| `container edit NAME --json='{...}'` | Edit the stored definition (#11). Fields given replace those fields; `null` removes one. Applies at the next start. `name`, `restart`, `restart_delay_seconds`, `depends_on`, `follow_rolling` and `follow_rolling_jitter_seconds` cannot be edited (`handle_container_patch()`); recreate the container to change them |
| `container start NAME` | Start a stopped or exited container |
| `container stop NAME` | Stop it and keep its definition; it shows as `stopped` ([ADR-0181](../adr/0181-persist-all-containers-restart-decoupled-from-existence.md)) |
| `container pause NAME` / `container unpause NAME` | Freeze / thaw through the cgroup v2 freezer |
| `container rm NAME` | Stop it if running, and delete it and its definition |
| `container service start\|stop\|restart NAME SERVICE` | Act on one declared service. A stop holds until the next container start |
| `container stats NAME` | Host-side CPU, memory, disk and network usage, including pressure-stall figures (ADR-0074) |
| `container console NAME [--console=NAME]` | Attach to a console the container **declares** (#248). `--console=` picks which; the default is the first declared. A container that declares none says so. There is no free-text command ([ADR-0261](../adr/0261-reaching-inside-a-container.md)) |
| `container files get NAME --path=/some/path [--output=PATH]` | Read one file's bytes out of the container; stdout if `--output=` is omitted |
| `container files put NAME --path=/some/path --file=LOCAL_PATH [--mode=0644]` | Write one file into an existing container, live and not persisted (ADR-0153) |
| `container migrate-storage NAME [--disk=NAME]` | Move the container's overlay storage to a disk carrying `container-storage`, or back to the OS disk with no `--disk=` (ADR-0142). Briefly restarts it |
| `container migrate-storage-status NAME` | State, disk and error of the most recent migration |
| `container network attach NAME --network=NETWORK [--ip=A.B.C.D] [--ifname=NAME]` | Attach a network to a running container, live and not persisted (ADR-0156). `--ifname=` names the interface ([ADR-0259](../adr/0259-an-interface-may-be-named.md)) |
| `container network detach NAME NETWORK` | Detach a live-attached network; a network from creation is refused (409) |
| `container device attach NAME ID` | Grant one more device to a running container; `ID` is a device id or devicemap name (ADR-0161) |
| `container device detach NAME ID` | Remove a live-attached device; a device from creation is refused (409) |
| `container volume attach NAME --volume=VOLUME --path=/mount/point [--read-only]` | Add a volume to the definition; see [Volumes](#volumes) |
| `container volume detach NAME VOLUME` | Remove it from the definition; the volume's data is untouched |

`container run`'s full flag set:

```
container run --name=NAME --image=IMAGE
    --service=NAME=/path [args] ...
    [--oneshot=NAME=/path [args] ...] [--after=NAME:DEP[,DEP] ...]
    [--ready=NAME:tcp:PORT|socket:PATH|command:/path [args] ...]
    [--on-exit=NAME:restart|stop|fail-container ...]
    [--console=NAME=/path [args] ...]
    [--memory-max=BYTES] [--memory-swap-max=BYTES] [--pids-max=N]
    [--cpu-max="QUOTA PERIOD"] [--cpuset=0-1,3]
    [--disk-quota=BYTES] [--disk=NAME] [--volume=NAME:/path[:ro] ...]
    [--network=NAME[:IP] ...] [--ip-forward] [--route=DEST/PREFIX:VIA ...]
    [--interface=IFNAME ...] [--dns-register] [--dns-server=A.B.C.D ...]
    [--device=ID ...] [--optional-device=ID ...] [--cap-add=CAP_NAME ...]
    [--userns] [--ksm] [--capture-output]
    [--ldap-client] [--ldap-allow-group=NAME ...]
    [--ldap-provision] [--ldap-user=NAME] [--ldap-group=NAME] [--ldap-uid=N] [--ldap-secret-dir=PATH]
    [--pki-issue | --pki-cert=NAME] [--pki-cert-dir=PATH] [--pki-days=N]
    [--restart=always|on-failure|unless-stopped] [--restart-delay=N]
    [--follow-rolling] [--follow-rolling-jitter-seconds=N]
    [--depends-on=NAME ...]
    [--file=CONTAINER_PATH=LOCAL_PATH[:MODE] ...] [--file-owner=CONTAINER_PATH:UID:GID ...]
    [--sysctl=KEY=VALUE ...] [--env=KEY=VALUE ...]
```

A container runs its declared services, started in `--after` order by `cix-init`, which is PID 1 in every container ([ADR-0260](../adr/0260-a-container-declares-services-not-a-command.md)). A container with one daemon declares one `--service=`. `--memory-swap-max=0` forbids swapping; omitting it leaves swap unlimited. Each flag maps to a `ContainerCreateRequest` field; see [Creating a container](../api/README.md#creating-a-container) for what each means and how it is validated. `--optional-device=ID` differs from `--device=ID` in one way: an unresolvable reference still creates the container, without that grant, and is matched against hardware as it appears (ADR-0161; see [Device hotplug](administration.md#device-hotplug)).

### Declaring a container's services and consoles

`--service=`, `--oneshot=` and `--console=` each take `NAME=/absolute/path [args...]` as **one** shell argument, so quote it. The value is split on spaces with no further quoting, so an argument that itself contains a space cannot be written this way; use a deployment (below), which declares these as JSON arrays.

```
cixctl container run --name=jump --image=jumpbox \
  --console='shell=/usr/bin/bash -l' \
  --console='logs=/usr/bin/tail -F /var/log/messages' \
  --oneshot='hostkeys=/usr/bin/ssh-keygen -A' \
  --service='nslcd=/usr/sbin/nslcd -d' --ready=nslcd:socket:/run/nslcd/socket \
  --service='sshd=/usr/sbin/sshd -D -e' --after=sshd:hostkeys,nslcd
```

`--console=` is repeatable up to four times (#248). The first declared is what `container console NAME` attaches to with no `--console=`. `argv[0]` must be an absolute path: it is `execve`'d directly, and a bare name is refused at creation.

Declaring no console is valid: an image holding one static binary and no shell has nothing for a console to run, and both the CLI and the dashboard say so.

### Full-screen programs in a console

`container console` gives the remote program a real terminal. It sends your terminal's size and `$TERM` when it attaches, and a new size when you resize the window ([ADR-0242](../adr/0242-a-console-is-a-sized-terminal-of-a-declared-type.md)), so `htop`, `btop` and `vim` fill the window.

```
cixctl container console jump --console=shell
```

Two requirements:

- The container must carry the terminfo entry your `$TERM` names. The `ncurses` package installs the full database (2903 entries, including `xterm` and `xterm-256color`), and anything linking `libncursesw` depends on it.
- Your own stdio must be a terminal. When it is piped, the session uses `xterm-256color` at 80×24.

The web dashboard's console is a real VT as well ([ADR-0243](../adr/0243-the-dashboard-terminal-is-a-real-vt.md)).

## Deployments

A deployment is a stored container definition: what to run and where. Deployment recipes live in the [cix-recipes](https://git.home.arpa/itdlabs/cix-recipes) repository as `recipes/deployment/<name>@<version>.json` ([ADR-0308](../adr/0308-recipes-are-their-own-repository-flat.md)) and arrive through `pkg sync`, or are published directly:

| Command | |
|---|---|
| `deployment add --name=NAME --file=PATH` | Publish one ([ADR-0151](../adr/0151-container-recipes.md)). The content is a full `POST /containers` body whose `"name"` matches `NAME` |
| `deployment show NAME` | Its raw, unsubstituted content |
| `deployment ls` / `deployment rm NAME` | List / remove. Removing one does not affect containers already created from it |
| `deployment apply NAME [--secret=KEY=VALUE ...]` | Render it, substituting `{{SECRET:KEY}}` tokens and `{{LDAP:*}}` tokens from `ldap config`, and create the container through the same path as `POST /containers` |

## Volumes

A volume's lifetime is independent of any container: deleting a container never removes its volumes. See [ADR-0183](../adr/0183-persistent-volumes.md) for the reasoning and [Persistent volumes](../api/README.md#persistent-volumes-issue-88-adr-0183) for the contract.

| Command | |
|---|---|
| `volume create --name=NAME [--disk=DISK] [--owner-uid=N --owner-gid=N]` | Create one. Without an owner it belongs to root, which a non-root workload cannot write to (#102) |
| `volume owner NAME --uid=N --gid=N [--recursive]` | Hand it to an account. `--recursive` also rewrites ownership of what is already inside |
| `volume owner NAME --root` | Hand it back to root |
| `volume ls` / `volume show NAME` | List / inspect one (disk, host path, creation time) |
| `volume usage NAME` | Space used, measured now |
| `volume quota NAME BYTES` | A kernel-enforced size limit; `0` removes it |
| `volume backups NAME [--enable \| --disable] [--retain=N] [--while-running=refuse\|pause\|allow]` | Show or set the backup policy (opt-in). `--while-running` decides what happens when a container uses it: `refuse` skips, `pause` freezes the users for the copy, `allow` copies live (crash-consistent) |
| `volume backup NAME` | Take one snapshot now |
| `volume restore NAME SNAPSHOT` | **Replace** the volume's contents with that snapshot |
| `volume migrate NAME [--disk=DISK]` | Move its data to another disk or partition, or back to the OS disk with no `--disk=`. Refused while a container using it runs |
| `volume rm NAME` | **Delete its data** permanently. Refused while any container definition references it (the error names which) |

Attach one at creation with `container run --volume=NAME:/path[:ro]`, or later with `container volume attach`. The volume must exist; an unknown name fails creation. A volume that cannot be mounted, or a `:ro` one that cannot be remounted read-only, fails the container's start. An attach to a running container takes effect immediately as well as being recorded; otherwise it applies at the next start, and the command says which. Containers reference volumes by name, so several can share one; `container inspect NAME` lists a container's volumes.

## Networks

| Command | |
|---|---|
| `network create --name=NAME --subnet=A.B.C.D --prefix=N [--address=A.B.C.D] [--alloc-start=IP --alloc-end=IP]` | Create a network. Without `--address=` it is pure L2 with no host address. `--alloc-start`/`--alloc-end` bound the automatic IP range (#70) |
| `network set-pool NAME [--alloc-start=IP] [--alloc-end=IP]` | Change the automatic IP range; `none` clears a bound |
| `network ls` / `network rm NAME` | List / remove |
| `network ports NAME` | What is plugged into the network's bridge, per port, with counters (#26). A port nothing accounts for prints as `unattributed` |
| `network attach-interface NAME --interface=IFNAME [--vlan=N]` | Attach a host interface to the network's bridge; `--vlan=` attaches an 802.1q sub-interface instead |
| `network detach-interface NAME --interface=IFNAME` | Detach it |
| `network flap-interface IFNAME` | Take a host NIC down and straight back up to recover a stuck link. Run it from the console: a reply over the flapped NIC may not arrive |
| `dhcp show` | DHCP-configured networks and static reservations ([ADR-0197](../adr/0197-dhcp-served-by-the-dns-server.md)) |
| `dhcp leases` | Current leases, read from the serving container's lease file |
| `dhcp server ls` / `dhcp server add CONTAINER` / `dhcp server rm CONTAINER` | Registered DHCP servers. `rm` also drops the server from every range naming it, and disables a range left with none |
| `dhcp enable --network=NAME --range=START-END --server=CONTAINER [--server=CONTAINER ...] [--lease-seconds=N] [--router=IP]` | Serve DHCP on a network. A range with several servers is split into disjoint slices. **A range change restarts the servers serving it** |
| `dhcp disable --network=NAME` | Stop serving, keeping the configuration |
| `dhcp remove --network=NAME` | Delete the network's DHCP configuration |
| `dhcp static add --mac=M --ip=IP [--hostname=NAME]` / `dhcp static rm MAC` | Add / remove a reservation, live |

See [`networking.md`](networking.md).

## Images

| Command | |
|---|---|
| `image create --name=NAME` | An empty image with the C runtime seeded, ready for `pkg install --image=NAME` |
| `image materialize NAME` | Create the image if absent, declare its manifest from the image recipe `NAME`, and install every package in it (#141) |
| `image ls` / `image show NAME` | List / inspect one (manifest, current version, version history) |
| `image rm NAME` | Remove one; refused for `base`, for an image in use, or with packages installed |
| `image manifest set --image=NAME --package=NAME --mode=pinned\|rolling --version=VERSION` | Add or replace one manifest entry (ADR-0107). `pinned` never advances; `rolling` rebuilds onto a newer recipe version when one is published |
| `image manifest rm --image=NAME --package=NAME` | Remove one entry |
| `image manifest show --image=NAME --version=VERSION` | The `name@version` pairs one image **version** holds, recorded when it was produced (#398). A version produced before these records existed returns 404 |
| `image recipe add --name=NAME --file=PATH` | Publish an image recipe (ADR-0123); recipe name equals image name |
| `image recipe show NAME` / `image recipe rm NAME` / `image recipe ls` | Print / remove / list image recipes |
| `image apply-recipe NAME` | Declare the image's manifest from its recipe. Packages still need installing (`image materialize` does both) |
| `image gc [--dry-run] [--measure]` | Reclaim image versions nothing references. `--dry-run` previews. `--measure` sizes what it finds and can block the daemon for minutes. Refused while a package job runs |

## Devices

| Command | |
|---|---|
| `device ls` | Host PCI/USB/GPU devices from sysfs, each with its `id` (for `--device=`), whether it is assignable, and the interfaces of a composite USB device (ADR-0161) |
| `devicemap create --name=NAME --kind=exact\|vendor_model --selector=SELECTOR` | A persisted, named device binding usable in place of an id in `--device=` (ADR-0048). `exact` takes a device id; `vendor_model` takes `<vendor_id>:<product_id>` and follows the device across USB ports |
| `devicemap ls` / `devicemap rm NAME` | List (with whether each resolves to hardware now) / remove |

## DNS

| Command | |
|---|---|
| `dns provision [--replica=NAME ...] [--no-resolver]` | Bring up the platform's DNS service in one call: create each replica from its deployment, register it as a DNS server, and point the host resolver at it unless `--no-resolver`. Re-runnable |
| `dns record create --name=NAME --ip=A.B.C.D` | Create a record |
| `dns record update --name=NAME --ip=A.B.C.D` | Change a record's address |
| `dns record ls` / `dns record rm NAME` | List / remove |
| `dns server register --container=NAME --hosts-path=PATH` | Register a running container as a DNS server |
| `dns server ls` / `dns server unregister CONTAINER` | List / unregister |
| `dns forwarders show` | Upstream forwarders the DNS servers use |
| `dns forwarders set [--forwarder=IP ...]` | Replace them; no flags clears them |

## LDAP

| Command | |
|---|---|
| `ldap server register --container=NAME --config-path=PATH` | Register a running glauth container as the LDAP server; `--config-path` is its own path to glauth's config file |
| `ldap server ls` / `ldap server unregister CONTAINER` | List / unregister |
| `ldap group add --name=NAME [--gidnumber=N]` | Create a group; the gid is allocated when omitted |
| `ldap group update --name=NAME --gidnumber=N [--new-name=NEWNAME]` | Change a group; a renamed admin group stays in `hostauth-config`'s `admin_groups` |
| `ldap group ls` / `ldap group rm NAME` | List / remove |
| `ldap user add --name=NAME [--uidnumber=N] --primarygroup=N [--secondary-groups=N,N,...] [--givenname=S] [--sn=S] [--mail=S] [--loginshell=S] [--homedirectory=S] [--password=S] [--disabled] [--ssh-key=S] [--can-search]` | Create a user; the uid is allocated when omitted. `--ssh-key=` is served as glauth's `sshkeys` attribute; `--can-search` grants the search right a bind/service account needs |
| `ldap user update --name=NAME [--new-name=NEWNAME] ...` | Replace a user's fields (same flags as `add`) |
| `ldap user ls` / `ldap user rm NAME` | List / remove |
| `ldap config show` | The uid/gid allocation floor, the client login settings, `effective_client_uri` (what `--ldap-client` containers receive now), and the listeners the servers serve |
| `ldap config set [--start-uid=N --start-gid=N] [--client-uri=URIS] [--base-dn=DN] [--bind-dn=DN] [--bind-password=PW] [--client-tls \| --no-client-tls] [--server-plaintext \| --no-server-plaintext] [--server-plaintext-port=N] [--server-tls \| --no-server-tls] [--server-tls-port=N] [--restart-servers]` | Change only the flags given. `--client-tls` picks which enabled listener clients use (see [LDAP over TLS](security.md#ldap-over-tls)). The `--server-*` flags set each glauth's listeners (#419, [ADR-0282](../adr/0282-glauths-listeners-are-configuration.md)); glauth reads listeners only at startup, so add `--restart-servers` to restart them one at a time |

## PKI

| Command | |
|---|---|
| `pki ca bootstrap [--common-name=NAME] [--days=N]` / `pki ca show` | Create / inspect the root CA |
| `pki intermediate bootstrap [--common-name=NAME] [--days=N]` / `pki intermediate show` | Create / inspect an intermediate CA. Needs the root; afterwards `pki cert create` signs with it |
| `pki cert create --name=NAME [--sans=a,b,c] [--days=N]` | Issue a leaf certificate |
| `pki cert ls` / `pki cert rm NAME` | List / remove |
| `pki reset [--root-common-name=NAME] [--intermediate-common-name=NAME] [--root-days=N] [--intermediate-days=N] [--leaf-days=N]` | Destructive: regenerate the whole chain and reissue every tracked leaf |
| `pki export [--passphrase-file=PATH] [--out=PATH]` | The whole PKI store (CA, intermediate, every leaf), encrypted under a passphrase ([ADR-0281](../adr/0281-the-ca-leaves-the-box-encrypted-or-it-is-lost.md)). This is how a CA survives a reinstall; see [Carrying the CA across a reinstall](security.md#carrying-the-ca-across-a-reinstall) |
| `pki import --in=PATH [--passphrase-file=PATH]` | Restore that bundle; refused (409) when a CA already exists |

## Packages

| Command | |
|---|---|
| `pkg ls` | Every known package, installed or in flight. A failure reads `failed:fetch`, `failed:build`, `failed:recipe` or `failed:install` (#101) |
| `pkg install --name=NAME [--image=IMAGE] [--version=VERSION] [--upgrade] [--keep-on-failure]` | Install or upgrade a package; an omitted version resolves by the package's policy. `--keep-on-failure` keeps a failed build's container for inspection ([Debugging a failed build](../api/README.md#debugging-a-failed-build-keep_on_failure-adr-0175-issue-35)) |
| `pkg rm NAME[@IMAGE]` | Uninstall |
| `pkg hostbuild NAME [--version=VERSION] [--wait] [--deploy] [--upgrade] [--keep-on-failure]` | Build or fetch a host artifact (`kernel`, `cix`, `isotools`) instead of installing into an image ([the hostbuild variant](writing-recipes.md#the-hostbuild-variant)). `--upgrade` reruns an installed one whose recipe version moved. `--deploy` implies `--wait` and then writes the result to the inactive slot with `update` (`kernel` and `cix`); it does not reboot |
| `pkg resume --name=NAME [--image=IMAGE] [--version=VERSION] [--keep-on-failure]` | Continue a kept failed build in place, keeping its extracted source ([Continuing a kept build](../api/README.md#continuing-a-kept-build-in-place-post-pkgresume-adr-0177-issue-46)) |
| `pkg cancel --name=NAME [--image=IMAGE]` | Stop an in-flight build, or release a slot whose build container has gone (#213, #339) |
| `pkg update-all` | Start an upgrade for the first installed package whose recipe version has moved on; run it again to take the next |
| `pkg drift` | Every installed package whose recipe is newer than what is installed, against the number installed (#217) |
| `pkg rebuilds` | Image rebuilds queued but not started. Publishing a recipe queues one for every image tracking that package `rolling` (#236) |
| `pkg verify` | Installed packages that are recorded but **not actually in their image**, and packages whose headers include files that do not resolve (#281, #289). Reports only; checks every file of every package |
| `pkg build-log [--name=NAME [--image=IMAGE]]` | Follow a running build's output live; exits when the build ends. 404 when nothing is building. `--name` is required when several builds run (#245); a host build is found by `--name` alone |
| `pkg build-logs [--last \| --file=NAME]` | The complete retained output of recent builds (#57). No arguments lists them with sizes; `--last` prints the newest; `--file=` prints one |
| `pkg buildenv [ls]` / `pkg buildenv rm NAME` | Composed build environments held on this host, and reclaim one now ([ADR-0221](../adr/0221-build-environments-are-reclaimed-by-last-use.md)) |
| `pkg recipes` | Every published recipe version |
| `pkg recipe add --name=NAME --file=PATH [--format=shell\|pbs]` | Publish a recipe version on this host. A published `(name, version)` is never overwritten. The format follows the file extension (`.cbs` is CPDL, `.sh` is shell); `--format=` is for a file not named that way |
| `pkg recipe show NAME [--version=VERSION]` | Print a recipe version; an omitted version means the highest |
| `pkg recipe rm NAME [--version=VERSION]` | Remove one version, or every version when omitted |
| `pkg repo-config show` | The recipe repository this host syncs from |
| `pkg repo-config set [--url=URL] [--kind=gitea\|github\|gitlab] [--ref=REF] [--token=TOKEN \| --clear-token]` | Change only the flags given. How often it syncs is a schedule (`pkg.sync` action) |
| `pkg sync [--wait] [--refetch=NAME@VERSION]` | Fetch the repository and merge its recipes (additive; an existing version is never overwritten). `--refetch=` lets this one sync replace exactly one already-seen version (#59) |
| `pkg sync-status` | The latest sync's outcome |
| `pkg policy ls` | Per-package version policy (#64); unlisted packages use `highest` |
| `pkg policy set NAME --policy=highest\|newest\|pinned [--version=V]` | `newest` takes the most recently published recipe; `pinned` holds a version that `update-all` and `follow_rolling` cannot move |
| `pkg policy clear NAME` | Back to the default |
| `pkg upstreams` | The upstream discovery kinds a recipe may declare, and their channels ([ADR-0255](../adr/0255-a-recipe-is-a-rule-not-a-version.md)) |
| `pkg source-catalogue` | What upstream has published, against what this platform has recipes for |
| `pkg source-policy ls` | Which upstream release each package builds |
| `pkg source-policy set-default [--channel=C] [--depth=n-<lines>.<releases>]` / `pkg source-policy set NAME [--channel=C] [--depth=D]` / `pkg source-policy clear NAME` | Set the default, set one package's policy, or return it to the default |
| `pkg cache-config show` / `pkg cache-config set --max-bytes=N` | The local build-artifact cache's size cap |
| `pkg cache-status` | Cache occupancy |
| `pkg cache-clear` | Remove every cached artifact |
| `pkg artifact-config show` / `pkg artifact-config set [--url=URL] [--token=TOKEN \| --clear-token] [--push \| --no-push]` | The artifact server. `--push` makes a fresh build publish its result there ([ADR-0201](../adr/0201-artifacts-are-retrievable-and-self-publishing.md)); off by default, needs a token |
| `pkg artifact-publish NAME` | Publish an already-built artifact without rebuilding it |
| `pkg artifact-export NAME [--out=FILE]` | Download a hostbuild package's artifact (`kernel`, `cix`, `isotools`) to a local file, named `{name}-{version}.tar.gz` by default |
| `pkg bootstrap [--toolchain=PATH]` / `pkg bootstrap --toolchain-url=URL --toolchain-sha256=SHA256 [--wait]` | Stage a toolchain artifact into the shared build image (ADR-0065). With no flag it copies the daemon host's own toolchain, which is empty on an installed host; build images are normally made with `image materialize` ([Build images](writing-recipes.md#build-images)) |
| `pkg bootstrap-status` | State and error of the most recent `--toolchain-url=` fetch |

### The pipeline

| Command | |
|---|---|
| `pipeline [--all]` | Where every package stands in the pipeline and what is stopping it ([ADR-0256](../adr/0256-the-pipeline-is-the-model.md)). Hides healthy rows unless `--all` |
| `pipeline runs [--name=NAME] [--image=IMAGE] [--limit=N]` | What has happened to a package, as a log ([ADR-0272](../adr/0272-a-pipeline-run-is-a-log-entry-not-join-state.md)) |
| `pipeline approvals` | What is held waiting for a person, and what has been approved ([ADR-0273](../adr/0273-a-gate-holds-automation-where-a-change-escapes-its-blast-radius.md)) |
| `pipeline approve publish\|roll\|deploy TARGET` | Let one held change through |
| `pipeline config [--run-retention=N] [--gate-publish=on\|off] [--gate-roll=on\|off] [--gate-deploy=on\|off]` | Pipeline settings |

See [`writing-recipes.md`](writing-recipes.md) for the recipe format, and [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) / [`building-cix.md`](building-cix.md) for the runbooks built on `pkg hostbuild`.
