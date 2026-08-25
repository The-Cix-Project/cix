#
# lldap -- a lightweight LDAP server with a real, first-party web UI
# for user/group management (users edit their own details, reset
# passwords, all through the browser -- "LDAP made easy," chosen
# specifically for this over glauth, which has no web UI at all).
#
# Read by cixd's own non-executing metadata scanner (pkg_name=/
# pkg_version=/pkg_source=/pkg_sha256=/pkg_depends=, daemon/src/pkg.c's
# parse_recipe()) AND sourced as a real POSIX shell script inside the
# isolated, network-less build container (". /build/recipe.sh" --
# daemon/include/pkg.h's own documented contract) to run pkg_build()/
# pkg_install() below. Never sourced or executed on the host itself.
#
# pkg_source/pkg_sha256 are multi-entry lists (ADR-0036), the whole
# reason this capability exists: lldap's web frontend compiles via
# Rust->WASM (wasm-pack, no Node.js needed), but its own real build
# (see its Dockerfile) also fetches 8 small external CDN files at
# image-build time -- Bootstrap CSS/JS, Bootstrap Icons, Font Awesome,
# and 3 font files -- that the isolated, network-less build container
# can't reach itself. Every one of those 8 files is verified two ways
# below: the recipe's own pkg_sha256 (cixd's real check before the
# build ever starts) AND, independently, their Subresource Integrity
# (SRI) sha384 hashes already hardcoded into lldap's own
# app/index_local.html -- confirmed directly, byte for byte, that
# every file this recipe fetches matches what lldap's own maintainers
# pinned, so the browser's own SRI enforcement will accept them as-is.
#
# Source 0 is lldap's real v0.6.3 release tag, fetched as GitHub's own
# generated source archive (lldap publishes no separate "-src" tarball
# the way gitea does) -- confirmed the archive is byte-for-byte
# deterministic for this tag (downloaded twice independently, same
# sha256 both times) before pinning pkg_sha256 to it.
#
# Needs the Rust toolchain staged (test_image_fixture_stage_toolchain(),
# test/test_image_fixture.c) -- including wasm-pack and the
# wasm32-unknown-unknown target, both pre-installed on the build host
# before staging, since installing either needs real network access
# (crates.io / Rust's own distribution server) the isolated build
# container doesn't have.
#
pkg_name="lldap"
pkg_version="0.6.3"
pkg_source="https://github.com/lldap/lldap/archive/refs/tags/v0.6.3.tar.gz https://cdn.jsdelivr.net/npm/bootstrap-dark-5@1.1.3/dist/css/bootstrap-nightshade.min.css https://cdn.jsdelivr.net/npm/bootstrap-dark-5@1.1.3/dist/js/darkmode.min.js https://cdn.jsdelivr.net/npm/bootstrap@5.1.1/dist/js/bootstrap.bundle.min.js https://cdn.jsdelivr.net/npm/bootstrap-icons@1.5.0/font/bootstrap-icons.css https://cdnjs.cloudflare.com/ajax/libs/font-awesome/4.7.0/css/font-awesome.min.css https://cdn.jsdelivr.net/npm/bootstrap-icons@1.5.0/font/fonts/bootstrap-icons.woff2 https://fonts.gstatic.com/s/bebasneue/v2/JTUSjIg69CK48gW7PXoo9Wdhyzbi.woff2 https://fonts.gstatic.com/s/bebasneue/v2/JTUSjIg69CK48gW7PXoo9Wlhyw.woff2"
pkg_sha256="2beb7840063bedd67d7afe891428ee7327eca162eee561ce75693af287d7ddf1 a38fcabce31baa46a74088442911b98f5d5a146607a607c1f19cbc9889c4cd25 4c91420d424894795d019a0e6a1506c66a2d731251895d99c0bdb3b70ff4b196 e5a12b84f9543d5ba3231837c2f2467563405aa66a582b6fc400985f85df49ad 3c325075337b768950583012228055ae392e384688d77ec5235e6ca88dcec6ef 799aeb25cc0373fdee0e1b1db7ad6c2f6a0e058dfadaa3379689f583213190bd 76506e128f2b47b7179f5037bd885a1674455ffeb6b5093cdb4c7eefbf436ce8 58bdaf33480d00d8c7eec1b0ee32e9f93f26ecfb05def7551044bc8f5cd0e2f3 dab7290ebc90b7ed3068b2921bf51e026225ad48e7b398b12321d036d340a458"
pkg_depends=""

# Run with $PWD already at /build/src (source 0, extracted -- one
# leading path component already stripped) and PATH=/usr/bin:/bin --
# no network access. CARGO_HOME/RUSTUP_HOME are set explicitly:
# rustup's own cargo/rustc are thin proxy binaries that look them up
# to find the active toolchain, and without them they'd default to
# $HOME/.rustup, which doesn't meaningfully exist in this isolated
# container -- confirmed directly during this recipe's own real local
# verification build. Builds the server (matching lldap's own
# Dockerfile exactly: server + migration-tool + set-password, not the
# whole workspace) and the WASM frontend (app/build.sh, lldap's own
# script), then copies the 8 pre-fetched CDN assets into the exact
# paths lldap's own Dockerfile puts them (5 libraries under
# app/static/, 3 fonts under app/static/fonts/).
pkg_build() {
	export CARGO_HOME=/usr/local/cargo
	export RUSTUP_HOME=/usr/local/rustup
	export PATH="/usr/local/cargo/bin:$PATH"
	# wasm-pack installs its own wasm-bindgen-cli (a version pinned by
	# this crate's own Cargo.lock, resolved via "cargo install") into
	# this cache dir on first use -- confirmed directly needing real
	# network access (crates.io) to do that install, which the
	# isolated build container doesn't have. Pointed at a path under
	# CARGO_HOME specifically so it's pre-populated once on the real
	# build host (the same host wasm-pack/the wasm32 target themselves
	# are pre-installed on, for the identical reason) and rides along
	# automatically with every existing /usr/local/cargo toolchain
	# staging, no separate mechanism needed.
	export WASM_PACK_CACHE=/usr/local/cargo/wasm-pack-cache

	cargo build --release -p lldap -p lldap_migration_tool -p lldap_set_password
	./app/build.sh

	cp /build/extra/bootstrap-nightshade.min.css /build/extra/darkmode.min.js \
	   /build/extra/bootstrap.bundle.min.js /build/extra/bootstrap-icons.css \
	   /build/extra/font-awesome.min.css app/static/
	cp /build/extra/bootstrap-icons.woff2 /build/extra/JTUSjIg69CK48gW7PXoo9Wdhyzbi.woff2 \
	   /build/extra/JTUSjIg69CK48gW7PXoo9Wlhyw.woff2 app/static/fonts/
}

# PKG_DESTDIR is set by cixd itself (daemon/src/pkg.c) -- everything
# written under it is what actually gets merged into the target image
# once this build container exits successfully. Installed as one
# self-contained directory under /opt/lldap -- lldap resolves its own
# app/ directory relative to its current working directory (no
# "static assets path" config option of its own), so the binary and
# its web assets have to stay co-located, the same layout lldap's own
# Dockerfile already uses (WORKDIR /app). Running it for real means
# `cd /opt/lldap && ./lldap run --config-file <path>` -- the working
# directory matters, the same "WorkPath" consideration gitea's own
# deployment already has.
pkg_install() {
	dir="$PKG_DESTDIR/opt/lldap"
	mkdir -p "$dir/app/pkg" "$dir/app/static/fonts"
	cp target/release/lldap target/release/lldap_migration_tool target/release/lldap_set_password "$dir/"
	cp app/index_local.html "$dir/app/index.html"
	cp -r app/pkg/. "$dir/app/pkg/"
	cp -r app/static/. "$dir/app/static/"
}
