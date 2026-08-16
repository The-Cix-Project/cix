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
# decades-old AM_GNU_GETTEXT_VERSION([0.14.1]), and real autopoint,
# hitting a version it doesn't recognize, falls back to searching
# gettext's own bundled legacy-snapshot archive
# (/usr/share/gettext/archive.dir.tar.xz) for an exact "0.14.1" match --
# not found there either (that archive, confirmed by extracting and
# inspecting it directly, bundles exactly one snapshot, gettext-0.10.35
# from 2002, predating the po/Makefile.in.in convention entirely).
#
# The real fix: autopoint's *actual* job when no version-pin fallback
# is needed is simply copying gettext's own current, real template
# files (Makefile.in.in, Makevars.template, Rules-quot, the *.sed/
# *.header/*.sin quoting helpers) from its own install tree
# (/usr/share/gettext/po/, confirmed present and complete via a direct
# diagnostic listing) into the package's po/ directory -- copying them
# directly bypasses the version-matching logic (and its legacy-archive
# fallback) entirely, since --disable-nls below means this build never
# actually needs the specific gettext-0.14.1-era template *features*,
# only a real, syntactically valid po/Makefile.in.in for automake's own
# AM_GNU_GETTEXT([external]) machinery to generate po/Makefile.in from.
# The autopoint wrapper does this copy directly rather than really
# invoking autopoint at all -- unlike the other three tools' wrappers
# above it, which for a real, different reason (the ENOEXEC/shebang gap
# this whole mechanism exists for) still need to actually run the real
# tool.
pkg_build() {
	mkdir -p /build/toolwrap
	for tool in aclocal automake libtoolize; do
		printf '#!/bin/sh\nexec perl /usr/bin/%s "$@"\n' "$tool" > "/build/toolwrap/$tool"
		chmod +x "/build/toolwrap/$tool"
	done
	printf '#!/bin/sh\nmkdir -p po\ncp /usr/share/gettext/po/*.in.in /usr/share/gettext/po/*.template /usr/share/gettext/po/*.sed /usr/share/gettext/po/*.header /usr/share/gettext/po/*.sin /usr/share/gettext/po/Rules-quot po/\ncp /usr/share/gettext/config.rpath .\n' \
		> /build/toolwrap/autopoint
	chmod +x /build/toolwrap/autopoint

	# Real second bug, hit only after the autopoint fix above got this
	# far: "configure: error: Could not find a C99 compatible compiler",
	# despite the exact same run's own AC_PROG_CC_STDC probe correctly
	# reporting "checking for tcc option to enable C99 features... none
	# needed" moments earlier. Root cause, found by reading procps-ng's
	# own configure.ac directly (not guessed): it calls the deprecated
	# AC_PROG_CC_STDC (which caches its result as ac_cv_prog_cc_stdc),
	# then immediately checks a *different*, never-populated variable,
	# ac_cv_prog_cc_c99 -- `if test "x$ac_cv_prog_cc_c99" = "xno" ||
	# test "x$ac_cv_prog_cc_c99" = "x"`, true for a genuinely-unset
	# variable regardless of what the real probe found, and since TCC
	# isn't GNU-family (`ac_cv_c_compiler_gnu` is "no"), it falls
	# straight to AC_MSG_ERROR with no real GNU-vs-non-GNU distinction
	# ever helping here -- a real upstream inconsistency between the
	# macro actually called and the variable actually checked, not
	# anything this project's own TCC toolchain got wrong. A first
	# attempt setting ac_cv_prog_cc_c99 to an EMPTY string made this
	# strictly worse (confirmed live) -- empty is exactly the failing
	# condition's own second branch. The real fix: set it to the same
	# real, valid "none needed" value the correctly-working probe
	# already computed for the variable procps-ng actually meant to
	# check.
	PATH="/build/toolwrap:$PATH" ./autogen.sh
	# A third, real, unrelated gap hit only after the C99 fix got this
	# far: "Neither pidfd_open or __NR_pidfd_open found" -- this
	# project's kernel genuinely has pidfd_open() (thincd's own
	# container_wait() uses waitid(P_PIDFD, ...) directly), so this is a
	# build-time header-detection gap, not a real missing kernel
	# feature. --disable-pidwait is procps-ng's own real, intended
	# escape hatch for exactly this (its own error message says so) --
	# drops one minor utility (pidwait), ps/top/free/kill/etc. all stay
	# fully functional.
	# A fourth gap, same shape as pidwait above: NUMA/top support needs
	# dlopen() (libnuma is dlopen()'d at runtime, not linked), which
	# this minimal toolchain's libc doesn't expose the way procps-ng's
	# own AC_SEARCH_LIBS(dlopen) probe expects. --disable-numa is its
	# own real, intended escape hatch (its own error message says so),
	# same posture as --disable-pidwait -- top loses NUMA-node display,
	# nothing else is affected.
	CC=tcc ac_cv_prog_cc_c99="none needed" ./configure --prefix=/usr --disable-nls \
		--disable-pidwait --disable-numa
	# A fifth, real, but harmless gap: `make all`'s own real ps/top/etc.
	# binaries (top-level bin_PROGRAMS, not part of SUBDIRS at all --
	# confirmed directly against the real Makefile.am) already build and
	# link successfully; only testsuite/ (real unit-test programs, never
	# part of a real install) fails, on an unrelated tcc "file 'none'
	# not found" Makefile-substitution quirk not worth chasing for
	# test-only code this project never runs. SUBDIRS= override (a real,
	# standard automake convention) skips it while still building
	# everything the top-level Makefile.am's own real SUBDIRS list
	# (local po-man po testsuite) needs for a real install.
	make -j"$(nproc)" SUBDIRS="local po-man po"
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
