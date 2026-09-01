#
# SCRATCH DIAGNOSTIC ONLY -- never installed, exits nonzero so the log
# is kept.
#
# #222 asks whether the 21 grandfathered gcc recipes still need gcc, and
# says the list is meant to shrink. Auditing them turned up something
# the issue did not expect: every one of the 21 DOES record a reason,
# as prose rather than in the declared field. So the question is not
# "why is this on gcc" but "is that reason still true", because
# ADR-0223 replaced the compiler underneath all of them.
#
# btrfs-progs states its reason plainly:
#
#   "Tier 3. TCC cannot parse `__thread`"
#
# Measured against the pinned compiler, that is no longer so:
#
#   tcctok.h:42   DEF(TOK___thread, "__thread")   /* GCC thread-local storage extension */
#   tccgen.c:4940 case TOK___thread:
#
# A real token with real codegen handling, not a mention. Which makes
# this a retirement candidate -- but "the stated blocker is gone" is a
# different claim from "it builds", and only the second one justifies
# moving a package off gcc. This builds it with tcc and reports which.
#
pkg_name="probe-toolchain-retire"
pkg_version="2"
pkg_source="https://www.kernel.org/pub/linux/kernel/people/kdave/btrfs-progs/btrfs-progs-v7.1.tar.xz"
pkg_sha256="d1f55cc2971398c9142eaa79d203e63d586a3b4b867f956664a1d68322cd4e34"
pkg_depends=""
pkg_build_depends="bash coreutils make tcc linux-headers binutils pkgconf zlib libuuid libblkid sed grep gawk findutils diffutils"
pkg_changelog="2: probe 1 used its own configure flags and died on ext2fs; this uses the recipe's exact invocation, changing only CC (#222)"

pkg_build() {
	echo "=== does this compiler parse __thread at all? ==="
	cat > /run/t.c <<'T_EOF'
__thread int per_thread_counter;
int main(void) { per_thread_counter = 7; return per_thread_counter == 7 ? 0 : 1; }
T_EOF
	if tcc /run/t.c -o /run/t 2>/run/t.err && /run/t; then
		echo "  __thread: compiles AND runs correctly"
	else
		echo "  __thread: FAILED"
		sed 's/^/    /' /run/t.err
		echo "  -- the recorded reason still stands; nothing further to test"
		exit 1
	fi

	echo
	echo "=== now the real question: does btrfs-progs BUILD with tcc ==="
	# The recipe's own invocation, verbatim, with CC swapped. Probe 1
	# invented its own flags, omitted --disable-convert, and died on a
	# missing ext2fs -- measuring my flag list rather than the
	# compiler. A comparison is only worth anything if exactly one
	# thing differs.
	if ! CC=tcc ./configure --prefix=/usr --disable-documentation --disable-python \
	            --disable-convert --disable-libudev --disable-zoned \
	            --with-crypto=builtin --disable-zstd --disable-lzo \
	            > /run/conf.log 2>&1; then
		echo "  configure FAILED under tcc:"
		tail -25 /run/conf.log | sed 's/^/    /'
		exit 1
	fi
	echo "  configure: ok"
	grep -iE '^(crypto provider|btrfs-progs)' /run/conf.log | head -4 | sed 's/^/    /'

	if ! make CC=tcc -j"$(nproc)" mkfs.btrfs > /run/make.log 2>&1; then
		echo "  make FAILED under tcc -- the first real errors:"
		grep -iE 'error|Error [0-9]' /run/make.log | head -12 | sed 's/^/    /'
		echo "  -- so the package needs gcc for a reason OTHER than __thread,"
		echo "     which is worth recording in its recipe rather than leaving"
		echo "     a reason that measurement has overtaken."
		exit 1
	fi
	echo "  make: SUCCEEDED under tcc"
	ls -la mkfs.btrfs 2>/dev/null | sed 's/^/    /'
	echo
	echo "  VERDICT: btrfs-progs builds with tcc -- a real retirement candidate (#222)"
	exit 1
}

pkg_install() {
	echo "probe-toolchain-retire is a diagnostic; nothing is installed" >&2
	exit 1
}
