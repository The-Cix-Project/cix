#
# probe-wifi-driver 5 -- is openssl/bio.h actually in the kernel-builder
# build sandbox, and if so where? (#30)
#
# The story so far, each step measured:
#
#   - enabling wireless forces CFG80211_REQUIRE_SIGNED_REGDB on (its
#     prompt is conditional, so it cannot be turned off from .config);
#   - that selects SYSTEM_DATA_VERIFICATION, which builds
#     certs/extract-cert, a HOST tool including <openssl/bio.h>;
#   - four kernel builds died there.
#
# The last theory was that kernel-builder simply lacked openssl -- its
# manifest listed 22 packages and openssl was not among them. But
# installing openssl into that image answered
# `409 package is already installed`, and PkgInstallRequest's own
# schema says packages are tracked independently per image, so it IS
# installed there. Which means the header is present somewhere and the
# kernel's HOSTCC is not finding it, or the sandbox composed for a
# build does not contain what the package table says is installed.
#
# Those two have completely different fixes -- a CPPFLAGS/HOSTCFLAGS
# path versus an image composition problem -- so this looks instead of
# reasoning. It is a two-minute build in the same image the kernel uses,
# and it answers the question directly.
#
pkg_name="probe-wifi-driver"
pkg_version="5"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz"
pkg_sha256="8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03"
pkg_build_image="kernel-builder"
pkg_build_depends="bash coreutils findutils grep"
pkg_changelog="5: look inside the kernel-builder build sandbox for openssl/bio.h, after four kernel builds failed on it and the last two theories about why were both wrong."

pkg_build() {
	echo "=== is the header anywhere at all? ==="
	if find / -xdev -name 'bio.h' -path '*openssl*' 2>/dev/null | head -20 | grep .; then
		:
	else
		echo "  NOT PRESENT ANYWHERE -- openssl's headers are not in this sandbox"
	fi

	echo "=== the usual places, checked explicitly ==="
	for p in /usr/include/openssl/bio.h /usr/local/include/openssl/bio.h \
	         /include/openssl/bio.h /usr/include/x86_64-linux-gnu/openssl/bio.h; do
		if [ -f "$p" ]; then echo "  FOUND $p"; else echo "  absent $p"; fi
	done

	echo "=== what openssl DID leave behind (so we can tell installed-but-pruned from absent) ==="
	find / -xdev \( -name 'libssl.so*' -o -name 'libcrypto.so*' -o -name 'openssl' \) \
		2>/dev/null | head -20 | sed 's/^/  /'
	echo "--- any openssl pkg-config?"
	find / -xdev -name 'openssl.pc' -o -xdev -name 'libcrypto.pc' 2>/dev/null | head -5 | sed 's/^/  /'

	echo "=== what /usr/include holds at all (is the sandbox header-less in general?) ==="
	ls /usr/include 2>/dev/null | head -30 | sed 's/^/  /'
	echo "  ... $(ls /usr/include 2>/dev/null | wc -l) entries total"

	echo "=== HOSTCC's own default search path ==="
	echo | /usr/bin/gcc -E -Wp,-v - 2>&1 | sed -n '/search starts here/,/End of search/p' | sed 's/^/  /'

	echo "WIFI-DRIVER RESULT: openssl header presence reported above"
	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
