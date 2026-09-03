#
# probe-tcc-conformance 22 -- what does tcc print when autotools asks it
# where things are? (#220)
#
# gettext dies at its FIRST compile, on a command line carrying raw C
# where flags belong:
#
#   tcc -DHAVE_CONFIG_H -I. -I..  unsigned short __builtin_bswap16(unsigned short); ...
#   /bin/sh: syntax error near unexpected token `('
#
# That text is verbatim what tcc 0.9.28rc-16 added to include/tccdefs.h
# for #208, and the upgrade is also what changed since #220 was filed.
# It occupies the $(CPPFLAGS) slot exactly, and autoconf did not inherit
# it from the environment -- the logged sub-configure invocation carries
# only CC=tcc, and autoconf passes every precious variable it inherited
# explicitly. So configure APPENDED it, and gettext-runtime/configure
# appends to CPPFLAGS in exactly one non-flag way: $INCINTL, computed by
# AC_LIB_LINKFLAGS_BODY, which probes the compiler by running it.
#
# The hypothesis is therefore that one of those probes captures compiler
# OUTPUT that now contains tcc's builtin declarations, and the captured
# text lands in an include-path variable. This measures it rather than
# assuming it, because the dev sandbox's tcc is Debian's and cannot
# answer a question about this platform's compiler.
#
# Three questions, all cheap:
#   1. what does -print-search-dirs print, and on which stream?
#   2. what does -print-multi-os-directory do with an unknown flag?
#   3. does -E on empty input emit the tccdefs.h prelude?
#
# Fails on purpose at the end, like every probe in this series -- the
# build log is the product.
#
pkg_name="probe-tcc-conformance"
pkg_version="22"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_build_depends="tcc make bash coreutils sed grep gawk binutils findutils"
pkg_changelog="22: measure what tcc prints for the compiler-probing options autotools uses (#220) -- -print-search-dirs, -print-multi-os-directory and -E on empty input, each with stdout and stderr kept apart, to find where gettext picks up raw C declarations as CPPFLAGS."

probe() { printf '  %-46s ' "$1"; }

pkg_build() {
	echo "=== compiler under test ==="
	tcc -v 2>&1 | head -2 | sed 's/^/  /'
	echo

	echo "=== 1. -print-search-dirs (what AC_LIB_PREPARE_MULTILIB captures) ==="
	echo "--- stdout only, which is what a backquote capture takes:"
	tcc -print-search-dirs 2>/dev/null | sed 's/^/  OUT: /' | head -20
	echo "--- stderr only:"
	tcc -print-search-dirs 2>&1 >/dev/null | sed 's/^/  ERR: /' | head -10
	echo "--- exit status: $(tcc -print-search-dirs >/dev/null 2>&1; echo $?)"
	echo

	echo "=== 2. -print-multi-os-directory (libtool captures this one) ==="
	echo "--- stdout only:"
	tcc -print-multi-os-directory 2>/dev/null | sed 's/^/  OUT: /' | head -20
	echo "--- stderr only:"
	tcc -print-multi-os-directory 2>&1 >/dev/null | sed 's/^/  ERR: /' | head -10
	echo "--- exit status: $(tcc -print-multi-os-directory >/dev/null 2>&1; echo $?)"
	echo

	echo "=== 3. -E on empty input: does the tccdefs.h prelude come out? ==="
	: > /run/empty.c
	echo "--- stdout line count: $(tcc -E /run/empty.c 2>/dev/null | wc -l)"
	echo "--- any bswap declarations in it?"
	if tcc -E /run/empty.c 2>/dev/null | grep -n '__builtin_bswap' | head -5; then
		echo "  ^^ PRESENT -- anything capturing preprocessor output picks these up"
	else
		echo "  none"
	fi
	echo "--- first 5 lines of -E output:"
	tcc -E /run/empty.c 2>/dev/null | head -5 | sed 's/^/  /'
	echo

	echo "=== 4. the same three, as a shell capture, which is how configure does it ==="
	sd=$(tcc -print-search-dirs 2>/dev/null)
	md=$(tcc -print-multi-os-directory 2>/dev/null)
	echo "  searchpath capture length : ${#sd}"
	echo "  multi-os capture length   : ${#md}"
	case "$sd$md" in
	*__builtin_bswap*) echo "  VERDICT: a probe capture CONTAINS the declarations -- this is the path" ;;
	*)                 echo "  VERDICT: neither probe capture contains them -- look elsewhere" ;;
	esac
	echo

	echo "=== probe complete -- failing on purpose so nothing installs ==="
	exit 1
}
