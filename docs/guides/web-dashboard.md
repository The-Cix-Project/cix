# Web dashboard tour

The dashboard is served by `kanxeod` itself, same origin and port as the API (`http://<host>:7620/` — no separate process, no build step, vanilla HTML/CSS/JS, ADR-0010). It's a pure REST client exactly like `kanxeoctl` — every action it takes is a call to the same endpoints documented in [`docs/api/README.md`](../api/README.md); this page covers the UI itself, not the API calls behind it.

## Layout

- **Header**: instance name (once [site config](../api/README.md#this-installs-identity-site-config) is set), a live health badge, `Reboot`/`Shutdown` buttons, and a **`+ Create`** dropdown covering every resource this dashboard can create directly: Container, Network, Image, Device Mapping, DNS Record, DNS Server, LDAP Server, LDAP SSH Target, LDAP Group, LDAP User, NTP Server, PKI Certificate, Package Recipe, Bootstrap Build Image, Install Package. Each opens the same shared modal component with the resource's own form.
- **Left tree**: every resource this daemon knows about, grouped by kind. Clicking a leaf navigates to its detail view (the URL's own hash, e.g. `#containers/my-container` — bookmarkable, shareable, browser-back-button-safe); clicking a category header expands/collapses it (remembered across reloads).
- **Main content area**: whatever the current hash routes to — a list view for a category, or a detail view for one specific resource.
- **Log panel** (bottom of every page, collapsible, ADR-0129): one merged, live-tailing stream — every mutating request this dashboard itself makes (source `web-ui`, kept entirely client-side, never sent to the server since a browser tab isn't a system component) plus the server's own consolidated log (`kernel`/`kanxeod`/`audit`/`container`, polled in) — filterable by a source dropdown in the panel's own header. A toast in the top-right corner (auto-dismisses after a few seconds) shows the outcome of the action that just happened; every toast also lands in this same panel as a `web-ui` entry, so nothing shown briefly as a toast is lost once it fades.

## The tree

```
Containers
  <one leaf per container>
Networks
  <one leaf per network>
Software
  Images
  Packages
  Recipes
  Repo & Sync
  Cache & Artifacts
System
  PKI
    Root CA
    Certificates
  DNS
    Records
    Servers
    Site
  LDAP
    Servers
    SSH Targets
    Groups
    Users
    Config
  NTP
    Config
    Servers
    Status
    Time
  Server
    Daemon
    Devices
    Routes
    Host Stats
    Processes
    Syslog Targets
    Logs
    Update
    Backup
```

Container leaves are colored by live status (running/paused/stopped — tinted icon, not a separate dot); network leaves are tinted by whether anything is currently attached. Right-clicking a container or network or image leaf opens a context menu with the relevant quick actions (a container's own menu is status-aware: `Start` only appears when stopped, `Pause`/`Unpause`/`Stop` only when applicable, `Remove` always last and marked destructive).

## Container detail view

Six tabs, Proxmox-style: **Summary**, **Hardware**, **Options**, **Stats**, **Console** (the default tab when you open a container), **Backup**.

- **Summary** — status, image, PID, exit status, command (the entrypoint argv this container was created with, ADR-0100), assigned IPs.
- **Hardware** — granted devices/interfaces, memory/pids limits.
- **Options** — restart policy, depends-on, readiness check, sysctls, staged files — everything set at creation time.
- **Stats** — four live graphs (CPU/memory/disk/network), hand-rolled canvas rendering, no charting library, polling only while this tab is open (see [`docs/api/README.md`](../api/README.md#container-stats) for what each number actually means).
- **Console** — a real, interactive shell inside the running container, over the browser's native `WebSocket` object talking directly to the console endpoint (ADR-0043). Deliberately reduced fidelity by design (ADR-0010's "no framework" constraint): a line-buffer renderer with `\r`/`\n`/backspace/Tab and SGR color support, no cursor-addressable screen model — full-screen redraw programs (`vim`, `top`, `less`) render wrong here specifically. **`kanxeoctl console`** (a real local terminal end to end) has no such limitation — reach for it instead when you need one of those tools.
- **Backup** — no relation to `/system/backup`; this tab is about the container's own definition (the persisted create-request, if any) for reference/reissue.

## Other detail views

- **Image detail** — four tabs: containers currently using this image, packages installed into it, **Manifest** (declared `pinned`/`rolling` package intent, ADR-0107 — add/remove entries directly from here), and **Versions** (every immutable version this image has ever produced, ADR-0108, newest first, with the currently-pinned-for-new-containers one marked). Recipes and packages can be added or removed directly from here.
- **Devices** — four tabs grouped by bus: USB, PCI, Network, GPU.
- **Package detail** — two tabs: **Recipe** (the raw `.recipe` text, viewable and editable — resubmitting goes through the same upsert `POST /pkg/recipes` every other recipe update uses) and **Installed** (every image this package is tracked against, independently).
- **Software > Repo & Sync** — the configured git-forge recipe source (ADR-0121: URL, kind, ref, auth token, sync interval) and a "Sync now" button with the outcome of the most recent attempt (`state`/`added`/`skipped`/`error`) — the web equivalent of `kanxeoctl pkg repo-config`/`pkg sync`.
- **Software > Cache & Artifacts** — the local build-artifact cache's occupancy (entry count, current/max bytes) with a size-cap field and a "Clear cache" button, plus the configured plain-HTTP precompiled-artifact server (ADR-0122: base URL, auth token) — the web equivalent of `kanxeoctl pkg cache-config`/`pkg cache-status`/`pkg cache-clear`/`pkg artifact-config`. Both auth-token fields work the same way: leaving the field blank on save keeps whatever token is already configured; a "Clear token" checkbox removes it explicitly.
- **Network detail** — attached interfaces (attach/detach directly from here), IP allocation, and a "Routes on this network" sub-table (routes the kernel resolves to this network's own bridge as their outgoing interface — read-only filter, remove still works from here).
- **System > Server > Routes** — the box's own real kernel IPv4 routing table (ADR-0066); moved here (under System's Server group) from the Networks tree, since it reads as system-level diagnostic state, not a Kanxeo-managed network resource. Add/remove real routes directly (ADR-0067 Part 3) — a route added or removed here is gone on the next reboot unless something else re-applies it, same as any kernel route not backed by persisted Kanxeo state.
- **System > Server > Daemon** — `kanxeod`'s own listen port, HTTP/HTTPS toggles, and which network it's currently bound to (Part 0.5). The management-network dropdown only lists networks with their own address (repointing anywhere else is refused server-side). A "Bind IP (optional)" field sets a dedicated second address on the management network's own bridge (ADR-0068) — kanxeod binds there instead of that network's own address; a "Clear bind IP" checkbox reverts to it. Since the dashboard's own requests are relative to the page it was loaded from, saving a change to the port, the management network, or the bind IP disconnects the page the moment it takes effect — confirmed with a dialog before submitting any of them. A "Host swap" block on the same page (ADR-0069) shows whether a swap file is currently enabled, with a size field + Enable button and a Disable button — useful for memory-heavy package builds on a box with limited RAM.
- **System > Server > Host Stats** — the host-wide counterpart to a container's own Stats tab (ADR-0130): four live graphs (CPU/memory/disk/network, same hand-rolled canvas rendering, no charting library) for `GET /system/stats`, plus a load-average text line. Network is every real interface combined (loopback and every container's own veth included), labeled as such. Polls only while this page is open, same as the container Stats tab.
- **System > Server > Processes** — every real process on the box (ADR-0131), correlated to a container if any (a link to that container's own detail page). Fetch-on-demand (a Refresh button), not folded into the global poll loop -- a real process table churns too fast for a full-table re-render every 2s to be anything but noisy. Kill is a real, immediate SIGKILL, guarded by a `confirm()` dialog matching this dashboard's own convention for genuinely destructive actions.
- **System > Server > Syslog Targets** — registered containers (e.g. `syslog-1`/`syslog-2` running `sysklogd`) that container-sourced log lines are also forwarded to as real RFC 3164 UDP datagrams (ADR-0127). Register one via the `+ Create` menu (`Syslog Target`); Unregister per row. The `+ Create` header dropdown also gained this entry alongside every other "register a container as X" form (DNS Server, LDAP Server, NTP Server).
- **System > Server > Logs** — configuration only (ADR-0129 moved browsing to the always-visible bottom log panel, see Layout above): a size-cap field for the consolidated log store (8 rotating segments, enforced at segment granularity — not byte-exact) with its own Save button.

## What's deliberately not here

No metrics history beyond what the Stats tab's own short rolling window holds while open, no authentication UI (none exists yet at the API level either). Anything the dashboard can't do, `kanxeoctl` or a direct API call can — nothing is dashboard-exclusive, per the API-First Mandate.
