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
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against two independent mirrors (ftp.gnu.org and mirrors.kernel.org)
# -- byte-identical, same sha256.
#
pkg_name="coreutils"
pkg_version="9.11"
pkg_source="https://ftp.gnu.org/gnu/coreutils/coreutils-9.11.tar.xz"
pkg_sha256="394024eda0a5955217ceda9cd1201e65dc8fa3aa29c2951135a49521d57c3cc3"
pkg_depends=""

# Plain autotools, confirmed directly. MAKEINFO=true skips texinfo doc
# generation, same reasoning as every other recipe in this batch.
# FORCE_UNSAFE_CONFIGURE=1 bypasses coreutils' own "you should not run
# configure as root" safety check -- a real, deliberate guard against
# running its *test suite* as root on a real workstation (root bypasses
# permission checks the tests rely on), not against a legitimate
# isolated build; this project's own build container has no non-root
# user at all (every other recipe in this project also builds as root,
# confirmed via `pkg_build` running inside the same real environment),
# and this recipe never runs `make check`.
pkg_build() {
	FORCE_UNSAFE_CONFIGURE=1 CC=tcc ./configure --prefix=/usr
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
