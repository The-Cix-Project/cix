# 0097 — mkbootroot never staged a CA certificate bundle for host-side HTTPS fetches

## Status

Accepted

## Context

`PKG_CURL_BIN` (`/usr/bin/curl` on the control-plane's own root, not a container's) is what `kanxeod` itself shells out to for every host-side `pkg_source` fetch — the fetch that runs before any build container exists (ADR-0034: a real minimal install has no other way onto the box). `curl.recipe`'s `./configure` auto-detects a default CA bundle path at *build* time from whatever machine actually builds it (Debian convention: `/etc/ssl/certs/ca-certificates.crt`) and compiles that path in as curl's own default — but `mkbootroot.c` (the tool that assembles the control-plane squashfs `kanxeod` itself runs from) has never actually staged a real bundle at that path. `openssl.cnf` gets exactly this treatment already (staged into `usr/lib/ssl/`, added after a real `pki ca bootstrap` failure, see the comment at that call site) — the CA bundle needed the same treatment and never got it.

Found live via ADR-0096's own new fetch-diagnostic capture, immediately after fixing an unrelated DNS-forwarding gap (`dns-1`/`dns-2` never forwarding the operator's real `home.arpa` zone upstream) during a `kanxeo` hostbuild redeploy this session: once DNS resolution actually worked, the same fetch failed a second, different way — `curl: (77) error setting certificate file: /etc/ssl/certs/ca-certificates.crt` — a bare `curl exit status 1` before ADR-0096 would have hidden this behind an identical, undiagnosable message to the DNS failure.

## Decision

`mkbootroot.c` now stages a real `/etc/ssl/certs/ca-certificates.crt` into the assembled control-plane root, immediately after the existing `openssl.cnf` staging block, same mechanism (`test_image_fixture_copy_file()`, which follows symlinks transparently — a distro's usual `ca-certificates -> /usr/share/ca-certificates/... ` chain lands as one plain file on the target, no symlink chain needed there). Sourced from wherever `mkbootroot` itself runs, exactly like every other host-tool binary/config this file already stages when no `host_tools_dir` override is given.

## Consequences

- Every host-side HTTPS `pkg_source` fetch (not just `kanxeo.recipe`'s own self-build source) now has a real CA bundle to verify against — a previously-universal, previously-silent gap (nothing in this codebase had ever staged one before), not specific to this one fetch.
- Live-verified directly: the exact `kanxeo` v1.7.1 hostbuild that surfaced this (git.home.arpa, HTTPS, token-authenticated) completed successfully — fetch, build, and install all succeeded — once this fix was deployed (via the standard local-mkbootroot + `system/update` loop, not the git-fetch-dependent hostbuild path, since that path was what needed fixing).
- A related, separate gap surfaced immediately after this one and is *not* fixed here: the server-side, kanxeod-triggered mkbootroot re-assembly that normally follows a successful `kanxeo` hostbuild (ADR-0057) itself failed with `sha256sum: No such file or directory` sourcing from the `kanxeo-hosttools` image's own rootfs — that image was never given a real `sha256sum` binary (`coreutils` is only installed onto the `kanxeo-builder` image, not `kanxeo-hosttools`). Tracked separately, not conflated with this fix.
