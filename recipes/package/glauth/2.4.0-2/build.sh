#
# glauth -- a lightweight, single-binary LDAP server (github.com/glauth/glauth),
# replacing lldap as Cix's own standard integrable LDAP provider. Chosen
# over lldap for this project's own LDAP redesign (tasks #723-731) because
# its SQLite backend can be built directly into the main binary (no
# separate embedded web UI/Rust+WASM frontend build lldap needed, ADR-0036) --
# a much smaller, simpler dependency footprint for something Cix's own
# REST layer manages entirely (task #726), never a human-facing admin UI.
#
# Read by cixd's own non-executing metadata scanner (pkg_name=/
# pkg_version=/pkg_source=/pkg_sha256=/pkg_depends=, daemon/src/pkg.c's
# parse_recipe()) AND sourced as a real POSIX shell script inside the
# isolated, network-less build container (". /build/recipe.sh") to run
# pkg_build()/pkg_install() below. Never sourced or executed on the host.
#
# pkg_source is NOT glauth's own plain upstream tarball -- glauth v2 needs
# real assembly first, all done once, reproducibly, off-box (this project's
# own build sandbox has real internet access; a deployed Cix host does
# not, ADR-0034/the "no outbound DNS by default" finding):
#
#   1. git clone --branch v2.4.0 --depth 1 https://github.com/glauth/glauth.git
#   2. cd glauth/v2
#   3. The SQLite backend is upstream's own separate git submodule
#      (glauth/glauth-sqlite, pinned via .gitmodules to a specific commit --
#      f34a4dea132abc1bfd5089410e1cc8e05e8163b3 for this v2.4.0 tag) and is
#      NOT included in a plain GitHub tag/branch tarball (submodules never
#      are) -- clone it separately into pkg/plugins/glauth-sqlite at that
#      exact pinned commit (git submodule status shows the pin).
#   4. Generate the static-embed shim glauth's own Makefile `testing` target
#      documents (never run automatically -- CI only ever builds the dynamic
#      .so plugin form): mkdir -p pkg/embed && sed -e 's/func main() {}//'
#      -e 's/^package main/package embed/' pkg/plugins/glauth-sqlite/sqlite.go
#      > pkg/embed/sqlite.go -- this, plus the upstream-committed
#      pkg/server/embed_sqlite.go (guarded by the `embedsqlite` build tag),
#      is what makes `datastore = "embed"` in a glauth config resolve to a
#      REAL, statically-linked SQLite backend -- no `plugin.Open()`/dlopen
#      of a separate .so at runtime, no Go-plugin-buildmode toolchain-version
#      fragility, one self-contained binary. Confirmed empirically: a
#      config with `datastore = "embed"`, a real inserted SQLite row, and a
#      real `ldapsearch` bind+query round-tripped correctly against this
#      exact build.
#   5. `go get github.com/mattn/go-sqlite3` (the sqlite plugin's own
#      dependency, needed once its source is merged into the main module's
#      own package tree above, not sha256sum implicit -- the main go.mod
#      never has any reason to already carry it).
#   6. `GOWORK=off go mod tidy && GOWORK=off go mod vendor` (this repo is a
#      multi-module go.work workspace at its root; v2/ alone, with
#      GOWORK=off, vendors as a plain single module) -- produces a complete,
#      offline-buildable vendor/ tree, since this project's own isolated
#      build containers have no network access at all (same reasoning
#      gitea.recipe's own GOFLAGS=-mod=vendor already established).
#   7. Drop .git/, docker/, and the other three unused plugin submodules
#      (glauth-mysql/glauth-postgres/glauth-pam -- Cix only ever uses the
#      embedded SQLite backend) to keep the tarball reasonably sized.
#   8. tar -cf glauth-2.4.0.tarball glauth-2.4.0/ (with the above tree
#      renamed to that top-level directory name).
#
# pkg_sha256 below is this exact custom-assembled tarball's own checksum,
# not upstream's release/tag tarball -- verified directly against the copy
# actually used for this recipe's own real end-to-end build/install
# verification through the real cixd pipeline (not just locally).
#
# Re-assembled and re-served during the LDAP re-provisioning session that
# followed the full-box reinstall (ADR-0146's own incident) -- the prior
# pkg_source URL above (192.168.15.31:8904) pointed at a long-gone earlier
# session's own scratch LAN server. Same offline-vendored assembly process
# documented above, same real end-to-end verification (glauth 2.4.0 tag,
# embed_sqlite shim, `go mod vendor`, built and run for real against the
# real Cix pkg pipeline) -- just re-hosted, since neither URL was ever
# meant to be a permanent artifact home (this project's own dev sandbox has
# no such thing yet). Whoever re-serves this next should do the same:
# rebuild per the steps above, re-host, update pkg_source/pkg_sha256 here.
pkg_name="glauth"
pkg_version="2.4.0-2"
pkg_source="http://192.168.15.31:8920/glauth-2.4.0.tarball"
pkg_sha256="3f654908f498ede1ec99eb951b3120c9f907df1fe380db0b5311b6ba46394170"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/glauth-2.4.0.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="e966af7b2cf3d174508208ad98082f14358e1a6eb70cf08f4fa3294963a1b3b8"
pkg_depends=""

# CGO_ENABLED=1 is load-bearing: mattn/go-sqlite3 compiles SQLite's own C
# amalgamation directly (no external libsqlite3 needed, matching gitea.recipe's
# own CGO+sqlite precedent) -- this is why the build image needs a working C
# toolchain staged (tcc/libc-dev AND a real gcc, e.g. cix-builder), not
# just Go.
#
# --- Bootstrap-compiler declaration (Tier 3 of this project's 3-tier TCC
# policy -- see gcc.recipe's own comment for the full policy statement).
# glauth/gitea's own CGO+sqlite build belongs there too, confirmed the
# hard way across a real, multi-stage investigation for issue #31 (full
# trail in git log), not assumed or guessed:
#
#  1. The SQLite C amalgamation itself (sqlite3-binding.c) compiles clean
#     under a standalone `tcc -c -std=gnu99 <the real #cgo CFLAGS>`, every
#     symbol cgo needs comes out as a real defined global -- a genuine
#     working compile of the actual C code being built, not the blocker.
#  2. cgo itself unconditionally passes `-Qunused-arguments` (Clang-only)
#     when compiling runtime/cgo's own bridging code -- fixable with a
#     thin wrapper stripping the one flag (chrony.recipe's own established
#     pattern), confirmed via `go build -x` isolating it as the only
#     problem flag among a dozen candidates tested individually.
#  3. cgo unconditionally emits GoComplex64/GoComplex128 (C99 `_Complex`)
#     typedefs into every package's generated _cgo_export.h whenever any
#     exported Go function exists anywhere in the build graph -- TCC 0.9.27
#     cannot parse `_Complex` in any spelling (`float _Complex`, `_Complex
#     float`, nor via <complex.h>'s own `complex` macro, which itself fails
#     inside glibc's bits/cmathcalls.h) -- also fixable, since nothing in
#     this build graph ever does complex arithmetic: neutralize the two
#     dead typedefs to a plain struct before compiling.
#  4. TCC has zero support for either the C11 __atomic_* or the legacy GCC
#     __sync_* builtin families (confirmed: both compile only as bare
#     implicit-declaration external calls, then fail to link) -- Go's own
#     runtime/cgo/gcc_libinit.c (part of EVERY cgo build, not package-
#     specific) uses __atomic_load_n/__atomic_store_n for real
#     synchronization. Still fixable: the only two call sites in the whole
#     runtime/cgo tree use CONSUME/RELEASE ordering on native-word-sized
#     operands, and x86-64's own TSO memory model already gives that
#     ordering for free on an aligned load/store plus a compiler barrier --
#     a real, provably-correct macro shim, not a hack.
#  5. THE ACTUAL, UN-WORK-AROUNDABLE WALL: TCC aborts its ENTIRE
#     compilation unit after the FIRST error anywhere in the translation
#     unit, rather than continuing to parse and report errors in later,
#     independent top-level declarations the way GCC/Clang do (confirmed
#     directly with a minimal 3-function probe: only the first function's
#     error is ever reported, compilation halts there, the second and
#     third functions' errors -- which absolutely would occur -- are never
#     reached at all). cmd/cgo's own type-probing (gcc.go's typeCheck)
#     depends structurally on this continue-past-errors behavior: it
#     batches every C symbol's "is this a type?/a constant?/declared at
#     all?" probe (5 tiny functions each) into ONE combined source file,
#     compiles it ONCE, and classifies every symbol from the resulting
#     error list in a single pass -- explicitly documented in cmd/cgo's
#     own source as "we can infer what we need from only the presence or
#     absence of an error on a specific line" (plural, all lines, one
#     pass). TCC stopping at the first error breaks this for every name
#     after the first in every single probed package -- confirmed
#     reproducing this exact failure against the Go standard library's own
#     os/user package (pulled in transitively), not just glauth's or
#     go-sqlite3's own code, so this is a systemic TCC-vs-cgo
#     incompatibility, not anything specific to this recipe. (A related,
#     independently-confirmed TCC #line-diagnostic bug -- prepending the
#     compiled file's own directory onto a #line-supplied filename in
#     error messages, breaking cgo's exact-string pseudo-filename matching
#     -- was also found and could be fixed at the wrapper level by
#     rewriting stderr; it's #5 above that is the real, unfixable wall.)
#
# No wrapper-level trick closes #5 without literally reimplementing cgo's
# own batch-probe compiler-driver semantics (splitting every probe into
# its own tcc invocation and synthesizing gcc-shaped combined output) --
# exactly the kind of brittle, bespoke reimplementation this project's own
# "No Hacks" maxim rules out. CC is therefore pinned to the real, already-
# staged ambient gcc EXPLICITLY, by absolute path (never a bare `gcc`/`cc`
# -- this project's own confirmed gcc-invocation gotcha: a bare-name
# invocation computes a wrong relative install prefix and breaks cc1
# lookup) rather than left to accidental ambient pickup, the same
# deliberate-not-implicit posture already fixed on openssh.recipe.
# GOFLAGS=-mod=vendor + GOPROXY=off make any accidental network module
# fetch fail loudly and immediately, same defensive posture gitea.recipe's
# own build already established, rather than silently depending on Go's
# own vendor/ auto-detection never changing behavior across a future
# upgrade. -tags embedsqlite selects the statically-linked SQLite backend
# (see pkg_source's own comment above) over glauth's own default `noembed`
# build tag, which would otherwise produce a binary with no working
# backend beyond the trivial in-memory `config`/`ldap`/`owncloud` ones.
pkg_build() {
	export PATH="/usr/local/go/bin:$PATH"
	export GOWORK="off"
	export GOFLAGS="-mod=vendor"
	export GOPROXY="off"
	export GOCACHE="/build/gocache"
	export CGO_ENABLED=1
	export CC=/usr/bin/gcc

	go build -tags embedsqlite -ldflags "-s -w" -o glauth .
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin"
	cp glauth "$PKG_DESTDIR/usr/bin/glauth"
}
