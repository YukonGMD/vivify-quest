#!/usr/bin/env python3
"""Tests for logdigest.py: parsing schema-2 session logs, naomi.9's old
format, sessions that ended cleanly, in the background and unexpectedly, and
index.txt."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import logdigest  # noqa: E402

failures = 0


def check(name, condition, detail=""):
    global failures
    if condition:
        print("PASS " + name)
    else:
        failures += 1
        print("FAIL " + name + ("  " + str(detail)[:600] if detail else ""))


HEADER = """=== Vivify session log, schema 2 ===
build: 0.14.25-naomi.10-beta.1 · Twilight Sparkle
commit: 89f98f5d26
started: 2026-10-25 01:47:12 -0700 (local time, UTC-0700)
01:47:12.100 s=- [HEADER] converter: conversion version 23
01:47:12.101 s=- [HEADER] device: Oculus Quest 3S; OS UP1A (Android 14, SDK 34, build 123)
01:47:20.000 s=- [HEADER] Beat Saber 1.40.8, Unity 2022.3.33f1
01:47:20.010 s=- [HEADER] changed since the last session: build naomi.9 -> naomi.10-beta.1; settings: detailedLogging (new) -> false
01:47:12.200 s=- [SESSION] previous session Vivify_2026-10-25_01-30-00_naomi.9-21b3a30.txt (naomi.9) ended unexpectedly: it stopped while running
"""

BODY = """01:48:00.000 s=- [CRUMB] +47000ms level start begin: 12 custom event(s) | gc 0/1/2=1/1/1 heap used=60.0MB of 85.0MB
01:48:00.010 s=- [ASSETS] 4968d (743 Aether): 37 asset(s), 6 prefab(s); 222 material(s) on 35 shader(s), 4 drawing a stand-in; shaders: Swifter/Bokeh x2, Swifter/TriangleExplosion x3 (3 stand-in), Mawntee/Beat Saber Wireframe 3D-Noise x1 (1 stand-in); 3 texture(s) by Unity format number: 4 x3
01:48:00.300 s=- [CRUMB] +47300ms first frame after level start: 640ms later | gc 0/1/2=1/1/1
01:48:00.300 s=- [LOAD] load timeline: bundle preload 161 ms, texture decode 7 ms; level start to first frame 300 ms
01:48:20.000 s=20.00 [INFO] Vivify particle emulation: 'Ground Fog' keeps the stand-in: its particles are drawn as billboards (render mode 0), and only mesh particles can be baked
01:48:21.000 s=21.00 [WARN] Vivify geometry emulation: an object kept the stand-in: 'Explosion' is not a mesh renderer
01:48:22.000 s=22.00 [WARN] Vivify frame stall: the game did not draw for 3.3s (songTime 22.00, bundle 'x')
01:48:23.000 s=23.00 [GL] error 0x0502 (GL_INVALID_OPERATION) since the last check
01:50:00.000 s=- [PERF] === song performance: 4968d (743 Aether - Swifter) ===
01:50:00.000 s=- [PERF]   'prefab_intro_2' song 0.0-31.9s: 3600 frames, median 24 ms (42 fps), 95th 31, max 98; 60 fps in 20% of frames; missed a refresh in 100%; Vivify's own work 0.05 ms a frame on average (max 1.0)
01:50:00.000 s=- [PERF]     'prefab_intro_2' frame times: under 9 ms 0.0%, 9-12 0.0%, 12-14 0.0%, 14-17 20.0%, 17-25 50.0%, 25-34 29.0%, 34-50 1.0%, 50-100 0.0%, 100+ 0.0%; dropped about 7000 of 10800 refreshes (64.8%); garbage collections 1 (the slowest frame with one 31 ms); memory peak 1290 MB resident, native heap 310 MB; hottest sensor 44.5 C, battery 33.0 C; GPU clock average 587 MHz
01:50:00.000 s=- [PERF]   'prefab_drop_2' song 31.9-86.7s: 6500 frames, median 9 ms (111 fps), 95th 12, max 40; 60 fps in 99% of frames; missed a refresh in 10%; Vivify's own work 0.04 ms a frame on average (max 0.5)
01:50:00.000 s=- [PERF]   render thread in the GPU driver's shader compiler (libllvm-glnext.so) in 40% of samples: first-use shader compiles
01:50:00.000 s=- [PERF] logging cost on the game's threads, per frame on average: session log 0.0021 ms, black box 0.0004 ms, detailed mode 0.0000 ms (off)
01:50:03.000 s=- [CRUMB] +170000ms leak check (menu, 3 s after the level ended): alive 8427 GameObject; since the previous level's menu check +402 GameObject, +131 Mesh, +1 Material | gc 0/1/2=2/2/2 heap used=68.4MB of 85.4MB
"""

clean = HEADER + BODY + "=== session ended cleanly at 2026-10-25 01:55:00: 900 lines written, 3 left out by the rate limit ===\n"
d = logdigest.digest(clean, "clean.txt")
check("build and codename", "build: 0.14.25-naomi.10-beta.1 · Twilight Sparkle" in d, d)
check("device and game", "device: Oculus Quest 3S" in d and "game: Beat Saber 1.40.8, Unity 2022.3.33f1" in d, d)
check("what changed", "changed since the last session: build naomi.9 -> naomi.10-beta.1" in d, d)
check("ended cleanly", "ended: cleanly" in d, d)
check("previous session's end is shown", "previous session Vivify_2026-10-25_01-30-00" in d, d)
check("frame stall listed", "the game did not draw for 3.3s" in d, d)
check("maps played", "maps played: 4968d (743 Aether - Swifter)" in d, d)
check("load timeline", "load timeline: bundle preload 161 ms" in d, d)
check("slowest section first", d.index("24 ms median, 60 fps in 20%: 'prefab_intro_2'") <
      d.index("9 ms median, 60 fps in 99%: 'prefab_drop_2'"), d)
check("dropped refreshes for a section", "dropped 7000 of 10800 refreshes (64.8%)" in d, d)
check("gray from the inventory", "4968d (743 Aether): 4 material(s): Swifter/TriangleExplosion x3 (3 stand-in), "
      "Mawntee/Beat Saber Wireframe 3D-Noise x1 (1 stand-in)" in d, d)
check("gray particle systems", "'Ground Fog': its particles are drawn as billboards" in d and
      "'Explosion': is not a mesh renderer" in d, d)
check("shader compiles", "shader compiler (libllvm-glnext.so) in 40% of samples" in d and
      "slow first frame: +47300ms first frame after level start: 640ms later" in d, d)
check("memory and heat", "resident 1290 MB, native heap 310 MB, managed heap 68.4 MB, hottest sensor 44.5 C" in d, d)
check("menu growth", "+402 GameObject, +131 Mesh, +1 Material" in d, d)
check("GL errors", "GL errors: 1 line(s): error 0x0502" in d, d)
check("logging cost", "logging cost on the game's threads, per frame on average: session log 0.0021 ms" in d, d)
check("one page", len(d.splitlines()) < 60, len(d.splitlines()))

unexpected = HEADER + BODY + """
=== black box: the last 4 events before this session ended (unexpected) ===
01:51:00.000 s=40.00 [FRAME] frame 8.3 ms, GPU 7.9 ms, scale 1.00
01:51:00.008 s=40.01 [CRUMB] particle emulation: baking 'Explosion'
01:51:00.010 s=40.01 [EVENT] beat 55.03 AssignObjectPrefab
01:51:00.016 s=40.02 [FRAME] frame 8.3 ms, GPU 8.0 ms, scale 1.00
=== end of black box ===
"""
d = logdigest.digest(unexpected, "crashed.txt")
check("ended unexpectedly", "ended: UNEXPECTEDLY" in d, d)
check("black box summarised", "black box: 4 events kept; last breadcrumb: 01:51:00.008 s=40.01 [CRUMB] particle "
      "emulation: baking 'Explosion'" in d and "[EVENT] beat 55.03 AssignObjectPrefab" in d, d)

background = HEADER + BODY + "01:52:00.000 s=- [SESSION] === app paused at 2026-10-25 01:52:00 ===\n" + \
    "\n=== black box: the last 1 events before this session ended (background) ===\n01:52:00.000 s=- [SESSION] app paused\n=== end of black box ===\n"
d = logdigest.digest(background, "bg.txt")
check("ended in the background", "ended: closed while in the background" in d, d)
running = HEADER + BODY + "01:52:00.000 s=- [SESSION] === app paused at 2026-10-25 01:52:00 ===\n"
check("paused, not yet settled", "last seen paused" in logdigest.digest(running), logdigest.digest(running))
check("cut off, not yet settled", "no footer: still running" in logdigest.digest(HEADER + BODY))

old = """=== Vivify 0.14.25-naomi.9 (commit 21b3a30c26) session log ===
[INFO] Vivify settings for this level: standInShading=true
[CRUMB +20554ms] geometry emulation end: 297 object(s) rebuilt, 8 kept the stand-in | gc 0/1/2=24/24/24 heap used=65.2MB of 85.4MB
[WARN] Vivify geometry emulation: an object kept the stand-in: 'Extending Arrows' is not a mesh renderer
[PERF] === song performance: 4968d (743 Aether) ===
[PERF]   'prefab_intro_2' song 0.0-31.9s: 3600 frames, median 24 ms (42 fps), 95th 31, max 98; 60 fps in 20% of frames; missed a refresh in 100%; Vivify's own work 0.05 ms a frame on average (max 1.0)
"""
d = logdigest.digest(old, "VivifySession.txt")
d2 = logdigest.digest(old + old.split("\n", 1)[1], "twice.txt")
check("a map played twice is counted", "maps played: 4968d (743 Aether) x2" in d2, d2)
check("naomi.9 log: build from its first line", "build: Vivify 0.14.25-naomi.9 (commit 21b3a30c26) session log" in d, d)
check("naomi.9 log: sections and gray still read", "24 ms median" in d and "'Extending Arrows': is not a mesh" in d and
      "managed heap 65.2 MB" in d, d)

index = "file\tstarted\tbuild\tmaps\tended\tbytes\nA.txt\t2026-10-25 01:00:00\tnaomi.10\t4968d,4839c\tunexpected\t12\nbad\n"
rows = logdigest.parse_index(index)
check("index rows", len(rows) == 1 and rows[0]["maps"] == "4968d,4839c" and rows[0]["ended"] == "unexpected" and
      rows[0]["bytes"] == 12, rows)

print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
