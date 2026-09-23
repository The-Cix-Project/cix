# Cix Brand — docs/brand/

The Cix brand system ([ADR-0200](../adr/0200-rebrand-to-cix.md)).
Content authority for everything in this folder stays with the owner's own
brand documents; files here are faithful transcriptions/copies of what the
owner provided, replaced only by new versions from them — never edited ad hoc
(see the Documentation Map in the root `CLAUDE.md`).

## Contents

| File | What it is |
|---|---|
| [brand-guidelines.md](brand-guidelines.md) | The full brand system, v1.0 — strategy, naming, verbal identity, colour, typography, product expression, ThinC→Cix migration, governance. Markdown transcription of the owner's `Cix_Brand_Guidelines_v1.0.docx` (32 sections). |
| [cix-logo-reference.webp](cix-logo-reference.webp) | The owner's one-page logo reference sheet: the Cix mark (joined C + X), construction, variations, lockups, clear space, usage do/don'ts. |
| [cix-ui-svg-icons/](cix-ui-svg-icons/) | The owner's SVG set, filed verbatim: the canonical Cix mark in four brand variants (`brand/`) and 102 UI icons (`icons/`, 24×24, `currentColor`, 1.75 stroke) with its own README and manifest. The web dashboard's tree icons are inlined from `icons/` (`TREE_ICONS` in `web/app.js`). |
| [assets/](assets/) | Derived assets: colour swatch chips (`swatch-*.svg`) rendered from the guidelines' palette, so the colour table shows real swatches; and the terminal rendering of the mark — [`cix-logo-ascii.md`](assets/cix-logo-ascii.md) (what it is and how it ships), `cix-logo-ascii.txt` / `cix-logo-ascii-small.txt` (the art itself), and `svg2blocks.py` (the generator that renders them from `cix-mark.svg`). |

## Notes

- The logo reference sheet carries its own earlier palette and type choices
  (Raw Iron/Silicon/C Cyan…, dated V1.0 — May 2024 on the sheet itself); the
  guidelines document is the newer and authoritative source where the two
  differ (Carbon/Ferrite/Copper…). The sheet remains the authority for the
  mark's geometry and lockups until dedicated logo artwork arrives.
- Final custom logo artwork is explicitly still subject to identity
  exploration per the guidelines' own status note — treat the mark as
  directional, not production-locked.
