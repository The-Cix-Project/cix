# The Cix mark, in a terminal

The canonical Cix mark — the joined C + X with the centred dot,
[`cix-ui-svg-icons/brand/cix-mark.svg`](../cix-ui-svg-icons/brand/cix-mark.svg) —
rendered as Unicode half-block art, for `fastfetch` and anything else that
draws a logo in a TTY.

```
           ▄▄▄▄
      ▄▄███████████▄
    ▄██████▀▀▀▀▀██████▄           ▄▄██████▄
   ████▀▀         ▀█████▄      ▄█████████▀▀
  ████▀     ▄▄      ▀█████▄  ▄█████▀
 ▄███▀    ▄█████▄     ▀▀██▀▄████▀
 ████     ███████        ▄█████
 ▀███▄    ▀█████▀      ▄████████▄
  ████▄     ▀▀       ▄████▀  ▀████▄▄
   ████▄▄         ▄▄████▀      ▀█████▄▄▄▄▄
    ▀██████▄▄▄▄▄██████▀          ▀▀███████▀
      ▀▀███████████▀▀
           ▀▀▀▀
```

## It is rendered, not drawn

This matters, because the first attempt was hand-drawn and wrong: it set
three separate block letters, which is a wordmark, and this platform already
has a canonical glyph that looks nothing like it.

The art is produced by rasterising the real SVG and downsampling it, so the
shape is the mark's own geometry rather than anyone's impression of it. Each
character cell is two vertically-stacked samples rendered as `▀`, `▄`, `█` or
a space, which buys twice the vertical resolution a plain-ASCII rendering
would have.

The accent region is **derived, not chosen by eye**. The dot is a separate
`<circle cx="77.5" cy="75" r="20.75">` in the SVG, so the mark is rendered
twice — whole, and with that element stripped — and the cells that differ
between the two renders are exactly the dot. Regenerating after an artwork
change therefore needs no hand-correction.

The generator lives with the session that produced these files rather than in
the build, because this is a brand asset that changes when the artwork
changes, not something to re-derive on every build.

## Colours

`$1` and `$2` are colour placeholders, the convention fastfetch uses.

| Slot | Brand colour | Hex | RGB escape |
|---|---|---|---|
| `$1` | Nickel | `#98A2A8` | `38;2;152;162;168` |
| `$2` | Copper | `#D87945` | `38;2;216;121;69` |

**Monochrome first**, which is the rule the guidelines state for every
layout: the mark reads completely with no accent at all. Copper falls on one
thing only — the dot, which is the focal point of the glyph — so the accent
identifies rather than decorates. Nickel rather than white for the body,
because the guidelines assign Nickel to secondary text and metadata, and a
logo standing beside a block of system facts is doing that job; pure white
would make the mark shout over the information it introduces.

## Files

| File | Size | Where it ships |
|---|---|---|
| `cix-logo-ascii.txt` | 43x13 | `/usr/share/cix/fastfetch/cix.txt` |
| `cix-logo-ascii-small.txt` | 25x8 | `/usr/share/cix/fastfetch/cix-small.txt` |

## Rendering it

```sh
fastfetch --logo /usr/share/cix/fastfetch/cix.txt \
          --logo-color-1 '38;2;152;162;168' \
          --logo-color-2 '38;2;216;121;69'
```

Selecting it **by name** rather than by path needs two things this platform
does not have yet, both tracked as issues: an `/etc/os-release` carrying
`ID=cix`, and the logo accepted upstream into fastfetch.

## Status

The mark itself is the owner's. This file is only its terminal rendering, and
the brand guidelines are explicit that final logo artwork "remains subject to
dedicated identity exploration and optical refinement before production
lock" — so regenerate this when the SVG changes rather than editing the art
by hand.
