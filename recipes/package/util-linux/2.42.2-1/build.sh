#
# util-linux -- sfdisk(8), and only sfdisk.
#
# This platform's partition management is sfdisk end to end: diskpart.h
# documents diskpart_create/append/delete as sfdisk invocations, and the
# daemon holds its path in a *_BIN define like every other staged host
# tool. There are 99 references to it across the daemon and the tests.
# It was never packaged, so cix-tests named util-linux as a build tool
# and the composer correctly refused a package that exists nowhere
# (#295).
#
# Only sfdisk is built. blkid, losetup and partx appear in this
# codebase's comments -- explaining what sfdisk --append does, or what
# a test cannot do here for want of loop devices -- and are never
# invoked, so packaging them would be shipping what nothing runs.
#
# The source is the same tarball libuuid already builds from, at the
# same checksum, which is the whole reason libuuid's own header calls
# that a standalone build of one part of the util-linux tree rather
# than a separate project.
#
pkg_name="util-linux"
pkg_version="2.42.2-1"
pkg_source="https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.2.tar.xz"
pkg_sha256="03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"
#
# libtinfo, which libsmartcols reaches for. Everything else sfdisk
# needs is built here and linked in statically, see pkg_build().
#
pkg_depends="ncurses"
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf"
pkg_changelog="2.42.2-1: first packaging. sfdisk only, statically linked against the libfdisk and libsmartcols built alongside it, so the package is one binary and one dependency rather than four libraries whose sonames have to agree with anything already installed."

pkg_build() {
	#
	# The flags are measured, not guessed. --enable-sfdisk does not
	# exist: configure.ac gates it behind --enable-fdisks, which covers
	# fdisk, sfdisk and cfdisk together, and the per-tool make target is
	# what narrows it back down to one binary.
	#
	# Each library is enabled explicitly because --disable-all-programs
	# disables them too, and configure then refuses with "libfdisk is
	# needed to build fdisk" rather than enabling it for us. sfdisk
	# requires libfdisk and libsmartcols, and libfdisk requires libuuid
	# (UL_REQUIRES_BUILD in configure.ac).
	#
	# --disable-shared makes those libraries static, which is the point:
	# libfdisk and libsmartcols are not packaged for this platform and
	# would otherwise have to ship beside sfdisk, with their sonames
	# needing to agree with whatever else ever links them. Statically
	# linked, the result is one binary that depends only on libtinfo and
	# libc.
	#
	# --without-readline drops the only other shared dependency; sfdisk
	# uses it for interactive prompting, which nothing here does.
	#
	# __STDC_NO_VLA__ is accurate rather than a workaround. glibc's
	# regex.h declares regexec()'s regmatch_t parameter with a C99
	# VLA-in-prototype whose size names the NEXT parameter, and tcc's
	# parser rejects it outright ("'__nmatch' undeclared"). The header
	# has an #ifndef __STDC_NO_VLA__ branch for exactly a compiler
	# without VLA support, and tcc is one, so saying so steers it onto
	# a branch tcc parses. Same treatment daemon/src/logstore.c uses.
	#
	CC=tcc CPPFLAGS="-D__STDC_NO_VLA__=1" ./configure \
	    --prefix=/usr \
	    --disable-all-programs \
	    --enable-fdisks \
	    --enable-libfdisk \
	    --enable-libsmartcols \
	    --enable-libuuid \
	    --enable-libblkid \
	    --disable-shared \
	    --enable-static \
	    --without-readline
	make -j"$(nproc)" sfdisk
}

pkg_install() {
	#
	# /usr/sbin/sfdisk, which is where daemon/src's own SFDISK_BIN
	# looks. Installed by hand rather than by make install, because
	# make install here would also install libfdisk, libsmartcols,
	# libuuid, libblkid and their headers -- everything the build
	# needed and nothing this package is for.
	#
	# libtool leaves the real ELF in .libs/ when it has wrapped the
	# link; with --disable-shared it usually does not, so both are
	# checked rather than one assumed.
	#
	mkdir -p "$PKG_DESTDIR/usr/sbin"
	if [ -x .libs/sfdisk ] && head -c 4 .libs/sfdisk | grep -q ELF; then
		cp .libs/sfdisk "$PKG_DESTDIR/usr/sbin/sfdisk"
	elif head -c 4 sfdisk | grep -q ELF; then
		cp sfdisk "$PKG_DESTDIR/usr/sbin/sfdisk"
	else
		echo "util-linux: neither sfdisk nor .libs/sfdisk is an ELF binary" >&2
		exit 1
	fi
	chmod 0755 "$PKG_DESTDIR/usr/sbin/sfdisk"
	#
	# Asserted against the real bytes: a partition tool that cannot
	# report its own version is not one this platform can drive, and a
	# broken install here would surface as a disk operation failing at
	# execve() long after the package reported success.
	#
	"$PKG_DESTDIR/usr/sbin/sfdisk" --version
}
