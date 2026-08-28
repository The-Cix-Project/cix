#
# gnu-efi -- EFI development headers (efi.h and friends) and a static
# libefi.a, needed to build sbsigntools (Part 5, bare-metal-readiness
# plan): sbsigntools' own configure.ac searches for crt0-efi-$ARCH.o
# and efi.h under standard prefix-relative paths rather than linking
# against a system EFI library of its own -- confirmed by reading
# sbsigntools' own configure.ac directly, not assumed. GRUB2 itself
# does NOT need this (it maintains its own self-contained EFI headers
# in-tree, confirmed against the real GRUB 2.14 source) -- this recipe
# exists solely for sbsigntools.
#
# Plain Makefile, no ./configure at all (confirmed directly against
# the real 3.0.18 source tree) -- DESTDIR/PREFIX are real Makefile
# variables (Make.defaults: INSTALLROOT defaults to $(DESTDIR),
# PREFIX defaults to /usr/local).
#
pkg_name="gnu-efi"
pkg_version="3.0.18-2"
pkg_source="https://sourceforge.net/projects/gnu-efi/files/gnu-efi-3.0.18.tar.bz2/download"
pkg_sha256="7f212c96ee66547eeefb531267b641e5473d7d8529f0bd8ccdefd33cf7413f5c"
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168). Revision bumped purely to carry this: a recipe
# version is immutable once published, so it could never reach a host
# that already has 3.0.18.
# A bare make with CC=tcc -- no configure, so no sed/grep/awk probing; binutils for the archive step its Makefile drives.
pkg_build_depends="bash coreutils make tcc libc-dev binutils"

pkg_build() {
	make -j"$(nproc)" CC=tcc PREFIX=/usr
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" PREFIX=/usr
}
