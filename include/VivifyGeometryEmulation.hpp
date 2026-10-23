#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "VivifyDxbc.hpp"

// Geometry-shader emulation (0.14.25-naomi.5).
//
// The Quest renders both eyes in one multiview pass, and its driver has no
// extension that allows a geometry stage there, so a PC shader built on one
// (wireframes, exploding triangles, generated grass) cannot run as authored.
// For a few known shaders whose geometry program has a simple shape, the
// geometry program is run inside the vertex stage instead:
//
// - Triangle mode: one triangle in, the same triangle out (three emits, no
//   flow control). The mesh is rebuilt at load so every triangle has its own
//   three vertices, each carrying its corner index and the other two corners'
//   positions. The vertex stage runs the PC vertex program for all three
//   corners, then the geometry program, and keeps the output of the emit that
//   belongs to its own corner.
// - Point mode: one point in, a fixed-length triangle strip out (a generated
//   grass blade). Each point becomes as many vertices as the strip has, each
//   carrying its position in the strip, with indices that form the strip's
//   triangles; the vertex stage keeps the matching emit.
//
// Everything here is Unity-free so the host tests can drive it; the converter
// composes the programs and the runtime rebuilds the meshes.
namespace Vivify::GeometryEmulation {

// Mesh channels the rebuilt mesh uses, as TEXCOORD indices (Unity uv5, uv6,
// uv7). kCornerChannel's .x is the corner (or the position in the strip) and
// its .w is kMarker, so a mesh that was not rebuilt draws nothing rather than
// garbage.
inline constexpr int kOtherAChannel = 5;
inline constexpr int kOtherBChannel = 6;
inline constexpr int kCornerChannel = 7;
inline constexpr float kMarker = 7.0f;
// A strip longer than this is not emulated (each point would become this many
// vertices).
inline constexpr int kMaxStripVertices = 32;

// The shaders this is done for. Anything else keeps its current treatment.
bool IsKnownShader(std::string_view shaderName);

enum class Mode { Triangle, Point };
struct Plan {
  Mode mode = Mode::Triangle;
  int outputVertices = 3;
};
char const* ModeName(Mode mode);

// Whether a parsed geometry program has a shape this can emulate, and how.
// On refusal, `reason` says why in a form fit for the log.
std::optional<Plan> PlanFor(Dxbc::Program const& geometry, std::string& reason);

// Composes one vertex program from a vertex program and a geometry program
// translated in their function modes (Dxbc::GlslOptions::functionMode).
struct Composed {
  bool ok = false;
  std::string source;  // a complete vertex stage, "#version ..." first
  int version = 300;
  std::string error;
};
Composed Compose(Dxbc::GlslResult const& vertexFunction, Dxbc::GlslResult const& geometryFunction,
                 Plan const& plan, bool multiview);

// The rebuilt mesh, as a mapping from new vertices to old ones; the runtime
// copies the attributes. For triangle mode, otherA/otherB are the other two
// corners of the vertex's triangle, in order (corner + 1, corner + 2).
struct Expansion {
  std::vector<uint32_t> source;   // per new vertex: the vertex it copies
  std::vector<float> corner;      // per new vertex: corner, or position in the strip
  std::vector<uint32_t> otherA;   // triangle mode only
  std::vector<uint32_t> otherB;   // triangle mode only
  std::vector<std::vector<uint32_t>> submeshes;  // triangle lists, one per submesh
};
// `submeshes` holds each submesh's triangle-list indices (triangle mode) or
// point indices (point mode). False, with `reason`, when an index is out of
// range, a triangle list is not a multiple of three, or the result would be
// too large.
bool ExpandTriangles(std::vector<std::vector<uint32_t>> const& submeshes, uint32_t vertexCount, Expansion& out,
                     std::string& reason);
bool ExpandPoints(std::vector<std::vector<uint32_t>> const& submeshes, uint32_t vertexCount, int stripVertices,
                  Expansion& out, std::string& reason);

// The list of emulated shaders the converter writes beside a bundle
// ("<bundle>.geometry"): a format line, then one "<shader name> <mode>
// <vertices>" line per shader. The format line marks a conversion whose
// readable flags can be trusted (naomi.6: 19's flagged streamed meshes, and
// rebuilding one crashed); a list without it belongs to an older conversion,
// whose objects all keep the stand-in.
inline constexpr char const* kEmulationListHeader = "# vivify-geometry 2";
std::string FormatEmulationList(std::vector<std::string> const& shaderLines);
struct EmulationList {
  bool current = false;  // has this build's format line
  std::map<std::string, Plan> plans;
};
EmulationList ParseEmulationList(std::string_view text);

}  // namespace Vivify::GeometryEmulation
