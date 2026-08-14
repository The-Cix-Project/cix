#
# glauth -- a lightweight, single-binary LDAP server (github.com/glauth/glauth),
# replacing lldap as Kanxeo's own standard integrable LDAP provider. Chosen
# over lldap for this project's own LDAP redesign (tasks #723-731) because
# its SQLite backend can be built directly into the main binary (no
# separate embedded web UI/Rust+WASM frontend build lldap needed, ADR-0036) --
# a much smaller, simpler dependency footprint for something Kanxeo's own
# REST layer manages entirely (task #726), never a human-facing admin UI.
#
# Read by kanxeod's own non-executing metadata scanner (pkg_name=/
# pkg_version=/pkg_source=/pkg_sha256=/pkg_depends=, daemon/src/pkg.c's
# parse_recipe()) AND sourced as a real POSIX shell script inside the
# isolated, network-less build container (". /build/recipe.sh") to run
# pkg_build()/pkg_install() below. Never sourced or executed on the host.
#
# pkg_source is NOT glauth's own plain upstream tarball -- glauth v2 needs
# real assembly first, all done once, reproducibly, off-box (this project's
# own build sandbox has real internet access; a deployed Kanxeo host does
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
#      (glauth-mysql/glauth-postgres/glauth-pam -- Kanxeo only ever uses the
#      embedded SQLite backend) to keep the tarball reasonably sized.
#   8. tar -cf glauth-2.4.0.tarball glauth-2.4.0/ (with the above tree
#      renamed to that top-level directory name).
#
# pkg_sha256 below is this exact custom-assembled tarball's own checksum,
# not upstream's release/tag tarball -- verified directly against the copy
# actually used for this recipe's own real end-to-end build/install
# verification through the real kanxeod pipeline (not just locally).
#
# Re-assembled and re-served during the LDAP re-provisioning session that
# followed the full-box reinstall (ADR-0146's own incident) -- the prior
# pkg_source URL above (192.168.15.31:8904) pointed at a long-gone earlier
# session's own scratch LAN server. Same offline-vendored assembly process
# documented above, same real end-to-end verification (glauth 2.4.0 tag,
# embed_sqlite shim, `go mod vendor`, built and run for real against the
# real Kanxeo pkg pipeline) -- just re-hosted, since neither URL was ever
# meant to be a permanent artifact home (this project's own dev sandbox has
# no such thing yet). Whoever re-serves this next should do the same:
# rebuild per the steps above, re-host, update pkg_source/pkg_sha256 here.
pkg_name="glauth"
pkg_version="2.4.0"
pkg_source="http://192.168.15.31:8920/glauth-2.4.0.tarball"
pkg_sha256="3f654908f498ede1ec99eb951b3120c9f907df1fe380db0b5311b6ba46394170"
pkg_depends=""

# CGO_ENABLED=1 is load-bearing: mattn/go-sqlite3 compiles SQLite's own C
# amalgamation directly (no external libsqlite3 needed, matching gitea.recipe's
# own CGO+sqlite precedent) -- this is why the build image needs a working C
# toolchain staged (tcc/libc-dev, e.g. kanxeo-builder), not just Go.
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
	go build -tags embedsqlite -ldflags "-s -w" -o glauth .
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin"
	cp glauth "$PKG_DESTDIR/usr/bin/glauth"
}
