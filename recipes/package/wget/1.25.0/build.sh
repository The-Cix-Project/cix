#
# wget -- the GNU non-interactive network downloader, asked for by name
# alongside telnet/curl/git for the `jump` box's own interactive use.
# This is a real user-facing tool here, not infrastructure: cixd's own
# host-side fetches go through curl (PKG_CURL_BIN, daemon/src/pkg.c),
# and nothing in this project shells out to wget.
#
# Read by cixd's own non-executing metadata scanner (pkg_name=/
# pkg_version=/pkg_source=/pkg_sha256=/pkg_depends=, daemon/src/pkg.c's
# parse_recipe()) AND sourced as a real POSIX shell script inside the
# isolated, network-less build container (". /build/recipe.sh" --
# daemon/include/pkg.h's own documented contract) to run pkg_build()/
# pkg_install() below. Never sourced or executed on the host itself.
#
# Source is the GNU release tarball, fetched from mirrors.kernel.org
# rather than ftp.gnu.org -- the established convention here (glibc and
# m4 already do), kept after ftp.gnu.org spent a period accepting this
# site's connections and then answering nothing. Checksum verified two
# ways, both downloads done and compared rather than trusted: the
# mirror's copy and ftp.gnu.org's own copy are byte-identical at
# 5,263,736 bytes with the same sha256 (measured 2026-09-13).
#
pkg_name="wget"
pkg_version="1.25.0"
pkg_source="https://mirrors.kernel.org/gnu/wget/wget-1.25.0.tar.gz"
pkg_sha256="766e48423e79359ea31e41db9e5c289675947a7fcf2efdcedb726ac9d0da3784"
pkg_depends="openssl zlib"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md. binutils is load-bearing and not
# boilerplate: wget's configure builds lib/libgnu.a, a gnulib
# convenience archive, so the build genuinely needs ar/ranlib -- and
# cix-builder carries no binutils unless a recipe says so, which is
# exactly the failure sysklogd hit.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf openssl zlib"
pkg_changelog="1.25.0: first revision. Asked for by name for the jump box."

# Two things here are not stylistic.
#
# CC=tcc explicitly: the cumulative build sandbox's bare `cc` can no
# longer be trusted to mean TCC since gcc's own output merged into it,
# and a recipe that relies on the default has been measured picking up
# real GCC with hardening flags TCC never emits (openssh's -2 revision,
# ADR-0144). Every configure-driven recipe pins it now.
#
# -D_GL_EXTERN_INLINE_STDHEADER_BUG=1: wget is gnulib-based, and
# gnulib's own _GL_INLINE selection trusts __STDC_VERSION__ alone when
# __GNUC__ is undefined -- which it is under TCC. It therefore picks
# the "real C99 inline works" branch, emits a strong external
# definition of every shared helper in every translation unit that
# includes the header, and the final link fails with dozens of
# "defined twice" symbols that appear exactly once in the source. This
# define is gnulib's own designed-in escape hatch (any value; only
# `defined` is checked) and selects `static _GL_UNUSED`, which is
# genuinely file-local. m4's recipe carries the full diagnosis.
#
# What is turned off, and why each is a real decision rather than
# minimalism for its own sake: --disable-nls because this toolchain
# has no msgfmt staged (the same gap git's recipe records as
# NO_GETTEXT=1) and translated messages are not functionality;
# --disable-iri and --without-libidn because internationalised
# hostnames need libidn2, which has no recipe here; --without-libpsl
# because the public-suffix list needs libpsl, which has no recipe
# either and only affects cookie-domain policy; --without-metalink
# because it needs libmetalink and gpgme; --without-libcares because
# asynchronous DNS needs c-ares and glibc's resolver is what every
# other binary in this image already uses; --without-libpcre/pcre2
# because --regex-type=pcre is an optional matcher and POSIX regex
# stays available. TLS (OpenSSL) and gzip/deflate decoding (zlib) are
# the two real, load-bearing dependencies and both are declared above.
pkg_build() {
	CC=tcc AR=ar RANLIB=ranlib \
	    CFLAGS="-D_GL_EXTERN_INLINE_STDHEADER_BUG=1" \
	    ./configure --prefix=/usr \
	                --with-ssl=openssl \
	                --with-zlib \
	                --without-libpsl \
	                --without-libidn \
	                --without-metalink \
	                --without-libcares \
	                --without-libpcre \
	                --without-libpcre2 \
	                --disable-iri \
	                --disable-nls \
	                --disable-rpath
	make -j"$(nproc)"
}

# The one file this package exists to ship is /usr/bin/wget. Everything
# else `make install` writes is documentation, the po/ tree and a
# sample wgetrc under /usr/etc -- dropped, matching every other
# recipe's own doc-stripping convention. The test is not decoration:
# an install that silently produced no binary would otherwise publish
# an artifact containing nothing.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share" "$PKG_DESTDIR/usr/etc"
	test -x "$PKG_DESTDIR/usr/bin/wget"
}
