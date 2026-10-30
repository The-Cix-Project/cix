# 0366 — An agent workstation is its own image, with Claude Code on its volume

## Status

Accepted by the owner on 2026-10-30, for #378, with decision 4 blocked on cix-build-system#299. Decision 3 records the owner's decision of 2026-09-14 on the same issue.

## Context

#378 asks for a container in which Claude Code operates on Cix from Cix: on the `services` network, working from a persistent volume that can be backed up, with git and the shell toolkit, and **deliberately no compiler**. The point is that a container with no compiler cannot be talked into building anything, so every build has to go through `POST /v1/pkg/install` or a hostbuild. Anything the API cannot do from inside it is a gap the product shows by being used.

Read on 2026-10-30:

- **Node exists.** `node@24.21.0-3` (#377).
- **The tooling exists.** The `jumpbox` image already carries almost all of #378's list (git, curl, ca-certificates, bash, coreutils, grep, diffutils, python, perl, procps, iproute2, vim, tar, openssh) and no compiler. The `jump` container already runs it on `services`, with a home volume, LDAP logins and sshd.
- **`cixctl` comes only inside `cix`,** which also carries `cixd`, `cix-init`, `mkbootroot`, `cix-install`, `cix-recover`, `mkinstalleriso` and `cix-boot.efi`. CBS builds one package per recipe, and nothing in the corpus does otherwise.
- **Claude Code is not buildable from source on Cix.** The Build Provenance Mandate admits nothing into an image, a package or the cache that a Cix host did not build.

## Decision

1. **The agent has its own image, `agent`, and its own container, `agent-1`.** It is kept out of `jumpbox` and `jump` so the agent works apart from the operator's interactive shell. One image per job: the agent workstation is a job of its own.
2. **The image is Cix-built packages only, with no compiler or build system.** That means no tcc, gcc, make, binutils, m4 or autotools. Its access model is jump's, which already works: LDAP users through nslcd, and sshd with a PKI host key. On top of that it carries `node` and `cixctl`.
3. **Claude Code lives on the container's volume, never in the image** (the owner's decision of 2026-09-14). The operator installs it into their home on the volume. It is data, like the git checkout beside it: never published, never cached, never part of an image version. Its credentials live there too, so the volume's backups carry them. That makes the volume backup schedule a secret-bearing one, to be set knowingly.
4. **`cixctl` comes as a package of its own, built from the `cix` recipe's one source, and the image waits for it.** The owner chose this over taking `cix` whole and over a second recipe pinning the same tarball, the shape ADR-0363 rejected. CBS cannot yet emit two packages from one recipe, which cix-build-system#299 asks for. Until it can, there is no `agent` image and no stop-gap.
5. **The volume is `agent-home` on a disk with the `volume-storage` role**, not the data directory. A move of the data directory refuses while any volume is there (ADR-0364).

## Alternatives considered

- **Extend `jumpbox` and `jump`.** One shell image, nothing duplicated. Rejected by the owner, to keep the agent apart from the operator's shell.
- **Take `cix` whole.** No packaging work, about 22 MB of unused binaries, among them a daemon and an installer in a workload image. Rejected by the owner.
- **A `cixctl` recipe pinning the cix tarball again.** Unblocks now, and pins one source twice. Rejected by the owner, as for tcc-win64.
- **Claude Code in the image** as a fetched, checksummed payload. It would be a foreign binary in an image (the Build Provenance Mandate), and every update would cost an image version.

## Consequences

- #378 is blocked on cix-build-system#299, then on cixd learning to install a named output of a recipe.
- The agent's reach is the API's. Whatever cannot be done from `agent-1` through `cixctl` and REST is a product gap, filed as found.
- `services` must route to the host's API and to the internet for the agent to work. This is the site gateway's concern (Cix builds no NAT), measured before the container is made.
