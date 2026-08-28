#
# glibc -- the C runtime, built from source on a Cix host with Cix's own
# compiler. This closes the last foreign dependency in the entire
# system.
#
# Until this recipe existed, every Cix image's C runtime came from
# whatever distribution the build host ran. ADR-0209 said so plainly and
# left it open: "glibc and ld.so remain host-sourced for now and are
# explicitly recorded as the last unclosed link in the bootstrap ...
# Everything above it is honest; this is not yet."
#
# Exactly six files were foreign, all from here:
#   pkg_seed_image_baseline():  ld-linux-x86-64.so.2, libc.so.6,
#                               libm.so.6, libnss_files.so.2
#   libc-dev.recipe:            glibc's headers and CRT objects
#
# That surface is also what made this session's three contamination
# defects possible at all -- gawk shipping Debian's libreadline, python
# shipping eleven Debian libraries, libc-dev copying a whole foreign
# /usr/include. Each one "worked" only because the baseline made Debian
# libraries ambiently present in every image. Remove the foreign
# baseline and the class stops existing.
#
# 2.36 matches what the build hosts have carried until now, so the
# cut-over changes provenance without changing ABI. Moving version is a
# separate decision from moving provenance, and doing both at once would
# make a failure impossible to attribute.
#
pkg_name="glibc"
pkg_version="2.36-1"
# mirrors.kernel.org, not ftp.gnu.org: every TLS handshake to
# ftp.gnu.org fails from this network (issue #167), reproduced from two
# hosts over both IPv4 and IPv6.
pkg_source="https://mirrors.kernel.org/gnu/libc/glibc-2.36.tar.xz"
pkg_sha256="1c959fea240906226062cb4b1e7ebce71a9f0e3c0836c09e7e3423d434fcfe75"
pkg_depends=""
# ADR-0211 Tier 3: glibc is built with gcc, and not as a judgement call
# -- glibc's own configure refuses anything else, and its source uses
# GCC-specific attributes, inline asm and symbol-versioning throughout.
# The compiler is Cix's own gcc 16.2.0-11 (gcc.recipe's three-stage
# bootstrap output), never an ambient host compiler, so the Build
# Provenance Mandate holds.
#
# linux-headers, not the host's: glibc compiles against kernel UAPI
# headers, and taking them from the build container would reintroduce
# exactly the foreign-header problem this recipe exists to end.
#
# No tcc here. One compiler per build environment -- two is how a real
# GCC was once silently picked over TCC on openssh.recipe, producing an
# sshd whose configure reported PAM support and whose ELF carried no
# libpam at all.
pkg_build_depends="bash coreutils make gcc binutils linux-headers sed grep gawk bison python findutils m4 diffutils"

pkg_build() {
	# glibc REFUSES to build in its source directory -- this is not a
	# style preference, configure exits with an error.
	mkdir -p /build/glibc-build
	cd /build/glibc-build

	# --enable-kernel=5.15: the oldest kernel this glibc will support.
	# Cix boots 6.18, so anything at or below that is safe; naming a
	# floor rather than "current" keeps the result usable if an older
	# kernel is ever booted for recovery.
	#
	# --with-headers: the UAPI headers from linux-headers, NOT the build
	# container's own. This is the line that makes the result Cix's.
	#
	# --disable-werror: glibc 2.36 against a much newer GCC 16 raises
	# warnings upstream never saw; they are not defects in the C
	# library, and failing on them would block the build for no gain.
	CC=/usr/bin/gcc CXX=/usr/bin/gcc /build/src/configure \
		--prefix=/usr \
		--with-headers=/usr/include \
		--enable-kernel=5.15 \
		--disable-werror \
		--disable-profile \
		libc_cv_slibdir=/lib/x86_64-linux-gnu

	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	# Everything the baseline and libc-dev used to take from the host is
	# now produced here. Asserted rather than assumed: a glibc that
	# built cleanly but produced no loader would replace every image's C
	# runtime with nothing, and the failure would surface as containers
	# that cannot execve anything at all -- far from this recipe.
	for f in lib/x86_64-linux-gnu/libc.so.6 \
	         lib/x86_64-linux-gnu/libm.so.6 \
	         lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 \
	         usr/include/stdio.h \
	         usr/lib/x86_64-linux-gnu/crt1.o; do
		test -e "$PKG_DESTDIR/$f" || {
			echo "glibc: expected output missing: $f" >&2
			exit 1
		}
	done

	# The documentation and locale data are large and nothing in this
	# project reads them; the C locale is built in.
	rm -rf "$PKG_DESTDIR/usr/share/locale" "$PKG_DESTDIR/usr/share/i18n" \
	       "$PKG_DESTDIR/usr/share/info" "$PKG_DESTDIR/usr/share/man"
}
