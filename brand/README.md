# thinC brand assets

## Files

| File | What it is | When to use it |
|---|---|---|
| `thinc-mark.svg` | The bare mark alone (flat C Cyan) — a fused C-bracket + chevron, chamfered where they join, reads as "C<" | **Default.** CLI banners, favicons, small UI chrome, anywhere space is tight. Use this unless the full lockup is explicitly requested. |
| `thinc-mark-mono.svg` | Same mark, `fill="currentColor"` | Single-color contexts (print, a UI theme that sets its own color, anywhere the gradient doesn't fit). |
| `thinc-mark-os.svg` | Mark + circular "OS" badge, no wordmark | Compact app-icon-style contexts where a bit more identity than the bare mark is useful, but a full wordmark lockup is too much. |
| `thinc-full.svg` | "thin" wordmark + mark + OS badge + "THIN HOST. C CORE." tagline | The full lockup — only when explicitly asked for (hero/marketing contexts, README header, install/boot splash). |
| `thinc-mark.ascii.txt` | Terminal renderings of the concise mark | CLI startup banners, log prefixes, anywhere a graphical asset can't render. |
| `GUIDELINES.md` | Full brand narrative, color system, typography, voice/tone | Reference for anyone writing copy or building UI under the thinC brand. |
| `logo-reference-sheet.png` | Original multi-variant design reference | Historical/design reference only — not a file to embed anywhere; the SVGs above are the real, current assets. |

## The rule

**Use the concise mark (`thinc-mark.svg`) by default, everywhere.** The full lockup (`thinc-full.svg`) is reserved for contexts that explicitly call for it — don't default to it just because there's room.

## Colors

- **C Cyan** `#00C8FF` — the mark's base color (flat, no gradient), the primary accent everywhere.
- **Source White** `#F2F5FA` — primary text/labels on dark surfaces.

Full palette (including the proposed UI color system) is in `GUIDELINES.md`.

## Typography

- Display/headings: **Space Grotesk**
- Body/prose: **IBM Plex Sans**
- Code/data/IDs/paths (and the OS badge + tagline in the logo itself): **JetBrains Mono**

## Regenerating / verifying

Every SVG here is hand-written path data — verify any edit by rendering it, not by trusting the source:

```
rsvg-convert -w <2x-viewbox-width> -h <2x-viewbox-height> brand/thinc-mark.svg -o /tmp/preview.png --background-color=black
```

Then view `/tmp/preview.png` directly. The mark's own path data (the C-bracket + two K-arm triangles) is duplicated identically across `thinc-mark.svg`, `thinc-mark-mono.svg`, `thinc-mark-os.svg`, and `thinc-full.svg` — if you ever change the mark's geometry, update all four in lockstep.
