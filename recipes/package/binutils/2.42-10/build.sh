#
# binutils -- the GNU binary utilities: as (assembler), ld/ld.bfd
# (linker), ar/ranlib (static archives), objcopy/objdump/readelf/nm/
# size/strings/strip/addr2line/c++filt/elfedit/gprof (object-file
# inspection and manipulation). The second dev-toolchain recipe (after
# m4) -- gcc.recipe needs a real as/ld present in the target image to
# actually produce working binaries; without this, a staged gcc could
# compile but not link anything.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against two independent mirrors (ftp.gnu.org and mirrors.kernel.org)
# -- byte-identical, same sha256.
#
pkg_name="binutils"
pkg_version="2.42-10"
pkg_source="https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.xz"
pkg_sha256="f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/binutils-2.42-8.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
# What these tools need to RUN, found by running them in an environment
# that held only what was declared -- never from reading configure
# output:
#   zlib  ar/ld/objdump link against libz for compressed sections;
#         `ar` died with "cannot open shared object file: libz.so.1"
#   flex  ar links against libfl (its own script-mode lexer); with libz
#         supplied it got one step further and died on libfl.so.2
# -2 declared neither, which was true only because the shared build
# sandbox happened to have both lying around (#110).
pkg_depends="zlib flex"
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD.
# Each entry earns its place:
#   tcc       the compiler
#   libc-dev  headers and libc to compile and link against
#   make      the out-of-tree build below
#   bash      pkg_build() runs under it, and configure is a shell script
#   coreutils nproc/mkdir/rm/cat, used throughout configure and install
#   sed       an autoconf configure rewrites its own output constantly
#   grep      every feature test that greps a compiler or header
#   gawk      AC_PROG_AWK, and config.status's own substitution fallback
#   binutils  binutils builds real static archives (libbfd.a, libopcodes.a,
#             libiberty.a) on its way to the tools it installs, so it
#             needs an `ar` before it can produce one
#   zlib      twice over: libbfd links against libz for compressed debug
#             sections, and the `ar` above needs libz.so.1 to start at
#             all
#   flex      the `ar` above also needs libfl.so.2 to start
#
# Those last two are normally the platform's problem, not a recipe's --
# a declared tool arrives with its own runtime dependencies. But that
# closure is read from what was recorded when the tool was INSTALLED,
# and every binutils installed anywhere predates this recipe declaring
# either. So they are here to break the bootstrap cycle exactly once;
# once a binutils built from this recipe is installed, consumers get
# both automatically and only libbfd's own link keeps zlib here.
#
# Sufficiency is enforced by the build itself. Minimality is review,
# not enforcement (ADR-0199).
# -8: built with the real gcc 9.5 instead of TCC (see the pkg_build()
# comment for why); tcc leaves the declared set, gcc enters it.
pkg_build_depends="gcc make linux-headers bash coreutils sed grep gawk binutils zlib flex"
pkg_changelog="2.42-10: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

# Out-of-tree build (binutils' own documented convention, avoids a real,
# known issue building certain subdirs in-tree). --disable-gold (the
# alternative ELF-only linker; ld.bfd already covers every real use
# case this project has, and gold needs a C++ toolchain this recipe set
# doesn't assume yet) and --disable-gprofng (a newer profiler needing
# extra deps this project has no use for) keep the build to the tools
# actually needed. MAKEINFO=true skips texinfo doc generation --
# `makeinfo` isn't part of this toolchain and none of this project's
# own images ship info pages for anything else either (same reasoning
# iputils.recipe's own -DBUILD_MANS=false already used).
#
# Re-pinned to -2: `bfd/bfd.c` (`static TLS bfd_error_type bfd_error;`
# et al) failed under TCC with "';' expected (got \"bfd_error_type\")".
# Two blind guesses (CPPFLAGS="-DTLS=", a single post-configure sed
# targeting a `#define TLS ...` line) both had zero effect -- a
# diagnostic-only build (dumping every generated config.h's own TLS-
# related lines) found the real, exact shape: `bfd/config.h` carries
#
#   /* If the compiler supports a TLS storage class, define it to that here */
#   /* #undef TLS */
#
# -- autoconf's own real, standard "detected as NOT available, left
# commented out" convention. `TLS` is genuinely undefined, not defined
# to `__thread` or anything else -- so the preprocessor leaves the bare
# identifier `TLS` untouched in `static TLS bfd_error_type ...`, which
# TCC's *parser* (not the preprocessor) then correctly rejects as
# invalid syntax: a plain undeclared identifier can't appear between
# `static` and a type name. Real GCC/Clang builds never hit this
# because their own TLS-storage-class autoconf probe succeeds and
# defines `TLS` to `__thread`; something about how the probe exercises
# TCC makes it conclude "no support" without also emitting the
# empty-string fallback binutils' upstream probably expects a genuinely
# TLS-incapable compiler to still receive. Confirmed via ldd against
# this build's own real output was never wrong about what a
# successful, already-built binutils looks like -- what's new is that
# this exact TCC/autoconf-probe interaction was apparently never
# exercised before this session, on this exact bootstrap toolchain.
#
# `bfd/config.h` is generated lazily, per subdirectory, as `make`
# descends into it (binutils' own real, older "Cygnus tree" multi-
# directory build convention) -- not eagerly by the top-level
# `../configure`. Fixed with the same bounded retry loop as before,
# now with the CORRECT sed target (`/* #undef TLS */` -> `#define TLS`,
# matching the exact comment-out convention above, not a `#define TLS
# ...` line that was never actually present) applied to every
# config.h that exists on disk so far after each failed attempt --
# `find`, not a hardcoded path, since other subdirectories (opcodes/,
# etc.) plausibly hit the identical gap once make reaches them. make's
# own dependency tracking is resumable, so each retry picks up exactly
# where the previous one left off.
# TCC does not pass binutils' own configure test for a thread-local
# storage class, so every generated config.h carries `/* #undef TLS */`
# -- and bfd.c then writes `static TLS bfd_error_type bfd_error;`, which
# is a syntax error rather than a missing feature (TCC reports
# `bfd.c:733: error: ';' expected (got "bfd_error_type")`).
#
# Through -5 this was handled by building, letting it fail, running a
# `find ... -exec sed` over every config.h to define TLS as empty, and
# retrying -- up to ten times. Two things were wrong with that. It is a
# retry-until-it-works loop, which this project does not do. And it had
# not actually worked in a long time: `findutils` was never installed
# anywhere on the real box, so the repair step was `find: command not
# found` ten times in a row while the loop dutifully rebuilt the same
# failure. Invisible under the old shared sandbox, which happened to
# have `find`; immediately visible once the environment held only what
# was declared (#109).
#
# Defining TLS to nothing on the command line is what the loop was
# trying to arrive at, done once and deterministically. Nothing here is
# threaded, and the alternative -- a real __thread -- is exactly what
# configure already established this compiler does not have.
#
# LDFLAGS="-lm" is the other TCC difference: gprof calls fabs(), which
# GCC folds into a builtin and TCC emits as a real call, so the link
# failed with `undefined symbol 'fabs'`. Harmless for every other
# binary here -- libm was already a NEEDED entry on all of them.
#
# Both flags were confirmed by building this exact tarball with this
# exact command line before publishing, rather than by another round
# trip to the real box. Same run confirmed pkg_depends above by reading
# the output instead of guessing at it:
#
#   ar       libz.so.1 libc.so.6 libm.so.6 libfl.so.2
#   objdump  libz.so.1 libc.so.6 libm.so.6
#   ld       libm.so.6 libc.so.6 libz.so.1
#   as       libm.so.6 libc.so.6 libz.so.1
#
# libc and libm come from the image baseline every container gets; libz
# and libfl are what pkg_depends has to name.
# -8: CC switches from TCC to the real gcc 9.5 (absolute path -- a bare
# `gcc` resolves a relative installation prefix in this environment and
# breaks cc1 lookup, confirmed previously and recorded in CLAUDE.md).
#
# Why: the gcc 16.2 bootstrap (#116) fails at stage 2 with corrupt
# binaries -- selftest segfaults plus heap-corruption aborts inside
# libgcc's unwind-frame registry teardown -- and the failure survived
# removing the seed's optimizer from stage 1 entirely (16.2.0-8), which
# exonerated that axis. The remaining suspects both involve what 16.2's
# newly-emitted assembly and links pass through: this TCC-built as/ld.
# TCC has four confirmed classes of silently-wrong-output miscompiles
# in this project's own record (static-inline duplication, the bare
# `linux` macro flipping squashfs-tools' entire endianness detection,
# packed-struct ABI, -pthread argument-dropping), and BFD -- which
# parses and REWRITES .eh_frame during every link -- is exactly the
# kind of dense, macro-heavy code where the next one hides. Stage-1
# binaries (9.5-compiled, 9.5-era assembly) pass through this same
# as/ld and work; stage-2 binaries (16.2-emitted assembly) come out
# broken. Rebuilding binutils with 9.5 -- whose directly-compiled
# output is the one provenance class demonstrably working under real
# load (stage 1 compiled all ~5000 stage-2 files) -- removes TCC from
# the assembler/linker axis so the next bootstrap run discriminates
# cleanly: green means this was the fault; the same failure means the
# seed 9.5 itself is next under the lamp.
#
# The two TCC-specific workarounds drop out deliberately, not
# incidentally: -DTLS= papered over TCC failing binutils' TLS
# storage-class probe (gcc passes it and gets a real __thread), and
# LDFLAGS=-lm covered TCC emitting fabs() as a real call where gcc
# folds it to a builtin. Keeping either against gcc would be carrying
# a workaround for a compiler that is no longer here.
pkg_build() {
	mkdir -p build
	cd build
	# --enable-targets=x86_64-pep adds a PE/COFF linker emulation
	# (i386pep) alongside the default ELF one. ADR-0215: the Cix EFI
	# boot manager is a PE32+ image, and GNU ld can link one directly --
	# `ld -m i386pep` -- so no ELF-to-PE conversion step is needed at
	# all. That is the whole reason systemd's sd-boot build needs Python
	# and pyelftools (its tools/elf2efi.py) and this does not.
	#
	# Configured for the default target only, ld has no such emulation
	# and the link fails outright. Adding one changes nothing about how
	# the default target behaves -- BFD emulations are independent, and
	# every existing package links exactly as before.
	CC=/usr/bin/gcc CFLAGS="-g -O2" ../configure --prefix=/usr \
		--enable-targets=x86_64-pep \
		--disable-multilib --disable-gold --disable-gprofng \
		--enable-deterministic-archives
	make -j"$(nproc)" MAKEINFO=true
}

# Confirmed via ldd against every one of the 16 real tools this build
# produces: every single one links against nothing but libc -- binutils
# statically links its own libbfd/libopcodes/libctf/libsframe into each
# tool, so no extra runtime libraries need staging here (unlike most of
# this project's other recipes). Static archives (.a/.la), headers, and
# docs (info/man/locale) are dropped -- nothing else in this project's
# image set links against libbfd directly. The triplet-prefixed
# duplicate copies under usr/x86_64-pc-linux-gnu/{bin,lib} (binutils'
# own cross-compilation convention) are skipped too -- this project
# never cross-compiles, so only the plain usr/bin/* names are staged.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/x86_64-pc-linux-gnu" "$PKG_DESTDIR/usr/share" \
	       "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/usr/lib"/*.a "$PKG_DESTDIR/usr/lib"/*.la
}
