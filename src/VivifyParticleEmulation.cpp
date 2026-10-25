// Geometry-shader emulation for mesh particles (0.14.25-naomi.8).
//
// A particle system draws its particles from vertex data Unity builds on the
// CPU every frame, with a handful of streams and none of the corner channels
// the emulated programs read (VivifyGeometryEmulation.hpp), so naomi.6 and 7
// gave such systems a stand-in: 743 Aether's "Triangle Explosion" clouds (200
// Cube.001 particles) and shards (Cone) came out light cyan instead of PC's
// procedural rainbow flying apart. Those systems emit one burst and never move
// a particle again (start speed 0, no velocity, force, noise, rotation, size or
// colour over lifetime), so once the burst is out and holds still, the
// particles are baked once into one mesh in the system's own space -- the
// space Unity draws a local-simulation system's particles in on PC -- with the
// corner channels filled in, drawn with the original emulated material, and
// the particle renderer is switched off. A system whose particles change
// afterwards gets its particles back. Setting: "Emulate Geometry Effects On
// Particles" (on); off, they keep the stand-in.
#include "VivifyRuntimeInternal.hpp"
#include "main.hpp"

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/Mesh.hpp"
#include "UnityEngine/MeshFilter.hpp"
#include "UnityEngine/MeshRenderer.hpp"
#include "UnityEngine/MeshTopology.hpp"
#include "UnityEngine/ParticleSystem.hpp"
#include "UnityEngine/ParticleSystemRenderMode.hpp"
#include "UnityEngine/ParticleSystemRenderer.hpp"
#include "UnityEngine/ParticleSystemScalingMode.hpp"
#include "UnityEngine/ParticleSystemSimulationSpace.hpp"
#include "UnityEngine/Time.hpp"
#include "UnityEngine/Rendering/IndexFormat.hpp"
#include "UnityEngine/Rendering/ShadowCastingMode.hpp"
#include "UnityEngine/Rendering/VertexAttribute.hpp"
#include "UnityEngine/Shader.hpp"
#include "UnityEngine/Transform.hpp"

#include <algorithm>
#include <cmath>

namespace Vivify {

namespace {

constexpr size_t kMaxBakedVertices = 400000;

UnityEngine::Rendering::VertexAttribute TexCoordAttribute(int channel) {
  return UnityEngine::Rendering::VertexAttribute(UnityEngine::Rendering::VertexAttribute::TexCoord0.value__ + channel);
}

// Unity's Euler order: Z, then X, then Y.
struct Rotation {
  float m[3][3];
  explicit Rotation(UnityEngine::Vector3 radians) {
    float const cx = std::cos(radians.x), sx = std::sin(radians.x);
    float const cy = std::cos(radians.y), sy = std::sin(radians.y);
    float const cz = std::cos(radians.z), sz = std::sin(radians.z);
    // Ry * Rx * Rz
    m[0][0] = cy * cz + sy * sx * sz;
    m[0][1] = -cy * sz + sy * sx * cz;
    m[0][2] = sy * cx;
    m[1][0] = cx * sz;
    m[1][1] = cx * cz;
    m[1][2] = -sx;
    m[2][0] = -sy * cz + cy * sx * sz;
    m[2][1] = sy * sz + cy * sx * cz;
    m[2][2] = cy * cx;
  }
  UnityEngine::Vector3 Apply(UnityEngine::Vector3 v) const {
    return UnityEngine::Vector3(m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z);
  }
};

// ParticleSystem.GetParticles(Particle[] particles, int size, int offset),
// called through Unity's internal call with the array itself. The generated
// binding declares the array parameter by reference (its [Out] attribute) and
// passes the address of the caller's ArrayW, while the managed method hands
// its argument straight to Unity, which fills the array it is given: naomi.8
// had Unity write every particle over Runtime::Update's stack, and Aether
// aborted on its first frame (__stack_chk_fail). Null when the call is
// missing; the system then keeps its stand-in.
using GetParticlesCall = int32_t (*)(UnityEngine::ParticleSystem* system, void* particles, int32_t size,
                                     int32_t offset);
GetParticlesCall ResolveGetParticles() {
  static auto* const call =
      reinterpret_cast<GetParticlesCall>(ResolveEngineIcall("UnityEngine.ParticleSystem::GetParticles"));
  return call;
}

// What says the particles held still: their count and a sum over all of them.
double ParticleSignature(ArrayW<UnityEngine::ParticleSystem_Particle> const& particles, int count) {
  double sum = count;
  for (int i = 0; i < count; i++) {
    auto const& p = particles[i];
    sum += p.m_Position.x * 1.3 + p.m_Position.y * 1.7 + p.m_Position.z * 1.9 + p.m_Rotation.x + p.m_Rotation.y +
           p.m_Rotation.z + p.m_StartSize.x;
  }
  return sum;
}

}  // namespace

void Runtime::FindParticleBakes(UnityEngine::GameObject* instance, std::string const& prefabId) {
  if (!GetEmulateParticleGeometry() || _standInOriginals.empty() || !IsAlive(instance)) return;
  for (auto* renderer : instance->GetComponentsInChildren<UnityEngine::ParticleSystemRenderer*>(true)) {
    if (!IsAlive(renderer)) continue;
    UnityEngine::Material* material = renderer->get_sharedMaterial().unsafePtr();
    if (!IsAlive(material)) continue;
    auto original = _standInOriginals.find(material->GetInstanceID());
    if (original == _standInOriginals.end() || !original->second) continue;
    UnityEngine::Shader* shader = original->second->get_shader().unsafePtr();
    if (!IsAlive(shader)) continue;
    auto plan = _emulationList.plans.find(ToStdString(shader->get_name()));
    if (plan == _emulationList.plans.end() || plan->second.mode != GeometryEmulation::Mode::Triangle) continue;
    auto* system = renderer->GetComponent<UnityEngine::ParticleSystem*>();
    if (!IsAlive(system)) continue;
    // naomi.10: these were skipped without a word, which left 743 Aether's
    // 'Ground Fog' unexplained in the gray ledger.
    if (auto const mode = renderer->get_renderMode(); mode != UnityEngine::ParticleSystemRenderMode::Mesh) {
      PaperLogger.info("Vivify particle emulation: '{}' keeps the stand-in: its particles are drawn as billboards "
                       "(render mode {}), and only mesh particles can be baked",
                       ToStdString(system->get_gameObject()->get_name()), static_cast<int>(mode.value__));
      continue;
    }
    auto bake = std::make_unique<ParticleBake>();
    bake->system = system;
    bake->renderer = renderer;
    bake->original = original->second.ptr();
    bake->prefabId = prefabId;
    bake->name = ToStdString(system->get_gameObject()->get_name());
    _particleBakes.push_back(std::move(bake));
  }
}

void Runtime::UpdateParticleBakes() {
  if (_particleBakes.empty()) return;
  double const now = UnityEngine::Time::get_realtimeSinceStartup();
  bool bakedThisFrame = false;
  for (auto& owned : _particleBakes) {
    ParticleBake& bake = *owned;
    if (bake.state == ParticleBake::kGaveUp || now - bake.lastCheck < 0.25) continue;
    bake.lastCheck = now;
    if (!bake.system || !bake.renderer) {
      bake.state = ParticleBake::kGaveUp;
      continue;
    }
    UnityEngine::ParticleSystem* system = bake.system.ptr();
    // Only systems that are drawing: the prefab may be inactive.
    if (!system->get_gameObject()->get_activeInHierarchy()) continue;
    int const count = system->get_particleCount();
    if (bake.state == ParticleBake::kBaked) {
      // Particles that change after the bake get their own renderer back.
      if (count != bake.count) {
        PaperLogger.info("Vivify particle emulation: '{}' changed from {} to {} particles; drawn by its particle "
                         "system again (stand-in)", bake.name, bake.count, count);
        ReleaseParticleBake(bake);
        bake.state = ParticleBake::kGaveUp;
      }
      continue;
    }
    if (count <= 0 || bakedThisFrame) continue;
    auto const getParticles = ResolveGetParticles();
    if (getParticles == nullptr) {
      bake.state = ParticleBake::kGaveUp;
      PaperLogger.warn("Vivify particle emulation: '{}' keeps the stand-in: ParticleSystem.GetParticles is missing "
                       "in this game build", bake.name);
      continue;
    }
    ArrayW<UnityEngine::ParticleSystem_Particle> particles(static_cast<il2cpp_array_size_t>(count));
    // Never more than the array holds.
    int const got = std::clamp(getParticles(system, particles.convert(), count, 0), 0, count);
    double const signature = ParticleSignature(particles, got);
    if (got != bake.count || signature != bake.signature) {
      // Still being emitted, or moving: look again in a moment.
      bake.count = got;
      bake.signature = signature;
      bake.stableChecks = 0;
      // Ten seconds of drawing without holding still: 743 Aether's 'Extending
      // Arrows' move for as long as they live. Each look allocates an array
      // of every particle, so stop looking.
      if (++bake.movingChecks >= 40) {
        bake.state = ParticleBake::kGaveUp;
        PaperLogger.info("Vivify particle emulation: '{}' keeps the stand-in: its particles were still changing "
                         "after 10 s, and only particles that hold still can be baked",
                         bake.name);
      }
      continue;
    }
    if (++bake.stableChecks < 2) continue;  // held still for half a second
    std::string reason;
    try {
      if (!BakeParticles(bake, particles, got, reason)) {
        bake.state = ParticleBake::kGaveUp;
        PaperLogger.warn("Vivify particle emulation: '{}' keeps the stand-in: {}", bake.name, reason);
      }
    } catch (std::exception const& ex) {
      bake.state = ParticleBake::kGaveUp;
      ReleaseParticleBake(bake);
      PaperLogger.warn("Vivify particle emulation: baking '{}' threw: {}", bake.name, ex.what());
    }
    bakedThisFrame = true;  // one a frame: each is a few megabytes of vertices
  }
}

bool Runtime::BakeParticles(ParticleBake& bake, ArrayW<UnityEngine::ParticleSystem_Particle> const& particles,
                            int count, std::string& reason) {
  auto const started = std::chrono::steady_clock::now();
  UnityEngine::ParticleSystem* system = bake.system.ptr();
  UnityEngine::ParticleSystemRenderer* renderer = bake.renderer.ptr();
  auto main = system->get_main();
  auto const space = main.get_simulationSpace();
  if (space != UnityEngine::ParticleSystemSimulationSpace::Local) {
    reason = "it simulates in world or custom space";
    return false;
  }
  UnityEngine::Mesh* source = renderer->get_mesh().unsafePtr();
  if (!IsAlive(source) || !source->get_isReadable()) {
    reason = "its particle mesh is missing or not readable";
    return false;
  }
  for (int channel : {GeometryEmulation::kOtherAChannel, GeometryEmulation::kOtherBChannel,
                      GeometryEmulation::kCornerChannel}) {
    if (source->HasVertexAttribute(TexCoordAttribute(channel))) {
      reason = "its particle mesh already uses texture coordinate channel " + std::to_string(channel);
      return false;
    }
  }
  std::vector<std::vector<uint32_t>> submeshes;
  for (int s = 0; s < source->get_subMeshCount(); s++) {
    if (source->GetTopology(s) != UnityEngine::MeshTopology::Triangles) {
      reason = "its particle mesh is not drawn as triangles";
      return false;
    }
    auto indices = source->GetIndices(s);
    std::vector<uint32_t> list;
    list.reserve(indices.size());
    for (int32_t index : indices) list.push_back(static_cast<uint32_t>(index));
    submeshes.push_back(std::move(list));
  }
  uint32_t const vertexCount = static_cast<uint32_t>(source->get_vertexCount());
  GeometryEmulation::Expansion expansion;
  if (!GeometryEmulation::ExpandTriangles(submeshes, vertexCount, expansion, reason)) return false;
  auto vertices = source->get_vertices();
  if (vertices.size() != vertexCount) {
    reason = "its particle mesh returned no vertex positions";
    return false;
  }
  size_t const per = expansion.source.size();
  size_t const total = per * static_cast<size_t>(count);
  if (total > kMaxBakedVertices) {
    reason = std::to_string(count) + " particles of " + std::to_string(per) + " vertices are more than " +
             std::to_string(kMaxBakedVertices);
    return false;
  }

  VivifyBreadcrumb(fmt::format("particle emulation: baking '{}' ({} particles, {} vertices)", bake.name, count, total));
  ArrayW<UnityEngine::Vector3> positions(static_cast<il2cpp_array_size_t>(total));
  ArrayW<UnityEngine::Vector4> otherA(static_cast<il2cpp_array_size_t>(total));
  ArrayW<UnityEngine::Vector4> otherB(static_cast<il2cpp_array_size_t>(total));
  ArrayW<UnityEngine::Vector4> corner(static_cast<il2cpp_array_size_t>(total));
  ArrayW<int32_t> indices(static_cast<il2cpp_array_size_t>(total));
  std::vector<UnityEngine::Vector3> placed(vertexCount);
  UnityEngine::Vector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
  for (int p = 0; p < count; p++) {
    auto const& particle = particles[p];
    Rotation const rotation(particle.m_Rotation);
    float const size = particle.m_StartSize.x;
    for (uint32_t v = 0; v < vertexCount; v++) {
      auto const local = rotation.Apply(UnityEngine::Vector3(vertices[v].x * size, vertices[v].y * size,
                                                             vertices[v].z * size));
      placed[v] = UnityEngine::Vector3(particle.m_Position.x + local.x, particle.m_Position.y + local.y,
                                       particle.m_Position.z + local.z);
      lo = UnityEngine::Vector3(std::min(lo.x, placed[v].x), std::min(lo.y, placed[v].y), std::min(lo.z, placed[v].z));
      hi = UnityEngine::Vector3(std::max(hi.x, placed[v].x), std::max(hi.y, placed[v].y), std::max(hi.z, placed[v].z));
    }
    size_t const base = per * static_cast<size_t>(p);
    for (size_t i = 0; i < per; i++) {
      auto const& at = placed[expansion.source[i]];
      auto const& a = placed[expansion.otherA[i]];
      auto const& b = placed[expansion.otherB[i]];
      positions[base + i] = at;
      otherA[base + i] = UnityEngine::Vector4(a.x, a.y, a.z, 1.0f);
      otherB[base + i] = UnityEngine::Vector4(b.x, b.y, b.z, 1.0f);
      corner[base + i] = UnityEngine::Vector4(expansion.corner[i], 0.0f, 0.0f, GeometryEmulation::kMarker);
      indices[base + i] = static_cast<int32_t>(base + i);  // every corner is its own vertex
    }
  }

  auto* mesh = UnityEngine::Mesh::New_ctor();
  mesh->set_name(StringW(bake.name + " (Vivify particle emulation)"));
  if (total > 65535) mesh->set_indexFormat(UnityEngine::Rendering::IndexFormat::UInt32);
  mesh->set_vertices(positions);
  mesh->SetUVs(GeometryEmulation::kOtherAChannel, otherA);
  mesh->SetUVs(GeometryEmulation::kOtherBChannel, otherB);
  mesh->SetUVs(GeometryEmulation::kCornerChannel, corner);
  mesh->SetIndices(indices, UnityEngine::MeshTopology::Triangles, 0);
  // Room for the triangles to fly apart: the geometry stage's output is not
  // bounded on PC either.
  UnityEngine::Vector3 const center((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f);
  UnityEngine::Vector3 const extent((hi.x - lo.x) * 1.5f + 1.0f, (hi.y - lo.y) * 1.5f + 1.0f, (hi.z - lo.z) * 1.5f + 1.0f);
  mesh->set_bounds(UnityEngine::Bounds(center, extent));
  mesh->UploadMeshData(true);

  auto* object = UnityEngine::GameObject::New_ctor(StringW("Vivify particle emulation"));
  UnityEngine::Transform* transform = object->get_transform().unsafePtr();
  UnityEngine::Transform* systemTransform = system->get_transform().unsafePtr();
  transform->SetParent(systemTransform, false);
  // Local scaling ignores the parents' scale; the hierarchy's is in the parent
  // already.
  if (main.get_scalingMode() == UnityEngine::ParticleSystemScalingMode::Local) {
    auto const own = systemTransform->get_localScale();
    auto const lossy = systemTransform->get_lossyScale();
    auto ratio = [](float a, float b) { return std::abs(b) > 1e-6f ? a / b : 1.0f; };
    transform->set_localScale(UnityEngine::Vector3(ratio(own.x, lossy.x), ratio(own.y, lossy.y), ratio(own.z, lossy.z)));
  }
  object->set_layer(system->get_gameObject()->get_layer());
  auto* filter = object->AddComponent<UnityEngine::MeshFilter*>();
  filter->set_sharedMesh(mesh);
  auto* meshRenderer = object->AddComponent<UnityEngine::MeshRenderer*>();
  meshRenderer->set_sharedMaterial(bake.original.ptr());
  meshRenderer->set_shadowCastingMode(UnityEngine::Rendering::ShadowCastingMode::Off);
  meshRenderer->set_receiveShadows(false);
  meshRenderer->set_sortingLayerID(renderer->get_sortingLayerID());
  meshRenderer->set_sortingOrder(renderer->get_sortingOrder());

  static auto* const setForceOff = reinterpret_cast<void (*)(UnityEngine::Renderer*, bool)>(
      ResolveEngineIcall("UnityEngine.Renderer::set_forceRenderingOff"));
  if (setForceOff != nullptr) {
    setForceOff(renderer, true);
  } else {
    renderer->set_enabled(false);
  }
  // A cost test that switches emulated objects (or transparent effects) off
  // switches this off too.
  int const costTest = GetDebugCostTest();
  if (costTest != kCostTestNone && CostTestMatches(costTest, meshRenderer)) {
    if (setForceOff != nullptr) {
      setForceOff(meshRenderer, true);
    } else {
      meshRenderer->set_enabled(false);
    }
  }
  bake.baked = object;
  bake.mesh = mesh;
  bake.state = ParticleBake::kBaked;
  bake.count = count;
  PaperLogger.info("Vivify particle emulation: '{}' baked, {} particles into {} vertices with its emulated material "
                   "'{}', in {:.1f} ms",
                   bake.name, count, total, ToStdString(bake.original->get_name()),
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
  return true;
}

void Runtime::ReleaseParticleBake(ParticleBake& bake) {
  if (bake.baked) UnityEngine::Object::Destroy(bake.baked.ptr());
  if (bake.mesh) UnityEngine::Object::Destroy(bake.mesh.ptr());
  bake.baked = nullptr;
  bake.mesh = nullptr;
  if (bake.renderer) {
    static auto* const setForceOff = reinterpret_cast<void (*)(UnityEngine::Renderer*, bool)>(
        ResolveEngineIcall("UnityEngine.Renderer::set_forceRenderingOff"));
    if (setForceOff != nullptr) {
      setForceOff(bake.renderer.ptr(), false);
    } else {
      bake.renderer->set_enabled(true);
    }
  }
}

void Runtime::ReleaseParticleBakes(std::string_view prefabId) {
  for (auto it = _particleBakes.begin(); it != _particleBakes.end();) {
    if (!prefabId.empty() && (*it)->prefabId != prefabId) {
      ++it;
      continue;
    }
    ReleaseParticleBake(**it);
    it = _particleBakes.erase(it);
  }
}

}  // namespace Vivify
