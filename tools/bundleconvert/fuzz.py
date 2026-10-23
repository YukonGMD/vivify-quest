import os, sys, random, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkbundle import build
HERE = os.path.dirname(os.path.abspath(__file__))
CONV = os.environ.get("VIVIFY_CONV", "/tmp/conv"); TMP = os.path.join(HERE, "work")
src = os.path.join(TMP, "fz_src.vivify"); mut = os.path.join(TMP, "fz.vivify")
rnd = random.Random(1234)
crashes = 0
def crashed(p):
    # exit 0/1 = clean success/handled failure; anything else is a crash. A
    # sanitizer report exits 1 as well (ASan's default) or not at all (UBSan),
    # so its output is looked for too.
    return p.returncode not in (0, 1) or b"AddressSanitizer" in p.stderr or b"runtime error" in p.stderr
for comp in (0, 1, 2, 3):
    build(src, compression=comp, block_compression=comp, n_files=2, payload=9000)
    base = bytearray(open(src, 'rb').read())
    for trial in range(150):
        b = bytearray(base)
        for _ in range(rnd.randrange(1, 12)):
            b[rnd.randrange(len(b))] = rnd.randrange(256)
        open(mut, 'wb').write(bytes(b))
        p = subprocess.run([CONV, mut, os.path.join(TMP, "fz.out")], capture_output=True)
        if crashed(p):
            crashes += 1
            print(f"CRASH comp={comp} trial={trial} rc={p.returncode}")
            print(p.stderr[:3000].decode("utf-8", "replace"))
            open(os.path.join(TMP, f"crash_{comp}_{trial}.bin"), 'wb').write(bytes(b))
            if crashes > 3: sys.exit(1)
# A 2021.3.16-layout PC shader (written by run_tests.py): corrupting it walks
# m_ParsedForm, the segmented program store and the in-place program-list
# patches over hostile bytes. The mutations are kept inside the serialized
# file, where they reach the parser rather than only the archive header.
seed = os.path.join(TMP, "fuzz_seed_2021.vivify")
if os.path.exists(seed):
    base = bytearray(open(seed, 'rb').read())
    lo = len(base) // 8
    for trial in range(300):
        b = bytearray(base)
        for _ in range(rnd.randrange(1, 10)):
            b[rnd.randrange(lo, len(b))] = rnd.randrange(256)
        open(mut, 'wb').write(bytes(b))
        p = subprocess.run([CONV, "--shaders", mut, os.path.join(TMP, "fz.out")], capture_output=True)
        if crashed(p):
            crashes += 1
            print(f"CRASH parsed-form trial={trial} rc={p.returncode}")
            print(p.stderr[:3000].decode("utf-8", "replace"))
            open(os.path.join(TMP, f"crash_pf_{trial}.bin"), 'wb').write(bytes(b))
            if crashes > 3: sys.exit(1)
else:
    print("note: no fuzz_seed_2021.vivify (run run_tests.py first); m_ParsedForm fuzzing skipped")
# Meshes under geometry-shader emulation (naomi.6): every field of every Mesh
# in a file holding a known geometry shader is walked to decide which may be
# marked readable. The mutations are kept inside the mesh bodies, the last
# objects in the file, in both stream-offset widths.
import mkshader
stream = dict(stream_offset=934508, stream_size=12684, stream_path="archive:/CAB-0/CAB-0.resS")
meshes = [dict(name="CubeKept", keep_vertices=True, keep_indices=True), dict(name="NurbsPath", vertex_count=453, **stream),
          dict(name="StreamedFlagged", is_readable=True, **stream),
          dict(name="Compressed", compression=3, vertex_count=0, inline_data=b"")]
emulate = dict(os.environ, VIVIFY_EMULATE_GEOMETRY="1")
for offset64, sf_version in ((False, 21), (True, 22)):
    sf = mkshader.serialized_file_with_shaders([("Swifter/TriangleExplosion", [4])], meshes=meshes,
                                               mesh_offset64=offset64, sf_version=sf_version)
    build(src, sf_bytes=[sf], with_resource=False, compression=0, block_compression=0)
    base = bytearray(open(src, 'rb').read())
    lo = base.find(b"CubeKept") - 4
    for trial in range(200):
        b = bytearray(base)
        for _ in range(rnd.randrange(1, 8)):
            b[rnd.randrange(lo, len(b))] = rnd.randrange(256)
        open(mut, 'wb').write(bytes(b))
        p = subprocess.run([CONV, "--shaders", mut, os.path.join(TMP, "fz.out")], capture_output=True, env=emulate)
        if crashed(p):
            crashes += 1
            print(f"CRASH mesh offset64={offset64} trial={trial} rc={p.returncode}")
            print(p.stderr[:3000].decode("utf-8", "replace"))
            open(os.path.join(TMP, f"crash_mesh_{int(offset64)}_{trial}.bin"), 'wb').write(bytes(b))
            if crashes > 3: sys.exit(1)
print("crashes:", crashes)
sys.exit(1 if crashes else 0)
