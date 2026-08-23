# 0199 — A recipe declares its build tools, and gets exactly those

## Status

Accepted

Supersedes the direction of [ADR-0198](0198-build-sandbox-is-a-real-image.md), which made the shared build sandbox observable while leaving it fungible. Measurement showed that was the wrong target.

## Context

Every ordinary package built against one shared sandbox that accreted from every install ever run on the box. Measured on a real host:

| | files |
|---|---|
| sandbox regular files | 79,438 |
| paths declared by all 78 installed packages | 15,430 |
| **at most attributable** | **19%** |

Of the rest, **53,535 files — 67% of the whole sandbox — were a Rust and Go toolchain** (`/usr/local/cargo`, `/usr/local/go`, `/usr/local/rustup`) that no recipe asks for and no package installs. Zero of 200 recipes write to `/usr/local`. The compiler in there answered to `/usr/bin/gcc`, resolved to a Debian-named `x86_64-linux-gnu-gcc-12`, and reported `gcc (GCC) 4.7.4`.

So "what does this build run against?" had no answer. Three consequences, all real and all observed:

- `gcc/4.7.4` built once and then stopped, failing at `C++ preprocessor "/lib/cpp" fails sanity check`, with no change to the recipe — unexplainable, because the environment was undescribed.
- A build could succeed *only* because something unrelated had been installed earlier. When that stopped being true, `sed: command not found` took down every install on the box.
- `pkg_depends` had drifted into meaning two different things, because build-visibility was a side effect of the sandbox merge rather than anything a recipe stated.

## Decision

**A recipe declares the tools it needs to build, and its build container is composed from exactly those packages and nothing else.**

`pkg_build_depends=` is that declaration, and it is deliberately a *second* field rather than a redefinition of `pkg_depends`, because the two answer different questions:

- **`pkg_build_depends`** — what must be present to *build* this. Composed into the build container; never installed into the target image.
- **`pkg_depends`** — what the built artifact needs at *runtime*. Installed into the target image; never, by itself, a build input.

Both halves of "no extras and no less" are then enforced by something real rather than by discipline:

- **no less** — the build fails, naming exactly what is missing
- **no extras** — nothing else is present to accidentally depend on

**Composed from each declared package's own cached build output**, not from an image. An image is a union of whatever was installed into it; a package's cache entry is the files that package itself produced, which is the only honest definition of what it contributes.

**The composed environment is an ordinary image named for the hash of the declared set** (`__buildenv-<hash>`), so the same set is composed once and reused, two different sets can never collide, and the set is sorted before hashing so order of declaration is not part of identity. That reuse is not an optimisation bolted on afterwards — it is what makes declaring tools cheap enough to do everywhere.

**A declared tool that cannot be provided fails the build.** There is deliberately no fallback to the shared sandbox. A fallback would let the build succeed against something fuller than it declared — the exact failure this replaces — and would teach everyone that the declaration is decorative.

## Consequences

Recipes that declare nothing keep the old shared sandbox. That is the migration path, and it is per-recipe and visible: each converted recipe is permanently pinned down, and the fungible branch shrinks as they convert. It is not a state to settle in.

**Sufficiency is enforced; minimality is not.** If a recipe declares a tool it does not need, the build still succeeds and nothing complains. Proving minimality would mean dropping each declared tool in turn and rebuilding — a real technique, and an expensive one. Review is the current answer, and this ADR says so rather than implying the field is self-policing in both directions.

**`pkg_depends` across the existing recipe set is inconsistent, and necessarily so**, because until now the word meant two things. `gcc` declares `binutils m4 zlib libc-dev` — a mix of build-only (`m4`) and link-time. `grub` declares `gettext patch`, where `patch` is build-only. `m4` declares `binutils`, which is purely build-time. Meanwhile `tcc`, `make`, `sed`, `bash`, `zlib` and `perl` declare nothing at all, despite every one of them needing a compiler, `make` and libc headers — those worked only because the sandbox happened to provide them. Splitting the field is what makes "accurate" a definable property; auditing the set is the work that follows, and it is now well-defined rather than a matter of taste.

**The bootstrap remains.** Something has to compile the first compiler, and it cannot itself be composed from cached packages. That is unchanged, and it is issues #36–#38's subject.

**There is a real chicken-and-egg during rollout**: composing clean environments needs tools that the *current* contaminated sandbox can no longer rebuild — `bash` and `grep` both fail in it today. Converting recipes is therefore gated on having buildable tools, not merely on writing declarations.
