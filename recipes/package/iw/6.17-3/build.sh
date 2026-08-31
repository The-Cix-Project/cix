#
# iw -- the nl80211-based CLI for configuring wireless devices (join a
# network, set a card to AP/monitor/managed mode, inspect link/station
# state) -- the modern replacement for the old ioctl-based
# wireless-tools (iwconfig etc., which this project doesn't package).
#
# Source is upstream's own canonical release location, kernel.org's
# software archive. Checksum computed directly from the downloaded
# bytes, not taken from any third party.
#
# -2 is a withdrawn revision: it was registered during the #206 sweep,
# failed for want of pkg-config and a libnl package, and was deleted
# locally in the same commit that created it. It stays published on the
# box (recipe versions are immutable, ADR-0107), so this is -3.
#
# WHAT CHANGED SINCE 6.17, and why it matters more than a version bump:
#
# 6.17 did not build iw's netlink dependency -- it COPIED the build
# host's Debian libnl into the package:
#
#     cp -a /lib/x86_64-linux-gnu/libnl-3.so.200 ... "$PKG_DESTDIR/..."
#
# That shipped a foreign binary inside a Cix package, which the Build
# Provenance Mandate forbids outright, and it was invisible: the
# artifact checksum verified perfectly and iw ran. It was defensible
# when written only because Cix had no libnl of its own; now it does
# (libnl@3.11.0-4), so the copy is replaced by a real dependency.
#
# Declaring pkg_depends rather than copying files is the correct shape
# regardless of provenance: the image resolves one shared libnl, instead
# of each consumer carrying its own private copy that no upgrade or
# audit can ever see.
#
pkg_name="iw"
pkg_version="6.17-3"
pkg_source="https://www.kernel.org/pub/software/network/iw/iw-6.17.tar.xz"
pkg_sha256="7d182e498289ab39b257da6780d562e415377107f50358ee5b55b8cfe40b1e33"
pkg_depends="libnl"
#
# Hand-rolled Makefile, no configure step, but it shells out to
# pkg-config for libnl-3.0/libnl-genl-3.0 -- so pkgconf is a real build
# tool here, not an optional nicety. Without it the 6.17 build failed
# with `pkg-config: command not found` four times over, then
# `Makefile:78: *** Cannot find development files for any supported
# version of libnl` -- a message that reads like libnl is absent when
# the actual missing tool is pkg-config.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf libnl"
pkg_changelog="6.17-3: builds against Cix's own libnl@3.11.0-4 and declares it as a dependency, instead of copying the build host's Debian libnl into the package (#206, #207)"

pkg_build() {
	#
	# Cix packages do not agree on where a .pc file lands, so a
	# consumer cannot assume its dependency picked the same convention
	# it did. libnl ships to /usr/lib/pkgconfig; others use the
	# multiarch path. Setting this explicitly is what ipset.recipe
	# needed for libmnl, and for the same reason: a genuinely installed
	# library still reports "not found" without it.
	#
	export PKG_CONFIG_PATH="/usr/lib/pkgconfig:/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig"

	# Fail here, with a clear message, rather than 60 lines later
	# inside a generated Makefile rule that blames libnl for a missing
	# pkg-config.
	pkg-config --exists libnl-3.0 || {
		echo "libnl-3.0 not visible to pkg-config" >&2
		exit 1
	}
	pkg-config --exists libnl-genl-3.0 || {
		echo "libnl-genl-3.0 not visible to pkg-config" >&2
		exit 1
	}
	echo "building against libnl $(pkg-config --modversion libnl-3.0)"

	make -j"$(nproc)" CC=tcc PREFIX=/usr
}

pkg_install() {
	dir="$PKG_DESTDIR/usr/sbin"
	mkdir -p "$dir"
	cp iw "$dir/"

	#
	# No library copying: libnl arrives via pkg_depends. Assert that
	# what shipped really is dynamically linked against it -- a build
	# that quietly went static, or resolved against something else,
	# would still exit 0 here (that exact class of silent degradation
	# is #113).
	#
	readelf -d "$dir/iw" | grep -q 'NEEDED.*libnl-3\.so\.200' || {
		echo "iw is not linked against libnl-3.so.200" >&2
		exit 1
	}
	readelf -d "$dir/iw" | grep -q 'NEEDED.*libnl-genl-3\.so\.200' || {
		echo "iw is not linked against libnl-genl-3.so.200" >&2
		exit 1
	}
}
