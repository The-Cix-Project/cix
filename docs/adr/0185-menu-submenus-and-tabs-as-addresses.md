# 0185 — Menu submenus, the last tree folder, and tabs as addresses

## Status

Accepted

Supersedes one decision in [ADR-0184](0184-dashboard-navigation-one-vocabulary.md) — that the menus are one level with headings. Everything else in 0184 stands, including the vocabulary rule this builds on.

## Context

ADR-0184 aligned the header menu bar with the tree and gave the two surfaces one vocabulary. It filed the branch-level names inside Services and System as non-interactive headings over one list, and explicitly rejected mirroring the tree more deeply. Shipped, that reads worse than it argued: the System menu is eleven items under three headings, and a heading is something you read past on the way to finding the item you want.

Two other things were still outstanding at the same time. Monitoring was the last folder in the tree — five leaves (Host Stats, Processes, Log Store, Kernel Log, Server Health), every other group having already collapsed into a tabbed leaf. And collapsing groups into tabs had left a real bug that nothing had noticed: on a tabbed page, clicking a tab only revealed its panel. The panel's data was fetched by a route handler keyed to the address that tab replaced, and nothing navigated, so nothing fetched. `Host > Routes` sat on "Loading…" indefinitely.

## Decision

**A branch with children gets a real submenu, opening on hover.** One submenu per child of that branch in the tree, in that branch's order — so the menu bar mirrors exactly two levels of the tree, and no more. Submenus also open on click, so touch and keyboard reach them; opening one closes its siblings; leaving the item closes it; one that would run off the right edge of the window flips to the left of its parent, which only JavaScript can decide because only JavaScript can measure it.

Pointing at three things beats reading past three headings, and the tree already has exactly this shape, so the second level costs no new vocabulary. The rejection in ADR-0184 was of mirroring the tree *fully* — three levels deep, re-creating the navigation problem the tree itself had just been flattened out of. Two levels is not that.

**Monitoring becomes one tabbed page**, on the same reasoning every other collapse used: its five pages are facets of one question — what is this box doing — and a folder of five single-purpose pages means navigating to find out which one a thing is on. The tree now has no folders at all; the deepest thing in it is one leaf per real resource.

**Every tab on a collapsed page is an address.** Clicking a tab sets the URL to the address that tab replaced, and the router does the rest: the tab's own route handler runs, and the panel has data. This is what makes tabs work at all after a collapse — the bug above was the same design missing its other half — and it makes each tab bookmarkable and back-button-walkable, which is what preserving those old addresses was for. Tabs that are not addresses (a container's own Summary/Hardware/Options/Console) are left alone.

## Alternatives considered

**Keep the headings and shorten the menus by moving actions elsewhere.** Rejected: it trades a real, stated placement rule ("whatever branch a thing lives under is the menu that creates it") for a cosmetic one about menu length. The rule is worth more than the eleven-item list costs.

**CSS-only `:hover` submenus, no JavaScript.** Genuinely simpler, and it was the first instinct. Rejected because it cannot serve click (touch has no hover) or keyboard focus, and cannot flip a submenu that would open off-screen — that needs a measurement. The JavaScript is about twenty lines and covers all three.

**Render every tab's data on arrival at the page, rather than navigating per tab.** Would also fix the "Loading…" bug, and the Catalogue page does exactly this. Rejected as the general answer: it fetches everything for a page whose user is looking at one thing, and it silently drops the addresses each tab was named after — the property that keeps old bookmarks working and lets a link point at a specific tab.

## Consequences

The menu bar mirrors the tree two levels deep and stops there. A new branch child gets a submenu; a new create action goes in the submenu for the branch it lives under. Neither is a judgement call.

Collapsing a group into tabs is now a complete pattern rather than half of one: name each tab after the address it replaces, register that address, and the tab navigates. Anything that skips the registration gets the "Loading…" symptom, which is worth recognising on sight.

One CSS trap is worth recording, because it presents as a positioning bug and isn't: a menu with `overflow-y: auto` is a clip container, so a submenu positioned outside its box has correct layout and correct measurements and is never painted. The scroll cap now lives on the submenus themselves, the only menus here long enough to need it.

Client-side only, as with ADR-0184 — no API, CLI or daemon change.
