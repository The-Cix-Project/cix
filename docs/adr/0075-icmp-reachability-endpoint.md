# 0075 — Reachability endpoint (GET/POST /v1/system/ping)

## Status

Accepted

## Context

Raised by the user while diagnosing 192.168.15.95's own real network limitations (no outbound DNS resolution, confirmed via `CURLE_COULDNT_RESOLVE_HOST`): the daemon had no way to answer "is there a route to X at all" independent of DNS. Every diagnosis up to this point relied on ad-hoc, throwaway tricks (a scratch `pkg_source` fetch, a raw-IP `curl`) — genuinely useful once, but not a real, reusable capability, and each one conflates several failure modes (DNS, routing, the target's own liveness) into one opaque error.

## Decision

**A real, hand-rolled ICMP echo, no shelling out to a `ping` binary** — this project's own images don't have one, and every other network primitive this daemon needs already talks to the kernel directly (rtnetlink for routes/addresses; this is the same discipline applied to reachability). `daemon/src/ping.c`/`include/ping.h`: `socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK, IPPROTO_ICMP)`, a real RFC 1071 checksum, `sendto()`/`recvfrom()` matched by echo identifier+sequence (a raw ICMP socket sees every ICMP packet system-wide, not just this job's own — a non-matching packet, e.g. unrelated ICMP traffic, is silently ignored rather than treated as a reply). `CAP_NET_RAW` is required but not a new constraint — `thincd` already runs as root on any real `--init-mode` host.

**Async, epoll-integrated, same "kick off + poll" shape as every other uncertain-duration job in this daemon** (disk format, ISO build, pkg fetch): `POST /v1/system/ping {"host":"A.B.C.D"}` starts it, `GET` on the same path polls. Two fds are genuinely in flight for one logical job — the raw socket (an echo reply arriving) and a `timerfd` (a fixed ~2s timeout) — whichever fires first resolves the job and tears down both. This hits the exact same "both fds ready in one epoll batch" hazard `console_session_teardown()` already solved for the WS/PTY console pairing; reused directly (`CONN_DEAD` + `queue_conn_free()`) rather than re-inventing a second answer to the same problem.

**v1 single-job constraint**, matching every other async job in this daemon — `409` if a ping is already in flight. A diagnostic tool, not a monitoring primitive; concurrent pings were never a real requirement.

**`thincctl ping HOST`** polls to completion client-side (same shape as `--wait` on `pkg hostbuild`/`iso build`) and exits nonzero on an unreachable result — a real, scriptable check, not just a status printer.

## Consequences

- IPv4 and raw-IP only in v1 — no hostname resolution (deliberately: resolving via a possibly-broken resolver would reintroduce exactly the DNS-vs-routing conflation this endpoint exists to eliminate; pair with `GET /v1/system/resolv`/real DNS once that lands).
- Purely additive: new module, new route, no existing schema touched.
- Verified live: a real loopback ping returns a genuine sub-millisecond RTT; a real unreachable target (`192.0.2.1`, RFC 5737 TEST-NET-1) times out cleanly at the fixed ~2s bound, both through the full async POST-then-poll path.
