#
# libnl -- the netlink protocol library suite (libnl-3, libnl-genl-3,
# libnl-route-3 and friends).
#
# The larger half of #207. Three recipes need it and could not be
# rebuilt from source without it:
#
#   iw          libnl-3 + libnl-genl-3   (nl80211 wireless configuration)
#   hostapd     libnl-3 + libnl-genl-3
#   keepalived  libnl-3 + libnl-genl-3
#
# Like libmnl, this is a library the OLD shared build sandbox supplied
# ambiently from Debian, so those three exist today only as artifacts
# from that era and cannot be reproduced. Under a composed build
# environment (ADR-0199) the ambient supply is correctly gone.
#
# Upstream is the project's own GitHub release tarball. Note this is
# libnl3 -- the "libnl-3.11.0" naming is the 3.x series, not a
# different project from the "libnl" a consumer's configure asks for.
#
pkg_name="libnl"
pkg_version="3.11.0-5"
pkg_source="https://github.com/thom311/libnl/releases/download/libnl3_11_0/libnl-3.11.0.tar.gz"
pkg_sha256="2a56e1edefa3e68a7c00879496736fdbf62fc94ed3232c0baba127ecfa76874d"
pkg_depends=""
#
# The autotools baseline (docs/guides/writing-recipes.md), plus flex and
# bison: libnl's route library carries a real generated parser
# (lib/route/pktloc_syntax.y and pktloc_grammar.l). A release tarball
# normally ships the pre-generated output, so these may prove
# unnecessary -- but this is the one package in this batch that has a
# grammar at all, and a missing generator surfaces as a confusing
# mid-build error rather than a clean refusal.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils flex bison"
pkg_changelog="3.11.0-5: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 3.11.0-4: the -3 shim included <strings.h> for ffs(), but a forced-include header is processed before the TU defines _GNU_SOURCE, so it latched glibc's feature-test macros early and hid struct ucred -- build died on 'field nm_creds has incomplete type'. The shim is now self-contained, no system headers. 3.11.0-3: TCC implements none of __builtin_ffs, __builtin_bswap16, __builtin_bswap32 or __builtin_bswap64 and emits each as an undefined external instead, so the -2 build produced a libnl-route-3.so the #176 install gate correctly refused. Supplies them from a compatibility header. 3.11.0-2: the version-script guard used grep --include, which this build image's grep does not accept and read as a filename -- the guard failed on its own error before make ran. 3.11.0: first packaging -- unblocks iw, hostapd and keepalived, none of which could be rebuilt without netlink development files (#207)"

pkg_build() {
	#
	# TCC implements none of the four GCC builtins libnl uses, and does
	# not reject them either -- it emits each as an ordinary undefined
	# external symbol, so the library links, installs and is broken
	# (#176). Confirmed here by direct probe, not assumed:
	#
	#   __builtin_ffs       lib/route/link/bridge.c's find_next_bit()
	#   __builtin_bswap16   \
	#   __builtin_bswap32    > libnl's bundled linux/swab.h byte-order macros
	#   __builtin_bswap64   /
	#
	# The -2 build of this recipe shipped exactly that: configure, make
	# and make install all exited 0, and the install gate refused the
	# resulting libnl-route-3.so over an undefined __builtin_ffs. Note a
	# SHARED LIBRARY is what makes this silent -- an executable fails at
	# link with "undefined symbol", but undefined symbols are legal in a
	# .so, so nothing complains until something calls the function.
	#
	# The header takes no system includes at all -- see its own comment;
	# the -3 revision included <strings.h> here and broke an unrelated
	# struct declaration by latching glibc feature-test macros early.
	#
	# Definitions are plain `static`, deliberately NOT `static inline`:
	# TCC emits a static inline function defined in a shared header as a
	# strong global in every translation unit that includes it, which is
	# the "defined twice" link failure m4 hit (see CLAUDE.md). Verified
	# by nm that these come out file-local, and verified for correct
	# results across two translation units before use here.
	#
	cat > "$PWD/tcc-builtins.h" <<'BUILTINS'
#ifndef CIX_TCC_BUILTINS_H
#define CIX_TCC_BUILTINS_H
#if !defined(__GNUC__)

/*
 * No system headers here, deliberately. A forced-include header is
 * processed before the translation unit can define _GNU_SOURCE, so
 * including any libc header at this point latches glibc's
 * feature-test macros too early. That really happened: an earlier
 * revision included <strings.h> for ffs() and hid struct ucred from
 * nl-core.h, failing the build with "field 'nm_creds' has incomplete
 * type" -- an error naming nothing to do with builtins.
 *
 * Plain `static`, never `static inline`: TCC emits a static inline
 * function from a shared header as a strong global in every
 * translation unit, which is the "defined twice" failure m4 hit.
 */

static __attribute__((unused)) int cix_ffs(unsigned int v)
{
	int i;
	if (!v)
		return 0;
	for (i = 1; !(v & 1u); i++)
		v >>= 1;
	return i;
}

static __attribute__((unused)) unsigned short cix_bswap16(unsigned short v)
{
	return (unsigned short)((v >> 8) | (v << 8));
}

static __attribute__((unused)) unsigned int cix_bswap32(unsigned int v)
{
	return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
	       ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

static __attribute__((unused)) unsigned long long cix_bswap64(unsigned long long v)
{
	return ((unsigned long long)cix_bswap32((unsigned int)v) << 32) |
	       (unsigned long long)cix_bswap32((unsigned int)(v >> 32));
}

#define __builtin_ffs(x)     cix_ffs((unsigned int)(x))
#define __builtin_bswap16(x) cix_bswap16((unsigned short)(x))
#define __builtin_bswap32(x) cix_bswap32((unsigned int)(x))
#define __builtin_bswap64(x) cix_bswap64((unsigned long long)(x))
#endif
#endif
BUILTINS

	export CPPFLAGS="-include $PWD/tcc-builtins.h"

	CC=tcc ./configure --prefix=/usr --libdir=/lib/x86_64-linux-gnu \
	    --disable-static --disable-cli

	#
	# TCC cannot honour --version-script; libmnl and ipset hit exactly
	# this, and it is documented in docs/guides/writing-recipes.md.
	# Stripped from the GENERATED makefiles so upstream's own tree stays
	# unmodified, then asserted -- a sed that silently matched nothing
	# would leave the original link failure to be rediscovered later.
	#
	find . -name Makefile -exec sed -i 's/-Wl,--version-script[=,][^ ]*//g' {} +
	#
	# The guard uses find+grep rather than `grep -r --include=`: the
	# build image's grep does not accept --include, and parsed it as a
	# FILENAME, so the guard failed on its own error message and the
	# build died before make ever ran. A guard that cannot fail for its
	# own reasons is the whole point of having one.
	#
	if find . -name Makefile -exec grep -l -- '--version-script' {} + | grep -q .; then
		echo "version-script flag survived the strip" >&2
		exit 1
	fi

	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	mkdir -p "$PKG_DESTDIR/usr/lib/pkgconfig"
	if [ -d "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig" ]; then
		mv "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig"/*.pc \
		   "$PKG_DESTDIR/usr/lib/pkgconfig/"
		rmdir "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig"
	fi

	rm -f "$PKG_DESTDIR/lib/x86_64-linux-gnu"/*.la

	#
	# Assert what the three consumers actually need, rather than
	# trusting that configure and make exited 0 (#113). iw, hostapd and
	# keepalived all link libnl-3 AND libnl-genl-3, so a build that
	# produced only the core library would satisfy `make` and still be
	# useless to every one of them.
	#
	test -e "$PKG_DESTDIR/lib/x86_64-linux-gnu/libnl-3.so.200"
	test -e "$PKG_DESTDIR/lib/x86_64-linux-gnu/libnl-genl-3.so.200"
	test -e "$PKG_DESTDIR/usr/include/libnl3/netlink/netlink.h"
	test -e "$PKG_DESTDIR/usr/lib/pkgconfig/libnl-3.0.pc"
	test -e "$PKG_DESTDIR/usr/lib/pkgconfig/libnl-genl-3.0.pc"
}
