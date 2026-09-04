# 0240 — A container declares its consoles, and declaring none means it has none

## Status

Accepted

## Context

Raised by the owner: *"if no console executable option is configured, then no console is available... a console executable could be a tail or cat of a log file, or a bash -l. or or or"* (issue #248).

Until now a console was the same thing for every container: `exec_into_container()` was handed `cmd_argv[2] = { "/usr/bin/bash", NULL }`. Two consequences, both bad and both invisible until someone tried:

- **A container without bash got a console that opened and died.** The WebSocket upgrade succeeded — `101 Switching Protocols` — and then the exec failed. That reads as a broken daemon rather than a missing binary. Most of this platform's own images are one static binary and no shell (`dns`, `ldap`, `syslog`, `chrony`), so this was the common case, not the edge.
- **A console needing arguments could not be expressed at all.** The argv had room for exactly one string, so `tail -F /var/log/messages` or `chronyc tracking` were not merely undeclared, they were unrepresentable. The `X-Cix-Exec-Cmd` header could swap that one token, and no more.

What a useful console is depends entirely on the workload, and this daemon cannot know. It was guessing, and guessing the same way every time.

## Decision

**A container declares a named set of consoles. Declaring none means it has none, and the endpoint says so.**

```json
"consoles": [
  { "name": "shell", "cmd": ["/usr/bin/bash", "-l"] },
  { "name": "logs",  "cmd": ["/usr/bin/tail", "-F", "/var/log/messages"] }
]
```

`GET /containers/{name}/console?console=NAME` selects one; omitting the parameter attaches to the **first declared**. Order carries that meaning rather than a separate `default` flag, which could disagree with the list it points into. An undeclared name is a `404` that lists what *is* declared — the response already knows the set, so making the caller guess would be a choice, not a limitation.

**A named set rather than a single command.** This was the owner's call at the design point, over a recommendation for a single one. The reasoning for one was that every example given is a single command and that a set adds a selector to the endpoint, both clients and the recipe format. The reasoning for a set, which won: a container reasonably offers more than one view of itself — a shell *and* a live log — and the whole point of the field is that the container describes itself rather than being described by whoever attaches. Recorded because the cheaper option was genuinely available and was not taken.

**A container declaring nothing gets `409`, not a fallback.** This is the substance of the change. Attaching to an arbitrary command nobody chose is what produced the dead sessions above, and an honest "this container has no console" is more useful to an operator than a terminal that connects and shows nothing.

**`X-Cix-Exec-Cmd` is retained, unchanged, and still wins — including on a container that declares nothing.** Also the owner's call, over a recommendation to remove it. The argument for removing it was that an arbitrary-command header makes "declares no console" bypassable and the declaration therefore decorative. The argument that won is that the two answer different questions and neither substitutes for the other: `consoles` is *what this container offers* — the published surface both clients present — and the header is *let me run this specific thing*. It is not a security boundary and never was: anyone authorized to reach this endpoint can already execute arbitrary code inside the container, and the endpoint is gated as a write despite being a `GET` (ADR-0144). Removing it would have cost a real operator capability to buy an appearance of safety that was never there.

**argv[0] must be an absolute path**, refused at create time with a message saying why. It is `execve`'d directly with no shell to resolve a bare name, so a bare `bash` would fail at attach with an `ENOENT` against a binary that exists — a confusing error, far from the declaration that caused it.

**Storage.** The set lives on the registry entry, sized deliberately small (4 consoles × 8 argv entries × 128 bytes) because that struct is one of `REGISTRY_MAX_CONTAINERS` in a static array and every byte is paid 256 times. Persistence came free: ADR-0025 replays the exact create body, so adding `consoles` to the allowlist is the whole of it — the same design paying off that made KSM's `ksm` field survive restarts with no definition-store change.

## Verification

`test/test_console_exec.c` — extended rather than duplicated, since it already owns a real running-container fixture:

- the declaration is echoed back by `GET /containers/{name}` **in order**, because a client cannot offer what it cannot see, and order is what makes "first declared" usable;
- an undeclared name is a `404` that lists `first` and `second`;
- a container declaring nothing refuses with `409`, and the message names `X-Cix-Exec-Cmd` rather than refusing blankly;
- `X-Cix-Exec-Cmd` still reaches `101` on that same console-less container — the retained capability, asserted rather than assumed.

## Consequences

- **Every existing container loses its console until its definition declares one.** That is the intended behaviour and the reason the change is worth making, but it is a real cut-over: `jump` gains a `shell` console in `recipes/container/jump/1.1.0`, and `dns`, `ldap`, `syslog` and `chrony` deliberately gain none, because none of those images contains a shell and never did. Their consoles were always dead; now they say so.
- The dashboard shows a picker only when there is more than one console — a select with a single option is noise — and replaces the terminal pane with "This container declares no console" when there are none.
- `CONSOLE_DEFAULT_CMD` is retired. There is no default console command any more, which is the point.
- The CLI's `--console=NAME=/path args` form splits on spaces and so cannot express an argument containing a literal space. A container recipe is a real JSON array with nothing to lose in quoting and is the better place for anything non-trivial; the limit is documented rather than worked around with a quoting scheme nobody would remember.
