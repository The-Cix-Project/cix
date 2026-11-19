# 0383 — A DHCP range may be relayed to its servers

## Status

Proposed. The owner asked for it on 2026-11-19 (*"I think we need a dhcp helper to add to the cores? that will allow us to redirect dhcp to the dnsmasq hosts that we have without having to have an interface on that subnet"*), and chose Cix networks only for now (*"cix networks for now"*). Amends [ADR-0197](0197-dhcp-served-by-the-dns-server.md)'s rule that only servers attached to a network serve its range.

## Context

ADR-0197 serves a range only from registered DHCP servers attached to that network: a server with no interface in the subnet hears no broadcast, so it cannot answer there. On 192.168.15.95 that means `access` (192.168.151.150–200) is the one DHCP-served network, from dns-1 and dns-2, which are attached to it. Serving another network today means adding an interface on it to both servers.

The cores, cr-1 and cr-2, are attached to `management`, `services`, `access` and `remote-access`, and route between them. A relay on a router is the standard answer (RFC 1542, Cisco's `ip helper-address`):
- the relay hears the client's broadcast;
- it forwards it as unicast to each server, with its own address on that subnet in `giaddr`;
- the server picks the range by `giaddr` and replies to the relay;
- the relay passes the reply to the client.

Measured in dnsmasq 2.90's own source and manual, the version `dnsmasq@2.90-4` packages (`probe-dnsrelay@1-1`, `@3-1`, 2026-11-19):

- **The server side:**
  - a request with `giaddr` set is matched to a range by that address (`rfc2131.c:289-291`), and the reply goes to `giaddr` (`dhcp.c:375-379`);
  - the manual: "For networks which receive DHCP service via a relay agent, dnsmasq cannot determine the netmask itself, so it should be specified". So such a `dhcp-range` carries its netmask.
- **The relay:**
  - dnsmasq is one. `--dhcp-relay=<local address>,<server address>[,<interface>]`, one line per server; the local address becomes `giaddr` (`dhcp.c:1118`);
  - the optional interface "controls on which interface DHCP replies from the server will be accepted";
  - relay-only start-up works: `dhcp-relay` lines with no `dhcp-range` run `dhcp_init()` and listen on the DHCP socket (`dnsmasq.c:312`, `:1106`, `:1276`);
  - with no relay lines, no range and `port=0` there is no `die()` path, so it idles;
  - it needs `CAP_NET_ADMIN` (`:318`). dns-1's dnsmasq already serves DHCP with the default set and no `cap_add`.
- **The manual's caveat:** acting as server and relay on one interface "is not supported: the relay function will take precedence".

dns-1 and dns-2 route everything not directly connected via `192.168.150.254`, the cores' VRRP address on `services` (dns-1@1.8.0's `routes`). So a reply to a core's address on another of the cores' networks reaches it.

## Decision

**A range may name relays. A relay is a registered container attached to that network, running dnsmasq in relay-only mode, whose configuration cixd renders from DHCP's own state, as it already does for servers.**

1. **dnsmasq, not a second tool.** The packaged dnsmasq relays, so the cores run it as one more service. A `dhcp-helper` or ISC `dhcrelay` package would be a second implementation of something we already build. *To be accepted by the owner with this ADR*, since their words were "dhcp helper".
2. **Relays are registered, like servers.**
   - `POST /v1/dhcp/relays {container}`, `DELETE /v1/dhcp/relays/{container}`, `GET /v1/dhcp/relays`.
   - A range gains `relays`, an ordered list of registered relays.
   - A container is never both a server and a relay, because of the manual's caveat above. Registering it as the other is refused.
3. **Who serves a relayed range.**
   - A server attached to the network serves it as before.
   - A server not attached is reached through every relay the range names.
   - Each relay forwards to that server at its address on a network both share, the first in the server's attachment order. Replies are accepted only on the relay's interface on that shared network.
   - The split into disjoint slices is unchanged: one slice per named server, attached or not.
4. **Refused at the point of asking** (ADR-0197's rule):
   - a relay that is not registered, or not attached to the range's network;
   - an unattached server with a relay it shares no network with;
   - an unattached server with no route covering the relayed subnet, since its reply goes to `giaddr` there. A connected network or one of its static `routes` must cover it; a default route counts;
   - `relays` on a range whose servers are all attached, because the relay would only duplicate what they hear.
5. **What is rendered.**
   - **A server's file, `/etc/dnsmasq-dhcp.conf`:** a range for a network the server is not attached to carries its netmask. Attached ranges are rendered exactly as today, so this release does not roll dns-1 or dns-2.
   - **A relay's file, `/etc/dnsmasq-relay.conf` (new):** `port=0`, then one `dhcp-relay=<relay's address on the network>,<server's address on the shared network>,<relay's interface on the shared network>` per relayed range and unattached server.
   - Both renderers are pure functions in `dhcp.c`. `main.c` passes in each container's attachments (address, prefix, interface name, static routes), so DHCP does not read another subsystem's registry (ADR-0197's containment). `test_dhcp` covers them.
6. **Applying a change restarts the relay service, never the relay container.**
   - A core's container also runs keepalived and bird, and restarting it would take routing down with it.
   - cixd writes the rendered file into the running container, then restarts that one service through cix-init (`/containers/{name}/services/{service}/restart`'s mechanism). The service is named by the registration (`service`, default `dhcp-relay`).
   - Like a server's files, the relay file is also staged at create, so a container restart starts with it.
   - The relay file names other containers' addresses, which no rendered file did before. So relays re-render when a registered server or relay is created, re-addressed or deleted, not only on a DHCP API call.
7. **A relay failing never fails its container.** The relay service is `on_exit: restart`. keepalived and bird on the cores are `fail-container`, and a bad relay file must not crash-loop routing.
8. **Everywhere ranges and servers travel, relays travel:**
   - the contract (`openapi.yaml` with its README; every new operation annotated);
   - `dhcp.json` (`relays` is optional, so an older cixd after `boot-next` ignores it and drops it at its next save);
   - config export and apply (a `dhcp_relays` section beside `dhcp_servers`);
   - cixctl;
   - the dashboard's DHCP page.
9. **The cores.**
   - The `router` image gains `dnsmasq`: a recipe version, applied by hand, since it is `manual`/`declare`.
   - cr-1 and cr-2 gain the relay service, its flags taken from dns-1's dnsmasq: `-k -u root --pid-file= --conf-file=/etc/dnsmasq-relay.conf`.
   - Each core is recreated in turn, telling the owner first, and the run ends with the vpn-1 check.
10. **Nothing is enabled unasked.** Which network gets a relayed range is the owner's choice. vpn-1 shares `remote-access` with the cores.

## Alternatives

- **Give dns-1 and dns-2 an interface on every network.** That is the situation the owner asked to leave. It also adds a DNS server on each wire, which is wider exposure than DHCP needs.
- **A `dhcp-helper` package.** Rejected above, unless the owner prefers it: a second relay implementation, and one more upstream to package.
- **Relay by broadcast to the servers' network** (dnsmasq's form with no server address). That makes the relay depend on the servers sharing one broadcast domain with it. Naming each server keeps the split explicit and survives a server on another network.
- **Subnets that are not Cix networks**, such as VLANs on a physical switch behind the cores. The owner chose Cix networks for now. Those would need a range keyed by subnet rather than by network name, which is a later decision.

## Consequences

- A relayed range answers only while a relay and a server are both up. Each core relays, and each server holds a slice, so losing one of each still serves; the cost is the usual split-scope one (ADR-0197).
- Both cores relay every request, so each server sees each client twice, from two `giaddr` values in the same subnet. dnsmasq answers both copies alike, and the client takes one offer.
- A client renews by unicast to the server that leased it, through its default gateway, not through the relay. So the range's router must route to the servers' network; on .95 the cores do.

## Live checks

- The release with no relay registered changes nothing: dns-1 and dns-2 are not rolled, and `access`'s leases continue.
- On the network the owner names, a scratch container running `dhcpcd` (packaged) with no static address:
  - gets a lease through a relay, from an unattached server;
  - renews it, with a short lease (60 s, the minimum), so the renewal is seen, not only the first lease;
  - resolves by name (`resolves_leases`).
- Changing the range restarts the relay service, and bird and keepalived keep their pids.
