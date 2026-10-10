# Web UX Guidelines — the dashboard's design system

This is the standard every change to the web dashboard follows. It exists because the dashboard is clear but had drifted in places: the same idea got expressed two ways, and a widget got swapped for another with no rule saying which was right. The cure is the same one the rest of this project uses — **one source of truth, no parallel implementations** — applied to the interface.

Read this as binding, not advisory. If a change would introduce a second way to say something the dashboard already says, the change is wrong, not the standard. If something here is genuinely inadequate for a new need, change *this document in the same commit* — never route around it.

It is a sibling of [`web-dashboard.md`](web-dashboard.md) (which tours the dashboard for an operator) and of [`building-cix.md`](building-cix.md) (which builds it). This one governs *how it is built* so it stays consistent. The reference implementations it names live in `web/form.js` (the primitives and the three renderers), `web/app.js` (the screens), `web/index.html` and `web/style.css`.

## The six governing principles

These are the [Immutable Maxims](../mission/MISSION.md) as they apply to an interface. Every rule further down is one of these made concrete.

1. **One Source of Truth.** A fact about a resource — its status, which actions apply to it, what it is called — is computed in exactly one place, and every surface that shows it reads that place. The reference is `diskActionEligibility(d)` in `web/app.js`: it decides which actions a disk or partition offers, and *both* the detail page (`renderDiskRoleTab`) and the tree's right-click menu (`contextMenuItemsFor`) render from it. Before it existed, each decided independently and they drifted — a fix landed in one and not the other. When you find a decision made twice, that is a bug to collapse, not a style to tolerate.

2. **No Parallel Implementations — one widget per job.** There is one status badge, one message toast, one confirm, one context menu. Do not add a second. A page that needs a "status pill" uses the `badge` family; it does not invent `pipeline-badge`. If an existing widget cannot do what you need, extend the existing widget so every caller benefits — never fork it for one page.

3. **API-First: the daemon is the authority, the dashboard is a client.** Every action a widget offers maps to a REST endpoint, and the dashboard never invents behaviour the daemon does not have (see [API-First Mandate](../api/README.md)). Crucially, **the UI never offers an action the daemon would refuse.** Eligibility mirrors the daemon's own rules, so a disabled or absent action reflects a real `409`, not a client guess.

4. **Show live truth, never re-derive it.** State the daemon owns — `mounted`, `protected`, `role`, `status`, `is_os_disk` — is read from its live response and shown as-is. The dashboard does not reconstruct it from other fields or cache it into a second copy. A value that is briefly absent (a partition label just after boot, say) is shown as "not known yet", never guessed.

5. **Name things by what the person recognises.** A control says what happens in the user's vocabulary, not the system's: "Remove", "Unmount", "Grow", not "DELETE /v1/…". The verb on the button matches the sentence in the toast that follows it ("Remove" → "Removed").

6. **The contract generates structure; a human authors meaning** ([ADR-0338](../adr/0338-the-dashboard-renders-from-the-contract.md)). The test is whether two competent people would write it identically from the schema. *Generated*, so it cannot drift: field names, types, required flags, enum options, constraints, descriptions-as-hints, which operations a resource has, which are destructive. *Authored*, because it takes judgement: layout, which columns a table shows and in what order, which tab a thing lives on, the three words on a button, the empty-state sentence. This is what finally makes principle 3 true rather than aspirational — a form that carries the schema's own `pattern` and bounds cannot offer an action the daemon would refuse. **Never hand-write a field a schema already describes**; where the generated default is genuinely wrong, the override is `x-cix-ui` in the contract, never a special case in `app.js`.

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
| **Key–value table** |  `renderDetailRows(bodyEl, schema, obj)` over `.detail-table` + `fieldBlock` | Show a resource's fields as label/value rows | the rows are many peer records → a **list table** |
| **List table** | `renderTableRows(bodyEl, schema, rows, columns, emptyText)` over `simpleTableRows` | Show many peer records, with a built-in empty state | it is one record's fields → a **key–value table** |
| **Status message** | `showStatus(message, isError)` | Every transient success/error after an action | it is a permanent condition → a **banner** or **badge**; you reached for `alert()`/`console` → **stop, use `showStatus`** |
| **Confirm** | native `confirm("specific question")` | Gate every destructive action before it fires | the action is neutral → no confirm (a confirm on a safe action trains people to click through) |
| **Banner** | `.auth-gating-banner` and kin | A standing, page-level condition (auth gating, degraded mode) | it is transient feedback → **status message** |
| **Liveness dot / LED** | `.led`, `.led-*` | A live up/down/among-states indicator | it carries a labelled state value → a **badge** |
| **Hint** | `.hint`, `.hint-inline` | One line of guidance next to a control or empty area | it is an action result → **status message** |
| **Console** | the VT ([ADR-0243](../adr/0243-the-dashboard-terminal-is-a-real-vt.md)) | An interactive terminal into a container | anything that is not a real TTY stream |
| **Floating window** | `.log-window` (`#build-log-window`), moved and sized by `makeResizable()` | Watch something that keeps changing -- a build log -- while using the rest of the page. Non-blocking, closable (button and Escape), position and size persisted | the reader must finish with it before doing anything else → a **modal form**; it is a fixed facet of one resource → a **tab** |
| **Session shell** | `#lock-shell` + `.lock-panel`, driven by `lockSession(reason)` / `unlockSession()` ([ADR-0338](../adr/0338-the-dashboard-renders-from-the-contract.md)) | The session boundary, and only that: parts on unlock, converges on lock or expiry. Left panel carries only what is knowable with no session (the wordmark, host reachability, whether the host gates); right panel is the generated login form and nothing else | the condition is standing but the UI is still usable → a **banner**; you reached for `openModal("login-form", …)` → **stop, that is the swap this row exists to end** |
| **Port faceplate** | `.switch-panel`, `.switch-port*` | Answer "what is plugged in where" for a virtual switch, as a physical panel: one jack per port, rx/tx **dots** for liveness, and attached / up / down / unattributed as the port's own state. Position carries meaning | the question is "which ports exist" or the viewer needs to compare figures → a **list table** (a faceplate is spatial, a table is peer records) |
| **Usage gauge** | `.usage-gauge`, `-track`, `-fill` | One proportion of a known whole, read at a glance. **Width carries the magnitude; colour carries the threshold state** (`--ok`, then `--paused` at 75%, `--error` at 90%). The width is what keeps the colour from carrying the state alone, so the pair is legitimate — and the value must appear as text beside it, because a gauge is not a measurement | the value has no known maximum, or precision matters more than shape → **text**; it is a reported state rather than a proportion → a **badge** |
| **Allocation chart** | `.alloc-chart`, `-legend`, `-swatch` | How one whole divides between a few named parts. A legend is always present, since identity must never be colour alone | the reader needs the exact figures → a **list table**; there is only one part → a **usage gauge** |
| **Stat tile** | `.stats-card` in a `.stats-grid` | One headline number with its label — the case where the right answer is *not a chart*. A flat material surface, never a glass card, and never boxed when a plain row would do | the numbers are one resource's fields → a **key–value table**; the number only matters as a trend → a **chart** |

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
- **…keep watching a stream while doing something else** → the **floating window** → never a modal (its overlay blocks the page the reader is trying to use), never an inline panel that grows the page under a refresh. A live stream follows its tail only while the reader is at the end, and says whether it is following.
- **…decide whether an action applies** → the resource's **eligibility function**, mirroring the daemon → never a client-side guess, never "offer it and let it 409".

## Interaction patterns

**Actions come from one eligibility source per resource.** For every resource that has conditional actions, one function returns which apply (see `diskActionEligibility`). Every surface — detail page, context menu, any future one — renders from it. Adding a surface never means re-deriving the rules.

**What the session may do is asked in one place: `sessionMay(opId)`** ([ADR-0317](../adr/0317-permissions-are-declared-by-the-api-contract.md), #544). The permission each operation needs is generated from the contract into `CIX_API.<opId>_PERMISSION` (`web/api.js`); what the session holds comes from `GET /v1/whoami`; whether the host enforces anything comes from `GET /v1/health`. So:

- An eligibility function includes `sessionMay(opId)` for each action it offers, and an action the session lacks is **disabled with `sessionRefusal(opId)` as its reason** -- a grant or a login would unblock it, so disabled, never hidden.
- A header entry or other standalone control names its operation with `data-op="<opId>"`, and `applySessionEligibility()` enables it or disables it with the reason. No control decides this itself.
- A control built at render time -- a row's Remove, a context-menu item -- names its operation too: `gateAction(button, opId)` after it is built, or `op: "<opId>"` on a context-menu item. `gateAction()` only ever *disables*, so it composes with a control's own reason to be disabled (a stopped container's service buttons stay disabled whichever reason came first). A persistent control that must also be re-enabled -- a modal's own submit button -- sets `disabled`/`title` from `sessionMay()` directly, as the `data-op` controls do.
- A panel whose read fails says so through `refusalText(e, what)`: "log in" for a 401, the missing permission (from the 403 body) for a 403, the error otherwise -- never "no <things>", which claims the host was asked. A list panel's empty state is `emptyStateText(listOp, what, "No <things>")`, which shows the refusal whenever that read was refused and the ordinary text otherwise.
- `apiRequest()` refuses a request the session may not make **before sending it**, with the same error shape a daemon 401/403 has (plus `local`), from the contract's `<opId>_PATH` templates. It is a mirror of the daemon's rule, not a second one: whoami is re-read every sweep, and where the two could disagree -- a grant made a moment ago -- the next sweep catches up.
- **Never write a permission word in app.js.** The contract owns the policy; the dashboard only asks.

**Disabled-with-reason vs hidden.** If an action *could* apply to this kind of resource but is refused *right now*, show it **disabled with its reason** (a mounted partition's "Delete", greyed, titled "Unmount it first"). If the action *never* applies to this kind of resource, **omit it entirely** (a whole disk has no "Grow"). The test: would unblocking one condition make it work? Disable it. Is it categorically wrong here? Hide it. Both the detail page and the context menu follow this — `addContextMenuItem` supports the disabled+reason form for exactly this.

**Destructive actions** are `.button-danger`, are placed **last** in any action group, are gated by a **`confirm`** whose question names the specific target, and end in a `showStatus` toast. All four, every time — a destructive action missing any one of them is incomplete.

**Semantic colour is fixed and separate from the accent.** `badge-ok` = healthy/present/running (green), `badge-error` = failed/broken (red), `badge-paused` = paused/idle/in-progress (amber), `badge-unknown` = unknown/not-applicable (grey). These map to the `--ok` / `--error` / `--paused` / `--muted` tokens (text) and their `--ok-bg` / `--error-bg` / `--paused-bg` / `--unknown-bg` fills, and mean the same thing on every page. Never repurpose a colour to mean something local.

**Every list and panel has three states: loading, empty, populated.** A list uses `simpleTableRows`' empty text; a panel that is genuinely empty says so in a muted line, never a bare blank that reads as "still loading" or "broken". Loading is a distinct state from empty — do not show "none" while a fetch is still in flight (this caused Processes to flash empty twice a second; see `web/app.js`).

**Left-click navigates, right-click acts.** A tree leaf's left-click opens its detail page; its right-click opens the context menu. A menu leads with any navigation entry, then neutral actions, then danger last. Do not put a destructive action where a navigation one is expected.

**Theme.** The dashboard is theme-aware; colours come only from the CSS tokens, never literals, so both themes stay legible. A new colour is a new token, defined for both themes, never a hex value inlined in a component.

## Four layers, and the rule has a JavaScript half

Tokens → primitives → compositions → screens. Each layer may consume only the one beneath it ([ADR-0338](../adr/0338-the-dashboard-renders-from-the-contract.md)).

| Layer | What lives there | A defect at this layer looks like |
|---|---|---|
| **Tokens** | every colour, space, radius, type size and duration — `web/style.css`'s `:root`, both themes | a hex literal or a bare `px` in a component |
| **Primitives** | one implementation each of the vocabulary's atoms: button, badge, dot, hint, input, row, panel | a second button family; a `*-badge` class |
| **Compositions** | the assembled patterns: detail page + tabs, list table, modal form, context menu, floating window, session shell, faceplate | a composition reaching for a token directly instead of a primitive |
| **Screens** | the 19 pages. They arrange compositions and supply authored meaning — column choice, labels, placement | **anything below** |

**A screen-level style rule is a defect — and so is a screen that constructs DOM or sets a class no composition exported.** Both clauses, because only the second one reaches where the drift actually is: measured on 2026-10-10, `web/app.js` is 585 KB with 612 `createElement`, 207 `className =` assignments, 31 inline `style.` writes, and **40 of `dg-*`'s 53 classes set from JavaScript**. A rule confined to stylesheets would have passed a clean `style.css` over an unchanged problem.

The practical form: a screen calls `openForm(opId, opts)`, `renderTableRows(bodyEl, schema, rows, columns, emptyText)`, `renderDetailRows(bodyEl, schema, obj)` and the other composition functions, all of which live in `web/form.js`. It does not build a `<table>`, and it does not invent a class name. When a screen needs something no composition offers, **extend the composition so every caller benefits** — the rule the vocabulary already states for widgets, applied one layer up.

Those three signatures are longer than the shorthand this line carried while they were still a plan (`renderTable(schema, rows, columns)`), and the difference is the one thing the shorthand could not express: **the element to render into**. Corrected here when they were built rather than left as an aspiration that no caller could satisfy.

**What is authored and what is derived, per renderer.** `openForm` derives every field, control, bound and hint from the request schema; the title, the button's words and the failure message are authored, and the refresh defaults to the visible view's own refreshers. `renderTableRows` derives each cell's text from its field's type; **which columns, and in what order, is authored** — the `networks` table shows 4 of `Network`'s 7 fields, because `has_address` is a flag the Address cell renders from and `management` and `interfaces` are not what that page is for. `renderDetailRows` derives everything and is authored only by an optional subset: a detail page's job is to show what is known, so a field added to the contract appears rather than waiting for someone to notice.

**One value reads one way everywhere**, which is `fieldText(f, value)`'s whole job and the measurable half of "not cohesive". Counted in `app.js` before it existed: a boolean read `yes`/`no` 16 times, `enabled`/`disabled` 4 and `true`/`false` once, while an absent value read `-` 67 times, `none` 41, `unknown` 12, `unset` 4 and an em dash 3. Now the field decides, with one subtlety the hand-written code had right: an absent boolean whose schema declares a `default` is not unknown.

## Charts and meters

Four rules, because three of the widgets above are data marks and a chart gone wrong is wrong *persuasively*.

**Form before colour, and sometimes the answer is not a chart.** Pick by the data's job — a proportion of a known whole is a **gauge**, a division of one whole is an **allocation chart**, a single headline figure is a **stat tile**, a quantity over time is a line. A number that only matters as itself does not become a chart to look busy.

**Status colour is reserved.** `--ok`, `--paused`, `--error` and `--muted` mean the four semantic states on every page and are **never** reused to tell one series from another. A series that happens to be green does not mean healthy, and that ambiguity is exactly why the rule exists.

**Series colour is assigned in fixed order and never cycled.** `--series-1` … `--series-3`, in that order, so a filter that changes how many series are shown never repaints the survivors — colour follows the entity, not its rank. A fourth series is not a new hue: it folds into "Other", or the chart becomes small multiples.

**Identity is never colour alone.** Two or more series always carry a legend; text wears the text tokens (`--text`, `--muted`), never a series colour. Dark mode is *chosen* rather than flipped — each series gets its own step, legible on `--bg` in that theme.

**One measured gap, recorded rather than left implicit.** The series palette has not been validated against the colour-vision-deficiency separation it needs, and two of its three entries are suspect on inspection: `--series-2` is `var(--nickel)`, which is also `--muted`, so a series wears the secondary-text colour; and `--series-3` is `var(--copper)`, the brand accent whose documented job is focus and selected state, so data wears the focus colour. Both are reasoning from the token definitions, not a validated result — see [#601](https://git.home.arpa/itdlabs/cix/issues/601).

## Responsiveness

Deliberately not specified yet, and said here rather than left as a silence: the dashboard commits to exactly one layout breakpoint (`max-width: 720px`, five rules in `style.css`), and this document has never named the widths it supports, what a too-wide table does, or what the tree does when the window is narrow. Naming them is a decision about who this dashboard is for, which is [#600](https://git.home.arpa/itdlabs/cix/issues/600)'s subject. Until it is answered, do not add a second breakpoint: one undocumented breakpoint is a gap, two are a drift.

## Reference implementations

When in doubt, copy the shape of these — they are the canonical form of each pattern:

- **Eligibility → many surfaces:** `diskActionEligibility(d)`, read by `renderDiskRoleTab` and `contextMenuItemsFor`.
- **Status message:** `showStatus(message, isError)` — the only user-facing feedback path.
- **Modal form:** `openModal(formId, title)` / `closeModal()` + a `submit` listener that calls the endpoint and `showStatus`.
- **Key–value detail:** `fieldBlock(label, value)` into a `.detail-table`.
- **List with empty state:** `simpleTableRows(body, columns, colCount, emptyText)`.
- **Context menu item, incl. disabled+reason:** `addContextMenuItem(item)`.
- **What the session may do:** `sessionMay(opId)`, `sessionRefusal(opId)`, `data-op` + `applySessionEligibility()`, `gateAction(el, opId)`, `refusalText(e, what)`, `emptyStateText(op, what, text)`.
- **Badge:** `.badge` + one of the four semantic modifiers.

## How this standard has already been applied

Every place the dashboard had drifted from the principles above has been folded back — proof the standard is real, and worked examples of each rule:

- **Disk action drift** — the exemplar this document is built around. The right-click menu and the detail page decided a disk's actions separately and disagreed (the online data-directory grow reached one and not the other). `diskActionEligibility(d)` is now the single source both read (One Source of Truth).
- **Ad-hoc badge classes** ([#459](https://git.home.arpa/itdlabs/cix/issues/459)). The `badge-*` class was built inline at nine call sites, each with its own `state === … ? …` ladder. Collapsed into one `statusBadge(kind)` helper, so the vocabulary and the state→colour mapping live once.
- **The `pipeline-badge` parallel** ([#460](https://git.home.arpa/itdlabs/cix/issues/460)). The build pipeline had its own `pipeline-badge` status-pill system. Removed; the pipeline now uses the one `.badge` widget through `statusBadge(pipelineStatusKind(status))` (No Parallel Implementations).
- **Every action and every list panel asks the session** ([#548](https://git.home.arpa/itdlabs/cix/issues/548)). #544 put `sessionMay()` in place for the header and two panels; the rest of the dashboard offered actions a session would be refused and, for a refused read, claimed "No <things>". Now 64 render-time buttons use `gateAction()`, 16 context-menu items carry `op`, 23 list panels use `emptyStateText()`, and `apiRequest()` pre-checks every request -- all reading the one session source.

- **The vocabulary was narrower than the stylesheet** ([#593](https://git.home.arpa/itdlabs/cix/issues/593), [ADR-0338](../adr/0338-the-dashboard-renders-from-the-contract.md)). Five widget families existed in `style.css` with no row here — the session shell, the switch port faceplate (15 classes), the usage gauge, the allocation chart and the stat tile. **A class with no row has no rule saying when it is wrong**, which is the hole the drift below came through, so the table gained all five and the chart rules that govern three of them. Found by counting class families against the table rather than by reading the code: `switch-*` turned out not to be a toggle at all but the best idea in the dashboard — a virtual switch drawn as a physical panel, which is the brand's "mechanism over machinery" principle made literal.

**The session shell, as built ([#597](https://git.home.arpa/itdlabs/cix/issues/597)).** Login was a modal — `openModal("login-form", …)`, the create-a-resource widget doing the session boundary's job — and is now `#lock-shell`: two panels over the dashboard, which is already rendered underneath. They part on unlock and converge on lock, `lockSession(reason)` and `unlockSession()` are the whole API, and the reason is one of `first`, `expired`, `manual` or `refused`, each with its own sentence. The left panel carries only what is honestly knowable with no session — the wordmark, the host, whether it answers, whether it gates — because `GET /v1/health` is the one read a caller without a session may perform and build, slot and kernel all need a login. The right panel carries the credentials and nothing else, and **its fields are generated from `postLogin`'s own request schema**, so the password is obscured because the contract says `format: password`.

Three things in it are worth copying rather than re-deciding. **Motion is the affordance, not the message**: under `prefers-reduced-motion` the panels cross-fade and never translate. **The dashboard is read fresh on unlock** — permissions, the core panels and the visible view — because parting the panels onto the previous session's rows is the same "looks right while being wrong" surface [#562](https://git.home.arpa/itdlabs/cix/issues/562) was filed for. And **the swap is gated, not just described**: `test_web_tree` fails if `openModal("login-form"` appears on a non-comment line of `app.js`, for the reason `test_naming` counts a forbidden spelling rather than preferring the right one in prose.

One violation is still **found and not yet fixed**, so it is filed rather than described here: `dg-*` is a 53-class parallel visual system for one page with 40 of those classes set from JavaScript ([#598](https://git.home.arpa/itdlabs/cix/issues/598)). It becomes a worked example here when it lands, not before.

When a new drift is found and fixed, add it here as a worked example in the same change — and when a new violation is found but not yet fixed, file it in the issue tracker rather than leaving it only in prose.
