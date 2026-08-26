#
# dhcpcd -- a real DHCP client, so this platform's own DHCP service
# (ADR-0197) can be verified by watching a lease happen rather than by
# trusting that dnsmasq does what its manual says (issue #106).
#
# Chosen over busybox's udhcpc for one reason: udhcpc is not separable
# from busybox, so using it would mean carrying a busybox build and its
# whole configuration just to get one client. dhcpcd is a single
# tarball with no dependencies beyond libcrypto, which openssl.recipe
# already provides.
#
# --disable-privsep: dhcpcd 10 drops to an unprivileged user by
# default, and this project's minimal images have no /etc/passwd for
# getpwnam() to find one in -- the same gap chrony.recipe hit and
# documented (a binary that merely LOOKS UP its configured user fails
# here, even when it would never actually change uid). Rather than
# staging an account for a privilege drop that buys nothing inside a
# container that is already isolated by namespaces, the separation is
# compiled out.
#
# IPv6/DHCPv6 disabled: this platform's own networking is IPv4
# throughout (network_def carries a single uint32_t address), so a v6
# client would be dead code with its own attack surface.
#
# --disable-auth is set too, and it does NOT remove the openssl
# dependency -- checked rather than assumed. Disabling DHCP
# authentication (RFC 3118, which nothing here uses) drops
# compat/crypt_openssl/hmac.o, but configure still adds -lcrypto and a
# link without it fails on OPENSSL_init_crypto: dhcpcd uses libcrypto
# for its DUID/hashing too. So this depends on openssl regardless, and
# that has a real consequence worth stating: openssl depends on perl,
# and perl cannot currently be built into a FRESH image at all --
# issue #55, gcc's include-fixed/limits.h never gets fixincludes-
# stitched, so INT_MAX/UINT_MAX come out undeclared. Confirmed
# directly by trying it. Until #55 is fixed, install this into an
# image that already carries openssl.
#
pkg_name="dhcpcd"
pkg_version="10.5.2-5"
pkg_source="https://github.com/NetworkConfiguration/dhcpcd/releases/download/v10.5.2/dhcpcd-10.5.2.tar.xz"
pkg_sha256="3e476657fdb6eeb38b277da3a48d0ac0113ecce5858ebdcddff2b629faed52b4"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/dhcpcd-10.5.2-5.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="b985187524896c82171f6d227128c1250aa87350d3c73476dbbc5745eebeff58"
pkg_depends="openssl"

pkg_build() {
	#
	# A real, newly-confirmed TCC conformance gap, not a dhcpcd bug:
	# TCC does not implement __attribute__((alias)). compat/setproctitle.c
	# defines the implementation as a static function and then exposes
	# it under the public name purely through an alias attribute --
	# so TCC compiles the file happily, emits no `setproctitle` symbol
	# at all, and the final link fails with "undefined symbol
	# 'setproctitle'" while the object file that should define it is
	# right there on the command line.
	#
	# Confirmed by inspection of the object (`nm` shows
	# setproctitle_init/_fini as real T symbols and setproctitle_impl
	# as a local `t`, with no setproctitle of any kind), not guessed
	# from the error.
	#
	# The fix makes the implementation itself the public function and
	# drops the symbol-versioning tail, which exists to give a SHARED
	# library two ABI versions of one symbol -- something this build,
	# which links dhcpcd statically against its own compat objects,
	# has no use for. Nothing about the function's behaviour changes.
	#
	python3 - <<'PATCH'
p = "compat/setproctitle.c"
s = open(p).read()
i = s.index("libbsd_symver_default(setproctitle, setproctitle_impl, LIBBSD_0.5);")
s = s[:i] + "\n"
s = s.replace("__printflike(1, 2) static void\nsetproctitle_impl(const char *fmt, ...)",
              "__printflike(1, 2) void\nsetproctitle(const char *fmt, ...)")
open(p, "w").write(s)
PATCH

	#
	# stdalign.h is a COMPILER-provided C11 header, and the real build
	# image has none -- confirmed the hard way: this recipe built clean
	# in a dev sandbox (where gcc's own copy is on the include path)
	# and then failed on the real image with "include file
	# 'stdalign.h' not found" at dhcp.c:53. The same class of
	# environment difference sysklogd.recipe hit with binutils: built
	# in one sandbox proves nothing about the other.
	#
	# dhcpcd uses exactly one thing from it (`alignas` on a single
	# receive buffer), and C11 defines the whole header as four
	# macros -- so the honest fix is to supply it rather than to drag
	# in a compiler's headers this project deliberately does not use.
	# Written at the tree root, which is already on the include path
	# (-I..), so no build-system change is needed.
	#
	# alignas maps to __attribute__((aligned)), NOT to the C11
	# _Alignas keyword: the build image's own TCC does not implement
	# _Alignas at all, and the first attempt at this header failed
	# there with "';' expected (got \"uint8_t\")" while compiling
	# cleanly in a dev sandbox whose TCC does. A second build-image-
	# only difference in one recipe, and worth stating plainly: even
	# if this TCC also ignores the attribute, the single use is a
	# receive buffer later read through a `struct ip *` on x86-64,
	# which performs unaligned loads correctly -- so the build is
	# right on this platform either way, and the attribute carries the
	# intent for any architecture where it would not be.
	#
	cat > stdalign.h <<'STDALIGN'
/* Supplied by dhcpcd.recipe: this build image has no compiler-provided
 * stdalign.h. C11 7.15 defines the header as exactly these four
 * macros. alignas uses the attribute form deliberately -- this
 * image's TCC has no _Alignas keyword; see the recipe. */
#ifndef _STDALIGN_H
#define _STDALIGN_H
#define alignas(x) __attribute__((__aligned__(x)))
#define alignof _Alignof
#define __alignas_is_defined 1
#define __alignof_is_defined 1
#endif
STDALIGN

	# CC=tcc explicitly: the build sandbox's own bare `cc` can no
	# longer be trusted to mean TCC (see CLAUDE.md on openssh.recipe's
	# own -2 revision).
	CC=tcc ./configure \
		--prefix=/usr \
		--sysconfdir=/etc \
		--libexecdir=/usr/libexec \
		--dbdir=/var/lib/dhcpcd \
		--rundir=/run \
		--disable-privsep \
		--disable-inet6 \
		--disable-dhcp6 \
		--disable-auth
	#
	# The __dso_handle gap this project has already met once
	# (sysklogd.recipe, CLAUDE.md): on this build image's TCC/glibc
	# combination the CRT does not provide the symbol glibc's static-
	# destructor bookkeeping expects, and the final link fails with
	# "undefined symbol '__dso_handle'". weak, so it is silently
	# superseded anywhere the toolchain does provide a real one --
	# harmless in a sandbox where the gap does not reproduce, and the
	# missing definition where it does.
	#
	cat > dso_handle.c <<'DSO'
void *__dso_handle __attribute__((weak)) = (void *)0;
DSO
	tcc -c dso_handle.c -o dso_handle.o

	# LDADD is assigned rather than appended: config.mk already put
	# -lcrypto in it, so it is restated here instead of lost.
	make LDADD="-lcrypto $(pwd)/dso_handle.o"
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/sbin" \
	         "$PKG_DESTDIR/usr/libexec/dhcpcd-hooks" \
	         "$PKG_DESTDIR/var/lib/dhcpcd" \
	         "$PKG_DESTDIR/etc"
	cp src/dhcpcd "$PKG_DESTDIR/usr/sbin/"
	# The hook script and its hooks are how dhcpcd applies a lease --
	# without them it obtains an address and does nothing with it.
	cp hooks/dhcpcd-run-hooks "$PKG_DESTDIR/usr/libexec/"
	chmod 0755 "$PKG_DESTDIR/usr/libexec/dhcpcd-run-hooks"
	for h in hooks/01-test hooks/20-resolv.conf hooks/30-hostname; do
		[ -f "$h" ] && cp "$h" "$PKG_DESTDIR/usr/libexec/dhcpcd-hooks/"
	done
	cp src/dhcpcd.conf "$PKG_DESTDIR/etc/dhcpcd.conf"
}
