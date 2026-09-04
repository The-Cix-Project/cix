# 0243 — the dashboard terminal is a real VT, written here

## Status

Accepted

Supersedes the terminal-fidelity boundary stated in
[ADR-0043](0043-container-console-exec-websocket.md). Everything else in
ADR-0043 — the hand-rolled WebSocket, the `setns()`+double-fork exec, the
PTY allocation — stands unchanged.

## Context

Raised directly by the owner, immediately after
[ADR-0242](0242-a-console-is-a-sized-terminal-of-a-declared-type.md) gave
the console a real terminal size and type: "i want to fix console, lets
build our vt! cix style".

ADR-0242 fixed the daemon's half. `cixctl console` now hands a program the
operator's actual terminal, and `vim`, `htop` and `top` fill it and resize
with it. The dashboard's console did not improve, because its limit was
never the size — it was that there was no screen to draw on at all.

What ADR-0043 shipped was a line buffer. It appended characters to a list
of lines and understood `\r`, `\n`, backspace, tab and SGR colour. Every
cursor-addressing escape was recognised structurally — so it did not leak
into the output as literal garbage — and then discarded. There was no
two-dimensional grid, so "move to row 12 column 40" had nothing to move
to. ADR-0043 named this plainly and accepted it, on the reasoning that
terminal *emulation* was extreme scope next to a WebSocket frame parser.

That reasoning was sound when it was written and it has now expired, for
two reasons. The first is that the boundary became the only thing between
the dashboard and a capability the platform otherwise has: after
ADR-0242, every other layer works, and the renderer is the sole reason an
operator must leave the browser. The second is that "extreme scope" turned
out to be an overestimate of the part that matters. A terminal is a
well-specified state machine over a byte stream and a grid of cells; what
makes emulators large is decades of compatibility with hardware nobody
runs. The subset a Linux container's ncurses actually emits is bounded,
and it is enumerable by capturing what real programs really send.

## Decision

**Write it, in `web/vt.js`, rather than vendoring one.** Same reasoning
ADR-0043 used to hand-roll the WebSocket and ADR-0010 used to refuse a
frontend framework: every layer of this platform is ours. Vendoring would
also have meant a real change of position on ADR-0010 for one surface,
which would need arguing on its own merits rather than arriving as a side
effect of a rendering problem.

**Its own file, not more of `app.js`.** `app.js` is 429 KB of dashboard;
a terminal emulator is a self-contained subsystem with a five-function
interface (`feed`, `fit`, `resize`, `size`, `dispose`) and no knowledge of
WebSockets, the REST API or the dashboard. `cp -r web/*` and
`copy_dir_files()` already stage the whole directory, so a new asset needs
no change to the Makefile, the recipe, `mkbootroot` or any C.

**A cell grid with dirty-row rendering, in the DOM, not a canvas.** A
canvas would render faster and is what a general-purpose emulator would
reach for. It would also lose text selection and copy — which is a real
thing operators do with console output, not a nicety — and would need its
own font metrics and theme plumbing rather than using the tokens the
stylesheet already defines. Rows are `<div>`s that persist across renders
and only rows marked dirty are rebuilt, which keeps a full-screen redraw
to the rows that actually changed.

**One palette, and backgrounds now use the same colours as foregrounds.**
The old stylesheet defined muted background variants (`#5c2323` for red)
separately from the foreground set. That made sense when colour appeared
in short spans. A full-screen program paints backgrounds by the row —
status bars, selections, meters — and rendering a red status bar as maroon
is exactly the misreporting ADR-0043's own stylesheet comment said a
console must never do. Same principle, applied where it now bites. The
palette moved into `vt.js`; the `.term-fg-*`/`.term-bg-*` classes are
deleted rather than left unused, because a stylesheet still carrying them
is a second definition of the same thing.

**Scrollback and the live screen are separate containers.** A row leaving
the top of the screen becomes history and never changes again, so the
transition is one `appendChild` rather than a rebuild, and hiding history
while a full-screen program runs is one toggle. Only lines leaving a
*full-height* screen become history: a line pushed out of a partial scroll
region was never the whole screen, and treating those as history is the
classic way a terminal's scrollback fills with fragments of a status bar.

**Queries are answered, not ignored.** A program that asks for the cursor
position (DSR) or device attributes (DA) and never hears back does not
degrade — it blocks on a read. This is the one class of omission that
hangs rather than renders wrong, which is why both are implemented and
both are asserted by the gate.

## Consequences

`vim`, `htop`, `top` and anything else ncurses-driven render in the
dashboard and resize with the pane. The console sends `term`, `cols` and
`rows` on attach and ADR-0242's resize control message whenever the pane
changes size, so the two halves of the contract now meet.

Mouse reporting (X10 and SGR encodings), bracketed paste, application
cursor keys, the alternate screen, scroll regions, origin mode, insert
mode, DEC special graphics, 256-colour and truecolour SGR are all
implemented because real captured output uses them. Application keypad
mode, DCS/sixel, and double-width lines are parsed to completion and
discarded — consumed rather than skipped, so an unterminated sequence
cannot swallow the screen.

**Verification is asymmetric, and this is the honest part.** Nothing in
this project executes JavaScript — there is no engine on a Cix host, and
adding one to run a test would be a much larger decision than the test is
worth. So the emulator's behaviour cannot be asserted by the gate. It was
verified by capturing real output from real programs in a real container
on 192.168.15.95 (`vim` editing a file, `watch` entering and leaving the
alternate screen, `ls --color` overflowing into scrollback) and replaying
those exact bytes through the emulator in a headless browser: 25 checks,
covering deferred wrap, scroll-region-scoped insert/delete, DSR, DA, DEC
graphics, character insert/delete/erase, alternate-screen isolation and
scrollback, plus a check that markup in terminal output stays text. The
rendering was also confirmed visually against the same captures.

`test_web_vt` asserts what a static scanner honestly can: the wiring. Load
order, that the terminal is fitted and disposed, that the ADR-0242
parameters and resize message are actually sent, that the superseded
renderer and its palette are gone rather than merely unused, that DSR and
DA are answered, and that no part of the render path assigns `innerHTML`
to bytes written by a process inside a container. Each of those was
confirmed to fail when its regression is reintroduced.

The remaining gap is that a behavioural regression in the emulator itself
would not be caught by the gate. Recorded here rather than papered over:
the replay harness exists and can be re-run, but it needs a browser, so it
is a deliberate manual step in the same category ADR-0010 already put
"does the dashboard render correctly".
