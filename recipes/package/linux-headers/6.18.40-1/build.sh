#
# linux-headers -- the Linux UAPI headers (linux/, asm/, asm-generic/)
# that glibc compiles against and that every C program including a
# kernel interface needs.
#
# First step in closing the last foreign dependency in the whole system.
# Until now these headers were harvested out of the build host by
# libc-dev.recipe, alongside glibc's own -- so a Cix image's kernel
# interface definitions came from whatever distribution the build
# container happened to be, not from the kernel this project builds and
# boots. ADR-0209 named glibc/ld.so as "the one remaining foreign
# dependency"; the UAPI headers sit in the same hole.
#
# Deliberately NOT a kernel build. `make headers_install` only
# preprocesses and copies the exported UAPI headers -- it compiles
# nothing, needs no compiler at all, and is independent of the kernel
# CONFIG used to build a bootable image. That is why this is a separate,
# cheap package rather than an output of kernel.recipe: an image needs
# these headers without wanting a 50-minute kernel build.
#
# Same 6.18.40 source the kernel package builds, so the headers an image
# compiles against are the ones the kernel it boots actually implements.
#
pkg_name="linux-headers"
pkg_version="6.18.40-1"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.40.tar.xz"
pkg_sha256="3712fc1ec839e4daac981176c8518912e8f452650aaedfe4381da4419613a431"
pkg_depends=""
# ADR-0199/0209: composed from exactly these. No compiler: headers_install
# runs the kernel's own Makefile plus sed/awk text processing and file
# copying, nothing more. rsync is deliberately absent -- modern
# headers_install uses a plain shell/Make copy path.
pkg_build_depends="bash coreutils make sed grep gawk findutils"

pkg_build() {
	# headers_install writes into usr/include/ under the build tree; it
	# does not compile, so ARCH is the only thing it needs told.
	make ARCH=x86 headers_install INSTALL_HDR_PATH=/build/hdr
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/include"
	cp -a /build/hdr/include/. "$PKG_DESTDIR/usr/include/"
	# headers_install leaves .install/..install.cmd bookkeeping behind;
	# they are build artifacts, not headers.
	find "$PKG_DESTDIR/usr/include" -name '.install' -o -name '..install.cmd' | \
		while read -r f; do rm -f "$f"; done
}
