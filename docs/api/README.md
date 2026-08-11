# Kanxeo Host API

[`openapi.yaml`](openapi.yaml) (OpenAPI 3.0) is the **authoritative** contract — every field, schema, and status code is defined there, not here. This page is a human-friendly index into it, per the project's API-First Mandate: the REST daemon (`daemon/`, binary `kanxeod`) is the only process with direct access to the container runtime, and everything else (CLI, web dashboard) is built by reading this contract, never the daemon's source.

Default base URL: `http://127.0.0.1:7620/v1` (loopback-only by default; see `daemon/src/main.c`'s `--bind`/`--port` flags).

## Endpoints at a glance

| Method | Path | Purpose |
|---|---|---|
| GET | `/health` | Liveness check -- minimal, low-latency, no build/slot identity |
| GET | `/system/boot` | Build version/time, A/B slot, kernel version (`uname`) |
| POST | `/system/shutdown` | Stop `kanxeod`; powers off the host too when running as real PID 1 |
| POST | `/system/reboot` | Stop `kanxeod`; restarts the host too when running as real PID 1 |
| POST | `/system/update` | Write a fresh control-plane squashfs and/or a fresh kernel onto this daemon's own inactive A/B slot |
| GET | `/system/backup` | Bundle platform configuration state (container defs, networks, DNS, package state, site config) |
| POST | `/system/restore` | Write a previously-backed-up bundle back to its real state files |
| GET | `/system/site` | This install's declared identity (`instance_name`/`site_name`/`domain_suffix`) |
| PUT | `/system/site` | Set this install's site identity |
| GET | `/system/daemon-config` | kanxeod's own listen port, HTTP/HTTPS exposure, and which network is currently its management one |
| PUT | `/system/daemon-config` | Live-reconfigure the listen port, HTTP/HTTPS listeners, or repoint the management network -- no restart |
| GET | `/system/iso` | Status of the most recent server-side installer ISO build |
| POST | `/system/iso` | Assemble a fresh installer ISO server-side, non-blocking |
| GET | `/system/routes` | The box's own real kernel IPv4 routing table |
| GET | `/system/stats` | Host-wide load/CPU/memory/disk/network snapshot |
| GET | `/system/ping` | Poll the current/last ICMP ping job |
| POST | `/system/ping` | Start a real ICMP echo against an IPv4 address |
| GET | `/system/resolv` | The host's own outbound DNS resolver config |
| PUT | `/system/resolv` | Replace it -- takes effect immediately, no reboot |
| GET | `/system/rolling-config` | The configured rolling-restart jitter window (`jitter_window_seconds`) |
| PUT | `/system/rolling-config` | Set the jitter window -- 0 disables jitter, restart happens immediately |
| GET | `/system/ntp` | Upstream NTP server address list used to sync the host clock |
| PUT | `/system/ntp` | Replace it |
| GET | `/system/ntp/status` | Outcome of the most recent sync attempt |
| POST | `/system/ntp/sync` | Trigger a sync attempt now, rather than waiting for the next hourly automatic one |
| GET | `/system/time` | The host's current date/time |
| PUT | `/system/time` | Manually set the host clock (`clock_settime()`, immediate, no reboot) |
| GET | `/ntp/servers` | List all registered NTP server bindings |
| POST | `/ntp/servers` | Register a running container as an available internal NTP time source |
| DELETE | `/ntp/servers/{container}` | Unregister an NTP server binding |
| GET | `/containers` | List all containers this daemon knows about |
| POST | `/containers` | Create and start a container |
| GET | `/containers/{name}` | Inspect one container |
| DELETE | `/containers/{name}` | Stop (if running), remove it, and forget any persisted definition |
| POST | `/containers/{name}/start` | Bring a stopped-but-still-defined container back to life |
| POST | `/containers/{name}/stop` | Kill it now, keep its persisted definition (for `restart: "unless-stopped"`) |
| POST | `/containers/{name}/pause` | Freeze a running container via the cgroup v2 freezer |
| POST | `/containers/{name}/unpause` | Thaw a paused container |
| GET | `/containers/{name}/stats` | Real, host-side CPU/memory/disk/network usage, a point-in-time snapshot |
| GET | `/containers/{name}/files` | Read one file's raw bytes back out of a container's rootfs |
| GET | `/containers/{name}/console` | Upgrade to a WebSocket; an interactive shell inside the running container |
| GET | `/devices` | List host PCI/USB/net devices discoverable via sysfs, available for passthrough |
| GET | `/devicemaps` | List persistent, operator-named device mappings |
| POST | `/devicemaps` | Create a persistent device mapping (name -> selector) |
| DELETE | `/devicemaps/{name}` | Remove a device mapping |
| GET | `/disks` | List real host block devices (whole disks only), for multi-disk management |
| GET | `/diskroles` | List persisted disk role assignments |
| POST | `/diskroles` | Assign a role (container-storage/backup) to a disk |
| DELETE | `/diskroles/{disk_name}` | Remove a disk's role assignment |
| GET | `/disks/{disk_name}/format` | Status of the most recent (or running) format+mount job for this disk |
| POST | `/disks/{disk_name}/format` | Destructive: mkfs (ext4 or btrfs) + mount an already role-assigned disk |
| GET | `/networks` | List all networks this daemon knows about |
| POST | `/networks` | Create a network (a real bridge, persisted across restarts) |
| GET | `/networks/{name}` | Inspect one network |
| DELETE | `/networks/{name}` | Remove a network (refused if any container is still attached, or if it's the management network) |
| POST | `/networks/{name}/interfaces` | Attach a real host network interface to this network's bridge |
| DELETE | `/networks/{name}/interfaces/{ifname}` | Detach a previously-attached interface (refused for the management network) |
| GET | `/images` | List every image this daemon knows about |
| POST | `/images` | Create an empty image (runtime pre-seeded, ready for `pkg install`) |
| GET | `/images/{name}` | Inspect one image, including its manifest |
| DELETE | `/images/{name}` | Remove an image (refused for `base`, if in use, or if it still has packages) |
| POST | `/images/{name}/manifest` | Upsert one `{package, mode, version}` manifest entry (ADR-0107) |
| DELETE | `/images/{name}/manifest/{package}` | Remove one manifest entry |
| GET | `/images/recipes` | List image recipes (metadata only) (ADR-0123) |
| POST | `/images/recipes` | Add/replace an image recipe (`image_packages=`/`image_artifact_sha256=`) |
| GET | `/images/recipes/{name}` | One image recipe's full detail, including its raw text |
| DELETE | `/images/recipes/{name}` | Remove an image recipe |
| POST | `/images/{name}/apply-recipe` | Apply `{name}`'s own stored recipe -- bulk-declares the manifest (204), or an async whole-rootfs artifact fetch for a fully-pinned recipe with a matching artifact server (202) |
| GET | `/images/recipe-apply-status` | Most recent image-recipe-apply artifact fetch's own state/image/error |
| GET | `/dns/records` | List all DNS records this daemon knows about |
| POST | `/dns/records` | Create a DNS record (name -> IP, persisted across restarts) |
| GET | `/dns/records/{name}` | Inspect one DNS record |
| PUT | `/dns/records/{name}` | Edit an existing DNS record's ip in place (task #749) |
| DELETE | `/dns/records/{name}` | Remove a DNS record |
| GET | `/dns/servers` | List all registered DNS server bindings |
| POST | `/dns/servers` | Register a running container as a DNS-serving target |
| DELETE | `/dns/servers/{container}` | Unregister a DNS server binding |
| GET | `/ldap/servers` | List all registered LDAP server bindings |
| POST | `/ldap/servers` | Register a running container as the LDAP-serving target |
| DELETE | `/ldap/servers/{container}` | Unregister an LDAP server binding |
| GET | `/ldap/ssh-targets` | List all registered LDAP SSH targets (task #731) |
| POST | `/ldap/ssh-targets` | Register a running container to receive real Unix accounts + `authorized_keys` rendered from LDAP users |
| DELETE | `/ldap/ssh-targets/{container}` | Unregister an LDAP SSH target (does not touch the container's own filesystem) |
| GET | `/ldap/groups` | List every LDAP group |
| POST | `/ldap/groups` | Create an LDAP group (`gidnumber` optional -- auto-allocated if omitted, task #748) |
| GET | `/ldap/groups/{name}` | Inspect one LDAP group |
| PUT | `/ldap/groups/{name}` | Edit an existing LDAP group's gidnumber in place (task #750) |
| DELETE | `/ldap/groups/{name}` | Delete an LDAP group |
| GET | `/ldap/users` | List every LDAP user |
| POST | `/ldap/users` | Create an LDAP user (`uidnumber` optional -- auto-allocated if omitted, task #748; `ssh_public_key` optional, task #731) |
| GET | `/ldap/users/{name}` | Inspect one LDAP user |
| PUT | `/ldap/users/{name}` | Update an existing LDAP user (full field replacement; `password` omitted keeps the existing credential) |
| DELETE | `/ldap/users/{name}` | Delete an LDAP user |
| GET | `/ldap/config` | Fetch the current `start_uid`/`start_gid` auto-allocation floor (task #748) |
| PUT | `/ldap/config` | Set the `start_uid`/`start_gid` floor -- takes effect for future allocations only, does not renumber existing records |
| GET | `/pki/ca` | Inspect the root CA (never includes the private key) |
| POST | `/pki/ca` | Bootstrap the root CA (once; see `/pki/reset` for regeneration) |
| GET | `/pki/intermediate` | Inspect the intermediate CA (never includes the private key) |
| POST | `/pki/intermediate` | Bootstrap a second CA tier, signed by the root |
| GET | `/pki/certs` | List all issued leaf certificates (metadata only) |
| POST | `/pki/certs` | Issue a leaf certificate signed by the root (or intermediate, if bootstrapped) |
| GET | `/pki/certs/{name}` | Inspect one issued certificate (metadata + cert, never the key) |
| DELETE | `/pki/certs/{name}` | Remove an issued certificate |
| POST | `/pki/reset` | Wipe and regenerate the entire CA chain, reissuing every currently-tracked leaf |
| POST | `/pkg/bootstrap` | Stage the sandboxed build toolchain image (`toolchain_path` local import, or `toolchain_url`+`toolchain_sha256` for the daemon to fetch it itself; once; idempotent) |
| GET | `/pkg/bootstrap` | Status of the most recent `toolchain_url` fetch |
| GET | `/pkg/recipes` | List every published recipe version known to this daemon (metadata only) |
| POST | `/pkg/recipes` | Publish a new recipe version — immutable once published, 409 if this exact (name,version) already exists |
| GET | `/pkg/recipes/{name}` | One recipe version's full detail, including its raw `build.sh` text; `?version=` selects a specific one, omitted resolves to the highest available |
| DELETE | `/pkg/recipes/{name}` | Remove recipe version(s) (does not affect anything already installed via it); `?version=` removes just that one, omitted removes every version |
| GET | `/pkg/repo-config` | The configured recipe-sync source (ADR-0121); `auth_token` itself is never returned |
| PUT | `/pkg/repo-config` | Partially update the configured recipe repo — fields omitted from the body are left unchanged |
| POST | `/pkg/sync` | Start an async fetch-and-merge of the configured repo's recipes (async — returns immediately) |
| GET | `/pkg/sync` | The most recent (or currently running) sync's status |
| GET | `/pkg/cache-config` | The configured local build-artifact cache size cap |
| PUT | `/pkg/cache-config` | Set the cache's size cap (always a real cap — no "unlimited" mode) |
| GET | `/pkg/cache` | Current local build-artifact cache occupancy |
| DELETE | `/pkg/cache` | Clear every cached artifact — an explicit operator reset |
| GET | `/pkg/artifact-config` | The configured plain-HTTP precompiled-artifact server (never a git forge) |
| PUT | `/pkg/artifact-config` | Partially update the configured artifact server |
| POST | `/pkg/install` | Start installing a package (async — returns immediately) |
| POST | `/pkg/update-all` | Start an upgrade for the first installed package whose recipe has drifted |
| POST | `/pkg/hostbuild` | Start a hostbuild job — build a standalone artifact instead of merging into an image |
| GET | `/pkg/hostbuild/{name}` | Inspect one hostbuild job's current state |
| GET | `/pkg` | List every known package (installed or in-flight) with its state |
| GET | `/pkg/{name}` | Inspect one package's current state |
| DELETE | `/pkg/{name}` | Uninstall a package, or clear a permanently-failed entry (never actually merged into any image, so no new image version is produced) |

Every error response is `{"error": "message"}` with an appropriate 4xx/5xx status. Every mutating endpoint that touches disk or spawns a subprocess can in principle also return `500` (a real I/O or subprocess failure, not a client mistake) — see `openapi.yaml`'s own per-path `"500"` response for exactly which internal failure each one covers; the specific set differs per endpoint and isn't repeated here.

## Creating a network

```
POST /v1/networks
{"name": "internal", "subnet": "172.31.0.0", "prefix_len": 24, "address": "172.31.0.1"}
```

- `name` must match `[A-Za-z0-9_-]{1,15}` — it's used verbatim as the Linux bridge interface's name (IFNAMSIZ is 15 chars).
- `subnet` must be the exact network address for `prefix_len` (host bits zero) — `"172.31.0.5"` with `prefix_len: 24` is rejected, only `"172.31.0.0"` is valid. It must also not overlap any existing network's range.
- `prefix_len` must be in `[8, 30]`.
- `address` is optional (ADR-0037, renamed by ADR-0067). Omitted (the default): the bridge is created purely L2, with no host-owned IP address at all — for networks whose own routing is owned by whatever's attached to them (a router pair running a routing protocol, a shared VRRP address, etc.), not the host; containers on it get no default route from this network. Given, it must be a real address within `subnet` (not the network/broadcast address) — it's assigned to the bridge device itself, and every attached container's *primary* network attachment gets it as an automatic default route.

Response (`201`):

```json
{"name": "internal", "subnet": "172.31.0.0", "prefix_len": 24, "address": "172.31.0.1"}
```

Creating a network creates its bridge immediately via rtnetlink and persists the definition to `/var/lib/kanxeo/networks.json` — unlike containers (safe to be in-memory-only, since they die with the daemon), a bridge outlives this process, so the daemon reloads and recreates every persisted network's bridge idempotently at startup.

### Attaching a real host interface

```
POST /v1/networks/internal/interfaces
{"ifname": "eth1", "vlan_id": 0}
```

Enslaves a real, currently-assignable host interface (`GET /devices`'s own `"net:<ifname>"` entries — not already moved into a container's netns, not already attached anywhere via this endpoint) directly into this network's bridge — the "physical ethernet on a host-managed switch" mechanism, distinct from `interfaces` on `POST /containers` (which moves a NIC straight into one container's own netns instead). `vlan_id` 0 or omitted enslaves `eth1` itself, untagged; a nonzero `vlan_id` instead creates and enslaves an 802.1q `eth1.<vlan_id>` sub-interface, leaving `eth1` free to attach (with a different `vlan_id`) to other networks too. `DELETE /v1/networks/internal/interfaces/eth1` detaches it — releasing the interface from the bridge, or deleting the VLAN sub-interface, whichever this call originally created.

## The management network and kanxeod's own listeners

At install time (`kanxeo-install`'s `--ip=`/`--prefix=`/`--gateway=`/`--interface=` flags — see [`installing.md`](../guides/installing.md)), kanxeod bootstraps a real, ordinary network named `management`: the given physical interface is attached to it exactly like `POST /networks/{name}/interfaces` above, and its own address (`--ip=`/`--prefix=`) becomes kanxeod's own bind address. This is a deliberate design choice (Part 0.5) — the host's own management IP lives on a bridge device via the same `network_def` mechanism every other network already uses, visible at `GET /networks`, not a separate GRUB-only address invisible to the API. (`--gateway=` means something different and unrelated: the box's own *upstream* default route, i.e. the home router this box's outbound traffic egresses through — not to be confused with the management network's own `address` field, which is kanxeod's bind address.)

Exactly one network has `is_management: true` at a time (`Network`'s own field, in every `GET /networks` response). Because deleting or detaching from that network's bridge would sever the connection you're managing the box through, `DELETE /networks/{name}` and `DELETE /networks/{name}/interfaces/{ifname}` both unconditionally refuse (`409`) while `is_management` is set — there is deliberately no override/force flag on either generic endpoint. Repointing management to a different network first is the only way past this:

```
GET /v1/system/daemon-config
```

```json
{"port": 7620, "bind": "192.168.50.10", "bind_ip": null, "management_network": "management", "http_enabled": true, "https_enabled": false, "https_port": 8443}
```

```
PUT /v1/system/daemon-config
{"management_network": "lan1"}
```

Resolves `lan1`'s own existing address (it must already have one — `has_address: true`, `400` otherwise) and performs a live listen-socket rebind to it — the new socket is created, bound, and added to `epoll` *before* the old one is torn down, so a failure rolls back to the still-working previous listener rather than leaving a gap. Only once the rebind succeeds does `is_management` actually move from the old network to `lan1`. This works identically whether `lan1` has a physical NIC attached directly or gets its connectivity entirely from a container (e.g. a WiFi-AP container bridging a passed-through wireless radio) — kanxeod only ever cares about the network's own address, never how it's fed.

### A dedicated bind IP, decoupled from the management network's own address

```
PUT /v1/system/daemon-config
{"bind_ip": "192.168.50.20"}
```

`bind_ip` (ADR-0068) is a *second*, dedicated address on the management network's own bridge — kanxeod binds there instead of that network's own address, without the two being the same thing. Useful when the bridge is shared with other traffic (containers, a routing daemon) and the operator wants kanxeod itself pinned to a specific, separate address on it. Must be a real, unused address within the management network's own subnet (`400` otherwise); added to the bridge via a real `rtnl_addr_add_ipv4()` *before* the listener rebinds to it. Any previously-set `bind_ip` is removed from the bridge a couple of seconds *after* the response for this same request has already gone out, not synchronously — confirmed necessary the hard way (see ADR-0068): deleting it immediately can race the kernel's own delivery of the response when this exact request arrived over a connection whose local address *is* the one being removed, which is the common case for an operator reaching the daemon at wherever it's currently bound. Set explicitly to `null` to clear it and revert to the management network's own address:

```
PUT /v1/system/daemon-config
{"bind_ip": null}
```

Repointing `management_network` without also giving a fresh `bind_ip` in the same request implicitly clears any previously-set one — a dedicated `bind_ip` only ever makes sense relative to whichever network is management at the time, so it doesn't silently follow a repoint onto a bridge it was never validated against.

`port`, `http_enabled`, `https_enabled`, and `https_port` are independently settable in the same request or separately:

```
PUT /v1/system/daemon-config
{"https_enabled": true}
```

Starts a second, independent listener on `https_port` (default `8443`), reusing the already-issued PKI `"host"` leaf certificate (see [PKI](#pki-a-ca-chain-and-issued-leaf-certificates) below) — `500` if no root CA has been bootstrapped yet (`POST /pki/ca`), since there's no certificate to serve TLS with. `http_enabled` and `https_enabled` can each be toggled off, but never both in the same request (`400`) — kanxeod must always have at least one live listener, since (installed) it runs as real PID 1 with no "restart" to fall back on. Every change here — port, network repoint, HTTP/HTTPS toggle — is live immediately and also persisted, so it survives a real reboot.

Since kanxeod is PID 1 on an installed system, there is no way to reach it again over the network if it's ever pointed at an address you can't get to — double-check reachability of a new `management_network` (or a firewalled `https_port`) before relying on it as your only way in; physical console access (`docs/guides/installing.md`'s "Console login") is always the fallback.

## The box's own kernel routing table

```
GET /v1/system/routes
```

```json
{"routes": [
  {"dest": "default", "prefix": 0, "gateway": "192.168.15.254", "interface": "eth0", "protocol": 3, "scope": 0},
  {"dest": "192.168.15.0", "prefix": 24, "gateway": null, "interface": "eth0", "protocol": 2, "scope": 253}
]}
```

A real, read-only `RTM_GETROUTE` dump (ADR-0066) — not a Kanxeo-managed resource of its own, just a window onto real kernel state. Exists purely because a real installed box has no SSH and no general shell at all (ADR-0034): before this, there was no way to ever confirm what the kernel actually did with the `--gateway=` value given at install time (fed into a real route add, `rtnl_route_add_default_ipv4()`, that otherwise runs invisibly at boot). `gateway`/`interface` are `null` for on-link routes the kernel derives automatically from each network's own assigned address (`GET /networks`) — only routes with a real next-hop (like the upstream default route) carry a `gateway`. `protocol`/`scope` are the kernel's own raw `rtm_protocol`/`rtm_scope` values, not reinterpreted into names.

### Adding and removing routes

```
POST /v1/system/routes
{"dest": "172.40.0.0", "prefix": 24, "gateway": "172.30.1.1"}
```

```
DELETE /v1/system/routes
{"dest": "172.40.0.0", "prefix": 24}
```

`204` on success. Thin wrappers over `rtnl_route_add_ipv4()`/the new `rtnl_route_del_ipv4()` (ADR-0067 Part 3) — like the `GET` above, neither call touches persisted state; a route added this way is gone on the next reboot, same as any other kernel route not re-applied at boot. Every field is optional: an empty body (or `prefix` omitted/`0`) identifies the default route, the same convention `rtnl_route_add_ipv4()` itself already uses; `gateway` omitted means a direct/on-link route. `POST` `400`s if the kernel itself rejects the route (already exists, unreachable gateway, malformed input); `DELETE` `404`s if no matching route exists to remove. Scoped to exactly what the existing primitives support — no interface/`RTA_OIF` binding, no route-replace semantics.

## A host swap file

```
GET /v1/system/swap
```

```json
{"enabled": false, "size_mb": 0, "path": ""}
```

```
POST /v1/system/swap
{"size_mb": 8192}
```

```
DELETE /v1/system/swap
```

One on-demand swap file, off by default (ADR-0069) — raised after a real Rust/wasm package build ran a freshly-installed box out of RAM. `POST` creates a real, fully-backed (never sparse) file of exactly `size_mb` megabytes, writes a genuine kernel swap-file header into it directly (no dependency on an external `mkswap` binary), and activates it via `swapon(2)`; `409` if swap is already enabled — `DELETE` (swapoff + remove) first to resize. `size_mb` must be in `[64, 1048576]`. Enabled state is persisted and re-applied automatically on every daemon start (including a real reboot) — best-effort, never blocks startup if the file is somehow missing or stale. On modern SSD/NVMe-backed storage, file-backed swap performs identically to a raw partition; a partition was deliberately not pursued here since this project's own install-time partition layout is fixed and repartitioning a live disk on demand is not a risk worth taking for this.

## A consolidated log

```
GET /v1/system/logs?source=audit&level=info&tail=50&since=1700000000
```

```json
[{"ts": 1700000012, "source": "audit", "level": "info", "msg": "POST /v1/networks"}]
```

```
GET /v1/system/logs/config
PUT /v1/system/logs/config
{"max_bytes": 5368709120, "min_level": "info"}
```

One consolidated, size-capped log (ADR-0070): real kernel `dmesg` (source `kernel`, read directly from `/dev/kmsg`), kanxeod's own internal diagnostics (source `kanxeod`, mirrored to stderr too — stderr is still the only channel during boot, before the API is reachable), and a per-request audit trail (source `audit`) — one entry per REST request this daemon handles, method + path, covering every action either `kanxeoctl` or the web dashboard takes since both are pure REST clients. `GET /health` and `GET /system/logs` itself are excluded from the audit trail as low-value polling noise. All three sources interleave into one chronologically-ordered store, not siloed per-source streams.

Storage is 8 rotating segment files, not a byte-exact ring buffer — the oldest whole segment is dropped once the configured `max_bytes` cap is reached (enforced at segment granularity, so expect a few percent of slop against the exact number, the same tradeoff `logrotate`/`journald` already make). `tail` defaults to 1000 and is capped at 5000; `since` is Unix seconds.

`PUT .../config`'s two fields — `max_bytes` and `min_level` — are independent; a real request only ever needs to give the one actually changing, and the response always echoes back the resulting full config. `min_level` (any real syslog severity name: `emerg`/`alert`/`crit`/`err` or `error`/`warning` or `warn`/`notice`/`info`/`debug`, default `debug` — log everything) is checked *at write time*, before an entry ever touches a segment file — genuinely different from `GET`'s own `level` query filter, which only ever filters what's already stored. Each captured log message itself is capped at 4096 bytes (`LOGSTORE_MSG_MAX`, raised from an original, too-small 512 after a real deployment failure's own build-output capture was silently truncated away before the actual error line) — a real build failure's captured output (`pkg %s@%s: build output: ...`) keeps the *tail* of the output, not the head, since the actual error is almost always the last thing printed.

## Creating a container

```
POST /v1/containers
{
  "name": "my-container",
  "image": "test",
  "cmd": ["/bin/some-binary", "arg1"],
  "memory_max": 67108864,
  "pids_max": 32,
  "cpu_max": "50000 100000",
  "cpuset_cpus": "0-1,3",
  "disk_quota_bytes": 1073741824,
  "networks": ["internal", "dmz"]
}
```

- `name` must match `[A-Za-z0-9_-]+` — it's used verbatim as the on-disk directory name under `/var/lib/kanxeo/containers/`.
- `image` must already exist and be populated at `/var/lib/kanxeo/images/{image}/rootfs` — the daemon never creates image content itself (see ADR-0004); a missing image is a `400`, not a silently-empty container.
- `memory_max`/`pids_max`/`cpu_max`/`cpuset_cpus` are optional cgroup v2 limits; omit for no limit. `cpu_max` is the raw cgroup-native `"<quota> <period>"` string in microseconds (e.g. `"50000 100000"` = 50% of one CPU); `cpuset_cpus` is the raw `cpuset.cpus` range-list value (e.g. `"0-1,3"`), restricting which host CPUs this container's processes may run on. Both are passed straight through, not reinterpreted into a percentage or another unit — the same pass-through convention `memory_max`'s bytes and `pids_max`'s raw count already use.
- `disk_quota_bytes` is an optional, real, kernel-enforced hard limit on this container's own overlay upperdir — the enforcement mechanism is picked automatically from the backing filesystem's own type, no separate flag needed. On ext4 (the default), this is a project quota (see [ADR-0062](../adr/0062-ext4-project-disk-quotas.md)); writes past it fail with `EDQUOT` at the filesystem level, and it requires the containers partition to have real project-quota support (`mkfs.ext4 -O quota -E quotatype=prjquota`, the default for a system installed via `kanxeo-install.c`). On btrfs, this is a qgroup hard limit set directly on the container's own upperdir subvolume (see [ADR-0103](../adr/0103-btrfs-quota-backend.md)) — equally real, kernel-enforced, not advisory. Either way, if the backing filesystem can't support the requested enforcement, creation fails `500` with a clear error rather than silently not enforcing the limit. Omit for no limit.
- `disk` is optional (task #638, [ADR-0102](../adr/0102-per-container-disk-selection.md)): a bare disk name (e.g. `"sdb"`, from `GET /disks`) to place this container's own writable storage on, instead of the default OS disk. The disk must already be mounted and carry the `"container-storage"` role (`POST /diskroles`) — `400` if it doesn't exist, isn't mounted, or lacks that role. Echoed back as `disk` on `GET /containers` (`null` for the default placement).
- `networks` is optional: 1–64 entries, each either a bare name (auto-allocated IP) or `{"name": "internal", "ip": "172.31.0.50"}` for an explicit, operator-chosen address — each network must already exist via `POST /v1/networks` (`400` if unknown), and an explicit `ip` must be a usable address on that network: in its subnet, not the reserved address/network address, and not already taken (`400`/`409`). Omit `networks` entirely for no networking (isolated netns, only `lo` — same as before this field existed). The **first** entry is primary and gets the default route; the rest only get their own subnet's connected route.
- `dns_register` is optional, default `false` — see [DNS: records + a real dnsmasq container](#dns-records--a-real-dnsmasq-container) below. Requires `networks` to be set (`400` otherwise).
- `pki_issue`/`pki_cert_dir`/`pki_days` are optional, default `false`/`/etc/kanxeo-tls`/`365` — see [PKI: a root CA and issued leaf certificates](#pki-a-root-ca-and-issued-leaf-certificates) below. Requires the CA to already be bootstrapped (`400` otherwise); does **not** require `networks`.
- `ldap_provision`/`ldap_user`/`ldap_group`/`ldap_uid`/`ldap_secret_dir` are optional, default `false`/(container's own name)/(required when `ldap_provision` is true)/(auto-allocated)/`/etc/kanxeo-ldap` — see [Automatic provisioning: ldap_provision](#automatic-provisioning-ldap_provision) below. Requires `ldap_group` to name an existing LDAP group (`400` otherwise); does **not** require `networks`.

Response (`201`):

```json
{
  "name": "my-container",
  "status": "running",
  "pid": 12345,
  "exit_status": null,
  "exit_reason": null,
  "networks": [
    {"name": "internal", "ip": "172.31.0.2"},
    {"name": "dmz", "ip": "172.32.0.2"}
  ],
  "ip_forward": false
}
```

`exit_reason` (ADR-0080) is a human-readable why once `exit_status` is non-null — either the container's own real diagnostic text (e.g. `"child: execve(/usr/bin/foo): No such file or directory"`) or, when that text isn't available, a fixed category string (e.g. `"clean exit"`, `"overlay: mount(2) itself failed"`). `GET .../{name}` and `GET /v1/containers` both include it the same way; a failure that also reaches `500` at creation time (before any process exists) is instead surfaced directly in that response's own error message and in `GET /system/logs`.

## Persisted, auto-restarting containers

By default a container is purely in-memory: it dies when its own process exits, and nothing about it survives a daemon restart. Add a `restart` policy other than the default `"no"` to make it durable:

```
POST /v1/containers
{
  "name": "router1",
  "image": "router",
  "cmd": ["/bin/bird", "-f"],
  "restart": "always"
}
```

This persists the exact request (`/var/lib/kanxeo/container_defs.json`) in addition to creating it live right now. From then on it's replayed automatically at every future daemon boot, and again after any unprompted exit — each restart after a real, exponentially-backed-off delay (`restart_delay_seconds`, 1–300, default 2 — doubling per consecutive failure, capped at 30s, reset to the base value once the container has stayed up at least 30s before exiting again — never instant, so a genuinely crash-looping container doesn't hammer the host).

`restart` has four values:
- `"always"` — restarts regardless of exit code, including a clean `0` exit.
- `"on-failure"` — restarts only after a nonzero-exit/signal-killed exit, not a clean `0` exit; this only governs the crash-restart timer, boot-time autostart always attempts it regardless of how it last exited (that fact isn't persisted).
- `"unless-stopped"` — behaves like `"always"`, except a prior `POST .../stop` is remembered across a daemon restart (it won't auto-start again until re-`POST`ed); the one policy where `stop` changes daemon-restart behavior.
- `"no"` (default) — today's original behavior, purely in-memory.

`depends_on` controls the order persisted containers start in at boot:

```json
{"name": "router1", "image": "router", "cmd": ["/bin/bird", "-f"], "restart": "always", "depends_on": ["dns1"], "readiness": {"tcp_port": 53, "timeout_seconds": 10}}
```

`dns1` (itself persisted) is guaranteed to have been *started* before `router1` — and, if `dns1` sets its own `readiness` (`{"tcp_port": N, "timeout_seconds": N}`, requires `networks` to be non-empty), genuinely TCP-ready, not just process-started. Readiness is a plain, blocking `connect()` retried until it succeeds or `timeout_seconds` elapses, consulted in exactly one place — daemon-boot autostart, right before a dependent starts — best-effort: if it never succeeds, a warning is logged and boot proceeds anyway, never blocking or failing it. It is never consulted for a live `POST` or for crash-restart. A `depends_on` naming an unknown or non-persisted container, or forming a cycle, is skipped at boot (logged, not fatal to anything else starting).

`GET`/inspect responses always report the current `restart`/`restart_delay_seconds`/`stopped`/`depends_on`/`readiness` state, read live from the persisted definition rather than a stale echo of what creation was originally given.

## Container lifecycle: start, stop, pause, unpause

`POST /v1/containers/{name}/stop` kills it now but keeps its persisted definition (and its on-disk state) — the container comes back on the next daemon restart for `"always"`/`"on-failure"` (a fresh chance every boot), but stays down for `"unless-stopped"` until explicitly re-`POST`ed. `DELETE /v1/containers/{name}` always means gone for good regardless of policy — it removes the persisted definition too, in the same call, and it won't come back on a pending crash-restart or any future boot. It also unmounts and recursively removes the container's own on-disk overlay directories (`upper`/`work`/`merged`, ADR-0106) — best-effort, a cleanup failure is logged but never turns the delete itself into an error, since the registry/definition state is the one source of truth for whether a container exists.

`POST /v1/containers/{name}/start` is the counterpart: brings a stopped-but-still-defined container back to life without a daemon restart, replaying its persisted definition through the same creation path a daemon restart's own autostart already uses — one source of truth for "how a definition becomes a live container." Idempotent — already-running is a plain `200`, not an error. `404` if no persisted definition exists for this name at all (this endpoint only ever replays an existing definition; `POST /containers` creates a new one). Unlike boot-time autostart, this endpoint ignores `restart: "unless-stopped"` gating entirely — an explicit, manual start always means start it.

`POST /v1/containers/{name}/pause` and `.../unpause` freeze/thaw a running container via the cgroup v2 freezer (`cgroup.freeze`) — every task in it is stopped at the kernel level, uninterceptable and unignorable, unlike `SIGSTOP` which a process can catch or handle. Both require an already-live container (`404` otherwise — pausing a stopped-but-defined or nonexistent container makes no sense); unlike `.../stop`'s idempotent double-call tolerance, pausing an already-paused container (or unpausing an already-running one) is a `409`, not a silent `200` — the caller should already know this from its last `GET`. A paused container's own on-disk state (cgroup leaf, upperdir, everything) is otherwise completely untouched — no stop/delete/recreate is involved, it's purely a freeze/thaw of already-running tasks.

`GET`/inspect responses report the current `"status"` (`"running"`/`"paused"`/`"stopped"`) live, not a stale echo.

## Host stats

```
GET /v1/system/stats
```

The host-wide counterpart to container stats below (ADR-0073) — mirrors its own conventions exactly: raw cumulative counters only, no server-side history, client computes its own deltas. Response shape:

```json
{
  "load": {"load1": 0.42, "load5": 0.61, "load15": 0.55},
  "cpu": {"user_jiffies": 12345, "nice_jiffies": 0, "system_jiffies": 4321, "idle_jiffies": 987654, "iowait_jiffies": 12, "irq_jiffies": 0, "softirq_jiffies": 5, "steal_jiffies": 0, "pressure": {"some": {"avg10": 3.03, "avg60": 2.8, "avg300": 2.36, "total_usec": 5812475431}, "full": {"avg10": 1.06, "avg60": 0.63, "avg300": 0.44, "total_usec": 1272027550}}},
  "memory": {"total_bytes": 17179869184, "free_bytes": 10066632704, "available_bytes": 15828671488, "buffers_bytes": 0, "cached_bytes": 5375279104, "swap_total_bytes": 4294967296, "swap_free_bytes": 4294967296, "pressure": {"some": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 17464583}, "full": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 15910263}}},
  "disk": {"total_bytes": 105492467712, "free_bytes": 45524393984, "avail_bytes": 40869130240, "pressure": {"some": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 111434355}, "full": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 90687340}}},
  "networks": [{"name": "eth0", "rx_bytes": 1024, "tx_bytes": 2048, "rx_packets": 12, "tx_packets": 9}]
}
```

- `load` is `/proc/loadavg`'s own 1/5/15-minute averages; `cpu` is `/proc/stat`'s own first `cpu` line (jiffies, cumulative since boot); `memory` is `/proc/meminfo` (bytes, converted from the source file's kB); `disk` is `statvfs()` on the daemon's own data directory (not necessarily the whole root filesystem, if `--data-dir=` points elsewhere).
- `memory.available_bytes` is the kernel's own best estimate of reclaimable-and-usable memory — the number that actually answers "is the box under real memory pressure," unlike a hypervisor's own guest-level "used" figure which typically conflates page cache with genuinely unavailable memory.
- `cpu.pressure`/`memory.pressure`/`disk.pressure` (ADR-0074) are the host-wide cgroup v2 PSI numbers (`cpu.pressure`/`memory.pressure`/`io.pressure`, read from the cgroup v2 root) — `avg10`/`avg60`/`avg300` are percentages of the last N seconds some/all tasks on the box were stalled waiting on that resource, `total_usec` is cumulative stalled time. This answers "is anything actually being held up," a genuinely different question from the raw usage counters above it — a box can show low CPU usage and still have real, measurable stall if something's contending hard for a moment. Zeroed on a kernel without PSI support, not an error.
- `networks` enumerates every real interface under `/sys/class/net` — not scoped to containers (unlike `networks[]` in container stats below, this can include bridges, physical NICs, and any leftover interface the kernel still reports).
- Every field is best-effort: a missing/unreadable source leaves that section zeroed rather than failing the whole request.

## Reachability (ICMP ping)

```
POST /v1/system/ping
{"host": "1.1.1.1"}
```

A real, hand-rolled ICMP echo (ADR-0075) — no shelling out to a `ping` binary, no DNS involved (`host` must be a literal IPv4 address; resolving a hostname through a possibly-broken resolver would reintroduce exactly the DNS-vs-routing ambiguity this endpoint exists to eliminate). Same "kick off + poll" shape as every other uncertain-duration job in this daemon:

```json
{"state": "pending", "host": "1.1.1.1", "reachable": null, "rtt_ms": null, "timed_out": null}
```

`GET /v1/system/ping` polls the same job — `state` moves to `"done"` within a fixed ~2s timeout either way:

```json
{"state": "done", "host": "1.1.1.1", "reachable": true, "rtt_ms": 12.4, "timed_out": false}
```

v1 single-job constraint (same as disk format/ISO build/every other async job here): `409` if a ping is already in flight. `kanxeoctl ping HOST` polls to completion and exits nonzero on an unreachable result, so it's usable directly in a script.

## The host's own outbound DNS resolver (ADR-0076)

```
PUT /v1/system/resolv
{"nameservers": ["1.1.1.1", "192.168.15.31"]}
```

A real, installed Kanxeo host has no outbound DNS resolution mechanism at all by default -- `kanxeod`'s own `curl` subprocess (every `pkg_source` fetch, `pkg bootstrap --toolchain-url=`, `pkg hostbuild`'s git fetch) fails immediately against any real hostname. This endpoint fixes that directly: up to 3 IPv4 addresses (matching glibc's own resolver limit), persisted at `<data-dir>/resolv.conf` and bind-mounted onto the real `/etc/resolv.conf` at boot -- a `PUT` here takes effect **immediately**, no reboot needed, because it writes the same file the bind mount already points at. An empty `nameservers` array clears it (falls back to no outbound resolution, the historical default).

This is deliberately generic -- a plain IP list, no notion of "which container is my DNS server." It covers pointing at one of this platform's own DNS containers (resolve its IP once via `GET /containers/{name}`, `PUT` it here) and pointing at a real external resolver, with the exact same mechanism. `GET /v1/system/resolv` reports the current list.

Note this fixes host-level resolution generally, not just for `pkg`'s own fetches -- every current and future tool `kanxeod` shells out to (`git`, `openssl`, anything added later) resolves through the same, single, canonical `/etc/resolv.conf` path.

## NTP: host clock sync (ADR-0110)

Two related but genuinely independent mechanisms:

**1. The host's own clock.** A small hand-rolled SNTP client (the client subset of RFC 5905) -- setting the host's own clock needs `CAP_SYS_TIME` from the host's own namespace, which no container can do without either breaking isolation or `kanxeod` doing the `clock_settime()` call itself anyway, so this can never be delegated to a containerized workload the way DNS/dnsmasq or LDAP/glauth are.

```
PUT /v1/system/ntp
{"upstream": ["192.168.15.1", "10.0.0.1"]}
```

Same list convention `PUT /v1/system/resolv` already has: up to 3 IPv4 addresses, tried in order at every sync attempt; an empty array clears it (the host then relies solely on any registered `/ntp/servers` container, if any). Synced automatically every hour, or immediately via:

```
POST /v1/system/ntp/sync
```

`202` starts a sync attempt (`409` if one's already in flight, `400` if nothing is configured at all -- no upstream and no registered server container). `GET /v1/system/ntp/status` reports the outcome once it lands:

```json
{"state": "ok", "synced_from": "192.168.15.1", "last_sync_unixtime": 1735689600}
```

`state` is `never` (nothing attempted yet), `ok`, or `failed` (every candidate exhausted with no valid reply, or a valid reply arrived but the final `clock_settime()` itself failed -- e.g. no `CAP_SYS_TIME`).

**2. Container-to-container NTP:** `POST`/`GET`/`DELETE /v1/ntp/servers`, registering a running container as an available time source -- mirrors `POST /v1/dns/servers`/`POST /v1/ldap/servers` exactly:

```
POST /v1/ntp/servers
{"container": "ntp1"}
```

Simpler than either: NTP is itself a live query/response protocol, so this is pure bookkeeping -- no pid/pidfd, no config-file push, no signal. `kanxeod` resolves the registered container's own live IP fresh at every sync attempt (never cached) and queries it directly with the exact same SNTP client as (1) -- a registered container is just one more candidate `ntp_sync_start()` tries, ahead of the configured upstream addresses. `404` if the named container doesn't exist or isn't currently running.

Once a running chrony container exists (`chrony.recipe`, this platform's own standard NTP server -- `chronyd`, plain from-source, `--without-libcap --without-seccomp` so it never attempts privilege dropping in the first place, since it's already the container's own root):

```
POST /v1/containers
{
  "name": "ntp1",
  "image": "ntp_server",
  "cmd": ["/usr/sbin/chronyd", "-d", "-f", "/etc/chrony.conf"],
  "capture_output": true,
  "files": [
    {"path": "/etc/chrony.conf", "content": "local stratum 10\nallow 192.168.15.0/24\ndriftfile /run/chrony.drift\n"},
    {"path": "/etc/passwd", "content": "root:x:0:0:root:/root:/usr/bin/bash\n"},
    {"path": "/etc/group", "content": "root:x:0:\n"}
  ]
}
```

`-d` (not daemonizing) is the same "stay in the foreground, log to stderr" requirement every containerized service here has (`kanxeod` has no init to reap a forking child). `local stratum 10` makes this an orphan reference -- a stable internal time source that doesn't need real upstream internet reachability, appropriate for a LAN-internal NTP source other containers or the host register against; a real deployment wanting real wall-clock accuracy would add real `server`/`pool` directives instead (see (1) above for the DNS-resolution caveat that applies to any hostname used inside a container's own config on a real installed host). **The `/etc/passwd`/`/etc/group` files are not optional**, confirmed the hard way: `chronyd` calls `getpwnam()` to resolve its own `--with-user=root` compile-time default even though privilege-dropping itself is compiled out and it never actually changes UID -- with no `/etc/passwd` at all (this project's minimal images ship none by default), that lookup legitimately fails and `chronyd` exits immediately with `Fatal error : Could not get user/group ID of root`. `capture_output` is what makes this diagnosable at all; see [Diagnosing a container that starts but exits on its own](#diagnosing-a-container-that-starts-but-exits-on-its-own-capture_output) above.

```
POST /v1/ntp/servers
{"container": "ntp1"}
```

`GET`/`PUT /v1/system/time` is a separate, manual escape hatch alongside automatic sync -- view or directly set the host's current clock:

```
PUT /v1/system/time
{"unixtime": 1735689600}
```

Real `clock_settime(CLOCK_REALTIME, ...)` -- immediate, host-wide effect, no reboot needed. Independent of NTP sync, which will overwrite it again at its own next scheduled or on-demand attempt.

### A container can already pull the full DNS record set itself

`GET /v1/dns/records` (below) is a real, working REST endpoint returning every current record as JSON -- any container that can route to the daemon's own bind address can already `curl` it directly and reformat the result into whatever its own DNS server software needs (a zone file, a different hosts format, etc.), as an alternative or supplement to the daemon pushing records into a registered server (`POST /v1/dns/servers`, below). No new mechanism needed for this -- it's a documented usage pattern of an endpoint that already exists, not a new capability.

## Container stats

```
GET /v1/containers/{name}/stats
```

A real, host-side, point-in-time snapshot — no in-container agent, no server-side history (the daemon never stores a sample; call again to get a fresh one, and compute rates/percentages client-side from consecutive samples, exactly what `kanxeoctl stats` and the web dashboard's own Stats tab both do). Response shape:

```json
{
  "cpu": {"usage_usec": 1234567, "user_usec": 900000, "system_usec": 334567, "pressure": {"some": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 0}, "full": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 0}}},
  "memory": {"current": 8388608, "peak": 12582912, "max": null, "pressure": {"some": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 0}, "full": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 0}}},
  "disk": {"upper_bytes": 4096, "read_bytes": 0, "write_bytes": 16384, "read_ios": 0, "write_ios": 4, "pressure": {"some": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 0}, "full": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 0}}},
  "networks": [{"name": "internal", "rx_bytes": 1024, "tx_bytes": 2048, "rx_packets": 12, "tx_packets": 9}]
}
```

- `cpu`/`disk.read_*`/`disk.write_*`/`networks[].*_bytes`/`networks[].*_packets` are cumulative counters (since the container started); `memory.*`/`disk.upper_bytes` are gauges (current value, not a delta). `disk.upper_bytes` is the real, current size of the container's own overlay upperdir content (its actual on-disk footprint, not including the shared, read-only image layer beneath it).
- `cpu.pressure`/`memory.pressure`/`disk.pressure` (ADR-0074) are this container's own `cpu.pressure`/`memory.pressure`/`io.pressure` (`disk.pressure` maps to `io.pressure` — kept alongside the existing `disk` object's other I/O-derived counters rather than a separate top-level key), same shape and semantics as host stats' own pressure fields above. `cpu.pressure.full` is structurally always near-zero for a lightly-threaded container — the kernel only populates it when every task in the cgroup is stalled at once.
- Works for a container that exited on its own (it stays queryable, same as `GET /containers/{name}` itself does, until a real `DELETE`); `404`s once actually removed.
- `networks[].name` is always the container-facing network name (`"internal"`), never the host-side veth implementation name — a host implementation detail this API never leaks.

## Reading a file back out of a container

```
GET /v1/containers/{name}/files?path=/etc/hosts
```

The read-path counterpart to `POST /containers`' own `files[]` (write-only, host-to-container, staged before the container's own `clone3()`). Response is raw bytes (`application/octet-stream`), not JSON — this daemon's only other non-JSON response besides the web dashboard's own static assets and the console WebSocket upgrade.

Path resolution depends on whether the container is currently running: while running, the file is read through `/proc/<pid>/root/<path>` (the container's own mount namespace and root, no privilege boundary crossed since the daemon already runs as real root). Once the container has exited on its own (but not been `DELETE`d — an exited-but-still-registered container is a valid target, same as `GET .../stats` above), the file is read from the container's own real, host-visible overlay upperdir first, falling back to the image's own read-only rootfs if the path was never written by the container itself.

`path` must be an absolute, `/`-leading, traversal-free path (no `.`/`..` component, no empty `//` component) — the exact same validation `POST /containers`' own `files[].path` already applies, reused verbatim. A path resolving to a directory is `400`, not a directory listing — this endpoint reads one file, it does not browse a tree. `kanxeoctl files get NAME --path=/some/path [--output=PATH]` is the CLI surface (stdout if `--output=` is omitted).

## Making a container act as a router

A container attached to two networks with `ip_forward` on will actually forward packets between them — enough for a container running BIRD/FRR to do dynamic routing on top, or for pure static routing on its own:

```
POST /v1/containers
{
  "name": "router",
  "image": "test",
  "cmd": ["/sbin/some-router-process"],
  "networks": ["internal", "dmz"],
  "ip_forward": true
}
```

Other containers then need a static route pointing at the router's IP on their own network to actually reach the far side:

```
POST /v1/containers
{
  "name": "internal-host",
  "image": "test",
  "cmd": ["/bin/some-binary"],
  "networks": ["internal"],
  "routes": [{"dest": "172.32.0.0", "prefix_len": 24, "via": "172.31.0.2"}]
}
```

- `routes` is optional: 0–8 entries, each `{dest, prefix_len, via}` (all required). `dest`/`via` must be well-formed IPv4; `prefix_len` in `[0, 32]`. Only format is validated — whether `via` is actually reachable is the caller's responsibility. Set once at creation; not modifiable on an already-running container.
- `ip_forward` is optional, default `false`. Per-netns — never affects the host or other containers.

## Interactive container console (`docker exec -it`-style)

`GET /v1/containers/{name}/console` opens a real, fully-interactive shell inside an already-running container (ADR-0043) — not a normal request/response endpoint, an HTTP/1.1 Upgrade to a hand-rolled RFC 6455 WebSocket (no fragmentation, 64KiB payload cap; OpenAPI 3.0 has no first-class way to type this, so `openapi.yaml` documents it as a GET whose success response is `101 Switching Protocols`). The exec'd process joins the target container's own mount/UTS/network/pid namespaces (`setns()`, equivalent to `nsenter --mount --uts --net --pid --target <pid>`) against a PTY allocated in the daemon's own namespace before any `setns()` call, so it never needs a working `devpts` inside the container itself. Command defaults to `/usr/bin/bash` (this project's own images stage everything under `usr/bin/`, never `/bin`); override with the `X-Kanxeo-Exec-Cmd` request header.

Two real, ready-to-use clients — neither requires hand-rolling the handshake yourself:

- **`kanxeoctl console NAME [--cmd=/path/to/shell]`** — a full, `termios` raw-mode terminal: tab completion, Ctrl-C, `vim`/`top`/`less` all work correctly, since it drives a real local terminal end to end.
- **The web dashboard's own Console tab** (a container's default view when selected in the left tree) — the browser's native `WebSocket` object talks directly to this endpoint, no hand-rolled handshake needed client-side. Deliberately reduced fidelity by design (ADR-0010's "no framework" constraint, confirmed with the user rather than silently accepted): a line-buffer renderer with `\r`/`\n`/backspace/Tab and SGR color support, no cursor-addressable screen model, so full-screen redraw programs (`vim`, `top`, `less`) render wrong there specifically — `kanxeoctl console` has no such limitation.

Any frame-parse failure post-upgrade (including an unmasked client frame, which RFC 6455 requires a server to reject) — or a WebSocket CLOSE frame from either side, or the exec'd process exiting on its own — ends the session the same way: `SIGKILL` the exec'd process, then close the raw connection immediately (no WS CLOSE frame is sent back, no HTTP status is possible once the connection is a WebSocket at all) — no leaked processes survive session teardown. No new authentication layer exists for this endpoint — exactly as protected as every other existing mutating endpoint today (network reachability only), a more sensitive capability than most, worth stating plainly rather than leaving implicit.

## Diagnosing a container that starts but exits on its own (`capture_output`)

The console endpoint above needs the target process to still be running — no help for a container whose command execve()s fine and then exits on its own, taking whatever it printed to stderr with it. `capture_output` (optional, default `false`, on `POST /containers`) closes that gap: it captures the container's own stdout/stderr — interleaved, oldest-first, bounded at 4096 bytes — into an in-memory buffer readable back afterward via `GET /containers/{name}`'s `captured_output` field, the same mechanism `pkg.c`'s own build containers have used internally since ADR-0087, now opt-in for ordinary containers too.

```
POST /v1/containers
{
  "name": "jumpbox1",
  "image": "jumpbox",
  "cmd": ["/usr/sbin/sshd", "-D", "-e"],
  "capture_output": true
}
```

```
GET /v1/containers/jumpbox1
{
  "name": "jumpbox1",
  "status": "exited",
  "exit_status": 1,
  "captured_output": "sshd: no hostkeys available -- exiting.\n",
  ...
}
```

`captured_output` is `null` (not `""`) when `capture_output` was never requested — the two are deliberately distinguishable: `null` means "opted out," `""` means "opted in, nothing written yet." It keeps growing live while the container runs and simply stops once the process (and every descendant it forked) exits and the capture pipe's write end fully closes — not a live tail (see the build-log WebSocket below, or the console endpoint above, for that), just a diagnostic snapshot worth reading after the fact. Bytes past the 4096-byte cap are silently dropped, not the earliest ones — generous enough for a real startup failure's own error text, bounded so one runaway-logging container can't grow a registry entry unbounded.

## Live-tailing an in-flight package build (task #676, ADR-0101)

`GET /v1/pkg/build/log` streams a currently-running `pkg install`/`pkg hostbuild` job's own stdout/stderr live, over the same minimal WebSocket upgrade the console endpoint uses — but it's a genuinely different, much simpler mechanism: a one-way relay of an already-epoll-drained pipe (ADR-0087), not an interactive exec/PTY session. Before this existed, a build's output was only ever visible after the fact, and only on failure (folded into the logged error, `GET /pkg/{name}`'s `error` field) — a slow build in progress (this project has hit real multi-minute ones: `perl`, `gcc` from source) had no REST-visible signal at all while it ran.

On a successful upgrade, whatever of the build's output was already captured is sent immediately as one frame (so attaching mid-build doesn't start blind), then every further chunk streams live as the build produces it. The daemon sends a real WebSocket CLOSE frame — and tears every attached client down — the instant the build finishes, success or failure; there's no "reconnect and keep watching," since by definition there's nothing left to watch. `404` if no build is currently in progress; `503` if 4 clients are already attached (a soft cap matching this project's own "one build in flight at a time" invariant, not a silent drop).

`kanxeoctl pkg build-log` is the CLI client — no raw terminal mode, no input relay (this stream is one-way), just prints each chunk to stdout as it arrives and exits cleanly once the daemon's own CLOSE frame lands.

## DNS: records + a real dnsmasq container

DNS records are a REST resource; the actual name resolution is done by a real DNS server (dnsmasq recommended) running as a normal containerized workload — not hand-rolled, the same reasoning BIRD wasn't hand-rolled for routing (ADR-0007's "no external libraries" rule is about this project's own platform components, not about workloads a container runs).

```
POST /v1/dns/records
{"name": "db.internal", "ip": "172.31.0.5"}
```

- `name` is a hostname (dot-separated labels, `[A-Za-z0-9-]`, RFC 1035 length limits) — a different charset from network/container names, which don't allow dots. A `name` with no `.` at all gets this install's own site suffix appended by default (`<name>.<site_name>.<domain_suffix>`, see [This install's identity](#this-installs-identity-site-config) below) — fully overridable by including a `.`.
- `ip` must be well-formed IPv4.

`PUT /v1/dns/records/{name}` (task #749) edits an existing record's `ip` in place — `name` is authoritative from the URL path and, unlike `POST`, is never re-qualified with the site suffix (that qualification only ever applies at creation time). `400` on a malformed `ip`, `404` if no record with that name exists.

Once a container running dnsmasq exists (e.g. `cmd: ["/usr/sbin/dnsmasq", "-k", "-u", "root", "-p", "53", "-H", "/etc/dnsmasq-hosts", "-R", "-h", "--pid-file=", "--server=1.1.1.1", "--server=8.8.8.8"]` — `-u root` since a minimal container image typically has no `/etc/passwd` for dnsmasq's default privilege drop to resolve; `-R`/`-h` skip `/etc/resolv.conf`/`/etc/hosts`, which likely don't exist either; `--pid-file=` (empty) disables dnsmasq's own default pidfile write, `/var/run/dnsmasq.pid` — confirmed the hard way, this project's own minimal images have no `/var/run` (only `/run`), so without this flag dnsmasq dies immediately with its own `EC_FILE` (exit status 3) on every single startup attempt, an instant, silent crash-loop under `restart: always` with no REST-visible cause; the two `--server=` flags are real, static upstream forwarders (ADR-0076) — without them `-R` alone leaves this container purely authoritative for `.internal`, with no recursion for anything else, which is what it was until this ADR), register it:

```
POST /v1/dns/servers
{"container": "dns1", "hosts_path": "/etc/dnsmasq-hosts"}
```

This writes every current record into `dns1`'s own `/etc/dnsmasq-hosts` (dnsmasq's `--addn-hosts` format) and sends `SIGHUP` so it reloads immediately. Every subsequent `POST`/`DELETE` on `/v1/dns/records` re-writes that file and re-signals `dns1` — genuinely live updates, not a one-time snapshot at registration.

The write itself goes through `/proc/<pid>/root/<hosts_path>` (the container's own filesystem view via the magic procfs symlink), not the container's raw upperdir directly — writing straight into a running container's upperdir does **not** reliably show up in its mounted view (confirmed empirically; the kernel documents this as unsupported/undefined for an already-mounted overlay). `/proc/<pid>/root/` correctly resolves through the container's real mount namespace without needing any new namespace-entry syscall.

### Automatic registration: `dns_register`

Instead of a separate `POST /v1/dns/records` call, a container can register its own name at creation time:

```
POST /v1/containers
{
  "name": "db",
  "image": "test",
  "cmd": ["/bin/some-binary"],
  "networks": ["internal"],
  "dns_register": true
}
```

This creates a record named `db` pointing at `db`'s IP on its primary (first) network, as part of container creation — any already-registered dnsmasq container (`POST /v1/dns/servers`) picks it up immediately via the same live-reload path as a manually-created record, so `dig db.internal.example @dns1` (or whatever domain dnsmasq is configured to answer for) resolves right away. `GET /v1/dns/records/db` shows `"owner": "db"` to distinguish it from a manually-created record (`"owner": null`). Deleting the `db` container automatically removes its record; a manually-created record is never touched by any container's deletion, even if it happens to share that container's name but wasn't the one that created it.

Registration is best-effort and non-fatal to container creation: if a record named `db` already exists (e.g. a stale one persisted from a previous container of the same name — DNS records outlive a daemon restart, containers don't), registration is silently skipped rather than overwriting it, and the container is still created successfully.

Also auto-maintained: this install's own instance DNS record (its FQDN pointing at its own `--bind=` address), reconciled at daemon startup and again on every `PUT /system/site` — see [This install's identity](#this-installs-identity-site-config) below.

## LDAP: server registration, user/group CRUD, and automatic provisioning

LDAP server registration mirrors DNS server registration's own REST shape and persistence discipline, but with one real, deliberate difference: **registration itself never touches the container's filesystem or sends any signal**. `glauth` (`pkg/recipes/glauth/2.4.0/build.sh`, this platform's own standard integrable LDAP provider, replacing `lldap`) runs a real `fsnotify` watcher on its own config file whenever that file sets `watchconfig = true` — confirmed directly against glauth's own source (`v2/glauth.go`'s `startConfigWatcher()`) — and reloads automatically on any write. DNS server registration exists because dnsmasq only reads its hosts file once at startup; glauth has no equivalent gap to work around.

Once a running glauth container exists (its own config file staged at creation time, `datastore = "config"` and `watchconfig = true` set, the same `--file=` staging convention `lldap.recipe` established), register it so its config file's path is on record for the LDAP user/group CRUD endpoints below to render into:

```
POST /v1/ldap/servers
{"container": "ldap1", "config_path": "/etc/glauth/glauth.cfg"}
```

- `container` must already exist and be running (`404` otherwise, same rule `POST /v1/dns/servers` enforces).
- `config_path` is the container's own absolute view of glauth's config file (the one it was started with `-c`) — rejected (`400`) if not absolute or if it contains `..`. Nothing is read or written at registration time beyond a full user/group sync (see below); this call is otherwise pure bookkeeping.

`GET /v1/ldap/servers` lists every current binding; `DELETE /v1/ldap/servers/{container}` unregisters one (does not touch the container itself). Deleting the container automatically removes its binding (`ldap_server_forget()`, called from the same container-delete cleanup path as `dns_server_forget()`). Bindings are persisted (`<data-dir>/ldap_servers.json`) and survive a daemon restart, the same as DNS server bindings (ADR-0091).

### User/group CRUD

Same durable-record-store model DNS records use (`dns_record_create()`/`delete()`/`find()`): Kanxeo itself is the source of truth for every user/group (persisted to `<data-dir>/ldap_users.json`/`ldap_groups.json`, survives a glauth container being deleted or rebuilt), and every create/update/delete re-renders the **full** current user/group set as glauth "config" datastore TOML and writes it into every currently-registered, currently-running server's own config file — the same always-whole-file-rewrite behavior `dns_write_hosts_file()`/`dns_server_sync_all()` already use for dnsmasq's hosts file. No signal is sent; glauth's own config watcher picks up the write. The write preserves everything above the first `[[users]]`/`[[groups]]` stanza in the container's current config file byte-for-byte (the operator's own `[backend]`/`[ldap]`/`[api]`/TLS settings), so only the managed tail is ever replaced.

```
POST /v1/ldap/groups
{"name": "superheros", "gidnumber": 5501}

POST /v1/ldap/users
{"name": "j_doe", "uidnumber": 5001, "primarygroup": 5501, "mail": "j.doe@kanxeo.internal", "password": "dogood"}
```

- Group `name`/user `name` follow POSIX-ish username rules (lowercase letters/digits/`_`/`-`, must start with a letter or `_`).
- A user's `primarygroup` must name an existing group's `gidnumber` (`404`-equivalent `LDAP_RECORD_ERR_GROUP_NOT_FOUND` otherwise) — create the group first.
- `password`, if given, is hashed with SHA-256 (`passsha256`, a real glauth "config" datastore field) via kanxeod's own already-linked OpenSSL `libcrypto` — never stored or echoed in plaintext, and never returned by any `GET` (only a `has_password` boolean is). Omitting `password` on `PUT .../users/{name}` leaves the existing credential unchanged.
- Capability/ACL grants (glauth's own `capabilities` config stanza) are set directly here only for the auto-provisioned service accounts below — the `password`/username/group CRUD above stays directory-data-only.
- `gidnumber`/`uidnumber` are optional on `POST` (task #748): omit either one and it's auto-allocated — `ldap_gid_alloc()`/`ldap_uid_alloc()` scan upward from a configurable floor (see below) for the next value not already in use, the same "scan for next free above floor" algorithm both have always used, just with the floor itself now settable instead of a hardcoded `10000`.

`GET`/`DELETE` follow the same `/v1/ldap/users/{name}` and `/v1/ldap/groups/{name}` shape as every other named resource in this API; `PUT /v1/ldap/users/{name}` updates an existing user (full field replacement, `password` optional as above); `PUT /v1/ldap/groups/{name}` updates an existing group's `gidnumber` (task #750) — unlike `POST`, `gidnumber` is always required in the body on `PUT` and is never auto-allocated, matching every other PUT resource's full-replacement semantics. Neither `PUT` cascades to records referencing the old value: editing a group's `gidnumber` does not update any user's `primarygroup`, and callers are responsible for that themselves if needed.

### Configurable uid/gid auto-allocation floor

```
GET /v1/ldap/config
{"start_uid": 10000, "start_gid": 10000}

PUT /v1/ldap/config
{"start_uid": 50000, "start_gid": 50000}
```

`start_uid`/`start_gid` (task #748) are the floors `ldap_uid_alloc()`/`ldap_gid_alloc()` scan upward from when `POST /v1/ldap/users`/`POST /v1/ldap/groups` omits `uidnumber`/`gidnumber`. Both default to `10000` until changed. Setting a new floor takes effect immediately for the *next* auto-allocation only — it never renumbers any user or group that already exists, and both values must be positive integers (`400` otherwise). Persisted to `<data-dir>/ldap_config.json`, survives a daemon restart.

### SSH target account sync (task #731)

There is no real LDAP-protocol NSS/PAM stack in this project (no `libnss_ldap`/`pam_ldap` recipe, and `openssh.recipe` was deliberately built without PAM — confirmed via `sshd -d` reporting "Unsupported option UsePAM"). Rather than take on building and testing that whole integration, an SSH target extends the same "Kanxeo owns the durable record, renders into the consumer's own filesystem" pattern DNS and LDAP-for-glauth above already establish, a third time: registering a container renders real `/etc/passwd`/`/etc/group`/`/etc/shadow` entries plus a per-user `~/.ssh/authorized_keys` file directly onto its filesystem for every LDAP user carrying a non-empty `ssh_public_key` — sshd itself never talks to LDAP or kanxeod, it just reads ordinary, locally-resolvable account files that happen to be kept in sync.

```
POST /v1/ldap/ssh-targets
{"container": "jumpbox1"}

POST /v1/ldap/users
{"name": "j_doe", "uidnumber": 5001, "primarygroup": 5501,
 "ssh_public_key": "ssh-ed25519 AAAA...", "password": "dogood"}
```

- `container` must already exist and be running (`404` otherwise, same rule `POST /v1/ldap/servers` enforces) — validated directly against the registry, not a pre-check elsewhere.
- Registering a target syncs it immediately, so a fresh registration starts current rather than empty — the same behavior `POST /v1/ldap/servers` already has.
- Every subsequent `POST`/`PUT`/`DELETE` against `/ldap/users` or `/ldap/groups` re-syncs every registered, currently-running SSH target automatically (`ldap_ssh_sync_all()`, called from the same `ldap_record_sync_all()` every other LDAP mutation already goes through) — no separate sync call is ever needed.
- Only users with a non-empty `ssh_public_key` and `disabled: false` get an account; disabled or key-less users are skipped by SSH target sync entirely (though they remain ordinary directory entries for glauth's own purposes).
- The rendered shadow entry always uses `*` (locked password, pubkey auth still works), never `!` (which OpenSSH treats as a fully locked account, blocking *all* auth methods including pubkey) — a real gotcha, confirmed the hard way during task #730's own manual provisioning.
- `/etc/passwd`/`/etc/group`/`/etc/shadow` writes use a marker-line-based "managed tail" (`write_managed_tail()`), not a whole-file rewrite: everything above the marker (root/sshd's own pre-provisioned entries) is preserved byte-for-byte, only the managed tail below it is replaced on every sync — the same conceptual approach `ldap_write_config_file()` uses for glauth's TOML, adapted for a format with no natural stanza boundary.
- `GET /v1/ldap/ssh-targets` lists every current registration; `DELETE /v1/ldap/ssh-targets/{container}` unregisters one (does not touch the container's own filesystem — already-rendered accounts are left in place). Deleting the container automatically removes its registration (`ldap_ssh_target_forget()`, same cleanup path as `ldap_server_forget()`). Persisted to `<data-dir>/ldap_ssh_targets.json`, survives a daemon restart.
- A general, related fix landed alongside this feature: every newly-created image now gets `libnss_files.so.2` staged into its shared-lib closure and a default `/etc/nsswitch.conf` (`passwd/group/shadow/hosts: files`) written if one doesn't already exist (`pkg_seed_image_baseline()`) — without it, even a correctly-rendered `/etc/passwd` entry fails to resolve at all (`getpwnam()` silently empty), since glibc's NSS "files" backend is a `dlopen()`ed module, never picked up by the `ldd`-based shared-library closure staging that already handles every ELF-`NEEDED` dependency.

### Automatic provisioning: `ldap_provision`

Mirrors `dns_register`/`pki_issue`'s own container-creation-hook shape closely, but provisions a **service/bind account for the container itself** — not a human login account, those stay entirely in the CRUD endpoints above:

```
POST /v1/containers
{
  "name": "svc1", "image": "myapp",
  "ldap_provision": true,
  "ldap_group": "svcaccts"
}
```

- `ldap_group` is required (`400` if omitted or the group doesn't exist) — the provisioned account's `primarygroup`.
- `ldap_user` is optional, defaults to the container's own name.
- `ldap_uid` is optional, defaults to an auto-allocated `uidnumber` (`ldap_uid_alloc()` picks the next free one above every currently-known user).
- `ldap_secret_dir` is optional, default `/etc/kanxeo-ldap` — where the delivered `bind.secret` (chmod `0600`) lands inside the container's own filesystem.
- A `"search"` capability (`object = "*"`) is granted by default — glauth denies all LDAP operations by default otherwise, and a bind-only account with zero capabilities couldn't do anything useful.
- The secret itself is generated fresh from `/dev/urandom` (`ldap_generate_secret()`, 16 raw bytes hex-encoded to 32 characters) on every single fire of this hook, including every `restart:"always"` respawn — **the plaintext secret is never persisted anywhere in Kanxeo's own state**, only its SHA-256 hash (`passsha256`) survives in the durable user record. This means a respawned container always gets both a fresh secret and a fresh delivery, even though the underlying account (same name, same owner) already existed — tolerated the same way `pki_issue` tolerates re-delivery to a respawned container's fresh pid.
- The account's `owner` field is set to the container's own name and it is automatically removed when the container is deleted (`ldap_user_forget_owner()`, called from the same container-delete cleanup path as `dns_record_forget_owner()`/`pki_cert_forget_owner()`).

## PKI: a CA chain and issued leaf certificates

A single internal root CA, an optional second intermediate tier, and leaf certificate issuance. Actual cryptography (keypair generation, CSR signing) is done by the daemon shelling out to the system's real, unmodified `openssl` binary as a short-lived subprocess — the same "real software, not hand-rolled" reasoning BIRD and dnsmasq were chosen under (ADR-0007's "no external libraries" rule governs this project's own platform components, not real software it invokes or runs as a workload).

Bootstrap the root CA once:

```
POST /v1/pki/ca
{"common_name": "Kanxeo Root CA", "days": 3650}
```

Both fields are optional (shown defaults). **The CA private key is never returned over the API, in any endpoint, ever** — it's the root of trust and must never leave the host. A second `POST /v1/pki/ca` is a `409` — `pki_ca_create()` is a one-shot by design; see [Regenerating the whole chain](#regenerating-the-whole-chain-post-pkireset) below for the real "start over" operation.

### A second, intermediate CA tier

```
POST /v1/pki/intermediate
{"common_name": "Kanxeo Intermediate CA", "days": 1825}
```

Requires the root to already be bootstrapped (`400` otherwise); a second call is `409`, the same one-shot-only precedent as `/pki/ca`. Once this succeeds, every future `POST /pki/certs` leaf is signed by the intermediate instead of the root automatically — transparent to that endpoint, no separate opt-in — and `GET /pki/certs/{name}` plus any `pki_issue`-delivered `tls.crt` then carries the real, complete chain (leaf + intermediate). The intermediate's own private key is never returned over the API either, same as the root's.

Issue a leaf certificate:

```
POST /v1/pki/certs
{"name": "svc.internal", "sans": ["svc.internal", "svc"], "days": 365}
```

- `name` is a hostname (same RFC 1035 rules as `DnsRecord.name`) — it becomes the certificate's CN and this endpoint's REST identifier. A `name` with no `.` gets this install's own site suffix appended by default, same rule as DNS records — fully overridable by including a `.`.
- `sans` is optional and defaults to `[name]` — a cert always carries at least its own name as a Subject Alternative Name. The default-qualification rule above is never applied to an explicitly-supplied `sans` entry, only to `name` and the default SAN derived from it.
- `days` is optional, default 365.

The `201` response (`PkiCertIssued`) is the **only** place the leaf's private key is ever returned:

```json
{
  "name": "svc.internal",
  "serial": "11EAFF2213CAE630340F388C62AE620833EBEDFF",
  "not_after": "Jul 27 10:25:19 2027 GMT",
  "sans": ["svc.internal", "svc"],
  "cert_pem": "-----BEGIN CERTIFICATE-----\n...",
  "key_pem": "-----BEGIN PRIVATE KEY-----\n..."
}
```

Neither `GET /v1/pki/certs` (list) nor `GET /v1/pki/certs/{name}` (single) ever includes `key_pem` again — save it now. Both do include `cert_pem` on the single-item view (list omits it too, to keep listing lightweight).

Deleting a cert (`DELETE /v1/pki/certs/{name}`) removes its key and cert files from disk, not just the metadata index entry.

### Automatic issuance + delivery: `pki_issue`

Instead of a separate `POST /v1/pki/certs` call (and then figuring out how to get the result into the container), a container can get its own cert issued *and delivered into its own filesystem* at creation time:

```
POST /v1/containers
{
  "name": "web",
  "image": "test",
  "cmd": ["/bin/some-binary"],
  "pki_issue": true,
  "pki_cert_dir": "/etc/kanxeo-tls",
  "pki_days": 365
}
```

`pki_cert_dir` and `pki_days` are optional (shown defaults). This issues a cert named `web` (CN and sole SAN) and writes `tls.crt`/`tls.key` (chmod 0600) into `/etc/kanxeo-tls` **inside the `web` container's own filesystem** — the same `/proc/<pid>/root/<path>` mechanism `POST /v1/dns/servers` already uses to reach into a running container (ADR-0013), just delivering a cert+key instead of a hosts file. Unlike DNS server bindings, delivery is **one-time**: there's no live resync, since a cert doesn't change after a container starts — except after a `/pki/reset` (below), which explicitly redelivers to every still-live container that owns a reissued leaf. `GET /v1/pki/certs/web` shows `"owner": "web"`; deleting the `web` container automatically removes its cert (both the index entry and the on-disk key/cert files) — a manually-created cert is never touched by any container's deletion, even if it happens to share that container's name but wasn't the one that created it.

Unlike `dns_register`, `pki_issue` does **not** require `networks` — the cert identifies the container by name, not by IP, and delivery works for any running container regardless of networking. It **does** require the CA to already be bootstrapped, checked upfront as a `400` (you can't issue a cert with no CA). A *name collision* discovered only at issuance time (e.g. a stale cert persisted from a same-named container created before a daemon restart) is best-effort instead: issuance is silently skipped rather than overwriting it, and the container is still created successfully.

This install also always keeps a `"host"` leaf current for itself, auto-(re)issued whenever `PUT /system/site` changes this install's identity, or whenever the CA chain changes at all — nothing an operator needs to request separately.

### Regenerating the whole chain: `POST /pki/reset`

`pki_ca_create()`/`pki_intermediate_create()` are one-shot by design and refuse a second call outright (`409`) — this is the explicit, real "start over" operation that design deliberately doesn't provide implicitly:

```
POST /v1/pki/reset
{"root_common_name": "Kanxeo Root CA - lab.internal", "intermediate_common_name": "Kanxeo Intermediate CA - lab.internal"}
```

Destructive: deletes the root (and the intermediate, if one was bootstrapped) and every leaf's on-disk key/cert, then re-bootstraps the root (and intermediate, only if one existed before this call) with new common names (each defaults to `"Kanxeo Root/Intermediate CA - <domain_suffix>"` if omitted), then reissues every leaf that was tracked beforehand — same name/SANs/owner, a fresh keypair and validity period for each. Every reissued leaf's `cert_pem` **and** `key_pem` are included in the response — a genuine new issuance moment for each, the identical "returned exactly once, right now" treatment a leaf's key already gets at first issuance. A leaf whose reissue itself fails is simply gone, not left in its old state, since its old key/cert (signed by a CA that no longer exists the instant this proceeds) are already unlinked before any reissue is attempted. Any leaf owned by a still-live, `pki_issue`-created container is automatically redelivered into that container's own filesystem afterward, so a running service's `tls.crt`/`tls.key` don't go stale.

## Device passthrough (PCI/USB/GPU)

```
POST /v1/containers
{
  "name": "nas",
  "image": "base",
  "cmd": ["/bin/some-binary"],
  "devices": ["usb:1-2", "gpu:0"]
}
```

- `devices` is optional: 0–N entries, each a discovered device id from `GET /v1/devices` (`"pci:..."`, `"usb:..."`, or `"gpu:N"` for a whole GPU — `gpu:N` is never itself listed by `GET /v1/devices`, only its individual member nodes are), **or** the name of a persistent device mapping (below). Real `/dev` nodes are granted via a `BPF_CGROUP_DEVICE` program on the container's own cgroup (ADR-0017) — nothing else on the host can reach them once bound. A bare `gpu:N` id expands into every node that physical GPU needs in one grant (DRM `cardN`/`renderDN` plus the shared `/dev/kfd` compute node) — see ADR-0028/ADR-0029.
- `interfaces` is optional: 0–N real host network interface names (e.g. `"eth1"`) moved directly into the container's own netns (not a veth pair) — fd-anchored teardown, correct even if the container crashes mid-move. See ADR-0022.
- `GET /v1/containers` echoes the real, expanded grants actually made, not an echo of what was requested.

### Persistent, named device mappings

`GET /devices`'s own ids are ephemeral — re-enumerated fresh from sysfs on every call, never persisted, and a USB device's bus/port-derived id can change if it's ever plugged into a different port. A device mapping is a real, named, persisted binding an operator creates once and references by a stable name thereafter, in a container's own `devices` field or here:

```
POST /v1/devicemaps
{"name": "backup-drive", "kind": "exact", "selector": "usb:1-2"}
```

`kind` is `"exact"` (pins one specific bus/port location) or `"vendor_model"` (matches by USB vendor:product id or PCI vendor:device id, following whichever physical port the matching device is actually plugged into — the more useful choice for a device that might move ports, like a specific model of USB drive). Real and creatable even for hardware that isn't currently plugged in — an operator predefining a mapping before plugging the device in, or one that's temporarily unplugged, are both legitimate states (`"present": false` on `GET`), not errors. Each mapping is still resolved fresh against current hardware on every `GET` (`present`/`resolved_ids`), never cached — only the *mapping itself* (name → selector) persists, not a hardware snapshot. `DELETE /devicemaps/{name}` does not touch anything about a container already using this mapping's name — device grants are resolved once, at container-creation time, never re-resolved live afterward.

## Disks (multi-disk management)

```
GET /v1/disks
```

Real host block devices, whole disks only (partitions are never listed independently — they aren't independently assignable), live-enumerated from `/sys/class/block` on every call, the same "real hardware, never persisted" convention `GET /devices` already established. `is_os_disk` flags the one disk holding this platform's own fixed ESP/root-a/root-b/config/containers layout — never a candidate for a role of its own or for formatting; every other disk is available for a role assignment and, once role-assigned, formatting.

`mounted`/`mount_path` are real, current ground truth read fresh from `/proc/mounts` on every call — true if any partition on the disk (or the whole-disk device itself) is currently mounted, regardless of whether this daemon is the one that mounted it. Deliberately independent of the disk format job's own `state` (`GET /diskformat/{name}`, `"ready"` once a format+mount this daemon itself ran succeeds), which is purely in-memory, per-daemon-process state — forgotten across a restart even though the real mount persists, and blind to a disk mounted by hand or from before this mechanism existed. `GET /disks` is the one place to check whether a disk is *actually* mounted right now.

### Persisted disk roles

```
POST /v1/diskroles
{"disk_name": "sdb", "role": "backup"}
```

`role` is `"container-storage"` or `"backup"` — a small, fixed, closed vocabulary, not an arbitrary operator-chosen string the way a `devicemap` name is, since a role only means something insofar as a later phase (format/mount, container-storage migration) actually understands and acts on it. `"swap"` is deliberately not a role: this platform already has a dedicated, working, on-demand host swap *file* mechanism (`POST /system/swap`, ADR-0069) with no disk-level equivalent defined yet — adding a same-named disk role would either duplicate or need reconciling with it, a real design question with no answer, so it's left out rather than added as a role nothing can act on.

Real and creatable even for a `disk_name` that isn't currently present (`present: false` on `GET`, not an error — the same tolerant convention `/devicemaps` already established for hardware that might be temporarily absent). Always rejected (`400`) for the disk currently flagged `is_os_disk` on `GET /disks` — the fixed OS-disk layout is never a role-assignment candidate. `409` if `disk_name` already has a role (`DELETE` it first to reassign, the same no-silent-overwrite convention `/devicemaps` already established).

### Format + mount

```
POST /v1/disks/sdb/format
{"confirm_disk_name": "sdb", "fs_type": "btrfs"}
```

Multi-disk management Phase C: destructively formats and mounts a disk that already has an assigned role (`POST /diskroles` — a disk with no role is `400`, `"assign one via POST /v1/diskroles first"`). Deliberately a **separate, explicit** action from role assignment — assigning a role never has a destructive side effect of its own — confirmed with the operator during design rather than assumed. `confirm_disk_name` in the request body must match `disk_name` in the URL exactly (`400` otherwise): a deliberate double-confirmation before overwriting every byte of existing content on the disk. Always rejected for the OS disk, same as role assignment.

`fs_type` is optional (ADR-0104), `"ext4"` (the default, omit the field entirely for the original behavior) or `"btrfs"` — `400` for any other value. `"btrfs"` requires a real `mkfs.btrfs` to actually be staged on this box (`btrfs-progs.recipe`, via a real `kanxeo-hosttools` image); if it isn't, the job still starts but fails fast with `"mkfs.btrfs failed"` once the child process's own exec attempt hits `ENOENT` — the same failure shape any other `mkfs` failure already has, not a special case.

Async, like every other potentially-slow host operation this daemon runs (`pkg install`, ISO assembly, `pkg bootstrap --toolchain-url=`) — `POST` returns `202` immediately with the job's initial status; poll `GET` on the same path for completion:

```
GET /v1/disks/sdb/format
{"disk_name": "sdb", "state": "ready", "fs_type": "btrfs", "mount_path": "/var/lib/kanxeo/disks/sdb"}
```

`state` is `"none"` (no job has ever run for this disk — including when a job ran/is running for a *different* disk, so a status check never shows another disk's unrelated job), `"running"`, `"ready"`, or `"failed"` (`error` distinguishes `mkfs.<fs_type>` failing outright from it succeeding but the subsequent `mount(2)` failing). Only one format job may run daemon-wide at a time (`409` otherwise) — the same v1 single-job constraint every other async job here already has. Mounted at a fixed path under this platform's own data directory by default; a `container-storage`-role disk can also be selected explicitly per container via `POST /containers`' own `disk` field (ADR-0102, Phase D, already built).

## Per-container config files + sysctls

```
POST /v1/containers
{
  "name": "router2",
  "image": "router",
  "cmd": ["/bin/bash", "/usr/local/bin/pbr.sh"],
  "files": [{"path": "/etc/bird.conf", "content": "...", "mode": "0644"}],
  "sysctls": [{"key": "net.ipv4.conf.all.rp_filter", "value": "0"}]
}
```

- `files` is optional: 0–N `{path, content, mode}` entries, staged directly onto the container's own filesystem *before* its process ever `execve()`s — so `cmd` can point straight at a staged script (e.g. `pbr.sh` above). `path` must be absolute with no `.`/`..` component (`400` otherwise); `content` is bounded at 64KiB per file. `GET /containers/{name}/files?path=...` (above) is the read-path counterpart, for after the container is running.
- `sysctls` is optional: 0–N `{key, value}` entries; `key` must start with `net.` (the one sysctl subtree the kernel actually namespaces end to end — `400` for anything else, a real security boundary, not incidental). Applied inside the container's own netns right after `clone3()`, the same mechanism `ip_forward` already uses.
- Both survive exactly like everything else in `restart: "always"`'s own replay mechanism — no separate persistence work needed. See ADR-0030.
- `cmd` itself is echoed back on every `GET /containers`/`GET /containers/{name}` response (`ADR-0100`) — previously there was no way to ask a running or stopped container "what is your entrypoint," since the create request's own `argv` only ever pointed into that request's transient parsed body.

## Package manager: source-based, asynchronous installs

A package manager built from scratch: recipes are shell scripts (the same format Gentoo ebuilds/Arch PKGBUILDs/CRUX Pkgfiles use), builds happen inside this project's own container runtime, and the daemon **never sources or executes a recipe on the host** — recipe metadata (`pkg_name=`, `pkg_version=`, `pkg_source=`, `pkg_sha256=`, `pkg_depends=`) is read with a strict, non-executing line scanner; the recipe's real shell code (`pkg_build()`/`pkg_install()`) only ever runs inside the isolated, network-less build container. See [`docs/guides/writing-recipes.md`](../guides/writing-recipes.md) for the full recipe-authoring contract.

**Every install targets one image**, `/var/lib/kanxeo/images/{image}/rootfs` — `"base"` by default (any container created with `"image": "base"` gets everything installed there, no separate host/container package paths to keep in sync), or an explicit other one (see [Per-image installs](#per-image-installs) below) for software that shouldn't be part of every container's baseline.

**Installs are asynchronous.** The daemon is single-threaded and non-blocking; a network fetch or a real compile can take anywhere from seconds to minutes, so `POST /v1/pkg/install` returns immediately (`202`) and the actual work happens in the background — poll `GET /v1/pkg/{name}` for progress. **Fetching happens on the host** (a `curl` subprocess — this project's networking plane has no outbound NAT, so a build container has no network access at all, a stronger isolation boundary for untrusted build scripts, not a limitation worked around).

One-time setup, before installing anything:

```
POST /v1/pkg/bootstrap
```

Stages a real build toolchain (`gcc`/`make`/`ld`/`as`/`cc1`/`sh`/`tar` and their real headers/libraries) into the sandboxed build image. No body: copies live from this daemon's own host `/usr/{include,lib,lib64,bin,libexec}` with the real `cp -a` — works for dev/test convenience when `kanxeod` happens to be running somewhere with a real toolchain already, but produces an empty, non-functional toolchain on a real minimal install (nothing under its own `/usr` beyond `kanxeod`/`kanxeoctl` and their bare runtime libs). `{"toolchain_path": "/local/path/to/toolchain.squashfs"}` imports a real, portable artifact (built once, elsewhere, with `image/src/mktoolchainimage.c`) via a local path already `scp`'d onto this box. Both of those are synchronous (`204`), unchanged.

**`{"toolchain_url": "...", "toolchain_sha256": "..."}` is a third mode (ADR-0065): the daemon fetches the artifact itself, host-side** — the same real `curl` primitive every recipe's own `pkg_source` already uses, not a second fetch mechanism. Closes a real gap the other two modes both rest on: a genuinely fresh, minimal Kanxeo install has no SSH server and no general shell at all (ADR-0034), so "the operator transfers it onto the box" was never actually possible for a from-scratch install with nothing else already on the network to reach it via — confirmed the hard way (`nc -z <box> 22` closed) rather than assumed. Async (`202`, since a real network fetch of a real, large artifact can't block this daemon's single-threaded event loop) — poll `GET /v1/pkg/bootstrap` (`state`: `none`/`fetching`/`ready`/`failed`) the same shape `GET /system/iso` already established. `409` if a fetch is already in flight; `400` if `toolchain_url` is given without a valid 64-char `toolchain_sha256`. All three modes are idempotent — safe to call again.

**Recipes are managed live, via the API itself (ADR-0040), and are version-keyed and immutable once published (ADR-0107)** — `POST /v1/pkg/recipes` publishes a new `(name, version)`, no ISO rebuild or reinstall needed:

```
POST /v1/pkg/recipes
{"name": "hello", "content": "pkg_name=hello\npkg_version=2.12.1\n..."}
```

`content` is validated (must parse, and its own `pkg_name=`/`pkg_version=` must equal `name` and the version this call actually publishes) *before* anything on disk changes — `400` on a mismatch or a recipe that fails to parse. Unlike the old flat-file layout, publishing an already-existing `(name, version)` pair is `409 Conflict`, not a silent overwrite — fixing a mistake means bumping `pkg_version=` and publishing again. `204` on success. `GET /v1/pkg/recipes/{name}` returns one recipe version's full detail (including its raw `build.sh` text and a `created_at` timestamp, unlike the list view's metadata-only shape) — powers the web dashboard's per-package Recipe tab; an optional `?version=` selects a specific published version, omitted resolves to the highest available. `DELETE /v1/pkg/recipes/{name}` removes recipe version(s) — `?version=` removes just that one, leaving any other published versions of `name` intact; omitted removes every version. Either way it only affects future `pkg install`/`update-all` lookups, never anything already installed via it. `GET /v1/pkg/recipes` lists every published version this daemon currently knows about — a package name with multiple published versions appears as multiple separate entries, not merged. This project's own git-tracked `pkg/recipes/<name>/<version>/build.sh` files (`bash`, `bird`, `iproute2`, etc.) are the *source* for a fresh deployment's initial catalog, uploaded through this same endpoint — never baked into the installer ISO or read directly off some fixed on-disk path by the daemon itself.

**A host can also stay current with a shared recipe repository instead of every recipe needing an individual manual push (ADR-0121)** — `PUT /v1/pkg/repo-config` points a host at one:

```
PUT /v1/pkg/repo-config
{"repo_url": "https://git.example.internal/team/recipes", "repo_kind": "gitea", "ref": "master"}
```

`PUT` is a **partial update** — any field left out of the body keeps its existing value; an explicit `"auth_token": ""` is the one way to clear an already-set token, and the token itself is never echoed back by either `GET` or `PUT`, only a derived `auth_token_set` boolean. `repo_kind` is one of `gitea`/`github`/`gitlab` — each forge has a genuinely different archive-download URL shape and auth convention, handled as three separate, explicit branches rather than one generic abstraction (only the `gitea` branch has been verified against a real forge from this project's own dev environment; `github`/`gitlab` follow each forge's own documented API but are unverified against a live account of either kind). `POST /v1/pkg/sync` starts an async fetch of the configured repo's recipe tree and merges it into this host's own catalog — **merge/additive, never a mirror**: an already-known `(name, version)` is counted as `skipped`, never overwritten (recipe versions stay immutable, ADR-0107), so a sync can never discard a recipe a host already has. `202`, poll `GET /v1/pkg/sync` (`state`: `never`/`running`/`success`/`failed`, plus `added`/`skipped` counts and an `error` string) for the outcome — `400` if no repo is configured yet, `409` if a sync is already running. `sync_interval_seconds` in the repo config (0 = disabled, the default) optionally re-arms an automatic periodic sync on top of the always-available manual `POST`.

**Installs are also backed by a local build-artifact cache and, optionally, a plain-HTTP precompiled-artifact server (ADR-0122)** — deliberately two separate, independently-configured things from the git-forge recipe repo above: recipes are small, versioned text that belongs in git; a compiled package artifact does not, and reusing the same git-forge fetch mechanism for binaries would mean checking them into that same git tree to make them fetchable. Every successful real build is cached locally (`<name>-<version>.tar.gz`, keyed exactly like the recipe itself); installing the identical package into a second image, or reinstalling after deletion, hits the cache instead of re-fetching and recompiling — `GET`/`DELETE /v1/pkg/cache` show/clear what's cached, `GET`/`PUT /v1/pkg/cache-config` show/set its size cap (always a real, enforced cap — least-recently-used entries are evicted first when a new one needs room). A recipe can additionally opt in to network-fetched precompiled artifacts by declaring a `pkg_artifact_sha256=` field alongside its usual `pkg_source=`/`pkg_sha256=` — when a plain-HTTP artifact server is configured (`GET`/`PUT /v1/pkg/artifact-config`, a bare `base_url` + optional bearer token, never a git API), an install first tries `<base_url>/<name>-<version>.tar.gz` and verifies it against the recipe's own declared checksum before ever trusting it; only on a miss (no server configured, 404, checksum mismatch) does it fall back to the recipe's real source and a real compile, exactly as if the artifact tier didn't exist. A recipe that never sets `pkg_artifact_sha256=` never touches this tier at all.

**An image's own whole package list can be declared as one recipe, not N individual manifest edits (ADR-0123)** — `POST /v1/images/recipes` with `{"name": "<image name>", "content": "image_packages=\"bird:pinned:2.19.1 keepalived:rolling:2.3.4\"\n"}` (a recipe's own name is always its target image's name, a 1:1 relationship). `POST /v1/images/{name}/apply-recipe` realizes it: the common case bulk-declares the manifest synchronously (`204`) — exactly what N manual `POST /images/{name}/manifest` calls already do, no rootfs touched, packages still need real `POST /pkg/install` calls afterward to actually build. A recipe that's entirely `pinned` **and** declares an `image_artifact_sha256=` **and** has a matching, configured artifact server instead gets a fast path: an async fetch (`202`, poll `GET /v1/images/recipe-apply-status`) of one whole-rootfs tarball at `<pkg/artifact-config base_url>/images/<name>-<hash>.tar.gz` (the same server Part 3 already configures, under a distinct URL prefix so package and image artifacts never collide), verified against the recipe's own checksum before being extracted directly as the new version — skipping every per-package build entirely. Any miss falls through cleanly; the image is never half-applied.

**A running container can opt in to auto-following its own rolling image's version drift (ADR-0124)** — set `"follow_rolling": true` on `POST /v1/containers` (requires `restart` other than `"no"`; silently ignored otherwise, same as `restart_delay_seconds`). Whenever the pinned image's `current_version` moves — a rolling auto-rebuild, a manual `pkg install`, or a Part 4 artifact-tier apply, checked after every one of those job completions — the container's own persisted `image_version` pin is patched to match and, if it's currently running, it's live-restarted onto the new version after an independent, uniformly-random delay so many containers following the same image don't all restart in the same instant. That delay window is a daemon-wide default, `GET`/`PUT /v1/system/rolling-config` (`jitter_window_seconds`, default 60, range 0-3600 — `0` disables jitter and restarts immediately) — a single container can override it for itself with `"follow_rolling_jitter_seconds": N` (same 0-3600 range) on its own `POST /v1/containers`, without changing the default every other `follow_rolling` container still uses. A container that never sets `follow_rolling` behaves exactly as before: its pin never moves on its own.

Install it:

```
POST /v1/pkg/install
{"name": "hello"}
```

Response (`202`):

```json
{"name": "hello", "version": "2.12.1", "state": "fetching", "error": null, "files": []}
```

Poll `GET /v1/pkg/hello` until `state` leaves `fetching`/`building`:

```json
{"name": "hello", "version": "2.12.1", "state": "installed", "error": null, "files": ["usr/bin/hello", "..."]}
```

`state: "failed"` populates `error` (checksum mismatch, build failure, etc.) — the package stays visible via `GET` so the failure is diagnosable, not silently dropped. `DELETE /v1/pkg/hello` unlinks every file in its manifest from the base image, not just the registry entry.

v1 serializes installs — only one may be in flight at a time (`POST /v1/pkg/install` for a second package while another is still `fetching`/`building` is a `409`). Hostbuild jobs (below) share this exact same job slot.

### Dependencies

A recipe's `pkg_depends` (space-separated names) is resolved automatically and recursively. Given a `top` recipe with `pkg_depends="leaf"`:

```
POST /v1/pkg/install
{"name": "top"}
```

Installs `leaf` first (skipped entirely if already installed), then `top` — one `POST`, both packages end up `installed`, visible individually via `GET /v1/pkg`. **The `202` response describes whichever package actually started fetching first** — here, `leaf`, not `top`, since `top` can't start until its dependency is done. Poll by name (`GET /v1/pkg/leaf`, then `GET /v1/pkg/top`) to follow the whole chain. A dependency with no matching recipe, or a circular dependency (`A` needs `B` needs `A`), is a `400` — nothing is fetched. A diamond (`top` needs both `mid1` and `mid2`, both need `leaf`) installs `leaf` exactly once, not twice.

### Upgrades

Re-`POST`ing an already-installed package is always `409`, even after its recipe's `pkg_version` has changed on disk — explicit intent is required:

```
POST /v1/pkg/install
{"name": "leaf"}
```
→ `409` (still installed at the old version; add `"upgrade": true` to proceed).

```
POST /v1/pkg/install
{"name": "leaf", "upgrade": true}
```
→ `202` if the recipe's version genuinely differs from what's installed (still `409`, "nothing to do," if it doesn't). The old version's manifested files are removed only once the new version's build actually succeeds — a failed upgrade attempt leaves the working old install untouched, not half-removed.

`GET /v1/pkg/{name}` shows `"available_version"` (`null`, or the recipe's current version) for any installed package whose recipe has since changed — the concrete "is this out of date" answer, checked live against the recipe on disk every time, not cached.

### Pinning a specific recipe version

A bare install/hostbuild always resolves `name` to its highest published recipe version. An explicit `"version"` field pins it to exactly that one instead (ADR-0107):

```
POST /v1/pkg/install
{"name": "curl", "version": "8.20.0"}
```

404/400 if no such `(name, version)` recipe is published. This pin applies only to the single package actually being installed — every dependency it pulls in via `pkg_depends` still always resolves to *its own* highest available version, regardless of what the top-level target is pinned to.

### Per-image installs

Install into something other than the default `base` image with `"image"`:

```
POST /v1/pkg/install
{"name": "bird", "image": "router"}
```

`bird` (and its dependencies, resolved the same way as always) builds into `/var/lib/kanxeo/images/router/rootfs` — containers created with `"image": "base"` never see it. The same package name is tracked independently per image: `bash` installed into both `base` and `router` are two separate entries, each independently upgradable/removable. `router` above doesn't need to exist beforehand — the first install into a name never seen before creates it implicitly; `POST /v1/images {"name": "router"}` creates one explicitly instead, useful when you want an image to exist (and be immediately usable — its C runtime is seeded right away) before installing anything into it.

**An image can also carry a real, persisted manifest** (ADR-0107) — declared package intent, distinct from whatever's actually installed right now:

```
POST /v1/images/router/manifest
{"package": "bird", "mode": "pinned", "version": "2.19.1"}
```

`mode: "pinned"` means exactly that version, never auto-advancing; `mode: "rolling"` means `version` is a floor, resolving to the highest available recipe version `>=` it. Re-`POST`ing the same `package` updates its mode/version in place (upsert), never duplicates. `GET /v1/images/router` echoes the full manifest alongside the bare `name` it always returned. `DELETE /v1/images/router/manifest/bird` removes one entry. This only records intent — it does not itself install anything.

**Every install/upgrade/uninstall against an image produces a new, immutable version** (ADR-0108) — `/var/lib/kanxeo/images/router/<version>/rootfs`, where `<version>` is a hash of the image's full installed-package manifest (`name@version` pairs, sorted). Prior versions are never mutated or deleted; a version whose content hash already exists in the image's history is deduplicated (no new directory, `current_version` just repoints). New containers created against `router` are pinned to whatever `current_version` resolves to at creation time (`registry.json`'s own `image_version` field) — they keep running against that exact rootfs even if `router` moves on to a newer version later; only a fresh create or explicit restart-with-replay re-resolves. `GET /v1/images/router` reports both:

```json
{
  "name": "router",
  "manifest": [{"package": "bird", "mode": "pinned", "version": "2.19.1"}],
  "current_version": "3f9c2a...",
  "versions": [
    {"version": "3f9c2a...", "created_at": 1786292454},
    {"version": "a01de8...", "created_at": 1786290011}
  ]
}
```

`versions` is newest-first. `current_version` is `""` for an image that has never had a package installed/upgraded/removed against it (no version produced yet).

**A `rolling`-mode manifest entry auto-rebuilds** the moment a matching recipe with a higher version is published via `POST /v1/pkg/recipes` — no manual re-install needed. The daemon re-derives what's satisfied on every attempt (pinned: exact version match; rolling: currently-installed version must equal the current highest recipe version `>=` the manifest's floor), queues at most one rebuild per image at a time, and re-checks the queue as each job completes.

`GET`/`DELETE` on a non-default image use the compound `{name}@{image}` path form:

```
GET /v1/pkg/bird@router
DELETE /v1/pkg/bird@router
```

A bare `GET /v1/pkg/bird` still means `bird@base`. `GET /v1/pkg` (the list) includes every `(name, image)` entry, each with its own `"image"` field. Note this compound form applies to `/pkg/{name}` only, not `/pkg/recipes/{name}` — a recipe isn't tracked per-image, so its own name parameter never accepts an `@` suffix.

Note: the C runtime for dynamically-linked binaries (`ld.so`/`libc.so.6`/`libtinfo.so.6`) is seeded automatically into whichever image a package lands in, `base` or otherwise (ADR-0019 for `base` at install time, ADR-0023 generalizes it to every image at first install).

### Hostbuild: standalone artifacts instead of merging into an image

Most installs merge their build output into a target image's rootfs. A hostbuild job is the second mode of the exact same pipeline: it harvests the output as a standalone artifact on the host instead — used to build the Linux kernel `kanxeod` boots, and to self-build `kanxeod`/`kanxeoctl`/`web` themselves from a running Kanxeo host (see [`docs/guides/kernel-build-and-ab-updates.md`](../guides/kernel-build-and-ab-updates.md) and [`docs/guides/building-kanxeo.md`](../guides/building-kanxeo.md) for the full operator runbooks).

```
POST /v1/pkg/hostbuild
{"name": "kernel", "build_image": "kanxeo-builder"}
```

`build_image` is always explicit (no default), and an optional `"version"` field pins the hostbuild to a specific published recipe version exactly like `/pkg/install`'s own (omitted resolves to the highest available) — the already-existing image whose rootfs supplies the build container's own toolchain (must already have whatever the recipe's `pkg_build()` needs actually installed, via ordinary `pkg install` first; a hostbuild recipe cannot itself declare `pkg_depends`, since dependency resolution has no meaning for a one-shot harvest). `202`, polled via `GET /pkg/hostbuild/{name}` (a thin wrapper over the same `GET /pkg/{name}` lookup, scoped to a reserved internal image name) exactly like an ordinary install. Once `state: "installed"`, the artifact lives on the host at a fixed, well-known path per recipe (`kernel.recipe` → a `bzImage`; `kanxeo.recipe` → `kanxeod`/`kanxeoctl`/`web/`/`kanxeo-install`/`mkinstalleriso` plus a server-side-assembled `kanxeod-root.squashfs`; `isotools.recipe` → a self-contained `grub-mkrescue`/`sbsign`/`sbverify`/`xorriso`/`mformat`/`mcopy` toolchain) — never merged into any container image's rootfs. `PkgEntry`'s own `files[]` stays empty for a hostbuild entry always, by design (nothing to `pkg_delete()` for a plain host artifact) — `artifact_path`'s own directory listing is the real answer to what a hostbuild produced. A hostbuild already in `state: "installed"` is a bare 409 on a repeat call unless `"upgrade": true` is given and the recipe's own `pkg_version=` has actually moved on (ADR-0094, mirrors `/pkg/install`'s own `upgrade` field exactly). `kanxeoctl pkg hostbuild <name> --build-image=<image> [--wait] [--deploy] [--upgrade]` is the CLI surface; `--deploy` reads the finished artifact and calls the existing, unmodified `/system/update` for you.

### Building a fresh installer ISO server-side

`POST /system/iso` closes the one gap the hostbuild mechanism above deliberately left open: assembling those artifacts into a bootable, Secure-Boot-signed installer `.iso` used to be a dev-machine-only tool (`image/src/mkinstalleriso.c`) an operator had to run by hand. It's now a real daemon capability, non-blocking and pidfd-tracked exactly like `POST /pkg/hostbuild`'s own async jobs:

```
POST /v1/system/iso
{"disk": "/dev/sda", "ip": "10.0.0.5", "prefix": "24", "gateway": "10.0.0.1", "interface": "eth0"}
```

Every field is optional — an empty body reproduces the tool's original default, a generic ISO with its kernel arguments left as the `CHANGEME` placeholder an operator edits at the GRUB boot menu. `202`, polled via `GET /system/iso` (`state`: `none`/`building`/`ready`/`failed`, `iso_path` once ready). Reuses whatever the most recent `kanxeo`/`kernel`/`isotools` hostbuild rounds already harvested — it does not trigger any of them itself, and fails fast (`400`) naming exactly which one is missing rather than a background failure the caller has to poll for to discover. Requires a real Secure Boot signing key pair, staged out of band by the operator at `<data-dir>/keys/kanxeo-signing.{key,crt,cer}` — deliberately never generated, fetched, or copied there by `kanxeod` itself (see ADR-0064: a release-signing private key must never propagate onto every deployed box, only whichever specific instance is actually cutting installer media). `kanxeoctl iso build [--disk=... --ip=... --prefix=... --gateway=... --interface=...] [--wait]` / `kanxeoctl iso status` is the CLI surface.

## Liveness vs. boot identity (ADR-0077)

```
GET /v1/health
{"status": "ok"}

GET /v1/system/boot
{"build_version": "v1.6.0-9-gc59e482-dirty", "build_time": "2026-08-08T00:52:00Z", "slot": "b", "kernel_version": "6.18.40", "bootroot_assembly_started_generation": 3, "bootroot_assembly_completed_generation": 3, "bootroot_assembly_running": false}
```

`GET /health` is deliberately minimal -- both `kanxeoctl` and the web dashboard poll it every few seconds purely for a status dot, and it's excluded from the audit trail (see [A consolidated log](#a-consolidated-log) above) as low-value polling noise. Build/slot/kernel identity is a separate, lower-frequency check: `GET /system/boot` reports `build_version` (`git describe --tags --always --dirty` at build time), `build_time`, `slot` (`"a"`/`"b"`, or `null` for a dev/test daemon started without `--slot=`), and `kernel_version` (the running `uname(2)` release string). This is the deploy/reboot verification signal referenced throughout [`docs/guides/kernel-build-and-ab-updates.md`](../guides/kernel-build-and-ab-updates.md) -- a `200` from `health` alone only proves *some* daemon answered, not that it's the one you just wrote; `slot`/`kernel_version` from `boot` are the direct answer to "did I actually boot into what I just wrote."

`bootroot_assembly_started_generation`/`bootroot_assembly_completed_generation`/`bootroot_assembly_running` (ADR-0105) are a real freshness signal for `pkg hostbuild kanxeo --deploy`'s own server-side follow-on assembly (ADR-0057) — `kanxeod-root.squashfs` existing at the hostbuild's `artifact_path` is not the same as it being *this* round's own fresh build, since that file is a leftover from whichever assembly last succeeded. `completed_generation` only ever advances on a real, confirmed success; `--deploy` captures it as a baseline before triggering anything and waits for it to advance past that baseline (`running` distinguishes "still working" from "gave up, that attempt failed") rather than trusting file-exists.

## Host + package updates

```
POST /v1/system/update
{"image_path": "/var/tmp/new-root.squashfs", "kernel_path": "/var/tmp/new-bzImage"}
```

- `image_path`/`kernel_path` are local paths the operator has already transferred onto the box (e.g. `scp`, or a hostbuild artifact already sitting on this same host — see [Hostbuild](#hostbuild-standalone-artifacts-instead-of-merging-into-an-image) above) — there is no upload endpoint; see ADR-0031 for why. Both are optional, but at least one is required — update just the root, just the kernel, or both together in one call. Writes whatever's given onto this daemon's own **inactive** A/B slot (the other one from whichever it's currently running as — `--slot=a` or `--slot=b`) and stages a fresh systemd-boot loader entry with a fresh boot-counter. The kernel is per-slot too (`kanxeo-bzImage-a`/`kanxeo-bzImage-b` on the ESP, both pre-staged identically at install time, ADR-0032). Omitting one of `image_path`/`kernel_path` no longer leaves the inactive slot's own existing copy stale (ADR-0095) — the omitted half is auto-filled from the **active** slot's own currently-running copy instead, so a root-only update still pairs with a kernel that's known-good (already booted), never a leftover from an earlier cycle; the response's own `updated` array is always `["root", "kernel"]` for exactly this reason — both genuinely are fresh in the inactive slot after the call. `400` if this daemon has no `--slot=` (not a real installed system), neither path is given, either path doesn't exist/isn't readable, or either file fails its own on-disk magic check (squashfs's `"hsqs"`, or a bzImage's boot-sector/`setup_header` magic) — checked for both before either is written, so a bad `kernel_path` never leaves a good `image_path` half-applied.
- Response includes `"updated"`, always `["root", "kernel"]` (see above).
- Deliberately does **not** reboot — call `POST /system/reboot` separately once ready to cut over; the existing boot-counter/`confirm_boot()` machinery, entirely unchanged, decides whether the fresh slot sticks.

See [`docs/guides/kernel-build-and-ab-updates.md`](../guides/kernel-build-and-ab-updates.md) for the full build → write → reboot → confirm runbook, and [`docs/guides/staying-updated.md`](../guides/staying-updated.md) for the day-to-day operational picture (this endpoint plus package updates below, together).

```
POST /v1/pkg/update-all
```

- Finds the first installed package (across every image) whose recipe's `pkg_version=` has drifted and starts an upgrade for it, reusing `POST /pkg/install {"upgrade": true}`'s entire existing mechanism — `202` with the started package's state, or `200 {"status": "nothing to update"}` if everything's already current. Starts at most one job at a time (the same v1 single-install-in-flight constraint every other install path has, honestly respected rather than worked around); call again once that job finishes to drain the whole backlog.

## This install's identity (site config)

```
GET /v1/system/site
```

A real, operator-configurable `instance_name`/`site_name`/`domain_suffix` triple (ADR-0046). `instance_name` labels this specific install (dashboard header, CLI, backup bundle) and is always non-empty (defaults to `"kanxeo"`); `site_name`/`domain_suffix` are client tooling's own suggested-FQDN pair (`<name>.<site_name>.<domain_suffix>`, or `<name>.<domain_suffix>` when `site_name` is empty) offered by default when creating a DNS record or issuing a PKI cert with a bare (dot-free) name — a convenience only, never enforced: DNS records and PKI SANs remain plain operator-supplied strings, unaffected by this endpoint's own value once explicitly given with a `.`. Always `200`s — defaults (`"kanxeo"`/`""`/`"internal"`) apply until the first `PUT`.

```
PUT /v1/system/site
{"instance_name": "kanxeo1", "site_name": "lab1", "domain_suffix": "internal"}
```

`instance_name`/`domain_suffix` are required; `site_name` is optional (omitting it entirely is equivalent to `""`, meaning no site tier — a single-site deployment). On success, also best-effort reissues this install's own `"host"` PKI leaf if a root CA is already bootstrapped, and reconciles a single auto-maintained DNS record for the same FQDN if this daemon was started with a real, specific `--bind=` address (not `"0.0.0.0"`/`"127.0.0.1"`, neither of which has one single correct address to publish) — both never fail this request even if they themselves fail. The DNS record is also reconciled once at every daemon startup, so it exists without needing a `PUT` after every restart.

## Backup and restore

```
GET /v1/system/backup
```

Bundles platform *configuration* state — container definitions, networks, DNS records, package install state and recipes, and site config — as one response. **Read this carefully before relying on it for disaster recovery:**

- **Does NOT include workload data.** Each container's own persistent data (a git host's repos, a resolver's zone files, a metrics database) is that container's own concern, backed up with its own native tooling. This endpoint has no way to reach into another container's filesystem and never tries to.
- **Does NOT include image rootfs content.** Since everything is compiled from source, an image's content is reproducible by re-running `pkg install` for whatever `pkg_installed` records — this bundle is the "shopping list" (what should be installed, where), not the built bytes. Getting all the way back to a fully-populated system after a restore means re-running those installs, not something this endpoint does for you automatically.
- **Never touches PKI, at all.** The CA private key (and every issued leaf certificate's own key) is never returned over the API anywhere in this system, by existing, deliberate design (see [PKI](#pki-a-ca-chain-and-issued-leaf-certificates) above) — that rule isn't bent or partially relaxed here. Back up `/var/lib/kanxeo/pki/` separately, directly on the host, outside the API entirely.

```
POST /v1/system/restore
{"container_defs": "...", "networks": "...", "dns_records": "...", "pkg_installed": "...", "pkg_recipes": {"hello": "..."}, "site_config": "..."}
```

The reverse of `GET /system/backup` — same shape, every field optional and independent (at least one required), so you can restore just container definitions, just networks, or the whole bundle. Every field is validated (must itself parse as JSON, or for `pkg_recipes`, must be an object of strings) *before* anything is written, so one bad field can't leave the others half-applied — but a real disk-write failure partway through (checked separately, after validation) can: fields already written before a failing one are not rolled back.

**Does not reboot or take effect immediately.** Restored files only get picked up on the next boot — the same startup sequence (including container autostart) that already runs every time. Call the existing `POST /system/reboot` once you're ready to actually cut over. A typical disaster-recovery sequence: boot a fresh install once (normal empty first boot) → `POST /system/restore` with your saved bundle → `POST /system/reboot` → the second boot comes up with your restored state.

`kanxeoctl backup --output=PATH` saves the bundle verbatim (byte-for-byte, not re-serialized) for later use with `kanxeoctl restore --input=PATH` — the same file round-trips exactly. A scheduled backup job (a container with network reachability to `kanxeod`, or a simple host-level cron entry — either is equally valid, this is a plain REST client either way) can run `kanxeoctl backup` on a schedule and ship the result off-host.

## Current scope boundaries (v1, deliberate — see ADR-0007)

- No image build/pull endpoint yet — images are provisioned onto disk out of band.
- No log retrieval endpoint yet — the daemon doesn't capture container stdout/stderr separately.
- No authentication yet — the daemon binds to loopback only as its safety boundary for now.
- HTTP: no keep-alive/pipelining (`Connection: close` on every response), no chunked bodies.
- Routes are set-once at creation and not echoed back or introspectable afterward; modifying them on a running container would need a new "enter another netns from outside" primitive, not built yet. See `docs/roadmap/ROADMAP.md`.
- DNS and LDAP server bindings are both persisted (ADR-0091 fixed this for DNS; LDAP's own binding table, task #725, was built with persistence from the start). DNS: only one hosts-format record type; no CNAME/MX/TXT/etc.
- PKI: no certificate revocation/CRL, no CSR-submission flow (the daemon always generates both the keypair and the cert itself). CA regeneration/rotation **is** built (`POST /pki/reset`, above) — that gap has closed since this list was first written.
- Package manager: only one install/hostbuild in flight at a time (dependency chains, and `POST /pkg/update-all`'s own successive calls, still serialize through that same single slot — see [Host + package updates](#host--package-updates) above); no version-constrained dependencies (any installed version satisfies a dependency); symlinks in a package's own `DESTDIR` output are skipped (regular files and directories only).
- No scheduled/periodic trigger for `POST /system/update` or `POST /pkg/update-all` — both are on-demand, operator- or cron-invoked; no automatic "update then reboot" chaining.
- No volume/bind-mount concept beyond small, content-inlined `files` (see [Per-container config files + sysctls](#per-container-config-files--sysctls) above) — a large binary asset or directory tree has no home in this model yet.

## Why this file exists alongside `openapi.yaml`

One Source of Truth means the *schema* lives in exactly one place (`openapi.yaml`). This page exists only so a human (or a future CLI/web implementer) can get oriented quickly without parsing YAML first — if the two ever disagree, `openapi.yaml` wins and this page is out of date and should be fixed. Per `CLAUDE.md`'s Documentation Map, this file is updated in the same change as any `openapi.yaml` edit, never after.
