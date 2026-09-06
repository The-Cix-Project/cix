#
# glibc -- the C library, built from source on a Cix host.
#
# Issue #181, and the deepest contamination this platform had. Until
# now glibc was never built here at all: pkg_seed_image_baseline()
# copied libc.so.6, ld-linux-x86-64.so.2 and libm.so.6 out of the
# running host's own filesystem by absolute path, and libc-dev.recipe
# copied glibc's headers and CRT objects from the build host, saying so
# in its own header comment while fetching and checksum-verifying an
# upstream tarball it then did not use. Every binary on the platform
# linked a C library that came from Debian, propagated forward through
# every image, with nothing marking it as foreign.
#
# The Build Provenance Mandate says everything Cix ships is built on a
# Cix host, by Cix, with Cix's own toolchain. This is that, for the one
# component everything else depends on.
#
# 2.44 rather than 2.36 (the version being copied today) for two
# reasons. It is contemporaneous with the gcc this platform builds
# with (16.2.0), and glibc is sensitive to compiler version -- a 2022
# glibc under a 2026 gcc is a fight worth not having. And moving
# forward is the safe direction: glibc keeps symbol versions, so
# binaries already linked against 2.36 run unchanged on 2.44. The
# reverse would not be true, which is why this can never be swapped for
# an older one afterwards.
#
# Kernel headers come from linux-headers, i.e. from the exact kernel
# source this platform builds and boots -- not from the build host.
# They define the syscall and structure ABI between userspace and the
# kernel, so taking another distribution's while building our own libc
# would have missed the point of doing this at all.
#
pkg_name="glibc"
pkg_version="2.44-14"
# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
#
# Built on 192.168.15.95 by Cix's own toolchain and published from
# there; this line approves those exact bytes and nothing else. Added
# after publication, which is the only order the Build Provenance
# Mandate permits -- a checksum is never carried forward from a
# previous version, and never computed over something built elsewhere.
#
# It is also what lets this version seed the test floor (ADR-0209): the
# floor verifies every artifact it uses against the checksum in its own
# recipe, so an artifact with no checksum here cannot be a floor member
# at all. The floor moves off libc-dev onto this plus linux-headers,
# which is the same pair every real recipe now declares (#187).
pkg_source="https://mirrors.kernel.org/gnu/libc/glibc-2.44.tar.xz"
pkg_sha256="37f600f2bef3c5e8300147059568b2a2e40a7ad6ccc65ce942556d49429cc667"
pkg_depends=""


# ADR-0199/0209: composed from exactly these, no fallback (#168).
# bison and python are glibc's own documented build requirements, not
# guesses: it generates parser code and runs Python scripts during the
# build. linux-headers is what --with-headers points at.
#
# gzip is an INSTALL-time requirement, not a build-time one, and that
# distinction cost a full build to find: glibc compiled cleanly all the
# way through and then died in `make install`, compressing locale
# charmaps --
#
#   gzip -9n .../usr/share/i18n/charmaps/ANSI_X3.110-1983
#   make[2]: gzip: No such file or directory
#
# A build environment holds exactly what a recipe declares (#168), so a
# tool used only by the install phase still has to be declared.
pkg_toolchain="gcc"
pkg_toolchain_reason="structural: this package IS libc: TCC cannot resolve crt1.o/crti.o/crtn.o while building the CRT that provides them"
pkg_changelog="2.44-13: declare pkg_toolchain=gcc and its reason (#222, ADR-0226)"
pkg_build_depends="bash coreutils make gcc binutils sed grep gawk findutils diffutils bison python perl m4 gzip linux-headers tcc"

pkg_build() {
	#
	# glibc refuses to configure in its own source tree, by design --
	# "you must configure in a separate build directory". So this is
	# not a style choice.
	#
	mkdir -p /build/obj
	cd /build/obj

	# CC/CXX by absolute path: a bare "gcc" makes gcc compute its
	# installation prefix relatively and fail cc1 with a misleading
	# posix_spawnp error (CLAUDE.md), and this build sandbox's own bare
	# "cc" can no longer be trusted to mean any particular compiler.
	#
	# --with-headers is the whole point: build against OUR kernel's
	# headers, at their real path, rather than whatever the build
	# container happens to have.
	#
	# --enable-kernel=5.4 sets the oldest kernel this libc will run on.
	# Cix boots 6.18, so anything older is dead weight, but leaving a
	# margin costs nothing and avoids a libc that refuses to start on a
	# recovery kernel.
	#
	# --disable-werror because a libc this size, under a compiler this
	# new, will find warnings that are not defects.
	# libc_cv_cxx_link_ok=no (2.44-10): glibc's own supported knob, same
	# cache-variable style as libc_cv_slibdir below. It skips the C++
	# TEST-SUPPORT programs (support/links-dso-program and friends) --
	# never shipped, never run here -- which are the one part of the
	# build that links against gcc's libstdc++. That link died resolving
	# libstdc++'s own DT_NEEDED libm.so.6: gcc's ld searches only /lib
	# and /usr/lib for a dependency's dependencies, and this platform
	# keeps libm at /lib/x86_64-linux-gnu -- reachable from /usr/lib only
	# via the symlinks THIS build stages (below). A glibc cannot depend
	# on the fix it has not shipped yet, so its own build sidesteps C++
	# support entirely instead. gcc's bootstrap needs no such favor: it
	# builds in an environment where this glibc is already installed.
	CC=/usr/bin/gcc CXX=/usr/bin/g++ ../src/configure \
		--prefix=/usr \
		--with-headers=/usr/include \
		--enable-kernel=5.4 \
		--disable-werror \
		--disable-profile \
		--without-selinux \
		--without-gd \
		--libdir=/usr/lib \
		libc_cv_slibdir=/lib/x86_64-linux-gnu \
		libc_cv_cxx_link_ok=no

	# MAKEINFO=: -- a colon, not "true", and the difference is the whole
	# point. glibc's manual/Makefile guards every manual rule with
	#
	#   ifneq ($(strip $(MAKEINFO)),:)
	#
	# so a literal colon skips building AND installing the manual
	# entirely, which is exactly what configure sets when it finds no
	# makeinfo. "true" (copied here from binutils.recipe, where it
	# works) instead makes the build step a successful no-op that
	# produces no file, and install then fails on the file that was
	# never created:
	#
	#   LANGUAGE=C LC_ALL=C true -P ... --output=.../libc.info libc.texinfo
	#   install: cannot stat '.../manual/libc.info*': No such file or directory
	#
	# texinfo is not packaged, and nothing here reads the manual --
	# pkg_install() deletes usr/share/info regardless.
	make -j"$(nproc)" MAKEINFO=:
}

pkg_install() {
	cd /build/obj
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=:

	rm -rf "$PKG_DESTDIR/usr/share/info" "$PKG_DESTDIR/usr/share/man" \
	       "$PKG_DESTDIR/usr/share/doc"

	#
	# The link-time files where this platform's compilers look for them.
	#
	# glibc puts CRT objects and the libc.so/libm.so linker scripts in
	# --libdir. That was the multiarch path until 2.44-7 moved it to
	# /usr/lib (#196, see the check below); the loop below therefore
	# copies them OUT of /usr/lib into both multiarch directories,
	# because that is where the compilers look and moving libdir must
	# not move them.
	# They ALSO need to exist under /lib/x86_64-linux-gnu, because that
	# is where TCC's own search finds them -- libc-dev staged both
	# copies for exactly this reason, and dropping one of them is how
	# 2.44-5 produced a complete, correct glibc that the daemon still
	# could not be built against:
	#
	#   tcc: error: file 'crt1.o' not found
	#   tcc: error: file 'crti.o' not found
	#
	# The headers were right, the libraries were right, and the four
	# object files every dynamically-linked binary needs at link time
	# were in a directory no compiler here looks in.
	#
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu" \
	         "$PKG_DESTDIR/usr/lib/x86_64-linux-gnu"
	for f in "$PKG_DESTDIR/usr/lib"/*crt*.o; do
		test -e "$f" || continue
		cp -a "$f" "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
		cp -a "$f" "$PKG_DESTDIR/usr/lib/x86_64-linux-gnu/"
	done
	#
	# libc.a, libpthread.a, librt.a and libdl.a were on this list and
	# are not any more. ADR-0251: this platform links dynamically
	# always, so an archive whose shared counterpart ships beside it is
	# dead weight, and the daemon's finalize phase drops it from the
	# artifact regardless. Copying libc.a here put 22.43 MiB into each
	# of two more directories -- 44.9 MiB of an artifact that is 146.4
	# MiB unpacked -- for the finalize phase to delete again.
	#
	# libc_nonshared.a stays: it has no shared counterpart, so it is
	# kept, and gcc's dynamic links genuinely need to find it here.
	# libc.so and libm.so stay because they are ASCII linker scripts,
	# not archives at all.
	#
	for f in libc.so libm.so libc_nonshared.a; do
		test -e "$PKG_DESTDIR/usr/lib/$f" || continue
		cp -a "$PKG_DESTDIR/usr/lib/$f" "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
		cp -a "$PKG_DESTDIR/usr/lib/$f" "$PKG_DESTDIR/usr/lib/x86_64-linux-gnu/"
	done

	#
	# 2.44-9 (#187): the CRT objects are stripped of debug info, and
	# TCC is made to prove it can actually link against them.
	#
	# TCC hardcodes /usr/lib/x86_64-linux-gnu as its CRT directory (-B
	# does not move it) and cannot parse a .rela.debug_info section:
	#
	#   /usr/lib/x86_64-linux-gnu/crt1.o: error: Invalid relocation
	#       entry [15] '.rela.debug_info'
	#   tcc: error: file 'crt1.o' not found
	#
	# So TCC could never link against OUR crt1.o. Nothing noticed
	# because libc-dev shipped Debian's stripped crt1.o to the same
	# path and sorted after "glibc", so it overwrote ours in every
	# build environment -- the platform's own compiler has been
	# linking against another distribution's startup objects all
	# along. Making glibc the last copy (#187) is what finally exposed
	# it.
	#
	# Stripping debug info from CRT objects is what every distribution
	# ships; they are tiny startup stubs and nothing debugs them. The
	# gate below is the part that matters: it compiles and links a real
	# program with the real TCC against these exact staged objects, so
	# a glibc that this platform's own compiler cannot use can never be
	# published again.
	#
	for f in "$PKG_DESTDIR/usr/lib"/*crt*.o \
	         "$PKG_DESTDIR/lib/x86_64-linux-gnu"/*crt*.o \
	         "$PKG_DESTDIR/usr/lib/x86_64-linux-gnu"/*crt*.o; do
		test -e "$f" || continue
		strip --strip-debug "$f" || exit 1
	done

	# 2.44-9 and 2.44-10 wrote this gate as `tcc -B<destdir>/usr/lib/...`
	# and it could never pass, for a reason worth recording: TCC's -B
	# sets where TCC looks for its OWN private files (libtcc1.a). It
	# does not move the CRT search. TCC resolves crt1.o/crti.o/crtn.o
	# through its compiled-in CRT prefix, which on this platform is the
	# hardcoded /usr/lib/x86_64-linux-gnu (CLAUDE.md records the same
	# hardcoding from a separate investigation). So the gate silently
	# tested the ALREADY-INSTALLED glibc's startup objects -- exactly
	# the ones this build exists to replace -- while -B sent TCC hunting
	# for libtcc1.a in a directory that has never contained it:
	#
	#   /usr/lib/x86_64-linux-gnu/crt1.o: error: Invalid relocation
	#       entry [15] '.rela.debug_info' @ 000001a6
	#   tcc: error: file 'crt1.o' not found
	#   tcc: error: file '<destdir>/usr/lib/x86_64-linux-gnu/libtcc1.a'
	#       not found
	#
	# A gate that reports on the wrong artifact is worse than none: it
	# fails a good build and would pass a bad one. The fix is to stop
	# asking TCC to find these objects and hand it their real paths:
	# -nostdlib turns off the built-in CRT and library defaults entirely,
	# and every input is then named explicitly, so what is linked is
	# exactly what this build staged and nothing else.
	#
	crtdir=""
	for d in "$PKG_DESTDIR/usr/lib/x86_64-linux-gnu" \
	         "$PKG_DESTDIR/usr/lib" \
	         "$PKG_DESTDIR/lib/x86_64-linux-gnu"; do
		if test -e "$d/crt1.o"; then crtdir="$d"; break; fi
	done
	if test -z "$crtdir"; then
		echo "glibc: no crt1.o staged anywhere -- nothing to gate" >&2
		exit 1
	fi

	libdir="$PKG_DESTDIR/lib/x86_64-linux-gnu"
	loader="$libdir/ld-linux-x86-64.so.2"

	cat > /build/crtgate.c <<'CRTGATE'
int main(void) { return 0; }
CRTGATE
	if ! tcc -nostdlib \
	         "$crtdir/crt1.o" "$crtdir/crti.o" \
	         /build/crtgate.c \
	         -L"$libdir" -lc \
	         "$crtdir/crtn.o" \
	         -o /build/crtgate 2>/build/crtgate.err; then
		echo "TCC cannot link against the CRT objects this glibc stages" \
		     "($crtdir):" >&2
		cat /build/crtgate.err >&2
		exit 1
	fi

	# Linking is not the bar. A control-plane root carrying glibc
	# objects from two different builds LINKS fine and then panics at
	# boot with exit 127 (CLAUDE.md, twice on real hardware). So run the
	# binary through this build's own loader against this build's own
	# libraries -- if that works, the set is internally consistent.
	if ! "$loader" --library-path "$libdir" /build/crtgate; then
		echo "glibc: a TCC-linked binary does not RUN against the" \
		     "loader and libraries this build stages -- refusing to" \
		     "publish a glibc whose own halves do not agree" >&2
		exit 1
	fi
	rm -f /build/crtgate.c /build/crtgate /build/crtgate.err

	#
	# 2.44-10 (#187/#184): every runtime SONAME is reachable from
	# /usr/lib, as a SYMLINK -- never a copy.
	#
	# GNU ld resolves a dependency's own dependencies (a DT_NEEDED of a
	# DT_NEEDED) through -rpath-link and then its defaults, /lib and
	# /usr/lib -- NOT /lib/x86_64-linux-gnu. Debian bridges that with
	# /etc/ld.so.conf and a multiarch-patched ld; this platform
	# inherited Debian's LAYOUT without that machinery (#184), so any
	# link touching gcc's libstdc++ died on "libm.so.6 ... not found".
	# That is what stopped glibc's own C++ test helpers, and it is what
	# would stop gcc's bootstrap, since gcc is C++.
	#
	# Symlinks, deliberately: one real file, one inode. Two independent
	# copies of glibc's own objects is exactly the mismatched-halves
	# failure that has already panicked a real machine twice (CLAUDE.md)
	# -- a symlink makes that hazard structurally impossible rather than
	# carefully avoided. This is also a deliberate bridge toward #184's
	# one-library-directory end state, not another layer of layout.
	#
	staged_links=""
	for f in "$PKG_DESTDIR/lib/x86_64-linux-gnu"/*.so*; do
		test -e "$f" || continue
		b=$(basename "$f")
		# The linker scripts and archives already live in /usr/lib
		# (libdir); only the SONAME runtime objects need reaching.
		test -e "$PKG_DESTDIR/usr/lib/$b" && continue
		ln -s "../../lib/x86_64-linux-gnu/$b" "$PKG_DESTDIR/usr/lib/$b" || exit 1
		staged_links="$staged_links $b"
	done

	#
	# Gate: each link we just made must name the right relative target,
	# and that target must exist in this build's own slibdir.
	#
	# 2.44-11's gate could never pass. It swept EVERY symlink in
	# /usr/lib and cmp'd each against a same-named file in slibdir:
	#
	#   for l in "$PKG_DESTDIR/usr/lib"/*.so*; do
	#       test -L "$l" || continue
	#       cmp -s "$l" "$PKG_DESTDIR/lib/x86_64-linux-gnu/$(basename $l)"
	#
	# Since 2.44-7 moved libdir to /usr/lib, glibc installs its own
	# development symlinks there -- libBrokenLocale.so, libm.so and the
	# rest -- each pointing at the VERSIONED file in slibdir
	# (../../lib/x86_64-linux-gnu/libBrokenLocale.so.1). Their
	# basenames end in .so, and slibdir holds .so.1, so the right-hand
	# path did not exist, cmp failed, and the build stopped on
	# libBrokenLocale.so: glibc's own correct work, reported as a
	# broken link this recipe never created.
	#
	# One thing that is NOT wrong with the old gate, recorded so it is
	# not "fixed" later: a relative symlink resolves against the
	# directory holding the LINK, so $PKG_DESTDIR/usr/lib/x ->
	# ../../lib/... does stay inside the staging tree and never reads
	# the build host's installed glibc. Following it was safe; sweeping
	# links this recipe did not create was not.
	#
	# So the loop is restricted to what the loop above actually made,
	# and it checks what each link SAYS plus whether its target is
	# really there -- which is the whole of what can be wrong with a
	# link we wrote ourselves.
	#
	for b in $staged_links; do
		l="$PKG_DESTDIR/usr/lib/$b"
		want="../../lib/x86_64-linux-gnu/$b"
		got=$(readlink "$l")
		if test "$got" != "$want"; then
			echo "glibc: /usr/lib/$b points at '$got', expected '$want'" >&2
			exit 1
		fi
		if ! test -e "$PKG_DESTDIR/lib/x86_64-linux-gnu/$b"; then
			echo "glibc: /usr/lib/$b would dangle -- no $b in this" \
			     "build's own slibdir" >&2
			exit 1
		fi
	done

	#
	# 2.44-8 (#187): our headers must also OWN the multiarch include
	# directory, not merely exist beside it.
	#
	# GCC searches /usr/include/x86_64-linux-gnu BEFORE /usr/include.
	# libc-dev stages 381 headers there from the build host -- glibc
	# 2.36's -- and glibc installs none, so ours were never the ones
	# found: 2.36's sys/cdefs.h set the include guard, __COLD went
	# undefined, and 2.44's stdio.h fell apart on it. Copying the C
	# library last into a build environment cannot fix that, because a
	# later copy can only win a path it actually owns.
	#
	# So install the same headers at both paths. On a multiarch layout
	# the C library's headers legitimately belong in both, and owning
	# both is what makes "our libc's headers win" true rather than
	# hoped for -- the same reasoning that puts the CRT objects in both
	# library directories above.
	#
	mkdir -p "$PKG_DESTDIR/usr/include/x86_64-linux-gnu"
	cp -a "$PKG_DESTDIR/usr/include/." "$PKG_DESTDIR/usr/include/x86_64-linux-gnu/" 2>/dev/null || true
	rm -rf "$PKG_DESTDIR/usr/include/x86_64-linux-gnu/x86_64-linux-gnu"
	if ! test -f "$PKG_DESTDIR/usr/include/x86_64-linux-gnu/sys/cdefs.h"; then
		echo "multiarch include staging produced no sys/cdefs.h" >&2
		exit 1
	fi
	if ! grep -q "__COLD" "$PKG_DESTDIR/usr/include/x86_64-linux-gnu/sys/cdefs.h"; then
		echo "the staged multiarch sys/cdefs.h is not this glibc's own" >&2
		exit 1
	fi

	#
	# 2.44-7 (#196): the loader must search /usr/lib, and this asserts
	# that it does rather than trusting the configure flag above.
	#
	# glibc's compiled-in search path is exactly slibdir + libdir; there
	# is no option to add a third. Until now libdir was the multiarch
	# directory, so the path was /lib/x86_64-linux-gnu and
	# /usr/lib/x86_64-linux-gnu -- and NOT /usr/lib, where a number of
	# packages install their libraries. flex is the confirmed case:
	# libfl.so.2 sat readable at /usr/lib in a composed build
	# environment while ar, which links it, could not start at all.
	#
	# Debian's glibc searched /usr/lib, so nothing noticed until this
	# platform started shipping its own libc.
	#
	# The link-time files compilers need stay in both multiarch
	# directories (copied above), because that is where gcc and TCC
	# look for CRT objects -- moving libdir must not move those.
	#
	if ! "$PKG_DESTDIR/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" --help 2>&1 \
	     | grep -q '^  /usr/lib (system search path)$'; then
		echo "the built loader does not report /usr/lib as a system search path" >&2
		"$PKG_DESTDIR/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" --help 2>&1 \
		    | sed -n '/search path/,/^$/p' >&2
		exit 1
	fi

	#
	# The dynamic loader at its ABI-FIXED path, /lib64/ld-linux-x86-64.so.2.
	#
	# This is not a convenience copy. Every x86-64 ELF binary on this
	# platform records that exact string as its PT_INTERP, so it is the
	# loader the kernel actually starts -- glibc's own install puts one
	# only in libc_cv_slibdir. 2.44-3 therefore replaced libc.so.6
	# while leaving the OLD 2.36 loader in place at /lib64, and the two
	# halves of one library disagreed:
	#
	#   bash: symbol lookup error: /lib/x86_64-linux-gnu/libc.so.6:
	#   undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE
	#
	# ld.so and libc.so.6 share a private, version-locked interface, so
	# a mismatched pair breaks every process on the image at once --
	# including the shell needed to look into it. They must always be
	# installed and replaced together.
	#
	# A real copy rather than a symlink, matching what
	# pkg_seed_image_baseline() already does with the same file: it
	# stages ld-linux twice, at /lib64 and at
	# /lib/x86_64-linux-gnu, because TCC's general library search
	# covers the latter and not the former (ADR-0057).
	#
	mkdir -p "$PKG_DESTDIR/lib64"
	cp -a "$PKG_DESTDIR/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" \
	      "$PKG_DESTDIR/lib64/ld-linux-x86-64.so.2"

	#
	# Asserted against the real output, because "it built" is not the
	# same as "it produced the three files this platform actually
	# stages into every image". A glibc that configured with the wrong
	# libdir would install a complete, working tree in the wrong place
	# and only fail much later, when an image came up with no C library
	# where the loader looks.
	#
	for f in lib/x86_64-linux-gnu/libc.so.6 lib/x86_64-linux-gnu/libm.so.6; do
		test -f "$PKG_DESTDIR/$f" || {
			echo "glibc: $f missing from install output" >&2
			exit 1
		}
	done
	# Both loader paths, checked by name rather than by find: a package
	# that ships libc.so.6 without the matching loader at /lib64 does
	# not merely lack a file, it makes the image it lands in unable to
	# start any process at all.
	for f in lib64/ld-linux-x86-64.so.2 lib/x86_64-linux-gnu/ld-linux-x86-64.so.2; do
		test -f "$PKG_DESTDIR/$f" || {
			echo "glibc: $f missing -- libc and its loader must ship together" >&2
			exit 1
		}
	done

	#
	# The header set, asserted by name. glibc is the package that
	# supplies <stdio.h> and <sys/cdefs.h> to every compile on the
	# platform, and shipping one without the other is not hypothetical:
	# with libc-dev still installed, this project had 2.44's stdio.h
	# resolving against Debian 2.36's cdefs.h, and gcc failed on a
	# macro the older header had never heard of --
	#
	#   /usr/include/stdio.h:872: error: expected declaration
	#   specifiers before '__COLD'
	#
	# -- which reads like a compiler bug and is really two C libraries
	# in one include path. libc-dev is retired by this package: every
	# file it supplied (headers, CRT objects, the libc.so/libm.so
	# linker scripts, and the libpthread/librt/libdl compat archives)
	# comes from here now, built rather than copied off a build host.
	#
	# The link-time set, at the paths a compiler here actually searches.
	for o in lib/x86_64-linux-gnu/crt1.o lib/x86_64-linux-gnu/crti.o \
	         lib/x86_64-linux-gnu/crtn.o lib/x86_64-linux-gnu/libc.so; do
		test -f "$PKG_DESTDIR/$o" || {
			echo "glibc: $o missing -- nothing on this platform could be linked" >&2
			exit 1
		}
	done

	for h in stdio.h stdlib.h string.h unistd.h sys/cdefs.h sys/types.h; do
		test -f "$PKG_DESTDIR/usr/include/$h" || {
			echo "glibc: usr/include/$h missing -- an incomplete header set is worse" >&2
			echo "than none, because it mixes with whatever else is on the include path" >&2
			exit 1
		}
	done

	echo "glibc runtime:"
	ls -la "$PKG_DESTDIR/lib/x86_64-linux-gnu/libc.so.6" \
	       "$PKG_DESTDIR/lib/x86_64-linux-gnu/libm.so.6"
	find "$PKG_DESTDIR" -name 'ld-linux-x86-64.so.2' -exec ls -la {} \;
}
