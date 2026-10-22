#include "VivifyRuntimeInternal.hpp"
#include "VivifyGeometryEmulation.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <tuple>

#include "System/Collections/Generic/List_1.hpp"
#include "UnityEngine/Bounds.hpp"
#include "UnityEngine/Color32.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/Mesh.hpp"
#include "UnityEngine/MeshFilter.hpp"
#include "UnityEngine/MeshRenderer.hpp"
#include "UnityEngine/MeshTopology.hpp"
#include "UnityEngine/Renderer.hpp"
#include "UnityEngine/Rendering/IndexFormat.hpp"
#include "UnityEngine/Rendering/VertexAttribute.hpp"
#include "UnityEngine/Shader.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/Vector4.hpp"

// Geometry-shader emulation at load (0.14.25-naomi.5).
//
// The converter ran the geometry program of a few known shaders inside the
// vertex stage (VivifyGeometryEmulation.hpp) and wrote which ones beside the
// bundle. Those programs need a rebuilt mesh: every triangle with its own
// vertices, carrying its corner and the other corners' positions (or, for a
// point shader, one vertex per strip position). This walks the bundle's
// prefabs, gives every object drawn with such a shader the rebuilt mesh, and
// gives anything it cannot rebuild a stand-in copy of the material, which is
// exactly what that object looked like before.

namespace Vivify {

namespace {

std::string ListPath(std::string const& bundlePath) {
  return bundlePath + ".geometry";
}

UnityEngine::Rendering::VertexAttribute TexCoord(int channel) {
  return UnityEngine::Rendering::VertexAttribute(static_cast<int32_t>(4 + channel));
}

}  // namespace

void WriteGeometryEmulationList(std::string const& bundlePath, std::vector<std::string> const& shaders) {
  std::error_code ec;
  if (shaders.empty()) {
    std::filesystem::remove(ListPath(bundlePath), ec);
    return;
  }
  std::ofstream out(ListPath(bundlePath), std::ios::out | std::ios::trunc);
  for (auto const& line : shaders) out << line << "\n";  // "<shader name> <mode> <vertices>"
}

std::map<std::string, GeometryEmulation::Plan> ReadGeometryEmulationList(std::string const& bundlePath) {
  std::map<std::string, GeometryEmulation::Plan> plans;
  std::ifstream in(ListPath(bundlePath));
  std::string line;
  while (std::getline(in, line)) {
    // The shader name may hold spaces; the mode and count are the last two words.
    size_t const countAt = line.rfind(' ');
    if (countAt == std::string::npos || countAt == 0) continue;
    size_t const modeAt = line.rfind(' ', countAt - 1);
    if (modeAt == std::string::npos) continue;
    std::string const mode = line.substr(modeAt + 1, countAt - modeAt - 1);
    GeometryEmulation::Plan plan;
    plan.mode = mode == "point" ? GeometryEmulation::Mode::Point : GeometryEmulation::Mode::Triangle;
    plan.outputVertices = std::atoi(line.c_str() + countAt + 1);
    if (mode != "point" && mode != "triangle") continue;
    if (!GeometryEmulation::IsKnownShader(line.substr(0, modeAt))) continue;
    plans[line.substr(0, modeAt)] = plan;
  }
  return plans;
}

UnityEngine::Mesh* Runtime::EmulationMeshFor(UnityEngine::Mesh* source, GeometryEmulation::Plan const& plan,
                                             std::string& reason) {
  auto const key = std::make_tuple(source, static_cast<int>(plan.mode), plan.outputVertices);
  if (auto it = _emulationMeshes.find(key); it != _emulationMeshes.end()) return it->second;
  // One that is already rebuilt (a second level start on the same bundle).
  if (_emulationMeshSet.contains(source)) return source;

  std::string const name = ToStdString(source->get_name());
  if (!source->get_isReadable()) {
    reason = "mesh '" + name + "' is not readable";
    return nullptr;
  }
  if (source->get_blendShapeCount() > 0 || source->get_bindposes().size() > 0) {
    reason = "mesh '" + name + "' is skinned or has blend shapes";
    return nullptr;
  }
  for (int channel : {GeometryEmulation::kOtherAChannel, GeometryEmulation::kOtherBChannel,
                      GeometryEmulation::kCornerChannel}) {
    if (source->HasVertexAttribute(TexCoord(channel))) {
      reason = "mesh '" + name + "' already uses texture coordinate channel " + std::to_string(channel);
      return nullptr;
    }
  }
  auto const wanted = plan.mode == GeometryEmulation::Mode::Triangle ? UnityEngine::MeshTopology::Triangles
                                                                     : UnityEngine::MeshTopology::Points;
  int const submeshCount = source->get_subMeshCount();
  std::vector<std::vector<uint32_t>> submeshes;
  for (int s = 0; s < submeshCount; s++) {
    if (source->GetTopology(s) != wanted) {
      reason = "mesh '" + name + "' submesh " + std::to_string(s) + " is not drawn as " +
               (plan.mode == GeometryEmulation::Mode::Triangle ? "triangles" : "points");
      return nullptr;
    }
    auto indices = source->GetIndices(s);
    std::vector<uint32_t> list;
    list.reserve(indices.size());
    for (int32_t index : indices) list.push_back(static_cast<uint32_t>(index));
    submeshes.push_back(std::move(list));
  }
  uint32_t const vertexCount = static_cast<uint32_t>(source->get_vertexCount());
  GeometryEmulation::Expansion expansion;
  bool const expanded =
      plan.mode == GeometryEmulation::Mode::Triangle
          ? GeometryEmulation::ExpandTriangles(submeshes, vertexCount, expansion, reason)
          : GeometryEmulation::ExpandPoints(submeshes, vertexCount, plan.outputVertices, expansion, reason);
  if (!expanded) {
    reason = "mesh '" + name + "': " + reason;
    return nullptr;
  }

  size_t const count = expansion.source.size();
  auto vertices = source->get_vertices();
  if (vertices.size() != vertexCount) {
    reason = "mesh '" + name + "' returned no vertex positions";
    return nullptr;
  }
  auto* mesh = UnityEngine::Mesh::New_ctor();
  mesh->set_name(StringW(name + " (Vivify geometry emulation)"));
  if (count > 65535) mesh->set_indexFormat(UnityEngine::Rendering::IndexFormat::UInt32);

  auto gather3 = [&](ArrayW<UnityEngine::Vector3> const& from) {
    ArrayW<UnityEngine::Vector3> to(static_cast<il2cpp_array_size_t>(count));
    for (size_t i = 0; i < count; i++) to[i] = from[expansion.source[i]];
    return to;
  };
  mesh->set_vertices(gather3(vertices));
  if (source->HasVertexAttribute(UnityEngine::Rendering::VertexAttribute::Normal)) {
    auto normals = source->get_normals();
    if (normals.size() == vertexCount) mesh->set_normals(gather3(normals));
  }
  if (source->HasVertexAttribute(UnityEngine::Rendering::VertexAttribute::Tangent)) {
    auto tangents = source->get_tangents();
    if (tangents.size() == vertexCount) {
      ArrayW<UnityEngine::Vector4> to(static_cast<il2cpp_array_size_t>(count));
      for (size_t i = 0; i < count; i++) to[i] = tangents[expansion.source[i]];
      mesh->set_tangents(to);
    }
  }
  if (source->HasVertexAttribute(UnityEngine::Rendering::VertexAttribute::Color)) {
    auto colors = source->get_colors();
    if (colors.size() == vertexCount) {
      ArrayW<UnityEngine::Color> to(static_cast<il2cpp_array_size_t>(count));
      for (size_t i = 0; i < count; i++) to[i] = colors[expansion.source[i]];
      mesh->set_colors(to);
    }
  }
  // The texture coordinates the original had (channels 0-4; 5-7 were checked
  // free above), at their own width.
  for (int channel = 0; channel < GeometryEmulation::kOtherAChannel; channel++) {
    if (!source->HasVertexAttribute(TexCoord(channel))) continue;
    int const dimension = source->GetVertexAttributeDimension(TexCoord(channel));
    if (dimension <= 2) {
      ArrayW<UnityEngine::Vector2> from;
      switch (channel) {
        case 0: from = source->get_uv(); break;
        case 1: from = source->get_uv2(); break;
        case 2: from = source->get_uv3(); break;
        case 3: from = source->get_uv4(); break;
        default: from = source->get_uv5(); break;
      }
      if (from.size() != vertexCount) continue;
      ArrayW<UnityEngine::Vector2> to(static_cast<il2cpp_array_size_t>(count));
      for (size_t i = 0; i < count; i++) to[i] = from[expansion.source[i]];
      mesh->SetUVs(channel, to);
    } else {
      auto* list = System::Collections::Generic::List_1<UnityEngine::Vector4>::New_ctor();
      source->GetUVs(channel, list);
      if (list->get_Count() != static_cast<int>(vertexCount)) continue;
      ArrayW<UnityEngine::Vector4> to(static_cast<il2cpp_array_size_t>(count));
      for (size_t i = 0; i < count; i++) to[i] = list->get_Item(static_cast<int>(expansion.source[i]));
      mesh->SetUVs(channel, to);
    }
  }
  // The emulation's own channels.
  ArrayW<UnityEngine::Vector4> corner(static_cast<il2cpp_array_size_t>(count));
  for (size_t i = 0; i < count; i++) {
    corner[i] = UnityEngine::Vector4(expansion.corner[i], 0.0f, 0.0f, GeometryEmulation::kMarker);
  }
  mesh->SetUVs(GeometryEmulation::kCornerChannel, corner);
  if (plan.mode == GeometryEmulation::Mode::Triangle) {
    ArrayW<UnityEngine::Vector4> otherA(static_cast<il2cpp_array_size_t>(count));
    ArrayW<UnityEngine::Vector4> otherB(static_cast<il2cpp_array_size_t>(count));
    for (size_t i = 0; i < count; i++) {
      auto const a = vertices[expansion.otherA[i]];
      auto const b = vertices[expansion.otherB[i]];
      otherA[i] = UnityEngine::Vector4(a.x, a.y, a.z, 1.0f);
      otherB[i] = UnityEngine::Vector4(b.x, b.y, b.z, 1.0f);
    }
    mesh->SetUVs(GeometryEmulation::kOtherAChannel, otherA);
    mesh->SetUVs(GeometryEmulation::kOtherBChannel, otherB);
  }
  mesh->set_subMeshCount(static_cast<int>(expansion.submeshes.size()));
  for (size_t s = 0; s < expansion.submeshes.size(); s++) {
    ArrayW<int32_t> indices(static_cast<il2cpp_array_size_t>(expansion.submeshes[s].size()));
    for (size_t i = 0; i < expansion.submeshes[s].size(); i++) indices[i] = static_cast<int32_t>(expansion.submeshes[s][i]);
    mesh->SetIndices(indices, UnityEngine::MeshTopology::Triangles, static_cast<int>(s));
  }
  // The original's bounds: culling then matches PC, where the geometry
  // stage's output is not bounded either.
  mesh->set_bounds(source->get_bounds());
  mesh->UploadMeshData(true);

  _emulationMeshes[key] = mesh;
  _emulationMeshSet.insert(mesh);
  return mesh;
}

void Runtime::PrepareGeometryEmulation(std::string const& bundlePath) {
  if (!GetEmulateGeometryShaderEffects()) return;
  auto const plans = ReadGeometryEmulationList(bundlePath);
  if (plans.empty()) return;

  int objectsRebuilt = 0;
  int objectsAlready = 0;
  int objectsFellBack = 0;
  std::set<UnityEngine::Material*> materials;
  std::vector<std::string> reasons;  // first few, for the log
  auto noteFallback = [&](std::string const& reason) {
    objectsFellBack++;
    if (reasons.size() < 8) reasons.push_back(reason);
  };
  auto planFor = [&](UnityEngine::Material* material) -> GeometryEmulation::Plan const* {
    if (!IsAlive(material)) return nullptr;
    auto shader = material->get_shader();
    if (!IsAlive(shader.unsafePtr())) return nullptr;
    auto it = plans.find(ToStdString(shader->get_name()));
    return it == plans.end() ? nullptr : &it->second;
  };

  for (auto const& [path, asset] : _assets) {
    auto* gameObject = il2cpp_utils::try_cast<UnityEngine::GameObject>(asset).value_or(nullptr);
    if (!IsAlive(gameObject)) continue;
    auto renderers = gameObject->GetComponentsInChildren<UnityEngine::Renderer*>(true);
    for (auto* renderer : renderers) {
      if (!IsAlive(renderer)) continue;
      auto shared = renderer->get_sharedMaterials();
      GeometryEmulation::Plan const* plan = nullptr;
      bool mixed = false;  // a point shader beside other materials
      bool any = false;
      for (auto material : shared) {
        auto const* found = planFor(material);
        if (found == nullptr) {
          if (IsAlive(material)) mixed = true;
          continue;
        }
        materials.insert(material);
        if (plan != nullptr && (plan->mode != found->mode || plan->outputVertices != found->outputVertices)) {
          mixed = true;
        }
        plan = found;
        any = true;
      }
      if (!any) continue;

      std::string reason;
      UnityEngine::Mesh* rebuilt = nullptr;
      auto* meshRenderer = il2cpp_utils::try_cast<UnityEngine::MeshRenderer>(renderer).value_or(nullptr);
      UnityEngine::MeshFilter* filter = nullptr;
      if (meshRenderer == nullptr) {
        reason = "'" + ToStdString(renderer->get_name()) + "' is not a mesh renderer";
      } else if (plan->mode == GeometryEmulation::Mode::Point && mixed) {
        // Points become grass blades for every material on the object.
        reason = "'" + ToStdString(renderer->get_name()) + "' draws other materials beside a point shader";
      } else {
        filter = renderer->GetComponent<UnityEngine::MeshFilter*>();
        auto* mesh = IsAlive(filter) ? filter->get_sharedMesh().unsafePtr() : nullptr;
        if (!IsAlive(mesh)) {
          reason = "'" + ToStdString(renderer->get_name()) + "' has no mesh";
        } else if (_emulationMeshSet.contains(mesh)) {
          objectsAlready++;
          continue;
        } else {
          try {
            rebuilt = EmulationMeshFor(mesh, *plan, reason);
          } catch (std::exception const& ex) {
            reason = "rebuilding mesh '" + ToStdString(mesh->get_name()) + "' threw: " + ex.what();
            rebuilt = nullptr;
          }
        }
      }
      if (rebuilt != nullptr) {
        filter->set_sharedMesh(rebuilt);
        objectsRebuilt++;
        continue;
      }
      // Fall back for this object only: each emulated material it draws gets
      // a stand-in copy, which is what it looked like without emulation.
      noteFallback(reason);
      bool changed = false;
      ArrayW<UnityEngine::Material*> replaced(shared.size());
      for (size_t i = 0; i < shared.size(); i++) {
        replaced[i] = shared[i];
        if (planFor(shared[i]) == nullptr) continue;
        // No copy (stand-in shading off): the original stays, and on a mesh
        // that was not rebuilt it draws nothing -- as it did before.
        if (auto* copy = StandInCopyFor(shared[i]); copy != nullptr) {
          replaced[i] = copy;
          changed = true;
        }
      }
      if (changed) renderer->set_sharedMaterials(replaced);
    }
  }

  PaperLogger.info(
      "Vivify geometry emulation: {} object(s) given a rebuilt mesh ({} mesh(es) rebuilt, {} object(s) already "
      "done), {} material(s) on emulated shaders, {} object(s) kept the stand-in",
      objectsRebuilt, _emulationMeshes.size(), objectsAlready, materials.size(), objectsFellBack);
  for (auto const& reason : reasons) {
    PaperLogger.warn("Vivify geometry emulation: an object kept the stand-in: {}", reason);
  }
  if (objectsFellBack > static_cast<int>(reasons.size())) {
    PaperLogger.warn("Vivify geometry emulation: {} further object(s) kept the stand-in",
                     objectsFellBack - static_cast<int>(reasons.size()));
  }
  VivifyBreadcrumb(fmt::format("geometry emulation end: {} object(s) rebuilt, {} kept the stand-in",
                               objectsRebuilt, objectsFellBack));
}

void Runtime::ReleaseGeometryEmulation() {
  for (auto const& [key, mesh] : _emulationMeshes) {
    if (IsAlive(mesh)) UnityEngine::Object::Destroy(mesh);
  }
  _emulationMeshes.clear();
  _emulationMeshSet.clear();
  for (auto const& [original, copy] : _emulationStandIns) {
    if (IsAlive(copy)) UnityEngine::Object::Destroy(copy);
  }
  _emulationStandIns.clear();
}

}  // namespace Vivify
