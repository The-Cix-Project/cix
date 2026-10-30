# 0365 — `POST /v1/pkg/bootstrap` is retired

## Status

Accepted by the owner on 2026-10-30, for #628. It supersedes [ADR-0035](0035-portable-toolchain-artifact-for-pkg-bootstrap.md) and [ADR-0065](0065-pkg-bootstrap-url-fetch.md).

## Context

`POST /v1/pkg/bootstrap` seeds the build sandbox image (`cix-builder`) from a toolchain squashfs: a local file with `toolchain_path` (ADR-0035), or one the daemon fetches with `toolchain_url` and `toolchain_sha256` (ADR-0065), polled through `GET /v1/pkg/bootstrap`. Its no-body mode, a copy of the build host's own tree, went with #168.

Nothing in the build path reads what it seeds. Measured from the code on 2026-10-30:

- A build runs in an environment composed from its recipe's declared tools (ADR-0199), not in the seeded sandbox.
- The sandbox's one reader is `pkg_build_sandbox_rootfs()`, whose one caller is `pkg_toolchain_has_gcc()`, whose one caller prints a status line in `main()` after `--test-bootstrap-toolchain=`.
- The one test of the path, `test_console_pkg_bootstrap`, built from `mktoolchainimage`, is in no release gate.
- A fresh host gets its packages from the installer's seed (ADR-0229) and the public cache, not from this endpoint.

So it is an operation an operator can call that changes an image no build uses.

## Decision

1. **`POST` and `GET /v1/pkg/bootstrap` are removed**, with `cixctl pkg bootstrap` and `pkg bootstrap-status`, from the contract, the daemon, the CLI and the dashboard.
2. **What existed only for them goes too**: `pkg_bootstrap_from_toolchain()` and its seed step, the `toolchain_url` fetch and its connection kind, `--test-bootstrap-toolchain=`, `pkg_toolchain_has_gcc()`, `test_console_pkg_bootstrap` and `mktoolchainimage`.
3. **The build sandbox image stays.** `cix-builder` is an ordinary image made from its recipe, like any other (ADR-0330).

## Alternatives considered

- **Keep the endpoint and put its test in `BOOT_SELFTESTS`.** That would gate every release on a QEMU boot test of an operation nothing consumes.
- **Point it at the build-environment composition instead.** Composition already happens per build from installed packages; there is nothing for a seed to add.

## Consequences

- The contract loses two operations, one change and one read.
- A host's existing `bootstrap_toolchain.squashfs` under the package directory, if any, is no longer read; it is left in place rather than deleted at start.
