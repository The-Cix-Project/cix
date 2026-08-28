# 0211 — TCC cannot build EFI MS-ABI code or CPython's atomics: gnu-efi, python and their dependents join the Tier-3 exception list

## Status

Accepted. Extends the Tier-3 TCC exception list established by
[ADR-0170](0170-tcc-cgo-tier3-exception.md) (`gcc`, `kernel`, `openssl`,
`glauth`, `gitea` already on it).

## Context

Building an installer ISO on a Cix host needs `grub`, `xorriso`, `mtools`
and `sbsigntools` present so `isotools` can harvest them. None of the ISO
toolchain had ever been built on a Cix host — the artifact cache holds 88
artifacts across 47 packages and not one of them — so every package had to
be built for the first time under the no-fallback rule (#168).

Two of them stopped for reasons that are not recipe bugs.

**`gnu-efi` needs MS-ABI variadics.** EFI uses the Microsoft calling
convention, and gnu-efi's `efistdarg.h` declares its `va_list` accordingly.
Probed directly rather than inferred:

```c
typedef __builtin_ms_va_list va_list;   /* tcc: ';' expected (got "va_list") */
typedef __builtin_va_list va_list;      /* compiles clean */
```

The first is exactly the error the real build produces. TCC implements
`__builtin_va_list` and not `__builtin_ms_va_list`, and no flag supplies it.

**CPython needs atomics TCC does not have.** Python 3.13's `pyatomic.h`
dispatches on GCC/clang builtins, MSVC intrinsics, or C11 `stdatomic.h`, and
falls through to an explicit `#error` when none is available:

```
./Include/cpython/pyatomic.h:543: error:
  "no available pyatomic implementation for this platform/compiler"
```

TCC provides none of the three. Atomics are foundational to CPython 3.13's
free-threading work, so this is a porting project rather than a flag —
categorically the same shape as ADR-0170's cgo finding.

Python matters here only because `grub`'s own `configure` hard-requires a
Python interpreter, even from an official release tarball, to run its
build-time generators.

## Decision

`gnu-efi` and `python` join the Tier-3 TCC exception list and are built with
**gcc 16.2.0-11 from this project's own artifact cache**, invoked by absolute
path (`CC=/usr/bin/gcc`).

`binutils-dev` and `sbsigntools` follow them: `sbsigntools` depends on both
`gnu-efi` and `binutils-dev`, and `binutils-dev` fails under TCC with a parse
error in `bfd.c` whose exact cause is not yet characterised — it is listed
here as a dependent, and if it turns out to build under TCC once its own
declaration is right, it should be removed from this list rather than left in
for convenience.

**This does not weaken the Build Provenance Mandate.** The compiler used is
Cix's own: `gcc.recipe`'s real three-stage bootstrap output, built on a Cix
host and published to the cache, never an ambient host compiler. The mandate
constrains *where and by what* something is built, not *which* of Cix's own
compilers is used.

`tcc` is deliberately removed from these recipes' `pkg_build_depends` rather
than left alongside `gcc`. Two compilers in one environment is how a real GCC
was once silently picked up over TCC on `openssh.recipe`, producing an `sshd`
whose `configure` reported PAM support and whose ELF carried no `libpam` at
all. An environment holding exactly one compiler cannot have that ambiguity.

## Consequences

**The exception list is now seven packages and every entry has a
demonstrated failure behind it.** That is the property worth protecting: the
list grows by evidence, never by difficulty or impatience. Both additions
here were reproduced down to a one-line probe or an upstream `#error`.

**A signed installer ISO stays achievable.** The alternative considered was
making `mkinstalleriso`'s Secure Boot signing optional, which would have cut
`gnu-efi`, `binutils-dev` and `sbsigntools` from the critical path at the cost
of unsigned installer media. Extending an existing, well-precedented exception
is the smaller change and loses nothing.

**Grub's Python requirement is now a hard dependency of the ISO path.** It is
worth revisiting whether grub can be configured without it; if so, `python`
could leave this list. It is here because grub needs it, not on its own merits.

**`binutils-dev` is provisional.** It is on the list as a dependent, with its
own failure not yet root-caused. Leaving that unstated would let an unexamined
entry look as well-evidenced as the two that were probed.
