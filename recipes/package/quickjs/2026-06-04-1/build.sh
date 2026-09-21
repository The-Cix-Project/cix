#
# quickjs 2026-06-04-1 -- a JavaScript parser the release build can run
# (#340).
#
# The dashboard is the only surface this project ships that no compiler
# ever reads. cixd, cixctl, every test and every recipe go through tcc;
# web/*.js goes through nothing. On 2026-09-08 a dangling `else` left
# behind by a refactor shipped and took the ENTIRE web UI down on a
# deployed host -- a file that does not parse runs no script at all, so
# it is a blank dashboard rather than one broken feature, and it was
# found by the owner opening the page.
#
# `make web-syntax` has covered the dev sandbox since, using node. It
# cannot join SELFTESTS, which runs in a build container on the box, and
# that is the gap this package closes.
#
# WHY NOT A TEXTUAL CHECK IN C. Considered and rejected on evidence
# (#340): the broken shape was an `else` whose preceding token is `;`,
# which is perfectly legal after a braceless `if`, and this codebase
# uses braceless `if` freely. Telling the two apart needs a real parser.
# web/app.js also carries regex literals, which is exactly where
# hand-rolled JS tokenizers go wrong, and a lint with false positives on
# the release gate is worse than no gate.
#
# WHY NOT node, WHICH IS ALREADY PACKAGED. recipes/package/node exists
# (24.21.0-1) and has never been built: it compiles V8 from source with
# -j2 against this host's ~7.7 GiB, which is hours for a syntax check.
# quickjs is ~600 KB of C that builds in seconds and implements ES2020,
# which is what web/app.js actually needs -- measured: 258 async
# functions, 401 awaits, 436 arrow functions, 33 template literals.
# duktape was ruled out on the same measurement: it has no async/await,
# so it would report syntax errors on valid code, which is the false
# positive this gate must not have.
#
# STATIC ONLY, deliberately, against this platform's usual rule. zlib,
# libsodium, zstd and libarchive all refuse a static-only outcome
# because the platform links dynamically always -- but their consumers
# are things the platform RUNS. This library's only consumer is
# test_web_syntax, built inside the build container and thrown away with
# it; nothing ships it. ADR-0251's finalize policy keeps an archive with
# no shared counterpart for exactly this reason, so the artifact is the
# archive and its header and nothing else.
#
pkg_name="quickjs"
pkg_version="2026-06-04-1"
pkg_source="https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz"
pkg_sha256="b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a"
pkg_artifact_sha256="1c6c51882fb149d5dd7cba6d017c9200c552dd697d5e842bc9f295cdbe07a1d6"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils make gcc binutils sed grep findutils"
pkg_depends=""
pkg_toolchain="gcc"
pkg_toolchain_reason="quickjs's Makefile adds -fwrapv unconditionally (its signed-overflow behaviour is load-bearing for a JS engine's integer semantics), which TCC does not implement. Measured 2026-09-17 against this tarball; re-measure on a compiler bump."
pkg_changelog="2026-06-04-1: first packaging. Library and header only, built with Cix gcc, so that test_web_syntax can parse web/*.js inside the release build on a Cix host (#340). No qjs/qjsc binaries: the gate links the library rather than shelling out, the same direction #410 and #411 took."

pkg_build() {
	# libquickjs.a is the no-LTO target, which is what a linker fed by
	# tcc can consume. CC by absolute path: a bare `gcc` computes its own
	# installation prefix relatively and fails to find cc1 with a
	# misleading "No such file or directory" (documented in CLAUDE.md).
	make CC=/usr/bin/gcc AR=ar libquickjs.a

	# Prove the archive carries the one symbol the gate is for, before
	# anything downstream depends on it. A library that builds but does
	# not export JS_Eval is a link failure one build cycle later and
	# somewhere else.
	if ! ar t libquickjs.a | grep -q '^quickjs\.o$'; then
		echo "quickjs: libquickjs.a has no quickjs.o" >&2
		exit 1
	fi
	if ! nm libquickjs.a | grep -q ' T JS_Eval$'; then
		echo "quickjs: libquickjs.a does not export JS_Eval" >&2
		exit 1
	fi
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/lib" "$PKG_DESTDIR/usr/include"
	cp libquickjs.a "$PKG_DESTDIR/usr/lib/"
	cp quickjs.h "$PKG_DESTDIR/usr/include/"
}
