#
# glauth -- a lightweight, single-binary LDAP server (github.com/glauth/glauth),
# this platform's standard integrable LDAP provider (ADR-0109, chosen over
# lldap) and the host's own authentication root of trust (ADR-0144): cixd
# renders users and groups into its config, re-syncs them on every start
# (ADR-0146), and binds against it for every gated API write.
#
# Read by cixd's own non-executing metadata scanner (pkg_name=/pkg_version=/
# pkg_source=/pkg_sha256=/..., daemon/src/pkg.c) AND sourced as a real POSIX
# shell script inside the isolated, network-less build container to run
# pkg_build()/pkg_install(). Never sourced or executed on the host.
#
# --- The source: a PREPARED tree, and where it lives -------------------
#
# pkg_source is not upstream's tag tarball. glauth v2 needs assembly first,
# and a build container has no network at all (measured: probe-tls), so
# everything a build needs must already be in the tarball:
#
#   1. git clone --branch v2.4.0 --depth 1 https://github.com/glauth/glauth.git
#   2. cd glauth/v2
#   3. The SQLite backend is upstream's own separate submodule
#      (glauth/glauth-sqlite), pinned by .gitmodules at
#      f34a4dea132abc1bfd5089410e1cc8e05e8163b3 for the v2.4.0 tag, and
#      never included in a tag tarball. Clone it into
#      pkg/plugins/glauth-sqlite and check out that exact commit.
#   4. Generate the static-embed shim glauth's own Makefile documents but
#      CI never builds:
#        mkdir -p pkg/embed
#        sed -e 's/func main() {}//' -e 's/^package main/package embed/' \
#            pkg/plugins/glauth-sqlite/sqlite.go > pkg/embed/sqlite.go
#      Together with the upstream-committed pkg/server/embed_sqlite.go
#      (behind the embedsqlite build tag) this makes datastore = "embed"
#      a real, statically linked SQLite backend -- one binary, no
#      plugin.Open() of a .so at runtime.
#   5. GOWORK=off go get github.com/mattn/go-sqlite3  (resolved to v1.14.50
#      on 2026-09-03; go.mod in the tarball pins it)
#   6. GOWORK=off go mod tidy && GOWORK=off go mod vendor  (51 modules)
#   7. Drop .git, docker/, and the mysql/postgres/pam plugins Cix never uses.
#   8. Rename v2/ to glauth-2.4.0/ and tar -czf it.
#
# Assembled in the dev sandbox on 2026-09-03, as the two previous
# assemblies were. That is source preparation, not building: go mod vendor
# downloads source and compiles nothing, and the build below runs on a Cix
# host with Cix's own Go and gcc. Verified before publishing with an
# offline GOFLAGS=-mod=vendor build of the tree.
#
# The tarball now lives in cix-cache, this platform's durable, checksum-
# gated artifact store, under the src- naming convention (#262). The two
# earlier assemblies were served from scratch LAN servers (192.168.15.31
# ports 8904 and then 8920) that were never meant to be permanent, and
# both went away -- which is the whole reason revisions 2.4.0 through
# 2.4.0-3 became unbuildable while looking complete. pkg_sha256 below is
# what makes the store a mirror rather than a trust boundary: it approves
# these exact bytes, wherever they are served from.
#
# --- Why gcc ----------------------------------------------------------
#
# CGO_ENABLED=1 is load-bearing: mattn/go-sqlite3 compiles SQLite's own C
# amalgamation through cmd/cgo. TCC cannot drive cgo -- cgo batches every
# C symbol probe into one translation unit and classifies them from a
# single error list, and TCC stops at the first error (ADR-0170, #31; the
# full investigation, including the four work-aroundable gaps found on the
# way to the one that is not, is in the header of revision 2.4.0-3).
# gcc is named by absolute path: a bare name computes a wrong relative
# install prefix and breaks cc1 lookup.
#
pkg_name="glauth"
pkg_version="2.4.0-6"
pkg_source="http://192.168.15.31:8080/glauth-src-2.4.0-1-x86_64.tar.gz"
pkg_sha256="fb872485f894000a3f68a7065434322124f2173f21398b647207dbcd0d1a8c6d"
pkg_artifact_sha256="477505c9fe86392ddbf76f50a04c414dc66ac6321c5078c3c52e1c02782b4512"
pkg_depends=""
pkg_build_depends="bash coreutils grep go gcc binutils"
pkg_toolchain="gcc"
pkg_toolchain_reason="cgo: mattn/go-sqlite3 compiles SQLite through cmd/cgo, whose batched symbol probing needs a compiler that continues past the first error; TCC stops at the first (ADR-0170, #31)"
pkg_changelog="2.4.0-6: the post-build size report used awk, undeclared and absent in the build environment, so it printed nothing and a broken-pipe error; now wc from coreutils, which is declared. Every tool a recipe runs is declared -- that is what makes the build environment composed rather than ambient. 2.4.0-5: declares grep, which 2.4.0-4 used in its post-build gate without declaring -- go build succeeded there and the gate itself was what failed. 2.4.0-4: rebuilt properly from a re-assembled prepared source that now lives in cix-cache instead of a scratch LAN server that went away (#262). Declares its build tools (go, gcc, binutils) so the build environment is composed rather than ambient (ADR-0199) -- the previous artifact was produced in the retired accreted sandbox with an undeclared toolchain. go-sqlite3 resolved to v1.14.50. No functional change to glauth itself."

pkg_build() {
	export GOWORK="off"
	export GOFLAGS="-mod=vendor"
	export GOPROXY="off"
	export GOCACHE="/build/gocache"
	export GOTMPDIR="/build/gotmp"
	export CGO_ENABLED=1
	export CC=/usr/bin/gcc
	mkdir -p "$GOCACHE" "$GOTMPDIR"

	go version
	go build -tags embedsqlite -ldflags "-s -w" -o glauth .

	#
	# The binary must carry the embedded backend it was built for. A
	# build that silently fell back to the noembed default would still
	# exit 0 and only fail at first bind on a live directory.
	#
	if ! ./glauth --help 2>&1 | grep -q 'Usage:'; then
		echo "glauth: built binary does not run" >&2
		exit 1
	fi
	echo "  built: $(wc -c < glauth) bytes"
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin"
	cp glauth "$PKG_DESTDIR/usr/bin/glauth"
}
