#
# GRUB 2.14 -- grub-mkrescue/grub-mkimage/grub-install and every real
# x86_64-efi *.mod file, for building Cix's own installer/update
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
# selects the x86_64-efi module set this platform needs (Cix boots
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
pkg_version="2.14-8"
pkg_source="https://ftp.gnu.org/gnu/grub/grub-2.14.tar.xz"
pkg_sha256="bc8d3c73535b8838d8c8e2654d73edc4e6ae8c8acdb45d5df5dc9a1547446d43"
# gettext dropped, with --disable-nls below. It was here only so grub
# could build translated messages, and pulling it in meant building
# gettext under TCC -- which fails twice over: libtool passes
# -version-script (unsupported), and with shared libraries disabled
# libtextstyle then wants libcroco/libxml symbols nothing provides.
# The installer menu is English; translations were never a requirement,
# so the dependency was cost with no benefit.
pkg_depends="patch"
# ADR-0199/0209: composed from exactly these, no fallback (#168).
# diffutils and pkgconf added after -2's configure said exactly what it
# wanted, which is the mechanism working rather than a guess:
#   checking for pkg-config... no
#   checking for cmp... no
#   configure: error: cmp is not found
# cmp is diffutils. Under the old fallback environment grub would have
# found whatever the shared sandbox happened to hold and built against
# it silently; naming them makes the environment exactly what this
# recipe asks for.
# bison and flex: grub generates its own parser and lexer, so both are
# real build inputs. bison came from -3's configure saying so
# ("configure: error: bison is not found"); flex is declared with it
# rather than waiting to be told, because a tool that generates a
# parser needs the one that generates the lexer beside it.
# python: grub's configure hard-requires an interpreter for its
# build-time generators, even from an official release tarball
# ("configure: error: no suitable Python interpreter found"). It is
# built under the Tier-3 gcc exception (ADR-0211) because CPython's
# atomics cannot compile under TCC; grub itself is still TCC.
pkg_build_depends="bash coreutils make gcc linux-headers sed grep gawk binutils patch findutils diffutils pkgconf bison flex m4 python freetype unifont"
pkg_changelog="2.14-8: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

pkg_build() {
	# CC=/usr/bin/gcc: GRUB's own configure refuses anything else --
	#   configure: error: GCC is required
	# It probes for GCC-specific attributes and code generation its EFI
	# targets depend on. Tier 3 of the TCC policy (ADR-0211), on the
	# build system's own say-so rather than a judgement call, using the
	# Cix-built gcc 16.2.0-11 from our own cache. Absolute path: a bare
	# "gcc" makes gcc resolve its installation prefix relatively and
	# fail cc1 with a misleading posix_spawnp error.
	#
	# FONT_SOURCE points GRUB's build at the unifont package's own BDF,
	# which is what makes it build grub-mkfont and generate the .pf2
	# fonts under share/grub. 2.14-6 had neither, and the consequence
	# only surfaced at the far end of an ISO build on a real host:
	#
	#   grub-mkrescue: error: cannot open `.../unicode.pf2':
	#   No such file or directory.
	#
	# grub-mkrescue always embeds a .pf2 for the media label. Isolated
	# directly rather than guessed: --fonts= correctly installs no menu
	# fonts (this project's ISO renders its menu on the EFI text
	# console and loads no font -- see mkinstalleriso's grub.cfg), and
	# the build still fails until --label-font has one. So a font is
	# genuinely required, and 2.14-6 shipped a GRUB that could not make
	# one: only grub-mkfont's bash completion was installed, never the
	# tool.
	#
	CC=/usr/bin/gcc ./configure --prefix=/usr --sysconfdir=/etc --target=x86_64 \
		--with-platform=efi --disable-efiemu --disable-werror --disable-nls \
		FONT_SOURCE=/usr/share/unifont/unifont.bdf
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/info" "$PKG_DESTDIR/usr/share/man" \
	       "$PKG_DESTDIR/usr/share/locale"

	# Asserted, not assumed. isotools harvests share/grub into its
	# artifact and mkinstalleriso points GRUB's own pkgdatadir there,
	# so a build that quietly produced no font would ship an ISO
	# toolchain that fails at its last step -- exactly the failure this
	# revision exists to fix, and one worth catching here instead.
	test -s "$PKG_DESTDIR/usr/share/grub/unicode.pf2" || {
		echo "grub: no unicode.pf2 was generated -- FONT_SOURCE was not used" >&2
		exit 1
	}
	test -x "$PKG_DESTDIR/usr/bin/grub-mkfont" || {
		echo "grub: grub-mkfont was not built -- freetype was not detected" >&2
		exit 1
	}
	echo "generated fonts:"
	ls -la "$PKG_DESTDIR/usr/share/grub/"*.pf2
}
