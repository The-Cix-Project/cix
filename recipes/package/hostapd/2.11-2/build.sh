#
# hostapd -- a full-featured IEEE 802.11 AP/authenticator daemon
# (w1.fi/hostap), for Cix's own wireless-AP container use case
# (issue #25, feeding into issue #30's real ar-1 example container).
#
# Read by cixd's own non-executing metadata scanner (pkg_name=/
# pkg_version=/pkg_source=/pkg_sha256=/pkg_depends=, daemon/src/pkg.c's
# parse_recipe()) AND sourced as a real POSIX shell script inside the
# isolated, network-less build container (". /build/recipe.sh") to run
# pkg_build()/pkg_install() below. Never sourced or executed on the
# host itself.
#
# pkg_source is NOT plain upstream hostapd -- issue #25's own explicit
# ask was original w1.fi source (never a distro-repackaged fork) with
# real patches layered on top for max flexibility, OpenWrt's own
# well-maintained patch series specifically called out. This tarball is
# a real, off-box, reproducible assembly (this project's own build
# containers have no network access, matching glauth.recipe's own
# precedent for exactly this shape of problem):
#
#   1. curl -fsSL -o hostapd-2.11.tar.gz https://w1.fi/releases/hostapd-2.11.tar.gz
#      (sha256 2b3facb632fd4f65e32f4bf82a76b4b72c501f995a4f62e330219fe7aed1747a,
#      the real upstream release tarball, verified directly)
#   2. Sparse-clone github.com/openwrt/openwrt's own
#      package/network/services/hostapd/patches/ (they carry 59 patches
#      as of this writing, against their own pinned hostap.git commit,
#      not the 2.11 tag -- 30 apply cleanly as-is against the clean
#      2.11 release, the other 29 are either already upstreamed into
#      2.11 or depend on earlier patches in their own stack not applied
#      here, e.g. their mbedtls CONFIG_TLS port, deliberately not used
#      -- see step 4 below for why).
#   3. Of those 30, individually read (not just filename/header-
#      keyword-matched -- that crude a filter produces real false
#      negatives, e.g. 610-hostapd_cli_ujail_permission.patch applies
#      cleanly but is genuinely procd-ujail-specific) and curated down
#      to 23 kept, 7 dropped:
#
#      KEPT (real upstream-quality fix/hardening, or a real feature
#      relevant to a generic deployment -- no OpenWrt-infra coupling):
#        011-mesh-use-deterministic-channel-on-channel-switch (DFS/mesh
#          channel-switch determinism, avoids random-channel mesh split)
#        021-fix-sta-add-after-previous-connection (real bugfix, STA
#          re-add after a missed deauth/disassoc)
#        051-nl80211-add-extra-ies-only-if-allowed-by-driver (driver-
#          quirk compat, broadcom-wl-class drivers)
#        052-AP-add-missing-null-pointer-check-in-hostapd_free_ha
#          (real NULL-deref hardening)
#        070-netlink-increase-buffer-size (real robustness fix for
#          high-event-volume netlink buffer overrun)
#        150-add-NULL-checks-encountered-during-tests-hwsim (real
#          NULL-check hardening, SAE/DPP)
#        160-dpp_pkex-EC-point-mul-w-value-prime (DPP/WPA3 correctness
#          fix -- commit message says "with mbedtls requires..." but
#          the actual code change is backend-agnostic, a modular
#          reduction before EC point mul that's mathematically correct
#          and harmless for any backend including OpenSSL; verified by
#          reading the diff, not the commit message)
#        170-hostapd-update-cfs0-and-cfs1-for-160MHz (real 802.11ax/
#          160MHz center-frequency correctness fix)
#        180-fix_owe_ssid_update (real OWE/WPA3-enhanced-open
#          transition-mode correctness fix)
#        300-noscan (generic noscan/ht_coex config options)
#        320-optional_rfkill (build-config simplification, optional
#          rfkill build dependency)
#        360-acs_retry (ACS retry-count hardening 15->50, matches
#          issue #25's own "ACS/DFS improvements" ask directly)
#        381-hostapd_cli_UNKNOWN-COMMAND (real bugfix -- infinite
#          100%-CPU loop in hostapd_cli on an unrecognized reply)
#        400-wps_single_auth_enc_type (real WPS Windows-7 interop fix)
#        410-limit_debug_messages (generic debug-log-volume control,
#          valuable given Cix's own consolidated log store,
#          ADR-0070/ADR-0126, that container stdout/stderr feeds)
#        465-hostapd-config-support-random-BSS-color (real config-
#          validation hardening, out-of-range HE BSS color)
#        470-survey_data_fallback (real ACS robustness fix for
#          incomplete survey data, matches the "ACS/DFS improvements"
#          ask)
#        720-iface_max_num_sta (generic device-level station-count
#          limit config option)
#        730-ft_iface (generic 802.11r inter-AP interface config
#          option)
#        760-dynamic_own_ip (generic RADIUS own-ip auto-config option)
#        761-shared_das_port (generic RADIUS DAS/CoA port-sharing fix)
#        762-AP-don-t-ignore-probe-requests-with-invalid-DSSS-par
#          (real driver/client-compat bugfix, Intel 8265 NIC, matches
#          the "driver quirks" ask directly)
#        803-hostapd-fix-80211be-build (real WiFi7/EHT build-
#          correctness fix)
#
#      DROPPED (OpenWrt-infra-coupled, or not applicable here):
#        050-Fix-OpenWrt-13156 -- REVERTS a real upstream DoS fix as an
#          mt7915-driver-specific workaround; Cix targets generic
#          hardware (a Realtek USB dongle, vendor:product 2357:012e),
#          reintroducing this DoS exposure for zero benefit is wrong.
#        250-hostapd_cli_ifdef -- solves an OpenWrt "mini vs full
#          build variant" split Cix has no equivalent of.
#        590-rrm-wnm-statistics, 600-ubus_support, 601-ucode_support,
#        610-hostapd_cli_ujail_permission -- explicitly ubus/ucode/
#          procd-ujail-specific (confirmed in each patch's own commit
#          message, not guessed).
#        710-vlan_no_bridge, 711-wds_bridge_force -- both explicitly
#          delegate VLAN-bridge management to netifd (OpenWrt's own
#          network config daemon); Cix has no netifd and its own,
#          completely different bridge/VLAN model
#          (netplane/src/rtnetlink.c, network_attach_interface()).
#
#   4. One further patch added from Debian's own wpa source package
#      (2:2.11-2, experimental) -- CVE-2024-5290-lib_engine_trusted_
#      path.patch, a real security fix (only load OpenSSL "engine"
#      shared libraries from a trusted path) directly relevant since
#      Cix builds hostapd against OpenSSL, not mbedtls (see step 5).
#      Debian's remaining patches are D-Bus/systemd/packaging-specific
#      or wpa_supplicant-client-side-only (this recipe builds hostapd,
#      the AP daemon, never wpa_supplicant) -- not relevant here.
#   5. CONFIG_TLS=openssl, not OpenWrt's own mbedtls port: cixd
#      itself already links libssl/libcrypto directly (CLAUDE.md's own
#      tech-stack notes, confirmed compiling/linking clean under TCC)
#      -- mbedtls exists in OpenWrt's own patch stack purely to shrink
#      an embedded router's flash footprint, a constraint that doesn't
#      apply here, and adding a second, separate crypto library for no
#      reason would be exactly the kind of parallel-implementation this
#      project's own Immutable Maxims forbid.
#   6. libnl-tiny (git.openwrt.org/project/libnl-tiny.git, a small,
#      dependency-free nl80211/genl helper library, OpenWrt's own
#      proven drop-in libnl-3/libnl-genl-3 replacement for exactly this
#      embedded/minimal-image use case) vendored directly into the
#      tarball under libnl-tiny/, built as a small .so alongside
#      hostapd itself (CONFIG_LIBNL_TINY=y) -- avoids needing a whole
#      new, heavier libnl3 package/dependency chain Cix has never
#      needed before, matching the exact reasoning that led OpenWrt to
#      the same choice. One small, real compat patch applied to it
#      directly (not upstream-unmodified -- justified below): its own
#      netlink-compat.h unconditionally (well, `#ifndef IFNAMSIZ`, but
#      that guard only helps if <net/if.h> was already included first)
#      redefines IFNAMSIZ, which glibc's own <net/if.h> also defines,
#      unguarded on its own side -- TCC treats ANY macro redefinition
#      as a hard error under -Werror regardless of value (confirmed:
#      GCC only warns, and only for a genuinely different value; TCC
#      doesn't make that distinction). Fixed by additionally gating the
#      vendored fallback on `!defined __linux__` -- glibc always
#      provides IFNAMSIZ on Cix's only target (Linux), so this
#      fallback was always dead code here regardless.
#   7. tar -czf hostapd-2.11.tarball hostapd-2.11/ (patches applied,
#      libnl-tiny vendored under hostapd-2.11/libnl-tiny/, matching
#      glauth.recipe's own precedent of shipping a fully-prepared,
#      already-patched, offline-buildable tree rather than a live
#      in-container patch-application step -- this project has no such
#      mechanism and, per this same investigation, doesn't need one).
#
# Full patch application AND the resulting build were both verified
# together, not separately assumed compatible: all 24 patches apply
# clean against the pristine 2.11 tree, and the fully-patched tree
# builds and links with zero errors/warnings under the exact toolchain
# below, producing a real, running `hostapd v2.11` binary.
pkg_name="hostapd"
pkg_version="2.11-2"
pkg_source="http://192.168.15.31:8920/hostapd-2.11.tarball"
pkg_sha256="7c2533bc322876fe5890d519c30578b50a48fb42034530973a270b5f8191c0a5"
pkg_depends="openssl"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils openssl"
pkg_changelog="2.11-2: declares its build tools so it can be rebuilt through the ordinary install path (#206)"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils openssl"

# TCC-by-default confirmed, not assumed (3-tier policy, CLAUDE.md) --
# hostapd is real, portable C, unlike the gcc/cgo class of genuine
# exception. Exactly one toolchain gap found and fixed: hostapd's own
# src/build.rules hardcodes `-MMD` (GCC/Clang dependency-file
# generation, excluding system headers) in CFLAGS; TCC only supports
# the plain `-MD` form. A thin toolwrap (matching chrony.recipe's own
# established pattern for an analogous single-flag gap) translates it;
# every other flag/construct in hostapd's own ~90 C files, including
# the nl80211 driver, DPP/WPA3, RADIUS, and the full EAP-TLS/PEAP/TTLS
# server stack, compiled clean with zero changes needed.
#
# Architectural note (issue #25's own explicit ask, verified against
# real code, not assumed): hostapd's driver_nl80211.c never shells out
# to ip/iw (confirmed: zero system("ip ...")/popen("ip ...") calls
# anywhere in src/) -- it talks to the kernel directly via real netlink
# sockets (NETLINK_GENERIC/nl80211 for wireless config, NETLINK_ROUTE
# for basic link state), the same direct-netlink philosophy Cix's own
# networking plane already follows. No conflict with that plane either:
# daemon/src/device.c's own enumerate_net_one()/container_net_host_
# attach_interfaces() move a physical netdev wholesale into a
# container's own network namespace -- once assigned, cixd's host-
# level rtnetlink code never touches that interface again ("it simply
# stops appearing here at all", that function's own doc comment) --  so
# hostapd running inside an AP container has real, exclusive,
# uncontested ownership of its own radio interface.
pkg_build() {
	export PATH="/usr/bin:$PATH"

	mkdir -p /build/toolwrap
	cat > /build/toolwrap/tcc-hostapd <<'WRAP'
#!/usr/bin/bash
args=()
for a in "$@"; do
	if [ "$a" = "-MMD" ]; then
		args+=("-MD")
	else
		args+=("$a")
	fi
done
exec tcc "${args[@]}"
WRAP
	chmod +x /build/toolwrap/tcc-hostapd

	SRCDIR="$(pwd)"

	# libnl-tiny first (a small .so hostapd's own nl80211 driver links
	# against) -- no ar/ranlib needed (this project's own build image
	# has neither by default, CLAUDE.md), a shared object is a single
	# tcc -shared invocation.
	mkdir -p libnl-tiny/build
	(cd libnl-tiny && tcc -shared -Wall -Werror -Iinclude *.c -o build/libnl-tiny.so)

	# A subshell, matching the libnl-tiny step above -- pkg_build() and
	# pkg_install() run in the same shell session (confirmed the hard
	# way: an earlier draft used a plain `cd hostapd` with no matching
	# `cd ..`, and its leaked CWD made pkg_install()'s own relative
	# paths resolve one directory too deep -- "cp: cannot stat
	# 'hostapd/hostapd': Not a directory", since CWD already *was*
	# hostapd/ and its own built binary is a file, not a dir, so
	# appending another /hostapd onto it isn't valid), so a plain `cd`
	# here would silently do the same to pkg_install() a second time.
	(
		cd hostapd
		cp defconfig .config
		sed -i \
			-e 's/^#CONFIG_TLS=openssl/CONFIG_TLS=openssl/' \
			-e 's/^CONFIG_LIBNL32=y/#CONFIG_LIBNL32=y/' \
			.config
		{
			echo "CONFIG_LIBNL_TINY=y"
			echo "CFLAGS += -I$SRCDIR/libnl-tiny/include -D_GNU_SOURCE"
			echo "LDFLAGS += -L$SRCDIR/libnl-tiny/build"
		} >> .config

		make CC=/build/toolwrap/tcc-hostapd -j1
	)
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin" "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp hostapd/hostapd "$PKG_DESTDIR/usr/bin/hostapd"
	cp hostapd/hostapd_cli "$PKG_DESTDIR/usr/bin/hostapd_cli"
	cp libnl-tiny/build/libnl-tiny.so "$PKG_DESTDIR/lib/x86_64-linux-gnu/libnl-tiny.so"
}
