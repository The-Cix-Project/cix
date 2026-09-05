#
# probe-tcc-conformance 23 -- does tcc emit PT_GNU_STACK, and can it be
# told to? (#292)
#
# A shared object with no PT_GNU_STACK program header means, to the ELF
# loader, that it requires an executable stack. glibc 2.41 and later
# refuse to grant that at dlopen time and fail the load outright:
#
#   cannot enable executable stack as shared object requires: Invalid argument
#
# Measured on 192.168.15.95 in the jump container against glibc 2.44:
# every gcc-built shared object on the platform carries the header and
# every tcc-built one lacks it, and libnss_ldap.so.2 consequently cannot
# be loaded by getent, which is correctly marked, while it loads fine
# into id and sshd, which are tcc-built and therefore unmarked
# themselves. That asymmetry is the whole of #292.
#
# The fix depends entirely on one fact this probe measures rather than
# assumes: whether the pinned compiler can be asked for the header. If a
# link flag produces it, every affected recipe is one LDFLAGS line from
# correct. If nothing produces it, the header does not exist anywhere in
# tcc and the choice is between moving packages to gcc one at a time and
# a compiler change, which is a different cost entirely.
#
# The dev sandbox cannot answer this. Its tcc is Debian's and this
# project has already been burned twice treating that as an answer about
# this platform's compiler.
#
# Five questions:
#   1. does a plain tcc -shared object carry PT_GNU_STACK?
#   2. does a plain tcc executable carry it?
#   3. is -Wl,-z,noexecstack accepted, and does it produce the header?
#   4. is bare -z noexecstack accepted, and does it produce the header?
#   5. does the tcc source mention PT_GNU_STACK at all?
#
# Question 5 is the decisive one and costs a grep of the source tree
# this recipe already downloads: a linker cannot emit a header it has no
# code to write.
#
# Fails on purpose at the end, like every probe in this series -- the
# build log is the product.
#
pkg_name="probe-tcc-conformance"
pkg_version="23"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_build_depends="tcc make bash coreutils sed grep gawk binutils findutils"
pkg_changelog="23: measure whether tcc emits PT_GNU_STACK and whether any link flag asks it to (#292). A shared object without that header is refused by glibc 2.41+ at dlopen, which is why libnss_ldap.so.2 loads into id and sshd but not into getent."

hdr() { echo; echo "=== $1 ==="; }

# Report whether one ELF file carries the header, and with which flags.
stack_hdr() {
	if [ ! -f "$1" ]; then
		echo "  $2: NOT BUILT"
		return
	fi
	line=$(readelf -lW "$1" 2>/dev/null | grep GNU_STACK)
	if [ -n "$line" ]; then
		echo "  $2: PRESENT -> $(echo "$line" | sed 's/  */ /g')"
	else
		echo "  $2: ABSENT  ($(readelf -lW "$1" 2>/dev/null | grep -c 'LOAD\|DYNAMIC\|NOTE\|TLS') other program headers)"
	fi
}

# Try one link, reporting whether tcc accepted the flags at all. A flag
# tcc rejects and a flag tcc silently ignores are different answers and
# the exit status is the only thing that separates them.
try_link() {
	out="$1"; shift
	rm -f "$out"
	if tcc "$@" -o "$out" /run/probe.c 2>/run/link.err; then
		echo "  accepted (exit 0)"
	else
		echo "  REJECTED (exit $?): $(head -1 /run/link.err)"
	fi
}

pkg_build() {
	hdr "compiler under test"
	tcc -v 2>&1 | head -2 | sed 's/^/  /'

	cat > /run/probe.c <<'EOF'
int probe_symbol(int n) { return n + 1; }
int main(void) { return probe_symbol(0); }
EOF

	hdr "1. plain shared object"
	try_link /run/plain.so -shared
	stack_hdr /run/plain.so "tcc -shared"

	hdr "2. plain executable"
	try_link /run/plain.bin
	stack_hdr /run/plain.bin "tcc (executable)"

	hdr "3. -Wl,-z,noexecstack"
	try_link /run/wlz.so -shared -Wl,-z,noexecstack
	stack_hdr /run/wlz.so "tcc -shared -Wl,-z,noexecstack"

	hdr "4. bare -z noexecstack"
	try_link /run/barez.so -shared -z noexecstack
	stack_hdr /run/barez.so "tcc -shared -z noexecstack"

	hdr "5. does tcc have any code to write PT_GNU_STACK?"
	echo "--- matches in the tcc source tree:"
	if grep -rn 'GNU_STACK' . 2>/dev/null | head -20; then
		echo "  ^^ the header is known to the source"
	else
		echo "  NONE -- tcc has no code that emits this header, so no flag can produce it"
	fi
	echo "--- for contrast, headers tcc demonstrably does write:"
	grep -rn 'PT_GNU_RELRO\|PT_GNU_EH_FRAME' . 2>/dev/null | head -5 | sed 's/^/  /'

	hdr "6. what gcc produces, for reference"
	if command -v gcc >/dev/null 2>&1; then
		gcc -shared -fPIC -o /run/gcc.so /run/probe.c 2>/dev/null
		stack_hdr /run/gcc.so "gcc -shared"
	else
		echo "  gcc not present in this build image -- skipped"
	fi

	hdr "probe complete -- failing on purpose so nothing installs"
	exit 1
}
