# 0274 — The platform says what it is

## Status

Accepted

Issue [#387](https://git.home.arpa/itdlabs/cix/issues/387).

## Context

Cix shipped no `/etc/os-release`. Neither the control-plane root nor any
container image had one, confirmed directly against the running `jump`
container:

```
$ GET /v1/containers/jump/files?path=/etc/os-release
{"error":"no such file"}
```

The freedesktop specification is explicit about what a reader does when
`ID` is absent: it falls back to `linux`. So every tool that asked what
this platform is got the one answer a system built from source
specifically so that it is **not** somebody else's distribution should
never give. Not a wrong answer it could report — a generic one, silently,
with nothing to indicate the question had failed.

It surfaced while packaging `fastfetch`, which reads exactly this file,
but `fastfetch` is the messenger rather than the reason. Anything that
branches on the host distribution — a config-management rule, an
installer, a support script, a container inspecting its own base — had
no way to identify Cix at all.

## Decision

Stage `/etc/os-release` in both places a Cix filesystem is assembled: the
control-plane root (`mkbootroot`) and every container image
(`pkg_seed_image_baseline()`).

**`ID=cix`.** Lower-case, matching the wordmark the brand guidelines name
as the primary identifier.

**One renderer, called from both.** `osrelease_render()` in
`daemon/src/osrelease.c`. The two stagers are different programs running
at different times, and two copies of this text would drift the way every
duplicated constant in this project has — here that drift means a host
and the containers running on it disagreeing about what they are, which
is worse than either being wrong consistently.

**`BUILD_ID` carries the build; there is no `VERSION_ID`.** Cix is
rolling-release by charter, so there is no release version to put in one.
The specification makes `VERSION_ID` optional for exactly this case.

**No URL fields yet.** Everything that exists points at `git.home.arpa`.

## Alternatives considered

**Generate it at boot instead of at assembly.** Rejected: the value is
known when the root is built, and computing it at boot adds a failure
mode to PID 1 — which on this platform panics the machine if it returns
from `main()` at all — in exchange for nothing. A file written once and
read forever is the correct shape.

**Put `VERSION_ID` in, set to the daemon build.** Rejected, and this is
the one worth recording because it is the plausible mistake. `VERSION_ID`
means "which release of this distribution", and readers compare it
between hosts to decide whether they are running the same thing. Two Cix
hosts on different builds are not on different *releases* — there are no
releases. Writing the build there would make every such comparison
answer confidently and wrongly. `BUILD_ID` is the field for a build, and
it is what an operator gets from `GET /v1/system/boot` too, so the two
surfaces agree rather than needing reconciliation.

**Have the image record its own version.** Rejected: ADR-0155's image
version is a hash of a package manifest, which is meaningful to this
platform and meaningless to a reader of `os-release`. `BUILD_ID` is the
daemon that produced the rootfs, which is the honest answer to "what made
this".

**Point `HOME_URL` at `git.home.arpa`.** Rejected: a URL that fails to
resolve for every reader outside this LAN is worse than an absent
optional field, because it looks like an answer. These go in when Cix is
public.

## Consequences

**`ID=cix` is now a compatibility surface.** The moment anything keys on
it — a logo table, a package manager, a config-management rule — changing
it breaks all of them silently, with no error anywhere. That is the
reason this is an ADR and not a commit: the string is cheap to write
today and expensive to change later.

A package that ships its own `/etc/os-release` still wins; seeding never
overwrites an existing file, matching what `nsswitch.conf` already does
in the same function.

Two things this does **not** do, both tracked separately. It does not
make `fastfetch` select the Cix logo by name — that additionally needs
the logo accepted upstream, which needs Cix to be public. And existing
images do not gain the file retroactively: seeding runs when an image is
created or re-seeded, so an image built before this lands has one only
after its next re-seed.

`test_osrelease` gates the content, and deliberately gates the
**omissions** as well as the fields — a later edit adding `VERSION_ID`,
which is an easy and well-meant change, has to fail that test first and
read why.
