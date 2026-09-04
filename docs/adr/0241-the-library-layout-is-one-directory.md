# 0241 — The library layout is a decision, and it collapses to one directory

## Status

Accepted

## Context

This platform carries five library directories. Measured from the live daemon's own package file lists on 192.168.15.95 (issue #184):

| directory | packages | files |
|---|---|---|
| `usr/lib/` | 54 | 683 |
| `lib/x86_64-linux-gnu/` | 52 entries, **19 distinct package names** | 483 |
| `usr/lib/x86_64-linux-gnu/` | 9 | 135 |
| `usr/lib64/` | 3 | 150 |
| `lib64/` | 11 | 11 |

Within glibc alone, **455 basenames exist in two or three of those at once** — `crt1.o`, `libc_nonshared.a`, `libresolv.so.2` — plus two full copies of the dynamic loader.

Almost none of that is a decision anyone made here. `lib/x86_64-linux-gnu` and `usr/lib/x86_64-linux-gnu` are Debian's multiarch convention, which exists so several architectures can share one filesystem; a Cix image has one architecture, and since #179 architecture belongs to package *identity* rather than to directory names. `usr/lib64` is where GCC's own build puts things. `usr/lib` is glibc's default and what `--libdir` wants to be. The layout is inherited, and the platform then spent real effort maintaining the inheritance.

Two incidents came directly out of the duplication: a libc replaced while its loader was not (#181), and files deleted from a package nobody touched because two packages shared a directory (#175). Every duplicated path is a chance for two copies to disagree.

**The reason it had never been reduced is worth naming, because it is the actual obstacle.** The layout was asserted in about sixty separate places in the platform's own C — forty of them a single hand-maintained list in `mkbootroot.c` where every entry spelled out `/lib/x86_64-linux-gnu/` for itself. Moving one package therefore meant finding and editing all sixty. The cost of the first step was the cost of the whole thing, so no first step was ever worth taking.

## Decision

**One library directory, `/usr/lib`, reached in stages — and in the platform's own code the layout is named once rather than asserted everywhere.**

`include/libdirs.h` names it three ways, because the platform asks three different questions of the layout and they have three different answers:

- **`CIX_LIB_DIR_RUNTIME`** — where a library is *put*. This is glibc's `slibdir`, because glibc's compiled-in runtime search path is exactly `slibdir + libdir` and there is no third slot: a library placed anywhere else is found only by the accident of one of those two happening to cover it.
- **`CIX_LIB_DIRS_SEARCH`** — where one is *looked for*. A superset, deliberately. During the migration a package that has already moved is staged into an image whose other libraries have not, and this list is what makes that intermediate state work rather than fail.
- **`CIX_LIB_DIRS_PLATFORM`** — the platform's own glibc set, staged and then verified. Narrower on purpose, and the two loops that use it must cover *exactly* the same directories: widening one without the other either misses files or refuses a build over files nobody promised anything about.

`/lib64/ld-linux-x86-64.so.2` is exempt and stays permanently. It is `PT_INTERP` in every binary ever built here and is fixed by the psABI — unlike the rest of the layout it is not ours to choose. It becomes a symlink into `/usr/lib` and is the one compatibility path the design deliberately keeps. `PKG_IMAGE_LOADER_REL` already names it in exactly one place.

### Why stages, and why this order

The measurement that shaped this: **`/usr/lib` is already on the runtime search path.** It has been glibc's `libdir` since 2.44-7 (#196), with a gate in that recipe asserting the loader really searches it, and 54 packages already install there and work.

So this is not the single deliberate cut-over it first looked like. A package can move from `/lib/<triplet>` to `/usr/lib` **one at a time**, each independently verifiable, because the running glibc finds both. glibc's own flip is *last*, by which point nothing remains in the directory it stops searching.

1. **The platform stops asserting the layout** — this header, and the sixty sites reduced to it. No package changes.
2. **The 18 non-glibc packages install to `/usr/lib`,** on their natural revision bumps rather than as a mass revision: each publish queues a rolling rebuild of every image tracking it.
3. **tcc and gcc drop the triplet from their search paths**; gcc's `--libdir` moves off `/usr/lib64`.
4. **glibc flips `libc_cv_slibdir` to `/usr/lib`.** The pivotal step, and the only one that can leave the box unbootable, with the inactive A/B slot as the net.

## Consequences

Stage 1 changes no behaviour at all: every value is what the code already did, and both search lists are strict supersets of the lists they replace. That was verified rather than asserted — real assemblies either side of the change staged **31** of the platform's own shared libraries, the same 31 — and the resulting root was booted on 192.168.15.95 (`v2.53.0`, slot `b`, all five containers up).

Stage 4 becomes a one-line edit on the platform side, which is the entire point of doing stage 1 first.

Test fixtures that build their own rootfs are deliberately left asserting their own paths. They create a library and reference it at the same path, so they are not describing the platform's layout — they are choosing their own, and a fixture that follows the real layout would be testing less, not more.

Two defects surfaced from writing the layout down, both found by review rather than by a test: a staging loop that walked to a `NULL` terminator its array no longer had, and the host-tools library search path composed separately for `LD_LIBRARY_PATH` and for the loader's own `--library-path` — the same string twice, free to drift while each looked correct in isolation. The second matters more than it reads: a silently shortened search path drops directories off the *end*, and the dynamic linker then falls back to the build host's own libraries with no error at all, which is ADR-0154's confirmed failure mode.

Supersedes nothing. Sequenced with #181 (whose libc lands in these directories) and #179 (which moves architecture out of them) — all three are the same question of what this platform's layout is, as designed rather than as inherited.
