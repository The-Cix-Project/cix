# 0324 — A host takes recipes from any number of sources, and packages from any number of repositories

## Status

Accepted by the owner on 2026-10-02, who answered each point in turn: any number of sources (*"should be able to have as many sources as they want and as many pkg repos as they want"*); the write switch belongs on a source; the public catalogue is the owner's alone *"until we find a mechanism to allow others to send packages and keep control"*; and the catalogue is signed *"as debian does"*.

It supersedes the single-repository shape of [ADR-0121](0121-pkg-redesign-part2-configurable-repo-and-sync.md) and the single-server shape of [ADR-0122](0122-pkg-redesign-part3-artifact-cache-and-server.md). It amends [ADR-0315](0315-the-public-catalogue-and-cache-are-the-defaults.md): the public catalogue and cache stay the defaults, as the first entries of each list. It moves the commit switch shipped in 0.2.57-432 ([ADR-0323](0323-every-package-can-roll-discovery-authentication-and-a-green-build.md)) from the host onto a source.

## Context

A host has exactly one recipe repository (`g_repo_url`) and one artifact server (`g_artifact_base_url`), both single values in `pkg.c`. The public defaults (ADR-0315) and 192.168.15.95's LAN overrides are just different values of the same single setting.

That cannot serve anyone beyond this site. A company running Cix wants the public catalogue *and* its own recipes, and the public cache *and* its own builds, the way a Debian machine lists several archives in `sources.list`.

ADR-0323 also makes the platform an author, and an author has to know where it may write. Two measurements make that question urgent:
- **Every commit to the owner's forge reaches the public catalogue.** `git.home.arpa/itdlabs/cix-recipes` push-mirrors to `github.com/The-Cix-Project/cix-recipes` on every commit (`sync_on_commit: true`; it pushed `cix@0.2.57-432` at 11:02, 2026-10-02).
- **The public cache serves what this site builds.** `cache.cix.world` held `cbs-v0.1.100-1` with the same sha256 as the LAN cache, five minutes later.

A commit from a host is therefore a publish to every Cix user, and "one switch for the whole host" is the wrong shape.

## Decision

### Two kinds of list, each as long as the operator wants

- **Recipe sources** say where recipes come from. Each is a named git repository on a forge: name, url, kind (gitea, github or gitlab), ref, an optional token, and a **role**.
- **Package repositories** say where built packages come from. Each is a named artifact server: name, base url, an optional token, and whether this host **pushes** what it builds there.

They are separate lists because they answer separate questions; a host may take recipes from three places and packages from five. There is no limit beyond what a host can hold.

### A source's role is read or write, and authoring follows it

A source is **read** (the default) or **write**. Write means this host may commit recipes it writes into that source (ADR-0323, git first). It is the per-source form of the `commit` switch 0.2.57-432 added to the single repo config, which this replaces before anything uses it.

**A host authors only for packages owned by a source it may write.** If every host ran discovery and wrote its own `hibr@0.91-1`, the world would hold thousands of different revisions of one version. So a source's packages are authored where that source is written, and read everywhere else.

### One package, one source, chosen explicitly

A package name is supplied by exactly one source on a host. A package offered by only one source simply belongs to it. When two sources offer the same name, the package **halts and says so**, naming both, until the operator chooses one per package. Nothing is chosen silently by list order.

This is ADR-0319's rule for paths, applied to packages. It is what keeps a site source from quietly overriding glibc: the site can override glibc, but only by an operator choosing it, visibly.

### The same version with different bytes is an alarm, never a choice

A `name@version` is one byte sequence forever (ADR-0107). If a source offers a recipe version the host already holds, with different content, the host refuses it, keeps what it has, and reports the conflict. That is true whichever source either copy came from. It is not a precedence question.

### Package repositories are mirrors; their order is speed, not trust

A built package is approved by its recipe's `artifact_sha256` and by its signature (ADR-0279), not by where it was fetched. Any repository holding the approved bytes is as good as any other. So repositories are tried in their listed order, and a copy that fails verification is refused and the next one tried. Order decides only which copy arrives first; it cannot change what is accepted.

### The public catalogue is the owner's alone

The public catalogue (`The-Cix-Project/cix-recipes`) is authored only from the owner's forge, by the owner and the owner's discovery host, and every other host reads it. Other people contributing packages while the owner keeps control, for example a pull request the owner approves, is a future decision of its own and is not designed here.

### The catalogue is signed, as Debian signs its archive

A package already carries a signature. A recipe does not: a host trusts the forge or mirror to serve exactly what the owner pushed. Following Debian's signed `Release` index, a writable source carries a signed index:
- it lists every recipe file and its sha256;
- it is signed with that source's own key, kept apart from artifact and upstream keys;
- each read source on a host is configured with its owner's public key;
- sync accepts a recipe only when the index signature verifies and the file matches its listed hash.

The key, its custody and the index format are settled when this is implemented, in its own change. The decision recorded here is that recipes get the same seal packages have.

### Only a source trusted for keys can grant artifact trust (amended 2026-10-02)

A sync does more than add package recipes. It also merges image and deployment recipes, and it adopts the signing keys a source carries in `docs/keys` into the trusted store that verifies every artifact (`releasekey_trust_adopt()`). With many sources, any source an operator added could thereby grant itself the power to vouch for packages. That was found reading the merge before implementing this ADR, and the owner decided it the same day (*"as proposed"*):

- **Each source has `trust_keys`, off by default.** A source without it may supply recipes, never keys.
- **It is on for the public catalogue** (ADR-0315's default) **and for the source a host's existing configuration migrates into.** Those are where a host is meant to get the owner's keys.
- **Turning it on for any other source is an operator's deliberate act.**

Image and deployment recipes follow the same one-name-one-source rule as package recipes. An image recipe arriving in an image whose policy follows its recipe is a deploy, so it must not arrive from a source the operator did not choose for it.

### A recipe published here has an owner from the moment it lands (amended 2026-10-02)

Ownership was first computed only from what each source's last sync offered. Implementing it showed two cases where that leaves a package with no owner, found before release by reading the code rather than on a box:
- **A recipe published here before its source's next sync carries it.** This is how every release and probe has reached 192.168.15.95 so far.
- **Every package on a migrated host until its first sync.** The migration writes the source list but no offers.

An owner decides whose token a `{{REPO_TOKEN}}` fetch carries, so in both cases the fetch would have gone out with no token. So:
- **A publish names its source, or has one implied.** A package a source already owns stays with it. A new or contested one takes the source named in the request, or the only source when the host has one, and that is recorded as the operator's choice. With several sources and none named, the publish is refused, naming them. A commit resolves its source by the same function.
- **An operator's choice decides ownership whatever the sources offer.** That includes a name no source offers yet, and a single other source that offers it later: ownership never moves on its own.
- **A migration seeds the migrated source's offers from the recipe store.** Everything there came from, or was added for, the one source the host had. The first sync replaces the seed with what git carries.

### Migration, and the defaults

There is one clean cut-over (no compatibility shim):
- `/v1/pkg/repo-config` and `/v1/pkg/artifact-config` give way to list resources for sources and repositories.
- A host's existing single configuration becomes the first entry of each list.
- A host that never saved one starts on ADR-0315's public catalogue (read) and public cache (pull only), as the first entries.

## Consequences

- **A third party** adds their own sources and repositories beside the public ones. They get byte-identical public packages, because immutability, approvals, signatures and (once built) the signed index make any copy equivalent. Their own packages come from sources only they write.
- **192.168.15.95** keeps its LAN forge and cache, with the forge marked write. That is the one host authoring the public catalogue.
- **Phase one of ADR-0323 is reordered.** Its author stage writes to a writable source, so the source list comes before discovery.
- **Not decided here:** how others contribute packages to the public catalogue, and the signed index's key and format.
