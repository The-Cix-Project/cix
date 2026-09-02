# Cix Host API

[`openapi.yaml`](openapi.yaml) (OpenAPI 3.0) is the **authoritative** contract — every field, schema, and status code is defined there, not here. This page is a human-friendly index into it, per the project's API-First Mandate: the REST daemon (`daemon/`, binary `cixd`) is the only process with direct access to the container runtime, and everything else (CLI, web dashboard) is built by reading this contract, never the daemon's source.

Default base URL: `http://127.0.0.1/v1` (port 80, loopback-only by default; see `daemon/src/main.c`'s `--bind`/`--port` flags).

## Endpoints at a glance

| Method | Path | Purpose |
|---|---|---|
| GET | `/health` | Liveness check -- minimal, low-latency, no build/slot identity |
| GET | `/config` | The whole running configuration as one ordered, redacted document (ADR-0206) |
| POST | `/login` | Authenticate, get a session token (ADR-0144) -- always open, exempt from write-gating |
| POST | `/logout` | Invalidate the current session (idempotent) |
| GET | `/whoami` | Is the caller's own bearer token currently authenticated -- read-only, never consumes a single-use session (ADR-0164) |
| GET | `/system/hostauth-config` | Current admin-group list, session idle timeout, live-LDAP backend config |
| PUT | `/system/hostauth-config` | Replace host-auth config (full replacement of admin_groups/idle_timeout_seconds; ldap_* fields optional) |
| GET | `/system/hostauth/sessions` | Every active session (username, expires-in) -- never a raw token, before or after issuance (ADR-0152) |
| DELETE | `/system/hostauth/sessions/{username}` | Revoke every active session for that user -- "log out everywhere" |
| GET | `/system/boot` | Build version/time, A/B slot, kernel version (`uname`) |
| GET | `/system/stalls` | Times the control plane stopped going round its own loop, recorded by a watchdog process (issue #100) |
| GET | `/system/kernel-policy` | Which kernel line this box tracks, what that channel currently points at, and how far behind the running kernel is (issue #65) |
| PUT | `/system/kernel-policy` | Set the channel — `pinned` / `longterm` / `stable` / `mainline` |
| POST | `/system/kernel-policy/refresh` | Re-ask kernel.org what each channel is at (202; the fetch is async) |
| GET | `/system/boot-console` | The installed system's own boot console parameters, and what each loader entry currently carries (issue #24) |
| PUT | `/system/boot-console` | Set them — rewrites the loader entries on the ESP, effective next boot |
| GET | `/system/esp` | The ESP's boot configuration — `loader.conf`'s default **pattern**, every loader entry, and `selected_entry`: which one the firmware will actually boot next (issue #128) |
| PUT | `/system/esp` | Set the default pattern and/or timeout. A default matching no entry is refused, not warned about |
| DELETE | `/system/esp/entries/{name}` | Remove one stale loader entry; refuses the last remaining entry for the running slot |
| GET | `/system/control-plane-reservation` | How much of the machine is held back for the daemon itself (issue #86) |
| PUT | `/system/control-plane-reservation` | Set it — applied to the live workload cgroup immediately |
| POST | `/system/factory-reset` | Return the box to its just-installed state and reboot — destroys everything including volumes (issue #63) |
| POST | `/system/shutdown` | Stop `cixd`; powers off the host too when running as real PID 1 |
| POST | `/system/reboot` | Stop `cixd`; restarts the host too when running as real PID 1 |
| POST | `/system/update` | Write a fresh control-plane squashfs and/or a fresh kernel onto this daemon's own inactive A/B slot |
| GET | `/system/boot-next` | What is armed to boot next, if anything |
| POST | `/system/boot-next` | Boot a slot **once**, then revert to normal selection (#154) |
| — | container `userns` field | User namespaces by default; opt-out per container or platform-wide (ADR-0207) |
| DELETE | `/system/boot-next` | Disarm |
| GET | `/system/backup` | Bundle platform configuration state (container defs, networks, DNS, package state, site config) |
| POST | `/system/restore` | Write a previously-backed-up bundle back to its real state files |
| GET | `/system/backup-config` | Which disk (if any) automatic backup snapshots write to, whether enabled, interval |
| PUT | `/system/backup-config` | Update backup-snapshot config (only the fields given are changed) |
| GET | `/system/backup-config/status` | Outcome of the most recent backup-snapshot attempt |
| POST | `/system/backup-config/snapshot-now` | Write a backup snapshot to the configured disk right now |
| GET | `/system/site` | This install's declared identity (`instance_name`/`site_name`/`domain_suffix`) |
| PUT | `/system/site` | Set this install's site identity |
| GET | `/system/daemon-config` | cixd's own listen port, HTTP/HTTPS exposure, and which network is currently its management one |
| PUT | `/system/daemon-config` | Live-reconfigure the listen port, HTTP/HTTPS listeners, or repoint the management network -- no restart |
| GET | `/system/iso` | Status of the most recent server-side installer ISO build |
| POST | `/system/iso` | Assemble a fresh installer ISO server-side, non-blocking |
| GET | `/system/signing-keys` | Whether this host holds a Secure Boot signing key pair, and which identity |
| PUT | `/system/signing-keys` | Install an operator-supplied signing key pair (two PEM blocks) |
| DELETE | `/system/signing-keys` | Remove this host's signing key pair |
| GET | `/system/release-key` | Whether this host holds an Ed25519 release-signing key, and its publishable public half |
| PUT | `/system/release-key` | Install the operator's Ed25519 release key (one PEM block) |
| DELETE | `/system/release-key` | Remove this host's release key |
| POST | `/system/iso/publish` | Publish the finished installer ISO and its signature to the artifact cache |
| GET | `/system/routes` | The box's own real kernel IPv4 routing table |
| POST | `/system/routes` | Add a real kernel route (gone on next reboot unless something else re-applies it) |
| DELETE | `/system/routes` | Remove a real kernel route |
| GET | `/system/swap` | Whether a host swap file is currently enabled, and its size |
| POST | `/system/swap` | Enable a host swap file at a given size |
| DELETE | `/system/swap` | Disable and remove the host swap file |
| GET | `/system/zswap` | The compressed swap cache: configured intent, what the kernel actually has, and which compressors it was built with (issue #51) |
| PUT | `/system/zswap` | Configure it -- partial update, applied to the running kernel and persisted |
| GET | `/system/kmsg` | Tail the kernel ring buffer directly (`tail` filter) -- dmesg for a host with no shell |
| GET | `/system/logs` | Query the consolidated log store (`source`/`level`/`container`/`regex`/`since`/`tail` filters) |
| GET | `/system/logs/config` | The consolidated log store's size cap |
| PUT | `/system/logs/config` | Set the log store's size cap |
| GET | `/system/stats` | Host-wide load/CPU/memory/disk/network snapshot |
| GET | `/system/server-health` | Health of every registered LDAP/DNS/NTP/syslog server (issue #81) |
| PUT | `/system/server-health/{kind}/{container}` | Drain or undrain one -- the operator override, persisted |
| GET | `/system/processes` | Every real process on the box, correlated to a container if any |
| DELETE | `/system/processes/{pid}` | Kill a process -- real, immediate SIGKILL |
| GET | `/system/ping` | Poll the current/last ICMP ping job |
| POST | `/system/ping` | Start a real ICMP echo against an IPv4 address |
| GET | `/system/resolv` | The host's own outbound DNS resolver config |
| PUT | `/system/resolv` | Replace it -- takes effect immediately, no reboot |
| GET | `/system/sysctl` | Every sysctl this daemon currently persists/manages |
| GET | `/system/sysctl/{key}` | Live current value of one host-level sysctl (persisted or not) |
| PUT | `/system/sysctl/{key}` | Write one host-level sysctl, live; persists by default |
| DELETE | `/system/sysctl/{key}` | Stop reapplying at boot; never touches the live value |
| GET | `/system/kmod` | Every currently-loaded kernel module (live `/proc/modules`) |
| GET | `/system/kmod/{name}` | Real `modinfo` for a built/available module, whether loaded or not |
| POST | `/system/kmod/{name}` | Load it (real `modprobe`); options fall back to its own persisted default |
| DELETE | `/system/kmod/{name}` | Unload it (real `modprobe -r`) |
| GET | `/system/kmod-config` | Every module with persisted default options and/or boot autoload |
| PUT | `/system/kmod-config/{name}` | Set default options and/or autoload; only fields given are touched |
| DELETE | `/system/kmod-config/{name}` | Clear a module's persisted config; never touches whether it's loaded |
| POST | `/system/kmod-build` | Rebuild the kernel with extra in-tree `=m` modules; poll via `/pkg/hostbuild/kernel` |
| GET | `/system/rolling-config` | The configured rolling-restart jitter window (`jitter_window_seconds`) |
| PUT | `/system/rolling-config` | Set the jitter window -- 0 disables jitter, restart happens immediately |
| GET | `/system/pkg-build-config` | The configured pkg install/hostbuild concurrency ceiling (`max_concurrent_jobs`) and per-build memory/CPU cgroup limits (`memory_max`/`cpu_max`, ADR-0165) |
| PUT | `/system/pkg-build-config` | Partial update -- only the fields given are changed; `max_concurrent_jobs` 1-10 (lowering it doesn't disrupt jobs already in flight); `memory_max`/`cpu_max` apply to every build's own sandbox from the next build onward, 0/null means unlimited |
| GET | `/system/tls-throttle` | Per-source-IP throttling config for repeated failed HTTPS handshakes |
| PUT | `/system/tls-throttle` | Partially update it -- fields omitted are left unchanged |
| GET | `/system/tls-throttle/status` | Every source currently tracked for failed handshakes, live |
| GET | `/system/ntp` | Upstream NTP server address list used to sync the host clock |
| PUT | `/system/ntp` | Replace it |
| GET | `/system/ntp/status` | Outcome of the most recent sync attempt |
| POST | `/system/ntp/sync` | Trigger a sync attempt now, rather than waiting for the next hourly automatic one |
| GET | `/system/time` | The host's current date/time |
| PUT | `/system/time` | Manually set the host clock (`clock_settime()`, immediate, no reboot) |
| GET | `/ntp/servers` | List all registered NTP server bindings |
| POST | `/ntp/servers` | Register a running container as an available internal NTP time source |
| DELETE | `/ntp/servers/{container}` | Unregister an NTP server binding |
| GET | `/syslog/targets` | List all registered syslog forward targets |
| POST | `/syslog/targets` | Register a running container as an optional syslog forward target |
| DELETE | `/syslog/targets/{container}` | Unregister a syslog forward target |
| GET | `/containers` | List all containers this daemon knows about |
| POST | `/containers` | Create and start a container |
| GET | `/containers/{name}` | Inspect one container |
| PATCH | `/containers/{name}` | Edit the stored definition in place — cmd, env, files, limits, volumes (issue #11). Applies at next start |
| DELETE | `/containers/{name}` | Stop (if running), remove it, and forget any persisted definition |
| POST | `/containers/{name}/start` | Bring a stopped or exited container back to life |
| POST | `/containers/{name}/stop` | Kill it now, always keep its persisted definition (only `DELETE` removes a container) |
| POST | `/containers/{name}/pause` | Freeze a running container via the cgroup v2 freezer |
| POST | `/containers/{name}/unpause` | Thaw a paused container |
| GET | `/containers/{name}/stats` | Real, host-side CPU/memory/disk/network usage, a point-in-time snapshot |
| GET | `/containers/{name}/migrate-storage` | Status of the most recent (or running) container-storage migration |
| POST | `/containers/{name}/migrate-storage` | Move this container's own overlay storage to a new disk, or back to the default |
| GET | `/containers/{name}/files` | Read one file's raw bytes back out of a container's rootfs |
| PUT | `/containers/{name}/files` | Write/overwrite one file inside an already-existing container, live, without a recreate (ADR-0153) |
| POST | `/containers/{name}/networks` | Attach a network to an already-running container, live, without a recreate (ADR-0156) |
| DELETE | `/containers/{name}/networks/{network}` | Detach a live-attached network; refuses a create-time attachment (409) |
| POST | `/containers/{name}/devices` | Attach a device to an already-running container, live, without a recreate (ADR-0161) |
| DELETE | `/containers/{name}/devices/{id}` | Detach a live-attached device; refuses a create-time attachment (409) |
| GET | `/containers/{name}/console` | Upgrade to a WebSocket; an interactive shell inside the running container |
| GET | `/containers/recipes` | List container recipes (metadata only) (ADR-0151) |
| POST | `/containers/recipes` | Add/replace a container recipe -- content must be a real `POST /containers` body, its own `"name"` matching the recipe's |
| GET | `/containers/recipes/{name}` | One container recipe's full detail, including its raw, unsubstituted text |
| DELETE | `/containers/recipes/{name}` | Remove a container recipe |
| POST | `/containers/recipes/{name}/apply` | Render `{name}`'s own stored recipe (substituting any `{{SECRET:KEY}}` tokens from the request's own `secrets` object, and `{{LDAP:URI}}`/`{{LDAP:BASE_DN}}`/`{{LDAP:BIND_DN}}`/`{{LDAP:BIND_PASSWORD}}` tokens from `GET/PUT /ldap/config`'s own stored client settings, issue #66 — no per-apply secret, no copied values) and create the container from it (201) |
| GET | `/devices` | List host PCI/USB/net/GPU/disk devices discoverable via sysfs, available for passthrough |
| GET | `/devicemaps` | List persistent, operator-named device mappings |
| POST | `/devicemaps` | Create a persistent device mapping (name -> selector) |
| DELETE | `/devicemaps/{name}` | Remove a device mapping |
| GET | `/disks` | List real host block devices, including their partitions, for multi-disk management |
| GET | `/diskroles` | List persisted disk role assignments |
| POST | `/diskroles` | Assign a role (container-storage/backup/state-storage/rebuildable-storage/log-storage/swap) to a disk or partition |
| DELETE | `/diskroles/{disk_name}` | Remove a disk's role assignment |
| GET | `/disks/{disk_name}/format` | Status of the most recent (or running) format+mount job for this disk |
| POST | `/disks/{disk_name}/format` | Destructive: mkfs (ext4 or btrfs) + mount an already role-assigned disk |
| POST | `/disks/{disk_name}/partition-table` | Destructive: writes a fresh, empty GPT partition table to a whole disk |
| POST | `/disks/{disk_name}/partitions` | Append one new partition to a disk's existing table |
| DELETE | `/disks/{disk_name}/partitions/{partition_name}` | Remove one partition |
| GET | `/system/state-storage` | Which disk (if any) is the active placement for Cix's own state |
| GET | `/system/state-storage/migrate` | Status of the most recent (or running) state-storage migration |
| POST | `/system/state-storage/migrate` | Move Cix's own state to a new disk, or back to the default |
| GET | `/system/log-storage` | Which disk (if any) is the active placement for the consolidated log store |
| GET | `/system/log-storage/migrate` | Status of the most recent (or running) log-storage migration |
| POST | `/system/log-storage/migrate` | Move the consolidated log store to a new disk, or back to the default |
| GET | `/system/rebuildable-storage` | Which disk (if any) is the active placement for images/packages/artifacts |
| GET | `/system/rebuildable-storage/migrate` | Status of the most recent (or running) rebuildable-storage migration |
| POST | `/system/rebuildable-storage/migrate` | Move images/packages/artifacts to a new disk, or back to the default |
| POST | `/containers/{name}/exec` | Run a command inside a running container, no shell and no pty (issue #62) |
| GET | `/containers/{name}/exec` | That command's state, output and exit status |
| POST | `/containers/{name}/volumes` | Attach a volume to an existing container -- edits the definition, applies on next start (issue #92) |
| DELETE | `/containers/{name}/volumes/{volume}` | Detach it again; never touches the volume or its data |
| POST | `/disks/{name}/partitions/{part}/resize` | Grow a partition **and** the filesystem inside it — grow only (issue #94) |
| GET | `/disks/{name}/free-space` | How much room is left in this disk's partition table, from sfdisk (issue #95) |
| GET | `/volumes` | List every persistent volume (issue #88, ADR-0183) |
| POST | `/volumes` | Create a volume -- named storage whose lifetime is independent of any container |
| GET | `/volumes/{name}` | Inspect one volume |
| GET | `/software` | What is declared (has a recipe) against what is actually installed (issue #97) |
| GET | `/system/volume-backup-config` | The shared schedule for volume content snapshots (issue #96) |
| PUT | `/system/volume-backup-config` | Set it — target disk, on/off, interval |
| GET | `/volumes/{name}/backups` | A volume's backup policy, its snapshots, and the last attempt |
| PUT | `/volumes/{name}/backups` | Opt a volume in (or out) and set its retention |
| POST | `/volumes/{name}/backup` | Take one snapshot now |
| POST | `/volumes/{name}/restore` | **Replace** the volume's contents with a snapshot |
| DELETE | `/volumes/{name}/backups/{snapshot}` | Delete one snapshot |
| PUT | `/volumes/{name}/owner` | Who owns the volume's directory — a volume is created root-owned, which a non-root workload cannot write to (issue #102) |
| PUT | `/volumes/{name}/quota` | Set or clear a real, kernel-enforced size limit on a volume (issue #93) |
| POST | `/volumes/{name}/migrate` | Move a volume's data to another disk or partition |
| DELETE | `/volumes/{name}` | Delete a volume **and all of its data** -- refused (409) while any container definition references it |
| GET | `/networks` | List all networks this daemon knows about |
| POST | `/networks` | Create a network (a real bridge, persisted across restarts) |
| GET | `/networks/{name}` | Inspect one network |
| DELETE | `/networks/{name}` | Remove a network (refused if any container is still attached, or if it's the management network) |
| POST | `/networks/{name}/interfaces` | Attach a real host network interface to this network's bridge |
| DELETE | `/networks/{name}/interfaces/{ifname}` | Detach a previously-attached interface (refused for the management network) |
| GET | `/dhcp` | Every DHCP range and reservation (ADR-0197) |
| GET | `/dhcp/servers` | Registered DHCP servers, and whether each also resolves its own leases |
| POST | `/dhcp/servers` | Register a container as a DHCP server |
| DELETE | `/dhcp/servers/{container}` | Unregister one -- also drops it from every range that named it |
| GET | `/dhcp/networks/{network}` | A network's DHCP range |
| PUT | `/dhcp/networks/{network}` | Set it: enabled, range, lease time, router, and which servers serve it |
| DELETE | `/dhcp/networks/{network}` | Remove it |
| GET | `/dhcp/leases` | Current leases, read from each server's own lease file |
| POST | `/dhcp/static` | Reserve an address for a MAC (live -- no restart) |
| DELETE | `/dhcp/static/{mac}` | Remove a reservation |
| GET | `/networks/{name}/ports` | What is plugged into this network's bridge right now, per port, with each port's own counters (issue #26) |
| GET | `/images` | List every image this daemon knows about |
| POST | `/images` | Create an empty image (runtime pre-seeded, ready for `pkg install`) |
| POST | `/images/gc` | Reclaim image versions nothing references; `{"dry_run":true}` to preview |
| GET | `/images/{name}` | Inspect one image, including its manifest |
| DELETE | `/images/{name}` | Remove an image (refused for `base`, if in use, or if it still has packages) |
| POST | `/images/{name}/manifest` | Upsert one `{package, mode, version}` manifest entry (ADR-0107) |
| DELETE | `/images/{name}/manifest/{package}` | Remove one manifest entry |
| GET | `/images/recipes` | List image recipes (metadata only) (ADR-0123) |
| POST | `/images/recipes` | Add/replace an image recipe (`image_packages=`) |
| GET | `/images/recipes/{name}` | One image recipe's full detail, including its raw text |
| DELETE | `/images/recipes/{name}` | Remove an image recipe |
| POST | `/images/{name}/apply-recipe` | Apply `{name}`'s own stored recipe -- bulk-declares the manifest (204), or an async whole-rootfs artifact fetch for a fully-pinned recipe with a matching artifact server (202) |
| POST | `/images/{name}/rename` | Rename an image, moving its package rows with it (issue #124). Refused while a container uses it |
| POST | `/images/{name}/export` | Export the image's current version as a whole-rootfs tarball, so compiled output moves to another host instead of being rebuilt (issue #126). Async (202) -- poll the GET |
| GET | `/images/{name}/export` | That export's state (`none`/`building`/`ready`/`failed`), its `version`, `artifact_path`, and `artifact_name`. One export runs at a time, of either kind -- images and hostbuild artifacts share this state machine (ADR-0201) and are matched on both kind and name |
| GET | `/images/{name}/export/download` | Bytes of a ready export, in bounded chunks (`?offset=&length=`, clamped to 8 MiB) the caller loops over |
| GET | `/dns/records` | List all DNS records this daemon knows about |
| POST | `/dns/records` | Create a DNS record (name -> IP, persisted across restarts) |
| GET | `/dns/records/{name}` | Inspect one DNS record |
| PUT | `/dns/records/{name}` | Edit an existing DNS record's ip in place (task #749) |
| DELETE | `/dns/records/{name}` | Remove a DNS record |
| GET | `/dns/servers` | List all registered DNS server bindings |
| POST | `/dns/servers` | Register a running container as a DNS-serving target |
| DELETE | `/dns/servers/{container}` | Unregister a DNS server binding |
| POST | `/dns/provision` | Bring up the whole DNS service in one call: replicas, registration, host resolver |
| GET | `/dns/forwarders` | The upstream resolvers every registered DNS server forwards through |
| PUT | `/dns/forwarders` | Replace the forwarder list and apply it live to every registered server (#134) |
| GET | `/ldap/servers` | List all registered LDAP server bindings |
| POST | `/ldap/servers` | Register a running container as the LDAP-serving target |
| DELETE | `/ldap/servers/{container}` | Unregister an LDAP server binding |
| GET | `/ldap/groups` | List every LDAP group |
| POST | `/ldap/groups` | Create an LDAP group (`gidnumber` optional -- auto-allocated if omitted, task #748) |
| GET | `/ldap/groups/{name}` | Inspect one LDAP group |
| PUT | `/ldap/groups/{name}` | Edit an existing LDAP group's gidnumber in place (task #750); a real, different `name` in the body renames it (ADR-0147) |
| DELETE | `/ldap/groups/{name}` | Delete an LDAP group |
| GET | `/ldap/users` | List every LDAP user |
| POST | `/ldap/users` | Create an LDAP user (`uidnumber` optional -- auto-allocated if omitted, task #748; `ssh_public_key` optional, task #731) |
| GET | `/ldap/users/{name}` | Inspect one LDAP user |
| PUT | `/ldap/users/{name}` | Update an existing LDAP user (full field replacement; `password` omitted keeps the existing credential); a real, different `name` in the body renames it (ADR-0147) |
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
| GET | `/pkg/policies` | Per-package rolling policy — which version an omitted version resolves to (issue #64) |
| PUT | `/pkg/policies/{name}` | Set it: `highest` (default), `newest`, or `pinned` with a version |
| DELETE | `/pkg/policies/{name}` | Back to the default |
### Why a package failed, as a field (issue #101)

`GET /v1/pkg/{name}` and the package list report **`failure_kind`** alongside `error`:

| kind | meaning | what to do about it |
|---|---|---|
| `fetch` | the source could not be reached, or did not match its checksum | usually transient — try again |
| `build` | the build container failed to start, or the build itself failed | a real defect in the recipe or toolchain — retrying changes nothing |
| `recipe` | the recipe is missing, unreadable or unparseable | a catalogue problem — sync or fix the recipe |
| `install` | the build succeeded; merging its output into the image did not | neither of the above; look at the image |
| `null` | nothing is wrong | — |

`failed` on its own could not be acted on: a package that could not be *reached* and one that failed to *build* read identically, and every caller had to pattern-match an error string to tell them apart. `cixctl pkg ls` shows it as `failed:fetch` / `failed:build`; the dashboard shows it next to the state with the message on hover.

The kind is set in the one function that records a failure, and it is a required parameter of that function — a failure cannot be recorded without saying what kind it was.

### Why a package failed, as a field (issue #101)

`GET /v1/pkg/{name}` and the package list report **`failure_kind`** alongside `error`:

| kind | meaning | what it calls for |
|---|---|---|
| `fetch` | the source could not be reached, or did not match its checksum | usually transient — try again |
| `build` | the build container failed to start, or the build itself failed | a real defect in the recipe or toolchain; retrying changes nothing |
| `recipe` | the recipe is missing, unreadable or unparseable | a catalogue problem — sync or fix the recipe |
| `install` | the build succeeded, merging its output into the image did not | neither of the above |
| `null` | nothing is wrong | — |

`failed` on its own could not be acted on: a package that could not be *reached* and one that failed to *build* read identically, and every caller had to pattern-match an error string to tell them apart. `cixctl pkg ls` shows it as `failed:fetch` / `failed:build`; the dashboard shows it beside the state, with the message on hover.

The kind is set in the single function that records a failure, and it is a **required parameter** of that function — a failure cannot be recorded without saying what kind it was.

| GET | `/pkg/build-logs` | Every persisted build log, newest first — the complete output of each recent build (issue #57) |
| GET | `/pkg/build-logs/{file}` | One build log as plain text, tail-first if it is larger than the response cap |
| GET | `/pkg/sync` | The most recent (or currently running) sync's status |
| GET | `/pkg/drift` | Every installed package whose recipe on disk is newer than what is installed — the aggregate `available_version` has always reported one at a time |
| GET | `/pkg/cache-config` | The configured local build-artifact cache size cap |
| PUT | `/pkg/cache-config` | Set the cache's size cap (always a real cap — no "unlimited" mode) |
| GET | `/pkg/cache` | Current local build-artifact cache occupancy |
| DELETE | `/pkg/cache` | Clear every cached artifact — an explicit operator reset |
| GET | `/pkg/artifact-config` | The configured plain-HTTP precompiled-artifact server (never a git forge), and whether fresh builds publish themselves to it |
| PUT | `/pkg/artifact-config` | Partially update the configured artifact server, including `push_enabled` (issue #129) |
| POST | `/pkg/{name}/artifact/export` | Export a hostbuild package's harvested artifact as a tarball, so it can leave the box that built it (issue #129). Async (202) -- poll the GET |
| GET | `/pkg/{name}/artifact/export` | That export's state (`none`/`building`/`ready`/`failed`), its `version`, `artifact_path`, and `artifact_name` |
| GET | `/pkg/{name}/artifact/export/download` | Bytes of a ready export, in bounded chunks (`?offset=&length=`, clamped to 8 MiB) |
| POST | `/pkg/{name}/artifact/publish` | Publish an already-built artifact to the configured artifact server without rebuilding it (issue #171) -- builds the tarball from the installed tree first when a hostbuild's doesn't exist yet. Async (202) |
| POST | `/pkg/install` | Start installing a package (async — returns immediately) |
| POST | `/pkg/update-all` | Start an upgrade for the first installed package whose recipe has drifted |
| POST | `/pkg/hostbuild` | Start a hostbuild job — build a standalone artifact instead of merging into an image |
| GET | `/pkg/hostbuild/{name}` | Inspect one hostbuild job's current state |
| POST | `/pkg/resume` | Continue a `keep_on_failure`-preserved build container in place, without a fetch/extract restart |
| POST | `/pkg/cancel` | Stop an in-flight build **or fetch**; the entry becomes failed with kind `cancelled` |
| GET | `/pkg/build/log` | Upgrade to a WebSocket; live-tail a currently-running install/hostbuild job's own stdout/stderr |
| GET | `/pkg` | List every known package (installed or in-flight) with its state |
| GET | `/pkg/{name}` | Inspect one package's current state |
| DELETE | `/pkg/{name}` | Uninstall a package, or clear a permanently-failed entry (never actually merged into any image, so no new image version is produced) |

**Seeing how far behind a host is.** `GET /pkg/{name}` has always answered "is this out of date" for one package, in `available_version`. Nothing aggregated it, so the only way to learn a host had drifted was to ask about every installed package and compare — which in practice nobody did. On the reference host that reached 34 of 139 installed packages, several revisions behind in places, and was found by accident (issue #217). `GET /pkg/drift` answers it in one call.

It reports; it does not act. `POST /pkg/update-all` is the verb, and it deliberately upgrades one package at a time (ADR-0031), so `drift` is how an operator sees what is still outstanding before, during and after. The comparison is the same code all three paths use — the recipe is re-read from disk on every call, so there is no cached answer that can go stale.


Every error response is `{"error": "message"}` with an appropriate 4xx/5xx status. Every mutating endpoint that touches disk or spawns a subprocess can in principle also return `500` (a real I/O or subprocess failure, not a client mistake) — see `openapi.yaml`'s own per-path `"500"` response for exactly which internal failure each one covers; the specific set differs per endpoint and isn't repeated here.

## The running configuration as one document (ADR-0206)

`GET /config` answers the question an operator actually asks -- *what is
this box configured to do?* -- in one call instead of a dozen.

```
GET /v1/config
```

Three properties are the contract, and each exists for a reason:

**It is derived, never stored.** The document is rendered fresh from the
daemon's own live state on every call. Nothing keeps a copy, because a
kept rendering would be a second source of truth for every setting and
would drift -- the exact failure `One Source of Truth` exists to prevent,
and one this project has already been bitten by in its own documentation.
`GET /config` is a view, exactly like the dashboard.

**Order is part of the contract.** A section never depends on one below
it: identity and resolver, then storage, then networks, then the DNS and
DHCP that sit on those networks, then images, then the packages installed
into them, and containers last because they consume all of it. A
rendering whose order cannot be replayed is not replayable.

**Secrets are never rendered.** A configuration document is the single
most likely artifact to be pasted into a ticket or committed to git, so
this is a hard rule rather than a default. Where a subsystem holds a
secret, the document carries a set/not-set boolean instead of the value:

```json
{
  "package_repo":      { "repo_url": "...", "auth_token_set": true },
  "package_artifacts": { "base_url": "...", "auth_token_set": true },
  "ldap":              { "bind_dn": null,   "bind_password_set": false }
}
```

PKI private key material never appears at all.

### Where the section list comes from

Not from the daemon. The `ConfigDocument` schema in
[`openapi.yaml`](openapi.yaml) lists the sections, in order, and is the
**only** place that list exists: `tools/apigen.c` reads it and generates
the section table `daemon/src/config.c` builds its renderers from. A
section in the schema with no renderer does not compile.

That is deliberate. Agreement between two hand-maintained lists is
precisely what drifts, and adding a configurable subsystem while
forgetting the document would produce something worse than no document --
one that looks complete and is not. So adding a subsystem to this
platform means adding it to that schema, and forgetting fails the build.

### Rendering

The document is the daemon's; the presentation is not. `cixctl show
running-config` renders it Cisco-style, and the dashboard renders the
same document its own way. That text is never parsed back by anything,
so its format is deliberately not a contract -- which is what lets the
CLI iterate on it without a daemon rebuild, an A/B slot write and a
reboot.

```
$ cixctl --host=... show running-config
!
site
  instance_name cix-01
  site_name home
  domain_suffix home.arpa
!
networks
  name lan
  bridge cixbr0
  subnet 10.0.0.0/24
!
```

`--json` gives the document itself, unrendered.

## Host authentication (ADR-0144)

`cixd` had no authentication at all until ADR-0144: every write below succeeded with zero credentials. The rule now is one and applies uniformly, not per-endpoint: every `POST`/`PUT`/`DELETE` in this document, plus `GET .../containers/{name}/console` (a WebSocket upgrade that is arbitrary command execution in real effect, gated by intent rather than HTTP method), needs a valid `Authorization: Bearer <token>` naming a user who is currently a member of one of the configured admin groups. Every `GET` — including every one already listed above — stays open, unconditionally, always; reads were never the concern. `openapi.yaml`'s `components.securitySchemes.bearerAuth` carries this note once rather than repeating it on all ~130 mutating operations individually.

```
POST /v1/login
{"username": "alice", "password": "correct horse battery staple"}
-> 200 {"token": "6245f4...", "expires_in_seconds": 900}

POST /v1/containers            (subsequent writes)
Authorization: Bearer 6245f4...
{"name": "web-1", ...}
```

**Gating only ever activates once someone exists to gate for.** `GET /system/hostauth-config` reports the current `admin_groups` list; as long as it's empty, or none of its groups has a member yet, every write stays open — a fresh install, or one where an operator hasn't gotten around to configuring this yet, can never lock itself out of its own API. The moment a real LDAP user (`POST /ldap/users`, below) becomes a member of a configured admin group, gating activates for every subsequent request. There is no in-band break-glass credential (nothing reachable over this API can bypass write-gating once it's active, by design) — the recovery path for a genuine lockout requires physical/hypervisor console access instead: `cix-recover`, a second boot option on the installer media, resets only `admin_groups` on an already-installed system after a real typed confirmation. See [`docs/guides/security.md`'s break-glass recovery section](../guides/security.md#break-glass-recovery-adr-0146) and [ADR-0146](../adr/0146-ldap-startup-resync-and-break-glass-recovery.md).

**Sessions are a sliding idle window, in memory only** — wiped on every daemon restart, same as the container registry itself. `idle_timeout_seconds` (`PUT /system/hostauth-config`) refreshes on every authenticated request that presents a still-valid token; `0` means something deliberately stronger — no session reuse at all, a token is consumed the instant it's used once, and every subsequent write needs a fresh `POST /login`.

**Session introspection (ADR-0152)**: `GET /system/hostauth/sessions` lists every currently active session (`username`, `expires_in_seconds` — `null` specifically for the `idle_timeout_seconds=0` single-use case, since no fixed "expires in" applies there). The raw token itself is never included — it is shown exactly once, in `POST /login`'s own response, and never again anywhere. `DELETE /system/hostauth/sessions/{username}` revokes every active session for that user at once ("log out everywhere") rather than one specific session — the only genuine per-session identifier is the raw token, which this API deliberately never surfaces again after issuance, so there is no safe way to target just one. Always `204`, even for a user with no active session (the same idempotent posture `POST /logout` already has for an unknown token).

**Self-introspection, distinct from admin introspection, and deliberately non-mutating (ADR-0164)**: `GET /system/hostauth/sessions` (above) answers "who's logged in, from an admin's point of view" — `GET /whoami` answers the narrower "is *my own* current token still good," reachable by any caller since it's an ordinary GET (write-gating leaves every GET open by construction). The reason it's a dedicated endpoint rather than just "try a GET and see if it 401s": an ordinary GET *always* succeeds regardless of auth state, so there was no way to answer that question at all without attempting a real mutation. Critically, checking is never itself destructive: it does not refresh a session's sliding idle window, and — the case that actually motivated this — it does not consume a single-use (`idle_timeout_seconds: 0`) token the way a real authenticated write would. `cixctl`'s own interactive shell prompt (`host.site.tld>` unauthenticated, `host.site.tld#` authenticated, refreshed after every `login`/`logout` command) is built on this endpoint.

**One directory, two interchangeable ways to check a password.** `POST /login` always checks against this platform's own LDAP user store (`POST /ldap/users`, below) — the same store, the same `passbcrypt` field, regardless of which path answers. With `ldap_enabled: false` (the default), it's a local, in-process bcrypt check, no network call. With `ldap_enabled: true`, a real LDAP simple-bind is attempted first against each server in `ldap_servers`, in order (a hand-rolled minimal LDAPv3 client, `daemon/src/ldapclient.c` — no `libldap` dependency, matching this project's own precedent for the daemon's other wire protocols); the first server to answer authoritatively — a successful bind, or a real credential rejection — decides the outcome, and only when every configured server is unreachable does this fall back to the local check. The fallback changes *availability*, never *which password is actually correct*.

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

Creating a network creates its bridge immediately via rtnetlink and persists the definition to `/var/lib/cix/state/networks.json` (ADR-0141) — unlike containers (safe to be in-memory-only, since they die with the daemon), a bridge outlives this process, so the daemon reloads and recreates every persisted network's bridge idempotently at startup.

### Attaching a real host interface

```
POST /v1/networks/internal/interfaces
{"ifname": "eth1", "vlan_id": 0}
```

Enslaves a real, currently-assignable host interface (`GET /devices`'s own `"net:<ifname>"` entries — not already moved into a container's netns, not already attached anywhere via this endpoint) directly into this network's bridge — the "physical ethernet on a host-managed switch" mechanism, distinct from `interfaces` on `POST /containers` (which moves a NIC straight into one container's own netns instead). `vlan_id` 0 or omitted enslaves `eth1` itself, untagged; a nonzero `vlan_id` instead creates and enslaves an 802.1q `eth1.<vlan_id>` sub-interface, leaving `eth1` free to attach (with a different `vlan_id`) to other networks too. `DELETE /v1/networks/internal/interfaces/eth1` detaches it — releasing the interface from the bridge, or deleting the VLAN sub-interface, whichever this call originally created.

## The management network and cixd's own listeners

At install time (`cix-install`'s `--ip=`/`--prefix=`/`--gateway=`/`--interface=` flags — see [`installing.md`](../guides/installing.md)), cixd bootstraps a real, ordinary network named `management`: the given physical interface is attached to it exactly like `POST /networks/{name}/interfaces` above, and its own address (`--ip=`/`--prefix=`) becomes cixd's own bind address. This is a deliberate design choice (Part 0.5) — the host's own management IP lives on a bridge device via the same `network_def` mechanism every other network already uses, visible at `GET /networks`, not a separate GRUB-only address invisible to the API. (`--gateway=` means something different and unrelated: the box's own *upstream* default route, i.e. the home router this box's outbound traffic egresses through — not to be confused with the management network's own `address` field, which is cixd's bind address.)

Exactly one network has `is_management: true` at a time (`Network`'s own field, in every `GET /networks` response). Because deleting or detaching from that network's bridge would sever the connection you're managing the box through, `DELETE /networks/{name}` and `DELETE /networks/{name}/interfaces/{ifname}` both unconditionally refuse (`409`) while `is_management` is set — there is deliberately no override/force flag on either generic endpoint. Repointing management to a different network first is the only way past this:

```
GET /v1/system/daemon-config
```

```json
{"port": 80, "bind": "192.168.50.10", "bind_ip": null, "management_network": "management", "http_enabled": true, "https_enabled": true, "https_port": 443}
```

```
PUT /v1/system/daemon-config
{"management_network": "lan1"}
```

Resolves `lan1`'s own existing address (it must already have one — `has_address: true`, `400` otherwise) and performs a live listen-socket rebind to it — the new socket is created, bound, and added to `epoll` *before* the old one is torn down, so a failure rolls back to the still-working previous listener rather than leaving a gap. Only once the rebind succeeds does `is_management` actually move from the old network to `lan1`. This works identically whether `lan1` has a physical NIC attached directly or gets its connectivity entirely from a container (e.g. a WiFi-AP container bridging a passed-through wireless radio) — cixd only ever cares about the network's own address, never how it's fed.

### What is plugged in: the switch panel (issue #26)

```
GET /v1/networks/management/ports
```

```json
{"network": "management", "bridge_present": true, "ports": [
  {"ifname": "eth0", "kind": "uplink", "vlan_id": 0, "container": null, "ip": null,
   "link": "up", "rx_bytes": 918273645, "tx_bytes": 51234567, "rx_packets": 0, "tx_packets": 0},
  {"ifname": "vh97-0", "kind": "container", "vlan_id": null, "container": "dns-1",
   "container_ifname": "eth0", "ip": "192.168.15.101", "link": "up",
   "rx_bytes": 59895, "tx_bytes": 4240, "rx_packets": 952, "tx_packets": 68}
]}
```

**The port list comes from the kernel.** Everything currently enslaved to this network's bridge, read from `/sys/class/net/<bridge>/brif` — not from what this daemon believes it attached. If the two ever disagree, the kernel is the one that is right, and a port that cannot be accounted for is reported as `kind: "unattributed"` rather than dropped. A veth sitting on the bridge that belongs to no container we know of is exactly the thing an operator needs to see, and asking the switch what is plugged into it is the only way to see it.

The registry is the annotation layer on top: which container owns a port, what the interface is called inside it, which IP it holds.

**A container that is not running has no port.** It has no veth on the bridge — nothing is plugged in — and drawing a port for it would say otherwise. The containers *defined* on a network are a different question, already answered by cross-referencing `GET /containers`.

**`rx`/`tx` are from the port's own side**, which is the switch's side: `rx_bytes` is what reached the switch from whatever is plugged in. That is the inverse of what the container sees on its own interface, and saying which way round it is here is the difference between a useful number and a misleading one. Raw counters only — the caller computes rates, exactly as with [`/containers/{name}/stats`](#container-stats) and [`/system/stats`](#host-stats).

**No port numbers.** Ports come and go with containers, so any number would be positional and would move; `ifname` is what names a port. The order returned *is* deterministic (uplinks, then container ports by container name, then unattributed) so a rendered panel does not reshuffle between polls — the dashboard numbers what it draws, and says that number is its own.

A VLAN sub-interface is a port in its own right, carrying its `vlan_id`; its parent NIC is not a port of this bridge unless separately enslaved. `bridge_present: false` with an empty list means the network is defined in state but not realised on this host — a fact, not a fault.

`cixctl network ports NAME` is the CLI surface; the dashboard draws it as a switch panel on the network's own page, with a per-network traffic chart under it.

### A dedicated bind IP, decoupled from the management network's own address

```
PUT /v1/system/daemon-config
{"bind_ip": "192.168.50.20"}
```

`bind_ip` (ADR-0068) is a *second*, dedicated address on the management network's own bridge — cixd binds there instead of that network's own address, without the two being the same thing. Useful when the bridge is shared with other traffic (containers, a routing daemon) and the operator wants cixd itself pinned to a specific, separate address on it. Must be a real, unused address within the management network's own subnet (`400` otherwise); added to the bridge via a real `rtnl_addr_add_ipv4()` *before* the listener rebinds to it. Any previously-set `bind_ip` is removed from the bridge a couple of seconds *after* the response for this same request has already gone out, not synchronously — confirmed necessary the hard way (see ADR-0068): deleting it immediately can race the kernel's own delivery of the response when this exact request arrived over a connection whose local address *is* the one being removed, which is the common case for an operator reaching the daemon at wherever it's currently bound. Set explicitly to `null` to clear it and revert to the management network's own address:

```
PUT /v1/system/daemon-config
{"bind_ip": null}
```

Repointing `management_network` without also giving a fresh `bind_ip` in the same request implicitly clears any previously-set one — a dedicated `bind_ip` only ever makes sense relative to whichever network is management at the time, so it doesn't silently follow a repoint onto a bridge it was never validated against.

`port`, `http_enabled`, `https_enabled`, and `https_port` are independently settable in the same request or separately. Both `http_enabled` and `https_enabled` default to `true` on a fresh install (ADR-0171, ports 80/443) — a genuinely fresh install just has HTTPS silently not come up at boot until a PKI root CA exists (non-fatal, logged), since there's no host certificate yet to serve TLS with:

```
PUT /v1/system/daemon-config
{"https_enabled": true}
```

Starts a second, independent listener on `https_port` (default `443`), reusing the already-issued PKI `"host"` leaf certificate (see [PKI](#pki-a-ca-chain-and-issued-leaf-certificates) below) — `500` if no root CA has been bootstrapped yet (`POST /pki/ca`), since there's no certificate to serve TLS with. Sent this way — an explicit `PUT`, as opposed to the one-time boot-time attempt — it's also how to bring HTTPS live *immediately*, with no reboot, right after bootstrapping PKI on a fresh install: the handler checks whether the listener is actually running, not just the persisted flag, so re-sending `{"https_enabled": true}` even though it's already the default still starts it for real once a certificate exists. `http_enabled` and `https_enabled` can each be toggled off, but never both in the same request (`400`) — cixd must always have at least one live listener, since (installed) it runs as real PID 1 with no "restart" to fall back on. Every change here — port, network repoint, HTTP/HTTPS toggle — is live immediately and also persisted, so it survives a real reboot.

Since cixd is PID 1 on an installed system, there is no way to reach it again over the network if it's ever pointed at an address you can't get to — double-check reachability of a new `management_network` (or a firewalled `https_port`) before relying on it as your only way in; physical console access (`docs/guides/installing.md`'s "Console login") is always the fallback.

## Per-source-IP throttling for failed HTTPS handshakes (ADR-0134)

```
GET /v1/system/tls-throttle
```

```json
{"enabled": true, "threshold": 20, "window_seconds": 60, "block_seconds": 300, "log_interval_seconds": 5}
```

Found live, not designed speculatively: a sustained flood of failed HTTPS handshakes from an untrusting client (several hundred/minute, since this daemon deliberately never does HTTP keep-alive — every attempt is a brand-new TCP+TLS connection) had no peer IP in its own log line and no way to stop the daemon spending a real `accept4()`+`SSL_new()`+`SSL_accept()` attempt on every single one. A source that fails `threshold` handshakes within `window_seconds` is refused outright — a bare `close()`, before any allocation or TLS negotiation — on **both** the HTTPS and plain HTTP listeners, for `block_seconds`. One shared in-memory table, checked once per `accept4()` regardless of which listener it came in on.

`log_interval_seconds` is a second, independent knob: caps how often a "handshake failed" log line is actually written for a given source (`0` logs every single failure). **Found live, again**: a legitimate desktop's own browser repeatedly failing TLS against an untrusted self-signed cert — not a hostile source — flooded the consolidated log store at 10+ lines/sec, crowding out everything else in its rotation window, long before `threshold` failures would ever justify a block. The failure is still counted toward `threshold`/`window_seconds` every time regardless of this setting — only the *logging* is rate-limited, never the accounting a real block still needs to be accurate.

```
PUT /v1/system/tls-throttle
{"threshold": 10}
```

Partial update — fields omitted are left unchanged, same convention `PUT /system/daemon-config`/`PUT /pkg/repo-config` already use. `enabled` defaults `true`; disabling it stops all enforcement without losing the configured thresholds. Persisted (survives a restart); the live tracking table itself is not — the same "an in-flight thing the daemon restarts through is simply lost" posture every other transient daemon state already has.

```
GET /v1/system/tls-throttle/status
```

```json
{"entries": [{"ip": "203.0.113.9", "fail_count": 24, "blocked": true, "blocked_until": 1786490300}]}
```

Every source currently tracked — live, read-only, in-memory state. `fail_count` is the count within the current rolling window; `blocked_until` is Unix seconds, `0` when not currently blocked. A clean, complete HTTP request (any status code — even a 404 proves the client speaks HTTP correctly) or a successful TLS handshake both reset a source's own count, so a client that had a handful of transient failures and then behaved normally isn't left one failure away from a block.

**Loopback (`127.0.0.1`) is never throttled or tracked, deliberately** — `cixctl`'s own default `--host=` is `127.0.0.1`, and since a block applies uniformly across both listeners, tripping it from loopback would lock out this daemon's own local admin access entirely, the same class of hazard as a firewall rule that can shut out its own operator. A genuinely hostile source is, by definition, never loopback.

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

A real, read-only `RTM_GETROUTE` dump (ADR-0066) — not a Cix-managed resource of its own, just a window onto real kernel state. Exists purely because a real installed box has no SSH and no general shell at all (ADR-0034): before this, there was no way to ever confirm what the kernel actually did with the `--gateway=` value given at install time (fed into a real route add, `rtnl_route_add_default_ipv4()`, that otherwise runs invisibly at boot). `gateway`/`interface` are `null` for on-link routes the kernel derives automatically from each network's own assigned address (`GET /networks`) — only routes with a real next-hop (like the upstream default route) carry a `gateway`. `protocol`/`scope` are the kernel's own raw `rtm_protocol`/`rtm_scope` values, not reinterpreted into names.

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
{"enabled": false, "size_mb": 0, "path": "", "disk": null}
```

```
POST /v1/system/swap
{"size_mb": 8192}
```

```
DELETE /v1/system/swap
```

One on-demand swap file, off by default (ADR-0069) — raised after a real Rust/wasm package build ran a freshly-installed box out of RAM. `POST` creates a real, fully-backed (never sparse) file of exactly `size_mb` megabytes, writes a genuine kernel swap-file header into it directly (no dependency on an external `mkswap` binary), and activates it via `swapon(2)`; `409` if swap is already enabled — `DELETE` (swapoff + remove) first to resize. `size_mb` must be in `[64, 1048576]`. Enabled state is persisted and re-applied automatically on every daemon start (including a real reboot) — best-effort, never blocks startup if the file is somehow missing or stale. On modern SSD/NVMe-backed storage, file-backed swap performs identically to a raw partition; a partition was deliberately not pursued here since this project's own install-time partition layout is fixed and repartitioning a live disk on demand is not a risk worth taking for this.

**Disk placement (issue #28)**: `POST /v1/system/swap` accepts an optional `disk` field —

```
POST /v1/system/swap
{"size_mb": 8192, "disk": "sdc"}
```

— placing the swap file on a real disk carrying the `swap` diskrole (`POST /diskroles {"disk": "sdc", "role": "swap"}` first) instead of the default OS-disk location. Validated at the exact moment of this call (present, mounted, currently carrying the `swap` role) — the same "operator names a disk, real validation happens where the real action happens" posture the [backup disk config](#backup-and-restore) already established, not a separate migration mechanism: there's no existing swap *content* worth preserving across a placement change, unlike state/rebuildable/log storage's own live directories, so this reuses `storageplacement.h`'s pure "which disk" pointer with no `storagemigrate.c`-style move job behind it. Every `POST` repoints, whether `disk` is given or not — omitting it explicitly means the default location, not "whatever the last call left it at." `GET`'s own `disk` field reflects the *configured* placement regardless of whether swap is currently enabled; `path` (like today) is only meaningful while enabled. A disk currently backing active swap placement is protected the same way the other four storage singletons already are — its role can't be removed nor the disk reformatted while it's the active swap placement (`409`).

## A consolidated log

```
GET /v1/system/logs?source=audit&level=info&tail=50&since=1700000000
GET /v1/system/logs?source=container&container=dns-1&regex=error
```

```json
[{"ts": 1700000012, "source": "audit", "level": "info", "container": "", "msg": "POST /v1/networks"}]
```

```
GET /v1/system/logs/config
PUT /v1/system/logs/config
{"max_bytes": 5368709120, "min_level": "info"}
```

One consolidated, size-capped log (ADR-0070): real kernel `dmesg` (source `kernel`, read directly from `/dev/kmsg`), cixd's own internal diagnostics (source `cixd`, mirrored to stderr too — stderr is still the only channel during boot, before the API is reachable), a per-request audit trail (source `audit`) — one entry per **mutating** (`POST`/`PUT`/`DELETE`) REST request this daemon handles, method + path, covering every real action either `cixctl` or the web dashboard takes since both are pure REST clients — and, since ADR-0126, **every container's own stdout/stderr, transparently** (source `container`, tagged with the `container` field) — no opt-in needed; a container's own `"capture_output":true` (see the Containers section) only additionally mirrors the same bytes into that one container's own `GET /containers/{name}` tail, it's a separate, still-opt-in feature fed from the same pipe. `container=` filters to one container's own lines; `regex=` (POSIX extended, case-insensitive) matches against `msg`, `400` on a malformed pattern. Every `GET` (not just `/health`) is excluded from the audit trail (ADR-0132) — a query is never an action, and the web dashboard's own routine ~30-request poll cycle every 2s would otherwise drown real actions in noise. All sources interleave into one chronologically-ordered store, not siloed per-source streams.

Storage is 8 rotating segment files, not a byte-exact ring buffer — the oldest whole segment is dropped once the configured `max_bytes` cap is reached (enforced at segment granularity, so expect a few percent of slop against the exact number, the same tradeoff `logrotate`/`journald` already make). `tail` defaults to 1000 and is capped at 5000; `since` is Unix seconds.

`PUT .../config`'s two fields — `max_bytes` and `min_level` — are independent; a real request only ever needs to give the one actually changing, and the response always echoes back the resulting full config. `min_level` (any real syslog severity name: `emerg`/`alert`/`crit`/`err` or `error`/`warning` or `warn`/`notice`/`info`/`debug`, default `debug` — log everything) is checked *at write time*, before an entry ever touches a segment file — genuinely different from `GET`'s own `level` query filter, which only ever filters what's already stored. Each captured log message itself is capped at 4096 bytes (`LOGSTORE_MSG_MAX`, raised from an original, too-small 512 after a real deployment failure's own build-output capture was silently truncated away before the actual error line) — a real build failure's captured output (`pkg %s@%s: build output: ...`) keeps the *tail* of the output, not the head, since the actual error is almost always the last thing printed.

### The kernel's own ring buffer

```
GET /v1/system/kmsg?tail=200
```

```json
{"entries": [{"ts_usec": 4213377, "priority": 4, "message": "overlayfs: fs on '/lower' does not support file handles, falling back to xino=off"}]}
```

`GET /system/logs` above already carries kernel entries — but only the ones cixd was running to witness, and only for as long as the size cap keeps them. This reads `/dev/kmsg` itself, so it also carries what the kernel said *before* cixd started, and is unaffected by the log store's own rotation. Use it when the question is what the kernel did; use `/system/logs` when the question is what happened on the host.

An installed host deliberately has no shell, so this is the only way to read dmesg on one. That is not hypothetical: it is what made overlayfs's own `mounting read-only` warning visible on a real box while proving out user namespaces (ADR-0179 phase 2c) — a message nothing else on the machine could have surfaced.

Entries come back oldest-first. `tail` defaults to 200 and is clamped to 1..512 (the daemon drains the buffer through a fixed 512-entry window, so asking for more cannot return more). `priority` is the raw syslog level from the kernel's own prefix — 0 `emerg` through 7 `debug`. A kernel whose `/dev/kmsg` cannot be opened answers `500`.

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

- `name` must match `[A-Za-z0-9_-]+` — it's used verbatim as the on-disk directory name under `/var/lib/cix/containers/`.
- `image` must already exist and be populated at `/var/lib/cix/rebuildable/images/{image}/rootfs` (ADR-0141) — the daemon never creates image content itself (see ADR-0004); a missing image is a `400`, not a silently-empty container.
- `memory_swap_max` (issue #52) is the one limit here where **0 is a real setting**, not "off": it means this container may not swap at all, so under memory pressure it is reclaimed or OOM-killed rather than pushed to disk — a genuine choice for a latency-sensitive workload on a box that does have swap. Omit the field for unlimited swap; a negative value is refused rather than silently meaning "unset", because omitting it is what means that. It reads back the same way: `0` stays `0`, `null` means no limit, and the two never collapse into one another. `GET /containers/{name}/stats` reports `memory.swap_current` alongside it — a limit with no way to see usage against it is half a feature. No kernel-config change was needed: `CONFIG_MEMCG_SWAP` no longer exists upstream (removed before 6.1), and `memory.swap.max` is registered unconditionally wherever memcg and `CONFIG_SWAP` are on, both of which this platform's kernel already had.
- `memory_max`/`pids_max`/`cpu_max`/`cpuset_cpus` are optional cgroup v2 limits; omit for no limit. `cpu_max` is the raw cgroup-native `"<quota> <period>"` string in microseconds (e.g. `"50000 100000"` = 50% of one CPU); `cpuset_cpus` is the raw `cpuset.cpus` range-list value (e.g. `"0-1,3"`), restricting which host CPUs this container's processes may run on. Both are passed straight through, not reinterpreted into a percentage or another unit — the same pass-through convention `memory_max`'s bytes and `pids_max`'s raw count already use. `memory_max`/`cpu_max`/`pids_max` (not `cpuset_cpus`) are echoed back on `GET /containers`/`GET /containers/{name}`, read live from the real cgroup rather than mirroring what was requested here — `null` for any not set. Previously write-only (settable here, but never retrievable again afterward) until Part 163 closed the gap.
- `disk_quota_bytes` is an optional, real, kernel-enforced hard limit on this container's own overlay upperdir — the enforcement mechanism is picked automatically from the backing filesystem's own type, no separate flag needed. On ext4 (the default), this is a project quota (see [ADR-0062](../adr/0062-ext4-project-disk-quotas.md)); writes past it fail with `EDQUOT` at the filesystem level, and it requires the containers partition to have real project-quota support (`mkfs.ext4 -O quota -E quotatype=prjquota`, the default for a system installed via `cix-install.c`). On btrfs, this is a qgroup hard limit set directly on the container's own upperdir subvolume (see [ADR-0103](../adr/0103-btrfs-quota-backend.md)) — equally real, kernel-enforced, not advisory. Either way, if the backing filesystem can't support the requested enforcement, creation fails `500` with a clear error rather than silently not enforcing the limit. Omit for no limit.
- `disk` is optional (task #638, [ADR-0102](../adr/0102-per-container-disk-selection.md)): a bare disk name (e.g. `"sdb"`, from `GET /disks`) to place this container's own writable storage on, instead of the default OS disk. The disk must already be mounted and carry the `"container-storage"` role (`POST /diskroles`) — `400` if it doesn't exist, isn't mounted, or lacks that role. Echoed back as `disk` on `GET /containers` (`null` for the default placement).
- `networks` is optional: 1–64 entries, each either a bare name (auto-allocated IP) or `{"name": "internal", "ip": "172.31.0.50"}` for an explicit, operator-chosen address — each network must already exist via `POST /v1/networks` (`400` if unknown), and an explicit `ip` must be a usable address on that network: in its subnet, not the reserved address/network address, and not already taken (`400`/`409`). Omit `networks` entirely for no networking (isolated netns, only `lo` — same as before this field existed). The **first** entry is primary and gets the default route; the rest only get their own subnet's connected route.
- `dns_register` is optional, default `false` — see [DNS: records + a real dnsmasq container](#dns-records--a-real-dnsmasq-container) below. Requires `networks` to be set (`400` otherwise).
- `pki_issue`/`pki_cert_dir`/`pki_days` are optional, default `false`/`/etc/cix-tls`/`365` — see [PKI: a CA chain and issued leaf certificates](#pki-a-ca-chain-and-issued-leaf-certificates) below. Requires the CA to already be bootstrapped (`400` otherwise); does **not** require `networks`.
- `ldap_provision`/`ldap_user`/`ldap_group`/`ldap_uid`/`ldap_secret_dir` are optional, default `false`/(container's own name)/(required when `ldap_provision` is true)/(auto-allocated)/`/etc/cix-ldap` — see [Automatic provisioning: ldap_provision](#automatic-provisioning-ldap_provision) below. Requires `ldap_group` to name an existing LDAP group (`400` otherwise); does **not** require `networks`.
- `files` is optional: up to 16 `{"path", "content", "mode", "owner", "group"}` entries, staged directly into the container's own upperdir before its process ever `execve()`s. `mode` is an octal permission string, default `"0644"`. `owner`/`group` (ADR-0144) are real, raw numeric uid/gid — never a username (no NSS lookup happens this early, and no ordering dependency on some other staged file like `/etc/passwd` existing first) — both default to root (this daemon's own real euid/egid). The one real reason to reach for these: a file only one specific non-root container process should be able to read (e.g. a bind credential a script running as a dedicated, non-root `AuthorizedKeysCommandUser` needs but nothing else in the container should) — `mode` alone can restrict *what* is allowed, but only `owner` can restrict *who*.

Response (`201`):

```json
{
  "name": "my-container",
  "status": "running",
  "pid": 12345,
  "exit_status": null,
  "term_signal": null,
  "exit_reason": null,
  "networks": [
    {"name": "internal", "ip": "172.31.0.2"},
    {"name": "dmz", "ip": "172.32.0.2"}
  ],
  "ip_forward": false
}
```

`exit_status` is the raw `waitid()` status and is ambiguous on its own — **`term_signal`** disambiguates it (issue #78): `0` means the container exited normally and `exit_status` is a real exit code; a nonzero `term_signal` is the signal that killed it, and `exit_status` is then that same signal number, *not* an exit code. Because a deliberate `stop`/`delete` `SIGKILL`s the container, `term_signal` is `9` for any container stopped or deleted while running — the reliable way to tell that apart from a genuine `exit 9`. Both are `null` while running.

`exit_reason` (ADR-0080) is a human-readable why once `exit_status` is non-null — either the container's own real diagnostic text (e.g. `"child: execve(/usr/bin/foo): No such file or directory"`), a signal string (e.g. `"killed by signal 9 (SIGKILL)"`) when `term_signal` is set, or, when neither is available, a fixed category string (e.g. `"clean exit"`, `"overlay: mount(2) itself failed"`). `GET .../{name}` and `GET /v1/containers` both include it the same way; a failure that also reaches `500` at creation time (before any process exists) is instead surfaced directly in that response's own error message and in `GET /system/logs`.

## Persisted containers and the `restart` policy

Every container is persisted at create time — its exact request is written to `/var/lib/cix/state/container_defs.json` (ADR-0141) — so it survives a daemon restart and is never destroyed by anything except `DELETE` (ADR-0181: *stop is stop, delete is delete*). The `restart` policy governs only whether, and when, it comes **back up on its own**, not whether it exists. A container with the default `restart: "no"` is fully persisted and kept; it just never auto-restarts:

```
POST /v1/containers
{
  "name": "router1",
  "image": "router",
  "cmd": ["/bin/bird", "-f"],
  "restart": "always"
}
```

For a policy other than `"no"`, the persisted request is replayed automatically at every future daemon boot, and again after any unprompted exit — each restart after a real, exponentially-backed-off delay (`restart_delay_seconds`, 1–300, default 2 — doubling per consecutive failure, capped at 30s, reset to the base value once the container has stayed up at least 30s before exiting again — never instant, so a genuinely crash-looping container doesn't hammer the host).

`restart` has four values:
- `"always"` — auto-restarts regardless of exit code, including a clean `0` exit.
- `"on-failure"` — auto-restarts only after a nonzero-exit/signal-killed exit, not a clean `0` exit; this only governs the crash-restart timer, boot-time autostart always attempts it regardless of how it last exited (that fact isn't persisted).
- `"unless-stopped"` — behaves like `"always"`, except a prior `POST .../stop` is remembered across a daemon restart (it won't auto-start again until explicitly started); the one policy where `stop` changes daemon-restart behavior.
- `"no"` (default) — **never** auto-restarted: not on its own exit, not at daemon boot, not by a rolling update. Still fully persisted and kept — after it exits it is retained (status `"exited"`, real exit code preserved, like Docker's `ps -a`), and a stop keeps it too (status `"stopped"`); bring it back with `POST .../start`, remove it with `DELETE`. (Before ADR-0181 `"no"` was ephemeral and a stop silently destroyed it — that surprise is fixed.)

`depends_on` controls the order persisted containers start in at boot:

```json
{"name": "router1", "image": "router", "cmd": ["/bin/bird", "-f"], "restart": "always", "depends_on": ["dns1"], "readiness": {"tcp_port": 53, "timeout_seconds": 10}}
```

`dns1` (itself persisted) is guaranteed to have been *started* before `router1` — and, if `dns1` sets its own `readiness` (`{"tcp_port": N, "timeout_seconds": N}`, requires `networks` to be non-empty), genuinely TCP-ready, not just process-started. Readiness is a plain, blocking `connect()` retried until it succeeds or `timeout_seconds` elapses, consulted in exactly one place — daemon-boot autostart, right before a dependent starts — best-effort: if it never succeeds, a warning is logged and boot proceeds anyway, never blocking or failing it. It is never consulted for a live `POST` or for crash-restart. A `depends_on` naming an unknown or non-persisted container, or forming a cycle, is skipped at boot (logged, not fatal to anything else starting).

`GET`/inspect responses always report the current `restart`/`restart_delay_seconds`/`stopped`/`depends_on`/`readiness` state, read live from the persisted definition rather than a stale echo of what creation was originally given.

## Registered-server health (issue #81)

Cix lets you register redundant backend servers for four subsystems &mdash; LDAP, DNS, NTP, syslog. Until this existed it only tracked *that* a server was registered, never whether it was actually serving: [#80](https://git.home.arpa/itdlabs/cix/issues/80) was a registered LDAP pair that answered on the wire but could not resolve anything under the configured base DN, and every login failed with nothing anywhere reporting a server as bad.

`GET /v1/system/server-health` reports every registered server across all four kinds:

```json
{"servers": [
  {"kind": "ldap", "container": "ldap-1", "state": "healthy", "in_service": true,
   "drained": false, "probe": "tcp:3893", "last_check_at": 1787400000,
   "last_ok_at": 1787400000, "consecutive_failures": 0, "last_error": null}
]}
```

- **`state`** &mdash; `healthy` / `unhealthy` / `unknown` (registered, not yet probed). One good probe makes a server healthy immediately; it takes several *consecutive* failures to declare it unhealthy, so a single dropped probe never pulls a working server out of service, while recovery is never delayed.
- **`in_service`** &mdash; the question consumers actually ask. False only when drained or confirmed unhealthy; a never-yet-probed server counts as in service, so switching health tracking on can never black-hole a working deployment during the first sweep.
- **`probe`** &mdash; how the verdict was reached, reported so `healthy` is never read as more than it is. `tcp:PORT` is a real service check (the server accepted a connection). `process` means only that the providing container is running &mdash; used for the UDP services (NTP, syslog), where a TCP connect would be a meaningless check dressed up as a real one.

**Servers that are not in service are withheld from generated client configuration.** Concretely: an unhealthy or drained LDAP server stops being handed to `ldap_client` containers. One deliberate safety rule &mdash; if filtering would leave *nothing*, the unfiltered list is used instead, because handing a client a possibly-down server beats handing it none at all.

This applies to an **explicitly configured `client_uri`** as well, not only to the list derived from registered servers (issue #84). It did not until that fix: an explicit list returned verbatim before any filtering ran, so every rule above was silently inert on any box that had one set &mdash; which included the real one, where draining a server changed nothing about what clients received. Health is keyed by container name and `client_uri` is free-form URIs, so each URI is mapped back to a registered server by resolving that server's live IP; two conservative rules bound it. A URI that maps to **no** registered server is left strictly alone (an operator may legitimately point at a directory this platform does not manage, and dropping it on evidence that does not exist would be far worse than not filtering), and a URI on a **different port** than the one health actually probes maps to nothing either &mdash; same IP, different port, is a different service, and dropping it on the strength of a probe that never touched it would be a guess dressed up as a health decision.

`GET /v1/ldap/config` reports **`effective_client_uri`** alongside the configured `client_uri`: what clients are handed right now, after derivation and after filtering. Configuration and effect are two different questions, and #84 stayed invisible for as long as it existed precisely because nothing anywhere answered the second one.

`PUT /v1/system/server-health/{kind}/{container}` with `{"drained": true|false}` is the operator override for maintenance. Draining is an *intent*, so unlike the observed health state it is persisted across a daemon restart; health itself is re-established by the next probe.

Probing runs on the daemon's own event loop as a **non-blocking** connect &mdash; a blocking probe against an unreachable server would stall the entire control plane, the exact failure [ADR-0180](../adr/0180-async-container-teardown.md) exists to prevent. `cixctl server-health [ls] | drain KIND NAME | undrain KIND NAME` and the dashboard's **System > Monitoring > Server Health** page are the CLI and web surfaces.


## Container lifecycle: start, stop, pause, unpause

`POST /v1/containers/{name}/stop` kills it now but keeps its persisted definition (and its on-disk state) — it reappears as status `"stopped"`, never vanishing, for **every** policy including `"no"` (ADR-0181: *stop is stop*). On the next daemon restart an `"always"`/`"on-failure"` container comes back (a fresh chance every boot), `"unless-stopped"` stays down until explicitly started, and `"no"` stays down always. `DELETE /v1/containers/{name}` is the only thing that removes a container — gone for good regardless of policy, it removes the persisted definition too, in the same call, and it won't come back on a pending crash-restart or any future boot. It also unmounts and recursively removes the container's own on-disk overlay directories (`upper`/`work`/`merged`, ADR-0106) — best-effort, a cleanup failure is logged but never turns the delete itself into an error, since the registry/definition state is the one source of truth for whether a container exists.

Every configured limit now reads back in `GET /containers/{name}` — `memory_max`/`cpu_max`/`pids_max`/`cpuset_cpus` live from the real cgroup, `disk_quota_bytes` from the creation request (#49) — and unknown fields in a create or recipe body are a `400` naming the offender instead of being silently dropped (#68); `cixctl ps` shows `cpuset=`/`disk_quota=`/`caps=` columns and the dashboard consolidates the resource envelope on the container's Hardware tab (#48/#69).

For a **running** container, both are asynchronous (ADR-0180, issue #67 — a synchronous kill-and-wait once froze the whole single-threaded daemon on a child stuck in uninterruptible D-state): the response means every durable effect is already applied (definition/ownership changes for delete, the stopped flag for stop) and the `SIGKILL` sent; reaping, registry release, and (for delete) the disk cleanup complete when the kernel confirms the exit, typically within one loop turn. Until then the container reports status `"deleting"`/`"stopping"`, a same-name create `409`s, and anything the unreaped process still holds isn't released yet — concretely, `DELETE` on a network it was attached to can transiently `409` ("still in use"). Chain on the settled state, not on the response: poll the container's own `GET` to `404` (delete) or status `"stopped"` (stop) first.

`POST /v1/containers/{name}/start` is the counterpart: brings a container that isn't currently running back to life without a daemon restart, replaying its persisted definition through the same creation path a daemon restart's own autostart already uses — one source of truth for "how a definition becomes a live container." It handles both non-running states persist-all produces: a `"stopped"` container (no live entry) and an `"exited"` one (ran and exited on its own, retained with its exit code — the stale exited record is cleared first, so a start always genuinely restarts it rather than no-opping). Idempotent — an already-**live** (running/paused) container is a plain `200`, not an error. `404` if no persisted definition exists for this name at all (this endpoint only ever replays an existing definition; `POST /containers` creates a new one). Unlike boot-time autostart, this endpoint ignores `restart: "unless-stopped"` gating entirely — an explicit, manual start always means start it.

`POST /v1/containers/{name}/pause` and `.../unpause` freeze/thaw a running container via the cgroup v2 freezer (`cgroup.freeze`) — every task in it is stopped at the kernel level, uninterceptable and unignorable, unlike `SIGSTOP` which a process can catch or handle. Both require an already-live container (`404` otherwise — pausing a stopped-but-defined or nonexistent container makes no sense); unlike `.../stop`'s idempotent double-call tolerance, pausing an already-paused container (or unpausing an already-running one) is a `409`, not a silent `200` — the caller should already know this from its last `GET`. A paused container's own on-disk state (cgroup leaf, upperdir, everything) is otherwise completely untouched — no stop/delete/recreate is involved, it's purely a freeze/thaw of already-running tasks.

`GET`/inspect responses report the current `"status"` (`"running"`/`"paused"`/`"stopped"`) live, not a stale echo.

## Host stats

```
GET /v1/system/stats
```

The host-wide counterpart to container stats below (ADR-0073) — mirrors its own conventions exactly: raw cumulative counters only, no server-side history, client computes its own deltas. Response shape:

```json
{
  "uptime": {"host_seconds": 157590, "daemon_seconds": 412},
  "load": {"load1": 0.42, "load5": 0.61, "load15": 0.55},
  "cpu": {"user_jiffies": 12345, "nice_jiffies": 0, "system_jiffies": 4321, "idle_jiffies": 987654, "iowait_jiffies": 12, "irq_jiffies": 0, "softirq_jiffies": 5, "steal_jiffies": 0, "pressure": {"some": {"avg10": 3.03, "avg60": 2.8, "avg300": 2.36, "total_usec": 5812475431}, "full": {"avg10": 1.06, "avg60": 0.63, "avg300": 0.44, "total_usec": 1272027550}}},
  "memory": {"total_bytes": 17179869184, "free_bytes": 10066632704, "available_bytes": 15828671488, "buffers_bytes": 0, "cached_bytes": 5375279104, "swap_total_bytes": 4294967296, "swap_free_bytes": 4294967296, "pressure": {"some": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 17464583}, "full": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 15910263}}},
  "disk": {"total_bytes": 105492467712, "free_bytes": 45524393984, "avail_bytes": 40869130240, "pressure": {"some": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 111434355}, "full": {"avg10": 0, "avg60": 0, "avg300": 0, "total_usec": 90687340}}},
  "networks": [{"name": "eth0", "rx_bytes": 1024, "tx_bytes": 2048, "rx_packets": 12, "tx_packets": 9}]
}
```

- `uptime.host_seconds` is `/proc/uptime`; `uptime.daemon_seconds` counts from this daemon process's own start. The two are separate on purpose — they diverge after a control-plane restart that was **not** a reboot (an update confirm, a crash and respawn), and "the box has been up for days but `cixd` restarted four minutes ago" is exactly the thing worth being able to see. The dashboard's status bar shows both.
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

v1 single-job constraint (same as disk format/ISO build/every other async job here): `409` if a ping is already in flight. `cixctl ping HOST` polls to completion and exits nonzero on an unreachable result, so it's usable directly in a script.

## The host's own outbound DNS resolver (ADR-0076)

```
PUT /v1/system/resolv
{"nameservers": ["1.1.1.1", "192.168.15.31"]}
```

A real, installed Cix host has no outbound DNS resolution mechanism at all by default -- `cixd`'s own `curl` subprocess (every `pkg_source` fetch, `pkg bootstrap --toolchain-url=`, `pkg hostbuild`'s git fetch) fails immediately against any real hostname. This endpoint fixes that directly: up to 3 IPv4 addresses (matching glibc's own resolver limit), persisted at `<data-dir>/resolv.conf` and bind-mounted onto the real `/etc/resolv.conf` at boot -- a `PUT` here takes effect **immediately**, no reboot needed, because it writes *in place* (same inode throughout, ADR-0132) to the same file the bind mount already points at, rather than the rename-based atomic write most other persisted state in this daemon uses -- a bind mount binds to the inode, not the path, so a rename-based write would silently swap in a new, unmounted inode and detach `/etc/resolv.conf`'s own bind mount from every write after the first one post-boot (confirmed the hard way; fixed in ADR-0132). An empty `nameservers` array clears it (falls back to no outbound resolution, the historical default).

A real internal `.home.arpa`/`.internal` DNS record (`POST /dns/records`, above) is **not**, by itself, resolvable by this host's own outbound `curl` -- that internal DNS is served by whatever container is registered via `POST /dns/servers` (e.g. `dns-1`/`dns-2`), a separate, containerized DNS server this endpoint has no relationship to unless that container's own IP is explicitly `PUT` here as a nameserver. If that container's own dnsmasq is also configured with real upstream forwarders (`PUT /dns/forwarders`, see [Upstream forwarders](#upstream-forwarders)), pointing this endpoint at it resolves both this platform's own internal names and real internet hostnames through the one configured resolver -- the practical fix for "I added a DNS record but the host still can't resolve it."

This is deliberately generic -- a plain IP list, no notion of "which container is my DNS server." It covers pointing at one of this platform's own DNS containers (resolve its IP once via `GET /containers/{name}`, `PUT` it here) and pointing at a real external resolver, with the exact same mechanism. `GET /v1/system/resolv` reports the current list.

Note this fixes host-level resolution generally, not just for `pkg`'s own fetches -- every current and future tool `cixd` shells out to (`git`, `openssl`, anything added later) resolves through the same, single, canonical `/etc/resolv.conf` path.

## Host-level sysctl (ADR-0160)

```
PUT /v1/system/sysctl/net.ipv4.ip_forward
{"value": "1"}
```

Distinct from the existing per-container `--sysctl=` flag (`POST /v1/containers`, create-time only, restricted to `net.*` keys applied inside that container's own netns): this endpoint writes directly against the **host's own** `/proc/sys`, with no key restriction -- anything writable there is writable through this endpoint, matching this project's own "fully open, no allowlist" scope decision. Dots translate to slashes the same way the kernel's own `sysctl` tool does (`net.ipv4.ip_forward` -> `/proc/sys/net/ipv4/ip_forward`); the key may not contain a literal `/` (`400` if it does -- that would escape the translated path).

Some kernel sysctl values are a single token; others (`net.ipv4.ip_local_port_range` being the standard example) are a whitespace-separated tuple. Rather than a curated table of which keys are which shape, this endpoint types generically by content: a `GET` or `PUT` value is a plain JSON string for a single-token value, or a JSON array of strings for a multi-token one -- always symmetric between what a `GET` reports and what a `PUT` accepts.

```
PUT /v1/system/sysctl/net.ipv4.ip_local_port_range
{"value": ["32768", "60999"]}
```

Every successful `PUT` takes effect immediately (a real `write()` against `/proc/sys`) and, by default, is also persisted to be reapplied automatically at every boot, right after configured kernel modules load and before the management network comes up. Pass `"persist": false` to write the live value without adding it to that boot-apply list -- useful for a one-off tuning change that shouldn't survive a reboot. `GET /v1/system/sysctl/{key}` always reports the true current live value, whether or not it's persisted. `DELETE /v1/system/sysctl/{key}` removes a key from the persisted boot-apply list only -- it never touches the live value (`404` if the key wasn't persisted to begin with). `GET /v1/system/sysctl` lists every currently-persisted key and its value; it is not a dump of the full kernel sysctl tree.

## Kernel module management (ADR-0159 Phase A)

```
GET /v1/system/kmod
```

A live `/proc/modules` read (no fork at all) -- every module the kernel currently has loaded, whether or not it has a persisted `kmod-config` entry below.

```
POST /v1/system/kmod/e1000e
{"options": {"debug": "1"}}
```

Real `modprobe <name> [key=value ...]` -- real dependency resolution (via the already-harvested `modules.dep`, see `kernel.recipe`/ADR-0061) and real module-parameter passing, neither reimplemented. `options` given in the body is used as-is; an omitted (or entirely absent) `options` field falls back to this module's own persisted `default_options` below, if one exists, otherwise loads with none. `DELETE /v1/system/kmod/{name}` is `modprobe -r <name>` -- real, reverse-dependency-aware removal, not a bare `rmmod` that would leave now-unused dependencies loaded. Both respond `404` if `modprobe` itself failed -- overwhelmingly "no such module" in practice, the one case a bare exit code can't usefully distinguish from "bad option" or "the real binary isn't staged on this box at all" (this project's own dev/build sandbox has neither `modprobe` nor `modinfo` -- only a real installed image does, via `kmod.recipe`).

```
GET /v1/system/kmod/e1000e
```

Real `modinfo <name>` output, parsed -- the one place this can't be replaced by `GET /v1/system/kmod` above, which only ever shows what's currently *loaded*, not what's available to load. Reports description, version, license, author, `depends` (an array), `in_tree` (built as part of this project's own kernel source vs. a genuinely third-party module), and `params` (every real `module_param()` the module declares, with its type and description) -- `404` if the module isn't built/available at all.

**Persisted per-module defaults + boot autoload** -- `kmod-config`, a small persisted table distinct from, and unrelated to, ADR-0061's own hardcoded, hardware-detection boot module list:

```
PUT /v1/system/kmod-config/e1000e
{"default_options": {"debug": "1"}, "autoload": true}
```

Read-modify-write, matching `daemon-config`'s own established `PUT` shape -- either field alone updates just that field (`400` if neither is given). `default_options` is what a bare `POST /v1/system/kmod/{name}` with no body falls back to; `autoload` marks this module for reload on every future boot, applied last of four ordered boot-time steps (`load_boot_modules()` -- ADR-0061's own fixed hardware-detection list -- then `apply_configured_sysctls()`, then the management network, then this one) -- deliberately last, since operator-configured autoload is the least boot-critical of the four. Best-effort per module at boot: a module that fails to load (hardware not present, or never actually got built) is logged and skipped, never a reason to fail boot, matching `load_boot_modules()`'s own established posture. `GET /v1/system/kmod-config` lists every module with a persisted entry; `DELETE /v1/system/kmod-config/{name}` clears both fields (`404` if there wasn't one) -- it never touches whether the module is currently loaded.

## Building an extra kernel module (ADR-0159 Phase B)

```
POST /v1/system/kmod-build
{"build_image": "kernel-builder", "config_symbols": ["CONFIG_DUMMY"]}
```

For a driver that isn't already in this platform's own curated `=m` module set, but does live in mainline Linux (the overwhelmingly common case) -- an ordinary hostbuild against the `kernel` recipe itself, the exact same mechanism `POST /pkg/hostbuild` drives (same shared pool of pkg-build chain slots, same `409`/`PkgEntry` response shape, progress polled the identical way via `GET /pkg/hostbuild/kernel`), gaining only an optional `config_symbols` list. Each entry (a bare `CONFIG_*` name) is merged into the same curated kernel config this platform already builds from, forced to `=m`, via a second `merge_config.sh` fragment -- strictly additive: an empty or omitted list reproduces the exact existing kernel build unchanged. `build_image` needs a real GCC toolchain and `kmod` installed, same requirement as any other kernel hostbuild (see [`kernel-build-and-ab-updates.md`](../guides/kernel-build-and-ab-updates.md)).

Deliberately **not** a new, separate, persistent kernel-build-tree mechanism -- an initial design draft proposed exactly that (a `KDIR` kept around indefinitely for on-demand module builds), abandoned per direct "keep the mechanics simple, no extra moving parts" feedback in favor of reusing the already-proven whole-kernel rebuild this platform's own `kernel.recipe` already does, which guarantees kernel/module ABI match by construction rather than needing new freshness-tracking bookkeeping. The real, honestly-stated tradeoff: this rebuilds the *whole* kernel (heavier than compiling one driver) and needs a reboot to take effect -- the resulting `bzImage` + `lib/modules/` tree only actually applies via the existing A/B kernel-update cutover, never a live, same-boot addition. Genuinely out-of-tree (third-party, not-in-mainline) module support is deliberately left undesigned until a real, concrete need for it shows up (ADR-0159's own Phase C).

A kernel build is exactly the shape of failure the `keep_on_failure` mechanism (ADR-0175, see [Debugging a failed build](#debugging-a-failed-build-keep_on_failure-adr-0175-issue-35) above) exists for -- pass `"keep_on_failure": true` here too (`cixctl kmod-build ... --keep-on-failure`) to preserve a crashing host-build container for real inspection instead of losing it the instant the build fails.

## NTP: host clock sync (ADR-0110)

Two related but genuinely independent mechanisms:

**1. The host's own clock.** A small hand-rolled SNTP client (the client subset of RFC 5905) -- setting the host's own clock needs `CAP_SYS_TIME` from the host's own namespace, which no container can do without either breaking isolation or `cixd` doing the `clock_settime()` call itself anyway, so this can never be delegated to a containerized workload the way DNS/dnsmasq or LDAP/glauth are.

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

Simpler than either: NTP is itself a live query/response protocol, so this is pure bookkeeping -- no pid/pidfd, no config-file push, no signal. `cixd` resolves the registered container's own live IP fresh at every sync attempt (never cached) and queries it directly with the exact same SNTP client as (1) -- a registered container is just one more candidate `ntp_sync_start()` tries, ahead of the configured upstream addresses. `404` if the named container doesn't exist or isn't currently running.

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

`-d` (not daemonizing) is the same "stay in the foreground, log to stderr" requirement every containerized service here has (`cixd` has no init to reap a forking child). `local stratum 10` makes this an orphan reference -- a stable internal time source that doesn't need real upstream internet reachability, appropriate for a LAN-internal NTP source other containers or the host register against; a real deployment wanting real wall-clock accuracy would add real `server`/`pool` directives instead (see (1) above for the DNS-resolution caveat that applies to any hostname used inside a container's own config on a real installed host). **The `/etc/passwd`/`/etc/group` files are not optional**, confirmed the hard way: `chronyd` calls `getpwnam()` to resolve its own `--with-user=root` compile-time default even though privilege-dropping itself is compiled out and it never actually changes UID -- with no `/etc/passwd` at all (this project's minimal images ship none by default), that lookup legitimately fails and `chronyd` exits immediately with `Fatal error : Could not get user/group ID of root`. `capture_output` is what makes this diagnosable at all; see [Diagnosing a container that starts but exits on its own](#diagnosing-a-container-that-starts-but-exits-on-its-own-capture_output) above.

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

## Syslog forward targets (ADR-0127)

`GET /system/logs` (below) stays the one source of truth this API and the web UI ever read from -- registering a syslog forward target is purely an optional, additional side channel for operators who already have real external syslog tooling and want this platform's container logs to reach it too:

```
POST /v1/syslog/targets
{"container": "syslog1"}
```

Mirrors `POST /v1/ntp/servers` exactly: pure bookkeeping, no pid/pidfd, `404` if the named container doesn't exist or isn't currently running, `409` if already registered. Once registered, every subsequent container-sourced log line (`source="container"` only -- kernel/cixd/audit entries are never forwarded) is also sent to it as a real RFC 3164 UDP datagram (`local0` facility; `HOSTNAME` is the originating container's own name; `TAG` is always `cixd`) -- fire-and-forget, silently dropped on any failure, since `GET /system/logs` already holds the durable, replayable copy of everything ever sent. A registered target never receives its own captured output forwarded back to itself.

A real `sysklogd` reference recipe (`sysklogd.recipe`, an unmodified upstream build) is this platform's own standard syslog-receiving container, the same "real protocol server as an ordinary containerized workload" precedent `dnsmasq.recipe`/`chrony.recipe` already established:

```
POST /v1/containers
{
  "name": "syslog1",
  "image": "syslog_server",
  "cmd": ["/usr/sbin/syslogd", "-F", "-K", "-n", "-H", "-P", "/run/syslogd.pid", "-C", "/run/syslogd.cache", "-f", "/etc/syslog.conf"],
  "networks": ["management"]
}
```

`-K` disables kernel-log reading (not meaningful inside a container), `-n` skips DNS lookups for senders (this project's own hostname-resolution caveats apply the same way here as everywhere else, see the NTP section above), `-P`/`-C` point at `/run` rather than the traditional `/var/run` (this platform's own minimal images have no `/var/run`, the same class of gap `chrony.recipe`'s own `--with-pidfile=/run/chronyd.pid` already worked around). **`-H` is not optional**: sysklogd's own default behavior for a remote-received datagram is to substitute the *sending* address as the logged hostname (cixd's own host IP, since every forward originates from the daemon's root netns) rather than trust the message's own embedded `HOSTNAME` field -- confirmed live, the difference between a `/var/log/messages` full of `cixd`'s own IP address on every line versus one correctly showing each real originating container's own name. `-H` tells it to trust the message's own `HOSTNAME` field instead, which is exactly what this daemon's own RFC 3164 datagrams already carry (ADR-0127). Received messages land in `/var/log/messages` inside the container's own persistent overlay upperdir (unlike `/run`, never reset on restart) -- any real external syslog tooling pointed at this container from outside sees the same standard, durable log file it would expect from any other syslog receiver.

### A container can already pull the full DNS record set itself

`GET /v1/dns/records` (below) is a real, working REST endpoint returning every current record as JSON -- any container that can route to the daemon's own bind address can already `curl` it directly and reformat the result into whatever its own DNS server software needs (a zone file, a different hosts format, etc.), as an alternative or supplement to the daemon pushing records into a registered server (`POST /v1/dns/servers`, below). No new mechanism needed for this -- it's a documented usage pattern of an endpoint that already exists, not a new capability.

## Container stats

```
GET /v1/containers/{name}/stats
```

A real, host-side, point-in-time snapshot — no in-container agent, no server-side history (the daemon never stores a sample; call again to get a fresh one, and compute rates/percentages client-side from consecutive samples, exactly what `cixctl stats` and the web dashboard's own Stats tab both do). Response shape:

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

## Host processes (ADR-0131)

```
GET /v1/system/processes
```

A real, direct `/proc` scan — every process on the box, not just ones this daemon itself spawned (host-level daemons, a shell an operator started over SSH into a jumpbox container's own console, everything). A point-in-time snapshot, same posture as `GET /system/stats`/`GET /containers/{name}/stats` above — no history, call again for a fresh one. Response shape (one entry per process):

```json
[{"pid": 1234, "ppid": 1, "comm": "chronyd", "command_line": "/usr/sbin/chronyd -d -f /etc/chrony.conf", "container": "ntp-1", "user_id": 0, "group_id": 0}]
```

`container` is populated by walking the process's own real host ppid chain against every currently-running container's own root pid — every container's own init is a direct `clone3()` child of `cixd` itself, so this reaches either a known container root (a match) or `cixd`'s own pid / pid 1 (empty string, a plain host-level process) for every case that matters. `command_line` is `/proc/<pid>/cmdline`'s own space-joined argv, or `"[comm]"` for a kernel thread (or a process caught between `execve()` calls) — the same convention `ps(1)` itself uses for an empty cmdline. `user_id`/`group_id` are the real (not effective/saved/filesystem) uid/gid from `/proc/<pid>/status`.
`state` and `wchan` (added alongside the build-stall reporter) are `/proc/<pid>/stat`'s state character and `/proc/<pid>/wchan` -- the kernel symbol a sleeping process is blocked in, empty while it is running. `wchan` is the field that turns "it is stuck" into a diagnosis, and it is worth knowing how to read: in a stalled build every process shows `do_wait`, which only means "waiting for a child" and says nothing, *except* the one process blocked on something else -- `__futex_wait` for a lock that will never be released, a filesystem symbol for I/O that never returns. That one is where the work actually stopped. Diagnosing a real gcc deadlock this way previously required `exec`ing into the container by hand, because this daemon collected everything about a process except what it was waiting for.


```
DELETE /v1/system/processes/{pid}
```

A real, immediate `SIGKILL` — no grace period, unlike `DELETE /containers/{name}` (which has real container-lifecycle semantics: the container's own restart policy, network/cgroup teardown, etc.). Refuses `pid 1` and this daemon's own real pid outright (`400`) — killing either would crash or reboot the whole host on a real installed system, where `cixd` runs as real PID 1. Every other pid is allowed, **including one that happens to belong to a running container** — killing a container's own init pid this way is exactly equivalent to that container crashing on its own; the existing `SIGCHLD`-driven exit handling already covers it correctly, restart policy and all. `404` for a pid that isn't currently running, `400` for a non-numeric path segment. `cixctl process ls`/`process kill PID` is the CLI surface.

## Reading a file back out of a container

```
GET /v1/containers/{name}/files?path=/etc/hosts
```

The read-path counterpart to `POST /containers`' own `files[]` (write-only, host-to-container, staged before the container's own `clone3()`). Response is raw bytes (`application/octet-stream`), not JSON — this daemon's only other non-JSON response besides the web dashboard's own static assets and the console WebSocket upgrade.

Path resolution depends on whether the container is currently running: while running, the file is read through `/proc/<pid>/root/<path>` (the container's own mount namespace and root, no privilege boundary crossed since the daemon already runs as real root). Once the container is down — **whether it exited on its own or was cleanly stopped** — the file is read from the container's own real, host-visible writable tree (its rootfs for a direct-rootfs container, its overlay upperdir for a classic one), falling back for an exited-but-still-registered container to the image's own read-only rootfs if the path was never written by the container itself.

A cleanly **stopped** container is a valid target (issue #162). It used to be a `404`, and the asymmetry was the confusing part: a clean stop removes the registry entry asynchronously, so this call raced that teardown and then failed permanently — while a container that exited *on its own* kept its entry and stayed readable indefinitely. An operator could read a crashed container's config but not a deliberately stopped one's, and nothing explained why. The container exists either way (startable, definition persisted, on-disk state intact), so the handler falls back to that persisted definition, taking the disk from the same `"disk"` field a start would use. Only a container with no definition at all is a `404`.

One limit worth stating plainly: for a stopped container this reaches its **writable** tree. A direct-rootfs container (the ADR-0207 default) has its whole filesystem there, so reads are complete; a legacy overlay container has only what it wrote, so a path existing solely in the image's lower layer is still a `404` while it is down — the lowerdir is recorded on the registry entry, and a stopped container has no entry to record it.

`path` must be an absolute, `/`-leading, traversal-free path (no `.`/`..` component, no empty `//` component) — the exact same validation `POST /containers`' own `files[].path` already applies, reused verbatim. A path resolving to a directory is `400`, not a directory listing — this endpoint reads one file, it does not browse a tree. `cixctl files get NAME --path=/some/path [--output=PATH]` is the CLI surface (stdout if `--output=` is omitted).

## Writing a file into an existing container, live, without a recreate

```
PUT /v1/containers/{name}/files?path=/etc/motd
{"content": "...", "mode": "0644", "owner": 0, "group": 0}
```

The write-path counterpart to the read above (ADR-0153) — same request shape as a `POST /containers` `files[]` entry (`content` required, `mode`/`owner`/`group` optional), applied to a container that already exists instead of staged at creation time. `204` on success.

Path resolution mirrors the read side's own running/not-running split: while running, written through `/proc/<pid>/root/<path>` (the overlay's own copy-up lands the result in the container's real upperdir, same as any in-container process writing that path would produce); once stopped or exited, written directly into the container's own writable tree. Same traversal-free, absolute-path validation as the read side.

A cleanly **stopped** container is a valid target here too (#162) — and this is the case that motivated the fix, since editing a file before starting a container is exactly the operation an operator wants on one that is down. It resolves to the same tree the next start will run from.

**Deliberately live and ephemeral, never persisted into the container's own `files[]` body** — a future restart or recreate replays the original persisted definition unchanged, with no memory of this write. If the change needs to survive a recreate, the durable path is a container recipe (ADR-0151): edit the recipe, `POST .../recipes/{name}/apply`. `cixctl files put NAME --path=/some/path --file=LOCAL_PATH [--mode=0644]` is the CLI surface.

## Attaching/detaching a network on an already-running container

```
POST /v1/containers/{name}/networks
{"name": "internal", "ip": "172.31.0.42"}
```

Same request shape as one entry of `POST /containers`' own `"networks"` array (a bare name string for an auto-allocated IP, or `{"name":..., "ip":...}` for an operator-chosen one) — attaches a network to a container that's already running, live, no recreate (ADR-0156). A real veth pair is created, moved into the running container's own network namespace via `setns()`, and the interface renamed/addressed/brought up from inside it — the identical mechanism `POST /containers` already uses at creation time, just performed against a live netns instead of a brand-new one. `200` with the container's full current state, networks included.

```
DELETE /v1/containers/{name}/networks/{network}
```

The reverse — but **only for a network attached this same live way**. A network attached the ordinary way, at container creation, gets `409` (not silently torn down): tearing it down mid-life would diverge the running container from its own persisted definition, which nothing else in this API does. `GET /containers/{name}`'s own `networks[]` entries each carry a `"live"` boolean so it's always clear which is which.

**Deliberately live and ephemeral, the same posture as `PUT .../files` above** — never persisted into the container's own definition; a restart or recreate replays the original `"networks"` list unchanged, any live attachment gone. The durable path is again a container recipe. `cixctl container network attach NAME --network=NETWORK [--ip=A.B.C.D]` / `container network detach NAME NETWORK` is the CLI surface.

**`cmd` has no equivalent, permanently, not just unimplemented** (ADR-0156): changing a running container's command means killing its own init process, which — per `pid_namespaces(7)` — makes the kernel `SIGKILL` every other process in that PID namespace and permanently retires it. There is no way to swap what a container runs without destroying and recreating its PID namespace, which is architecturally a real recreate regardless of what it's called.

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

`GET /v1/containers/{name}/console` opens a real, fully-interactive shell inside an already-running container (ADR-0043) — not a normal request/response endpoint, an HTTP/1.1 Upgrade to a hand-rolled RFC 6455 WebSocket (no fragmentation, 64KiB payload cap; OpenAPI 3.0 has no first-class way to type this, so `openapi.yaml` documents it as a GET whose success response is `101 Switching Protocols`). The exec'd process joins the target container's own mount/UTS/network/pid namespaces (`setns()`, equivalent to `nsenter --mount --uts --net --pid --target <pid>`) against a PTY allocated in the daemon's own namespace before any `setns()` call, so it never needs a working `devpts` inside the container itself. Command defaults to `/usr/bin/bash` (this project's own images stage everything under `usr/bin/`, never `/bin`); override with the `X-Cix-Exec-Cmd` request header.

Two real, ready-to-use clients — neither requires hand-rolling the handshake yourself:

- **`cixctl console NAME [--cmd=/path/to/shell]`** — a full, `termios` raw-mode terminal: tab completion, Ctrl-C, `vim`/`top`/`less` all work correctly, since it drives a real local terminal end to end.
- **The web dashboard's own Console tab** (a container's default view when selected in the left tree) — the browser's native `WebSocket` object talks directly to this endpoint, no hand-rolled handshake needed client-side. Deliberately reduced fidelity by design (ADR-0010's "no framework" constraint, confirmed with the user rather than silently accepted): a line-buffer renderer with `\r`/`\n`/backspace/Tab and SGR color support, no cursor-addressable screen model, so full-screen redraw programs (`vim`, `top`, `less`) render wrong there specifically — `cixctl console` has no such limitation.

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

On a successful upgrade, whatever of the build's output was already captured is sent immediately as one frame (so attaching mid-build doesn't start blind), then every further chunk streams live as the build produces it. The daemon sends a real WebSocket CLOSE frame — and tears every attached client down — the instant that specific build finishes, success or failure; there's no "reconnect and keep watching," since by definition there's nothing left to watch. `404` if no matching build is currently in progress; `503` if 4 clients are already attached to it (a soft cap, not a silent drop).

ADR-0157 Phase 2: up to two builds can genuinely be in flight at once (`pkg-build-config`'s configurable limit, Phase 3, will raise this default further). Optional `?name=`/`?image=` query params pick which one to attach to — omitting `name` falls back to "the one build in progress" when that's unambiguous (zero or one running); with two builds running at once, omitting `name` gets a `400` asking for an explicit one rather than guessing which the caller meant.

`cixctl pkg build-log` is the CLI client — no raw terminal mode, no input relay (this stream is one-way), just prints each chunk to stdout as it arrives and exits cleanly once the daemon's own CLOSE frame lands.

## DNS: records + a real dnsmasq container

DNS records are a REST resource; the actual name resolution is done by a real DNS server (dnsmasq recommended) running as a normal containerized workload — not hand-rolled, the same reasoning BIRD wasn't hand-rolled for routing (ADR-0007's "no external libraries" rule is about this project's own platform components, not about workloads a container runs).

```
POST /v1/dns/records
{"name": "db.internal", "ip": "172.31.0.5"}
```

- `name` is a hostname (dot-separated labels, `[A-Za-z0-9-]`, RFC 1035 length limits) — a different charset from network/container names, which don't allow dots. A `name` with no `.` at all gets this install's own site suffix appended by default (`<name>.<site_name>.<domain_suffix>`, see [This install's identity](#this-installs-identity-site-config) below) — fully overridable by including a `.`.
- `ip` must be well-formed IPv4.

`PUT /v1/dns/records/{name}` (task #749) edits an existing record's `ip` in place — `name` is authoritative from the URL path and, unlike `POST`, is never re-qualified with the site suffix (that qualification only ever applies at creation time). `400` on a malformed `ip`, `404` if no record with that name exists.

Once a container running dnsmasq exists (e.g. `cmd: ["/usr/sbin/dnsmasq", "-k", "-u", "root", "-p", "53", "-H", "/etc/dnsmasq-hosts", "-R", "-h", "--pid-file=", "--servers-file=/etc/dnsmasq-servers"]` — `-u root` since a minimal container image typically has no `/etc/passwd` for dnsmasq's default privilege drop to resolve; `-R`/`-h` skip `/etc/resolv.conf`/`/etc/hosts`, which likely don't exist either; `--pid-file=` (empty) disables dnsmasq's own default pidfile write, `/var/run/dnsmasq.pid` — confirmed the hard way, this project's own minimal images have no `/var/run` (only `/run`), so without this flag dnsmasq dies immediately with its own `EC_FILE` (exit status 3) on every single startup attempt, an instant, silent crash-loop under `restart: always` with no REST-visible cause; `--servers-file=` points dnsmasq at a file the daemon owns and rewrites, which is where upstream forwarders now come from — **not** the `--server=1.1.1.1 --server=8.8.8.8` command-line flags this example used to carry (ADR-0076), since baking them into a container's argv meant changing a forwarder required recreating core infrastructure and two replicas could silently disagree; see [Upstream forwarders](#upstream-forwarders) below), register it:

```
POST /v1/dns/servers
{"container": "dns1", "hosts_path": "/etc/dnsmasq-hosts"}
```

This writes every current record into `dns1`'s own `/etc/dnsmasq-hosts` (dnsmasq's `--addn-hosts` format) and sends `SIGHUP` so it reloads immediately. Every subsequent `POST`/`DELETE` on `/v1/dns/records` re-writes that file and re-signals `dns1` — genuinely live updates, not a one-time snapshot at registration.

The write itself goes through `/proc/<pid>/root/<hosts_path>` (the container's own filesystem view via the magic procfs symlink), not the container's raw upperdir directly — writing straight into a running container's upperdir does **not** reliably show up in its mounted view (confirmed empirically; the kernel documents this as unsupported/undefined for an already-mounted overlay). `/proc/<pid>/root/` correctly resolves through the container's real mount namespace without needing any new namespace-entry syscall.

### Provisioning DNS in one call

Standing up DNS by hand is four distinct steps — create each replica from its recipe, register each as a DNS server, read back the addresses they actually got, point the host resolver at them — and getting any one of them wrong leaves a half-configured service that looks healthy. This does all of it:

```
POST /v1/dns/provision
{"replicas": ["dns-1", "dns-2"], "set_resolver": true}
```

Both fields are optional. Omitting `replicas` uses this platform's own default topology (`dns-1`, `dns-2`), so the standard case needs no body at all.

**This is an endpoint rather than a client-side sequence on purpose.** It was briefly implemented as four calls driven from `cixctl`, which the API-First Mandate (see `CLAUDE.md`) forbids outright — and the moment the dashboard wanted the same button, that sequence would have had to be written a second time in JavaScript. The workflow is the product, so the workflow is an endpoint and both clients are thin.

- **Re-runnable by design.** A replica that already exists reports `"created": "exists"` and counts as success, not a conflict. Provisioning is precisely what an operator retries after fixing whatever failed the first time; a call that refused because half its work was already done would be useless there.
- **Per-replica reporting, not one overall status.** `200` when everything worked, `207 Multi-Status` when anything did not, with `created`/`registered`/`ip` for each replica. "Created but not registered" and "registered but the resolver was not updated" are genuinely different states to be stranded in, and recovery depends on knowing which one happened.
- **The address comes from the container, not the recipe.** The recipe is intent; the running container is fact, and only the fact is worth writing into `resolv.conf`.
- **The host resolver is set only if nothing failed.** Pointing the host at a resolver list that is missing a replica, or at one that was never registered, leaves the machine worse off than before the call.

`cixctl dns provision [--replica=NAME ...] [--no-resolver]` is the CLI surface — argument parsing and printing over this one call.

### Upstream forwarders

A registered DNS server is authoritative for this platform's own names and, on its own, nothing else — `-R` tells dnsmasq to ignore `/etc/resolv.conf`, so without upstream forwarders it has no way to answer for anything outside `.home.arpa`/`.internal`. The forwarders are what give it recursion:

```
GET /v1/dns/forwarders
PUT /v1/dns/forwarders
{"forwarders": ["1.1.1.1", "8.8.8.8"]}
```

These used to be `--server=` flags baked into each DNS container's `cmd` at creation time (ADR-0076). That had three concrete problems, all of which showed up in practice: changing a forwarder meant **recreating core infrastructure**, two replicas could **silently disagree** with each other because nothing tied their argv together, and nothing could answer "what does this platform resolve through?" at all — the value existed only inside a running container's command line.

They are now ordinary daemon-owned configuration. The daemon writes the list into a servers-file each dnsmasq reads (`--servers-file=/etc/dnsmasq-servers`) and `SIGHUP`s every registered server, so **one `PUT` updates every replica consistently** and a newly registered server picks up the current list at registration time without being told about it separately (the write happens before the binding is recorded, so a server is never left half-registered with a hosts file but no forwarders).

- The `PUT` **replaces** the whole list rather than appending; the response echoes the list as applied. At most 8 entries, each a valid IPv4 address — `400` otherwise, with nothing applied.
- An empty array is legal and means exactly what it says: authoritative-only, no upstream recursion.
- This is separate from [`/system/resolv`](#the-hosts-own-outbound-dns-resolver-adr-0076), which is what *this host itself* resolves through. The common setup points `/system/resolv` at the DNS containers and `/dns/forwarders` at real upstream resolvers, so host lookups resolve both internal names and the wider internet through one path.

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

LDAP server registration mirrors DNS server registration's own REST shape and persistence discipline, but with one real, deliberate difference: **registration never sends a signal**. `glauth` (`recipes/package/glauth/2.4.0/build.sh`, this platform's own standard integrable LDAP provider, replacing `lldap`) runs a real `fsnotify` watcher on its own config file whenever that file sets `watchconfig = true` — confirmed directly against glauth's own source (`v2/glauth.go`'s `startConfigWatcher()`) — and reloads automatically on any write. DNS server registration exists because dnsmasq only reads its hosts file once at startup; glauth has no equivalent gap to work around. Registration *does* immediately write into the container's own config file, though: the full current user/group set (task #726, so a fresh/replacement instance starts current instead of empty) and, as of ADR-0148, the `baseDN` line kept in sync with `hostauth-config`'s own real `ldap_base_dn`.

Once a running glauth container exists (its own config file staged at creation time, `datastore = "config"` and `watchconfig = true` set, the same `--file=` staging convention `lldap.recipe` established), register it so its config file's path is on record for the LDAP user/group CRUD endpoints below to render into:

```
POST /v1/ldap/servers
{"container": "ldap1", "config_path": "/etc/glauth/glauth.cfg"}
```

- `container` must already exist and be running (`404` otherwise, same rule `POST /v1/dns/servers` enforces).
- `config_path` is the container's own absolute view of glauth's config file (the one it was started with `-c`) — rejected (`400`) if not absolute or if it contains `..`. Nothing is read or written at registration time beyond a full user/group sync (see below); this call is otherwise pure bookkeeping.

`GET /v1/ldap/servers` lists every current binding; `DELETE /v1/ldap/servers/{container}` unregisters one (does not touch the container itself). Deleting the container automatically removes its binding (`ldap_server_forget()`, called from the same container-delete cleanup path as `dns_server_forget()`). Bindings are persisted (`<data-dir>/ldap_servers.json`) and survive a daemon restart, the same as DNS server bindings (ADR-0091).

### User/group CRUD

Same durable-record-store model DNS records use (`dns_record_create()`/`delete()`/`find()`): Cix itself is the source of truth for every user/group (persisted to `<data-dir>/ldap_users.json`/`ldap_groups.json`, survives a glauth container being deleted or rebuilt), and every create/update/delete re-renders the **full** current user/group set as glauth "config" datastore TOML and writes it into every currently-registered, currently-running server's own config file — the same always-whole-file-rewrite behavior `dns_write_hosts_file()`/`dns_server_sync_all()` already use for dnsmasq's hosts file. No signal is sent; glauth's own config watcher picks up the write. The write preserves everything above the first `[[users]]`/`[[groups]]` stanza in the container's current config file byte-for-byte (the operator's own `[ldap]`/`[api]`/TLS settings, and `[backend]`'s own `datastore`/`behaviors`), with one deliberate exception (ADR-0148): a `baseDN = "..."` line in that prefix, if present in that exact form, has its quoted value rewritten to match `hostauth-config`'s own real `ldap_base_dn` on every one of these same writes — the base DN is a real, canonical, API-owned value, not a copy the operator has to keep in sync by hand. A config that spells it differently (single quotes, or omits it) is left untouched; this never invents structure that isn't already there.

```
POST /v1/ldap/groups
{"name": "superheros", "gidnumber": 5501}

POST /v1/ldap/users
{"name": "j_doe", "uidnumber": 5001, "primarygroup": 5501, "mail": "j.doe@cix.internal", "password": "dogood"}
```

- Group `name`/user `name` follow POSIX-ish username rules (lowercase letters/digits/`_`/`-`, must start with a letter or `_`).
- A user's `primarygroup` must name an existing group's `gidnumber` (`404`-equivalent `LDAP_RECORD_ERR_GROUP_NOT_FOUND` otherwise) — create the group first.
- `password`, if given, is hashed with bcrypt (ADR-0144) and rendered hex-encoded into glauth's own `passbcrypt` config field (glauth's own config-backend bind path expects it hex-encoded, then decodes it back before comparing — see `daemon/src/ldap.c`'s `render_users_groups_toml()`) — never stored or echoed in plaintext, and never returned by any `GET` (only a `has_password` boolean is). Omitting `password` on `PUT .../users/{name}` leaves the existing credential unchanged.
- `secondary_groups` (ADR-0144) is an optional array of additional `gidnumber`s, each validated the same way `primarygroup` is; rendered as glauth's own `othergroups` array.
- `can_search` (ADR-0144 task #838) grants glauth's own minimal "search" capability (object `"*"`) — off by default. A real bind/service account (an `nslcd` `binddn`, a live `AuthorizedKeysCommand`'s own search bind) needs this to search the directory at all; ordinary user accounts don't. Settable directly on `POST`/`PUT` like any other field — the auto-provisioned service accounts below (task #727) are simply one caller among others now, not the only path that can grant it.
- `gidnumber`/`uidnumber` are optional on `POST` (task #748): omit either one and it's auto-allocated — `ldap_gid_alloc()`/`ldap_uid_alloc()` scan upward from a configurable floor (see below) for the next value not already in use, the same "scan for next free above floor" algorithm both have always used, just with the floor itself now settable instead of a hardcoded `10000`.

`GET`/`DELETE` follow the same `/v1/ldap/users/{name}` and `/v1/ldap/groups/{name}` shape as every other named resource in this API; `PUT /v1/ldap/users/{name}` updates an existing user (full field replacement, `password` optional as above); `PUT /v1/ldap/groups/{name}` updates an existing group's `gidnumber` (task #750) — unlike `POST`, `gidnumber` is always required in the body on `PUT` and is never auto-allocated, matching every other PUT resource's full-replacement semantics. gidnumber changes don't cascade to records referencing the old value: editing a group's `gidnumber` does not update any user's `primarygroup`, and callers are responsible for that themselves if needed.

**Renaming (ADR-0147)**: on either `PUT`, a real, different `name` in the body — not the URL path's own `{name}`, which always identifies WHICH record — renames it in place. Omitted, or identical to the current name, means no rename (the pre-existing behavior). A group rename has one real, deliberate cascade: if the group's old name is currently a member of `hostauth-config`'s own `admin_groups` list, that list is automatically rewritten to the new name too, before the rename is considered committed — a renamed admin group can never silently drop out of write-gating, the same class of incident ADR-0146 closed once already for a different cause. A user rename has no equivalent cascade (nothing else in this directory indexes a record by a user's own name; group membership is already by `gidnumber`, not name). Neither rename updates external config that referenced the record's old rendered LDAP DN (`cn=<user>,ou=<group>,<base-dn>`) — `nslcd.conf`'s own `binddn`, for instance — this daemon has no visibility into config it didn't itself render, so that stays the operator's own responsibility, same as it already was for a `gidnumber` change.

### Configurable uid/gid auto-allocation floor

```
GET /v1/ldap/config
{"start_uid": 10000, "start_gid": 10000}

PUT /v1/ldap/config
{"start_uid": 50000, "start_gid": 50000}
```

`start_uid`/`start_gid` (task #748) are the floors `ldap_uid_alloc()`/`ldap_gid_alloc()` scan upward from when `POST /v1/ldap/users`/`POST /v1/ldap/groups` omits `uidnumber`/`gidnumber`. Both default to `10000` until changed. Setting a new floor takes effect immediately for the *next* auto-allocation only — it never renumbers any user or group that already exists, and both values must be positive integers (`400` otherwise). Persisted to `<data-dir>/ldap_config.json`, survives a daemon restart.

### Real, live-LDAP SSH login (ADR-0144 task #838)

`sshd` queries LDAP live, at connection time, instead of reading files Cix pre-renders — this platform's one real SSH-login mechanism (an earlier, file-rendered alternative from task #731 existed briefly during ADR-0144's own development and was retired once this one was proven working end to end; see [ADR-0145](../adr/0145-retire-file-rendered-ssh-target-sync.md) for why). Needs `openssh` built `--with-pam` (`recipes/package/openssh/10.4p1-8/build.sh` onward -- `-8` specifically for a real, standard-OpenSSH fix: with `UsePAM yes`, PAM's own conversation (and therefore `pam_ldap.so`'s nslcd-socket conversation) runs inside sshd's own privsep pre-auth child, which `chroot()`s to `--with-privsep-path` before that conversation happens; `-8` relocates that path from `/var/empty` to `/run/sshd-empty` so a same-filesystem hard link of `/run/nslcd/socket` into the chroot is reachable at all, closing a real "invalid user"/PAM-auth-failure gap found live), plus `linux-pam` and `nss-pam-ldapd` (Parts 116-118) already installed on the target image. Not a container-creation flag — this is real per-container runtime configuration, staged the same way `glauth.cfg`/`dnsmasq.conf`/`chrony.conf` already are, via the container's own `files` entries at creation time:

- `/etc/nslcd.conf` (mode `0600`, root-only): `uri`/`base`/`binddn`/`bindpw` pointing at the real LDAP servers (`ldap-1`/`ldap-2`) and a real bind/service account carrying `can_search: true` (see below) — `nslcd` itself runs as root in this project's minimal images (no `uid`/`gid` directive; matches `chrony.recipe`'s own "no privilege drop" precedent), so no separate account is needed just for it to read its own config. Also needs `pam_authc_ppolicy no` — `glauth`'s `config` backend doesn't handle the LDAP password-policy control `nslcd` requests by default, and answers with a spurious `Invalid credentials` rather than ignoring the unsupported control, confirmed live.
- `/etc/nsswitch.conf`: `passwd`/`group`/`shadow: files ldap` — routes `getpwnam()`/`getgrnam()` through `nslcd`'s NSS module for any account not already in the container's own local `/etc/passwd`.
- `/etc/pam.d/sshd`: `auth`/`account` each try `pam_unix.so` first (`sufficient` — succeeds for local-only accounts like `root`, silently falls through for LDAP accounts since glauth exposes no real shadow-compatible hash via NSS), then `pam_ldap.so` (`sufficient` — a real bind-as-user LDAP verification, the actual password check for every LDAP account), then `pam_deny.so` (`required` — reject anything neither step accepted).
- `AuthorizedKeysCommand`/`AuthorizedKeysCommandUser` in `sshd_config`, plus `UsePAM yes` and `PasswordAuthentication yes` (both default to `no` even in a `--with-pam` build, see `openssh/10.4p1-7`'s own header comment; `10.4p1-8`'s own header comment covers the separate privsep-chroot fix) — the command script itself does a real `ldapsearch` for the connecting user's `sshPublicKey` attribute (rendered by `render_users_groups_toml()`, see the `ssh_public_key` field above) and prints it as an `authorized_keys` line. Must be invoked via an explicit interpreter (`AuthorizedKeysCommand /usr/bin/bash /usr/sbin/ldap-authorized-keys %u`, not a bare shebang path) — confirmed live that `sshd`'s own direct `execve()` of a shebang script fails with `ENOEXEC` in this project's own TCC-built binary/kernel combination, root cause not further isolated once the working fix was found; a real, environment-specific gap worth remembering for any future `subprocess()`-style exec of a script from this exact `openssh` build.
- `can_search` (exposed as a real public API/CLI field by this same task, see the `LdapUser` schema above): grant it to one real, durable, named service account (e.g. `svc-nslcd`) and use that account's DN/password as both `nslcd.conf`'s own `binddn`/`bindpw` and the `AuthorizedKeysCommand` script's own bind credentials — glauth requires a real, capability-granted bind for any search, confirmed live (an anonymous bind gets `50 Insufficient access` even with `IgnoreCapabilities = true` set, which only bypasses *capability* checks for an already-authenticated bind, not the bind requirement itself).
- A real, unrelated gap this task also found and fixed: every standard device node (`/dev/null` etc., `pkg_seed_image_baseline()`) ended up mode `0644` instead of the real `0666` requested — `mknod()`'s own requested permission bits are subject to whatever umask the daemon process inherits (POSIX), and nothing in this codebase ever called `umask(0)`. Fixed with an explicit `chmod()` immediately after `mknod()` (both `pkg_seed_image_baseline()` and `container_dev_mknod()`) rather than a process-wide `umask(0)` call. Confirmed the hard way: `AuthorizedKeysCommandUser`'s own script, running as an unprivileged, non-root user for the first time anywhere in this project, was the first process to ever actually need to *write* to `/dev/null` — every prior daemon in this project's own images runs as root, which a mode-`0644` `/dev/null` never blocks.
- LDAP users intended for real SSH login need a real `loginshell` set (e.g. `/usr/bin/bash` — this project's own real path, not `/bin/bash`, see `CLAUDE.md`) — an empty `loginshell` renders as glauth's own default, which doesn't resolve on this project's minimal images (`sshd` rejects the login: "User ... not allowed because shell ... does not exist"). **The symptom is actively misleading and has cost real debugging time**: `sshd` treats an account whose shell doesn't exist as invalid and then deliberately runs the PAM/LDAP auth with a *bogus* credential (so it leaks no information about whether the account exists), so the client sees a plain `Permission denied` and `nslcd -d` logs `ldap_sasl_bind(...) → Invalid credentials` for the user's own DN — pointing straight at the password, which is entirely correct the whole time. Confirmed by A/B: the identical account and password authenticate the moment `loginshell` is changed to a path that exists, and a bare `ldapsearch -D <user-dn> -w <password>` bind succeeds against glauth throughout. If a login fails with `Invalid credentials` in the nslcd log but a direct `ldapsearch` bind as that same user works, check `loginshell` before touching the password.

Verified end-to-end against a real throwaway LDAP account (created, tested, deleted) on the real `jumpbox1` container: real pubkey login via a live `AuthorizedKeysCommand` LDAP query, real password login via `pam_ldap.so`'s own live bind-as-user check, and a wrong password correctly rejected.

### Who may log in here: `ldap_allow_groups` (issue #76)

An `ldap_client` container hands every account in the directory to the container's own NSS and PAM stack, which means every account can log into every such container. `ldap_allow_groups` on `POST /v1/containers` narrows that to named groups:

```json
{"name": "jumpbox1", "image": "jumpbox", "cmd": ["/usr/sbin/sshd", "-D"],
 "ldap_client": true, "ldap_allow_groups": ["jumpusers", "admins"]}
```

which renders one extra line into the staged `/etc/nslcd.conf`:

```
pam_authz_search (&(objectClass=posixAccount)(uid=$username)(|(memberOf=ou=jumpusers,ou=groups,dc=cix,dc=internal)(memberOf=ou=admins,ou=groups,dc=cix,dc=internal)))
```

`nslcd` runs that search *after* authenticating and refuses the login when it matches nothing, so the restriction is enforced by the same daemon that resolves the account — not by anything of Cix's that has to still be running for it to hold. Group membership is the mechanism because it is what this directory actually supports: a user entry carries `memberOf` DNs of the form `ou=<group>,ou=groups,<base-dn>` for both its primary *and* its secondary groups (verified live against the deployed glauth), and a user can be in many groups, which is exactly what "may log into several hosts" needs. The `host` attribute idiom this would traditionally use needs custom-attribute support glauth's `config` backend does not expose.

Three rules, all enforced at creation time so a mistake is a `400` rather than a container that silently admits everyone:

- **Every named group must already exist** — a typo is refused, naming the offending group, instead of rendering a filter that matches nobody and locks the container out entirely.
- **Names are refused, never escaped** — this string is written into a file `nslcd` parses as an LDAP filter, and a group name is already constrained to a plain character set everywhere else in this daemon.
- **`ldap_allow_groups` without `ldap_client` is refused** — there is nothing to restrict without it, and silently ignoring it would read as a restriction that is in force when it is not.

Omitting the field stages no `pam_authz_search` at all, which is the pre-existing behaviour: any valid account may log in. That absence is deliberate — a filter written to allow everyone is a filter that can be got wrong.

`cixctl container run ... --ldap-client --ldap-allow-group=jumpusers --ldap-allow-group=admins` is the CLI form; the web dashboard's container form carries it as a comma-separated field under the LDAP login checkbox.

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
- `ldap_secret_dir` is optional, default `/etc/cix-ldap` — where the delivered `bind.secret` (chmod `0600`) lands inside the container's own filesystem.
- A `"search"` capability (`object = "*"`) is granted by default — glauth denies all LDAP operations by default otherwise, and a bind-only account with zero capabilities couldn't do anything useful.
- The secret itself is generated fresh from `/dev/urandom` (`ldap_generate_secret()`, 16 raw bytes hex-encoded to 32 characters) on every single fire of this hook, including every `restart:"always"` respawn — **the plaintext secret is never persisted anywhere in Cix's own state**, only its bcrypt hash (`passbcrypt`, see above) survives in the durable user record. This means a respawned container always gets both a fresh secret and a fresh delivery, even though the underlying account (same name, same owner) already existed — tolerated the same way `pki_issue` tolerates re-delivery to a respawned container's fresh pid.
- The account's `owner` field is set to the container's own name and it is automatically removed when the container is deleted (`ldap_user_forget_owner()`, called from the same container-delete cleanup path as `dns_record_forget_owner()`/`pki_cert_forget_owner()`).

## PKI: a CA chain and issued leaf certificates

A single internal root CA, an optional second intermediate tier, and leaf certificate issuance. Actual cryptography (keypair generation, CSR signing) is done by the daemon shelling out to the system's real, unmodified `openssl` binary as a short-lived subprocess — the same "real software, not hand-rolled" reasoning BIRD and dnsmasq were chosen under (ADR-0007's "no external libraries" rule governs this project's own platform components, not real software it invokes or runs as a workload).

Bootstrap the root CA once:

```
POST /v1/pki/ca
{"common_name": "Cix Root CA", "days": 3650}
```

Both fields are optional (shown defaults). **The CA private key is never returned over the API, in any endpoint, ever** — it's the root of trust and must never leave the host. A second `POST /v1/pki/ca` is a `409` — `pki_ca_create()` is a one-shot by design; see [Regenerating the whole chain](#regenerating-the-whole-chain-post-pkireset) below for the real "start over" operation.

### A second, intermediate CA tier

```
POST /v1/pki/intermediate
{"common_name": "Cix Intermediate CA", "days": 1825}
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
  "pki_cert_dir": "/etc/cix-tls",
  "pki_days": 365
}
```

`pki_cert_dir` and `pki_days` are optional (shown defaults). This issues a cert named `web` (CN and sole SAN) and writes `tls.crt`/`tls.key` (chmod 0600) into `/etc/cix-tls` **inside the `web` container's own filesystem** — the same `/proc/<pid>/root/<path>` mechanism `POST /v1/dns/servers` already uses to reach into a running container (ADR-0013), just delivering a cert+key instead of a hosts file. Unlike DNS server bindings, delivery is **one-time**: there's no live resync, since a cert doesn't change after a container starts — except after a `/pki/reset` (below), which explicitly redelivers to every still-live container that owns a reissued leaf. `GET /v1/pki/certs/web` shows `"owner": "web"`; deleting the `web` container automatically removes its cert (both the index entry and the on-disk key/cert files) — a manually-created cert is never touched by any container's deletion, even if it happens to share that container's name but wasn't the one that created it.

Unlike `dns_register`, `pki_issue` does **not** require `networks` — the cert identifies the container by name, not by IP, and delivery works for any running container regardless of networking. It **does** require the CA to already be bootstrapped, checked upfront as a `400` (you can't issue a cert with no CA). A *name collision* discovered only at issuance time (e.g. a stale cert persisted from a same-named container created before a daemon restart) is best-effort instead: issuance is silently skipped rather than overwriting it, and the container is still created successfully.

This install also always keeps a `"host"` leaf current for itself, auto-(re)issued whenever `PUT /system/site` changes this install's identity, or whenever the CA chain changes at all — nothing an operator needs to request separately. Its own `"owner"` is `"__host"` (ADR-0128), a reserved sentinel distinguishing "owned by the daemon itself" from a plain `null` owner — the CLI and web dashboard both render it as "host (this daemon)" rather than the raw sentinel string. Same sentinel on the auto-maintained instance DNS record mentioned above.

### Regenerating the whole chain: `POST /pki/reset`

`pki_ca_create()`/`pki_intermediate_create()` are one-shot by design and refuse a second call outright (`409`) — this is the explicit, real "start over" operation that design deliberately doesn't provide implicitly:

```
POST /v1/pki/reset
{"root_common_name": "Cix Root CA - lab.internal", "intermediate_common_name": "Cix Intermediate CA - lab.internal"}
```

Destructive: deletes the root (and the intermediate, if one was bootstrapped) and every leaf's on-disk key/cert, then re-bootstraps the root (and intermediate, only if one existed before this call) with new common names (each defaults to `"Cix Root/Intermediate CA - <domain_suffix>"` if omitted), then reissues every leaf that was tracked beforehand — same name/SANs/owner, a fresh keypair and validity period for each. Every reissued leaf's `cert_pem` **and** `key_pem` are included in the response — a genuine new issuance moment for each, the identical "returned exactly once, right now" treatment a leaf's key already gets at first issuance. A leaf whose reissue itself fails is simply gone, not left in its old state, since its old key/cert (signed by a CA that no longer exists the instant this proceeds) are already unlinked before any reissue is attempted. Any leaf owned by a still-live, `pki_issue`-created container is automatically redelivered into that container's own filesystem afterward, so a running service's `tls.crt`/`tls.key` don't go stale.

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

- `devices` is optional: 0–N entries, each a discovered device id from `GET /v1/devices` (`"pci:..."`, `"usb:..."`, `"gpu:N"` for a whole GPU — `gpu:N` is never itself listed by `GET /v1/devices`, only its individual member nodes are — or `"disk:<name>"` for a raw whole disk, ADR-0142), **or** the name of a persistent device mapping (below). Real `/dev` nodes are granted via a `BPF_CGROUP_DEVICE` program on the container's own cgroup (ADR-0017) — nothing else on the host can reach them once bound. A bare `gpu:N` id expands into every node that physical GPU needs in one grant (DRM `cardN`/`renderDN` plus the shared `/dev/kfd` compute node) — see ADR-0028/ADR-0029. A `"disk:<name>"` entry is any non-OS, role-less whole disk `GET /disks` reports — fresh, unlabeled media straight from a USB enclosure or a PCI-passthrough disk with no filesystem/mount concept of its own, handed to the container as a raw block device; a disk already carrying a role (`POST /diskroles`) is never listed here, since it's already owned by this daemon's own storage-placement system.
- `interfaces` is optional: 0–N real host network interface names (e.g. `"eth1"`) moved directly into the container's own netns (not a veth pair) — fd-anchored teardown, correct even if the container crashes mid-move. See ADR-0022.
- `GET /v1/containers` echoes the real, expanded grants actually made, not an echo of what was requested.

### Composite USB devices (ADR-0161 Phase A)

The passthrough unit is always the whole device — never an individual USB interface, even for a composite device (a combo HID+storage device, say). `GET /v1/devices` still reports what such a device is actually made of, purely descriptively:

```json
{"id": "usb:0a12:4007:...", "bus": "usb", "assignable": true,
 "interfaces": [{"number": 0, "class": "03", "subclass": "00", "protocol": "00"},
                {"number": 1, "class": "01", "subclass": "01", "protocol": "00"}]}
```

`class`/`subclass`/`protocol` are the real `bInterfaceClass`/`bInterfaceSubClass`/`bInterfaceProtocol` values (2 hex digits each). Empty for every non-USB device, and for a USB device with only the one implicit interface a single-function device already has.

### A device the container wants but doesn't have yet (ADR-0161 Phase B)

```json
{"devices": [{"id": "printer-map", "optional": true}]}
```

Alongside the bare-string shorthand (`{"id": ..., "optional": false}`), a `devices[]` entry can opt into being **optional**: if `id` doesn't currently resolve to a present, assignable device, container creation still succeeds — without that grant — instead of the usual `400`. The reference itself is remembered (`GET /containers/{name}`'s own `pending_devices` field echoes it) for later automatic hotplug attachment or a manual live-attach call (below). `optional` defaults `false`, so every existing bare-string caller's behavior is unchanged.

### Live device attach/detach + real hotplug reaction (ADR-0161 Phases C/D)

```
POST /v1/containers/{name}/devices
{"id": "usb:0a12:4007:ABCDEF0123456789"}
```

Grants one more device to an **already-running** container, live — no recreate. Same request shape as a creation-time `devices[]` bare-string entry (a raw id or a devicemap name, resolved fresh); the underlying `BPF_CGROUP_DEVICE` program is atomically replaced (empirically verified against 2,000,000 concurrent `open()` attempts spanning the swap — see ADR-0161's own "Phase D verification" for the real probe), so there's no window where an already-granted device is briefly ungranted or unrestricted. `404` if `id` doesn't currently resolve to a present, assignable device; `409` if the container is already at the maximum device count. Deliberately live and ephemeral, the same posture `PUT .../files`/`POST .../networks` already established — never persisted into the container's own create-request body.

```
DELETE /v1/containers/{name}/devices/{id}
```

The reverse — but **only for a device attached this same live way**. A device granted at container creation gets `409` (recreate the container to remove it), the same rule `DELETE .../networks/{network}` already enforces for networks; `GET /containers/{name}`'s own `devices[]` entries each carry a `"live"` boolean so it's always clear which is which.

This same live-attach/detach primitive is also what the daemon's own hotplug reaction calls internally, automatically: a persistent `NETLINK_KOBJECT_UEVENT` listener reacts to real kernel USB add/remove events. On a real `add`, every running container's own `pending_devices` entries are re-resolved against current hardware — a match live-attaches it, removing the entry from `pending_devices`. **Contention**: if more than one running container's own pending reference would resolve to the exact same newly-appeared device, it's granted to **neither** (logged, not silently arbitrated by scan order) — give the devices distinct devicemap names instead of matching the same raw vendor:product pair if this matters. On a real `remove`, every currently-granted device (creation-time or live, either one) whose underlying hardware can no longer be found has its grant **actively revoked** — never left in place as an inert grant for hardware that's gone. Every hotplug-driven grant/revoke is written to the consolidated log (`GET /system/logs`).

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

Real host block devices, live-enumerated from `/sys/class/block` on every call, the same "real hardware, never persisted" convention `GET /devices` already established. Since task #844, every partition on a disk is also listed as its own ordinary entry (`is_partition`/`parent_disk`) alongside the whole disk itself — a partition is independently assignable everywhere a whole disk name is accepted (`POST /diskroles`, `POST /disks/{name}/format`, and see "Partition-level disk management" below). `model`/`removable` are always empty/`false` for a partition entry — neither sysfs attribute exists per-partition, only on the parent whole disk. `is_os_disk` flags the one disk holding this platform's own fixed ESP/root-a/root-b/config/containers layout, and (propagated) every one of its own partitions — never a candidate for a role of its own or for formatting/repartitioning; every other disk or partition is available for a role assignment and, once role-assigned, formatting.

`mounted`/`mount_path` are real, current ground truth read fresh from `/proc/mounts` on every call — true if any partition on the disk (or the whole-disk device itself) is currently mounted, regardless of whether this daemon is the one that mounted it. Deliberately independent of the disk format job's own `state` (`GET /diskformat/{name}`, `"ready"` once a format+mount this daemon itself ran succeeds), which is purely in-memory, per-daemon-process state — forgotten across a restart even though the real mount persists, and blind to a disk mounted by hand or from before this mechanism existed. `GET /disks` is the one place to check whether a disk is *actually* mounted right now.

`reads_completed`/`writes_completed`/`sectors_read`/`sectors_written`/`io_time_ms` (ADR-0142) are real, live counters straight from `/sys/block/<name>/stat` — monotonically increasing since boot, the same "poll and difference two samples yourself for a rate" convention this API's other cumulative counters already use, never a rate this daemon computes on your behalf. `io_time_ms` (the kernel's own "time spent doing I/Os") is the closest real, cheaply-available proxy for disk I/O delay/business this daemon exposes. `used_bytes`/`free_bytes` are real `statvfs(2)` numbers for `mount_path` — only meaningful when `mounted` is true; an unmounted disk has no filesystem context to ask, and both are `0` in that case (check `mounted`, don't infer it from these being zero).

### Persisted disk roles

```
POST /v1/diskroles
{"disk_name": "sdb", "role": "backup"}
```

`role` is `"container-storage"`, `"backup"`, `"state-storage"`, `"rebuildable-storage"`, `"log-storage"`, or `"swap"` (ADR-0141, `swap` added by issue #28) — a small, fixed, closed vocabulary, not an arbitrary operator-chosen string the way a `devicemap` name is, since a role only means something insofar as this daemon actually understands and acts on it. `state-storage`/`rebuildable-storage`/`log-storage`/`swap` are each a daemon-wide *singleton* placement: several disks can carry the same one of these roles as eligible candidates, but only one is ever the currently-active placement, tracked separately (`GET /system/state-storage`, `GET /system/swap`, etc.), not by this role field alone. `swap` reconciles with the existing on-demand host swap *file* mechanism (`POST /system/swap`, ADR-0069) rather than duplicating it — the role names which disk is *allowed* to back an operator-chosen placement; `POST /system/swap`'s own `disk` field is what actually resolves and activates it (see [A host swap file](#a-host-swap-file) above).

Real and creatable even for a `disk_name` that isn't currently present (`present: false` on `GET`, not an error — the same tolerant convention `/devicemaps` already established for hardware that might be temporarily absent). Rejected (`400`) for the **fixed OS layout** — the OS disk itself, and its first four partitions (the ESP, both root slots and `/config`), the same set `protected: true` marks on `GET /disks`. A partition an operator appended into the OS disk's reserved free space is *not* part of that layout and can be given a role like any other disk: being able to create such a partition and then never use it was the gap (issue #9). `409` if `disk_name` already has a role (`DELETE` it first to reassign, the same no-silent-overwrite convention `/devicemaps` already established).

`fs_type` is present on a `GET`/`POST` response once this disk has been successfully formatted at least once (`POST /disks/{name}/format`) — persisted here specifically so it survives a restart even though the format job's own `state` (below) doesn't: at daemon startup, every present, unmounted, role-assigned disk with a remembered `fs_type` is automatically remounted with it (ADR-0142), closing what used to be a real gap where a formatted-but-unmounted disk after a reboot needed a manual, destructive re-format to recover.

### Format + mount

```
POST /v1/disks/sdb/format
{"confirm_disk_name": "sdb", "fs_type": "btrfs"}
```

Multi-disk management Phase C: destructively formats and mounts a disk that already has an assigned role (`POST /diskroles` — a disk with no role is `400`, `"assign one via POST /v1/diskroles first"`). Deliberately a **separate, explicit** action from role assignment — assigning a role never has a destructive side effect of its own — confirmed with the operator during design rather than assumed. `confirm_disk_name` in the request body must match `disk_name` in the URL exactly (`400` otherwise): a deliberate double-confirmation before overwriting every byte of existing content on the disk. Always rejected for the OS disk, same as role assignment.

`fs_type` is optional (ADR-0104), `"ext4"` (the default, omit the field entirely for the original behavior) or `"btrfs"` — `400` for any other value. `"btrfs"` requires a real `mkfs.btrfs` to actually be staged on this box (`btrfs-progs.recipe`, via a real `cix-hosttools` image); if it isn't, the job still starts but fails fast with `"mkfs.btrfs failed"` once the child process's own exec attempt hits `ENOENT` — the same failure shape any other `mkfs` failure already has, not a special case.

Async, like every other potentially-slow host operation this daemon runs (`pkg install`, ISO assembly, `pkg bootstrap --toolchain-url=`) — `POST` returns `202` immediately with the job's initial status; poll `GET` on the same path for completion:

```
GET /v1/disks/sdb/format
{"disk_name": "sdb", "state": "ready", "fs_type": "btrfs", "mount_path": "/var/lib/cix/disks/sdb"}
```

`state` is `"none"` (no job has ever run for this disk — including when a job ran/is running for a *different* disk, so a status check never shows another disk's unrelated job), `"running"`, `"ready"`, or `"failed"` (`error` distinguishes `mkfs.<fs_type>` failing outright from it succeeding but the subsequent `mount(2)` failing). Only one format job may run daemon-wide at a time (`409` otherwise) — the same v1 single-job constraint every other async job here already has. Mounted at a fixed path under this platform's own data directory by default; a `container-storage`-role disk can also be selected explicitly per container via `POST /containers`' own `disk` field (ADR-0102, Phase D, already built).

### Unmount (issue #34)

```
POST /v1/disks/sdb/unmount
{"confirm_disk_name": "sdb"}
```

Found live: a disk with its role already removed (`DELETE /diskroles/{name}`) had no way to actually be let go of — it just stayed mounted forever, and a real, currently-mounted-but-role-less disk was found to correlate with a genuine `mountns_pivot()` `EXDEV` ("Invalid cross-device link") failure in every subsequent container creation on the box where it was found. A real, synchronous `umount2(2)` against whatever `GET /disks` currently reports as this disk's own `mount_path` — synchronous, not async like format, since a plain `umount2(2)` is fast. Unlike `POST .../format`, this never touches the filesystem's own on-disk content, only its attachment to the running system; a later `POST .../format` (destructive) or a real `mount(2)` are the only ways back. Deliberately does **not** require an assigned role the way format does — the opposite precondition direction, since unmounting a role-less disk is exactly the case this exists to cover. Same double-confirmation shape as format, and the identical two data-safety checks: `409` if this disk is the active placement for state-storage, log-storage, rebuildable-storage, or swap, or the currently configured backup-config disk, or a `container-storage`-role disk one or more containers currently have their own storage on — unmounting any of those out from under whatever relies on it would break it the instant this succeeds, migrate away first. `400` if the disk is the OS disk, doesn't exist, or isn't currently mounted at all. Response is the disk's own current state (`GET /disks` shape):

```
{"name": "sdb", "mounted": false, "mount_path": "", ...}
```

### Partition-level disk management (task #844, ADR-0158)

```
POST /v1/disks/sdb/partition-table
{"confirm_disk_name": "sdb"}
```

Writes a fresh, empty GPT partition table to a whole disk — destructive (wipes any existing table and everything on it), same double-confirmation as `POST .../format`. Rejected (`400`) if `disk_name` is itself a partition or the OS disk, `409` if it already has a role assigned directly to it or is currently mounted. Synchronous — unlike `mkfs`, writing a partition table is metadata-only and near-instantaneous, so there's no async job/poll shape here.

```
POST /v1/disks/sdb/partitions
{"name": "data", "size_mib": 51200}
```

Appends one new partition to a disk's existing table via `sfdisk --append` — never touches any partition already on the disk. `name` is the new partition's cosmetic GPT name attribute; `GET /disks` reports the new partition by its real kernel device name (e.g. `"sdb2"`), never by this. `size_mib` is optional — omit it (or `0`) to consume all remaining space on the disk (must be the last partition added, if so). Same preconditions as `partition-table` above: a disk in whole-disk role/format use must never also be partitioned underneath that use. Response is every partition currently on the disk after the add:

```
{"disk_name": "sdb", "partitions": [{"name": "sdb1", "is_partition": true, "parent_disk": "sdb", ...}]}
```

```
DELETE /v1/disks/sdb/partitions/sdb1
```

Removes one partition — every other partition on the disk is untouched. `404` if `partition_name` doesn't currently exist or doesn't actually belong to `disk_name`; `400` if it isn't actually a partition at all (a whole disk name given where a partition was expected); `409` if it's part of the OS disk's own layout, still has a role assigned (`DELETE /diskroles/{name}` first, same no-silent-data-loss convention role removal already has elsewhere), or is currently mounted.

A disk is used in exactly one of two mutually-exclusive modes: role assigned directly to the whole disk (the original model), or partitioned with roles assigned to the individual partitions instead — `diskrole.c`/`diskformat.c` needed no code changes of their own for this, since both already operate purely on whatever `GET /disks` reports, partition or whole disk alike.

`POST /disks/{name}/partitions/{part}/resize` grows a partition. **Grow only, deliberately** — shrinking is not the mirror image of growing: the filesystem has to shrink *first*, and cutting the table entry before that destroys the tail of a live filesystem. Refusing is the difference between an operation that cannot lose data and one that can.

It does **both halves** of the job: the table entry, then the filesystem inside it. Growing only the entry would leave the extra space invisible to everything using the filesystem, which reads as the resize having silently done nothing. That is also why it is limited to ext4 or an unformatted partition — a btrfs filesystem is refused rather than half-grown, because `btrfs filesystem resize` needs the filesystem *mounted* and this operation needs it unmounted, making it a genuinely different flow rather than another binary to call. `resize2fs` requires a clean filesystem, so `e2fsck -f -p` runs first; preen mode makes only the automatic, unambiguous repairs and refuses anything needing a human, and if it refuses, the partition table is left unchanged and you are told to check by hand.

Only free space **immediately after** the partition can be used. Free space elsewhere on the disk cannot extend it, so a `409` here is about adjacency, not about the disk being full.

`GET /disks/{name}/free-space` answers what will actually fit before you ask for it. It looks like a client could subtract the reported partition sizes from the disk size instead — it can't: that misses partition alignment, the GPT's own reserved areas at both ends, and any gap an earlier delete left in the middle. Two numbers come back and they are genuinely different: `total_free_bytes` and `largest_free_bytes`, the latter being what actually bounds one new partition, since free space split across several gaps can't be handed to a single request. `has_partition_table` is reported separately rather than inferred from a zero total, because "partitioned and full" and "not partitioned yet" are different problems with different fixes and sfdisk reports the second as no output at all. It is its own endpoint rather than a field on `GET /disks` because it forks a subprocess and `GET /disks` runs on every poll for every disk.

A partition also carries `part_label` — its own name in the GPT, which is what created it actually called it. The fixed OS layout carries `cix-esp`, `cix-root-a`, `cix-root-b`, `cix-config` and `cix-containers`; a partition created through `POST /disks/{name}/partitions` carries the `name` that request gave it. That field was **write-only** until this was reported: the API accepted it, the installer set it, and nothing could ever read it back — which left the five most load-bearing devices on the box showing as indistinguishable partitions with no role at all. It is read straight from the partition table rather than via `/dev/disk/by-partlabel` (which needs udev and device nodes) or by forking sfdisk (a subprocess per partition per poll).

`role` and `part_label` answer two different questions and both matter: `role` is what an **operator assigned**, which the OS partitions never have and never should, while `part_label` is what they **are**. Clients showing only the first make the OS layout look unused.

Disks and partitions come back in a stable, meaningful order rather than `readdir()`'s: disks sorted naturally by name (so `dm-2` precedes `dm-10`), each partition grouped under its parent and ordered by **start sector** — its real order on the disk, which is also the order that makes "the free space after this one" obvious to read.

Every disk and partition row also carries `fs_type` and `role` (issue #90). `fs_type` is read from the device's own superblock — `ext4`, `btrfs`, `vfat`, `swap`, `squashfs`, or `""` for none recognised, which on a partition means genuinely unformatted. It is answered regardless of role, which is the point: the OS layout carries no role at all, and its partitions are exactly where "is this formatted, and as what" is least guessable. It is read directly rather than by shelling out to `blkid`, because a compiled-in binary path that was never staged into the control-plane image is precisely how partitioning came to be broken for a whole release (see above). `role` is joined in here rather than leaving every client to fetch `GET /diskroles` and match by name, which is what every client was doing.

`mounted` on a partition means that partition itself is mounted, and `mount_path` is its own mountpoint. A whole disk carries a separate `has_mounted_partition` for "something on this disk is in use" — that is what makes `POST .../partition-table` and `POST .../partitions` return `409`, and it is reported so an operator with no shell can see why. The two were one field until partitions became addressable, at which point a genuinely mounted partition started reporting `mounted: false` while its parent reported the partition's mountpoint as its own. That mattered because `DELETE .../partitions/{name}` reads exactly this flag to refuse deleting a partition that is in use — confirmed on real hardware: the delete returned `204` and removed a partition whose ext4 was live and mounted.

## Multi-disk storage placement (ADR-0141)

```
GET /v1/system/state-storage
{"disk": null}
```

Which disk (if any) is the *active* placement for Cix's own state — networks, DNS/LDAP/NTP/syslog-forwarding config, PKI (CA keys and every issued cert), container definitions, device mappings, site identity, and daemon/rolling-restart/TLS-throttle configuration. `disk: null` is the default OS-disk placement, unchanged from before this feature existed. Deliberately excludes container workload data (never covered), rebuildable content (images/packages/artifacts — its own separate `rebuildable-storage` concern, not yet exposed via REST), and logs (`log-storage`, its own endpoint below) — see `docs/adr/0141-multi-disk-storage-placement.md` for the full role/multiplicity model.

```
POST /v1/system/state-storage/migrate
{"disk": "sdc"}
```

`disk` is required — a real disk name that already carries the `state-storage` role (`POST /diskroles`) and is currently mounted, or `null` to migrate *back* to the default OS-disk placement (a real, symmetric operation, not a dead end once you've moved off the OS disk once). Omitting the field entirely is a `400` — deliberately distinct from an explicit `null`, since the two mean different things. `404` for an unknown disk; `400` for the OS disk itself, a disk with the wrong role, or a role-correct disk that isn't currently mounted; `409` if a migration is already running or the requested disk is already the active placement.

Async and genuinely live — the daemon keeps operating normally against the *current* location for the entire bulk-copy phase, no pause, no readonly window:

```
GET /v1/system/state-storage/migrate
{"state": "ready", "disk": "sdc"}
```

`state` is `"none"`, `"running"`, `"ready"`, or `"failed"` (`error` present on failure). Once the bulk copy (a forked child, `treecopy_recursive()` — the same permission-preserving primitive `POST /system/backup-config/snapshot-now`'s own future implementation and this daemon's package-install pipeline both use, never a second copy of the same logic) finishes successfully, this daemon's own single-threaded reactor does one more synchronous pass — a second, fast copy (cheap, since little changes during the bulk phase) — and only then repoints every affected subsystem's own live path, re-establishes the real `/etc/resolv.conf` bind mount against the new location (a real, previously-live lesson: a bind mount is tied to the inode it captured, not the path — see `CHANGELOG.md`'s Part 103), persists the new placement, and removes the old location's data. A `state:"failed"` migration at any point before that final repoint leaves the daemon still using the old location, completely untouched — the partially-copied new-location data is left for inspection or the next attempt to overwrite.

`DELETE /v1/diskroles/{name}` and `POST /v1/disks/{name}/format` both now refuse (`409`) against a disk that's the current active state-storage, log-storage, or rebuildable-storage placement, *or* the currently configured backup-config disk, *or* a `container-storage`-role disk one or more containers currently have their own storage on (`ADR-0142`, see below) — removing the role or destroying the disk's content out from under a live placement would silently strand the daemon's own state (or a container's own workload data). Migrate away first (`disk: null` back to the default, or to a different role-eligible disk — or, for backup-config, `PUT /system/backup-config` with a different disk/`null` — or, for a container, `POST /containers/{name}/migrate-storage`).

### Log-storage placement (ADR-0141 Phase 3)

```
GET  /v1/system/log-storage              {"disk": null}
GET  /v1/system/log-storage/migrate      {"state": "none"}
POST /v1/system/log-storage/migrate      {"disk": "sdd"}
```

Identical contract to state-storage above, for where the consolidated log store (kernel dmesg, this daemon's own diagnostics, the per-request audit trail, every container's stdout/stderr — ADR-0070/ADR-0126) lives, with its own independent job slot (a state-storage migration and a log-storage migration can run concurrently, each acting on its own kind). The one real difference under the hood, not in the contract: `logstore.c` holds a persistently-open file handle across writes (`ensure_current_segment_open()` only reopens when it's `NULL`, not per write) — the finalize step closes it and lets the next write naturally reopen (in append mode, resuming the same logical segment) against the already-migrated new location, rather than leaving it silently still writing to the old disk.

### Rebuildable-storage placement (ADR-0141 Phase 4)

```
GET  /v1/system/rebuildable-storage              {"disk": null}
GET  /v1/system/rebuildable-storage/migrate      {"state": "none"}
POST /v1/system/rebuildable-storage/migrate      {"disk": "sde"}
```

Same contract again, for where container images, installed packages, build artifacts, and staged ISOs live — content that's regenerable from recipes/sources, never irreplaceable, the reason it's a separate concern from state-storage in the first place. Its own independent job slot, same as log-storage. Every consumer (`image.c`, and `pkg.c`'s several distinct concerns — package state, repo-sync config, cache config, artifact-fetch config, image-recipe-apply state) was confirmed to cache nothing but plain path strings with no persistently-open handle of its own, so the finalize step is a straightforward repoint across all of them, no `logstore.c`-style extra care needed.

### Container-storage migration (ADR-0142)

```
GET  /v1/containers/{name}/migrate-storage      {"state": "none"}
POST /v1/containers/{name}/migrate-storage      {"disk": "sdc"}
```

Same `{"disk": "name"|null}` contract as the three daemon-wide kinds above, narrowed to one container's own overlay storage (the `disk` field `POST /v1/containers` already accepts at creation time, `ADR-0102`, now movable after the fact too). Requires a persisted definition to restart from — `restart` other than `"no"` — since the cutover below always ends with a real stop-then-replay; a `"no"`-policy container is rejected with `400`, not silently accepted and left half-migrated.

The one real difference from state/log/rebuildable-storage's own contract: those three are daemon-wide singletons this single-threaded reactor alone reads/writes, so repointing a cached path while the bulk copy runs is safe. A container's own overlay is actively read/written by that container's own live process the entire time, so the bulk async copy here is knowingly a first, possibly-stale pass — once it finishes, this daemon briefly **stops the container**, does one more synchronous copy pass (catching anything written since), patches the persisted definition's own `disk` field, and **automatically restarts** the container from the new location, the same `create_container_from_body()` replay `POST /containers/{name}/start` and a crash-restart both already use. A failure at any point after the stop — the final copy pass, or the restart itself — brings the container back up from its **original** location instead of leaving it down: the whole migration is all-or-nothing from the caller's point of view, not a partial state to reconcile by hand.

`404` for an unknown container; `400` for a missing/invalid `disk` field, an unknown/OS disk, a disk with the wrong role, a role-correct disk that isn't currently mounted, or a container with no persisted definition; `409` if a migration for this container is already running or the requested placement is already active.

`DELETE /v1/diskroles/{name}` and `POST /v1/disks/{name}/format` also now refuse (`409`) against a `container-storage`-role disk that one or more real containers currently have their own storage on — the exact same class of protection the four daemon-wide placements already had, extended to cover a gap that predated this ADR entirely (the original `POST /containers` `disk` field, `ADR-0102`, never had this safety check until migrate-storage gave the whole project a reason to add it). Migrate the container(s) away first via this endpoint.

## Per-container config files + sysctls + env

```
POST /v1/containers
{
  "name": "router2",
  "image": "router",
  "cmd": ["/usr/bin/bash", "/usr/local/bin/pbr.sh"],
  "files": [{"path": "/etc/bird.conf", "content": "...", "mode": "0644"}],
  "sysctls": {"net.ipv4.conf.all.rp_filter": "0"},
  "env": {"DEBUG": "1", "LOG_LEVEL": "info"}
}
```

- `files` is optional: 0–N `{path, content, mode}` entries, staged directly onto the container's own filesystem *before* its process ever `execve()`s — so `cmd` can point straight at a staged script (e.g. `pbr.sh` above). `path` must be absolute with no `.`/`..` component (`400` otherwise); `content` is bounded at 64KiB per file. `GET`/`PUT /containers/{name}/files?path=...` (above) are the read/write counterparts for after the container already exists — `PUT` there is live/ephemeral only, unlike this creation-time `files[]`, which is part of the container's real persisted definition.
- `sysctls` is optional: an object of up to 32 `"key": "value"` entries; `key` must start with `net.` (the one sysctl subtree the kernel actually namespaces end to end — `400` for anything else, a real security boundary, not incidental). Applied inside the container's own netns right after `clone3()`, the same mechanism `ip_forward` already uses.
- `env` is optional: an object of up to 32 `"KEY": "VALUE"` entries, merged into the container's own process environment at `execve()` time — a container gets *only* these, never a copy of `cixd`'s own environment. `KEY` must be a POSIX-portable environment variable name (letters, digits, underscore, not starting with a digit; `400` otherwise). This is a real runtime value, not a config-templating mechanism — compare to `files[]`'s own `{{SECRET:KEY}}` substitution (a one-time, build-time text fill-in with no runtime presence at all, see container recipes' `{{SECRET:KEY}}` substitution further below); `env` is the same shape `docker run -e KEY=VALUE` gives a container.
- All three survive exactly like everything else in `restart: "always"`'s own replay mechanism — no separate persistence work needed. See ADR-0030.
- `cmd` itself is echoed back on every `GET /containers`/`GET /containers/{name}` response (`ADR-0100`) — previously there was no way to ask a running or stopped container "what is your entrypoint," since the create request's own `argv` only ever pointed into that request's transient parsed body. `env` is echoed back the same way.

## Persistent volumes (issue #88, ADR-0183)

Everything a container writes at runtime lives in its overlay upper layer, and `DELETE` removes that layer outright ([ADR-0106](../adr/0106-container-delete-disk-cleanup.md)). A **volume** is the one exception: a named directory whose lifetime is independent of any container using it.

```
POST /v1/volumes
{"name": "jump-home"}

POST /v1/containers
{
  "name": "jump",
  "image": "jumpbox",
  "cmd": ["/usr/bin/bash", "/usr/local/bin/jumpbox-start.sh"],
  "volumes": [{"name": "jump-home", "path": "/home"}]
}
```

That `/home` now survives the container being deleted and recreated — which for a container with `follow_rolling: true` happens on every image rebuild, without anyone asking for it. This is exactly the case that raised the feature: a jump box recreated about a dozen times in one working session, taking every shell history and dotfile with it, silently.

- `volumes` is optional: 0–8 `{name, path, read_only}` entries. `name` must be an **existing** volume — an unknown name is a hard `400`, never an implicit create, because a typo silently producing a fresh empty volume is exactly how someone loses data and then concludes persistence "didn't work". `path` is absolute, with no `..`, and is created inside the container if the image has no such directory (so a volume can be mounted at a `/home` a minimal rootfs never had).
- The bind mount happens inside the container's own mount namespace, before `pivot_root`, so nothing accumulates on the host — the mounts go away with the namespace.
- **A volume that fails to mount kills the container** (exit 125) rather than starting without it. A container silently running without its persistent storage would write to the overlay upper layer instead, look perfectly healthy, and lose that data on its next recreate. Same for `read_only: true`: if the read-only remount fails, the container fails to start rather than coming up writable, because a guarantee that silently isn't one is worse than none.
- `PUT /volumes/{name}/quota` gives a volume a real, kernel-enforced size limit — an ext4 project quota on its own directory, the same mechanism container overlays have had since [ADR-0062](../adr/0062-ext4-project-disk-quotas.md), so a write past it fails with `EDQUOT` at the filesystem level rather than being noticed afterwards. Without one, a volume can grow until its disk is full, and it becomes an unbounded thing to copy when scheduled backups run. On a **btrfs**-backed volume it is refused rather than accepted and left unenforced — there the limit lives on a qgroup attached to a subvolume, and a volume directory is not one; accepting a limit that is not in force is worse than refusing it, because the operator then believes it exists. It is re-applied automatically after a migrate, since the project-quota tag lives on the directory and a volume that moved would otherwise arrive untagged and unlimited.
- `POST /volumes/{name}/migrate` moves a volume's data to another disk or partition. A volume's placement was fixed at create time, which for storage that deliberately outlives its containers is the wrong thing to decide once and never revisit. The order is the safety property: copy, persist the new placement, and only then remove the original — a failure at any point leaves the volume still pointing at data that exists, whereas repointing first would open a window where it points somewhere the data has not reached. Refused while a container mounting it is **running**, because a bind mount resolves to a host path once at container start; a stopped or exited container picks up the new location on its next start like any other definition change. Synchronous, unlike container-storage migration (ADR-0142), which has to be async because it moves storage while the container keeps running — nothing is running here.
- `GET /containers`/`GET /containers/{name}` echo a container's `volumes` back by **name**, not by resolved host path, for both a running and a stopped container -- a field the API accepts and then never echoes is unverifiable from the outside, the same write-only gap Part 163 had to close for `memory_max`/`cpu_max`/`pids_max` (see [ROADMAP](../roadmap/ROADMAP.md)).
- `disk` on `POST /volumes` places the volume by the same disk-role naming a container's own `disk` field uses ([ADR-0141](../adr/0141-multi-disk-storage-placement.md)) — deliberately the same mechanism, not a second storage story. Omitted means default OS-disk placement.
- `DELETE /volumes/{name}` is the only thing that ever removes a volume's data, and is refused `409` while **any container definition** references it — not merely while one is running. A stopped container will come back and expect its data; the error names the container holding it.

Not covered, deliberately (each its own decision rather than a silent default): per-volume quotas — container overlays have `disk_quota_bytes`, a volume is currently an unbounded way to fill a disk; concurrent sharing between containers — nothing prevents it, but no locking or coordination is offered; and inclusion in the backup bundle — volumes are workload data, and [ADR-0033](../adr/0033-platform-state-backup-restore.md)'s config-only boundary stands.

### Attaching a volume to a container that already exists

```
POST /v1/containers/jump/volumes
{"name": "jump-home", "path": "/home"}

DELETE /v1/containers/jump/volumes/jump-home
```

Both edit the container's persisted definition, and an attach to a **running** container also takes effect immediately — the response's `applies` field says `now` or `on next start` rather than leaving you to infer it.

That is deliberately the opposite emphasis to `POST /containers/{name}/networks`, which is live and *ephemeral*: that one vanishes on the next restart. Here the definition is the source of truth and the live mount is it taking effect early, so nothing silently disappears later. A live mount that fails does not undo the definition — the volume genuinely is part of the container now and will be there next start, so rolling back a correct definition because one optional step failed would be worse; the response says `on next start` instead of claiming success.

The live path needed a primitive this daemon was described as lacking, and did not: `container_net.c` has used a short-lived forked helper that `setns()`es into a container's *net* namespace all along, and this is the same dance for the mount namespace. The helper is thrown away because `setns()` is whole-process — the daemon entering a container's mount namespace itself would leave every path it later resolves resolving inside that container.

Detaching never deletes the volume or its data — only this container's reference to it. One thing worth knowing: the mount point the attach created stays behind in the container's overlay upper layer, so writes to that path still succeed after a detach; they simply land in the overlay and are lost on the next recreate, like any other unvolumed path.

A volume must already exist (`404` otherwise, never an implicit create), a container cannot mount the same volume twice or two volumes at one path (`409`), and the per-container maximum is 8.

### Backing up what a volume holds

The platform bundle above carries configuration only, and that boundary stands — but it left volumes with no recovery story at all. A volume exists *precisely* because its contents should outlive the container that wrote them, and "whatever you arranged yourself outside Cix" was the only answer available, with no way to arrange anything inside it either, since a volume is a host directory no container can reach.

```
PUT /v1/system/volume-backup-config   {"disk":"sdb","enabled":true,"interval_hours":24}
PUT /v1/volumes/jump-home/backups     {"enabled":true,"retain":7}
POST /v1/volumes/jump-home/backup
POST /v1/volumes/jump-home/restore    {"snapshot":"20260822T030000Z","confirm_volume_name":"jump-home"}
```

- **Opt-in per volume.** Nothing is copied unless asked for — a volume holding a scratch build tree should not be snapshotted just for existing, and copying workload data is something to request rather than assume.
- **The policy belongs to the volume, not the container.** A volume is the thing with data; a container merely mounts one, and two containers can mount the same volume. Hanging the policy on the container would mean two policies over one set of bytes, and would still exclude the container's own overlay, which is ephemeral by design. A container's detail page *shows* the backups of the volumes it mounts, derived — the same way it already shows which volumes those are.
- **One shared interval, per-volume retention.** How much history is worth keeping depends on what the volume holds; when the sweep runs does not, and N independent timers is N ways for a schedule to be quietly wrong. The sweep rides the same periodic tick the configuration snapshots already use.
- **What happens when a container is using it is a per-volume choice**, and it decides whether the volume is ever actually backed up. A service container is normally `restart: always` and never stops, so the default means "never" — quietly, forever, for exactly the volumes that most need it.
  - `refuse` (default) — skip it. Safe, and useless for an always-on workload.
  - `pause` — freeze every running container that mounts this volume for the duration of the copy, then thaw. The freeze is the kernel cgroup freezer ([ADR-0045](../adr/0045-container-start-pause-lifecycle.md)), **not** SIGSTOP, so a process can neither ignore nor handle it: nothing writes while the copy runs and the snapshot is genuinely consistent rather than crash-consistent. **All** mounting containers are paused, not just one — any of them could write, and a snapshot that is only mostly quiesced is a crash-consistent one wearing a consistent one's label. The cost is real downtime for however long the copy takes, and every failure path thaws again, because a missed backup must not become an outage.
  - `allow` — copy live, accepting a crash-consistent snapshot: it sees whatever was on disk at that moment, so a file being written mid-copy is caught half-written. Normally fine for a home or config tree and exactly what an ordinary filesystem backup has always given you; wrong for a database, which wants its own dump from something that understands its format.
- **Restore is refused while a container is running whatever the mode says.** It replaces data rather than reading it, and no amount of pausing makes swapping a filesystem out from under a live process safe.
- In the scheduled sweep, a volume left on `refuse` with a running container is **skipped before an attempt is made**, so no failure is recorded. It is a normal, expected state, and logging it every interval would fill the status with something the operator already knows and drown the real failures in it.
- **Restore replaces, it does not merge.** Merging would leave files the snapshot never contained, producing a third state that is neither what was there nor what was backed up — a restore yielding something nobody has ever seen is worse than a refusal. It needs `confirm_volume_name` to match.
- **Retention is applied after a successful snapshot, never before.** Deleting the oldest to make room for one that then fails would lose history for nothing. A copy that fails part-way is removed rather than left behind to be restored later as though whole.
- Snapshots go to a disk carrying the `backup` role — the same role the configuration snapshots use, so there is one answer to "where do backups go" rather than two.


## What is declared, against what is actually here

```
GET /v1/software
-> {"software":[{"name":"jumpbox","kind":"image","declared":true,"installed":true},
                {"name":"glauth-cctc-test","kind":"image","declared":false,"installed":true}]}
```

Three states, each meaning something different:

- **declared + installed** — normal.
- **installed, not declared** — the one that matters. It has no recipe, so it **cannot be rebuilt from source control**, which on a platform whose premise is "compiled from source, reproducibly" is exactly what should be visible. Either capture a recipe for it, or it is debris.
- **declared, not installed** — a recipe nobody has applied. Harmless, but worth knowing.

This is reconciled by the daemon rather than left to each client, because the join spans four endpoints (images, image recipes, packages, package recipes) and two clients each implementing it is how they drift from one another — the same reasoning that folded the disk `role`/`part_label` join server-side for [#90](#partition-level-disk-management-task-844-adr-0158).

It reports; it does not act. Capturing a recipe from an existing image is a real operation with real choices in it, and deleting an image is destructive — seeing the three states is the piece worth having first.

The gap was not hypothetical: on a live box 11 images existed and 8 had recipes, and two of the three without one were leftovers from earlier investigations that nobody had noticed, because nothing anywhere put the two sets side by side.


`GET /containers/{name}/files?list=1` lists a directory instead of reading a file. A **running** container is listed through `/proc/<pid>/root`, which is the kernel's own merged overlay view and correct by construction. An **exited** one has no such view, so its upper and lower layers are merged here — with whiteouts handled, because overlayfs marks a deleted file as a character device with `rdev 0` and a naive listing would show files the container had deleted, which is a listing that lies. Opaque directories are not handled, and that is stated rather than assumed away: the failure mode is showing a stale name, not hiding a real one.

## Running a command in a container without a terminal

```
POST /v1/containers/jump/exec   {"argv":["/usr/bin/ps","aux"],"timeout_seconds":20}
GET  /v1/containers/jump/exec
```

There were two ways to do this before and both were poor diagnostics. The interactive console runs through a **pty**, whose line discipline echoes and edits what passes through it — a `/proc`-walking one-liner came back visibly corrupted during a hang investigation (`$p`→`$pp`) and fed a wrong diagnosis. A throwaway container with `capture_output` gets a **fresh namespace**, which is useless for inspecting the state of an already-running container, which is the actual need.

This enters the running container's own namespaces and runs `argv` directly. **No shell** — nothing resolves a bare name, expands a glob or splits a word, which is the point: a shell between you and the command is one more thing that can reinterpret what you asked for. **No pty** — the output is exactly the bytes the command wrote.

It is **asynchronous with a poll endpoint** rather than a blocking call, because holding the daemon's event loop for the length of someone's command is precisely the wedge [ADR-0180](../adr/0180-async-container-teardown.md) exists to prevent; every other slow operation here has the same shape, and `cixctl exec NAME -- cmd...` polls for you so it still feels synchronous.

A command that outruns `timeout_seconds` is SIGKILLed rather than left running invisibly, and reported as `timeout` rather than `done` so it is never mistaken for a command that finished. Output is capped, and `truncated` says so — silently dropping the tail of a diagnostic is how someone concludes the wrong thing from it.


## Factory reset

```
POST /v1/system/factory-reset   {"confirm": "<this install's instance name>"}
```

The most destructive endpoint here. It destroys every container and its storage, every image and all its versions, every network/route/DNS/PKI/LDAP/NTP/syslog registration, all package state, recipes, build cache and artifacts, the log store, and **every volume along with all data in it**. Volumes are the one category that is genuinely irreplaceable workload data rather than regenerable platform state — and a just-installed box has none, so a reset that kept them would not be a reset.

It keeps the installed OS itself (both root slots, the kernel, the ESP, the install-time config), so the box returns to how `cix-install` left it rather than to nothing. Disks that carried a role are **forgotten, not reformatted** — the filesystems on them are untouched.

`confirm` is this install's own instance name, not a boolean. A boolean can be sent by a client that misunderstood the call; a name can only be sent by something that looked it up first.

**The mechanism is a sentinel plus a reboot, and both halves of that are correctness rather than taste.** The daemon holds this state in memory and rewrites its files on any change, so deleting them underneath a running daemon is a race it usually loses — the next container event or health probe writes the file straight back and the reset silently half-happens. And it is crash-safe: once armed the reset happens, on this boot or the next. A reset that got as far as telling the operator "yes" and then evaporated because the reboot did not complete is the worst possible outcome for an operation whose entire purpose is being certain of the starting state.

The wipe runs at startup **before any subsystem reads its state**. That placement is the whole correctness of it: put later in the sequence — which is where it was first written — the earlier subsystems have already loaded the old world into memory and will write every bit of it back on the next change. The files vanish, the API still answers with everything that was supposed to be gone, and the reset looks like it worked.


## Control-plane reservation (issue #86)

```
GET  /v1/system/control-plane-reservation
PUT  /v1/system/control-plane-reservation   {"enabled": true, "cpu_percent": 10, "memory_bytes": 536870912}
```

```json
{"enabled": true, "cpu_percent": 10, "memory_bytes": 536870912,
 "host_cpus": 4, "host_memory_bytes": 17179869184,
 "workload_cpu_max": "360000 100000", "workload_memory_max": 16642998272,
 "cgroup": "cix-workload"}
```

Here the REST daemon is not one management path among several — it is the only one. There is no SSH and no general shell ([ADR-0034](../adr/0034-console-login-via-supervised-cixctl.md)), so a starved `cixd` is not a degraded box, it is a box nobody can reach until someone walks to the hypervisor. That is not hypothetical: four concurrent package builds oversubscribed a 2-CPU machine, the kernel stayed perfectly healthy throughout (ping 0% loss, 0.26ms), and the daemon simply stopped answering.

**The mechanism bounds everything else rather than favouring the daemon.** Every container and every package build is a leaf under one `cix-workload` cgroup whose ceiling is the machine minus this reservation (builds nest one level deeper, under `cix-workload/cix-pkgbuild`, so the build budget sits *inside* the workload budget rather than beside it — beside it would be two ceilings that add up to more than the machine, which is the same class of mistake issue #85 was). What the control plane has left is then enforced by the kernel, not hoped for. Priority (`nice -20`, `oom_score_adj -1000`, set at startup) is the cheap 80% of the same goal and remains in place.

**It is expressed as what the control plane keeps, not as what workloads may have.** An operator reasons about "leave the daemon a tenth of the box", and that reasoning stays correct when the box is replaced by a bigger one — the ceiling is derived from live host totals on every apply, so the same config means the same thing on a 2-CPU VM and on a 32-core machine. `workload_cpu_max`/`workload_memory_max` report that derivation, raw, because a percentage on its own is not something anyone can act on.

**Enabled by default** (10%, 512 MiB). A reservation that has to be discovered and switched on is a reservation nobody has when they need it, and the failure it prevents is the worst one this platform has. `cpu_percent` is capped at 50 and `memory_bytes` floored at 64 MiB: a bigger reservation is not a safety margin, it is a second workload budget. Disabling it leaves the hierarchy exactly as it is and only removes the ceilings — turning it off is not a migration.

Changes apply to the live cgroup immediately, not at the next container creation: an operator raising the reservation because the box is under strain needs it while it is under strain.


## Build logs, and re-fetching one recipe version

```
GET /v1/pkg/build-logs
GET /v1/pkg/build-logs/{file}
POST /v1/pkg/sync   {"refetch": "kernel@6.18.40-13"}
```

**Every build's complete output is kept** (issue #57). The consolidated log store keeps a ~4KB *tail* of build output and `pkg build-log` is a live-only stream, so until now a finished build's real output survived nowhere unless the recipe itself redirected to a file *and* the container was preserved. That cost two multi-hour round trips on one package: first a verbose `configure` swamped the tail and hid the actual error; then the workaround — redirecting to a file inside the container — silenced the live stream, and a healthy 71-minute build was misread as hung and killed by hand. Two failure modes, one missing primitive.

The daemon now tees each build's stdout/stderr to `<data-dir>/pkg/build-logs/<name>-<version>-<started>.log` as it streams. A build that is killed keeps its log up to the moment it died, which is the case most worth reading. The directory is bounded (oldest pruned when a new build starts) — diagnostics must not fill the disk they exist to debug. `GET /pkg/build-logs/{file}` returns text, tail-first if the log is larger than the response cap, and says so on its own first line rather than only in a header. `cixctl pkg build-logs --last` prints the newest one whole.

**`refetch` re-fetches exactly one recipe version** (issue #59). `pkg sync` is additive and never overwrites a `name@version` it has already seen — a sound default, since [ADR-0107](../adr/0107-package-image-versioning.md)'s resolution rules depend on version immutability. During active development of a recipe, though, the only workaround was burning a new version number per iteration, and the catalogue really did collect five dead kernel pins and four dead gcc pins from a single investigation. `{"refetch": "name@version"}` lets that one sync replace that one version. It is one-shot (cleared when the sync completes, so the periodic background sync can never inherit it), and a bare package name is a `400` — immutability is per version, so the escape hatch is too.


## Editing a container without recreating it (issue #11)

```
PATCH /v1/containers/{name}   {"cmd": ["/usr/bin/dnsmasq", "-k", "..."], "env": {"TZ": "UTC"}}
```

Until now, changing a `cmd`, an env var or a staged file meant delete-and-recreate — reconstructing the whole request body by hand, with every chance to drop a field. A `PATCH` merges the fields you give into the stored definition: a key present replaces that key, a key set to `null` removes it, everything else is untouched. The merge is top-level only; a deep merge would make "how do I clear one entry of `files[]`" unanswerable.

**It applies at the container's next start, and the response says so** (`"applies": "next-start"`, plus `"restart_required": true` when it is running right now). That is not a limitation being papered over: a running process's argv cannot be changed without re-exec'ing it, so "change `cmd` on a running container" is `restart` by another name. What genuinely *can* change live already does, through its own endpoints — [volumes](#persistent-volumes-issue-88-adr-0183) and [network attach](#a-dedicated-bind-ip-decoupled-from-the-management-networks-own-address).

Two groups of fields are refused rather than silently ignored:

- **`name`** — a container's name is its identity; that would be a different container.
- **`restart`, `restart_delay_seconds`, `depends_on`, `readiness`, `follow_rolling`, `follow_rolling_jitter_seconds`** — these feed the definition index, and its one parser lives in the create path. A second parser here would be a parallel implementation of the same validation, which this project does not do. The `400` names the offending field and says to recreate the container; extracting that parser is the follow-up that lifts the restriction.

`cixctl container edit NAME --json='{...}'` is the CLI surface.


## Per-package rolling policy (issue #64)

```
GET    /v1/pkg/policies
PUT    /v1/pkg/policies/gcc      {"policy": "pinned", "version": "6.4.0-5"}
DELETE /v1/pkg/policies/gcc
```

Every omitted-version resolution here — a plain `pkg install`, dependency resolution, `update-all`, a `follow_rolling` image's rebuild, the "available" column — has always used one rule: dpkg-style **highest version wins** ([ADR-0107](../adr/0107-package-image-versioning.md)). That is the right default for a rolling-release platform and stays the default.

It is not always the right answer, and this project has the scar: the Part 201 toolchain work published gcc `4.7.4` and `6.4.0` *after* `16.2.0` already existed. For `gcc` the highest version and the newest published are different recipes, and neither is what an operator walking a bootstrap chain wants installed — they want one specific link of that chain, held.

| Policy | Resolves to |
|---|---|
| `highest` | The highest version (the default; no policy set means this) |
| `newest` | The most recently **published** recipe. A recipe version's file is written exactly once and never touched again (ADR-0107 immutability), so its mtime is a real first-published timestamp rather than an approximation |
| `pinned` | The version named, and nothing bumps it — a real hold. `update-all` and `follow_rolling` resolve to the pinned version, so there is simply never an update to apply |

Two deliberate refusals: a `pinned` policy with no version is a `400` (it would claim to hold something while meaning "highest" — the exact drift a pin exists to prevent), and a pinned version that is not published makes the package unresolvable rather than quietly resolving somewhere else.

The policy is applied in the single function every implicit resolution goes through, so it applies everywhere by construction rather than by each of thirteen call sites remembering to consult it. Policy is operator state, never recipe content: a recipe cannot know which link of a chain a particular box is meant to sit on. `cixctl pkg policy ls|set|clear`.


## Boot console (issue #24)

```
GET /v1/system/boot-console
PUT /v1/system/boot-console   {"consoles": ["tty0", "ttyS0,115200n8"], "extra": "nomodeset"}
```

An installed Cix host boots through systemd-boot, and every loader entry carried a hardcoded `console=tty0 console=ttyS0`. That is a reasonable default and a poor thing to be stuck with — real hardware sometimes needs a serial console at a particular baud, a framebuffer argument to produce any output at all, or exactly the opposite (`nomodeset`) when the framebuffer is what breaks it. None of it was reachable without reinstalling.

**A change is applied to the loader entries already on the ESP**, not only to entries written by future updates: an operator who cannot see the console is not in a position to wait for the next A/B update. It takes effect at the next boot, and the response says how many entries were rewritten.

**Everything from `root=` onward is preserved byte for byte.** That half of the options line — `root=`, `rw`, `init=` and the daemon's own arguments — decides whether the machine boots at all, and is deliberately not editable here. This is a display setting, not a kernel command line editor: letting the whole string be edited would turn "I want serial output" into a way to make the box unbootable.

Validation refuses rather than escapes. A console is a bare tty name with optional comma-separated options (`ttyS0,115200n8`, never `/dev/ttyS0`); extra parameters are plain kernel arguments; `console=`, `root=` and `init=` are refused inside `extra`, and anything that could split the boot line — whitespace inside a console name, a newline, a quote — is a `400`. The file being written is the one that decides whether this machine comes back.

`GET` reports the configured intent **and** the options line each entry on the ESP currently carries, because those differ on any box whose ESP is not writable from here. On a dev daemon `loader_entries` is empty — a fact, not an error.

`cixctl boot-console show|set --console=… --extra="…"` is the CLI surface; the dashboard has a **Boot Console** tab under System > Host.

**The ESP itself is a REST resource (ADR-0202, issue #128)** — and this one exists because of a real failure that was expensive precisely because the state was invisible. A host could not complete an A/B update: `POST /v1/system/update` wrote the new image and staged a correct loader entry, the machine rebooted, and came back running exactly what it had been running. Repeatedly, with no error anywhere. The cause was one line, `default thinc-*`, written at install time under the project's previous name — because **systemd-boot's `default` is a glob PATTERN, not an entry name**, it went on matching six stale entries and outranking the one correctly-staged new one. Nothing could report that, and until this endpoint existed nothing could even read `loader.conf`.

```
GET    /v1/system/esp
PUT    /v1/system/esp                  {"default": "cix-*"}          # partial; timeout too
DELETE /v1/system/esp/entries/thinc-a+3.conf
```

The most important field is the one you cannot set: **`selected_entry`** names the entry systemd-boot would actually boot, and each entry carries `matches_default`. The daemon computes that rather than leaving an operator to derive it from a glob and a directory listing — which is the step that went wrong. `cixctl esp show` leads with `will boot: <entry>`.

Two refusals, both hard errors rather than warnings, because anyone using this endpoint is by definition not at the console: a `default` matching **zero** existing entries is refused with 409 (it is silent, and strands the host on next boot — the error names what does exist), and the **last remaining** entry for the running slot cannot be deleted, though duplicates of it can, since clearing accumulated duplicates is what removal is for. A `loader.conf` rewrite preserves directives this daemon has no opinion about (`console-mode`, `editor`, …) rather than regenerating the file and silently dropping them.


## DHCP: a self-contained service (ADR-0197)

```
GET    /v1/dhcp/servers
POST   /v1/dhcp/servers          {"container": "dns-1"}
PUT    /v1/dhcp/networks/lab     {"enabled": true, "range_start": "172.30.7.100",
                                  "range_end": "172.30.7.200", "lease_seconds": 3600,
                                  "router": "172.30.7.1", "servers": ["dns-1", "dns-2"]}
POST   /v1/dhcp/static           {"mac": "aa:bb:cc:dd:ee:01", "ip": "172.30.7.50", "hostname": "printer"}
GET    /v1/dhcp/leases
```

**Contained is the design constraint.** DHCP registers its own servers, owns its own ranges and reservations, and every endpoint it has lives under `/v1/dhcp`. It reads no other service's registry and nothing DHCP-shaped hangs off another resource — which is what has to be true for a service to be made optional later rather than tangled through everything.

**It still pairs with DNS, and the pairing is reported rather than enforced.** dnsmasq answers for the names of clients it has itself leased addresses to, so registering the *same* container as both a DNS server and a DHCP server makes a lease resolvable the instant it is issued. `resolves_leases` on each server says whether that is true:

```json
{"servers": [{"container": "dns-1", "running": true, "resolves_leases": true},
             {"container": "dns-2", "running": true, "resolves_leases": true}]}
```

Enforcing it would mean one service reaching into another's table; reporting it means an operator can see what they have.

**A lease is not a DNS record.** A record is durable operator intent — persisted, surviving every server, listed at `/v1/dns/records`. A lease is short-lived state owned by the server that issued it. Mirroring leases into the record store would put two writers in one namespace and leave a record pointing at an address another machine now holds the first time a lease expired uncleanly. `GET /v1/dhcp/leases` reads them from each server's own lease file, every time.

**Redundancy is split scope, because with dnsmasq it has to be.** dnsmasq implements no failover protocol — there is no equivalent of ISC dhcpd's primary/backup peer relationship for it to join, so two instances share no lease database and cannot coordinate. A range names the servers that serve it, and Cix divides it into one disjoint slice each: all of them answer, a client takes whichever offer reaches it first, and handing one address to two machines is impossible because no two servers hold it. Any one alone keeps serving from its own slice.

```json
{"network": "lab", "enabled": true, "range_start": "172.30.7.100", "range_end": "172.30.7.200",
 "servers": ["dns-1", "dns-2"],
 "slices": [{"server": "dns-1", "range_start": "172.30.7.100", "range_end": "172.30.7.150"},
            {"server": "dns-2", "range_start": "172.30.7.151", "range_end": "172.30.7.200"}]}
```

One server, three, or more are all valid — how many a wire should have is an operator's decision. The **order** is preserved because it is the slice order: a derived order would move every client's address whenever something unrelated changed. Unregistering a server drops it from every range that named it, and disables any range left with no servers at all — a range naming a server nobody serves from is a slice handed to nothing, which looks like coverage and is not.

If you need true primary/backup with a failover protocol — a hot standby that takes over the *whole* pool knowing what its peer has leased — that needs a DHCP server implementing one (ISC Kea or dhcpd), which this platform does not package.

**Static reservations are live; range changes are not.** dnsmasq re-reads the reservations file on `SIGHUP`, so `POST /v1/dhcp/static` takes effect immediately. It reads ranges only at startup — so **changing a range restarts the servers that serve it**, through the same jittered rolling-restart timer a rolling image update uses, and only those whose own rendered conf actually changed.

Everything that could not serve is refused at the point of asking: a range outside the network's own subnet, a range covering the network's own address, a range that runs backwards, a lease time outside 60s–30d, no servers named, a server that is not registered, a server not attached to that network, or a range with fewer addresses than servers to split it between. Two reservations for one address is a `409`. MACs and hostnames are refused, never sanitised.

### Setting up a serving container

The rendered files live at three well-known paths, so the dnsmasq container is started against them:

```
--conf-file=/etc/dnsmasq-dhcp.conf
--dhcp-hostsfile=/etc/dnsmasq-dhcp-hosts
--dhcp-leasefile=/run/dnsmasq.leases
```

Create the container with `files[]` entries for the first two (empty is fine — dnsmasq refuses to start on a `--conf-file` that does not exist), then `POST /v1/dhcp/servers`. From then on Cix stages the **rendered** files into the container at creation, before its process starts, so the file dnsmasq opens is always the current one. Register the same container with `POST /v1/dns/servers` as well and its leases resolve.

> **Enabling DHCP on a network that reaches a real LAN will answer requests from machines that are not this platform's.** A home or office LAN almost certainly already has a DHCP server, and a second one is not a redundant pair — it is two servers with separate lease databases handing out overlapping addresses. Nothing here prevents it, because nothing here can tell a lab bridge from an uplinked one. Use an isolated network unless you own the LAN's addressing.

`cixctl dhcp server ls|add|rm`, `cixctl dhcp enable --network=… --range=… --server=… [--server=…]`, `cixctl dhcp static add|rm`, `cixctl dhcp show|leases` is the CLI surface; the dashboard has DHCP under Services (Servers / Ranges / Reservations / Leases), with each network's own page showing the leases on it.

## Compressed swap cache: zswap (issue #51)

```
GET /v1/system/zswap
PUT /v1/system/zswap   {"enabled": true, "max_pool_percent": 20, "compressor": "lzo"}
```

zswap compresses pages in RAM before they would otherwise be written to the real swap device — memory pressure costs CPU instead of disk I/O. It is here because of a specific incident, not as a general nicety: 192.168.15.95 went fully unresponsive during a heavy `-j6` gcc bootstrap that drove it into swap thrashing, on a disk that had already produced two kernel Oopses in the page-cache/writeback path that same session. zswap does not fix a bug in that path; it reduces how often that path is entered at all.

```json
{"supported": true, "enabled": true, "max_pool_percent": 20, "compressor": "lzo",
 "kernel": {"enabled": true, "max_pool_percent": 20, "compressor": "lzo"},
 "available_compressors": ["lzo", "lz4", "zstd", "deflate"]}
```

**Configured intent and kernel state are reported separately, and the interesting case is when they differ.** The kernel silently ignores a parameter it cannot honour, so an endpoint that echoed back its own input would report success for a setting that never took. `kernel.*` is read back from `/sys/module/zswap/parameters`; when `kernel.enabled` disagrees with `enabled`, that is the whole story.

**`available_compressors` comes from `/proc/crypto`** — what this kernel was actually built with. It is what decides whether the `compressor` knob means anything at all, and it is why a compressor this kernel lacks is a `409` rather than a write the kernel would quietly drop. An empty list means no zswap at all (`supported: false`).

**On by default**, unlike the swap file itself, which is opt-in because it consumes real disk. zswap consumes nothing until the box is already swapping, and at that point there is no reading of "off by default" that helps the operator whose box is thrashing. The kernel's own `CONFIG_ZSWAP_DEFAULT_ON` is deliberately **not** set: whether zswap is on is a setting this platform owns, and a kernel that also decided it at boot would be a second source of truth for one fact.

Validation happens before the write, never after. An out-of-range percentage or a compressor name that is not a plain algorithm name is a `400` — the string goes into a sysfs file, so it is refused rather than escaped. And if the kernel refuses the settings, the previous ones are restored and **nothing is persisted**: a config file describing a state the machine is not in is worse than the failure it was recording.

`cixctl zswap show|set [--enable|--disable] [--max-pool-percent=N] [--compressor=NAME]` is the CLI surface; the dashboard has it under System > Host > Swap, below the swap file itself.

## Kernel line (issue #65)

```
GET  /v1/system/kernel-policy
PUT  /v1/system/kernel-policy          {"channel": "longterm"}
POST /v1/system/kernel-policy/refresh
```

The platform used to make one judgment call on every operator's behalf: a single pinned kernel version in the recipe, with nothing anywhere expressing which *line* it belonged to or how far behind that line it had drifted. Both facts matter, and neither was anywhere. A cautious box wants `longterm`; an aggressive one wants `stable` or `mainline`; that is operator state, exactly like the per-package policy [#64](#per-package-rolling-policy-issue-64) introduced.

**The channels are kernel.org's own monikers**, resolved from kernel.org's own `releases.json`. There is no copy of that data in this repo, deliberately: a mirror of a fact that changes weekly without anyone here noticing would be a second source of truth by construction.

```json
{"channel": "longterm", "running_version": "6.18.40", "running_series": "6.18",
 "resolved_version": "6.18.46",
 "resolved_source_url": "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.46.tar.xz",
 "behind": true, "running_series_maintained": true,
 "newest_longterm": "6.18.46", "newest_longterm_series": "6.18",
 "releases_fetched_at": 1787496132, "refreshing": false, "last_refresh_error": null}
```

**Three states, not two.** `resolved_version` and `behind` are `null` until a refresh has succeeded, and `null` on the `pinned` channel, which proposes no version at all. A box that has never asked is not a box that is up to date, and reporting `behind: false` for it would be the one wrong answer that reads as reassuring.

**`longterm` resolves within the line you are already on.** kernel.org lists six longterm lines at once (6.18, 6.12, 6.6, 6.1, 5.15, 5.10), so the moniker alone does not name a version. Resolving it to whichever is newest would silently propose a cross-major jump to a box deliberately sitting on an older longterm line. It falls back to the newest only once kernel.org stops listing your line — `running_series_maintained` tells you which of those happened, and `newest_longterm` reports the newest line either way, so both facts are visible rather than one hidden behind the other.

**`running_version` comes from `uname()`, not from the recipe pin.** The pin says what was last built; the running kernel says what actually booted, and after a failed A/B update those are not the same fact.

**Selecting a channel never moves the pin and never fetches anything.** It changes what is *reported*. `POST .../refresh` is the only thing that reaches the network, and it returns `202` immediately: the fetch is a forked `curl` watched by a pidfd, never a blocking call inside the event loop — a host with no upstream resolvers set (see [`/system/resolv`](#the-hosts-own-outbound-dns-resolver-adr-0076)) would otherwise stall the whole control plane behind a DNS timeout, which is precisely the class of wedge the [stall watchdog](#control-plane-stalls-issue-100) exists to catch. A failed fetch keeps the previous answer and says why in `last_refresh_error`.

The fetched file is cached on disk, so a box that has resolved once still answers "how far behind am I" after a restart with no network at all.

`cixctl kernel-policy show|set --channel=…|refresh` is the CLI surface; the dashboard has a **Kernel Line** tab under System > Host.


## Control-plane stalls (issue #100)

```
GET /v1/system/stalls
```

```json
{"stalls": [
   {"ts": 1787476574, "event": "recovered", "seconds": 10, "state": "S", "wchan": "do_epoll_wait", "activity": ""},
   {"ts": 1787476569, "event": "stall", "seconds": 5, "state": "T", "wchan": "do_signal_stop", "activity": "POST /v1/system/update"}
 ],
 "threshold_seconds": 5,
 "loop": {"worst_pass_ms": 3120, "last_pass_ms": 1, "slow_passes": 47,
          "slow_pass_threshold_ms": 750, "worst_pass_activity": ""}}
```

The daemon is a single-threaded event loop, and on an installed host it is the only way in — no SSH, no shell ([ADR-0034](../adr/0034-console-login-via-supervised-cixctl.md)). It has been observed accepting TCP while answering nothing for minutes, then recovering on its own, with **nothing in the log store from inside the window**. That silence is structural rather than an oversight: the loop that would record "I am stuck" is the one that is stuck, so a wedge's only trace is its own absence and nothing about it can be analysed afterwards.

**These records are written by a separate process**, forked at startup, so they exist precisely when the daemon cannot write anything. That also buys something a signal handler could never do safely: the watchdog reads `/proc/<pid>/wchan` — the kernel function the daemon is sleeping in — which is the single most useful fact about a wedge and is unavailable from inside it. `state` is the process state (`D` is uninterruptible sleep, the kind that cannot even be killed); `activity` is the request being served when the loop went quiet, so a stall names the thing that did not come back.

A stall is five seconds without the loop completing an iteration. The loop wakes at least once a second on its own, so an idle daemon is never mistaken for a stalled one — and a legitimate slow synchronous operation crossing the threshold is not a false positive, it is exactly the thing worth knowing about.

**`loop` answers the other question, and it is the one that was missing (issue #229).** The records above say when the loop *stopped*. A daemon can be entirely unusable without ever stopping: on 2026-09-01 a publish went unanswered for **247 seconds** while this store recorded nothing at all — correctly, by its own definitions. The loop never went quiet, and no single request was in flight for long; it simply spent seconds of *every pass* starting a rebuild, with every other client waiting to be accepted. Alive by the only measure it had, and unusable by every other.

So each pass is now timed, counting work and excluding the epoll wait, because that duration is the latency floor every waiting client is subject to. `worst_pass_ms` is the worst single pass since boot, `slow_passes` counts those over `slow_pass_threshold_ms`, and a rising count on an otherwise quiet box means the daemon is spending itself on something. `worst_pass_activity` names the request in flight at the time — **empty is informative rather than missing**: it means the loop was busy with work no client asked for, which is precisely the case that used to be invisible. A `slow-pass` record is also appended (rate-limited) so the trail survives a reset, kept as a distinct event from `stall` because "what is it blocked on" and "what is it spending itself on" are different questions.

Records are appended to `<data-dir>/state/control_plane_stalls.jsonl` and survive the daemon, a restart and a reboot: a wedge is usually followed by one of those, and a diagnostic that dies with what it was diagnosing is not a diagnostic. `cixctl stalls` prints them; the dashboard has a **Stalls** tab under System > Monitoring.


### Volume ownership (issue #102)

```
POST /v1/volumes              {"name": "home", "owner_uid": 10000, "owner_gid": 10000}
PUT  /v1/volumes/home/owner   {"uid": 10000, "gid": 10000, "recursive": false}
PUT  /v1/volumes/home/owner   {"uid": null, "gid": null}     # hand it back to root
```

A volume is a directory this daemon creates as **root**, and until now nothing could change that — so a workload that does not run as root could not write to its own volume. That was found the plain way: an operator logged into the jump box and their home directory, which is a volume, belonged to root.

`pam_mkhomedir` does not save you here, and it is worth knowing why: it creates a home that does *not* exist, and does nothing at all to one that does. Any directory the daemon has already put inside a volume — staged files, a restored backup, a migration — is there before the first login, and stays root's.

- **Both ids together.** `owner_uid` without `owner_gid` is a `400`: a volume owned by one user and an unrelated group is almost always a typo. On the `PUT`, pass the value a field already has to leave it alone.
- **`recursive` defaults to false**, and is the caller's explicit choice. A volume that has been in use holds files whose ownership someone may have set deliberately, and rewriting all of them because the top-level owner changed is a quiet kind of data loss.
- **Null uid and gid hands it back to root**, and really `chown`s it there rather than leaving whoever owned it last still owning it.
- **`owner_uid` is null, not 0, when nobody has said.** 0 is a real uid, and a field that cannot tell "deliberately root" from "unsaid" is one that gets read wrong eventually.
- The ids are the numeric ones **the container sees**, which for every container without a user namespace are the host's own ids too.

For an `ldap_client` container the right ids are already known — `GET /v1/ldap/users` reports each account's `uidnumber` and `primarygroup` — so giving a user their home is one call rather than a `chown` through an exec.

`cixctl volume create --name=home --owner-uid=10000 --owner-gid=10000`, `cixctl volume owner home --uid=10000 --gid=10000 [--recursive]`, `cixctl volume owner home --root`; the dashboard has the same controls on the volume's own page.


## Capability restriction (issue #29)

Every container's process runs as real root with no user namespace — a genuine, tracked gap (issue #29: full user-namespace support is the eventual complete fix, still open, large enough to need its own design pass). Until that lands, `cixd` closes the single most severe consequence of it directly: right before a container's `cmd` is `execve()`'d, its capability bounding set is permanently trimmed to a small default-safe list via `prctl(PR_CAPBSET_DROP, ...)` (see `src/container_caps.c` for the exact list and the reasoning behind each entry). `CAP_SYS_MODULE` is the headline case — kernel modules aren't namespaced, so an untrimmed container could otherwise call `init_module()` directly and compromise the host kernel with no exploit required at all. `CAP_SYS_ADMIN`, `CAP_SYS_PTRACE`, `CAP_SYS_RAWIO`, `CAP_SYS_BOOT`, `CAP_BPF`, and a dozen others narrower still are dropped the same way. `CAP_NET_ADMIN`/`CAP_NET_RAW`/`CAP_SETUID`/`CAP_SETGID` stay by default — confirmed, real, in-use needs on this project's own containers today (e.g. `jumpbox1`'s `sshd`).

```
POST /v1/containers
{
  "name": "ntp-1",
  "image": "chrony",
  "cmd": ["/usr/sbin/chronyd", "-d", "-f", "/etc/chrony.conf"],
  "cap_add": ["CAP_SYS_TIME"]
}
```

- `cap_add` is optional: 0–8 capability name strings (e.g. `"CAP_SYS_TIME"`), each an explicit exception to the default deny-list above — chrony's own `clock_settime()` call is the confirmed real case. Only a name that's actually on the default deny-list is meaningful; an unrecognized name is a `400`.
- Because the drop happens on the bounding set (not just the current process's own effective/permitted sets), it persists through every exec a container's process tree ever makes afterward — a shell script execing a further binary can't reclaim a dropped capability, regardless of how many levels deep.
- `GET /containers`/`GET /containers/{name}` echo back whatever `cap_add` the container was actually created with (empty means the fixed default list applies with no exceptions).
- Set once at creation; not modifiable on an already-running container, same as `interfaces`.

## Container DNS resolution (ADR-0143)

```
POST /v1/containers
{
  "name": "app1",
  "image": "base",
  "cmd": ["/usr/bin/myapp"],
  "dns_servers": ["192.168.15.101", "192.168.15.102"]
}
```

`dns_servers` is optional: 0–3 IPv4 addresses (matches glibc's own `resolv.conf` `MAXNS`, the same `RESOLV_MAX_NAMESERVERS` constant `PUT /system/resolv` already enforces for the *host's* own resolver). When given, staged as a real `/etc/resolv.conf` (`nameserver a.b.c.d` per line) directly into the container's own overlay before its process ever `execve()`s — the exact same pre-clone3 staging mechanism `files` above already uses, just a dedicated field so an operator doesn't have to hand-construct resolv.conf syntax themselves. `400` if the request also supplies a `files` entry whose `path` is `/etc/resolv.conf` — an unresolvable ambiguity, surfaced loudly rather than one silently overwriting the other. Echoed back on every `GET /containers`/`GET /containers/{name}` response (id-only, like `files`), and survives `restart: "always"`'s own replay mechanism the same way every other creation-time field does.

**Deliberately explicit — no automatic wiring to a registered internal DNS server.** This project's own `.internal`-zone DNS servers (`dns record create`, `dns server register --container=NAME` — see below) are entirely opt-in and there can be zero, one, or several of them on different networks at once; Cix never guesses which one a given container should use, the same posture `PUT /system/resolv` already established for the host's own outbound resolution (ADR-0076). To give a container working `.internal` resolution, point `dns_servers` at that DNS-server container's own real IP directly (`GET /containers/{name}` on it, or `cixctl inspect dns-1`).

Before this field existed, a container had no DNS resolution capability from Cix at all — not even the host's own outbound resolver config was ever propagated into a container's namespace. A container that needs DNS and doesn't set `dns_servers` still has none unless its own image bakes one in.

## Package manager: source-based, asynchronous installs

A package manager built from scratch: recipes are shell scripts (the same format Gentoo ebuilds/Arch PKGBUILDs/CRUX Pkgfiles use), builds happen inside this project's own container runtime, and the daemon **never sources or executes a recipe on the host** — recipe metadata (`pkg_name=`, `pkg_version=`, `pkg_source=`, `pkg_sha256=`, `pkg_depends=`, `pkg_changelog=`) is read with a strict, non-executing line scanner; the recipe's real shell code (`pkg_build()`/`pkg_install()`) only ever runs inside the isolated, network-less build container. See [`docs/guides/writing-recipes.md`](../guides/writing-recipes.md) for the full recipe-authoring contract.

**Every install targets one image**, `/var/lib/cix/rebuildable/images/{image}/rootfs` (ADR-0141) — `"base"` by default (any container created with `"image": "base"` gets everything installed there, no separate host/container package paths to keep in sync), or an explicit other one (see [Per-image installs](#per-image-installs) below) for software that shouldn't be part of every container's baseline.

**Installs are asynchronous.** The daemon is single-threaded and non-blocking; a network fetch or a real compile can take anywhere from seconds to minutes, so `POST /v1/pkg/install` returns immediately (`202`) and the actual work happens in the background — poll `GET /v1/pkg/{name}` for progress. **Fetching happens on the host** (a `curl` subprocess — this project's networking plane has no outbound NAT, so a build container has no network access at all, a stronger isolation boundary for untrusted build scripts, not a limitation worked around).

**A build or a fetch can be stopped, and only by a person asking (`POST /v1/pkg/cancel`, issues #213 and #239).** A build that will never finish — one whose own `configure` is waiting on stdin, which is exactly how `perl` hung — otherwise holds its chain slot until the daemon restarts, because nothing else can stop it. `GET /v1/pkg` has reported `last_output_seconds_ago` since issue #58; this is the verb that acts on it. **A stuck fetch counts, and used to not.** Cancel accepted a fetching entry, set the flag and stopped there, on the reasoning that a fetch is a subprocess rather than a container so there was nothing to kill — leaving the flag to whichever completion path ran next. If the fetch never completes, none ever runs: a fetch retrying an unreachable upstream held its chain slot indefinitely, every later install was refused `409`, and cancel returned `200` having changed nothing. The fetch child is now killed, which drives the ordinary completion path that honours the flag.

Deliberately **not** an automatic timeout on silence. "No output for N minutes" does not mean "hung": `gcc`'s own bootstrap goes quiet for long stretches and a large link produces nothing for minutes at a time. A threshold tight enough to catch a real hang eventually kills a legitimate build, mid-way through, on nobody's schedule — and the asymmetry decides it, because failing to kill a hung build costs a slot while killing a live one costs the whole build. A person looking at an hour of silence can tell `gcc` linking from `perl` waiting on stdin; a threshold cannot.

The cancelled entry records `failure_kind` **`cancelled`**, which is not `build`: a build an operator stopped did not fail on its own merits, and a reader who cannot tell those apart goes looking for a defect that is not there (the same reasoning issue #101 used to make the kind a required parameter rather than something inferred from the message). A cancelled **upgrade** leaves the package installed at the version it already had, exactly as any other failed upgrade does. Cancelling something with no build in flight is refused with `409` rather than half-acted on, so a cancel racing a build that has just finished cannot mark a completed install as cancelled.

**Up to `max_concurrent_jobs` install/hostbuild jobs may genuinely run at once (ADR-0157), default 10** — each its own independent chain, its own build container, its own captured output; a `POST /pkg/install`/`POST /pkg/hostbuild` while every chain slot is already busy gets `409`, same as before this existed (v1 was hardcoded to exactly one). `GET`/`PUT /v1/system/pkg-build-config` (`max_concurrent_jobs`, range 1-10 — 10 is also the daemon's own hard compile-time ceiling, not just this config's default) show/set the real ceiling; lowering it never disrupts jobs already in flight, only future ones. `GET /v1/pkg/build/log`'s own `?name=&image=` query params (below) exist specifically because more than one build can now be live-tailed at once.

**Every build's own sandbox is now cgroup-limited (ADR-0165), not just its job-count slot** — found missing the hard way: a burst of concurrent installs with no per-build resource ceiling hung a real production `cixd` entirely (TCP still accepted connections, no HTTP request ever completed again). `memory_max` (bytes, default 2GiB) and `cpu_max` (raw cgroup v2 `cpu.max` syntax, default `"100000 100000"` — one full CPU's worth) are the same `PUT /v1/system/pkg-build-config` resource, applied to every `__pkgbuild-N` container's own cgroup via the identical mechanism a regular container's own `memory_max`/`cpu_max` fields already use — not a second implementation. 0/null means unlimited for either field, a deliberate opt-out for a box that genuinely has spare capacity to give, not a validation error.

One-time setup, before installing anything:

```
POST /v1/pkg/bootstrap
```

Stages a real build toolchain (`gcc`/`make`/`ld`/`as`/`cc1`/`sh`/`tar` and their real headers/libraries) into the sandboxed build image. No body: copies live from this daemon's own host `/usr/{include,lib,lib64,bin,libexec}` with the real `cp -a` — works for dev/test convenience when `cixd` happens to be running somewhere with a real toolchain already, but produces an empty, non-functional toolchain on a real minimal install (nothing under its own `/usr` beyond `cixd`/`cixctl` and their bare runtime libs). `{"toolchain_path": "/local/path/to/toolchain.squashfs"}` imports a real, portable artifact (built once, elsewhere, with `image/src/mktoolchainimage.c`) via a local path already `scp`'d onto this box. Both of those are synchronous (`204`), unchanged.

**`{"toolchain_url": "...", "toolchain_sha256": "..."}` is a third mode (ADR-0065): the daemon fetches the artifact itself, host-side** — the same real `curl` primitive every recipe's own `pkg_source` already uses, not a second fetch mechanism. Closes a real gap the other two modes both rest on: a genuinely fresh, minimal Cix install has no SSH server and no general shell at all (ADR-0034), so "the operator transfers it onto the box" was never actually possible for a from-scratch install with nothing else already on the network to reach it via — confirmed the hard way (`nc -z <box> 22` closed) rather than assumed. Async (`202`, since a real network fetch of a real, large artifact can't block this daemon's single-threaded event loop) — poll `GET /v1/pkg/bootstrap` (`state`: `none`/`fetching`/`ready`/`failed`) the same shape `GET /system/iso` already established. `409` if a fetch is already in flight; `400` if `toolchain_url` is given without a valid 64-char `toolchain_sha256`. All three modes are idempotent — safe to call again.

**The build sandbox is the `cix-builder` image** ([ADR-0198](../adr/0198-build-sandbox-is-a-real-image.md)), resolved exactly the way a hostbuild's own `--build-image=` already is rather than living at a flat path beside the image mechanism. It used to be an unversioned directory grown silently by every successful install, so what it contained was a function of one box's entire install history — expressible nowhere, comparable to nothing, and the reason a recipe could stop building with no visible change anywhere. Now `GET /v1/images/cix-builder` shows its version history like any other image, and a version can be rolled back to.

That has a consequence worth knowing: this image is grown by **every** ordinary install, not only by an explicit `pkg install --image=cix-builder`, so a hostbuild's own inputs move when unrelated things are installed. That was always true; it is now visible. Its *manifest*, though, still lists only what was installed into it directly — the accreted union is real content without a declared list, which is the remaining half of issue #40.

A box upgrading from the flat layout has its existing sandbox folded in as a real version at startup and the old directory **set aside, not deleted** (renamed with the version it produced). What the sandbox contains does not change; only how it is tracked does.

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

`PUT` is a **partial update** — any field left out of the body keeps its existing value; an explicit `"auth_token": ""` is the one way to clear an already-set token, and the token itself is never echoed back by either `GET` or `PUT`, only a derived `auth_token_set` boolean. `repo_kind` is one of `gitea`/`github`/`gitlab` — each forge has a genuinely different archive-download URL shape and auth convention, handled as three separate, explicit branches rather than one generic abstraction (only the `gitea` branch has been verified against a real forge from this project's own dev environment; `github`/`gitlab` follow each forge's own documented API but are unverified against a live account of either kind). `repo_url` needs only `<scheme>://<host>/<owner>/<repo>` — a real forge *browse* URL pasted verbatim (e.g. gitea's own `.../owner/repo/src/branch/<ref>/<path>`, exactly what the address bar shows while looking at a repo) works too (ADR-0133): only the first two path segments are ever read as owner/repo, anything after is ignored, so there's no need to trim it down by hand first. `POST /v1/pkg/sync` starts an async fetch of the configured repo's recipe tree and merges it into this host's own catalog — **merge/additive, never a mirror**: an already-known `(name, version)` is counted as `skipped`, never overwritten (recipe versions stay immutable, ADR-0107), so a sync can never discard a recipe a host already has. `202`, poll `GET /v1/pkg/sync` (`state`: `never`/`running`/`success`/`failed`, plus `added`/`skipped` counts and an `error` string) for the outcome — `400` if no repo is configured yet, `409` if a sync is already running. `sync_interval_seconds` in the repo config (0 = disabled, the default) optionally re-arms an automatic periodic sync on top of the always-available manual `POST`.

**Installs are also backed by a local build-artifact cache and, optionally, a plain-HTTP precompiled-artifact server (ADR-0122)** — deliberately two separate, independently-configured things from the git-forge recipe repo above: recipes are small, versioned text that belongs in git; a compiled package artifact does not, and reusing the same git-forge fetch mechanism for binaries would mean checking them into that same git tree to make them fetchable. Every successful real build is cached locally (`<name>-<version>.tar.gz`, keyed exactly like the recipe itself); installing the identical package into a second image, or reinstalling after deletion, hits the cache instead of re-fetching and recompiling — `GET`/`DELETE /v1/pkg/cache` show/clear what's cached, `GET`/`PUT /v1/pkg/cache-config` show/set its size cap (always a real, enforced cap — least-recently-used entries are evicted first when a new one needs room). A recipe can additionally opt in to network-fetched precompiled artifacts by declaring a `pkg_artifact_sha256=` field alongside its usual `pkg_source=`/`pkg_sha256=` — when a plain-HTTP artifact server is configured (`GET`/`PUT /v1/pkg/artifact-config`, a bare `base_url` + optional bearer token, never a git API), an install first tries `<base_url>/<name>-<version>.tar.gz` and verifies it against the recipe's own declared checksum before ever trusting it; only on a miss (no server configured, 404, checksum mismatch) does it fall back to the recipe's real source and a real compile, exactly as if the artifact tier didn't exist. A recipe that never sets `pkg_artifact_sha256=` never touches this tier at all.

**A built artifact can leave the host that built it, and can publish itself (ADR-0201, issue #129)** — the artifact tier above describes *pulling*; this is the other direction, and it exists because an artifact's default fate used to be to be forgotten. Two halves. First, **export**: a hostbuild's harvested output (`kernel`, `cix`, `isotools`) lands in a plain host directory that is deliberately never container-visible (ADR-0056), so `GET /containers/{name}/files` cannot reach it — it had no retrieval route at all, meaning the self-hosted kernel, roughly a 50-minute build at the end of a four-stage toolchain chain, existed as bytes in exactly one directory on one machine, excluded from backup and wiped by factory reset. `POST /v1/pkg/{name}/artifact/export` tars it (202, poll the GET, then pull `?offset=&length=` chunks), producing `<name>-<version>.tar.gz` — deliberately the exact filename the artifact server serves a package at, so an export is pushed verbatim with no renaming step to drift. This shares one state machine with the image export (issue #126) rather than duplicating it: one export runs at a time, of either kind, matched on both kind and name so an image and a package sharing a name can never be confused. Second, **publish on request**: `POST /v1/pkg/{name}/artifact/publish` sends an artifact that already exists to the configured artifact server — the recovery path for a push that failed, was disabled, or predated the server's configuration, none of which should cost a rebuild. A hostbuild's tarball is built from the installed tree first (same export machinery), and its completion queues the push. This endpoint ran in production before it appeared in this contract; it entered during the ADR-0218 cutover, whose generated dispatch refuses to route an operation the spec does not declare — the exact drift that ADR makes impossible from now on.

Second, **push**: set `"push_enabled": true` (plus an `auth_token`) on `PUT /v1/pkg/artifact-config` and a **genuine fresh build** uploads its own result to the configured server — a `PUT` to exactly the URL a puller will later fetch from, carrying the bearer token and an `X-Cix-Sha256` header. Four properties hold. It never publishes what it did not build: a cache or artifact *hit*'s bytes already came from there, so only the fresh-build branch enqueues. It never becomes a trust boundary: the consumer still verifies every byte against the recipe's own git-tracked `pkg_artifact_sha256`, and the digest header is a corruption check at the door, not a claim taken on faith — the three-way validation (recipe from git, binary from the server, checksum from the recipe) is untouched. It never blocks the event loop: the upload runs in a forked child that also computes the digest. And it is never silent: every reason to skip is logged, and the reaper reports what the server actually said, because a host that silently publishes nothing looks exactly like a host with nothing to publish. Push is **off by default** — publishing is outward-facing, so it is opted into deliberately rather than inherited from having a `base_url` set for pulling.

Cache tarballs are **reproducible** as of the same change (`--sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner`), which is what makes an artifact server's immutability rule usable: two hosts building the same content now produce byte-identical output, so a rejected re-push means two builds genuinely diverged rather than being the expected outcome of ordinary operation.

**Image versions are reclaimable, and before ADR-0209 they were not (`POST /v1/images/gc`)** — every `POST /pkg/install` produces a new immutable image version (ADR-0107/0108), a container pins whichever version was current when it was created, and until this endpoint existed *nothing anywhere in the API ever removed one*. That is not a theoretical leak: 66 versions of an 8 GB rootfs filled a 16 GiB partition, and the disk filling up is how it was noticed. A version is kept if it is its image's `current_version`, if a live container pins it, or if a stored container definition names it — that third case matters because a stopped container has no registry entry at all, and collecting the version its definition points at would leave it permanently unable to start. `{"dry_run": true}` reports what would go without touching anything, which is worth using first since the removal is irreversible. **`{"measure": true}` is opt-in and off by default**, because sizing walks every collectable version and this daemon is single-threaded: on a real host with 80 of them that walk took 117 seconds during which nothing else could be served, and deletion needs no size at all. Without it every `apparent_bytes` is `null`. Refused with `409` while any package job is in flight: a build holds its image's rootfs as its container's lowerdir, and collection is never urgent enough to race one. `apparent_bytes` is the summed file size of what was removed — on btrfs a version is a snapshot sharing extents with its neighbours, so the space actually returned is typically well below it. Measured live on a real host: the walk reported **390 GB apparent** where the filesystem returned **4.5 GB**, which is why the field is named `apparent` and never presented as reclaimed space.

**An image's own whole package list can be declared as one recipe, not N individual manifest edits (ADR-0123)** — `POST /v1/images/recipes` with `{"name": "<image name>", "content": "image_packages=\"bird:pinned:2.19.1 keepalived:rolling:2.3.4\"\n"}` (a recipe's own name is always its target image's name, a 1:1 relationship). `POST /v1/images/{name}/apply-recipe` realizes it, always synchronously (`204`): it bulk-declares the manifest — exactly what N manual `POST /images/{name}/manifest` calls already do — without touching a rootfs, so packages still need real `POST /pkg/install` calls afterward to actually build. **ADR-0209 removed the artifact fast path this used to have**, where a fully-`pinned` recipe declaring an `image_artifact_sha256=` fetched one whole-rootfs tarball and extracted it as the new version, skipping every per-package build. That was the mechanism that shipped one contaminated capture to every host (issue #168), and it was a second representation of something a package set plus a manifest already describes completely. Package artifacts are now the only published binaries, each checksummed and traceable to the recipe and the Cix host that built it.

**A container's own full runtime definition can be declared as one git-syncable recipe, not retyped by hand on every recreation (ADR-0151)** — `POST /v1/containers/recipes` with `{"name": "<name>", "content": "<a complete POST /containers body, as raw JSON text>"}`. Unlike an image recipe's own small declarative format, a container recipe's content already IS the exact JSON `POST /v1/containers` accepts — the recipe's own `"name"` field must equal the name it's published under (a mismatch is rejected, 400), and `POST /v1/containers/recipes/{name}/apply` renders it and creates the container via the identical code path a direct `POST /containers` uses: always synchronous, 201 with the new container. A recipe's `files[].content` may embed `{{SECRET:KEY}}` tokens anywhere inside a string value — `apply`'s own request body's `secrets` object (`{"KEY": "real value"}`) substitutes each one, correctly JSON-escaped, before the container is ever created, so a real credential (an LDAP bind password, say) never has to be committed to git; an unmatched token is left completely untouched, never silently swallowed. `GET /v1/containers/recipes/{name}` always shows the raw, unsubstituted text.

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

Up to `max_concurrent_jobs` installs may be in flight at once (ADR-0157, default 10; see [Package manager](#package-manager-source-based-asynchronous-installs) above) — a `POST /v1/pkg/install` while every chain slot is already busy is `409`. Hostbuild jobs (below) share this exact same pool of slots.

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

`bird` (and its dependencies, resolved the same way as always) builds into `/var/lib/cix/rebuildable/images/router/rootfs` — containers created with `"image": "base"` never see it. The same package name is tracked independently per image: `bash` installed into both `base` and `router` are two separate entries, each independently upgradable/removable. `router` above doesn't need to exist beforehand — the first install into a name never seen before creates it implicitly; `POST /v1/images {"name": "router"}` creates one explicitly instead, useful when you want an image to exist (and be immediately usable — its C runtime is seeded right away) before installing anything into it.

**An image can also carry a real, persisted manifest** (ADR-0107) — declared package intent, distinct from whatever's actually installed right now:

```
POST /v1/images/router/manifest
{"package": "bird", "mode": "pinned", "version": "2.19.1"}
```

`mode: "pinned"` means exactly that version, never auto-advancing; `mode: "rolling"` means `version` is a floor, resolving to the highest available recipe version `>=` it. Re-`POST`ing the same `package` updates its mode/version in place (upsert), never duplicates. `GET /v1/images/router` echoes the full manifest alongside the bare `name` it always returned. `DELETE /v1/images/router/manifest/bird` removes one entry. This only records intent — it does not itself install anything.

**Every install/upgrade/uninstall against an image produces a new, immutable version** (ADR-0108) — `/var/lib/cix/rebuildable/images/router/<version>/rootfs`, where `<version>` is a hash of the image's full installed-package manifest (`name@version` pairs, sorted). Prior versions are never mutated or deleted; a version whose content hash already exists in the image's history is deduplicated (no new directory, `current_version` just repoints). New containers created against `router` are pinned to whatever `current_version` resolves to at creation time (`registry.json`'s own `image_version` field) — they keep running against that exact rootfs even if `router` moves on to a newer version later; only a fresh create or explicit restart-with-replay re-resolves. `GET /v1/images/router` reports both:

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

Most installs merge their build output into a target image's rootfs. A hostbuild job is the second mode of the exact same pipeline: it harvests the output as a standalone artifact on the host instead — used to build the Linux kernel `cixd` boots, and to self-build `cixd`/`cixctl`/`web` themselves from a running Cix host (see [`docs/guides/kernel-build-and-ab-updates.md`](../guides/kernel-build-and-ab-updates.md) and [`docs/guides/building-cix.md`](../guides/building-cix.md) for the full operator runbooks).

```
POST /v1/pkg/hostbuild
{"name": "kernel", "build_image": "cix-builder"}
```

`build_image` is always explicit (no default), and an optional `"version"` field pins the hostbuild to a specific published recipe version exactly like `/pkg/install`'s own (omitted resolves to the highest available) — the already-existing image whose rootfs supplies the build container's own toolchain (must already have whatever the recipe's `pkg_build()` needs actually installed, via ordinary `pkg install` first; a hostbuild recipe cannot itself declare `pkg_depends`, since dependency resolution has no meaning for a one-shot harvest). `202`, polled via `GET /pkg/hostbuild/{name}` (a thin wrapper over the same `GET /pkg/{name}` lookup, scoped to a reserved internal image name) exactly like an ordinary install. Once `state: "installed"`, the artifact lives on the host at a fixed, well-known path per recipe (`kernel.recipe` → a `bzImage`; `cix.recipe` → `cixd`/`cixctl`/`web/`/`cix-install`/`mkinstalleriso` plus a server-side-assembled `cixd-root.squashfs`; `isotools.recipe` → a self-contained `grub-mkrescue`/`sbsign`/`sbverify`/`xorriso`/`mformat`/`mcopy` toolchain) — never merged into any container image's rootfs. `PkgEntry`'s own `files[]` stays empty for a hostbuild entry always, by design (nothing to `pkg_delete()` for a plain host artifact) — `artifact_path`'s own directory listing is the real answer to what a hostbuild produced. A hostbuild already in `state: "installed"` is a bare 409 on a repeat call unless `"upgrade": true` is given and the recipe's own `pkg_version=` has actually moved on (ADR-0094, mirrors `/pkg/install`'s own `upgrade` field exactly). `cixctl pkg hostbuild <name> --build-image=<image> [--wait] [--deploy] [--upgrade]` is the CLI surface; `--deploy` reads the finished artifact and calls the existing, unmodified `/system/update` for you.

### Debugging a failed build: `keep_on_failure` (ADR-0175, issue #35)

A build container that fails is normally torn down immediately, like any other exited container — there's nothing left to inspect afterward. Set `"keep_on_failure": true` on `POST /pkg/install`, `POST /pkg/hostbuild`, or `POST /system/kmod-build` to change that: on a build failure (any nonzero exit from the recipe's `pkg_build()`/`pkg_install()`), the exited build container is left registered and its overlay mounted instead of being removed. The preserved container's own name comes back as `PkgEntry`'s new `kept_build_container` field:

```
POST /v1/pkg/install
{"name": "gcc", "keep_on_failure": true}

# ... poll GET /v1/pkg/gcc until state leaves fetching/building ...

GET /v1/pkg/gcc
{"name": "gcc", "state": "failed", "kept_build_container": "__pkgbuild-0", ...}
```

From there, the preserved container is a completely ordinary, addressable exited container — no new mechanism, reusing two endpoints that already exist:

- `GET /v1/containers/__pkgbuild-0/files?path=/build/src/...` (ADR-0055) reads a crashing binary, a core dump, or any partially-built object tree straight out of the failed build's own overlay upperdir (or lowerdir, for anything untouched from the base toolchain sandbox) for real, offline debugging — the same endpoint already used for reading any exited container's files.
- `DELETE /v1/containers/__pkgbuild-0` tears it down once you're done — an ordinary container delete, nothing preservation-specific about cleanup.

Only ever applies to the single package actually being installed/hostbuilt — an incidental dependency that fails mid-chain is never preserved, matching the existing (and much more common) "just a normal failure" case. `cixctl pkg install --name=NAME --keep-on-failure`, `cixctl pkg hostbuild NAME --build-image=IMAGE --keep-on-failure`, and `cixctl kmod-build --build-image=IMAGE --keep-on-failure` are the CLI surface.

### Continuing a kept build in place: `POST /pkg/resume` (ADR-0177, issue #46)

A preserved build container (above) is useful for inspection, but iterating on a recipe fixup by re-running the whole install from scratch still pays for a full fetch, re-extract, and build restart every time — a real, repeated cost on a long build (a multi-hour compiler bootstrap, restarted after every one-line recipe change). `POST /pkg/resume` continues a `failed` entry's own preserved build container in place instead: no fetch subprocess runs, and the already-extracted source tree inside its overlay is left completely untouched — only the recipe staged inside it (optionally a newly-published, fixed version) and the previous attempt's partial install destination are refreshed.

```
POST /v1/pkg/install
{"name": "gcc", "keep_on_failure": true}

# ... poll GET /v1/pkg/gcc until state leaves fetching/building ...

GET /v1/pkg/gcc
{"name": "gcc", "state": "failed", "kept_build_container": "__pkgbuild-0", ...}

# fix gcc.recipe, publish it as a new version (POST /pkg/recipe), then:

POST /v1/pkg/resume
{"name": "gcc", "version": "12.5.0-5"}

# ... poll GET /v1/pkg/gcc again -- resumes inside __pkgbuild-0's own overlay,
# picking up from its own already-extracted, already-partially-built source tree ...
```

`name`/`image` must address an entry currently `failed` with a non-null `kept_build_container` — `404` otherwise (nothing to resume). `version` is optional and independent of the version the failed attempt originally used: omitting it resolves to the highest available recipe version, same as every other version-optional field in this API (ADR-0107). `409` if the original build's own chain slot has since been claimed by a different, unrelated job (a real, if narrow, possibility once a failed attempt's slot goes idle) — retry once that job finishes. A successful resume tears its build container down automatically afterward, exactly like any other clean build exit — `keep_on_failure` on the resume request itself controls only what happens if *this* attempt also fails. `cixctl pkg resume --name=NAME [--image=IMAGE] [--version=VERSION] [--keep-on-failure]` is the CLI surface.

### Building a fresh installer ISO server-side

`POST /system/iso` closes the one gap the hostbuild mechanism above deliberately left open: assembling those artifacts into a bootable, Secure-Boot-signed installer `.iso` used to be a dev-machine-only tool (`image/src/mkinstalleriso.c`) an operator had to run by hand. It's now a real daemon capability, non-blocking and pidfd-tracked exactly like `POST /pkg/hostbuild`'s own async jobs:

```
POST /v1/system/iso
{"disk": "/dev/sda", "ip": "10.0.0.5", "prefix": "24", "gateway": "10.0.0.1", "interface": "eth0"}
```

Every field is optional — an empty body reproduces the tool's original default, a generic ISO with its kernel arguments left as the `CHANGEME` placeholder an operator edits at the GRUB boot menu. `202`, polled via `GET /system/iso` (`state`: `none`/`building`/`ready`/`failed`, `iso_path` once ready). Reuses whatever the most recent `cix`/`kernel`/`isotools` hostbuild rounds already harvested — it does not trigger any of them itself, and fails fast (`400`) naming exactly which one is missing rather than a background failure the caller has to poll for to discover. Requires a real Secure Boot signing key pair at `<data-dir>/keys/cix-signing.{key,crt,cer}`, installed via `PUT /system/signing-keys` (ADR-0212 — see below; it was an out-of-band-only precondition until then, which no real Cix host had any way to satisfy) — deliberately never generated, fetched, or copied there by `cixd` itself (see ADR-0064: a release-signing private key must never propagate onto every deployed box, only whichever specific instance is actually cutting installer media). `cixctl iso build [--disk=... --ip=... --prefix=... --gateway=... --interface=...] [--wait]` / `cixctl iso status` is the CLI surface. When a release key is installed (`PUT /system/release-key`, ADR-0220) the finished ISO is also minisign-signed and `signature_path` names the `.minisig` beside it; without one the build still succeeds and that field stays `null`.

## Approving a published recipe's artifact

A published `(name, version)` recipe is immutable (ADR-0107) — `POST /v1/pkg/recipes` answers `409` for one that already exists. There is exactly one edit it will accept: **adding `pkg_artifact_sha256=` when the recipe does not already have one, changing nothing else.**

The asymmetry is the point. A recipe's build instructions must not change under a version already installed somewhere — that is the drift immutability prevents. But the artifact checksum is not an instruction, it is an *approval of the bytes those instructions produced*, and it cannot be known until after the version has been built and published — strictly later than the recipe has to exist.

Without this, the circularity had a real cost. `pkg.c` enters the artifact tier only when a recipe declares a checksum, so a package built on a Cix host and pushed to the cache was still rebuilt **from source** on every other host: the artifact existed, was checksum-verified on upload, and was never consulted. On the first real box, **12 of 37 installed packages** sat in the cache unapproved — including `grub`, `python`, `openssl` and `tcc`, the expensive ones. "Rebuild the box from the cache" could not work.

Only absent → present is accepted. Replacing an existing checksum is refused, because one version naming two different byte sequences is exactly what immutability protects against — and it is the failure [#145](https://git.home.arpa/itdlabs/cix/issues/145) documents, where an edited checksum silently stops matching and every install falls back to source with an error that never mentions checksums. An approval smuggled in alongside any other change is refused too; otherwise this would be mutability with extra steps.

The workflow is therefore: publish the recipe, build it on a Cix host, let the artifact push to the cache, then re-post the same recipe with the checksum of those exact bytes added. `pkg_artifact_sha256` still approves one specific byte sequence, and still may only be computed over something a Cix host actually produced (see the Build Provenance Mandate).

## The Secure Boot signing key pair

```
GET /v1/system/signing-keys
PUT /v1/system/signing-keys
{"key": "-----BEGIN PRIVATE KEY-----\n...", "cert": "-----BEGIN CERTIFICATE-----\n..."}
DELETE /v1/system/signing-keys
```

`POST /system/iso` above needs this pair, and until ADR-0212 there was no way to put it on a box. ADR-0064 called it an operator-populated-out-of-band precondition and compared it to a file already `scp`'d onto the machine — but a real Cix host runs no sshd, exposes no host-side exec, and has no console, so **that endpoint has never been servable on any real installed host.** Not a bug in it: a required input with no supported way to supply it.

So the pair is pasted in as two PEM blocks. The DER `.cer` that `mokutil` needs for MOK enrolment is **derived here from the certificate** rather than accepted as a third input — accepting it separately would only create a way for the two to disagree, and that disagreement means an enrolled identity that does not match what actually signed the image.

The certificate is checked against the private key **before anything is written**, and the write is atomic. A mismatched paste is the realistic mistake here — both blobs are individually valid, so nothing else would catch it, and the consequence surfaces much later as an image that signs cleanly and then refuses to boot. A rejected `PUT` leaves the previous pair exactly as it was, which matters on a host actively cutting media. `400` distinguishes an unparseable key, an unparseable certificate, and a well-formed pair that simply is not a pair.

**The private key goes in and never comes back.** `GET` returns `key_set`/`cert_set` booleans plus the certificate's own public identity (`subject`, `not_after`, `fingerprint_sha256` — the fingerprint being what an operator compares against the certificate they enrolled as a MOK). No endpoint, error, or log line ever emits key material. That is the same shape `GET /pkg/repo-config` already has for the git token, which it reports as `auth_token_set`.

ADR-0064's security property is unchanged: `cixd` still never generates, fetches, or copies this key on its own initiative, and only the public DER `.cer` is ever staged onto an installed target. **Install it on the one host that cuts media, not on every box** — a release-signing private key present fleet-wide means any single machine's compromise leaks the fleet's Secure Boot identity. `DELETE` exists so a host being decommissioned or repurposed can drop signing material it no longer needs without being reinstalled.

One caveat worth stating plainly: on a host with `https_enabled: false` and no authentication configured, the pasted key crosses the management network in clear text and anyone on that network can replace it. Enable HTTPS and host auth before treating a host that holds signing material as production.

`cixctl signing-keys [show]` / `cixctl signing-keys set --key=PATH --cert=PATH` / `cixctl signing-keys clear` is the CLI surface — it takes **paths**, not the PEM text, so a private key never lands in shell history or this host's process list. The dashboard's Host → Signing Keys tab is the paste box.

### Publishing the ISO

```
POST /v1/system/iso/publish
```

Signing was necessary but not sufficient. A signed ISO still had no route off the box: `POST /system/iso` writes to local disk and reports a *filesystem path*, the automatic artifact publisher covers packages only, and no endpoint returns an ISO's bytes — so the artifact existed, correctly signed, and nothing could move it.

**The signature uploads first, and that ordering is required.** The cache refuses an ISO with no signature beside it (`409`), which is the right refusal — an unsigned installer is precisely the artifact that must not be downloadable. Publishing in this order means a failure between the two leaves a signature with no ISO (harmless, overwritten by the next attempt) rather than a bootable image nobody can verify. An ISO built with no release key is refused *here*, naming the missing key, instead of letting the cache's 409 describe the symptom while hiding the cause.

Published as `cix-installer-{version}-{release}-{arch}.iso` — the cache's own canonical naming, with the architecture in the name because a checksum cannot tell an aarch64 image from an x86_64 one. The leading `v` is stripped from the version so the cache's own name/version/release split agrees with every package published from the same tag.

This is deliberately **not** routed through the package push queue. That queue publishes freshly built packages keyed by `name@version`; an ISO has neither, since no recipe stands behind it and nothing resolves it by version — which is exactly why the cache counts installers separately from packages. `publish_state`, `published_name` and `publish_error` are reported apart from `state` for a similar reason: a built-but-unpublished ISO is a usable local artifact, and folding the two together would make a perfectly good ISO look broken.

`cixctl iso publish [--wait]` is the CLI surface.

## The release-signing key (ADR-0220)

```
GET /v1/system/release-key
PUT /v1/system/release-key
{"key": "-----BEGIN PRIVATE KEY-----\n..."}
DELETE /v1/system/release-key
```

A **second, separate** key, and the separation is the design rather than an accident of implementation. The pair above is RSA because UEFI mandates RSA, and it answers *may this firmware boot this image?* This one is Ed25519 because minisign mandates Ed25519, and it answers *did Cix publish these bytes?* They are not two encodings of one secret. Sharing a key between those questions would mean whoever can sign a download can also sign a bootloader — and the blast radii are nothing alike: recovering from a leaked release key is republishing a public key, recovering from a leaked Secure Boot key is re-enrolling firmware on every host in the fleet.

This is also why issue #197 could not be built as it was written. It asked for the existing platform key *and* Ed25519 signatures; those are mutually exclusive, and the mismatch would not have surfaced until the first signing call on a real release.

`POST /system/iso` signs its finished ISO with this key when one is installed, writing `cix-install.iso.minisig` beside it and reporting it as `signature_path`. **A missing key does not fail the build** — an unsigned ISO still installs, and most hosts able to build one are not the designated release host and should hold no signing material at all. The absence is reported honestly (`signature_path` stays `null`) rather than being inferred from a successful build. A signing failure *with* a key present is a different thing and is logged as an error: the key is there and did not work, which an operator needs to know before shipping the result as signed.

The signature is plain [minisign](https://jedisct1.github.io/minisign/) in legacy `Ed` mode — Ed25519 over the ISO's own bytes, not over a hash of them, because OpenSSL's Ed25519 is PureEdDSA and signs the message directly, so `cixd` needs no BLAKE2b of its own. Stock minisign verifies both modes. Anyone who downloads a Cix ISO checks it with:

```
minisign -Vm cix-install.iso -p cix-release.pub
```

with no Cix software involved on their side, which is the point: the last check between a substituted ISO and a machine should not be code from the project that produced the ISO. Cix's own test for this uses stock minisign as its oracle, never a verifier written here.

Two format details are load-bearing. The **key id** is the first 8 bytes of `SHA-256(public key)` rather than minisign's random one — the daemon holds a bare PEM key with nowhere to keep a random id, and deriving it means the public key file and every signature agree by construction, with no stored id to drift. The **global signature** covers `signature ‖ trusted comment`, not just the file; omitting it is the usual mistake in hand-rolled minisign writers, and upstream rejects the result. That second signature is what makes the trusted comment a real claim about the artifact — it names the build that produced the ISO, so a verifier learns *which release* these bytes are, not merely that Cix signed something.

**The private key goes in and never comes back**, the same rule the pair above follows. The public key is the deliberate exception: `GET` returns it in minisign's own public-key-file format, verbatim and publishable, because a signature nobody can check is not a signature. `PUT` validates by deriving the public half before writing anything, so an **RSA key is refused here** — the realistic mistake, given the Secure Boot key sitting right beside it — rather than at signing time on a real release. `DELETE` is for a host being decommissioned; signatures it already published stay verifiable, since they are checked against the published public key and not against anything still held on the box.

Generate one with `openssl genpkey -algorithm ed25519`. `cixctl release-key [show]` / `cixctl release-key set --key=PATH` / `cixctl release-key clear` is the CLI surface, taking a **path** rather than PEM text for the same reason as `signing-keys`. The dashboard's Host → Signing Keys tab carries the paste box, below the Secure Boot pair. The same clear-text caveat above applies: enable HTTPS and host auth before a host holds this key.

## Liveness vs. boot identity (ADR-0077)

```
GET /v1/health
{"status": "ok"}

GET /v1/system/boot
{"build_version": "v1.6.0-9-gc59e482-dirty", "build_time": "2026-08-08T00:52:00Z", "slot": "b", "kernel_version": "6.18.40", "bootroot_assembly_started_generation": 3, "bootroot_assembly_completed_generation": 3, "bootroot_assembly_running": false, "bootroot_image_path": "/var/lib/cix/rebuildable/bootroot/cixd-root.squashfs"}
```

`GET /health` is deliberately minimal -- both `cixctl` and the web dashboard poll it every few seconds purely for a status dot, and it's excluded from the audit trail (see [A consolidated log](#a-consolidated-log) above) as low-value polling noise. Build/slot/kernel identity is a separate, lower-frequency check: `GET /system/boot` reports `build_version` (`git describe --tags --always --dirty` at build time), `build_time`, `slot` (`"a"`/`"b"`, or `null` for a dev/test daemon started without `--slot=`), and `kernel_version` (the running `uname(2)` release string). This is the deploy/reboot verification signal referenced throughout [`docs/guides/kernel-build-and-ab-updates.md`](../guides/kernel-build-and-ab-updates.md) -- a `200` from `health` alone only proves *some* daemon answered, not that it's the one you just wrote; `slot`/`kernel_version` from `boot` are the direct answer to "did I actually boot into what I just wrote."

`bootroot_assembly_started_generation`/`bootroot_assembly_completed_generation`/`bootroot_assembly_running` (ADR-0105) are a real freshness signal for `pkg hostbuild cix --deploy`'s own server-side follow-on assembly (ADR-0057) — `cixd-root.squashfs` existing at the hostbuild's `artifact_path` is not the same as it being *this* round's own fresh build, since that file is a leftover from whichever assembly last succeeded. `completed_generation` only ever advances on a real, confirmed success; `--deploy` captures it as a baseline before triggering anything and waits for it to advance past that baseline (`running` distinguishes "still working" from "gave up, that attempt failed") rather than trusting file-exists.

`bootroot_image_path` is where that assembled image actually is, so a client never has to compose the path itself. It used to sit inside the `cix` hostbuild's own artifact directory, and `--deploy` built the path from `artifact_path` — which meant the published artifact carried a ~10 MB squashfs and a ~12 MB staging tree on top of ~1.5 MB of real package content, taking `cix` from 561 KB to 22.8 MB (issue #178). The assembly output lives beside the artifacts now rather than inside one, and the daemon reports where.

## User namespaces: secure by default (ADR-0207)

Every container created without an explicit `userns` field gets its own user namespace — its root mapped onto a dedicated host subordinate-ID range, so a namespace-boundary escape lands as an unprivileged host user, not host uid 0. This is the platform default on a fresh install; `"userns": false` on the create opts a container out, and `PUT /system/daemon-config {"userns_default": false}` restores opt-in platform-wide for operators who need it.

Three rules keep this predictable rather than surprising:

- **The per-container field always beats the default, in both directions.** The platform itself uses the opt-out for its own build/hostbuild containers — trusted internal infrastructure running this project's own checksummed recipes, with no security case for isolating them from the host they build on.
- **A container's mode is pinned at creation.** The resolved value is written into its persisted definition exactly as `image_version` is, so flipping the platform default never changes an existing container on its next restart — a restart that silently swapped a container's isolation mode (and with it its whole storage layout) is precisely the failure the pin exists to prevent. The container object reports its actual mode in its own `userns` field.
- **A host whose kernel or LSM cannot do user namespaces fails loudly at create**, not silently downgraded — a security default that quietly turns itself off is not a default.

## Rolling back: boot a slot once

```
POST /v1/system/boot-next
{"slot": "b"}
```

Before this existed, the answer to "this update is bad, go back" was to wait for the boot counter to exhaust over three reboots, or reinstall (#154).

**The obvious workaround is worse than the problem, which is why this is an endpoint rather than advice.** Repointing the loader `default` at a slot looks like the way to force it — but `default` is *sticky*, so the **next** update stages the *other* slot, matches nothing, and silently never boots. That mistake was made on a real host during this project's own deploy and had to be reverted.

A one-shot has the property that matters for rollback: it **self-clears**. A machine that fails to come back falls back to ordinary selection on its own rather than staying pinned to a broken slot.

- Sets systemd-boot's `LoaderEntryOneShot` EFI variable. The value is the entry id as systemd-boot itself spells it — `.conf` kept, any boot counter stripped, so a file named `cix-b+3.conf` is armed as `cix-b.conf`.
- **A slot with no loader entry is refused (`409`), not armed.** Booting into an entry that does not exist needs a console to recover from — precisely the situation this endpoint exists to avoid.
- `DELETE` is idempotent: disarming an already-disarmed machine is the desired state, so it is safe to call blindly.
- Needs a real EFI system. Where `efivarfs` is absent or read-only — including a dev sandbox, which mounts `/sys` read-only — arming returns `409` saying so rather than failing obscurely.
- Distinct from `PUT /system/esp`, which sets the *persistent* default. Use that to fix a wrong pattern; use this to steer one boot.
- `GET /system/esp` reports an armed one-shot in `boot_next`, and its `selected_entry` follows it. It has to: a one-shot overrides the default pattern entirely, so reporting the pattern's choice while one is armed names the wrong entry — which happened on a real host that said `cix-a.conf` and then booted `cix-b`.

`cixctl boot-next [a|b|clear]` is the CLI surface; with no argument it reports what is armed.

## Host + package updates

```
POST /v1/system/update
{"image_path": "/var/tmp/new-root.squashfs", "kernel_path": "/var/tmp/new-bzImage"}

POST /v1/system/update
{"image_url": "http://192.168.15.31:8080/cix-root-2.0.2.squashfs",
 "image_sha256": "3f7a...c1"}
```

- **`image_url` is how a real installed host actually updates itself (#141).** The `image_path` form assumes someone can already put a multi-megabyte file on the box — but an installed Cix host has no shell and no `scp` target, and the API's own 1 MiB request cap rules out sending a squashfs as a request body. That left `/system/update` unable to update the control plane of the machine it runs on, and every deploy in this project's history routed around it instead. With `image_url` the daemon fetches the image itself, writes it to its own staging path, and from there the existing `image_path` logic runs unchanged — one staging code path, not two. `image_sha256` is **required** alongside it and verified before the image is used anywhere: this call writes a boot slot, so an unverified image would only be caught at the next reboot, which is the worst possible moment to catch it. The daemon resolves the URL itself, so on a host with no working upstream resolver (see [`/system/resolv`](#the-hosts-own-outbound-dns-resolver-adr-0076)) only a literal IP will work. `image_url` and `image_path` are alternatives — give one, not both.
- `image_path`/`kernel_path` are local paths the operator has already transferred onto the box (e.g. `scp`, or a hostbuild artifact already sitting on this same host — see [Hostbuild](#hostbuild-standalone-artifacts-instead-of-merging-into-an-image) above) — there is no upload endpoint; see ADR-0031 for why. Both are optional, but at least one is required — update just the root, just the kernel, or both together in one call. Writes whatever's given onto this daemon's own **inactive** A/B slot (the other one from whichever it's currently running as — `--slot=a` or `--slot=b`) and stages a fresh systemd-boot loader entry with a fresh boot-counter. The kernel is per-slot too (`cix-bzImage-a`/`cix-bzImage-b` on the ESP, both pre-staged identically at install time, ADR-0032). Omitting one of `image_path`/`kernel_path` no longer leaves the inactive slot's own existing copy stale (ADR-0095) — the omitted half is auto-filled from the **active** slot's own currently-running copy instead, so a root-only update still pairs with a kernel that's known-good (already booted), never a leftover from an earlier cycle; the response's own `updated` array is always `["root", "kernel"]` for exactly this reason — both genuinely are fresh in the inactive slot after the call. `400` if this daemon has no `--slot=` (not a real installed system), neither path is given, either path doesn't exist/isn't readable, or either file fails its own on-disk magic check (squashfs's `"hsqs"`, or a bzImage's boot-sector/`setup_header` magic) — checked for both before either is written, so a bad `kernel_path` never leaves a good `image_path` half-applied.
- Response includes `"updated"`, always `["root", "kernel"]` (see above).
- Deliberately does **not** reboot — call `POST /system/reboot` separately once ready to cut over; the existing boot-counter/`confirm_boot()` machinery, entirely unchanged, decides whether the fresh slot sticks.

See [`docs/guides/kernel-build-and-ab-updates.md`](../guides/kernel-build-and-ab-updates.md) for the full build → write → reboot → confirm runbook, and [`docs/guides/staying-updated.md`](../guides/staying-updated.md) for the day-to-day operational picture (this endpoint plus package updates below, together).

```
POST /v1/pkg/update-all
```

- Finds the first installed package (across every image) whose recipe's `pkg_version=` has drifted and starts an upgrade for it, reusing `POST /pkg/install {"upgrade": true}`'s entire existing mechanism — `202` with the started package's state, or `200 {"status": "nothing to update"}` if everything's already current. Starts at most one job per call (a deliberate, still-current design choice, independent of ADR-0157's own concurrency ceiling — each call finds and starts exactly one drifted package, `409` only if every pkg-build chain slot happens to already be busy); call again to find and start the next drifted package, repeating until the whole backlog drains.

## This install's identity (site config)

```
GET /v1/system/site
```

A real, operator-configurable `instance_name`/`site_name`/`domain_suffix` triple (ADR-0046). `instance_name` labels this specific install (dashboard header, CLI, backup bundle) and is always non-empty (defaults to `"cix"`); `site_name`/`domain_suffix` are client tooling's own suggested-FQDN pair (`<name>.<site_name>.<domain_suffix>`, or `<name>.<domain_suffix>` when `site_name` is empty) offered by default when creating a DNS record or issuing a PKI cert with a bare (dot-free) name — a convenience only, never enforced: DNS records and PKI SANs remain plain operator-supplied strings, unaffected by this endpoint's own value once explicitly given with a `.`. Always `200`s — defaults (`"cix"`/`""`/`"internal"`) apply until the first `PUT`.

```
PUT /v1/system/site
{"instance_name": "cix1", "site_name": "lab1", "domain_suffix": "internal"}
```

`instance_name`/`domain_suffix` are required; `site_name` is optional (omitting it entirely is equivalent to `""`, meaning no site tier — a single-site deployment). On success, also best-effort reissues this install's own `"host"` PKI leaf if a root CA is already bootstrapped, and reconciles a single auto-maintained DNS record for the same FQDN if this daemon was started with a real, specific `--bind=` address (not `"0.0.0.0"`/`"127.0.0.1"`, neither of which has one single correct address to publish) — both never fail this request even if they themselves fail. The DNS record is also reconciled once at every daemon startup, so it exists without needing a `PUT` after every restart.

## Backup and restore

```
GET /v1/system/backup
```

Bundles platform *configuration* state — container definitions, networks, DNS records, package install state and recipes, the volume registry, and site config — as one response. **Read this carefully before relying on it for disaster recovery:**

- **Does NOT include workload data.** Each container's own persistent data (a git host's repos, a resolver's zone files, a metrics database) is that container's own concern, backed up with its own native tooling. This endpoint has no way to reach into another container's filesystem and never tries to.
- **Includes the volume registry, but not volume contents.** Which volumes exist and where they are placed is configuration and is in the bundle — it has to be, because container definitions reference volumes by name and an unknown name is a hard `400` ([ADR-0183](../adr/0183-persistent-volumes.md)), so a bundle without it restores onto a box where every container with a volume fails to start. What a volume *holds* is workload data and falls under the previous bullet: a restore recreates the volumes empty, and refilling them is yours to do, the same as image content below.
- **Does NOT include image rootfs content.** Since everything is compiled from source, an image's content is reproducible by re-running `pkg install` for whatever `pkg_installed` records — this bundle is the "shopping list" (what should be installed, where), not the built bytes. Getting all the way back to a fully-populated system after a restore means re-running those installs, not something this endpoint does for you automatically.
- **Never touches PKI, at all.** The CA private key (and every issued leaf certificate's own key) is never returned over the API anywhere in this system, by existing, deliberate design (see [PKI](#pki-a-ca-chain-and-issued-leaf-certificates) above) — that rule isn't bent or partially relaxed here. Back up `/var/lib/cix/state/pki/` (ADR-0141) separately, directly on the host, outside the API entirely.

```
POST /v1/system/restore
{"container_defs": "...", "networks": "...", "dns_records": "...", "pkg_installed": "...", "pkg_recipes": {"hello": "..."}, "site_config": "..."}
```

The reverse of `GET /system/backup` — same shape, every field optional and independent (at least one required), so you can restore just container definitions, just networks, or the whole bundle. Every field is validated (must itself parse as JSON, or for `pkg_recipes`, must be an object of strings) *before* anything is written, so one bad field can't leave the others half-applied — but a real disk-write failure partway through (checked separately, after validation) can: fields already written before a failing one are not rolled back.

**Does not reboot or take effect immediately.** Restored files only get picked up on the next boot — the same startup sequence (including container autostart) that already runs every time. Call the existing `POST /system/reboot` once you're ready to actually cut over. A typical disaster-recovery sequence: boot a fresh install once (normal empty first boot) → `POST /system/restore` with your saved bundle → `POST /system/reboot` → the second boot comes up with your restored state.

`cixctl backup --output=PATH` saves the bundle verbatim (byte-for-byte, not re-serialized) for later use with `cixctl restore --input=PATH` — the same file round-trips exactly. A scheduled backup job (a container with network reachability to `cixd`, or a simple host-level cron entry — either is equally valid, this is a plain REST client either way) can run `cixctl backup` on a schedule and ship the result off-host.

### Automatic backup snapshots (ADR-0141 Phase 5)

```
GET /v1/system/backup-config
{"disk": null, "enabled": false, "interval_hours": 0}
```

Turns the `backup` disk role (present since multi-disk management Phase B, but until this phase a pure inert label nothing ever acted on) into a real, working mechanism. `disk: null` means no automatic snapshots are possible regardless of `enabled` — a disk carrying the `backup` role must be configured first (`POST /diskroles`).

```
PUT /v1/system/backup-config
{"interval_hours": 6}
```

Mirrors `PUT /system/daemon-config`'s own "only the fields given are changed" convention — set just `disk`, just `enabled`, just `interval_hours`, or any combination. `disk` is *not* validated at PUT time (it doesn't need to already be mounted, or even exist yet) — real validation happens at snapshot time, the same "pure bookkeeping vs. real action" split every other storage-placement endpoint here already uses; check `GET .../status` for what actually happened. `interval_hours: 0` means no automatic schedule — `POST .../snapshot-now` remains the only trigger. Changing `enabled` or `interval_hours` re-arms the daemon's own periodic timer immediately, the same shape NTP's hourly auto-sync and `pkg/repo-config`'s own sync interval already use.

```
POST /v1/system/backup-config/snapshot-now
GET  /v1/system/backup-config/status
{"state": "ok", "last_attempt_unixtime": 1786600000}
```

Writes **exactly the same bundle `GET /system/backup` itself produces** — container definitions, networks, DNS records, installed-package state, every on-disk recipe version, site config, and (unchanged from `GET /system/backup`'s own long-standing, deliberate design) **never PKI keys/certs** — to `<mount_path>/backup.json` on the configured disk. A single, always-current snapshot, not a timestamped history: this mechanism exists to guarantee a real, fresh disaster-recovery copy always exists somewhere off the OS disk, not to be a backup-retention system in its own right. Synchronous (a plain JSON write, not a network fetch or external process), so `POST .../snapshot-now`'s own response *is* the resulting status object — no separate poll needed, though `GET .../status` reports the identical thing for checking after the fact or after an automatic run. `state: "never"` if no attempt (manual or automatic) has ever run this daemon lifetime.

`DELETE /v1/diskroles/{name}` and `POST /v1/disks/{name}/format` both refuse (`409`) against the currently configured backup-config disk, the same safety net every storage-placement kind already has — reconfigure `backup-config` (a different disk, or `disk: null`) first.

## Current scope boundaries (v1, deliberate — see ADR-0007)

- No image build/pull endpoint yet — images are provisioned onto disk out of band.
- No authentication yet — the daemon binds to loopback only as its safety boundary for now.
- HTTP: no keep-alive/pipelining (`Connection: close` on every response), no chunked bodies.
- Routes are set-once at creation and not echoed back or introspectable afterward; modifying them on a running container would need a new "enter another netns from outside" primitive, not built yet. See `docs/roadmap/ROADMAP.md`.
- DNS and LDAP server bindings are both persisted (ADR-0091 fixed this for DNS; LDAP's own binding table, task #725, was built with persistence from the start). DNS: only one hosts-format record type; no CNAME/MX/TXT/etc.
- PKI: no certificate revocation/CRL, no CSR-submission flow (the daemon always generates both the keypair and the cert itself). CA regeneration/rotation **is** built (`POST /pki/reset`, above) — that gap has closed since this list was first written.
- Package manager: up to `max_concurrent_jobs` (default/max 10, ADR-0157) install/hostbuild chains may run at once, but each individual dependency chain still resolves and builds serially within itself (a chain's own dependencies-then-target order never parallelizes), and `POST /pkg/update-all`'s own successive calls still compete for that same shared pool of slots — see [Package manager](#package-manager-source-based-asynchronous-installs) above; no version-constrained dependencies (any installed version satisfies a dependency); symlinks in a package's own `DESTDIR` output are skipped (regular files and directories only).
- No scheduled/periodic trigger for `POST /system/update` or `POST /pkg/update-all` — both are on-demand, operator- or cron-invoked; no automatic "update then reboot" chaining.
- Volumes ([above](#persistent-volumes-issue-88-adr-0183)) closed the "no bind-mount concept" gap this list used to record, but have no per-volume quota, no coordination for concurrent sharing between containers, and are not part of the backup bundle. Injecting a *large* binary asset or directory tree at creation time still has no home — creation-time `files` are content-inlined and bounded at 64KiB each.
- Per-connection log lines elsewhere in this daemon (the audit trail, container lifecycle events) still don't carry a peer IP the way `log_tls_error()` now does (ADR-0134) — closed for the one case that was actually flooding a real deployment's logs, not generalized to every log line this daemon writes.

## Why this file exists alongside `openapi.yaml`

One Source of Truth means the *schema* lives in exactly one place (`openapi.yaml`). This page exists only so a human (or a future CLI/web implementer) can get oriented quickly without parsing YAML first — if the two ever disagree, `openapi.yaml` wins and this page is out of date and should be fixed. Per `CLAUDE.md`'s Documentation Map, this file is updated in the same change as any `openapi.yaml` edit, never after.

A note for anyone re-auditing endpoint parity against the daemon's own source: `daemon/src/main.c`'s `dispatch()` is *not* the complete ground truth by itself. [`GET /containers/{name}/console`](#interactive-container-console-docker-exec--it-style) and [`GET /pkg/build/log`](#live-tailing-an-in-flight-package-build-task-676-adr-0101) are both real, correctly-documented endpoints, but — being WebSocket upgrades rather than ordinary request/response calls — they're handled by dedicated upgrade-detection code in the connection read loop (`try_console_upgrade()`/`try_pkg_build_log_upgrade()`) that runs *before* `dispatch()` is ever reached, not by a case inside it. A `dispatch()`-only diff will misreport both as "documented but not implemented."
