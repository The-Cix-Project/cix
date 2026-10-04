# Rolling: what gets built, and what an image takes

"Rolling" is three separate decisions, taken in the order a new upstream release travels through them. The choice of *latest* or *one release back* belongs to the first. CBS only ever sees the second. An image's `rolling` entry is the third, and it follows the first rather than choosing for itself.

| | Decides | Where it is set | Owner |
|---|---|---|---|
| 1. Source policy | which upstream release gets **built** | `cixctl pkg source-policy` | the operator, per package |
| 2. The recipe | how that release is built | a CPDL file in cix-recipes | the recipe, one per version |
| 3. Image manifest and artifact policy | which **built** version an image takes | the image recipe, and `cixctl pkg policy` | the image, and the operator per package |

Keeping 1 and 3 apart is deliberate ([ADR-0255](../adr/0255-a-recipe-is-a-rule-not-a-version.md)): a package can roll its source while one image holds an older artifact, which is what a staged rollout needs.

## 1. Which upstream release gets built: the source policy

A recipe that declares `upstream "<kind>"` can roll. One that declares none is pinned for good, which is a first-class answer rather than a gap ([ADR-0323](../adr/0323-every-package-can-roll-discovery-authentication-and-a-green-build.md)). For a rolling package the operator chooses three things:

```
cixctl pkg upstreams                       # the discovery kinds, and the channels each offers
cixctl pkg source-policy ls
cixctl pkg source-policy set NAME [--channel=C] [--depth=D] [--pinned=on|off]
cixctl pkg source-policy set-default [--channel=C] [--depth=D]
cixctl pkg source-policy clear NAME
```

- **Channel**: the stream the kind offers. kernel.org offers `mainline`, `stable` and `longterm`; a kind with one linear sequence offers none.
- **Depth**: where to stand in that stream. The grammar is `n-<lines>.<releases>`, read literally: go back that many release lines, then that many releases within the line. Against kernel.org `stable` with 7.2.3 newest and 7.1.13 the newest of the previous line:

  | Depth | Means | Resolves to |
  |---|---|---|
  | `n` | newest release in the channel | 7.2.3 |
  | `n-0.1` | same line, one release back | 7.2.2 |
  | `n-1` | previous line, its newest release | 7.1.13 |
  | `n-2` | two lines back, its newest release | 7.0.x |

  `n-0.1` and `n-1` are spelled differently on purpose: on that channel they differ by a whole release line. A depth that cannot be satisfied is an error with a reason, never a fallback to whatever exists.
- **Pinned**: a hold. The package keeps the version it has until the hold is lifted, and `clear` keeps the hold.

The hourly `pkg.discover` schedule applies this policy to the upstream's release list, authenticates what it finds by the recipe's declared method, and hands the chosen version to step 2. `cixctl pkg source-catalogue` shows where each package's roll stands, and where it stopped and why.

## 2. How it is built: the recipe, and where CBS comes in

A CPDL recipe describes **one version** of a package, and cbs builds exactly what it says. cbs knows nothing about channels, depth or rolling. The only rolling-related thing a recipe carries is its `upstream` block, which says how releases are discovered and authenticated.

When discovery picks a new version, cixd's author stage asks cbs for `cbs revise` on the newest published recipe (`POST /pkg/recipe-revise`). That changes `version`, sets `release 1`, replaces the main source's url and sha256, removes the old `artifact_sha256`, and adds a changelog line naming the digest and how it was verified. Every other byte is kept. The new file is committed to cix-recipes first and only then published. `kernel@7.2.9-1.cbs` was written this way.

What this means for a recipe author: nothing changes. A version is written once and never edited ([ADR-0107](../adr/0107-package-image-versioning.md)); rolling adds new recipe files and never touches published ones. See [writing-recipes.md](writing-recipes.md) for the recipe itself.

The control plane rolls through the same chain: the nightly `host-roll` schedule (03:00 for 3h) hostbuilds whatever is newer of the kernel and cix, stages them together, and reboots inside the window ([ADR-0327](../adr/0327-the-host-follows-its-rolling-packages-in-a-nightly-window.md)).

## 3. Which built version an image takes

Each entry in an image's manifest has a mode ([images.md](images.md)):

- `pinned`: exactly that version.
- `rolling`: the version is a **floor**; the image takes the highest published version at or above it. Publishing a new version queues a rebuild of every image that tracks that package as rolling.

"Highest" is itself a per-package choice, the artifact policy ([ADR-0188](../adr/0188-per-package-rolling-policy.md)):

```
cixctl pkg policy ls
cixctl pkg policy set NAME --policy=highest|newest|pinned [--version=V]
cixctl pkg policy clear NAME
```

| Policy | Resolves to |
|---|---|
| `highest` | the highest version (the default) |
| `newest` | the most recently published recipe |
| `pinned` | the version named, and nothing moves it |

## Putting it together

**`n-1` is never something an image says. It is said once, at the source.** Set the kernel to `stable` with depth `n-1` and only 7.1.x recipes are written, built and published. An image that tracks `kernel` as rolling then follows 7.1.x, because that is the highest that exists. The image decides only whether, and how far, it follows what the source policy produced.

So making an image's entries `rolling` does not make it follow "latest". It follows whatever each package's source policy produces, through the artifact policy, which is `highest` unless set.
