#!/usr/bin/env python3
"""Runs every test step of .github/workflows/validate.yml on this computer.

    python3 tools/run-host-tests.py [job ...]

Each job's `run:` steps run in order with bash (in the step's
working-directory), the same commands CI runs. Steps that only install
packages (apt-get, pip install) are skipped: install those once yourself.

Needs: g++, glslangValidator, zlib headers, pwsh, and a Python with PyYAML,
lz4, UnityPy and semver (run this script with that Python: its folder is put
first on PATH, so the tests' own `python3` is the same one). When
`clang++-18` is not installed, the host syntax check uses the Android NDK's
clang (ndkpath.txt or ANDROID_NDK_HOME) with the NDK's libc++ headers.

Exit status 0 when every job passed.
"""
import os
import stat
import subprocess
import sys
import tempfile
import time

try:
    import yaml
except ImportError:
    print("PyYAML is missing: pip install pyyaml lz4 UnityPy semver (in a venv), then run this with that python")
    sys.exit(2)

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def ndk_path():
    path = os.path.join(REPO, "ndkpath.txt")
    if os.path.exists(path):
        with open(path) as f:
            return f.readline().strip()
    return os.environ.get("ANDROID_NDK_HOME", "")


def host_bin():
    """A folder with clang++-18, when the system has none."""
    for folder in os.environ.get("PATH", "").split(os.pathsep):
        if os.path.exists(os.path.join(folder, "clang++-18")):
            return None
    ndk = ndk_path()
    prebuilt = os.path.join(ndk, "toolchains", "llvm", "prebuilt", "linux-x86_64")
    if not os.path.isdir(prebuilt):
        return None
    folder = tempfile.mkdtemp(prefix="vivify-hostbin-")
    wrapper = os.path.join(folder, "clang++-18")
    with open(wrapper, "w") as f:
        f.write('#!/bin/sh\nexec "%s/bin/clang++" --target=x86_64-linux-gnu -nostdinc++ '
                '-isystem "%s/sysroot/usr/include/c++/v1" "$@"\n' % (prebuilt, prebuilt))
    os.chmod(wrapper, os.stat(wrapper).st_mode | stat.S_IXUSR)
    return folder


def main():
    wanted = set(sys.argv[1:])
    with open(os.path.join(REPO, ".github", "workflows", "validate.yml")) as f:
        workflow = yaml.safe_load(f)
    env = dict(os.environ)
    paths = [os.path.dirname(sys.executable)]
    extra = host_bin()
    if extra:
        paths.append(extra)
    env["PATH"] = os.pathsep.join(paths + [env.get("PATH", "")])

    results = []
    for name, job in workflow.get("jobs", {}).items():
        if wanted and name not in wanted:
            continue
        started = time.time()
        ok = True
        log = []
        for step in job.get("steps", []):
            command = step.get("run")
            if not command:
                continue
            if "apt-get" in command or "pip install" in command:
                lines = [line for line in command.splitlines()
                         if "apt-get" not in line and "pip install" not in line and line.strip()]
                if not lines:
                    continue
                command = "\n".join(lines)
            cwd = os.path.join(REPO, step.get("working-directory", "."))
            if step.get("shell") == "pwsh":
                runner = ["pwsh", "-NoProfile", "-NonInteractive", "-Command", command]
            else:
                runner = ["bash", "-eo", "pipefail", "-c", command]
            result = subprocess.run(runner, cwd=cwd, env=env, capture_output=True, text=True, errors="replace")
            log.append("$ %s\n%s%s" % (step.get("name", command[:60]), result.stdout, result.stderr))
            if result.returncode != 0:
                ok = False
                break
        seconds = time.time() - started
        results.append((name, ok, seconds))
        print("%s  %-28s %5.0f s" % ("PASS" if ok else "FAIL", name, seconds), flush=True)
        if not ok:
            print("\n".join(log)[-4000:], flush=True)
    failed = [name for name, ok, _ in results if not ok]
    print("%d of %d jobs passed" % (len(results) - len(failed), len(results)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
