#
# automake -- GNU Automake: aclocal/automake (both also installed
# under their own versioned names, aclocal-1.16/automake-1.16, the same
# two-name convention Debian and every other real distro ships this
# under). The macro/data directories this recipe installs
# (usr/share/aclocal-1.16, usr/share/automake-1.16) are the exact same
# real, load-bearing directories test/test_image_fixture.c's own
# extras[] already stages into the isolated BUILD sandbox for this
# project's own procps.recipe -- this recipe makes the equivalent
# content real and installable into a target runtime image instead.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against two independent mirrors (ftp.gnu.org and mirrors.kernel.org)
# -- byte-identical, same sha256.
#
pkg_name="automake"
pkg_version="1.16.5-3"
pkg_source="https://ftp.gnu.org/gnu/automake/automake-1.16.5.tar.xz"
pkg_sha256="f01d58cd6d9d77fbdca9eb4bbd5ead1988228fdb73d6f7a201f5f8d6b118b469"
pkg_artifact_sha256="67501e08e911faf7a7b7a94753cbbd0596054478a8387cafeaf20dd8d6deef10"
pkg_depends="autoconf"

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168). Revision bumped purely to carry this: a recipe
# version is immutable once published, so it could never reach a host
# that already has 1.16.5.
# ./configure && make, and autoconf is a real build input -- automake regenerates its own configure machinery.
pkg_build_depends="bash coreutils make tcc linux-headers sed grep gawk binutils m4 perl autoconf findutils diffutils"
pkg_changelog="1.16.5-3: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

# Needs a real autoconf present (pkg_depends="autoconf" above,
# transitively pulling in m4/perl/gawk) -- automake's own ./configure
# probes for it, and aclocal's real-world job is expanding autoconf's
# own macro files. Plain autotools otherwise, confirmed directly --
# aclocal/automake are themselves real Perl scripts (sed-substituted
# from bin/aclocal.in/bin/automake.in), not compiled binaries.
pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

# Real, load-bearing runtime dependency confirmed via shebang
# inspection: aclocal/automake (and their -1.16-suffixed twins) are all
# #!/usr/bin/perl (pkg_depends's own transitive "autoconf" ->
# "perl" already covers this, not restated separately). Everything
# under usr/share/aclocal-1.16 (automake's own .m4 macro contributions)
# and usr/share/automake-1.16 (the real Automake::* Perl modules, plus
# the install-sh/missing/depcomp/config.guess/config.sub/compile/
# ar-lib/ylwrap boilerplate scripts --add-missing copies into any
# project using this tool) is load-bearing runtime data, not
# documentation, so it's kept in full -- the same "can't run without
# it" reasoning autoconf.recipe's own usr/share/autoconf already
# established. usr/share/aclocal (just a shared README + the directory
# itself, aclocal's own default -I search path) is kept too since
# aclocal expects it to exist.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share/info" "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc"
}
