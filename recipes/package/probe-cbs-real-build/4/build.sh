#
# probe-cbs-real-build -- does cix-build-system actually build a real
# package?
#
# cix-build-system#134 records that no real upstream source has ever
# been through `cbs build`: its suite builds one fixture that writes a
# text file, and a two-file archive the test tars itself. Everything
# else about the engine is therefore asserted rather than measured.
# This probe answers it with the smallest honest subject available --
# CBS's OWN recipes/zstd.cbs, whose declared sha256 matches the real
# upstream tarball byte for byte (verified: both are
# 35ad983197f8f8eb0c963877bf8be50490a0b3df54b4edeb8399ba8a8b2f60a4).
#
# It also tests one specific prediction, cix-build-system#133: that
# src/archive.c refuses any archive containing a symlink or hardlink
# (`archive_entry_filetype(entry) == AE_IFLNK` -> `goto fail`), and
# that the zstd source contains two -- tests/cli-tests/bin/unzstd and
# zstdcat, both pointing at zstd. That issue was filed from reading the
# code and listing the tarball, never from running CBS. If this probe
# extracts cleanly, #133 is wrong and gets corrected.
#
# Why the source is repacked rather than used as fetched: cixd extracts
# a package's source host-side into /build/src, so the build container
# never sees the tarball. The probe rebuilds one with `cp -a` (which
# preserves symlinks) and tars it, then hands CBS that. The archive is
# not byte-identical to upstream and its checksum is therefore computed
# here and substituted into the recipe -- what is being tested is
# whether CBS can read an archive of this SHAPE, and the shape is real.
#
# Reports and exits 0 either way. A probe that fails the build hides
# the second half of its own output.
#
# Revision 1 never reached extraction. It failed at
#
#   source transport unavailable: libcurl is not available:
#   libcurl.so: cannot open shared object file
#   cbs build rc=3
#
# which is two findings at once. CBS dlopen()s libcurl at RUNTIME
# rather than linking it -- that is what the Makefile's -ldl is for --
# so the binary links, installs, and answers `check` and `explain`
# perfectly while being unable to build anything. And it refuses
# BEFORE consulting the cache, even though --cache was supplied and
# the source was sitting in it, which is the shape cixd would use
# (build containers have no network by policy, so cixd pre-places
# sources at <cache>/<sha256> -- cix-build-system#132's whole design).
#
# curl 8.21.0-4 is declared here so the probe can get past it and test
# the thing it was written to test. That is a workaround for this
# probe, not an answer to the design question, which is filed
# separately.
#
# Revision 2 then confirmed cix-build-system#133 exactly as predicted.
# The cache hit worked -- CBS found the archive by sha256 and did not
# fetch -- extraction ran, and it refused:
#
#   error[CPDL-E6001]: source: source `zstd` archive format or entry
#   is unsupported: unsafe archive
#
# "unsafe archive" is archive.c's fallback text for when
# archive_error_string() returns NULL, i.e. libarchive reported nothing
# wrong and the refusal was CBS's own goto fail on AE_IFLNK. Two
# symlinks in a 2.1 MB tarball stop the build.
#
# So this revision asks the next question, which is the one that
# matters for the tickets: is #133 the ONLY thing between CBS and a
# real package, or the first of several? It repacks the same source
# with the two symlinks removed -- they are test fixtures under
# tests/cli-tests/bin and nothing in the lib build reads them, so their
# absence cannot change the built library -- and runs the whole
# pipeline: build, verify, inspect, extract, and a second build
# compared byte for byte for cix-build-system#135.
#
# Removing them is a probe technique, not a fix. The fix is #133.
#
# Revision 3 got past extraction and hit the next thing, which is a
# new finding rather than a known one:
#
#   error[CPDL-E4005]: runtime: required directory `${src}/zstd-1.5.4`
#   does not exist
#     20 | prepare { require directory "${src}/zstd-1.5.4" { exists } }
#
# with `ls $src` showing a single entry, `zstd`. So CBS extracts into
# $src/<SOURCE-NAME>/, and the archive's own top directory lands at
# $src/zstd/zstd-1.5.4. ADR-0001 says "the main source is extracted by
# CBS into $src", and CBS's own recipes/zstd.cbs was written against
# that sentence rather than against the code -- which is what never
# running a real recipe looks like (cix-build-system#134).
#
# This revision uses the layout CBS actually produces and lists $src
# deeply so the layout is recorded rather than inferred.
#
pkg_name="probe-cbs-real-build"
pkg_version="4"
pkg_source="https://github.com/facebook/zstd/archive/refs/tags/v1.5.4.tar.gz"
pkg_sha256="35ad983197f8f8eb0c963877bf8be50490a0b3df54b4edeb8399ba8a8b2f60a4"
pkg_artifact_sha256="ee4ef7c2ad4954e29bb804403a7ee572ccd484dfa6bc76653fe61a363dc80474"
pkg_build_image="cix-builder"
# cbs plus its own runtime libraries, plus what CBS will need to shell
# out to for zstd's build (it runs make, which runs the compiler) and
# what this probe needs to repack the source.
pkg_build_depends="cbs libarchive zstd curl tcc make bash coreutils grep sed binutils findutils tar gzip linux-headers"
pkg_depends=""
pkg_changelog="4: use the extraction layout CBS actually produces. Revision 3 passed extraction once the symlinks were removed and then failed CPDL-E4005 on require directory ${src}/zstd-1.5.4 -- CBS extracts into $src/<source-name>/, so the tree is at $src/zstd/zstd-1.5.4, and CBS own bundled recipe encodes the ADR wording instead. 3: repack with the two symlinks removed, to find out whether #133 is the only blocker or the first of several. Revision 2 confirmed #133 by measurement: cache hit worked, extraction refused with CPDL-E6001 unsafe archive. 2: declare curl. Revision 1 failed at rc=3 with 'source transport unavailable: libcurl is not available' before reaching extraction -- CBS dlopen()s libcurl at runtime, and refuses even with a populated --cache. Also lists the cache directory before the build. 1: first run of cbs against a real upstream package (cix-build-system#134), using CBS's own recipes/zstd.cbs. Also tests the #133 prediction that source extraction refuses the two symlinks in the zstd tarball."

pkg_build() {
	set +e
	echo "==================== 1. what cixd handed us"
	echo "cbs version: $(cbs --version 2>&1)"
	echo -n "symlinks in the extracted source: "
	find /build/src -type l | wc -l
	find /build/src -type l | head -5

	echo
	echo "==================== 2. CBS's own zstd.cbs, parsed"
	mkdir -p /run/cbs/cache /run/cbs/workspace /run/cbs/pack
	cat > /run/cbs/zstd.cbs <<'CBSEOF'
# Source-built zstd runtime and development library for the CBS host toolchain.
package "zstd" {
    version "1.5.4"
    release 1
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
    prepare { require directory "${src}/zstd/zstd-1.5.4" { exists } }
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
	echo "==================== 3. repack the source, preserving symlinks"
	cp -a /build/src /run/cbs/pack/zstd-1.5.4
	echo -n "symlinks carried into the repack: "
	find /run/cbs/pack -type l | wc -l
	# Remove them, so the pipeline behind #133 can be measured. These
	# two are tests/cli-tests/bin/unzstd and zstdcat, both pointing at
	# zstd; the lib build does not read either.
	find /run/cbs/pack -type l -print -delete
	echo -n "symlinks after removal: "
	find /run/cbs/pack -type l | wc -l
	tar -czf /run/cbs/src.tar.gz -C /run/cbs/pack zstd-1.5.4
	SHA=$(sha256sum /run/cbs/src.tar.gz | cut -d' ' -f1)
	echo "repacked archive sha256: $SHA"
	sed -i "s/SHA256PLACEHOLDER/$SHA/" /run/cbs/zstd.cbs
	cp /run/cbs/src.tar.gz "/run/cbs/cache/$SHA"
	echo -n "symlinks visible to tar in that archive: "
	tar tvzf /run/cbs/src.tar.gz | grep -c '^[lh]'

	echo
	echo "==================== 4. cbs check"
	cbs check /run/cbs/zstd.cbs; echo "  rc=$?"

	echo
	echo "==================== 5. cbs explain --json"
	cbs explain /run/cbs/zstd.cbs --json > /run/cbs/explain.json 2>/run/cbs/explain.err
	echo "  rc=$?"
	head -c 900 /run/cbs/explain.json; echo
	echo "  stderr: $(cat /run/cbs/explain.err)"

	echo
	echo "==================== 6. the cache CBS is being handed"
	ls -l /run/cbs/cache
	echo "recipe sha256 line: $(grep sha256 /run/cbs/zstd.cbs)"

	echo
	echo "==================== 6b. cbs build -- THE TEST"
	cbs build /run/cbs/zstd.cbs \
	    --arch x86_64 \
	    --staged /run/cbs/workspace \
	    --output /run/cbs/zstd.cixpkg \
	    --cache /run/cbs/cache
	rc=$?
	echo "  cbs build rc=$rc"

	echo
	echo "==================== 7. what it left behind"
	echo "-- workspace:"; ls -la /run/cbs/workspace 2>&1 | head
	echo "-- src tree (3 deep):"; find /run/cbs/workspace/src -maxdepth 3 2>&1 | head -20
	echo "-- dest:";      find /run/cbs/workspace/dest -maxdepth 3 2>&1 | head -15
	echo "-- artifact:";  ls -l /run/cbs/zstd.cixpkg 2>&1

	if [ -f /run/cbs/zstd.cixpkg ]; then
		echo
		echo "==================== 8. cbs verify / inspect / extract"
		cbs verify /run/cbs/zstd.cixpkg; echo "  verify rc=$?"
		cbs inspect /run/cbs/zstd.cixpkg 2>&1 | head -20; echo "  inspect rc=$?"
		cbs extract /run/cbs/zstd.cixpkg --into /run/cbs/extracted >/dev/null 2>&1
		echo "  extract rc=$?"
		echo "-- extracted tree:"; find /run/cbs/extracted 2>&1 | head -20
		echo -n "-- symlinks preserved through CIXPKG (issue #128): "
		find /run/cbs/extracted -type l 2>/dev/null | wc -l
		find /run/cbs/extracted -type l 2>/dev/null | head

		echo
		echo "==================== 9. determinism (issue #135)"
		mkdir -p /run/cbs/workspace2
		cbs build /run/cbs/zstd.cbs --arch x86_64 --staged /run/cbs/workspace2 \
		    --output /run/cbs/zstd2.cixpkg --cache /run/cbs/cache >/dev/null 2>&1
		echo "  second build rc=$?"
		if [ -f /run/cbs/zstd2.cixpkg ]; then
			a=$(sha256sum /run/cbs/zstd.cixpkg | cut -d' ' -f1)
			b=$(sha256sum /run/cbs/zstd2.cixpkg | cut -d' ' -f1)
			echo "  build 1: $a"
			echo "  build 2: $b"
			[ "$a" = "$b" ] && echo "  DETERMINISTIC" || echo "  NOT DETERMINISTIC"
		fi
	fi
	echo
	echo "==================== probe complete"
	set -e
}

pkg_install() {
	# A probe installs nothing; the build log is the whole product.
	mkdir -p "$PKG_DESTDIR/usr/share/probe-cbs-real-build"
	echo "see the build log" > "$PKG_DESTDIR/usr/share/probe-cbs-real-build/README"
}
