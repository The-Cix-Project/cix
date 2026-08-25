# 0139 — Web dashboard: light/dark theme toggle, and remembering the last-viewed page

## Status

Accepted

## Context

Asked directly, as two related "make the dashboard remember more of itself" requests: (1) have the tree remember which categories are open/closed and which page is showing, always; (2) add light and dark modes.

Investigating (1) against the real code first, rather than assuming a gap existed: tree collapse state was already persisted (`cix-tree-collapsed` in `localStorage`, `renderTree()`/`saveCollapsedCategories()`, predating this ADR) and confirmed working correctly for any category unrelated to the page currently being viewed (verified live: collapsing "Software" while on an unrelated page, reloading, "Software" stays collapsed). `ensureActiveCategoryExpanded()` deliberately force-expands whichever category chain contains the *currently active* page and persists that expansion -- correct behavior (a page whose own containing category is hidden would be actively confusing), not a bug, but real behavior worth stating plainly since a category the current page lives inside will never appear to "stay collapsed" the way an unrelated one does.

What genuinely was missing: a bare load with no hash in the URL at all -- a fresh tab, or a bookmark to the plain origin -- always landed on Containers, with no memory of whatever page was last open. Confirmed live (a fresh headless-browser tab against a running scratch daemon consistently produced `location.hash === ""`).

For (2): `style.css` already had a real, complete `prefers-color-scheme: dark` block driving CSS custom properties -- the dashboard already looked reasonable in a dark-OS browser. What it couldn't do was let an operator choose a theme independent of their OS setting, or force one regardless of what the OS reports.

## Decision

**Last-viewed page**, `cix-last-view` in `localStorage`: `renderCurrentView()` saves `location.hash` every time it resolves to a real, valid view (right after confirming the view element exists -- an unknown/garbage hash is never remembered). On startup, `restoreLastViewIfNoHash()` runs once, before the very first `poll()`, and only acts when `location.hash` is already empty -- an ordinary page reload with a hash already in the URL bar needs nothing from this, the browser already keeps that on its own; this only covers the case where there's genuinely nothing for the browser itself to have remembered. Per-instance hashes (`#containers/my-container`) are remembered exactly the same as category hashes, since they're just as real a "last page."

**Theme toggle**, `cix-theme` in `localStorage`, three states cycled by one header button: `auto` (the default -- no `data-theme` attribute at all, meaning `prefers-color-scheme` in `style.css` is the only thing deciding colors, never duplicated or shadowed by a JS-side default), `light`, `dark`. `light`/`dark` set `document.documentElement`'s `data-theme` attribute, which two new CSS blocks (`:root[data-theme="light"]`, `:root[data-theme="dark"]`, exact copies of the existing base `:root` and `@media (prefers-color-scheme: dark)` blocks respectively) override with via normal CSS specificity -- an attribute selector on `:root` always outranks a bare `:root` a media query targets, regardless of source order, so these reliably win over the OS preference whenever a manual choice is active, and fall straight back to it the moment the attribute is removed (`auto`).

**No flash of the wrong theme.** `index.html` gained a small inline `<script>`, before the stylesheet finishes loading and before `app.js` runs at all, that reads `cix-theme` and sets `data-theme` synchronously if it's `light`/`dark`. `app.js`'s own theme logic only needs to pick the label back up (`currentTheme()` reads the attribute `index.html`'s bootstrap already set) and wire the click handler -- the two are deliberately split, since only the inline, head-blocking half actually matters for avoiding a visible flash; the rest can load however app.js normally does.

## Consequences

- Reopening the dashboard fresh (new tab, bookmark, bare URL) now returns to whatever page was last open, closing the one real gap found in the "remember where I was" investigation. The tree's own collapse memory was already correct and needed no code change -- confirmed live rather than assumed, and the one non-obvious real behavior (a page's own containing category always shows expanded) is now stated plainly in `docs/guides/web-dashboard.md` instead of being silently surprising.
- An operator can now force light or dark regardless of their OS/browser setting, or leave it on Auto to keep following the OS the way the dashboard always has. No change to the underlying color values themselves in any of the three states -- Auto's colors are unchanged from before this ADR, Light/Dark are exact copies of what Auto already produced in each respective OS state, so nothing about the dashboard's actual palette changed, only who controls which one shows.
- Verified with a real headless-browser session (Chromium via `puppeteer-core`) against a scratch daemon: theme cycling through all three states confirmed via computed background color and the `data-theme` attribute at each step, zero console/page errors; reloading with a saved `dark` choice booted directly into dark with no separate light-then-dark transition observed; visiting a page then opening a fresh tab at the bare origin landed back on that exact page (including a per-instance container-detail hash), confirmed via both the restored URL hash and the actually-rendered page heading.
- Zero API/daemon changes -- both features are entirely client-side preferences, `web/` only.
