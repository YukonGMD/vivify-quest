#!/usr/bin/env python3
"""Tests for tools/ovrmetrics/summarize.py, on synthetic captures.

    python3 tools/ovrmetrics/run_tests.py
"""
import os, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import summarize  # noqa: E402

failures = 0


def check(name, cond, detail=""):
    global failures
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else " " + str(detail)))
    if not cond:
        failures += 1


def capture(header, rows):
    f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False)
    f.write(",".join(header) + "\n")
    for r in rows:
        f.write(",".join(str(v) for v in r) + "\n")
    f.close()
    try:
        return summarize.summarize(f.name)
    finally:
        os.unlink(f.name)


H = ["Time Stamp", "average_frame_rate", "display_refresh_rate", "app_gpu_time_microseconds",
     "gpu_utilization_percentage", "cpu_utilization_percentage", "cpu_utilization_percentage_core0",
     "cpu_utilization_percentage_core1", "stale_frame_count"]

text = capture(H, [[i, 90, 90, 8000, 60, 30, 40, 20, 0] for i in range(20)])
check("steady capture keeps up", "kept up with the refresh rate" in text, text)
check("budget from refresh rate", "90 Hz, so each frame has 11.1 ms" in text, text)
check("columns are listed", "app GPU time (us) = 'app_gpu_time_microseconds'" in text, text)

text = capture(H, [[i, 70, 90, 12500, 98, 35, 50, 20, 4] for i in range(20)])
check("GPU-bound", "looks GPU-bound" in text, text)
check("GPU time over budget counted", "worst 12.5 ms (over the frame budget in 20 of 20 samples)" in text, text)
check("stale frames summed", "80 in total" in text, text)

text = capture(H, [[i, 72, 90, 6000, 50, 30, 97, 10, 3] for i in range(20)])
check("one busy core reads as CPU-bound", "looks CPU-bound" in text and "core 0 at 97%" in text, text)

text = capture(H, [[i, 80, 90, 6000, 50, 30, 50, 10, 2] for i in range(20)])
check("missed frames without load reads as stalls", "points to stalls" in text, text)

text = capture(H, [[i, 72, 90, 6000, 50, 30, 50, 10, i * 3] for i in range(20)])
check("a running stale total is not summed", "57 in total" in text, text)

text = capture(["timestamp", "fps"], [[i, 90] for i in range(5)])
check("missing columns are said, not guessed", "Display refresh rate: not in this capture" in text and
      "Not found:" in text, text)
text = capture(H, [])
check("empty capture", "has no samples" in text, text)
text = capture(H, [[0, "", "", "", "", "", "", "", ""], [1, 90, 90, 8000, 60, 30, 40, 20, 0]])
check("blank cells are skipped", "average 90.0 fps" in text, text)

print("\n%d failure(s)" % failures)
sys.exit(1 if failures else 0)
