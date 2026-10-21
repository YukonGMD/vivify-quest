#!/usr/bin/env python3
"""Tests for src/VivifyWatchdog.cpp. Build with:
    g++ -std=c++20 -O1 -g -fsanitize=address,undefined -pthread -I include -o /tmp/watchdog \\
        tools/watchdog/main.cpp src/VivifyWatchdog.cpp
    VIVIFY_WATCHDOG=/tmp/watchdog python3 tools/watchdog/run_tests.py
"""
import os, re, subprocess, sys, tempfile

BIN = os.environ.get("VIVIFY_WATCHDOG", "/tmp/watchdog")
failures = 0


def check(name, cond, detail=""):
    global failures
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else " " + str(detail)))
    if not cond:
        failures += 1


def call(*args):
    r = subprocess.run([BIN, *map(str, args)], capture_output=True, text=True, timeout=60)
    if "AddressSanitizer" in r.stderr or "runtime error" in r.stderr:
        raise AssertionError(r.stderr[:800])
    return r


def parse(text):
    with tempfile.NamedTemporaryFile("w", delete=False) as f:
        f.write(text)
    try:
        return call("parse", f.name).stdout.strip()
    finally:
        os.unlink(f.name)


def run(healthy, stall, after, mode, max_reports=12):
    return call("run", healthy, stall, after, mode, max_reports).stdout.splitlines()


stat_tail = " 1 2 3 4 5 6 7 8 9 10 1234 56 0 0 20 0 30 0 100"
check("parses a plain stat line", parse("4321 (UnityMain) R" + stat_tail) ==
      "state=R utime=1234 stime=56", parse("4321 (UnityMain) R" + stat_tail))
check("comm with spaces and parentheses", parse("77 (a) b (c)) S" + stat_tail) ==
      "state=S utime=1234 stime=56")
check("truncated line is refused", parse("77 (UnityMain) S 1 2 3") == "unparsed")
check("empty text is refused", parse("") == "unparsed")
real = parse(open("/proc/self/stat").read())
check("this process's own stat parses", real.startswith("state="), real)

lines = run(600, 0, 0, "sleep")
check("no report while the main thread beats", lines == [], lines)

lines = run(100, 700, 100, "sleep")
stalls = [l for l in lines if l.startswith("WATCHDOG: main thread stalled")]
check("a stall is reported", len(stalls) >= 1, lines)
check("report names the last breadcrumb", all("last breadcrumb=phase two" in l for l in stalls), stalls)
check("report has the main thread's state and wchan",
      all(re.search(r"main tid=\d+ state=\S cpu user=\d+ms sys=\d+ms in the last [\d.]+s wchan=", l) for l in stalls),
      stalls)
check("a missing render thread is said so", all("render (no-such-thread) not found" in l for l in stalls), stalls)
check("sleeping main thread shows as not running", all(re.search(r"main tid=\d+ state=[SD]", l) for l in stalls), stalls)
check("resume is reported once", sum(l.startswith("WATCHDOG: main thread resumed") for l in lines) == 1, lines)

lines = run(100, 900, 50, "busy")
stalls = [l for l in lines if l.startswith("WATCHDOG: main thread stalled")]
busy = [int(m.group(1)) for l in stalls for m in [re.search(r"main tid=\d+ state=R cpu user=(\d+)ms", l)] if m]
check("busy main thread shows as running and using CPU", busy and max(busy) >= 50, stalls)

lines = run(100, 2000, 50, "sleep", max_reports=3)
stalls = [l for l in lines if l.startswith("WATCHDOG: main thread stalled")]
check("reports are capped", len(stalls) == 3, lines)
check("the cap is announced", any("further stall reports suppressed" in l for l in lines), lines)

lines = run(100, 700, 100, "paused")
check("no report while paused", lines == [], lines)

print("\n%d failure(s)" % failures)
sys.exit(1 if failures else 0)
