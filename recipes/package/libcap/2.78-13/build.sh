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
pkg_version="2.78-13"
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
pkg_build_depends="tcc make linux-headers bash coreutils sed grep binutils diffutils findutils"
pkg_changelog="2.78-13: the 2.78-12 rewrite hardcoded the exact attribute spelling and got the destructor's spacing wrong, so it matched nothing; the recipe's own assertion caught that and failed the build. Now whitespace-tolerant, priority-agnostic and applied across the tree. 2.78-12: drops the priority argument from libcap's constructor and destructor attributes. The upgraded tcc (ADR-0223) does not parse a prioritised attribute at all, though a bare one works correctly -- measured, not assumed. A declared capability loss under ADR-0222: the ordering it asks for was never enforceable in a TCC-built system, since tcc cannot express a priority on any constructor. 2.78-11: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 2.78-10: the guard now checks LINE 1 only, which is what the sed rewrites. -8 flagged progs/quicktest.sh because that file contains #!/bin/bash inside two heredocs that generate a test script at run time -- not a shebang, and nothing the rewrite should touch. The -9 explanation blaming grep --include was wrong and is retracted."

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
	# Drop the PRIORITY argument from libcap's two constructor
	# attributes -- a declared capability loss under ADR-0222.
	#
	# libcap writes:
	#
	#     __attribute__((constructor (300))) void _libcap_initialize(void)
	#     __attribute__((destructor  (300))) static void _cleanup_libcap(void)
	#
	# tcc 0.9.27 accepted that. The upgraded compiler (0.9.28rc,
	# ADR-0223) does not parse the priority at all:
	#
	#     cap_alloc.c:23: error: ')' expected (got '(')
	#
	# Measured rather than assumed (probe-tcc-conformance/13): a BARE
	# __attribute__((constructor)) works correctly on the new compiler
	# -- the constructor runs before main and the destructor after --
	# while every prioritised form fails to compile. gcc accepts both
	# and honours the ordering.
	#
	# What the priority actually buys, and why dropping it is honest
	# here rather than a shrug: a priority orders this constructor
	# against OTHER constructors. libcap has exactly one constructor
	# and one destructor, so there is nothing to order within the
	# library. Against a consumer's constructors, 300 asks libcap to
	# initialise early -- but tcc cannot express a priority on any
	# constructor at all, so no TCC-built consumer has one to be
	# ordered against. The guarantee being given up was never
	# enforceable in a TCC-built system.
	#
	# It is not nothing: a GCC-built consumer that called into libcap
	# from its own prioritised constructor could now run first. No
	# package in this set does that, and the alternative -- patching
	# the compiler's attribute parser -- is a maintenance burden this
	# project deliberately stepped away from when it stopped patching
	# 0.9.27 (ADR-0223). Tracked upstream-side as its own issue.
	#
	# Whitespace-tolerant and priority-value-agnostic, applied across
	# the tree rather than to two named files. 2.78-12 hardcoded the
	# exact spelling and got the destructor's spacing wrong -- one
	# space in the source, two in the pattern -- so that sed matched
	# nothing. The assertion below caught it and failed the build
	# rather than letting a half-rewritten source reach the compiler,
	# which is what that assertion is for.
	find libcap -name '*.c' -o -name '*.h' | xargs sed -i -E \
	    's/__attribute__\s*\(\(\s*(constructor|destructor)\s*\([0-9]+\)\s*\)\)/__attribute__((\1))/g' 
	#
	# Assert BOTH directions, so neither half can be wrong silently:
	# no prioritised form may remain anywhere, and the two plain
	# attributes must be present. A sed whose pattern stopped matching
	# would otherwise leave the build to fail later with the same
	# opaque parse error.
	#
	if grep -rn 'constructor ([0-9]\|destructor  *([0-9]' libcap/ 2>/dev/null; then
		echo "libcap: a prioritised constructor/destructor attribute survived the rewrite" >&2
		exit 1
	fi
	grep -q '__attribute__((constructor))' libcap/cap_alloc.c || {
		echo "libcap: cap_alloc.c lost its constructor attribute entirely" >&2
		exit 1
	}
	grep -q '__attribute__((destructor))' libcap/cap_text.c || {
		echo "libcap: cap_text.c lost its destructor attribute entirely" >&2
		exit 1
	}

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
	# Check LINE 1 ONLY -- that is what the sed above rewrites, and a
	# whole-file search is wrong here for a concrete reason: libcap's
	# progs/quicktest.sh contains #!/bin/bash inside two heredocs that
	# write a test script at run time (lines 172 and 216). Those are not
	# shebangs of this file, nothing should rewrite them, and a guard
	# that greps the whole file reports them forever.
	#
	# Done with head + case rather than another grep invocation, so the
	# check cannot fail for its own reasons -- which two earlier
	# revisions of this guard both managed to do.
	bad=""
	for f in $(find . -name '*.sh'); do
		case "$(head -1 "$f")" in
		"#!/bin/bash"*) bad="$bad $f" ;;
		esac
	done
	if [ -n "$bad" ]; then
		echo "a #!/bin/bash shebang survived the rewrite:$bad" >&2
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
