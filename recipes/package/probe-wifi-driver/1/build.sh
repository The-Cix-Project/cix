#
# probe-wifi-driver 1 -- which mainline driver claims the USB WiFi
# adapter on this site, in THIS kernel version? (#30, ADR-0048)
#
# 192.168.15.95 has a USB 802.11ac adapter that GET /v1/devices reports
# as:
#
#     usb:2357:012e   "Realtek 802.11ac NIC"   driver: usb
#
# `driver: usb` is the finding: the generic USB core claimed it because
# nothing else did. image/kernel/qemu-part1.config contains zero
# wireless symbols -- no CFG80211, no MAC80211, no WLAN, no Realtek
# driver at all -- so this kernel cannot drive any wireless hardware.
#
# Enabling wireless is a kernel rebuild, which is expensive (~50
# minutes) and worth getting right in one attempt. Realtek's USB parts
# are split across rtl8xxxu, rtw88 and drivers that only ever existed
# out of tree, and which one carries 2357:012e in 7.2 is a FACT about
# this source tree rather than something to recall. So: ask the source.
#
# The build here is a grep. It fetches the same kernel tarball the
# kernel recipe pins, byte for byte -- same URL, same checksum -- and
# searches every USB device-ID table under drivers/net/wireless for
# this vendor and product. It also lists what Realtek USB drivers the
# tree actually has and the Kconfig symbol each one needs, so the
# follow-up kernel revision can name symbols that exist instead of
# symbols that sound right.
#
# Fails on purpose at the end, like every probe here -- the log is the
# product. The lines to read are prefixed WIFI-DRIVER.
#
pkg_name="probe-wifi-driver"
pkg_version="1"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz"
pkg_sha256="8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03"
pkg_build_image="kernel-builder"
pkg_build_depends="bash coreutils findutils grep gawk sed tar xz"
pkg_changelog="1: find which driver in linux-7.2.3 claims USB 2357:012e, and what Kconfig symbols it needs, before spending a kernel rebuild on a guess."

VID="2357"
PID="012e"

pkg_build() {
	echo "=== tree ==="
	pwd
	ls -d drivers/net/wireless 2>/dev/null || {
		echo "WIFI-DRIVER RESULT: no drivers/net/wireless -- wrong source root"
		ls
		exit 1
	}

	# USB_DEVICE(0x2357, 0x012e) is the usual spelling, but drivers also
	# write the pair with other macros and spacing, so match the two
	# hex values near each other rather than one exact form.
	echo "=== exact id search: ${VID}:${PID} ==="
	if grep -rniE "0x${VID}[^0-9a-fA-F].{0,40}0x${PID}" drivers/net/wireless/ 2>/dev/null; then
		:
	else
		echo "(no direct hit for 0x${VID} .. 0x${PID} under drivers/net/wireless)"
	fi

	echo "=== every driver mentioning vendor 0x${VID} at all ==="
	grep -rlniE "0x${VID}" drivers/net/wireless/ 2>/dev/null | sed 's/^/  /' || echo "  (none)"

	echo "=== and anywhere else in the tree (a non-wireless subsystem may own it) ==="
	grep -rlniE "0x${VID}[^0-9a-fA-F].{0,40}0x${PID}" drivers/ 2>/dev/null | sed 's/^/  /' || echo "  (none)"

	echo "=== Realtek wireless drivers present in this tree ==="
	ls drivers/net/wireless/realtek/ 2>/dev/null | sed 's/^/  /' || echo "  (no realtek directory)"

	echo "=== their Kconfig symbols (what a config fragment would name) ==="
	find drivers/net/wireless/realtek -name Kconfig 2>/dev/null |
		while read -r f; do
			echo "  --- $f"
			grep -E "^\s*(config|tristate|bool)" "$f" | sed 's/^/    /'
		done

	echo "=== USB-capable Realtek drivers (which ones build a USB module at all) ==="
	find drivers/net/wireless/realtek -name 'Makefile' 2>/dev/null |
		xargs grep -lin 'usb' 2>/dev/null | sed 's/^/  /' || echo "  (none)"

	echo "=== does any of them request firmware? (Build Provenance question) ==="
	grep -rhoE 'request_firmware[^;]{0,120}' drivers/net/wireless/realtek/ 2>/dev/null |
		head -20 | sed 's/^/  /' || echo "  (none found)"
	echo "--- firmware paths named in Realtek drivers:"
	grep -rhoE '"(rtw8[89]|rtlwifi|rtl_?[0-9a-z]+)/[a-zA-Z0-9_./-]+"' drivers/net/wireless/realtek/ 2>/dev/null |
		sort -u | head -30 | sed 's/^/    /' || echo "    (none)"

	echo "WIFI-DRIVER RESULT: search complete -- read the sections above"
	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
