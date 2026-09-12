#
# probe-nic-modules -- what the installer would have to stage to see a
# real NIC (#429 follow-on).
#
# The installer ISO carries no module tree, so on real hardware only
# built-in drivers produce an interface -- measured on a bare-metal
# attempt, 2026-09-12, where a machine's built-in Ethernet was absent
# from the list entirely. Staging the five NIC modules is the fix, and
# the question this answers is WHICH FILES: r8169 needs a PHY driver,
# igb and ixgbe pull in helpers, and guessing wrong means staging a
# module that cannot load, which presents exactly like staging nothing.
#
# So this reads the kernel's own modules.dep rather than reasoning
# about it. pkg_build_depends="kernel" puts lib/modules in the build
# sandbox, which is the only place this is answerable without a shell
# on a host.
#
# Prints, for each of the five: its dep line, and the size of every
# file in its closure. Also the whole tree's size, for comparison --
# the deciding number for "stage the lot" versus "prune".
#
pkg_name="probe-nic-modules"
pkg_version="1"
pkg_source="https://mirrors.kernel.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db20"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils kernel"
pkg_depends=""
pkg_changelog="1: reads the kernel's own modules.dep to establish exactly which module files the installer must stage to make a real NIC visible, instead of guessing the dependency closure."

pkg_build() {
	set +e
	MODROOT=""
	for c in /lib/modules /usr/lib/modules; do
		if [ -d "$c" ]; then MODROOT="$c"; fi
	done
	echo "=== module roots present ==="
	ls -d /lib/modules /usr/lib/modules 2>/dev/null || echo "  none"
	if [ -z "$MODROOT" ]; then
		echo "PROBE RESULT: no module tree in the build sandbox -- pkg_build_depends did not"
		echo "supply one, so this question has to be answered another way."
		return 0
	fi
	KVER="$(ls "$MODROOT" | head -1)"
	echo "=== kernel version dir: $KVER ==="
	DEP="$MODROOT/$KVER/modules.dep"
	echo "=== modules.dep present? ==="
	ls -l "$DEP" 2>/dev/null || echo "  ABSENT -- modprobe could not resolve anything"
	echo
	echo "=== whole tree size ==="
	du -sh "$MODROOT/$KVER" 2>/dev/null
	echo
	for m in e1000e igb ixgbe r8169 tg3; do
		echo "=== $m ==="
		line="$(grep -E "/$m\.ko(\.[a-z]+)?:" "$DEP" 2>/dev/null | head -1)"
		if [ -z "$line" ]; then
			echo "  not in modules.dep -- not built, or named differently"
			continue
		fi
		echo "  dep line: $line"
		self="$(echo "$line" | cut -d: -f1)"
		deps="$(echo "$line" | cut -d: -f2-)"
		total=0
		for f in $self $deps; do
			sz="$(stat -c%s "$MODROOT/$KVER/$f" 2>/dev/null || echo 0)"
			echo "    $sz  $f"
			total=$((total + sz))
		done
		echo "  closure bytes: $total"
	done
	echo
	echo "=== the metadata modprobe needs beside the .ko files ==="
	for f in modules.dep modules.dep.bin modules.alias modules.alias.bin modules.symbols \
	         modules.symbols.bin modules.builtin modules.builtin.bin modules.order; do
		sz="$(stat -c%s "$MODROOT/$KVER/$f" 2>/dev/null || echo "-")"
		echo "  $sz  $f"
	done
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-nic-modules"
	echo "see the build log" > "$PKG_DESTDIR/usr/share/probe-nic-modules/README"
}
