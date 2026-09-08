#
# probe-wifi-driver 2 -- what firmware does rtw88 need for the 8822BU,
# and exactly which Kconfig symbols does RTW88_8822BU pull in? (#30)
#
# Revision 1 answered the question it was written for, conclusively:
#
#     drivers/net/wireless/realtek/rtw88/rtw8822bu.c:56
#         { USB_DEVICE_AND_INTERFACE_INFO(0x2357, 0x012e, 0xff, 0xff, 0xff),
#
# So the site's adapter is an RTL8822BU driven by rtw88, not one of the
# 8811AU/8812AU parts that vendor id is also used for -- which is
# exactly why this was measured instead of recalled.
#
# It left two things unanswered, both of which cost a 65-minute kernel
# build if guessed wrong:
#
# 1. FIRMWARE. rtw88 calls request_firmware(), and revision 1's own
#    listing was sorted alphabetically and cut at thirty entries, so it
#    showed rtlwifi/* and never reached rtw88/*. The exact filename
#    matters more than usual here: a firmware blob cannot be built from
#    source, so it collides head-on with the Build Provenance Mandate
#    and the owner has to decide whether it may ship at all. "Probably
#    rtw8822b_fw.bin" is not good enough to put that decision on.
#
# 2. THE SYMBOL CHAIN. RTW88_8822BU is the visible symbol, but the
#    tristates it selects (RTW88_CORE, RTW88_USB, RTW88_8822B) have to
#    survive olddefconfig too, and the kernel recipe's own gate asserts
#    every requested symbol is still =y afterwards. Reading the select
#    lines is how the fragment names all of them rather than finding
#    out from a failed gate an hour later.
#
# Fails on purpose at the end, like every probe here.
#
pkg_name="probe-wifi-driver"
pkg_version="2"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz"
pkg_sha256="8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03"
pkg_build_image="kernel-builder"
pkg_build_depends="bash coreutils findutils grep gawk sed tar xz"
pkg_changelog="2: the two things revision 1 left open -- rtw88's own firmware filenames (alphabetical truncation hid them) and the exact select chain under RTW88_8822BU. 1: found which driver claims USB 2357:012e -- rtw88's rtw8822bu, an RTL8822BU."

pkg_build() {
	echo "=== every firmware file rtw88 can ask for ==="
	grep -rhoE '"rtw88/[a-zA-Z0-9_.-]+"' drivers/net/wireless/realtek/rtw88/ 2>/dev/null |
		sort -u | sed 's/^/  FIRMWARE /' || echo "  (none matched)"

	echo "=== how 8822b builds its firmware name (it may be composed, not literal) ==="
	grep -rnE 'fw_name|FIRMWARE|firmware' drivers/net/wireless/realtek/rtw88/rtw8822b.c 2>/dev/null |
		head -20 | sed 's/^/  /'
	echo "--- and the shared loader:"
	grep -rnE '"rtw88/%s|fw_name|MODULE_FIRMWARE' drivers/net/wireless/realtek/rtw88/main.c \
		drivers/net/wireless/realtek/rtw88/fw.c 2>/dev/null | head -15 | sed 's/^/  /'

	echo "=== MODULE_FIRMWARE declarations (the authoritative list per module) ==="
	grep -rn 'MODULE_FIRMWARE' drivers/net/wireless/realtek/rtw88/ 2>/dev/null | sed 's/^/  /'

	echo "=== what RTW88_8822BU selects, verbatim ==="
	awk '/^config RTW88_8822BU/,/^$/' drivers/net/wireless/realtek/rtw88/Kconfig | sed 's/^/  /'
	echo "--- and what those in turn select:"
	for sym in RTW88_8822B RTW88_USB RTW88_CORE RTW88; do
		echo "  --- config $sym"
		awk -v s="^config $sym\$" '$0 ~ s,/^$/' drivers/net/wireless/realtek/rtw88/Kconfig |
			sed 's/^/    /'
	done

	echo "=== does rtw88 need anything from MAC80211 beyond the obvious? ==="
	grep -nE 'depends on|select' drivers/net/wireless/realtek/rtw88/Kconfig | sed 's/^/  /'

	echo "WIFI-DRIVER RESULT: firmware and symbol chain reported above"
	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
