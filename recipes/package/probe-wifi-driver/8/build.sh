#
# probe-wifi-driver 8 -- does a composed build environment that DECLARES
# openssl actually contain openssl's headers? (#30, #338)
#
# Revisions 5, 6 and 7 all asked the wrong question, and the reason is
# worth writing down because it invalidates a conclusion this project
# had already filed as an issue.
#
# They each set pkg_build_image="kernel-builder" and then inspected the
# sandbox they ran in, on the assumption that the build container is
# composed from that image. It is not. daemon/src/pkg.c:6072 takes a
# recipe with a non-empty pkg_build_depends down a different path
# entirely: buildenv_image_for() hashes the DECLARED TOOL SET, and the
# build runs in an image composed from exactly those tools --
# "the environment is a property of the recipe, not of this box's
# install history", in that code's own words. pkg_build_image is
# consulted only by the hostbuild path, to name the image a host build
# targets.
#
# So all three probes declared bash, coreutils, findutils and grep, and
# all three faithfully reported a sandbox holding bash, coreutils,
# findutils and grep. "No zlib.h, no libelf.h, 131 glibc-only entries"
# was not a platform gap at all -- it was an accurate description of a
# four-tool environment that was never supposed to have them. Issue
# #338 rests on that reading and needs correcting either way this
# lands.
#
# The question that actually matters is the one below, and it is asked
# the only way that answers it: by declaring openssl the way kernel
# 7.2.3-7 does, and looking. That revision put openssl in its
# pkg_build_depends, which by the mechanism above should have composed
# a NEW environment containing it -- and still died on
#
#   certs/extract-cert.c:21:10: fatal error: openssl/bio.h:
#                               No such file or directory
#
# at exactly the same line as 7.2.3-5, which had not declared it. Two
# outcomes, and they need opposite fixes:
#
#   - headers PRESENT: the environment is right and the kernel's HOSTCC
#     is not finding them; the fix is a search path (HOSTCFLAGS), not a
#     package.
#   - headers ABSENT: openssl's own artifact carries libraries without
#     headers, and no amount of declaring it will ever satisfy a
#     compile. The fix is in openssl.recipe or in what finalize keeps.
#
# Only what is declared is used: no sed, no awk, no tr. Revision 5 died
# on an undeclared sed and lost the sections that mattered.
#
pkg_name="probe-wifi-driver"
pkg_version="8"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz"
pkg_sha256="8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03"
pkg_build_image="kernel-builder"
pkg_build_depends="bash coreutils findutils grep openssl"
pkg_changelog="8: declares openssl, which is what revisions 5-7 failed to do. Those set pkg_build_image=kernel-builder and inspected the sandbox they ran in, but a recipe with pkg_build_depends runs in an environment composed from its DECLARED TOOLS (pkg.c:6072), not from that image -- so all three measured a four-tool sandbox and correctly found no package headers in it, which is what #338 was filed on."

pkg_build() {
	echo "=== the header the kernel's certs/extract-cert.c needs ==="
	echo "  /usr/include/openssl/bio.h: $([ -f /usr/include/openssl/bio.h ] && echo PRESENT || echo ABSENT)"

	echo "=== anywhere at all? ==="
	find / -xdev -name 'bio.h' -path '*openssl*' 2>/dev/null | head -5
	echo "--- (nothing above means no openssl header is in this environment)"

	echo "=== how much of openssl's include tree is here ==="
	echo "  /usr/include/openssl entries: $(ls /usr/include/openssl 2>/dev/null | wc -l)"
	ls /usr/include/openssl 2>/dev/null | head -12

	echo "=== openssl's libraries, for contrast ==="
	find / -xdev -name 'libcrypto.so*' -o -xdev -name 'libssl.so*' 2>/dev/null | head -6

	echo "=== and its pkg-config, which 3.0.20-6 asserts it installs ==="
	find / -xdev -name 'libcrypto.pc' 2>/dev/null | head -3

	echo "=== /usr/include overall ==="
	echo "  entries: $(ls /usr/include 2>/dev/null | wc -l)"

	echo "WIFI-DRIVER RESULT: header PRESENT => kernel HOSTCC search-path fix; ABSENT => openssl artifact ships no headers"
	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
