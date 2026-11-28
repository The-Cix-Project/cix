# 0399 — A container is shown in its category and its group

## Status

Accepted, 2026-11-28, by the owner ("yes"), after asking what the consequences were. The owner asked to make it clear which containers form one redundant group, *"just to demonstrate our already available data"*, and to *"categorise the machines/containers ... so we have trees and nested trees ... natural groups (redundant groups) inside categories"*, with categories following one source of truth and no parallels, and *"Always 100% api first, no compromise, if it's not an api, it cannot exist"*.

## Context

A redundant set already has one declaration: `roll_group` on a container's definition (ADR-0385), whose contract text reads "A redundant pair, such as two routers or two DNS servers, declares one group". `roll_group_of()` parses it from the stored definition body each time it is asked, to keep a group's members from restarting together. No read returns it, so nothing can show it.

Measured on a Cix host, 2026-11-28:

- **Some pairs declare a group and some do not.** Four redundant pairs declare `roll_group`. Several pairs that are pairs by name and purpose declare none.
- **Every deployment is still a whole container body.** ADR-0397's recipe-and-values form is not converted yet.
- **A re-apply restarts on any change.** `handle_deployment_apply()` stores a changed body and restarts a running container through the rolling restart ("definition changed, restarting it"), so a label added to every deployment would restart every container.
- **Nothing else holds a copy of the body that a no-restart update would leave stale.** `containerdef_set_body()` replaces the stored body; the registry mirrors neither `roll_group` nor any label; the handover record carries only what the body and the kernel cannot give back.
- **What a re-apply announces.** `applyDeployment` announces `listContainers` but not `getContainer`.
- **What the container list is made of.** `GET /v1/containers` is written from the running registry and, separately, from stopped definitions.
- **The dashboard shows containers twice.** The navigation tree's "Containers" branch lists them flat, and so does the Containers page's table.

## Decision

1. **A group is `roll_group`, and nothing else.** Container reads (`GET /v1/containers` and `GET /v1/containers/{name}`) return `roll_group` from the stored definition, running and stopped alike, `null` when none is declared. A group is never inferred from names, a shared image or a server registration: that would be a second source of truth beside the declaration. A pair that should be one group declares it, which also changes how it rolls (ADR-0385), so it is the deployment owner's change.

2. **A category is one optional field of a container's definition, `category`: a path.** For example `network/core`, `services/directory` or `edge`. Segments are separated by `/`, each 1–63 letters, digits, `_` or `-` (`roll_group`'s pattern), at most 4 deep. The path is the nesting, so there is no category registry, no category resource and no second place that says which container is in which category. A category exists while a container names it.

3. **A category is the site's, so it belongs to the deployment, never to a container recipe.** In today's whole-body form it is a field of the body, beside `roll_group`. In ADR-0397's form it is a top-level field of the deployment (`{"name": ..., "recipe": ..., "category": ..., "variables": ...}`) that apply copies into the rendered body. A container recipe that declares `category` is refused, because a generic recipe cannot know where a site files it.

4. **Clients build the tree; the API returns facts.** Each container read carries `category` and `roll_group`. The tree is: category path, then its roll groups, then their members, then ungrouped containers; a container with no category is under "Uncategorised". One dashboard function builds it, and both the navigation tree's Containers branch and the Containers page draw from it. `cixctl container ls` draws the same tree. No tree endpoint is added: device records work the same way, with a flat `parent` and the tree drawn by clients.

5. **Members of a group that name different categories are shown, not refused.** A group's members are re-applied one at a time, so refusing the difference would block the very change that resolves it. Each member is shown in its own category, and the group is marked as split.

6. **A label changes nothing that runs.** `roll_group` and `category` are read from the stored body when used, never cached in the definition's index. A re-apply whose only difference from the stored body is in those fields stores it and answers 200 `updated`, with no restart. One list in `main.c` names the fields that apply live; a field joins it only when nothing derived from it is held anywhere but the stored body. `applyDeployment` also announces `getContainer`.

7. **API first.** The field is in the contract's container body and container reads, with its pattern; `apigen` carries it to the generated form and to cixctl. Every surface reads the API, and nothing in the dashboard or cixctl holds a category or a group of its own.

## Consequences

- **The declared groups show at once.** Pairs that declare no `roll_group` show as ungrouped until their deployments declare one.
- **Categorising a site is a commit to the site's repository** (ADR-0396), adding `category` to each deployment, then re-applying it. Each re-apply answers `updated` with no restart.
- **Trade-offs, stated rather than hidden:**
  - With no vocabulary, a mistyped path silently starts a new branch. It is visible in the tree, and not refused.
  - Categories are ordered alphabetically, and a path segment is its own display name.
  - A container created directly rather than from a deployment changes category only by being created again.
- **Gates:** a daemon test, in DAEMON_SELFTESTS, that a re-apply changing only `category` answers `updated` and leaves the running container's pid unchanged; the contract gates for the new field; and a signed-in screenshot of both surfaces.

## Alternatives considered

- **Categories as their own resource** (`/v1/categories` with member lists). Rejected: membership would live in two places, the category's list and the container's definition, and they would disagree.
- **Inferring groups from names.** Rejected: a guess presented as data, and a second source of truth beside `roll_group`.
- **Tags** (several labels per container). Rejected for now: a tree needs one path per container, and a set of tags cannot be drawn as one tree without a rule choosing among them, which would be a hidden second decision.
- **A category in the container recipe.** Rejected: the recipe is generic and public; where a site files a container is the site's own fact.
