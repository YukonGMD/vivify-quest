// Performance mode for heavy maps (0.14.25-naomi.8). See VivifyPerfMode.hpp
// for the decisions; this file measures, applies, guards and logs.
//
// naomi.7 lowered XRSettings.eyeTextureResolutionScale, which reallocates the
// eye buffers: with the game running slowly the compositor showed white or
// black for seconds at every step. Nothing here touches the eye buffers. The
// scale is XRSettings.renderViewportScale -- how much of the eye buffers is
// rendered into -- which Unity applies on this game's forward path (Meta's own
// dynamic resolution in OVRManager.Update uses the same setter) and keeps only
// where the rendering path supports it, so reading it back says whether it
// took.
#include "VivifyRuntimeInternal.hpp"
#include "main.hpp"

#include "GlobalNamespace/OVRManager.hpp"
#include "UnityEngine/XR/XRSettings.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Vivify {

namespace {

// OVRPlugin's own performance metrics, asked natively: no il2cpp call and no
// allocation. Both return 0 (ovrpSuccess) when they worked.
struct OvrPerfMetrics {
  using Get = int (*)(int metric, float* value);
  using Supported = int (*)(int metric, int* supported);
  Get get = nullptr;
  Supported supported = nullptr;
};

OvrPerfMetrics const& PerfMetrics() {
  static OvrPerfMetrics const functions = [] {
    OvrPerfMetrics found;
    if (void* plugin = dlopen("libOVRPlugin.so", RTLD_NOW | RTLD_NOLOAD)) {
      found.get = reinterpret_cast<OvrPerfMetrics::Get>(dlsym(plugin, "ovrp_GetPerfMetricsFloat"));
      found.supported = reinterpret_cast<OvrPerfMetrics::Supported>(dlsym(plugin, "ovrp_IsPerfMetricsSupported"));
    }
    return found;
  }();
  return functions;
}

constexpr int kAppGpuTimeMetric = 1;  // OVRPlugin.PerfMetrics.App_GpuTime_Float
constexpr int kGpuBusyMetric = 7;     // System_GpuUtilPercentage_Float
constexpr float kHitchMs = 250.0f;
constexpr int kFoveationHighTop = 4;  // OVRManager.FoveatedRenderingLevel
constexpr int kGpuBoost = 3;          // OVRManager.ProcessorPerformanceLevel

// Why the scale stays at full size. Vivify's blits copy the camera's image
// 1:1 and run the map's effect over the whole texture, and Unity keeps every
// camera texture (the depth texture too) full size with the smaller picture
// in one corner: point effects survive that, blurs and anything centred do
// not, and a shader sampling the depth texture at its own screen position
// reads the wrong place. Until those are corrected, the scale holds.
constexpr char const* kHoldBlit = "a Vivify blit is active";
constexpr char const* kHoldScreenTexture = "the map uses screen textures";
constexpr char const* kHoldCamera = "a Vivify camera is active";
constexpr char const* kHoldDepth = "this map's shaders read the scene depth texture";

}  // namespace

std::string PerfProfileDir() {
  return "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/PerfProfiles";
}

// The app's GPU time and the GPU's busy percentage, once a frame for [PERF]
// and the viewport controller. Negative when the plugin does not report them.
void Runtime::ReadGpuMetrics(float& gpuMs, float& gpuBusy) {
  gpuMs = gpuBusy = -1.0f;
  auto const& metrics = PerfMetrics();
  if (metrics.get == nullptr) return;
  if (_gpuMetricsChecked == false) {
    _gpuMetricsChecked = true;
    int supported = 0;
    _gpuTimeSupported = metrics.supported != nullptr && metrics.supported(kAppGpuTimeMetric, &supported) == 0 &&
                        supported != 0;
    supported = 0;
    _gpuBusySupported = metrics.supported != nullptr && metrics.supported(kGpuBusyMetric, &supported) == 0 &&
                        supported != 0;
    PaperLogger.info("Vivify GPU metrics from the OVR plugin: GPU time {}, GPU busy {}",
                     _gpuTimeSupported ? "reported" : "not reported", _gpuBusySupported ? "reported" : "not reported");
  }
  float value = -1.0f;
  if (_gpuTimeSupported && metrics.get(kAppGpuTimeMetric, &value) == 0 && value > 0.0f) {
    // Seconds or milliseconds: no GPU frame takes a second, none under one.
    if (_perfGpuReadings == 0) _perfGpuUnit = value < 1.0f ? 1000.0f : 1.0f;
    _perfGpuReadings++;
    gpuMs = value * _perfGpuUnit;
  }
  value = -1.0f;
  if (_gpuBusySupported && metrics.get(kGpuBusyMetric, &value) == 0 && value >= 0.0f) {
    gpuBusy = value <= 1.0f ? value * 100.0f : value;  // a fraction or a percentage
  }
}

void Runtime::BeginPerformanceMode(float refreshHz) {
  EndPerformanceMode(false);  // anything an interrupted song left behind
  if (!_perfChanges) _perfChanges = std::make_unique<PerfModeChangeLog>();
  if (!_perfPlayProfile) _perfPlayProfile = std::make_unique<PerfProfile>();
  if (!_perfStoredProfile) _perfStoredProfile = std::make_unique<PerfProfile>();
  _perfChanges->Reset(kHitchMs);
  _perfPlayProfile->Clear();
  _perfStoredProfile->Clear();
  for (bool& off : _perfOff) off = false;
  _perfHeavyApplied = false;
  _perfOverSince = -1.0;
  _perfHold = nullptr;
  _perfHeldSeconds = 0.0;
  _perfLastWall = 0.0;
  _perfMinScale = 1.0f;
  _perfScaleSum = 0.0;
  _perfScaleFrames = 0;
  _perfScale = 1.0f;
  _perfDepthCheckedAt = -1e9;
  _perfMapReadsDepth = true;
  _perfFixedScale = static_cast<float>(GetPerformanceModeFixedScale()) / 100.0f;
  _perfModeOn = GetPerformanceMode() || _perfFixedScale > 0.0f;
  if (!_perfModeOn) return;

  float const hz = refreshHz > 1.0f ? refreshHz : 72.0f;
  ViewportController::Config config;
  config.floor = static_cast<float>(GetPerformanceModeFloor()) / 100.0f;
  config.targetMs = 1000.0f / hz * (GetPerformanceModeHalfRefresh() ? 2.0f : 1.0f);
  _viewport.Reset(config);

  _perfGpuTime = _gpuTimeSupported;

  using GlobalNamespace::OVRManager;
  try {
    _perfGameViewportScale = UnityEngine::XR::XRSettings::get_renderViewportScale();
    _perfGameFoveation = OVRManager::get_foveatedRenderingLevel().value__;
    _perfGameGpuLevel = OVRManager::get_suggestedGpuPerfLevel().value__;
    _perfGameGpuFixed = OVRManager::get_gpuLevel();
    _perfGameSaved = true;
  } catch (...) {
    _perfGameSaved = false;
    _perfModeOn = false;
    PaperLogger.warn("Vivify performance mode: the game's display settings could not be read; off for this song");
    return;
  }

  _perfProfileKey = _selectedProfileKey;
  _perfProfileLoaded = false;
  if (GetPerformanceModeLearn() && !_perfProfileKey.empty()) {
    std::ifstream in(PerfProfileDir() + "/" + PerfProfileFileName(_perfProfileKey) + ".txt");
    if (in) {
      std::stringstream text;
      text << in.rdbuf();
      _perfProfileLoaded = _perfStoredProfile->Parse(text.str(), _perfProfileKey);
    }
  }

  VivifyWriteFlushedLine(fmt::format(
      "[PERF] performance mode: {}; target {:.1f} ms ({:.0f} Hz{}), floor {}%, measured by {}; eye buffers "
      "{}x{} at scale {:.2f}, viewport scale {:.2f}, foveation {}, GPU level {}; learned profile: {}",
      _perfFixedScale > 0.0f ? fmt::format("fixed viewport scale {:.2f} (debug setting)", _perfFixedScale)
                             : std::string("on"),
      config.targetMs, hz, GetPerformanceModeHalfRefresh() ? ", half the refresh rate" : "",
      GetPerformanceModeFloor(), _perfGpuTime ? "the app's GPU time from the OVR plugin" : "frame times",
      UnityEngine::XR::XRSettings::get_eyeTextureWidth(), UnityEngine::XR::XRSettings::get_eyeTextureHeight(),
      UnityEngine::XR::XRSettings::get_eyeTextureResolutionScale(), _perfGameViewportScale, _perfGameFoveation,
      _perfGameGpuFixed,
      !GetPerformanceModeLearn() ? std::string("off")
      : _perfProfileLoaded       ? fmt::format("{} s stored, {} heavy", _perfStoredProfile->Seconds(),
                                               _perfStoredProfile->HeavySeconds())
                                 : std::string("none yet for this map and difficulty")));

  if (_perfFixedScale > 0.0f) {
    SetViewportScale(_perfFixedScale, "fixed scale (debug setting)");
  } else if (GetPerformanceMode() && _perfProfileLoaded && _perfStoredProfile->HeavySeconds() >= 10) {
    ApplyHeavyMapSettings("the learned profile says this map is heavy");
  }
}

char const* Runtime::PerformanceHoldReason() {
  if (!_preEffects.empty() || !_postEffects.empty()) return kHoldBlit;
  for (auto const& effects : _midEffects) {
    if (!effects.empty()) return kHoldBlit;
  }
  if (!_declaredTextures.empty()) return kHoldScreenTexture;
  if (!_secondaryCameras.empty()) return kHoldCamera;
  if (!GetPerformanceModeScaleDepthMaps() && GetSceneDepthTexture() && _perfMapReadsDepth) return kHoldDepth;
  return nullptr;
}

void Runtime::PerformanceModeFrame(float frameMs, float gpuMs, double wallSeconds) {
  if (!_perfModeOn) return;
  double const dt = _perfLastWall > 0.0 ? wallSeconds - _perfLastWall : 0.0;
  _perfLastWall = wallSeconds;
  unsigned const hitched = _perfChanges->AddFrame(frameMs);
  for (int m = 0; m < static_cast<int>(PerfMechanism::Count); m++) {
    if (hitched & (1u << m)) {
      TurnOffPerfMechanism(static_cast<PerfMechanism>(m), "a frame over 250 ms, twice as long as those before, "
                                                          "followed its change");
    }
  }
  _perfMinScale = std::min(_perfMinScale, _perfScale);
  _perfScaleSum += _perfScale;
  _perfScaleFrames++;
  if (_perfFixedScale > 0.0f) return;  // debug: the scale stays where it was put

  bool const gpu = gpuMs >= 0.0f;
  float const measured = gpu ? gpuMs : frameMs;
  // The scene-depth decision can arrive after the song starts (the bundle scan
  // runs on its own thread); asked every two seconds, as it allocates.
  if (wallSeconds - _perfDepthCheckedAt >= 2.0) {
    _perfDepthCheckedAt = wallSeconds;
    _perfMapReadsDepth = MapShadersNeedSceneDepth().value_or(true);
  }
  char const* const hold = PerformanceHoldReason();
  _perfHold = hold;
  if (hold != nullptr) _perfHeldSeconds += dt;

  float const song = CurrentSongTime();
  float const cap = _perfProfileLoaded ? _perfStoredProfile->CapAt(song, 2.0f, 0.05f) : 1.0f;
  bool const scaleOff = _perfOff[static_cast<int>(PerfMechanism::Scale)];
  float const wanted = _viewport.Update(measured, gpu, wallSeconds, cap, hold != nullptr || scaleOff);
  if (!scaleOff && GetPerformanceMode() && std::abs(wanted - _perfScale) >= 0.005f) {
    SetViewportScale(wanted, hold != nullptr && wanted >= 0.999f ? hold : _viewport.Reason());
  }

  // Foveation and the GPU boost are set once, when the map turns out heavy,
  // and stay until the song ends: never toggled mid-song.
  if (!_perfHeavyApplied && GetPerformanceMode()) {
    bool const over = _viewport.SmoothedMs() > _viewport.TargetMs() * (gpu ? 0.95f : 1.15f);
    if (!over) {
      _perfOverSince = -1.0;
    } else if (_perfOverSince < 0.0) {
      _perfOverSince = wallSeconds;
    } else if (wallSeconds - _perfOverSince >= 2.0) {
      ApplyHeavyMapSettings("over the frame budget for 2 s");
    }
  }
  if (GetPerformanceModeLearn()) {
    _perfPlayProfile->Note(song, _perfScale, 100.0f * measured / _viewport.TargetMs());
  }
}

void Runtime::SetViewportScale(float scale, char const* reason) {
  float const before = _perfScale;
  double const wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - _perfStartWall).count();
  try {
    UnityEngine::XR::XRSettings::set_renderViewportScale(scale);
    // Unity keeps the value only where the rendering path supports a smaller
    // viewport; anywhere else it stays as it was.
    float const actual = UnityEngine::XR::XRSettings::get_renderViewportScale();
    if (std::abs(actual - scale) > 0.02f) {
      _perfChanges->Record(wall, CurrentSongTime(), PerfMechanism::Scale, before, actual,
                           "Unity did not take the viewport scale");
      TurnOffPerfMechanism(PerfMechanism::Scale, "Unity did not keep the viewport scale on this rendering path");
      return;
    }
    _perfScale = actual;
  } catch (...) {
    TurnOffPerfMechanism(PerfMechanism::Scale, "setting the viewport scale threw");
    return;
  }
  _perfChanges->Record(wall, CurrentSongTime(), PerfMechanism::Scale, before, _perfScale, reason);
}

void Runtime::ApplyHeavyMapSettings(char const* reason) {
  if (_perfHeavyApplied || !_perfGameSaved) return;
  _perfHeavyApplied = true;
  using GlobalNamespace::OVRManager;
  double const wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - _perfStartWall).count();
  try {
    if (!_perfOff[static_cast<int>(PerfMechanism::Foveation)] && _perfGameFoveation < kFoveationHighTop) {
      OVRManager::set_foveatedRenderingLevel(GlobalNamespace::OVRManager_FoveatedRenderingLevel(kFoveationHighTop));
      _perfChanges->Record(wall, CurrentSongTime(), PerfMechanism::Foveation, static_cast<float>(_perfGameFoveation),
                           static_cast<float>(OVRManager::get_foveatedRenderingLevel().value__), reason);
    }
    if (!_perfOff[static_cast<int>(PerfMechanism::Boost)] && _perfGameGpuLevel != kGpuBoost) {
      OVRManager::set_suggestedGpuPerfLevel(GlobalNamespace::OVRManager_ProcessorPerformanceLevel(kGpuBoost));
      _perfChanges->Record(wall, CurrentSongTime(), PerfMechanism::Boost, static_cast<float>(_perfGameGpuFixed),
                           static_cast<float>(OVRManager::get_gpuLevel()), reason);
    }
  } catch (...) {
    PaperLogger.warn("Vivify performance mode: foveation or the GPU boost could not be set");
  }
}

void Runtime::TurnOffPerfMechanism(PerfMechanism mechanism, char const* why) {
  int const index = static_cast<int>(mechanism);
  if (_perfOff[index]) return;
  _perfOff[index] = true;
  using GlobalNamespace::OVRManager;
  double const wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - _perfStartWall).count();
  try {
    switch (mechanism) {
      case PerfMechanism::Scale:
        if (std::abs(_perfScale - _perfGameViewportScale) > 0.001f) {
          float const before = _perfScale;
          UnityEngine::XR::XRSettings::set_renderViewportScale(_perfGameViewportScale);
          _perfScale = _perfGameViewportScale;
          _perfChanges->Record(wall, CurrentSongTime(), mechanism, before, _perfScale, "turned off: back to full size");
        }
        break;
      case PerfMechanism::Foveation:
        if (_perfHeavyApplied && _perfGameSaved) {
          OVRManager::set_foveatedRenderingLevel(GlobalNamespace::OVRManager_FoveatedRenderingLevel(_perfGameFoveation));
          _perfChanges->Record(wall, CurrentSongTime(), mechanism, kFoveationHighTop,
                               static_cast<float>(_perfGameFoveation), "turned off: the game's level back");
        }
        break;
      case PerfMechanism::Boost:
        if (_perfHeavyApplied && _perfGameSaved) {
          OVRManager::set_suggestedGpuPerfLevel(GlobalNamespace::OVRManager_ProcessorPerformanceLevel(_perfGameGpuLevel));
          _perfChanges->Record(wall, CurrentSongTime(), mechanism, static_cast<float>(OVRManager::get_gpuLevel()),
                               static_cast<float>(_perfGameGpuFixed), "turned off: the game's level back");
        }
        break;
      case PerfMechanism::Count:
        break;
    }
  } catch (...) {
  }
  PaperLogger.warn("Vivify performance mode: {} turned off for the rest of the song: {}", PerfMechanismName(mechanism),
                   why);
}

void Runtime::EndPerformanceMode(bool writeLog) {
  using GlobalNamespace::OVRManager;
  if (_perfGameSaved) {
    _perfGameSaved = false;
    try {
      if (std::abs(UnityEngine::XR::XRSettings::get_renderViewportScale() - _perfGameViewportScale) > 0.001f) {
        UnityEngine::XR::XRSettings::set_renderViewportScale(_perfGameViewportScale);
      }
      if (_perfHeavyApplied) {
        OVRManager::set_foveatedRenderingLevel(GlobalNamespace::OVRManager_FoveatedRenderingLevel(_perfGameFoveation));
        OVRManager::set_suggestedGpuPerfLevel(GlobalNamespace::OVRManager_ProcessorPerformanceLevel(_perfGameGpuLevel));
        // Should the game have set a fixed level rather than a suggested one.
        if (_perfGameGpuFixed >= 0 && OVRManager::get_gpuLevel() != _perfGameGpuFixed) {
          OVRManager::set_gpuLevel(_perfGameGpuFixed);
        }
      }
    } catch (...) {
      PaperLogger.warn("Vivify performance mode: the game's display settings could not all be put back");
    }
    _perfScale = 1.0f;
  }
  if (!writeLog || !_perfModeOn || !_perfChanges) {
    if (!writeLog) _perfModeOn = false;
    return;
  }
  _perfModeOn = false;
  float const average = _perfScaleFrames > 0 ? static_cast<float>(_perfScaleSum / _perfScaleFrames) : 1.0f;
  VivifyWriteFlushedLine(fmt::format(
      "[PERF] performance mode summary: viewport scale lowest {:.2f}, average {:.2f}; held at full size {:.0f} s{}; "
      "{} change(s); foveation and GPU boost {}; GPU time readings {}{}",
      _perfMinScale, average, _perfHeldSeconds,
      _perfHold != nullptr ? fmt::format(" (at the end because {})", _perfHold) : std::string(),
      _perfChanges->Count(), _perfHeavyApplied ? "applied" : "not needed", _perfGpuReadings,
      _perfGpuReadings > 0 ? fmt::format(" (unit: {})", _perfGpuUnit > 1.0f ? "seconds" : "milliseconds")
                           : std::string()));
  for (auto const& line : _perfChanges->Lines()) VivifyWriteFlushedLine("[PERF] performance mode change: " + line);

  // The learned profile: saved for songs that played for at least 30 s.
  if (GetPerformanceModeLearn() && !_perfProfileKey.empty() && _perfPlayProfile->Seconds() >= 30) {
    _perfStoredProfile->MergeFrom(*_perfPlayProfile);
    std::error_code ec;
    std::filesystem::create_directories(PerfProfileDir(), ec);
    std::string const path = PerfProfileDir() + "/" + PerfProfileFileName(_perfProfileKey) + ".txt";
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (out) {
      out << _perfStoredProfile->Serialize(_perfProfileKey);
      VivifyWriteFlushedLine(fmt::format("[PERF] performance mode: learned profile saved ({} s, {} heavy) to {}",
                                         _perfStoredProfile->Seconds(), _perfStoredProfile->HeavySeconds(), path));
    } else {
      PaperLogger.warn("Vivify performance mode: could not write the learned profile {}", path);
    }
  }
}

}  // namespace Vivify
