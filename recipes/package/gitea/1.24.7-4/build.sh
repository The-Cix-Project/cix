#
# gitea -- a painless, self-hosted Git service (code.gitea.io).
#
# Read by cixd's own non-executing metadata scanner (pkg_name=/
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
pkg_version="1.24.7-4"
pkg_source="https://dl.gitea.com/gitea/1.24.7/gitea-src-1.24.7.tar.gz"
pkg_sha256="76a91742902fc353369e948b93666018ffe7eb0c565241a944c14f3d9232d808"
pkg_artifact_sha256="89acd61cc4b48117e150ec4f916cae14f3ec6be5b2eaf8512ee9242500d40246"
pkg_depends=""
#
# ADR-0199/0209: composed from exactly these, with no fallback (#168).
#
# This is the first revision able to declare them at all. Until #261
# there was no `go` package on this platform -- gitea was built in the
# accreted sandbox ADR-0199 retired, where a Go toolchain existed
# ambiently and undeclared, so this recipe described software the
# platform could no longer produce.
#
# gcc is not optional here and is not a toolchain preference: TAGS=sqlite
# is a cgo build linking a C sqlite amalgamation (ADR-0170), so a C
# compiler is part of the Go build rather than beside it.
#
pkg_build_depends="bash coreutils make go gcc binutils sed grep gawk findutils"
pkg_toolchain="gcc"
pkg_toolchain_reason="build system requires GCC internals: cgo: TCC aborts on the first compile error, which breaks cmd/cgo's batched type probing (#31)"
pkg_changelog="1.24.7-4: builds in the directory the tarball actually creates. -3 proved the Go bootstrap works -- go version go1.24.9 linux/amd64, resolved from PATH inside a real build container -- and then failed with make: No rule to make target backend, because the upstream tarball extracts into gitea-src-1.24.7/ and the build ran in its parent. An error that reads like a missing Makefile target and is really a missing cd. Resolved by glob rather than hardcoded so a version bump cannot silently reintroduce it, and asserted so a changed tarball shape fails with a sentence rather than thirty lines later. 1.24.7-3: declares its build tools, which was impossible until now (#206, #261). There was no go package on this platform: gitea was built in the accreted sandbox ADR-0199 retired, where a Go toolchain existed ambiently and undeclared, so this recipe described software the platform could no longer produce. gcc is declared alongside go and is not a toolchain preference -- TAGS=sqlite is a cgo build linking a C sqlite amalgamation (ADR-0170). Drops the PATH=/usr/local/go/bin line: the go package symlinks /usr/bin/go, and that hardcoded path was where the retired sandbox happened to keep it, which is the assumption that made this unbuildable. TMPDIR points at /run, since this platform images have neither /tmp nor /var/tmp. 1.24.7-2: declare pkg_toolchain=gcc and its reason (#222, ADR-0226)"

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
# directly, no external libsqlite3 needed) -- the natural choice for a
# single-container, no-separate-database-service deployment, matching
# how gitea's own official Docker image defaults too. CC is pinned to the
# real, already-staged ambient gcc EXPLICITLY, by absolute path -- never
# a bare `gcc`/`cc` (this project's own confirmed gcc-invocation gotcha:
# a bare-name invocation computes a wrong relative install prefix and
# breaks cc1 lookup), and deliberately NOT tcc (issue #31's own
# investigation, full trail on glauth.recipe's own comment -- TCC
# categorically cannot build cgo-based packages: it aborts its entire
# compilation unit after the first error anywhere in the file rather
# than continuing like GCC/Clang, which breaks cmd/cgo's own batched
# type-probing mechanism structurally, confirmed reproducing against the
# Go standard library's own os/user package, not just this package's
# code -- a genuine Tier-3 TCC exception, not an oversight). This
# comment used to say "via the already-staged gcc" -- an honest
# description of what was happening before, but never a deliberate,
# explicit choice until now, closing the exact risk class already
# confirmed once on openssh.recipe (a real GCC picked up silently).
# `make backend` (not `make build`, which also chains
# `frontend`) deliberately skips gitea's own webpack/npm frontend
# rebuild entirely -- the pre-built assets already in public/ (part of
# pkg_source's own tarball, see above) are used as-is; this project's
# own toolchain has no Node.js staged at all, and doesn't need it here.
pkg_build() {
	#
	# No PATH surgery: the `go` package symlinks /usr/bin/go, so the
	# composed build environment already has it. The old line pointed
	# at /usr/local/go, which is where the retired ambient sandbox
	# happened to keep it -- exactly the kind of assumption that made
	# this recipe unbuildable once that sandbox was gone.
	#
	go version

	#
	# The upstream tarball extracts into gitea-src-<version>/, and the
	# build has to happen inside it. -3 ran make in the parent and got
	# "No rule to make target 'backend'" -- an error that reads like a
	# missing Makefile target when it is really a missing cd.
	#
	# Resolved by glob rather than hardcoded, so a version bump does not
	# silently reintroduce this, and asserted rather than assumed: a
	# tarball whose shape changes should fail here with a sentence,
	# not thirty lines further on.
	#
	src_dir=$(ls -d gitea-src-*/ 2>/dev/null | head -1)
	if [ -z "$src_dir" ] || [ ! -f "${src_dir}Makefile" ]; then
		echo "gitea: no gitea-src-*/ directory with a Makefile in it" >&2
		ls -la >&2
		exit 1
	fi
	cd "$src_dir"
	echo "  building in $src_dir"
	export GOFLAGS="-mod=vendor"
	export GOPROXY="off"
	export GOCACHE="/build/gocache"
	mkdir -p /run/gotmp
	export TMPDIR="/run/gotmp"
	export CGO_ENABLED=1
	export CC=/usr/bin/gcc
	make backend TAGS=sqlite
}

# gitea has no autotools-style "make install DESTDIR=" target of its own
# (its whole distribution model is "copy the one binary") -- PKG_DESTDIR
# is set by cixd itself (daemon/src/pkg.c), everything written under
# it is what actually gets merged into the target image once this build
# container exits successfully.
pkg_install() {
	#
	# pkg_install starts from the source ROOT again, not wherever
	# pkg_build happened to end up -- so the same directory has to be
	# entered here too. The binary is inside the extracted tree.
	#
	src_dir=$(ls -d gitea-src-*/ 2>/dev/null | head -1)
	mkdir -p "$PKG_DESTDIR/usr/bin"
	cp "${src_dir}gitea" "$PKG_DESTDIR/usr/bin/gitea"
}
