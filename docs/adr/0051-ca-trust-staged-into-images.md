# 0051 — CA trust chain staged into every image at creation time

## Status

Accepted

## Context

Direct follow-up in the same identity-focused conversation as ADR-0049/0050: "we also make sure that our root and intermediate are installed on any of our images right? so that we are trusted by our containers." Investigated before writing any code: `pkg_seed_image_baseline()` (`daemon/src/pkg.c:829`, called once from `image_create()`) seeds a fixed, hardcoded list -- runtime libs, device nodes, `run/` -- and has never touched `/etc/ssl`. Confirmed directly: **no image this platform has ever built has shipped any CA trust at all, not even public roots.** Any TLS client running inside a container (`curl`, `openssl`, ...) fails certificate verification outright today unless the caller passes `-k`/`--insecure` or an explicit `--cacert`.

Two real design questions, settled before implementation:

1. **Where does the bundle come from, and who assembles it?** `pki.c` already owns every fact needed (the root/intermediate cert paths, whether an intermediate exists) and already has one file-assembly precedent for a different purpose (`pki_cert_deliver()`'s leaf-plus-intermediate `fullchain.pem`). Duplicating that path-knowledge into `pkg.c` (a different module, a different job) would violate One Source of Truth for where PKI's own on-disk layout lives.
2. **When does staging happen, and what if PKI isn't bootstrapped yet?** `pkg_seed_image_baseline()` already runs exactly once, at image-creation time -- the natural fit, since the bundle is identical across every container built from that image and doesn't change per-container the way a leaf does. A fresh install's very first image is very likely to be created before PKI is ever bootstrapped at all; that must not fail image creation.

## Decision

**New `pki_write_trust_bundle_file(dest_path)`** (`daemon/src/pki.c`/`.h`): writes the current root cert, plus the intermediate's if one is bootstrapped, concatenated to `dest_path`. Order (root-then-intermediate) is arbitrary and doesn't matter here the way `pki_cert_deliver()`'s own leaf-first `fullchain.pem` order does -- a pure trust-anchor bundle loaded via `-CAfile`/a system trust store builds its own chain from whatever certs are present, regardless of file order. `PKI_ERR_NOT_BOOTSTRAPPED` if no root exists yet -- the caller's job to treat that as "nothing to stage yet," not this function's.

**`pkg_seed_image_baseline()` gains a fourth block**, writing `etc/ssl/certs/thinc-ca-bundle.pem` into the new image's rootfs via the function above. Same idempotent-skip-if-already-present shape the runtime-libs loop already uses. `PKI_ERR_NOT_BOOTSTRAPPED` is tolerated (skip, not fatal -- the common state on a fresh install's first-ever image); any other error (a real write failure) is fatal, matching the dev-nodes/`run/` blocks' own "real I/O problem, not a tolerable gap" stance rather than the runtime-libs loop's "missing source is fine" one.

**No retroactive propagation to already-existing images.** This seeding step only runs at `image_create()` time -- an image created before this feature existed, or before PKI was ever bootstrapped, needs one manual reseed (this project's own `base`/`router`/`pkgbuild` images, done once during this phase's own verification) to pick up the bundle. A later CA rotation (`POST /pki/reset`, ADR-0049) does **not** retroactively update any already-staged image's bundle either -- the real image-level analog to the "redeliver to live containers" problem CA reset already solves for individual leaves, deliberately not solved here (no consumer has asked for it yet; every currently-live container's own trust relationship is with whatever CA was current when its image was built, which remains internally self-consistent even after a host-side rotation until that image is next reseeded).

## Consequences

- Every container built from an image created (or reseeded) after this phase can verify a TLS cert genuinely signed by this platform's own PKI without `-k`/`--insecure` -- a real, previously-missing capability, not a convenience layer over something that already worked another way.
- A CA reset (ADR-0049) silently diverges from every already-staged image's own bundle the instant it completes -- a container built on such an image, talking to a service holding a cert reissued under the new chain, will fail verification until that image is reseeded. Not designed or built yet: any mechanism to detect or propagate this drift automatically.
- `thinc-ca-bundle.pem` is a fixed, chosen filename -- not a `ca-certificates`-package-managed, `update-ca-certificates`-style merged system bundle (no such package exists in this platform's own recipe set). A container's own TLS client must be pointed at this specific file explicitly (`--cacert=/etc/ssl/certs/thinc-ca-bundle.pem`, or an application-level trust-store config) -- it is not automatically consulted as `/etc/ssl/certs/ca-certificates.crt` or any other convention a stock distro's TLS stack might assume by default.
