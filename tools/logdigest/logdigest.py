#!/usr/bin/env python3
"""One-page summary of Vivify session logs (naomi.10).

    logdigest.py <session file> [<session file> ...] [--write]
    logdigest.py --index <index.txt>

For each session file (Sessions/Vivify_*.txt, schema 2; naomi.9's
VivifySession.txt is read as well as it can be): how the session ended, what
points at a crash, the maps played, the slowest sections, what was drawn
gray, shader-compile stalls, memory peaks, GL errors, what logging cost, and
what changed since the session before. --write puts each summary beside its
file as <name>.digest.txt. --index lists the sessions an index.txt holds.
LOGGING.md describes the lines this reads.
"""
import os
import re
import sys

LINE = re.compile(r"^(\d\d:\d\d:\d\d\.\d{3}) s=(\S+) \[([A-Z_0-9]+)\] (.*)$")
SECTION = re.compile(r"^  '(.+)' song ([\d.]+)-([\d.]+)s: (\d+) frames, median (\d+) ms \((\d+) fps\), 95th (\d+), "
                     r"max (\d+); 60 fps in (\d+)% of frames; missed a refresh in (\d+)%")
DETAIL = re.compile(r"^    '(.+)' frame times: (.*)$")
FOOTER = "=== session ended"


class Entry:
    __slots__ = ("time", "song", "tag", "text")

    def __init__(self, time, song, tag, text):
        self.time, self.song, self.tag, self.text = time, song, tag, text


def parse(text):
    """(header lines, entries, black-box lines, footer line or None)."""
    header, entries, box, footer = [], [], [], None
    in_box = False
    for raw in text.splitlines():
        if raw.startswith("=== black box:"):
            in_box = True
            box.append(raw)
            continue
        if in_box:
            box.append(raw)
            if raw.startswith("=== end of black box"):
                in_box = False
            continue
        if raw.startswith(FOOTER):
            footer = raw
            continue
        m = LINE.match(raw)
        if m:
            entries.append(Entry(m.group(1), m.group(2), m.group(3), m.group(4)))
            continue
        # naomi.9 and earlier: "[INFO] text", "[CRUMB +12ms] text", "[PERF] text".
        old = re.match(r"^\[([A-Z]+)[^\]]*\] ?(.*)$", raw)
        if old and not entries and not header:
            header.append(raw)
        elif old:
            entries.append(Entry("", "-", old.group(1), old.group(2)))
        elif not entries:
            header.append(raw)
    return header, entries, box, footer


def header_value(header, entries, key):
    for line in header:
        if line.startswith(key + ": "):
            return line[len(key) + 2:]
    for e in entries:
        if e.tag == "HEADER" and e.text.startswith(key + ": "):
            return e.text[len(key) + 2:]
    return None


def how_it_ended(footer, box, entries):
    if footer:
        return "cleanly (quit through the game)"
    if box:
        m = re.search(r"\(([a-z]+)\) ===", box[0])
        how = m.group(1) if m else "unexpected"
        if how == "background":
            return "closed while in the background (the headset's usual way to quit)"
        return "UNEXPECTEDLY: stopped while running (crash, hang the system killed, or power)"
    if any(e.tag == "SESSION" and "=== app paused" in e.text for e in entries[-20:]):
        return "no footer; last seen paused (probably closed in the background; the next launch settles it)"
    return "no footer: still running when copied, or ended unexpectedly (the next launch settles it)"


def digest(text, name="session"):
    header, entries, box, footer = parse(text)
    out = []
    out.append("=== digest of %s ===" % name)
    build = header_value(header, entries, "build")
    if build is None and header:
        build = header[0].strip("= ")
    out.append("build: %s" % (build or "?"))
    for key in ("commit", "started", "device"):
        value = header_value(header, entries, key)
        if value:
            out.append("%s: %s" % (key, value))
    game = next((e.text for e in entries if e.tag == "HEADER" and e.text.startswith("Beat Saber ")), None)
    if game:
        out.append("game: %s" % game)
    changed = next((e.text for e in entries if e.tag == "HEADER" and e.text.startswith("changed since")), None)
    if changed:
        out.append(changed)
    out.append("ended: %s" % how_it_ended(footer, box, entries))

    # Crash evidence: the previous session's note, watchdog lines, the black box.
    notes = [e.text for e in entries if e.tag == "SESSION" and e.text.startswith("previous session")]
    watchdog = [e for e in entries if e.tag == "WATCHDOG"]
    errors = [e for e in entries if e.tag == "ERROR"]
    out.append("")
    out.append("crashes and stalls:")
    for note in notes:
        out.append("  " + note)
    for e in watchdog[:5]:
        out.append("  watchdog %s s=%s: %s" % (e.time, e.song, e.text[:160]))
    if len(watchdog) > 5:
        out.append("  (%d more watchdog lines)" % (len(watchdog) - 5))
    for e in errors[:5]:
        out.append("  error %s s=%s: %s" % (e.time, e.song, e.text[:160]))
    stalls = [e for e in entries if "frame stall" in e.text]
    for e in stalls[:5]:
        out.append("  %s" % e.text[:160])
    # naomi.10-beta.5: a map's saber trail handed a time gap no frame explains.
    trails = [e for e in entries if e.text.startswith("Vivify saber trail:")]
    for e in trails[:3]:
        out.append("  saber trail %s s=%s: %s" % (e.time, e.song, e.text[20:200]))
    if box:
        crumbs = [l for l in box if "[CRUMB]" in l]
        out.append("  black box: %d events kept; last breadcrumb: %s" % (len(box) - 2, crumbs[-1] if crumbs else "none"))
        for line in box[-7:-1]:
            out.append("    " + line[:160])
    if len(out) and out[-1] == "crashes and stalls:":
        out.append("  none")

    # Maps.
    maps = []
    for e in entries:
        m = re.match(r"=== song performance: (.*) ===", e.text)
        if m:
            maps.append(m.group(1))
    out.append("")
    plays = {}
    for m in maps:
        plays[m] = plays.get(m, 0) + 1
    out.append("maps played: %s" % (", ".join("%s%s" % (m, " x%d" % n if n > 1 else "") for m, n in plays.items())
                                     if plays else "none"))
    for e in entries:
        if e.tag == "LOAD" and e.text.startswith("load timeline"):
            out.append("  " + e.text[:200])

    # Slowest sections.
    sections = []
    details = {}
    song_map = None
    for e in entries:
        m = re.match(r"=== song performance: (.*) ===", e.text)
        if m:
            song_map = m.group(1)
        if e.tag != "PERF":
            continue
        s = SECTION.match(e.text)
        if s:
            sections.append((int(s.group(5)), int(s.group(9)), s.group(1), song_map, s.group(2), s.group(3)))
        d = DETAIL.match(e.text)
        if d:
            details[(song_map, d.group(1))] = d.group(2)
    out.append("")
    out.append("slowest sections (median frame, share at 60 fps):")
    for median, held, label, song, start, end in sorted(sections, reverse=True)[:5]:
        line = "  %s ms median, 60 fps in %d%%: '%s' in %s (song %s-%ss)" % (median, held, label[:60], song, start, end)
        out.append(line)
        drop = re.search(r"dropped about (\d+) of (\d+) refreshes \(([\d.]+)%\)", details.get((song, label), ""))
        if drop:
            out.append("    dropped %s of %s refreshes (%s%%)" % drop.groups())
    if not sections:
        out.append("  none recorded")

    # Gray.
    out.append("")
    out.append("drawn gray (stand-ins):")
    gray = 0
    for e in entries:
        if e.tag == "ASSETS":
            m = re.search(r"(\d+) drawing a stand-in; shaders: (.*?); \d+ texture", e.text)
            if m and int(m.group(1)) > 0:
                with_stand_ins = [s for s in m.group(2).split(", ") if "stand-in" in s]
                out.append("  %s: %s material(s): %s" % (e.text.split(":")[0], m.group(1), ", ".join(with_stand_ins)))
                gray += 1
    seen = set()
    for e in entries:
        m = re.search(r"'(.+?)' keeps the stand-in: (.*)", e.text) or re.search(r"kept the stand-in: '(.+?)' (.*)", e.text)
        if m and (m.group(1), m.group(2)) not in seen:
            seen.add((m.group(1), m.group(2)))
            out.append("  '%s': %s" % (m.group(1), m.group(2)[:120]))
            gray += 1
    if not gray:
        out.append("  none recorded")

    # Shader compiles.
    out.append("")
    out.append("shader compile stalls:")
    compiles = [e.text for e in entries if "shader compiler" in e.text]
    first = [e.text for e in entries if e.tag == "CRUMB" and "first frame after level start" in e.text]
    slow_first = [t for t in first if re.search(r"(\d+)ms later", t) and int(re.search(r"(\d+)ms later", t).group(1)) >= 500]
    for text in compiles:
        out.append("  " + text.strip()[:200])
    for text in slow_first:
        out.append("  slow first frame: " + text[:160])
    if not compiles and not slow_first:
        out.append("  none seen (the slow-frame sampler names the compiler when it catches one)")

    # Memory.
    out.append("")
    resident = [float(x) for e in entries for x in re.findall(r"memory peak (\d+) MB resident", e.text)]
    native = [float(x) for e in entries for x in re.findall(r"native heap (\d+) MB", e.text)]
    hottest = [float(x) for e in entries for x in re.findall(r"hottest sensor ([\d.]+) C", e.text)]
    heap = [float(x) for e in entries for x in re.findall(r"heap used=([\d.]+)MB", e.text)]
    parts = []
    if resident:
        parts.append("resident %.0f MB" % max(resident))
    if native:
        parts.append("native heap %.0f MB" % max(native))
    if heap:
        parts.append("managed heap %.1f MB" % max(heap))
    if hottest:
        parts.append("hottest sensor %.1f C" % max(hottest))
    out.append("memory and heat peaks: %s" % (", ".join(parts) if parts else "not recorded"))
    growth = [e.text for e in entries if "since the previous level's menu check" in e.text]
    if growth:
        m = re.search(r"since the previous level's menu check (.*?)( \||$)", growth[-1])
        if m:
            out.append("  last level-to-level growth in the menu: " + m.group(1))
    names = [e.text for e in entries if e.text.startswith("Vivify leak check (menu): meshes there are more of")]
    if names:
        out.append("  meshes that grew, by name: " + names[-1].split(": ", 2)[-1][:200])

    gl = [e for e in entries if e.tag == "GL"]
    out.append("GL errors: %d line(s)%s" % (len(gl), (": " + gl[0].text) if gl else ""))
    cost = [e.text for e in entries if e.text.startswith("logging cost")]
    if cost:
        out.append(cost[-1])
    if footer:
        out.append(footer)
    return "\n".join(out) + "\n"


def parse_index(text):
    rows = []
    for line in text.splitlines():
        if not line or line.startswith("file\t"):
            continue
        fields = line.split("\t")
        if len(fields) < 6:
            continue
        rows.append(dict(file=fields[0], started=fields[1], build=fields[2], maps=fields[3], ended=fields[4],
                         bytes=int(fields[5]) if fields[5].isdigit() else 0))
    return rows


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    if argv[0] == "--index":
        with open(argv[1], encoding="utf-8", errors="replace") as f:
            for row in parse_index(f.read()):
                print("%-58s %-28s %-24s %-10s %8d  %s" % (row["file"], row["started"], row["build"], row["ended"],
                                                         row["bytes"], row["maps"]))
        return 0
    write = "--write" in argv
    for path in [a for a in argv if a != "--write"]:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        summary = digest(text, os.path.basename(path))
        if write:
            with open(os.path.splitext(path)[0] + ".digest.txt", "w", encoding="utf-8") as f:
                f.write(summary)
        print(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
