# 0131 — Host process list + kill, correlated to containers

## Status

Accepted

## Context

Part 6 (the final part) of the logging/web-UI epic. User request, verbatim: "I want a host process command with ls and kill maybe and stuff? also maybe show the process as id, command, command_line, container, user_ID, group_id, and so on? something that shows processes but also shows containers? right? something that is natural and plays nice with all we've built?"

No prior mechanism in this codebase enumerates real host processes at all — `GET /containers/{name}/stats` reports per-container cgroup usage, but nothing lists individual pids, and nothing correlates an arbitrary host pid back to the container (if any) that owns it.

The one real design question: how to determine which container (if any) a given host pid belongs to. Two candidate mechanisms, considered directly:
1. Compare `/proc/<pid>/ns/pid` (the pid namespace identity) against each known container's own namespace.
2. Walk the process's own real host ppid chain, checking each ancestor against the registry's own known container root pids.

(2) was chosen: every container's own init process is a direct `clone3()` child of `kanxeod` itself (confirmed by reading `create_container_from_body()`), so a ppid-chain walk from any process reaches either a known container root (a match) or `kanxeod`'s own pid / pid 1 (no match) — exactly the same answer (1) would give, for every case that actually matters here, without needing to open and compare namespace inodes for every candidate process on every list call.

## Decision

**New module, `daemon/src/hostproc.c`/`daemon/include/hostproc.h`** — a real, synchronous `/proc` walk (no caching, no history, same "point-in-time snapshot, call again for a fresh one" posture `GET /system/stats` and `GET /containers/{name}/stats` already have). Per process: `comm`/`ppid` from `/proc/<pid>/stat` (the standard "find the last `)`, comm is bounded by the first `(` and that last `)`" parse, since `comm` can itself contain anything including further parens), `command_line` from `/proc/<pid>/cmdline` (NUL-joined argv, space-joined for display; `"[comm]"` for a kernel thread or a process caught between `execve()` calls, matching `ps(1)`'s own convention for that case), `user_id`/`group_id` from `/proc/<pid>/status`'s own real (not effective/saved/filesystem) `Uid`/`Gid` lines, and `container` via the ppid-chain walk above (empty string if none).

**`GET`/`DELETE /v1/system/processes`/`/v1/system/processes/{pid}`** — `GET` returns the bare array (matching `GET /system/logs`'s own convention). `DELETE` is a real, immediate `SIGKILL` — no grace period, unlike container stop (which has real container-lifecycle semantics, `SIGTERM` then a timeout): this is a blunt, general host-process admin primitive, and an operator reaching for "kill" on a raw pid already means "now," not "please." Refuses pid 1 and this daemon's own real pid outright (`400`) — killing either would crash or reboot the whole host on a real installed system, where `kanxeod` runs as real PID 1 (established fact, `CLAUDE.md`). Every other pid is allowed, **including one that happens to belong to a running container**: killing a container's own init pid this way is exactly equivalent to that container crashing on its own, and the existing `SIGCHLD`-driven exit handling (`handle_container_event()`) already covers it correctly — no special case needed, confirmed by reading that handler rather than assumed.

**CLI**: `kanxeoctl process ls`/`process kill PID` — a new top-level `process` command (not folded into the existing `ps`, which lists containers, a genuinely different resource).

**Web**: a new System > Server > Processes page — fetch-on-demand (a Refresh button), not folded into the global 2s poll loop every other category view uses, since a real host's process table churns constantly and a full-table re-render every 2s would be visually noisy for a snapshot view rather than a live feed (the same reasoning the old fetch-on-demand Logs page already had, before Part 3 moved browsing elsewhere). A `container` value links to that container's own detail page. `Kill` uses a `confirm()` guard, matching this dashboard's own established convention for genuinely destructive actions (reboot/shutdown/CA regeneration), not the lighter no-confirm pattern a low-risk removal (e.g. a route) gets.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. Manual live checks against a real scratch daemon confirmed: self-protection refuses both pid 1 and (once corrected for a `sudo`-forks-a-wrapper false alarm during initial testing — the API's own reported pid, read back from a clean `GET /v1/system/processes` response and confirmed against the daemon's own real listening-socket pid via `ss`, is what must be tested against, not a shell's own `$!`) the daemon's own real pid; a normal kill genuinely terminates the target process; a nonexistent pid is `404`; a non-numeric pid is `400`. New `test/test_hostproc.c`: the daemon's own real pid (unchanged across `execve()`) is confirmed present in `GET /v1/system/processes` with `comm=="kanxeod"`; a real running container's own process is confirmed correlated to it by name; all four kill-validation cases (pid 1, this daemon's own pid, nonexistent, non-numeric) are exercised over real HTTP; a real, disposable process forked by the test itself (never a container) is killed via the API and confirmed to have actually died via `waitpid()`/`WIFSIGNALED`/`WTERMSIG`. `node --check web/app.js`. Full regression sweep (38 test binaries) confirms zero regressions.

## Consequences

- Closes the whole logging/web-UI epic (Parts 1-6) the user originally scoped in one large request.
- The ppid-chain correlation is a real, deliberate simplification: a process that re-parents itself away from its container's own process tree (double-forking, `setsid()`, being adopted by an unrelated reaper) would stop showing a `container` value, the same class of edge case any ppid-based tool (not just this one) already has. Not solved here — a pid-namespace-based correlation would close it, at the cost of an `open()`+`readlink()` pair per candidate process per list call; not judged worth the extra cost for a v1 admin convenience view.
