# 0197 — DHCP is served by the DNS server, and a lease is not a record

## Status

Accepted

Builds on the model [ADR-0013](0013-proc-pid-root-for-live-container-file-writes.md) established and DNS has used since: thinC owns the durable state, a real dnsmasq in an ordinary container does the serving, and every change re-renders the whole file into every registered server through `/proc/<pid>/root`.

## Context

The platform could hand a container a static address and a name, and nothing else. Anything that arrives on a wire expecting to be told its address — a laptop, a printer, a machine being provisioned — had no answer here.

The operator asked for DHCP with ranges per network, an enable switch per network, static reservations, all API-driven, and — the part that shapes everything else — for it to "play nice with DNS", with the DNS entry appearing on the fly.

## Decision

**DHCP is served by every registered DNS server, and the range is split into one disjoint slice per server.**

The pairing with DNS is not a convenience. dnsmasq resolves the names of clients it has itself leased addresses to, so when the instance handing out the address is the instance answering for the name, a lease is resolvable the moment it exists — no synchronisation, no window in which the two disagree, and no component whose job is to keep them agreeing. Serving from the DNS servers makes that structural rather than something an operator has to get right.

**Redundancy is split scope, because with dnsmasq it has to be.** The operator asked for both servers working redundantly, primary and backup, the way ISC dhcpd is configured. dnsmasq implements no failover protocol — there is no peer relationship for it to join, the two instances share no lease database, and neither can know what the other has handed out. The standard technique in its absence is to give each server a disjoint slice of one range: both answer, a client takes whichever offer reaches it first, and handing one address to two machines is impossible because no two servers hold it. Either server alone keeps serving, from its own slice, which is the practical property primary/backup exists to provide. What it does not provide is a hot standby that takes over the *whole* pool knowing its peer's leases; that needs a server implementing a failover protocol (ISC Kea or dhcpd), which is a package this platform does not have.

**The split is computed, never configured.** Given a range and a number of servers it has exactly one correct answer, and asking an operator to write it into each server would be asking them to keep two copies of one fact in step. Adding or removing a DNS server re-splits every other server's slice, so "did the configuration change" is a question about the whole set rather than about one member of it. The slices are reported back, because redundancy that cannot be seen cannot be trusted.

**A lease is not a DNS record.** A record here is durable operator intent: persisted, surviving every server, listed at `GET /v1/dns/records`. A lease is short-lived state owned by the server that issued it. Mirroring leases into the record store would put two writers in one namespace and would leave a record pointing at an address a different machine now holds the first time a lease expired uncleanly. Leases are read back from the server's own lease file at `GET /v1/dhcp/leases` — every time, never cached, because they are the server's answer and not ours — while remaining resolvable through that same dnsmasq regardless.

**Two rendered files, because dnsmasq treats them differently.** The reservations file is re-read on `SIGHUP`, so a static entry is live exactly like a DNS record. The conf file carrying the ranges is read only at startup. Rendering both and signalling would leave a range change silently inert — a setting that says it is in force and is not, which is the failure mode this project keeps finding and closing. So **a range change restarts the serving containers**, through the same jittered rolling-restart timer a rolling image update uses. With two servers registered, that jitter is what keeps them from going down together, which is the entire reason there are two of them; reusing it rather than writing a second restart path is what keeps that property true here for free.

The restart happens whatever the serving container's restart policy says. [ADR-0181](0181-persist-all-containers-restart-decoupled-from-existence.md)'s rule — `restart: "no"` is never auto-recreated — is about recreation nobody asked for, a rolling image update arriving on its own schedule. This restart is the direct, immediate consequence of an operator changing a setting on that very container; declining it would hand them a range that reports itself in force and is not. That distinction was checked rather than assumed: the first version of this refused such a server outright, on the belief that a `restart: "no"` container had no persisted definition to replay. It does have one, the restart works, and the refusal was removed.

**Everything that could not actually serve is refused at the point of asking.** A range outside its own subnet is a range no client on that wire can be given. A range covering the network's own address hands the host's IP to a client, which is a conflict rather than a lease. A range that runs backwards, a lease time of four seconds, an enabled network with no addresses — each is a configuration that would start a server which then answers nothing, and each is a `400` here instead. Two reservations for one address is a conflict waiting for both machines to be powered on at once, and is likewise refused where it is one message rather than discovered where it is an outage.

MAC addresses and hostnames are **refused, never sanitised**: both are written into a file dnsmasq parses, and a hostname additionally becomes a name it answers for, so it is held to a DNS label's shape.

**It is a service, not only a network setting.** A range belongs to a network, so enabling it lives on that network's own page — but who is serving what, every reservation and every lease span networks, and those live on a DHCP page under Services alongside DNS, LDAP, NTP and Syslog. That is the same split DNS already has between its records and its servers, and the operator asked for exactly that consistency.

## Consequences

Enabling is refused when no DNS server is registered at all — nothing would answer — and when the range holds fewer addresses than there are servers, since rounding a server's slice down to nothing would quietly make it not a server. Both are cheap checks for states that are otherwise discovered by a client that never got an address.

A split range is a smaller pool per server than the operator wrote down. Two servers over a hundred addresses means fifty each, and if one is down the other still only has its own fifty. That is the honest cost of having no failover protocol, and it is why the split is reported rather than hidden: the number that matters when a server is down is the slice, not the range.

The serving container must be started against the three well-known paths (`/etc/dnsmasq-dhcp.conf`, `/etc/dnsmasq-dhcp-hosts`, `/run/dnsmasq.leases`) with those first two staged, even empty, at creation — dnsmasq refuses to start on a `--conf-file` that does not exist. That is the same shape every other service container here already has, where thinC renders into a file the container was created holding.

**Enabling DHCP on a network that reaches a real LAN will answer requests from machines that are not this platform's.** The management network on a home or office LAN almost certainly already has a DHCP server, and a second one is not a redundant pair — it is two servers with separate lease databases handing out overlapping addresses. Nothing here prevents that, because nothing here can tell a lab bridge from an uplinked one; it is an operator decision, and the documentation says so plainly.
