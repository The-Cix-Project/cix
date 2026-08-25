# 0182 — Registered-server health probing, as one primitive for every subsystem

## Status

Accepted

## Context

Cix lets an operator register redundant backend servers for four subsystems — LDAP, DNS, NTP and syslog — each with its own `*_server_register()` binding table. Until now every one of those tables recorded only *that* a server was registered. Nothing anywhere asked whether a registered server was actually **serving**.

That gap has already cost real debugging time. Issue #80 was a registered LDAP pair (`ldap-1`/`ldap-2`) that was running, listening, and answering on the wire — but could not resolve anything under the base DN the clients were configured with. Every SSH login to the jump box failed, and no surface anywhere reported a server as bad; the failure had to be traced by hand through `nslcd -d` output. A health check that actually probed the service would have shown it immediately.

The same shape applies to all four subsystems: a registered server can be down, unreachable, or up-but-not-serving, and clients are handed its address either way.

## Decision

**One health primitive serves all four kinds** (`daemon/src/serverhealth.c`), rather than four parallel implementations. "A registered server, probed on an interval, healthy or not, optionally drained by an operator" is the same concept in every subsystem; only the enumerator and the probe target differ. Each kind contributes one row to a table in `main.c`:

```c
{ "ldap", HOSTAUTH_LDAP_DEFAULT_PORT, serverhealth_list_ldap },
{ "dns",  53,                         serverhealth_list_dns  },
{ "ntp",  0,                          serverhealth_list_ntp  },
{ "syslog", 0,                        serverhealth_list_syslog },
```

Adding a fifth registered-server subsystem means adding a row. The record, the state machine, and the whole REST/CLI/web surface are already shared. Three of the four kinds needed a `*_list_containers()` enumerator to match the one LDAP already had; adding them made the four registries uniform, which is a small win in itself.

**The probe is a non-blocking `connect()` driven by the daemon's own reactor.** This is the load-bearing design constraint, not an implementation detail: `cixd` is a single `epoll` loop, and a blocking probe against an unreachable server would stall the entire control plane — the exact failure ADR-0180 exists to prevent. `serverhealth.c` therefore performs no I/O at all; it is pure state, and `main.c` reports each probe result back into it. In-flight probes are tracked so a sweep can time out stragglers rather than leaving them to the kernel's own multi-minute TCP timeout.

**A probe reports what it actually did.** `probe` is `tcp:PORT` when the server accepted a real connection, and `process` when the check was only "the providing container is running". NTP and syslog are UDP: a TCP connect against them would be a meaningless check dressed up as a real one, so they get the honest weaker check and the API says so. An operator is never left guessing how much a `healthy` verdict is worth.

**Asymmetric state transitions.** One successful probe restores health immediately; declaring a server unhealthy takes several *consecutive* failures. A single dropped probe (a busy box, a container mid-restart) is not a reason to pull a working directory server out of service, and flapping one in and out is worse than reacting slowly — but recovery must never be delayed.

**Drain is persisted; health is not.** Draining is operator *intent* ("I am doing maintenance on this one") and must survive a daemon restart, exactly like a container's own `stopped` flag. Observed health is a live measurement that would be a lie if replayed from disk, so it always starts `unknown` and is re-established by the first sweep.

**Not-in-service servers are withheld from generated client configuration** — this is the point of the whole mechanism, not a follow-on. An unhealthy or drained LDAP server stops being handed to `ldap_login` containers. Two safety rules bound that:

- a never-yet-probed (`unknown`) server counts as **in service**, so enabling health tracking cannot black-hole a working deployment during the first sweep;
- if filtering would leave **nothing**, the unfiltered list is used instead. Handing a client a possibly-down server is strictly better than handing it none at all — the client retries, whereas an empty list turns a partial outage into a total one.

## Consequences

- The #80 class of failure is now visible and self-correcting: a registered-but-not-serving LDAP server is reported `unhealthy` and drops out of the client URI list, instead of silently breaking every login.
- Health is observable and actionable from all three surfaces per the API-First Mandate: `GET/PUT /v1/system/server-health...`, `cixctl server-health`, and the dashboard's Server Health page.
- The honest limitation, stated rather than hidden: NTP and syslog get container-liveness only. A real SNTP exchange (the daemon already speaks SNTP for its own clock sync) would be a genuine improvement and is deliberately left as later work rather than faked now.
- A drained server stays drained across restarts, so a forgotten drain is a real operational hazard — which is exactly why `in_service` and `drained` are separate, prominently reported fields rather than one merged status.
- The probe interval (30s) and failure threshold (3) are compiled-in constants, not yet operator-configurable. That is a deliberate YAGNI boundary: one sensible cadence for every deployment until someone has a real reason to differ.
