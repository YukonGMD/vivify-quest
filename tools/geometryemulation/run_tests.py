#!/usr/bin/env python3
"""Tests for src/VivifyGeometryEmulation.cpp and the translator's function
modes, on synthetic DXBC. Build with:
    g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I include -o /tmp/geoemu \\
        tools/geometryemulation/main.cpp src/VivifyGeometryEmulation.cpp src/VivifyDxbc.cpp
    VIVIFY_GEOEMU=/tmp/geoemu python3 tools/geometryemulation/run_tests.py
"""
import os, shutil, subprocess, sys, tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dxbc"))
import mkdxbc as m  # noqa: E402

BIN = os.environ.get("VIVIFY_GEOEMU", "/tmp/geoemu")
GLSLANG = shutil.which("glslangValidator")
tmp = tempfile.mkdtemp(prefix="geoemu-")
failures = 0

ADD, IF, ENDIF, MOV, MUL, RET = 0, 31, 21, 54, 56, 62
EMIT, CUT, DCL_GS_INPUT_PRIMITIVE, DCL_GS_OUTPUT_TOPOLOGY, DCL_MAXOUT = 19, 9, 93, 92, 94
DCL_CONSTANT_BUFFER = 89
X, Y, Z, W = 0, 1, 2, 3


def check(name, cond, detail=""):
    global failures
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else " " + str(detail)[:1500]))
    if not cond:
        failures += 1


def write(name, data):
    path = os.path.join(tmp, name)
    with open(path, "wb") as f:
        f.write(data)
    return path


def call(*args):
    r = subprocess.run([BIN, *map(str, args)], capture_output=True, text=True, timeout=60)
    if "AddressSanitizer" in r.stderr or "runtime error" in r.stderr:
        raise AssertionError(r.stderr[:800])
    return r.stdout


def fields(text):
    head, _, source = text.partition("---\n")
    out = {}
    for line in head.splitlines():
        key, _, value = line.partition("=")
        out.setdefault(key, value)
    return out, source


def globals_rdef():
    return m.rdef_chunk(constant_buffers=[{"name": "$Globals", "size": 16, "variables": [
        {"name": "_Scale", "offset": 0, "size": 16, "class": 1, "type": 3, "rows": 1, "columns": 4}]}])


POS = {"name": "SV_POSITION", "index": 0, "register": 0, "sv": 1}
UV = {"name": "TEXCOORD", "index": 0, "register": 1}


def vertex_program(extra_input=False):
    inputs = [{"name": "POSITION", "index": 0, "register": 0}]
    if extra_input:
        inputs.append({"name": "NORMAL", "index": 0, "register": 1})
    code = m.insn(DCL_CONSTANT_BUFFER, m.src_cb(0, 1))
    code += m.insn(MUL, m.dest(m.OPERAND_OUTPUT, 0), m.src(m.OPERAND_INPUT, 0), m.src_cb(0, 0))
    code += m.insn(MOV, m.dest(m.OPERAND_OUTPUT, 1), m.src(m.OPERAND_INPUT, 1 if extra_input else 0))
    code += m.insn(RET)
    return m.container([globals_rdef(), m.signature_chunk(inputs, b"ISGN"),
                        m.signature_chunk([POS, UV], b"OSGN"), m.shex_chunk([code], stage=1)])


def per_vertex(vertex, register):
    return m._operand(m.OPERAND_INPUT, [vertex, register], swizzle=(X, Y, Z, W))


def geometry_program(primitive=3, maxout=3, topology=5, emits=3, branch=False, mid_cut=False):
    code = m.insn(DCL_CONSTANT_BUFFER, m.src_cb(0, 1))
    code += m.insn(DCL_GS_INPUT_PRIMITIVE, controls=primitive)
    code += m.insn(DCL_GS_OUTPUT_TOPOLOGY, controls=topology)
    code += m.insn(DCL_MAXOUT, extra=[maxout])
    if branch:
        code += m.insn(IF, m.src_cb(0, 0, (X, X, X, X)), controls=(1 << 7))
        code += m.insn(ENDIF)
    vertices = 3 if primitive == 3 else 1
    for k in range(emits):
        source_vertex = k % vertices
        if primitive == 3:
            code += m.insn(MOV, m.dest(m.OPERAND_OUTPUT, 0), per_vertex(source_vertex, 0))
        else:
            code += m.insn(ADD, m.dest(m.OPERAND_OUTPUT, 0), per_vertex(0, 0), m.imm_float(0.1 * k, 0.2 * k, 0, 0))
        # A pass-through varying: TEXCOORD0 in and TEXCOORD0 out.
        code += m.insn(MUL, m.dest(m.OPERAND_OUTPUT, 1), per_vertex(source_vertex, 1), m.src_cb(0, 0))
        code += m.insn(EMIT)
        if mid_cut and k == 1:
            code += m.insn(CUT)
    code += m.insn(RET)
    return m.container([globals_rdef(), m.signature_chunk([POS, UV], b"ISGN"),
                        m.signature_chunk([POS, UV], b"OSGN"), m.shex_chunk([code], stage=2)])


def glslang(source, stage, label, link_with=None):
    if GLSLANG is None:
        return True
    path = write(label + "." + stage, source.encode())
    args = [GLSLANG, path]
    if link_with:
        other = write(label + "_link.frag", link_with.encode())
        args = [GLSLANG, "-l", path, other]
    r = subprocess.run(args, capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        print((r.stdout + r.stderr)[:1200])
    return r.returncode == 0


vs = write("vs.dxbc", vertex_program())
vs_normal = write("vs_normal.dxbc", vertex_program(extra_input=True))
tri = write("tri.dxbc", geometry_program())
point = write("point.dxbc", geometry_program(primitive=1, maxout=4, emits=4))

# Plans.
check("triangle plan", call("plan", tri).strip() == "mode=triangle vertices=3", call("plan", tri))
check("point plan", call("plan", point).strip() == "mode=point vertices=4", call("plan", point))
out = call("plan", write("four.dxbc", geometry_program(maxout=4, emits=4)))
check("a triangle program with a fourth emit is refused", "does not emit exactly its own three vertices" in out, out)
out = call("plan", write("branch.dxbc", geometry_program(branch=True)))
check("a triangle program with a branch is refused", "loops or branches" in out, out)
out = call("plan", write("cut.dxbc", geometry_program(mid_cut=True)))
check("a cut between emits is refused", "ends a strip between its emits" in out, out)
out = call("plan", write("lines.dxbc", geometry_program(topology=3)))
check("a line-strip output is refused", "does not output a triangle strip" in out, out)
out = call("plan", write("lineinput.dxbc", geometry_program(primitive=2)))
check("a line input is refused", "neither points nor triangles" in out, out)
out = call("plan", vs)
check("a vertex program is not a geometry plan", out.startswith("refused="), out)

# Fusing a triangle program that passes TEXCOORD0 through (what the stage-by-
# stage translation refuses).
for multiview in (False, True):
    label = "triangle" + (" multiview" if multiview else "")
    f, source = fields(call("fuse", vs, tri, *(["mv"] if multiview else [])))
    check(label + ": composes", f.get("ok") == "1", f.get("error", ""))
    check(label + ": runs the vertex program per corner",
          "vivify_vi_POSITION0 = vivify_corners[vivify_c];" in source and "vivify_vs_main();" in source, source)
    check(label + ": feeds the geometry program its inputs",
          "vivify_gi_TEXCOORD0[vivify_c] = vivify_vo_TEXCOORD0;" in source and
          "vivify_gi_Position[vivify_c] = vivify_vo_Position;" in source, source)
    check(label + ": the pass-through varying is an output once",
          source.count("out vec4 vs_TEXCOORD0;") == 1, source)
    check(label + ": a uniform both stages read is declared once", source.count("uniform vec4 _Scale;") == 1,
          source)
    check(label + ": other corners and corner index are attributes",
          all(("in vec4 in_TEXCOORD%d;" % c) in source for c in (5, 6, 7)), source)
    check(label + ": unprepared meshes draw nothing", "if (abs(in_TEXCOORD7.w - 7.0) > 0.5) return;" in source,
          source)
    check(label + ": no geometry-stage syntax left",
          "EmitVertex" not in source and "gl_in" not in source and "layout(triangles)" not in source, source)
    check(label + ": multiview declared only when asked", ("num_views = 2" in source) == multiview, source)
    check(label + ": glslang compiles it", glslang(source, "vert", "tri%d" % multiview), source)
    fragment = ("#version 300 es\nprecision highp float;\nin vec4 vs_TEXCOORD0;\n"
                "layout(location = 0) out vec4 SV_Target0;\nvoid main() { SV_Target0 = vs_TEXCOORD0; }\n")
    check(label + ": glslang links it with a fragment program",
          glslang(source, "vert", "trilink%d" % multiview, link_with=fragment))

f, source = fields(call("fuse", vs, point, "mv"))
check("point: composes", f.get("ok") == "1", f.get("error", ""))
check("point: picks its emit from the strip position", "vivify_wantEmit = int(in_TEXCOORD7.x + 0.5);" in source,
      source)
check("point: no other-corner attributes", "in_TEXCOORD5" not in source, source)
check("point: glslang compiles it", glslang(source, "vert", "point"), source)

f, source = fields(call("fuse", vs_normal, tri))
check("triangle: a vertex program reading more than a position is refused",
      f.get("ok") == "0" and "only POSITION0 can be supplied" in f.get("error", ""), f)
f, source = fields(call("fuse", vs_normal, point))
check("point: a vertex program reading a normal is fine", f.get("ok") == "1", f.get("error", ""))

# Mesh expansion.
out = call("expand-triangles", 4, "0,1,2,2,1,3").splitlines()
check("triangles: one vertex per corner", [l for l in out if l.startswith("v ")] == [
    "v 0 0 1 2", "v 1 1 2 0", "v 2 2 0 1", "v 2 0 1 3", "v 1 1 3 2", "v 3 2 2 1"], out)
check("triangles: indices in order", "s 0 1 2 3 4 5" in out, out)
out = call("expand-triangles", 3, "0,1,2", "2,1,0").splitlines()
check("triangles: submeshes kept apart", out[-2:] == ["s 0 1 2", "s 3 4 5"], out)
check("triangles: out-of-range index refused",
      call("expand-triangles", 2, "0,1,2").startswith("refused="), "")
check("triangles: ragged list refused", call("expand-triangles", 3, "0,1").startswith("refused="), "")
out = call("expand-points", 2, 5, "0,1").splitlines()
check("points: one vertex per strip position", [l for l in out if l.startswith("v ")][:5] == [
    "v 0 0", "v 0 1", "v 0 2", "v 0 3", "v 0 4"], out)
check("points: strip triangles alternate winding",
      out[-1] == "s 0 1 2 2 1 3 2 3 4 5 6 7 7 6 8 7 8 9", out[-1])
check("points: strips longer than the limit refused",
      call("expand-points", 1, 40, "0").startswith("refused="), "")

print("\n%d failure(s)" % failures)
sys.exit(1 if failures else 0)
