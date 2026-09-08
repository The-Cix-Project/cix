#
# probe-wifi-driver 9 -- what does kernel-builder ACTUALLY contain?
# (#30, #338)
#
# Nothing has measured this yet, which is the whole problem. Revisions
# 5, 6 and 7 each set pkg_build_image="kernel-builder" and were started
# with POST /v1/pkg/install, so all three ran in a composed environment
# built from their own four declared tools and faithfully reported its
# contents. Revision 7 is the control that proves it: it declared
# cix-builder instead and produced byte-identical output.
#
# daemon/src/pkg.c decides in this order:
#
#   if (is_hostbuild)          -> lowerdir = build_image's rootfs
#   else if (cache_hit)        -> no build environment at all
#   else if (build_depends)    -> composed from the declared tool set
#
# So the sandbox a recipe gets depends on how the build was STARTED,
# not only on what it declares. The kernel is built with
# POST /v1/pkg/hostbuild and therefore runs in kernel-builder's own
# rootfs; the probes were started with POST /v1/pkg/install and never
# went near it.
#
# This revision is identical in shape and is started as a HOST BUILD,
# so it runs where the kernel runs. Two outcomes, opposite fixes:
#
#   - headers PRESENT: kernel-builder is fine and HOSTCC is not finding
#     them; the fix is a search path, and it is the kernel recipe that
#     changes.
#   - headers ABSENT: the image really is missing them despite its
#     manifest naming openssl, zlib and elfutils. Then the question is
#     why -- an ADR-0155 dedup repointing a new version at an old tree
#     is the candidate this project has been caught by twice -- and it
#     is the IMAGE that changes.
#
# gcc's own search path is printed too, because if the headers turn out
# to be present the very next question is where it looked. gcc is in
# kernel-builder's manifest, so it is there to ask.
#
pkg_name="probe-wifi-driver"
pkg_version="9"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz"
pkg_sha256="8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03"
pkg_build_image="kernel-builder"
pkg_build_depends="bash coreutils findutils grep"
pkg_changelog="9: the first probe to actually run inside kernel-builder. Started as a host build rather than an install, because pkg.c picks the build sandbox by how the build was started -- is_hostbuild uses pkg_build_image's rootfs, an ordinary install composes one from pkg_build_depends -- so revisions 5-7 measured a four-tool composed environment and never touched the image they named."

pkg_build() {
	echo "=== which sandbox is this, really ==="
	echo "  /usr/include entries: $(ls /usr/include 2>/dev/null | wc -l)"
	echo "  gcc present:  $([ -x /usr/bin/gcc ] && echo yes || echo no)"
	echo "  make present: $([ -x /usr/bin/make ] && echo yes || echo no)"
	echo "  (a four-tool composed env has neither; kernel-builder has both)"

	echo "=== the three headers whose absence #338 reported ==="
	echo "  openssl/bio.h: $([ -f /usr/include/openssl/bio.h ] && echo PRESENT || echo ABSENT)"
	echo "  zlib.h:        $([ -f /usr/include/zlib.h ] && echo PRESENT || echo ABSENT)"
	echo "  libelf.h:      $([ -f /usr/include/libelf.h ] && echo PRESENT || echo ABSENT)"

	echo "=== openssl's include tree ==="
	echo "  /usr/include/openssl entries: $(ls /usr/include/openssl 2>/dev/null | wc -l)"
	ls /usr/include/openssl 2>/dev/null | head -10

	echo "=== anywhere else on the filesystem ==="
	find / -xdev -name 'bio.h' -path '*openssl*' 2>/dev/null | head -5
	echo "--- (empty means nowhere)"

	echo "=== openssl runtime, for contrast ==="
	find / -xdev -name 'libcrypto.so*' 2>/dev/null | head -4

	echo "=== where HOSTCC looks ==="
	echo | /usr/bin/gcc -E -Wp,-v - 2>&1 | head -20

	echo "WIFI-DRIVER RESULT: PRESENT => fix the kernel recipe search path; ABSENT => fix the kernel-builder image"
	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
