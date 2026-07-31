# Kanxeo Host API

[`openapi.yaml`](openapi.yaml) (OpenAPI 3.0) is the **authoritative** contract — every field, schema, and status code is defined there, not here. This page is a human-friendly index into it, per the project's API-First Mandate: the REST daemon (`daemon/`, binary `kanxeod`) is the only process with direct access to the container runtime, and everything else (CLI, web dashboard) is built by reading this contract, never the daemon's source.

Default base URL: `http://127.0.0.1:7620/v1` (loopback-only by default; see `daemon/src/main.c`'s `--bind`/`--port` flags).

## Endpoints at a glance

| Method | Path | Purpose |
|---|---|---|
| GET | `/health` | Liveness check |
| POST | `/system/shutdown` | Stop `kanxeod`; powers off the host too when running as real PID 1 |
| POST | `/system/reboot` | Stop `kanxeod`; restarts the host too when running as real PID 1 |
| POST | `/system/update` | Write a fresh OS image onto this daemon's own inactive A/B slot |
| GET | `/containers` | List all containers this daemon knows about |
| POST | `/containers` | Create and start a container |
| GET | `/containers/{name}` | Inspect one container |
| DELETE | `/containers/{name}` | Stop (if running), remove it, and forget any persisted definition |
| POST | `/containers/{name}/stop` | Kill it now, keep its persisted definition (for `restart: "unless-stopped"`) |
| GET | `/networks` | List all networks this daemon knows about |
| POST | `/networks` | Create a network (a real bridge, persisted across restarts) |
| GET | `/networks/{name}` | Inspect one network |
| DELETE | `/networks/{name}` | Remove a network (refused if any container is still attached) |
| GET | `/devices` | List host PCI/USB/net devices discoverable via sysfs, available for passthrough |
| GET | `/images` | List every image this daemon knows about |
| POST | `/images` | Create an empty image (runtime pre-seeded, ready for `pkg install`) |
| GET | `/images/{name}` | Inspect one image |
| DELETE | `/images/{name}` | Remove an image (refused for `base`, if in use, or if it still has packages) |
| GET | `/dns/records` | List all DNS records this daemon knows about |
| POST | `/dns/records` | Create a DNS record (name -> IP, persisted across restarts) |
| GET | `/dns/records/{name}` | Inspect one DNS record |
| DELETE | `/dns/records/{name}` | Remove a DNS record |
| GET | `/dns/servers` | List all registered DNS server bindings |
| POST | `/dns/servers` | Register a running container as a DNS-serving target |
| DELETE | `/dns/servers/{container}` | Unregister a DNS server binding |
| GET | `/pki/ca` | Inspect the root CA (never includes the private key) |
| POST | `/pki/ca` | Bootstrap the root CA (once; no regeneration in v1) |
| GET | `/pki/certs` | List all issued leaf certificates (metadata only) |
| POST | `/pki/certs` | Issue a leaf certificate signed by the root CA |
| GET | `/pki/certs/{name}` | Inspect one issued certificate (metadata + cert, never the key) |
| DELETE | `/pki/certs/{name}` | Remove an issued certificate |
| POST | `/pkg/bootstrap` | Stage the sandboxed build toolchain image (once; idempotent) |
| GET | `/pkg/recipes` | List recipes found on disk (provisioned out of band) |
| POST | `/pkg/install` | Start installing a package (async -- returns immediately) |
| POST | `/pkg/update-all` | Start an upgrade for the first installed package whose recipe has drifted |
| GET | `/pkg` | List every known package (installed or in-flight) with its state |
| GET | `/pkg/{name}` | Inspect one package's current state |
| DELETE | `/pkg/{name}` | Uninstall a package |

Every error response is `{"error": "message"}` with an appropriate 4xx/5xx status.

## Creating a network

```
POST /v1/networks
{"name": "internal", "subnet": "172.31.0.0", "prefix_len": 24}
```

- `name` must match `[A-Za-z0-9_-]{1,15}` — it's used verbatim as the Linux bridge interface's name (IFNAMSIZ is 15 chars).
- `subnet` must be the exact network address for `prefix_len` (host bits zero) — `"172.31.0.5"` with `prefix_len: 24` is rejected, only `"172.31.0.0"` is valid. It must also not overlap any existing network's range.
- `prefix_len` must be in `[8, 30]`.
- The gateway is always the subnet's first host address (`.1`) — there's no separate gateway field.

Response (`201`):

```json
{"name": "internal", "subnet": "172.31.0.0", "prefix_len": 24, "gateway": "172.31.0.1"}
```

Creating a network creates its bridge immediately via rtnetlink and persists the definition to `/var/lib/kanxeo/networks.json` — unlike containers (safe to be in-memory-only, since they die with the daemon), a bridge outlives this process, so the daemon reloads and recreates every persisted network's bridge idempotently at startup.

## Creating a container

```
POST /v1/containers
{
  "name": "my-container",
  "image": "test",
  "cmd": ["/bin/some-binary", "arg1"],
  "memory_max": 67108864,
  "pids_max": 32,
  "networks": ["internal", "dmz"]
}
```

- `name` must match `[A-Za-z0-9_-]+` — it's used verbatim as the on-disk directory name under `/var/lib/kanxeo/containers/`.
- `image` must already exist and be populated at `/var/lib/kanxeo/images/{image}/rootfs` — the daemon never creates image content itself (see ADR-0004); a missing image is a `400`, not a silently-empty container.
- `memory_max`/`pids_max` are optional cgroup v2 limits; omit for no limit.
- `networks` is optional: 1–64 entries, each either a bare name (auto-allocated IP) or `{"name": "internal", "ip": "172.31.0.50"}` for an explicit, operator-chosen address — each network must already exist via `POST /v1/networks` (`400` if unknown), and an explicit `ip` must be a usable address on that network: in its subnet, not the reserved gateway/network address, and not already taken (`400`/`409`). Omit `networks` entirely for no networking (isolated netns, only `lo` — same as before this field existed). The **first** entry is primary and gets the default route; the rest only get their own subnet's connected route.
- `dns_register` is optional, default `false` — see [DNS: records + a real dnsmasq container](#dns-records--a-real-dnsmasq-container) below. Requires `networks` to be set (`400` otherwise).
- `pki_issue`/`pki_cert_dir`/`pki_days` are optional, default `false`/`/etc/kanxeo-tls`/`365` — see [PKI: a root CA and issued leaf certificates](#pki-a-root-ca-and-issued-leaf-certificates) below. Requires the CA to already be bootstrapped (`400` otherwise); does **not** require `networks`.

Response (`201`):

```json
{
  "name": "my-container",
  "status": "running",
  "pid": 12345,
  "exit_status": null,
  "networks": [
    {"name": "internal", "ip": "172.31.0.2"},
    {"name": "dmz", "ip": "172.32.0.2"}
  ],
  "ip_forward": false
}
```

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

`POST /v1/containers/{name}/stop` kills it now but keeps its persisted definition — the container comes back on the next daemon restart for `"always"`/`"on-failure"` (a fresh chance every boot), but stays down for `"unless-stopped"` until explicitly re-`POST`ed. `DELETE /v1/containers/{name}` always means gone for good regardless of policy — it removes the persisted definition too, in the same call, and it won't come back on a pending crash-restart or any future boot.

`GET`/inspect responses always report the current `restart`/`restart_delay_seconds`/`stopped`/`depends_on`/`readiness` state, read live from the persisted definition rather than a stale echo of what creation was originally given.

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

## DNS: records + a real dnsmasq container

DNS records are a REST resource; the actual name resolution is done by a real DNS server (dnsmasq recommended) running as a normal containerized workload — not hand-rolled, the same reasoning BIRD wasn't hand-rolled for routing (ADR-0007's "no external libraries" rule is about this project's own platform components, not about workloads a container runs).

```
POST /v1/dns/records
{"name": "db.internal", "ip": "172.31.0.5"}
```

- `name` is a hostname (dot-separated labels, `[A-Za-z0-9-]`, RFC 1035 length limits) — a different charset from network/container names, which don't allow dots.
- `ip` must be well-formed IPv4.

Once a container running dnsmasq exists (e.g. `cmd: ["/usr/sbin/dnsmasq", "-k", "-u", "root", "-p", "53", "-H", "/etc/dnsmasq-hosts", "-R", "-h"]` — `-u root` since a minimal container image typically has no `/etc/passwd` for dnsmasq's default privilege drop to resolve; `-R`/`-h` skip `/etc/resolv.conf`/`/etc/hosts`, which likely don't exist either), register it:

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

## PKI: a root CA and issued leaf certificates

A single internal root CA plus leaf certificate issuance. Actual cryptography (keypair generation, CSR signing) is done by the daemon shelling out to the system's real, unmodified `openssl` binary as a short-lived subprocess — the same "real software, not hand-rolled" reasoning BIRD and dnsmasq were chosen under (ADR-0007's "no external libraries" rule governs this project's own platform components, not real software it invokes or runs as a workload).

Bootstrap the CA once:

```
POST /v1/pki/ca
{"common_name": "Kanxeo Root CA", "days": 3650}
```

Both fields are optional (shown defaults). **The CA private key is never returned over the API, in any endpoint, ever** — it's the root of trust and must never leave the host. A second `POST /v1/pki/ca` is a `409`; there's no CA regeneration in v1.

Issue a leaf certificate:

```
POST /v1/pki/certs
{"name": "svc.internal", "sans": ["svc.internal", "svc"], "days": 365}
```

- `name` is a hostname (same RFC 1035 rules as `DnsRecord.name`) — it becomes the certificate's CN and this endpoint's REST identifier.
- `sans` is optional and defaults to `[name]` — a cert always carries at least its own name as a Subject Alternative Name.
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

`pki_cert_dir` and `pki_days` are optional (shown defaults). This issues a cert named `web` (CN and sole SAN) and writes `tls.crt`/`tls.key` (chmod 0600) into `/etc/kanxeo-tls` **inside the `web` container's own filesystem** — the same `/proc/<pid>/root/<path>` mechanism `POST /v1/dns/servers` already uses to reach into a running container (ADR-0013), just delivering a cert+key instead of a hosts file. Unlike DNS server bindings, delivery is **one-time**: there's no live resync, since a cert doesn't change after a container starts. `GET /v1/pki/certs/web` shows `"owner": "web"`; deleting the `web` container automatically removes its cert (both the index entry and the on-disk key/cert files) — a manually-created cert is never touched by any container's deletion, even if it happens to share that container's name but wasn't the one that created it.

Unlike `dns_register`, `pki_issue` does **not** require `networks` — the cert identifies the container by name, not by IP, and delivery works for any running container regardless of networking. It **does** require the CA to already be bootstrapped, checked upfront as a `400` (you can't issue a cert with no CA). A *name collision* discovered only at issuance time (e.g. a stale cert persisted from a same-named container created before a daemon restart) is best-effort instead: issuance is silently skipped rather than overwriting it, and the container is still created successfully.

## Package manager: source-based, asynchronous installs

A package manager built from scratch: recipes are shell scripts (the same format Gentoo ebuilds/Arch PKGBUILDs/CRUX Pkgfiles use), builds happen inside this project's own container runtime, and the daemon **never sources or executes a recipe on the host** — recipe metadata (`pkg_name=`, `pkg_version=`, `pkg_source=`, `pkg_sha256=`, `pkg_depends=`) is read with a strict, non-executing line scanner; the recipe's real shell code (`pkg_build()`/`pkg_install()`) only ever runs inside the isolated, network-less build container.

**Every install targets one image**, `/var/lib/kanxeo/images/{image}/rootfs` — `"base"` by default (any container created with `"image": "base"` gets everything installed there, no separate host/container package paths to keep in sync), or an explicit other one (see [Per-image installs](#per-image-installs) below) for software that shouldn't be part of every container's baseline.

**Installs are asynchronous.** The daemon is single-threaded and non-blocking; a network fetch or a real compile can take anywhere from seconds to minutes, so `POST /v1/pkg/install` returns immediately (`202`) and the actual work happens in the background — poll `GET /v1/pkg/{name}` for progress. **Fetching happens on the host** (a `curl` subprocess — this project's networking plane has no outbound NAT, so a build container has no network access at all, a stronger isolation boundary for untrusted build scripts, not a limitation worked around).

One-time setup, before installing anything:

```
POST /v1/pkg/bootstrap
```

Stages a real build toolchain (`gcc`/`make`/`ld`/`as`/`cc1`/`sh`/`tar` and their real headers/libraries) into the sandboxed build image by copying this host's own `/usr/{include,lib,lib64,bin,libexec}` with the real `cp -a`. Idempotent — safe to call again.

A recipe (provisioned onto disk at `/var/lib/kanxeo/pkg/recipes/<name>.recipe`, out of band, the same v1 boundary container images already have):

```sh
pkg_name=hello
pkg_version=2.12.1
pkg_source=https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz
pkg_sha256=8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db8
pkg_depends=""

pkg_build() {
    ./configure --prefix=/usr
    make -j"$(nproc)"
}

pkg_install() {
    make DESTDIR="$PKG_DESTDIR" install
}
```

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

v1 serializes installs — only one may be in flight at a time (`POST /v1/pkg/install` for a second package while another is still `fetching`/`building` is a `409`).

### Dependencies

A recipe's `pkg_depends` (space-separated names) is resolved automatically. Given a `top` recipe with `pkg_depends="leaf"`:

```
POST /v1/pkg/install
{"name": "top"}
```

Installs `leaf` first (skipped entirely if already installed), then `top` — one `POST`, both packages end up `installed`, visible individually via `GET /v1/pkg`. **The `202` response describes whichever package actually started fetching first** — here, `leaf`, not `top`, since `top` can't start until its dependency is done. Poll by name (`GET /v1/pkg/leaf`, then `GET /v1/pkg/top`) to follow the whole chain. A dependency with no matching recipe, or a circular dependency (`A` needs `B` needs `A`), is a `400` — nothing is fetched.

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

### Per-image installs

Install into something other than the default `base` image with `"image"`:

```
POST /v1/pkg/install
{"name": "bird", "image": "router"}
```

`bird` (and its dependencies, resolved the same way as always) builds into `/var/lib/kanxeo/images/router/rootfs` — containers created with `"image": "base"` never see it. The same package name is tracked independently per image: `bash` installed into both `base` and `router` are two separate entries, each independently upgradable/removable. `router` above doesn't need to exist beforehand — the first install into a name never seen before creates it implicitly; `POST /v1/images {"name": "router"}` creates one explicitly instead, useful when you want an image to exist (and be immediately usable — its C runtime is seeded right away) before installing anything into it. See [Image lifecycle](#endpoints-at-a-glance) in the endpoint table above, or `openapi.yaml`'s own `/images` paths for the full contract.

`GET`/`DELETE` on a non-default image use the compound `{name}@{image}` path form:

```
GET /v1/pkg/bird@router
DELETE /v1/pkg/bird@router
```

A bare `GET /v1/pkg/bird` still means `bird@base`. `GET /v1/pkg` (the list) includes every `(name, image)` entry, each with its own `"image"` field.

Note: the C runtime for dynamically-linked binaries (`ld.so`/`libc.so.6`/`libtinfo.so.6`) is seeded automatically into whichever image a package lands in, `base` or otherwise (ADR-0019 for `base` at install time, ADR-0023 generalizes it to every image at first install).

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

- `devices` is optional: 0–N entries, each a discovered device id from `GET /v1/devices` (`"pci:..."`, `"usb:..."`, or `"gpu:N"` for a whole GPU — `gpu:N` is never itself listed by `GET /v1/devices`, only its individual member nodes are). Real `/dev` nodes are granted via a `BPF_CGROUP_DEVICE` program on the container's own cgroup (ADR-0017) — nothing else on the host can reach them once bound. A bare `gpu:N` id expands into every node that physical GPU needs in one grant (DRM `cardN`/`renderDN` plus the shared `/dev/kfd` compute node) — see ADR-0028/ADR-0029.
- `interfaces` is optional: 0–N real host network interface names (e.g. `"eth1"`) moved directly into the container's own netns (not a veth pair) — fd-anchored teardown, correct even if the container crashes mid-move. See ADR-0022.
- `GET /v1/containers` echoes the real, expanded grants actually made, not an echo of what was requested.

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

- `files` is optional: 0–N `{path, content, mode}` entries, staged directly onto the container's own filesystem *before* its process ever `execve()`s — so `cmd` can point straight at a staged script (e.g. `pbr.sh` above). `path` must be absolute with no `.`/`..` component (`400` otherwise); `content` is bounded at 64KiB per file.
- `sysctls` is optional: 0–N `{key, value}` entries; `key` must start with `net.` (the one sysctl subtree the kernel actually namespaces end to end — `400` for anything else, a real security boundary, not incidental). Applied inside the container's own netns right after `clone3()`, the same mechanism `ip_forward` already uses.
- Both survive exactly like everything else in `restart: "always"`'s own replay mechanism — no separate persistence work needed. See ADR-0030.

## Host + package updates

```
POST /v1/system/update
{"image_path": "/var/tmp/new-root.squashfs", "kernel_path": "/var/tmp/new-bzImage"}
```

- `image_path`/`kernel_path` are local paths the operator has already transferred onto the box (e.g. `scp`) — there is no upload endpoint; see ADR-0031 for why. Both are optional, but at least one is required — update just the root, just the kernel, or both together in one call. Writes whatever's given onto this daemon's own **inactive** A/B slot (the other one from whichever it's currently running as — `--slot=a` or `--slot=b`) and stages a fresh systemd-boot loader entry with a fresh boot-counter. The kernel is per-slot too (`kanxeo-bzImage-a`/`kanxeo-bzImage-b` on the ESP, both pre-staged identically at install time, ADR-0032) — a root-only update leaves the inactive slot's own existing kernel file untouched, it's never implicitly replaced. `400` if this daemon has no `--slot=` (not a real installed system), neither path is given, either path doesn't exist/isn't readable, or either file fails its own on-disk magic check (squashfs's `"hsqs"`, or a bzImage's boot-sector/`setup_header` magic) — checked for both before either is written, so a bad `kernel_path` never leaves a good `image_path` half-applied.
- Response includes `"updated"`, an array of whichever of `["root", "kernel"]` were actually written this call.
- Deliberately does **not** reboot — call `POST /system/reboot` separately once ready to cut over; the existing boot-counter/`confirm_boot()` machinery, entirely unchanged, decides whether the fresh slot sticks.

```
POST /v1/pkg/update-all
```

- Finds the first installed package (across every image) whose recipe's `pkg_version=` has drifted and starts an upgrade for it, reusing `POST /pkg/install {"upgrade": true}`'s entire existing mechanism — `202` with the started package's state, or `200 {"status": "nothing to update"}` if everything's already current. Starts at most one job at a time (the same v1 single-install-in-flight constraint every other install path has, honestly respected rather than worked around); call again once that job finishes to drain the whole backlog.

## Backup and restore

```
GET /v1/system/backup
```

Bundles platform *configuration* state — container definitions, networks, DNS records, package install state and recipes — as one response. **Read this carefully before relying on it for disaster recovery:**

- **Does NOT include workload data.** Each container's own persistent data (a git host's repos, a resolver's zone files, a metrics database) is that container's own concern, backed up with its own native tooling. This endpoint has no way to reach into another container's filesystem and never tries to.
- **Does NOT include image rootfs content.** Since everything is compiled from source, an image's content is reproducible by re-running `pkg install` for whatever `pkg_installed` records — this bundle is the "shopping list" (what should be installed, where), not the built bytes. Getting all the way back to a fully-populated system after a restore means re-running those installs, not something this endpoint does for you automatically.
- **Never touches PKI, at all.** The CA private key (and every issued leaf certificate's own key) is never returned over the API anywhere in this system, by existing, deliberate design (see [PKI](#pki-a-root-ca-and-issued-leaf-certificates) above) — that rule isn't bent or partially relaxed here. Back up `/var/lib/kanxeo/pki/` separately, directly on the host, outside the API entirely.

```
POST /v1/system/restore
{"container_defs": "...", "networks": "...", "dns_records": "...", "pkg_installed": "...", "pkg_recipes": {"hello": "..."}}
```

The reverse of `GET /system/backup` — same shape, every field optional and independent (at least one required), so you can restore just container definitions, just networks, or the whole bundle. Every field is validated (must itself parse as JSON, or for `pkg_recipes`, must be an object of strings) *before* anything is written, so one bad field can't leave the others half-applied.

**Does not reboot or take effect immediately.** Restored files only get picked up on the next boot — the same startup sequence (including container autostart) that already runs every time. Call the existing `POST /system/reboot` once you're ready to actually cut over. A typical disaster-recovery sequence: boot a fresh install once (normal empty first boot) → `POST /system/restore` with your saved bundle → `POST /system/reboot` → the second boot comes up with your restored state.

`kanxeoctl backup --output=PATH` saves the bundle verbatim (byte-for-byte, not re-serialized) for later use with `kanxeoctl restore --input=PATH` — the same file round-trips exactly. A scheduled backup job (a container with network reachability to `kanxeod`, or a simple host-level cron entry — either is equally valid, this is a plain REST client either way) can run `kanxeoctl backup` on a schedule and ship the result off-host.

## Current scope boundaries (v1, deliberate — see ADR-0007)

- No image build/pull endpoint yet — images are provisioned onto disk out of band.
- No log retrieval endpoint yet — the daemon doesn't capture container stdout/stderr separately.
- No authentication yet — the daemon binds to loopback only as its safety boundary for now.
- HTTP: no keep-alive/pipelining (`Connection: close` on every response), no chunked bodies.
- Routes are set-once at creation and not echoed back or introspectable afterward; modifying them on a running container would need a new "enter another netns from outside" primitive, not built yet. See `docs/ROADMAP.md`.
- DNS server bindings are in-memory only (not persisted, like the container registry itself — a binding referencing a container that dies with the daemon means nothing after a restart anyway). Only one hosts-format record type; no CNAME/MX/TXT/etc.
- PKI: no certificate revocation/CRL, no CA regeneration/rotation, no CSR-submission flow (the daemon always generates both the keypair and the cert itself) — see `docs/ROADMAP.md` Phase 9.
- Package manager: only one install in flight at a time (dependency chains, and `POST /pkg/update-all`'s own successive calls, still serialize through that same single slot — see [Host + package updates](#host--package-updates) below); no version-constrained dependencies (any installed version satisfies a dependency); symlinks in a package's own `DESTDIR` output are skipped (regular files and directories only) — see `docs/ROADMAP.md` Phase 10.
- No scheduled/periodic trigger for `POST /system/update` or `POST /pkg/update-all` — both are on-demand, operator- or cron-invoked; no automatic "update then reboot" chaining — see `docs/ROADMAP.md` Phase 16.
- No volume/bind-mount concept beyond small, content-inlined `files` (see [Per-container config files + sysctls](#per-container-config-files--sysctls) below) — a large binary asset or directory tree has no home in this model yet — see `docs/ROADMAP.md` Phase 15.

## Why this file exists alongside `openapi.yaml`

One Source of Truth means the *schema* lives in exactly one place (`openapi.yaml`). This page exists only so a human (or a future CLI/web implementer) can get oriented quickly without parsing YAML first — if the two ever disagree, `openapi.yaml` wins and this page is out of date and should be fixed.
