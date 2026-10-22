#!/usr/bin/env python3
"""Summarise an OVR Metrics Tool capture (CSV) in plain language.

    python3 tools/ovrmetrics/summarize.py <capture.csv>
    python3 tools/ovrmetrics/summarize.py --pull [destination-folder]

--pull copies the newest capture off the headset with adb (read-only on the
headset) into the destination (default: testlogs/ovrmetrics) and summarises it.

OVR Metrics Tool's column names differ a little between versions, so columns
are found by name pattern; the script lists which ones it used and says when
one is missing rather than guessing.
"""
import csv, os, re, statistics, subprocess, sys

CAPTURE_DIR = "/sdcard/Android/data/com.oculus.ovrmonitormetricsservice/files/CapturedMetrics/"

# (key, description, patterns tried in order against lower-cased column names)
COLUMNS = [
    ("time", "time stamp", [r"^time.?stamp", r"^timestamp", r"^time$"]),
    ("fps", "average frame rate", [r"average_frame_rate", r"^fps$", r"frame_?rate"]),
    ("refresh", "display refresh rate", [r"display_refresh_rate", r"refresh_rate"]),
    ("app_gpu_us", "app GPU time (us)", [r"app_gpu_time_micro", r"app.?gpu.?time"]),
    ("app_cpu_us", "app CPU time (us)", [r"app_cpu_time_micro", r"app.?cpu.?time"]),
    ("gpu_util", "GPU utilisation (%)", [r"^gpu_utili[sz]ation", r"gpu.?util"]),
    ("cpu_util", "CPU utilisation, all cores (%)", [r"^cpu_utili[sz]ation_percentage$", r"^cpu_utili[sz]ation$",
                                                     r"^cpu.?util(?!.*core)"]),
    ("stale", "stale frames", [r"stale_frame", r"stale"]),
    ("gpu_level", "GPU level", [r"^gpu_level"]),
    ("cpu_level", "CPU level", [r"^cpu_level"]),
]
CORE_PATTERN = re.compile(r"cpu.?utili[sz]ation.*core.?(\d+)")


def number(text):
    try:
        value = float(str(text).strip())
    except ValueError:
        return None
    return value if value == value else None  # NaN


def find_columns(header):
    lowered = [h.strip().lower() for h in header]
    found = {}
    for key, _, patterns in COLUMNS:
        for pattern in patterns:
            hit = next((header[i] for i, h in enumerate(lowered) if re.search(pattern, h)), None)
            if hit is not None:
                found[key] = hit
                break
    cores = {int(m.group(1)): header[i] for i, h in enumerate(lowered) for m in [CORE_PATTERN.search(h)] if m}
    return found, cores


def series(rows, column):
    if column is None:
        return []
    return [v for v in (number(r.get(column)) for r in rows) if v is not None]


def summarize(path):
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        reader = csv.DictReader(f)
        header = reader.fieldnames or []
        rows = list(reader)
    found, cores = find_columns(header)
    out = []
    say = out.append
    say("OVR Metrics capture: %s" % os.path.basename(path))
    say("%d sample(s)." % len(rows))
    if not rows:
        say("The file has no samples.")
        return "\n".join(out)

    def avg(values):
        return statistics.fmean(values) if values else None

    refresh = series(rows, found.get("refresh"))
    hz = statistics.median(refresh) if refresh else None
    budget_us = 1e6 / hz if hz else None
    fps = series(rows, found.get("fps"))
    gpu_us = series(rows, found.get("app_gpu_us"))
    cpu_us = series(rows, found.get("app_cpu_us"))
    gpu_util = series(rows, found.get("gpu_util"))
    cpu_util = series(rows, found.get("cpu_util"))
    stale = series(rows, found.get("stale"))
    core_series = {n: series(rows, c) for n, c in sorted(cores.items())}
    busiest_core = None
    if core_series:
        busiest_core = max(((n, avg(v)) for n, v in core_series.items() if v), key=lambda t: t[1], default=None)

    if hz:
        say("Display refresh rate: %.0f Hz, so each frame has %.1f ms." % (hz, budget_us / 1000))
    else:
        say("Display refresh rate: not in this capture.")
    if fps:
        low = sum(1 for v in fps if hz and v < hz * 0.95)
        say("Frame rate: average %.1f fps, lowest %.1f fps%s." % (
            avg(fps), min(fps), (", below 95%% of the refresh rate in %d of %d samples" % (low, len(fps))) if hz else ""))
    else:
        say("Frame rate: not in this capture.")
    if gpu_us:
        line = "App GPU time: average %.1f ms, worst %.1f ms" % (avg(gpu_us) / 1000, max(gpu_us) / 1000)
        if budget_us:
            over = sum(1 for v in gpu_us if v > budget_us)
            line += " (over the frame budget in %d of %d samples)" % (over, len(gpu_us))
        say(line + ".")
    else:
        say("App GPU time: not in this capture.")
    if cpu_us:
        say("App CPU time: average %.1f ms, worst %.1f ms." % (avg(cpu_us) / 1000, max(cpu_us) / 1000))
    if gpu_util:
        say("GPU utilisation: average %.0f%%, highest %.0f%%." % (avg(gpu_util), max(gpu_util)))
    else:
        say("GPU utilisation: not in this capture.")
    if cpu_util:
        say("CPU utilisation (all cores): average %.0f%%, highest %.0f%%." % (avg(cpu_util), max(cpu_util)))
    else:
        say("CPU utilisation: not in this capture.")
    if busiest_core:
        say("Busiest CPU core: core %d at %.0f%% on average (one busy core is what a main-thread-bound game looks "
            "like, even when the all-core figure is low)." % busiest_core)
    stale_total = 0
    if stale:
        # The column is usually a per-sample count; a running total only ever grows.
        cumulative = all(b >= a for a, b in zip(stale, stale[1:])) and stale[-1] > stale[0] and len(stale) > 3 \
            and max(stale) > 10 * max(1.0, statistics.median([b - a for a, b in zip(stale, stale[1:])]))
        stale_total = stale[-1] - stale[0] if cumulative else sum(stale)
        say("Stale frames (the compositor showed an old frame again): %d in total." % stale_total)
    else:
        say("Stale frames: not in this capture.")

    # CPU- or GPU-bound: only meaningful when frames were actually missed.
    missing = (fps and hz and avg(fps) < hz * 0.97) or stale_total > 0
    gpu_busy = (gpu_util and avg(gpu_util) >= 85) or (gpu_us and budget_us and avg(gpu_us) >= 0.85 * budget_us)
    cpu_busy = (busiest_core and busiest_core[1] >= 85) or (cpu_us and budget_us and avg(cpu_us) >= 0.85 * budget_us)
    if not missing:
        verdict = "The level kept up with the refresh rate; it is neither CPU- nor GPU-bound in this capture."
    elif gpu_busy and not cpu_busy:
        verdict = "It looks GPU-bound: frames were missed while the GPU was close to fully busy."
    elif cpu_busy and not gpu_busy:
        verdict = "It looks CPU-bound: frames were missed while a CPU core was close to fully busy and the GPU was not."
    elif gpu_busy and cpu_busy:
        verdict = "Both the GPU and a CPU core were close to fully busy while frames were missed; it is not clear which limits it."
    else:
        verdict = ("Frames were missed but neither the GPU nor a CPU core was close to fully busy: that points to "
                   "stalls (waiting, loading, garbage collection) rather than steady load.")
    say("Verdict: " + verdict)

    used = ", ".join("%s = '%s'" % (desc, found[key]) for key, desc, _ in COLUMNS if key in found)
    say("Columns used: " + (used or "none recognised"))
    missing_cols = [desc for key, desc, _ in COLUMNS if key not in found and key not in ("time", "cpu_level", "gpu_level")]
    if missing_cols:
        say("Not found: " + ", ".join(missing_cols) + ". Column names in the file: " + ", ".join(header))
    return "\n".join(out)


def pull(destination):
    listing = subprocess.run(["adb", "shell", "ls", "-t", CAPTURE_DIR], capture_output=True, text=True)
    names = [n.strip() for n in listing.stdout.splitlines() if n.strip().lower().endswith(".csv")]
    if listing.returncode != 0 or not names:
        sys.exit("No capture found in %s (%s)" % (CAPTURE_DIR, (listing.stderr or listing.stdout).strip()))
    os.makedirs(destination, exist_ok=True)
    local = os.path.join(destination, names[0])
    subprocess.run(["adb", "pull", CAPTURE_DIR + names[0], local], check=True, capture_output=True)
    return local


def main(argv):
    if len(argv) >= 2 and argv[1] == "--pull":
        path = pull(argv[2] if len(argv) >= 3 else os.path.join("testlogs", "ovrmetrics"))
    elif len(argv) == 2:
        path = argv[1]
    else:
        sys.exit(__doc__)
    print(summarize(path))


if __name__ == "__main__":
    main(sys.argv)
