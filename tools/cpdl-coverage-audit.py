#!/usr/bin/env python3
#
# cpdl-coverage-audit.py -- what would it take to express this recipe
# corpus in CPDL?
#
# Answers cix-build-system#142 from the consumer's side, and it is
# deliberately a script rather than a prose survey: the numbers move
# every time a recipe is revised, and an audit nobody can re-run is a
# claim with a date on it rather than a measurement.
#
# Run from the repository root:
#
#     python3 tools/cpdl-coverage-audit.py
#
# What it does. For the LATEST revision of every package it extracts the
# bodies of pkg_prepare/pkg_build/pkg_check/pkg_install -- the only part
# a CPDL recipe would have to express, since cixd already owns fetch,
# verify, extract, finalize and package -- and classifies every
# non-comment line.
#
# Two things it is careful about, both of which change the answer:
#
#   - heredoc CONTENT is excluded. A recipe that writes a C file and
#     compiles it is `write` plus `run` in CPDL; counting the C source
#     as shell inflates the corpus and invents constructs that are not
#     there. An earlier pass without this reported `int`, `rc`, `args`
#     and `import` among the commands recipes invoke.
#   - probe-* recipes and the *-tests harnesses are separated out. They
#     are diagnostic scaffolding run on a box, not packages that ship,
#     and they are where the gnarliest shell lives. Counting them makes
#     the migration look far worse than it is.
#
import glob
import os
import re
import sys
import collections

PHASE = re.compile(r"^pkg_(build|install|check|prepare)\(\)")

# A line needing something CPDL 0.1 has no word for.
HARD = re.compile(
    r"\$\((?!nproc\))|`"                      # command substitution
    r"|^\s*(for|while|until|case)\s"          # loops and branches
    r"|[^|&>]\|[^|]"                          # pipelines
    r"|(?<![0-9&])>>?\s*[^&\s]|2>"            # redirection
    r"|^\s*\("                                # subshells
)

# What a hard line is actually TRYING to do. Order matters: the first
# match wins, most specific first.
INTENT = [
    ("assert on command OUTPUT", re.compile(
        r"\|\s*grep|grep -q|\|\s*sed -n|2>&1\s*\||\$\([^)]*\)\s*(=|!=|-eq)|\[\s*\"\$\(")),
    ("capture output as a VALUE", re.compile(
        r"^\s*(local\s+)?[A-Za-z_][A-Za-z0-9_]*=\$\(|\$\((?!nproc\))[^)]*\)")),
    ("assert over a SET of files", re.compile(
        r"^\s*for\s+\w+\s+in\s.*(\*|\.o|\.so|\.a|\.h|lib|/)")),
    ("generate a file from a LIST", re.compile(
        r"^\s*for\s.*;\s*do\s*$|>>\s*\S|:\s*>\s*\S")),
    ("redirect output to a LOG", re.compile(
        r">\s*/(run|build|tmp)/\S+|2>\s*/(run|build|tmp)/\S+|2>&1")),
    ("branch on a VALUE (case)", re.compile(r"^\s*case\s")),
    ("subshell for scoped cd", re.compile(r"^\s*\(")),
]

# Constructs CPDL 0.1 already has a word for, so they are translation
# work rather than language work.
MECHANICAL = [
    ("$(nproc)", re.compile(r"\$\(nproc\)"), "$jobs"),
    ("if/test guard then exit", re.compile(r"^\s*if\s+.*\[|^\s*\[\s"), "require ... { exists }"),
    ("&& / ||", re.compile(r"&&|\|\|"), "separate run operations"),
    ("heredoc writing a file", re.compile(r"<<-?\s*['\"]?[A-Za-z_]"), "write"),
    ("export VAR=", re.compile(r"^\s*export\s"), "env"),
    ("glob argument", re.compile(r"[\s=][^\s\"']*[*?][^\s\"']*"), "glob value"),
]


def phase_bodies(path):
    """Lines inside the phase functions, with heredoc content removed."""
    out = []
    inside = heredoc = False
    tag = None
    for line in open(path, errors="replace").read().split("\n"):
        if PHASE.match(line):
            inside = True
            continue
        if inside and line == "}":
            inside = False
            continue
        if not inside:
            continue
        if heredoc:
            if line.strip() == tag:
                heredoc = False
            continue
        opener = re.search(r"<<-?\s*'?\"?([A-Za-z_][A-Za-z0-9_]*)", line)
        if opener:
            heredoc, tag = True, opener.group(1)
        out.append(line)
    return [l for l in out if l.strip() and not l.strip().startswith("#")]


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "recipes/package"
    revisions = collections.defaultdict(list)
    for path in glob.glob(os.path.join(root, "*", "*", "build.sh")):
        revisions[path.split(os.sep)[-3]].append(path)
    if not revisions:
        sys.exit("no recipes under %s -- run from the repository root" % root)

    bodies = {pkg: phase_bodies(sorted(v)[-1]) for pkg, v in revisions.items()}
    scaffolding = {p for p in bodies
                   if p.startswith("probe-") or p.endswith("-tests")
                   or p == "cix-aggressive-test"}
    real = sorted(set(bodies) - scaffolding)

    hard_pkgs, clean_pkgs = [], []
    intent_lines = collections.Counter()
    intent_pkgs = collections.defaultdict(set)
    mech_lines = collections.Counter()
    mech_pkgs = collections.defaultdict(set)

    for pkg in real:
        hard_here = 0
        for line in bodies[pkg]:
            for label, rx, _ in MECHANICAL:
                if rx.search(line):
                    mech_lines[label] += 1
                    mech_pkgs[label].add(pkg)
            if not HARD.search(line):
                continue
            hard_here += 1
            for label, rx in INTENT:
                if rx.search(line):
                    intent_lines[label] += 1
                    intent_pkgs[label].add(pkg)
                    break
        (hard_pkgs if hard_here else clean_pkgs).append((pkg, hard_here))

    total = sum(len(bodies[p]) for p in real)
    print("=" * 78)
    print("CPDL 0.1 COVERAGE AUDIT")
    print("=" * 78)
    print("%d packages (%d probe/test harnesses excluded), %d phase-body lines"
          % (len(real), len(scaffolding), total))
    print()
    print("CONVERT MECHANICALLY: %d of %d packages" % (len(clean_pkgs), len(real)))
    print("NEED A CPDL ANSWER:   %d of %d packages" % (len(hard_pkgs), len(real)))
    print()
    print("Already expressible, so translation work rather than language work:")
    print("  %-26s %6s %6s   %s" % ("construct", "lines", "pkgs", "CPDL"))
    for label, _, cpdl in MECHANICAL:
        if mech_lines[label]:
            print("  %-26s %6d %6d   %s"
                  % (label, mech_lines[label], len(mech_pkgs[label]), cpdl))
    print()
    print("Not expressible -- by what the line is actually doing:")
    print("  %-30s %6s %6s" % ("intent", "lines", "pkgs"))
    for label, _ in INTENT:
        if intent_lines[label]:
            print("  %-30s %6d %6d"
                  % (label, intent_lines[label], len(intent_pkgs[label])))
    print()
    print("Packages needing an answer, heaviest first:")
    for pkg, n in sorted(hard_pkgs, key=lambda x: -x[1]):
        print("  %-24s %3d hard lines" % (pkg, n))


if __name__ == "__main__":
    main()
