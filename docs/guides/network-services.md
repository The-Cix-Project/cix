# Network services: DNS, DHCP, NTP and syslog

Cix does not implement these protocols itself. Each one is served by an ordinary container — dnsmasq for DNS and DHCP, chrony for NTP, sysklogd for syslog — and the daemon owns the configuration those containers serve: the records, the forwarders, the ranges and reservations, the upstream time sources, the forward targets. You declare intent over the API; the daemon writes it into every registered server and keeps it there.

This guide covers standing each service up, feeding it, and taking a server out of service. The exact request and response shapes are in [`docs/api/README.md`](../api/README.md); every command below is `cixctl` against a real host (`--host=` implied).

## The shape every service shares

1. **A container runs the server.** It is created from a deployment (a container recipe, [ADR-0151](../adr/0151-container-recipes.md)) — `dns-1`/`dns-2`, `ntp-1`/`ntp-2`, `syslog-1`/`syslog-2` in the [cix-recipes](https://git.home.arpa/itdlabs/cix-recipes) repository — and runs from an image (`dns`, `chrony`, `syslog`) that must already hold its packages. Running containers and deployments is covered in [`containers-and-services.md`](containers-and-services.md); building an image is covered in [`images.md`](images.md).
2. **The container is registered** as a server of its kind. Registration is what makes the daemon write configuration into it and track its health. Deleting a container forgets its registration.
3. **Health is probed**, and a server can be drained by hand. See [Health, drain and undrain](#health-drain-and-undrain).

A deployment can declare its own registration, so recreating the container from its recipe restores the server and not only the process: `dns_server` (`{"hosts_path": ...}`), `ntp_server: true`, `syslog_target: true` and `ldap_server` in the recipe are each equivalent to the matching register call after creation (see the `ContainerCreateRequest` schema in [`openapi.yaml`](../api/openapi.yaml)). The shipped `dns-1`/`dns-2` recipes declare `dns_server`; the `ntp-*` and `syslog-*` recipes do not, so those are registered by hand as shown below. DHCP registration has no recipe field: after a DHCP server's container is deleted and recreated, register it again with `cixctl dhcp server add` and name it in its ranges again.

The recipes, images and deployments reach a host through the recipe repository sync (`cixctl pkg sync`, configured with `cixctl pkg repo-config set`), which publishes package recipes, image recipes and deployments together.

## DNS

Records are daemon state; dnsmasq containers serve them. Every record change rewrites each registered server's hosts file and signals it to reload, so a change is live on every replica at once.

### Standing up DNS in one call

```sh
cixctl dns provision
```

This creates each replica from its deployment, registers it as a DNS server, and points the host's own resolver at the addresses the replicas actually got. With no flags it provisions this platform's default topology, `dns-1` and `dns-2`; name others with `--replica=NAME` (repeatable). `--no-resolver` leaves the host resolver alone.

- The replicas are created directly from their deployments, so the `dns` image must already be built: `cixctl image materialize dns` ([`images.md`](images.md)).
- It is safe to re-run. A replica that already exists counts as success, which is what makes it the right command to repeat after fixing whatever failed.
- The output reports each replica separately — created or not, registered or not, its address. The command exits nonzero if any replica had a problem, and in that case the host resolver is not changed.

The dashboard's **Services > DNS > Servers** tab has the same action as **Provision DNS**. The full semantics are in [Provisioning DNS in one call](../api/README.md#provisioning-dns-in-one-call).

### Registering a DNS server by hand

For a DNS container that is not one of the provisioned replicas:

```sh
cixctl dns server register --container=dns-3 --hosts-path=/etc/dnsmasq-hosts
cixctl dns server ls
cixctl dns server unregister dns-3
```

`--hosts-path` is the path, as the container sees it, that dnsmasq reads with `-H`/`--addn-hosts`. Registration writes the current record set there immediately. Unregistering stops the updates; it touches neither the file nor the container. The dnsmasq flags a minimal Cix image needs are in the shipped `dns-1` deployment and explained in [DNS: records + a real dnsmasq container](../api/README.md#dns-records--a-real-dnsmasq-container).

### Records

```sh
cixctl dns record create --name=db --ip=192.168.150.20
cixctl dns record update --name=db --ip=192.168.150.21
cixctl dns record ls
cixctl dns record rm db
```

A name with no dot is qualified with this install's site suffix (`db` becomes `db.<site_name>.<domain_suffix>`, [ADR-0052](../adr/0052-server-side-default-name-qualification.md)); a name containing a dot is stored as given. `update` and `rm` qualify a bare name the same way, so the short name works for all three ([ADR-0117](../adr/0117-dns-record-qualify-on-read.md)). Set the suffix with `cixctl site set --site-name=NAME --domain-suffix=NAME` ([ADR-0046](../adr/0046-site-scoped-dns-pki-naming.md)).

A container can register its own record at creation: `cixctl container run ... --dns-register`, or `"dns_register": true` in a deployment. The record is removed with the container, and a manually created record is never touched by a container's deletion ([Automatic registration](../api/README.md#automatic-registration-dns_register)). The host's own FQDN record is maintained automatically ([ADR-0053](../adr/0053-auto-maintained-instance-dns-record.md)).

### Upstream forwarders

On its own a registered server answers only for this platform's records. Forwarders give it recursion for everything else:

```sh
cixctl dns forwarders set --forwarder=1.1.1.1 --forwarder=9.9.9.9
cixctl dns forwarders show
cixctl dns forwarders set          # no flags: authoritative-only, no recursion
```

`set` replaces the whole list (at most 8 IPv4 addresses) and applies it live to every registered server; a server registered later picks up the current list at registration ([ADR-0203](../adr/0203-dns-forwarders-are-daemon-owned-config.md)).

### Who resolves through what

Three separate settings, often confused:

| Setting | Who it affects | Command |
|---|---|---|
| Forwarders | What the DNS containers ask for names they are not authoritative for | `cixctl dns forwarders set` |
| Host resolver | What the host itself (package fetches, git, `curl`) resolves through — up to 3 addresses | `cixctl resolv set --nameserver=A.B.C.D ...` ([ADR-0076](../adr/0076-host-dns-resolver-config.md)); `dns provision` sets it for you |
| Container resolver | What a container's own `/etc/resolv.conf` names | Default: the registered DNS servers that share a network with it, up to 3 ([ADR-0295](../adr/0295-a-container-in-the-directory-can-read-it.md)); override with `--dns-server=A.B.C.D` on `container run` or `dns_servers` in a deployment ([ADR-0143](../adr/0143-container-dns-servers-field.md)) |

The usual arrangement: forwarders at real upstream resolvers, the host resolver at the DNS containers, and containers left on the default. Then the host and every container resolve both internal names and the internet through one path.

## DHCP

DHCP is served by the same dnsmasq that serves DNS, but it is its own service with its own registry: registering a container for DHCP is separate from registering it for DNS ([ADR-0197](../adr/0197-dhcp-served-by-the-dns-server.md)). A container registered as both makes each lease resolvable the moment it is handed out, and `dhcp server ls` reports whether that is true (`RESOLVES LEASES`).

### Servers

```sh
cixctl dhcp server add dns-1
cixctl dhcp server add dns-2
cixctl dhcp server ls
cixctl dhcp server rm dns-2
```

Removing a server also drops it from every range that named it, and disables any range left with no server.

### A range on a network

```sh
cixctl dhcp enable --network=access --range=192.168.151.100-192.168.151.200 \
    --server=dns-1 --server=dns-2 --lease-seconds=3600 --router=192.168.151.254
cixctl dhcp show
```

- `--server=` is repeatable. dnsmasq has no failover protocol, so a range named to more than one server is split into disjoint slices, one per server in the order given; `dhcp show` prints which server holds which slice. Each server keeps serving its own slice if the other is down.
- Each named server must be registered for DHCP and attached to that network.
- The range must sit inside the network's subnet, must not cover the network's own address, and must have at least as many addresses as servers. Lease time is 60 seconds to 30 days.
- `--router=` is the default route handed to clients. The update is partial, so omitting `--router=` keeps the stored value; pass `--router=0.0.0.0` to hand out none.
- **Changing a range restarts the servers that serve it**, because dnsmasq reads ranges only at startup. Only servers whose rendered configuration changed are restarted.
- A network that reaches a real LAN will answer DHCP requests from machines that are not part of this platform.

```sh
cixctl dhcp disable --network=access   # stop serving, keep the range
cixctl dhcp remove --network=access    # delete the range entirely
```

### Reservations and leases

```sh
cixctl dhcp static add --mac=aa:bb:cc:dd:ee:01 --ip=192.168.151.50 --hostname=printer
cixctl dhcp static rm aa:bb:cc:dd:ee:01
cixctl dhcp leases
```

A reservation takes effect without a restart. The MAC must be lower-case `aa:bb:cc:dd:ee:ff`, and the hostname a plain DNS label; a MAC or address already reserved is refused. `dhcp leases` reads each running server's own lease file on every call. A lease is not a DNS record: records are durable intent, leases are short-lived state owned by the server that issued them.

The dashboard's **Services > DHCP** page has the same four tabs (Servers, Ranges, Reservations, Leases), and a network's own **DHCP** tab lists the leases on that network. Full contract: [DHCP: a self-contained service](../api/README.md#dhcp-a-self-contained-service-adr-0197).

## NTP

The host clock is synced by the daemon itself over SNTP, hourly and on demand ([ADR-0110](../adr/0110-ntp-host-clock-sync.md)). It tries registered NTP server containers first, then the configured upstream addresses.

```sh
cixctl ntp config set --server=192.168.15.1 --server=162.159.200.1   # up to 3, tried in order
cixctl ntp config show
cixctl ntp sync       # start one attempt now
cixctl ntp status     # outcome of the most recent attempt
```

`ntp config set` with no flags clears the upstream list; the host then relies on registered servers alone. `ntp sync` is refused when there is nothing to sync from, and while an attempt is already in flight.

To serve time to containers and the LAN, run `ntp-1`/`ntp-2` (chrony, from the `chrony` image) and register them:

```sh
cixctl deployment apply ntp-1
cixctl ntp server register --container=ntp-1
cixctl ntp server ls
cixctl ntp server unregister ntp-1
```

Registration is bookkeeping only: the container's address is looked up fresh at every sync, and nothing is written into it. The shipped `ntp-*` deployments carry chrony's own configuration as a staged file and grant it `CAP_SYS_TIME` with `cap_add`.

The clock can be set by hand with `cixctl time set --unixtime=N` (`cixctl time` shows it); the next sync overwrites it. Full contract: [NTP: host clock sync](../api/README.md#ntp-host-clock-sync-adr-0110).

## Syslog

Every container's stdout/stderr already lands in the consolidated log store (`cixctl logs`). A syslog target receives a copy: every container-sourced line is also sent to each registered target as an RFC 3164 UDP datagram ([ADR-0127](../adr/0127-syslog-forward-targets.md)).

```sh
cixctl deployment apply syslog-1
cixctl syslog target register --container=syslog-1
cixctl syslog target ls
cixctl syslog target unregister syslog-1
```

Forwarding is in addition to the log store, never instead of it, and a failed send is dropped — UDP syslog has no delivery guarantee. Only container-sourced lines are forwarded. Full contract: [Syslog forward targets](../api/README.md#syslog-forward-targets-adr-0127).

## Health, drain and undrain

The daemon probes every registered LDAP, DNS, NTP and syslog server on an interval ([ADR-0182](../adr/0182-registered-server-health-probing.md)). DHCP servers are not part of this view.

```sh
cixctl server-health
```

Each line gives the kind, container, `state` (`healthy`, `unhealthy`, or `unknown` before the first probe), whether it is `in_service`, whether it is drained, how it was probed, and consecutive failures with the last error. Warnings print first; a warning means the daemon is holding state it has no server to deliver to.

- `probe=tcp:PORT` is a real service check — DNS on 53, LDAP on the port its clients are given. `probe=process` means only that the container is running; it is used for NTP and syslog, where a TCP connect would prove nothing.
- One good probe makes a server healthy at once; it takes several consecutive failures to mark it unhealthy.
- A server that is not in service is withheld from the client configuration the daemon generates. If that would leave none, the full list is used instead.

To take a server out of service on purpose — for maintenance, or to test a replica's absence:

```sh
cixctl server-health drain dns dns-2
cixctl server-health undrain dns dns-2
```

`KIND` is one of `ldap`, `dns`, `ntp`, `syslog`. Unlike the probed state, a drain is persisted across daemon restarts. The dashboard's **Services** page shows the same table with a Drain/Undrain button per row. Full contract: [Registered-server health](../api/README.md#registered-server-health-issue-81).

## Where the decisions live

| Topic | ADR |
|---|---|
| Deployments (container recipes) | [ADR-0151](../adr/0151-container-recipes.md) |
| Site-scoped names, default qualification | [ADR-0046](../adr/0046-site-scoped-dns-pki-naming.md), [ADR-0052](../adr/0052-server-side-default-name-qualification.md), [ADR-0117](../adr/0117-dns-record-qualify-on-read.md) |
| Forwarders are daemon-owned | [ADR-0203](../adr/0203-dns-forwarders-are-daemon-owned-config.md) |
| Host resolver | [ADR-0076](../adr/0076-host-dns-resolver-config.md) |
| Container resolver | [ADR-0143](../adr/0143-container-dns-servers-field.md), [ADR-0295](../adr/0295-a-container-in-the-directory-can-read-it.md) |
| DHCP | [ADR-0197](../adr/0197-dhcp-served-by-the-dns-server.md) |
| NTP and the host clock | [ADR-0110](../adr/0110-ntp-host-clock-sync.md) |
| Syslog forwarding | [ADR-0127](../adr/0127-syslog-forward-targets.md) |
| Server health | [ADR-0182](../adr/0182-registered-server-health-probing.md) |
