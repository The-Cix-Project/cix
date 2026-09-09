# 0227 — Responses are buffered against their connection and drained on EPOLLOUT, never written blocking

## Status

Accepted

Issue [#237](https://git.home.arpa/itdlabs/cix/issues/237). Decided 2026-09-01.

## Context

`cixd` is a single-threaded `epoll` daemon. Until this change it wrote
every HTTP response by putting the client socket back into **blocking**
mode (`http_set_blocking()`, called by `respond_json()` and by
`static_serve()`) and then writing the whole response inline, inside the
event loop, via `tls_write_all()`.

That made the entire control plane hostage to its slowest reader. A
client that issued an ordinary GET and then simply stopped reading held
every other request off for as long as it liked. No authentication was
required, and no misbehaviour was needed beyond being slow.

Measured on 192.168.15.95 running v2.14.0, with one client requesting
`GET /v1/pkg` (1,798,212 bytes on that host) and never reading it:

```
baseline            health=200 in 7ms / 11ms / 12ms
slow client open    health=000 in 5012ms  (8 consecutive samples, 8s)
slow client closed  health=200 in 14ms / 14ms / 15ms
```

Recovery on socket close was immediate, which rules out every other
explanation.

This was the real cause of a series of episodes recorded as "the box
hung and had to be reset". Considerable effort had gone into the
package-rebuild queue and build-environment composition first, on the
theory that a build was blocking the loop. Those were real costs and
worth reducing (#236), but they were not this. The evidence that would
have pointed here was already being collected: the stall store had been
recording

```
{"event":"stall","state":"S","wchan":"wait_woken","activity":"GET /v1/pkg"}
```

for some time. `wait_woken` in state `S` is a socket wait, not disk and
not CPU, and `GET /v1/pkg` is not a build. The records named the cause
and were read as a symptom of something else.

Worth stating plainly: the daemon's own **web dashboard** polls
`/v1/pkg`. On a phone or a slow link, the operator's browser was the
slow reader. The thing wedging the control plane was the thing built to
observe it.

## Decision

**The event loop never blocks on a peer.**

A response is written into a per-connection buffer (`struct conn`'s
`out_buf`) rather than to the socket. The connection then:

1. attempts one non-blocking drain immediately -- which is what happens
   in the overwhelmingly common case, where the whole response fits in
   the socket buffer and the connection closes exactly as before;
2. otherwise registers for `EPOLLOUT` and is finished across later
   events, while the loop goes on serving everyone else;
3. is dropped if it makes **no write progress at all** for
   `CONN_OUT_DEADLINE_SECONDS` (30), so a peer that has genuinely
   stopped cannot pin a connection and its buffered response forever.

The deadline is measured from the last time bytes actually moved, not
from the start of the response. A client on a slow link keeps resetting
it and is never punished for being slow; only one that has stopped
reading is closed.

Three supporting pieces:

- `tls_write_some()` (`daemon/src/tlsconn.c`) is the non-blocking
  counterpart of `tls_write_all()`. The TLS case is why this is a real
  function and not an inline `write()`: OpenSSL requires that a
  `WANT_WRITE` retry repeat byte-identical arguments, so the drain
  offers a deterministic slice of a stable buffer.
- `http_set_response_sink()` (`daemon/src/http.c`) is a hook rather than
  a direct call, because the buffer lives on `main.c`'s own `struct
  conn`, which the HTTP layer must not know about. With no sink
  installed the layer behaves exactly as it always did, which is what
  keeps every non-client writer (WebSocket frames, exec streams)
  unchanged.
- Every `struct conn` is now `calloc`'d rather than `malloc`'d. The
  fields above are only correct if they start zeroed, and 25 allocation
  sites each setting fields by hand is a defect waiting for the next
  person to add a field.

## Alternatives considered

**Threads, or a worker pool for responses.** Rejected. It would make
every piece of daemon state shared mutable state, for a problem that is
purely "do not block on a peer". The event loop is the right shape; it
was simply being used incorrectly.

**A write timeout on the blocking socket (`SO_SNDTIMEO`).** Rejected.
It bounds the damage rather than removing it: a slow client would still
stall the loop, just for a bounded interval, and every other client
would still pay. It also turns a legitimately slow reader into a failed
request.

**Shrinking `GET /v1/pkg`.** Necessary but not sufficient, and kept as
a separate concern. 1.8 MB is a genuinely bad payload -- it embeds the
complete installed file list of every package -- and it should shrink.
But any endpoint can outgrow a socket buffer, and a control plane whose
liveness depends on every response staying small is not one to rely on.
Fixing the architecture first is what makes the payload an ordinary
efficiency question instead of an availability one.

**Capping the response size.** Rejected as a fix; it would break real
API consumers to work around a bug in how the daemon writes.

## Consequences

- One slow or hostile client can no longer affect any other. This closes
  an unauthenticated denial of service against the whole control plane.
- Memory: a deferred response is held until it is sent. Bounded in
  practice by the write deadline and by the connection throttle
  (ADR-0134) that already limits how many connections a source may
  open.
- `http_set_blocking()` calls remain on the response paths and remain
  correct. For the in-flight client request the sink takes the bytes and
  no write happens there at all; any other fd reaching that path is
  written straight out and does need blocking mode.
- `test_slow_client` reproduces the original failure exactly -- a client
  that requests a large asset and never reads, with health sampled
  concurrently -- and also asserts the write deadline fires. The bug is
  cheap and deterministic to reproduce, so there is no excuse for it to
  return unnoticed.

## Verified on 192.168.15.95, v2.14.4

The same experiment that produced the numbers above, re-run against the
fix:

```
baseline            health=200 in 8ms / 11ms / 14ms
slow client open    health=200 in 130ms / 18 / 11 / 11 / 13 / 15 / 15 / 15
slow client closed  health=200 in 12ms / 12ms / 11ms
```

The single 130 ms sample is the first one, and is the cost of buffering
1.8 MB once. Everything after it is ordinary.

The write deadline was confirmed separately, and how it had to be
confirmed is worth recording: **a dropped stalled client cannot observe
being dropped.** A peer that never reads advertises a zero receive
window, and TCP cannot deliver a FIN through one -- the kernel sits in
zero-window probing instead. So the obvious test, waiting for EOF on the
stalled socket, fails against entirely correct behaviour. What actually
happened was visible only in the daemon's own log:

```
dropping a client that stopped reading its response after 110048 of 1797803 bytes
```

`test_slow_client` therefore asserts on that record rather than on the
socket, with the reason written into the test so it is not "simplified"
back into a broken assertion later.

## The lesson worth keeping

The stall records were right and were read wrong. `activity` named the
request that did not come back and `wchan` named what it was waiting on,
and both were correct for days while the investigation went elsewhere.
When instrumentation that was built for exactly this purpose is
disagreeing with the current theory, the theory is what should be
re-examined first.
