# 0386 — DHCP is served by an ISC dhcpd failover pair

## Status

Accepted by the owner on 2026-11-20 (#107). The owner's three answers:
- *"yes go ahead with dhcpd, load-balancing"*, chosen over Kea (*"too big ... I don't want bulky rubbish"*), after being told ISC ended dhcpd's maintenance;
- *"load-balancing would be best, always active-active"*;
- *"auto ... don't get cixd involved"*, for how a survivor takes the whole pool.

Supersedes the serving half of [ADR-0197](0197-dhcp-served-by-the-dns-server.md): split scope, dnsmasq as the DHCP server, and `resolves_leases`. ADR-0197's containment rule, its refusals and its rule that a lease is not a record stand. [ADR-0383](0383-a-dhcp-range-may-be-relayed.md)'s relays stand unchanged, and now forward to both peers.

## Context

ADR-0197 shipped split scope because dnsmasq has no failover protocol. Each server holds a disjoint slice of a range, so a survivor has only its own slice. On 192.168.15.95 a range of 51 addresses is two slices of 26 and 25.

The owner asked for active-active in 2026-08 (#107). On 2026-09-14 they chose ISC dhcpd over Kea, as later feature work. On 2026-11-20 they were told dhcpd's maintenance ended in October 2022 with 4.4.3-P1 ([ISC](https://isc.org/blogs/isc-dhcp-eol/)). Kea is ISC's maintained successor, but needs C++, Boost and log4cplus. The owner kept dhcpd. It is the one small server with a real active-active protocol: the DHCP failover draft, in its load-balancing form.

Measured on 192.168.15.95, 2026-11-20, with a scratch pair (`dhcpt-1`/`dhcpt-2`, image `dhcp`, `isc-dhcp@4.4.3-P1-3`, `auto-partner-down 60`, `mclt 120`):

- **Build and start.** dhcpd builds with tcc. It starts in a userns container and serves through its own raw socket there.
- **Fresh pair.** It reached `Both servers normal` within a second and balanced a 191-address pool 96/95.
- **Peer stopped.** The survivor went to communications-interrupted, then to partner-down exactly 60 s later (`auto-partner-down`). In partner-down it serves the whole pool.
- **Peer back after partner-down.** The returning peer sat in recover-wait for one MCLT (118 s against 120) while the survivor served everything. Then both were normal.
- **Quick restart with the lease file kept.** A restart of one peer's dhcpd (the service, not the container) brought both back to normal in under a second, with no MCLT wait.
- **Lease file missing.** dhcpd refuses to start without one: the manual's *"Please read the dhcpd.leases manual page"* and *"exiting"*. The API has no route to write into a volume: `container files put` refuses a volume path (409).

From dhcpd's own manuals ([dhcpd.conf(5)](https://manpages.debian.org/bookworm/isc-dhcp-server/dhcpd.conf.5.en.html), [dhcpd(8)](https://manpages.debian.org/bookworm/isc-dhcp-server/dhcpd.8.en.html)):
- dhcpd has no reload signal: *"Whenever changes are made to the dhcpd.conf file, dhcpd must be restarted"*.
- An authoritative server answers a request for an address it knows nothing about with a NAK.

## Decision

**Every DHCP range is served by registered dhcpd containers. A range names one server, or two that form a failover pair in load-balancing mode. dnsmasq serves DHCP nowhere; it keeps DNS on dns-1/dns-2 and relaying on the cores.**

1. **One server, never two.** There is no second DHCP backend and no split scope.
   - The DHCP service renders only `dhcpd.conf`.
   - dnsmasq's DHCP files, flags and lease volume leave the DNS servers in the same cut-over.
   - The contract loses `slices` and `resolves_leases`.
2. **A pair is two servers, and a server is in at most one pair.** A range's `servers` holds one or two names; three or more is a 400.
   - All ranges naming two servers that include one server name the same partner; a range pairing it with a different partner is refused.
   - Roles are computed, never configured: the name that sorts first is primary, the other secondary. So every range's order is irrelevant, and a pair's roles never move.
   - The peer channel runs over the first network, in the primary's attachment order, that both are attached to; a pair sharing no network is refused.
   - Peer addresses are re-rendered when either is re-addressed, as ADR-0383's relay addresses are.
3. **Load-balancing, with dhcpd's own takeover.** The rendered peer declaration carries:
   - `split 128` and `mclt 3600` on the primary (the manual: mclt *"must be specified on the primary, and may not be specified on the secondary"*);
   - `load balance max seconds 3`, `max-response-delay 60` and `max-unacked-updates 10` on both. *(Corrected 2026-11-21: this said load balancing was the primary's alone, and on 0.2.57-708 the secondary logged "load balancing will be disabled for failover peer, cix, because its load balance max secs is 0".)*
   - `auto-partner-down 600` on both.

   **cixd never declares partner-down** (the owner, *"don't get cixd involved"*). dhcpd moves itself after ten minutes without its peer, and then serves the whole pool. A restart takes seconds, so it never reaches partner-down. Only a real outage does, and a peer returning from one waits one MCLT while the survivor serves everything.
4. **Leases are the server's, on its volume, and cixd only seeds the file.** A server mounts a volume at `/var/lib/dhcp`. Its lease file is `/var/lib/dhcp/dhcpd.leases`. Before the server starts, cixd creates that file empty, host-side on the volume, when it does not exist, and never writes it otherwise. That is the one write cixd makes there, and it exists only because dhcpd will not start without the file.
5. **A change restarts the pair one member at a time, through the timer every daemon-chosen restart already uses.** dhcpd has no reload.
   - A range, reservation, server or relay change that alters a server's rendered `dhcpd.conf` arms `arm_rolling_restart_timer()` for that server, exactly as a range change armed it for dnsmasq.
   - The pair declares `roll_group` (`dhcp` on .95), so ADR-0385's gate restarts the second only once the first is running and ready. A quick restart returns both to normal at once (measured above).
   - The DNS servers no longer restart for any DHCP change.
6. **A reservation is a host declaration outside every range.** A reservation's address must lie in a network a range is defined on, and outside every range's pool; one inside a pool is a 400. dhcpd gives a `fixed-address` host its address without a lease, so cixd's own reservations are the source of their names, not the lease file.
7. **Leases are read from the lease files, off the loop.** dhcpd's lease file is an append log, rewritten only periodically, so its size is not bounded by what is asked for (ADR-0342). `GET /v1/dhcp/leases` and the DNS names below read it in a helper child, and the request waits for the result (a park kind).
   - Both peers hold every lease, so a lease is reported once: the newest record for each address, across both files.
   - Each server's failover state (its own and its peer's, from the file's last `failover peer ... state` block) is reported on `GET /v1/dhcp/servers`.
8. **Lease names reach DNS through cixd, into a file of their own.** dnsmasq accepts no dynamic updates, so dhcpd sends none (`ddns-update-style none`).
   - cixd renders each active lease's client hostname into `/etc/dnsmasq-leases` in every registered DNS server, which dnsmasq reads as an additional hosts file. It is put in force by `container_reload()` (ADR-0384), and only when the rendered file changed.
   - The records file is never touched, so a lease is still not a record (ADR-0197).
   - A name an operator record or a container already resolves always wins, and the lease name is left out.
   - A client-supplied hostname that is not a valid DNS label is left out. It is untrusted input.
   - The file is refreshed after the lease read above, on a timer while any range is enabled, and from the same read a `GET /v1/dhcp/leases` makes.
9. **Relays forward to both peers.** ADR-0383's relay file already writes one `dhcp-relay` line per server of a relayed range, so a pair is two lines, and ADR-0383's refusals apply to each peer.

## Consequences

- **Survival.** A pair now survives the loss of either member with the whole pool, after ten minutes, where split scope left the survivor its own slice forever.
- **Reservation changes.** A reservation change is no longer live. It costs the pair two quick restarts, one at a time.
- **dhcpd is frozen.** A security fix found in it is carried as a patch in the `isc-dhcp` recipe, by this project. Its only clients are the relays on internal networks.
- **Cut-over on .95.**
  - dhcp-1 and dhcp-2 are created on `services` (roll group `dhcp`, leases on volumes on vda) and registered. `access` moves first, and `remote-access`, the owner's own way in, moves last, with the owner told first.
  - Clients holding a dnsmasq lease are NAKed at their next renewal and take a new lease from the pair. Two leases were live when this was written (`cixctl dhcp leases`), so no lease is imported.
  - dns-1 and dns-2 lose DHCP in a new deployment revision, after every range has moved.

## Alternatives

- **Kea.** Maintained, and it has HA hot-standby and load-balancing. Refused by the owner as too large: C++, Boost and log4cplus, and a long build.
- **cixd declares partner-down over OMAPI.** cixd knows when a peer container is down for certain, but it would have to implement dhcpd's control protocol. Refused by the owner.
- **Keep split scope.** It never gives a survivor the whole pool, which is what #107 is for.
- **dhcpd sends dynamic DNS updates.** dnsmasq does not accept them, so this would mean replacing dnsmasq's DNS first (#463).
