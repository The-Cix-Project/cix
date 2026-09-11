#
# libsodium -- the crypto library minisign is built on (#406).
#
# Here for one reason, stated plainly because it is a real cost and the
# package has no other consumer yet: test_releasekey is in SELFTESTS and
# has SKIPPED on every release since ADR-0220, because its oracle is
# stock minisign and minisign was in no recipe and no image. A test that
# prints one SKIP line and exits 0 inside a release gate is invisible in
# a log that only checks the exit status, and the release-signature
# format -- the last thing between a substituted installer ISO and a
# machine that boots it -- has therefore never been gated on a Cix host.
#
# Built with TCC, and that is a measurement rather than a hope. The two
# documented TCC hazards for a crypto library were both checked against
# this exact tarball before the recipe was written:
#
#   - __builtin_bswap16/32/64 are STILL missing from the pinned tcc
#     (#208), and are the dangerous kind: undefined symbols are legal in
#     a shared library, so a .so links, installs and stays broken until
#     something calls it. libsodium 1.0.20 does not use them anywhere --
#     `grep -rl __builtin_bswap src/` finds nothing.
#   - __int128 is not implemented by TCC. libsodium probes for it rather
#     than assuming it (HAVE_TI_MODE), so configure simply selects the
#     portable 32-bit limb code. That is a slower curve25519, not a
#     wrong one.
#
# --disable-asm because the assembly paths are the other thing TCC
# cannot take, and because a constant-time guarantee written in
# hand-rolled SSE is worth less here than one the C compiler we actually
# use can be held to.
#
# daemon/src/elfcheck.c's install-time gate is the backstop either way:
# it refused a libnl built with those same missing builtins, and it
# would refuse this.
#
pkg_name="libsodium"
pkg_version="1.0.20-2"
pkg_source="https://download.libsodium.org/libsodium/releases/libsodium-1.0.20.tar.gz"
pkg_sha256="ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19"
pkg_build_image="cix-builder"
# An autotools configure shells out constantly, and a composed build
# environment has only what is declared here -- the -1 revision listed
# neither grep nor gawk and died in configure with "grep: command not
# found", then "no acceptable egrep could be found". This is chrony's
# list, which is the reference for a tcc-plus-autotools build.
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils"
pkg_depends=""
pkg_changelog="1.0.20-2: declare grep, gawk, findutils and diffutils. The -1 build died in configure with 'grep: command not found' and then 'no acceptable egrep could be found' -- a composed build environment contains precisely what the recipe declares, which is the mistake wireless-regdb 2026.09.03-2 documents at length and this recipe made anyway. 1.0.20-1: libsodium, so minisign can be built and test_releasekey's oracle half stops skipping (#406)."

pkg_build() {
	# CC=tcc explicitly, never a bare `cc`: this sandbox's default
	# compiler has silently become real GCC before (#109, openssh's
	# -2 revision), and a crypto library is the last place to discover
	# that a recipe did not build what it says it did.
	CC=tcc ./configure \
	    --prefix=/usr \
	    --libdir=/usr/lib \
	    --disable-asm \
	    --disable-static
	make -j"$(nproc)"
}

pkg_install() {
	# The same refusal zlib makes, for the same reason: a silent fall
	# back to static-only produces a package whose only consumer then
	# fails to link, one build cycle later and somewhere else.
	if [ ! -f src/libsodium/.libs/libsodium.so ]; then
		echo "libsodium: no shared library was built -- configure fell back to static-only" >&2
		exit 1
	fi
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/lib"/*.a
}
