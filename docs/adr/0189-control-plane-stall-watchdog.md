# 0189 — A watchdog process, because the loop cannot report its own silence

## Status

Accepted

## Context

`cixd` is a single-threaded `epoll` loop, and on an installed host it is the only way in: there is no SSH and no general shell (ADR-0034). Three separate mechanisms already exist to keep it answering — ADR-0180 made container teardown asynchronous after two real freezes, issue #85 gave package builds an aggregate budget after a real lockout, and ADR-0187 reserved a share of the machine for the control plane so no workload can starve it.

None of them helped with what happened on 2026-08-23. The production host accepted TCP on both listeners while answering nothing, for at least five minutes and possibly thirty-five, then recovered on its own. Containers kept serving throughout; the kernel was healthy; `ping` was 0.5ms.

**The log store had nothing from inside the window.** Not an oversight — a structural consequence. Every diagnostic this daemon has runs on the loop, so when the loop stops, so does every way of noticing that it stopped. The only trace a wedge leaves is its own absence, and absence cannot be analysed: after the fact there was no way to tell a blocked syscall from a spin, a slow disk from a hung network call, or which request was in flight.

## Decision

**A separate process watches the loop.**

The daemon forks a watchdog at startup. The parent stores a monotonic timestamp into a shared page once per event-loop iteration; the child compares it against the clock twice a second, and after five seconds of silence writes a record. It keeps writing one every thirty seconds while the stall continues, and writes a final record when heartbeats resume, so a long wedge leaves a trail with timestamps rather than one line whose end is unknown.

**A process rather than a signal handler.** A `SIGALRM` handler would run on the stuck thread under async-signal-safety rules, which rules out doing anything genuinely useful. A separate process can take its time — and, crucially, can read what the kernel already knows: `/proc/<pid>/wchan` names the kernel function the parent is sleeping in, and `/proc/<pid>/stat` gives its state (`D` being the uninterruptible sleep that cannot even be killed). Those two facts are the most useful things about a wedge and are precisely what is unavailable from inside it.

**The loop wakes at least once a second.** `epoll_wait` previously blocked indefinitely when idle, which would make an idle daemon indistinguishable from a wedged one. A one-second timeout is nothing next to the per-two-second polling every dashboard client already does, and it makes silence mean something.

**Records outlive everything.** They are appended to a JSON-lines file under the data directory, so they survive the stall, a daemon restart and a reboot — a wedge is usually followed by one of those, and a diagnostic that dies with what it was diagnosing is not a diagnostic. `GET /v1/system/stalls` reads the file back rather than caching it: the file is written by another process, and a cache would be a second, staler copy of the one thing that has to be trustworthy.

**The watchdog closes every inherited descriptor.** The parent's listening sockets are among them, and a watchdog still holding one after the daemon died would keep the port bound — turning a diagnostic into the reason the daemon cannot be restarted.

## Alternatives considered

**A `SIGALRM` timer in-process.** Simpler, no second process, and it does fire while the loop is blocked. Rejected: inside a signal handler almost nothing is legal to call, so the record would be a bare `write()` of a fixed string — no `/proc` reads, no formatting, no useful detail. The interesting part of a wedge is exactly the part a handler cannot collect.

**An external monitor on another machine.** It would notice unreachability, which is the symptom already visible. Rejected as the primary mechanism because it cannot see *why*: no process state, no `wchan`, no in-flight request, and it needs a second machine to exist and be reachable — on a platform whose whole point is that a single box is self-sufficient.

**Making the stall impossible instead — audit every synchronous call.** Worth doing, and partly done (ADR-0180, #85, ADR-0187 all came from that instinct). Rejected as a substitute: it is a claim about code that is true until the next change, and the failure it guards against costs a physical trip to the machine. Detection and prevention answer different questions.

**A hard watchdog that restarts the daemon after N seconds.** Deliberately not done yet. Killing a control plane that might be one second from completing a legitimate slow operation can turn a hiccup into an outage, and until there is real data about what actually stalls, the reset threshold would be a guess. The records this ADR adds are what that decision needs first.

## Consequences

A stall is now a fact with a timestamp, a duration, a kernel function and a request attached, readable over REST after the event. The next occurrence of the incident that prompted this is analysable instead of being a shrug.

The threshold (five seconds) is above anything the loop should ever do and below anything an operator would call an outage. Legitimate slow synchronous work — `POST /system/update` copying a multi-megabyte image with `fsync`, for one — will cross it and be recorded. That is the intent: those are exactly the places worth making asynchronous, and now there is evidence for which ones actually matter rather than a reading of the code.
