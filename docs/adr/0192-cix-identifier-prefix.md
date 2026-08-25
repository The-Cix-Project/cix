# 0192 — One identifier prefix, and it is the project's own name

## Status

Accepted

## Context

A short legacy prefix, inherited from a name this project carried before its current one, survived an earlier rename on the compat structs (the `epoll`/`bpf`/`btrfs`/`fsxattr` replacements), the whole HTTP client (its request and response types and every one of their functions), and a handful of utilities (`mkdir_p`, `write_all`, `console_run`, the capability helpers). 3,601 occurrences across the tree, 1,345 of them a single function: the client's own request call.

Meanwhile the codebase had *already* been using the project's own current prefix for everything named since — `CIX_VERSION`, `CIX_INSTALL_BIN`, `cix_devcg`, every test's own temp-directory template. So there were two prefixes in one codebase for one project, and the older one carried a name that no longer refers to anything.

## Decision

**One prefix, `cix_`/`CIX_`, everywhere, in one pass — the project's own name, and the one the codebase already used.**

The prefix is the one the project already uses. That is the whole argument: adopting `cix_` does not introduce a convention, it finishes applying the one that was already there. A codebase with one prefix has one answer to "what do I call this"; a codebase with two has a question.

`tc_` — the shorter candidate the issue suggested — was **not** taken, for a reason specific to this project rather than to taste. `tc` is the name of the Linux traffic-control subsystem, in a codebase that ships its own networking data plane (`netplane/`), and it also reads as Tiny C in a codebase whose only compiler is TCC. A prefix that invites two wrong readings on a project that contains both of those things is a poor prefix, and four saved characters do not pay for it.

Historical records are left alone. `CHANGELOG.md` says what the code was called when each entry was written and is not rewritten; the ADRs that name these identifiers *are* updated, because they document mechanisms that still exist and a reader following one to a symbol should find it.

## Consequences

`git blame` on the renamed lines now points here rather than at the commit that last changed the logic. That cost is paid once, and it is why this was done as its own commit touching nothing else: a reviewer can verify the whole change by reading the diff for anything that is not the prefix.

The prefix survived the later rename to Cix unchanged in spirit — it is still exactly one prefix, and it is still the project's own name (see [ADR-0200](0200-rebrand-to-cix.md)).

Verified by the compiler and the suite rather than by reading 3,601 sites: a full `-Wall -Werror` build (a missed rename is an undeclared identifier, not a silent behaviour change — no renamed identifier was ever a string literal or a preprocessor-assembled token, both checked before starting) and the full regression suite, 62/62.
