# 0192 — One identifier prefix, and it is the project's own name

## Status

Accepted

## Context

`kx_` is the **Kanxeo** prefix — this project's name before it was thinC. It survived the rename on the compat structs (`kx_epoll_event`, `kx_bpf_*`, `kx_btrfs_*`, `kx_fsxattr`), the HTTP client (`kx_client`, `kx_response`, and every one of their functions), and a handful of utilities (`kx_mkdir_p`, `kx_write_all`, `kx_console_run`, `kx_cap_*`). 3,601 occurrences across the tree, 1,345 of them a single function: `kx_client_request`.

Meanwhile the codebase had *already* been using `thinc_`/`THINC_` for everything named since — `THINC_VERSION`, `THINC_INSTALL_BIN`, `thinc_devcg`, every test's own temp-directory template. So there were two prefixes in one codebase for one project, and the older one carried a name that no longer refers to anything.

## Decision

**`kx_` → `thinc_`, `KX_` → `THINC_`, everywhere, in one pass.**

The prefix is the one the project already uses. That is the whole argument: adopting `thinc_` does not introduce a convention, it finishes applying the one that was already there. A codebase with one prefix has one answer to "what do I call this"; a codebase with two has a question.

`tc_` — the shorter candidate the issue suggested — was **not** taken, for a reason specific to this project rather than to taste. `tc` is the name of the Linux traffic-control subsystem, in a codebase that ships its own networking data plane (`netplane/`), and it also reads as Tiny C in a codebase whose only compiler is TCC. A prefix that invites two wrong readings on a project that contains both of those things is a poor prefix, and four saved characters do not pay for it.

Historical records are left alone. `CHANGELOG.md` says what the code was called when each entry was written and is not rewritten; the ADRs that name these identifiers *are* updated, because they document mechanisms that still exist and a reader following one to a symbol should find it.

## Consequences

`git blame` on the renamed lines now points here rather than at the commit that last changed the logic. That cost is paid once, and it is why this was done as its own commit touching nothing else: a reviewer can verify the whole change by reading the diff for anything that is not the prefix.

Verified by the compiler and the suite rather than by reading 3,601 sites: a full `-Wall -Werror` build (a missed rename is an undeclared identifier, not a silent behaviour change — no `kx_` name was ever a string literal or a preprocessor-assembled token, both checked before starting) and the full regression suite, 62/62.
