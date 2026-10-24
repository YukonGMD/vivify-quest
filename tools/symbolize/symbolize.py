#!/usr/bin/env python3
"""Names the functions in Vivify's slow-frame and watchdog lines.

    symbolize.py VivifySession.txt [--libs DIR ...] [--all]

The slow-frame summary (naomi.7) gives functions as "<library>+0x<start>"
(a function's start address) or "<library>@0x<address>" (an address no
function covers), and a watchdog stack sample gives frames as
"#NN pc <address>  <library>". Each is looked up in the library of that name
found under the --libs directories (searched recursively; default
testlogs/symbols and testlogs/libs; names compared without case, so
libvivify.so finds libVivify.so) and the function's name is added after it in
brackets. Only lines with something to name are printed, unless --all.

Uses llvm-symbolizer when it is on PATH or in the NDK (QPM-RS's, or
$ANDROID_NDK_HOME), else addr2line (or addr2line always, with
SYMBOLIZE_TOOL=addr2line).
"""
import argparse, glob, os, re, shutil, subprocess, sys

TOKEN = re.compile(r"\b([A-Za-z0-9_.+-]+\.so(?:\.[0-9.]+)?)([+@])0x([0-9a-f]+)\b")
FRAME = re.compile(r"(#\d\d pc )([0-9a-f]{16})(  )(\S+)")


def find_symbolizer():
    if os.environ.get("SYMBOLIZE_TOOL") == "addr2line":
        found = shutil.which("addr2line")
        return (found, "binutils") if found else (None, None)
    found = shutil.which("llvm-symbolizer")
    if found:
        return found, "llvm"
    roots = [os.environ.get("ANDROID_NDK_HOME", ""), os.path.expanduser("~/.local/share/QPM-RS/ndk/*")]
    for root in roots:
        for path in sorted(glob.glob(os.path.join(root, "toolchains/llvm/prebuilt/*/bin/llvm-symbolizer"))):
            return path, "llvm"
    found = shutil.which("addr2line")
    return (found, "binutils") if found else (None, None)


def index_libraries(dirs):
    libraries = {}
    for directory in dirs:
        for root, _, files in os.walk(directory):
            for name in files:
                if ".so" in name:
                    # The last one found wins: list the newest build's directory last.
                    libraries[name.lower()] = os.path.join(root, name)
    return libraries


def lookup(tool, kind, library, addresses):
    """{address: name} for one library."""
    hexes = ["0x%x" % a for a in addresses]
    if kind == "llvm":
        args = [tool, "--obj=" + library, "--functions=linkage", "--demangle", "--no-inlines"] + hexes
    else:
        args = [tool, "-f", "-C", "-e", library] + hexes
    try:
        out = subprocess.run(args, capture_output=True, text=True, timeout=120).stdout.splitlines()
    except (OSError, subprocess.TimeoutExpired):
        return {}
    names = {}
    if kind == "llvm":
        # Each address: function, file:line, then a blank line.
        blocks, current = [], []
        for line in out:
            if line.strip() == "":
                if current:
                    blocks.append(current)
                current = []
            else:
                current.append(line)
        if current:
            blocks.append(current)
        for address, block in zip(addresses, blocks):
            if block and block[0] != "??":
                names[address] = block[0]
    else:
        for address, i in zip(addresses, range(0, len(out), 2)):
            if out[i] != "??":
                names[address] = out[i]
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log")
    parser.add_argument("--libs", nargs="*", default=None)
    parser.add_argument("--all", action="store_true")
    options = parser.parse_args()
    dirs = options.libs if options.libs is not None else [d for d in ("testlogs/symbols", "testlogs/libs")
                                                          if os.path.isdir(d)]
    libraries = index_libraries(dirs)
    tool, kind = find_symbolizer()
    if tool is None:
        sys.exit("neither llvm-symbolizer nor addr2line found")
    with open(options.log, errors="replace") as f:
        lines = f.read().splitlines()

    wanted = {}
    for line in lines:
        for module, _, address in TOKEN.findall(line):
            wanted.setdefault(module.lower(), set()).add(int(address, 16))
        for _, address, _, module in FRAME.findall(line):
            wanted.setdefault(module.lower(), set()).add(int(address, 16))
    names = {}
    missing = []
    for module, addresses in sorted(wanted.items()):
        library = libraries.get(module)
        if library is None:
            missing.append(module)
            continue
        ordered = sorted(addresses)
        for address, name in lookup(tool, kind, library, ordered).items():
            names[(module, address)] = name

    def name_token(match):
        name = names.get((match.group(1).lower(), int(match.group(3), 16)))
        return match.group(0) + (" [%s]" % name if name else "")

    def name_frame(match):
        name = names.get((match.group(4).lower(), int(match.group(2), 16)))
        return match.group(0) + (" [%s]" % name if name else "")

    for line in lines:
        named = FRAME.sub(name_frame, TOKEN.sub(name_token, line))
        if options.all or named != line or (TOKEN.search(line) or FRAME.search(line)):
            print(named)
    if missing:
        print("not named (library not found under %s): %s" % (", ".join(dirs) or "no directory",
                                                               ", ".join(missing)), file=sys.stderr)


if __name__ == "__main__":
    main()
