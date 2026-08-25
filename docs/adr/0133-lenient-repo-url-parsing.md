# 0133 — Lenient `repo_url` parsing (only owner/repo, rest ignored)

## Status

Accepted

## Context

User-reported: `pkg sync` failed with `state: failed`, `curl exit 6` (`CURLE_COULDNT_RESOLVE_HOST`) at first — that half was a real, separate `resolv_set()` bug (ADR-0132). Once DNS resolution was fixed, the *same* sync attempt failed differently: `curl exit 22` (an HTTP error response), against a `repo_url` of `https://git.home.arpa/itdlabs/cix/src/branch/master/pkg/recipes`.

`parse_repo_url()` (`daemon/src/pkg.c`) documented its own expected input as `<scheme>://<host>/<owner>/<repo>[.git][/]` and split owner/repo at the **last** `/` in the path. Against the URL above, the path is `itdlabs/cix/src/branch/master/pkg/recipes` — the last slash falls between `pkg` and `recipes`, so `owner` became the entire garbage string `itdlabs/cix/src/branch/master/pkg` and `repo` became `recipes`. The resulting gitea API fetch URL was nonsense, and gitea correctly 404'd it — reported to the operator as an opaque `curl exit 22` with no hint that the *URL itself*, not the network or the target repo, was the problem.

The URL the user pasted is exactly what a browser's address bar shows while browsing a gitea repository (`.../owner/repo/src/branch/<ref>/<path>`) — an entirely natural thing to copy directly, not a mistake specific to this user. `parse_repo_url()`'s own documented contract only ever promised to need `owner/repo`; it just didn't defend itself against a caller supplying more.

## Decision

`parse_repo_url()` rewritten to take the **first two** path segments as owner/repo and discard everything after, instead of splitting at the last slash. A bare `owner/repo`, `owner/repo/`, `owner/repo.git`, and `owner/repo/src/branch/<ref>/<path>` (or any other trailing suffix) all now resolve to the identical, correct `owner`/`repo` pair. This is a real behavior change, not just a bug fix dressed up as a design decision: the module now deliberately accepts a broader, more forgiving input shape than its own prior contract promised, because the realistic failure mode (an operator pasting a browse URL) is common enough, and the prior behavior's failure was silent enough (a confusing 404 two layers removed from the actual mistake), that leniency is the better default here. The existing, narrower "gitlab subgroup paths are unsupported" boundary (ADR-0121) is unchanged — a subgroup path still folds its own first segment into `owner` and fails cleanly at fetch time, a known, flagged v1 gap, not something this change tries to also solve.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. `test/test_pkg_sync.c` gained a new scenario: the same stand-in repo already proven reachable via a bare `owner/repo` URL (scenario 4) is re-pointed at via a URL with a real browse-style suffix appended, and `pkg sync` is confirmed to still resolve it to the *same* repo (`skipped=1`, not a fresh `added=1` or a failure) — proving the parse, not just that some fetch happened to succeed. Full regression sweep (38 test binaries) confirms zero regressions. Live-verified on 192.168.15.95: the exact `repo_url` the user had configured (now updated to the bare form as an immediate workaround, then re-tested with the original browse-URL form to confirm the fix directly) both sync successfully once this build was deployed.

## Consequences

- `repo_url` is now genuinely tolerant of the single most likely real-world paste mistake, closing a gap that previously required an operator to already understand this daemon's own internal parsing convention to avoid.
- A URL is never rejected for having *too much* path — only for missing a host or a second path segment entirely (`owner` with no `repo` at all), which still `400`s clearly.
