#
# probe-wifi-driver 4 -- run the real config procedure and READ THE
# ANSWER, instead of reasoning about Kconfig a third time (#30).
#
# Two kernel builds have now died in the same place:
#
#     certs/extract-cert.c:21:10: fatal error: openssl/bio.h
#
# Revision 3 found that net/wireless/Kconfig:92 selects
# SYSTEM_DATA_VERIFICATION under CFG80211_REQUIRE_SIGNED_REGDB, and
# 7.2.3-6 turned that option off. certs/ was built anyway. So the
# finding was true and incomplete: revision 3's own output listed SEVEN
# `select SYSTEM_DATA_VERIFICATION` sites and only one of them was
# checked. Reasoning about the other six would be the same mistake a
# third time.
#
# This runs the exact procedure the kernel recipe runs -- allnoconfig,
# merge_config -m with this platform's own fragment, olddefconfig --
# and then prints what the resulting .config actually says. No
# compilation: the config step is the whole question, and it takes
# a couple of minutes rather than the ten each failed build has cost.
#
# What it answers, in order of what would change the fix:
#
#   1. Did `# CONFIG_CFG80211_REQUIRE_SIGNED_REGDB is not set` survive
#      olddefconfig, or did something select it back on? If it came
#      back, the fix is to find what selects it. If it stayed off, the
#      fix is elsewhere entirely and 7.2.3-6's change was correct but
#      insufficient.
#   2. What is SYSTEM_DATA_VERIFICATION set to, and which of the seven
#      selecting symbols are enabled in this config?
#   3. Which symbol actually causes certs/ to be built -- certs/Makefile
#      keys off specific ones, and that is the thing to turn off.
#
pkg_name="probe-wifi-driver"
pkg_version="4"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/raw/image/kernel/qemu-part1.config?ref=24b49625a5424e9fc87cda6aab6ae7ef7a7e315f"
pkg_sha256="8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03 0b95da7f2d53e9eceac6763a3b41702c6286aad94b5f8a14f6e0864a6014e5ee"
pkg_build_image="kernel-builder"
pkg_build_depends="bash bc binutils bison coreutils diffutils findutils flex gawk gcc grep make sed tar xz"
pkg_changelog="4: runs the real allnoconfig+merge+olddefconfig and prints what the .config actually says about the certs/ chain, after two builds died the same way and reasoning about Kconfig twice produced a true-but-incomplete cause."

pkg_build() {
	echo "=== generate the config exactly as the kernel recipe does ==="
	cp /build/extra/qemu-part1.config .config
	make ARCH=x86_64 SHELL=/usr/bin/bash HOSTCC=/usr/bin/gcc CC=/usr/bin/gcc allnoconfig >/dev/null 2>&1
	bash ./scripts/kconfig/merge_config.sh -m .config /build/extra/qemu-part1.config >/dev/null 2>&1
	make ARCH=x86_64 SHELL=/usr/bin/bash HOSTCC=/usr/bin/gcc CC=/usr/bin/gcc olddefconfig >/dev/null 2>&1
	[ -f .config ] || { echo "no .config produced"; exit 1; }

	echo "=== 1. did the regdb disable survive? ==="
	grep -E 'CFG80211_REQUIRE_SIGNED_REGDB' .config | sed 's/^/  CONFIG /' ||
		echo "  CONFIG (absent from .config entirely)"

	echo "=== 2. the certs chain, as resolved ==="
	for sym in SYSTEM_DATA_VERIFICATION SYSTEM_TRUSTED_KEYRING MODULE_SIG \
	           MODULE_SIG_ALL SECONDARY_TRUSTED_KEYRING SYSTEM_BLACKLIST_KEYRING \
	           INTEGRITY_SIGNATURE FS_VERITY DM_VERITY UBIFS_FS SECURITY_IPE; do
		v=$(grep -E "^(CONFIG_$sym=|# CONFIG_$sym is not set)" .config || true)
		echo "  ${v:-CONFIG_$sym: absent}"
	done

	echo "=== 3. which of the seven SYSTEM_DATA_VERIFICATION selectors are ON ==="
	grep -rn 'select SYSTEM_DATA_VERIFICATION' --include=Kconfig . 2>/dev/null |
		while IFS= read -r line; do
			f=${line%%:*}
			echo "  --- $line"
			# name the config symbol this select belongs to, then say
			# whether that symbol is enabled here
			ln=$(echo "$line" | cut -d: -f2)
			sym=$(awk -v n="$ln" 'NR<=n && /^config /{s=$2} END{print s}' "$f")
			[ -n "$sym" ] && {
				v=$(grep -E "^(CONFIG_$sym=|# CONFIG_$sym is not set)" .config || true)
				echo "      owner: CONFIG_$sym -> ${v:-absent}"
			}
		done

	echo "=== 4. what makes certs/ get built ==="
	grep -nE 'obj-|hostprogs|extract-cert' certs/Makefile | sed 's/^/  /'
	echo "--- top-level Makefile references to certs:"
	grep -nE 'certs' Makefile | head -10 | sed 's/^/  /'

	echo "WIFI-DRIVER RESULT: config values reported above"
	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
