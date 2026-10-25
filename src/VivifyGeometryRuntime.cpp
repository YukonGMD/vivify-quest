#include "VivifyRuntimeInternal.hpp"
#include "VivifyGeometryEmulation.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <tuple>

#include "System/Collections/Generic/List_1.hpp"
#include "System/IntPtr.hpp"
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
//
// naomi.6: a mesh is only rebuilt once Unity's own read-only mesh data says
// its vertices and indices are in memory. 743 Aether crashed on every start in
// naomi.5 because the converter had flagged a streamed mesh readable, and
// Mesh.GetIndices copied from the null buffer behind it.

namespace Vivify {

namespace {

std::string ListPath(std::string const& bundlePath) {
  return bundlePath + ".geometry";
}

UnityEngine::Rendering::VertexAttribute TexCoord(int channel) {
  return UnityEngine::Rendering::VertexAttribute(static_cast<int32_t>(4 + channel));
}

// Unity's two read-only mesh data calls that take an IntPtr*. cordl's
// bindings for them never resolve: they look the method up with a by-value
// IntPtr parameter, il2cpp counts no type as assignable to a pointer type, so
// FindMethod finds nothing and every call throws. They are found here by name
// and argument count instead, once, with their signatures checked, and run
// the way cordl runs any static method: through runtime_invoke, which takes a
// pointer argument as the pointer itself and hands back a managed exception
// instead of unwinding through this code.
struct MeshDataCalls {
  MethodInfo const* acquire = nullptr;  // AcquireReadOnlyMeshData(Mesh, IntPtr*)
  MethodInfo const* release = nullptr;  // ReleaseMeshDatas(IntPtr*, int)
};

// Null, said once in the log, if this game's Unity does not have them as
// expected; no mesh is rebuilt then.
MeshDataCalls const* FindMeshDataCalls() {
  static std::optional<MeshDataCalls> const calls = []() -> std::optional<MeshDataCalls> {
    il2cpp_functions::Init();
    auto* klass = il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<UnityEngine::Mesh_MeshDataArray>::get();
    auto* meshClass = il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<UnityEngine::Mesh*>::get();
    auto const* acquire = il2cpp_utils::FindMethodUnsafe(klass, "AcquireReadOnlyMeshData", 2);
    auto const* release = il2cpp_utils::FindMethodUnsafe(klass, "ReleaseMeshDatas", 2);
    auto isStaticVoid = [](MethodInfo const* method) {
      return method != nullptr && (method->flags & METHOD_ATTRIBUTE_STATIC) != 0 &&
             method->return_type != nullptr && method->return_type->type == IL2CPP_TYPE_VOID;
    };
    auto isPointerToIntPtr = [](Il2CppType const* type) {
      return type != nullptr && !type->byref && type->type == IL2CPP_TYPE_PTR && type->data.type != nullptr &&
             type->data.type->type == IL2CPP_TYPE_I;
    };
    bool const acquireOk = isStaticVoid(acquire) && acquire->parameters[0] != nullptr &&
                           !acquire->parameters[0]->byref &&
                           il2cpp_functions::class_from_il2cpp_type(acquire->parameters[0]) == meshClass &&
                           isPointerToIntPtr(acquire->parameters[1]);
    bool const releaseOk = isStaticVoid(release) && isPointerToIntPtr(release->parameters[0]) &&
                           release->parameters[1] != nullptr && !release->parameters[1]->byref &&
                           release->parameters[1]->type == IL2CPP_TYPE_I4;
    if (!acquireOk || !releaseOk) {
      auto state = [](MethodInfo const* method, bool ok) {
        return ok ? "ok" : method == nullptr ? "not found" : "found with a different signature";
      };
      PaperLogger.error("Vivify geometry emulation: Unity's read-only mesh data calls are not as expected "
                        "(AcquireReadOnlyMeshData {}, ReleaseMeshDatas {}); no mesh is rebuilt, and objects on "
                        "emulated shaders keep the stand-in", state(acquire, acquireOk), state(release, releaseOk));
      return std::nullopt;
    }
    return MeshDataCalls{acquire, release};
  }();
  return calls ? &*calls : nullptr;
}

// Whether Unity holds the mesh's vertices and indices in CPU memory, asked
// through its own read-only mesh data before anything copies from them.
// A mesh streamed from the bundle's .resS has neither, whatever its readable
// flag says, and Mesh.GetIndices and the vertex getters copy from the null
// buffer without checking. Unity's bindings return the vertex pointer as base
// plus stream offset and the index pointer as is, without reading through
// either (libunity MeshDataBindings::GetVertexDataPtr / GetIndexDataPtr), so
// this cannot fault; stream 0 starts at offset 0, so a missing copy reads as
// null, and anything below 64 KiB counts as missing too.
bool HasCpuMeshData(MeshDataCalls const& calls, UnityEngine::Mesh* mesh) {
  System::IntPtr data{};
  il2cpp_utils::RunMethodRethrow<void, false>(nullptr, calls.acquire, mesh,
                                              cordl_internals::Ptr<System::IntPtr>(&data));
  if (static_cast<void*>(data.m_value) == nullptr) return false;
  auto release = [&]() {
    int32_t const count = 1;
    il2cpp_utils::RunMethodRethrow<void, false>(nullptr, calls.release, cordl_internals::Ptr<System::IntPtr>(&data),
                                                count);
  };
  auto isRealPointer = [](System::IntPtr pointer) {
    return reinterpret_cast<uintptr_t>(static_cast<void*>(pointer.m_value)) >= 0x10000;
  };
  bool present = false;
  try {
    present = UnityEngine::Mesh_MeshData::GetVertexBufferCount(data) > 0 &&
              isRealPointer(UnityEngine::Mesh_MeshData::GetVertexDataPtr(data, 0)) &&
              UnityEngine::Mesh_MeshData::GetIndexDataSize(data) > 0 &&
              isRealPointer(UnityEngine::Mesh_MeshData::GetIndexDataPtr(data));
  } catch (...) {
    release();
    throw;
  }
  release();
  return present;
}

}  // namespace

bool WriteGeometryEmulationList(std::string const& bundlePath, std::vector<std::string> const& shaders) {
  std::error_code ec;
  if (shaders.empty()) {
    std::filesystem::remove(ListPath(bundlePath), ec);
    if (ec) {
      PaperLogger.warn("Vivify geometry emulation: could not remove the old list beside '{}': {}", bundlePath,
                       ec.message());
      return false;
    }
    return true;
  }
  std::ofstream out(ListPath(bundlePath), std::ios::out | std::ios::trunc);
  out << GeometryEmulation::FormatEmulationList(shaders);
  out.close();
  if (!out) {
    PaperLogger.warn("Vivify geometry emulation: could not write the list beside '{}'", bundlePath);
    std::filesystem::remove(ListPath(bundlePath), ec);  // not a partial one
    return false;
  }
  return true;
}

GeometryEmulation::EmulationList ReadGeometryEmulationList(std::string const& bundlePath) {
  std::ifstream in(ListPath(bundlePath));
  std::string const text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return GeometryEmulation::ParseEmulationList(text);
}

UnityEngine::Mesh* Runtime::EmulationMeshFor(UnityEngine::Mesh* source, GeometryEmulation::Plan const& plan,
                                             std::string& reason) {
  int const sourceId = source->GetInstanceID();
  auto const key = std::make_tuple(sourceId, static_cast<int>(plan.mode), plan.outputVertices);
  if (auto it = _emulationMeshes.find(key); it != _emulationMeshes.end()) {
    if (it->second) return it->second.ptr();
    _emulationMeshes.erase(it);
  }
  // One that is already rebuilt (a second level start on the same bundle).
  if (_emulationMeshSet.contains(sourceId)) return source;

  // Metadata only until HasCpuMeshData has said the data is there: the name,
  // counts, flags, topology and attribute layout never touch the buffers.
  std::string const name = ToStdString(source->get_name());
  if (!source->get_isReadable()) {
    reason = "mesh '" + name + "' is not readable (streamed, or not flagged readable by the conversion), so Unity "
             "keeps no copy to rebuild from";
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
  for (int s = 0; s < submeshCount; s++) {
    if (source->GetTopology(s) != wanted) {
      reason = "mesh '" + name + "' submesh " + std::to_string(s) + " is not drawn as " +
               (plan.mode == GeometryEmulation::Mode::Triangle ? "triangles" : "points");
      return nullptr;
    }
  }
  auto const* calls = FindMeshDataCalls();
  if (calls == nullptr) {
    reason = "mesh '" + name + "' cannot be checked for vertex data in memory (Unity's read-only mesh data calls "
             "were not found)";
    return nullptr;
  }
  if (!HasCpuMeshData(*calls, source)) {
    reason = "mesh '" + name + "' has no vertex or index data in memory (streamed to the GPU), so it cannot be rebuilt";
    return nullptr;
  }
  // Named before its data is read, so that if reading it ever takes the game
  // down, the log says which mesh it was.
  VivifyBreadcrumb(fmt::format("geometry emulation: rebuilding mesh '{}' ({} vertices, {} submesh(es))", name,
                               source->get_vertexCount(), submeshCount));
  std::vector<std::vector<uint32_t>> submeshes;
  for (int s = 0; s < submeshCount; s++) {
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
  try {
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
      for (size_t i = 0; i < expansion.submeshes[s].size(); i++) {
        indices[i] = static_cast<int32_t>(expansion.submeshes[s][i]);
      }
      mesh->SetIndices(indices, UnityEngine::MeshTopology::Triangles, static_cast<int>(s));
    }
    // The original's bounds: culling then matches PC, where the geometry
    // stage's output is not bounded either.
    mesh->set_bounds(source->get_bounds());
    mesh->UploadMeshData(true);
  } catch (...) {
    // A half-built mesh is no use to anything.
    UnityEngine::Object::Destroy(mesh);
    throw;
  }

  _emulationMeshes[key] = mesh;
  _emulationMeshSet.insert(mesh->GetInstanceID());
  return mesh;
}

void Runtime::PrepareGeometryEmulation(std::string const& bundlePath) {
  // The list read when the bundle in memory was loaded, which describes it
  // even if a conversion has rewritten the files beside it since; and it is
  // followed whatever the setting says now, because that bundle's emulated
  // programs draw nothing on a mesh that was not rebuilt. A conversion with
  // the setting off writes no list.
  auto const& list = _emulationList;
  auto const& plans = list.plans;
  if (plans.empty()) return;
  // A list without this build's format line came with an older conversion,
  // whose readable flags cannot be trusted: nothing is rebuilt, and every
  // object on an emulated shader keeps the stand-in. Such a bundle is never
  // current for this build (its marker predates conversion 20, and a list is
  // written before a bundle is marked current), so it is converted again
  // before it is loaded; this only guards against files put there by hand.
  if (!list.current) {
    PaperLogger.warn("Vivify geometry emulation: '{}' came with a list from an older build; its objects keep the "
                     "stand-in until it is converted again (Force Reconvert)", bundlePath);
  }

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
      if (!list.current) {
        reason = "the bundle was converted by an older build";
      } else if (meshRenderer == nullptr) {
        reason = "'" + ToStdString(renderer->get_name()) + "' is not a mesh renderer";
      } else if (plan->mode == GeometryEmulation::Mode::Point && mixed) {
        // Points become grass blades for every material on the object.
        reason = "'" + ToStdString(renderer->get_name()) + "' draws other materials beside a point shader";
      } else {
        filter = renderer->GetComponent<UnityEngine::MeshFilter*>();
        auto* mesh = IsAlive(filter) ? filter->get_sharedMesh().unsafePtr() : nullptr;
        if (!IsAlive(mesh)) {
          reason = "'" + ToStdString(renderer->get_name()) + "' has no mesh";
        } else if (_emulationMeshSet.contains(mesh->GetInstanceID())) {
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

// The RGBA32 copies DecodeUnsupportedBundleTextures made for the bundle being
// unloaded. Its materials go with it, so nothing can still be drawing them.
void Runtime::ReleaseDecodedTextures() {
  for (auto* texture : _decodedTextureObjects) {
    if (IsAlive(texture)) UnityEngine::Object::Destroy(texture);
  }
  _decodedTextureObjects.clear();
  _decodedTextures.clear();
}

void Runtime::ReleaseGeometryEmulation() {
  ReleaseParticleBakes("");
  _standInOriginals.clear();
  for (auto& [key, mesh] : _emulationMeshes) {
    if (mesh) UnityEngine::Object::Destroy(mesh.ptr());
  }
  _emulationMeshes.clear();
  _emulationMeshSet.clear();
  for (auto& [original, copy] : _emulationStandIns) {
    if (copy) UnityEngine::Object::Destroy(copy.ptr());
  }
  _emulationStandIns.clear();
  _emulationList = {};
}

}  // namespace Vivify
