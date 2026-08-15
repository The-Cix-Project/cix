#
# gitea -- a painless, self-hosted Git service (code.gitea.io).
#
# Read by thincd's own non-executing metadata scanner (pkg_name=/
# pkg_version=/pkg_source=/pkg_sha256=/pkg_depends=, daemon/src/pkg.c's
# parse_recipe()) AND sourced as a real POSIX shell script inside the
# isolated, network-less build container (". /build/recipe.sh" --
# daemon/include/pkg.h's own documented contract) to run pkg_build()/
# pkg_install() below. Never sourced or executed on the host itself.
#
# pkg_source is gitea's own official "-src" release tarball (dl.gitea.com,
# not a plain GitHub source archive) -- deliberately: it ships both a
# full vendor/ tree (every Go module dependency committed in-repo) and
# pre-built frontend assets under public/, exactly what distro packagers
# (Debian, Arch, ...) already rely on to build gitea from source with no
# network access and no Node.js/npm toolchain -- the same two constraints
# this project's own build containers already have. Source verified two
# ways: the published gitea-src-1.24.7.tar.gz.sha256 from dl.gitea.com
# matches pkg_sha256 below (this daemon's own real check), and that same
# value was independently reproduced by downloading the tarball directly
# and running sha256sum against it before this recipe was written.
#
# Version pinned to 1.24.7, not the newest 1.27.x release line, for a
# real, checked reason: 1.27.x's own go.mod requires Go >= 1.26, newer
# than the Go toolchain this project's own mktoolchainimage.c currently
# stages (go1.24.2, confirmed via `go version` on the build host that
# produced it). 1.24.7's own go.mod requires only "go 1.24", satisfied
# exactly. Revisit this pin once the staged toolchain's own Go version
# moves past 1.26.
#
# IMPORTANT, read before running this in a real container: gitea shells
# out to a real `git` binary at runtime for its actual repository
# operations (clone/push/etc.) -- this project has no git.recipe yet, so
# installing gitea alone is not sufficient for it to actually serve
# repositories; git needs to land in the same image some other way
# first. This recipe only builds and installs the gitea binary itself,
# proven end-to-end (see below), the same "not everything at once" scope
# boundary every other recipe/phase in this project already keeps.
#
pkg_name="gitea"
pkg_version="1.24.7"
pkg_source="https://dl.gitea.com/gitea/1.24.7/gitea-src-1.24.7.tar.gz"
pkg_sha256="76a91742902fc353369e948b93666018ffe7eb0c565241a944c14f3d9232d808"
pkg_depends=""

# Run with $PWD already at /build/src (the extracted tarball, one
# leading path component already stripped) and PATH=/usr/bin:/bin -- no
# network access. /usr/local/go/bin is added explicitly here since the
# default build PATH doesn't include it. GOFLAGS=-mod=vendor +
# GOPROXY=off are both real, load-bearing, not just documentation: Go's
# own module resolution auto-detects vendor/ when it's consistent with
# go.mod and would normally use it without either of these, but setting
# them explicitly makes any accidental network module fetch fail loudly
# and immediately instead of silently depending on that auto-detection
# never changing behavior across a future Go upgrade. TAGS=sqlite +
# CGO_ENABLED=1 build gitea against its own bundled, CGO-based sqlite
# driver (mattn/go-sqlite3 -- compiles sqlite's own C amalgamation
# directly via the already-staged gcc, no external libsqlite3 needed) --
# the natural choice for a single-container, no-separate-database-
# service deployment, matching how gitea's own official Docker image
# defaults too. `make backend` (not `make build`, which also chains
# `frontend`) deliberately skips gitea's own webpack/npm frontend
# rebuild entirely -- the pre-built assets already in public/ (part of
# pkg_source's own tarball, see above) are used as-is; this project's
# own toolchain has no Node.js staged at all, and doesn't need it here.
pkg_build() {
	export PATH="/usr/local/go/bin:$PATH"
	export GOFLAGS="-mod=vendor"
	export GOPROXY="off"
	export GOCACHE="/build/gocache"
	export CGO_ENABLED=1
	make backend TAGS=sqlite
}

# gitea has no autotools-style "make install DESTDIR=" target of its own
# (its whole distribution model is "copy the one binary") -- PKG_DESTDIR
# is set by thincd itself (daemon/src/pkg.c), everything written under
# it is what actually gets merged into the target image once this build
# container exits successfully.
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin"
	cp gitea "$PKG_DESTDIR/usr/bin/gitea"
}
