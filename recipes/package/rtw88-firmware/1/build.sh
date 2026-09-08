#
# rtw88-firmware -- the one firmware blob the RTL8822BU access point
# adapter needs (#30).
#
# THIS PACKAGE IS AN EXPLICIT, OWNER-APPROVED EXCEPTION TO THE BUILD
# PROVENANCE MANDATE, AND IT SHOULD BE READ AS ONE.
#
# Everything else this platform ships is compiled on a Cix host by
# Cix's own toolchain. This is not compiled at all. It is Realtek's
# signed microcode for the adapter's own processor, distributed only as
# a binary, and nobody -- including Realtek's customers -- can build it
# from source. Without it the driver loads, the radio never comes up,
# and the failure looks like a hardware fault rather than a missing
# file.
#
# What makes it acceptable rather than a hole:
#
#   - It never executes on the host CPU. It is uploaded to the adapter
#     over USB and runs there. It is not linked into anything Cix
#     builds and cannot be, which is a materially different thing from
#     a foreign host binary in the artifact cache.
#   - It is pinned by content, twice. The URL names an immutable git
#     commit in linux-firmware, and pkg_sha256 approves those exact
#     150984 bytes. A mirror cannot substitute different bytes without
#     failing a gate that already exists.
#   - There is precedent, and it is the same shape: ADR-0029 already
#     stages amdgpu firmware into the control-plane root, for exactly
#     the same reason (request_firmware() runs on the host root at
#     driver-probe time, before any container exists).
#
# WHICH FILE, AND HOW THAT WAS ESTABLISHED. Not guessed:
#
#   drivers/net/wireless/realtek/rtw88/rtw8822b.c:2614
#       MODULE_FIRMWARE("rtw88/rtw8822b_fw.bin");
#
# measured against the same kernel tree this platform builds
# (recipes/package/probe-wifi-driver/2). Exactly one file -- the 8822C
# has a separate WoW blob, the 8822B does not, so anything staging a
# second file here is staging something this device never asks for.
#
# The driver itself was measured the same way: rtw8822bu.c:56 carries
# USB_DEVICE_AND_INTERFACE_INFO(0x2357, 0x012e), so the site's adapter
# is an RTL8822BU on rtw88 rather than one of the 8811AU/8812AU parts
# that vendor id also covers.
#
pkg_name="rtw88-firmware"
pkg_version="1"
pkg_source="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/rtw88/rtw8822b_fw.bin?id=338684a0c7760644031483311464c7cf5b3aac94"
pkg_sha256="378ff7b43ae7da18a0311175abc351a1758d25ce553b44f3c5694758efbea84c"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils"
pkg_changelog="1: rtw88/rtw8822b_fw.bin for the RTL8822BU access-point adapter, pinned to an immutable linux-firmware commit and approved by checksum. An owner-approved exception to the Build Provenance Mandate: device microcode that runs on the adapter, not the host, and that nobody can build from source."

pkg_build() {
	#
	# Nothing to compile. The fetch already happened and pkg_sha256
	# already approved these exact bytes; this only checks that what
	# arrived is the blob and not, say, a git.kernel.org error page
	# that happened to hash consistently.
	#
	# The header check is cheap and specific: rtw88 firmware begins
	# with its chip id in the first two bytes, little-endian, so an
	# 8822B image starts 0x22 0x88. A checksum proves the bytes are
	# the ones approved; this proves the ones approved are the right
	# KIND of thing, which is a different question and the one that
	# catches a wrong-file mistake at review time.
	#
	src=$(ls rtw8822b_fw.bin* 2>/dev/null | head -1)
	[ -n "$src" ] || src=$(ls | head -1)
	echo "fetched: $src ($(wc -c < "$src") bytes)"

	head -c 2 "$src" | od -An -tx1 | tr -d ' \n' | grep -qi '^2288$' || {
		echo "rtw88-firmware: not an 8822B firmware image -- header is not 22 88" >&2
		head -c 16 "$src" | od -An -tx1 >&2
		exit 1
	}
	echo "header ok: 8822B firmware image"
	cp "$src" rtw8822b_fw.bin 2>/dev/null || true
}

pkg_install() {
	#
	# /lib/firmware/rtw88/, which is where the kernel looks. Not
	# /usr/lib/firmware: the firmware search path is the kernel's own
	# constant, not a libdir this platform gets to choose (#184 governs
	# where LIBRARIES live, and this is not one).
	#
	dir="$PKG_DESTDIR/lib/firmware/rtw88"
	mkdir -p "$dir"
	cp rtw8822b_fw.bin "$dir/rtw8822b_fw.bin"
	chmod 0644 "$dir/rtw8822b_fw.bin"

	# Say what shipped, so the build log carries the evidence rather
	# than requiring a later archaeology pass to find out.
	echo "installed $(wc -c < "$dir/rtw8822b_fw.bin") bytes to /lib/firmware/rtw88/rtw8822b_fw.bin"
}
