#!/usr/bin/env python3
"""Tests for src/VivifyPerfStats.cpp. Build with:
    g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I include -o /tmp/perfstats \\
        tools/perfstats/main.cpp src/VivifyPerfStats.cpp
    VIVIFY_PERFSTATS=/tmp/perfstats python3 tools/perfstats/run_tests.py
"""
import os, re, subprocess, sys

BIN = os.environ.get("VIVIFY_PERFSTATS", "/tmp/perfstats")
failures = 0


def check(name, cond, detail=""):
    global failures
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else " " + str(detail)))
    if not cond:
        failures += 1


def run(hz, frames):
    r = subprocess.run([BIN, str(hz)], input="\n".join(frames) + "\n", capture_output=True, text=True, timeout=60)
    if "AddressSanitizer" in r.stderr or "runtime error" in r.stderr:
        raise AssertionError(r.stderr[:800])
    return r.stdout.splitlines()


def find(lines, prefix):
    return next((l for l in lines if l.startswith(prefix)), "")


# 1000 steady 11.1 ms frames at 90 Hz, then a few hitches.
frames = ["11.1 %.3f 0" % (i / 90) for i in range(1000)]
frames += ["22.2 20.0 0 two refreshes", "2112.0 61.07 1 InstantiatePrefab at beat 132", "skip", "skip",
           "1200.0 62.22 0 Blit", "350.5 70.0 2"]
lines = run(90, frames)
check("header names the map", lines[0] == "=== song performance: test map ===", lines[:1])
check("frames and skipped counted", "1004 frames measured, 2 left out (paused)" in find(lines, "song "),
      find(lines, "song "))
check("budget from the given refresh rate", find(lines, "refresh").startswith("refresh 90 Hz, frame budget 11.1 ms"),
      find(lines, "refresh"))
ft = find(lines, "frame time")
check("median at 0.1 ms resolution", "median 11.2" in ft, ft)  # 11.1 lands in the 11.1-11.2 bin
check("max is exact past the histogram", "max 2112.0" in ft, ft)
check("99th percentile ignores four hitches in a thousand", "99th 11.2" in ft, ft)
over = find(lines, "frames that missed")
check("frames over 1.2x budget counted", "(over 13.3 ms): 4 (" in over, over)
worst = [l for l in lines if re.match(r"\s+\d+\.", l)]
check("worst list is sorted longest first", worst and "2112.0 ms at song 61.07s" in worst[0] and
      "1200.0 ms" in worst[1], worst[:3])
check("gc change and label kept", "1 garbage collection(s) during it; InstantiatePrefab at beat 132" in worst[0],
      worst[0] if worst else "")
check("no label says so", any("350.5 ms" in l and "no Vivify event or breadcrumb" in l for l in worst), worst)
check("at most ten worst frames", len(worst) == 10, len(worst))
calls = int(find(lines, "label calls").split(": ")[1])
check("labels are written only for frames entering the worst list", calls < 40, calls)
gc = find(lines, "garbage collections")
check("gc and heap context", "4 at start, 7 at end (3 during the song)" in gc and "60.2MB of 70.0MB at start" in gc
      and "? of ? at end" in gc, gc)
check("prefab counts", "2 at end (peak 5)" in find(lines, "Vivify prefab"), find(lines, "Vivify prefab"))

lines = run(0, ["13.9 %.3f 0" % (i / 72) for i in range(500)])
check("unknown refresh rate estimated from the median", "refresh 72 Hz (not reported" in find(lines, "refresh"),
      find(lines, "refresh"))
lines = run(0, ["8.3 %.3f 0" % (i / 120) for i in range(500)])
check("120 Hz estimated", find(lines, "refresh").startswith("refresh 120 Hz"), find(lines, "refresh"))

lines = run(90, [])
check("no frames: no statistics, no crash", not find(lines, "frame time") and find(lines, "song "), lines)
lines = run(90, ["nan 1 0", "-5 1 0", "11.1 1 0"])
check("NaN and negative frames refused", "1 frames measured" in find(lines, "song "), find(lines, "song "))
lines = run(90, ["11.1 %.3f 0 %s" % (i, "x" * 400) for i in range(3)])
check("long labels are cut, not overrun", all(len(l) < 300 for l in lines), max(map(len, lines)))

print("\n%d failure(s)" % failures)
sys.exit(1 if failures else 0)
