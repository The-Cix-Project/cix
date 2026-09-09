# 0266 — The module tools are an image; the module tree is the kernel's own artifact

## Status

Accepted

Issue [#347](https://git.home.arpa/itdlabs/cix/issues/347). Follows [ADR-0263](0263-firmware-is-an-image-the-host-root-is-assembled-from.md)
exactly, for the third of `mkbootroot`'s four optional staging arguments; mirrors
[ADR-0078](0078-from-source-host-tools-bootstrap.md)'s convention.

## Context

No kernel module could load on any Cix host this platform has ever assembled.

`GET /v1/system/kmod/e1000e` on 192.168.15.95 answered `{"error":"no such module (not
built/available)"}`, and so did `tg3`, `usb-storage` and `ehci-hcd` — every module
`load_boot_modules()` tries at boot. Not "failed to load": *not present*. The root carried no
module tree to find them in and no `modprobe` to load them with.

Nothing about this was half-built. `recipes/package/kernel`'s own `pkg_install()` has always run
`modules_install` followed by `depmod -b`, so the kernel artifact carries a complete,
dependency-indexed tree — measured on the box, `kernel@7.2.3-8` holds 21 `.ko` files alongside
`modules.dep`, `modules.alias` and `modules.symbols`. `image/src/mkbootroot.c` has always accepted
a `modules_dir` and a `kmod_bin_dir` and staged both correctly, with a test proving it.
`recipes/package/kmod` has always built the tools. `daemon/src/kmod.c` has always known how to
invoke them.

The one thing missing was the call. `spawn_cix_bootroot_assembly()` passed a literal `""` for both
arguments, with a comment explaining that a control-plane-only rebuild touches no kernel module
tree.

**That is ADR-0263's failure, verbatim, on the next argument along.** The same wrong reasoning —
`mkbootroot` assembles a *fresh* root on every run, so whatever it is not given is simply absent
from the result — had already been found and corrected one argument to the left, for firmware,
weeks earlier. It survived on `argv[7]` and `argv[8]` because nothing looked at them.

Two of `mkbootroot`'s four optional arguments shipped complete, tested, and never called. That is a
pattern rather than an accident, and neither instance was reachable by a runtime test: the code was
correct, and the call site was where the feature was silently switched off.

Why it went unnoticed for so long: 192.168.15.95 is a VM whose NIC is `virtio_net`, built in. Every
driver that box needs to boot and be reached on is `=y`, so nothing ever asked for a module. **On
bare metal it is fatal.** A machine whose NIC driver is a module has no network, and the
control-plane root is deliberately shell-less, so there is no way in to fix it. The host is
unreachable, not degraded.

## Decision

**The module tools are an ordinary Cix image named `cix-kmod`, resolved exactly as `cix-firmware`
is. The module tree is not an image: it is the kernel package's own artifact directory.**

`spawn_cix_bootroot_assembly()` resolves both and passes them:

- `argv[7]` — `ARTIFACTS_DIR/kernel/lib/modules`, a sibling of the `cix` artifact directory the
  function already receives.
- `argv[8]` — `KMOD_IMAGE`'s current version's `usr/bin`, via `image_current_version()` and
  `image_version_rootfs_path()`, the same two calls `FIRMWARE_IMAGE` uses.

Both keep the `""` fallback, so a box that has neither is affected exactly as much as it is today,
and both log which branch they took — firmware gained that line because "did this assembly stage
firmware?" turned out to be unanswerable after the fact, and the same question will be asked of
modules.

### Why the tools are an image and the tree is not

They are staged by different copies, and the difference decides it.

`kmod_bin_dir` is staged by the **flat** copy: every file in the directory, into `usr/bin`. So the
directory's whole contents become the control-plane root's contents, and the only safe thing to
point it at is a directory that holds exactly what is wanted. A one-package `cix-kmod` image's
`usr/bin` holds precisely the seven files `kmod` installs — the real binary and its six symlinks,
which the flat copy dereferences with `stat()` rather than `lstat()` into six real working copies,
`kmod` dispatching on `argv[0]`.

Adding `kmod` to `cix-hosttools` instead would have been the smaller diff and is wrong: hosttools'
`usr/bin` carries `curl`, `tar`, `perl` and `bash` among others, and the flat copy would put all of
them — including a shell — into the control-plane root. The root's shell-lessness is a deliberate
property of this platform, and hosttools is staged by named absolute path precisely so that it can
hold more than the root does.

`modules_dir` is staged by a **recursive** copy of one named tree, so it has no such constraint. And
the module tree is not a package set at all — it is one build output of one package, which the
kernel artifact already is. Wrapping it in an image would add a version indirection between the
`bzImage` and the modules built beside it, which is the one relationship that must not drift.

### On matching the modules to the kernel

Nothing here checks that the staged tree matches the booting `bzImage`, because nothing needs to.
Modules are namespaced by kernel release — `lib/modules/<release>/` — and `modprobe` selects by
`uname -r`. A mismatched release degrades to "module not found", which is exactly today's
behaviour; a matching release built from a different config is refused outright with `invalid
module format`. Neither failure is silent, and neither is worse than the state this ADR replaces.

It remains why a deploy should pass `kernel_path` from the same artifact the assembly staged from,
which `POST /v1/system/update`'s paired `image_path`/`kernel_path` already makes natural.

## Alternatives considered

**Load modules from the daemon with `finit_module(2)` and drop `modprobe`.** Architecturally the
purest option, and consistent with the networking plane's refusal to shell out to `ip`. Rejected
for now: `modprobe`'s value is dependency resolution from `modules.dep`, alias matching and
decompression, none of which is trivial, and `kmod` is already packaged and built by Cix. It stays
on the table as part of #351/#352's general question about what the control-plane root shells out
to.

**Build the modules into the kernel and keep `=y` everywhere.** This is the status quo, and it is
what hid the bug. It also cannot scale: every driver any Cix host might ever need would have to be
in every Cix kernel, and the owner's direction is the opposite — modularise every driver that is an
*implementation specific* rather than a *platform dependency*.

**A gate that asserts the assembled root contains a module tree.** Weaker than what was chosen. The
defect is at the call site and is visible there: `test_bootroot_args` fails the build if any of
`mkbootroot`'s four optional arguments is assigned a string literal in
`spawn_cix_bootroot_assembly()`. That is the check that would have caught both this and ADR-0263.

## Consequences

`POST /v1/system/kmod/<name>` becomes usable on an installed host for the first time, and
`load_boot_modules()` gets its first chance to succeed. A bare-metal host with a modular NIC driver
becomes reachable.

Two more images an operator may build, both optional, both ordinary: `pkg install --image=cix-kmod
kmod`. A box without `cix-kmod` assembles exactly as it does today.

`test_bootroot_args` is in `SELFTESTS`, so this class of defect now fails a release rather than a
site.

This makes the driver tiering work possible — `=m` is not a decision anyone could take while no
module could load.

#350 proposes replacing the whole hand-maintained staging table with an image manifest, which would
subsume both of these arguments and this ADR's mechanism with them. `cix-kmod` is one package and
costs nothing to retire when that lands.
