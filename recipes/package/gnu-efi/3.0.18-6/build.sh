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
pkg_version="3.0.18-6"
pkg_source="https://sourceforge.net/projects/gnu-efi/files/gnu-efi-3.0.18.tar.bz2/download"
pkg_sha256="7f212c96ee66547eeefb531267b641e5473d7d8529f0bd8ccdefd33cf7413f5c"
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168). Revision bumped purely to carry this: a recipe
# version is immutable once published, so it could never reach a host
# that already has 3.0.18.
# A bare make with CC=/usr/bin/gcc -- no configure, so no sed/grep/awk probing; binutils for the archive step its Makefile drives.
# Tier-3 TCC exception (ADR-0211), built with the Cix-built gcc
# 16.2.0-11 from our own artifact cache. gnu-efi needs MS-ABI variadics
# (__builtin_ms_va_list) for the EFI calling convention, which TCC does
# not implement -- reduced to a one-line probe. binutils-dev follows it
# as sbsigntools' other dependency; its own TCC parse failure in bfd.c
# is NOT root-caused, so if it turns out to build under TCC once its
# declaration is right, it should come back off the list.
# sed: gnu-efi's Makefile derives ARCH by shelling out --
#   ARCH := $(shell uname -m | sed s,i[3456789]86,ia32,)
# -- so without sed the pipeline yields nothing, ARCH is empty, and the
# build fails three steps later at the archive step with a path that
# names nothing useful:
#   ar: /initplat.o: No such file or directory
# (that leading slash is $(ARCH)/ expanding to nothing). A missing tool
# producing an empty variable rather than an error is exactly the shape
# that makes an under-declaration hard to read.
pkg_toolchain="gcc"
pkg_toolchain_reason="missing language feature: needs __builtin_ms_va_list for the EFI calling convention, which TCC does not implement"
pkg_build_depends="bash coreutils make gcc linux-headers binutils sed"
pkg_changelog="3.0.18-6: declare pkg_toolchain=gcc and its reason (#222, ADR-0226)"

pkg_build() {
	# ARCH stated rather than derived. sed is declared above so the
	# Makefile's own uname|sed pipeline would now work, but a build
	# should not depend on shelling out to discover something this
	# recipe already knows -- and an empty ARCH fails far from its
	# cause. Cix is x86_64 only today; when that stops being true this
	# line is the one place that has to change.
	make -j"$(nproc)" ARCH=x86_64 CC=/usr/bin/gcc PREFIX=/usr
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" PREFIX=/usr
}
