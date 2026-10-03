# Writing a package recipe

> **Recipes live in their own repository**, [cix-recipes](https://git.home.arpa/itdlabs/cix-recipes),
> and a recipe is one flat file: `recipes/package/<name>@<version>-<release>.cbs`
> ([ADR-0308](../adr/0308-recipes-are-their-own-repository-flat.md)).
> Clone it beside `cix`; tests here resolve it through `CIX_RECIPES_DIR`,
> defaulting to `../cix-recipes/recipes`.

This is the canonical, complete reference for writing a Cix package recipe — the one place this content lives (`daemon/include/pkg.h`'s own header comment points here rather than repeating it). For what the REST endpoints that consume a recipe do, see [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs); for the design behind the pieces, see the ADRs linked from each section.

## One recipe language: CPDL

A recipe is a **CBS recipe**: a declarative document in CPDL 1.0, built by [cix-build-system](https://git.home.arpa/itdlabs/cix-build-system) (`cbs`) and producing a `.cixpkg` artifact ([ADR-0305](../adr/0305-a-recipes-format-is-its-filename.md), [ADR-0307](../adr/0307-a-packages-artifact-format-is-the-one-its-recipe-declares.md), [ADR-0328](../adr/0328-cix-produces-no-tarball.md)). There is no other recipe language and no other artifact format.

- **In cix-recipes** a recipe is the flat file `recipes/package/<name>@<version>-<release>.cbs`.
- **On a host** it is stored as `<name>/<version>/build.cbs`.

`cixd` treats a recipe two ways depending on which part is being read:

- **What it declares** — name, version and release, sources and their `sha256`, build and runtime requirements, `metadata { }` — is read from `cbs explain --json`, run once per published version. The daemon never executes a recipe on the host to learn these values.
- **Its phases** (`prepare`, `configure`, `build`, `check`, `install`) only ever run inside an isolated build container, which has no network access (this project's networking plane has no outbound NAT — see ADR-0007 and `daemon/include/pkg.h`'s own header comment) and none of the host's filesystem. That container is not a user namespace, so its root is host root confined by namespaces and a reduced capability set — see [Build capabilities](#build-capabilities) for exactly what it holds.

**Shell recipes were retired.** `build.sh` recipes, with their `pkg_name=`/`pkg_source=`/... fields and `pkg_build()`/`pkg_install()` functions, no longer exist ([ADR-0329](../adr/0329-shell-recipes-and-tar-gz-artifacts-do-not-exist.md), [#569](https://git.home.arpa/itdlabs/cix/issues/569); the earlier stage is [ADR-0309](../adr/0309-shell-recipes-are-history-the-shell-path-retires-with-its-last-dependent.md)). The daemon refuses any publish that is not CPDL, deletes any stored `build.sh` at startup, and deletes `.tar.gz` artifacts from its local cache. Do not write one, and do not write a test fixture in that form.

## The shape of a recipe

One `package` block. `bash@5.2.37-6.cbs` is a complete, small one (see [A complete, real worked example](#a-complete-real-worked-example)); the skeleton is:

```
# Comments are '#'. There is no /* */.
package "hello" {
    version "2.12.1"
    release 1
    format "cixpkg"

    sources {
        main "hello" {
            url "https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz"
            sha256 "…"
        }
    }

    requires {
        build {
            compiler "tcc"
            tool "make"
            tool "bash"
            …
        }
        runtime {
            package "…"
        }
    }

    metadata {
        "changelog" "…"
    }

    build {
        …
    }

    install {
        …
    }
}
```

The package-level declarations come in CPDL's canonical order: `upstream`, `sources`, `requires`, `replaces`, `resources`, `metadata`, then `capability` and `privileged file` lines, then the phases.

- **`package "NAME"`** — must equal the `name` the recipe is published as (`POST /v1/pkg/recipes {"name": ...}`); a mismatch is a `400`, so a recipe can never silently attach to the wrong name. Charset `[A-Za-z0-9_-]+`, like every simple name in this platform.
- **`version` + `release`** — the package version is always `<version>-<release>`: `version "2.0"` / `release 1` is `2.0-1`, never `2.0`. There is no separate recipe revision; a new release *is* the revision, and anything that should trigger an upgrade gets one.
- **`format "cixpkg"`** — required. CBS's build pipeline refuses to run a recipe declaring anything else.

## Sources

```
sources {
    main "sbsigntools" {
        url "https://git.kernel.org/pub/scm/linux/kernel/git/jejb/sbsigntools.git/snapshot/sbsigntools-v0.9.5.tar.gz"
        sha256 "88ffb0eead3687bed6b0c741e5f440d8f00978493794fa1d11a32bb6a316f7cf"
    }
    extra "ccan" {
        url "https://codeload.github.com/rustyrussell/ccan/tar.gz/b1f28e1"
        sha256 "79f709f16f6223c6d464fe17ea0dc4432ee67abaad575ed35b000a85b998f4f7"
    }
}
```

(`sbsigntools@0.9.5-14.cbs`.) Sources are handed to CBS, not fetched by it: cixd fetches each one host-side and checksum-verifies it, then places it in a cache directory named by its own digest, which CBS re-verifies. A mismatch fails the job outright, with no partial state. The build container has no network and no credentials, so anything a build needs must be named here. Up to 16 sources (`PKG_MAX_SOURCES`, ADR-0036).

- **The `main` source is unpacked under `${src}/<source-name>/`.** `bash`'s `main "bash"` lands at `${src}/bash/bash-5.2.37`. A main source that is not an archive is placed as `${src}/<source-name>/<basename>`: `ca-certificates`' `main "cacerts"` is read as `${src}/cacerts/cacert.pem`.
- **An `extra` source is NOT placed under `${src}`.** A recipe reaches it by name, with `materialize` for a file or `extract` for an archive:

  ```
  materialize $source.shimsigned to "${build}/shim-signed/shim-signed.deb"
  extract $source.ccan into "${glob.top}/lib" as "ccan.git"
  ```

  (`shim@16.1-2.cbs`, `sbsigntools@0.9.5-14.cbs`.) Measured on 192.168.15.95 by `probe-plainsrc@1-1`: listing `${src}` showed only the main source. Assuming otherwise fails with an errno=2 that names the *destination* of the copy rather than the missing source (cix-build-system#236), so when a filesystem operation reports errno=2 against a path you just created, suspect the other operand.
- **`${source.NAME}`** is the verified archive itself, not the unpacked tree.
- **A source may declare mirrors, and they are tried in order.** Repeat `url` inside one source; every url shares that source's one `sha256`, which is what makes a mirror safe — no mirror can substitute different bytes without failing a gate that already exists.

  ```
  sources {
      main "freetype" {
          url "https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz"
          url "https://downloads.sourceforge.net/project/freetype/freetype2/2.13.3/freetype-2.13.3.tar.xz"
          sha256 "0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289"
      }
  }
  ```

  The fetch tries the first, then each of the rest, and reports every url it failed on rather than only the last — so a misspelled mirror does not read exactly like one that is down. Up to 12 fallback urls across all of a recipe's sources (`PKG_MAX_MIRRORS`); a thirteenth is refused at publish rather than dropped.
- **An archive's decompressor must be declared.** CBS unpacks sources *inside* the build container, and this platform's libarchive shells out for bzip2. `libmnl@1.0.5-6` failed before any phase ran with `cannot open archive: Can't initialize filter; unable to run program "bzip2 -d"`; `libmnl@1.0.5-8` declares `tool "bzip2"` for its `.tar.bz2`.
- **`{{REPO_TOKEN}}` in a url** (#60). The literal string is replaced at fetch time with the token of the source that owns the package (`cixctl pkg source set NAME --token=`, ADR-0324; never another source's). A recipe that fetches from the private Gitea can then be committed in its final, working form with no credential in it. An absent token leaves the url unchanged.
- **A source that needs preparing first** — vendored Go modules, a submodule merged in, a generated file — is assembled once and published to cix-cache as `<name>-src-<version>-<release>`; the source's `url` names it and its `sha256` approves the exact bytes, with the assembly steps written out in the recipe's header comment. See the [cix-recipes README](https://git.home.arpa/itdlabs/cix-recipes) for the convention ([#262](https://git.home.arpa/itdlabs/cix/issues/262)), and `glauth@2.4.0-7.cbs` for a worked example.

### Upstream: how a package rolls

A recipe that declares `upstream` can be discovered and rolled automatically ([ADR-0323](../adr/0323-every-package-can-roll-discovery-authentication-and-a-green-build.md)); one that declares none is updated by a person. Two shapes from the corpus:

```
upstream "kernel.org"
```

```
upstream "gitea-tags" {
    tag "v{version}"
    source "https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/hibr/archive/v{version}.tar.gz"
    verify origin
}
```

(`kernel@7.2.3-17.cbs`, `hibr@0.99.4-3.cbs`.) The kinds, the authentication ladder and what each catalogue state means are in [`docs/api/README.md`](../api/README.md). **A recipe that rolls names its release only as `${version}`.** The author stage changes the version, release, main url, sha256 and changelog, and nothing else, so a literal release anywhere else in the body (`${src}/linux-7.2.3`, `lib/modules/7.2.3`) would stay on the old one. The author stage refuses such a revision before committing it, naming the line, and `cixctl pkg source-catalogue` says which literal to replace (cix#567). Comments may name a release freely.

## Requirements: two questions, two blocks

A recipe answers two different questions, and they are **not** the same list:

| block | question | where it goes |
|---|---|---|
| `requires { build { … } }` | what must be present to **build** this? | composed into the build container |
| `requires { runtime { … } }` | what does the built thing need at **runtime**? | installed into the target image |

`bison` needs `m4` at runtime (it shells out to it) — that is `runtime`. `m4` needs `binutils` to build (it wants `ar`) — that is `build`. `gcc` needs both, and different ones each.

```
requires {
    build {
        compiler "tcc"
        tool "make"
        tool "linux-headers"
        tool "bash"
        tool "coreutils"
        tool "sed"
        tool "grep"
        tool "gawk"
        tool "binutils"
        tool "findutils"
    }
    runtime {
        package "libnl"
    }
}
```

### Build requirements (ADR-0199)

**The build container is composed from exactly the declared packages and nothing else** ([ADR-0199](../adr/0199-recipes-declare-their-build-tools.md)). It *is* the environment. Two properties follow, and both are enforced:

- **no less** — a tool you did not declare is not there, and the build fails naming what it wanted
- **no extras** — nothing else is present for the build to depend on by accident

Declare what you *use*. Each declared tool arrives with its own runtime requirements, resolved recursively, so you never enumerate the shared libraries of a tool you named — `binutils` brings `zlib` because `ar` links against it, and that is binutils' business, not yours.

A declared tool that cannot be provided — installed in no image, or with no recorded files to compose from — **fails the build and names it**. The artifact cache is necessary and not sufficient: a cached artifact nobody has installed composes nothing. There is deliberately no fallback to a fuller environment.

**A recipe that declares no build requirements is refused** before its build starts (`daemon/src/pkg.c`).

**Two things you never declare:**

- **The C library.** `glibc` is added to every composed environment automatically ([ADR-0216](../adr/0216-the-glibc-floor-is-closed.md)), because nothing in the environment can `execve` without the loader. If a build fails with `execve(/usr/bin/bash): No such file or directory` for a binary you can see was staged, that is a missing loader, not a missing binary.
- **The engine.** Do not put `cbs` in `requires { build { … } }`; cixd adds it, the same way it adds the C library.

Either may be named only to pin a version, as `tool "NAME@VERSION-RELEASE"`; a declared pin wins over the implicit one, since declared tools resolve first and duplicates collapse by name.

**`tool "bash"` is required in every recipe with a build phase.** It is not a dependency of what the phase invokes; it is what lets the phase start. Without it the build container's PID 1 cannot run the phase (`cix-init: execve(/usr/bin/bash) failed, errno 2`), and the daemon reports `build killed by signal 14` — a **timeout**, not an error naming bash ([cix-build-system#224](https://git.home.arpa/itdlabs/cix-build-system/issues/224)).

The composed environment is cached as an image named for the hash of the declared set, so recipes sharing a tool set share one environment, built once. Declaration order does not matter; the set is sorted before hashing. **Sufficiency is enforced, minimality is not**: a tool declared but not needed still builds, so keep the list honest by review.

### Working out a recipe's build tools

The right set comes from three sources:

1. **The baseline.** For an autotools-shaped package the corpus converges on `compiler "tcc"` plus `tool` lines for `make linux-headers bash coreutils sed grep gawk binutils findutils`. `findutils` is easy to forget: autotools trees shell out to `find` (openldap's own `shtool mkln` does).
2. **What your phases run.** Read every `run` back and take the executables literally — `autoreconf`, `perl`, `python`, `bison`, `m4`, `go`, `cargo`. If the recipe runs it, declare it. That includes whatever upstream's build system runs on its own.
3. **What you link against.** A library needed at *build* time belongs in `build` even when it is already in `runtime` — the two answer different questions. `--with-tls=openssl` or `LIBS=-llber` make those build-time needs.

**Then build it, and let the failure correct you.** An incomplete set names what is missing:

```
tcc: error: undefined symbol 'crypt'      -> libxcrypt (glibc 2.44 moved crypt() out)
/bin/sh: find: command not found          -> findutils
```

What is *not* safe is declaring a set and never building it: an unverified requirement list is indistinguishable from a correct one until someone needs it.

### Runtime requirements

`runtime` packages are resolved automatically and recursively before the recipe's own build runs. Installing `top` whose runtime block names `leaf1` and `leaf2` installs those first (skipping any already installed), then `top` — one `POST /v1/pkg/install {"name": "top"}`, three packages tracked individually. A dependency diamond installs the shared dependency once. A missing recipe or a circular dependency is rejected (`400`) before anything is fetched.

## Metadata

```
metadata {
    "artifact_sha256" "…"
    "changelog" "…"
}
```

- **`changelog`** — a short, single-line summary of what changed in this published version (ADR-0176), shown on the dashboard's per-version Versions tab. Capped at 511 bytes (`PKG_CHANGELOG_MAX`); over it, the publish is refused with a message about parsing that is not about parsing (#493).
- **`artifact_sha256`** — the approval of one specific `.cixpkg` byte sequence ([ADR-0122](../adr/0122-pkg-redesign-part3-artifact-cache-and-server.md)). Absent means the recipe always builds from source. Present means: when package repositories are configured (`/v1/pkg/repositories`, ADR-0324), an install tries each in order at `<url>/<name>-<version>-<arch>.cixpkg` and verifies every copy against this checksum before trusting it; a copy that fails is refused and the next tried, and when every one misses it builds from source, unchanged. The repository itself is never a trust boundary.

  **You do not write the checksum yourself.** Publish without one, let it build, and cixd writes the approval into the stored recipe — then fetch the recipe back and commit **that**, or the copy in git and the published one differ. Never carry a checksum forward from another version: a publish whose `artifact_sha256` another version of the same package already declares is a `400` (#525). Adding the approval is the one edit permitted on a published version (ADR-0107), and only when it is the whole difference, so do not write a comment saying a revision has no approval yet — it would have to change in the same edit, and that is refused. A fresh build publishing itself to a repository marked push (`cixctl pkg repository set NAME --push=on`) keeps the same property: every consumer still verifies against the recipe's own checksum ([ADR-0201](../adr/0201-artifacts-are-retrievable-and-self-publishing.md)).

## Package-level declarations

### Build capabilities

```
capability "CAP_SYS_ADMIN"
```

Almost every recipe declares no capability and should not. It exists for one shape of recipe: one whose build **creates containers**. The recipes that declare it run this platform's own tests — `cix` (its `make selftest` gate), `cix-tests` (`make testreport`) and their probes (#224). A build container is otherwise measured to have no `CLONE_NEWNET`, no `CLONE_NEWNS`, no `mount()` and no cgroup tree. The capability is declared in the recipe rather than configured on the host because it is a property of what the build *does*, and a recipe is immutable and reviewable. Capabilities are read by name; an unrecognised one fails the build rather than being dropped, and an engine too old to report names is refused rather than read as zero.

**Forgetting it produces no error.** The build runs without a cgroup tree or the right to clone namespaces, and any step that creates containers fails in a way that reads like a platform fault. Measured on `cix@v2.57.244` ([cix#515](https://git.home.arpa/itdlabs/cix/issues/515)):

| release | `capability "CAP_SYS_ADMIN"` | `cgroup_create` errors | `ns_clone3: Operation not permitted` | selftest |
|---|---|---|---|---|
| `-4` | absent | 258 | 2 | 64 PASS, 28 FAIL |
| `-6` | present | 0 | 0 | 92 PASS |

Nothing else differs between the two. The declared capabilities are the only thing the recipe contributes to the container spec (`start_build_container_spec()` in `daemon/src/pkg.c`), and cixd mounts cgroup2 into the container only when `CAP_SYS_ADMIN` is among them (`src/container.c`, the `mount_cgroup2` derivation).

**What this grants is real host privilege.** A build container gets new PID, mount, UTS, network and cgroup namespaces and **no user namespace** (`CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWCGROUP`, no `CLONE_NEWUSER`), so uid 0 in a build is uid 0 on the host. By default `container_caps_drop()` (`src/container_caps.c`) removes 20 capabilities from the bounding set — `CAP_SYS_ADMIN`, `CAP_SYS_MODULE`, `CAP_SYS_PTRACE`, `CAP_SYS_RAWIO`, `CAP_BPF` and the rest of its deny-list — and sets `PR_SET_NO_NEW_PRIVS`. Every other root capability (`CAP_DAC_OVERRIDE`, `CAP_CHOWN`, `CAP_SETUID`, `CAP_NET_ADMIN`, ...) stays, confined by those namespaces. Declaring `CAP_SYS_ADMIN` keeps that one out of the drop: host root may then mount, create namespaces and perform the wide set of administrative operations `container_caps.c`'s own comment describes as including well-known container-breakout vectors. There is no seccomp filter or LSM behind it.

### Setuid and setgid files

A file that must be setuid or setgid is declared at package level, before the first phase, and given that exact mode in `install`:

```
privileged file "${dest}/sbin/unix_chkpwd" mode 04755
...
install {
    ...
    chmod 04755 "${dest}/sbin/unix_chkpwd"
}
```

cbs refuses any setuid or setgid entry that is not declared, and refuses a declared one whose staged mode differs. The installer keeps a declared mode from cix 0.2.57-417 on (#552; 416 failed to compile); before that it masked every file to `0777`, so no setuid bit reached any image. Leave a program unprivileged when it has an unprivileged route: `ping` and `mtr` use ICMP datagram sockets through `net.ipv4.ping_group_range`, not setuid. `linux-pam` and `openssh` are the worked examples.

### Moving a file between packages

A path in an image belongs to one installed package ([ADR-0319](../adr/0319-a-path-belongs-to-one-package.md)). An install that ships a path another installed package owns is refused, naming both packages and the path. When a file really is moving, from `util-linux` into its own `libuuid` package for example, the package receiving it says so at package level, after `requires` and before the first phase:

```
requires {
    ...
}

replaces {
    package "util-linux"
}
```

The install is then allowed, and every path it shares with `util-linux` becomes its own: `util-linux` no longer lists them, and removing `util-linux` leaves them in place. It works only in that direction. The package giving the file up needs no declaration, and naming a package you do not take files from changes nothing. Needs cbs v0.1.99 (cix-build-system#275) and cix 0.2.57-423. A collision is a recipe bug to fix with this declaration, never a path to exempt.

### Declaring how much memory the build needs

Every build shares one memory budget, `memory_max` in `cixctl pkg-build-config` (2 GiB by default, #85). A package whose build needs more says so at package level, after `requires` and any `replaces` and before `metadata` and the first phase ([ADR-0322](../adr/0322-a-recipe-declares-its-build-memory-within-an-operator-ceiling.md)):

```
resources {
    memory "4GiB"
}
```

The value is the whole build's peak, all phases and processes together, with a `KiB`, `MiB` or `GiB` suffix. It is not a per-process limit. While the build runs, the shared budget is raised to it, up to the operator's `memory_max_ceiling`. A recipe that declares more than the ceiling is refused before it builds, and the error names both numbers and the command that raises the ceiling. The ceiling equals `memory_max` until an operator raises it, so on a fresh host a declaration above 2 GiB is refused until someone decides the host can give it.

Declare it only from a measured build. A build short of memory stalls rather than fails: `node` sat at about 1.5% CPU in iowait at 2 GiB, and peaked at 3.73 GB once given 4 GiB. Needs cbs v0.1.100 (cix-build-system#276) and the cix release that carries #558.

## Phases and operations

Five ordered phases — `prepare`, `configure`, `build`, `check`, `install` — each optional, validated before anything runs. Inside a cixd build container the recipe's paths are:

| variable | path | holds |
|---|---|---|
| `${src}` | `/build/cbsws/src` | unpacked `main` sources |
| `${build}` | `/build/cbsws/build` | scratch for the build |
| `${dest}` | `/build/cbsws/dest` | the staged package — exactly what becomes the artifact |

Measured by `probe-cpdlpaths@1-1` on 192.168.15.95. Anything reading a build container from outside (`GET /v1/containers/<name>/files`) needs the literal `/build/cbsws/...` path for what the recipe wrote. `$jobs` is the parallelism to pass to `make`.

The install phase is finished when the package's files are under `${dest}`. Everything there is merged into the target image once the build exits successfully (an ordinary install) or kept as a host artifact (a [hostbuild](#the-hostbuild-variant)). A failure at any point lands the package in `PKG_STATE_FAILED` with a diagnosable `error` field; nothing partial is merged.

### The operations you will reach for

Every form below is used in a real recipe in cix-recipes:

| operation | example | note |
|---|---|---|
| run a command | `run "make" { jobs $jobs }` | a nonzero exit fails the build, and CBS prints the failing command |
| set the environment | `run "./configure" { "--prefix=/usr" env "CC" = "tcc" }` | `env` applies to that one `run` |
| run in a directory | `cd "dir" { run "cmd" { } }` | |
| capture one line | `run "pkg-config" { "--modversion" "x" stdout "ver" }` … `${stdout.ver}` | a bound name substitutes anywhere a value does |
| capture output to a file | `run "head" { "-c" "4" "out.squashfs" stdout file "${build}/magic" }` | bounded at 64 KiB |
| expect a non-zero exit | `run "cmd" { expect exit 2 }` | `expect exit N` needs the real N |
| assert on one line of stdout | `run "grep" { … expect { stdout contains "…" } }` | **one line only** — see below |
| allow a failure | `on_fail { run "y" { … allow_failure } }` | `allow_failure` belongs inside an `on_fail` block; a phase's `on_fail` runs whenever that phase fails |
| make a directory | `mkdir "${dest}/usr/bin" parents chmod 0755` | write `parents` explicitly — see below |
| copy | `copy "a" to "b"`, `copy glob "…/*.h" to "DEST"` | one source to one destination; `move` and `remove` take `glob` too |
| remove | `remove tree "dir"` | but see [What you do NOT have to clean up](#what-you-do-not-have-to-clean-up) |
| symlink | `symlink "/usr/bin/bash" to "${dest}/bin/sh"` | target first |
| write a file | `write "f" "text"`, or a `"""` block string for several lines | `write` does not create parent directories |
| edit a file | `replace "f" { from "X" to "Y" exactly 1 }` | literal; `exactly N` fails loudly when upstream moves it |
| edit many files | `replace glob "…/**/M" { from "X" to "Y" exactly N }` | `**` crosses directories, `*` stays within one segment |
| edit a flag and its argument | `replace "f" { from "-Wl,--version-script" until whitespace to "" exactly 1 }` | |
| edit only line 1 | `replace glob "…/*.sh" { from "#!/bin/bash" at line 1 to "#!/usr/bin/bash" exactly 12 }` | |
| assert a file | `require file "x" { exists nonempty }` | `exists` is required and must come first |
| assert a symlink | `require symlink "l" { exists target "dest" }` | |
| assert a directory | `require directory "d" { exists }` | |
| assert linkage | `links "${dest}/usr/lib/libnftnl.so.11" { needs "libmnl.so.0" }` | `forbids` is the negative check |
| use an extra source | `materialize $source.X to "PATH"`, `extract $source.X into "DIR" as "NAME"` | see [Sources](#sources) |

**`exactly N` is the guard.** A literal `replace` that matched nothing fails with the file and the count, so there is no need for a separate check that the edit took. On a `replace glob`, **`exactly N` is a TOTAL across every matched file**, not a count per file. You usually cannot know that number in advance; declare `exactly 1`, let the build fail, and read the real total out of `source edit expected 1 matches but found 23`. One deliberate failing build buys an assertion that names the count whenever upstream changes.

**`require file` matches regular files only.** It `lstat()`s and demands `S_ISREG`, so it reports a **symlink** as "does not exist" (cix-build-system#175). Assert on `libz.so.1.3.2`, and assert the `libz.so.1` soname with `require symlink … { exists target "libz.so.1.3.2" }`. `target` belongs to `require symlink`; `require file` rejects it with `error[CPDL-E3004]: file assertion must use contains, same_as, or nonempty`. `require file` is also confined to the recipe's own trees and refuses an absolute path.

**CPDL has no "does not contain", and `grep` is the idiom for one.** `grep` exits 1 when it matches nothing, so a `run` with `expect exit 1` is a negative assertion — exact rather than a substring search when the file is exactly the bytes you are testing:

```
run "head" { "-c" "4" "out.squashfs" stdout file "${build}/magic" }
require file "${build}/magic" { exists contains "hsqs" }

run "grep" { "-F" "$1" "${build}/ff-logo.txt" expect exit 1 }
```

Use `-F` whenever the needle contains a regex metacharacter, which `$1` and a leading `.` both do.

**CPDL will not run a shell, and will not let you name one.** `sh`, `bash`, `dash`, `ash`, `ksh`, `zsh` and `env` are rejected as `run` executables, at validation and again at runtime, so a recipe cannot embed logic in an interpreter argument. A verified upstream script is fine — the kernel honours its `#!` line. When that line is wrong for this platform, **fix the shebang rather than reaching for the interpreter** (see [`#!/bin/bash` does not work in the build sandbox](#binbash-does-not-work-in-the-build-sandbox)).

**Parallelism is a choice.** `jobs $jobs` on `make` is the normal form; a package whose build is known to break under `-j` leaves it off. A command that parallelises internally, like `go build`, is unaffected.

### Three things measured the expensive way

Each of these cost a completed build on 192.168.15.95 before failing.

- **`write` does not create parent directories.** `fastfetch@2.68.1-5` wrote a logo into `cixlogo/` after a full cmake build and got `error[CPDL-E4004]: filesystem operation failed: ...; errno=2 (No such file or directory)`. Make the directory first with `mkdir … parents`. **Write `parents` explicitly**: whether `mkdir` creates missing parents by default changed in cbs v0.1.66 and changed back in v0.1.70 (cix-build-system#243, #246), and an explicit `parents` builds under every engine version.
- **`expect { stdout contains ... }` is for single-line output only.** Assert on anything chattier and you get `error[CPDL-E4001]: process stdout must be one line`. For a multi-line program use `stdout file "..."` and then `require file "..." { exists contains "..." }`, which is what cbs#185 added the file form for.
- **Only stdout is captured; there is no `2>&1` and no stderr assertion.** `bird@2.19.1-11` asserted `stdout contains "2.19.1"`, the build log shows `BIRD version 2.19.1` printed and the command exiting 0, and the assertion still failed — bird writes `--version` to stderr, and cbs `dup2`s the capture over `STDOUT_FILENO` alone. A `--version` gate on a daemon is likely to need cbs#206 before it can be written at all.

**`expect { stdout contains … }` takes ONE LINE.** It is right for `pkg-config --modversion`, and wrong for almost everything else — including a `--version` banner, the case it looks designed for: GNU's convention is four lines, so `msgfmt --version` is refused. Three ways out, in order of preference:

1. **If you are asserting a link dependency, use `links`.** It is a first-class DT_NEEDED assertion with both directions:

   ```
   links "${dest}/usr/sbin/nft" {
       needs "libnftables.so.1"
       needs "libgmp.so.10"
   }
   links "${dest}/usr/bin/btop" {
       forbids "libstdc++.so.6"
   }
   ```

   **`links` compares a FULL SONAME, not a substring** — `needs "libnftables"` fails on a binary that does link it, with `artifact is missing required library`, which reads as "not linked" when the fault is the spelling.

2. **If the thing you want is "did this succeed", drop the assertion and keep a bare `run`.** A nonzero exit fails the build. `msgfmt --version` proves the tool runs; matching its own name in its own banner was never the part doing the work.

3. **If you need to search real output, `stdout file PATH` then `require file { exists contains … }`** — bounded at 64 KiB, so not for `nm` over an archive. `iw@6.17-5` does this with `readelf`.

**A command that prints a lot cannot be asserted on at all.** `nm` over a static archive is far past 64 KiB, and there is no pipe, no filter and no line-oriented match. Where the question is really *is this object in that archive*, ask it directly — `ar x ARCHIVE MEMBER` exits nonzero when the member is absent, and what it extracts is an ordinary file `require file` can check:

```
mkdir "${build}/absorb" parents
cd "${build}/absorb" {
    run "ar" {
        "x"
        "${src}/…/libtextstyle.a"
        "rpl_la-cr-parser.o"
    }
}
require file "${build}/absorb/rpl_la-cr-parser.o" {
    exists
    nonempty
}
```

`gettext@1.0-22` does this, and it asks the exact question the failure it guards against (cix#220) raised: whether objects were *members* of the archive. A `require file … contains "libtextstyle_cr_"` on the archive would have been silently useless — libtextstyle's own objects *reference* those renamed symbols, so the bytes are in the file whether or not the definitions arrived.

### What CPDL cannot do yet

These shapes exist and the corpus uses each: a compound body per list item ([#176](https://git.home.arpa/itdlabs/cix-build-system/issues/176)), `replace … until whitespace` for a flag and its argument ([#177](https://git.home.arpa/itdlabs/cix-build-system/issues/177)), `stage library` ([#178](https://git.home.arpa/itdlabs/cix-build-system/issues/178)), and `stdout file PATH` for output an assertion needs to read ([#185](https://git.home.arpa/itdlabs/cix-build-system/issues/185)).

What is open, measured on 2026-09-22:

| shape | what to do instead | issue |
|---|---|---|
| assert a tool exists in the build environment | a bare `run "find" { "--version" }` — `require file` is confined to the recipe's own trees and refuses an absolute path | [#225](https://git.home.arpa/itdlabs/cix-build-system/issues/225) |
| a recipe that omits `tool "bash"` | declare it in every recipe with a build phase (see [Build requirements](#build-requirements-adr-0199)); the failure is reported as a timeout rather than naming bash | [#224](https://git.home.arpa/itdlabs/cix-build-system/issues/224) |
| `stage library` on a symlink | nothing — it dereferences, which is what the corpus needs; the spec says otherwise | [#222](https://git.home.arpa/itdlabs/cix-build-system/issues/222) |
| run each of many files separately (a loop) | put the loop in the project's own build system and call that one target. `run` takes `timeout` and `allow_failure` for one command, and `args glob` passes every match to a single command; there is no per-match iteration, and an interpreter is not a valid `run` executable (CPDL-E3006). `cix-tests` calls `make testreport` for exactly this reason | by design |

## Real gotchas, found the hard way

These are confirmed facts about this project's build environment and toolchain — not Cix bugs, just what a from-scratch, `/usr/bin`-only environment looks like to an upstream build system that assumes a normal Linux distro underneath it.

- **No `/bin`, only `/usr/bin`.** Packages stage their binaries under `/usr/bin/`; `/bin/sh` exists because `bash`'s recipe creates the link (see the [worked example](#a-complete-real-worked-example)). glibc's `popen()`/`system()` hardcode `/bin/sh`, so any build step that shells out (make's own recipe lines, `configure` macros) needs `bash` declared.
- **Scratch space.** A build container has `tmpfs` mounts on both `/tmp` and `/run` (`/proc/mounts`, measured by `probe-host-cache-visible@1`, 2026-09-25); a runtime image may have no `/tmp`, so a script the *package ships* should use `/run`. Inside the build, prefer `${build}`.
- **Absolute tool paths, not bare names, when a recipe execs a compiler directly.** gcc resolves its own installation prefix differently depending on how it is invoked — prefer `/usr/bin/gcc` over a bare `gcc` for a `run` that names a compiler rather than going through make's `$(CC)`.
- **`CC=tcc` explicitly.** A bare `cc` in an environment that also holds gcc is not guaranteed to be TCC; `bash`, `libmnl` and `libnftnl` all pass `env "CC" = "tcc"` to `configure`.
- **A recipe only ever sees what it declared** — see [Build requirements](#build-requirements-adr-0199).

### TCC does not support `--version-script`

Symbol versioning is the single most common reason a library fails to link under TCC:

```
CCLD     libmnl.la
tcc: error: unsupported linker option '--version-script=./libmnl.map'
```

Strip it out of the **generated** Makefile after `configure`, never out of upstream's source. The argument is a filename, different at every site, so a literal match cannot express it; `until whitespace` matches from a literal prefix to the next whitespace, which is the shape of a flag carrying an argument:

```
build {
    cd "${src}/libmnl/libmnl-1.0.5" {
        run "./configure" {
            "--prefix=/usr"
            "--libdir=/usr/lib"
            "--disable-static"
            env "CC" = "tcc"
        }
    }
    replace "${src}/libmnl/libmnl-1.0.5/src/Makefile" {
        from "-Wl,--version-script"
        until whitespace
        to ""
        exactly 1
    }
    cd "${src}/libmnl/libmnl-1.0.5" {
        run "make" {
            jobs $jobs
        }
    }
}
```

(`libmnl@1.0.5-8.cbs`.) The edit comes after `configure`, which generates the Makefile it edits, and `exactly N` is the guard that the strip took — a recipe with several Makefiles uses `replace glob` and the total (`libnl@3.11.0-7`'s changelog records seven strips).

Dropping it costs symbol versioning and nothing else — same soname, same exported symbols — and nothing in this project links against a specific symbol version. `libmnl`, `libnl`, `ipset`, `nss-pam-ldapd` and `zlib` all make the same trade.

How packages fail here differs. `libmnl` and `ipset` **fail loudly** at the link step, which is the good case. `zlib`'s configure merely *probed*, printed "No shared library support", built a static library instead and installed cleanly — and the next thing to link against it died. That is issue #113, and it is why a recipe should assert what it built (`require file`, `require symlink`, `links`) rather than trust that `make` exited 0.

### TCC does not implement the byte-swap builtins

The pinned TCC (`tcc@0.9.28rc-*`, [ADR-0223](../adr/0223-the-compiler-is-a-pinned-upstream-snapshot.md)) does not implement `__builtin_bswap16`, `__builtin_bswap32` or `__builtin_bswap64`, measured on a Cix host by `probe-tcc-conformance@9` in cix-recipes. It does not reject them either — it emits each as an ordinary undefined external symbol. The other bit-twiddling builtins that probe checks (`__builtin_ffs`, `__builtin_clz`, `__builtin_popcount`, ...) are implemented.

Whether that is loud or silent depends on what you are building:

- an **executable** fails at link — `tcc: error: undefined symbol '__builtin_bswap32'`
- a **shared library** links fine, because undefined symbols are legal in a `.so`. The install-time ELF gate then refuses it (#176), which tells you *that* a builtin is missing, not what to do about it.

The fix in a recipe is a small compatibility header, written by the recipe and force-included through `CPPFLAGS`. `libnl@3.11.0-7.cbs` is the reference:

```
write "${src}/libnl/libnl-3.11.0/tcc-builtins.h" """
#ifndef CIX_TCC_BUILTINS_H
#define CIX_TCC_BUILTINS_H
…
#endif
"""
…
run "./configure" {
    …
    env "CPPFLAGS" = "-include ${src}/libnl/libnl-3.11.0/tcc-builtins.h"
}
```

Two rules for the shim:

**Use `static inline` or plain `static`.** `static inline` is the one inline spelling that links correctly on this TCC and would on any other compiler; bare `inline` and `extern inline` are not portable here (`probe-tcc-conformance@9`, section 2). Verify with `nm` that your symbols come out lowercase `t`, not `T`.

**Include no system headers in the shim.** A `-include` header is processed before the translation unit can define `_GNU_SOURCE`, so pulling in any libc header there latches glibc's feature-test macros too early — libnl's shim once hid `struct ucred` that way, failing with `field 'nm_creds' has incomplete type`. Write the implementations out by hand, using plain `unsigned int`/`unsigned long long` rather than `<stdint.h>` types.

**Probe compiler behaviour on a Cix host, never in a dev sandbox.** A sandbox's `/usr/bin/tcc` is not Cix's, and an answer from it is not an answer about this platform. `probe-tcc-conformance` is the pattern — a recipe that runs the probes and deliberately fails, so its output is kept in the build log (read it with `cixctl pkg build-logs --file=NAME`).

#208 tracks implementing the byte-swap builtins in TCC itself, after which these shims should be deleted rather than copied into another recipe.

### `#!/bin/bash` does not work in the build sandbox

The bash package ships exactly two entry points:

```
bin/sh
usr/bin/bash
```

There is **no `/bin/bash`**. So any script a build actually *runs* — not just ships — fails if it starts `#!/bin/bash`. `/bin/sh` does exist, so make's own recipe lines are fine; only the bash shebang is not.

The error is misleading. The kernel returns ENOENT for the missing *interpreter*, and the shell reports it against the *script*:

```
/bin/sh: line 1: ./mkcapshdoc.sh: cannot execute: required file not found
make[1]: *** [Makefile:56: capshdoc.c.cf] Error 127
```

which reads as though `mkcapshdoc.sh` is missing. It is present and executable; `/bin/bash` is what is missing. **Exit status 127 plus "required file not found" on a script that visibly exists means a bad shebang, not a bad path.**

Rewrite line 1 before building:

```
replace glob "${src}/libcap/libcap-2.78/**/*.sh" {
    from "#!/bin/bash"
    at line 1
    to "#!/usr/bin/bash"
    exactly 12
}
```

(`libcap@2.78-18.cbs`.) **Match line 1 only.** libcap's `progs/quicktest.sh` contains `#!/bin/bash` inside two heredocs that write a test script at run time; those are not shebangs of that file and nothing should rewrite them. `at line 1` restricts the edit to the real shebangs, and `exactly 12` fails the build when upstream adds or removes one. For a single script, a plain `replace` does the same: `sbsigntools@0.9.5-11` corrects `create-ccan-tree`'s shebang rather than running it through an explicit interpreter.

`libcap` is worth knowing for what it blocks: it is a build dependency of iproute2, so a single unrunnable script stopped an unrelated package from building, with the failure reported against libcap rather than against the thing being installed. CLAUDE.md records the same trap for this project's runtime container images.

### Consuming another package's pkg-config file

Set `PKG_CONFIG_PATH` explicitly. Packages here do not all use one convention — `libmnl` and `libuuid` install to `usr/lib/pkgconfig`, while a package configured with a multiarch `--libdir` lands in `lib/x86_64-linux-gnu/pkgconfig` — and a consumer should not have to know which its dependency picked:

```
run "./configure" {
    …
    env "CC" = "tcc"
    env "PKG_CONFIG_PATH" = "/usr/lib/pkgconfig:/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig"
}
```

(`libnftnl@1.2.9-8.cbs`.) Without it, a dependency that is genuinely installed still reports missing (`checking for libmnl >= 1... no`), which reads like a packaging failure rather than a search-path one.

### pkg-config files: ship one exactly when you ship what it describes

A `.pc` file is a **claim about what your package provides** — include paths, a link line, a version. So the rule is not "always keep them" or "always strip them", it is that the claim must be true:

- Your package ships headers and a `.so`? **Keep the `.pc`.** Stripping it means a consumer asking pkg-config gets "not found" for something you really do provide (issue #174).
- Your package deliberately ships a **runtime only** — no `usr/include`, no `.a`, no `.so` — as `procps` does? **Strip the `.pc` too.** Keeping it would make pkg-config *succeed* and hand a consumer include paths and a link line for files that are not in the package. The failure then surfaces much later as a confusing compile error instead of an honest "not found", and **a false claim is worse than a missing one.**

The test: *does everything this `.pc` file promises exist under `${dest}`?* If yes, ship it. If no, either ship the missing pieces or strip the `.pc` — never ship a `.pc` that describes files you deleted.

## What you do NOT have to clean up

The install phase is finished when the package's files are under `${dest}`. The daemon then runs a finalize phase over that tree, in your build container, before it becomes an artifact ([ADR-0251](../adr/0251-a-package-artifact-carries-what-the-platform-runs.md)). It:

- **strips ELF output** — `--strip-unneeded` for shared objects and executables, `--strip-debug` for relocatable objects (ELF type `ET_REL`: `.o`, `.ko`, Go's `.syso` — chosen by the ELF header, not the file name). An archive it keeps is left alone (`go-bootstrap` ships Go `.a` files that `strip` rejects);
- **drops `libfoo.a` when `libfoo.so*` ships beside it** — this platform links dynamically always, so that archive is dead weight. An archive with **no** shared counterpart (`libtcc1.a`, `libgcc.a`, `libc_nonshared.a`) is kept, and **a compiler runtime archive is kept regardless** (`libstdc++`, `libsupc++`, `libgcc*`, `libatomic`, `libgomp`, `libitm`, `libquadmath`, `libssp`, `libobjc`): it is what `-static-libstdc++` and `-static-libgcc` link ([ADR-0310](../adr/0310-a-compiler-runtime-archive-is-not-a-duplicate.md), #521);
- **removes `*.la`**.

It does **not** remove documentation or locale trees ([ADR-0306](../adr/0306-a-package-keeps-its-documentation-and-its-licence.md)). The rule is **remove what the platform cannot use, never what it merely does not read** — `usr/share/doc/<package>/COPYING` is where GNU packages install their licence. Your recipe should not delete those trees either.

So **do not write the three rules above into your recipe.** A recipe restating platform policy is a second place for that policy to live. A `remove tree` under `${dest}/usr/share` is worse than redundant: it deletes something the platform keeps, typically the licence. (`remove glob` also fails on a glob matching nothing, which is one more reason not to write one for `*.la`.)

Two consequences for you:

- **If your package produces ELF, declare `tool "binutils"`.** Without `strip` the build **fails** naming it, rather than quietly shipping an unstripped package. A package that produces no ELF needs nothing.
- **The `.pc` rule above has one more input.** If a `.pc` file you ship promises a static archive that the finalize phase drops, the claim has become false.

## Build images

Every install targets one image's rootfs. The build itself runs in a build *container* composed from the recipe's build requirements ([ADR-0199](../adr/0199-recipes-declare-their-build-tools.md)), the same way for an ordinary install and a hostbuild ([ADR-0304](../adr/0304-a-hostbuild-composes-its-build-environment-like-every-other-build.md)); the result is then merged into the target image, or kept as a host artifact for a hostbuild. A target image must already exist — `cixctl image create --name=NAME` makes one, and an install naming an image that does not exist is refused (#500). An install that names no image goes to `base`.

`POST /pkg/bootstrap` stages a toolchain into the shared, sandboxed build image — see [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs). It plays no part in a composed build environment, which holds the declared packages and nothing else.

## A complete, real worked example

`bash@5.2.37-6.cbs` in cix-recipes, with its header comment removed and its changelog shortened:

```
package "bash" {
    version "5.2.37"
    release 6
    format "cixpkg"

    sources {
        main "bash" {
            url "https://ftp.gnu.org/gnu/bash/bash-5.2.37.tar.gz"
            sha256 "9599b22ecd1d5787ad7d3b7bf0c59f312b3396d1e281175dd1f8a4014da621ff"
        }
    }

    requires {
        build {
            compiler "tcc"
            tool "make"
            tool "linux-headers"
            tool "bash"
            tool "coreutils"
            tool "sed"
            tool "grep"
            tool "gawk"
            tool "binutils"
        }
    }

    metadata {
        "artifact_sha256" "822bd9af49c83a7b0034bcbbcc78935d0f8739712f302d95a85cb3806103d5f9"
        "changelog" "5.2.37-6: converted from build.sh to build.cbs (ADR-0305 stage 4) and published as a .cixpkg (ADR-0307). …"
    }

    build {
        cd "${src}/bash/bash-5.2.37" {
            run "./configure" {
                "--prefix=/usr"
                env "CC" = "tcc"
            }
            run "make" {
                jobs $jobs
            }
        }
    }

    install {
        cd "${src}/bash/bash-5.2.37" {
            run "make" {
                "install"
                "DESTDIR=${dest}"
            }
        }
        mkdir "${dest}/bin" chmod 0755
        symlink "/usr/bin/bash" to "${dest}/bin/sh"
        require file "${dest}/usr/bin/bash" {
            exists
            nonempty
        }
    }
}
```

`env "CC" = "tcc"` is explicit because a bare `cc` in a build environment that also holds gcc is not guaranteed to be TCC. `binutils` is declared because the finalize phase needs `strip`. The `mkdir`/`symlink` in `install` is deliberate: any image that installs `bash` also gets `/bin/sh -> /usr/bin/bash`, the one path every Linux distribution guarantees and the one glibc's `popen()` and `system()` exec literally. That `mkdir` is a single level under `${dest}`, which already exists; a new recipe should still write `parents`. `require file` asserts the package built what it claims rather than trusting `make install`'s exit status.

## Adding, updating, and installing a recipe

```
POST /v1/pkg/recipes
{"name": "hello", "content": "package \"hello\" {\n    version \"2.12.1\"\n    release 1\n ..."}
```

Publishes a new `(name, version)` recipe (ADR-0107). There is no `format` field: the content is CPDL, it must satisfy `cbs explain --json`, and the package name it declares must equal `name`. Anything else is a `400` and never becomes a stored file. A published version is immutable: publishing an existing `(name, version)` is `409 Conflict`, not an overwrite, so **fixing a mistake means publishing a new release**. The CLI equivalent is

```sh
cixctl pkg recipe add --name=hello --file=recipes/package/hello@2.12.1-1.cbs [--source=NAME]
```

A bare `pkg install`/`pkg hostbuild` (no explicit `version`) resolves to the highest published version for that name. Then:

```
POST /v1/pkg/install
{"name": "hello"}
```

starts the build — see [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs) for the full async install/poll/upgrade contract, which this guide does not repeat.

**Committing a recipe to cix-recipes from the host** is how a new revision should normally arrive ([ADR-0323](../adr/0323-every-package-can-roll-discovery-authentication-and-a-green-build.md)). `cixctl pkg recipe commit --name=hello --file=./hello@2.12.1-1.cbs --wait` runs every check a publish applies, commits the recipe as `recipes/package/hello@2.12.1-1.cbs` on the configured ref, and publishes it on this host only once the commit exists. Git therefore holds every revision a host builds. It goes to the source that owns the package, which must be a gitea source this host may write (`cixctl pkg source set NAME --write=on`, with a token that can write); `--source=NAME` names it for a package no source offers yet. Committing to the ref the hosts sync from is a deploy to every one of them, which is why no source is writable by default. A version git already has is refused by the forge, and nothing is published.

Rather than pushing every recipe individually, a host takes recipes from any number of recipe sources and pulls every tree in one call: `cixctl pkg source add site --url=https://git.example.internal/team/recipes --kind=gitea`, then `cixctl pkg sync --wait` ([ADR-0324](../adr/0324-many-recipe-sources-and-many-package-repositories.md)). A package belongs to the one source that offers it; one that two sources offer is held until `cixctl pkg source own` chooses. A sync adds versions this host does not have and never removes one (see [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs) and [ADR-0121](../adr/0121-pkg-redesign-part2-configurable-repo-and-sync.md) for the `gitea`/`github`/`gitlab` URL shapes).

**More than one cbs reads your recipe.** `cbs explain` at publish runs the engine in the host's root; a package build runs the one in the build environment; the ADR-0209 test floor runs whatever cbs the cix release recipe staged. A recipe or test fixture using a CPDL feature newer than the engine that runs it publishes cleanly and then fails at build time, so check which engine will run it before using a new spelling.

Every real build is also cached locally ([ADR-0122](../adr/0122-pkg-redesign-part3-artifact-cache-and-server.md)) as `<name>-<version>.cixpkg` — installing the same `(name, version)` into a second image, or reinstalling after deletion, reuses it instead of fetching and compiling again. `cixctl pkg cache-status` / `pkg cache-config` manage its size cap.

## The hostbuild variant

A recipe can also be built as a standalone, host-side artifact instead of merging into a container image's rootfs — used for the Linux kernel and for [self-hosted rebuilds of Cix's own control plane](building-cix.md) ([ADR-0056](../adr/0056-hostbuild-artifact-mechanism.md)). The recipe format is identical, and the build environment comes from the same place it does for an ordinary install: the recipe's build requirements, composed into a build container holding exactly the declared tools ([ADR-0304](../adr/0304-a-hostbuild-composes-its-build-environment-like-every-other-build.md)). There is no build image to prepare and none to name.

Runtime requirements are worth declaring on a hostbuild recipe ([ADR-0303](../adr/0303-a-hostbuild-carries-pkg-depends-it-does-not-resolve-it.md), issue #465). They describe what the finished artifact needs in order to *run*: a hostbuild records them on the entry and carries them forward — visible in `GET /v1/pkg` — but does not resolve or install them, since it has no image to install into. They matter because the same recipe may also be installed the ordinary way into an image, where they are what the linkage gate checks a binary's libraries against. `cix`'s own recipe is the worked example: `cixd` links `-lssl -lcrypto -larchive -lcurl`, so those packages are what it declares.

```
install {
    copy "${build}/mything" to "${dest}/mything"
}
```

The contents of `${dest}` are copied to `<data-dir>/rebuildable/artifacts/<name>/` on the host instead of being merged anywhere — a plain host directory, deliberately never container-visible, so `GET /containers/{name}/files` cannot reach it. To get the artifact *off* the box, use `cixctl pkg artifact-export NAME [--out=FILE]`, which exports a `.cixpkg` ([ADR-0201](../adr/0201-artifacts-are-retrievable-and-self-publishing.md), [ADR-0328](../adr/0328-cix-produces-no-tarball.md)). See [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) and [`building-cix.md`](building-cix.md) for the two operator runbooks built on this mechanism.
