#
# wireless-regdb -- the regulatory database cfg80211 loads to learn
# which radio channels and transmit powers are legal where (#30).
#
# THIS IS NOT A FIRMWARE BLOB, AND THE DIFFERENCE IS THE WHOLE POINT.
#
# rtw88-firmware ships a binary this platform cannot build, and says so
# at length because it is an owner-approved exception to the Build
# Provenance Mandate. This package is the opposite case and deserves to
# be read that way: `regulatory.db` is GENERATED from `db.txt`, a plain
# text file in this tarball, by `db2fw.py`, a script in this same
# tarball whose only imports are io, struct, hashlib, sys, math and its
# own bundled dbparse. No third-party crypto, no vendor tooling,
# nothing that cannot run on a Cix host.
#
# So Cix builds it, and pkg_build() below proves the claim rather than
# asserting it: it regenerates the database from db.txt and compares
# against the one upstream shipped. Byte-identical (verified in the dev
# sandbox before this recipe was written: both
# 7e236caecd939c8ec98be4870bf30422f28ffef2565a38aaaa2d9ddabd0c2641),
# which means the generation is deterministic and this platform's copy
# is genuinely its own build rather than a repackaged download.
#
# THE .p7s SHIPS TOO, AND THAT IS LOAD-BEARING. The kernel demands a
# signature whenever CFG80211_REQUIRE_SIGNED_REGDB is set, and on this
# platform it always is -- not by choice but by construction: that
# symbol's prompt is conditional on CFG80211_CERTIFICATION_ONUS, so
# with that off it has no prompt, cannot be set from .config, and
# olddefconfig forces it to its `default y` (measured directly,
# probe-wifi-driver/4, after a config line trying to disable it turned
# out to do nothing).
#
# Which is fine, because the signature is real protection worth having
# and it costs nothing here: upstream signs regulatory.db with a key
# already carried in the kernel's own net/wireless/certs/, and this
# recipe reproduces regulatory.db byte-for-byte from db.txt -- so
# upstream's detached signature validates over OUR build. Ship the .db
# we built and the .p7s upstream published, and the kernel is satisfied
# without this platform holding a signing key.
#
# Note what would break silently without the byte-for-byte check in
# pkg_build(): a .db that differed from upstream's by even one byte
# would fail signature verification at boot, cfg80211 would fall back
# to the world domain, and the only symptom would be an access point
# refusing channels it should allow.
#
# WHY IT MATTERS FOR AN ACCESS POINT. Without a regulatory database
# cfg80211 falls back to its built-in world domain, which is the most
# restrictive interpretation of every band: fewer channels, lower
# power, and no DFS. An AP can fail to start on a channel that is
# perfectly legal here and give no reason beyond a bare failure from
# hostapd. This file is what makes the radio's own idea of what is
# permitted match the country it is actually in.
#
# It lands in /lib/firmware because that is where request_firmware()
# looks (net/wireless/reg.c calls request_firmware_nowait for
# "regulatory.db"), not because it is firmware in any meaningful sense.
#
pkg_name="wireless-regdb"
pkg_version="2026.09.03"
pkg_source="https://mirrors.kernel.org/pub/software/network/wireless-regdb/wireless-regdb-2026.09.03.tar.xz"
pkg_sha256="b22e0901227b820cd1c280abe681a15b773a5103a5e10dc442e94ebb34cbf58d"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils python"
pkg_changelog="2026.09.03: the regulatory database, BUILT from db.txt by the tarball's own stdlib-only db2fw.py rather than shipped as a blob -- and the build proves it by regenerating and comparing byte-for-byte against upstream's copy. Needed so an access point is not stuck on cfg80211's most-restrictive built-in world domain. Ships upstream's regulatory.db.p7s alongside: this kernel always requires a signed database (the symbol is promptless and forced to its default y), and upstream's signature validates over our build precisely because we reproduce it byte-for-byte."

pkg_build() {
	PY=""
	for c in python3 python /usr/bin/python3 /usr/bin/python; do
		if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
	done
	[ -n "$PY" ] || {
		echo "wireless-regdb: no python interpreter found" >&2
		exit 1
	}
	echo "interpreter: $PY ($("$PY" --version 2>&1))"

	[ -f db.txt ] && [ -f db2fw.py ] || {
		echo "wireless-regdb: db.txt or db2fw.py missing from the source tree" >&2
		ls >&2
		exit 1
	}

	# Keep upstream's copy aside, then build our own from the text
	# source and compare. This is the whole provenance argument for
	# this package, so it is a hard gate rather than an echo.
	if [ -f regulatory.db ]; then
		mv regulatory.db upstream-regulatory.db
	fi

	"$PY" ./db2fw.py regulatory.db db.txt || {
		echo "wireless-regdb: generating regulatory.db from db.txt failed" >&2
		exit 1
	}
	[ -s regulatory.db ] || {
		echo "wireless-regdb: generated regulatory.db is empty" >&2
		exit 1
	}

	if [ -f upstream-regulatory.db ]; then
		ours=$(sha256sum regulatory.db | cut -d' ' -f1)
		theirs=$(sha256sum upstream-regulatory.db | cut -d' ' -f1)
		echo "built:    $ours"
		echo "upstream: $theirs"
		[ "$ours" = "$theirs" ] || {
			echo "wireless-regdb: our build does not reproduce upstream's regulatory.db." >&2
			echo "  That means generation is not deterministic here, or db.txt and the" >&2
			echo "  shipped binary disagree. Either way this package's provenance claim" >&2
			echo "  is false, so it fails rather than shipping the difference silently." >&2
			exit 1
		}
		echo "reproduced upstream byte-for-byte"
	else
		echo "note: upstream shipped no prebuilt regulatory.db to compare against"
	fi

	# The database carries its own magic; check it for the same reason
	# rtw88-firmware does. A checksum proves the bytes are the ones
	# approved, never that they are the right kind of thing -- and a
	# cgit HTML error page once passed for a firmware file during this
	# very investigation.
	head -c 4 regulatory.db | od -An -c | tr -d ' \n' | grep -q 'RGDB' || {
		echo "wireless-regdb: generated file does not start with the RGDB magic" >&2
		head -c 16 regulatory.db | od -An -tx1 >&2
		exit 1
	}
	echo "magic ok: RGDB"
}

pkg_install() {
	dir="$PKG_DESTDIR/lib/firmware"
	mkdir -p "$dir"
	cp regulatory.db "$dir/regulatory.db"
	chmod 0644 "$dir/regulatory.db"
	echo "installed $(wc -c < "$dir/regulatory.db") bytes to /lib/firmware/regulatory.db"

	# The detached signature, upstream's own, over the bytes we just
	# proved we reproduce. Without it the kernel refuses the database
	# and silently falls back to the world regulatory domain.
	[ -f regulatory.db.p7s ] || {
		echo "wireless-regdb: upstream shipped no regulatory.db.p7s, which this kernel requires" >&2
		exit 1
	}
	cp regulatory.db.p7s "$dir/regulatory.db.p7s"
	chmod 0644 "$dir/regulatory.db.p7s"
	echo "installed $(wc -c < "$dir/regulatory.db.p7s") bytes to /lib/firmware/regulatory.db.p7s"
}
