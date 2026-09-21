# Writing a package recipe

> **Recipes live in their own repository**, [cix-recipes](https://git.home.arpa/itdlabs/cix-recipes),
> and a recipe is one flat file: `recipes/package/<name>@<version>.<ext>`
> ([ADR-0308](../adr/0308-recipes-are-their-own-repository-flat.md)).
> Clone it beside `cix`; tests here resolve it through `CIX_RECIPES_DIR`,
> defaulting to `../cix-recipes/recipes`.

This is the canonical, complete reference for Cix's recipe format — the source of truth this content lives in exactly once (`daemon/include/pkg.h`'s own header comment points here rather than repeating it). For what the REST endpoints that consume a recipe actually do, see [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs); for the underlying design rationale, see ADR-0036 (multi-source recipes) and ADR-0056 (the hostbuild variant).

## What a recipe is

A recipe is usually a POSIX shell script, one per package, matching the same well-proven format Gentoo ebuilds, Arch PKGBUILDs, and CRUX Pkgfiles all use — and since ADR-0305 it may instead be a declarative CPDL document (see [Two recipe languages](#two-recipe-languages-adr-0305) below). The rest of this section describes the shell form. `cixd` treats it two completely different ways depending on which part is being read:

- **Metadata** (`pkg_name=`, `pkg_version=`, `pkg_source=`, `pkg_sha256=`, `pkg_depends=`, `pkg_changelog=`) is read by a strict, non-executing line scanner (`parse_recipe()` in `daemon/src/pkg.c`) — the daemon never runs a shell interpreter over your recipe to extract these values.
- **Build logic** (`pkg_build()`/`pkg_install()`, real shell functions) is only ever invoked inside an isolated, network-less build container, `". /build/recipe.sh"` sourced by a tiny driver script. This is the *only* place a recipe's own shell code ever actually runs — never on the host, never outside a container.

This split is deliberate and load-bearing: a malicious or buggy recipe's shell code can corrupt its own build container's filesystem, but it can never touch the host, and it has no network access to exfiltrate anything even if it tried (this project's networking plane has no outbound NAT — see ADR-0007 and `daemon/include/pkg.h`'s own header comment).

## Two recipe languages (ADR-0305)

Everything below describes a **shell recipe**, `build.sh`, which is what every recipe in this repo is today. A second language exists: a **PBS recipe**, `build.cbs`, written in CPDL 0.1 and built by [cix-build-system](https://git.home.arpa/itdlabs/cix-build-system) rather than by a shell.

**The filename is the format.** `<name>/<version>/build.sh` is a shell recipe, `<name>/<version>/build.cbs` a PBS one, and a version holds one or the other — never both, which is refused at publish. Nothing sniffs the content and no recipe declares its own language, because a filename cannot disagree with what will actually run. `cixctl pkg recipe add` takes the format from the file you point it at, so publishing one needs no extra flag:

```sh
cixctl pkg recipe add --name=zstd --file=recipes/package/zstd@1.5.7-5.cbs
```

A PBS recipe is declarative: five ordered phases (`prepare`, `configure`, `build`, `check`, `install`) instead of two shell functions, validated before anything runs. What it declares maps onto the same fields the rest of this guide describes:

| shell recipe | PBS recipe |
|---|---|
| `pkg_version=` | `version` + `release`, joined as `<version>-<release>` |
| `pkg_source=` / `pkg_sha256=` | `sources { main "…" { url … sha256 … } }` |
| `pkg_depends=` | `requires { runtime { package "…" } }` |
| `pkg_build_depends=` | `requires { build { compiler "…" tool "…" } }` |
| `pkg_build()` / `pkg_install()` | the phases; an install phase stages into `${dest}` |

Four things to know before writing one, each of which will otherwise cost you a build:

- **`format "cixpkg"` is required**, even though cixd still tars the staged tree itself. CPDL accepts `"tar.gz"` as a value, but CBS's build pipeline refuses to run any recipe declaring anything else.
- **The engine is not yours to declare.** Do not put `cbs` in `requires { build { … } }`; cixd adds it, the same way it adds the C library. Declare `cbs@<version>` only if you need a specific engine revision, which wins the slot.
- **The artifact checksum and changelog live in `metadata { }`.** Both are ordinary metadata keys:

  ```
  metadata {
      "artifact_sha256" "…"
      "changelog" "…"
  }
  ```

  You do not write the checksum yourself. Publish without one, let it build, and cixd writes the approval into the stored recipe — then fetch the recipe back and commit **that**, or the copy in this tree and the published one differ. A changelog is capped at 511 bytes; over it, the publish is refused with a message about parsing that is not about parsing (#493).
- **Build capabilities work.** `capability "CAP_SYS_ADMIN"` is read by name and granted. An engine too old to report names is refused rather than read as zero, so a `cbs` predating that will fail the publish rather than silently build without the capability.

### Shell idiom → CPDL equivalent

CPDL has no shell, and the reflex when converting is to assume a missing feature. Usually it is there under another name. Every row below was used in a real conversion in this repo:

| shell | CPDL | note |
|---|---|---|
| `make -j"$(nproc)"` | `run "make" { jobs $jobs }` | 92 of the 217 command substitutions in this corpus are this one |
| `VER=$(pkg-config --modversion x)` … `$VER` | `run "pkg-config" { "--modversion" "x" stdout "ver" }` … `${stdout.ver}` | a bound name substitutes anywhere a value does |
| `cd dir && cmd` | `cd "dir" { run "cmd" { } }` | |
| `cp a b c DEST/` | three `copy … to …` lines, full destination paths | `copy` is one source to one destination |
| `cp *.h DEST/` | `copy glob "…/*.h" to "DEST"` | `move` and `remove` take `glob` too |
| `rm -rf dir` | `remove tree "dir"` | but see **what not to remove** below |
| `ln -s TARGET LINK` | `symlink "TARGET" to "LINK"` | operand order is target first, the reverse of how `ln -s` reads in prose |
| `echo 'text' > f` | `write "f" "text"` | |
| `sed -i 's/X/Y/' f` | `replace "f" { from "X" to "Y" exactly 1 }` | literal only; `exactly N` fails loudly when upstream moves it, which `sed` does not |
| `find . -name M -exec sed -i 's/X/Y/g' {} +` | `replace glob "…/**/M" { from "X" to "Y" exactly N }` | `**` crosses directories, `*` stays within one segment |
| `grep -q X f \|\| exit 1` before a `sed` | nothing — delete the guard | `exactly N` already is that guard, and names the file and the count when it fails |
| `grep -q X f \|\| exit 1` | `require file "f" { exists contains "X" }` | `exists` is REQUIRED and must come first -- the parser consumes it unconditionally before the rest (`src/parser.c:533`). This row used to omit it and cost a publish round-trip |
| `test -L l \|\| exit 1` | `require symlink "l" { exists target "dest" }` | **`target` belongs to `require symlink`, not `require file`.** A file assertion takes only contains/same_as/nonempty and rejects `target` with `error[CPDL-E3004]: file assertion must use contains, same_as, or nonempty`. This row is here because that cost a publish round-trip too: `require file` matches regular files only and reports a symlink as "does not exist" (cbs#175), so a soname link needs this form |
| `test -f x \|\| exit 1` | `require file "x" { exists }` | |
| `cmd; [ $? -eq 2 ]` | `run "cmd" { expect exit 2 }` | |
| `$(pwd)` | the absolute path you already know | inside `cd "${src}/n/top"`, that is `${src}/n/top` |
| the source tarball itself | `${source.NAME}` | the verified archive, not the unpacked tree |

**`exactly N` on a `replace glob` is a TOTAL across every matched file, not a count per file**, and the replacement itself then changes every occurrence in each. So converting a `find … -exec sed -i 's/X/Y/g'` needs a number you cannot know without counting — the practical route is to declare `exactly 1`, let the build fail, and read the real total out of `source edit expected 1 matches but found 23`. One deliberate failing build is the price, and it buys an assertion the shell form never had.

**`require file` matches regular files only.** It `lstat()`s and demands `S_ISREG`, so it fails on a **symlink** and reports "does not exist" (cix-build-system#175). Assert on `libz.so.1.3.2`, never on the `libz.so.1` soname link — the soname is the name you know, and it is the one that fails.

**What not to remove.** Do not translate a `rm -rf "$PKG_DESTDIR/usr/share/{man,doc,locale}"` — [ADR-0306](../adr/0306-a-package-keeps-its-documentation-and-its-licence.md) withdrew that practice, and carrying it across is how a converted recipe keeps a deleted licence deleted. Do not translate `rm *.la` or a `.a` beside its `.so` either: ADR-0251's finalize phase already does both, after your install phase, and a recipe restating platform policy is a second place for that policy to live.

### What CPDL cannot do yet

Three shapes have no form, and a recipe needing one stays on `build.sh` until it does. Each is filed with the corpus count behind it:

| shape | packages | issue |
|---|---|---|
| apply several steps to each item of a list | 15 | [#176](https://git.home.arpa/itdlabs/cix-build-system/issues/176) |
| strip `-Wl,--version-script=<path>` (pattern edit) | 6 | [#177](https://git.home.arpa/itdlabs/cix-build-system/issues/177) |
| find a shared library across candidate lib directories | 4 | [#178](https://git.home.arpa/itdlabs/cix-build-system/issues/178) |
| assert on a process that prints more than one line, and so any NEGATIVE assertion built from one | 1 | [#185](https://git.home.arpa/itdlabs/cix-build-system/issues/185) |

A `for` loop whose body is a **single** operation is not blocked — write it as N lines.

**#185 is the one that costs an assertion rather than a conversion, so it is worth knowing the shape.** `expect { stdout contains … }` and `stdout "name"` both require the process to print exactly one line, which is right for binding `pkg-config --modversion` and leaves `ldd`, `--version` banners and anything else multi-line with no form at all. That in turn means a negative check — *this output must NOT contain X* — cannot be built either, since the usual route is to bind the output and grep it for an exit status. `btop` stays on `build.sh` for exactly this: its build ends by proving `ldd` does not name a shared `libstdc++`, and the failure that check catches is silent at build time and only appears on someone else's machine. Convert a recipe like that and you keep the conversion and lose the guard, which is the wrong trade. `iw` stays on `build.sh` for the same reason in the opposite direction: it asserts that its binary *does* carry `NEEDED libnl-3.so.200` and `libnl-genl-3.so.200`, via `readelf -d | grep -q`, which catches a build that silently went static or resolved against something else. `require file { exists contains "libnl-3.so.200" }` looks like a substitute and is not one to reach for on faith — whether that check is binary-safe on an ELF has not been measured here, and a `contains` that quietly stops at the first NUL would pass vacuously, which is the one failure direction a guard must not have.

Sources are handed to CBS, not fetched by it: cixd fetches and checksum-verifies as it does for any recipe, then places each source in a cache directory named by its own digest, which CBS re-verifies. So a PBS build container still has no network and no credentials, exactly like a shell one.

## Required metadata fields

```sh
pkg_name="hello"
pkg_version="2.12.1"
pkg_source="https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db8"
pkg_depends=""
```

- **`pkg_name`** — must exactly match the `{name}` this recipe is uploaded as (`POST /pkg/recipes {"name": ...}`) — a mismatch is a `400`, so a bad upload can never silently attach to the wrong name. Same charset as every other simple name in this platform: `[A-Za-z0-9_-]+`.
- **`pkg_version`** — a plain string, compared byte-for-byte against what's installed to detect drift (`GET /pkg/{name}`'s `available_version` field, and what `POST /pkg/update-all` scans for). Bump it whenever the recipe changes in a way that should trigger an upgrade — there's no separate "recipe revision" concept, `pkg_version` *is* the revision.
- **`pkg_source`** — where to fetch from. Almost always an `https://` URL; a local self-hosted git remote's own archive-download endpoint works identically (see [`building-cix.md`](building-cix.md) for a real example). Fetched host-side by the daemon's own `curl` subprocess, before the build container ever starts — the container itself has no network access at all, so anything a build needs must already be named here.
- **`pkg_sha256`** — the fetched source's checksum, verified before extraction. A mismatch fails the job outright (`PKG_STATE_FAILED`, no partial state).
- **`pkg_build_depends`** — a space-separated list of packages that must be present to *build* this one (ADR-0199). The build container is composed from exactly these; see [Dependencies](#dependencies-two-questions-two-fields) below.
- **`pkg_build_image`** — **retired** ([ADR-0304](../adr/0304-a-hostbuild-composes-its-build-environment-like-every-other-build.md), [#482](https://git.home.arpa/itdlabs/cix/issues/482)). It named the image a recipe was meant to be *hostbuilt* in, and the daemon no longer reads it: a hostbuild composes its build container from `pkg_build_depends` like every other build, so there is no image to name. New recipes must not declare it; the line is inert in already-published revisions and disappears as each recipe next bumps. The field existed because which image could build what was convention rather than contract ([#182](https://git.home.arpa/itdlabs/cix/issues/182), [ADR-0230](../adr/0230-the-five-lifecycle-domains.md)) — a declaration of the build environment, which is what `pkg_build_depends` already is, more precisely.
- **`pkg_build_caps`** — a space-separated list of Linux capabilities the *build container* needs. Almost every recipe leaves this empty and should. It exists for one shape of recipe: one whose build has to **create containers** — this platform's own test suite is the only such recipe today (issue #224). A build container is otherwise measured to have no `CLONE_NEWNET`, no `CLONE_NEWNS`, no `mount()` and no cgroup tree, so a test that makes a real container cannot run in one. Declared here rather than configured on the host because it is a property of what the build *does*, and a recipe is immutable and reviewable. It grants nothing a recipe did not effectively have already: a build script runs as uid 0 inside a **user-namespaced** container, so this widens what that confined root may do to *itself*, not what it may do to the host. `CAP_SYS_ADMIN` additionally delegates the container's own cgroup subtree to it (its `cgroup.procs`, `cgroup.subtree_control` and `cgroup.threads` — never the limit files, so a build still cannot raise the budget it was given). An unrecognised name fails the build rather than being dropped.
- **`pkg_depends`** — a space-separated list of other recipe names to install first (empty string if none), i.e. what the built thing needs at *runtime*. See [Dependencies](#dependencies) below. A hostbuild recipe may declare it too, and should when the artifact links anything: it is recorded on the entry and carried forward, but not resolved there (see [The hostbuild variant](#the-hostbuild-variant) below).
- **`pkg_source` does not have to be an archive.** The daemon decides from the fetched file's own leading bytes, not from the URL: gzip, xz, bzip2, zstd or a bare `ustar` header is extracted into `/build/src`; anything else is placed there as `/build/src/<basename>`, the same basename rule the extra sources below already use. `ca-certificates` is the first real case — its source is a single `.pem`.
- **A source that needs preparing first** — vendored Go modules, a submodule merged in, a generated file — is assembled once in the dev sandbox and published to cix-cache as `<name>-src-<version>-<release>`; `pkg_source` then names that URL and `pkg_sha256` approves the exact bytes, with the assembly steps written out in full in the recipe header. See the [cix-recipes README](https://git.home.arpa/itdlabs/cix-recipes) for the convention and why a scratch LAN server is not a home for one ([#262](https://git.home.arpa/itdlabs/cix/issues/262)), and `recipes/package/glauth` for a real worked example.
- **`{{REPO_TOKEN}}` in `pkg_source`** — optional (issue #60). The literal string `{{REPO_TOKEN}}` anywhere in a source URL is replaced at fetch time with the daemon's own stored repo auth token (`pkg repo-config set --token=`). Lets a recipe that self-fetches from the private Gitea be committed in its final, working form — no credential in the recipe, none in the catalog. Absent/empty token leaves the URL unchanged.
- **`pkg_artifact_sha256`** — optional (ADR-0122). Absent means this recipe always builds from source, exactly as above. Set means: if a plain-HTTP precompiled-artifact server is configured (`GET`/`PUT /v1/pkg/artifact-config`, a separate, non-git thing from `pkg_source` — see [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs)), an install first tries `<base_url>/<name>-<version>-<arch>.tar.gz` and verifies it against this checksum before ever trusting it; a miss (no server, 404, mismatch) silently falls back to `pkg_source`/`pkg_build()`/`pkg_install()` below, unchanged. This is the *only* thing that makes a fetched artifact trustworthy — the server itself is never a trust boundary. When you explain this line in a recipe comment, write the filename generically as `<name>-<version>-<arch>.tar.gz` rather than spelling out this revision's: the comment is copied forward on every version bump and the hardcoded name is never updated, which is how 113 recipe revisions ended up naming a file that is not theirs ([#226](https://git.home.arpa/itdlabs/cix/issues/226)) — `bison@3.8.2-7` still said `bison-3.8.2-2.tar.gz`. Published revisions are immutable so those cannot be corrected; `test_recipe_hygiene` holds the count so it can only fall. And when this line is *absent* — a new revision whose artifact does not exist yet — do not write a comment saying so. Adding the checksum later is the single edit ADR-0107 permits on a published revision, and the daemon accepts it only when the added line is the **whole** difference: `recipe_adds_only_artifact_sha256()` compares the stored and updated recipes line by line and rejects anything else that moved. A comment reading "No pkg_artifact_sha256 ..." therefore has to be edited at the same moment, which makes the change two edits and refuses it — and leaving the comment in place would make the file contradict itself. Omit the line silently; the field's own purpose is documented here, not in every recipe. This cost `procps@4.0.6-9` its approval: the artifact was built and published correctly and could not be approved without burning a whole new revision and rebuild. The reverse direction (a fresh build *publishing* itself to that same server, `cixctl pkg artifact-config set --push`) keeps exactly this property: the digest it sends is a corruption check at the door, and every consumer still verifies against this recipe's own checksum ([ADR-0201](../adr/0201-artifacts-are-retrievable-and-self-publishing.md)).
- **`pkg_changelog`** — optional (ADR-0176). A short, single-line, free-text summary of what changed in this specific published version (a commit subject line, not a release note) — shown in the Web dashboard's package detail page, on a real per-version "Versions" tab. Deliberately single-line: the scanner reads up to the closing quote or a newline, whichever comes first, so a real multi-paragraph changelog isn't representable here by construction. Absent for any recipe that doesn't set it — nothing retroactively required of existing recipes, adopt it whenever you next re-pin one.

## Multi-source recipes

`pkg_source`/`pkg_sha256` are each really a space-separated *list*, positionally paired (ADR-0036) — a single URL is just the one-element case, which is why every simple recipe above still works unchanged. Real need: a package whose build fetches a few small external assets alongside its main tarball (lldap's own web frontend pulls CSS/JS/font files from a CDN), and the network-less build container can't fetch them itself.

```sh
pkg_source="https://example.org/pkg-1.0.tar.gz https://cdn.example.org/font.woff2 https://cdn.example.org/icons.css"
pkg_sha256="<tarball's sha256> <font.woff2's sha256> <icons.css's sha256>"
```

Index 0 is treated as "the" source: fetched, verified, and extracted into `/build/src` exactly like a single-source recipe. Every entry after that is fetched and verified the same way, but never extracted — each lands as a plain file at `/build/extra/<basename-of-its-own-URL>` for `pkg_build()`/`pkg_install()` to reference directly (e.g. `/build/extra/font.woff2`). Up to 16 entries (`PKG_MAX_SOURCES`). Any single entry's fetch failure or checksum mismatch fails the whole job — no partial-success state, matching the single-source case's own all-or-nothing guarantee.

**A real trap this bit in practice**: `/build/extra/`'s filename comes from `url_basename()` of the URL *as written in `pkg_source`*, not from the original file's name at its ultimate origin. If a source needs re-hosting — e.g. mirroring a CDN asset over the LAN (the [remote-development](remote-development.md) trick) for a box with no outbound DNS — the mirror URL's own path must still end in the exact basename `pkg_build()` expects, not a generic renamed pattern (`asset-1.src`, `pkg-name-version-N.src`, etc.). A real `lldap.recipe` LAN-mirror workaround once re-served all 8 of its CDN assets under a generic `lldap-0.6.3-N.src` naming scheme; every checksum passed, the whole build ran to completion, and only the final `cp /build/extra/bootstrap-nightshade.min.css ...` step failed with "No such file or directory" — silent and easy to miss until the log store's own tail-capture fix (ADR-0072) made the real error visible instead of truncated build-progress noise.

## The `pkg_build()`/`pkg_install()` contract

```sh
pkg_build() {
	./configure --prefix=/usr
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
}
```

Both run inside the isolated build container, in this order, with:

- **CWD already at `/build/src`** — the extracted source tree, one leading path component already stripped (so a tarball extracting to `hello-2.12.1/` still leaves you at its own root, not a level above it).
- **`PATH=/usr/bin:/bin`**, **`HOME=/build`** — nothing else in the environment. Whatever your build needs (a compiler, `make`, `autoconf`, ...) must already be present in the build image (see [Build images](#build-images) below) — there is no implicit toolchain.
- **No network access at all** — every source `pkg_build()`/`pkg_install()` could possibly need was already fetched host-side per [Multi-source recipes](#multi-source-recipes) above.
- **`$PKG_DESTDIR`** — set by the daemon to a real, empty staging directory (currently `/build/pkg-dest`). Everything `pkg_install()` writes under it is exactly what gets merged into the target image's rootfs once the build container exits successfully (an ordinary install) or harvested as a standalone artifact (a hostbuild — see below). The overwhelmingly common pattern is `make install DESTDIR="$PKG_DESTDIR"`, which every reasonably well-behaved upstream `Makefile`/`configure` script supports natively.

A failure at any point (`pkg_build()`/`pkg_install()` returning nonzero, the container itself failing to start) lands the package in `PKG_STATE_FAILED` with a diagnosable `error` field — nothing is silently dropped, and nothing partial gets merged into the target image.

### Real gotchas, found the hard way this project's own recipe catalog was built up against

These are genuine, confirmed environment facts about this project's own minimal images — not Cix bugs, just what a from-scratch, `/usr/bin`-only, no-`/tmp` image actually looks like to a build script that assumes a normal Linux distro underneath it:

- **No `/bin`, only `/usr/bin`.** This project's images stage everything under `/usr/bin/` — `/bin/sh` doesn't exist unless something explicitly creates it (`bash.recipe`'s own `pkg_install()` symlinks it, see the real recipe below). glibc's `popen()`/`system()` hardcode `/bin/sh` with no override, so any build step that shells out (`make`'s own recipe lines, `configure`'s `$(shell ...)`-style macros) needs it present in the *build image*, not just the target.
- **No `/tmp`.** Use `/run` instead for any scratch path a build step needs.
- **Absolute tool paths inside a container, not bare names.** `gcc`/`ld` resolve their own installation prefix differently depending on how they're invoked (see `CLAUDE.md`'s own environment notes) — prefer `/usr/bin/gcc` over a bare `gcc` if a recipe's own build step execs a compiler directly rather than through `make`'s normal `$(CC)` indirection.
- **A recipe only ever sees what it declared, or (if it declared nothing) whatever its build image already has.** With `pkg_build_depends` set, the environment is exactly your declared packages — nothing is auto-detected or auto-installed on demand, and anything missing fails the build by name.

### Metadata strings are shell, so no backticks

A recipe is a **sourced shell script**, and `pkg_changelog=` and friends are ordinary double-quoted assignments. So markdown habits are dangerous there:

```sh
# WRONG -- the shell runs `ip vrf` and `ip link ... type vrf`
pkg_changelog="removes the `ip vrf` command; `ip link ... type vrf` is unaffected"
```

```
/build/recipe.sh: line 81: ip: command not found
```

Backticks are command substitution. `$(...)` is too. And the obvious repair is also wrong:

```sh
# ALSO WRONG -- the inner quotes end the string, leaving `ip vrf` as a command
pkg_changelog="removes the "ip vrf" command"
```

Use single quotes inside the double-quoted value:

```sh
pkg_changelog="removes the 'ip vrf' command; 'ip link ... type vrf' is unaffected"
```

This bites hardest in a *changelog*, because that is the field most likely to quote a command name, and the failure appears as a mystery `command not found` at a line number that looks like metadata rather than code. Check before publishing:

```sh
grep -nE '^pkg_[a-z_]+=' build.sh | grep -E '`|\$\('   # must print nothing
```

and confirm the block still sources cleanly:

```sh
( sed -n '/^pkg_name=/,/^pkg_changelog=/p' build.sh > /tmp/m.sh; . /tmp/m.sh; \
  echo "$pkg_version ${#pkg_changelog}" )
```

A changelog that silently truncates to 0 characters is the tell that a quote ended the string early.

### Working out a recipe's build tools

A recipe with no `pkg_build_depends` cannot be built at all — ADR-0199 composes every build environment from a recipe's declared tools **and nothing else**, so it is refused before it starts. Working out the right set is not guesswork; it comes from three real sources:

1. **The baseline.** For an autotools-shaped package, the recipes that already declare their tools converge on `tcc make linux-headers bash coreutils sed grep gawk binutils findutils`. `findutils` is easy to forget and easy to justify: autotools trees shell out to `find` (openldap's own `shtool mkln` does).
2. **What your own `pkg_build()` invokes.** Read it back and take the tools literally — `autoreconf`, `perl`, `python`, `bison`, `m4`, `go`, `cargo`. If the script runs it, declare it.
3. **What you link against.** A library needed at *build* time belongs in `pkg_build_depends` even when it is already in `pkg_depends` — the two answer different questions. `--with-tls=openssl` or `LIBS="-llber"` makes those build-time needs.

**Then build it, and let the failure correct you.** An incomplete set does not fail vaguely — it names what is missing:

```
tcc: error: undefined symbol 'crypt'      -> libxcrypt (glibc 2.44 moved crypt() out)
/bin/sh: find: command not found          -> findutils
```

That loop is what makes this safe to do without a perfect first guess. What is *not* safe is declaring a set and never building it: an unverified `pkg_build_depends` is indistinguishable from a correct one until someone needs it.

### TCC does not support `--version-script`

Symbol versioning is the single most common reason a library fails to link under TCC:

```
CCLD     libmnl.la
tcc: error: unsupported linker option '--version-script=./libmnl.map'
```

Strip it out of the **generated** Makefile after `configure`, never out of upstream's source:

```sh
before=$(find . -name Makefile -exec cat {} + | grep -c '\\$')
find . -name Makefile -exec sed -i 's/-Wl,--version-script[=,][^[:space:]]*//g' {} +
find . -name Makefile -exec grep -l -- '--version-script' {} + | grep -q . && exit 1
after=$(find . -name Makefile -exec cat {} + | grep -c '\\$')
[ "$before" = "$after" ] || exit 1
```

Three details in that snippet each cost a real build:

**`[^[:space:]]`, not `[^ ]`.** A bracket negation of a literal space still matches a TAB. libnftnl's line is

```
libnftnl_la_LDFLAGS = -Wl,--version-script=$(srcdir)/libnftnl.map<TAB>\
```

so `[^ ]*` ran straight through the TAB and the line-continuation backslash and deleted both, orphaning the next line. `make` then failed with `recipe commences before first target`, several directories deep in a recursive build, naming nothing to do with version scripts.

**Count the line-continuations before and after.** The damage above is silent at edit time. Comparing the count of lines ending in `\` across every Makefile catches exactly it. Count over the concatenated stream, not `grep -c` per file: `grep -c` prints `file:count` for multiple files but a bare count for one, so an `awk -F:` sum silently yields 0 for a single-Makefile project and the guard passes without checking anything.

**Not `grep -r --include=`.** The build image's grep does not accept `--include` and reads it as a *filename*, so the guard fails on its own error message before `make` ever runs.

The guard itself matters for the ordinary reason too: a `sed` that silently matched nothing leaves the original failure to be rediscovered at link time, and looks like the fix simply did not work.

Dropping it costs symbol versioning and nothing else — same soname, same exported symbols — and nothing in this project links against a specific symbol version. `libmnl`, `ipset`, `nss-pam-ldapd` and `zlib 1.3.2-6` all make the same trade.

Note the difference between how packages fail here. `libmnl` and `ipset` **fail loudly** at the link step, which is the good case. `zlib`'s configure merely *probed*, printed "No shared library support", built a static library instead and installed cleanly — and the next thing to link against it died. That is issue #113, and it is why a recipe should assert what it built rather than trust that `make` exited 0.

### TCC implements none of the bit-twiddling builtins

TCC does not implement `__builtin_ffs`, `__builtin_clz`, `__builtin_clzll`,
`__builtin_popcount`, `__builtin_bswap16`, `__builtin_bswap32` or
`__builtin_bswap64`. It does not reject them either — it emits each as an
ordinary undefined external symbol. (`__builtin_constant_p`,
`__builtin_expect`, `__builtin_types_compatible_p` and
`__builtin_choose_expr` *are* implemented.)

Whether that is loud or silent depends entirely on what you are building:

- an **executable** fails at link — `tcc: error: undefined symbol '__builtin_ffs'`
- a **shared library** links fine, because undefined symbols are legal in a
  `.so`. It installs, publishes, and stays broken until something calls it.

Both real cases so far have been shared libraries, which is not a
coincidence. `libblkid` shipped an undefined `__builtin_clz` (#176); libnl
needed four of them at once (#207). The install-time gate catches these now
and fails the build rather than publishing — see #176 — but the gate tells
you *that* a builtin is missing, not what to do about it.

The fix in a recipe is a small compatibility header force-included via
`CPPFLAGS`. `libnl`'s recipe is the reference. Two traps it had to avoid,
both of which cost a build each:

**Use plain `static`.** Measured on Cix's own tcc (via
`recipes/package/probe-tcc-conformance`, not a dev sandbox's tcc — see
below): `static inline`, plain `static` and `extern inline` all come out
file-local and link correctly. The one spelling that breaks is **bare
`inline`**, which TCC emits as a strong global in every translation unit
— that is the "defined twice" failure m4 hit, via gnulib's `_GL_INLINE`.
Plain `static` is chosen here as the least surprising of the safe three.
Verify with `nm` that your symbols come out lowercase `t`, not `T`.

**Probe compiler behaviour on a Cix host, never in a dev sandbox.** This
sandbox's `/usr/bin/tcc` is Debian's, and it demonstrably answers
differently from Cix's `tcc@0.9.27-10`: it accepts a compound-literal
array initializer Cix's rejects, rejects anonymous unions Cix's parses,
and reports six builtins as present that Cix's does not implement. A
conclusion from it is not a conclusion about this platform.
`recipes/package/probe-tcc-conformance` is the pattern — a recipe that
runs the probes and deliberately exits nonzero so its output is kept in
the build log.

**Include no system headers in the shim.** A `-include` header is processed
before the translation unit can define `_GNU_SOURCE`, so pulling in any libc
header there latches glibc's feature-test macros too early. libnl's first
attempt included `<strings.h>` for `ffs()` and hid `struct ucred` from an
unrelated header, failing with `field 'nm_creds' has incomplete type` — an
error naming nothing to do with builtins. Write the implementations out by
hand instead, using plain `unsigned int`/`unsigned long long` rather than
`<stdint.h>` types.

This is a workaround, not the fix. #208 tracks implementing the builtins in
TCC itself, after which these shims should be deleted rather than copied
into a third recipe.

### `#!/bin/bash` does not work in the build sandbox

The bash package ships exactly two entry points:

```
bin/sh
usr/bin/bash
```

There is **no `/bin/bash`**. So any script a build actually *runs* — not just ships — fails if it starts `#!/bin/bash`. Note `/bin/sh` does exist, so make's own recipe lines are fine; only the bash shebang is not.

The error is badly misleading. The kernel returns ENOENT for the missing *interpreter*, and the shell reports it against the *script*:

```
/bin/sh: line 1: ./mkcapshdoc.sh: cannot execute: required file not found
make[1]: *** [Makefile:56: capshdoc.c.cf] Error 127
```

which reads as though `mkcapshdoc.sh` is missing. It is present and executable. `/bin/bash` is what is missing. **Exit status 127 plus "required file not found" on a script that visibly exists means a bad shebang, not a bad path.**

Rewrite them before building, and assert the rewrite took:

```sh
find . -name '*.sh' -exec sed -i '1s|^#!/bin/bash|#!/usr/bin/bash|' {} +

bad=""
for f in $(find . -name '*.sh'); do
	case "$(head -1 "$f")" in
	"#!/bin/bash"*) bad="$bad $f" ;;
	esac
done
[ -n "$bad" ] && { echo "shebang survived:$bad" >&2; exit 1; }
```

**Check line 1 only.** A whole-file `grep` for `^#!/bin/bash` is wrong, and libcap is the concrete reason: its `progs/quicktest.sh` contains `#!/bin/bash` inside two heredocs that write a test script at run time. Those are not shebangs of that file, nothing should rewrite them, and a whole-file guard reports them forever — which cost two revisions before the guard was narrowed to match what the `sed` actually changes.

The `head`/`case` form is also deliberate: it cannot fail for its own reasons the way a mis-specified `grep` invocation can. **A guard that can fail on its own error message is worse than no guard**, because it reports a problem that is not there — the failure then looks like the fix did not work.

`libcap`'s recipe is the reference. This one is worth knowing because of what it blocks: libcap is a build dependency of iproute2, so a single unrunnable script stopped a completely unrelated package from building, with the failure reported against libcap rather than against the thing being installed.

CLAUDE.md records the same trap for this project's runtime container images. It applies to the build sandbox for the same reason.

### Consuming another package's pkg-config file

Set `PKG_CONFIG_PATH` explicitly. Packages here do not all use one convention — `libmnl` and `libuuid` install to `usr/lib/pkgconfig`, while a package configured with a multiarch `--libdir` lands in `lib/x86_64-linux-gnu/pkgconfig` — and a consumer should not have to know which its dependency happened to pick:

```sh
export PKG_CONFIG_PATH="/usr/lib/pkgconfig:/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig"
```

Without it, a dependency that is genuinely installed still reports missing (`checking for libmnl >= 1... no`), which reads like a packaging failure rather than a search-path one.

### pkg-config files: ship one exactly when you ship what it describes

A `.pc` file is a **claim about what your package provides** — include paths, a link line, a version. So the rule is not "always keep them" or "always strip them", it is that the claim must be true:

- Your package ships headers and a `.so`? **Keep the `.pc`.** Stripping it means a consumer asking pkg-config gets "not found" for something you really do provide. Three recipes did exactly this on the stale premise that nothing here used pkg-config; 20+ recipes now do (issue #174).
- Your package deliberately ships a **runtime only** — no `usr/include`, no `.a`, no `.so` — as `procps` does? **Strip the `.pc` too.** Restoring it would make pkg-config *succeed* and hand a consumer include paths and a link line for files that are not in the package. The failure then surfaces much later as a confusing compile error instead of an honest "not found", and **a false claim is worse than a missing one.**

The test to apply: *does everything this `.pc` file promises actually exist in `$PKG_DESTDIR`?* If yes, ship it. If no, either ship the missing pieces or strip the `.pc` — never ship a `.pc` that describes files you deleted.

## What you do NOT have to clean up

`pkg_install()` is finished when the package's files are in `$PKG_DESTDIR`. The daemon then runs a finalize phase over that tree, in your build container, before it becomes an artifact ([ADR-0251](../adr/0251-a-package-artifact-carries-what-the-platform-runs.md)). It:

- **strips ELF output** — `--strip-unneeded` for shared objects and executables, `--strip-debug` for `.o` and `.ko`. An archive it keeps is left alone (`go-bootstrap` ships Go `.a` files that `strip` rejects);
- **drops `libfoo.a` when `libfoo.so*` ships beside it** — this platform links dynamically always, so that archive is dead weight. An archive with **no** shared counterpart (`libtcc1.a`, `libgcc.a`, `libc_nonshared.a`) is kept;
- **removes `*.la`**.

It does **not** remove documentation or locale trees. It used to
([ADR-0251](../adr/0251-a-package-artifact-carries-what-the-platform-runs.md) clause 4);
that clause is withdrawn by
[ADR-0306](../adr/0306-a-package-keeps-its-documentation-and-its-licence.md). The line the
finalize phase now holds is **remove what the platform cannot use, never what it merely does
not read** — and the prune had been deleting `usr/share/doc/<package>/COPYING`, which is where
GNU packages install their licence. Your recipe should not delete those trees either.

So **do not hand-write the three rules above in your recipe.** They used to be per-recipe, and the result is the reason the phase exists: of 115 recipes, 37 pruned anything at all, in twenty-one different spellings, while `glibc` shipped `libc.so.6` with 9.46 MiB of debug sections and `libc.a` three times over. A recipe's own strip/`.a`/`.la` pruning is redundant now, not wrong, and comes out when the recipe next revises. **A recipe's own `rm -rf .../share/...` is a different matter since ADR-0306: it is live, and it deletes something the platform now keeps.** Measured across the 147 current revisions on 2026-09-18, **57 delete something under `$PKG_DESTDIR/usr/share`** — 37 of them naming `man`, `doc` or `info` directly, and some, like `xz`, removing the whole tree in one line. Those lines are why withdrawing clause 4 does not by itself give those packages their licences back. Do not write a new one, and take the existing one out when you revise a recipe that has it.

Two consequences for you:

- **If your package produces ELF, declare `binutils`** in `pkg_build_depends`. Without `strip` the build **fails** naming it, rather than quietly shipping an unstripped package. A package that produces no ELF at all needs nothing.
- **The `.pc` rule above still applies, and now has one more input.** If a `.pc` file you ship promises a static archive that the finalize phase drops, the claim has become false — the same failure that section warns about, arriving from the other direction.

## Dependencies: two questions, two fields

A recipe answers two different questions, and they are **not** the same list:

| field | question | where it goes |
|---|---|---|
| `pkg_build_depends` | what must be present to **build** this? | composed into the build container |
| `pkg_depends` | what does the built thing need at **runtime**? | installed into the target image |

`bison` needs `m4` at runtime (it shells out to it) — that is `pkg_depends`. `m4` needs `binutils` to build (it wants `ar`) — that is `pkg_build_depends`. `gcc` needs both, and different ones each.

### `pkg_build_depends` — declare your build tools (ADR-0199)

```sh
pkg_build_depends="tcc make glibc linux-headers bash coreutils sed"
```

**The build container is composed from exactly these packages and nothing else.** Not a suggestion, not documentation — it *is* the environment. Two properties follow, and both are enforced rather than trusted:

- **no less** — a tool you did not declare is not there, and the build fails naming what it wanted
- **no extras** — nothing else is present for the build to accidentally depend on

Declare what you *use*. Each declared tool arrives with its own runtime dependencies (its `pkg_depends`, resolved recursively), so you never enumerate the shared libraries of a tool you named — `binutils` brings `zlib` because `ar` links against it, and that is binutils' business, not yours. A tool that cannot start is not a leaner environment.

A declared tool that cannot be provided (not installed anywhere, or with no recorded files to compose from) **fails the build and names it**. There is deliberately no fallback to a fuller environment: falling back would let the build succeed against something it never declared, which is the exact problem this replaces.

**The one thing you never declare: the C library.** `glibc` is added to every composed environment automatically ([ADR-0216](../adr/0216-the-glibc-floor-is-closed.md)), because every binary in every package links against it and nothing in the environment can `execve` at all without the loader. That is a property of an environment being usable, not something that distinguishes one recipe from another — so it is named once in the mechanism rather than in sixty recipes, where sixty chances to omit it would each fail only at exec time with a bare `ENOENT` naming nothing.

You *may* still name it, and the only reason to is to pin a version:

```sh
pkg_build_depends="tcc make bash coreutils glibc@2.44-6"
```

Declared tools resolve first and duplicates collapse by name, so an explicit pin always wins over the implicit one. Naming it unpinned is harmless and simply redundant.

Before ADR-0216 the loader and libc were *copied into every image off the build host* rather than coming from a package at all. If a build ever fails with `execve(/usr/bin/bash): No such file or directory` for a binary you can see was staged, that is a missing loader, not a missing binary.

Write the list by building and reading the failures. Each one names precisely the next thing to add, and it converges quickly — `zlib` needs seven packages and its declaration says why each one earns its place, including the two it learned the hard way on a real box.

The composed environment is cached as an image named for the hash of your declared set, so recipes sharing a tool set share one environment and it is built once. Declaration order does not matter; the set is sorted before hashing.

**Sufficiency is enforced, minimality is not.** If you declare a tool you do not actually need, the build still succeeds and nothing complains. Keep the list honest by review.

**A recipe declaring nothing** falls back to the shared build sandbox — the old behaviour, which is fungible by construction and on its way out. Prefer declaring.

### Recipes run under `set -e`

`pkg_build()` and `pkg_install()` are executed as `set -e; . recipe.sh; cd src; pkg_build; pkg_install`. **Any command that fails ends the build**, and the package is recorded as failed rather than installed with whatever happened to make it into `$PKG_DESTDIR`.

That matters because the alternative was worse: without it, a `make install` could die halfway, the `rm -rf` after it succeed, and the package be recorded **installed** while missing binaries. A real `libcap` shipped that way, and nothing downstream could tell.

The semicolons matter as much as the flag. POSIX suspends `set -e` for any command in an `&&` list except the last — *including inside functions called from there* — so `pkg_build && pkg_install` would have left every failure inside `pkg_build()` ignored.

Two consequences to write for:

- **A command whose failure is expected must say so**, with `|| true` or an `if`. The common case is stopping a helper you started yourself: `kill` on a job that already exited returns 1, and `wait` on a job you just `kill`ed returns 143. Both are normal, and both end the build unless you mark them:

  ```sh
  ( while true; do keep_fixing_things; sleep 2; done ) &
  helper=$!
  make -j"$(nproc)"
  kill "$helper" 2>/dev/null || true
  wait "$helper" 2>/dev/null || true
  ```

  Skipping those `|| true`s produces a spectacularly misleading failure: the package builds completely, then the recipe dies cleaning up, and the daemon reports exit 143 as *"overlay mount or exec failed: No such process"* — because 143 is both `128+SIGTERM` and, in the daemon's own errno encoding, `140+ESRCH`. A finished build reported as a container that never started.

- **A grep or test used for its answer, not its success**, needs the same treatment — `grep -q pattern file || true` if not matching is a legitimate outcome.

### `pkg_depends` — runtime dependencies

`pkg_depends` is resolved automatically and recursively, before your own recipe's `pkg_build()` ever runs:

```sh
pkg_name="top"
pkg_depends="leaf1 leaf2"
```

Installing `top` installs `leaf1` and `leaf2` first (skipping any already installed), then `top` — one `POST /pkg/install {"name": "top"}` call, three packages end up tracked individually. A dependency diamond (two packages both depending on the same third one) installs the shared dependency exactly once. A missing recipe or a circular dependency (`A` needs `B` needs `A`) is rejected outright (`400`) before anything is fetched.

## Build images

Every install targets one image's rootfs — `pkg_build()`/`pkg_install()` write into a build *container* whose own toolchain is composed from the recipe's `pkg_build_depends` ([ADR-0199](../adr/0199-recipes-declare-their-build-tools.md)), the same way for an ordinary install and a hostbuild alike since [ADR-0304](../adr/0304-a-hostbuild-composes-its-build-environment-like-every-other-build.md) — then the result is merged into the target image, or harvested as a host artifact for a hostbuild.

`POST /pkg/bootstrap` is a separate, one-time convenience specifically for the *shared, default* build sandbox ordinary installs use — see [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs) for that specific mechanism; it doesn't apply to a custom `build_image` used for a hostbuild.

## A complete, real worked example

`recipes/package/bash@5.2.37.sh`, verbatim, annotated with why each line is there:

```sh
pkg_name="bash"
pkg_version="5.2.37"
pkg_source="https://ftp.gnu.org/gnu/bash/bash-5.2.37.tar.gz"
pkg_sha256="9599b22ecd1d5787ad7d3b7bf0c59f312b3396d1e281175dd1f8a4014da621ff"
pkg_depends=""

pkg_build() {
	./configure --prefix=/usr
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	mkdir -p "$PKG_DESTDIR/bin"
	ln -sf /usr/bin/bash "$PKG_DESTDIR/bin/sh"
}
```

The extra `mkdir`/`ln` in `pkg_install()` is a real, deliberate fix, not boilerplate: any image that installs `bash` also gets a standard `/bin/sh -> /usr/bin/bash` symlink, the one path every real Linux distribution guarantees exists — closing the "no `/bin`" gotcha above for every future recipe that needs a working `/bin/sh` in its own build image.

## Adding, updating, and installing a recipe

```
POST /v1/pkg/recipes
{"name": "hello", "content": "pkg_name=hello\npkg_version=2.12.1\n..."}
```

Publishes a new `(name, version)` recipe (ADR-0107) — validated (parses, and its own `pkg_name=`/`pkg_version=` match `name` and the version this call actually publishes) before anything on disk changes. Recipe versions are immutable once published: an already-published `(name, version)` pair is rejected (`409 Conflict`), not silently overwritten — fixing a mistake means bumping `pkg_version=` and publishing again, not re-uploading under the same version. `cixctl pkg recipe add --name=hello --file=./hello.recipe` is the CLI equivalent. A bare `pkg install`/`pkg hostbuild` (no explicit `version`) always resolves to the highest published version for that name. Then:

```
POST /v1/pkg/install
{"name": "hello"}
```

starts the actual build — see [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs) for the full async install/poll/upgrade contract, which this guide doesn't repeat.

Rather than pushing every recipe individually, a host can also point itself at a shared recipe repository and pull its whole tree in one call: `cixctl pkg repo-config set --url=https://git.example.internal/team/recipes --kind=gitea`, then `cixctl pkg sync --wait`. This is additive/merge only — a sync never overwrites or removes a recipe version this host already has, it only adds ones it doesn't (see [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs) and ADR-0121 for the full contract, including the `gitea`/`github`/`gitlab` URL shapes).

Every real build is also cached locally (ADR-0122) — installing the same `(name, version)` into a second image, or reinstalling after deletion, reuses it instead of fetching/compiling again, with no recipe change needed. `cixctl pkg cache-status` / `pkg cache-config` manage its size cap; see [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs) for the full cache + optional precompiled-artifact-server contract.

## The hostbuild variant

A recipe can also be built as a standalone, host-side artifact instead of merging into a container image's rootfs — used for building the Linux kernel and for [self-hosted rebuilds of Cix's own control plane](building-cix.md) (ADR-0056). The recipe format is identical, and the build environment comes from the same place it does for an ordinary install: `pkg_build_depends`, composed into a build container holding exactly the declared tools and nothing else ([ADR-0199](../adr/0199-recipes-declare-their-build-tools.md), [ADR-0304](../adr/0304-a-hostbuild-composes-its-build-environment-like-every-other-build.md)). There is no build image to prepare and none to name. A hostbuild used to be the one exception to that — rooted on `--build-image=`'s rootfs, which meant its recipe's declared build tools were read by nothing at all, and an undeclared tool the image happened to carry worked anyway. Issue #482 is what that cost: the `kernel` recipe deliberately moved `wireless-regdb` out of its own declaration and into the build image's manifest, recording the reason in its changelog, because the declaration could not work.

`pkg_depends` is still worth declaring on a hostbuild recipe, and is no longer refused ([ADR-0303](../adr/0303-a-hostbuild-carries-pkg-depends-it-does-not-resolve-it.md), issue #465). It describes what the finished artifact needs in order to *run*: a hostbuild records it on the entry and carries it forward — visible in `GET /v1/pkg` — but does not resolve or install it, since resolving means "install the closure into an image" and a hostbuild has no image to merge into. The declaration matters because the same recipe may also be installed the ordinary way into an image, where `pkg_depends` is what the linkage gate checks a binary's libraries against. `cix`'s own recipe is the worked example: `cixd` links `-lssl -lcrypto -larchive -lcurl`, so those three packages are what it has to declare.

```sh
pkg_install() {
	cp build/mything "$PKG_DESTDIR/mything"
}
```

`$PKG_DESTDIR`'s contents are copied verbatim to `BASE_DIR/artifacts/<name>/` on the host instead of being merged anywhere — a plain host directory, deliberately never container-visible, so `GET /containers/{name}/files` cannot reach it. To get those bytes *off* the box, use `cixctl pkg artifact-export NAME` ([ADR-0201](../adr/0201-artifacts-are-retrievable-and-self-publishing.md)); before that existed there was no retrieval route at all, and a hostbuild artifact was excluded from backup and wiped by factory reset, which for a multi-hour kernel build is a real loss — see [`docs/guides/kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) and [`docs/guides/building-cix.md`](building-cix.md) for the two real, complete operator runbooks built on this mechanism.
