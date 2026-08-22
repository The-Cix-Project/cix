# 0186 — Header chrome: the menu bar is not a grid column

## Status

Accepted

Supersedes one decision in [ADR-0184](0184-dashboard-navigation-one-vocabulary.md) — that the header's logo cell is sized and bordered to match the tree column. The vocabulary rule 0184 exists for is untouched.

## Context

ADR-0184 aligned the header menu bar with the tree in two ways at once: the *names* (the menu's topics are the tree's top-level names) and the *pixels* (the logo cell exactly as wide and as tall as the tree column, with a border continuing into the tree panel's own, and the first topic at the content pane's left inset).

The names half is the one that carries meaning, and it holds up. The pixels half was tried on a real screen and did not: at a 240px tree the mark sits in a large empty block, with the whole menu pushed a quarter of the way across the header before it starts. The divider was removed first, on the same reading; removing it left the empty block without the line that had been its justification.

## Decision

The header is ordinary chrome, not a continuation of the layout grid. The mark sits at its own size, the topics start immediately after it, and the header keeps its own padding.

`--tree-width` stays — it is still one named place for "how wide is the tree", read by the layout grid and available to anything that genuinely needs to line up with that column. Nothing in the header does.

The theme toggle also loses its default button styling (solid accent blue) in favour of a plain icon in the page's own text color, which resolves dark on light and white on dark without either being hard-coded. It cycles a display preference; it should not be the loudest control in the header.

## Consequences

Two surfaces still agree on names, which is what the rule was for; they no longer pretend to be one grid, which they are not — the header spans the page, the tree is one column of what is below it.

Nothing about ADR-0184's placement rule, ADR-0185's submenus, or the tree changes.
