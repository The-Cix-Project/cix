# 0197 — DHCP is served by the DNS server, and a lease is not a record

## Status

Accepted

Builds on the model [ADR-0013](0013-proc-pid-root-for-live-container-file-writes.md) established and DNS has used since: thinC owns the durable state, a real dnsmasq in an ordinary container does the serving, and every change re-renders the whole file into every registered server through `/proc/<pid>/root`.

## Context

The platform could hand a container a static address and a name, and nothing else. Anything that arrives on a wire expecting to be told its address — a laptop, a printer, a machine being provisioned — had no answer here.

The operator asked for DHCP with ranges per network, an enable switch per network, static reservations, all API-driven, and — the part that shapes everything else — for it to "play nice with DNS", with the DNS entry appearing on the fly.

## Decision

**DHCP is served by a container that is already a registered DNS server, and enabling DHCP on a network requires naming one.**

That is not a convenience. dnsmasq resolves the names of clients it has itself leased addresses to, so when the instance handing out the address is the instance answering for the name, a lease is resolvable the moment it exists — no synchronisation, no window in which the two disagree, and no component whose job is to keep them agreeing. Requiring a registered DNS server makes that property structural rather than something an operator has to get right. A container that is running but not a registered DNS server is refused, with the reason.

**A lease is not a DNS record.** A record here is durable operator intent: persisted, surviving every server, listed at `GET /v1/dns/records`. A lease is short-lived state owned by the server that issued it. Mirroring leases into the record store would put two writers in one namespace and would leave a record pointing at an address a different machine now holds the first time a lease expired uncleanly. Leases are read back from the server's own lease file at `GET /v1/dhcp/leases` — every time, never cached, because they are the server's answer and not ours — while remaining resolvable through that same dnsmasq regardless.

**Two rendered files, because dnsmasq treats them differently.** The reservations file is re-read on `SIGHUP`, so a static entry is live exactly like a DNS record. The conf file carrying the ranges is read only at startup. Rendering both and signalling would leave a range change silently inert — a setting that says it is in force and is not, which is the failure mode this project keeps finding and closing. So **a range change restarts the serving containers**, through the same jittered rolling-restart timer a rolling image update uses. With two servers registered, that jitter is what keeps them from going down together, which is the entire reason there are two of them; reusing it rather than writing a second restart path is what keeps that property true here for free.

**Everything that could not actually serve is refused at the point of asking.** A range outside its own subnet is a range no client on that wire can be given. A range covering the network's own address hands the host's IP to a client, which is a conflict rather than a lease. A range that runs backwards, a lease time of four seconds, an enabled network with no addresses — each is a configuration that would start a server which then answers nothing, and each is a `400` here instead. Two reservations for one address is a conflict waiting for both machines to be powered on at once, and is likewise refused where it is one message rather than discovered where it is an outage.

MAC addresses and hostnames are **refused, never sanitised**: both are written into a file dnsmasq parses, and a hostname additionally becomes a name it answers for, so it is held to a DNS label's shape.

## Consequences

DHCP on a network is only as available as the container serving it. Registering two DNS servers and enabling DHCP on both would have two independent lease databases handing out the same range — so the config names **one** server per network, and redundancy for DHCP specifically is not something this design provides. That is a real limit, stated rather than papered over.

The serving container must be started against the three well-known paths (`/etc/dnsmasq-dhcp.conf`, `/etc/dnsmasq-dhcp-hosts`, `/run/dnsmasq.leases`) with those first two staged, even empty, at creation — dnsmasq refuses to start on a `--conf-file` that does not exist. That is the same shape every other service container here already has, where thinC renders into a file the container was created holding.

**Enabling DHCP on a network that reaches a real LAN will answer requests from machines that are not this platform's.** The management network on a home or office LAN almost certainly already has a DHCP server, and a second one is not a redundant pair — it is two servers with separate lease databases handing out overlapping addresses. Nothing here prevents that, because nothing here can tell a lab bridge from an uplinked one; it is an operator decision, and the documentation says so plainly.
