# 0378 — A rolling image's dependencies roll with it

## Status

Accepted by the owner on 2026-11-13. Asked how the control-plane root's libraries should follow once it is one image (ADR-0371), the owner chose: *"Drain upgrades deps — A rolling image's dependencies follow too: when a newer version of an installed dependency is published, the drain upgrades it, so every image's libraries stay current, not only cix-root's."* The other choices were to have cix-root's recipe name every library it carries, or to leave libraries to move only when something names them.

- It extends [ADR-0107](0107-package-image-versioning.md)'s rebuild queue and [ADR-0323](0323-every-package-can-roll-discovery-authentication-and-a-green-build.md)'s rolling drain from manifest entries to the dependencies they bring.
- It keeps [ADR-0320](0320-an-image-policy-decides-how-its-three-copies-agree.md)'s meaning of a pin: a pinned entry holds the named package's version, and nothing more.

## Context

An image's manifest names the packages it wants, each `rolling` or `pinned`. Installing one also installs its runtime dependencies, and those are not manifest entries. The rebuild queue knew only entries:

- publishing a version queued the images whose manifest names that package (`queue_rolling_rebuilds_for()`);
- the drain checked each manifest entry against the newest recipe (`pkg_try_start_queued_rebuild()`);
- the startup and post-sync re-derivation checked the same entries (`pkg_rebuild_queue_rederive()`).

A dependency was installed once, when first needed, and never moved again. The chain table said so in its own comment: "dependencies are only ever fresh-installed if missing, never auto-upgraded as a side effect".

Measured on 192.168.15.95 on 2026-11-13, with `pkg ls` read against each image's manifest:

| image | rolling entries | dependencies behind their newest version |
|---|---|---|
| cix-hosttools | 17 | libblkid, zstd, libarchive, ncurses |
| router | 4 | openssl, ncurses |
| wifi_router | 3 | openssl |
| agent, jumpbox, kernel-builder | 7, 6, 27 | zstd |

cix-root, which ADR-0371 makes the control-plane root, held openssl, curl, xz, ncurses and libblkid one revision behind until they were upgraded by hand that day. Once the root is that image, an openssl fix would never reach the control plane unless someone moved it.

## Decision

**In an image with at least one rolling manifest entry, an installed package that is not a manifest entry, and that an installed package of the image depends on, follows the newest published version, as a rolling entry does.**

- **What a dependency is.** A package installed in the image that is not one of its manifest entries, and that some installed package of the image names in its recorded `depends`. A package nothing names is not a dependency, so the rule never moves a stray, which converge removes (ADR-0330).
- **Which images.** One with at least one rolling entry. An image whose entries are all pinned is held whole: gcc-tcc-bootstrap, glibc-hold and the probe images stay exactly as they are.
- **A recipe's own pin holds.** A `depends` entry recorded as `name@version` holds that dependency where it is. That is the recipe's declaration, and the drain never overrides it.
- **A pin means what it meant.** A pinned manifest entry holds its own version, not its dependencies'. ADR-0320 set pins that way, and every pin on .95 was set with that meaning.
- **One test for "behind":** `pkg_entry_drift()`, which `GET /v1/pkg/drift` and the available-version column already use.

**Where it applies:**

1. `queue_rolling_rebuilds_for()` also queues an image in which the published package, or an output of its recipe, is a dependency that follows.
2. `pkg_try_start_queued_rebuild()`, once an image's entries need nothing, upgrades its dependencies that follow and are behind, one job at a time. The same in-flight deferral applies (#555, #681), and the same refusal logging. The install goes through `pkg_install_start()`, so the ABI hold (ADR-0373), the sibling rule (#678) and the path rule (#682) apply unchanged.
3. `pkg_rebuild_queue_rederive()` queues an image whose only stale packages are dependencies, or a sync would never bring it back.

## Alternatives

- **cix-root names every library.** No daemon change. But the list is kept by hand, which is what ADR-0371 set out to remove, and every other image stays where it is.
- **A pin holds everything it reaches.** Rejected. It invents a meaning ADR-0320's pins never had. agent's pinned bash would freeze libraries nobody meant to freeze, and the rule would cost a closure walk per image per publish.
- **Every dependency always follows, even in an all-pinned image.** That would break the one way an operator holds an image whole today.

## Consequences

- The first drain pass after the release upgrades the dependencies in the table above. Follower containers of those images restart once, through ADR-0323's probation: agent-1, jump, cr-1, cr-2 and ar-1. vpn names openssl as an entry and is not among them.
- router and wifi_router move to openssl 3.0.20-9, and router to ncurses 6.6-13. Neither declares the tool outputs, so they lose the openssl command and the terminfo database (ADR-0371 decision E). No deployment runs either.
- base has no rolling entry, so its glibc 2.44-19 and its other packages stay where they are.
- A dependency's upgrade is a rolling job like any other: a failed build moves nothing, and a refused install is logged and drops the image from the queue (ADR-0330).
