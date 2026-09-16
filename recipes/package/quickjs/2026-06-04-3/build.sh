#
# quickjs 2026-06-04-3 -- a JavaScript parser the release build can run
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
# 64-BIT BIGINT LIMBS ARE OFF, via -U__SIZEOF_INT128__, and this fixes
# two things at once.
#
# The link problem: quickjs.h picks JS_LIMB_BITS 64 when the compiler
# has __int128, quickjs.c then typedefs its bigint double-limb to
# `unsigned __int128`, and gcc emits libgcc's TImode division helpers
# for it. Measured with nm on the -2 artifact: quickjs.o -- which is
# always pulled -- references __udivti3 and __udivmodti4, and no other
# member references any. tcc links libtcc1.a, not libgcc, and
# libtcc1.a has no TImode helpers (tcc has no __int128 at all), so a
# tcc consumer fails with `undefined symbol '__udivti3'`.
#
# The ABI problem, which is the more serious one: JS_LIMB_BITS lives
# in the PUBLIC header and selects the width of JSValueUnion's
# short_big_int member. Built with gcc the library chose 64; the gate
# compiles that same header under tcc, which predefines no
# __SIZEOF_INT128__ and so chose 32 -- library and consumer disagreeing
# about a public type. Benign for this gate as it happens (the union
# also holds a uint64_t, a double and a void*, so sizeof(JSValue) is 16
# either way, and only __JS_NewShortBigInt's store width differs, which
# a parse-only consumer never calls) but it is a divergence at a
# struct-by-value boundary, which is the class of bug that shows up as
# wrong data rather than an error.
#
# Undefining the macro makes gcc take the same arm tcc takes, so the
# header means one thing on both sides and the helpers are never
# emitted. The only cost is 32-bit bigint limbs -- slower BigInt
# arithmetic in a library that exists to parse and never runs a line.
# Verified against the tarball that every __int128 use is inside the
# JS_LIMB_BITS guard (quickjs.c's int128_t/uint128_t typedefs are the
# only ones; cutils.c's exchange_int128s is a function name, not a
# type), so undefining it removes the type rather than breaking a use.
#
# It goes through CC, not DEFINES: the Makefile assigns DEFINES with
# `:=`, so a command-line DEFINES= would silently drop -D_GNU_SOURCE
# and -DCONFIG_VERSION.
#
# NO quickjs-libc.o IN THE ARCHIVE. Upstream's libquickjs.a bundles
# quickjs-libc.o, the module loader and std/os bindings, and that
# member is the only one referencing dlopen/dlsym/dlclose, fork and
# execve (measured with nm on the -1 artifact: 11 such references
# there, 0 in quickjs.o/dtoa.o/libregexp.o/libunicode.o/cutils.o).
# The gate parses text and runs nothing, so it has no use for a
# loader that can open a shared object -- and carrying one forces a
# -ldl on every consumer. cix-builder's glibc 2.44-14 ships
# libdl.so.2 and libpthread.so.0 but NO libdl.so/libpthread.so
# linker stubs and no .a (it does ship libc.so and libm.so), so
# `tcc -ldl` fails with "library 'dl' not found" even though the
# symbols themselves live in libc.so.6 since glibc 2.34. That is
# what failed the v2.57.191 build. Dropping the member is the fix
# rather than hunting a stub, because the gate never wanted it.
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
pkg_version="2026-06-04-3"
pkg_source="https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz"
pkg_sha256="b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils make gcc binutils sed grep findutils"
pkg_depends=""
pkg_toolchain="gcc"
pkg_toolchain_reason="quickjs's Makefile adds -fwrapv unconditionally (its signed-overflow behaviour is load-bearing for a JS engine's integer semantics), which TCC does not implement. Measured 2026-09-17 against this tarball; re-measure on a compiler bump."
pkg_changelog="2026-06-04-3: build with -U__SIZEOF_INT128__. It drops quickjs.o's __udivti3/__udivmodti4 references, which tcc cannot resolve because it links libtcc1.a rather than libgcc, and it makes the gcc-built library agree with the tcc-compiled public header about JS_LIMB_BITS -- a switch that selects the width of a JSValueUnion member, so the two sides had disagreed about a public type. 2026-06-04-2: drop quickjs-libc.o from the archive -- it is the only member needing dlopen/fork/execve, and cix-builder has no libdl.so linker stub, so every consumer was forced into a -ldl that cannot resolve. The gate parses and runs nothing, so it never needed the module loader. 2026-06-04-1: first packaging. Library and header only, built with Cix gcc, so that test_web_syntax can parse web/*.js inside the release build on a Cix host (#340). No qjs/qjsc binaries: the gate links the library rather than shelling out, the same direction #410 and #411 took."

pkg_build() {
	# libquickjs.a is the no-LTO target, which is what a linker fed by
	# tcc can consume. CC by absolute path: a bare `gcc` computes its own
	# installation prefix relatively and fails to find cc1 with a
	# misleading "No such file or directory" (documented in CLAUDE.md).
	make CC="/usr/bin/gcc -U__SIZEOF_INT128__" AR=ar libquickjs.a

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

	# Remove the module loader. See the header note: it is the only
	# member wanting dlopen, and this image has no libdl.so to link
	# against.
	ar d libquickjs.a quickjs-libc.o
	ranlib libquickjs.a
	if ar t libquickjs.a | grep -q '^quickjs-libc\.o$'; then
		echo "quickjs: quickjs-libc.o is still in the archive" >&2
		exit 1
	fi

	# The gate that the drop actually bought something: no member may
	# reference dlopen, or a consumer needs -ldl again and this image
	# cannot supply it.
	if nm -u libquickjs.a | grep -qE ' (dlopen|dlsym|dlclose)$'; then
		echo "quickjs: the archive still references dlopen -- a consumer" >&2
		echo "         would need -ldl, which cix-builder cannot link" >&2
		exit 1
	fi

	# No libgcc TImode helper may be referenced. This is both the link
	# gate (tcc links libtcc1.a, which has none) and the ABI gate: a
	# TImode reference means gcc took the JS_LIMB_BITS 64 arm of the
	# public header, which is not the arm a tcc consumer compiles.
	if nm -u libquickjs.a | grep -qE ' __[a-z]+ti[0-9]+$'; then
		echo "quickjs: the archive references a libgcc TImode helper --" >&2
		echo "         -U__SIZEOF_INT128__ did not take, so gcc chose" >&2
		echo "         JS_LIMB_BITS 64 while a tcc consumer chooses 32" >&2
		nm -u libquickjs.a | grep -E ' __[a-z]+ti[0-9]+$' | sort -u >&2
		exit 1
	fi
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/lib" "$PKG_DESTDIR/usr/include"
	cp libquickjs.a "$PKG_DESTDIR/usr/lib/"
	cp quickjs.h "$PKG_DESTDIR/usr/include/"
}
