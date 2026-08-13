# Web dashboard tour

The dashboard is served by `kanxeod` itself, same origin and port as the API (`http://<host>:7620/` -- no separate process, no build step, vanilla HTML/CSS/JS, ADR-0010). It's a pure REST client exactly like `kanxeoctl` -- every action it takes is a call to the same endpoints documented in [`docs/api/README.md`](../api/README.md); this page covers the UI itself, not the API calls behind it.

## Layout

- **Header**: a theme toggle (see Theme below), instance name (once [site config](../api/README.md#this-installs-identity-site-config) is set), a live health badge, an authentication status badge and `Log in`/`Log out` button (see Authentication below), `Reboot`/`Shutdown` buttons, and a **`+ Create`** dropdown covering every resource this dashboard can create directly: Container, Network, Route, Image, Device Mapping, Disk Role, DNS Record, DNS Server, LDAP Server, LDAP SSH Target, LDAP Group, LDAP User, NTP Server, Syslog Target, PKI Certificate, Package Recipe, Bootstrap Build Image, Install Package. Each opens the same shared modal component with the resource's own form.
- **Left tree**: every resource this daemon knows about, grouped by kind. Clicking a leaf navigates to its detail view (the URL's own hash, e.g. `#containers/my-container` -- bookmarkable, shareable, browser-back-button-safe); clicking a category header expands/collapses it (remembered across reloads, ADR-0139 -- a category containing whatever page you're currently viewing is always shown expanded, even if you'd previously collapsed it, so you're never on a page you can't see the tree location of; every other category's own collapsed/expanded state persists exactly as you left it).
- **Main content area**: whatever the current hash routes to -- a list view for a category, or a detail view for one specific resource. Reopening the dashboard with no hash at all (a fresh tab, a bookmark to the plain origin) returns you to whichever page you were last on, instead of always starting at Containers (ADR-0139) -- a hash already present in the URL bar (an ordinary page reload) is unaffected, since the browser already keeps that on its own.
- **Log panel** (bottom of every page, collapsible, ADR-0129): one merged, live-tailing stream -- every mutating request this dashboard itself makes (source `web-ui`, kept entirely client-side, never sent to the server since a browser tab isn't a system component) plus the server's own consolidated log (`kernel`/`kanxeod`/`audit`/`container`, polled in) -- filterable by a source dropdown in the panel's own header. A toast in the top-right corner (auto-dismisses after a few seconds) shows the outcome of the action that just happened; every toast also lands in this same panel as a `web-ui` entry, so nothing shown briefly as a toast is lost once it fades.

## Theme

The header's leftmost button cycles **Auto → Light → Dark → Auto** (ADR-0139). Auto means "follow the browser/OS's own `prefers-color-scheme`" -- the default, and the only state with no explicit color override of its own. Light/Dark force the dashboard's colors regardless of what the OS is set to. The choice is saved (`localStorage`) and reapplied before the page first paints, so switching to Dark and reloading never flashes light for a frame first.

## Authentication (ADR-0144)

The header's `Log in` button opens a modal for username/password; on success the session token is kept in the browser's own `localStorage` (this dashboard's analog of `kanxeoctl`'s `~/.kanxeoctl_token`) and attached automatically to every request from then on, replacing the button with a badge (`logged in: <username>`) and a `Log out` button. Reads (every list/detail view) never need this — they work identically whether logged in or not, exactly like `GET` requests in the API itself. The moment a mutating action (any `+ Create` form, a delete, a start/stop) hits a `401` — because [write-gating](../api/README.md#host-authentication-adr-0144) is active and there's no valid session yet, or a prior one expired — the login modal opens automatically so there's an immediate, actionable next step instead of just a status-bar error; log in and retry the same action.

Gating is inactive by default (no admin group configured, or none has a member yet), so a fresh install's dashboard works with no login at all until an operator sets one up via `PUT /system/hostauth-config` — the `Log in` button is always present, but only ever functionally required once that's done.

## The tree

```
Containers
  <one leaf per container>
Networks
  <one leaf per network>
Software
  Catalog
    Images
    Packages
    Recipes
  Build Pipeline
    Repo & Sync
    Cache & Artifacts
System
  PKI
    Root CA
    Intermediate CA
    Certificates
  DNS
    Records
    Servers
  LDAP
    Servers
    SSH Targets
    Groups
    Users
    Config
  NTP
    Config
    Servers
    Time & Sync
  Host
    Daemon
    Site
    Devices
    Disks
    Routes
    Host Swap
    Rolling Restart
  Monitoring
    Host Stats
    Processes
    Log Store
    Syslog Targets
  Maintenance
    TLS Throttle
    Update
    Backup
```

Reorganized (ADR-0138) from an earlier, flatter shape where a single 10-leaf "Server" group held everything that wasn't PKI/DNS/LDAP/NTP -- now split by what each page actually is: **Host** (core identity/hardware/network config), **Monitoring** (live visibility into what the box is doing), **Maintenance** (protecting and evolving the running system over time), none of them standing out as a dumping ground the way the original single group did. **Software** picked up the same two-tier shape (Catalog vs. Build Pipeline) for the same reason.

Container leaves are colored by live status (running/paused/stopped -- tinted icon, not a separate dot); network leaves are tinted by whether anything is currently attached. Right-clicking a container or network or image leaf opens a context menu with the relevant quick actions (a container's own menu is status-aware: `Start` only appears when stopped, `Pause`/`Unpause`/`Stop` only when applicable, `Remove` always last and marked destructive).

## Container detail view

Four tabs, Proxmox-style: **Summary** (the default tab when you open a container), **Hardware**, **Options**, **Console**.

- **Summary** -- status, image, PID, exit status, command (the entrypoint argv this container was created with, ADR-0100), plus the same four live graphs (CPU/memory/disk/network, hand-rolled canvas rendering, no charting library) a Stats tab used to hold separately (ADR-0138 -- landing on a container's detail page now shows both identity and live health in one place, polling starts automatically the moment the page opens rather than waiting for a manual tab click; see [`docs/api/README.md`](../api/README.md#container-stats) for what each stat number actually means). A short note at the bottom covers backup scope (platform config only, never this container's own filesystem content) with a link to **Maintenance > Backup**.
- **Hardware** -- granted devices/interfaces, assigned IPs per attached network.
- **Options** -- restart policy, depends-on, readiness check, sysctls, staged files -- everything set at creation time. Below those, a **Storage placement** section (ADR-0142) shows which disk (if any) this container's own overlay currently lives on, with a select (populated from disks already carrying the `container-storage` role) and **Migrate** button -- unlike the daemon-wide placements on the Disks page, this briefly stops and automatically restarts the container for the final cutover once the bulk copy finishes, and requires a restart policy other than `"no"` (a persisted definition to restart from).
- **Console** -- a real, interactive shell inside the running container, over the browser's native `WebSocket` object talking directly to the console endpoint (ADR-0043). Deliberately reduced fidelity by design (ADR-0010's "no framework" constraint): a line-buffer renderer with `\r`/`\n`/backspace/Tab and SGR color support, no cursor-addressable screen model -- full-screen redraw programs (`vim`, `top`, `less`) render wrong here specifically. **`kanxeoctl console`** (a real local terminal end to end) has no such limitation -- reach for it instead when you need one of those tools.

## Other detail views

- **Image detail** -- five tabs: containers currently using this image, packages installed into it, **Manifest** (declared `pinned`/`rolling` package intent, ADR-0107 -- add/remove entries directly from here), **Recipe** (a declarative image recipe's own raw text, ADR-0123 -- Edit/Remove, and an "Apply recipe" button that bulk-declares the manifest immediately or, for a fully-pinned recipe with a matching artifact server configured, starts an async whole-rootfs fetch), and **Versions** (every immutable version this image has ever produced, ADR-0108, newest first, with the currently-pinned-for-new-containers one marked). Recipes and packages can be added or removed directly from here.
- **Devices** -- four tabs grouped by bus: USB, PCI, Network, GPU.
- **Package detail** -- two tabs: **Recipe** (the raw `.recipe` text, viewable and editable -- resubmitting goes through the same upsert `POST /pkg/recipes` every other recipe update uses) and **Installed** (every image this package is tracked against, independently).
- **Software > Build Pipeline > Repo & Sync** -- the configured git-forge recipe source (ADR-0121: URL, kind, ref, auth token, sync interval) and a "Sync now" button with the outcome of the most recent attempt (`state`/`added`/`skipped`/`error`) -- the web equivalent of `kanxeoctl pkg repo-config`/`pkg sync`.
- **Software > Build Pipeline > Cache & Artifacts** -- the local build-artifact cache's occupancy (entry count, current/max bytes) with a size-cap field and a "Clear cache" button, plus the configured plain-HTTP precompiled-artifact server (ADR-0122: base URL, auth token) -- the web equivalent of `kanxeoctl pkg cache-config`/`pkg cache-status`/`pkg cache-clear`/`pkg artifact-config`. Both auth-token fields work the same way: leaving the field blank on save keeps whatever token is already configured; a "Clear token" checkbox removes it explicitly.
- **Network detail** -- a "Containers on this network" table (name/IP/status, ADR-0138) is the top section: the reverse of what a container's own Hardware tab already shows about itself, cross-referenced client-side against the already-cached container list (no dedicated API field needed). Below it, attached host interfaces (attach/detach directly from here) and a "Routes on this network" sub-table (routes the kernel resolves to this network's own bridge as their outgoing interface -- read-only filter, remove still works from here).
- **System > Host > Routes** -- the box's own real kernel IPv4 routing table (ADR-0066), created via the header's `+ Create > Route` modal like every other resource (ADR-0138 -- previously its own inline form, the one creatable resource that didn't go through the shared modal). Add/remove real routes directly (ADR-0067 Part 3) -- a route added or removed here is gone on the next reboot unless something else re-applies it, same as any kernel route not backed by persisted Kanxeo state.
- **System > Host > Daemon** -- `kanxeod`'s own listen port, HTTP/HTTPS toggles, and which network it's currently bound to (Part 0.5). The management-network dropdown only lists networks with their own address (repointing anywhere else is refused server-side). A "Bind IP (optional)" field sets a dedicated second address on the management network's own bridge (ADR-0068) -- kanxeod binds there instead of that network's own address; a "Clear bind IP" checkbox reverts to it. Since the dashboard's own requests are relative to the page it was loaded from, saving a change to the port, the management network, or the bind IP disconnects the page the moment it takes effect -- confirmed with a dialog before submitting any of them.
- **System > Host > Site** -- this install's own identity (instance_name/site_name/domain_suffix), used to label it in the dashboard header and backups and to suggest a default FQDN when creating a DNS record or issuing a PKI cert. Filed under Host (ADR-0138) rather than DNS -- it's general instance identity consumed by DNS *and* PKI, not a DNS-specific setting.
- **System > Host > Disks** -- multi-disk management (ADR-0071/ADR-0102/ADR-0104, ADR-0140, ADR-0141, ADR-0142): every real host block device, live from sysfs, flagging which one is the fixed OS disk (never a role/format candidate), each row also showing live usage (a capacity bar, once mounted) and I/O rate. "Assign role…" (also reachable via the header `+ Create > Disk Role`) binds a disk to `container-storage`, `backup`, `state-storage`, `rebuildable-storage`, or `log-storage` -- non-destructive, reversible via "Remove role" (refused, 409, if the disk is the active placement for any of the three storage-singleton kinds below, the currently configured backup-config disk -- see **Maintenance > Backup** -- or a `container-storage`-role disk one or more containers currently have their own storage on, migrate those away first via that container's own detail page). Once a role is assigned, a per-row filesystem select + **Format…** button destructively `mkfs`s (ext4 or btrfs) and mounts the disk -- guarded by a `confirm()` dialog, since the disk is already unambiguous from the row it's on (the same "you already specified which one" reasoning `kanxeoctl disks format NAME` itself uses, not a second typed-name field); likewise refused (409) against any active placement. A container's own storage disk is chosen at creation time (`run --disk=NAME`) and can be moved afterward from that container's own **Options** tab (ADR-0142) -- not from this page. Three placement sections above the table -- **State storage placement**, **Log storage placement**, and **Rebuildable storage placement** -- each show which disk (if any) currently holds that concern, with a select (populated from disks already carrying the matching role) and **Migrate** button to move it: live, no downtime, poll status shown inline once a migration starts, and all three migrate independently of each other.
- **System > Host > Host Swap** -- whether a swap file is currently enabled (ADR-0069), with a size field + Enable button and a Disable button -- useful for memory-heavy package builds on a box with limited RAM.
- **System > Host > Rolling Restart** -- the daemon-wide `jitter_window_seconds` (ADR-0124) used to spread out `follow_rolling` container restarts after a rolling image rebuild -- 0 disables jitter (restart happens immediately).
- **System > Monitoring > Host Stats** -- the host-wide counterpart to a container's own Summary-tab graphs (ADR-0130): four live graphs (CPU/memory/disk/network, same hand-rolled canvas rendering, no charting library) for `GET /system/stats`, plus a load-average text line. Network is every real interface combined (loopback and every container's own veth included), labeled as such. Polls only while this page is open.
- **System > Monitoring > Processes** -- every real process on the box (ADR-0131), correlated to a container if any (a link to that container's own detail page). Fetch-on-demand (a Refresh button), not folded into the global poll loop -- a real process table churns too fast for a full-table re-render every 2s to be anything but noisy. Kill is a real, immediate SIGKILL, guarded by a `confirm()` dialog matching this dashboard's own convention for genuinely destructive actions.
- **System > Monitoring > Log Store** -- configuration only (ADR-0129 moved browsing to the always-visible bottom log panel, see Layout above; renamed from "Logs" to "Log Store" under ADR-0138, since this page has never been where you actually view logs): a size-cap field for the consolidated log store (8 rotating segments, enforced at segment granularity -- not byte-exact) with its own Save button.
- **System > Monitoring > Syslog Targets** -- registered containers (e.g. `syslog-1`/`syslog-2` running `sysklogd`) that container-sourced log lines are also forwarded to as real RFC 3164 UDP datagrams (ADR-0127). Register one via the `+ Create` menu (`Syslog Target`); Unregister per row.
- **System > Maintenance > Backup** -- platform configuration state (container defs, networks, DNS records, package install state + recipes, site config -- never workload data, never image content, never PKI keys): a "Download backup" button (`GET /system/backup`, saved client-side as `kanxeo-backup.json`) and a Restore form (upload a previously-saved file, does not reboot or hot-reload -- reboot separately for it to take effect). An **Automatic snapshots** section below (ADR-0141 Phase 5) turns the `backup` disk role from an inert label into a real mechanism: a disk select (populated from disks already carrying the `backup` role), Enabled toggle, and interval-hours field, plus a "Snapshot now" button and inline status showing the outcome of the most recent attempt (manual or automatic) -- writes exactly the same bundle the Download button produces, to `<mount_path>/backup.json` on the configured disk, a single always-current snapshot rather than a timestamped history.
- **System > Maintenance > TLS Throttle** -- per-source-IP throttling config for repeated failed HTTPS handshakes (ADR-0134): enabled toggle, threshold/window/block-duration fields, a log-interval field (caps how often a repeatedly-failing source's own log line is written, independent of the block threshold -- found live: a legitimate but untrusted browser can flood the log store long before it fails enough to warrant a block), and a live table of every source currently tracked (failure count, blocked state, blocked-until time). Loopback is never throttled or shown here -- a hostile source sharing the box can never lock out this dashboard's own access. A block only ever applies to the HTTPS listener (ADR-0137) -- a source failing HTTPS handshakes keeps its plain-HTTP access throughout.
- **System > PKI > Root CA** -- bootstrap status, a "Download certificate (.crt)" button once bootstrapped, and the destructive "Reset CA chain" action (wipes and regenerates the whole chain, reissuing every tracked leaf).
- **System > PKI > Intermediate CA** -- its own leaf as of ADR-0138 (previously bundled onto the Root CA page under a second heading), same bootstrap-status-plus-download shape, requires the root to already exist. Both pages' download buttons come with inline instructions for trusting the cert on your own device (Windows/macOS/Linux/Firefox all differ -- see [`security.md`](security.md#trusting-the-ca-on-your-own-device) for the full steps); the download itself is a client-side save-as over `cert_pem`, already part of `GET /pki/ca`/`GET /pki/intermediate`'s own response.
- **System > NTP > Time & Sync** -- merged from two previously-separate thin pages (Status, Time -- ADR-0138): the most recent sync attempt's outcome plus a "Sync now" button, and the host's current wall clock plus a manual `clock_settime()` override form, both answering the one real question ("what time does this box think it is, and can it be trusted") on one page instead of two.

## What's deliberately not here

No metrics history beyond what the Summary tab's own short rolling window holds while open, no authentication UI (none exists yet at the API level either). Anything the dashboard can't do, `kanxeoctl` or a direct API call can -- nothing is dashboard-exclusive, per the API-First Mandate.
