# A cut-over invalidates assumptions held by code it did not change

A retrospective on the day ADR-0260 landed. The design was right, the
implementation passed a gate that had already refused four revisions for real
faults, and the deploy was verified live across eleven containers. Then the very
next thing anyone asked the platform to do — build a package — failed, and had
been failing for every build since the release shipped.

This document exists because the three bugs that followed are the *same* bug
wearing three costumes, and the shape is worth more than any of the fixes.

## The pattern

**ADR-0260 changed what a container is. Every one of the three failures came
from code that was not changed, holding an assumption that used to be true.**

| # | The code that broke | What it assumed | When that stopped being true |
|---|---|---|---|
| 1 | `container_create()`'s `capture_output` tidy-up | "after dup2 onto 1 and 2, nothing else needs `stdout_fd`" | a build container's capture pipe became the fd `cix-init` is told to keep |
| 2 | `pkg.c`'s own exit-code table | "I know every code `container.c` can emit" | `container.c` gained exit 138 |
| 3 | `test_cleanup.c`'s two-second budget | "a deleted container is gone at once" | `registry_begin_kill` became `SIGTERM`, not `SIGKILL` |

None of the three lines was touched by the cut-over. Every one of them was
*about* something the cut-over redefined. A grep for the changed symbols would
have found none of them, because none of them mentions a service, a supervisor,
or `cix-init`.

## What each one actually cost

### 1. No package could be built, and it shipped

A build container has exactly one service whose output *is* the build log the
daemon already holds, so `init_transport_open()` is handed that write end as the
shared output fd rather than making a pipe per service. It is therefore
simultaneously `spec->stdout_fd` and a member of `spec->keep_fds`. The pre-exec
setup dup2'd it onto 1 and 2 and then closed the original — correct and harmless
for years — so the `fcntl(F_SETFD)` loop that clears close-on-exec for
`cix-init`'s descriptors ran against a closed fd. `EBADF`. Every build on the
platform, before a byte of output.

Ordinary containers were untouched: they get a pipe per service, so their capture
fd is a different descriptor and the close is still right. **That is exactly why
it survived verification** — eleven containers were checked running and ready,
`birdcl show protocols` on both routers, a real SSH banner from the jump host.
Everything a container could be asked to do worked. Nothing asked the daemon to
build.

### 2. The diagnostic was rewritten into a fiction on the way out

`pkg.c` carried its own copies of `container.c`'s exit-code tables. ADR-0260
added exit 138 to the real one and not to the copy, so `pkg.c` fell through to
its "128+N means killed by signal N" branch. 138 is also 128+10.

The daemon reported `build killed by signal 10`. There is no signal 10 here. One
second earlier, from the same event, the container's own line said:

    child: fcntl(keep_fds, F_SETFD): Bad file descriptor

A precise, correctly-produced diagnostic, and a fictional one printed next to it,
because two tables described one thing. The copy also had a latent bug of its
own that nobody had hit: its overlay branch (130–136) had no output guard, so a
recipe shell killed by `SIGKILL` would have been reported as an overlay mkdir
failure.

### 3. Three release cycles spent on arithmetic

`test_daemon_net` failed intermittently on `n7` — one container told to stop,
never reported as exited, every sibling gone in under a second. It was read as a
race twice and chased as one.

It was not a race. `test_cleanup.c` allowed ten passes at 200 ms — two seconds —
for a deleted container to disappear. The daemon's own
`container_stop_grace_seconds()` is `max(10, longest stop_timeout) + 5`: fifteen
seconds. The test was asserting a promise the platform had deliberately stopped
making, and the intermittency was the tell — it failed only on a container still
*running* a service when deleted, never on one whose service had already exited.
`n7` is created and deleted milliseconds apart, so `n7` is the one that catches
it.

## Why the gate did not catch any of it

The selftest gate is good. It refused four revisions of this same change, each
for a real fault, and none of them reached the box. It could not catch these
three:

- **1 was invisible to it** because a build container is created by the build
  pipeline, and the tests that would exercise that path cannot run inside a build
  container (#224) — they need `mount()` and a cgroup.
- **2 was invisible to it** because nothing asserts that two tables agree; that
  is what having one table is for.
- **3 *was* the gate**, misfiring, and the cost of a gate that flakes is not the
  re-run — it is that the next person re-runs instead of reading.

## The one that generalises

**A release that changes the build path is itself built by the daemon it
replaces.** So the first real exercise of new build code is always the build
*after* the one that ships it. `v2.55.24` was compiled by `v2.55.17` and worked
perfectly; `v2.55.24` could not compile anything.

That is a property of self-hosting, not of ADR-0260, and it will be true of every
future change to that path. It is now written into
[docs/guides/remote-development.md](../guides/remote-development.md) as the last
step of a deploy, with `recipes/package/probe-selftest-one/` as the ~30-second
check that runs it.

## Recovery, and the one thing that would have made it unrecoverable

The box could not build its own fix. Recovery was API-only and needed no console:

1. Delete the containers whose declarations the older daemon predates.
2. Install a previous version whose artifact is already in the cache — **a cache
   hit does not need a working build**, which is the fact the whole recovery
   turns on.
3. Let the assembly run, `POST /system/update`, reboot.
4. Build the fix there, deploy it, recreate the containers.

Step 1 is not tidiness. A daemon that cannot load its own persisted state is the
single failure the API cannot recover from, and the rolled-back daemon predated
`services[]` entirely.

The recovery script itself nearly caused the outage it was written to prevent: it
carried a hardcoded check for the version it was first written against, and four
revisions since had edited only its sibling. Had it run as written it would have
halted immediately after the reboot with all eleven containers down and nothing
bringing them back. **When a deployment's recovery step can refuse to run, the
refusal is the outage.** A guard protecting a destructive action must be bumped in
the same edit as the thing it guards, or must derive the value it checks rather
than restate it.

## What changed as a result

- A kept descriptor is never closed by the tidy-up next to it.
- `pkg.c` decodes through `container_decode_exit_status()`. One table.
- The cleanup budget is derived from the daemon's documented grace, not from
  behaviour that was replaced.
- Every deploy now ends with a real package build, not just a health check.
- ADR-0260 records the shared-descriptor coincidence as a consequence of the
  design, so the next person to touch that path finds it stated rather than
  discovering it.

## What to take from this

When a change redefines something fundamental — what a container is, what a stop
means, who owns a descriptor — the risk is not in the diff. It is in every place
that *depended* on the old definition without naming it. Those places do not show
up in a diff, do not fail to compile, and in two of these three cases did not
fail a test either.

The only reliable way to find them is to ask what the change *promised* that is
no longer true, and then go looking for who was relying on that promise. All
three of these were findable that way, in advance, and none of them was found
that way.
