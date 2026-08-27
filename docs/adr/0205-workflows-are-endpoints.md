# 0205 — A workflow is an endpoint, not a client-side sequence

## Status

Accepted. Applies the API-First Mandate (`CLAUDE.md`) to multi-step operator workflows; supersedes nothing.

## Context

`cixctl dns provision` was added as a convenience: one command to stand up DNS instead of four. It worked, and it was implemented entirely in the CLI — apply each container recipe, register each replica as a DNS server, read each container's address back, write the host resolver. Four REST calls, sequenced in C, in the client.

That is a direct violation of the mandate this project already holds: *"Every capability either surface offers must first exist as a REST endpoint; a CLI or web feature with no corresponding endpoint is not allowed to exist."* It was caught by an ordinary question — whether the recent work was reflected in the CLI and the dashboard — not by anything structural. Nothing in the build, the tests, or the review process notices a CLI command that talks to four endpoints instead of one.

The cost is not theoretical, and it is worth being precise about what it actually is:

- **The dashboard cannot offer the same button without writing the sequence a second time**, in JavaScript. Two implementations of one workflow, guaranteed to drift — the exact shape "No Parallel Implementations" exists to prevent. The violation creates the parallel implementation the moment anyone tries to reach feature parity.
- **The ordering and the error handling *are* the feature.** Register before the container is running and records never arrive. Set the resolver when a replica failed and the host is worse off than before. That knowledge lived in a CLI binary, where no other client could reach it.
- **A hardcoded `/etc/dnsmasq-hosts` string literal sat in the CLI**, a second copy of a value the daemon owns.

The general form: when a workflow is only ever expressed as a client-side sequence, the platform does not actually have the capability. It has a client that knows how to fake it.

## Decision

`POST /v1/dns/provision` owns the workflow. `cixctl dns provision` becomes argument parsing and output formatting over exactly one call.

Three properties fall out of moving it server-side, and each is a decision rather than an accident:

- **Re-runnable.** A replica that already exists is `"created": "exists"` and counts as success. Provisioning is what an operator retries after fixing a failure, so a call that refused because half its work was done would be useless precisely when it is needed.
- **Per-replica results, `207` when any step fails.** "Created but not registered" and "registered but the resolver was not updated" are different states to be stranded in. One overall status would hide which.
- **The host resolver is written only when nothing failed**, because a resolver list missing a replica leaves the machine worse off than before the call.

Implementing this required splitting `handle_create()` into a non-responding `create_container_persisted()` plus a thin HTTP wrapper. That split is the point: the endpoint creates containers through *the same* implementation as `POST /v1/containers`, not a second one, and emphatically not by making HTTP calls back to itself.

## Consequences

The dashboard can now offer DNS provisioning as one button against one endpoint, with no logic of its own — which is what the mandate always intended and what the CLI-side version made impossible.

`create_container_persisted()` is now the single place a container gets created and persisted, available to any future workflow endpoint that needs one. That is a small refactor with a large implication: it removes the main practical excuse for building a workflow client-side, which was that the daemon had no reusable way to create a container as part of something larger.

**The general rule this sets: a multi-step operator workflow is an endpoint.** If a client finds itself sequencing calls and interpreting intermediate failures to deliver one user-visible action, that is a missing endpoint, not a clever client. Worth checking for directly — nothing detects it automatically, and this one survived review until someone asked whether the dashboard had caught up.
