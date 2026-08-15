#
# GRUB 2.14 -- grub-mkrescue/grub-mkimage/grub-install and every real
# x86_64-efi *.mod file, for building thinC's own installer/update
# ISOs from source (Part 5, bare-metal-readiness plan) instead of
# relying on whatever grub tools happen to be on the build host.
#
# A single ./configure pass builds everything -- confirmed directly
# against the real GRUB 2.14 source (util/grub-mkrescue.c, util/grub-
# mkimage.c, the top-level INSTALL file): grub-mkrescue is a real
# compiled C binary, not a wrapper script, and it statically links the
# same in-tree image-generation code grub-mkimage.c itself calls --
# there is no second, separately-configured "target" build tree needed,
# contrary to an earlier, unresearched assumption in this project's own
# bare-metal-readiness plan. --target=x86_64 --with-platform=efi
# selects the x86_64-efi module set this platform needs (thinC boots
# via systemd-boot/UEFI only, ADR-0031 -- the BIOS/i386-pc pass real
# distros also build is deliberately skipped entirely, and its own
# --image-base sed-patch gotcha with it, since that's a BIOS/EfiEmu-
# only concern). --disable-efiemu/--disable-werror mirror Linux From
# Scratch's own real, tested 2.14 x86_64-efi build flags.
#
# GRUB2 maintains its own self-contained EFI headers/PE32+ structs
# in-tree (grub-core/kern/efi/, include/grub/efi/, confirmed directly)
# -- no gnu-efi dependency here, unlike sbsigntools.recipe.
#
# xorriso/mtools are explicitly NOT build dependencies (confirmed:
# upstream's own INSTALL file lists them only under "prerequisites for
# running make-check", GRUB's own test suite, not its build) -- they're
# runtime dependencies of grub-mkrescue itself (see xorriso.recipe/
# mtools.recipe), staged onto whichever image actually runs
# grub-mkrescue later, not needed here.
#
pkg_name="grub"
pkg_version="2.14"
pkg_source="https://ftp.gnu.org/gnu/grub/grub-2.14.tar.xz"
pkg_sha256="bc8d3c73535b8838d8c8e2654d73edc4e6ae8c8acdb45d5df5dc9a1547446d43"
pkg_depends="gettext patch"

pkg_build() {
	CC=tcc ./configure --prefix=/usr --sysconfdir=/etc --target=x86_64 \
		--with-platform=efi --disable-efiemu --disable-werror
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/info" "$PKG_DESTDIR/usr/share/man" \
	       "$PKG_DESTDIR/usr/share/locale"
}
