# Letting an agent use Cix through MCP

cixd can serve its API to an MCP client, such as Claude Code, as typed tools: one tool per operation the contract marks `x-cix-mcp: tool`, at `POST /v1/mcp`. The decision record is [ADR-0368](../adr/0368-cixd-serves-the-api-as-mcp-tools.md). The contract, and which operations are tools today, is in the API walkthrough: [`docs/api/README.md`](../api/README.md#the-gateway-post-mcp).

This guide connects one agent. The worked example is `agent-1`, the agent workstation ([ADR-0366](../adr/0366-an-agent-workstation-is-its-own-image-with-claude-code-on-its-volume.md)).

## 1. Turn the service on

```sh
cixctl mcp show    # off on every host until turned on
cixctl mcp on
```

While it is off, `POST /v1/mcp` answers 503 to a caller holding `mcp:call`. Turning it on or off writes an audit line naming who did it.

## 2. Give the agent its own principal

An agent authenticates as a directory user of its own, in the `cix-agents` standard group, with an app password. The group holds `mcp:call` and every `read` permission and nothing else, so an agent can read the host and reach the gateway but cannot change anything until its group is granted more ([ADR-0317](../adr/0317-permissions-are-declared-by-the-api-contract.md)). Its calls audit under its own name.

```sh
cixctl ldap group ls                              # cix-agents and its gidnumber
cixctl ldap user add --name=cix-agent --primarygroup=<cix-agents gidnumber>
cixctl ldap user app-password add cix-agent claude-code
```

The user has no login password, so it can authenticate only with its app passwords. `app-password add` prints the secret once and keeps only a hash, so put it straight into the client's configuration (step 3). An app password is revoked with `cixctl ldap user app-password rm cix-agent claude-code`, and a new one can be made at any time.

To let the agent make a change, grant its group the permission that change needs:

```sh
cixctl hostauth-permissions ls                    # what cix-agents holds now
cixctl hostauth-permissions set cix-agents mcp:call containers:read ... services:write
```

`set` replaces the whole list, so name every permission the group should keep.

### Agent levels

Give each agent its own user and build its level from groups. A user holds the union of their groups, and `tools/list` offers each agent only what that union allows. Keep `cix-agents` as every agent's primary group: the gateway and read-only. Add what a given agent may do as further groups, instead of widening `cix-agents` for all of them:

```sh
cixctl ldap group add --name=agents-full
cixctl hostauth-permissions set agents-full $(cixctl hostauth-permissions vocabulary)
cixctl ldap user add --name=cix-agent-full --primarygroup=<cix-agents gid> \
    --secondary-groups=<agents-full gid>
```

On 192.168.15.95, `cix-agent` (read-only) sees 14 of the 17 tools, and `cix-agent-full` sees all 17. A narrower level is a group holding one or two permissions, such as `services:write` for an agent that manages DNS.

Three things to know about a level:
- **A group holding every permission is an admin group** ([ADR-0369](../adr/0369-an-admin-group-holds-every-permission-including-new-ones.md)). It appears in `admin_groups` and gains any permission a later build adds. That includes `identity:write`, so such an agent can create users and app passwords and change what groups grant.
- **The finest grain is an area and a verb** (`services:write`, `containers:operate`). "DNS records but not DNS servers" is not expressible today.
- **An app password works on the REST API too.** An agent's groups are its whole reach; `mcp:call` decides only whether it may use the gateway.

## 3. Point the client at the gateway

The gateway is `https://cix.internal/v1/mcp`. cixd's certificate names `cix.internal`, which resolves through the host's DNS servers, and is issued by the Cix platform CA. The client sends the app password as HTTP Basic, `cix-agent:<secret>` in base64.

For Claude Code, `agent-1`'s deployment does this itself. `recipes/deployment/agent-1@1.2.0.json` in cix-recipes stages two files:

- `/etc/claude-code/managed-settings.json` declares the server under `managedMcpServers`. That key adds a server alongside any the operator adds, and needs Claude Code 2.1.259 or later. Its header carries a `{{SECRET:CIX_MCP_BASIC}}` token.
- `/etc/profile` exports `NODE_EXTRA_CA_CERTS=/etc/ssl/certs/cix-ca-bundle.pem`, so Claude Code trusts the platform CA.

The token is filled when the deployment is applied, so the secret is never in git:

```sh
cixctl container rm agent-1                       # apply creates; the home volume stays
cixctl deployment apply agent-1 --secret=CIX_MCP_BASIC=<base64 of cix-agent:secret>
```

The container's stored definition then carries the secret, as it already carries the LDAP bind password in `/etc/ldap-authkeys.conf`, so a system backup does too.

## 4. Check it

Inside the client's environment:

```sh
claude mcp list            # cix: https://cix.internal/... (HTTP) - Connected
```

On the host, each call the agent makes is in the audit log twice: once as the request (`cix-agent (app-password claude-code) POST /v1/mcp`), and once as the tool call with its result (`mcp: cix-agent (app-password claude-code) called listContainers (GET /v1/containers) -> 200 in 2 ms`). A call refused for a missing permission writes a warning naming the permission.

```sh
cixctl logs --source=audit
```

If `claude mcp list` reports a failure, `claude --debug mcp list` shows whether the CA was loaded (`CA certs: Appended extra certificates from NODE_EXTRA_CA_CERTS`) and what the gateway answered.
