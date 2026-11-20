// Host tests for include/VivifyParticleChunks.hpp, the chunk planning of the
// Debug experiment "Bake Large Particle Effects In Chunks".
#include "VivifyParticleChunks.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace C = Vivify::ParticleChunks;

namespace {

int failures = 0;
int passes = 0;

void Check(bool ok, std::string const& what, std::string const& detail = "") {
  if (ok) {
    passes++;
    return;
  }
  failures++;
  std::printf("FAIL: %s %s\n", what.c_str(), detail.c_str());
}

// Particles spread around a centre, like Aether's rock particles around the
// player: a ring with some height, in a fixed pseudo-random order.
std::vector<C::Point> Ring(int count, float radius) {
  std::vector<C::Point> points;
  unsigned state = 12345;
  for (int i = 0; i < count; i++) {
    state = state * 1103515245u + 12345u;
    float const a = static_cast<float>((state >> 8) % 36000) / 36000.0f * 6.2831853f;
    state = state * 1103515245u + 12345u;
    float const h = static_cast<float>((state >> 8) % 1000) / 100.0f;
    points.push_back({3.0f + radius * std::cos(a), h, -2.0f + radius * std::sin(a)});
  }
  return points;
}

double Angle(C::Point const& p, double cx, double cz) {
  return std::atan2(p.z - cz, p.x - cx);
}

}  // namespace

int main() {
  // Aether's wireframe rock particles: 1000 of 876 vertices.
  {
    auto const points = Ring(1000, 6.0f);
    C::Plan plan;
    bool const ok = C::PlanChunks(points, 876, C::kMaxChunkVertices, C::kMaxChunkedVertices, plan);
    Check(ok, "Aether's rock particles get a plan", plan.reason);
    Check(plan.particlesPerChunk == 74, "74 particles a chunk", std::to_string(plan.particlesPerChunk));
    Check(plan.chunks.size() == 14, "14 chunks", std::to_string(plan.chunks.size()));
    Check(plan.totalVertices == 876000, "876,000 vertices in all", std::to_string(plan.totalVertices));
    std::set<int> seen;
    size_t largest = 0;
    for (auto const& chunk : plan.chunks) {
      largest = std::max(largest, chunk.size() * 876);
      for (int index : chunk) seen.insert(index);
    }
    size_t members = 0;
    for (auto const& chunk : plan.chunks) members += chunk.size();
    Check(members == 1000 && seen.size() == 1000 && *seen.begin() == 0 && *seen.rbegin() == 999,
          "every particle is in exactly one chunk");
    Check(largest <= C::kMaxChunkVertices, "every chunk fits 16-bit indices", std::to_string(largest));
    Check(plan.chunks.back().size() == 1000 - 13 * 74, "the last chunk holds the rest",
          std::to_string(plan.chunks.back().size()));
    // Each chunk is a wedge: its directions cover about 1/14 of the circle.
    double cx = 0, cz = 0;
    for (auto const& p : points) {
      cx += p.x;
      cz += p.z;
    }
    cx /= points.size();
    cz /= points.size();
    double previousEnd = -10.0;
    bool ordered = true, narrow = true;
    for (auto const& chunk : plan.chunks) {
      double lo = 10.0, hi = -10.0;
      for (int index : chunk) {
        lo = std::min(lo, Angle(points[index], cx, cz));
        hi = std::max(hi, Angle(points[index], cx, cz));
      }
      if (lo < previousEnd) ordered = false;
      previousEnd = hi;
      if (hi - lo > 2.0 * 3.14159265 / 14.0 * 1.6) narrow = false;
    }
    Check(ordered, "chunks follow each other around the circle");
    Check(narrow, "each chunk is one narrow wedge of directions, so the camera can cull it");
    C::Plan again;
    C::PlanChunks(points, 876, C::kMaxChunkVertices, C::kMaxChunkedVertices, again);
    Check(again.chunks == plan.chunks, "the same particles give the same plan");
    Check(C::BytesFor(plan.totalVertices) == 876000u * 62u, "876,000 vertices hold about 54 MB",
          std::to_string(C::BytesFor(plan.totalVertices)));
  }
  // Refusals.
  {
    C::Plan plan;
    Check(!C::PlanChunks({}, 876, C::kMaxChunkVertices, C::kMaxChunkedVertices, plan) && !plan.reason.empty(),
          "no particles: no plan");
    Check(!C::PlanChunks(Ring(10, 1.0f), 0, C::kMaxChunkVertices, C::kMaxChunkedVertices, plan),
          "a mesh with no vertices: no plan");
    Check(!C::PlanChunks(Ring(10, 1.0f), 70000, C::kMaxChunkVertices, C::kMaxChunkedVertices, plan) &&
              plan.reason.find("more than a chunk") != std::string::npos,
          "one particle over a chunk: no plan", plan.reason);
    Check(!C::PlanChunks(Ring(1200, 5.0f), 876, C::kMaxChunkVertices, C::kMaxChunkedVertices, plan) &&
              plan.reason == "1200 particles of 876 vertices (1051200) are more than the chunked bake's 1000000",
          "over the whole-system cap: no plan", plan.reason);
    Check(plan.chunks.empty(), "a refusal leaves no chunks");
  }
  // Exactly at the edges.
  {
    C::Plan plan;
    Check(C::PlanChunks(Ring(1, 1.0f), C::kMaxChunkVertices, C::kMaxChunkVertices, C::kMaxChunkedVertices, plan) &&
              plan.chunks.size() == 1 && plan.particlesPerChunk == 1,
          "one particle of exactly a chunk's vertices");
    Check(C::PlanChunks(Ring(1000, 2.0f), 1000, C::kMaxChunkVertices, C::kMaxChunkedVertices, plan) &&
              plan.totalVertices == C::kMaxChunkedVertices,
          "exactly the whole-system cap");
    std::vector<C::Point> same(200, C::Point{1.0f, 2.0f, 3.0f});
    Check(C::PlanChunks(same, 876, C::kMaxChunkVertices, C::kMaxChunkedVertices, plan) && plan.chunks.size() == 3 &&
              plan.chunks[0][0] == 0 && plan.chunks[2].back() == 199,
          "particles all in one place keep their own order");
  }
  // The experiment only takes over above the one-mesh limit and below its cap.
  Check(C::kMaxChunkedVertices > 400000, "the chunked cap is above the one-mesh limit");
  std::printf("%d passed, %d failed\n", passes, failures);
  return failures == 0 ? 0 : 1;
}
