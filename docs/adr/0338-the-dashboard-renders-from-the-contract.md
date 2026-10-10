# 0338 — The dashboard renders from the contract; a human authors what it means

## Status

Accepted, 2026-10-10. Issue [#592](https://git.home.arpa/itdlabs/cix/issues/592). Applies the [API-First Mandate](../api/README.md) to the dashboard's own construction, and extends [`web-ux-guidelines.md`](../guides/web-ux-guidelines.md) — which remains the binding design system — rather than creating a second one. Builds on [ADR-0317](0317-permissions-are-declared-by-the-api-contract.md) (the contract already declares permissions, and `sessionMay()` already reads them) and on [ADR-0218](0218-the-api-is-the-daemon-and-decides-what-each-channel-exposes.md) (the contract already decides what each channel exposes). Does not touch [ADR-0243](0243-the-dashboard-terminal-is-a-real-vt.md); the VT is a real TTY stream and is generated from nothing.

## Context

The owner's judgement, 2026-10-10: the dashboard works and is not cohesive — *"many things are in the wrong places, and many are bad UI experience"* — with the bar set as a product an operator trusts, inspired by the confidence Proxmox's interface inspires and explicitly **not** by its appearance.

Measured before anything was designed, because "incoherent" is a claim and not yet a cause:

**The design system is not the problem.** `web/style.css` defines **164 tokens**, already carrying the brand palette by name (`--carbon --ferrite --machined --paper --nickel --copper --phosphor --bus-blue --amber --fault`), the four semantic states with their fills, an 8 px spacing scale, radii, a type scale and both themes. Light mode is already correct where it is easiest to get wrong: `--muted` is `#6b767d`, a *darkened* nickel, because Nickel on Paper is about 2.3:1 and fails. Someone had already solved that.

**The contract is not the problem either, and it is far richer than the UI it serves:**

| | |
|---|---|
| `docs/api/openapi.yaml` | 22,036 lines, 340 operations, 227 paths, **181 schemas**, **107 enums**, 1,982 descriptions |
| Constraint metadata | 9 `pattern`, 48 `minimum`, 32 `maximum`, 5 `maxLength`, 14 `format`, 66 `default` — **174 constraints** |
| `x-cix-*` extensions in use | `permission` (340), `expose` (339), `config-kind` (33), `config-apply` (33), `config-key` (24), `config-state` (13), `rest-param` (6) |
| What `apigen` emits to `web/api.js` | `PERMISSION`, `PATH`, `METHOD` — **three symbols per operation, and nothing else** |
| Who reads the semantic extensions | `daemon/src/api_config.c`, `daemon/src/config.c`, `tools/apigen.c` — **not `web/app.js`** |

So the daemon already renders behaviour from the contract's meaning while the dashboard receives only addresses and permissions, and then **re-types by hand what the contract already states**: 36 hand-written modal forms in `index.html` against 30 request-shaped schemas, every field, label, enum option and table column written twice. The 174 constraints reach the operator as a round-trip `400`.

**A worked example, because the principle should be checked against one real case before it becomes a rule.** `createNetwork`'s `NetworkCreateRequest` against the hand-written `modal-network-form`:

| Field | Hand-written | In the contract |
|---|---|---|
| `name` | `type=text`, required | + `pattern: ^[A-Za-z0-9_-]{1,15}$`, and why (it becomes the bridge's `IFNAMSIZ`-limited name) |
| `subnet` | `type=text`, required | + the host-bits-must-be-zero rule, and the overlap `400` |
| `prefix_len` | `type=number`, required | + `minimum: 8`, `maximum: 30` |
| `address` | `type=text`, optional | + nine lines distinguishing an L2-only bridge from an addressed one |

The generated form is **strictly better than the one we wrote**: today the name box carries no `pattern` and the prefix box no bounds, so a bad value is a round trip, and nothing warns that `subnet`'s host bits must already be zero. Generating it is what finally makes this document's own principle 3 — *"the UI never offers an action the daemon would refuse"* — true rather than aspirational.

**And the drift lives in JavaScript, not CSS**, which decides where the rule has to bite: `web/app.js` is 585 KB with **612 `createElement`**, **207 `className =`** assignments, 31 inline `style.` writes, and **40 of the 53 `dg-*` classes set from JS**. A rule confined to stylesheets would have missed the entire problem.

## Decision

**1. The contract generates structure. A human authors meaning.** The test: if two competent people would write it identically from the schema, generate it; if it takes judgement, author it somewhere a gate can check against the generated structure.

- *Generated*: field names, types, required flags, enum options, constraints, descriptions-as-hints, which operations a resource has, which are destructive, which are reads.
- *Authored*: layout, which columns a table shows and in what order, which tab a thing lives on, the three words on a button, the empty-state sentence.

**2. `apigen` emits two new things**, extending machinery that already parses `components/parameters` and already emits `build/generated/api_shapes.h` (#575):

- `<opId>_SHAPE` — `{ method, path, params, body, response, destructive, kind }`, replacing the three existing symbols and adding what a UI needs in order to pick a widget at all.
- `<SchemaName>_FIELDS` — the ordered field list with type, required, enum, description, readOnly.

`_FIELDS` is the lever: **the modal form, the list table and the key–value table collapse into three renderers over one generated structure.** `openModal(formId, title)` becomes `openForm(opId)`; the confirm fires because `_SHAPE.destructive` says so, not because someone remembered. Adding an operation to the contract then *produces* a correct form, table and detail view with the right permission gate — and the dashboard cannot drift from the contract because it no longer holds a copy of it.

**3. One new extension, `x-cix-ui`, is the only override, and it lives in the contract.** Optional, per field or schema: `{ label?, widget?, hidden? }`, for the minority where the generated default is wrong — a field named `token` wants a password input; `status` wants a badge. Putting it in `openapi.yaml` rather than in `app.js` keeps one source of truth for the override too. `NetworkCreateRequest` needs none of it, so the override surface is smaller than first assumed.

**4. Four layers, and the containment rule has a JavaScript half.** Tokens → primitives → compositions → screens, each consuming only the layer beneath. **A screen-level style rule is a defect, and so is a screen that constructs DOM or sets a class no composition exported.** Without the second clause the rule would not reach `dg-*`, which is where the drift actually is.

**5. The information architecture is authored, and these are the owner's decisions** (accepted 2026-10-10, both flagged alternatives included). Four defects were measured against the tree's own stated rule — *"tabs live on the page; the tree's children are the live things"*:

- Every group label was also a link to an unrelated page: Services → Server Health, Software → Pipeline, Host → host stats. **A group label now expands only.** Nothing becomes harder to reach.
- One page carried nine tabs spanning identity, auth, swap, stats, processes, logs, config and reset — the settings drawer ADR-0258's rework says it removed. **It splits into Health, Identity and Configuration.**
- Logs sat on Host while kmsg sat on Kernel, though [ADR-0070](0070-consolidated-log-store.md) gives the daemon **one** consolidated store with a source filter. **They merge**; the split was a UI invention, not a daemon fact.
- Factory Reset — destructive and irreversible — was a peer tab of Stats, reachable by clicking a group header and then one tab. **It becomes an action**: a danger button with a confirm, last on Configuration. It is no longer somewhere you can land.

## Consequences

- **A new endpoint produces UI.** This is the bar-raising claim, stated so it can be held against us: after this, adding an operation to `openapi.yaml` yields a form, a table and a detail view, correctly gated and correctly confirmed, with no dashboard change at all.
- **The gate runs both ways.** An authored column list cannot name a field the contract lacks, and a contract field no screen places is reported. A new field *surfaces* instead of silently not appearing — the inverse of drift.
- **36 hand-written forms become deletions.** Each conversion removes markup from `index.html` rather than adding more.
- **`apigen`'s output contract changes, and four SELFTESTS read it** — `test_apigen`, `test_apiroute`, `test_api_surfaces`, `test_apishape`. They are read before the generator is touched; a test asserting a fixed symbol count per operation is the first thing to change, and it is found by reading rather than by a red gate.
- **The vocabulary table gains the rows the stylesheet already has** — the session shell, the switch (15 classes), the chart primitives — because a class with no row has no rule saying when it is wrong, and that is the hole drift comes through.
- **`prefers-reduced-motion` is a contract, not a courtesy.** The session transition is the one motion permitted past the brand's 220 ms ceiling, as a meaningful system process; with reduced motion it is a cross-fade.
- **What this does NOT do:** it does not decide the tree, the pages or which tab holds what. Factory Reset's placement was fixed by a person, and no gate can catch the next one of those. "Render from the API" must never become "the API decides the information architecture".

## Alternatives considered

**Keep hand-writing the forms and fix the inconsistencies one by one.** Rejected by arithmetic: 36 forms against 30 schemas means every fix is 36 opportunities to diverge again, and the 174 constraints stay invisible. The guidelines already call a decision made twice a bug to collapse rather than a style to tolerate.

**Generate the whole interface, including layout and placement.** Rejected, and it is the more tempting error. The contract has no vocabulary for *"tabs live on the page; the tree's children are the live things"*, which is a human rule about what this system *is*. A fully generated UI would have produced the forty-column table and kept Factory Reset wherever the schema order put it.

**A second design document for the widget architecture.** Rejected: a parallel design doc would breach One Source of Truth in the very change meant to enforce it. `web-ux-guidelines.md` is extended instead.

**A CSS-only containment rule** ("no screen-level stylesheet rules"). Rejected by measurement: 40 of the 53 `dg-*` classes are set from `app.js`, so the rule would have passed a clean stylesheet over an unchanged problem.
