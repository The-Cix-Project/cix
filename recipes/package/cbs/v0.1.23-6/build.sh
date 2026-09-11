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
# hand a platform to. -Werror stayed, and it cost a revision: the -1
# build failed at zstd.h:72 with "error: #pragma message ignored".
#
# That is the __GNUC__ story for the third time in this same sequence
# of packages, and it is worth stating plainly because it will keep
# recurring. TCC DEFINES NEITHER __GNUC__ NOR _MSC_VER, so a header
# that dispatches on those two and falls back to a third arm sends TCC
# down a path essentially nothing else takes:
#
#   zstd's MEM_swap32   -> the fallback, which is why the missing
#                          __builtin_bswap32 (#208) never bit
#   archive_blake2.h    -> _Pragma("pack 1"), which TCC cannot parse
#   zstd.h              -> #pragma message, which TCC does not implement
#
# The first was harmless, the second was caught by a gate, and this
# third one is fixed with zstd's own designed-in switch:
# ZSTD_DISABLE_DEPRECATE_WARNINGS selects an empty ZSTD_DEPRECATED,
# which is byte for byte what the unreachable arm defines anyway -- the
# pragma is a note to a compiler porter, not a semantic. Nothing about
# CBS's own warnings is relaxed.
#
# The gate below runs CBS against a real recipe, not a fixture. CBS's
# own test suite is fixture-only -- tests/cli-build-test.sh builds a
# recipe that writes one text file, and a fetch smoke test against a
# local python http.server -- so "it builds" and "it parses a real
# recipe" are separate claims and only the second one is worth
# anything here. cbs.cbs, CBS's own recipe for itself, ships in the
# tarball and is the obvious subject.
#
# THIS REVISION BUILDS A COMMIT, NOT A TAG. 0a5313f6 ("complete backlog
# implementation batch") is ahead of v0.1.23 and no tag was cut for it,
# so the version string here is a lie of convenience: the package says
# v0.1.23-3 and contains code the v0.1.23 tag does not. That is exactly
# the provenance problem cix-build-system#136 is about, arriving from
# the other direction, and it should be replaced by a tagged revision
# rather than left standing.
#
# It is built anyway because the commit claims to fix four things this
# recipe's own earlier revisions and probe-cbs-real-build measured, and
# a claim of that kind is worth checking the same day:
#   - archive.c now handles symlinks and hardlinks (#133)
#   - the requires-block compiler declaration now reaches the
#     environment as CC
#   - cache hits no longer demand libcurl
#   - --flag=value forms are accepted (#137)
#
# `make test` is added as a gate here for the first time. The suite grew
# substantially in that commit -- repro, fuzz, seams, typed-package,
# policy and observe tests all appear -- and none of it had ever been run
# under this platform's own pinned tcc. Its result is REPORTED rather
# than allowed to fail the build, because a failing upstream test should
# produce a readable log rather than an opaque exit status, and because
# the package is still worth having either way. Note `upstream-test`,
# the only target that builds a real package, is deliberately NOT part
# of `make test` and is not run here: it fetches over the network, which
# a build container does not have.
#
# The -3 build then died two steps into `make test`:
#
#   ./tests/parser-validation.sh ./cbs
#   parser and validation tests: PASS (50 cases)
#   ./tests/recipe-metadata-test.sh cbs.cbs
#   make: ./tests/recipe-metadata-test.sh: No such file or directory
#   make: *** [Makefile:61: test] Error 127
#
# The file is present, tracked at mode 100755, and in the fetched
# tarball. The cause is its shebang: it is the only one of the four
# test scripts that starts `#!/bin/bash`, and this platform's images
# ship `bin/sh` and `usr/bin/bash` and no `/bin/bash` at all. The
# kernel's ENOENT is for the missing INTERPRETER, so the message names
# the script, which reads as a missing file when the file is right
# there -- the exact shape libcap's own mkcapshdoc.sh produced and
# which CLAUDE.md records.
#
# It is rewritten here to /usr/bin/bash rather than /bin/sh: the script
# really is bash and may use bashisms, so pointing at the real bash is
# truthful where downgrading the interpreter would be a guess. Upstream
# should fix the shebang; until then this recipe cannot run the suite
# without the edit, which means CBS's test suite has never executed on
# a Cix host.
#
pkg_name="cbs"
pkg_version="v0.1.23-6"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix-build-system/archive/0a5313f6.tar.gz"
pkg_sha256="edabc285013f31f85c3ce9da8282b7033f30da23088ca6d2f8e70fee0b575b1f"
pkg_build_image="cix-builder"
# A composed build environment contains precisely what is declared here.
# No configure script, so this is the plain make list plus the two
# libraries CBS links and the headers it compiles against.
pkg_build_depends="tcc make linux-headers bash coreutils grep sed gawk binutils findutils diffutils tar gzip python curl libarchive zstd"
# ADR-0276: declare every linked library. The Makefile links -larchive
# -lzstd -ldl; -ldl resolves into libc on this glibc and needs no
# package of its own.
pkg_depends="libarchive zstd"
pkg_changelog="v0.1.23-6: disable errexit around the diagnostics. The -5 build printed nothing from the traced run because pkg_build inherits set -e from cixd, so the failing script aborted the recipe before its own rc could be reported. v0.1.23-5: trace cli-build-test.sh before running the suite. The -4 build got past the shebang and then failed at tests/cli-build-test.sh with a bare Error 1 -- the script runs under set -eu and so exits silently at its first failing command. v0.1.23-4: point tests/recipe-metadata-test.sh at /usr/bin/bash. It is the only one of the four test scripts with a #!/bin/bash shebang, and this platform has no /bin/bash, so make test died at Error 127 naming a file that is present -- the kernel ENOENT is for the interpreter. The suite has therefore never run on a Cix host. v0.1.23-3: build commit 0a5313f6, which is ahead of the v0.1.23 tag and has no tag of its own -- the version string is therefore inaccurate and this should be replaced by a tagged revision. Adds make test as a reported gate; the suite grew by repro, fuzz, seams, typed-package, policy and observe tests that had never run under this platform tcc. v0.1.23-2: define ZSTD_DISABLE_DEPRECATE_WARNINGS. The -1 build failed at zstd.h:72 -- TCC defines neither __GNUC__ nor _MSC_VER, takes the header fallback arm, and that arm emits a #pragma message TCC does not implement, which -Werror makes fatal. CBS keeps -Werror. v0.1.23-1: first build of cix-build-system on a Cix host, closing cix-build-system#124. TCC, against the Cix-built libarchive 3.8.1-4 and zstd 1.5.7-3. Builds and validates a real recipe; not integrated with cixd, and cix-build-system#132 remains open."

pkg_build() {
	# CC=tcc explicitly, never a bare cc: the build sandbox's default
	# compiler has silently become real GCC before (#109), and a build
	# system compiled by the wrong compiler is the least useful place
	# for that to go unnoticed.
	# CPPFLAGS carries -Isrc because passing it on the command line
	# replaces the Makefile assignment rather than adding to it.
	make CC=tcc CPPFLAGS="-Isrc -DZSTD_DISABLE_DEPRECATE_WARNINGS" -j"$(nproc)"

	# The whole suite, reported rather than fatal.
	sed -i '1s|^#!/bin/bash$|#!/usr/bin/bash|' tests/recipe-metadata-test.sh
	head -1 tests/recipe-metadata-test.sh
	# cixd runs recipes under `set -e`, so every diagnostic below must
	# be allowed to fail without taking the recipe with it -- the -5
	# revision lost its own trace to exactly that.
	set +e
	echo "==================== what the suite needs"
	for t in python3 python tar gzip sha256sum awk; do
		printf '  %-10s %s\n' "$t" "$(command -v $t 2>/dev/null || echo MISSING)"
	done
	echo "==================== cli-build-test.sh, traced"
	sh -x ./tests/cli-build-test.sh ./cbs > /run/cli.trace 2>&1
	echo "  rc=$?"
	tail -25 /run/cli.trace
	echo "==================== make test"
	make CC=tcc CPPFLAGS="-Isrc -DZSTD_DISABLE_DEPRECATE_WARNINGS" test
	echo "make test rc=$?"
	echo "==================== end make test"
	set -e

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
