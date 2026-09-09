# 0245 — The exec command is a query parameter, and the header is retired

## Status

Accepted

## Context

Raised by the owner: *"can we expose the console command on the webui and cli? it's an API right?"*

The declared-console surface from [ADR-0240](0240-a-container-declares-its-consoles.md) was already on both clients — `cixctl container console NAME --console=NAME`, and a picker in the dashboard built from `GET /v1/containers/{name}`'s own `consoles[]`. The *arbitrary command* was not. `cixctl` had `--cmd=PATH`; the dashboard had nothing, and could not have had anything, because that parameter was the `X-Cix-Exec-Cmd` **request header** and a browser's `WebSocket` constructor sets no request headers at all.

So one of the two clients could use a documented feature and the other was structurally incapable of it. Not an oversight in the dashboard — a consequence of the transport.

This project had already found this exact rule and written it down. [ADR-0242](0242-a-console-is-a-sized-terminal-of-a-declared-type.md) made `term`, `cols` and `rows` query parameters, and its own comment in `main.c` names the reason and the counter-example:

> Query parameters rather than the `X-Cix-Exec-Cmd`-style request header this endpoint already uses, for one decisive reason: a browser's own WebSocket constructor cannot set request headers at all, so a header would have silently worked for cixctl and been unreachable from the dashboard — two clients with different capabilities for the same feature.

The rule was right and was applied to the three new parameters. The parameter it was written *about* was left as it was.

## Decision

**`cmd` is a query parameter, and `X-Cix-Exec-Cmd` is removed rather than kept alongside it.**

Retiring it rather than adding a second spelling is the whole point. Two ways to say the same thing is not compatibility; it is two code paths, two documentation entries and two things that can disagree — the exact shape *No Parallel Implementations* exists to refuse. The capability is unchanged, and that matters here because **[ADR-0240](0240-a-container-declares-its-consoles.md) recorded the owner's explicit decision to keep it**, over a recommendation to remove it. That decision stands untouched. What changes is how it is spelled, so that both clients can spell it.

Three details follow from the transport rather than from taste:

- **It is percent-decoded.** The dashboard builds the URL with `encodeURIComponent()`, which escapes `/` as `%2F`, and has no choice about it. A raw path and an encoded one must mean the same thing or the split this ADR closes reopens immediately. Decoding is applied to `cmd` alone — `console` is a name, `term` is validated against a restricted charset, `cols`/`rows` are numbers, and decoding those would only change what a literal `%` means in a value allowed to hold one.
- **It must be absolute.** It goes to `execve()` with no shell and no `PATH` search, so a bare name cannot work. Refusing it with `400` says which of two indistinguishable things went wrong: an unusable request, or a broken container. Left to fail at exec, it presents as a session that opens and instantly dies — the same symptom ADR-0240 was written to stop producing.
- **It is still not a security boundary.** Unchanged from ADR-0240, and worth restating because moving it into the URL makes it look more like an ordinary input: it wins over any declaration, works on a container that declares nothing, and anyone authorised to reach this endpoint can already run arbitrary code in that container. The endpoint is gated as a write despite being a `GET` ([ADR-0144](0144-host-authentication-and-real-ldap.md)).

## Consequences

`cixctl` percent-encodes `--cmd` into the URL; `do_ws_handshake()` no longer takes a header argument at all, and nothing about the request now varies between the console and the build-log stream. The dashboard gets a **Run** box next to the console picker, and a container declaring no console keeps its terminal pane — with the picker hidden and a note pointing at Run — where it used to hide the pane outright, because a control that works should not be hidden.

`test_console_exec` asserts what the transport change actually risks: that a percent-encoded path means the same as a raw one, and that a relative one is refused with a message saying why.

The old header is gone with no fallback, per this project's standing preference for clean cut-overs over compatibility shims. Any caller still sending it gets the container's first declared console — which is the documented behaviour of a request that names no command, and is what the header now is.
