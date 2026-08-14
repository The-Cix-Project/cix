#
# tar -- GNU tar. PKG_TAR_BIN's own real use (daemon/src/pkg.c's
# extract_tarball()) is plain archive extraction, no ACL/SELinux
# extended-attribute preservation, no exotic backend -- a real local
# build confirmed this project's own default `./configure` here
# produces a `tar` binary linking against nothing but libc.so.6 (this
# dev sandbox's own Debian-packaged system tar additionally links
# libacl/libselinux/libpcre2 for optional features this project never
# uses; the default from-source build simply doesn't build those in).
#
# tar itself has no compression code at all (confirmed via a real
# `ldd`/ABI trace on this build) -- gzip/bzip2/xz (this recipe set's
# own gzip.recipe/bzip2.recipe/xz.recipe) are separately shelled out
# to via $PATH for any compressed archive, matching mkbootroot.c's own
# documented reasoning for staging all three onto the control-plane
# image alongside tar.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against a second independent mirror (mirrors.kernel.org) --
# byte-identical, same sha256.
#
pkg_name="tar"
pkg_version="1.35"
pkg_source="https://ftp.gnu.org/gnu/tar/tar-1.35.tar.gz"
pkg_sha256="14d55e32063ea9526e057fbf35fcabd53378e769787eff7919c3755b02d2b57e"
pkg_depends=""

# Plain autotools, confirmed directly. FORCE_UNSAFE_CONFIGURE=1/
# MAKEINFO=true: the same reasoning coreutils.recipe's own header
# comment already established (this build container runs everything
# as root with no texinfo present).
pkg_build() {
	FORCE_UNSAFE_CONFIGURE=1 CC=tcc ./configure --prefix=/usr MAKEINFO=true
	make -j"$(nproc)" MAKEINFO=true
}

# Real files from this recipe's own build. usr/libexec/rmt (remote-tape
# support, needs a real rsh/ssh-reachable remote host -- nothing in
# this project ever uses `tar --rmt-command`), info/man pages, and
# locale data are dropped, matching every other recipe's own
# doc-stripping convention.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/libexec" "$PKG_DESTDIR/usr/share"
}
