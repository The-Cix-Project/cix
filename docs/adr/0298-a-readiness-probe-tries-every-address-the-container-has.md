# 0298 — A readiness probe tries every address the container has, because "listening" has no single address

## Status

Accepted. Issue [#477](https://git.home.arpa/itdlabs/cix/issues/477). Refines [ADR-0260](0260-a-container-declares-services-not-a-command.md)'s `ready: {tcp_port: N}` probe; the rest of that decision stands. Related to [ADR-0295](0295-a-container-in-the-directory-can-read-it.md) only in that both are code that needed the container's parsed networks and ran before the loop that parses them.

## Context

`create_container_from_body()` built cix-init's service table beside the `services[]` parse, several hundred lines above the loop that populates `net_attachments[]`:

```c
if (cixinit_table_from_json(jservices, net_count > 0 ? net_attachments[0].ip_be : 0, &init_table,
```

`net_count` is initialised to 0 and assigned from `jnetworks` **further down the same function**. So the ternary took its `0` arm for every container that has ever run, `parse_one()` set `s->ready_addr_be = 0`, and `cix_init.c`'s

```c
a.sin_addr = s->def.ready_addr_be != 0 ? s->def.ready_addr_be : 0x0100007f; /* 127.0.0.1 */
```

fell back to loopback unconditionally. The fallback was the only path, not the exception it reads as. No uninitialised read occurred — the ternary's short-circuit is what kept `net_attachments[0]` untouched — so nothing crashed and nothing warned.

**Why it had not surfaced.** Measured on 192.168.15.95, 2026-09-17: exactly one container on the box declares a `tcp_port` probe, `jump`'s `sshd`, and `/proc/net/tcp` read from inside that container over its console shows a single listener:

```
  sl  local_address rem_address   st ...
   0: 00000000:0016 00000000:0000 0A ...
```

`00000000:0016` is `0.0.0.0:22`. A service bound to any address answers on loopback, so the probe passed for the only reason it could.

**Two comments asserted the mechanism that did not exist**, which is why this needed a ticket rather than a quiet fix — reading either file alone confirmed the intended design instead of revealing that it never ran. `main.c` said "a tcp probe connects to the container's own first address"; `cix_init.c` said "`ready_addr_be` is the container's own non-loopback address ... that case could not be measured in a build container, which has no such address", a sentence that explains the loopback branch as the *exception*. A third was found while fixing this: `test_container_restart.c`'s own depR comment said the probe runs "against its own address". All three are corrected in this change, and the `#413` changelog entry that repeats the claim carries a dated correction.

## Decision

**A TCP readiness probe tries every address the container has, and is ready on the first that answers: `127.0.0.1` first, then each attached network's address in declaration order, one candidate per supervision turn.**

The simple fix — move the call past the attachment loop and keep one address — was rejected, and the reason is a regression it would have introduced. Today `ready_addr_be` is always 0, so loopback is always probed, so a service bound **only to loopback** passes. Set the field to the first network address and that service goes from ready to never-ready. "No Regressions" forbids trading one broken shape for another, and the choice of *which* single address to privilege has no defensible answer: a service may bind `0.0.0.0`, or loopback only, or exactly one interface of a multi-network container, and all three are listening. A probe that asks "is anything accepting connections on this port" should not also be asserting where.

Trying each candidate dissolves the issue's own open question — "is 'first address' even right for a multi-network container" — rather than answering it.

**The addresses belong to the container, so they go in the hello, once, not on each service.** `cixinit_service.ready_addr_be` is gone; `cixinit_hello` carries `addr_count` and `addr_be[CIXINIT_MAX_ADDRS]`. That placement is also the structural half of the fix: a per-service address field invited the daemon to compute one address per service, which is how it came to compute it before the networks existed. A list has no "first address" to reach for early, and `cixinit_table_from_json()` now takes the array, so the call cannot be written before the loop that fills it without passing something visibly empty.

**Loopback is tried first, deliberately.** It is the candidate every `0.0.0.0` binder answers on its first attempt, so the common case costs exactly what it did when loopback was the only candidate. A candidate is advanced only when an attempt *concludes* — `probe_tcp()` leaves `probe_fd >= 0` while a connect is in flight and returns "not ready" for that too, so advancing on the return value alone would move the target out from under a connect that had not answered, and the next turn would poll that fd believing it belonged to a different address. The index resets to loopback on every fresh spawn.

`CIXINIT_VERSION` goes to 2. Both sides ship in one artifact and the table is rebuilt from the persisted body on every replay, so no old table ever meets a new reader; the version exists so a mismatch dies with a message instead of misreading a struct. `test_cix_init` pins the new sizes (hello 272, service 1372).

## Consequences

A service that binds only its container's network address is reported ready. A service that binds only loopback still is. A multi-network container's service is found on whichever interface it chose.

Worst-case detection latency for an *N*-address container is *N*+1 turns of 250 ms per round — 1 second for the widest container 192.168.15.95 actually runs (`cr-1`/`cr-2`, on `management`, `services` and `access`, so four candidates), against a 30-second default probe timeout. The common case is unchanged, because loopback is first.

Moving the table build past the attachment loop changes one thing beyond the probe: a body with both invalid networks and invalid services now reports the network error first. Nothing else between the old and new positions reads `init_table` — it is consumed by `init_transport_open()` and `registry_set_services()` much further down.

`tcp_listen_child` gained a bind-address argument (`any` | `loopback` | a dotted quad) because this could not be tested without a service that binds exactly one address; an unrecognised value is refused rather than falling back to `INADDR_ANY`, since a bind-address test that quietly became "any" would pass for the wrong reason. `test_container_restart`'s new `bindsplit` container is the gate — two services, one bound to loopback only and one to the container's own address only, both required to reach ready with no `probe-timeout` failure — and it runs in `DAEMON_SELFTESTS`.

That gate's first revision asserted a `"ready"` service state, which the API does not have: `registry.c` maps `CIXINIT_EV_READY` to `REGISTRY_SVC_RUNNING` and `CIXINIT_EV_STARTED` to `REGISTRY_SVC_STARTING`, so `"starting"` is a service whose probe has not passed and `"running"` is one whose has. It cost a build cycle and paid for itself: the failure it reported was `loopsvc/running` and `netsvc/running`, both with no `probe-timeout`, which is this decision working — a service bound only to `172.60.0.200` reached ready, which could not happen before.
