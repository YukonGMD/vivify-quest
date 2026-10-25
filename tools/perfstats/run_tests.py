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

# Sections (naomi.7): a heavy intro, a light drop, then a section with live
# samples, at 120 Hz.
frames = ["S 0 intro"] + ["F 100.0 0.5 %.2f" % (i / 10) for i in range(250)]
frames += ["S 25 drop"] + ["F 8.3 0.2 %.3f" % (25 + i / 120) for i in range(1200)]
frames += ["S 35 drop"] + ["F 8.3 0.2 35"]  # the same section again: no new one
frames += ["S 36 ambient", "L 1147 300 111 5000 410 380 210000", "L 1000 200 90 6000 -1 -1 -1"]
frames += ["F 16.0 1.5 %d" % (36 + i) for i in range(30)] + ["F 40.0 3.0 70"]
lines = run(120, frames)
by = find(lines, "by section")
check("sections listed", by.startswith("by section (3)"), by)
intro = find(lines, "  'intro'")
check("a heavy section's numbers", "song 0.0-24.9s" in intro and "250 frames" in intro and "median 100 ms (10 fps)" in
      intro and "60 fps in 0%" in intro and "missed a refresh in 100%" in intro, intro)
check("Vivify's own work averaged", "Vivify's own work 0.50 ms a frame on average (max 0.5)" in intro, intro)
drop = find(lines, "  'drop'")
check("a light section holds 60 fps", "1201 frames" in drop and "60 fps in 100%" in drop and
      "missed a refresh in 0%" in drop, drop)
amb = find(lines, "  'ambient'")
check("live samples averaged, peaks kept", "live: 1074 renderers (250 in view, peak 300), peak 111 particle systems "
      "with 6000 particles" in amb, amb)
check("draw calls only from samples that had them", "draw calls 410 (peak 410), batches 380, triangles 210000" in amb,
      amb)
check("the share at 60 fps counts frames of 16 ms", "60 fps in 97%" in amb, amb)
lines = run(120, ["S 0 heavy"] + ["F 25 0.1 %.2f %.1f 98" % (i / 40, 20 + i % 10) for i in range(100)] +
            ["S 3 light", "F 8.3 0.1 3.1"])
heavy = find(lines, "  'heavy'")
check("sections: GPU time and GPU busy from the OVR plugin", "GPU time (OVR plugin) median 25 ms, 95th 29, max 29" in
      heavy and "GPU busy 98%" in heavy, heavy)
check("sections: no GPU numbers where none were given", "GPU" not in find(lines, "  'light'"), find(lines, "  'light'"))
lines = run(90, ["S %d s%d" % (i, i) for i in range(40)] + ["F 10 0 1"])
check("sections past the limit fold into the last", find(lines, "by section").startswith("by section (32)") and
      find(lines, "  'later sections'"), [l for l in lines if "section" in l][:3])
lines = run(120, frames + ["D 0", "D 2", "D 7", "D -1"])
check("a section described for the slow-frame summary", find(lines, "describe 0:") ==
      "describe 0: 'intro' (song 0.0-24.9s)" and find(lines, "describe 2:") == "describe 2: 'ambient' (song 36.0-70.0s)",
      [l for l in lines if l.startswith("describe")])
check("an unknown section is named by its number", find(lines, "describe 7:") == "describe 7: section 7" and
      find(lines, "describe -1:") == "describe -1: section -1", [l for l in lines if l.startswith("describe")])
lines = run(90, ["F 10 0 1", "L 1 1 1 1 1 1 1"])
check("frames before any section are ignored", not find(lines, "by section"), lines)

# Performance mode (naomi.8): the viewport controller.
def control(floor, target, frames):
    lines = run(90, ["V %s %s" % (floor, target)] + ["v %s %d %.4f %s %d" % f for f in frames])
    return [l for l in lines if l.startswith("scale ")]


def steady(ms, gpu, start, seconds, cap=1.0, hold=0, step=None):
    out, t = [], start
    dt = (step if step else max(ms, 8.33)) / 1000.0
    while t < start + seconds:
        t += dt
        out.append((ms, gpu, t, cap, hold))
    return out


def scales(changes):
    return [float(l.split()[1]) for l in changes]


changes = control(0.6, 8.33, steady(5.0, 1, 0, 10))
check("viewport: a light map stays at full size", changes == [], changes)
changes = control(0.6, 8.33, steady(20.0, 1, 0, 10, step=8.33))
values = scales(changes)
check("viewport: an overloaded GPU steps down to the floor and stops there", values and values[-1] == 0.6 and
      min(values) == 0.6, changes)
check("viewport: steps of 2 to 5 percent", all(0.0199 <= a - b <= 0.0501 for a, b in zip([1.0] + values, values)),
      values)
times = [float(l.split(" at ")[1].split()[0]) for l in changes]
check("viewport: down fast, at most every 0.1 s", all(b - a >= 0.099 for a, b in zip(times, times[1:])) and
      times[-1] < 2.0, times)
frames = steady(20.0, 1, 0, 3, step=8.33) + steady(4.0, 1, 3, 30, step=8.33)
changes = control(0.6, 8.33, frames)
ups = [l for l in changes if float(l.split(" at ")[1].split()[0]) > 3]
check("viewport: up slowly, 2 percent at a time, once there is headroom", ups and all("(headroom)" in l for l in ups)
      and scales(ups)[-1] == 1.0, ups[-3:])
up_times = [float(l.split(" at ")[1].split()[0]) for l in ups]
check("viewport: the first step up waits a second of headroom, then at least 0.5 s apart",
      up_times and up_times[0] - 3 >= 0.99 and all(b - a >= 0.499 for a, b in zip(up_times, up_times[1:])), up_times[:4])
changes = control(0.6, 8.33, steady(7.0, 1, 0, 10, step=8.33))
check("viewport: between 75% and 95% of the budget nothing moves (hysteresis)", changes == [], changes)
changes = control(0.6, 8.33, steady(20.0, 1, 0, 2, step=8.33) + steady(20.0, 1, 2, 1, hold=1, step=8.33))
check("viewport: a hold goes straight back to full size", changes and changes[-1].startswith("scale 1.00") and
      "held at full size" in changes[-1], changes[-2:])
changes = control(0.6, 8.33, steady(5.0, 1, 0, 5, cap=0.8))
check("viewport: a learned cap eases down ahead of a heavy part, 2 percent a step", scales(changes) ==
      [0.98, 0.96, 0.94, 0.92, 0.9, 0.88, 0.86, 0.84, 0.82, 0.8] and "known to be heavy" in changes[0], changes)
# Frame times (no GPU time): vsync makes them whole refreshes.
changes = control(0.6, 8.33, [(16.67, 0, i * 0.01667, 1.0, 0) for i in range(1, 120)])
check("viewport: frame times that miss refreshes step down too", changes and scales(changes)[-1] < 1.0, changes[:2])
changes = control(0.6, 16.67, [(16.67, 0, i * 0.01667, 1.0, 0) for i in range(1, 300)])
check("viewport: at half the refresh rate, 60 fps frames are within budget", changes == [], changes)
changes = control(0.6, 8.33, [(float("nan"), 1, 0.1, 1.0, 0), (-3, 1, 0.2, 1.0, 0)])
check("viewport: nonsense measurements change nothing", changes == [], changes)
changes = control(0.6, 8.33, steady(150.0, 1, 0, 2, step=150))
gaps = [b - a for a, b in zip([float(l.split(" at ")[1].split()[0]) for l in changes][:-1],
                              [float(l.split(" at ")[1].split()[0]) for l in changes][1:])]
check("viewport: at 150 ms a frame it waits three frames between steps (late GPU times)",
      gaps and min(gaps) >= 0.449, gaps)

# The change log and its safety check.
lines = run(90, ["C 250"] + ["cf 30"] * 5 + ["cr 10.5 34.25 0 1.0 0.95 over the frame budget"] +
            ["cf 31", "cf 300", "cf 32", "cf 30", "cf 30", "cf 30"])
logged = [l for l in lines if l.startswith("change: ")]
check("change log: the change with the five frames before and after it", logged and logged[0].startswith(
    "change: song 34.25s (wall +10.500s): viewport scale 1.00 -> 0.95 (over the frame budget); frames before: 30.0 "
    "30.0 30.0 30.0 30.0 | after: 31.0 300.0 32.0 30.0 30.0"), logged)
check("change log: a 300 ms frame right after it is a hitch", "hitch 1" in lines and logged and
      "turned off for the song" in logged[0], lines)
lines = run(90, ["C 250"] + ["cf 280"] * 5 + ["cr 1 2 1 3 4 foveation once"] + ["cf 300"] * 5)
check("change log: in a run of slow frames a slow frame is not a hitch", not any(l.startswith("hitch") for l in lines),
      [l for l in lines if l.startswith("hitch") or l.startswith("change")])
lines = run(90, ["C 250", "cr 1 2 2 0 3 boost"] + ["cf 20", "cf 20", "cf 20", "cf 400", "cf 20"])
check("change log: a slow frame three frames later no longer counts", not any(l.startswith("hitch") for l in lines),
      lines)
lines = run(90, ["C 250", "cr 1 2 2 0 3 boost", "cf 400"])
check("change log: with no frames before it, any frame over the limit counts", "hitch 4" in lines, lines)

# The learned profile.
profile = ["P note %.1f %.2f %d" % (t / 10, 1.0 if t < 300 else 0.7, 120) for t in range(0, 600)]
lines = run(90, profile + ["P merge", "P cap 10 2 0.05", "P cap 28.5 2 0.05", "P cap 45 2 0.05", "P cap 100 2 0.05",
                           "P heavy", "P roundtrip 743Aether/Standard/ExpertPlus", "P wrongkey a b"])
check("profile: full size where it never scaled", find(lines, "cap 1.00") and lines[lines.index("cap 1.00")] ==
      "cap 1.00", lines)
caps = [l for l in lines if l.startswith("cap ")]
check("profile: two seconds ahead of a heavy part the cap comes down, with a margin", caps == [
    "cap 1.00", "cap 0.75", "cap 0.75", "cap 1.00"], caps)
check("profile: heavy seconds counted", find(lines, "heavy") == "heavy 30 of 60", find(lines, "heavy"))
check("profile: saved and read back the same, under a safe file name",
      find(lines, "roundtrip") == "roundtrip parsed=1 same=1 lines=62 file=743Aether_Standard_ExpertPlus",
      find(lines, "roundtrip"))
check("profile: another map's file is refused", find(lines, "wrongkey") == "wrongkey parsed=0", find(lines, "wrongkey"))
lines = run(90, ["P note 5 1.0 100", "P merge", "P note 5 0.6 200", "P merge", "P cap 5 0 0", "P roundtrip k"])
check("profile: plays are blended", find(lines, "cap ") == "cap 0.80", find(lines, "cap "))

print("\n%d failure(s)" % failures)
sys.exit(1 if failures else 0)
