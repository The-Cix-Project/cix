# 0388 — The forge is declared in git and reconciled through its API

## Status

Accepted, 2026-11-22, by the owner: *"Accept it as written, let's please write it in hibr"*, which sets decision 6's language. Proposed the same day. Written at the owner's request after the forge moved to gibsson-1: *"I want our git to be totally API driven so we don't have to faff around with the interface, and I want its config ... to be in a git repo"*, *"I want the sync to be independent of the repo"*, and *"please proceed now"*.
**Built the same day:** forgesync 1.0.0-3 runs as a service of the `git` deployment (git@1.1.0, gitea image 1.0.2). Measured on gibsson-1: `--export` wrote 75 repositories and 21 mirrors as the starting `forge.json`; a dry pass found nothing to change; a commit to site-config was applied in the next pass (1 change, then 0); and a setting changed through the API behind its back was put back in the following pass.

## Context

The forge is Gitea 1.25.5 in the `git` container on gibsson-1 (git.uk.home.arpa). Its own configuration (`app.ini`, the LDAP source) is already in git: the `git@1.0.0` deployment in cix-recipes, applied by cixd. Everything else the forge holds as settings lives only in its database and is changed by clicking:

- organisations and their teams;
- each repository's description, visibility and default branch;
- the 21 push mirrors to GitHub, which Gitea's migration does not carry and whose GitHub credentials cannot be read back, so the move from 192.168.15.15 lost every one of them.

That last point is the measured cost of the current state. A setting that exists only in one database is lost with that database, and it can only be recreated by someone who remembers it.

Gitea's REST API covers all of these: `/orgs`, `/orgs/{org}/teams`, `/repos/{owner}/{repo}` (PATCH), and `/repos/{owner}/{repo}/push_mirrors`. Its LDAP sources are the exception: they have a CLI and no API, which is why they stay in the deployment.

## Decision

1. **The forge's settings are declared in one file, `forge.json`, in a private repository, `itdlabs/site-config`.** It is not in cix-recipes, which is push-mirrored to the public GitHub catalogue: repository names, team membership and mirror targets are site facts, not catalogue content.

2. **A reconciler, `forgesync`, makes the forge match the file through the forge's API**, and nothing else changes those settings. It reads the file at a commit, compares it with what the API reports, and applies only the differences. It logs each change it makes. The UI stays usable, but a setting changed there is put back on the next pass. That is the point: the file is the one source of truth.

3. **What the file does not mention is left alone and reported, never deleted.** A repository created by a push, or a team added by hand, appears in the pass's report as undeclared. Removal happens only when the file marks an object `"absent": true`. The alternative, deleting whatever is undeclared, makes a typo in the file destroy a repository, and a forge's repositories are the one thing that cannot be rebuilt from anything else.

4. **The sync is independent of the forge's storage.** Push mirrors are declared in `forge.json` with their remote and interval, and recreated by `forgesync` if missing. Their credentials are named, never written: a mirror says `"credential": "github"`, and the token lives in a secret file on the forge's volume. Moving the data, or replacing Gitea with Forgejo, loses no mirror.

5. **`forgesync` runs in the `git` container, as a service of the deployment.** It runs once at start and then every 5 minutes. It authenticates with an admin token the setup service mints locally (`gitea admin user generate-access-token`) on the volume, so no forge credential is in git either. It reads `forge.json` from the forge's own copy of `itdlabs/site-config`.

6. **`forgesync` is written in hibr** (the owner, 2026-11-22), settled with the hibr maintainer the same day: its source is its own repository, `itdlabs/forgesync`, and not the hibr catalogue, which is the desktop's signed store of apps and the wrong shape for infrastructure baked into an image. It is a Cix package, a CPDL recipe in cix-recipes depending on `hibr`, that installs one script as `/usr/bin/forgesync` with a `#!/usr/bin/hibr` first line, so an image needs nothing fetched at run time. It uses hibr's `fetch` module for HTTPS and JSON, verifying the forge against the site CA through `HIBR_TLS_CAFILE` and never skipping verification, and the built-in `json`. It logs one line per change to standard error, which hibr does not buffer, and `hibr --plan` runs it as a dry run that refuses every write.

## Alternatives considered

- **Gitea's own UI, as now.** Rejected: it is the clicking the owner asked to stop, and it lost all 21 mirrors in one move.
- **Terraform with the Gitea provider.** Rejected: a large toolchain and a state file that is itself a second source of truth, beside the forge, for a few dozen objects.
- **cixd reconciling the forge.** Rejected: the forge is a workload on the platform, not part of it. Gitea's API belongs to Gitea, and cixd learning it would tie the platform to one application.
- **Deleting undeclared objects.** Rejected for the reason in decision 3.

## Consequences

- A change to the forge's settings is a commit to `itdlabs/site-config`, reviewable and revertible; the next pass applies it.
- Settings changed in the UI do not stick. Anyone using the UI to change them has to learn that.
- The first pass declares what exists today. `forgesync --export` writes the current forge as a `forge.json`, which is committed as the starting point, so nothing is lost when the reconciler starts enforcing.
- The GitHub mirrors need a GitHub token from the owner, once, stored as the `github` credential on the forge's volume.
- LDAP sources stay in the deployment, because Gitea offers no API for them.
