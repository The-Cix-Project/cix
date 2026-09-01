#
# SCRATCH DIAGNOSTIC ONLY -- never installed, exits nonzero so the log
# is kept.
#
# A v2.9.0 deploy failed with:
#
#     cixctl: image_path is not a squashfs image (HTTP 400)
#
# while the assembly itself reported "succeeded". The daemon's check is
# squashfs's own on-disk magic, "hsqs". A byte-SWAPPED magic reads
# "sqsh", and that is the exact signature of the endianness bug this
# project already hit in squashfs-tools: its endian_compat.h branches
# every byte-order decision on the bare `linux` macro, which TCC does
# not predefine, so a build without -Dlinux=1 silently byte-swaps every
# multi-byte on-disk field while mksquashfs reports clean success.
#
# squashfs-tools was rebuilt against the new compiler (ADR-0223) as
# part of the #216 remediation, so the question is whether that rebuild
# reintroduced it. The recipe still passes -Dlinux=1, so the fix is
# present in the source -- which is a reason to MEASURE the output
# rather than conclude from the recipe that it must be fine.
#
# Makes a squashfs and reads the first four bytes.
#
pkg_name="probe-tcc-conformance"
pkg_version="15"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils squashfs-tools"
pkg_changelog="15: does the rebuilt mksquashfs still write a correct 'hsqs' magic, or has the endianness bug come back as 'sqsh'"

pkg_build() {
	mkdir -p /run/p15/src && cd /run/p15
	echo "hello from cix" > src/hello.txt
	mkdir -p src/sub
	printf 'second file\n' > src/sub/two.txt

	echo "=== mksquashfs ==="
	command -v mksquashfs || echo "  (not on PATH)"
	mksquashfs src out.squashfs -noappend 2>&1 | tail -6
	echo "  exit=$?"

	echo
	echo "=== the first four bytes ==="
	if [ ! -f out.squashfs ]; then
		echo "  no image produced at all"
		exit 1
	fi
	echo "  size: $(stat -c%s out.squashfs) bytes"
	magic=$(head -c 4 out.squashfs)
	printf '  magic as text: %s\n' "$magic"
	od -An -tx1 -N4 out.squashfs | sed 's/^/  magic as hex:/'
	case "$magic" in
	hsqs) echo "  VERDICT: correct little-endian magic" ;;
	sqsh) echo "  VERDICT: BYTE-SWAPPED -- the endianness bug is back" ;;
	*)    echo "  VERDICT: neither hsqs nor sqsh -- something else is wrong" ;;
	esac

	echo
	echo "=== does the kernel's own tooling agree it is a squashfs ==="
	unsquashfs -s out.squashfs 2>&1 | head -8 | sed 's/^/  /' || echo "  unsquashfs could not read it"

	echo
	echo "=== probe complete -- failing on purpose ==="
	exit 1
}

pkg_install() {
	:
}
