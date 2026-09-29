# 0315 — the public catalogue and cache are the defaults

## Status

Accepted. Directed by the owner on 2026-09-29: *"yes, both are defaults moving forwards, and can be changed if the use wants to? right?"* — in answer to whether someone who downloads the ISO can fetch recipes and artifacts with nothing configured. Amends the "no URL configured" and "unconfigured" defaults of [ADR-0121](0121-pkg-redesign-part2-configurable-repo-and-sync.md) and [ADR-0122](0122-pkg-redesign-part3-artifact-cache-and-server.md). Neither mechanism changes; only the values a host starts with do.

## Context

A freshly installed host had an empty recipe repository URL, and an empty artifact server URL. Before its first `pkg install` could use either, an operator had to know both addresses and set them. The project now publishes both:

- the recipe catalogue at `github.com/The-Cix-Project/cix-recipes`, branch `main`, public, so it needs no token;
- the artifact cache at `https://cache.cix.world`, which serves artifacts at the same root-path, arch-stamped names as the LAN cache (`/zstd-1.5.7-5-x86_64.cixpkg` answered 200 from both on 2026-09-29).

Anonymous sync from that catalogue was measured on 192.168.15.95 on 2026-09-29: `repo_kind github`, `ref main`, no token, `POST /v1/pkg/sync` ended `state=success, added=28, skipped=522`. The box's own LAN configuration was put back straight afterwards, and a sync through it succeeded too.

## Decision

**A host that has never saved a repo or artifact config starts on the public ones:**

| | Default |
|---|---|
| Recipe catalogue | `repo_url https://github.com/The-Cix-Project/cix-recipes`, `repo_kind github`, `ref main`, no token |
| Artifact cache | `base_url https://cache.cix.world`, no token, `push_enabled false` |

The values are `PKG_DEFAULT_REPO_URL`/`_KIND`/`_REF` and `PKG_DEFAULT_ARTIFACT_URL` in `daemon/include/pkg.h`, and `pkg_repo_init()`/`pkg_artifact_init()` apply them only when there is no saved file.

**A default is only a starting point.** The first `PUT` of either config writes the whole config to disk (`save_repo_config()`/`save_artifact_config()` write every field), and a saved file is read as it is from then on. An empty `repo_url` or `base_url` in it means the operator **cleared** it. A cleared source stays cleared across restarts, upgrades and a move of rebuildable storage to another disk, because the move copies the files and only repoints paths.

**Pull only, never push.** The default cache carries no token and push stays off. Publishing is an outward act an operator opts into ([ADR-0201](0201-artifacts-are-retrievable-and-self-publishing.md), #129); a default must never make a host publish.

**No automatic sync.** The default says where recipes come from, not when. A fresh host still fetches the catalogue only when asked (`cixctl pkg sync`), or when an operator adds a `pkg.sync` schedule ([ADR-0257](0257-one-scheduler-structured-schedules.md)). Whether a fresh host should also get a default schedule was left as a separate decision; [ADR-0316](0316-a-fresh-host-syncs-recipes-on-a-schedule.md) makes it.

### Consequences

- **An existing host that never saved a config gains the defaults when it upgrades.** That is the intended effect. A host that did save one, like 192.168.15.95, is unchanged.
- **A saved config file that cannot be parsed now means the public sources**, where it used to mean "nothing configured". `pkg_repo_init()` and `pkg_artifact_init()` already treated a corrupt file as no file.
- **Tests start from a cleared config.** A test daemon must not reach the internet, and in a build container it cannot (no egress). `test_data_dir_create()` therefore saves a cleared repo and artifact config into every test data directory, through the same files a clearing `PUT` writes, not a test-only switch. Tests that wipe their pkg state between runs save it again (`test_pkg_config_seed_cleared()`). `test_pkg` checks the defaults themselves against a data directory with no saved config: it reads both defaults, clears both, restarts, and checks that the clear held. `test_pkg` is in `FLOOR_SELFTESTS`, so every release runs the check.

### What this rejects

- **Writing the defaults into a config file at install time.** It would give fresh installs the same result, but every host that predates the installer change would keep an empty config forever. It would also make "never set" and "set to the public values" look identical on disk.
- **A daemon flag or environment variable to turn the defaults off for tests.** That would be a test-only switch. A cleared saved config is how an operator turns them off, and the tests use it too.
