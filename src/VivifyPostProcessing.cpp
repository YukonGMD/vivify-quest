#include "VivifyRuntimeInternal.hpp"
#include "UnityEngine/Experimental/Rendering/GraphicsFormat.hpp"
#include "UnityEngine/CameraClearFlags.hpp"
#include "UnityEngine/RenderSettings.hpp"
#include "UnityEngine/Rect.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "VivifyComponents.hpp"
#include "UnityEngine/Rendering/CameraEvent.hpp"
#include "UnityEngine/Rendering/GraphicsDeviceType.hpp"
#include "UnityEngine/SystemInfo.hpp"
#include "UnityEngine/Camera.hpp"
#include "UnityEngine/Matrix4x4.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/RenderTexture.hpp"
#include "UnityEngine/Rendering/TextureDimension.hpp"
#include "UnityEngine/RenderTextureDescriptor.hpp"
#include "UnityEngine/HideFlags.hpp"

namespace Vivify {

namespace {
UnityEngine::Rendering::RenderTargetIdentifier ToTargetId(UnityEngine::Texture* texture) {
  return UnityEngine::Rendering::RenderTargetIdentifier::op_Implicit___UnityEngine__Rendering__RenderTargetIdentifier(texture);
}
// An eye-resolution size scaled by the Effect Resolution setting.
int EffectSize(int size) {
  float const scale = GetEffectResolutionScale();
  if (scale >= 1.0f) return size;
  return std::max(1, static_cast<int>(static_cast<float>(size) * scale + 0.5f));
}
UnityEngine::Rendering::RenderTargetIdentifier CameraTargetId() {
  return UnityEngine::Rendering::RenderTargetIdentifier::op_Implicit___UnityEngine__Rendering__RenderTargetIdentifier(
      UnityEngine::Rendering::BuiltinRenderTextureType::CameraTarget);
}
}

void Runtime::ApplyBlits(UnityEngine::RenderTexture* src, UnityEngine::RenderTexture* dest,
                         std::string const& cameraName, int phase) {
  PerfScope const perfScope{*this};  // counted as Vivify's own work (naomi.7)
  if (!IsAlive(src) || (dest != nullptr && !IsAlive(dest))) return;
  bool const passthroughOnly = GetDisableAllBlits() || _isResetting || _pauseMenuActive ||
                               _currentBeatmapData == nullptr ||
                               (phase == 1 ? _preEffects.empty()
                                           : (phase == 2 ? _postEffects.empty()
                                                         : (_preEffects.empty() && _postEffects.empty())));
  auto destId = (dest != nullptr) ? ToTargetId(dest) : CameraTargetId();
  // The frame as it leaves Vivify, for the white-view detector (detailed mode).
  bool const probe = phase != 1 && GetDetailedLogging() && !_pauseMenuActive && _currentBeatmapData != nullptr;
  if (passthroughOnly) {
    auto* cb = ImmediateCommandBuffer();
    cb->Blit(ToTargetId(src), destId);
    if (probe) ProbeView(cb, ToTargetId(src));
    UnityEngine::Graphics::ExecuteCommandBuffer(cb);
    cb->Clear();
    if (_viewProbeQueued) IssueViewReadback();
    return;
  }
  auto* main = EnsureCachedBlitTexture(_mainBlitTexture, src);
  auto* scratch = EnsureCachedBlitTexture(_scratchBlitTexture, src);
  if (!IsAlive(main) || !IsAlive(scratch)) {
    auto* cb = ImmediateCommandBuffer();
    cb->Blit(ToTargetId(src), destId);
    UnityEngine::Graphics::ExecuteCommandBuffer(cb);
    cb->Clear();
    return;
  }

  auto* cb = ImmediateCommandBuffer();

  std::vector<UnityEngine::RenderTexture*> selfBlitTemps;

  cb->Blit(ToTargetId(src), ToTargetId(main));

  auto* mainCurrent = main;
  auto* mainScratch = scratch;

  auto emitBlit = [&](UnityEngine::Rendering::RenderTargetIdentifier srcId, UnityEngine::RenderTexture* dst,
                      UnityEngine::Material* material, int pass) {
    auto const dstId = ToTargetId(dst);
    material = BlitMaterialFor(material, dst);
    if (material == nullptr) {
      cb->Blit(srcId, dstId);
    } else if (pass >= 0) {
      cb->Blit(srcId, dstId, material, pass);
    } else {
      cb->Blit(srcId, dstId, material);
    }
  };
  auto renderEffects = [&](std::vector<ActiveBlitEffect> const& effects) {

    for (auto it = effects.rbegin(); it != effects.rend(); ++it) {
      auto const& data = it->data;
      if (data.material != nullptr && !CanUseBlitMaterial(data.material, data.pass)) continue;
      bool const sourceIsMain = data.source == kMainCameraId;
      UnityEngine::RenderTexture* blitSrc = nullptr;
      if (sourceIsMain) {
        blitSrc = mainCurrent;
      } else if (auto found = _declaredTextures.find(data.source); found != _declaredTextures.end()) {
        blitSrc = found->second.texture;
      } else if (auto found = _secondaryCameras.find(data.source); found != _secondaryCameras.end()) {
        blitSrc = SecondaryCameraColorRT(found->second);
      }
      if (!IsAlive(blitSrc)) continue;
      auto blitSrcId = ToTargetId(blitSrc);
      for (auto const& targetName : data.targets) {
        if (targetName == kMainCameraId) {
          if (sourceIsMain) {
            emitBlit(blitSrcId, mainScratch, data.material, data.pass);
            std::swap(mainCurrent, mainScratch);

            blitSrc = mainCurrent;
            blitSrcId = ToTargetId(mainCurrent);
          } else {

            emitBlit(blitSrcId, mainCurrent, data.material, data.pass);
          }
        } else if (auto found = _declaredTextures.find(targetName); found != _declaredTextures.end()) {
          auto* target = found->second.texture;
          if (!IsAlive(target)) continue;
          if (target == blitSrc) {

            auto desc = target->get_descriptor();
            auto temp = UnityEngine::RenderTexture::GetTemporary(desc);
            auto* tempPtr = temp.unsafePtr();
            if (!IsManagedAlive(tempPtr)) continue;
            selfBlitTemps.emplace_back(tempPtr);
            emitBlit(blitSrcId, tempPtr, data.material, data.pass);
            cb->Blit(ToTargetId(tempPtr), ToTargetId(target));
          } else {
            emitBlit(blitSrcId, target, data.material, data.pass);
          }
        }
      }
    }
  };
  if (phase == 0 || phase == 1) {
    renderEffects(_preEffects);
  }
  if (phase == 0 || phase == 2) {
    renderEffects(_postEffects);
  }

  cb->Blit(ToTargetId(mainCurrent), destId);
  if (probe) ProbeView(cb, ToTargetId(mainCurrent));
  UnityEngine::Graphics::ExecuteCommandBuffer(cb);
  cb->Clear();
  if (_viewProbeQueued) IssueViewReadback();
  for (auto* temp : selfBlitTemps) {
    UnityEngine::RenderTexture::ReleaseTemporary(temp);
  }
}

// ---------------------------------------------------------------------------
// Mostly-white view detector (naomi.10-beta.2, detailed mode)
//
// naomi.10-beta.1's tester saw the view go white mid-song and come back the
// moment the game was paused; the logs could show which Blits ran, but not
// that the view was white. Four times a second of song, the frame as it leaves
// Vivify is copied into a 16x16 two-layer texture (one cheap draw), its left
// eye read back -- through AsyncGPUReadback where Unity supports it on this
// GPU, so nothing waits; otherwise a ReadPixels once a second, which stalls a
// little and is only ever done in detailed mode -- and judged on the main
// thread. When nine in ten of the samples are near white, the log says so
// with everything that could be drawing it.

namespace {

// The engine's signatures, as Beat Saber 1.40.8's libunity (Unity 2022.3.33f1)
// names them. Request takes four arguments: the texture, the mip, a pointer to
// a NativeArray destination (null: Unity allocates the buffer itself, which is
// what AsyncGPUReadback.Request passes) and the request it fills in. beta.2
// passed three, so Unity stored the request through whatever the fourth
// register held and the game crashed on the first probe (naomi.10-beta.3).
//   AsyncGPUReadback_CUSTOM_Request_Internal_Texture_1_Injected(
//       ScriptingBackendNativeObjectPtrOpaque*, int, void*, AsyncGPUReadbackRequest&)
//   AsyncGPUReadbackRequest_CUSTOM_{Update,IsDone,HasError,GetLayerDataSize}_Injected(
//       AsyncGPUReadbackRequest*)
//   AsyncGPUReadbackRequest_CUSTOM_GetDataRaw_Injected(AsyncGPUReadbackRequest*, int)
struct ReadbackCalls {
  using Supported = bool (*)();
  using Request = void (*)(UnityEngine::Texture* source, int32_t mip, void* nativeArrayData, ViewReadback* out);
  using Flag = bool (*)(ViewReadback* self);
  using Size = int32_t (*)(ViewReadback* self);
  using Data = void* (*)(ViewReadback* self, int32_t layer);
  using Update = void (*)(ViewReadback* self);
  Request request = nullptr;
  Flag isDone = nullptr;
  Flag hasError = nullptr;
  Size layerSize = nullptr;
  Data data = nullptr;
  Update update = nullptr;
  bool usable = false;
};

// The engine's own calls (the game's managed code has no AsyncGPUReadback
// wrappers), looked up once; any missing, or the GPU without support, and the
// ReadPixels fallback is used.
ReadbackCalls const& Readback() {
  static ReadbackCalls const calls = [] {
    ReadbackCalls found;
    auto const supported = reinterpret_cast<ReadbackCalls::Supported>(
        ResolveEngineIcall("UnityEngine.SystemInfo::SupportsAsyncGPUReadback"));
    found.request = reinterpret_cast<ReadbackCalls::Request>(
        ResolveEngineIcall("UnityEngine.Rendering.AsyncGPUReadback::Request_Internal_Texture_1_Injected"));
    found.isDone = reinterpret_cast<ReadbackCalls::Flag>(
        ResolveEngineIcall("UnityEngine.Rendering.AsyncGPUReadbackRequest::IsDone_Injected"));
    found.hasError = reinterpret_cast<ReadbackCalls::Flag>(
        ResolveEngineIcall("UnityEngine.Rendering.AsyncGPUReadbackRequest::HasError_Injected"));
    found.layerSize = reinterpret_cast<ReadbackCalls::Size>(
        ResolveEngineIcall("UnityEngine.Rendering.AsyncGPUReadbackRequest::GetLayerDataSize_Injected"));
    found.data = reinterpret_cast<ReadbackCalls::Data>(
        ResolveEngineIcall("UnityEngine.Rendering.AsyncGPUReadbackRequest::GetDataRaw_Injected"));
    found.update = reinterpret_cast<ReadbackCalls::Update>(
        ResolveEngineIcall("UnityEngine.Rendering.AsyncGPUReadbackRequest::Update_Injected"));
    found.usable = supported != nullptr && supported() && found.request && found.isDone && found.hasError &&
                   found.layerSize && found.data && found.update;
    PaperLogger.info("Vivify white-view detector: {}", found.usable
                                                           ? "reads the frame back without waiting (AsyncGPUReadback)"
                                                           : "AsyncGPUReadback unavailable; a small ReadPixels once a "
                                                             "second instead");
    return found;
  }();
  return calls;
}

constexpr int kProbeSize = 16;

}  // namespace

void Runtime::ProbeView(UnityEngine::Rendering::CommandBuffer* cb,
                        UnityEngine::Rendering::RenderTargetIdentifier image) {
  if (_viewReadbackPending || _viewProbeQueued || !_hasMainDescriptor) return;
  float const now = CurrentSongTime();
  if (now < _viewProbeDue) return;
  bool const async = Readback().usable;
  _viewProbeDue = now + (async ? 0.25f : 1.0f);
  if (!_viewProbe) {
    auto desc = _cachedMainDescriptor;
    desc.set_width(kProbeSize);
    desc.set_height(kProbeSize);
    desc.set_msaaSamples(1);
    desc.set_depthBufferBits(0);
    desc.set_graphicsFormat(UnityEngine::Experimental::Rendering::GraphicsFormat::R8G8B8A8_UNorm);
    auto* texture = UnityEngine::RenderTexture::New_ctor(desc);
    if (!IsAlive(texture) || !texture->Create()) return;
    texture->set_name(StringW("VivifyViewProbe"));
    _viewProbe = texture;
  }
  cb->Blit(image, ToTargetId(_viewProbe.ptr()));
  _viewProbeQueued = true;
}

void Runtime::IssueViewReadback() {
  _viewProbeQueued = false;
  if (!_viewProbe) return;
  auto const& calls = Readback();
  if (calls.usable) {
    _viewReadback = ViewReadback{};
    calls.request(_viewProbe.ptr(), 0, nullptr, &_viewReadback);
    _viewReadbackPending = _viewReadback.ptr != nullptr;
    return;
  }
  // Fallback: read the probe's first layer now.
  try {
    if (!_viewProbePixels) {
      _viewProbePixels = UnityEngine::Texture2D::New_ctor(kProbeSize, kProbeSize,
                                                         UnityEngine::TextureFormat::RGBA32, false);
    }
    auto* previous = UnityEngine::RenderTexture::get_active().unsafePtr();
    UnityEngine::RenderTexture::set_active(_viewProbe.ptr());
    _viewProbePixels->ReadPixels(UnityEngine::Rect(0, 0, kProbeSize, kProbeSize), 0, 0, false);
    UnityEngine::RenderTexture::set_active(previous);
    auto pixels = _viewProbePixels->GetPixels32();
    if (pixels && pixels.size() == kProbeSize * kProbeSize) {
      JudgeView(reinterpret_cast<uint8_t const*>(pixels.begin()), pixels.size() * 4u, "ReadPixels");
    }
  } catch (...) {
  }
}

void Runtime::PollViewProbe() {
  if (!_viewReadbackPending) return;
  auto const& calls = Readback();
  calls.update(&_viewReadback);
  if (!calls.isDone(&_viewReadback)) return;
  _viewReadbackPending = false;
  if (calls.hasError(&_viewReadback)) return;
  int32_t const size = calls.layerSize(&_viewReadback);
  auto const* data = static_cast<uint8_t const*>(calls.data(&_viewReadback, 0));
  if (data == nullptr || size != kProbeSize * kProbeSize * 4) return;
  JudgeView(data, static_cast<size_t>(size), "AsyncGPUReadback");
}

void Runtime::JudgeView(uint8_t const* pixels, size_t bytes, char const* how) {
  size_t const count = bytes / 4u;
  if (count == 0) return;
  size_t white = 0;
  uint64_t sum = 0;
  for (size_t i = 0; i < count; i++) {
    uint8_t const r = pixels[i * 4], g = pixels[i * 4 + 1], b = pixels[i * 4 + 2];
    if (r >= 235 && g >= 235 && b >= 235) white++;
    sum += r + g + b;
  }
  float const share = static_cast<float>(white) / static_cast<float>(count);
  float const brightness = static_cast<float>(sum) / static_cast<float>(count * 3u * 255u);
  float const now = CurrentSongTime();
  bool const isWhite = share >= 0.9f;
  if (isWhite == _viewWhite) return;
  _viewWhite = isWhite;
  if (isWhite) {
    _viewWhiteSince = now;
    _viewWhiteStretches++;
    VivifyLogLine("WHITE", fmt::format("the view turned mostly white: {:.0f}% of a {}x{} sample of the left eye near "
                                       "white, average brightness {:.2f} ({}); active: {}",
                                       share * 100.0f, kProbeSize, kProbeSize, brightness, how,
                                       DescribeActiveEffects()),
                  true, false);
    VivifyBlackBoxRecord(BlackBox::kWarning, "view turned mostly white");
  } else {
    _viewWhiteSeconds += std::max(0.0f, now - _viewWhiteSince);
    VivifyLogLine("WHITE", fmt::format("the view is no longer mostly white, after {:.1f} s (from song time {:.2f})",
                                       now - _viewWhiteSince, _viewWhiteSince),
                  true, false);
    VivifyBlackBoxRecord(BlackBox::kWarning, "view no longer mostly white");
  }
}

void Runtime::EndViewProbeSong() {
  if (_viewWhite) _viewWhiteSeconds += std::max(0.0f, CurrentSongTime() - _viewWhiteSince);
  if (GetDetailedLogging()) {
    VivifyWriteFlushedLine(fmt::format("[PERF] mostly-white view (detailed mode): {} stretch(es), {:.1f} s in all",
                                       _viewWhiteStretches, _viewWhiteSeconds));
  }
  _viewWhite = false;
  _viewWhiteStretches = 0;
  _viewWhiteSeconds = 0.0f;
  _viewProbeDue = 0.0f;
  _viewReadbackPending = false;
  _viewProbeQueued = false;
  _cameraParamsLoggedAt.clear();
  _cameraParamLines = 0;
}

// Detailed Logging (naomi.10-beta.5): the per-camera values a Blit or
// full-screen effect on this camera reads -- _ProjectionParams,
// _WorldSpaceCameraPos (each eye's under multiview), _ScreenParams,
// _ZBufferParams and unity_OrthoParams. Unity sets these itself for every
// camera it renders, and keeps them out of the global property sheet
// (Shader.GetGlobalVector reads zeros for them), so they are worked out from
// the camera the way Unity works them out; GL.GetGPUProjectionMatrix and
// SystemInfo.graphicsUVStartsAtTop are stripped from this game, so the flip
// comes from the graphics API: Unity flips the projection (x = -1) only where
// texture rows start at the top (Direct3D, Metal, Vulkan) and only when
// rendering into a texture, never on OpenGL ES. The first use of each camera in
// a level, then at most every 30 s of song, eight lines a level: no cost
// otherwise, and none with Detailed Logging off.
void Runtime::LogCameraParams(UnityEngine::Camera* camera, std::string_view role) {
  if (!GetDetailedLogging() || _pauseMenuActive || _currentBeatmapData == nullptr || _cameraParamLines >= 8) return;
  if (!IsManagedAlive(camera)) return;
  float const now = CurrentSongTime();
  std::string const name = ToStdString(camera->get_name());
  std::string const key = std::string(role) + ":" + name;
  if (auto it = _cameraParamsLoggedAt.find(key); it != _cameraParamsLoggedAt.end() && now - it->second < 30.0f) return;
  _cameraParamsLoggedAt[key] = now;
  _cameraParamLines++;
  try {
    float const nearPlane = camera->get_nearClipPlane();
    float const farPlane = camera->get_farClipPlane();
    int const width = camera->get_pixelWidth();
    int const height = camera->get_pixelHeight();
    using UnityEngine::Rendering::GraphicsDeviceType;
    auto const api = UnityEngine::SystemInfo::get_graphicsDeviceType();
    int const apiValue = static_cast<int>(api.value__);
    bool const openGl = apiValue == GraphicsDeviceType::OpenGLES3.value__ ||
                        apiValue == GraphicsDeviceType::OpenGLES2.value__ ||
                        apiValue == GraphicsDeviceType::OpenGLCore.value__;
    auto target = camera->get_targetTexture();
    bool const intoTexture = IsManagedAlive(target.unsafePtr()) || camera->get_stereoEnabled();
    float const projectionX = (!openGl && intoTexture) ? -1.0f : 1.0f;
    bool const reversedZ = UnityEngine::SystemInfo::get_usesReversedZBuffer();
    float const ratio = nearPlane != 0.0f ? farPlane / nearPlane : 0.0f;
    float const zx = reversedZ ? -1.0f + ratio : 1.0f - ratio;
    float const zy = reversedZ ? 1.0f : ratio;
    float const zz = farPlane != 0.0f ? zx / farPlane : 0.0f;
    float const zw = farPlane != 0.0f ? zy / farPlane : 0.0f;
    auto const position = camera->get_transform()->get_position();
    std::string eyes;
    if (camera->get_stereoEnabled()) {
      for (auto eye : {UnityEngine::Camera_StereoscopicEye::Left, UnityEngine::Camera_StereoscopicEye::Right}) {
        auto const toWorld = camera->GetStereoViewMatrix(eye).get_inverse();
        eyes += fmt::format("{}({:.3f}, {:.3f}, {:.3f})", eyes.empty() ? "; per eye " : " / ", toWorld.m03, toWorld.m13,
                            toWorld.m23);
      }
    }
    bool const ortho = camera->get_orthographic();
    float const orthoSize = camera->get_orthographicSize();
    std::string targetText = "the eye buffers";
    if (IsManagedAlive(target.unsafePtr())) {
      targetText = fmt::format("render texture '{}' {}x{}", ToStdString(target->get_name()), target->get_width(),
                               target->get_height());
    } else if (!camera->get_stereoEnabled()) {
      targetText = "the screen";
    }
    VivifyLogLine(
        "CAMERA",
        fmt::format("{} camera '{}' at song {:.2f} (worked out from the camera as Unity does; API {}): "
                    "_ProjectionParams=({:+.0f}, {:.3f}, {:.1f}, {:.6f}); _WorldSpaceCameraPos=({:.3f}, {:.3f}, "
                    "{:.3f}){}; _ScreenParams=({}, {}, {:.6f}, {:.6f}); _ZBufferParams=({:.4f}, {:.4f}, {:.6f}, "
                    "{:.6f}){}; unity_OrthoParams=({:.3f}, {:.3f}, 0, {}); drawing into {}",
                    role, name, now, apiValue, projectionX, nearPlane, farPlane,
                    farPlane != 0.0f ? 1.0f / farPlane : 0.0f, position.x, position.y, position.z, eyes, width, height,
                    width > 0 ? 1.0f + 1.0f / static_cast<float>(width) : 0.0f,
                    height > 0 ? 1.0f + 1.0f / static_cast<float>(height) : 0.0f, zx, zy, zz, zw,
                    reversedZ ? " (reversed depth)" : "", orthoSize * camera->get_aspect(), orthoSize, ortho ? 1 : 0,
                    targetText),
        false, false);
  } catch (std::exception const& ex) {
    PaperLogger.warn("Vivify camera values (detailed logging) could not be read for '{}': {}", name, ex.what());
  }
}

// Everything that could be painting the view, in one line.
std::string Runtime::DescribeActiveEffects() {
  std::string out;
  auto blits = [&](char const* label, std::vector<ActiveBlitEffect> const& effects) {
    for (auto const& effect : effects) {
      std::string shader = "no material";
      if (IsAlive(effect.data.material)) shader = ShaderNameForLog(effect.data.material->get_shader().unsafePtr());
      out += fmt::format("{}blit '{}' ({}, {}, source {}, until {:.2f}{})", out.empty() ? "" : "; ",
                         effect.data.asset, shader, label, effect.data.source, effect.expireTime,
                         ShaderReadsMainTexAs2D(shader) ? ", reads the screen as 2D" : "");
    }
  };
  blits("before the game's effect", _preEffects);
  blits("after the game's effect", _postEffects);
  for (size_t i = 0; i < _midEffects.size(); i++) {
    blits(fmt::format("mid-render order {}", i).c_str(), _midEffects[i]);
  }
  for (auto const& [name, texture] : _declaredTextures) {
    out += fmt::format("{}screen texture '{}'{}", out.empty() ? "" : "; ", name,
                       IsAlive(texture.texture) ? fmt::format(" {}x{}x{}", texture.texture->get_width(),
                                                              texture.texture->get_height(),
                                                              texture.texture->get_descriptor().get_volumeDepth())
                                                : std::string(" (not created)"));
  }
  for (auto const& [name, camera] : _secondaryCameras) {
    out += fmt::format("{}camera '{}' {}", out.empty() ? "" : "; ", name,
                       IsAlive(camera.camera) && camera.camera->get_enabled() ? "on" : "off");
  }
  try {
    auto* main = UnityEngine::Camera::get_main().unsafePtr();
    if (IsAlive(main)) {
      auto const background = main->get_backgroundColor();
      out += fmt::format("{}main camera clear {} background ({:.2f}, {:.2f}, {:.2f})", out.empty() ? "" : "; ",
                         static_cast<int>(main->get_clearFlags().value__), background.r, background.g, background.b);
    }
    auto const fog = UnityEngine::RenderSettings::get_fogColor();
    out += fmt::format("; fog {} colour ({:.2f}, {:.2f}, {:.2f}) density {:.3f}",
                       UnityEngine::RenderSettings::get_fog() ? "on" : "off", fog.r, fog.g, fog.b,
                       UnityEngine::RenderSettings::get_fogDensity());
  } catch (...) {
  }
  char label[192];
  WritePerfLabel(label, sizeof(label));
  out += std::string("; ") + label;
  return out.empty() ? "nothing" : out;
}

void Runtime::CacheMainRenderDescriptor(UnityEngine::RenderTexture* src) {
  if (!IsAlive(src)) return;
  auto desc = src->get_descriptor();

  desc.set_msaaSamples(1);
  desc.set_depthBufferBits(0);
  desc.set_width(EffectSize(desc.get_width()));
  desc.set_height(EffectSize(desc.get_height()));
  bool const changed = !_hasMainDescriptor || desc.get_width() != _cachedMainDescriptor.get_width() ||
                       desc.get_height() != _cachedMainDescriptor.get_height() ||
                       desc.get_dimension() != _cachedMainDescriptor.get_dimension() ||
                       desc.get_volumeDepth() != _cachedMainDescriptor.get_volumeDepth() ||
                       desc.get_graphicsFormat() != _cachedMainDescriptor.get_graphicsFormat() ||
                       src->get_vrUsage().value__ != _cachedMainVrUsage;
  _cachedMainDescriptor = desc;
  _cachedMainVrUsage = src->get_vrUsage().value__;
  _hasMainDescriptor = true;
  // Screen textures follow the eye texture, as upstream's CreateDeclaredTextures
  // does from the camera's OnRenderImage.
  for (auto& [name, dt] : _declaredTextures) {
    if (changed || !dt.fromMainDescriptor) CreateDeclaredTexture(dt);
  }
}

bool Runtime::EnsureMidRenderTextures() {
  if (!_hasMainDescriptor) return false;
  bool const mismatch = !IsAlive(_midMainRT) || !IsAlive(_midScratchRT) ||
                        _midMainRT->get_width() != _cachedMainDescriptor.get_width() ||
                        _midMainRT->get_height() != _cachedMainDescriptor.get_height() ||
                        _midMainRT->get_vrUsage().value__ != _cachedMainVrUsage;
  if (!mismatch) return true;
  ReleaseMidRenderTextures();
  _midMainRT = UnityEngine::RenderTexture::New_ctor(_cachedMainDescriptor);
  _midScratchRT = UnityEngine::RenderTexture::New_ctor(_cachedMainDescriptor);
  VivifyBlackBoxRecord(BlackBox::kTarget, "mid-frame copies (2)", static_cast<float>(_cachedMainDescriptor.get_width()),
                       static_cast<float>(_cachedMainDescriptor.get_height()),
                       static_cast<float>(_cachedMainDescriptor.get_graphicsFormat().value__));
  if (!IsAlive(_midMainRT) || !IsAlive(_midScratchRT) || !_midMainRT->Create() || !_midScratchRT->Create()) {
    ReleaseMidRenderTextures();
    return false;
  }
  ClearRenderTexture(_midMainRT);
  ClearRenderTexture(_midScratchRT);
  return true;
}

void Runtime::ReleaseMidRenderTextures() {
  if (IsAlive(_midMainRT)) {
    _midMainRT->Release();
    UnityEngine::Object::Destroy(_midMainRT);
  }
  if (IsAlive(_midScratchRT)) {
    _midScratchRT->Release();
    UnityEngine::Object::Destroy(_midScratchRT);
  }
  _midMainRT = nullptr;
  _midScratchRT = nullptr;
}

UnityEngine::Rendering::CommandBuffer* Runtime::ImmediateCommandBuffer() {
  if (!_immediateCommandBuffer) {
    _immediateCommandBuffer.emplace(UnityEngine::Rendering::CommandBuffer::New_ctor());
  }
  auto* cb = _immediateCommandBuffer.ptr();
  cb->Clear();
  return cb;
}

UnityEngine::Rendering::CommandBuffer* Runtime::BuildMidRenderCommandBuffer(
    std::vector<ActiveBlitEffect> const& effects, int bucket) {
  using RTI = UnityEngine::Rendering::RenderTargetIdentifier;
  auto& pooled = _midCommandBufferPool[bucket];
  if (!pooled) pooled.emplace(UnityEngine::Rendering::CommandBuffer::New_ctor());
  auto* cb = pooled.ptr();
  cb->Clear();
  RTI srcId{};
  srcId._ctor(UnityEngine::Rendering::BuiltinRenderTextureType::CurrentActive);
  RTI dstId{};
  dstId._ctor(UnityEngine::Rendering::BuiltinRenderTextureType::CameraTarget);
  cb->Blit(srcId, ToTargetId(_midMainRT));

  auto* mainCurrent = _midMainRT;
  auto* mainScratch = _midScratchRT;

  auto emitBlit = [&](RTI blitSrc, UnityEngine::RenderTexture* dst, UnityEngine::Material* material, int pass) {
    RTI const blitDst = ToTargetId(dst);
    material = BlitMaterialFor(material, dst);
    if (material == nullptr) {
      cb->Blit(blitSrc, blitDst);
    } else if (pass >= 0) {
      cb->Blit(blitSrc, blitDst, material, pass);
    } else {
      cb->Blit(blitSrc, blitDst, material);
    }
  };

  for (auto it = effects.rbegin(); it != effects.rend(); ++it) {
    auto const& data = it->data;
    if (data.material != nullptr && !CanUseBlitMaterial(data.material, data.pass)) continue;
    bool const sourceIsMain = data.source == kMainCameraId;
    UnityEngine::RenderTexture* blitSrcTexture = nullptr;
    if (sourceIsMain) {
      blitSrcTexture = mainCurrent;
    } else if (auto found = _declaredTextures.find(data.source); found != _declaredTextures.end()) {
      blitSrcTexture = found->second.texture;
    } else if (auto found = _secondaryCameras.find(data.source); found != _secondaryCameras.end()) {
      blitSrcTexture = SecondaryCameraColorRT(found->second);
    }
    if (!IsAlive(blitSrcTexture)) continue;
    auto blitSrcId = ToTargetId(blitSrcTexture);
    for (auto const& targetName : data.targets) {
      if (targetName == kMainCameraId) {
        if (sourceIsMain) {
          emitBlit(blitSrcId, mainScratch, data.material, data.pass);
          std::swap(mainCurrent, mainScratch);
          blitSrcTexture = mainCurrent;
          blitSrcId = ToTargetId(mainCurrent);
        } else {

          emitBlit(blitSrcId, mainCurrent, data.material, data.pass);
        }
      } else if (auto found = _declaredTextures.find(targetName); found != _declaredTextures.end()) {
        auto* target = found->second.texture;
        if (!IsAlive(target)) continue;
        if (target == blitSrcTexture) {

          auto temp = UnityEngine::RenderTexture::GetTemporary(target->get_descriptor());
          auto* tempPtr = temp.unsafePtr();
          if (!IsManagedAlive(tempPtr)) continue;
          _midSelfBlitTemps.emplace_back(tempPtr);
          emitBlit(blitSrcId, tempPtr, data.material, data.pass);
          cb->Blit(ToTargetId(tempPtr), ToTargetId(target));
        } else {
          emitBlit(blitSrcId, target, data.material, data.pass);
        }
      }
    }
  }

  cb->Blit(ToTargetId(mainCurrent), dstId);

  // CommandBuffer.Blit binds its destination as a colour-only render target, so
  // once this buffer has run the camera is left rendering into the camera
  // target with no depth attachment for the rest of the frame. Everything the
  // camera still has to draw after this CameraEvent -- which for
  // BeforeForwardAlpha/AfterForwardOpaque and earlier is all the transparent
  // geometry: arcs, saber trails, note debris -- then has no depth buffer to
  // test or write against and drops out. That is why arcs disappear only on
  // maps that use a mid-render Blit.
  //
  // SetRenderTarget(rt) with a single identifier re-binds rt as BOTH the colour
  // and the depth surface with LoadAction.Load, so the camera's own depth
  // contents survive and the rest of the frame renders normally.
  cb->SetRenderTarget(CameraTargetId());
  return cb;
}

void Runtime::AddMidRenderCommandBuffers(UnityEngine::Camera* camera) {

  RemoveMidRenderCommandBuffers();
  if (!IsAlive(camera) || GetDisableAllBlits() || _isResetting || _pauseMenuActive ||
      _currentBeatmapData == nullptr || !HasMidRenderEffects()) {
    return;
  }

  if (!EnsureMidRenderTextures()) return;

  static constexpr std::array<int32_t, kMidRenderOrderCount> kOrderEvents = {
      static_cast<int32_t>(UnityEngine::Rendering::CameraEvent::__CameraEvent_Unwrapped::__E_BeforeSkybox),
      static_cast<int32_t>(UnityEngine::Rendering::CameraEvent::__CameraEvent_Unwrapped::__E_AfterSkybox),
      static_cast<int32_t>(UnityEngine::Rendering::CameraEvent::__CameraEvent_Unwrapped::__E_BeforeForwardOpaque),
      static_cast<int32_t>(UnityEngine::Rendering::CameraEvent::__CameraEvent_Unwrapped::__E_AfterForwardOpaque),
      static_cast<int32_t>(UnityEngine::Rendering::CameraEvent::__CameraEvent_Unwrapped::__E_BeforeForwardAlpha),
      static_cast<int32_t>(UnityEngine::Rendering::CameraEvent::__CameraEvent_Unwrapped::__E_AfterForwardAlpha),
  };
  for (int bucket = 0; bucket < kMidRenderOrderCount; bucket++) {
    auto const& effects = _midEffects[bucket];
    if (effects.empty()) continue;
    auto* cb = BuildMidRenderCommandBuffer(effects, bucket);
    if (cb == nullptr) continue;
    camera->AddCommandBuffer(UnityEngine::Rendering::CameraEvent(kOrderEvents[bucket]), cb);

    _activeMidCommandBuffers.emplace_back(kOrderEvents[bucket],
                                          SafePtr<UnityEngine::Rendering::CommandBuffer>(cb));
  }
  _midCommandBufferCamera = camera;
}

void Runtime::RemoveMidRenderCommandBuffers() {
  for (auto& [cameraEvent, cb] : _activeMidCommandBuffers) {
    if (!cb) continue;
    if (IsAlive(_midCommandBufferCamera)) {
      _midCommandBufferCamera->RemoveCommandBuffer(UnityEngine::Rendering::CameraEvent(cameraEvent), cb.ptr());
    }
    // Pooled (BuildMidRenderCommandBuffer): emptied for the next frame, not disposed.
    cb->Clear();
  }
  _activeMidCommandBuffers.clear();
  _midCommandBufferCamera = nullptr;

  for (auto* temp : _midSelfBlitTemps) {
    if (IsAlive(temp)) UnityEngine::RenderTexture::ReleaseTemporary(temp);
  }
  _midSelfBlitTemps.clear();
}

UnityEngine::RenderTexture* Runtime::EnsureCachedBlitTexture(UnityEngine::RenderTexture*& texture,
                                                             UnityEngine::RenderTexture* src) {
  auto desc = src->get_descriptor();
  desc.set_msaaSamples(1);
  desc.set_depthBufferBits(0);
  desc.set_width(EffectSize(desc.get_width()));
  desc.set_height(EffectSize(desc.get_height()));
  int const width = desc.get_width();
  int const height = desc.get_height();
  int const format = desc.get_graphicsFormat().value__;
  if (IsAlive(texture)) {
    if (_cachedBlitWidth == width &&
        _cachedBlitHeight == height &&
        _cachedBlitFormat == format &&
        texture->IsCreated()) {
      return texture;
    }
    ReleaseRenderTexture(texture);
  }
  texture = UnityEngine::RenderTexture::New_ctor(desc);
  if (!IsAlive(texture)) { texture = nullptr; return nullptr; }
  if (!texture->Create()) { ReleaseRenderTexture(texture); return nullptr; }
  _cachedBlitWidth = width;
  _cachedBlitHeight = height;
  _cachedBlitFormat = format;
  ClearRenderTexture(texture);
  VivifyBlackBoxRecord(BlackBox::kTarget, "blit target", static_cast<float>(width), static_cast<float>(height),
                       static_cast<float>(format));
  return texture;
}

void Runtime::ReleaseRenderTexture(UnityEngine::RenderTexture*& texture) {
  if (IsAlive(texture)) {
    texture->Release();
    UnityEngine::Object::Destroy(texture);
  }
  texture = nullptr;
}

void Runtime::ReleaseCachedBlitTextures() {
  ReleaseRenderTexture(_mainBlitTexture);
  ReleaseRenderTexture(_scratchBlitTexture);
}

bool Runtime::CanUseBlitMaterial(UnityEngine::Material* material, int pass) const {
  if (!IsAlive(material)) {
    return false;
  }
  if (auto it = _blitMaterialValidCache.find(material); it != _blitMaterialValidCache.end()) {
    return it->second;
  }
  if (_fallbackShadedMaterials.contains(material)) {
    // The map's own post-processing shader could not run here and was replaced
    // with a generic stand-in. A stand-in is fine on a mesh but meaningless as
    // a screen effect -- running it would paint the frame with an unrelated
    // shader. Skipping the blit leaves the frame untouched instead, which is
    // the closest thing to "this effect is unavailable".
    PaperLogger.warn("Vivify blit skipped: material '{}' is using a stand-in shader, so its screen effect "
                     "cannot be reproduced (this is expected for a converted PC bundle)",
                     ToStdString(material->get_name()));
    _blitMaterialValidCache[material] = false;
    return false;
  }
  auto shader = material->get_shader();
  auto* rawShader = shader.unsafePtr();
  auto shaderName = ShaderNameForLog(rawShader);
  if (!IsAlive(rawShader) || !rawShader->get_isSupported() || IsInternalErrorShaderName(shaderName)) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify blit material invalid: material='{}' shader='{}' supported={} internalError={} pass={}",
                       ToStdString(material->get_name()), shaderName,
                       BoolText(IsAlive(rawShader) && rawShader->get_isSupported()),
                       BoolText(IsInternalErrorShaderName(shaderName)), pass);
    }
    _blitMaterialValidCache[material] = false;
    return false;
  }
  int const passCount = material->get_passCount();
  if (passCount <= 0 || (pass >= 0 && pass >= passCount)) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify blit material invalid pass: material='{}' pass={} passCount={}",
                       ToStdString(material->get_name()), pass, passCount);
    }
    _blitMaterialValidCache[material] = false;
    return false;
  }
  _blitMaterialValidCache[material] = true;
  return true;
}

bool Runtime::SameBlitData(BlitMaterialData const& left, BlitMaterialData const& right) {
  return left.material == right.material &&
         left.priority == right.priority &&
         left.source == right.source &&
         left.targets == right.targets &&
         left.pass == right.pass;
}

PostProcessingOrder Runtime::ParsePostProcessingOrder(rapidjson::Value const& json) {
  // Friendlier spellings, from webbs7524-wq's Vivify-Quest-2: a boolean
  // "beforeMainEffect"/"afterMainEffect", and "phase"/"timing" as synonyms for
  // "order" with "before"/"pre" as short forms. PC Vivify only reads "order",
  // so a map written against it is unaffected; these only widen what a
  // hand-written Quest map is allowed to say.
  if (auto beforeMain = ReadBool(json, "beforeMainEffect"); beforeMain.has_value()) {
    return *beforeMain ? PostProcessingOrder::BeforeMainEffect : PostProcessingOrder::AfterMainEffect;
  }
  if (auto afterMain = ReadBool(json, "afterMainEffect"); afterMain.has_value()) {
    return *afterMain ? PostProcessingOrder::AfterMainEffect : PostProcessingOrder::BeforeMainEffect;
  }
  auto order = ReadStringView(json, "order");
  if (!order.has_value()) order = ReadStringView(json, "phase");
  if (!order.has_value()) order = ReadStringView(json, "timing");
  if (!order.has_value()) return PostProcessingOrder::AfterMainEffect;
  std::string normalized = NormalizeAssetKey(*order);
  if (normalized == "beforemaineffect" || normalized == "beforemain" || normalized == "before" ||
      normalized == "pre") {
    return PostProcessingOrder::BeforeMainEffect;
  }
  if (normalized == "beforeskybox") return PostProcessingOrder::BeforeSkybox;
  if (normalized == "afterskybox") return PostProcessingOrder::AfterSkybox;
  if (normalized == "beforeopaque") return PostProcessingOrder::BeforeOpaque;
  if (normalized == "afteropaque") return PostProcessingOrder::AfterOpaque;
  if (normalized == "beforealpha") return PostProcessingOrder::BeforeAlpha;
  if (normalized == "afteralpha") return PostProcessingOrder::AfterAlpha;
  return PostProcessingOrder::AfterMainEffect;
}

void Runtime::AddOrUpdateBlitEffect(std::vector<ActiveBlitEffect>& effects, ActiveBlitEffect effect) {
  auto existing = std::find_if(effects.begin(), effects.end(), [&](ActiveBlitEffect const& active) {
    return SameBlitData(active.data, effect.data);
  });
  if (existing != effects.end()) {
    if (effect.data.frame.has_value()) {
      existing->data.frame = effect.data.frame;
      existing->expireTime = 0.0f;
    } else {
      existing->data.frame.reset();
      existing->expireTime = effect.expireTime <= 0.0f ? 0.0f : std::max(existing->expireTime, effect.expireTime);
    }
  } else {
    effects.emplace_back(std::move(effect));
  }
  std::stable_sort(effects.begin(), effects.end(), [](ActiveBlitEffect const& left, ActiveBlitEffect const& right) {
    return left.data.priority < right.data.priority;
  });
  while (effects.size() > kMaxActiveBlitEffects) {
    effects.erase(effects.begin());
  }
}

// "asset" is what PC Vivify reads. The others are accepted for hand-written
// Quest maps (from webbs7524-wq's Vivify-Quest-2), in this order of precedence.
std::optional<std::string_view> Runtime::ReadBlitAssetName(rapidjson::Value const& json) {
  for (auto key : {"asset"sv, "material"sv, "effect"sv, "postProcessMaterial"sv, "postProcessingMaterial"sv}) {
    if (auto value = ReadStringView(json, key)) return value;
  }
  return std::nullopt;
}

// Removes the active effects a clearing Blit event describes. Each field the
// event actually names narrows the match; an event naming nothing but
// "clear": true empties that order's whole list.
void Runtime::RemoveMatchingBlitEffects(std::vector<ActiveBlitEffect>& effects, rapidjson::Value const& json,
                                        BlitMaterialData const& filter, bool filterAsset) {
  bool const filterSource = ReadValuePtr(json, "source") != nullptr;
  bool const filterTargets = ReadValuePtr(json, "destination") != nullptr;
  bool const filterPriority = ReadValuePtr(json, "priority") != nullptr;
  bool const filterPass = ReadValuePtr(json, "pass") != nullptr;
  effects.erase(std::remove_if(effects.begin(), effects.end(), [&](ActiveBlitEffect const& active) {
    if (filterAsset && active.data.asset != filter.asset) return false;
    if (filterSource && active.data.source != filter.source) return false;
    if (filterTargets && active.data.targets != filter.targets) return false;
    if (filterPriority && active.data.priority != filter.priority) return false;
    if (filterPass && active.data.pass != filter.pass) return false;
    return true;
  }), effects.end());
}

// A Blit of the screen through a shader whose two-eye program reads _MainTex
// as one plain texture draws Unity's default texture -- solid white, for most
// -- for as long as it runs (naomi.10-beta.2). Said once per shader a level,
// so a white view in a session log has its explanation beside it.
void Runtime::WarnIfBlitReadsScreenAs2D(UnityEngine::Material* material, std::string_view asset,
                                        std::string_view source, float startTime, float duration) {
  if (!IsAlive(material) || source != kMainCameraId) return;
  auto* shader = material->get_shader().unsafePtr();
  if (!IsAlive(shader)) return;
  std::string const name = ShaderNameForLog(shader);
  if (!ShaderReadsMainTexAs2D(name) || !_blitScreenWarned.insert(name).second) return;
  PaperLogger.warn("Vivify Blit '{}' (shader '{}') reads the screen as one plain 2D texture in its two-eye program; "
                   "the Quest's screen is a two-layer array, so this Blit draws Unity's default texture (usually "
                   "solid white) from song time {:.2f} to {:.2f}",
                   asset, name, startTime, startTime + duration);
  VivifyBlackBoxRecord(BlackBox::kWarning, "Blit reads the screen as 2D: " + name);
}

void Runtime::HandleBlit(CustomJSONData::CustomEventData* customEventData, rapidjson::Value const& json) {
  auto asset = ReadBlitAssetName(json);
  // "clear"/"remove": true, or "enabled": false, turns the event into a stop
  // for effects already running rather than a new one. Without it the only way
  // to end a long Blit early was to author its duration exactly.
  bool const clear = ReadBool(json, "clear").value_or(false) || ReadBool(json, "remove").value_or(false) ||
                     !ReadBool(json, "enabled").value_or(true);
  std::string normalizedAsset = asset.has_value() ? NormalizeAssetKey(*asset) : std::string();
  if (GetDisableAllBlits()) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify Blit skipped at time {}: Disable All Blits enabled asset='{}'",
                       customEventData->time, asset.has_value() ? std::string(*asset) : std::string("<none>"));
    }
    return;
  }
  if (asset.has_value() && GetDisableBeat0FilmgrainBlit() && std::fabs(customEventData->time) < 0.01f &&
      normalizedAsset.find("filmgrain") != std::string::npos) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify Blit skipped at time {}: beat-0 filmgrain isolation asset='{}'",
                       customEventData->time, std::string(*asset));
    }
    return;
  }
  float const durationBeats = ReadFloat(json, "duration").value_or(0.0f);
  float const duration = DurationBeatsToSeconds(durationBeats);
  UnityEngine::Material* material = nullptr;
  if (asset.has_value() && !clear) {
    material = GetAssetAs<UnityEngine::Material>(*asset);
    if (material == nullptr) {
      if (GetVivifyDebugLogging()) {
        PaperLogger.warn("Vivify Blit material load failed at time {}: asset='{}'",
                         customEventData->time, std::string(*asset));
      }
      return;
    }
    RepairMaterialShader(material, *asset);
    LogMaterialShader("Blit", *asset, material);
    WarnIfBlitReadsScreenAs2D(material, *asset, ReadStringView(json, "source").value_or(kMainCameraId),
                              customEventData->time, DurationBeatsToSeconds(ReadFloat(json, "duration").value_or(0.0f)));
    bool malformed = false;
    auto properties = ParseMaterialProperties(json, &malformed);
    if (malformed) return;
    if (!properties.empty()) {
      Functions easing = ParseEasing(ReadStringView(json, "easing").value_or("easeLinear"));
      float const startTime = customEventData->time;
      float const currentSongTime = CurrentSongTime();
      bool const completed = duration <= 0.0f || startTime + duration <= currentSongTime;
      float const initialProgress = completed ? 1.0f : 0.0f;
      for (auto const& property : properties) ApplyMaterialProperty(material, property, initialProgress);
      if (!completed) {
        QueueMaterialPropertyAnimation(material, properties, startTime, duration, easing);
      }
    }
  }
  int const priority = ReadInt(json, "priority").value_or(0);
  int const pass = ReadInt(json, "pass").value_or(-1);
  std::string sourceStr{ReadStringView(json, "source").value_or(kMainCameraId)};
  std::vector<std::string> targets = ReadStringListOrSingle(json, "destination");
  if (targets.empty()) targets.emplace_back(kMainCameraId);
  PostProcessingOrder const order = ParsePostProcessingOrder(json);
  auto& effects = [&]() -> std::vector<ActiveBlitEffect>& {
    switch (order) {
      case PostProcessingOrder::BeforeMainEffect: return _preEffects;
      case PostProcessingOrder::AfterMainEffect: return _postEffects;
      default: return _midEffects[static_cast<int>(order)];
    }
  }();
  BlitMaterialData bd{material, priority, std::move(sourceStr), std::move(targets), pass,
                      std::nullopt, std::move(normalizedAsset), customEventData->time};
  if (clear) {
    size_t const before = effects.size();
    RemoveMatchingBlitEffects(effects, json, bd, asset.has_value());
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify Blit clear: time={} asset='{}' order={} removed={}", customEventData->time,
                       asset.has_value() ? std::string(*asset) : std::string("<any>"), static_cast<int>(order),
                       before - effects.size());
    }
    return;
  }
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify Blit event: time={} asset='{}' source='{}' targets={} priority={} pass={} order={} durationBeats={}",
                     customEventData->time,
                     asset.has_value() ? std::string(*asset) : std::string("<none>"),
                     bd.source, bd.targets.size(), priority, pass,
                     static_cast<int>(order), durationBeats);
  }
  float const songTime = CurrentSongTime();
  if (durationBeats == 0.0f) {

    bd.frame = static_cast<int>(UnityEngine::Time::get_frameCount());
    AddOrUpdateBlitEffect(effects, ActiveBlitEffect{std::move(bd), 0.0f});
  } else if (durationBeats > 0.0f && songTime <= customEventData->time + duration) {
    AddOrUpdateBlitEffect(effects, ActiveBlitEffect{std::move(bd), customEventData->time + duration});
  } else if (durationBeats < 0.0f) {
    // A negative duration keeps the effect until a clearing event (or the end
    // of the map) removes it. It is stored with an expiry at infinity rather
    // than 0, because 0 is how a single-frame effect is marked.
    AddOrUpdateBlitEffect(effects, ActiveBlitEffect{std::move(bd), std::numeric_limits<float>::infinity()});
  }

}

// A blit draws into either the eye texture -- a two-slice array, one pass for
// both eyes, which takes the multiview programs a converted map's shaders
// have -- or into a single texture (screen textures, temporaries), where a
// multiview program draws nothing. For the latter, a converted map's blit
// material is swapped for a copy on the single-view build of its shader from
// the blit companion (VivifyAssets.cpp). Quest-built shaders have both
// programs already and are never in _singleViewShaders.
UnityEngine::Material* Runtime::BlitMaterialFor(UnityEngine::Material* material,
                                                UnityEngine::RenderTexture* destination) {
  if (material == nullptr || _singleViewShaders.empty() || !IsAlive(material) || !IsAlive(destination)) {
    return material;
  }
  auto descriptor = destination->get_descriptor();
  if (descriptor.get_dimension() == UnityEngine::Rendering::TextureDimension::Tex2DArray ||
      descriptor.get_volumeDepth() > 1) {
    return material;
  }
  auto* shader = material->get_shader().unsafePtr();
  if (!IsAlive(shader)) return material;
  auto const singleView = _singleViewShaders.find(ShaderNameForLog(shader));
  if (singleView == _singleViewShaders.end() || !IsAlive(singleView->second)) return material;
  auto& copy = _singleViewBlitMaterials[material];
  if (!IsAlive(copy)) {
    copy = UnityEngine::Material::New_ctor(material);
    copy->set_shader(singleView->second);
    copy->set_hideFlags(UnityEngine::HideFlags::DontUnloadUnusedAsset);
    PaperLogger.info("Vivify Blit: material '{}' draws into single-screen textures with the single-view build "
                     "of '{}'", ToStdString(material->get_name()), singleView->first);
  }
  return copy;
}

// Keeps each single-view blit copy in step with its original, which the map
// animates (SetMaterialProperty, AnimateTrack). Once a frame, before blits run.
void Runtime::SyncSingleViewBlitMaterials() {
  for (auto const& [original, copy] : _singleViewBlitMaterials) {
    if (!IsAlive(original) || !IsAlive(copy)) continue;
    copy->CopyPropertiesFromMaterial(original);
  }
}

void Runtime::UpdateBlitEffects() {
  SyncSingleViewBlitMaterials();
  if (GetDisableAllBlits()) {
    if ((!_preEffects.empty() || !_postEffects.empty() || HasMidRenderEffects()) && GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify Blit effects cleared: Disable All Blits enabled pre={} post={}",
                       _preEffects.size(), _postEffects.size());
    }
    _preEffects.clear();
    _postEffects.clear();
    for (auto& bucket : _midEffects) bucket.clear();
    return;
  }
  float const songTime = CurrentSongTime();
  int const frame = static_cast<int>(UnityEngine::Time::get_frameCount());
  bool const dropBeat0Filmgrain = GetDisableBeat0FilmgrainBlit();
  auto cleanup = [&](std::vector<ActiveBlitEffect>& effects) {
    effects.erase(std::remove_if(effects.begin(), effects.end(), [&](ActiveBlitEffect const& e) {
      if (dropBeat0Filmgrain && std::fabs(e.data.eventBeat) < 0.01f &&
          e.data.asset.find("filmgrain") != std::string::npos) {
        return true;
      }
      if (e.data.frame.has_value()) return e.data.frame.value() != frame;
      return e.expireTime > 0.0f && songTime >= e.expireTime;
    }), effects.end());
  };
  cleanup(_preEffects);
  cleanup(_postEffects);
  for (auto& bucket : _midEffects) cleanup(bucket);
}

void Runtime::HandleCreateScreenTexture(rapidjson::Value const& json) {
  auto id = ReadStringView(json, "id");
  if (!id.has_value()) return;
  std::string name(*id);
  DestroyDeclaredTextureById(name);
  DeclaredTextureData dt;
  dt.name = name;
  dt.propertyId = UnityEngine::Shader::PropertyToID(StringW(name));
  dt.xRatio = ReadFloat(json, "xRatio").value_or(1.0f);
  dt.yRatio = ReadFloat(json, "yRatio").value_or(1.0f);
  if (auto w = ReadInt(json, "width")) dt.width = *w;
  if (auto h = ReadInt(json, "height")) dt.height = *h;
  if (auto fmt = ReadStringView(json, "colorFormat"))
    dt.format = [&]() -> std::optional<UnityEngine::RenderTextureFormat> {
      auto s = *fmt;
      if (s == "ARGB32") return UnityEngine::RenderTextureFormat::ARGB32;
      if (s == "Depth") return UnityEngine::RenderTextureFormat::Depth;
      if (s == "ARGBHalf") return UnityEngine::RenderTextureFormat::ARGBHalf;
      if (s == "Shadowmap") return UnityEngine::RenderTextureFormat::Shadowmap;
      if (s == "RGB565") return UnityEngine::RenderTextureFormat::RGB565;
      if (s == "ARGB4444") return UnityEngine::RenderTextureFormat::ARGB4444;
      if (s == "ARGB1555") return UnityEngine::RenderTextureFormat::ARGB1555;
      if (s == "Default") return UnityEngine::RenderTextureFormat::Default;
      if (s == "ARGBFloat") return UnityEngine::RenderTextureFormat::ARGBFloat;
      if (s == "RGFloat") return UnityEngine::RenderTextureFormat::RGFloat;
      if (s == "RFloat") return UnityEngine::RenderTextureFormat::RFloat;
      if (s == "RHalf") return UnityEngine::RenderTextureFormat::RHalf;
      if (s == "R8") return UnityEngine::RenderTextureFormat::R8;
      if (s == "DefaultHDR") return UnityEngine::RenderTextureFormat::DefaultHDR;
      // The integer and packed formats Unity's enum also names. Maps that pack
      // IDs or bit masks into a screen texture ask for these, and an unknown
      // name used to fall through to ARGB32 -- which quantises exactly the data
      // those maps need to read back intact. SupportedRenderTextureFormat still
      // downgrades any this GPU cannot render to.
      if (s == "ARGB2101010") return UnityEngine::RenderTextureFormat::ARGB2101010;
      if (s == "ARGB64") return UnityEngine::RenderTextureFormat::ARGB64;
      if (s == "RGHalf") return UnityEngine::RenderTextureFormat::RGHalf;
      if (s == "ARGBInt") return UnityEngine::RenderTextureFormat::ARGBInt;
      if (s == "RGInt") return UnityEngine::RenderTextureFormat::RGInt;
      if (s == "RInt") return UnityEngine::RenderTextureFormat::RInt;
      if (s == "BGRA32") return UnityEngine::RenderTextureFormat::BGRA32;
      if (s == "RGB111110Float") return UnityEngine::RenderTextureFormat::RGB111110Float;
      if (s == "RG32") return UnityEngine::RenderTextureFormat::RG32;
      if (s == "RGBAUShort") return UnityEngine::RenderTextureFormat::RGBAUShort;
      if (s == "RG16") return UnityEngine::RenderTextureFormat::RG16;
      if (s == "R16") return UnityEngine::RenderTextureFormat::R16;
      return std::nullopt;
    }();
  if (auto fm = ReadStringView(json, "filterMode"))
    dt.filterMode = [&]() -> std::optional<UnityEngine::FilterMode> {
      auto s = *fm;
      if (s == "Point") return UnityEngine::FilterMode::Point;
      if (s == "Bilinear") return UnityEngine::FilterMode::Bilinear;
      if (s == "Trilinear") return UnityEngine::FilterMode::Trilinear;
      return std::nullopt;
    }();
  if (dt.xRatio <= 0.0f) dt.xRatio = 1.0f;
  if (dt.yRatio <= 0.0f) dt.yRatio = 1.0f;
  if (!CreateDeclaredTexture(dt) && !_hasMainDescriptor) return;
  // Kept even if creation from the eye descriptor failed, so the next change
  // of descriptor can try again.
  _declaredTextures[name] = std::move(dt);
}

// Creates (or re-creates) a screen texture.
//
// Upstream builds it from the camera's own render descriptor: on a headset
// that is the eye texture -- a two-slice array with the camera's colour format
// -- so a map's blits draw into it with the same two-eye programs they draw
// the screen with, and its shaders sample it the way they sample the screen.
// A single 2D texture, which is what this port created, is neither: multiview
// programs draw nothing into it, and a screen-space sampler reads nothing from
// it. Until the main camera has rendered once there is no descriptor yet; the
// texture is then made from the camera's pixel size and replaced on the first
// frame (CacheMainRenderDescriptor).
bool Runtime::CreateDeclaredTexture(DeclaredTextureData& dt) {
  if (IsAlive(dt.texture)) {
    dt.texture->Release();
    UnityEngine::Object::Destroy(dt.texture);
  }
  dt.texture = nullptr;
  dt.fromMainDescriptor = _hasMainDescriptor;

  UnityEngine::RenderTextureDescriptor desc{};
  int baseW = 1024;
  int baseH = 512;
  auto mainCam = UnityEngine::Camera::get_main();
  if (_hasMainDescriptor) {
    desc = _cachedMainDescriptor;
    baseW = desc.get_width();
    baseH = desc.get_height();
  } else if (IsAlive(mainCam.unsafePtr())) {
    baseW = EffectSize(std::max(1, mainCam->get_pixelWidth()));
    baseH = EffectSize(std::max(1, mainCam->get_pixelHeight()));
  }
  int w = std::clamp(dt.width.value_or(baseW), 1, kMaxRenderTextureSize);
  int h = std::clamp(dt.height.value_or(baseH), 1, kMaxRenderTextureSize);
  w = std::clamp(static_cast<int>(w / dt.xRatio), 1, kMaxRenderTextureSize);
  h = std::clamp(static_cast<int>(h / dt.yRatio), 1, kMaxRenderTextureSize);
  // No colorFormat: the eye texture's own format, as upstream.
  std::optional<UnityEngine::RenderTextureFormat> fmt;
  if (dt.format.has_value()) {
    fmt = SupportedRenderTextureFormat(dt.format.value(), "CreateScreenTexture:" + dt.name);
  } else if (!_hasMainDescriptor) {
    fmt = UnityEngine::RenderTextureFormat::ARGB32;
  }
  if (_hasMainDescriptor) {
    desc.set_width(w);
    desc.set_height(h);
    if (fmt.has_value()) desc.set_colorFormat(fmt.value());
  } else {
    desc._ctor(w, h, fmt.value(), 0);
    if (IsAlive(mainCam.unsafePtr()) && mainCam->get_stereoEnabled()) {
      desc.set_vrUsage(UnityEngine::VRTextureUsage::TwoEyes);
    }
  }
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify CreateScreenTexture: id='{}' size={}x{} slices={} requestedFormat={} finalFormat={} "
                     "graphicsFormat={} fromEyeTexture={} filter={} propertyId={}",
                     dt.name, w, h, desc.get_volumeDepth(), dt.format.has_value() ? dt.format->value__ : -1,
                     fmt.has_value() ? fmt->value__ : -1, desc.get_graphicsFormat().value__,
                     BoolText(_hasMainDescriptor), dt.filterMode.has_value() ? dt.filterMode->value__ : -1,
                     dt.propertyId);
  }

  dt.texture = UnityEngine::RenderTexture::New_ctor(desc);
  VivifyBlackBoxRecord(BlackBox::kTarget, "screen texture '" + dt.name + "'", static_cast<float>(w),
                       static_cast<float>(h), static_cast<float>(desc.get_graphicsFormat().value__));
  if (IsAlive(dt.texture) && dt.filterMode.has_value()) dt.texture->set_filterMode(dt.filterMode.value());
  if (!IsAlive(dt.texture) || !dt.texture->Create()) {
    PaperLogger.warn("Vivify CreateScreenTexture failed: id='{}' size={}x{} slices={} graphicsFormat={}", dt.name, w,
                     h, desc.get_volumeDepth(), desc.get_graphicsFormat().value__);
    bool const fromMain = dt.fromMainDescriptor;
    ReleaseDeclaredTextureData(dt);
    dt.fromMainDescriptor = fromMain;
    return false;
  }
  ClearRenderTexture(dt.texture);
  UnityEngine::Shader::SetGlobalTexture(dt.propertyId, static_cast<UnityEngine::Texture*>(dt.texture));
  return true;
}

void Runtime::ReleaseDeclaredTextureData(DeclaredTextureData& data) {

  if (data.propertyId != 0) {
    UnityEngine::Shader::SetGlobalTexture(data.propertyId, static_cast<UnityEngine::Texture*>(nullptr));
  }
  if (IsAlive(data.texture)) {
    data.texture->Release();
    UnityEngine::Object::Destroy(data.texture);
  }
  data.texture = nullptr;
}

bool Runtime::DestroyDeclaredTextureById(std::string const& id) {
  auto it = _declaredTextures.find(id);
  if (it == _declaredTextures.end()) return false;
  ReleaseDeclaredTextureData(it->second);
  _declaredTextures.erase(it);
  return true;
}

void Runtime::HandleCreateCamera(rapidjson::Value const& json) {
  auto id = ReadStringView(json, "id");
  if (!id.has_value()) return;
  std::string name(*id);
  DestroySecondaryCameraById(name);
  if (GetDisableCreateCameraDepth()) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify CreateCamera skipped: isolation toggle enabled id='{}'", name);
    }
    return;
  }
  SecondaryCameraData cam;
  cam.name = name;
  if (auto tex = ReadStringView(json, "texture")) {
    cam.textureName = std::string(*tex);
    cam.texturePropertyId = UnityEngine::Shader::PropertyToID(StringW(*tex));
  }
  if (auto dtex = ReadStringView(json, "depthTexture")) {
    cam.depthTextureName = std::string(*dtex);
    cam.depthTexturePropertyId = UnityEngine::Shader::PropertyToID(StringW(*dtex));
  }

  if (auto stored = _cameraProperties.find(name); stored != _cameraProperties.end()) {
    cam.properties = stored->second;
  }
  auto* propsVal = ReadValuePtr(json, "properties");
  if (propsVal != nullptr && propsVal->IsObject()) {
    ParseCameraPropertyData(*propsVal, cam.properties);
  }
  _cameraProperties[name] = cam.properties;
  auto mainCam = UnityEngine::Camera::get_main();
  auto* mainCamPtr = mainCam.unsafePtr();
  auto* go = UnityEngine::GameObject::New_ctor(StringW("VivifyCamera_" + name));
  if (IsAlive(mainCamPtr)) {
    go->get_transform()->SetParent(mainCam->get_transform(), false);
  }
  cam.camera = go->AddComponent<UnityEngine::Camera*>();
  if (IsAlive(mainCamPtr)) cam.camera->CopyFrom(mainCamPtr);
  cam.camera->set_depth(IsAlive(mainCamPtr) ? mainCam->get_depth() - 1 : -2.0f);
  EnsureMultipassKeywordController(go);
  int w = 1024, h = 512;
  if (IsAlive(mainCamPtr)) {
    w = std::max(1, mainCam->get_pixelWidth());
    h = std::max(1, mainCam->get_pixelHeight());
  }
  bool const wantColor = cam.texturePropertyId.has_value();
  bool const wantDepth = cam.depthTexturePropertyId.has_value();
  VivifyBlackBoxRecord(BlackBox::kCamera, "camera '" + name + "'" + (wantColor ? " colour" : "") + (wantDepth ? " depth" : ""),
                       static_cast<float>(w), static_cast<float>(h), cam.camera->get_depth());

  if (!wantColor && !wantDepth) {
    cam.camera->set_enabled(false);
  } else {
    // Deliberately no targetTexture and no SetTargetBuffers here.
    //
    // Upstream Vivify puts the capture in SecondaryCameraController and notes
    // why: assigning a target texture disables stereo on the camera. This port
    // did assign one, which on a stereo headset is exactly the wrong thing --
    // and for a camera that only asked for a depth texture it also skipped
    // creating the controller altogether, so nothing ever captured anything.
    // The camera now renders normally (one frame ahead of the main camera) and
    // the controller copies what it needs out of OnRenderImage, per eye.
    cam.camera->set_targetTexture(nullptr);
    cam.camera->set_enabled(true);

    auto* controller = go->AddComponent<SecondaryCameraController*>();
    if (IsAlive(controller)) {
      controller->colorKey = cam.texturePropertyId;
      controller->depthKey = cam.depthTexturePropertyId;
    } else {
      PaperLogger.warn("Vivify CreateCamera '{}': could not attach SecondaryCameraController", name);
    }
  }
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify CreateCamera: id='{}' colorTexture='{}' depthTexture='{}' size={}x{} mainEffect={}",
                     name, cam.textureName.value_or("<none>"), cam.depthTextureName.value_or("<none>"),
                     w, h, BoolText(cam.properties.mainEffect.value_or(false)));
  }
  ApplyCameraProperties(cam.camera, cam.properties);
  ApplyCameraGameObjectProperties(cam.camera, cam.properties);

  // A camera asked for a depth texture is useless without Unity generating one,
  // and _CameraDepthTexture only exists when the camera's depthTextureMode says
  // so. Upstream leaves this to the map declaring depthTextureMode itself;
  // forcing the bit on costs nothing when it was already set and avoids an
  // empty depth texture when the map forgot (or set it on a different camera).
  if (wantDepth && !GetDisableCreateCameraDepth() && IsAlive(cam.camera)) {
    int const mode = cam.camera->get_depthTextureMode().value__;
    int const withDepth = mode | UnityEngine::DepthTextureMode::Depth.value__;
    if (withDepth != mode) {
      cam.camera->set_depthTextureMode(UnityEngine::DepthTextureMode(withDepth));
    }
  }

  _secondaryCameras[name] = std::move(cam);
}

void Runtime::ReleaseSecondaryCameraData(SecondaryCameraData& data) {
  if (data.texturePropertyId.has_value()) {
    UnityEngine::Shader::SetGlobalTexture(data.texturePropertyId.value(), static_cast<UnityEngine::Texture*>(nullptr));
  }
  if (data.depthTexturePropertyId.has_value()) {
    UnityEngine::Shader::SetGlobalTexture(data.depthTexturePropertyId.value(), static_cast<UnityEngine::Texture*>(nullptr));
  }
  if (IsAlive(data.colorRT)) {
    data.colorRT->Release();
    UnityEngine::Object::Destroy(data.colorRT);
  }
  if (IsAlive(data.depthRT)) {
    data.depthRT->Release();
    UnityEngine::Object::Destroy(data.depthRT);
  }
  if (IsAlive(data.camera)) {
    UnityEngine::Object::Destroy(data.camera->get_gameObject());
  }
  data.colorRT = nullptr;
  data.depthRT = nullptr;
  data.camera = nullptr;
}

UnityEngine::RenderTexture* Runtime::SecondaryCameraColorRT(SecondaryCameraData const& cam) {
  if (IsAlive(cam.colorRT)) return cam.colorRT;
  if (!IsAlive(cam.camera)) return nullptr;
  auto* go = cam.camera->get_gameObject().unsafePtr();
  if (!IsAlive(go)) return nullptr;
  auto* controller = go->GetComponent<SecondaryCameraController*>();
  if (!IsAlive(controller) || controller->colorTextures.empty()) return nullptr;
  int eye = 0;
  if (auto* mainCam = IsAlive(_lastMainCameraGO) ? _lastMainCameraGO->GetComponent<UnityEngine::Camera*>() : nullptr;
      IsAlive(mainCam)) {
    eye = mainCam->get_stereoActiveEye().value__;
  }
  if (auto it = controller->colorTextures.find(eye); it != controller->colorTextures.end() && IsAlive(it->second)) {
    return it->second;
  }
  return controller->colorTextures.begin()->second;
}

// Rebinds every secondary camera's captured textures as global shader
// properties for the eye about to be rendered, once per frame -- the same thing
// upstream does from PostProcessingController.OnPreRender.
//
// This used to bail out on `!cam.texturePropertyId.has_value()`, so a camera
// that declared only a depth texture -- which is precisely what a raymarching
// map creates, since it wants scene depth rather than a colour copy -- was
// skipped entirely and its depth texture was never bound to the name the map's
// shader samples.
void Runtime::BindSecondaryCameraTextures() {
  if (_secondaryCameras.empty()) return;
  auto* mainCam = IsAlive(_lastMainCameraGO) ? _lastMainCameraGO->GetComponent<UnityEngine::Camera*>() : nullptr;
  int const eye = IsAlive(mainCam) ? mainCam->get_stereoActiveEye().value__ : 0;

  for (auto& [n, cam] : _secondaryCameras) {
    if (!IsAlive(cam.camera)) continue;
    if (!cam.texturePropertyId.has_value() && !cam.depthTexturePropertyId.has_value()) continue;

    auto* go = cam.camera->get_gameObject().unsafePtr();
    if (!IsAlive(go)) continue;
    auto* controller = go->GetComponent<SecondaryCameraController*>();
    if (!IsAlive(controller)) continue;

    auto bind = [eye](std::optional<int> const& propertyId,
                      std::unordered_map<int, UnityEngine::RenderTexture*> const& textures) {
      if (!propertyId.has_value() || textures.empty()) return;
      auto it = textures.find(eye);
      if (it == textures.end() || !IsManagedAlive(it->second)) {
        // Before both eyes have been rendered once there may only be one
        // texture; binding it is better than leaving the property unset.
        it = textures.begin();
        if (!IsManagedAlive(it->second)) return;
      }
      UnityEngine::Shader::SetGlobalTexture(propertyId.value(),
                                            static_cast<UnityEngine::Texture*>(it->second));
    };

    bind(cam.texturePropertyId, controller->colorTextures);
    bind(cam.depthTexturePropertyId, controller->depthTextures);
  }
}

void Runtime::RestoreSecondaryCullingLayers() {
  for (auto& [name, cam] : _secondaryCameras) {
    if (!IsAlive(cam.camera)) continue;
    auto* go = cam.camera->get_gameObject().unsafePtr();
    if (!IsAlive(go)) continue;
    auto* culling = go->GetComponent<CullingCameraController*>();
    if (IsAlive(culling)) {
      culling->CullingPostRender();
    }
  }
}

bool Runtime::DestroySecondaryCameraById(std::string const& id) {
  auto it = _secondaryCameras.find(id);
  if (it == _secondaryCameras.end()) return false;
  ReleaseSecondaryCameraData(it->second);
  _secondaryCameras.erase(it);
  _cameraProperties.erase(id);
  return true;
}

void Runtime::ApplySecondaryCameraMainEffects(GlobalNamespace::MainEffectController* mainEffectController) {
  if (_secondaryCameras.empty() || _isResetting || _pauseMenuActive || _currentBeatmapData == nullptr) return;
  if (!IsManagedAlive(mainEffectController) ||
      mainEffectController->_mainEffectContainer.ptr() == nullptr ||
      !IsManagedAlive(mainEffectController->_mainEffectContainer)) {
    return;
  }
  auto mainEffectSO = mainEffectController->_mainEffectContainer->get_mainEffect();
  if (mainEffectSO.ptr() == nullptr || !IsManagedAlive(mainEffectSO) || !mainEffectSO->get_hasPostProcessEffect()) {
    return;
  }
  for (auto& [name, cam] : _secondaryCameras) {

    if (!cam.properties.mainEffect.value_or(false)) continue;
    // Secondary cameras no longer own a single RenderTexture -- capture is
    // per-eye and lives on the controller -- so resolve the texture for the eye
    // currently being rendered.
    auto* colorRT = SecondaryCameraColorRT(cam);
    if (!IsAlive(colorRT)) continue;
    auto desc = colorRT->get_descriptor();
    desc.set_msaaSamples(1);
    desc.set_depthBufferBits(0);
    auto temp = UnityEngine::RenderTexture::GetTemporary(desc);
    auto* tempPtr = temp.unsafePtr();
    if (!IsManagedAlive(tempPtr)) continue;
    mainEffectController->OnPreRender();
    mainEffectSO->Render(colorRT, tempPtr, mainEffectController->____fadeValue);
    mainEffectController->OnPostRender();
    UnityEngine::Graphics::Blit(static_cast<UnityEngine::Texture*>(tempPtr), colorRT);
    UnityEngine::RenderTexture::ReleaseTemporary(tempPtr);
  }
}

std::optional<UnityEngine::DepthTextureMode> Runtime::ParseDepthTextureModeName(std::string_view s) {
  if (s == "None") return UnityEngine::DepthTextureMode::None;
  if (s == "Depth") return UnityEngine::DepthTextureMode::Depth;
  if (s == "DepthNormals") return UnityEngine::DepthTextureMode::DepthNormals;
  if (s == "MotionVectors") return UnityEngine::DepthTextureMode::MotionVectors;
  return std::nullopt;
}

std::optional<UnityEngine::DepthTextureMode> Runtime::ParseDepthTextureModeValue(rapidjson::Value const& value) {
  if (value.IsString()) {
    return ParseDepthTextureModeName(std::string_view(value.GetString(), value.GetStringLength()));
  }
  if (!value.IsArray()) {
    return std::nullopt;
  }
  int flags = 0;
  bool parsedAny = false;
  for (auto const& item : value.GetArray()) {
    if (!item.IsString()) continue;
    auto mode = ParseDepthTextureModeName(std::string_view(item.GetString(), item.GetStringLength()));
    if (!mode.has_value()) continue;
    flags |= mode->value__;
    parsedAny = true;
  }
  if (!parsedAny) return std::nullopt;
  return UnityEngine::DepthTextureMode(flags);
}

void Runtime::ParseCameraPropertyData(rapidjson::Value const& json, CameraPropertyData& out) {
  if (auto* dtm = ReadValuePtr(json, "depthTextureMode")) {
    out.depthTextureMode = dtm->IsNull() ? std::nullopt : ParseDepthTextureModeValue(*dtm);
    if (GetDisableCreateCameraDepth()) out.depthTextureMode.reset();
  }
  if (auto* cf = ReadValuePtr(json, "clearFlags")) {
    out.clearFlags = std::nullopt;
    if (cf->IsString()) {
      std::string_view s(cf->GetString(), cf->GetStringLength());
      if (s == "Skybox") out.clearFlags = UnityEngine::CameraClearFlags::Skybox;
      else if (s == "Color" || s == "SolidColor") out.clearFlags = UnityEngine::CameraClearFlags::Color;
      else if (s == "Depth") out.clearFlags = UnityEngine::CameraClearFlags::Depth;
      else if (s == "Nothing") out.clearFlags = UnityEngine::CameraClearFlags::Nothing;
    }
  }
  if (auto* bgVal = ReadValuePtr(json, "backgroundColor")) {
    out.backgroundColor = bgVal->IsNull() ? std::nullopt : ReadColorArray(*bgVal);
  }
  if (auto* cullingVal = ReadValuePtr(json, "culling")) {
    if (cullingVal->IsObject()) {
      CullingMaskData culling;
      bool const v2 = _currentBeatmapData != nullptr && _currentBeatmapData->v2orEarlier;
      culling.tracks = ReadTracks(*cullingVal, v2);
      culling.whitelist = ReadBool(*cullingVal, "whitelist").value_or(false);
      if (culling.tracks.empty()) {
        out.culling.reset();
      } else {
        out.culling = std::move(culling);
      }
    } else {
      out.culling.reset();
    }
  }
  if (auto* bp = ReadValuePtr(json, "bloomPrePass")) {
    out.bloomPrePass = bp->IsBool() ? std::optional(bp->GetBool()) : std::nullopt;
  }
  if (auto* me = ReadValuePtr(json, "mainEffect")) {
    out.mainEffect = me->IsBool() ? std::optional(me->GetBool()) : std::nullopt;
  }
}

void Runtime::ApplyCameraProperties(UnityEngine::Camera* camera, CameraPropertyData const& props) {
  if (!IsAlive(camera)) return;
  bool const isMainCamera = camera->get_gameObject().unsafePtr() == _lastMainCameraGO;
  if (props.depthTextureMode.has_value()) {
    int mode = props.depthTextureMode->value__;

    if (isMainCamera && _mainCamOriginalDepthMode.has_value()) {
      mode |= _mainCamOriginalDepthMode.value();
    }
    camera->set_depthTextureMode(UnityEngine::DepthTextureMode(mode));
  } else if (isMainCamera && _mainCamOriginalDepthMode.has_value()) {
    camera->set_depthTextureMode(UnityEngine::DepthTextureMode(_mainCamOriginalDepthMode.value()));
  }
  if (props.clearFlags.has_value()) {
    camera->set_clearFlags(props.clearFlags.value());
  } else if (isMainCamera && _mainCamOriginalClearFlags.has_value()) {
    camera->set_clearFlags(UnityEngine::CameraClearFlags(_mainCamOriginalClearFlags.value()));
  }
  if (props.backgroundColor.has_value()) {
    camera->set_backgroundColor(props.backgroundColor.value());
  } else if (isMainCamera && _mainCamOriginalBackgroundColor.has_value()) {
    camera->set_backgroundColor(_mainCamOriginalBackgroundColor.value());
  }
}

void Runtime::ApplyCameraGameObjectProperties(UnityEngine::Camera* camera, CameraPropertyData const& props) {
  if (!IsAlive(camera)) return;
  auto* cameraGO = camera->get_gameObject().unsafePtr();
  if (!IsAlive(cameraGO)) return;
  ApplyCullingProperty(cameraGO, props.culling);
  auto* bloomPrePass = cameraGO->GetComponent<GlobalNamespace::BloomPrePass*>();
  if (IsAlive(bloomPrePass)) {
    bloomPrePass->set_enabled(props.bloomPrePass.value_or(true));
  }
}

CullingCameraController* Runtime::EnsureCullingController(UnityEngine::GameObject* gameObject) {
  if (!IsAlive(gameObject)) return nullptr;
  auto* controller = gameObject->GetComponent<CullingCameraController*>();
  if (!IsAlive(controller)) {
    controller = gameObject->AddComponent<CullingCameraController*>();
  }
  return controller;
}

void Runtime::ApplyCullingProperty(UnityEngine::GameObject* cameraGO, std::optional<CullingMaskData> const& culling) {
  if (!IsAlive(cameraGO)) return;
  if (culling.has_value()) {
    auto* controller = EnsureCullingController(cameraGO);
    if (controller != nullptr) {
      controller->SetCullingData(culling->whitelist, culling->tracks);
      controller->set_enabled(true);
    }
  } else {
    auto* controller = cameraGO->GetComponent<CullingCameraController*>();
    if (IsAlive(controller)) {
      controller->ClearCullingData();
      controller->set_enabled(false);
    }
  }
}

void Runtime::HandleSetCameraProperty(rapidjson::Value const& json) {
  auto id = ReadStringView(json, "id");
  std::string camId = id.has_value() ? std::string(*id) : std::string(kMainCameraId);
  auto* propsVal = ReadValuePtr(json, "properties");
  if (propsVal == nullptr || !propsVal->IsObject()) return;
  if (GetDisableCreateCameraDepth() && camId != kMainCameraId) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify SetCameraProperty skipped: CreateCamera/Depth isolation camera='{}'", camId);
    }
    return;
  }

  auto& stored = _cameraProperties[camId];
  ParseCameraPropertyData(*propsVal, stored);
  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify SetCameraProperty: camera='{}' depthTextureMode={} clearFlags={} hasBackground={} culling={} bloomPrePass={} mainEffect={}",
                     camId,
                     stored.depthTextureMode.has_value() ? stored.depthTextureMode->value__ : -1,
                     stored.clearFlags.has_value() ? stored.clearFlags->value__ : -1,
                     BoolText(stored.backgroundColor.has_value()),
                     BoolText(stored.culling.has_value()),
                     stored.bloomPrePass.has_value() ? BoolText(*stored.bloomPrePass) : "unset",
                     stored.mainEffect.has_value() ? BoolText(*stored.mainEffect) : "unset");
  }
  if (camId == kMainCameraId) {
    _mainCameraPropsDirty = true;
    auto mainCam = UnityEngine::Camera::get_main();
    auto* mainCamPtr = mainCam.unsafePtr();
    if (IsAlive(mainCamPtr)) {
      CaptureMainCameraOriginals(mainCamPtr, mainCamPtr->get_gameObject().unsafePtr());
      ApplyCameraProperties(mainCamPtr, stored);
      ApplyCameraGameObjectProperties(mainCamPtr, stored);
    }
    for (auto const& mirror : _mirrorCameras) {
      ApplyMainPropertiesToMirrorCamera(mirror.camera);
    }
  } else {
    auto it = _secondaryCameras.find(camId);
    if (it != _secondaryCameras.end()) {

      ParseCameraPropertyData(*propsVal, it->second.properties);
      ApplyCameraProperties(it->second.camera, it->second.properties);
      ApplyCameraGameObjectProperties(it->second.camera, it->second.properties);
    }
  }
}

void Runtime::CaptureMainCameraOriginals(UnityEngine::Camera* mainCam, UnityEngine::GameObject* mainCamGO) {
  if (!IsAlive(mainCam) || mainCamGO != _lastMainCameraGO) return;
  if (!_mainCamOriginalDepthMode.has_value()) {
    _mainCamOriginalDepthMode = mainCam->get_depthTextureMode().value__;
  }
  if (!_mainCamOriginalClearFlags.has_value()) {
    _mainCamOriginalClearFlags = mainCam->get_clearFlags().value__;
  }
  if (!_mainCamOriginalBackgroundColor.has_value()) {
    _mainCamOriginalBackgroundColor = mainCam->get_backgroundColor();
  }
}

void Runtime::RestoreMainCameraOriginals() {
  if (IsAlive(_lastMainCameraGO)) {
    auto* mainCam = _lastMainCameraGO->GetComponent<UnityEngine::Camera*>();
    if (IsAlive(mainCam)) {
      if (_mainCamOriginalDepthMode.has_value()) {
        mainCam->set_depthTextureMode(UnityEngine::DepthTextureMode(_mainCamOriginalDepthMode.value()));
      }
      if (_mainCamOriginalClearFlags.has_value()) {
        mainCam->set_clearFlags(UnityEngine::CameraClearFlags(_mainCamOriginalClearFlags.value()));
      }
      if (_mainCamOriginalBackgroundColor.has_value()) {
        mainCam->set_backgroundColor(_mainCamOriginalBackgroundColor.value());
      }
    }
    if (IsAlive(mainCam)) RestoreFarCulling(mainCam);
    ApplyCullingProperty(_lastMainCameraGO, std::nullopt);
    auto* bloomPrePass = _lastMainCameraGO->GetComponent<GlobalNamespace::BloomPrePass*>();
    if (IsAlive(bloomPrePass)) {
      bloomPrePass->set_enabled(true);
    }
  }
  _mainCamOriginalDepthMode.reset();
  _mainCamOriginalClearFlags.reset();
  _mainCamOriginalBackgroundColor.reset();
}

// A mirror's camera gets the _Main camera properties, as upstream's
// AddMirrorComponents gives it a CullingCameraController and a
// CameraPropertyController with no id (which resolves to _Main). Without it, a
// map that culls objects from the main view still shows them in mirrors.
// Depth texture mode is OR-ed onto the camera's own, and unset properties fall
// back to the camera's own values, as CameraPropertyController does.
void Runtime::ApplyMainPropertiesToMirrorCamera(UnityEngine::Camera* mirrorCamera) {
  if (_currentBeatmapData == nullptr || _isResetting || !IsAlive(mirrorCamera)) return;
  auto* go = mirrorCamera->get_gameObject().unsafePtr();
  if (!IsAlive(go)) return;
  auto known = std::find_if(_mirrorCameras.begin(), _mirrorCameras.end(),
                            [mirrorCamera](MirrorCameraOriginals const& m) { return m.camera == mirrorCamera; });
  if (known == _mirrorCameras.end()) {
    _mirrorCameras.push_back(MirrorCameraOriginals{
        .camera = mirrorCamera,
        .depthTextureMode = mirrorCamera->get_depthTextureMode().value__,
        .clearFlags = mirrorCamera->get_clearFlags().value__,
        .backgroundColor = mirrorCamera->get_backgroundColor(),
    });
    known = std::prev(_mirrorCameras.end());
    EnsureMultipassKeywordController(go);
  }
  CameraPropertyData props;
  if (auto found = _cameraProperties.find(std::string(kMainCameraId)); found != _cameraProperties.end()) {
    props = found->second;
  }
  int const depthMode = props.depthTextureMode.has_value()
                            ? (props.depthTextureMode->value__ | known->depthTextureMode)
                            : known->depthTextureMode;
  mirrorCamera->set_depthTextureMode(UnityEngine::DepthTextureMode(depthMode));
  mirrorCamera->set_clearFlags(props.clearFlags.value_or(UnityEngine::CameraClearFlags(known->clearFlags)));
  mirrorCamera->set_backgroundColor(props.backgroundColor.value_or(known->backgroundColor));
  ApplyCameraGameObjectProperties(mirrorCamera, props);
}

void Runtime::RestoreMirrorCameras() {
  for (auto const& mirror : _mirrorCameras) {
    if (!IsAlive(mirror.camera)) continue;
    mirror.camera->set_depthTextureMode(UnityEngine::DepthTextureMode(mirror.depthTextureMode));
    mirror.camera->set_clearFlags(UnityEngine::CameraClearFlags(mirror.clearFlags));
    mirror.camera->set_backgroundColor(mirror.backgroundColor);
    auto* go = mirror.camera->get_gameObject().unsafePtr();
    ApplyCullingProperty(go, std::nullopt);
    if (IsAlive(go)) {
      auto* bloomPrePass = go->GetComponent<GlobalNamespace::BloomPrePass*>();
      if (IsAlive(bloomPrePass)) bloomPrePass->set_enabled(true);
    }
  }
  _mirrorCameras.clear();
}

void Runtime::RestoreFarCulling(UnityEngine::Camera* mainCam) {
  if (_appliedFarCulling > 0 && IsAlive(mainCam) && _mainCamOriginalCullDistances.has_value()) {
    auto const& original = _mainCamOriginalCullDistances.value();
    auto distances = ArrayW<float>(static_cast<il2cpp_array_size_t>(original.size()));
    for (size_t i = 0; i < original.size(); i++) distances[i] = original[i];
    mainCam->set_layerCullDistances(distances);
    mainCam->set_layerCullSpherical(_mainCamOriginalCullSpherical);
  }
  _appliedFarCulling = 0;
  _mainCamOriginalCullDistances.reset();
}

// Far Culling Distance: the camera skips any renderer farther than the chosen
// distance, on every layer (a layer the game already culls nearer keeps its
// own distance). Measured as a sphere around the camera, so turning the head
// does not pop things in and out.
void Runtime::ApplyFarCulling(UnityEngine::Camera* mainCam) {
  int const wanted = (_currentBeatmapData != nullptr && !_isResetting) ? GetFarCullingDistance() : 0;
  if (wanted == _appliedFarCulling || !IsAlive(mainCam)) return;
  if (wanted == 0) {
    RestoreFarCulling(mainCam);
    return;
  }
  if (!_mainCamOriginalCullDistances.has_value()) {
    std::vector<float> original(32, 0.0f);
    auto current = mainCam->get_layerCullDistances();
    if (current) {
      for (int i = 0; i < current.size() && i < 32; i++) original[i] = current[i];
    }
    _mainCamOriginalCullDistances = original;
    _mainCamOriginalCullSpherical = mainCam->get_layerCullSpherical();
  }
  auto const& original = _mainCamOriginalCullDistances.value();
  auto distances = ArrayW<float>(static_cast<il2cpp_array_size_t>(32));
  for (int i = 0; i < 32; i++) {
    float const own = original[i];
    distances[i] = (own > 0.0f && own < static_cast<float>(wanted)) ? own : static_cast<float>(wanted);
  }
  mainCam->set_layerCullDistances(distances);
  mainCam->set_layerCullSpherical(true);
  _appliedFarCulling = wanted;
  PaperLogger.info("Vivify main camera: far culling at {}m", wanted);
}

void Runtime::RefreshCameraComponents(bool allowCameraApplier) {
  auto mainCam = UnityEngine::Camera::get_main();
  auto* mainCamPtr = mainCam.unsafePtr();
  auto* mainCamGO = IsAlive(mainCamPtr) ? mainCam->get_gameObject().unsafePtr() : nullptr;
  bool const cameraChanged = mainCamGO != _lastMainCameraGO;
  if (cameraChanged) {
    _lastMainCameraGO = mainCamGO;
    _appliedFarCulling = 0;
    _mainCamOriginalCullDistances.reset();
    _mainCamOriginalDepthMode.reset();
    _mainCamOriginalClearFlags.reset();
    _mainCamOriginalBackgroundColor.reset();
    if (IsAlive(mainCamPtr)) {
      auto* camTransform = mainCamPtr->get_transform().unsafePtr();
      auto pos = IsAlive(camTransform) ? camTransform->get_position() : UnityEngine::Vector3(0.0f, 0.0f, 0.0f);
      auto fwd = IsAlive(camTransform) ? camTransform->get_forward() : UnityEngine::Vector3(0.0f, 0.0f, 0.0f);
      PaperLogger.info("Vivify main camera: name='{}' cullingMask=0x{:08x} stereoMode={} multipassSetting={} pos=({:.2f},{:.2f},{:.2f}) fwd=({:.2f},{:.2f},{:.2f})",
                       IsAlive(mainCamGO) ? ToStdString(mainCamGO->get_name()) : std::string("?"),
                       static_cast<uint32_t>(mainCamPtr->get_cullingMask()),
                       static_cast<int>(UnityEngine::XR::XRSettings::get_stereoRenderingMode().value__),
                       BoolText(GetMultipassRenderingEnabled()),
                       pos.x, pos.y, pos.z, fwd.x, fwd.y, fwd.z);
    }
  }

  if (IsAlive(mainCamPtr)) {
    static int sLastLoggedCullingMask = -2;
    int const mask = mainCamPtr->get_cullingMask();
    if (mask != sLastLoggedCullingMask) {
      sLastLoggedCullingMask = mask;
      PaperLogger.info("Vivify main camera cullingMask is now 0x{:08x}", static_cast<uint32_t>(mask));
    }
  }

  RefreshMultipassRendering(mainCamGO);
  if (IsAlive(mainCamPtr)) {
    if (auto props = _cameraProperties.find(std::string(kMainCameraId)); props != _cameraProperties.end()) {
      CaptureMainCameraOriginals(mainCamPtr, mainCamGO);
      ApplyCameraProperties(mainCamPtr, props->second);
      ApplyCameraGameObjectProperties(mainCamPtr, props->second);
    }
    // Scene depth, the way PC Beat Saber always has it.
    //
    // Raymarchers, black holes, distortion and soft-intersection shaders all
    // sample _CameraDepthTexture to know where the world is. PC Beat Saber's
    // camera renders that texture every frame, so maps never ask for it; the
    // Quest build's camera does not, and a shader sampling it gets Unity's
    // empty stand-in -- every ray "hits" at once and the effect draws nothing.
    // It costs a depth pre-pass over the whole scene, so it is only on while a
    // Vivify map plays, only for maps whose shaders read it
    // (ScanBundleForSceneDepth; Hold My Hand's do not), and it can be turned
    // off in settings. A map that asks for depth itself (SetCameraProperty)
    // still gets it from that.
    auto const mapNeedsDepth = MapShadersNeedSceneDepth();
    if (_currentBeatmapData != nullptr && mapNeedsDepth.has_value() && !mapNeedsDepth.value() &&
        !_sceneDepthSkipLogged) {
      _sceneDepthSkipLogged = true;
      PaperLogger.info("Vivify main camera: no shader in this map reads the scene depth texture; not rendering it");
    }
    // Cost test (debug, naomi.8): the depth pre-pass switched off for the song.
    if (GetDebugCostTest() == kCostTestDepth && _currentBeatmapData != nullptr && !_isResetting) {
      int const mode = mainCamPtr->get_depthTextureMode().value__;
      if ((mode & UnityEngine::DepthTextureMode::Depth.value__) != 0) {
        CaptureMainCameraOriginals(mainCamPtr, mainCamGO);
        mainCamPtr->set_depthTextureMode(
            UnityEngine::DepthTextureMode(mode & ~UnityEngine::DepthTextureMode::Depth.value__));
        PaperLogger.info("Vivify cost test: the main camera's depth pre-pass switched off for this song");
      }
    } else if (GetSceneDepthTexture() && mapNeedsDepth.value_or(true) && _currentBeatmapData != nullptr &&
        !_isResetting) {
      int const mode = mainCamPtr->get_depthTextureMode().value__;
      if ((mode & UnityEngine::DepthTextureMode::Depth.value__) == 0) {
        CaptureMainCameraOriginals(mainCamPtr, mainCamGO);
        mainCamPtr->set_depthTextureMode(UnityEngine::DepthTextureMode(mode | UnityEngine::DepthTextureMode::Depth.value__));
        PaperLogger.info("Vivify main camera: scene depth texture enabled for this map");
      }
    }
  }
  _mainCameraPropsDirty = false;
  if (IsAlive(mainCamPtr)) ApplyFarCulling(mainCamPtr);
  RefreshCameraApplier(mainCamGO, allowCameraApplier);
}

void Runtime::RefreshMultipassRendering(UnityEngine::GameObject* mainCamGO) {

  if (IsAlive(mainCamGO) && _currentBeatmapData != nullptr && !_isResetting) {
    _multipassController = EnsureMultipassKeywordController(mainCamGO);
    return;
  }
  if (_multipassController != nullptr && UnityEngine::Object::op_Implicit_bool(_multipassController)) {
    _multipassController->set_enabled(false);
    UnityEngine::Object::Destroy(_multipassController);
  }
  _multipassController = nullptr;
  SetMultipassShaderState(false);
}

MultipassKeywordController* Runtime::EnsureMultipassKeywordController(UnityEngine::GameObject* gameObject) {
  if (!IsAlive(gameObject)) return nullptr;
  // Called every frame; the component is only looked up again when the
  // camera changes or the one found is gone.
  auto* controller = gameObject == _multipassControllerObject && IsAlive(_multipassController)
                         ? _multipassController
                         : gameObject->GetComponent<MultipassKeywordController*>();
  if (!IsAlive(controller)) {
    controller = gameObject->AddComponent<MultipassKeywordController*>();
  }
  _multipassControllerObject = gameObject;
  if (IsAlive(controller)) {
    bool const enabled = GetMultipassRenderingEnabled();
    if (controller->get_enabled() != enabled) {
      controller->set_enabled(enabled);
    }
    if (!enabled) {
      SetMultipassShaderState(false);
    }
  }
  return controller;
}

void Runtime::RefreshCameraApplier(UnityEngine::GameObject* mainCamGO, bool allowCameraApplier) {
  if (_pauseMenuActive) allowCameraApplier = false;
  if (!allowCameraApplier) {
    if (_cameraApplier != nullptr && UnityEngine::Object::op_Implicit_bool(_cameraApplier) &&
        _cameraApplier->get_enabled()) {
      _cameraApplier->set_enabled(false);
    }
    return;
  }
  if (!IsAlive(mainCamGO)) {
    DestroyCameraApplier();
    return;
  }
  if (_cameraApplier == nullptr || !UnityEngine::Object::op_Implicit_bool(_cameraApplier) ||
      _cameraApplierGO != mainCamGO) {
    DestroyCameraApplier();
    _cameraApplier = mainCamGO->AddComponent<CameraApplier*>();
    _cameraApplier->set_enabled(false);
    _cameraApplierGO = mainCamGO;

    _cameraApplier->mainEffectController = nullptr;
    _cameraApplier->hasMainEffect = false;
  }

  bool const needsBlit = !GetDisableAllBlits() &&
                         (!_preEffects.empty() || !_postEffects.empty() || HasMidRenderEffects() ||
                          !_secondaryCameras.empty());
  if (_cameraApplier->get_enabled() != needsBlit) _cameraApplier->set_enabled(needsBlit);
}

void Runtime::DestroyCameraApplier() {
  RemoveMidRenderCommandBuffers();
  if (_cameraApplier != nullptr && UnityEngine::Object::op_Implicit_bool(_cameraApplier)) {
    _cameraApplier->set_enabled(false);
    UnityEngine::Object::Destroy(_cameraApplier);
  }
  _cameraApplier = nullptr;
  _cameraApplierGO = nullptr;
}

bool Runtime::ShouldBypassImageEffect(GlobalNamespace::ImageEffectController* controller) {
  if (_cameraApplier == nullptr || !UnityEngine::Object::op_Implicit_bool(_cameraApplier) ||
      !_cameraApplier->get_enabled() || !_cameraApplier->hasMainEffect) {
    return false;
  }
  if (!IsManagedAlive(controller)) return false;
  return controller->get_gameObject().unsafePtr() == _cameraApplierGO;
}

}
