#
# probe-wifi-driver 10 -- is perl in kernel-builder now? (#30)
#
# Kernel 7.2.3-7 got past certs/ (extract-cert, x509_certificate_list,
# signing_key.x509 and certs/built-in.a all built) and then died in
# lib/:
#
#   GEN     lib/oid_registry_data.c
#   /usr/bin/bash: line 1: perl: command not found
#   make[3]: *** [lib/Makefile:295: lib/oid_registry_data.c] Error 127
#
# lib/build_OID_registry is a Perl script, and the OID registry is
# reached from asymmetric key parsing, which is reached from
# SYSTEM_DATA_VERIFICATION -- the same symbol that pulled in openssl.
# One config change, two build dependencies, a build apart.
#
# kernel-builder 1.4.0 adds perl and the manifest now carries it
# (24 entries, read back from GET /v1/images/kernel-builder). Whether
# the FILES are in the image is a different question from whether the
# manifest names them, and this asks the second one -- two minutes,
# against twenty-two for a kernel build that answers it by failing.
#
# Run as a HOST BUILD, because that is the only way to land in
# kernel-builder: an ordinary install composes its own environment from
# pkg_build_depends and never touches the named image (pkg.c, and
# revisions 5-7 of this probe learned it the hard way).
#
pkg_name="probe-wifi-driver"
pkg_version="10"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz"
pkg_sha256="8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03"
pkg_build_image="kernel-builder"
pkg_build_depends="bash coreutils findutils grep"
pkg_changelog="10: does kernel-builder actually have perl, after 1.4.0 added it to the manifest. The kernel build reached lib/oid_registry_data.c and died on 'perl: command not found' -- a Perl generator reached from SYSTEM_DATA_VERIFICATION, the same symbol that pulled in openssl one build earlier."

pkg_build() {
	echo "=== perl ==="
	for p in /usr/bin/perl /bin/perl /usr/local/bin/perl; do
		if [ -x "$p" ]; then echo "  FOUND $p"; else echo "  absent $p"; fi
	done

	echo "=== does it run, and what version ==="
	if /usr/bin/perl -e 'print "PERL-RUNS: $]\n"'; then
		:
	else
		echo "  perl did not run"
	fi

	echo "=== the exact thing the kernel needs it for ==="
	# lib/build_OID_registry is a plain #!/usr/bin/perl script, so the
	# question is only whether a bare `perl` resolves the way the
	# kernel's own generated command line invokes it.
	if perl -e 'print "BARE-PERL-ON-PATH: ok\n"'; then
		:
	else
		echo "  BARE-PERL-ON-PATH: NOT RESOLVABLE (the kernel invokes it bare)"
	fi

	echo "=== sanity: still the right sandbox ==="
	echo "  gcc: $([ -x /usr/bin/gcc ] && echo yes || echo no)  make: $([ -x /usr/bin/make ] && echo yes || echo no)"

	echo "WIFI-DRIVER RESULT: perl presence reported above"
	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
