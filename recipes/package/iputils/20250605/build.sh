#
# iputils -- ping/arping/tracepath/clockdiff, the standard Linux
# network diagnostic toolset. Same recipe contract as bash.recipe --
# see that file's own header comment for the metadata-scanner-vs-
# sourced-shell-script split.
#
# Source is Debian's own ".orig.tar.xz" for their iputils package --
# the exact, unmodified upstream release tarball (Debian repackages
# nothing into "orig" tarballs, by their own packaging policy), used
# here rather than github.com/iputils/iputils's own release page
# directly because that 403s any non-browser User-Agent, the same
# workaround bird.recipe's own header comment already documents for
# an unrelated upstream. Checksum verified two ways: against Debian's
# own published .dsc Checksums-Sha256 field for this exact file, and
# independently by downloading it and computing sha256 directly --
# both matched.
#
pkg_name="iputils"
pkg_version="20250605"
pkg_source="https://deb.debian.org/debian/pool/main/i/iputils/iputils_20250605.orig.tar.xz"
pkg_sha256="2343570656f3cfc191eedd887fd8b5b78f68d0b68e59f2d45b17209cdcfd35a3"
pkg_depends=""

# Meson, not autotools -- confirmed directly (meson.build/meson_options.txt,
# no configure.ac at all). NO_SETCAP_OR_SUID=true: iputils' own default
# tries to run build-aux/setcap-setuid.sh (setcap/chmod +s on ping) at
# install time -- meaningless and unnecessary here, since every
# container this ends up in runs its own cmd as root already (real
# CAP_NET_RAW, no setuid/setcap dance needed). BUILD_MANS=false: the
# only other non-default flag needed -- confirmed empirically, the
# first real build attempt failed outright on a missing xsltproc
# nothing else in this toolchain provides, not worth adding a whole
# docs-generation dependency for man pages that never ship into any
# image (no `man` present in any Cix image either). USE_CAP/
# USE_IDN stay at their real upstream defaults (true) -- both found
# via the toolchain's own already-real libcap-dev/libidn2-dev.
pkg_build() {
	meson setup build --prefix=/usr -DNO_SETCAP_OR_SUID=true -DBUILD_MANS=false
	ninja -C build
}

# ping/arping/clockdiff link against libcap (CAP_NET_RAW capability
# handling) and ping additionally against libidn2 (IDN hostname
# support) + its own transitive libunistring -- confirmed directly via
# ldd against the real build above. None of the four are part of
# pkg_seed_image_runtime()'s own global bootstrap set (ld.so/libc/
# libtinfo/libgcc_s/libm -- the bare minimum every built binary needs,
# not a full transitive-dependency closure for every package), so this
# recipe stages its own three extras itself, into the exact same
# /lib/x86_64-linux-gnu/ path every other runtime lib in this project
# already resolves from -- copied from the isolated build container's
# own toolchain-provided copy of them (the same host libraries the
# binaries above were actually linked against a moment ago).
pkg_install() {
	dir="$PKG_DESTDIR/usr/bin"
	mkdir -p "$dir" "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp build/ping/ping build/arping build/tracepath build/clockdiff "$dir/"
	cp /lib/x86_64-linux-gnu/libcap.so.2 /lib/x86_64-linux-gnu/libidn2.so.0 \
	   /lib/x86_64-linux-gnu/libunistring.so.2 "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
}
