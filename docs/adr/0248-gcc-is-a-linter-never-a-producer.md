# 0248 — gcc is a linter, never a producer

## Status

Accepted

## Context

This project compiles with TCC and only TCC for its own code. [ADR-0224](0224-the-toolchain-tenet.md)
states that without qualification and [ADR-0226](0226-gcc-is-an-ordinary-choice-for-third-party-packages.md)
deliberately does not touch it: third-party packages may use gcc, our own
code may not. Sovereignty lives there.

Every build runs `-Wall -Werror` and has done for the project's whole life,
so "warning-free" was believed to be a settled property rather than an open
question.

It was not. A single `gcc -fsyntax-only -Wall -Wextra` pass over the same
sources — run once, as an audit, against a tree that TCC was compiling green
under `-Wall -Werror` — reported ten real defects:

- two `int` versus `size_t` comparisons in `containerdef.c`, in a file whose
  own other array loops already used `size_t`, so the codebase disagreed with
  itself and nothing said so
- four variables declared and never used, one of them left behind by a
  removal made in the same session that ran the audit
- a comment block indented as though the `if` above it guarded the code that
  followed, which is precisely how a reader mis-reads control flow
- a struct initialised by position with a field silently left off the end

None of these changed behaviour on the day they were found. That is the
point: they are the class of defect that becomes a bug on the *next* edit,
and no runtime test can reach them, because there is nothing yet to observe.

The measurement that settles it: with an unused variable injected into
`stallwatch.c`, TCC compiles the file clean under `-Wall -Werror` and reports
nothing at all.

## Decision

**gcc runs over this project's own C as a linter, in the selftest, and a
warning fails the build.**

It runs `-fsyntax-only`. It emits no object file, produces nothing that could
be linked, and therefore nothing it touches can reach a binary, an image, a
package or the artifact cache. TCC still compiles every byte this project
ships.

So the Toolchain Tenet is untouched, and the distinction it turns on is
*producer versus reader*. ADR-0224 is about what may compile our code; this
is about what may read it. A second reader that disagrees is worth having
precisely because it disagrees — a linter that only ever confirmed the first
compiler would be pure cost.

Three details are deliberate:

- **gcc is invoked by absolute path** (`/usr/bin/gcc`). A bare `gcc` resolved
  through `$PATH` computes its own installation prefix as a relative path and
  then fails to find `cc1` with a misleading "No such file or directory".
  That is a measured property of this environment, recorded in `CLAUDE.md`.
- **`-Wno-comment` is the one suppression**, and it is style rather than
  substance: this codebase's comments quote code containing `/*`, which that
  warning objects to and nothing else does. Forty-six of the fifty-six
  warnings in the first audit were this and this alone; keeping them would
  have buried the ten that mattered.
- **`test/` is not linted.** A test fixture is allowed to be blunt, and
  holding 117 files to this bar would make the gate about the tests rather
  than about the product.

A missing gcc **fails** the gate rather than skipping it. gcc is a declared
`pkg_build_depends` of the cix recipe, so its absence means the build
environment is not the one the recipe asked for, and a gate that quietly
passes in that case is not a gate — the same reasoning `tcc.recipe` arrived
at after three revisions of gates that could not report why they failed.

## Consequences

The ten defects are fixed and the eleventh cannot land silently.

The cost is one more build dependency on the lint path, which was already
present, and the risk that a future gcc release adds a warning that fails a
build for a new reason. That is a real cost and it is the right one to pay:
a new warning from a newer compiler is information, and the alternative is
learning the same thing from a bug.

This does not open the door to gcc compiling our code. If that is ever
proposed it needs its own ADR arguing against ADR-0224 on its merits, and
this one is not that argument.
