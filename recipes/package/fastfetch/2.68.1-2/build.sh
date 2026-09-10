#
# fastfetch 2.68.1-1 -- first packaging.
#
# A system-information tool: it reads the host it runs on and prints a
# summary beside a distro logo. Requested for the `jump` container so an
# operator attaching to a console immediately sees what they are on.
#
# The first CMake-only package in this recipe set, which is why
# `cmake 4.4.3-1` had to be packaged first -- fastfetch's CMakeLists.txt
# opens with `cmake_minimum_required(VERSION 3.12.0)` and ships no
# autotools path.
#
# Source is the GitHub *archive* tarball (codeload), not a release
# asset. CLAUDE.md records cixd's own curl failing specific GitHub
# release-asset redirect chains; archive tarballs are a different path
# and are what several recipes here already fetch. Bytes fetched twice
# and hashed identically before this recipe was written.
#
# TWO THINGS THIS DOES NOT DO YET, stated rather than discovered later:
#
#  1. It cannot identify Cix. fastfetch reads /etc/os-release and this
#     platform ships none -- confirmed directly against the running
#     `jump` container, where GET /containers/jump/files?path=/etc/os-release
#     returns `no such file`. Without it fastfetch falls back to a
#     generic Linux identification and the Cix logo can never be
#     selected automatically. The os-release itself is a platform
#     change, not a fastfetch one, and is tracked separately.
#
#  2. The Cix logo is not upstream. fastfetch's own Logo Request
#     template requires a public repository URL and a distro website
#     with a downloadable ISO, and warns that a request not meeting them
#     "may be closed at any time". Cix has neither yet. So the logo
#     ships HERE and is selected with --logo, which is exactly what that
#     template recommends for this situation ("we recommend keeping them
#     locally"). Upstreaming is a decision for when Cix is public.
#
# The art is a terminal rendering of the owner's own mark -- the joined
# C + X with the centred dot, docs/brand/cix-ui-svg-icons/brand/
# cix-mark.svg -- not an invention. A first draft set three separate
# letters and was wrong: it was a wordmark, and this platform already
# has a canonical glyph. Copper falls only on the dot, which is the
# focal point of that glyph; everything else is Nickel, so the mark
# still reads with no accent at all.
#
pkg_name="fastfetch"
pkg_version="2.68.1-2"
pkg_source="https://github.com/fastfetch-cli/fastfetch/archive/refs/tags/2.68.1.tar.gz"
pkg_sha256="c268cfcd230cc7ed5447fb34ed21bf4977315c7104356a39388b6ba784ad11b0"
pkg_depends=""
#
# ADR-0224 as amended by ADR-0226.
#
pkg_toolchain="gcc"
pkg_toolchain_reason="language: fastfetch is C23 -- its CMakeLists.txt sets the C standard to 23 and the sources use C23 constructs throughout. TCC reports __STDC_VERSION__ 199901 even on the pinned 0.9.28rc snapshot (probe-tcc-conformance/9), so this is a language-version gap rather than a bug to file. Re-measure if TCC gains C23."
#
# ADR-0199/0209: composed from exactly these.
#   cmake          the build system; nothing else can configure this
#   gcc            the C23 compiler
#   make           cmake generates Makefiles and runs make
#   bash           pkg_build() runs under it
#   coreutils      cmake's own configure-time probes shell out constantly
#   sed grep       used by cmake's feature detection
#   findutils      cmake globs the source tree
#   binutils       ar/ranlib/ld
#   pkgconf        cmake's find_package(PkgConfig); fastfetch's optional
#                  dependencies are discovered through it
#   glibc          the C library
#   linux-headers  kernel uapi headers
#
# Deliberately NOT declared: python. fastfetch uses it only to generate
# embedded PCI/AMDGPU id tables and prints a warning and carries on
# without it (CMakeLists.txt:366). Those tables only enrich GPU naming,
# which is not why this is being installed, and pulling python into the
# build for a cosmetic string is the same trade btop's recipe declines
# for git. Stated so the absence is a decision rather than an oversight.
pkg_build_depends="cmake gcc make bash coreutils sed grep findutils binutils pkgconf glibc linux-headers"
pkg_changelog="2.68.1-2: the shipped logo is now a rendering of the owner's own mark (the joined C+X with the centred dot, docs/brand/cix-ui-svg-icons/brand/cix-mark.svg). Revision 1 shipped a three-letter wordmark drawn from the guidelines text before the canonical glyph in docs/brand/ had been looked at -- it was not the Cix mark. No other change."

pkg_build() {
	#
	# Every optional feature is left to cmake's own detection rather
	# than forced on: fastfetch is explicitly built around optional
	# dependencies, and a build image carrying only what this recipe
	# declares is precisely the environment that detection is for.
	#
	# BUILD_TESTS is off because the suite wants a populated /sys and
	# /proc that a build container does not have; the gate below tests
	# the thing that actually matters instead.
	#
	cmake -S . -B build \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=/usr \
		-DBUILD_TESTS=OFF

	cmake --build build -j"$(nproc 2>/dev/null || echo 4)"

	#
	# A build-time gate. `--version` proves a binary starts; it does not
	# prove the thing this package exists to do. So run a real query and
	# require real output back.
	#
	# `--structure Title` keeps this to one module that cannot depend on
	# hardware the build container does not have -- a GPU or display
	# probe would make this gate flaky for reasons unrelated to whether
	# fastfetch works.
	#
	out=$(./build/fastfetch --structure Title --pipe 2>&1) || {
		echo "fastfetch did not run: $out" >&2
		exit 1
	}
	if [ -z "$out" ]; then
		echo "fastfetch ran and printed nothing -- it reports no title at all" >&2
		exit 1
	fi
	echo "=== fastfetch gate: produced real output ==="
	echo "$out" | sed 's/^/  /'

	#
	# And the logo it will actually be asked for. This is a real gate:
	# a logo file with a malformed colour placeholder renders as literal
	# `$1` text rather than failing, so "it printed something" is not
	# enough -- the escape has to be gone from the output.
	#
	mkdir -p "$PWD/cixlogo"
	cat > "$PWD/cixlogo/cix.txt" <<'LOGO'
$1           ▄▄▄▄
$1      ▄▄███████████▄
$1    ▄██████▀▀▀▀▀██████▄           ▄▄██████▄
$1   ████▀▀         ▀█████▄      ▄█████████▀▀
$1  ████▀     $2▄▄$1      ▀█████▄  ▄█████▀
$1 ▄███▀    $2▄█████▄$1     ▀▀██▀▄████▀
$1 ████     $2███████$1        ▄█████
$1 ▀███▄    $2▀█████▀$1      ▄████████▄
$1  ████▄     $2▀▀$1       ▄████▀  ▀████▄▄
$1   ████▄▄         ▄▄████▀      ▀█████▄▄▄▄▄
$1    ▀██████▄▄▄▄▄██████▀          ▀▀███████▀
$1      ▀▀███████████▀▀
$1           ▀▀▀▀
LOGO
	logo_out=$(./build/fastfetch --logo "$PWD/cixlogo/cix.txt" --structure Title --pipe 2>&1) || {
		echo "fastfetch could not render the Cix logo: $logo_out" >&2
		exit 1
	}
	case "$logo_out" in
	*'$1'*|*'$2'*)
		echo "the Cix logo rendered its colour placeholders literally:" >&2
		echo "$logo_out" >&2
		exit 1
		;;
	esac
	echo "=== Cix logo gate: rendered with placeholders resolved ==="
}

pkg_install() {
	DESTDIR="$PKG_DESTDIR" cmake --install build

	if [ ! -x "$PKG_DESTDIR/usr/bin/fastfetch" ]; then
		echo "usr/bin/fastfetch missing from the artifact" >&2
		exit 1
	fi

	#
	# The Cix logo ships with the package, so `fastfetch --logo
	# /usr/share/cix/fastfetch/cix.txt` works on any image that installs
	# it without the operator having to carry a file around. Once the
	# platform has an /etc/os-release this becomes selectable by name
	# instead, and once Cix is public it can go upstream -- neither of
	# which changes where the file lives.
	#
	mkdir -p "$PKG_DESTDIR/usr/share/cix/fastfetch"
	cat > "$PKG_DESTDIR/usr/share/cix/fastfetch/cix.txt" <<'LOGO'
$1           ▄▄▄▄
$1      ▄▄███████████▄
$1    ▄██████▀▀▀▀▀██████▄           ▄▄██████▄
$1   ████▀▀         ▀█████▄      ▄█████████▀▀
$1  ████▀     $2▄▄$1      ▀█████▄  ▄█████▀
$1 ▄███▀    $2▄█████▄$1     ▀▀██▀▄████▀
$1 ████     $2███████$1        ▄█████
$1 ▀███▄    $2▀█████▀$1      ▄████████▄
$1  ████▄     $2▀▀$1       ▄████▀  ▀████▄▄
$1   ████▄▄         ▄▄████▀      ▀█████▄▄▄▄▄
$1    ▀██████▄▄▄▄▄██████▀          ▀▀███████▀
$1      ▀▀███████████▀▀
$1           ▀▀▀▀
LOGO
	cat > "$PKG_DESTDIR/usr/share/cix/fastfetch/cix-small.txt" <<'LOGO'
$1     ▄▄▄▄▄▄
$1  ▄███▀▀▀▀███▄      ▄▄██▄
$1 ▄██▀      ▀███▄  ▄██▀▀▀▀
$1 ██   $2███▄$1   ▀▀█▄██▀
$1 ██   $2███▀$1   ▄█████▄
$1 ▀██▄      ▄███▀ ▀███▄▄
$1  ▀███▄▄▄▄███▀      ▀████
$1     ▀▀▀▀▀▀
LOGO
}
