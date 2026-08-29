#
# shim -- the Microsoft-signed UEFI first-stage loader and MokManager,
# as Debian ships them: usr/lib/shim/shimx64.efi.signed and
# usr/lib/shim/mmx64.efi.signed.
#
# These are the one class of binary this project can never build, and
# that is deliberate rather than a gap. shim's whole purpose is to be
# signed by Microsoft's UEFI CA, which is what lets it load on a
# machine with Secure Boot enabled and no enrollment at all; it then
# chain-loads our own Cix-signed systemd-boot, and MokManager enrolls
# our signing certificate on the one occasion that is needed
# (ADR-0015). A shim we compiled ourselves would carry no signature
# anyone's firmware trusts, so building it would produce a file that
# cannot do the only job it has.
#
# Until now they were not packaged at all -- image/src/mkinstalleriso.c
# read them straight off whatever machine happened to run it, at
# /usr/lib/shim/. That worked because every ISO this project has ever
# produced was built on the development machine, which has Debian's
# shim-signed installed. It is also exactly why `POST /v1/system/iso`
# could not run on a real Cix host: the control-plane root mkbootroot
# builds stages neither file, so the tool failed on a path nobody had
# looked at. Packaging them makes the dependency explicit, versioned
# and checksum-verified instead of an unstated property of one
# machine's apt history.
#
# This does not weaken the Build Provenance Mandate; it is the honest
# form of the exception the mandate already implies. The rule forbids
# passing off a foreign binary as Cix-built and forbids copying another
# system's files in by accident. Here the binary is named, pinned, and
# fetched from its real distributor, with a comment saying why it can
# never be ours -- the same posture kernel.recipe takes toward an
# upstream tarball it does not write.
#
# pkg_source[0] is shim's own real upstream source release, and it is
# not built here. It is the provenance anchor: it states which shim
# these signed binaries actually are (16.1, matching Debian's
# shim-unsigned 16.1-2~deb12u1), so the version in this recipe means
# something checkable rather than being Debian's packaging revision.
# The same posture isotools.recipe already takes with grub's tarball,
# and libc-dev.recipe with glibc's -- a real, checksum-verified source
# alongside an install step that copies prebuilt files.
#
# The two .debs follow as extra sources, placed verbatim at
# /build/extra/<basename> (ADR-0036) rather than extracted, since a
# .deb is an ar archive and source[0] is the only one the daemon
# untars. Both were fetched from deb.debian.org and confirmed
# byte-identical to the copies the development machine has been
# building ISOs from, so this changes where the bytes come from and
# nothing about the bytes.
#
pkg_name="shim"
pkg_version="16.1-1"
pkg_source="https://github.com/rhboot/shim/releases/download/16.1/shim-16.1.tar.bz2 http://deb.debian.org/debian/pool/main/s/shim-signed/shim-signed_1.51~1+deb12u1+16.1-2~deb12u1_amd64.deb http://deb.debian.org/debian/pool/main/s/shim-helpers-amd64-signed/shim-helpers-amd64-signed_1+16.1+2~deb12u1_amd64.deb"
pkg_sha256="46319cd228d8f2c06c744241c0f342412329a7c630436fce7f82cf6936b1d603 c2acf5e559664bbadb7ce5789f9a321f39754a34854d3097b8cc0369df513b3b 26acac35133946c1642a09f3fda353ee3b849e5fc643fadb97a8dfec9e2b55fb"
pkg_artifact_sha256="30d7f210bc2d9e0e28ac1cab064d6d6949139c4f478214d114bbd43ecfe5b024"
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly these
# and nothing else -- there is no fallback (#168).
#   bash coreutils  the recipe functions, mkdir/cp/test
#   binutils        `ar` -- a .deb is an ar archive, and nothing else
#                   here can open one
#   tar xz          the data member inside each .deb is a tar.xz
pkg_build_depends="bash coreutils binutils tar xz"

# Unpacks both .debs. Nothing is compiled: see the header for why these
# binaries cannot be, and must not be, produced here.
#
# Each .deb is `ar x`'d in its own directory -- the members are all
# named data.tar.xz/control.tar.xz/debian-binary, so unpacking two of
# them in one place would have the second silently overwrite the first
# and install MokManager twice under two names.
pkg_build() {
	for d in shim-signed shim-helpers; do
		mkdir -p "/build/$d"
	done
	( cd /build/shim-signed && ar x /build/extra/shim-signed_1.51~1+deb12u1+16.1-2~deb12u1_amd64.deb && tar xJf data.tar.xz )
	( cd /build/shim-helpers && ar x /build/extra/shim-helpers-amd64-signed_1+16.1+2~deb12u1_amd64.deb && tar xJf data.tar.xz )

	# Fail here, loudly, rather than installing an incomplete pair. A
	# missing MokManager does not surface until an operator is standing
	# at a firmware prompt that never appears.
	test -f /build/shim-signed/usr/lib/shim/shimx64.efi.signed
	test -f /build/shim-helpers/usr/lib/shim/mmx64.efi.signed
}

# Installed at exactly the paths image/src/mkinstalleriso.c already
# reads (SHIM_EFI_SRC / MOKMANAGER_EFI_SRC), so nothing in that tool
# changes -- it stops depending on the build machine's apt history and
# starts depending on a package, which is the whole point.
#
# fbx64.efi.signed (the fallback loader, also in shim-helpers) is
# deliberately not installed: it exists to re-create a removed EFI boot
# entry from a BOOTX64 fallback path, and this project's installer
# writes its own boot entries directly (populate_esp()). Shipping a
# loader nothing invokes would be one more signed binary to reason
# about for no behaviour.
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/lib/shim"
	cp -a /build/shim-signed/usr/lib/shim/shimx64.efi.signed \
	      "$PKG_DESTDIR/usr/lib/shim/"
	cp -a /build/shim-helpers/usr/lib/shim/mmx64.efi.signed \
	      "$PKG_DESTDIR/usr/lib/shim/"
}
