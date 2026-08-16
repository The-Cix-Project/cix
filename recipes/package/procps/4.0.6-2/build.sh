#
# procps -- ps/top/free/kill/pgrep/pkill/pidof/pidwait/pmap/pwdx/
# slabtop/tload/uptime/vmstat/w/watch/hugetop, plus sysctl (also
# available via iproute2's own sysctl-adjacent tooling, but this is
# the canonical one) -- the standard Linux process/system-inspection
# toolset. Same recipe contract as bash.recipe -- see that file's own
# header comment for the metadata-scanner-vs-sourced-shell-script split.
#
# Source is upstream procps-ng's own GitLab release tag archive (real,
# reproducible git-archive snapshot of the tagged release, the same
# kind of source distros already build straight from -- no separate
# "orig tarball" repackaging step exists for this project the way
# Debian's own packaging does for e.g. ipset/iputils). Checksum
# computed directly from the downloaded bytes (sha256sum), not taken
# from any third party.
#
pkg_name="procps"
pkg_version="4.0.6-2"
pkg_source="https://gitlab.com/procps-ng/procps/-/archive/v4.0.6/procps-v4.0.6.tar.gz"
pkg_sha256="1bbe8ff21dcd05a6adcda99a67d2e99cbd515c9e3a78fd3cc915b12aeb330d40"
pkg_depends=""

# Autotools, not meson -- confirmed directly (configure.ac/Makefile.am,
# no meson.build). A git-archive snapshot ships no pre-generated
# ./configure, so autogen.sh must run first; that needs autopoint (part
# of this project's own from-source gettext.recipe, unlike Debian's
# split packaging) plus autoconf/automake/libtoolize, all already real,
# working recipes in this project's own catalog.
#
# procps-ng's own autogen.sh calls autopoint/aclocal/automake/
# libtoolize as bare command names with no env-var override support
# (confirmed by reading it directly -- unlike libuv's own autogen.sh,
# which does support ACLOCAL=/AUTOMAKE=/LIBTOOLIZE=). The real,
# confirmed bug those tools hit here is the same one libuv.recipe's
# own comment documents in detail: this build sandbox's real automake/
# aclocal/libtoolize/autopoint are themselves real Perl scripts, and
# the kernel's own #! shebang-exec path fails to run them directly
# (ENOEXEC, so the shell's own fallback then mis-parses the Perl
# source as shell -- "package: command not found", confirmed live
# against this exact recipe before this fix), even though the exact
# same file runs fine when Perl is invoked on it explicitly. Since
# there's no env var to redirect here, small wrapper scripts are
# placed under their own real bare names (not "-wrap" suffixed, unlike
# libuv.recipe's own env-var-driven names) in a directory prepended to
# PATH ahead of autogen.sh, so procps-ng's own bare `automake`/etc.
# calls resolve to them first.
#
# --prefix=/usr matches every other recipe in this set; --disable-nls
# keeps this container-image build simple (no locale infrastructure
# needed for a diagnostic CLI toolset) -- confirmed via ./configure
# --help, no other non-default flag needed. ncurses (for top/watch)
# stays at its real upstream default (enabled) -- already real via
# this toolchain's own ncurses.recipe.
#
# Re-pinned to -2: autogen.sh's own real `autopoint` call failed --
# confirmed directly from captured build output -- with "infrastructure
# files for version 0.14.1 not found; this is autopoint from GNU
# gettext-tools 0.21": procps-ng's own configure.ac pins a real,
# decades-old AM_GNU_GETTEXT_VERSION([0.14.1]), and this toolchain's
# real gettext 0.21 (gettext.recipe) no longer bundles infrastructure
# archives that far back. Since --disable-nls below means this build
# never actually needs working i18n/gettext infrastructure at all, the
# autopoint wrapper is a genuine, targeted no-op here (unlike the other
# three tools' wrappers above it, which for real reasons -- the
# ENOEXEC/shebang gap this whole mechanism exists for -- still need to
# actually run) rather than trying to reconcile two real, independently
# correct version pins that were simply never meant to line up.
pkg_build() {
	mkdir -p /build/toolwrap
	for tool in aclocal automake libtoolize; do
		printf '#!/bin/sh\nexec perl /usr/bin/%s "$@"\n' "$tool" > "/build/toolwrap/$tool"
		chmod +x "/build/toolwrap/$tool"
	done
	printf '#!/bin/sh\nexit 0\n' > /build/toolwrap/autopoint
	chmod +x /build/toolwrap/autopoint

	echo "=== DIAG: gettext-0.10.35 internal layout ==="
	mkdir -p /build/gettext-extract
	tar xJf /usr/share/gettext/archive.dir.tar.xz -C /build/gettext-extract
	find /build/gettext-extract/gettext-0.10.35 -name 'Makefile.in.in' 2>&1
	find /build/gettext-extract/gettext-0.10.35 -name 'config.rpath' 2>&1
	echo "=== END DIAG ==="

	PATH="/build/toolwrap:$PATH" ./autogen.sh
	CC=tcc ./configure --prefix=/usr --disable-nls
	make -j"$(nproc)"
}

# ps/top/free/kill/pgrep/pkill/pidof/pidwait/pmap/pwdx/slabtop/tload/
# uptime/vmstat/w/watch/hugetop link against this package's own
# libproc2.so.1 (confirmed via ldd against a real `make install
# DESTDIR=...` -- top/watch additionally need libtinfo.so.6, already
# part of pkg_seed_image_baseline()'s own global runtime seeding, not
# copied again here). libproc2 is this recipe's own shared library, not
# a system one -- staged from this exact build's own DESTDIR output,
# not the host. sysctl lands in usr/sbin, matching where every other
# *-management binary in this project's own images already lives.
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin" "$PKG_DESTDIR/usr/sbin" "$PKG_DESTDIR/usr/lib"
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share" "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/usr/lib/pkgconfig" \
	       "$PKG_DESTDIR/usr/lib/libproc2.a" "$PKG_DESTDIR/usr/lib/libproc2.la" \
	       "$PKG_DESTDIR/usr/lib/libproc2.so"
}
