#!/usr/bin/env python3
"""Tests for tools/symbolize/symbolize.py: builds a small shared library with
the host compiler, writes log lines naming two of its functions the way the
slow-frame summary and a watchdog sample do, and checks the names come back.
Needs gcc and nm (and addr2line or llvm-symbolizer)."""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
failures = 0


def check(name, cond, detail=""):
    global failures
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else " " + str(detail)))
    if not cond:
        failures += 1


with tempfile.TemporaryDirectory() as work:
    source = os.path.join(work, "lib.c")
    with open(source, "w") as f:
        f.write("int vivify_alpha(int x) { return x * 3 + 1; }\n"
                "int vivify_beta(int x) { int s = 0; for (int i = 0; i < x; i++) s += vivify_alpha(i); return s; }\n")
    libs = os.path.join(work, "libs", "build")
    os.makedirs(libs)
    library = os.path.join(libs, "libVivifyTest.so")
    subprocess.run(["gcc", "-shared", "-fPIC", "-O1", "-g", "-o", library, source], check=True)
    symbols = {}
    for line in subprocess.run(["nm", library], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in ("vivify_alpha", "vivify_beta"):
            symbols[parts[2]] = int(parts[0], 16)
    check("test library built with both functions", len(symbols) == 2, symbols)
    alpha, beta = symbols.get("vivify_alpha", 0), symbols.get("vivify_beta", 0)
    log = os.path.join(work, "VivifySession.txt")
    with open(log, "w") as f:
        f.write("[INFO] an unrelated line\n")
        f.write("[PERF]   main thread was in: libvivifytest.so+0x%x 40%%, libmissing.so+0x10 5%%\n" % alpha)
        f.write("[PERF]   main thread, anywhere on the stack: libvivifytest.so@0x%x 20%%\n" % (beta + 4))
        f.write("[+12ms] WATCHDOG:   #01 pc %016x  libVivifyTest.so\n" % (beta + 2))
    for tool in ("default", "addr2line"):
        env = dict(os.environ, SYMBOLIZE_TOOL=tool)
        r = subprocess.run([sys.executable, os.path.join(HERE, "symbolize.py"), log, "--libs",
                            os.path.join(work, "libs")], capture_output=True, text=True, env=env)
        out = r.stdout.splitlines()
        check("%s: the same names" % tool, "[vivify_alpha]" in r.stdout and "[vivify_beta]" in r.stdout, out)
    check("only lines with something to name are printed", len(out) == 3 and "unrelated" not in r.stdout, out)
    check("a function start is named", "libvivifytest.so+0x%x [vivify_alpha] 40%%" % alpha in r.stdout, out)
    check("an address inside a function is named", "libvivifytest.so@0x%x [vivify_beta]" % (beta + 4) in r.stdout,
          out)
    check("a watchdog frame is named", re.search(r"#01 pc [0-9a-f]{16}  libVivifyTest\.so \[vivify_beta\]", r.stdout),
          out)
    check("a library that is not there is left alone and listed", "libmissing.so+0x10 5%" in r.stdout and
          "libmissing.so" in r.stderr, (out, r.stderr))
    r = subprocess.run([sys.executable, os.path.join(HERE, "symbolize.py"), log, "--libs", os.path.join(work, "libs"),
                        "--all"], capture_output=True, text=True)
    check("--all prints every line", len(r.stdout.splitlines()) == 4, r.stdout)

print("\n%d failure(s)" % failures)
sys.exit(1 if failures else 0)
