#
# coreutils -- GNU Coreutils: cat/ls/cp/mv/rm/mkdir/chmod/chown/ln/
# mkfifo/sort/head/tail/wc/cut/tr/basename/dirname/readlink/realpath/
# stat/touch/date/env/printf/sleep/timeout/yes/tee/split/uniq/comm and
# the rest of the ~100 basic file/text utilities every real shell
# environment assumes exist. Found as a real, missing gap two ways at
# once in the same session: the user asked for it directly mid-session,
# and independently, verifying gcc/python self-host inside a real
# 'dev' container hit the exact same wall -- test scripts using
# `cat > file << EOF` silently failed ("command not found") because
# this minimal image had no coreutils at all, only bash's own
# builtins.
#
# Source is GNU's own canonical ftp.gnu.org release, unchanged from
# 9.11, checksum verified against two independent mirrors (ftp.gnu.org
# and mirrors.kernel.org) -- byte-identical, same sha256.
#
pkg_name="coreutils"
pkg_version="9.11-5"
pkg_source="https://ftp.gnu.org/gnu/coreutils/coreutils-9.11.tar.xz"
pkg_sha256="394024eda0a5955217ceda9cd1201e65dc8fa3aa29c2951135a49521d57c3cc3"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/coreutils-9.11-3.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_depends=""
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils"
pkg_changelog="9.11-5: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 9.11-4: declares its build tools so it can be rebuilt through the ordinary install path (#206)"

# 9.11-2 fixed the glibc fortify #error (-D_FORTIFY_SOURCE=0) but the
# same build then hit the identical, already root-caused TCC/gnulib
# static-inline conformance gap m4.recipe and sed 4.9-2 both already
# document in full: gnulib's `_GL_INLINE`/`_GL_EXTERN_INLINE` machinery
# trusts a bare `__STDC_VERSION__ >= 199901L` to mean real, correctly
# file-scoped C99 inline semantics whenever `__GNUC__` is undefined;
# TCC claims that version without implementing the "declared inline
# nowhere given a non-inline instantiation" rule correctly, so every
# translation unit pulling in a shared gnulib header (the c32*/mbszero
# wide-char shims, memeq/streq/_gl_strnul, rpl_realloc, xsum*, ...)
# emitted a full, strong, globally-visible definition instead of a
# file-local one -- dozens of "defined twice" archive-link errors,
# same signature, same fix: `_GL_EXTERN_INLINE_STDHEADER_BUG=1` forces
# gnulib's own designed-in `static _GL_UNUSED` fallback.
#
# The accompanying `tcc: error: undefined symbol '__dso_handle'` is the
# same real, environment-specific bare-tcc-link CRT gap sysklogd/m4/sed
# all already document -- fixed the same proven way: a `weak` stub
# object passed as a bare object-file path in LIBS (never `-lxxx`,
# which m4.recipe's own trail already found gets conditionally
# extracted away for a weak-only archive member on this toolchain).
pkg_build() {
	echo 'void *__dso_handle __attribute__((weak)) = (void *)0;' > dso_stub.c
	tcc -c dso_stub.c -o dso_stub.o

	FORCE_UNSAFE_CONFIGURE=1 CC=tcc \
	    CFLAGS="-D_FORTIFY_SOURCE=0 -D_GL_EXTERN_INLINE_STDHEADER_BUG=1" \
	    ./configure --prefix=/usr LIBS="$(pwd)/dso_stub.o"
	make -j"$(nproc)" MAKEINFO=true
}

# Confirmed via ldd across every one of the 102 real binaries this
# build produces: all but two link against nothing but libc. `ls`
# links libcap.so.2 (its own file-capability display support, real
# glibc/libcap integration, not optional bloat) and the sha*sum family
# links libcrypto.so.3 (real OpenSSL-backed digest implementations).
# Both staged the same pattern every other recipe's own runtime libs
# already use -- libcap via its real SONAME symlink plus versioned
# target (confirmed via `ls -la`), libcrypto as a plain real file
# (already the exact SONAME, no separate symlink needed, confirmed the
# same way hostapd.recipe's own libcrypto staging already established).
# usr/libexec/coreutils/libstdbuf.so (stdbuf's own small, real
# LD_PRELOAD buffering-interception helper) is kept; usr/share (info/
# man/locale) is dropped, matching every other recipe's own doc
# stripping.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share"
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp -a /lib/x86_64-linux-gnu/libcap.so.2 /lib/x86_64-linux-gnu/libcap.so.2.66 \
	   /lib/x86_64-linux-gnu/libcrypto.so.3 \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
}
