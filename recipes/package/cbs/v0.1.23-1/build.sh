#
# cbs -- cix-build-system, built on a Cix host for the first time.
#
# This recipe is the answer to cix-build-system#124, which reported that
# CBS could not be built on a Cix host because the two libraries its
# Makefile links, libarchive and libzstd, were not packaged. They are
# now: zstd 1.5.7-3 and libarchive 3.8.1-4, both TCC-built, both in
# cix-builder.
#
# CBS is not integrated with cixd and this recipe does not integrate it.
# Nothing in the daemon invokes cbs, no recipe format discriminator
# exists, and cix-build-system#132 -- the contract for what cixd would
# pass in and read back -- is still open. What this package establishes
# is the one thing that had to come first and could not be reasoned
# about: that CBS compiles and runs under this platform's own pinned
# compiler, against this platform's own libraries.
#
# Built with TCC because CBS is a build system for Cix and ADR-0224's
# first clause is not negotiable for code in that position. CBS's own
# Makefile asks for -std=c11 -Wall -Wextra -Werror -pedantic, which is a
# real test of the pinned 0.9.28rc snapshot rather than a formality; it
# is passed through unchanged rather than softened, because a build
# system that needs its warnings turned off to compile is not one to
# hand a platform to.
#
# The gate below runs CBS against a real recipe, not a fixture. CBS's
# own test suite is fixture-only -- tests/cli-build-test.sh builds a
# recipe that writes one text file, and a fetch smoke test against a
# local python http.server -- so "it builds" and "it parses a real
# recipe" are separate claims and only the second one is worth
# anything here. cbs.cbs, CBS's own recipe for itself, ships in the
# tarball and is the obvious subject.
#
pkg_name="cbs"
pkg_version="v0.1.23-1"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix-build-system/archive/v0.1.23.tar.gz"
pkg_sha256="66d5d353ffce97e07c0c8150f32f2d4e795c502b3dc24d04f092b43e2a18f18c"
pkg_build_image="cix-builder"
# A composed build environment contains precisely what is declared here.
# No configure script, so this is the plain make list plus the two
# libraries CBS links and the headers it compiles against.
pkg_build_depends="tcc make linux-headers bash coreutils grep binutils findutils libarchive zstd"
# ADR-0276: declare every linked library. The Makefile links -larchive
# -lzstd -ldl; -ldl resolves into libc on this glibc and needs no
# package of its own.
pkg_depends="libarchive zstd"
pkg_changelog="v0.1.23-1: first build of cix-build-system on a Cix host, closing cix-build-system#124. TCC, against the Cix-built libarchive 3.8.1-4 and zstd 1.5.7-3. Builds and validates a real recipe; not integrated with cixd, and cix-build-system#132 remains open."

pkg_build() {
	# CC=tcc explicitly, never a bare cc: the build sandbox's default
	# compiler has silently become real GCC before (#109), and a build
	# system compiled by the wrong compiler is the least useful place
	# for that to go unnoticed.
	make CC=tcc -j"$(nproc)"

	# It exists and it runs. An undefined symbol in an executable fails
	# at link, so reaching this line already proves more than it would
	# for a shared library -- but a binary that cannot print its own
	# version is not a build system.
	./cbs --version

	# It parses a real recipe. cbs.cbs is CBS's own, and carries a
	# sources block, a requires block with three roles, and four
	# phases. explain --json is additionally the exact call
	# cix-build-system#132 proposes cixd would make to learn a recipe's
	# declared facts before composing a build environment, so this gate
	# exercises the one interface an integration would depend on.
	./cbs check cbs.cbs
	./cbs explain cbs.cbs --json > /run/cbs-explain.json
	grep -q '"name":"cbs"' /run/cbs-explain.json
	grep -q '"phases"' /run/cbs-explain.json
}

pkg_install() {
	# No install target upstream; the Makefile builds ./cbs and stops.
	install -D -m 0755 cbs "$PKG_DESTDIR/usr/bin/cbs"
}
