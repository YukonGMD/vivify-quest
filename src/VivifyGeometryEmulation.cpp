#include "VivifyGeometryEmulation.hpp"

#include <algorithm>
#include <map>
#include <regex>
#include <set>

namespace Vivify::GeometryEmulation {

namespace {

// D3D10/11 shader bytecode opcodes (fixed by the format).
constexpr uint32_t kOpCut = 9;
constexpr uint32_t kOpDiscard = 13;
constexpr uint32_t kOpEmit = 19;
constexpr uint32_t kOpEmitThenCut = 20;
constexpr uint32_t kOpIf = 31;
constexpr uint32_t kOpLoop = 48;
constexpr uint32_t kOpSwitch = 76;
constexpr uint32_t kOpCall = 4;
constexpr uint32_t kOpCallC = 5;
constexpr uint32_t kOpGsOutputTopology = 92;
constexpr uint32_t kOpGsInputPrimitive = 93;
constexpr uint32_t kOpEmitStream = 117;
constexpr uint32_t kOpCutStream = 118;
constexpr uint32_t kOpEmitThenCutStream = 119;
constexpr uint32_t kOpDclStream = 143;
constexpr uint32_t kOpGsInstanceCount = 206;

constexpr uint32_t kMaxRebuiltVertices = 4u * 1024u * 1024u;

bool IsEmit(uint32_t opcode) {
  return opcode == kOpEmit || opcode == kOpEmitStream || opcode == kOpEmitThenCut ||
         opcode == kOpEmitThenCutStream;
}
bool IsCut(uint32_t opcode) {
  return opcode == kOpCut || opcode == kOpCutStream;
}

struct Split {
  std::string declarations;
  std::string functions;
};
std::optional<Split> SplitFunctionSource(std::string const& source) {
  size_t const at = source.find(Dxbc::kFunctionDeclarationsEnd);
  if (at == std::string::npos) return std::nullopt;
  return Split{source.substr(0, at), source.substr(at + std::string(Dxbc::kFunctionDeclarationsEnd).size())};
}

std::vector<std::string> Lines(std::string const& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

// The names a declaration line introduces: variables, arrays, block members
// and block names.
std::set<std::string> DeclaredNames(std::string const& line) {
  static std::regex const variable(R"((\w+)\s*(\[[^\]]*\])?\s*(;|=))");
  static std::regex const block(R"(uniform\s+(\w+)\s*\{)");
  std::set<std::string> names;
  for (auto it = std::sregex_iterator(line.begin(), line.end(), variable); it != std::sregex_iterator(); ++it) {
    names.insert((*it)[1].str());
  }
  for (auto it = std::sregex_iterator(line.begin(), line.end(), block); it != std::sregex_iterator(); ++it) {
    names.insert((*it)[1].str());
  }
  return names;
}

// Suffixes of `vec4 <prefix>NAME;` (or `vec4 <prefix>NAME[n];`) declarations.
std::vector<std::string> DeclaredWithPrefix(std::string const& declarations, std::string const& prefix) {
  std::regex const pattern("(?:^|\\n)vec4 " + prefix + R"((\w+)(\[\d+\])?;)");
  std::vector<std::string> out;
  for (auto it = std::sregex_iterator(declarations.begin(), declarations.end(), pattern);
       it != std::sregex_iterator(); ++it) {
    out.push_back((*it)[1].str());
  }
  return out;
}

}  // namespace

bool IsKnownShader(std::string_view shaderName) {
  return shaderName == "Swifter/TriangleExplosion" || shaderName == "Mawntee/Beat Saber Wireframe 3D-Noise" ||
         shaderName == "Bytesized/Grass";
}

char const* ModeName(Mode mode) {
  return mode == Mode::Triangle ? "triangle" : "point";
}

std::optional<Plan> PlanFor(Dxbc::Program const& geometry, std::string& reason) {
  if (!geometry.ok) {
    reason = "the geometry program did not parse";
    return std::nullopt;
  }
  int inputVertices = 0;
  int topology = 0;
  int emits = 0;
  int firstEmit = -1;
  int lastEmit = -1;
  bool flowControl = false;
  bool loops = false;
  std::vector<int> cuts;
  for (size_t i = 0; i < geometry.instructions.size(); i++) {
    auto const& instruction = geometry.instructions[i];
    uint32_t const opcode = instruction.opcode;
    if (opcode == kOpGsInputPrimitive) {
      uint32_t const primitive = instruction.controls & 0x3fu;
      inputVertices = primitive == 1 ? 1 : primitive == 3 ? 3 : -1;
    } else if (opcode == kOpGsOutputTopology) {
      topology = static_cast<int>(instruction.controls & 0x3fu);
    } else if (opcode == kOpGsInstanceCount) {
      if (!instruction.extra.empty() && instruction.extra[0] > 1) {
        reason = "the geometry program runs several instances per primitive";
        return std::nullopt;
      }
    } else if (opcode == kOpDclStream) {
      if (!instruction.operands.empty() && !instruction.operands[0].indices.empty() &&
          instruction.operands[0].indices.back().immediate != 0) {
        reason = "the geometry program writes a second stream";
        return std::nullopt;
      }
    } else if (IsEmit(opcode)) {
      emits++;
      if (firstEmit < 0) firstEmit = static_cast<int>(i);
      lastEmit = static_cast<int>(i);
      if (opcode == kOpEmitThenCut || opcode == kOpEmitThenCutStream) cuts.push_back(static_cast<int>(i) + 1);
    } else if (IsCut(opcode)) {
      cuts.push_back(static_cast<int>(i));
    } else if (opcode == kOpLoop) {
      loops = true;
    } else if (opcode == kOpIf || opcode == kOpSwitch || opcode == kOpDiscard || opcode == kOpCall ||
               opcode == kOpCallC) {
      flowControl = true;
    }
  }
  if (inputVertices != 1 && inputVertices != 3) {
    reason = "the geometry program's input is neither points nor triangles";
    return std::nullopt;
  }
  if (topology != 5) {
    reason = "the geometry program does not output a triangle strip";
    return std::nullopt;
  }
  // A cut in the middle would split the strip into pieces this cannot place.
  for (int cut : cuts) {
    if (cut > firstEmit && cut <= lastEmit) {
      reason = "the geometry program ends a strip between its emits";
      return std::nullopt;
    }
  }
  int const declared = static_cast<int>(geometry.maxOutputVertexCount);
  Plan plan;
  if (inputVertices == 3) {
    if (loops || flowControl) {
      reason = "the triangle geometry program has loops or branches, so which vertex each emit is cannot be known";
      return std::nullopt;
    }
    if (emits != 3 || declared != 3) {
      reason = "the triangle geometry program does not emit exactly its own three vertices (" +
               std::to_string(emits) + " emit(s), " + std::to_string(declared) + " declared)";
      return std::nullopt;
    }
    plan.mode = Mode::Triangle;
    plan.outputVertices = 3;
    return plan;
  }
  if (declared < 3 || declared > kMaxStripVertices) {
    reason = "the point geometry program declares " + std::to_string(declared) +
             " output vertices; between 3 and " + std::to_string(kMaxStripVertices) + " can be emulated";
    return std::nullopt;
  }
  if (emits == 0) {
    reason = "the point geometry program never emits";
    return std::nullopt;
  }
  plan.mode = Mode::Point;
  plan.outputVertices = declared;
  return plan;
}

Composed Compose(Dxbc::GlslResult const& vertexFunction, Dxbc::GlslResult const& geometryFunction,
                 Plan const& plan, bool multiview) {
  Composed out;
  auto const vs = SplitFunctionSource(vertexFunction.source);
  auto const gs = SplitFunctionSource(geometryFunction.source);
  if (!vertexFunction.ok || !geometryFunction.ok || !vs || !gs) {
    out.error = "a stage was not translated in its function mode";
    return out;
  }
  int const expectedInputs = plan.mode == Mode::Triangle ? 3 : 1;
  if (geometryFunction.geometryInputVertices != expectedInputs) {
    out.error = "the geometry program's input primitive does not match the plan";
    return out;
  }
  if (gs->declarations.find("vec4 vivify_go_Position;") == std::string::npos) {
    out.error = "the geometry program does not write a position";
    return out;
  }
  if (vs->declarations.find("vec4 vivify_vo_Position;") == std::string::npos) {
    out.error = "the vertex program does not write a position";
    return out;
  }

  auto const vertexInputs = DeclaredWithPrefix(vs->declarations, "vivify_vi_");
  auto const vertexOutputs = DeclaredWithPrefix(vs->declarations, "vivify_vo_");
  auto const geometryInputs = DeclaredWithPrefix(gs->declarations, "vivify_gi_");
  std::set<std::string> const written(vertexOutputs.begin(), vertexOutputs.end());
  for (auto const& input : geometryInputs) {
    if (!written.count(input)) {
      out.error = "the geometry program reads " + input + ", which the vertex program does not write";
      return out;
    }
  }
  std::string const cornerAttribute = "TEXCOORD" + std::to_string(kCornerChannel);
  if (plan.mode == Mode::Triangle) {
    // The other corners can only supply their positions.
    for (auto const& input : vertexInputs) {
      if (input != "POSITION0") {
        out.error = "the vertex program reads " + input +
                    "; only POSITION0 can be supplied for the other corners of a triangle";
        return out;
      }
    }
    if (vertexInputs.empty()) {
      out.error = "the vertex program reads no position";
      return out;
    }
  } else {
    for (auto const& input : vertexInputs) {
      if (input == cornerAttribute) {
        out.error = "the vertex program reads " + input + ", which the rebuilt mesh uses for the strip position";
        return out;
      }
    }
  }

  // The two stages' declarations, with each name declared once. Identical
  // lines (the same uniform, the same block) are kept once; the same name
  // declared two different ways cannot be reconciled here.
  if (vs->declarations.find("ImmCB") != std::string::npos && gs->declarations.find("ImmCB") != std::string::npos) {
    out.error = "both stages carry an immediate constant buffer";
    return out;
  }
  std::set<std::string> vertexLines;
  std::map<std::string, std::string> vertexNames;  // name -> its line
  for (auto const& line : Lines(vs->declarations)) {
    vertexLines.insert(line);
    for (auto const& name : DeclaredNames(line)) vertexNames[name] = line;
  }
  std::string geometryDeclarations;
  for (auto const& line : Lines(gs->declarations)) {
    if (vertexLines.count(line)) continue;
    for (auto const& name : DeclaredNames(line)) {
      if (vertexNames.count(name)) {
        out.error = "both stages declare '" + name + "' differently";
        return out;
      }
    }
    geometryDeclarations += line + "\n";
  }

  out.version = std::max({300, vertexFunction.version, geometryFunction.version});
  std::string& src = out.source;
  src += "#version " + std::to_string(out.version) + " es\n";
  if (multiview) src += "#extension GL_OVR_multiview2 : require\nlayout(num_views = 2) in;\n";
  src += "precision highp float;\nprecision highp int;\n";
  for (auto const& input : vertexInputs) src += "in vec4 in_" + input + ";\n";
  if (plan.mode == Mode::Triangle) {
    src += "in vec4 in_TEXCOORD" + std::to_string(kOtherAChannel) + ";\n";
    src += "in vec4 in_TEXCOORD" + std::to_string(kOtherBChannel) + ";\n";
  }
  src += "in vec4 in_" + cornerAttribute + ";\n";
  src += vs->declarations;
  src += geometryDeclarations;
  src += vs->functions;
  src += gs->functions;

  std::string const corner = "in_" + cornerAttribute;
  std::string const marker = std::to_string(static_cast<int>(kMarker)) + ".0";
  src += "void main() {\n";
  // Clipped away unless the geometry program emits this vertex.
  src += "  gl_Position = vec4(0.0, 0.0, 2.0, 1.0);\n";
  src += "  if (abs(" + corner + ".w - " + marker + ") > 0.5) return;\n";
  auto copyOutputs = [&](std::string const& index) {
    std::string text;
    for (auto const& input : geometryInputs) {
      text += "    vivify_gi_" + input + "[" + index + "] = vivify_vo_" + input + ";\n";
    }
    return text;
  };
  if (plan.mode == Mode::Triangle) {
    src += "  int vivify_corner = clamp(int(" + corner + ".x + 0.5), 0, 2);\n";
    src += "  vec4 vivify_corners[3];\n";
    src += "  vivify_corners[vivify_corner] = in_POSITION0;\n";
    src += "  vivify_corners[(vivify_corner + 1) % 3] = in_TEXCOORD" + std::to_string(kOtherAChannel) + ";\n";
    src += "  vivify_corners[(vivify_corner + 2) % 3] = in_TEXCOORD" + std::to_string(kOtherBChannel) + ";\n";
    src += "  for (int vivify_c = 0; vivify_c < 3; vivify_c++) {\n";
    src += "    vivify_vi_POSITION0 = vivify_corners[vivify_c];\n";
    src += "    vivify_vs_main();\n";
    src += copyOutputs("vivify_c");
    src += "  }\n";
    src += "  vivify_wantEmit = vivify_corner;\n";
  } else {
    for (auto const& input : vertexInputs) src += "  vivify_vi_" + input + " = in_" + input + ";\n";
    src += "  {\n    vivify_vs_main();\n";
    src += copyOutputs("0");
    src += "  }\n";
    src += "  vivify_wantEmit = int(" + corner + ".x + 0.5);\n";
  }
  src += "  vivify_emitIndex = 0;\n";
  src += "  vivify_gs_main();\n";
  src += "}\n";
  out.ok = true;
  return out;
}

bool ExpandTriangles(std::vector<std::vector<uint32_t>> const& submeshes, uint32_t vertexCount, Expansion& out,
                     std::string& reason) {
  out = Expansion();
  size_t total = 0;
  for (auto const& indices : submeshes) {
    if (indices.size() % 3 != 0) {
      reason = "a submesh's triangle list is not a multiple of three indices";
      return false;
    }
    total += indices.size();
  }
  if (total > kMaxRebuiltVertices) {
    reason = "the rebuilt mesh would have " + std::to_string(total) + " vertices";
    return false;
  }
  out.source.reserve(total);
  out.corner.reserve(total);
  out.otherA.reserve(total);
  out.otherB.reserve(total);
  for (auto const& indices : submeshes) {
    std::vector<uint32_t> rebuilt;
    rebuilt.reserve(indices.size());
    for (size_t t = 0; t < indices.size(); t += 3) {
      uint32_t const triangle[3] = {indices[t], indices[t + 1], indices[t + 2]};
      for (uint32_t index : triangle) {
        if (index >= vertexCount) {
          reason = "an index (" + std::to_string(index) + ") is past the mesh's " + std::to_string(vertexCount) +
                   " vertices";
          return false;
        }
      }
      for (int k = 0; k < 3; k++) {
        rebuilt.push_back(static_cast<uint32_t>(out.source.size()));
        out.source.push_back(triangle[k]);
        out.corner.push_back(static_cast<float>(k));
        out.otherA.push_back(triangle[(k + 1) % 3]);
        out.otherB.push_back(triangle[(k + 2) % 3]);
      }
    }
    out.submeshes.push_back(std::move(rebuilt));
  }
  return true;
}

bool ExpandPoints(std::vector<std::vector<uint32_t>> const& submeshes, uint32_t vertexCount, int stripVertices,
                  Expansion& out, std::string& reason) {
  out = Expansion();
  if (stripVertices < 3 || stripVertices > kMaxStripVertices) {
    reason = "a strip of " + std::to_string(stripVertices) + " vertices";
    return false;
  }
  size_t total = 0;
  for (auto const& indices : submeshes) total += indices.size() * static_cast<size_t>(stripVertices);
  if (total > kMaxRebuiltVertices) {
    reason = "the rebuilt mesh would have " + std::to_string(total) + " vertices";
    return false;
  }
  out.source.reserve(total);
  out.corner.reserve(total);
  for (auto const& indices : submeshes) {
    std::vector<uint32_t> rebuilt;
    rebuilt.reserve(indices.size() * static_cast<size_t>(stripVertices - 2) * 3);
    for (uint32_t point : indices) {
      if (point >= vertexCount) {
        reason = "an index (" + std::to_string(point) + ") is past the mesh's " + std::to_string(vertexCount) +
                 " vertices";
        return false;
      }
      uint32_t const base = static_cast<uint32_t>(out.source.size());
      for (int j = 0; j < stripVertices; j++) {
        out.source.push_back(point);
        out.corner.push_back(static_cast<float>(j));
      }
      // A strip's triangle i is (i, i+1, i+2), with every other one turned
      // round so they all face the same way.
      for (int i = 0; i + 2 < stripVertices; i++) {
        uint32_t const a = base + static_cast<uint32_t>(i);
        if (i % 2 == 0) {
          rebuilt.insert(rebuilt.end(), {a, a + 1, a + 2});
        } else {
          rebuilt.insert(rebuilt.end(), {a + 1, a, a + 2});
        }
      }
    }
    out.submeshes.push_back(std::move(rebuilt));
  }
  return true;
}

}  // namespace Vivify::GeometryEmulation
