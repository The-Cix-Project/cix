# 0250 — A build that cannot find a tool is a failed build, whatever it exits with

## Status

Accepted

## Context

`gettext` took fifteen recipe revisions to build, and the first eight of
them were spent on the wrong cause.

The visible symptom was that libtool never absorbed the bundled
convenience archives: `libcroco_rpl.a` held 412 renamed definitions and
`libtextstyle.a` held none, so every tool linking `libtextstyle` failed on
symbols that had been compiled and thrown away. Revision 1.0-8 attributed
that to TCC. Revision 1.0-11 moved the package to gcc on that basis.
Revision 1.0-14 attributed it to `--disable-shared`. Both attributions
were published, and both were false.

The actual cause was in the build log the whole time:

```
libtool: link: (cd .libs/libtextstyle.lax/libcroco_rpl.a && ar x ".../libcroco_rpl.a")
../libtool: line 6182: find: command not found
```

libtool absorbs a `noinst_LTLIBRARIES` convenience archive by extracting
it under `.libs/<lib>.lax/` and then enumerating the extracted objects
with `find`. With `find` absent the object list comes back empty, the
final `ar cr` runs without those objects, and **libtool reports success**.

`pkg_build_depends` omitted `findutils`, which 334 other recipes in the
catalogue declare. [ADR-0199](0199-recipes-declare-their-build-tools.md)
composes the build container from exactly that list, so there is nothing
to inherit a missing tool from — which is the property that makes builds
reproducible, and also the property that makes an omission invisible.

Fixing that revealed two more of the same, and the split between them is
the point of this ADR:

- **`gzip`** was missing. `gettext-tools/autotools` died at `Error 127` on
  `archive.dir.tar.gz` — eighteen thousand lines in, but it *did* fail.
- **`cmp`** (from `diffutils`) was missing, and **nothing failed at all**.
  It appeared six times inside `configure`, where `checking for a working
  dd` and `checking whether perror matches strerror` each answered from a
  tool that was not there. Wrong config answers, exit 0, no diagnostic.
- **`xargs`** was missing too, on the same `findutils` omission.

So the class has a loud half and a silent half, and the silent half is
the dangerous one: it produces a package that installs, checksums,
publishes and boots, built without something it asked for.

Three things that already exist do not catch this. `elfcheck.c` gates
undefined symbols at install time, but it inspects ELF objects and a
static archive missing members is not an ELF-level defect. The recipe's
own gates catch only what the recipe author already thought to check. And
the ~4KB captured build-output tail is precisely the wrong window — the
missing-command line is usually thousands of lines before the end, which
is exactly why nobody saw it for eight revisions.

## Decision

**`cixd` reads its own build output for the shell's own missing-command
report, and refuses the build — including a build that exited 0.**

`pkg_build_output_append()` is the single chokepoint every byte of build
output passes through. It assembles complete lines across `read()`
boundaries and matches the one string measured in practice, `":
command not found"`, taking the token immediately before it as the tool
name. On any hit the build is failed with `PKG_FAILURE_BUILD` and a
message that **names the tool** — because a failure that says only "build
failed" puts the reader back where gettext's eight revisions started.

Scanned over every byte, not over the captured tail, for the reason
above. Over-long lines are kept by their tail rather than their head,
because the tool name sits immediately before the phrase; the buffer is
halved rather than shifted per character, so a 200 KB link command line
stays linear.

Only that exact wording is matched. `dash` says `not found` instead, and
no image here ships dash — matching a second format nothing has ever
produced would be guessing.

### Alternatives considered

**A mandatory baseline in every build environment** (always install
`findutils`, `gzip`, `diffutils`, `tar` on top of `pkg_build_depends`).
Rejected: it removes this particular trap by weakening the property
ADR-0199 exists for. "Composed from exactly what is declared" is what
stops a shared sandbox accumulating whatever happened to pass through it,
which is how 8 GB of a developer workstation's `/usr` ended up inside
`cix-builder` (#168). It also fixes only the four tools someone
enumerated today.

**Warn rather than fail.** Rejected: a warning nobody reads is a
stop-gap, and the whole defect being fixed is that the information was
already present and unread.

**Leave it to each recipe.** Rejected: this is a property of the build
*environment*, and a per-recipe check can only cover tools someone
already thought of. gettext's own preflight gate (added in 1.0-16) is
kept as a useful early, named failure — but it lists `find`, `gzip` and
`cmp` because those are the three that were found, which is exactly the
limitation.

## Consequences

Measured before this was made fatal: of the 41 build logs on
192.168.15.95 at the time, **exactly one package's logs contained the
phrase** — gettext, the bug this came from. Nothing that builds correctly
today is refused by it.

A recipe that genuinely tolerates a missing optional tool now has to
declare that tool. That is the intended cost: under
[ADR-0222](0222-a-declared-capability-loss-beats-a-foreign-toolchain.md)
a dropped capability is declared, not discovered, and a tool the build
silently did without is a capability dropped by accident.

The failure is REST-visible like any other build failure — `failure_kind`
`build`, with the tool named in `error` — so an operator sees what to add
to `pkg_build_depends` rather than a generic build failure.

This does not detect a tool that is missing and never invoked, nor one
whose absence a build script handles itself without the shell reporting
it. It catches the case that actually occurred, four times in one
package, twice silently.
