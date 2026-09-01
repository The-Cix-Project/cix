#
# iproute2 -- ip/ss/tc and the rest of the standard Linux networking
# toolset.
#
# Source verified against kernel.org's own published sha256sums.asc for
# this exact release.
#
# WHAT CHANGED IN -2:
#
# 6.18.0 copied FOUR of the build host's Debian libraries into the
# package -- libelf-0.188, libmnl, libcap and libz. All four now exist
# as real Cix packages (elfutils, libmnl, libcap, zlib), so they are
# declared instead of copied. The version numbers in the old copy list
# name Debian's builds outright (libelf-0.188.so, libz.so.1.2.13,
# libcap.so.2.66); Cix's own are 0.192, 1.3.2 and 2.78.
#
# The more important change is the assertion at the end. iproute2's
# ./configure is a plain shell script that PROBES for each optional
# dependency and degrades silently when one is missing -- it does not
# fail, it just writes a smaller Config and builds a reduced `ip`. That
# is exactly the #113 shape, and it is how this package could appear to
# build perfectly while quietly losing BPF program loading or netlink
# support. So the recipe now reads configure's own Config file back and
# fails if a feature it declared a dependency for did not actually turn
# on.
#
pkg_name="iproute2"
pkg_version="6.18.0-3"
pkg_source="https://www.kernel.org/pub/linux/utils/net/iproute2/iproute2-6.18.0.tar.xz"
pkg_sha256="6ba520e1975e4c50dc931eeae91ea37c198b8a173744885f8895b84325f9d456"
pkg_depends="libmnl elfutils libcap zlib"
#
# zlib is here because libelf's compressed-debug-info support pulls it
# in transitively, which the 6.18.0 copy list had already discovered the
# hard way.
#
# Deliberately NOT declared, though both are now available: iptables
# (xtables) and ipset. configure probes for them too and would enable
# tc's xtables action and ipset matching -- real features 6.18.0 never
# had. Adding them is a capability change that should be its own
# decision with its own verification, not a side effect of a
# provenance fix. Under a composed build environment (ADR-0199) the
# sandbox contains only what is declared here, so leaving them out is
# deterministic rather than accidental.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf libmnl elfutils libcap zlib"
pkg_changelog="6.18.0-3: reads config.mk, which is what configure actually writes (the -2 assertion looked for a file named Config and died on cat). Needs elfutils@0.192-11, the first revision to ship libelf.pc. 6.18.0-2: declares libmnl/elfutils/libcap/zlib instead of copying the build host's Debian copies into the package, and asserts configure actually enabled each of them rather than silently degrading (#206, #207, #113)"

pkg_build() {
	export PKG_CONFIG_PATH="/usr/lib/pkgconfig:/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig"

	echo "=== libraries visible to pkg-config ==="
	for m in libmnl libelf zlib libcap; do
		if pkg-config --exists "$m" 2>/dev/null; then
			echo "  present  $m $(pkg-config --modversion "$m")"
		else
			echo "  ABSENT   $m"
		fi
	done

	CC=tcc ./configure

	#
	# configure records what it found in Config. Read it back rather
	# than trusting that configure exited 0 -- it exits 0 either way.
	#
	# configure writes config.mk (CONFIG=config.mk in its own source),
	# not "Config" -- the -2 revision asserted against the wrong
	# filename and died on `cat` after an otherwise fine configure.
	echo "=== config.mk as configure wrote it ==="
	cat config.mk

	for feat in HAVE_MNL HAVE_ELF HAVE_CAP; do
		grep -q "^$feat:=y" config.mk || {
			echo "$feat did not get enabled -- configure degraded silently" >&2
			exit 1
		}
	done

	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	# No library copying: all four arrive via pkg_depends.

	echo "=== ip DT_NEEDED ==="
	readelf -d "$PKG_DESTDIR/usr/sbin/ip" | grep NEEDED

	# The binaries that justify the package existing.
	for b in ip tc ss; do
		test -x "$PKG_DESTDIR/usr/sbin/$b" || test -x "$PKG_DESTDIR/usr/bin/$b" || {
			echo "missing binary $b" >&2
			exit 1
		}
	done

	# ip must really be dynamically linked against the netlink and ELF
	# libraries this recipe declared, not have quietly dropped them.
	readelf -d "$PKG_DESTDIR/usr/sbin/ip" | grep -q 'NEEDED.*libmnl\.so\.0' || {
		echo "ip is not linked against libmnl.so.0" >&2
		exit 1
	}
	readelf -d "$PKG_DESTDIR/usr/sbin/ip" | grep -q 'NEEDED.*libelf\.so\.1' || {
		echo "ip is not linked against libelf.so.1" >&2
		exit 1
	}
}
