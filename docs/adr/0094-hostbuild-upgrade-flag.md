# 0094 — pkg hostbuild had no upgrade/force path, a bare permanent 409

## Status

Accepted

## Context

`pkg_hostbuild_start()` (`daemon/src/pkg.c`) rejected any hostbuild request for a `name` already in `PKG_STATE_INSTALLED` with `PKG_ERR_DUPLICATE` (409), unconditionally -- unlike `pkg_install_start()`, which has always accepted an `upgrade` flag letting a caller explicitly re-run the build when the recipe's own version has moved on. A hostbuild recipe whose upstream source can change under the same tracked name (`kanxeo.recipe`'s own self-build source snapshot, fetched fresh from a git tag on every real rebuild) had no way to trigger a second build at all once the first one succeeded -- a real, permanent dead end, not a one-time nuisance.

## Decision

Give `pkg_hostbuild_start()` the same `upgrade` parameter `pkg_install_start()` already has, with identical semantics: still 409 if `upgrade` is false, or if it's true but the recipe's current `pkg_version=` matches what's already installed (a genuinely unchanged version has nothing to rebuild); proceeds otherwise. Wired through `POST /pkg/hostbuild`'s JSON body (`"upgrade": true`) and `kanxeoctl pkg hostbuild NAME --build-image=IMAGE --upgrade`, matching `/pkg/install`'s own field/flag naming exactly.

## Consequences

- A hostbuild recipe can now be rebuilt from fresh source under an unchanged name, the same way an ordinary package can be upgraded -- no more permanent 409 once the first build succeeds.
- No behavior change for the default (`upgrade` omitted/false) case -- existing callers see identical behavior to before.
- Local `test_pkg` regression suite re-run clean; the hostbuild fixture there uses the default (non-upgrade) path, unaffected.
