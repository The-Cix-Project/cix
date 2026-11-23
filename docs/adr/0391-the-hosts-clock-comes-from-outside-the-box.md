# 0391 — The host's clock comes from outside the box

## Status

Accepted, 2026-11-23, by the owner. I asked whether the host should never take its time from its own NTP containers and should get a public default time source, and the owner answered *"go ahead"*. Issues #718 and #716. This supersedes the part of [ADR-0110](0110-ntp-host-clock-sync.md) that made a registered container a time source for the host. The default source is a second step, recorded under Consequences.

## Context

ADR-0110 built two things: an SNTP client that sets the host's clock from configured upstream servers, and registration of NTP containers. Its candidate list put every registered, running container first and the upstream servers after.

A Cix NTP container runs chrony with `local stratum 10` and no `server` line. It serves the clock of the container, which is the host's clock. So the host asked itself, got an answer, and reported success. Measured on 2026-11-21:

- On .95, with upstream `132.163.96.5` configured, `ntp status` read `state=ok synced_from=192.168.150.105`, which is its own ntp-1.
- On gibsson-1 the same happened with `192.168.250.105`. That box ran 8 hours fast, and nothing ever corrected it.

When a sync did fail on gibsson-1, it reported `{"state":"failed"}` with no reason in the status and no line in the log.

## Decision

1. **The host syncs only from its configured upstream servers.** A registered NTP container is never a source for the host it runs on. Registration still records which containers serve time to the network.
2. **A failed sync says why.** The status carries `reason`: no server answered within the per-server timeout (naming the servers asked), a request could not be sent, or the clock could not be set. The same sentence is logged once each time it changes, never at every periodic attempt.

## Alternatives considered

- **Keep the containers but try upstream first.** Rejected. A container answers whenever upstream does not, so a host that cannot reach the outside would still report `ok` from its own clock, which is the failure this ADR exists to end.
- **Give the containers' chrony an upstream and keep them as sources.** Rejected. The host would then depend on a container to set the host's own clock, and those containers start after the clock matters.

## Consequences

- A host with no upstream server configured has no time source and says so: `POST /v1/system/ntp/sync` answers 400 naming `PUT /v1/system/ntp`.
- The NTP containers serve the host's own clock to the network, so their time is now only as good as the host's upstream.
- **Still to do (#716):** a fresh install gets a public time source by default. It is changeable and clearable like the default package sources (ADR-0315). It needs upstream entries that are host names, resolved in a helper child rather than on the event loop, and test data directories that start with the list saved empty. Which public name to use is the owner's choice. The NTP Pool asks products that ship with a default to use their own vendor zone rather than `pool.ntp.org`.
- `test_ntp` checks that a silent server ends as `failed` with a reason naming it.
