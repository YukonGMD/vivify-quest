#!/usr/bin/env python3
"""Pull Vivify's saved session logs from the headset (naomi.10).

    pull_sessions.py <dest dir> [--serial SERIAL] [--no-digest]

Read-only on the headset: it lists Mods/Vivify/Sessions with `adb shell stat`
and copies files with `adb pull`; nothing there is changed or deleted.

- Every session file (Vivify_*.txt) keeps its original name. One already
  pulled is never overwritten: the same size is skipped; a different size (a
  session that was still running when pulled before) is pulled beside it as
  <name>~<size>.txt.
- index.txt, latest.txt and last-summary.txt are pulled too; a changed one
  replaces the local copy after the old copy is kept as <name>-<its time>.txt.
- Each newly pulled session gets a <name>.digest.txt from logdigest.py, and
  the digests are printed. LOGGING.md has the rest.
"""
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "logdigest"))
import logdigest  # noqa: E402

REMOTE = "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/Sessions"
POINTERS = ("index.txt", "latest.txt", "last-summary.txt")


def adb(args, serial=None, check=True):
    command = ["adb"] + (["-s", serial] if serial else []) + args
    result = subprocess.run(command, capture_output=True, text=True, errors="replace")
    if check and result.returncode != 0:
        raise RuntimeError("%s failed: %s" % (" ".join(command), (result.stderr or result.stdout).strip()))
    return result.stdout


def remote_listing(serial):
    """{name: size} of the files in the remote Sessions folder."""
    out = adb(["shell", "stat", "-c", "'%s %n'", REMOTE + "/*"], serial, check=False)
    files = {}
    for line in out.splitlines():
        parts = line.strip().split(" ", 1)
        if len(parts) == 2 and parts[0].isdigit():
            files[os.path.basename(parts[1])] = int(parts[0])
    return files


def plan(remote, local_sizes):
    """What to pull: [(remote name, local name)]. `remote` and `local_sizes`
    map file names to sizes."""
    actions = []
    for name in sorted(remote):
        if name.startswith("Vivify_") and name.endswith(".txt"):
            if name not in local_sizes:
                actions.append((name, name))
            elif local_sizes[name] != remote[name]:
                grown = "%s~%d.txt" % (name[:-4], remote[name])
                if grown not in local_sizes:
                    actions.append((name, grown))
        elif name in POINTERS:
            actions.append((name, name))
    return actions


def keep_old_copy(path):
    """Moves an existing local copy aside as <name>-<its time>.txt."""
    stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(os.path.getmtime(path)))
    stem, ext = os.path.splitext(path)
    target = "%s-%s%s" % (stem, stamp, ext)
    n = 2
    while os.path.exists(target):
        target = "%s-%s-%d%s" % (stem, stamp, n, ext)
        n += 1
    shutil.move(path, target)


def main(argv):
    if not argv or argv[0].startswith("-"):
        print(__doc__)
        return 2
    dest = argv[0]
    serial = argv[argv.index("--serial") + 1] if "--serial" in argv else None
    digest = "--no-digest" not in argv
    os.makedirs(dest, exist_ok=True)
    remote = remote_listing(serial)
    if not remote:
        print("no saved sessions on the headset (is it connected, and has naomi.10 run?)")
        return 1
    local = {name: os.path.getsize(os.path.join(dest, name)) for name in os.listdir(dest)
             if os.path.isfile(os.path.join(dest, name))}
    pulled = []
    for name, target in plan(remote, local):
        temporary = os.path.join(dest, ".pulling-" + target)
        adb(["pull", REMOTE + "/" + name, temporary], serial)
        final = os.path.join(dest, target)
        if name in POINTERS:
            if os.path.exists(final):
                with open(final, "rb") as a, open(temporary, "rb") as b:
                    same = a.read() == b.read()
                if same:
                    os.remove(temporary)
                    continue
                keep_old_copy(final)
            os.replace(temporary, final)
            continue
        os.replace(temporary, final)
        pulled.append(final)
        print("pulled %s%s" % (target, "" if target == name else " (grew since the last pull)"))
    if not pulled:
        print("nothing new")
    if digest:
        for path in pulled:
            with open(path, encoding="utf-8", errors="replace") as f:
                summary = logdigest.digest(f.read(), os.path.basename(path))
            with open(os.path.splitext(path)[0] + ".digest.txt", "w", encoding="utf-8") as f:
                f.write(summary)
            print(summary)
    index = os.path.join(dest, "index.txt")
    if os.path.exists(index):
        with open(index, encoding="utf-8", errors="replace") as f:
            for row in logdigest.parse_index(f.read()):
                print("%-58s %-10s %8d  %s" % (row["file"], row["ended"], row["bytes"], row["maps"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
