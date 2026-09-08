#
# probe-wifi-driver 6 -- installed-but-pruned, or never composed in?
# (#30)
#
# Revision 5 answered half the question and then killed itself: it
# established that openssl/bio.h is NOWHERE in the kernel-builder
# sandbox, and then died on `sed: command not found` -- a tool it used
# for indentation and never declared. The sections that would have said
# WHY were the ones lost.
#
# That distinction is the whole remaining question, and the two answers
# need opposite fixes:
#
#   - if libssl.so/libcrypto.so ARE present and only the headers are
#     missing, the package is installed and something pruned its
#     headers (ADR-0251's finalize phase drops what the platform does
#     not run, and headers are exactly that);
#   - if nothing of openssl is present at all, the manifest gained the
#     package but the rootfs never did -- an image whose new version
#     deduped to an old tree, which is the ADR-0155 hash-of-manifest
#     trap this project has already been caught by twice.
#
# The manifest now genuinely lists openssl:pinned:3.0.20-6 (23 entries,
# read back from GET /v1/images/kernel-builder) and a new image version
# was created at that moment -- yet revision 5 ran AFTER that and still
# found nothing. So this is not "the change has not landed yet".
#
# No sed, no awk, no tr. Only what pkg_build_depends declares.
#
pkg_name="probe-wifi-driver"
pkg_version="6"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz"
pkg_sha256="8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03"
pkg_build_image="kernel-builder"
pkg_build_depends="bash coreutils findutils grep"
pkg_changelog="6: finishes what 5 started -- it proved openssl/bio.h is absent from the kernel-builder sandbox and then died on an undeclared sed before saying whether anything else of openssl is there. Uses only declared tools."

pkg_build() {
	echo "=== openssl shared libraries (present => installed, headers pruned) ==="
	find / -xdev -name 'libssl.so*' 2>/dev/null | head -10
	find / -xdev -name 'libcrypto.so*' 2>/dev/null | head -10
	echo "--- (nothing above means openssl is not in this rootfs at all)"

	echo "=== the openssl binary ==="
	for p in /usr/bin/openssl /bin/openssl /usr/sbin/openssl; do
		if [ -x "$p" ]; then echo "  FOUND $p"; else echo "  absent $p"; fi
	done

	echo "=== any openssl pkg-config or cmake bits ==="
	find / -xdev -name 'libcrypto.pc' -o -xdev -name 'openssl.pc' 2>/dev/null | head -5

	echo "=== headers in general: is this sandbox header-bearing at all? ==="
	echo "  /usr/include entries: $(ls /usr/include 2>/dev/null | wc -l)"
	echo "  a few of them:"
	ls /usr/include 2>/dev/null | head -15
	echo "  zlib.h (zlib is in the manifest and the kernel needs it): $([ -f /usr/include/zlib.h ] && echo present || echo ABSENT)"
	echo "  elfutils libelf.h (also in the manifest): $([ -f /usr/include/libelf.h ] && echo present || echo ABSENT)"

	echo "WIFI-DRIVER RESULT: see above -- libs present means pruned headers, nothing means not composed"
	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
