# 0276 — A link is a declaration

## Status

Accepted

Issue [#389](https://git.home.arpa/itdlabs/cix/issues/389). Applies
[ADR-0199](0199-a-build-environment-is-declared-not-inherited.md)'s rule
— a build environment is declared, not inherited — to the *runtime*
half, which had no equivalent gate.

## Context

`cmake@4.4.3-2` declared `pkg_depends=""` and built against openssl.
openssl was present while it compiled, so cmake linked
`DT_NEEDED libssl.so.3` and was installed **recording no runtime
dependency**. It ran fine in `cix-builder`, where openssl happened to be
installed for unrelated reasons.

An hour later, an unrelated package failed. `fastfetch` declares
`pkg_build_depends="cmake gcc make …"` — correctly, since fastfetch does
not use openssl. Its entire build log was 148 bytes:

```
cmake: error while loading shared libraries: libssl.so.3: cannot open shared object file: No such file or directory
cix-init: oneshot failed: build
```

The reason is `buildenv_add_tool()`'s own comment:

> What this package needs in order to run, **as recorded when it was
> installed** — not as the current recipe now says, since the
> environment is built from the installed copy.

That recursion is correct and it was fed a lie. A build environment is
composed from exactly the resolved tool set, so cmake arrived in it
without openssl and died before fastfetch's source was touched.

Three properties make this worth a gate rather than a lesson.

**Blame lands on the wrong package.** Nothing in the message, the state,
or the failing recipe names cmake. The failing recipe is correct.

**It reproduces or not depending on where you look.** cmake works in
`cix-builder` and fails in a composed environment, so "it worked
yesterday" and "it works over there" are both true and neither is
evidence.

**Publishing a corrected recipe does not fix it.** The recorded field
only changes on reinstall — correcting `cmake` cost a full C++ bootstrap
rebuild to change one string.

## Decision

**A package must declare every shared library it links. The check is
about the declaration, not about the library being present.**

At install, before a single byte is staged, every `DT_NEEDED` soname of
every ELF object in the staged tree must be provided by one of:

- the staged tree itself — a package whose binary links its own library
  declares nothing and is right not to; that dependency is internal;
- the transitive closure of its declared `pkg_depends`, resolved from
  the **installed** copies, mirroring `buildenv_add_tool()`;
- the C library, which is implicit for the same reason
  `buildenv_resolve_tools()` adds it implicitly — no recipe declares it,
  every binary needs it, and requiring the declaration would be a rule
  every package on this platform violates.

Anything else refuses the install, naming the file, the soname, and the
fix.

**Presence in the target image is deliberately not consulted.** This is
the whole decision, and the cheaper check is the tempting one: ask
whether `libssl.so.3` exists in the image being installed into. That
check would have **passed `cmake@4.4.3-2`**, because openssl was sitting
right there — and the false record would still have been written, to be
believed by the next environment composed from it. Presence is a
property of one image at one moment; a declaration is a property of the
package, and it is the declaration that travels.

**A question this cannot answer is not a refusal.** An unresolvable
dependency, an unreadable tree, or an allocation failure logs why the
gate did not run and lets the install proceed. A gate that fails an
install must fire on evidence; absence of evidence is not evidence.
`elfcheck_undeclared_links()` returns three values for exactly this
reason, and its header says a caller must never read `-1` as "clean".

## Alternatives considered

**Warn instead of refusing.** Rejected. The warning would be written to
the log store at the moment nobody is reading it, and read an hour later
by someone debugging a different package — which is precisely the
situation this exists to end. `elfcheck`'s existing `__builtin_*` check
(#176) already refuses an install outright, and this is the same shape
of fact: mechanically determined, not a heuristic.

**Check at build-environment composition instead.** Rejected as the
primary home, though it is where the pain is felt. Composition is a
*consequence* — catching it there still means an unrelated build fails,
just faster and with a better message. The lie is written at install,
and that is where it should be refused. One rule, one place.

**Derive `pkg_depends` from the ELF instead of checking it.** Rejected,
and it is the seductive option. A soname is not a package name, the map
from one to the other is what the package database is for, and a recipe
that never states its dependencies has no reviewable record of them —
the same reasoning ADR-0199 gives for declaring build tools rather than
inheriting whatever the sandbox happened to accumulate. Deriving would
also make every recipe's dependency set change silently whenever a
build's incidental linkage changed.

## Consequences

An operator installing an under-declaring package gets a refusal naming
the package, the file, the soname, and what to add, instead of an
unrelated failure an hour later in a package whose recipe is correct.

**Expect this to fire on existing recipes.** Nothing re-checks packages
already installed, so no running host changes — but any package whose
recipe under-declares will be refused on its next build or install, and
that includes installs from cached artifacts, which are staged into the
same tree and gated identically. That is a latent bug becoming visible
at the moment it can be fixed cheaply, which is the point; it will not
feel like it the first time it happens.

`elfcheck_needed_libs()` gains its first caller. It was written for
#224, has been correct and unused since, and needed no change.

A `dlopen()`-only dependency is invisible to this check, because it is
invisible in the ELF — there is no `DT_NEEDED` to find. That is a real
limit and not a defect of the mechanism: this gate makes a linked
dependency impossible to leave undeclared, and says nothing about
dependencies discovered at runtime.
