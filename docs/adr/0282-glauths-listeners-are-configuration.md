# 0282 — glauth's listeners are configuration, not recipe text

## Status

Accepted — 2026-09-12. Extends [ADR-0148](0148-glauth-basedn-daemon-managed.md); issue #419.

## Context

Which listeners a registered glauth serves — plain LDAP, LDAPS, and on which
ports — was literal TOML inside each deployment recipe's `files[]` blob:

```
"[ldap]\n  enabled = false\n  listen = \"0.0.0.0:3893\"\n\n[ldaps]\n  enabled = true\n  listen = \"0.0.0.0:636\"\n..."
```

Meanwhile `client_tls` and `client_tls_port` on `PUT /v1/ldap/config` were real
API fields. Two halves of one decision, only one of them in the API, and nothing
connecting them: `client_tls_port: 636` against a server whose `[ldaps]` section
said `9999` was an accepted configuration that handed every client a URI nothing
answered. There was no way to check, because the server's port was not knowable
from the API at all.

The cost showed up concretely in #416. Turning the plaintext listener off meant
hand-editing TOML for both replicas, publishing two new deployment versions, and
delete-and-recreating both containers one at a time. It then exposed a second
bug — the server-health probe was fixed at 3893 and reported both servers
`unhealthy` against a port that had been deliberately closed — precisely because
the daemon's idea of "the LDAP port" was a constant rather than a fact about the
server.

The daemon already owns parts of that same file. It re-renders the whole
`[[users]]`/`[[groups]]` tail on every record change, and ADR-0148 made it keep
the `baseDN` line in sync after that value drifted as a hand-typed copy. So the
mechanism existed; the listeners simply were not in it.

## Decision

**`PUT /v1/ldap/config` owns the listener configuration**, rendered into every
registered, running server's own config file as `[ldap]`/`[ldaps]` `enabled` and
`listen`. The operator's file supplies the initial value; the API is the source
of truth from the first PUT onward. Same ownership terms as `baseDN`, one step
further.

**`client_tls_port` is removed, and the client's port is derived.** There is one
port per listener; the port clients are given *is* the server's own
(`ldap_client_port()` returns `server_tls_port` or `server_plaintext_port`).
`client_tls` stops being an independent setting and becomes "which of the
enabled listeners clients are pointed at" — refused when it names a disabled
one. That makes the mismatch above unrepresentable rather than merely validated
against, which is the whole reason to prefer this over adding server fields
beside the client ones.

**`listeners_managed` gates the render.** False until an operator PUTs one of
the `server_*` fields, and while false the render touches neither section.

**Absent sections are created**, unlike ADR-0148's `baseDN` rewrite, which is a
deliberate no-op when the line is missing.

## Consequences

- The API is the one place a listener is configured, and the render puts it into
  every registered server's config file. **It does not yet take effect on a
  running server**, measured on 192.168.15.95 (v2.57.114): `[ldap] enabled = true`
  was written into both live configs and port 3893 stayed refused. Two causes,
  both verified and both outside what this ADR decided:

  1. glauth's `watchconfig` reloads the record datastore, not its listeners — it
     never binds or unbinds a socket, so a listener change needs the process
     restarted. The assumption that the watcher covered this came from record
     syncs having always worked without a signal, which is a different thing.
  2. `handle_start()` re-stages `files[]` from the persisted definition and calls
     `resync_managed_services()` *after* that, so a restarted glauth reads the
     recipe's own value and the re-render arrives too late. Verified with a real
     stop/start: the config then read `enabled = true` and 3893 was still refused.

  Both are closed. The render also runs during **staging**, before `clone3()` —
  the same ordering `pki_cert_deliver()` needed in #414, and for the same reason:
  a service reads its identity once, at startup. Every path that brings a
  container up replays that body, so a create, a restart, an autostart and a
  rolling rebuild all now stage the managed value rather than the recipe's.

  A **running** server therefore adopts a listener change when it restarts, and
  `cixctl ldap config set --restart-servers` does that one at a time, each back
  before the next is touched. **Off by default**, decided with the owner: this is
  the directory that authenticates the control plane, and a plain config PUT
  should not disrupt it. It is composed from the existing stop/start endpoints
  rather than a new one — the capability is already in the API and this is the
  client sequencing it, the same way `hostauth-config set` composes a GET and a
  PUT. No new daemon seam, and no second implementation of "how a container
  stops", which is deliberately one thing (`registry_remove()` plus the reactor
  teardown, shared by `handle_stop()`, the rolling restart and the storage
  migration).
- A recipe's `[ldap]`/`[ldaps]` stanzas become initial values that the daemon
  owns after the first PUT. `recipes/deployment/README.md` says so, because an
  operator editing TOML that gets overwritten would otherwise file a bug.
- `server_tls` cannot be turned on while a registered, running server has no
  delivered certificate: 409, naming the container. glauth exits on its next
  config reload if told to serve TLS without one, so accepting the PUT would take
  down a working server — strictly worse than refusing a setting.
- Both listeners off is refused, as is `client_tls` naming a disabled listener.
  Same rule `hostauth_set_config()` applies to `ldap_enabled`: a saved state that
  cannot work is never accepted, because it reads as correct afterwards.
- A custom bind address in `listen` is not preserved; the value is rewritten
  whole to `0.0.0.0:<port>`. Keeping the host half would make this render the
  authority on a value the API cannot express, and a half-owned field is exactly
  how `baseDN` drifted.

## Alternatives considered

**Add `server_*` fields beside `client_tls_port` and keep both.** Simpler, and
rejected: it leaves four ports and two sources of truth for the TLS port, which
is the drift this exists to end. The client port is not an independent fact.

**Default the listener state and render unconditionally.** This was the first
design and it would have taken the live deployment down. A host upgrading to this
build has no `server_*` fields in its persisted state, so they would come up as
defaults derived from nothing; autostart brings the servers up, ADR-0146's
post-autostart resync runs, the render rewrites both live configs, and glauth's
watcher applies it within seconds. `server_tls` defaulting false drops LDAPS on
both replicas and every nslcd client loses authentication; `server_plaintext`
defaulting true silently undoes #416. Either default is a guess about the server,
which is the exact class of mismatch this ADR removes. Hence
`listeners_managed`, and hence it being reported in `GET` rather than inferred —
"not managed yet" must be visible, not indistinguishable from "managed, and
happens to match".

**Strip the stanzas from the recipes entirely and render from scratch.**
Rejected on ordering: `files[]` are staged before `clone3()`, the container
starts, glauth reads its config, and only then does registration render. A
container whose config named no listeners would have a window with none, and
glauth exits rather than idling. The recipe keeps a working starting value, the
same way it keeps a `baseDN`.

**Preserve `cert`/`key` when creating `[ldaps]`.** There is nothing to preserve
when the section is being created, and no per-container knowledge to draw on:
`pki_cert_dir` is a create parameter the registry does not record. The default
written is `PKI_CONTAINER_CERT_DIR`, extracted to one definition as part of this
change because it was duplicated in `main.c` and `api_pki.c` and is now read in
a third place. An existing section's own paths are left untouched.
