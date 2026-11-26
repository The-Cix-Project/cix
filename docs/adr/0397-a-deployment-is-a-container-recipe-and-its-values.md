# 0397 — A deployment is a container recipe and its values

## Status

Accepted, 2026-11-26, by the owner ("Accept"), on their directions of the same day, after ADR-0396 moved deployments into the site's own repository: the reusable part is a **container recipe**, the site's part a **deployment**; container recipes live in `cix-recipes` for now (*"we can split later if needed"*); variables are *"declared in the recipe, used by the deployment"*; and gibsson-1's deployments are converted (*"conversion time!"*). Decision 8, includes, was added before implementation at the owner's request (*"I would like to put includes ... that will make it much easier for the operator to maintain that file since it will be in the correct format"*), with the same variables in scope (*"our includes will then be able to naturally utilise the same variable/scope that our recipe does"*).

## Context

A deployment today is a whole `POST /v1/containers` body. How to run a service and where this site runs it are mixed in one file:
- dns-1 and dns-2 are two near-identical copies that differ by address.
- Nothing about running dnsmasq, glauth or Radicale can be shared with another site without also sharing this site's addresses, networks and groups.

ADR-0396 put deployments in a private site repository. That made the mix visible: nothing generic is left in the public catalogue for a container, and everything site-specific is now private.

Two kinds of token already render into a deployment at apply: `{{SECRET:KEY}}`, from the apply request or kept on the box (ADR-0392), and `{{LDAP:...}}`, from the host (#66, ADR-0389). A variable is the same idea with its value written in the deployment.

## Decision

1. **Three things, three names.**

   | | What it is | Where |
   |---|---|---|
   | **Package** | software, built by CBS from a CPDL recipe | `cix-recipes`: `recipes/package/` |
   | **Container recipe** | how to run a service: image, services, files, checks, with declared variables | `cix-recipes`: `recipes/container/<name>@<version>.json` |
   | **Deployment** | one container at one site: a container recipe and the values for its variables | the site's repository: `recipes/deployment/<name>@<version>.json` (ADR-0396) |

2. **A container recipe declares its variables, and its container body uses them.**

   ```json
   {
     "description": "A DNS server: dnsmasq answering for the site's records",
     "variables": {
       "IP":      { "type": "ipv4",   "required": true, "description": "the server's address on NETWORK" },
       "NETWORK": { "type": "string", "default": "services" }
     },
     "container": {
       "image": "dns",
       "networks": [{ "name": "{{VAR:NETWORK}}", "ip": "{{VAR:IP}}" }],
       "...": "..."
     }
   }
   ```

   - The body has no `name`: the container is named after its deployment.
   - The types are `string`, `text` (multi-line), `integer`, `boolean`, `ipv4`, `cidr` and `list` (of strings).
   - A string value that is exactly one `{{VAR:X}}` becomes X's typed JSON value, so an integer stays an integer and a list stays a list. A token inside a longer string is replaced by X's text, JSON-escaped, as a secret is.
   - Publishing refuses a recipe that uses an undeclared variable or declares one it never uses.

3. **A deployment names a container recipe and gives the values.**

   ```json
   { "name": "dns-1", "recipe": "dns", "variables": { "IP": "192.168.250.101" } }
   ```

   - `"recipe": "dns"` takes the newest version; `"dns@1.2.0"` pins one.
   - Adding, committing or applying a deployment is refused, naming each variable, when a required variable has no value and no default, when a value is not of its declared type, or when a value is given for a variable the recipe does not declare.
   - Secrets and LDAP values stay tokens in the recipe and are rendered at apply, as now.

4. **One form.** A deployment is a recipe and values, never a whole container body. The whole-body form goes once gibsson-1's deployments are converted: no parallel implementation. A service only one site runs still gets a container recipe, with what is site-specific in it (a router's configuration, say) as `text` variables, whose values the deployment keeps in included files (decision 8).

5. **Apply renders, then everything after is as today.** `deployment apply` renders the recipe with the deployment's values, then secrets and LDAP values, and goes through the same create or in-place update (ADR-0392). A new container recipe version changes nothing running until the deployment is applied again. Whether a deployment should follow its recipe as an image follows its packages is a later decision.

6. **The store, the API and the code say what they mean.** Today's daemon calls a deployment a "container recipe" (`container_recipe_*()`, `container-recipes/`, a name left from before #371). Those become `deployment_*`, and the name `container recipe` goes to the new thing, in the same change: two meanings for one name is a parallel vocabulary. The box stores container recipes as it stores image recipes, one per name and version; `GET /v1/container-recipes` and `cixctl container-recipe ls|show` read them, and `pkg sync` takes them from `recipes/container/` like the other kinds.

7. **The conversion.** Each of gibsson-1's 22 deployments becomes a deployment of a container recipe: one recipe for a pair (dns, dhcp, ldap, syslog, ntp, the cores, vpn) and one for each single service. Each is checked by rendering it and comparing the result with the definition the running container was created from: identical, or the difference is shown before anything is applied. Nothing is re-applied by the conversion itself. Every config file that was an escaped `content` string becomes an included file.

8. **A file's content is an include, in its own format, rendered in the recipe's scope.**

   - **Where:** a version's included files sit in a directory beside it, named after the version:

     ```
     recipes/container/dnsmasq@1.0.0.json
     recipes/container/dnsmasq@1.0.0.d/dnsmasq.conf
     recipes/deployment/cr-1@2.1.0.json
     recipes/deployment/cr-1@2.1.0.d/bird.conf
     ```

     The version owns its files: changing one is a new version, as changing the JSON is, so what a published version means never changes.
   - **In a container recipe**, a file's content names an include in place of `content`: `{ "path": "/etc/dnsmasq.conf", "include": "dnsmasq.conf" }`.
   - **In a deployment**, a `text` variable's value may be an include: `"variables": { "BIRD_CONF": { "include": "bird.conf" } }`. That is how site-specific configuration reaches a generic recipe without being pasted into JSON.
   - **One scope.** Included text is rendered as if it were written in the recipe: the recipe's declared variables with the deployment's values, then secrets and LDAP values, in the same pass. A deployment's included value is rendered in that scope before it is inserted, so `bird.conf` may use `{{VAR:ROUTER_ID}}` and `{{SECRET:RIP_PASSWORD}}`. The publish check counts a token in an include as used, so every token it carries is declared.
   - **Bounded.** Text only, one level: an include holds no includes, its name is a plain file name inside its own `.d` directory (no `/`, no `..`), and an included value may not use another included variable.
   - **Read with its version.** A sync reads a version's JSON and its `.d` files together; a missing or unreadable include refuses that version by name and leaves what is stored as it was. A commit from the box writes the JSON and its included files in one commit (#740's commit, extended).
   - **The rule:** content someone edits as a file is an include. A whole configuration file is never an escaped string in JSON.

## Alternatives considered

- **Keep whole-body deployments and add variables to them.** Rejected: nothing becomes shareable, and the site's layout and the service's know-how stay mixed.
- **Container recipes in a separate repository.** Deferred by the owner: they sit beside packages and images for now, and splitting later is a move of files.
- **Untyped variables (text substitution only).** Rejected: an integer or a list could not be a variable, and a mistyped address would surface only when the container failed.

## Consequences

- A container recipe in `cix-recipes` is public, so it must hold no site's details. Those are variables, and their values are in the private site repository.
- A second site runs dnsmasq by writing a few lines, not by copying a body.
- Four releases or so: the rename, the container-recipe kind with rendering and includes, the deployment form and its checks, then the conversion. The whole-body form goes last.
- An operator edits `bird.conf` or `dnsmasq.conf` as the files they are, with their own syntax highlighting and diffs, and the JSON beside them stays a few lines.
