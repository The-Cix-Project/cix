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
pkg_version="4.0.6-6"
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
	# project's kernel genuinely has pidfd_open() (cixd's own
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
	# "none needed" was wrong in a different way than the empty-string
	# attempt: procps-ng's own Makefile.am literally substitutes
	# $ac_cv_prog_cc_c99 straight into CC (confirmed directly from a
	# verbose (V=1) build's own real compile command: `tcc none needed
	# -DHAVE_CONFIG_H ...`) -- "none needed" is autoconf's own *display*
	# text for the "checking ..." line, never meant to be used as a
	# literal compiler argument, and TCC then tried to compile a source
	# file literally named "none". The value this variable actually
	# needs to hold is a real, valid, harmless compiler flag -- exactly
	# the same -std=gnu99 procps-ng's own GNU-compiler code path already
	# appends in the working case (see the `if test "x$ac_cv_c_compiler_
	# gnu" = "xyes"` branch above in configure.ac) -- non-empty (passes
	# procps' own buggy check) and syntactically harmless either way
	# (TCC accepts -std= flags without erroring).
	# A sixth, real, already-known gap (this project's own established
	# TCC-vs-glibc-regex.h issue, daemon/src/logstore.c's own include-
	# block comment documents the identical root cause): src/sysctl.c
	# includes <regex.h>, whose real regexec() prototype uses a genuine
	# C99 VLA-in-prototype size expression TCC's parser rejects
	# ("__nmatch undeclared"). The header's own _REGEX_NELTS macro
	# already has a #ifndef __STDC_NO_VLA__ branch for exactly a
	# compiler without VLA support -- defining it via CFLAGS (never
	# editing procps' own unmodified upstream source) steers the header
	# onto the branch TCC parses fine.
	# A seventh gap: src/ps/global.c's own real code (not a preprocessor
	# #if, confirmed by reading the exact line -- a plain diagnostic
	# fprintf(..., __GNUC__, __GNUC_MINOR__) printing compiler version
	# info) references __GNUC__/__GNUC_MINOR__ as real C identifiers.
	# TCC deliberately never predefines these (unlike its own confirmed
	# predefinition of bare `linux`, __GNUC__ specifically staying
	# undefined is intentional -- it's what keeps glibc's own headers
	# off GCC-specific extended-asm/builtin code paths TCC can't
	# handle). Defining it broadly (e.g. -D__GNUC__=1) risks silently
	# activating #ifdef __GNUC__ branches elsewhere in glibc's own
	# headers this build never exercised before -- confirmed by reading
	# this exact call site that it's cosmetic-only, so only the two
	# literal identifiers this one fprintf() needs get defined, nothing
	# broader.
	# An eighth gap (issue #15) -- and it turned out to be SELF-INFLICTED
	# by the seventh fix above, which is why it only appeared once that
	# one was in place. configure died at:
	#
	#   checking for tcc -std=gnu99 options needed to detect all
	#     undeclared functions... cannot detect
	#   configure: error: cannot make tcc -std=gnu99 report undeclared
	#     builtins
	#
	# Autoconf's _AC_UNDECLARED_BUILTIN probe (autoconf/general.m4) runs
	# two compiles and needs BOTH to behave: a program referencing an
	# undeclared `strchr` must FAIL, and a correct program including
	# <float.h> <limits.h> <stdarg.h> <stddef.h> must SUCCEED.
	#
	# Verified directly against this project's own tcc 0.9.27 rather than
	# assumed: with no flags at all TCC already satisfies both (it errors
	# with "'strchr' undeclared" on the first and compiles the second
	# cleanly). The probe was failing purely because of a flag this
	# recipe itself adds -- `-D__GNUC__=0`, added for gap seven so
	# src/ps/global.c's cosmetic version-printing fprintf would compile.
	#
	# With __GNUC__ defined, glibc's own /usr/include/limits.h:124 takes
	# its GCC branch:
	#
	#   #if defined __GNUC__ && !defined _GCC_LIMITS_H_
	#   # include_next <limits.h>          <- expects GCC's own copy
	#   #endif
	#
	# ...and TCC has no such next header on this include path, so every
	# translation unit including <limits.h> died with "include file
	# 'limits.h' not found". Gap seven's own comment explicitly warned
	# that defining __GNUC__ "risks silently activating #ifdef __GNUC__
	# branches elsewhere in glibc's own headers" -- this is precisely
	# that risk landing.
	#
	# The fix is targeted at the one branch actually being tripped rather
	# than at the probe: also define _GCC_LIMITS_H_, glibc's own documented
	# "GCC's limits.h has already been included" marker, so that branch is
	# simply not taken. Confirmed by real compile+run, not reasoning: the
	# resulting binary reports INT_MAX=2147483647 and CHAR_BIT=8, i.e. the
	# limits are genuinely correct and nothing is lost -- and both halves
	# of autoconf's probe then behave exactly as it requires, so it
	# succeeds honestly ("none needed") instead of being bypassed with a
	# pre-seeded cache variable.
	# A TENTH gap, at link time this time: `tcc: error: undefined symbol
	# '__dso_handle'`. This is the same real, environment-specific
	# TCC/glibc CRT gap sed/sysklogd/m4/coreutils/grep/bison/gawk already
	# document -- the real build sandbox has no ambient __dso_handle the
	# way a dev workstation's glibc might. Fixed the same proven way this
	# catalog already established: a `weak` stub object passed as a bare
	# object-file path in LIBS (never `-lxxx`, which m4.recipe's own trail
	# found gets conditionally-extracted away for a weak-only archive
	# member on this toolchain).
	echo 'void *__dso_handle __attribute__((weak)) = (void *)0;' > dso_stub.c
	tcc -c dso_stub.c -o dso_stub.o

	CC=tcc ac_cv_prog_cc_c99="-std=gnu99" LIBS="$(pwd)/dso_stub.o" \
		CFLAGS="-D__STDC_NO_VLA__=1 -D__GNUC__=0 -D__GNUC_MINOR__=0 -D_GCC_LIMITS_H_ -D__thread=" \
		./configure --prefix=/usr --disable-nls --disable-pidwait --disable-numa
	# A separate, real, but harmless gap: `make all`'s own real ps/top/
	# etc. binaries (top-level bin_PROGRAMS, not part of SUBDIRS at all --
	# confirmed directly against the real Makefile.am) build and link
	# fine; only testsuite/ (real unit-test programs, never part of a
	# real install) fails, on an unrelated issue not worth chasing for
	# test-only code this project never runs. SUBDIRS= override (a real,
	# standard automake convention) skips it while still building
	# everything the top-level Makefile.am's own real SUBDIRS list
	# (local po-man po testsuite) needs for a real install.
	# A NINTH gap, and a real one: TCC 0.9.27 has no `__thread` support at
	# all -- confirmed with a minimal standalone probe (`static __thread
	# node *p;` -> "error: ';' expected", while gcc accepts it), not
	# inferred from the build failure. procps uses it in 28 places for
	# per-thread caches (library/pwcache.c, escape.c, meminfo.c,
	# devname.c, ...), so `library/devname.c:62` was simply the first one
	# the compiler reached.
	#
	# Defining `__thread` away turns those caches into ordinary statics.
	# That is only safe in a program that never runs them from more than
	# one thread -- so whether it is a legitimate fix or a data race
	# depends entirely on which binaries we build. Checked directly
	# rather than assumed: `grep -l pthread_create` across the whole
	# source tree matches ONLY src/top/*. Every other tool here (ps,
	# free, kill, pgrep, pkill, pidof, pmap, pwdx, tload, uptime, vmstat,
	# w, watch, slabtop, hugetop, sysctl) is genuinely single-threaded,
	# and for those a plain static is semantically identical.
	#
	# So: build everything EXCEPT top, and define __thread away only for
	# that set. top genuinely spawns three threads (top.c:3773 onward),
	# and shipping it with shared-instead-of-per-thread caches would be a
	# real correctness bug dressed up as a build fix -- deliberately not
	# done. htop covers the interactive-process-viewer role instead, and
	# the omission is stated plainly rather than left for someone to
	# discover.
	#
	# bin_PROGRAMS/sbin_PROGRAMS are overridden on the make command line
	# -- the same standard automake convention this recipe already uses
	# for SUBDIRS just below, not a patch to upstream source.
	# An ELEVENTH gap: `tcc: error: unsupported linker option
	# '--version-script=./library/libproc2.sym'`. procps versions
	# libproc2's exported symbols with a GNU ld version script; TCC's
	# linker has no equivalent and rejects the flag outright.
	#
	# Dropped rather than worked around, because a version script is
	# purely about symbol-visibility/versioning metadata in the produced
	# .so -- nothing in this image resolves procps symbols by version, and
	# every consumer here is procps' own binaries linked against it in the
	# same build. -version-info is deliberately KEPT (it is what makes the
	# library install as libproc2.so.1, which those binaries record as
	# their DT_NEEDED), as is -no-undefined. Overridden on the make
	# command line, the same automake convention used for bin_PROGRAMS
	# above -- never a patch to upstream source.
	make -j"$(nproc)" SUBDIRS="local po-man po" \
		bin_PROGRAMS="$PROCPS_BIN_PROGRAMS" sbin_PROGRAMS="src/sysctl" \
		library_libproc2_la_LDFLAGS='-version-info $(LIBproc2_CURRENT):$(LIBproc2_REVISION):$(LIBproc2_AGE) -no-undefined' 
}

# Every procps tool except top -- see pkg_build()'s own ninth-gap comment
# for why top is deliberately excluded.
PROCPS_BIN_PROGRAMS="src/ps/pscommand src/free src/pgrep src/pkill src/pmap src/pwdx src/tload src/uptime src/vmstat src/pidof src/kill src/w src/watch src/slabtop src/hugetop"

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
	make install DESTDIR="$PKG_DESTDIR" SUBDIRS="local po-man po" \
		bin_PROGRAMS="$PROCPS_BIN_PROGRAMS" sbin_PROGRAMS="src/sysctl" \
		library_libproc2_la_LDFLAGS='-version-info $(LIBproc2_CURRENT):$(LIBproc2_REVISION):$(LIBproc2_AGE) -no-undefined' 
	rm -rf "$PKG_DESTDIR/usr/share" "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/usr/lib/pkgconfig" \
	       "$PKG_DESTDIR/usr/lib/libproc2.a" "$PKG_DESTDIR/usr/lib/libproc2.la" \
	       "$PKG_DESTDIR/usr/lib/libproc2.so"
}
