#!/bin/bash
#
# carry-artifact-approvals.sh -- move pkg_artifact_sha256= from a
# daemon's recipe store back into this checkout.
#
# approve_published_artifact() (#306) writes the approval into the
# DAEMON's copy of a recipe at publish time, the one moment the bytes
# are known to be both built here and accepted by the cache. Nothing
# carries it back to git, and git is what a freshly installed host
# syncs from -- so that host rebuilds from source exactly the packages
# whose finished artifacts are already in the cache. Measured on
# 2026-09-11: 16 of 107 packages, including glibc, openssl, zlib,
# linux-headers and linux-pam (#403).
#
# Direction matters, and only one direction works by itself.
# pkg_recipe_add() accepts an incoming recipe that differs from the
# stored one by EXACTLY ONE added pkg_artifact_sha256= line and nothing
# else (recipe_adds_only_artifact_sha256(), daemon/src/pkg.c), so an
# approval that reaches git reaches the whole fleet on its next sync.
# Nothing carries one the other way. This script is that other way.
#
# The one-line rule is literal. glibc 2.44-6's approval sat in git from
# 2026-08-30 to 2026-09-11 and never propagated, because the commit
# that added it added an eleven-line comment beside it; a full sync
# (added 27, skipped 1255) left the box unapproved, and the next sync
# after the comment was removed applied it. So this writes a bare line
# and nothing else -- put the explanation in the commit message.
#
# Two checks before any file is written, because an approval is a
# statement that specific bytes are trusted:
#
#   1. the daemon's copy minus its approval line must be byte-identical
#      to this checkout's copy -- otherwise the two have diverged for
#      some other reason and that is the thing to look at first
#   2. the checksum must equal what the artifact server records for
#      that exact (name, version, release) -- never carried forward
#      from another revision, never computed locally
#
# Scope. By default this carries the LATEST revision of each package,
# because that is the one a freshly installed host installs and so the
# only one the cold-box gap is actually about. --all takes every
# revision, which on 2026-09-11 meant 76 files of which 64 were historic
# cix release recipes and 6 were throwaway probe-* diagnostics -- noise
# that closes no gap. probe-* recipes are skipped in both modes.
#
# Usage: tools/carry-artifact-approvals.sh [--all] <host> <token> [cache-url]
#   e.g. tools/carry-artifact-approvals.sh 192.168.15.95 "$(cat tok)"
#
# Writes nothing without both checks passing, prints every decision,
# and leaves the result staged for a human to read as a diff.

set -u

ALL=0
if [ "${1:-}" = "--all" ]; then
	ALL=1
	shift
fi

HOST="${1:-}"
TOKEN="${2:-}"
CACHE="${3:-http://192.168.15.31:8080}"

if [ -z "$HOST" ] || [ -z "$TOKEN" ]; then
	echo "usage: $0 [--all] <host> <token> [cache-url]" >&2
	exit 2
fi

if [ ! -d recipes/package ]; then
	echo "run from the repository root (recipes/package not found)" >&2
	exit 2
fi

api() { curl -sk -H "Authorization: Bearer $TOKEN" "https://$HOST/v1$1"; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

api /pkg/recipes > "$WORK/recipes.json" || { echo "cannot reach $HOST" >&2; exit 1; }
curl -s "$CACHE/api/v1/artifacts" > "$WORK/artifacts.json" || {
	echo "cannot reach the artifact server at $CACHE" >&2; exit 1; }

python3 - "$WORK" "$HOST" "$TOKEN" "$ALL" <<'PY'
import json, os, re, ssl, sys, urllib.request

work, host, token = sys.argv[1], sys.argv[2], sys.argv[3]
every = sys.argv[4] == "1"
ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

def body(name, version):
    req = urllib.request.Request(
        "https://%s/v1/pkg/recipes/%s?version=%s" % (host, name, version),
        headers={"Authorization": "Bearer " + token})
    return json.load(urllib.request.urlopen(req, context=ctx, timeout=60))["content"]

arts = json.load(open(os.path.join(work, "artifacts.json")))
if isinstance(arts, dict):
    arts = arts.get("artifacts", [])
cache = {}
for a in arts:
    cache[(a.get("artifact"), str(a.get("version")), str(a.get("release")))] = a.get("sha256")

recipes = json.load(open(os.path.join(work, "recipes.json")))["recipes"]
recipes = [r for r in recipes if not r["name"].startswith("probe-")]

if not every:
    def key(v):
        return [int(x) if x.isdigit() else x
                for x in re.split(r"[-.]", v.lstrip("v"))]
    latest = {}
    for r in recipes:
        cur = latest.get(r["name"])
        try:
            newer = cur is None or key(r["version"]) > key(cur["version"])
        except TypeError:
            newer = cur is None or r["version"] > cur["version"]
        if newer:
            latest[r["name"]] = r
    recipes = list(latest.values())

written = skipped = diverged = uncached = 0
for r in recipes:
    name, rev = r["name"], r["version"]
    path = "recipes/package/%s/%s/build.sh" % (name, rev)
    if not os.path.exists(path):
        continue
    local = open(path).read()
    if "pkg_artifact_sha256=" in local:
        continue
    try:
        remote = body(name, rev)
    except Exception as exc:
        print("  UNREADABLE %s %s: %s" % (name, rev, exc))
        continue
    lines = [l for l in remote.split("\n") if l.startswith("pkg_artifact_sha256=")]
    if len(lines) != 1:
        continue
    if "\n".join(l for l in remote.split("\n")
                 if not l.startswith("pkg_artifact_sha256=")) != local:
        print("  DIVERGED   %s %s -- differs beyond the approval line, not touched"
              % (name, rev))
        diverged += 1
        continue
    sha = lines[0].split('"')[1]
    version, _, release = rev.rpartition("-")
    if not version:
        version, release = rev, "1"
    recorded = cache.get((name, version, release))
    if recorded != sha:
        print("  UNCACHED   %s %s -- recipe says %s, artifact server says %s"
              % (name, rev, sha[:12], str(recorded)[:12]))
        uncached += 1
        continue
    open(path, "w").write(local.rstrip("\n") + "\n" + lines[0] + "\n")
    print("  carried    %s %s  %s" % (name, rev, sha[:12]))
    written += 1

print("\ncarried %d, diverged %d, not in cache %d" % (written, diverged, uncached))
if written:
    print("review with: git diff -- recipes/package")
PY
