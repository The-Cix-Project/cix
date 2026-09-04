# 0242 — a console is a sized terminal of a declared type

## Status

Accepted

## Context

Raised directly by the owner: "I want us to extend our terminal/console
implementation to support things like htop btop vim and so on. what's
missing? what's the best path for this?"

The honest answer was that the hard part had been built correctly in
ADR-0043 and the easy part had never been done at all. `exec_into_container()`
allocates a real PTY, `setsid()`s, makes it the controlling terminal with
`TIOCSCTTY`, and relays it over a WebSocket. That is the mechanism a
full-screen program needs. But a program like `htop` or `vim` asks the
terminal two questions before it draws a single cell, and this endpoint
answered neither:

- **How big are you?** `ioctl(TIOCGWINSZ)`. A PTY created by `posix_openpt()`
  has a window size of 0×0 until something sets one, and nothing ever did.
  There was no `TIOCSWINSZ` call anywhere in the codebase.
- **What can you do?** `$TERM`, naming an entry in the terminfo database.
  The exec'd process inherited the daemon's own `environ`, and nothing in
  the daemon ever set `TERM`.

**The failure mode is worse than a refusal, because it is silent.** Measured
on 192.168.15.95 against the `jump` container before the fix, rather than
reasoned about: `stty size` inside a console session reports `0 0`, and
`$TERM` is `linux` — `cixd` runs as pid 1 from the bootloader, so what it
passes on is the kernel console's own terminal type. ncurses does not fail
in that state. It falls back to the terminfo entry's own `lines#`/`cols#`,
so `tput cols`/`tput lines` answer 80 and 24, and a real `vim` starts
perfectly happily — the same probe captured it emitting `ESC[1;24r`, setting
itself a 24-line scroll region.

So a full-screen program ran, and drew into the top-left 80×24 corner of
whatever the operator's terminal actually was, forever, no matter how large
that window was or how it was resized. And every session claimed to be a
Linux virtual console regardless of which client was actually attached,
mis-describing the colour and key-sequence capabilities of anything that is
not one. A plain interactive shell needs neither answer and is unaffected,
which is exactly why this survived unremarked from ADR-0043 until someone
asked about `htop`.

An earlier draft of this ADR said such programs "refused to start". That was
inference, and the probe above disproved it before it shipped — the defect is
real, but its symptom is a wrong fixed size, not an error message.

Two things were already in place and needed no work, confirmed rather than
assumed. The `ncurses` package installs the complete terminfo database —
2903 entries at `usr/share/terminfo`, `xterm` and `xterm-256color` among
them — and it is already present in the `base` and `jumpbox` images
alongside the `htop` and `vim` binaries themselves. And a resize needs no
signal from the daemon: setting the size on the PTY master makes the kernel
raise `SIGWINCH` on the foreground process group as a direct consequence.

## Decision

**The terminal's type and initial size are query parameters on the upgrade
(`term`, `cols`, `rows`), not request headers.** This is the one genuinely
constrained choice here, and it goes against the local precedent:
`X-Cix-Exec-Cmd` on this same endpoint is a header. A browser's own
`WebSocket` constructor cannot set request headers at all. A header would
therefore have worked for `cixctl` and been permanently unreachable from the
dashboard — the same feature with two different capabilities depending on
which client you are. `?console=` (ADR-0240/#248) had already established
query parsing on this endpoint, so this adds a mechanism rather than
inventing one. Its three hand-rolled `strstr`-and-copy scans were folded
into a single `query_get_param()` at the same time, since three copies of
one loop is the parallel implementation the maxims rule out.

**An omitted parameter takes a default; a malformed one is a 400.** The
defaults (`xterm-256color`, 80×24) live in `exec.h` next to the struct that
carries them, so the daemon's parsing and the exec itself cannot disagree
about what "unspecified" means. The asymmetry is deliberate: a caller with
no terminal of its own to describe — a piped `cixctl` — is making a
perfectly ordinary request and gets the default. A caller that tried to say
something specific and got it wrong should hear so, rather than silently
receive a terminal of a different size than it believes it asked for.

**A live resize is a WebSocket TEXT frame carrying JSON; keystrokes are
BINARY.** RFC 6455 already separates a UTF-8 text message from an opaque
binary one, so the two kinds of traffic this session carries need no framing
of their own layered on top — the opcode is the discriminator. The
alternative, an in-band escape sequence in the byte stream, would have meant
either reserving a byte that can no longer be typed or building an escaping
scheme, both worse than using a distinction the protocol already has.

This is a clean cut-over rather than a change of meaning under a live
client: both clients already sent keystrokes as BINARY (`cixctl`'s own
`send_masked_frame(..., 0x2, ...)`, and the dashboard's `Uint8Array`, which
the browser frames as binary), so nothing was relying on TEXT carrying
input.

**A bad control message is ignored; the session continues.** This inverts
the rule applied to the query parameters above, and the difference is the
point: at upgrade time no session exists yet, so refusing costs nothing and
tells the caller something true. Mid-session, a control message is advisory
about presentation, and killing a working shell because a client sent a
field wrong would be a far worse outcome than the terminal keeping the size
it already has.

**`$TERM` is set on the PTY path only, never on the piped one.**
`exec_into_container_piped()` (issue #62) exists precisely to give a command
a clean byte stream with no terminal, because a PTY's line discipline
corrupted a piped one-liner and fed a wrong diagnosis. Advertising a
terminal type there would invite a program to emit cursor escapes into
exactly that stream.

**Sizing happens before the fork.** ncurses reads the window size once
during `setupterm()` at program start, so a size arriving even slightly
later is a size the program has already missed. There is no race to lose
if it is set before the child exists.

A failed `TIOCSWINSZ` is reported and not fatal. It cannot realistically
happen on a PTY master created three lines earlier, and if it somehow does,
the result is the unsized terminal that was this endpoint's behaviour for
its entire history — on which a plain shell still works perfectly well.
Refusing the whole console over it would turn a cosmetic failure into a
regression for every caller who is not running a full-screen program.

## Consequences

`htop`, `btop` and `vim` get the operator's real terminal over
`cixctl console`, which now sends its actual size and `$TERM` on attach and
a fresh size on every `SIGWINCH`. Resizing the local window resizes the
remote program, where before it drew at a fixed 80×24 that no resize reached.

**The web dashboard's terminal still cannot render them, and this change
does not alter that.** It is a line-buffer that interprets `\r`/`\n`/
backspace/tab and SGR colour and structurally discards every
cursor-addressing escape — a boundary ADR-0043 stated deliberately. What
does improve there is that its PTY is genuinely 80×24 rather than nominally
0×0, so a program's own idea of the screen and the daemon's now agree instead
of coinciding by accident through a terminfo fallback. Making that surface render full-screen
output needs a two-dimensional screen model (cursor addressing, scroll
regions, an alternate screen buffer) or a reconsideration of ADR-0043's
no-framework stance, and is a separate decision of a much larger size that
has deliberately not been taken here.

The daemon now trusts a client-supplied string into another process's
environment, which is why `term` is restricted to the character set real
terminfo names actually use rather than passed through free-form.

A container must carry the terminfo entry it is told about. Anything
linking `libncursesw` already depends on `ncurses`, which installs the whole
database, so an image with `htop` or `vim` in it satisfies this by
construction — but an image carrying a hand-built ncurses consumer and no
`ncurses` package would not, and would fail the same way this ADR exists to
fix.
