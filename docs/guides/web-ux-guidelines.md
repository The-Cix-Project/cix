# Web UX Guidelines — the dashboard's design system

This is the standard every change to the web dashboard follows. It exists because the dashboard is clear but had drifted in places: the same idea got expressed two ways, and a widget got swapped for another with no rule saying which was right. The cure is the same one the rest of this project uses — **one source of truth, no parallel implementations** — applied to the interface.

Read this as binding, not advisory. If a change would introduce a second way to say something the dashboard already says, the change is wrong, not the standard. If something here is genuinely inadequate for a new need, change *this document in the same commit* — never route around it.

It is a sibling of [`web-dashboard.md`](web-dashboard.md) (which tours the dashboard for an operator) and of [`building-cix.md`](building-cix.md) (which builds it). This one governs *how it is built* so it stays consistent. The reference implementations it names live in `web/app.js`, `web/index.html` and `web/style.css`.

## The five governing principles

These are the [Immutable Maxims](../mission/MISSION.md) as they apply to an interface. Every rule further down is one of these made concrete.

1. **One Source of Truth.** A fact about a resource — its status, which actions apply to it, what it is called — is computed in exactly one place, and every surface that shows it reads that place. The reference is `diskActionEligibility(d)` in `web/app.js`: it decides which actions a disk or partition offers, and *both* the detail page (`renderDiskRoleTab`) and the tree's right-click menu (`contextMenuItemsFor`) render from it. Before it existed, each decided independently and they drifted — a fix landed in one and not the other. When you find a decision made twice, that is a bug to collapse, not a style to tolerate.

2. **No Parallel Implementations — one widget per job.** There is one status badge, one message toast, one confirm, one context menu. Do not add a second. A page that needs a "status pill" uses the `badge` family; it does not invent `pipeline-badge`. If an existing widget cannot do what you need, extend the existing widget so every caller benefits — never fork it for one page.

3. **API-First: the daemon is the authority, the dashboard is a client.** Every action a widget offers maps to a REST endpoint, and the dashboard never invents behaviour the daemon does not have (see [API-First Mandate](../api/README.md)). Crucially, **the UI never offers an action the daemon would refuse.** Eligibility mirrors the daemon's own rules, so a disabled or absent action reflects a real `409`, not a client guess.

4. **Show live truth, never re-derive it.** State the daemon owns — `mounted`, `protected`, `role`, `status`, `is_os_disk` — is read from its live response and shown as-is. The dashboard does not reconstruct it from other fields or cache it into a second copy. A value that is briefly absent (a partition label just after boot, say) is shown as "not known yet", never guessed.

5. **Name things by what the person recognises.** A control says what happens in the user's vocabulary, not the system's: "Remove", "Unmount", "Grow", not "DELETE /v1/…". The verb on the button matches the sentence in the toast that follows it ("Remove" → "Removed").

## The widget vocabulary

Each widget has exactly one job. The "reach for instead" column is the anti-swap rule: it names what to use when this widget is tempting but wrong.

| Widget | Class / function | Its one job | Reach for instead when… |
|---|---|---|---|
| **Neutral button** | `<button>` (no class) | A non-destructive action (Assign a role, Unmount, Grow, Start) | it destroys data → **danger button**; it only navigates → a **link/tree item** |
| **Danger button** | `.button-danger` | A destructive or irreversible action (Remove, Format, Delete) — **always** paired with a confirm | the action is reversible/neutral → **neutral button** |
| **Compact button** | `.button-small` | A button inside a dense row (a table cell, a toolbar) | it is a page-level action → the full-size button |
| **Status badge** | `.badge` + `.badge-ok` / `.badge-paused` / `.badge-unknown` / `.badge-error` | Show a resource's state at a glance, colour-coded | you need free-form emphasis text → a **hint**; you invented a new `*-badge` class → **stop, use `.badge`** |
| **Tree** | the left nav tree | Answer "what have I got" — the durable inventory, one leaf per real resource | it is an action list → a **context menu**; it is a facet of one resource → **tabs** |
| **Context menu** | `#tree-context-menu`, `contextMenuItemsFor` | Per-item actions on a **tree leaf**, right-click | the node is a page, not an item → no menu; the action is global → the **header menu bar** |
| **Header menu bar** | `.menu-bar`, `.menu-dropdown` | Global navigation and resource creation ("New …") | the action belongs to one existing item → its **context menu** or **detail page** |
| **Modal form** | `openModal(id, title)` / `closeModal()`, `.modal` | Create or edit one resource through a form | you are only showing information → a **detail page**; you are confirming a yes/no → **confirm** |
| **Detail page + tabs** | routed `category/name`, `.detail-topbar`, `.tab-bar` + `.tab-button` + `.tab-panel` | View and act on one resource; tabs split its facets | the resource is one of many being compared → a **list table** |
| **Key–value table** | `.detail-table` + `fieldBlock(label, value)` | Show a resource's fields as label/value rows | the rows are many peer records → a **list table** |
| **List table** | `simpleTableRows(body, cols, n, emptyText)` | Show many peer records, with a built-in empty state | it is one record's fields → a **key–value table** |
| **Status message** | `showStatus(message, isError)` | Every transient success/error after an action | it is a permanent condition → a **banner** or **badge**; you reached for `alert()`/`console` → **stop, use `showStatus`** |
| **Confirm** | native `confirm("specific question")` | Gate every destructive action before it fires | the action is neutral → no confirm (a confirm on a safe action trains people to click through) |
| **Banner** | `.auth-gating-banner` and kin | A standing, page-level condition (auth gating, degraded mode) | it is transient feedback → **status message** |
| **Liveness dot / LED** | `.led`, `.led-*` | A live up/down/among-states indicator | it carries a labelled state value → a **badge** |
| **Hint** | `.hint`, `.hint-inline` | One line of guidance next to a control or empty area | it is an action result → **status message** |
| **Console** | the VT ([ADR-0243](../adr/0243-the-dashboard-terminal-is-a-real-vt.md)) | An interactive terminal into a container | anything that is not a real TTY stream |

Two widgets that look similar are still not interchangeable: a **badge** carries a *value* (a state the daemon reported), a **dot/LED** carries *liveness* (reachable / not). A **banner** is a standing condition, a **status message** is a moment. Pick by which of those the thing actually is, not by which looks nicer in the spot.

## The decision logic (so a widget can never be swapped on a whim)

"I need to…" → use → never:

- **…offer the actions for one tree item** → its **context menu**, built from that resource's single eligibility source → never hand-list actions inline per surface.
- **…create a new resource** → a **modal form** opened from the header **menu bar's** "New …" → never a bespoke inline creation panel.
- **…show one resource's fields** → a **detail page** with a **key–value table** → never a list table of one row.
- **…show many resources** → a **list table** with an **empty state** → never a stack of key–value tables.
- **…report an action succeeded or failed** → **`showStatus`** → never `alert`, never a silent no-op, never `console`.
- **…confirm a destructive action** → **`confirm`** with a question naming the specific thing → never proceed unconfirmed, never confirm a safe action.
- **…show a resource's state** → a **`badge`** with the semantic colour → never a new per-page badge class, never a coloured word of free text.
- **…indicate a service is up/down** → an **LED/dot** → never a badge (a badge is for a reported value, not liveness).
- **…decide whether an action applies** → the resource's **eligibility function**, mirroring the daemon → never a client-side guess, never "offer it and let it 409".

## Interaction patterns

**Actions come from one eligibility source per resource.** For every resource that has conditional actions, one function returns which apply (see `diskActionEligibility`). Every surface — detail page, context menu, any future one — renders from it. Adding a surface never means re-deriving the rules.

**Disabled-with-reason vs hidden.** If an action *could* apply to this kind of resource but is refused *right now*, show it **disabled with its reason** (a mounted partition's "Delete", greyed, titled "Unmount it first"). If the action *never* applies to this kind of resource, **omit it entirely** (a whole disk has no "Grow"). The test: would unblocking one condition make it work? Disable it. Is it categorically wrong here? Hide it. Both the detail page and the context menu follow this — `addContextMenuItem` supports the disabled+reason form for exactly this.

**Destructive actions** are `.button-danger`, are placed **last** in any action group, are gated by a **`confirm`** whose question names the specific target, and end in a `showStatus` toast. All four, every time — a destructive action missing any one of them is incomplete.

**Semantic colour is fixed and separate from the accent.** `badge-ok` = healthy/present/running (green), `badge-error` = failed/broken (red), `badge-paused` = paused/idle/in-progress (amber), `badge-unknown` = unknown/not-applicable (grey). These map to the `--ok` / `--error` / `--paused` / `--muted` tokens (text) and their `--ok-bg` / `--error-bg` / `--paused-bg` / `--unknown-bg` fills, and mean the same thing on every page. Never repurpose a colour to mean something local.

**Every list and panel has three states: loading, empty, populated.** A list uses `simpleTableRows`' empty text; a panel that is genuinely empty says so in a muted line, never a bare blank that reads as "still loading" or "broken". Loading is a distinct state from empty — do not show "none" while a fetch is still in flight (this caused Processes to flash empty twice a second; see `web/app.js`).

**Left-click navigates, right-click acts.** A tree leaf's left-click opens its detail page; its right-click opens the context menu. A menu leads with any navigation entry, then neutral actions, then danger last. Do not put a destructive action where a navigation one is expected.

**Theme.** The dashboard is theme-aware; colours come only from the CSS tokens, never literals, so both themes stay legible. A new colour is a new token, defined for both themes, never a hex value inlined in a component.

## Reference implementations

When in doubt, copy the shape of these — they are the canonical form of each pattern:

- **Eligibility → many surfaces:** `diskActionEligibility(d)`, read by `renderDiskRoleTab` and `contextMenuItemsFor`.
- **Status message:** `showStatus(message, isError)` — the only user-facing feedback path.
- **Modal form:** `openModal(formId, title)` / `closeModal()` + a `submit` listener that calls the endpoint and `showStatus`.
- **Key–value detail:** `fieldBlock(label, value)` into a `.detail-table`.
- **List with empty state:** `simpleTableRows(body, columns, colCount, emptyText)`.
- **Context menu item, incl. disabled+reason:** `addContextMenuItem(item)`.
- **Badge:** `.badge` + one of the four semantic modifiers.

## How this standard has already been applied

Every place the dashboard had drifted from the principles above has been folded back — proof the standard is real, and worked examples of each rule:

- **Disk action drift** — the exemplar this document is built around. The right-click menu and the detail page decided a disk's actions separately and disagreed (the online data-directory grow reached one and not the other). `diskActionEligibility(d)` is now the single source both read (One Source of Truth).
- **Ad-hoc badge classes** ([#459](https://git.home.arpa/itdlabs/cix/issues/459)). The `badge-*` class was built inline at nine call sites, each with its own `state === … ? …` ladder. Collapsed into one `statusBadge(kind)` helper, so the vocabulary and the state→colour mapping live once.
- **The `pipeline-badge` parallel** ([#460](https://git.home.arpa/itdlabs/cix/issues/460)). The build pipeline had its own `pipeline-badge` status-pill system. Removed; the pipeline now uses the one `.badge` widget through `statusBadge(pipelineStatusKind(status))` (No Parallel Implementations).

When a new drift is found and fixed, add it here as a worked example in the same change — and when a new violation is found but not yet fixed, file it in the issue tracker rather than leaving it only in prose.
