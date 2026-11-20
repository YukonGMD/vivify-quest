// Chunk planning for the Debug experiment "Bake Large Particle Effects In
// Chunks" (naomi.10, off by default).
//
// A particle system whose particles hold still is baked into one mesh so a
// geometry-shader effect can draw on it (VivifyParticleEmulation.cpp). One
// mesh is capped at 400,000 vertices, and 743 Aether's wireframe 'rock
// particles' (1000 particles of a 292-triangle cube, 876 vertices each once
// every corner is its own vertex) are over it. The experiment builds such a
// system as several meshes instead: each small enough for 16-bit indices,
// built one a frame so no frame pays for all of them, and each holding the
// particles in one wedge of directions around the system's centre, so a
// wedge behind the camera is culled instead of drawn.
//
// Plain C++ with no Unity types, so the host tests (tools/particlechunks)
// check the plan.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <string>
#include <vector>

namespace Vivify::ParticleChunks {

// One chunk's vertices: 16-bit indices reach 65,535.
inline constexpr size_t kMaxChunkVertices = 65535;
// A whole system: about 62 MB of vertex and index data at this size
// (BytesFor). Aether's rock particles are 876,000.
inline constexpr size_t kMaxChunkedVertices = 1000000;
// Each baked vertex: a position and three float4 channels (the triangle's
// other two corners and the corner marker), as the one-mesh bake writes them.
inline constexpr size_t kVertexBytes = 12 + 3 * 16;
inline constexpr size_t kIndexBytes = 2;

struct Point {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Plan {
  std::vector<std::vector<int>> chunks;  // particle indices, each chunk's in order
  size_t particlesPerChunk = 0;
  size_t totalVertices = 0;
  std::string reason;  // why there is no plan
};

// The memory a chunked bake of this many vertices holds once built.
inline size_t BytesFor(size_t vertices) {
  return vertices * (kVertexBytes + kIndexBytes);
}

// Which particles go into which chunk. Particles are ordered by their
// direction around the centre of all of them (the angle in the horizontal
// plane; then height, then index, so the order is the same every time), and
// consecutive runs of that order become chunks of at most maxChunkVertices.
// False, with a reason, when there is nothing to bake, one particle is more
// than a chunk, or all of them are more than maxTotalVertices.
inline bool PlanChunks(std::vector<Point> const& positions, size_t verticesPerParticle, size_t maxChunkVertices,
                       size_t maxTotalVertices, Plan& plan) {
  plan = Plan{};
  if (positions.empty() || verticesPerParticle == 0) {
    plan.reason = "there are no particles to bake";
    return false;
  }
  if (verticesPerParticle > maxChunkVertices) {
    plan.reason = "one particle's " + std::to_string(verticesPerParticle) + " vertices are more than a chunk's " +
                  std::to_string(maxChunkVertices);
    return false;
  }
  size_t const total = verticesPerParticle * positions.size();
  if (total > maxTotalVertices) {
    plan.reason = std::to_string(positions.size()) + " particles of " + std::to_string(verticesPerParticle) +
                  " vertices (" + std::to_string(total) + ") are more than the chunked bake's " +
                  std::to_string(maxTotalVertices);
    return false;
  }
  double cx = 0.0, cz = 0.0;
  for (auto const& p : positions) {
    cx += p.x;
    cz += p.z;
  }
  cx /= static_cast<double>(positions.size());
  cz /= static_cast<double>(positions.size());
  std::vector<double> angle(positions.size());
  for (size_t i = 0; i < positions.size(); i++) {
    angle[i] = std::atan2(static_cast<double>(positions[i].z) - cz, static_cast<double>(positions[i].x) - cx);
  }
  std::vector<int> order(positions.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    if (angle[a] != angle[b]) return angle[a] < angle[b];
    if (positions[a].y != positions[b].y) return positions[a].y < positions[b].y;
    return a < b;
  });
  plan.particlesPerChunk = maxChunkVertices / verticesPerParticle;
  plan.totalVertices = total;
  for (size_t start = 0; start < order.size(); start += plan.particlesPerChunk) {
    size_t const end = std::min(order.size(), start + plan.particlesPerChunk);
    plan.chunks.emplace_back(order.begin() + static_cast<std::ptrdiff_t>(start),
                             order.begin() + static_cast<std::ptrdiff_t>(end));
  }
  return true;
}

}  // namespace Vivify::ParticleChunks
