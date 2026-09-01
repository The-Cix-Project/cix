#
# htop -- interactive process viewer (task #729, the jump box recipe
# set).
#
# Source is htop-dev's own canonical GitHub Releases asset (a real
# dist tarball with configure already generated, not a bare git-tag
# archive) -- this project's own single-source-of-truth GitHub
# organization for the project, no second mirror exists to
# cross-verify against; the release asset itself is immutable once
# published.
#
pkg_name="htop"
pkg_version="3.5.2-7"
pkg_source="https://github.com/htop-dev/htop/releases/download/3.5.2/htop-3.5.2.tar.xz"
pkg_sha256="225128e697c4a8c8a878fd0078c965ff8bd5fb24913bfc8473b8edbd50f843f8"
pkg_artifact_sha256="17ee0bda7a2e1d7ca3c38159de5de44399071b3ba3e14be71d512fdbfbdfebfa"
pkg_depends="ncurses"
# ADR-0199/0209: the build environment is composed from exactly these
# and nothing else -- there is no fallback environment to inherit a
# missing tool from (#168). Bumped to -4 purely to carry this
# declaration: a recipe version is immutable once published, so adding
# it to -3 in place could never reach a host that already has -3.
#
# ncurses and libc-dev are build inputs as well as runtime ones here:
# configure probes for curses headers, and tcc needs glibc's headers
# and CRT objects to link anything at all.
pkg_build_depends="bash coreutils sed grep gawk make tcc linux-headers ncurses"
pkg_changelog="3.5.2-7: removes the NAN workaround, which had become the failure. Measured on the current compiler: plain <math.h> gives working NAN, isgreater and isgreaterequal, while -DNAN=(__builtin_nanf("")) fails because TCC has no __builtin_nanf. The upgrade fixed the gap and the workaround outlived it. 3.5.2-6: supplies __builtin_isgreater and __builtin_isgreaterequal, which TCC does not implement and glibc's <math.h> macros expand to, so configure reported isgreater()/isgreaterequal() as missing (#87, #208 family). Defines the builtins rather than the macros so glibc's own definitions are used. Adds a gate that runs the binary. 3.5.2-5: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

# capabilities/delayacct/sensors/hwloc are all real htop features this
# configure script silently auto-enables whenever it happens to find
# their headers/libs already present -- confirmed the hard way in
# this sandbox (a dev box with libcap-dev installed turned on
# "capabilities: yes" with zero flags passed). None of their backing
# libraries (libcap, libnl for delayacct, libsensors, hwloc) exist as
# a recipe in this project, so a real isolated build container
# wouldn't have found them anyway -- but disabling explicitly here
# documents that as a deliberate scope decision for this jump box's
# own process viewer, not an accident of whatever happens to be lying
# around in a given build container. --with-curses=ncursesw pins the
# real dependency by name rather than letting the default probe guess.
# Re-pinned to -2 to close a real, confirmed build failure whose cause
# was NOT what its message suggested:
#
#   checking for sys/param.h... no
#   configure: error: cannot find required generic header files
#
# The header is present. Read directly out of a preserved failing build
# container (keep_on_failure + the files API, ADR-0175), config.log shows
# what actually happened:
#
#   configure:6528: gcc -std=gnu11 -c -g -O2 conftest.c
#   /usr/include/limits.h:124:26: error: no include path in which to
#     search for limits.h
#
# Two separate problems in one line. First, configure was using **gcc**,
# not tcc, despite this recipe's own `CC=tcc` -- the ambient-gcc
# contamination of the shared build sandbox that issue #37 tracks;
# autoconf had already cached `ac_cv_prog_CC=gcc` before the environment
# CC could take effect. Second, that ambient gcc is itself broken here:
# glibc's limits.h does `#include_next <limits.h>` expecting the
# compiler's own copy, and this gcc has no internal include directory in
# the sandbox to provide one -- so ANY header check pulling in limits.h
# fails, and autoconf reports it as a missing header.
#
# Forcing autoconf's own cache variable (not just the CC environment
# variable) makes the probe use the compiler this project actually
# builds with. TCC resolves limits.h correctly here -- verified with a
# direct compile -- so the header check then measures reality.
pkg_build() {
	# A second gap, reached only once the compiler fix above got configure
	# to succeed: `GPUMeter.c:21: error: division by zero in constant` on
	# `static double totalUsage = NAN;`.
	#
	# glibc's math.h picks its NAN definition by compiler: with __GNUC__
	# it uses `(__builtin_nanf (""))`, otherwise it falls back to
	# `(0.0f / 0.0f)` (math.h:98 vs :103). TCC takes the fallback, and the
	# build sandbox's own TCC refuses to constant-fold that division in a
	# static initializer. (Notably this project's DEV-sandbox tcc 0.9.27
	# accepts the same expression -- verified directly -- so this is a
	# real behavioural difference between the two TCCs, not a universal
	# TCC property, and is worth knowing before assuming a NAN
	# initializer is portable across them.)
	#
	# Overriding NAN to the builtin form asks TCC for the same thing GCC
	# is given, without touching upstream source.
	#
	# No math.h workarounds. That is the fix, not an omission.
	#
	# Earlier revisions passed -DNAN=(__builtin_nanf("")) because the
	# old compiler could not produce a usable NAN, and configure
	# reports all three of its math checks together:
	#
	#   configure: error: cannot find required macros: NAN, isgreater()
	#                     and isgreaterequal()
	#
	# Measured against the current compiler rather than reasoned about
	# (probe-tcc-conformance/16), and the result inverts the problem:
	#
	#   NAN from <math.h>, unmodified          ok
	#   isgreater / isgreaterequal, unmodified ok
	#   all three together, NO FLAGS           PASSES
	#   all three with -DNAN=(__builtin_nanf("")) FAILS
	#
	# TCC has no __builtin_nanf, so the workaround that once supplied
	# NAN is now the only thing preventing it. The compiler upgrade
	# (ADR-0223) fixed the underlying gap and the workaround outlived
	# it -- exactly what happened to iproute2's -D_FILE_OFFSET_BITS=64
	# strip, and the reason a workaround should name the thing it works
	# around so it can be retired when that thing is gone.
	#
	# ac_cv_prog_CC=tcc stays, and is a different matter: autoconf
	# caches the compiler it probes, and #37's ambient-gcc
	# contamination is what that pin exists to prevent.
	#
	CC=tcc ac_cv_prog_CC=tcc \
		./configure --prefix=/usr --with-curses=ncursesw \
	            --disable-capabilities --disable-delayacct \
	            --disable-sensors --disable-hwloc
	make -j"$(nproc)"

	#
	# Run what was built. htop is interactive, so --version is the
	# honest check available: it proves the binary links, loads
	# ncurses, and starts.
	#
	out=$(./htop --version 2>&1) || {
		echo "htop: the binary just built does not run:" >&2
		echo "$out" >&2
		exit 1
	}
	case "$out" in
	*3.5.2*) echo "  built htop runs: $out" ;;
	*)
		echo "htop: --version did not report the expected version, got: $out" >&2
		exit 1
		;;
	esac
}

# Real files from this recipe's own DESTDIR install, confirmed via a
# local build: just the htop binary, its man page, and its own
# example config under /usr/share/doc -- confirmed link line is
# "-lncursesw -lm" (`ldd htop`, run with LD_LIBRARY_PATH pointed at
# this project's own from-scratch ncurses build rather than this
# sandbox's ambient system copy to get a true reading: only
# libncursesw.so.6/libm.so.6/libc.so.6, nothing else).
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -rf "$PKG_DESTDIR/usr/share/doc"
}
