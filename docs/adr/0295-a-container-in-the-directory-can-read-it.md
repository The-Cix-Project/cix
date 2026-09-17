# 0295 — A container in the directory can read it: omitted `dns_servers` defaults to the registered servers

## Status

Accepted. Issue [#451](https://git.home.arpa/itdlabs/cix/issues/451). Supersedes one decision in [ADR-0143](0143-container-dns-servers-field.md) — its "deliberately explicit, no auto-wiring to a registered internal DNS server" paragraph. Everything else in ADR-0143 (the field, its 3-entry cap, the `400` against a conflicting `files` entry, and the pre-`clone3()` staging mechanism) stands unchanged.

## Context

Measured inside the running `jump` on 192.168.15.103, 2026-09-13:

```
claude@jump:~$ cat /etc/resolv.conf
cat: /etc/resolv.conf: No such file or directory
claude@jump:~$ getent hosts git.home.arpa
rc=2
```

`ping 8.8.8.8` worked; `ping anything.by.name` could not. This was the documented behaviour, not a regression — ADR-0143 staged a `/etc/resolv.conf` only when the request carried a non-empty `dns_servers`, and said so.

**What made it worth reopening is that `jump`'s own definition carries `dns_register: true`.** The platform published that container's name into its own DNS, on a box running `dns-1` (192.168.150.101) and `dns-2` (192.168.150.102) on the very network the container is attached to — and then handed the container no way to query it. A container that is *in* the directory and cannot *read* it is not a conservative default; it is an internally inconsistent one, and its operator-visible form is a jump box where `ssh somehost` fails for no stated reason.

ADR-0143's reasoning was sound for the case it considered: there is no fixed notion of "the" internal DNS server, zero or several can exist on different networks, and ADR-0076 had already settled the host's own equivalent question the same explicit way. What it did not consider is that the platform *already knows* the answer for the specific case where it publishes the container itself — the registration table and the container's own network list are both in the daemon's hands at creation time.

An intermediate step shipped first: a `warn` at creation when `dns_register` was set with no `dns_servers`, so the silent form became visible. That is option 1 of the three the issue listed. This ADR is option 2.

## Decision

**An OMITTED `dns_servers` defaults to the registered DNS server containers that share a network with this container**, in registration order, capped at `RESOLV_MAX_NAMESERVERS` (3, unchanged). An explicit `[]` still means no resolver, and is now the only way to say it.

**The default is read from each DNS server's persisted definition, never from the live registry.** This is the load-bearing choice, not an implementation detail. Autostart runs in `containerdef_resolve_order()`'s `depends_on` order, and every definition on 192.168.15.95 declares `depends_on: []` (measured 2026-09-17) — so the order among them is arbitrary. A registry-backed lookup would find `dns-1` on the boots where it happened to start first and nothing on the others: #451's own symptom, made intermittent, which is worse than deterministic. `dns_init()` and `containerdef_init()` both run before autostart, so the definition is readable whenever a container is created and gives the same answer on every replay — including while the server is stopped, which `test_container_dns_servers` asserts directly.

**Only an explicit `ip` in the server's definition counts.** A registered server whose address was auto-allocated contributes nothing, because that address is not stable across boots either — a `resolv.conf` pinned to a previous allocation is confidently wrong rather than honestly absent. Those names are logged with the reason, since the operator's fix is one explicit `ip`.

**Three cases get no default:**

- an explicit `dns_servers` of any length, `[]` included;
- an operator-supplied `files` entry for `/etc/resolv.conf` — the same question, already answered by hand (combining *that* with an explicit `dns_servers` remains ADR-0143's `400`; supplying only the file is not an error and now simply wins);
- a container that is itself a DNS server. Checked from the request body's own `dns_server` field *and* from `dns_server_is_registered()`, because registration happens after creation — the body covers first creation, the binding covers every replay after it.

The reason for that third rule is One Source of Truth, not loop-avoidance: a resolver's upstream is `dns_forwarders_set()` and its own `--servers-file`, so a staged `resolv.conf` would be a second, conflicting answer to where that resolver sends queries. Worth recording what the measurement actually showed, because it argues the rule is right for a reason other than the obvious one: `dns-1` and `dns-2` both omit `dns_servers` *and* both run dnsmasq with `-R` (`--no-resolv`), so without this rule they would have been pointed at each other and would have ignored the file anyway. Harmless — by accident, through a flag in a deployment recipe the daemon does not control. That is exactly the coupling not to rely on.

## Consequences

- **`dns_register` is coherent.** A container the platform publishes can resolve the directory it is published in, with no operator action. On the real box, a container on `services` now gets 192.168.150.101 and .102 — the same two values `recipes/deployment/jump/1.13.0` had to set by hand.
- **Omitting and `[]` now differ**, where before they were the same. This is a behaviour change for any caller that sent `[]` meaning "I don't care". Both first-party clients already omit the field when empty rather than sending `[]` (`cli/src/main.c`'s `if (dns_server_count > 0)`, `web/app.js`'s `if (dnsServersText !== "")`), verified before shipping — had either serialised `[]`, every container created through that surface would have silently opted out and the feature would never have reached an operator.
- **The default is recomputed on every replay**, because the stored request body keeps no `dns_servers` and `create_container_from_body()` runs again on each revival. So registering a DNS server later reaches containers created before it existed, on their next start. The cost of the same property: a container's `resolv.conf` can change across a restart if the registered set changed, which is the intended behaviour and is logged each time.
- **`GET /containers/{name}` reports the EFFECTIVE list.** It is rendered from the registry entry, which now carries the defaulted values — so the field says what the container's `resolv.conf` actually contains rather than what the request said. The config document (ADR-0292) renders containers from the same registry, so a `POST /config` round-trip that applies containers would pin today's default as an explicit list. That is the true running state and is acceptable, but it is a real edge worth knowing rather than discovering.
- **A per-deployment workaround is no longer needed** but is left in place: `jump`'s definition still names both resolvers explicitly, which now takes the explicit path and produces the identical result. Reverting it to an omitted field is a recipe bump, and the owner's call.
- **A resolver is not the same as resolution, and the difference is measured.** Verified on 192.168.15.95, 2026-09-17, from inside `jump` (which carries `ldap_client`): `getent hosts git.home.arpa` -> 192.168.15.15, `getent hosts jump` -> 192.168.150.109, `getent hosts cix.internal` -> 192.168.15.95, all rc=0 -- the first of those being the exact command #451 reported failing. But a container whose `/etc/nsswitch.conf` comes from `pkg_seed_image_baseline()` has `hosts: files` and **no `dns` backend**, so glibc never consults DNS and the staged `resolv.conf` is inert. Measured in a throwaway `jumpbox` container on `services`: correct `resolv.conf`, `libnss_dns.so.2` present, `getent` rc=2. The containers that do resolve work because the `ldap_client` nsswitch replaces the baseline and omits the `hosts` line entirely, letting a glibc compiled-in default supply `dns` -- which is precisely the default the baseline's own comment says this project has no reason to depend on. So this ADR delivered what it claims (a container in the directory is given the resolvers to read it) while the layer above it was not yet coherent. **Closed by [ADR-0296](0296-the-platform-keeps-nsswitch-correct.md) (#478)**, which puts `hosts: files dns` in both variants from one shared definition and makes the baseline converge -- deliberately a separate decision, since it reverses ADR-0111's stated posture. Note ADR-0155 means that convergence reaches an image on its next real install rather than on deploy, so a check made right after upgrading still shows the old content.

- **ADR-0076 is untouched.** The host's own `PUT /system/resolv` keeps its explicit, no-discovery posture. The asymmetry is deliberate: the host is not published in the platform's own DNS by the platform, so the inconsistency this ADR closes does not arise there.
