# recipes/container/

Container recipes (ADR-0151): `<name>/<version>/container.json`, content is a real `POST /v1/containers` body, verbatim — the exact same JSON `POST /containers` accepts, applied via the identical code path (`POST /containers/recipes/{name}/apply`). Directory naming and version-directory meaning are identical to [`recipes/image/`](../image/) — the version tracks the *recipe's own* revision history, a separate axis from the container's own runtime state (which has no version concept at all).

This directory starts empty in a fresh checkout — nothing here is required for `thincd` to run; a container recipe only exists once an operator publishes one (`thincctl container recipe add NAME --file=PATH`, or `pkg sync` from a configured git repo). A worked example, matching this project's own `writing-recipes.md` convention of showing a real one rather than describing the shape abstractly:

`recipes/container/echo-web/1.0.0/container.json`:

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
thincctl container recipe add echo-web --file=container.json
thincctl container apply-recipe echo-web --secret=DEPLOY_TOKEN=a-real-value-never-committed
```

See [`docs/adr/0151-container-recipes.md`](../../docs/adr/0151-container-recipes.md) for the design rationale and [`docs/api/README.md`](../../docs/api/README.md) for the full request/response contract.
