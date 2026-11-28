# 0400 — A crate is a declared source

## Status

Accepted, 2026-11-28, by the owner ("yes lift the cap (I'm not sure why there is a cap)"), choosing it over a lockfile source kind in CBS. Built for #771.

## Context

A build container is sealed: no egress, and nothing of the host's filesystem (CLAUDE.md, "A build container is SEALED"). Everything a build reads arrives as a declared source, which cixd fetches host-side, verifies against the recipe's sha256 and hands to cbs by that checksum (ADR-0305, ADR-0382).

A Rust application builds against the crates its `Cargo.lock` pins, and `cargo` fetches them from crates.io unless a vendored directory replaces the registry. Measured on 2026-11-28 from each project's own `Cargo.lock`:

| Application | Registry crates | Git dependencies |
|---|---|---|
| vaultwarden (main) | 638 | 0 |
| tuwunel (main) | 566 | 25 |
| granian 2.8.9 (SearXNG's server) | 185 | 3 |

Each lock entry carries the sha256 of its `.crate` file, which is exactly the checksum a declared source needs.

cixd refused any recipe with more than 32 sources. The sources were two fixed arrays inside `struct pkg_recipe`, 577 bytes each, and that struct lives on the stack of about 25 functions, so the cap was a stack-size budget, not a judgement about recipes. `cbs explain` output was also read into a 64 KB buffer, which fills near 200 sources. And a recipe over the cap was committed to git and published before cixd refused it at parse (#771): `probe-pylayout@1-1`, with 38 sources, sat in git while the box could not load it.

## Decision

1. **Every crate is an ordinary declared source**: its `.crate` url on crates.io and the sha256 from `Cargo.lock`. A git dependency is a source too, as the forge's archive of the pinned commit. The recipe assembles cargo's vendor directory and its `replace-with` configuration from them, and builds offline. Nothing new is added to CPDL or to the fetch: mirrors, the source cache and verification apply to a crate exactly as to a tarball.

2. **A recipe's sources leave `struct pkg_recipe`.** It keeps the count and the path of the recipe's explain document. `pkg_recipe_source()` reads a source's url and sha256 from that document, through a cache of four heap lists, and copies them out. Nothing holds a pointer into a list, so copying the struct copies nothing owned, and an eviction leaves nothing dangling. A url is returned with the owning source's token only when the caller asks for it, which only the fetch does. So the url a log line prints never held the token (#461).

3. **The ceiling is 4096 sources**, sized for the measured applications with room, and is a refusal, not an allocation. The `cbs explain` buffer is 4 MiB, heap-allocated per explain. A recipe over the ceiling is refused when it is published, before a commit writes it to git (#771).

## Consequences

- SearXNG's granian and pydantic-core, Vaultwarden and Tuwunel can build on a Cix host, with every crate verified like any other source.
- A Rust application's recipe is long and machine-generated from its `Cargo.lock`. That is the price of declaring every byte. The generator is a step in writing the recipe, not part of cixd.
- A crate shared by two applications is fetched once: the source cache keys by checksum (ADR-0382).

## Alternatives considered

- **A lockfile source kind in CBS**: the recipe declares only the pinned `Cargo.lock`, and the crates are derived from it. The recipes would be short. But it is a new CPDL feature, plus a fetcher in cixd that understands Cargo's format, and so a second way for bytes to enter a build. It may still come, as a CBS ticket. A recipe written this way would then be the input to it, not something it replaces.
- **Raising the fixed arrays to 1024.** That puts about 590 KB on the stack of about 25 functions, one of them static. Refused.
- **One vendored tarball per application, prepared outside a Cix host.** That is a foreign artifact in the build (Build Provenance Mandate). Refused.
