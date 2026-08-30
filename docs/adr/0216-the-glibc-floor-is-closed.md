# 0216 — The glibc floor is closed

## Status

Accepted. Supersedes the "glibc floor" caveat in
[ADR-0209](0209-derived-images-and-one-container-mechanism.md), whose own
closing section said plainly: *"What this does not fix. The `glibc`/`ld.so`
floor above."* This fixes it.

## Context

`pkg_seed_image_baseline()` copied four files into every image by absolute
path, off whatever machine happened to be running `cixd`:

```
/lib64/ld-linux-x86-64.so.2                     (twice: lib64/ and lib/x86_64-linux-gnu/)
/lib/x86_64-linux-gnu/libc.so.6
/lib/x86_64-linux-gnu/libm.so.6
/lib/x86_64-linux-gnu/libnss_files.so.2
```

Everything else that function does — device nodes, `/run`, a written
`/etc/nsswitch.conf` — it *creates*. These four it *borrowed*. They were the
last bytes in the system this project did not build, and on the first real
host they were Debian's, arriving via `mkbootroot` from a Debian build
machine. ADR-0209 stated this rather than burying it, and named the
condition for closing it: build glibc from source on a Cix host.

That condition is met. `glibc` is an ordinary recipe, built on 192.168.15.95
with this platform's own gcc against its own kernel headers, published to the
artifact cache, and its package carries all four of those paths — both
spellings of the loader included.

## The circle, and why it is not a one-line change

Removing the copies was tried and measured before this decision was taken.
`image_create()` and `install_mutate()` were unaffected. Every *build* died:

```
container __pkgbuild-1 exited abnormally:
  child: execve(/usr/bin/bash): No such file or directory
```

That `ENOENT` is not a missing bash — bash was staged, as a declared tool. It
is a missing **loader**. A composed build environment (ADR-0199, issue #168)
holds exactly its declared tools' manifest files, and no tool's manifest
carries libc; the loader in it came only from this same copy. And glibc
itself is *built by* a composed environment.

Two other approaches were tried and rejected on measurement, recorded here so
they are not re-attempted:

- **Resolve glibc as an implicit dependency of every install.** Works, but
  couples the entire package system to a buildable glibc: `test_images` has
  no toolchain and only ever installs a package designed to fail at checksum
  verification, and every install there began failing 400. Too heavy a
  coupling for what it buys.
- **Source the build environment's runtime from its tools' own image.** Clean
  in principle, but those images only had a libc because of this same copy —
  the circle again, one level out.

## Decision

**The circle is broken the way every real toolchain bootstrap breaks one:
with a checksummed seed artifact, used once, after which the system feeds
itself.** Both halves of that machinery already existed.

1. **A cache-hit install needs no build environment at all.** So glibc can be
   *installed* into any image from its published artifact without being
   *built* there. `pkg_artifact_sha256` in the recipe approves those exact
   bytes — produced and published by a real Cix host, per the Build
   Provenance Mandate.

2. **Every composed build environment gets glibc implicitly**
   (`PKG_BASE_LIBC`, resolved in `buildenv_resolve_tools()` through the same
   `buildenv_add_tool()` path as any declared tool). Declared tools resolve
   first and dedup is by name, so a recipe keeps the right to pin
   `glibc@2.44-6`.

   Named once in the mechanism rather than written into sixty recipes: every
   binary in every package links against libc, so it is a property of an
   environment being usable, not of any one recipe. Sixty declarations would
   be sixty chances to omit it, each failing only at exec time with a bare
   `ENOENT` that names nothing. This is not in tension with ADR-0209's
   "sixty recipes must declare their build tools" — that argument is about
   what distinguishes one recipe from another. A universal requirement
   distinguishes nothing.

   An environment's identity is the sorted tool-set hash, so every
   environment composed under the old rule retires by construction and the
   next build re-composes with glibc inside. The migration is automatic and
   self-cleaning; no ADR-0155-style staleness is possible, because the hash
   honestly reflects the content change.

3. **Composition is verified, not trusted.** Tools are copied in sorted name
   order, so a tool sorting after `glibc` that ships a path glibc owns would
   silently win it — and glibc's objects share a private, version-locked
   interface (`GLIBC_PRIVATE`): halves of two C libraries cannot exec.
   `buildenv_verify_libc_intact()` re-reads the loader and `libc.so.6` before
   the environment is sealed and refuses one whose copies are not
   byte-identical to what the glibc package installed.

   This is the discipline `mkbootroot` earned days earlier, for exactly this
   failure: the build host's `libm`/`libpthread`/`libresolv` landed on top of
   the platform's own and panicked a real machine at boot, twice, while
   assembly reported success both times. Copying files is not the same as
   producing something that runs.

4. **The four copies are deleted.** A freshly created image has no runtime,
   and that is correct rather than a gap: `POST /v1/containers` refuses an
   image with no dynamic loader and names what to install, instead of letting
   it surface later as a container exiting 127 with `execve failed`. That
   guard checks for the *loader*, not for a package named glibc — the
   requirement is a working runtime; which package provides it belongs to the
   catalogue, not the daemon.

5. **An image that runs containers declares its C library.** `glibc` is first
   in `image_packages=` for every image on the first real host.

## Consequences

**Nothing in any image, environment, or boot root is copied from the build
host any more.** The invariant is held by three verifying gates rather than
by care: `mkbootroot`'s `verify_platform_libs_intact()`, this ADR's
`buildenv_verify_libc_intact()`, and the container-create loader check.

An image's C library is now a manifest entry with a version and an auditable
origin, upgradeable like anything else — `pkg update-all` can move a host's
libc, which was never previously expressible.

**A box that cannot supply its own C library now says so** and refuses,
rather than quietly shipping somebody else's.

**Cost.** Composed environments carry glibc's 1443 files; each is created
once per tool set and snapshotted thereafter. Every existing `__buildenv-*`
retires at deploy and the first build after re-composes.

**What this does not fix.** The test floor's `libc-dev-2.36-3` is still
Debian-derived content from before our glibc existed — headers and link
objects, not a runtime, and not shipped in any image. It should eventually be
cut from our own glibc; tracked separately.

## Verification

- `test_pkg` stages the collision deliberately: a `zzlibc` package shipping
  its own `libc.so.6`, declared as a build tool, sorting after `glibc`,
  isolated in its own image so it cannot corrupt `base`. Composition must
  refuse. **Measured with the gate removed, which is the argument for having
  it:** the build still fails, but as `build failed (exit 127)` — the opaque
  broken-loader exit that names no file, package, or cause.
- `test_images` asserts a freshly created image has **no** C runtime, the
  exact inverse of what it asserted before. Proven to catch the regression by
  restoring the copies from the previous commit: it fails with *"something is
  copying one off the build host again"*.
- Every image on 192.168.15.95 had glibc installed as a cache hit, needing no
  build environment, and a container created from the resulting image version
  ran a real dynamically-linked binary (`dnsmasq --version`) to a clean exit 0.
- Full suite: `test_pkg`, `test_pkg_cache`, `test_images`, `test_daemon`,
  `test_container_restart`, `test_rolling_restart`, `test_pkg_build_log`,
  `test_pkg_concurrent_stress`, `test_console_exec`, `test_web`.
