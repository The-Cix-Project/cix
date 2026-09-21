#
# probe-cbs-real-build 7 -- does CBS build a real package now?
#
# Revisions 1-6 walked cix-build-system from "cannot start" to five
# distinct failures against one real recipe. Commit 0a5313f6 claims to
# fix four of them. This revision re-runs the same test with nothing
# softened, to check the claim rather than the tracker:
#
#   - the source keeps its two symlinks (#133 said they were refused)
#   - the cache is pre-populated and no network exists (CBS refused to
#     start without libcurl before, ignoring the cache)
#   - CBS's own recipes/zstd.cbs is used VERBATIM from 0a5313f6, only
#     its sha256 substituted for the repacked archive
#   - no env "CC" binding is added, so the requires-block compiler
#     declaration has to be what puts tcc in the environment
#
# Then the whole pipeline: build, verify, inspect, extract, whether the
# symlinks survive CIXPKG (#128), and the same build twice compared
# byte for byte (#135).
#
# Reports and exits 0 either way.
#
# Revision 8 re-runs the identical test against commit 0fcbd35b, as a
# regression check rather than a discovery. Revision 7 proved the whole
# pipeline at 0a5313f6; three commits have landed since, touching the
# Makefile, the test suite and the version derivation. None of them
# should have changed the engine, and that is exactly the kind of
# expectation worth measuring rather than assuming.
#
pkg_name="probe-cbs-real-build"
pkg_version="8"
pkg_source="https://github.com/facebook/zstd/archive/refs/tags/v1.5.4.tar.gz"
pkg_sha256="35ad983197f8f8eb0c963877bf8be50490a0b3df54b4edeb8399ba8a8b2f60a4"
pkg_artifact_sha256="ee4ef7c2ad4954e29bb804403a7ee572ccd484dfa6bc76653fe61a363dc80474"
pkg_build_image="cix-builder"
pkg_build_depends="cbs libarchive zstd curl tcc make bash coreutils grep sed binutils findutils tar gzip linux-headers"
pkg_depends=""
pkg_changelog="8: regression re-run of revision 7 against commit 0fcbd35b. 7: re-run against commit 0a5313f6 with nothing softened -- symlinks intact, cache-only, CBS own zstd.cbs verbatim, no env CC binding. Revisions 1-6 found five distinct failures; this checks the four the commit claims to fix."

pkg_build() {
	set +e
	echo "==================== 1. binary"
	echo "cbs version: $(cbs --version 2>&1)"
	echo -n "symlinks in the extracted source: "; find /build/src -type l | wc -l

	echo
	echo "==================== 2. CBS's own zstd.cbs from 0a5313f6, verbatim"
	mkdir -p /run/cbs/cache /run/cbs/workspace /run/cbs/workspace2 /run/cbs/pack
	cat > /run/cbs/zstd.cbs <<'CBSEOF'
# Source-built zstd runtime and development library for the CBS host toolchain.
package "zstd" {
    version "1.5.4"
    release 1
    format "cixpkg"
    sources {
        main "zstd" {
            url "https://github.com/facebook/zstd/archive/refs/tags/v1.5.4.tar.gz"
            sha256 "SHA256PLACEHOLDER"
        }
    }
    requires {
        build {
            compiler "tcc"
            tool "make"
            tool "bash"
            tool "coreutils"
            tool "binutils"
        }
    }
    prepare {
        require directory "${src}/zstd/zstd-1.5.4" { exists }
        replace "${src}/zstd/zstd-1.5.4/lib/Makefile" {
            from "DEPFLAGS = -MT $@ -MMD -MP -MF"
            to "DEPFLAGS ="
            exactly 1
        }
        replace "${src}/zstd/zstd-1.5.4/lib/Makefile" {
            from """
                $(COMPILE.c) $(DEPFLAGS) $(ZSTD_DYNLIB_DIR)/$*.d $(OUTPUT_OPTION) $<
                """
            to """
                $(COMPILE.c) $(OUTPUT_OPTION) $<
                """
            exactly 1
        }
        replace "${src}/zstd/zstd-1.5.4/lib/Makefile" {
            from """
                $(COMPILE.c) $(DEPFLAGS) $(ZSTD_STATLIB_DIR)/$*.d $(OUTPUT_OPTION) $<
                """
            to """
                $(COMPILE.c) $(OUTPUT_OPTION) $<
                """
            exactly 1
        }
    }
    build {
        cd "${src}/zstd/zstd-1.5.4" { run "make" { "-C" "lib" jobs $jobs } }
    }
    check {
        require file "${src}/zstd/zstd-1.5.4/lib/libzstd.a" { exists }
    }
    install {
        cd "${src}/zstd/zstd-1.5.4" {
            run "make" { "-C" "lib" "install" "PREFIX=/usr" "DESTDIR=${dest}" jobs $jobs }
        }
    }
}
CBSEOF

	echo
	echo "==================== 3. repack, SYMLINKS KEPT"
	cp -a /build/src /run/cbs/pack/zstd-1.5.4
	echo -n "symlinks in the repack: "; find /run/cbs/pack -type l | wc -l
	tar -czf /run/cbs/src.tar.gz -C /run/cbs/pack zstd-1.5.4
	SHA=$(sha256sum /run/cbs/src.tar.gz | cut -d' ' -f1)
	sed -i "s/SHA256PLACEHOLDER/$SHA/" /run/cbs/zstd.cbs
	cp /run/cbs/src.tar.gz "/run/cbs/cache/$SHA"
	echo "cache: $(ls /run/cbs/cache)"

	echo
	echo "==================== 4. check"
	cbs check /run/cbs/zstd.cbs; echo "  rc=$?"

	echo
	echo "==================== 5. cbs build -- THE TEST"
	cbs build /run/cbs/zstd.cbs --arch x86_64 \
	    --staged /run/cbs/workspace --output /run/cbs/zstd.cixpkg \
	    --cache /run/cbs/cache
	echo "  cbs build rc=$?"

	echo
	echo "==================== 6. results"
	echo "-- dest:"; find /run/cbs/workspace/dest 2>&1 | head -12
	echo "-- artifact:"; ls -l /run/cbs/zstd.cixpkg 2>&1

	if [ -f /run/cbs/zstd.cixpkg ]; then
		echo
		echo "==================== 7. verify / inspect / extract"
		cbs verify /run/cbs/zstd.cixpkg; echo "  verify rc=$?"
		cbs inspect /run/cbs/zstd.cixpkg 2>&1 | head -25
		cbs extract /run/cbs/zstd.cixpkg --into /run/cbs/extracted >/dev/null 2>&1
		echo "  extract rc=$?"
		find /run/cbs/extracted 2>&1 | head -15
		echo -n "-- symlinks through CIXPKG (#128): "
		find /run/cbs/extracted -type l 2>/dev/null | wc -l
		ls -l /run/cbs/extracted/usr/lib 2>/dev/null | head

		echo
		echo "==================== 8. determinism (#135)"
		cbs build /run/cbs/zstd.cbs --arch x86_64 \
		    --staged /run/cbs/workspace2 --output /run/cbs/zstd2.cixpkg \
		    --cache /run/cbs/cache >/dev/null 2>&1
		echo "  second build rc=$?"
		if [ -f /run/cbs/zstd2.cixpkg ]; then
			a=$(sha256sum /run/cbs/zstd.cixpkg | cut -d' ' -f1)
			b=$(sha256sum /run/cbs/zstd2.cixpkg | cut -d' ' -f1)
			echo "  1: $a"
			echo "  2: $b"
			[ "$a" = "$b" ] && echo "  DETERMINISTIC" || echo "  NOT DETERMINISTIC"
		fi
	fi
	echo
	echo "==================== probe complete"
	set -e
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-cbs-real-build"
	echo "see the build log" > "$PKG_DESTDIR/usr/share/probe-cbs-real-build/README"
}
