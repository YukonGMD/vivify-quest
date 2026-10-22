// Host test driver for src/VivifyGeometryEmulation.cpp.
//
//   geoemu plan <geometry.dxbc>
//     prints "mode=<triangle|point> vertices=N", or "refused=<reason>"
//   geoemu fuse <vertex.dxbc> <geometry.dxbc> [mv]
//     translates both in their function modes and composes them; prints
//     "ok=1" and the source after "---", or "ok=0" and "error=..."
//   geoemu expand-triangles <vertexCount> <submesh> [<submesh>...]
//   geoemu expand-points <vertexCount> <stripVertices> <submesh> [<submesh>...]
//     a submesh is comma-separated indices; prints one line per new vertex
//     ("v source corner otherA otherB") and one per submesh ("s indices...")
#include "VivifyGeometryEmulation.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace Vivify;

namespace {

std::vector<uint8_t> ReadFile(char const* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<uint32_t> ParseList(std::string const& text) {
  std::vector<uint32_t> out;
  std::stringstream in(text);
  std::string item;
  while (std::getline(in, item, ',')) {
    if (!item.empty()) out.push_back(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 10)));
  }
  return out;
}

void PrintExpansion(GeometryEmulation::Expansion const& e) {
  for (size_t i = 0; i < e.source.size(); i++) {
    std::printf("v %u %g", e.source[i], e.corner[i]);
    if (!e.otherA.empty()) std::printf(" %u %u", e.otherA[i], e.otherB[i]);
    std::printf("\n");
  }
  for (auto const& submesh : e.submeshes) {
    std::printf("s");
    for (uint32_t index : submesh) std::printf(" %u", index);
    std::printf("\n");
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string const command = argc > 1 ? argv[1] : "";
  if (command == "plan" && argc >= 3) {
    auto const bytes = ReadFile(argv[2]);
    auto const program = Dxbc::ParseProgram(bytes.data(), bytes.size());
    std::string reason;
    auto const plan = GeometryEmulation::PlanFor(program, reason);
    if (!plan) {
      std::printf("refused=%s\n", reason.c_str());
      return 0;
    }
    std::printf("mode=%s vertices=%d\n", GeometryEmulation::ModeName(plan->mode), plan->outputVertices);
    return 0;
  }
  if (command == "fuse" && argc >= 4) {
    bool const multiview = argc >= 5 && std::string(argv[4]) == "mv";
    auto const vsBytes = ReadFile(argv[2]);
    auto const gsBytes = ReadFile(argv[3]);
    auto const vsProgram = Dxbc::ParseProgram(vsBytes.data(), vsBytes.size());
    auto const gsProgram = Dxbc::ParseProgram(gsBytes.data(), gsBytes.size());
    std::string reason;
    auto const plan = GeometryEmulation::PlanFor(gsProgram, reason);
    if (!plan) {
      std::printf("ok=0\nerror=plan: %s\n", reason.c_str());
      return 0;
    }
    Dxbc::GlslOptions options;
    options.multiview = multiview;
    options.functionMode = Dxbc::GlslOptions::FunctionMode::VertexFunction;
    auto const vs = Dxbc::TranslateToGlsl(vsProgram, options);
    options.functionMode = Dxbc::GlslOptions::FunctionMode::GeometryFunction;
    auto const gs = Dxbc::TranslateToGlsl(gsProgram, options);
    if (!vs.ok || !gs.ok) {
      std::printf("ok=0\nerror=translate: %s%s\n", vs.error.c_str(), gs.error.c_str());
      return 0;
    }
    auto const composed = GeometryEmulation::Compose(vs, gs, *plan, multiview);
    std::printf("ok=%d\n", composed.ok ? 1 : 0);
    if (!composed.ok) {
      std::printf("error=%s\n", composed.error.c_str());
      return 0;
    }
    std::printf("---\n%s", composed.source.c_str());
    return 0;
  }
  if ((command == "expand-triangles" && argc >= 4) || (command == "expand-points" && argc >= 5)) {
    bool const points = command == "expand-points";
    uint32_t const vertexCount = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
    int const strip = points ? std::atoi(argv[3]) : 0;
    std::vector<std::vector<uint32_t>> submeshes;
    for (int i = points ? 4 : 3; i < argc; i++) submeshes.push_back(ParseList(argv[i]));
    GeometryEmulation::Expansion expansion;
    std::string reason;
    bool const ok = points ? GeometryEmulation::ExpandPoints(submeshes, vertexCount, strip, expansion, reason)
                           : GeometryEmulation::ExpandTriangles(submeshes, vertexCount, expansion, reason);
    if (!ok) {
      std::printf("refused=%s\n", reason.c_str());
      return 0;
    }
    PrintExpansion(expansion);
    return 0;
  }
  std::fprintf(stderr, "usage: see the comment at the top of tools/geometryemulation/main.cpp\n");
  return 2;
}
