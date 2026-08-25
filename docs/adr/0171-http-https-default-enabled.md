# 0171 — `https_enabled` defaults to `true` on a fresh install, matching real deployed behavior

## Status

Accepted

## Context

Raised directly by the user, correcting a wrong assumption made this session: 192.168.15.95 (the real, live deployed Cix install) serves on both port 80 (HTTP) and port 443 (HTTPS), confirmed directly via `GET /v1/system/daemon-config` (`http_enabled=true https_enabled=true https_port=443`) after an earlier connectivity check mistakenly polled ports 8085/8443 (a different, unrelated project's own boilerplate port numbers, confused in with this one) and wrongly concluded the box was down.

`daemon_config.c`'s own `DEFAULT_PORT` (`main.c`) and `DEFAULT_HTTPS_PORT` (`daemon_config.c`) were already `80`/`443` — the port *numbers* already matched. The actual gap: `daemon_config_init()` set `g_https_enabled = 0` on a fresh install (`g_http_enabled = 1`), meaning a brand-new install starts HTTP-only, with HTTPS an explicit operator opt-in (`PUT /v1/system/daemon-config {"https_enabled": true}`) rather than the out-of-the-box default — not matching 192.168.15.95's own real, already-configured state.

## Decision

`daemon_config_init()` now defaults `g_https_enabled = 1`, matching `g_http_enabled`'s existing default — a fresh install starts with **both** listeners wanted, on ports 80/443.

This is safe by construction, not merely convenient: `create_tls_ctx()` (`main.c`) already refuses gracefully with a clear log message when no PKI host certificate exists yet (`PKI_ERR_NOT_BOOTSTRAPPED`-equivalent), and the boot-time listener-start call already treats a failed HTTPS start as non-fatal ("`https_enabled but could not start the HTTPS listener -- continuing without it`"), the same "wanted but not yet available" posture ADR-0163 (persisted state) and ADR-0169 (swap placement) already established elsewhere in this codebase. A genuinely fresh, pre-PKI-bootstrap install's first boot simply has HTTPS silently not come up; once `POST /pki/ca` is run, `PUT /v1/system/daemon-config {"https_enabled": true}` (even though it's already the default) brings it up live immediately, with no reboot — `handle_daemon_config_put()` checks whether the listener is actually *running* (`g_https_listener_conn.fd < 0`), not just whether the persisted flag changed value.

**A real regression found during verification, fixed in the same change**: `handle_daemon_config_put()`'s own HTTPS-transition block previously treated *any* failed HTTPS start as a hard `500`, unconditionally. With `https_enabled` now true by default, `want_https` is routinely true on a request that never mentioned HTTPS at all — a plain `{"management_network": "lan1"}` or `{"port": 8080}` on a fresh, pre-PKI install would now spuriously 500 on an unrelated field, purely because the ambient default couldn't be honored. Fixed by tracking whether *this specific request* explicitly included `"https_enabled"` (`have_https_req`, already computed, just not used here) — a real, explicit ask that can't be honored still hard-fails (correct, unchanged behavior); the ambient default failing to start no longer blocks whatever the request actually asked for, soft-failing (logged, non-fatal) the same way boot already does. Caught live by `test_daemon_bind_ip`'s own regression run, not assumed safe.

## Consequences

- `daemon/src/daemon_config.c` (default flip + comment), `daemon/include/daemon_config.h` (three doc-comment updates), `daemon/src/main.c` (`handle_daemon_config_put()`'s HTTPS-transition block now `have_https_req`-gated for the hard-vs-soft-fail decision).
- `docs/api/openapi.yaml`/`docs/api/README.md` updated together (default example response, `https_enabled` schema description, the endpoint-level defaults description) — same-change requirement per the Documentation Map.
- `docs/guides/installing.md` updated: `https_enabled` is no longer something a fresh install must opt into: PKI bootstrap, then (optionally) an explicit re-`PUT` to bring it live immediately rather than waiting for the next reboot.
- Verified: full clean rebuild (`-Wall -Werror`, zero warnings); full test-suite sweep run (every `build/test_*` binary) — one real regression found and fixed (`test_daemon_bind_ip`, see above), every other suite passes; the few pre-existing `build/bzImage`-missing failures (`test_boot*`/`test_console_*`/`test_installer`) are an unrelated, already-documented environment gap in this dev sandbox (CLAUDE.md), not caused by this change.
- No live redeploy needed to 192.168.15.95 — that box already has this exact configuration (`https_enabled=true`) persisted from its own prior, real configuration; this ADR brings a *fresh* install's own defaults into line with it, not the other way around.
