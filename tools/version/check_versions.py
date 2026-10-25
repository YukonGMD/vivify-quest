#!/usr/bin/env python3
"""Checks the version scheme (naomi.10): mod.json, qpm.json and
qpm_defines.cmake agree, the version is valid SemVer, and it sorts the way
ModsBeforeFriday (SemVer 2.0 precedence) will sort it.

Betas are 0.14.25-naomi.N-beta.M. A release is 0.14.25-naomi.N-release, not
0.14.25-naomi.N: under SemVer a numeric identifier ranks below an
alphanumeric one, so "naomi.10" would sort *below* "naomi.10-beta.1".
"-release" sorts above every "-beta.M" and below the next version's betas.
(Identifiers like "10-beta" compare as text, so naomi.100 would sort below
naomi.99; far off.)
"""
import json
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$")


def key(version):
    """A sort key with SemVer 2.0 precedence."""
    m = SEMVER.match(version)
    if not m:
        raise ValueError("not SemVer: " + version)
    core = tuple(int(m.group(i)) for i in (1, 2, 3))
    if m.group(4) is None:
        return core, (1,)  # a release outranks every pre-release of it
    ids = []
    for part in m.group(4).split("."):
        # Numeric identifiers compare as numbers and rank below alphanumeric ones.
        ids.append((0, int(part), "") if part.isdigit() else (1, 0, part))
    return core, (0, ids)


failures = 0


def check(name, ok, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)))
    failures += 0 if ok else 1


order = ["0.14.25-naomi.9", "0.14.25-naomi.10-beta.1", "0.14.25-naomi.10-beta.2", "0.14.25-naomi.10-beta.10",
         "0.14.25-naomi.10-release", "0.14.25-naomi.11-beta.1", "0.14.25-naomi.11-release", "0.14.25"]
check("the scheme sorts in release order", sorted(order, key=key) == order, sorted(order, key=key))
check("a bare naomi.10 would sort below its betas (why releases say -release)",
      key("0.14.25-naomi.10") < key("0.14.25-naomi.10-beta.1"))

mod = json.load(open(os.path.join(ROOT, "mod.json")))["version"]
qpm = json.load(open(os.path.join(ROOT, "qpm.json")))["info"]["version"]
defines = re.search(r'set\(MOD_VERSION "([^"]+)"\)', open(os.path.join(ROOT, "qpm_defines.cmake")).read()).group(1)
check("mod.json, qpm.json and qpm_defines.cmake agree", mod == qpm == defines, (mod, qpm, defines))
check("the version is SemVer", SEMVER.match(mod) is not None, mod)
check("the version follows the scheme", re.match(r"^0\.14\.25-naomi\.\d+(-beta\.\d+|-release)?$", mod) is not None, mod)
check("the version sorts above naomi.9", key(mod) > key("0.14.25-naomi.9"), mod)
print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
