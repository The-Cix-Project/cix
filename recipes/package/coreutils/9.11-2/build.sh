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
pkg_version="9.11-2"
pkg_source="https://ftp.gnu.org/gnu/coreutils/coreutils-9.11.tar.xz"
pkg_sha256="394024eda0a5955217ceda9cd1201e65dc8fa3aa29c2951135a49521d57c3cc3"
pkg_depends=""

# 9.11's own bare `CC=tcc ./configure` failed rebuilding against this
# project's now-real gcc/bash/make/sed-equipped "dev" image toolchain
# (confirmed live): lib/hash.c's #include chain (gnulib's own
# generated ./lib/stdlib.h, wrapping the real
# /usr/include/x86_64-linux-gnu/bits/stdlib.h) hit glibc's own
# fortified-wctomb() inline, which hard #errors ("Assumed value of
# MB_LEN_MAX wrong") the moment _FORTIFY_SOURCE resolves to a nonzero
# level under a compiler glibc doesn't recognize as a real, sufficient
# GCC (TCC defines no __GNUC__ at all, confirmed via `tcc -dM -E -
# </dev/null`) -- confirmed directly and reproduced standalone
# (`tcc -O2 -D_FORTIFY_SOURCE=2 ...` triggers glibc's own
# "_FORTIFY_SOURCE requires GCC 4.1 or later" warning down the same
# code path). This is not a new class of gap: CLAUDE.md's own standing
# TCC compile convention already mandates `-D_FORTIFY_SOURCE=0` on
# every invocation for exactly this reason -- this recipe was simply
# the first coreutils build attempted since that convention started
# actually mattering here (an earlier install of this exact recipe
# predates gcc/bash/make/sed landing in the same cumulative pkgbuild
# sandbox). Fixed by adding the same standard define to CFLAGS, no
# other change.
pkg_build() {
	FORCE_UNSAFE_CONFIGURE=1 CC=tcc CFLAGS="-D_FORTIFY_SOURCE=0" \
	    ./configure --prefix=/usr
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
