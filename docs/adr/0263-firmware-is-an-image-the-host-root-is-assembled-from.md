# 0263 — Firmware is an image the host root is assembled from

## Status

Accepted

Issue [#30](https://git.home.arpa/itdlabs/cix/issues/30). Extends [ADR-0029](0029-gpu-kernel-driver-firmware-and-kfd.md);
mirrors [ADR-0078](0078-from-source-host-tools-bootstrap.md)'s convention.

## Context

A driver asks for firmware by calling `request_firmware()`, and the kernel answers it by reading a
file out of the root filesystem it booted — during device probe, before any container exists. There
is no point in the boot sequence at which a firmware blob living inside a container could be
reached. ADR-0029 established this when a GPU was the only device on this platform that wanted a
blob, and it staged `lib/firmware/amdgpu` into the control-plane root for exactly that reason.

The access-point work needs a second blob. An RTL8822BU adapter is bound by `rtw88`, which declares
one firmware file, `rtw88/rtw8822b_fw.bin`; and `cfg80211` loads `regulatory.db` to know which
channels and powers are legal in a given domain. Neither is optional in practice — without the
first the driver probes and brings up no interface, and without the second an AP is confined to the
most restrictive world domain.

Two facts made the existing arrangement unable to deliver either.

**The staging had no caller.** `mkbootroot` accepted a firmware directory and copied it in, and the
daemon's only invocation passed `""`, with a comment explaining that a control-plane-only rebuild
needs no GPU firmware re-staging. That reasoning does not hold: `mkbootroot` assembles a *fresh*
root on every run, so whatever it is not given is simply absent from the result. The capability
existed and no code path reached it.

**The shape did not generalise.** The argument was an `amdgpu` directory specifically, flat-copied
into a hardcoded `lib/firmware/amdgpu`. A second driver would have meant a second hardcoded
subdirectory beside the first, and a third would have meant a third — one mechanism per device, all
doing the same job.

## Decision

**Device firmware is an ordinary Cix image named `cix-firmware`, and the control-plane root is
assembled from its `lib/firmware`.**

Three parts, each the smallest form of itself:

**`mkbootroot` takes a firmware ROOT, not a driver directory.** Its contents mirror `/lib/firmware`
exactly, and it is copied in recursively. So `amdgpu/vega10_smc.bin`, `rtw88/rtw8822b_fw.bin` and a
bare `regulatory.db` all land where `request_firmware()` looks, with no knowledge in `mkbootroot`
of which drivers exist. amdgpu keeps working by being a subdirectory of the root rather than the
whole of it.

**The daemon resolves that root from a well-known image**, exactly as it already resolves
`cix-hosttools` (ADR-0078): current version via `image_current_version()`, then that version's
rootfs, then its own `lib/firmware` — which is where firmware packages install, so no rearranging
is needed. Purely additive: a box that never built the image gets `""` and today's behaviour, never
a hard failure.

**The image is built with ordinary `pkg install --image=cix-firmware` calls.** No special-cased
creation path, no new concept — it is an image whose packages happen to contain no executables.

## Consequences

Adding firmware for a new device is now a recipe and a line in the image manifest, with no change
to `mkbootroot`, the daemon, or this decision. That is the property worth having; the alternative
grew a hardcoded branch per device.

**A blob that cannot be built from source enters the platform here, and that is a real exception
worth naming.** `rtw88-firmware` is a signed image executed by the adapter's own processor; nobody
can build it from source, including its vendor's customers. The Build Provenance Mandate is about
Cix's own toolchain never being bypassed for things Cix compiles, and this compiles nothing. It is
nevertheless pinned harder than anything Cix builds: one linux-firmware commit, a checksum over
those exact bytes, and a `pkg_build()` that asserts the 8822B header before installing. The owner
approved shipping it explicitly.

`wireless-regdb` is the opposite case and is treated as such: it is fully buildable, so the recipe
regenerates `regulatory.db` from `db.txt` and **refuses to install unless the result is
byte-identical to upstream's**. That is what makes shipping upstream's `regulatory.db.p7s`
signature alongside this platform's own build correct — the kernel verifies a signature over bytes
this platform produced and proved identical, rather than trusting a file it was handed.

**Assembly says what it staged.** Firmware staging was entirely silent — the only success output
was `wrote <path>` — so the first evidence a blob had landed was a device working, or not, after a
reboot. On a shell-less host that is an expensive place to learn it, and this project's standing
rule is to verify an image before booting it. `mkbootroot` now prints `staged N firmware file(s)
from <root>`. A count and not a bare line, because the failure worth catching is a firmware root
that exists and is empty — an image created but never installed into copies nothing, succeeds, and
is indistinguishable from a correct run in any output that does not count.

**The old comment encoded a false premise, and removing it is part of the fix.** "A control-plane
rebuild needs no firmware re-staging" was not a description of a working system — it was the reason
firmware never reached a single assembled root. It is replaced by a comment saying what is actually
true of `mkbootroot`: it builds a fresh root, so every input it needs must be given every time.

## Alternatives considered

**Ship firmware inside the `cix` package.** It would work, and it would mean every host downloads
every blob for every device it does not have, with no way to opt out and no way to add one without
a platform release. Firmware is per-site hardware, not platform code.

**A firmware directory on disk, outside the image system.** Simpler to wire and it discards
everything images already provide: versioning, checksummed artifacts, a manifest saying what is in
it, and the ability to rebuild it identically on another host. It would also be the only
platform input assembled from an unversioned path.

**Load firmware from a container at runtime.** Not possible, and worth recording so it is not
re-proposed: `request_firmware()` runs in the kernel against the host root during probe, long
before container infrastructure exists.
