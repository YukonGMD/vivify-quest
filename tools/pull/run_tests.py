#!/usr/bin/env python3
"""Tests for pull_sessions.py's planning: what is pulled, what is never
overwritten, and how an older copy of a changed pointer file is kept."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pull_sessions  # noqa: E402

failures = 0


def check(name, condition, detail=""):
    global failures
    if condition:
        print("PASS " + name)
    else:
        failures += 1
        print("FAIL " + name + ("  " + str(detail) if detail else ""))


A = "Vivify_2026-10-25_01-00-00_naomi.10-beta.1-89f98f5.txt"
B = "Vivify_2026-10-25_02-00-00_naomi.10-beta.1-89f98f5.txt"
remote = {A: 100, B: 500, "index.txt": 80, "latest.txt": 60, "blackbox.bin": 524544, "last-summary.txt": 900}
actions = pull_sessions.plan(remote, {})
check("first pull: every session and pointer, not the black box",
      sorted(t for _, t in actions) == sorted([A, B, "index.txt", "latest.txt", "last-summary.txt"]), actions)
actions = pull_sessions.plan(remote, {A: 100, B: 500})
check("already pulled, same size: skipped", all(t not in (A, B) for _, t in actions), actions)
actions = pull_sessions.plan(remote, {A: 100, B: 300})
check("a session that grew is pulled beside the old copy, never over it",
      (B, B[:-4] + "~500.txt") in actions and all(t != B for _, t in actions), actions)
actions = pull_sessions.plan(remote, {A: 100, B: 300, B[:-4] + "~500.txt": 500})
check("and only once", all(not t.startswith(B[:-4]) for _, t in actions), actions)

with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "index.txt")
    with open(path, "w") as f:
        f.write("old")
    pull_sessions.keep_old_copy(path)
    kept = [n for n in os.listdir(tmp) if n.startswith("index-")]
    check("a replaced pointer file's old copy is kept", not os.path.exists(path) and len(kept) == 1, os.listdir(tmp))
    with open(path, "w") as f:
        f.write("older")
    os.utime(path, (os.path.getmtime(os.path.join(tmp, kept[0])),) * 2)
    pull_sessions.keep_old_copy(path)
    check("two old copies with the same time both kept",
          len([n for n in os.listdir(tmp) if n.startswith("index-")]) == 2, os.listdir(tmp))

print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
