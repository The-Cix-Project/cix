# recipes/deployment/

Deployments (ADR-0151, renamed in #371): `<name>/<version>/container.json`, content is a real `POST /v1/containers` body, verbatim — the exact same JSON `POST /containers` accepts, applied via the identical code path (`POST /deployments/{name}/apply`). Directory naming and version-directory meaning are identical to [`recipes/image/`](../image/) — the version tracks the *recipe's own* revision history, a separate axis from the container's own runtime state (which has no version concept at all).

Nothing here is required for `cixd` to run; a container recipe only exists once an operator publishes one (`cixctl container recipe add NAME --file=PATH`, or `pkg sync` from a configured git repo). `dns-1`/`dns-2`/`ldap-1`/`ldap-2`/`syslog-1`/`syslog-2`/`jumpbox`/`ntp-1`/`ntp-2` (issue #18) are real, live examples captured directly from 192.168.15.95's own `GET /system/backup` container definitions, not hand-written from scratch — `jumpbox` in particular is worth reading for the `{{SECRET:LDAP_BIND_PASSWORD}}` substitution used in earnest (a real bind password had to be scrubbed out of the captured definition before committing it). A worked, simpler example, matching this project's own `writing-recipes.md` convention of showing a real one rather than describing the shape abstractly:

`recipes/deployment/echo-web/1.0.0/container.json`:

```json
{
  "name": "echo-web",
  "image": "jumpbox",
  "cmd": ["/usr/bin/bash", "-c", "cat /etc/motd; sleep infinity"],
  "restart": "always",
  "networks": ["internal"],
  "files": [
    {
      "path": "/etc/motd",
      "content": "deployed via container recipe, token={{SECRET:DEPLOY_TOKEN}}\n",
      "mode": "0644"
    }
  ]
}
```

`{{SECRET:DEPLOY_TOKEN}}` is substituted at apply time, never committed here in real form:

```sh
cixctl container recipe add echo-web --file=container.json
cixctl container apply-recipe echo-web --secret=DEPLOY_TOKEN=a-real-value-never-committed
```

See [`docs/adr/0151-container-recipes.md`](../../docs/adr/0151-container-recipes.md) for the design rationale and [`docs/api/README.md`](../../docs/api/README.md) for the full request/response contract.
