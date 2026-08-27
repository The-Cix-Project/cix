# 0204 — The daemon fetches its own update image

## Status

Accepted. Extends [ADR-0031](0031-host-and-package-update-mechanism.md) and [ADR-0095](0095-update-one-sided-footgun.md); does not reverse either.

## Context

Issue #141. `POST /v1/system/update` took `image_path` — a path to a squashfs **already on the box**. That framing was inherited from a development machine, where putting a file somewhere is trivial, and it quietly made the endpoint unusable on the thing it exists for.

A real installed Cix host has, by charter, no SSH and no general shell. There is no `scp` target. And the API's own request cap is 1 MiB, so a ~10 MB control-plane image cannot be sent as a request body either. Every route by which a file could arrive at `image_path` was therefore closed on exactly the machines that needed updating.

The result: **the one endpoint whose entire job is updating the control plane could not be used to update the control plane.** Every deploy in this project's history routed around it — an ISO reinstall, a hostbuild that happened to leave an artifact on the same host, or an ad-hoc LAN-mirror trick. Each of those worked, which is why the gap survived so long: the workaround was always available, so the defect never presented as a blocker, only as friction. This ADR exists partly to record that pattern, because it is the same one ADR-0202 and ADR-0203 each found in a different subsystem — a capability that looks present, is routed around in practice, and is never actually exercised.

The daemon already had the capability required. `pkg bootstrap --toolchain-url=` fetches a 1.4 GB tarball over HTTP and verifies it. Update simply never offered it.

## Decision

`POST /v1/system/update` accepts `image_url` as an alternative to `image_path`. The daemon fetches that URL itself, writes the result to its own staging path under the rebuildable directory, and from there **the existing `image_path` logic runs unchanged**.

That last point is the design constraint, not an implementation detail: the fetch rewrites its result into the request the rest of the handler already understands, so there is one staging and validation path, not two. A second path would be a parallel implementation of slot-writing — the highest-consequence code in the system — and would eventually diverge from the first.

**`image_sha256` is required with `image_url`, not optional.** This call writes a boot slot. An image that is truncated, corrupted in transit, or simply the wrong file produces no error at fetch time and no error at write time; it produces a host that fails to boot, discovered at the next reboot, which is the worst possible moment and the hardest state to recover from remotely. Verification is cheap, the failure it prevents is not, and an optional checksum is one an operator under time pressure will omit. `image_path` keeps its existing on-disk magic check and gains no checksum requirement — the operator who placed that file already had the access needed to verify it.

## Consequences

An installed host can now be updated through its own API, with no reinstall and no out-of-band file transfer: fetch, verify, write the inactive slot, reboot. This is the intended deploy path from here.

The daemon resolves the URL itself, which couples updating to the host's own resolver working. On a host whose `/system/resolv` is unset or broken — including a fresh install hitting issue #138 — only a literal IP will work. This is a real constraint and is documented at the endpoint rather than hidden, because the failure (`CURLE_COULDNT_RESOLVE_HOST` while trying to repair a host) is otherwise confusing precisely when an operator is least able to investigate.

This does not make the endpoint self-updating in the general sense: the fix ships *in* a build, so a host running an older build still needs one delivery by another means before it can use this. That one-time cost is unavoidable for any change of this kind and is not a reason to defer it.
