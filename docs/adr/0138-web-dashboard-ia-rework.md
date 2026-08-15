# 0138 — Web dashboard information-architecture rework: regrouped tree, merged/split pages, closed a real data gap

## Status

Accepted

## Context

Asked directly, separate from any specific bug: "step back and have a look at how the UI tree and items are split... a lack of consistency on what's on a leaf, the number of items on a page." A full audit of the tree definition and every static leaf's render function (`app.js`'s `topLevel`, every `render*`/`refresh*` function) found the taxonomy itself sound but its application inconsistent:

- **System > Server held 10 leaves; every sibling subgroup held 2-5** (PKI=2, DNS=3, NTP=4, LDAP=5) -- a clear outlier that read as "everything left over," not a coherent group. `Software` had the same flat-list shape with no subgrouping at all.
- **Leaf names didn't always match leaf content.** "Root CA" routed to a page that actually held three sections (Root CA bootstrap, Intermediate CA bootstrap, a destructive Reset action) -- Intermediate CA had no leaf of its own despite being a real, independent, user-facing bootstrap step. "Logs" was a 26-line, one-field size-cap form; the real log *viewer* has lived in the always-visible bottom panel since ADR-0129, entirely independent of that leaf.
- **Two thin pages answered one question.** NTP's Status (sync outcome) and Time (current clock + manual override) were separate leaves a user had to visit both of to answer "what time does this box think it is, and can it be trusted" -- the same pattern, at a smaller scale, as the Root CA overload above.
- **One resource's create flow didn't match every other's.** Every creatable resource in this dashboard goes through the shared header `+ Create` modal except Route, which had its own inline form embedded in the Routes page and wasn't even listed in the dropdown.
- **Container detail's default tab, and its Stats/Backup tabs, didn't match how the page is actually used.** Opening a container's own detail view landed on **Console** -- an interactive terminal -- before showing whether the container was even running; Summary and Stats were two separate tabs an operator had to click between to get one picture of container health; the Backup tab's entire content was one sentence explaining backup is *not* per-container, plus a button that navigated away to the real Backup page -- a tab that existed only to say "not here."
- **A real, verified data gap**, raised directly during the same conversation as a concrete example: `GET /networks/{name}` carries no per-container view at all -- a container's own `networks[]` field already has the network-to-IP mapping, but only from the container's own side. An operator on a network's own detail page had no way to see who was using it without opening every container one at a time.

## Decision

**System > Server split into three subgroups sized like its siblings**, each named for what its leaves actually share rather than "whatever's left":
- **Host** -- Daemon, Site (moved from DNS -- general instance identity, not DNS-specific, consumed by DNS *and* PKI), Devices, Routes, Host Swap, Rolling Restart (the latter two split out of the old bundled Daemon page).
- **Monitoring** -- Host Stats, Processes, Log Store (renamed from Logs), Syslog Targets.
- **Maintenance** -- TLS Throttle, Update, Backup.

**Software split into Catalog (Images, Packages, Recipes) and Build Pipeline (Repo & Sync, Cache & Artifacts)**, the same two-tier shape System's own subgroups already use, for the same reason: a flat list of differently-shaped pages (CRUD catalogs vs. build-pipeline config) read better grouped by what they are.

**PKI's "Root CA" leaf split into Root CA and Intermediate CA**, each now a real, independent leaf (PKI: 2 -> 3 leaves) -- Root CA keeps the Reset CA chain action, since a reset regenerates the whole chain regardless of which tier you're looking at.

**NTP's Status and Time merged into one "Time & Sync" leaf** (NTP: 4 -> 3 leaves) -- sync outcome, Sync-now button, current clock, and manual override all on one page, since they're one operator question, not two.

**Route creation moved into the shared `+ Create` modal**, added to the dropdown alongside every other resource; the Routes page itself is now a plain list, matching every other CRUD leaf in the dashboard. No API change -- `POST /system/routes` already existed; this is purely which HTML the same request comes from.

**Container detail: Summary absorbed Stats, Backup was removed, Summary became the default tab** (6 tabs -> 4: Summary/Hardware/Options/Console). The four live graphs render directly on Summary now, with polling keyed off whichever tab is `cd-tab-summary` instead of `cd-tab-stats` -- opening a container immediately shows both identity and live health, and switching to Hardware/Options/Console stops the poll the same way switching off the old Stats tab did. Backup's one sentence of real content (this container's own filesystem is never backed up; platform config is) became a short note at the bottom of Summary with a link to Maintenance > Backup, instead of occupying a whole tab whose only job was pointing elsewhere.

**Network detail gained a "Containers on this network" table**, the reverse of what a container's own Hardware tab already shows about itself -- name, IP, status, cross-referenced client-side from the already-cached container list (`container.networks[].ip`, confirmed against `openapi.yaml`'s `ContainerNetworkAttachment` schema). No new endpoint: `GET /networks/{name}` still returns exactly what it always did; this is client-side aggregation over data already fetched for the tree's own container list, the same "presentation, not a new capability" reasoning ADR-0135's CA-download button already established.

## Consequences

- Every System subgroup now sits in a 2-6 leaf range instead of one outlier at 10; Software gained the same two-tier shape as System for consistency across the two top-level groups that have subgroups at all.
- A leaf's name now matches what's actually on the page it routes to, in every case audited (Root CA, Intermediate CA, Log Store, Time & Sync).
- Full cross-check (`CATEGORY_VIEWS`/`DETAIL_VIEWS` in `app.js` against every `id="view-*"` in `index.html`, and every tree leaf's `hash` against both) confirmed 100% consistent in both directions after the rework -- no dangling routes, no unreachable views.
- Verified with a real headless-browser session (Chromium via `puppeteer-core`) against a scratch daemon: the full tree structure was extracted from the live DOM and matches the planned shape exactly (subgroup names, leaf order, leaf counts); Root CA, Intermediate CA, Time & Sync, Daemon, Host Swap, Rolling Restart, Network detail's new Containers table, the Route creation modal, and Container detail's merged Summary tab (4 tabs, Summary active by default, fields + 4 live charts + backup note all present) were each screenshotted and confirmed rendering correctly with zero console/page errors.
- `docs/guides/web-dashboard.md`'s tree diagram and every affected page bullet rewritten to match; `docs/guides/administration.md`'s two "System > Server > ..." cross-references and `docs/guides/installing.md`'s one "System > Daemon" reference corrected to the new paths. `CHANGELOG.md`'s own historical entries (which described the dashboard as it was *at the time*) are left untouched, per this project's established precedent of not rewriting history when something later changes.
- Zero API/daemon changes -- this is entirely a `web/` client-side reorganization. No new endpoint, no changed response shape, nothing for `thincctl` or a direct API caller to adjust to.
