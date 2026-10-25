#!/usr/bin/env python3
"""Offline shader audit of a library of Vivify maps (naomi.10).

    audit.py --conv <host conv> --bundles <dir> --out <dir> [--jobs N]

<bundles>/<map key>/ holds a map's bundle files as on the headset
(bundleWindows2021.vivify, bundleWindows2019.vivify, bundleAndroid2021.vivify).

- A PC map (no Android bundle) is converted the way the headset converts it
  (2021: --shaders-split; 2019: --shaders; geometry emulation on, unused
  variants skipped), then every translated program is compiled and linked
  with glslangValidator. A map that ships both builds has both checked: the
  headset takes whichever its folder lists first.
- A Quest map's bundle is repacked uncompressed and the GLSL Unity built for it
  is compiled and linked the same way; a shader that has no GLES program at all
  was shipped empty, and the headset draws its objects with a stand-in unless a
  PC build supplies the shader.

Writes <out>/audit.json and prints one line per map and a total. Needs
glslangValidator on PATH.
"""
import argparse, concurrent.futures, json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "bundleconvert"))
from mkbundle import read_converted  # noqa: E402

GLES_TYPES = {4, 5}  # Unity's GLES 3 GPU program types in this format (from the converter's own output)


def run(args, env=None):
    return subprocess.run(args, capture_output=True, text=True, errors="replace", env=env)


def split_stages(code):
    """The #ifdef VERTEX / FRAGMENT / GEOMETRY sections of a Unity GLSL program."""
    sections, stage, depth, lines = {}, None, 0, []
    for line in code.split("\n"):
        s = line.strip()
        if stage is None:
            m = re.match(r"#ifdef (VERTEX|FRAGMENT|GEOMETRY)$", s)
            if m:
                stage, depth, lines = m.group(1), 0, []
            continue
        if s.startswith("#if"):
            depth += 1
        elif s.startswith("#endif"):
            if depth == 0:
                sections[stage] = "\n".join(lines) + "\n"
                stage = None
                continue
            depth -= 1
        lines.append(line)
    return sections


def validate(job):
    """Compile and link one program's stages; returns (ok, first error line)."""
    code, work = job
    secs = split_stages(code)
    if "VERTEX" not in secs:
        return None  # not a whole program (a stage kept for Unity's own use)
    files = []
    for stage, ext in (("VERTEX", "vert"), ("FRAGMENT", "frag"), ("GEOMETRY", "geom")):
        if stage in secs:
            fd, path = tempfile.mkstemp(suffix="." + ext, dir=work)
            with os.fdopen(fd, "w") as f:
                f.write(secs[stage])
            files.append(path)
    r = run(["glslangValidator", "-l"] + files)
    output = r.stdout + r.stderr  # glslang reports on either, by version
    if r.returncode != 0 and ("'binding' : not supported" in output or "location qualifier on uniform" in output):
        # Unity's own GLES programs say "#version 300 es" and use ES 3.1
        # binding/location qualifiers, which the headset's GLES 3.2 driver
        # takes; glslang holds them to 3.00. Checked again as 3.10.
        for path in files:
            with open(path) as f:
                text = f.read()
            with open(path, "w") as f:
                f.write(text.replace("#version 300 es", "#version 310 es", 1))
        r = run(["glslangValidator", "-l"] + files)
    for path in files:
        os.unlink(path)
    if r.returncode == 0:
        return (True, "")
    error = next((l.strip() for l in (r.stdout + r.stderr).splitlines() if "ERROR" in l), "unknown error")
    return (False, re.sub(r"/[^ :]*/tmp[^ :]*\.(vert|frag|geom)", "<\\1>", error)[:300])


def programs_of(conv, bundle, work):
    """(shader name, code, is GLSL) for every program entry in an uncompressed bundle."""
    _, _, _, nodes, data = read_converted(bundle)
    out = []
    for off, size, _, name in nodes:
        if name.endswith(".resS") or name.endswith(".resource"):
            continue
        path = os.path.join(work, "sf.bin")
        with open(path, "wb") as f:
            f.write(data[off:off + size])
        text = run([conv, "--inspect", path]).stdout
        shader = None
        for line in text.splitlines():
            if line.startswith("shader="):
                shader = line[7:].split(" platforms")[0]
                out.append((shader, None, False))
            elif line.startswith("entry ") and " code=" in line and shader:
                code = line.split(" code=", 1)[1].replace("\\n", "\n")
                out.append((shader, code, "#ifdef VERTEX" in code or "#version" in code))
    return out


def audit_bundle(conv, key, kind, src, mode, work, pool):
    result = {"key": key, "kind": kind, "bundle": os.path.basename(src), "conversion": {}, "shaders": {}}
    if kind == "quest":
        bundle = os.path.join(work, key + "-repacked.vivify")
        r = run([conv, "--repack", src, bundle])
        if r.returncode != 0 or not os.path.exists(bundle):
            result["error"] = "repack failed: " + (r.stdout + r.stderr).strip()[:200]
            return result
    else:
        bundle = os.path.join(work, key + "-converted.vivify")
        env = dict(os.environ, VIVIFY_EMULATE_GEOMETRY="1", VIVIFY_SKIP_UNUSED="1")
        r = run([conv, mode, src, bundle], env=env)
        for line in r.stdout.splitlines():
            for part in line.split():
                if "=" in part and not part.startswith(("message=", "refusal=")):
                    k, v = part.split("=", 1)
                    result["conversion"][k] = v
        result["conversion"]["refusals"] = [l[8:] for l in r.stdout.splitlines() if l.startswith("refusal=")]
        if not os.path.exists(bundle):
            result["error"] = "conversion failed: " + r.stdout.strip()[:300]
            return result
    progs = programs_of(conv, bundle, work)
    jobs, owners = [], []
    for shader, code, glsl in progs:
        s = result["shaders"].setdefault(shader, {"programs": 0, "glsl": 0, "ok": 0, "failed": 0, "errors": []})
        if code is None:
            continue
        s["programs"] += 1
        if glsl:
            s["glsl"] += 1
            jobs.append((code, work))
            owners.append(shader)
    for shader, verdict in zip(owners, pool.map(validate, jobs)):
        if verdict is None:
            continue
        s = result["shaders"][shader]
        if verdict[0]:
            s["ok"] += 1
        else:
            s["failed"] += 1
            if len(s["errors"]) < 3 and verdict[1] not in s["errors"]:
                s["errors"].append(verdict[1])
    os.unlink(bundle)
    return result


def audit_map(conv, key, folder, work, pool):
    """One result per bundle the headset could use for this map."""
    files = set(os.listdir(folder))
    if "bundleAndroid2021.vivify" in files:
        return [audit_bundle(conv, key, "quest", os.path.join(folder, "bundleAndroid2021.vivify"), None, work, pool)]
    out = []
    for name, mode in (("bundleWindows2021.vivify", "--shaders-split"), ("bundleWindows2019.vivify", "--shaders")):
        if name in files:
            out.append(audit_bundle(conv, key, "pc", os.path.join(folder, name), mode, work, pool))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--conv", required=True)
    parser.add_argument("--bundles", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = parser.parse_args()
    os.makedirs(a.out, exist_ok=True)
    results = []
    with tempfile.TemporaryDirectory() as work, concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for key in sorted(os.listdir(a.bundles)):
            folder = os.path.join(a.bundles, key)
            if not os.path.isdir(folder):
                continue
            for r in audit_map(a.conv, key, folder, work, pool):
                results.append(r)
                shaders = r["shaders"].values()
                empty = sum(1 for s in shaders if s["glsl"] == 0)
                year = re.search(r"(\d{4})\.vivify$", r["bundle"]).group(1)
                print("%-6s %-5s %s shaders %3d, programs %5d, GLSL %5d, link ok %5d, failed %3d, "
                      "shaders with no GLSL %3d%s" % (
                          key, r["kind"], year, len(r["shaders"]), sum(s["programs"] for s in shaders),
                          sum(s["glsl"] for s in shaders), sum(s["ok"] for s in shaders),
                          sum(s["failed"] for s in shaders), empty,
                          ("  ERROR " + r["error"]) if "error" in r else ""), flush=True)
    with open(os.path.join(a.out, "audit.json"), "w") as f:
        json.dump(results, f, indent=1)
    total_ok = sum(s["ok"] for r in results for s in r["shaders"].values())
    total_failed = sum(s["failed"] for r in results for s in r["shaders"].values())
    print("total: %d programs link, %d fail" % (total_ok, total_failed))


if __name__ == "__main__":
    main()
