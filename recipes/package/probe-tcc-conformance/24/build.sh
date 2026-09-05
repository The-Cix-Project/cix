#
# probe-tcc-conformance 24 -- does tcc emit PT_GNU_STACK, and can it be
# told to? (#292)
#
# 23 answered question 1 by dying on it. Its stack_hdr() assigned from a
# grep whose only job was to find nothing, and a failing command
# substitution in an assignment is fatal under set -e, so the log ends
# mid-probe at "tcc -shared". Absent is the answer to question 1, but
# the build never reached the four questions that decide the fix. Every
# grep whose empty result is a legitimate answer is now guarded, and the
# source grep runs first because it is the decisive one and needs no
# toolchain beyond the tarball.
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
#   1. does the tcc source mention PT_GNU_STACK at all?
#   2. does a plain tcc -shared object carry it?
#   3. does a plain tcc executable carry it?
#   4. is -Wl,-z,noexecstack accepted, and does it produce the header?
#   5. is bare -z noexecstack accepted, and does it produce the header?
#
# Question 1 is the decisive one and costs a grep of the source tree
# this recipe already downloads: a linker cannot emit a header it has no
# code to write. It runs first for that reason.
#
# Fails on purpose at the end, like every probe in this series -- the
# build log is the product.
#
pkg_name="probe-tcc-conformance"
pkg_version="24"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_build_depends="tcc make bash coreutils sed grep gawk binutils findutils"
pkg_changelog="24: 23 died on its own first question -- an assignment from a grep that correctly found nothing is fatal under set -e, so the log ends at question 1 with the remaining four unanswered. Every such grep is guarded now, the decisive source grep runs first, and readelf is checked for rather than assumed."

hdr() { echo; echo "=== $1 ==="; }

# Report whether one ELF file carries the header, and with which flags.
stack_hdr() {
	if [ ! -f "$1" ]; then
		echo "  $2: NOT BUILT"
		return
	fi
	line=$(readelf -lW "$1" 2>/dev/null | grep GNU_STACK || true)
	if [ -n "$line" ]; then
		echo "  $2: PRESENT -> $(echo "$line" | sed 's/  */ /g')"
	else
		n=$(readelf -lW "$1" 2>/dev/null | grep -c 'PHDR\|LOAD\|DYNAMIC\|NOTE\|TLS\|GNU_' || true)
		echo "  $2: ABSENT  (${n:-?} other program headers)"
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

	hdr "1. does tcc have any code to write PT_GNU_STACK?"
	echo "--- matches in the tcc source tree:"
	m=$(grep -rn 'GNU_STACK' . 2>/dev/null | head -20 || true)
	if [ -n "$m" ]; then
		echo "$m" | sed 's/^/  /'
		echo "  ^^ the header is known to the source"
	else
		echo "  NONE -- tcc has no code that emits this header, so no flag can produce it"
	fi
	echo "--- for contrast, headers tcc demonstrably does write:"
	grep -rn 'PT_GNU_RELRO\|PT_GNU_EH_FRAME' . 2>/dev/null | head -5 | sed 's/^/  /' || true
	echo "--- is readelf available for the link tests below?"
	command -v readelf >/dev/null 2>&1 && echo "  yes: $(readelf --version 2>&1 | head -1)" || echo "  NO -- the link tests below cannot report headers"

	hdr "2. plain shared object"
	try_link /run/plain.so -shared
	stack_hdr /run/plain.so "tcc -shared"

	hdr "3. plain executable"
	try_link /run/plain.bin
	stack_hdr /run/plain.bin "tcc (executable)"

	hdr "4. -Wl,-z,noexecstack"
	try_link /run/wlz.so -shared -Wl,-z,noexecstack
	stack_hdr /run/wlz.so "tcc -shared -Wl,-z,noexecstack"

	hdr "5. bare -z noexecstack"
	try_link /run/barez.so -shared -z noexecstack
	stack_hdr /run/barez.so "tcc -shared -z noexecstack"

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
