#
# probe-wifi-driver 3 -- what did enabling cfg80211 drag in, and does
# this platform need a regulatory database? (#30)
#
# kernel 7.2.3-5 got its config exactly right -- the gate confirmed all
# ten wireless symbols =y, RTW88_8822BU included -- and then died in a
# HOST tool:
#
#     CC      certs/system_keyring.o
#     HOSTCC  certs/extract-cert
#     certs/extract-cert.c:21:10: fatal error: openssl/bio.h: No such
#                                 file or directory
#
# certs/ was never built before this revision, so something in the
# wireless stack turned it on. The likely candidate is cfg80211's
# regulatory-database signature checking selecting
# SYSTEM_DATA_VERIFICATION -- but that is an INFERENCE, and the two
# possible fixes point in opposite directions:
#
#   - if it really is the regdb signature check, disabling it removes
#     certs/ entirely and costs nothing this platform uses;
#   - if certs/ came from somewhere else, disabling it fixes nothing
#     and the answer is to give the kernel build openssl headers.
#
# A wrong guess here is a 65-minute rebuild, so this reads the Kconfig
# instead of assuming.
#
# The second question is not incidental. A wireless AP is regulated
# hardware: without regulatory data the kernel falls back to the most
# restrictive world domain, which limits channels and transmit power
# and can stop an AP coming up on a channel that is perfectly legal
# here. Modern cfg80211 loads regulatory.db from /lib/firmware -- which
# would be a SECOND firmware blob, on the same Build Provenance footing
# as rtw8822b_fw.bin and worth knowing about now rather than after the
# radio refuses to start.
#
pkg_name="probe-wifi-driver"
pkg_version="3"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz"
pkg_sha256="8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03"
pkg_build_image="kernel-builder"
pkg_build_depends="bash coreutils findutils grep gawk sed tar xz"
pkg_changelog="3: why enabling cfg80211 pulled in certs/ (which broke the 7.2.3-5 build in a host tool), and whether a regulatory.db blob is needed. 2: rtw88 firmware filenames and the 8822BU select chain. 1: found rtw8822bu.c carries USB 2357:012e."

pkg_build() {
	echo "=== cfg80211 Kconfig, verbatim ==="
	sed -n '1,120p' net/wireless/Kconfig | sed 's/^/  /'

	echo "=== who selects SYSTEM_DATA_VERIFICATION anywhere in the tree ==="
	grep -rn 'select SYSTEM_DATA_VERIFICATION' --include=Kconfig . 2>/dev/null | sed 's/^/  /'

	echo "=== what SYSTEM_DATA_VERIFICATION itself pulls in ==="
	awk '/^config SYSTEM_DATA_VERIFICATION/,/^$/' certs/Kconfig lib/Kconfig 2>/dev/null | sed 's/^/  /'

	echo "=== what builds certs/extract-cert (the thing that failed) ==="
	grep -nE 'extract-cert|hostprogs|obj-' certs/Makefile 2>/dev/null | sed 's/^/  /'
	echo "--- and what gates certs/ from the top-level Makefile:"
	grep -nE 'certs' Makefile 2>/dev/null | head -10 | sed 's/^/  /'

	echo "=== regulatory database: does cfg80211 load one, and from where ==="
	grep -rnE '"regulatory\.db|regulatory\.db\.p7s|request_firmware' net/wireless/reg.c 2>/dev/null |
		head -12 | sed 's/^/  /'
	echo "--- regdb-related config symbols:"
	grep -nE 'REGDB|CRDA|REGULATORY' net/wireless/Kconfig 2>/dev/null | sed 's/^/  /'

	echo "WIFI-DRIVER RESULT: certs/ cause and regdb requirement reported above"
	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
