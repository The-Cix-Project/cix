#
# libcap -- POSIX.1e capability manipulation: libcap.so.2, the header
# every consumer compiles against, and the setcap/getcap/getpcaps/capsh
# tools. Also libpsx, libcap's own helper for applying a syscall across
# every thread of a process.
#
# This package exists because of issue #110. `coreutils` links against
# libcap (that is how `ls` shows file capabilities) and had been
# staging the BUILD HOST's Debian copy -- `cp -a
# /lib/x86_64-linux-gnu/libcap.so.2 ...` -- a library no recipe built,
# no package owned, and nothing declared. That works only for as long
# as a build container happens to be seeded from a Debian filesystem,
# which under ADR-0199 it no longer is. The alternative was building
# coreutils --disable-libcap and losing a real feature. A distribution
# builds its own libraries.
#
# Source is kernel.org's own canonical libcap2 release directory.
#
pkg_name="libcap"
pkg_version="2.78-7"
pkg_source="https://mirrors.edge.kernel.org/pub/linux/libs/security/linux-privs/libcap2/libcap-2.78.tar.xz"
pkg_sha256="0d621e562fd932ccf67b9660fb018e468a683d7b827541df27813228c996bb11"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/libcap-2.78-4.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
# Nothing at runtime: confirmed with readelf against the built
# libcap.so.2.78 -- libc.so.6 is its only NEEDED entry.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD.
# Each entry earns its place:
#   tcc       the compiler, pinned via CC/BUILD_CC below
#   libc-dev  headers and libc to compile and link against
#   make      libcap has no configure at all -- it is a plain Makefile
#   bash      pkg_build() runs under it, and the Makefiles shell out
#   coreutils ln/rm/cat/mkdir/install/nproc throughout the build and
#             its install step
#   binutils  three of its tools: objcopy, which libcap uses to read the
#             ELF interpreter path out of a test binary (`objcopy
#             --dump-section .interp`) for cap_magic.o, plus ar and
#             ranlib for libcap.a/libpsx.a
#   diffutils the build verifies its own generated capshdoc.c against a
#             regenerated copy with `diff -u` and fails if they differ
#   sed       generates libcap.pc, and rewrites the kernel capability
#             header into cap_names.list.h
#   grep      extracts the CAP_* defines that sed then rewrites
#
# Getting this list right took being wrong twice, in both directions,
# which is worth recording because the mistakes are instructive.
#
# 2.78 declared sed, grep and gawk by assuming a build of this shape
# must use them. 2.78-2 then removed all three after "measuring" a
# local build -- but that measurement was taken over a tree that had
# already been built once, so the generated files existed and their
# rules never re-ran. The build environment corrected it immediately:
#
#     /bin/sh: line 1: sed: command not found
#     /bin/sh: line 1: grep: command not found
#
# gawk stays out, and that part was right: libcap has no configure, and
# a build with no awk anywhere completes.
#
# The lesson is the design's own: an environment holding exactly what
# is declared is the only reliable oracle for what a build needs.
# Reading logs and Makefiles is how you form the guess; the build is
# what settles it.
#
# Sufficiency is enforced by the build itself. Minimality is review,
# not enforcement (ADR-0199).
pkg_build_depends="tcc make linux-headers bash coreutils sed grep binutils diffutils"
pkg_changelog="2.78-7: rewrites #!/bin/bash shebangs to /usr/bin/bash -- the bash package ships bin/sh and usr/bin/bash but no /bin/bash, so progs/mkcapshdoc.sh died with exit 127 and blocked every dependent build. 2.78-6: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

# Three overrides, each for a real reason:
#
#   lib=lib     Make.Rules computes the library subdirectory by running
#               `ldd /usr/bin/ld | grep ld-linux | cut -d/ -f2`, which
#               needs ldd (a glibc shell script this project does not
#               package) and only exists to tell lib from lib64 on
#               multilib distributions. This project installs to
#               /usr/lib, so the answer is known and asking is a
#               dependency for nothing.
#
#   LD=...      Make.Rules defines LD as `$(CC) -Wl,-x -shared`. TCC
#               rejects `-x` outright: `unsupported linker option`.
#               -Wl,-x discards local symbols from the output -- a size
#               and tidiness measure with no effect on the library's
#               API, ABI, or behaviour -- so LD is set to the same
#               command without it.
#
#   MAGIC=      libcap links libcap.so with `-Wl,-e,__so_start`, which
#               makes the shared object directly executable so that
#               running it prints its own version. TCC rejects `-e` the
#               same way. Nothing links against or depends on that; it
#               is a diagnostic convenience. The library, its SONAME,
#               and all 75 exported functions are unaffected.
#
# Both rejected flags are genuine gaps in TCC 0.9.27's linker option
# handling rather than anything wrong with libcap, and are tracked as
# such rather than quietly worked around forever.
#
# GOLANG=no because libcap's optional Go bindings are not something this
# project has any consumer for, and detecting them shells out to `go`.
#
# PAM_CAP=no for two reasons, and the second is the interesting one.
# Nothing here consumes pam_cap (it grants capabilities at PAM login;
# Debian ships it as a separate libpam-cap package for the same
# reason), and pam_cap/Makefile hardcodes -Wl,-e,__so_start rather than
# taking it from $(MAGIC), so it cannot be built with TCC at all.
#
# But it was being built, and that is the part worth recording.
# Make.Rules decides with:
#
#   PAM_CAP ?= $(shell if [ -f /usr/include/security/pam_modules.h ]; \
#                      then echo $(SHARED); else echo no; fi)
#
# and that header WAS present in a build environment where no PAM
# package was declared or installed -- because `libc-dev` stages the
# build host's entire /usr/include, not libc's own headers. So a probe
# for a library this environment does not have answered yes, and libcap
# went off to build a PAM module against headers with no library behind
# them. Exactly the class of silent wrongness #113 is about, arriving
# from a direction that issue did not anticipate: not a probe the
# compiler fails, a probe that succeeds for the wrong reason.
#
# Passing it explicitly makes the answer a property of this recipe
# rather than of whatever happened to be in the build host's
# /usr/include the day libc-dev was packaged.
pkg_build() {
	#
	# libcap's build RUNS some of its own shipped scripts (progs/
	# mkcapshdoc.sh generates capshdoc.c). They start #!/bin/bash, and
	# this build environment has no /bin/bash: the bash package ships
	# bin/sh and usr/bin/bash, nothing else. Verified against the
	# package's own file list, not assumed.
	#
	# The failure is a bad one to read. The kernel returns ENOENT for
	# the missing INTERPRETER, and the shell reports it against the
	# SCRIPT:
	#
	#     /bin/sh: line 1: ./mkcapshdoc.sh: cannot execute: required file not found
	#     make[1]: *** [Makefile:56: capshdoc.c.cf] Error 127
	#
	# which reads as though mkcapshdoc.sh is missing. It is present and
	# executable; /bin/bash is what is missing. Note /bin/sh does exist,
	# so make's own recipe lines are fine -- only the bash shebang is
	# not. CLAUDE.md records the same trap for this project's runtime
	# container images; it applies to the build sandbox too.
	#
	find . -name '*.sh' -exec sed -i '1s|^#!/bin/bash|#!/usr/bin/bash|' {} +
	if grep -rl '^#!/bin/bash' --include='*.sh' . | grep -q .; then
		echo "a #!/bin/bash shebang survived the rewrite" >&2
		exit 1
	fi

	make CC=tcc BUILD_CC=tcc LD="tcc -shared" MAGIC= GOLANG=no PAM_CAP=no \
	     lib=lib prefix=/usr -j"$(nproc)"

	# libcap's own build produces no shared library at all if SHARED=no
	# is ever inferred, and a static-only libcap would satisfy nothing
	# that links against it. Assert the artifact rather than trust the
	# exit status (the lesson zlib 1.3.2-6 records).
	if [ ! -f libcap/libcap.so.2 ]; then
		echo "libcap: no shared library was built" >&2
		exit 1
	fi
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" CC=tcc BUILD_CC=tcc LD="tcc -shared" \
	     MAGIC= GOLANG=no PAM_CAP=no lib=lib prefix=/usr
	# Static archives and manual pages are dropped the same way every
	# other recipe in this catalog drops them -- nothing here links
	# statically, and no image ships man pages.
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/lib"/*.a
}
