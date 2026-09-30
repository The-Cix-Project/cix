# 0319 — A path in an image belongs to one installed package

## Status

Accepted by the owner on 2026-09-30, who chose Debian's rule on [#553](https://git.home.arpa/itdlabs/cix/issues/553). The replaces declaration it allows for depends on cix-build-system#275.

## Context

Until this decision, two packages installed into one image could both install the same path. The last install decided what was on disk, and nothing recorded whose bytes those were. [#175](https://git.home.arpa/itdlabs/cix/issues/175) had already made uninstall keep a path another package still claimed, and its comment called shared paths "normal": glibc and linux-headers were supposed to share usr/include.

Two consumers read a shared path as if it were the other package's:

- **Build-environment composition** ([ADR-0199](0199-recipes-declare-their-build-tools.md)) copies each declared tool's recorded paths out of its image. coreutils 9.11-8 bundled `usr/lib/libcap.so.2` as a link to its own `libcap.so.2.66`. The libcap package ships the same path as a link to `libcap.so.2.78`. In jumpbox the path held coreutils' link: 47128 bytes, identical to `.2.66`, while `.2.78` is 59656. An environment composed from libcap got that link without its target. So when coreutils 9.11-9 stopped bundling libcap and depended on it instead, every build declaring coreutils failed with `libcap.so.2: cannot open shared object file` ([#510](https://git.home.arpa/itdlabs/cix/issues/510), measured on 192.168.15.95).
- **Uninstall** keeps a path another package claims, but not the files that path's *content* depends on. Upgrading coreutils away from its bundled copy would keep the link and delete `.2.66`, leaving the link dangling in the image.

Measured across every installed package on 192.168.15.95 on the same day, 80 paths were shared, by five pairs: coreutils with libcap and with openssl (bundled libraries), mtr with glibc (a copy of `libresolv.so.2`), binutils-dev with glibc (libiberty's `fnmatch.h`, `getopt.h`, `obstack.h`), and libuuid, libblkid and login with one another (util-linux's message catalogue and `terminal-colors.d.5`). glibc and linux-headers shared nothing. Every shared path was a packaging bug.

## Decision

**An install is refused when any regular file or symlink it would stage is owned by another installed package in the same image.** The error names the installing package, the path, and the package that owns it. The check runs in `path_owner_gate()` before a byte is staged, beside the undeclared-link gate. So a refused install leaves nothing in the image, and the message names the package at fault, not the image.

- **A package's own previous version is not another package.** An ordinary upgrade is unaffected, including one that keeps every path it had.
- **Giving a path up is always allowed.**
- **Moving a path needs a declaration.** Under Debian's `Replaces:`, the receiving package names the package whose files it takes over, and only that takeover is allowed. CPDL says it with a package-level `replaces { package "NAME" }` from cbs v0.1.99 (cix-build-system#275). When this ADR was accepted it could not: both spellings tried were refused at parse on cbs v0.1.98, and a move took two published revisions. The taken paths change owner in the installed set, so removing the giver later leaves them.
- **There is no allow-list.** A collision is a recipe bug, and each of the five above was fixed in its recipe before this shipped: coreutils 9.11-9, mtr 0.96-11, binutils-dev 2.42-8, and libuuid 2.42.2-7, libblkid 2.42.2-9 and login 2.42.2-6 with a new util-linux-common package that owns the shared util-linux data. That follows Debian's shape for the same tree, and [ADR-0306](0306-a-package-keeps-its-documentation-and-its-licence.md)'s rule that files are not deleted just because nothing reads them.

The ownership index is one sorted array of every path the image's other packages claim, searched once per staged file, and uninstall uses the same index. A linear scan would compare each file of a large package, such as gcc into cix-builder, against every file in the image, on the daemon's one event loop.

## Alternatives considered

- **Keep last-install-wins and record per path whose bytes are present**, so composition and uninstall read the right owner. It keeps a state every consumer must reason about, and no shared path found was legitimate. Rejected by the owner.
- **Compose build environments from the package artifact instead of the image.** It would fix composition only, and ADR-0199 rejected it already: the cache is prunable, and an environment must not depend on what has aged out.

## Consequences

- A recipe that bundles another package's file fails at install with a message naming both, not later in someone else's build.
- A library split upstream (files moving from A to a new B that A depends on) cannot be packaged in one step until cix-build-system#275 lands, because B installs first.
- Images that predate the rule may still hold shared paths. Uninstall keeps those, as #175 did. Nothing new creates one.
- **hostbuilds are not covered**: they harvest into their own artifact directory (`ARTIFACTS_DIR/<name>`), not an image. isotools, which deliberately bundles the libraries it runs with, is one.
