#include "VivifyRuntimeInternal.hpp"
#include <limits>
#include <map>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include "VivifyComponents.hpp"
#include "UnityEngine/ParticleSystem.hpp"
#include "UnityEngine/Mesh.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "GlobalNamespace/PlayerDataModel.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "GlobalNamespace/PlayerSpecificSettings.hpp"
#include "GlobalNamespace/MultiplayerController.hpp"
#include "GlobalNamespace/OVRDisplay.hpp"
#include "GlobalNamespace/OVRManager.hpp"
#include "GlobalNamespace/OVRPlugin.hpp"
#include "UnityEngine/XR/XRSettings.hpp"
#include "Unity/Profiling/ProfilerCategory.hpp"
#include "Unity/Profiling/ProfilerRecorder.hpp"
#include "Unity/Profiling/ProfilerRecorderOptions.hpp"

namespace Vivify {

Runtime& Runtime::Instance() {
  static Runtime runtime;
  return runtime;
}

bool Runtime::IsAlive(UnityEngine::Object* object) const {
  return IsManagedAlive(object);
}

// Return true if a well-known multiplayer mod is loaded (MultiplayerCore).
// Kept only for diagnostics -- see IsInMultiplayerGameplay() for why this must
// NOT be used to decide whether to draw Vivify's visuals.
bool Runtime::IsMultiplayerModLoaded() const {
  CModInfo info{"MultiplayerCore", nullptr, 0};
  auto res = modloader_get_mod(&info, MatchType_IdOnly);
  return res.handle != nullptr;
}

// True only while an actual multiplayer game is running.
//
// The rbatteries1-design/Lars27110 base gated the "Disable Vivify Visuals In
// Multiplayer" setting on IsMultiplayerModLoaded(), i.e. on whether the
// MultiplayerCore *mod is installed*, not on whether the player is actually in
// a multiplayer lobby. Since that setting defaults to on and MultiplayerCore is
// installed on most Quest setups, the old check disabled custom sabers (and
// custom debris) in solo play for basically everyone -- notes still worked,
// because ReplaceNoteVisuals never consulted this. That's the "custom sabers
// don't work" bug.
//
// MultiplayerController only exists in the multiplayer gameplay scene, in both
// vanilla and MultiplayerCore-modded lobbies, so its presence is the actual
// signal. The lookup is cached for the lifetime of a beatmap (ResetRuntime
// clears it) so this stays off the per-note/per-frame path.
bool Runtime::IsInMultiplayerGameplay() {
  if (!_inMultiplayerGameplayCached) {
    _inMultiplayerGameplayCached = true;
    auto* controller = UnityEngine::Object::FindObjectOfType<GlobalNamespace::MultiplayerController*>();
    _inMultiplayerGameplay = IsManagedAlive(controller);
    VIVIFY_DEBUG("Vivify multiplayer check: inMultiplayerGameplay={} multiplayerModLoaded={}",
                 BoolText(_inMultiplayerGameplay), BoolText(IsMultiplayerModLoaded()));
  }
  return _inMultiplayerGameplay;
}

bool Runtime::ShouldDisableVisualsForMultiplayer() {
  return GetDisableVisualsInMultiplayer() && IsInMultiplayerGameplay();
}

void Runtime::LateLoad() {
  auto cjdModInfo = CustomJSONData::modInfo.to_c();
  auto tracksModInfo = CModInfo{.id = "Tracks"};
  modloader_require_mod(&cjdModInfo, CMatchType::MatchType_IdOnly);
  modloader_require_mod(&tracksModInfo, CMatchType::MatchType_IdOnly);
  EnsureBehaviour();
  SongCore::API::Capabilities::RegisterCapability(kCapability);
  // What BSAudioLink registers, so maps that require AudioLink are playable.
  if (GetAudioLinkEnabled()) SongCore::API::Capabilities::RegisterCapability("AudioLink");
  CustomJSONData::CustomEventCallbacks::AddCustomEventCallback(&Runtime::OnCustomEventStatic);
  SongCore::API::LevelSelect::GetLevelWasSelectedEvent() += [](SongCore::API::LevelSelect::LevelWasSelectedEventArgs const& event) {
    Runtime::Instance().HandleLevelSelected(event);
  };

  // A greyed-out play button says nothing about which mod greyed it out, and
  // SongCore aggregates the decision across every installed mod -- so a map
  // blocked by an unrelated requirement looks exactly like one Vivify blocked.
  // Log the full picture whenever it changes; this is usually the fastest way
  // to tell "Vivify has not loaded assets yet" apart from "some other mod is
  // holding this level".
  SongCore::API::PlayButton::GetPlayButtonDisablingModsChangedEvent() +=
      [](std::span<SongCore::API::PlayButton::PlayButtonDisablingModInfo const> disablingMods) {
        if (disablingMods.empty()) {
          PaperLogger.info("Vivify: play button is enabled (no mod is blocking it)");
          return;
        }
        std::string blockers;
        for (auto const& info : disablingMods) {
          if (!blockers.empty()) blockers += ", ";
          blockers += info.modID;
          if (!info.reason.empty()) blockers += " (\"" + info.reason + "\")";
        }
        PaperLogger.info("Vivify: play button blocked by {}", blockers);
      };
}

void Runtime::EnsureBehaviour() {
  if (_behaviour != nullptr) {
    return;
  }
  auto* gameObject = UnityEngine::GameObject::New_ctor(u"VivifyRuntime");
  UnityEngine::Object::DontDestroyOnLoad(gameObject);
  _behaviour = gameObject->AddComponent<RuntimeBehaviour*>();
  RefreshCameraComponents(false);
}

void Runtime::OnBehaviourDestroyed(RuntimeBehaviour* behaviour) {
  if (_behaviour == behaviour) _behaviour = nullptr;
}

void Runtime::OnCustomEventStatic(GlobalNamespace::BeatmapCallbacksController* callbackController,
                                  CustomJSONData::CustomEventData* customEventData) {
  Runtime::Instance().HandleCustomEvent(callbackController, customEventData);
}

void Runtime::Update() {
  // Once the watchdog has stood down, the only per-frame work left is the one
  // check that can bring us back: a new beatmap clears the flag in ResetRuntime.
  if (_selfDisabledThisLevel) return;

  // Budget for one frame of Vivify work. Beat Saber renders at 72-90Hz, so a
  // frame is 11-14ms in total; anything from this mod taking longer than
  // kSlowFrameMs is already visible as a stutter, and a sustained run of them
  // is what a player experiences as a freeze.
  constexpr double kSlowFrameMs = 50.0;
  constexpr int kSlowFrameStreakLimit = 30;
  auto const frameStart = std::chrono::steady_clock::now();

  try {
    // Runs in the menu too, so a stalled download is not left pending.
    CheckDownloadTimeout();
    UpdateSyncedObjects();
    if (_audioTimeSyncController != nullptr && !UnityEngine::Object::op_Implicit_bool(_audioTimeSyncController)) {
      VivifyBlackBoxRecord(BlackBox::kUnload, "level ended (quit or finished)");
      ResetRuntime("level ended (quit or finished)");

      _activeSabers.clear();
      return;
    }
    if (_currentBeatmapData == nullptr) {
      VivifySetSongTime(std::numeric_limits<float>::quiet_NaN());
      RefreshCameraComponents(false);
      _lastGameplayFrame = {};
      if (_leakCheckDueAt >= 0.0f && UnityEngine::Time::get_realtimeSinceStartup() >= _leakCheckDueAt) {
        _leakCheckDueAt = -1.0f;
        LogLeakCheck("menu, 3 s after the level ended");
      }
      return;
    }
    // A frame that took far longer than the game's own pacing is a stall
    // somewhere outside this function -- on a converted map's first frames,
    // the GPU driver compiling its translated shaders. Logged with where in
    // the song it happened, since nothing else marks it in the log.
    if (_lastGameplayFrame != std::chrono::steady_clock::time_point{} && _stallsLogged < 10) {
      double const gap = std::chrono::duration<double>(frameStart - _lastGameplayFrame).count();
      if (gap >= 0.5 && !_pauseMenuActive) {
        _stallsLogged++;
        PaperLogger.warn("Vivify frame stall: the game did not draw for {:.1f}s (songTime {:.2f}, bundle '{}')",
                         gap, CurrentSongTime(), _preloadedBundlePath);
      }
    }
    _lastGameplayFrame = frameStart;
    VivifySetSongTime(CurrentSongTime());
    PollViewProbe();
    RecordPerfFrame(frameStart);
    // The frame level start ran in has finished, rendering included: whatever
    // freezes after "level start end" and before this line is in that frame.
    if (_firstFrameCrumbPending && UnityEngine::Time::get_frameCount() != _levelStartFrame) {
      _firstFrameCrumbPending = false;
      VivifyBreadcrumb(fmt::format("first frame after level start: {:.0f}ms later",
                                   std::chrono::duration<double, std::milli>(frameStart - _levelStartedAt).count()));
    }
    // Ten seconds into the song the converted bundle has loaded and drawn;
    // whatever happens after that is not what the crash guard is for.
    if (!_loadGuardPath.empty() && CurrentSongTime() > 10.0f) DisarmLoadGuard();
    // Detailed mode (naomi.10) times each part; otherwise this is a plain call.
    bool const timeParts = _perfActive && GetDetailedLogging();
    auto part = [this, timeParts](int index, auto&& work) {
      if (!timeParts) {
        work();
        return;
      }
      auto const start = std::chrono::steady_clock::now();
      work();
      _detailPartMs[index] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    part(0, [this]() { UpdateParticleBakes(); });
    part(1, [this]() {
      UpdateMaterialAnimations();
      UpdateGlobalAnimations();
      UpdateAnimatorAnimations();
      UpdatePrefabAnimationSpeed();
    });
    part(2, [this]() { UpdateBlitEffects(); });
    part(3, [this]() {
      UpdateRenderSettingAnimations();
      RefreshCameraComponents(true);
    });
    part(4, [this]() {
      DetectSongRestart();
      UpdateSaberReplacementColors();
      SyncReplacementTintColors();
    });
  } catch (std::exception const& ex) {
    LogThrottledUpdateError(ex.what());
  } catch (...) {
    LogThrottledUpdateError("non-std exception");
  }

  double const frameMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
  _worstFrameMs = std::max(_worstFrameMs, frameMs);
  if (_perfActive) {
    _perfVivifyMs += frameMs;
    // After the frame's own work, so its cost is not counted as Vivify's.
    if (!_pauseMenuActive && CurrentSongTime() >= _perfLiveDue) {
      _perfLiveDue = CurrentSongTime() + 2.0f;
      SamplePerfLive();
      VivifyCheckGlErrors();
    }
    if (GetDetailedLogging() && !_pauseMenuActive) {
      auto const start = std::chrono::steady_clock::now();
      _detailFrames++;
      if (CurrentSongTime() >= _detailDue && _detailFrames > 0) {
        double const n = _detailFrames;
        VivifyLogLine("COST", fmt::format("Vivify's own work per frame over the last {} frame(s): particle bakes {:.3f} ms, "
                                          "animations {:.3f}, blits {:.3f}, render settings and cameras {:.3f}, "
                                          "notes and sabers {:.3f}",
                                          _detailFrames, _detailPartMs[0] / n, _detailPartMs[1] / n,
                                          _detailPartMs[2] / n, _detailPartMs[3] / n, _detailPartMs[4] / n),
                      false, false);
        _detailFrames = 0;
        for (double& ms : _detailPartMs) ms = 0.0;
        _detailDue = CurrentSongTime() + 1.0f;
      }
      VivifyAddLayer3Cost(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()));
    }
  }
  if (frameMs > kSlowFrameMs) {
    _slowFrameStreak++;
    if (_slowFrameStreak == 1) {
      PaperLogger.warn("Vivify frame took {:.1f}ms, over the {:.0f}ms budget", frameMs, kSlowFrameMs);
    }
    if (_slowFrameStreak >= kSlowFrameStreakLimit) {
      _selfDisabledThisLevel = true;
      PaperLogger.error(
          "Vivify has stood down for this level: {} consecutive frames over {:.0f}ms (worst {:.1f}ms). "
          "The map loses its Vivify visuals, but the game keeps running instead of freezing. "
          "Please report this log line with the map name.",
          _slowFrameStreak, kSlowFrameMs, _worstFrameMs);
    }
  } else {
    _slowFrameStreak = 0;
  }
}

void Runtime::LogThrottledUpdateError(std::string_view what) {

  int const frame = static_cast<int>(UnityEngine::Time::get_frameCount());
  if (frame - _lastUpdateErrorFrame < 60) return;
  _lastUpdateErrorFrame = frame;
  PaperLogger.error("Vivify Update threw (frame {}, songTime {}): {} — repeats suppressed ~1s",
                    frame, CurrentSongTime(), std::string(what));
}

float Runtime::CurrentSongTime() {

  int const frame = static_cast<int>(UnityEngine::Time::get_frameCount());
  if (frame == _songTimeCacheFrame) return _songTimeCache;
  _songTimeCacheFrame = frame;
  if (!IsAlive(_audioTimeSyncController)) {
    _audioTimeSyncController = UnityEngine::Object::FindObjectOfType<GlobalNamespace::AudioTimeSyncController*>();
  }
  _songTimeCache = IsAlive(_audioTimeSyncController) ? _audioTimeSyncController->get_songTime() : 0.0f;
  return _songTimeCache;
}

void Runtime::DetectSongRestart() {
  if (_currentBeatmapData == nullptr || _isResetting) return;
  float songTime = CurrentSongTime();
  if (_lastSongTime >= 0.0f && songTime + 0.25f < _lastSongTime) {
    ResetRuntime("song restarted");
    return;
  }
  _lastSongTime = songTime;
  if (IsManagedAlive(_audioTimeSyncController)) {
    _lastKnownSongLength = _audioTimeSyncController->get_songLength();
  }
}

float Runtime::CurrentBpm() const {
  if (TracksStatic::bpmController) {
    return TracksStatic::bpmController->get_currentBpm();
  }
  return 0.0f;
}

float Runtime::DurationBeatsToSeconds(float durationBeats) const {
  float bpm = CurrentBpm();
  if (bpm <= 0.0f) {
    return 0.0f;
  }
  return (60.0f * durationBeats) / bpm;
}

void Runtime::RegisterSyncedObject(UnityEngine::GameObject* root, float startTime) {
  if (!IsAlive(root)) return;
  UnregisterSyncedObject(root);

  SyncedObject synced;
  synced.root = root;
  synced.startTime = startTime;

  auto videoPlayers = root->GetComponentsInChildren<UnityEngine::Video::VideoPlayer*>(true);
  for (int i = 0; i < videoPlayers.size(); i++) {
    auto* vp = videoPlayers[i];
    if (IsAlive(vp)) {

      synced.videoPlayers.emplace_back(vp);
    }
  }

  auto animators = root->GetComponentsInChildren<UnityEngine::Animator*>(true);
  for (int i = 0; i < animators.size(); i++) {
    if (IsAlive(animators[i])) synced.animators.emplace_back(animators[i]);
  }
  auto particleSystems = root->GetComponentsInChildren<UnityEngine::ParticleSystem*>(true);
  for (int i = 0; i < particleSystems.size(); i++) {
    if (IsAlive(particleSystems[i])) synced.particleSystems.emplace_back(particleSystems[i]);
  }

  if (!synced.videoPlayers.empty() || !synced.animators.empty() || !synced.particleSystems.empty()) {
    VIVIFY_DEBUG("Vivify sync: registered '{}' with {} video / {} animator / {} particle component(s), startTime={}",
                 IsAlive(root) ? ToStdString(root->get_name()) : std::string("?"),
                 synced.videoPlayers.size(), synced.animators.size(), synced.particleSystems.size(), startTime);
    _levelPins.Pin(synced.root);
    for (auto* player : synced.videoPlayers) _levelPins.Pin(player);
    for (auto* animator : synced.animators) _levelPins.Pin(animator);
    for (auto* system : synced.particleSystems) _levelPins.Pin(system);
    _syncedObjects.emplace_back(std::move(synced));
    _animationTargetsDirty = true;
  }
}

void Runtime::UpdatePrefabAnimationSpeed() {
  if (_syncedObjects.empty() && _livePrefabs.empty()) {

    _lastSyncSongTime = IsManagedAlive(_audioTimeSyncController) ? _audioTimeSyncController->get_songTime() : -1.0f;
    return;
  }

  float speed = 0.0f;
  if (!_pauseMenuActive && IsManagedAlive(_audioTimeSyncController)) {
    float const songTime = _audioTimeSyncController->get_songTime();
    float const deltaTime = UnityEngine::Time::get_deltaTime();
    float const deltaSong = songTime - _lastSyncSongTime;
    if (_lastSyncSongTime < 0.0f) {
      speed = 1.0f;
    } else if (deltaTime > 1e-5f && deltaSong > 0.0f) {
      speed = deltaSong / deltaTime;
    } else {
      speed = 0.0f;
    }
    _lastSyncSongTime = songTime;
  }

  // The speed jitters from frame to frame (song time over frame time), so it
  // is written every frame -- except when it is exactly what was written last
  // and no object has been added since, as while paused.
  if (speed == _lastAppliedAnimationSpeed && !_animationTargetsDirty) return;
  _lastAppliedAnimationSpeed = speed;
  _animationTargetsDirty = false;

  auto drive = [speed](std::vector<UnityEngine::Animator*> const& animators,
                       std::vector<UnityEngine::ParticleSystem*> const& particleSystems) {
    for (auto* animator : animators) {
      if (IsManagedAlive(animator)) animator->set_speed(speed);
    }
    for (auto* particleSystem : particleSystems) {
      if (!IsManagedAlive(particleSystem)) continue;
      auto main = particleSystem->get_main();
      main.set_simulationSpeed(speed);
    }
  };
  for (auto& synced : _syncedObjects) drive(synced.animators, synced.particleSystems);
  for (auto& [id, prefab] : _livePrefabs) drive(prefab.animators, prefab.particleSystems);
}

void Runtime::UnregisterSyncedObject(UnityEngine::GameObject* root) {
  _syncedObjects.erase(std::remove_if(_syncedObjects.begin(), _syncedObjects.end(),
                                      [root](SyncedObject const& synced) { return synced.root == root; }),
                       _syncedObjects.end());
}

void Runtime::UpdateSyncedObjects() {
  if (_syncedObjects.empty()) return;
  _syncedObjects.erase(std::remove_if(_syncedObjects.begin(), _syncedObjects.end(),
                                      [](SyncedObject const& synced) { return !IsManagedAlive(synced.root); }),
                       _syncedObjects.end());
  if (_syncedObjects.empty()) return;

  bool const paused = _pauseMenuActive || !IsManagedAlive(_audioTimeSyncController);
  float const songTime = IsManagedAlive(_audioTimeSyncController) ? _audioTimeSyncController->get_songTime() : 0.0f;

  bool const heartbeat = [&]() {
    if (!GetVivifyDebugLogging()) return false;
    int const frame = static_cast<int>(UnityEngine::Time::get_frameCount());
    if (frame - _lastSyncHeartbeatFrame < 120) return false;
    _lastSyncHeartbeatFrame = frame;
    return true;
  }();

  for (auto& synced : _syncedObjects) {
    for (auto* vp : synced.videoPlayers) {
      if (!IsManagedAlive(vp)) continue;
      if (paused) {
        if (vp->get_isPlaying()) {
          VIVIFY_DEBUG("Vivify sync: pausing video (songTime={})", songTime);
          vp->Pause();
        }
        continue;
      }
      if (!vp->get_isPlaying()) {
        VIVIFY_DEBUG("Vivify sync: starting video (prepared={} songTime={})", vp->get_isPrepared(), songTime);
        vp->Play();
      }

      float const drift = static_cast<float>(vp->get_time()) - songTime;
      if (std::abs(drift) > 0.15f) {
        vp->set_time(static_cast<double>(songTime));
      }
      if (heartbeat) {
        PaperLogger.info("Vivify sync heartbeat: videoTime={} songTime={} drift={} prepared={} playing={}",
                         static_cast<float>(vp->get_time()), songTime, drift, vp->get_isPrepared(),
                         vp->get_isPlaying());
      }
    }
  }
}

bool Runtime::IsReduceDebrisEnabled() {

  if (!_reduceDebrisCached) {
    _reduceDebrisCached = true;
    _reduceDebris = false;
    auto* playerDataModel = UnityEngine::Object::FindObjectOfType<GlobalNamespace::PlayerDataModel*>();
    if (IsManagedAlive(playerDataModel)) {
      auto* playerData = playerDataModel->get_playerData();
      if (playerData != nullptr) {
        auto* settings = playerData->get_playerSpecificSettings();
        if (settings != nullptr) {
          _reduceDebris = settings->get_reduceDebris();
        }
      }
    }
    VIVIFY_DEBUG("Vivify: Reduce Debris player option = {} (custom debris {})", _reduceDebris,
                 _reduceDebris ? "disabled" : "enabled");
  }
  return _reduceDebris;
}

void Runtime::SetPauseMenuActive(bool active) {
  if (_currentBeatmapData == nullptr) {
    _pauseMenuActive = false;
    return;
  }
  if (_pauseMenuActive == active) return;
  _pauseMenuActive = active;
  UpdateSlowFrameSection();
  VIVIFY_DEBUG("Vivify pause menu {}: syncedObjects={} secondaryCameras={}",
               active ? "opened" : "closed", _syncedObjects.size(), _secondaryCameras.size());
  if (active) {

    if (_cameraApplier != nullptr && UnityEngine::Object::op_Implicit_bool(_cameraApplier)) {
      _cameraApplier->set_enabled(false);
    }
    for (auto& [name, cam] : _secondaryCameras) {
      if (IsAlive(cam.camera)) cam.camera->set_enabled(false);
    }

    RestoreGlobalProperties();
    RestoreRenderSettings();

    _pausedMaterialAnimations = std::move(_materialAnimations);
    _pausedGlobalAnimations = std::move(_globalAnimations);
    _pausedAnimatorAnimations = std::move(_animatorAnimations);
    _pausedRenderSettingAnimations = std::move(_renderSettingAnimations);
    _materialAnimations.clear();
    _globalAnimations.clear();
    _animatorAnimations.clear();
    _renderSettingAnimations.clear();
    RestoreOverlayRenderState();
    for (auto& synced : _syncedObjects) {
      for (auto* vp : synced.videoPlayers) {
        if (IsManagedAlive(vp) && vp->get_isPlaying()) {
          vp->Pause();
        }
      }
    }
  } else {
    for (auto& [name, cam] : _secondaryCameras) {
      if (IsAlive(cam.camera)) cam.camera->set_enabled(true);
    }

    ReapplyCurrentGlobalProperties();
    ReapplyCurrentRenderSettings();
    _materialAnimations = std::move(_pausedMaterialAnimations);
    _globalAnimations = std::move(_pausedGlobalAnimations);
    _animatorAnimations = std::move(_pausedAnimatorAnimations);
    _renderSettingAnimations = std::move(_pausedRenderSettingAnimations);
    _pausedMaterialAnimations.clear();
    _pausedGlobalAnimations.clear();
    _pausedAnimatorAnimations.clear();
    _pausedRenderSettingAnimations.clear();

    if (!_isResetting) RefreshCameraComponents(true);
  }
}

void Runtime::RefreshMultipassRendering() {
  auto mainCam = UnityEngine::Camera::get_main();
  auto mainCamGO = IsAlive(mainCam.unsafePtr()) ? mainCam->get_gameObject().unsafePtr() : nullptr;
  RefreshMultipassRendering(mainCamGO);
  RefreshLoadedMaterialStereoKeywords();
}

void Runtime::RefreshIsolationSettings() {
  if (GetDisableAllBlits()) {
    if ((!_preEffects.empty() || !_postEffects.empty()) && GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify isolation: clearing active blits pre={} post={}", _preEffects.size(), _postEffects.size());
    }
    _preEffects.clear();
    _postEffects.clear();
    for (auto& bucket : _midEffects) bucket.clear();
    ReleaseMidRenderTextures();
    ReleaseCachedBlitTextures();
    DestroyCameraApplier();
  } else if (GetDisableBeat0FilmgrainBlit()) {
    UpdateBlitEffects();
  }

  if (GetDisableCreateCameraDepth()) {
    if (!_secondaryCameras.empty() && GetVivifyDebugLogging()) {
      PaperLogger.info("Vivify isolation: releasing secondary cameras count={}", _secondaryCameras.size());
    }
    for (auto& [name, camera] : _secondaryCameras) {
      ReleaseSecondaryCameraData(camera);
    }
    _secondaryCameras.clear();
    for (auto it = _cameraProperties.begin(); it != _cameraProperties.end();) {
      if (it->first != kMainCameraId) {
        it = _cameraProperties.erase(it);
      } else {
        it++;
      }
    }
  }

  RefreshCameraComponents(_currentBeatmapData != nullptr && !_isResetting && !_pauseMenuActive);
}

void Runtime::ResetRuntime(std::string_view reason) {
  SetSongPlaying(false);
  if (_perfActive) WritePerfSummary();
  EndPerformanceMode(false);  // the game's display settings back, whatever happened
  ReleaseParticleBakes("");
  if (_currentBeatmapData != nullptr) {
    LogLeakCheck("level end, before Vivify tears down");
    _leakCheckDueAt = UnityEngine::Time::get_realtimeSinceStartup() + 3.0f;
  }
  // Written before anything is torn down, while the numbers are still valid.
  // The song position is included rather than a guess at "quit" vs "beaten":
  // by the time this runs the AudioTimeSyncController is usually already gone,
  // so the honest thing is to report how far the song got and let that say.
  if (_levelReportOpen) {
    std::string outcome(reason);
    if (_lastSongTime >= 0.0f) {
      outcome += fmt::format("  (reached {:.1f}s", _lastSongTime);
      outcome += _lastKnownSongLength > 0.0f ? fmt::format(" of {:.1f}s)", _lastKnownSongLength)
                                             : std::string(")");
    }
    WriteLevelEndReport(outcome);
  }
  EndLevelVRCenterAdjustCounts();

  VIVIFY_DEBUG("Vivify ResetRuntime: livePrefabs={} preloaded={} synced={} secondaryCameras={} declaredTextures={} assets={}",
               _livePrefabs.size(), _instantiatePrefabs.size(), _syncedObjects.size(),
               _secondaryCameras.size(), _declaredTextures.size(), _assets.size());
  _isResetting = true;
  DestroyCameraApplier();
  RestoreGlobalProperties();
  RestoreAllVisualReplacements();
  ClearReplacementPool();
  RestoreOverlayRenderState();
  RestoreMainCameraOriginals();
  RestoreMirrorCameras();
  std::unordered_set<UnityEngine::GameObject*> destroyed;
  for (auto& [id, prefab] : _livePrefabs) {
    if (prefab.gameObject == nullptr) {
      continue;
    }
    for (auto const& track : prefab.tracks) {
      track.UnregisterGameObject(prefab.gameObject);
    }
    if (destroyed.emplace(prefab.gameObject).second) {
      if (UnityEngine::Object::op_Implicit_bool(prefab.gameObject)) {
        UnityEngine::Object::Destroy(prefab.gameObject);
      }
    }
  }
  for (auto& [eventData, instantiate] : _instantiatePrefabs) {
    if (instantiate.instance != nullptr && destroyed.emplace(instantiate.instance).second) {
      if (UnityEngine::Object::op_Implicit_bool(instantiate.instance)) {
        UnityEngine::Object::Destroy(instantiate.instance);
      }
    }
    instantiate.instance = nullptr;
  }
  _livePrefabs.clear();
  _instantiatePrefabs.clear();
  _syncedObjects.clear();
  _blitMaterialValidCache.clear();
  _cachedBlitWidth = 0;
  _cachedBlitHeight = 0;
  _cachedBlitFormat = 0;
  _materialAnimations.clear();
  _globalAnimations.clear();
  _animatorAnimations.clear();
  _pausedMaterialAnimations.clear();
  _pausedGlobalAnimations.clear();
  _pausedAnimatorAnimations.clear();
  _pausedRenderSettingAnimations.clear();
  _savedGlobalProperties.clear();
  _savedGlobalKeywords.clear();
  _currentGlobalProperties.clear();
  _currentGlobalKeywords.clear();
  _repairedMaterials.clear();
  _prefabRenderability.clear();
  _fallbackShadedMaterials.clear();
  _standInShaderNames.clear();
  // A new beatmap gets a fresh chance: the watchdog is per level, not permanent.
  _selfDisabledThisLevel = false;
  _animationTargetsDirty = true;
  _slowFrameStreak = 0;
  _worstFrameMs = 0.0;
  _decodedTextures.clear();
  _assets.clear();
  _assetsByName.clear();
  _supportedShadersByName.clear();
  _gameShadersByName.clear();
  _gameShaderIndexBuilt = false;
  _catchUpAppliedCustomEvents.clear();
  for (auto& [name, dt] : _declaredTextures) ReleaseDeclaredTextureData(dt);
  _declaredTextures.clear();
  for (auto& [name, cam] : _secondaryCameras) ReleaseSecondaryCameraData(cam);
  _secondaryCameras.clear();
  _preEffects.clear();
  _postEffects.clear();
  for (auto& bucket : _midEffects) bucket.clear();
  RemoveMidRenderCommandBuffers();
  ReleaseMidRenderTextures();
  _hasMainDescriptor = false;
  _sceneDepthSkipLogged = false;
  _lastGameplayFrame = {};
  _stallsLogged = 0;
  _blitScreenWarned.clear();
  ReleaseCachedBlitTextures();
  // What the level ran with, before it is put back (Debug logging only).
  if (_currentBeatmapData != nullptr && GetVivifyDebugLogging()) DumpRenderingState("level end");
  RestoreRenderSettings();
  _renderSettingAnimations.clear();
  _savedRenderSettings.clear();
  _currentRenderSettings.clear();
  _assignedPrefabs.clear();
  _activeDebrisPrefabStack.clear();
  _lastCutDebrisPrefabs.clear();
  _cameraProperties.clear();
  _mainCameraPropsDirty = false;

  if (_mainBundle != nullptr && !UnityEngine::Object::op_Implicit_bool(_mainBundle)) {
    ReleaseMainBundle();
    _preloadedBundlePath.clear();
  }
  _currentBeatmapData = nullptr;
  _beatmapAD = nullptr;
  _audioTimeSyncController = nullptr;
  _lastSongTime = -1.0f;
  _lastSyncSongTime = -1.0f;
  _songTimeCacheFrame = -1;
  _songTimeCache = 0.0f;
  _pauseMenuActive = false;
  _reduceDebrisCached = false;
  _reduceDebris = false;
  _inMultiplayerGameplayCached = false;
  _inMultiplayerGameplay = false;
  _isResetting = false;
  // Everything this level kept has been put back or destroyed above.
  _levelPins.ReleaseAll();
}

// ---------------------------------------------------------------------------
// Song performance summary (naomi.4)
//
// Gathered from the first gameplay frame to the end of the level, and written
// to the session log as [PERF] lines when ResetRuntime runs. Per frame it is
// one clock read, the garbage collection count, the song time (already cached
// for the frame) and PerfStats::AddFrame, none of which allocates.

namespace {

// Unity's render counters (naomi.7), read with ProfilerRecorder where this
// build keeps them; a counter that never reads above zero counts as missing.
struct PerfRecorders {
  Unity::Profiling::ProfilerRecorder recorders[5];
  bool valid[5] = {};
  bool started = false;
  bool sawValue = false;
};
PerfRecorders gPerfRecorders;

void StartPerfRecorders() {
  if (gPerfRecorders.started) return;
  gPerfRecorders.started = true;
  gPerfRecorders.sawValue = false;
  static char16_t const* const names[5] = {u"Draw Calls Count", u"Batches Count", u"SetPass Calls Count",
                                           u"Triangles Count", u"Vertices Count"};
  for (int i = 0; i < 5; i++) {
    try {
      gPerfRecorders.recorders[i] = Unity::Profiling::ProfilerRecorder::StartNew(
          Unity::Profiling::ProfilerCategory::get_Render(), StringW(names[i]), 1,
          Unity::Profiling::ProfilerRecorderOptions(0x18 | 0x1));  // Default | StartImmediately
      gPerfRecorders.valid[i] = gPerfRecorders.recorders[i].get_Valid();
    } catch (...) {
      gPerfRecorders.valid[i] = false;
    }
  }
}

void ReadPerfRecorders(int64_t& drawCalls, int64_t& batches, int64_t& setPassCalls, int64_t& triangles,
                       int64_t& vertices) {
  int64_t* const out[5] = {&drawCalls, &batches, &setPassCalls, &triangles, &vertices};
  for (int i = 0; i < 5; i++) {
    *out[i] = -1;
    if (!gPerfRecorders.valid[i]) continue;
    try {
      int64_t const value = gPerfRecorders.recorders[i].get_LastValue();
      if (value > 0) {
        *out[i] = value;
        gPerfRecorders.sawValue = true;
      }
    } catch (...) {
      gPerfRecorders.valid[i] = false;
    }
  }
}

bool PerfRecordersValid() {
  return gPerfRecorders.sawValue;
}

void StopPerfRecorders() {
  for (int i = 0; i < 5; i++) {
    if (!gPerfRecorders.valid[i]) continue;
    try {
      gPerfRecorders.recorders[i].Dispose();
    } catch (...) {
    }
    gPerfRecorders.valid[i] = false;
  }
  gPerfRecorders.started = false;
}

}  // namespace

void Runtime::BeginPerfSummary() {
  float refreshHz = 0.0f;
  try {
    if (auto* display = GlobalNamespace::OVRManager::get_display()) refreshHz = display->get_displayFrequency();
  } catch (...) {
  }
  // OVRManager has no display in some sessions (naomi.6's first Aether play
  // guessed 90 Hz from its frames on a 120 Hz display, and counted missed
  // refreshes against that); the plugin itself always knows.
  if (!(refreshHz > 1.0f)) {
    try {
      refreshHz = GlobalNamespace::OVRPlugin::get_systemDisplayFrequency();
    } catch (...) {
      refreshHz = 0.0f;
    }
  }
  _perf.Reset(refreshHz);
  _perfSections.Reset();
  _perfLogCostStart = VivifyLoggingCost();
  _detailFrames = 0;
  for (double& ms : _detailPartMs) ms = 0.0;
  _detailSlowSection = -1;
  _detailSlowCount = 0;
  _perfVivifyMs = 0.0;
  _perfLiveDue = 0.0f;
  StartPerfRecorders();
  _perfActive = true;
  _perfSkipNext = true;  // the first frame's gap is level loading, not play
  _perfStartWall = std::chrono::steady_clock::now();
  _perfGcStart = _perfGcLast = VivifyGcCount();
  if (!VivifyHeapMB(_perfHeapUsedStartMB, _perfHeapSizeStartMB)) _perfHeapUsedStartMB = _perfHeapSizeStartMB = -1.0;
  VivifySlowFrameReset();
  UpdatePerfPrefabCounts();
  _perfGpuReadings = 0;
  _perfGpuUnit = 1.0f;
  float gpuMs = -1.0f, gpuBusy = -1.0f;
  ReadGpuMetrics(gpuMs, gpuBusy);  // checks once which metrics the plugin reports
  BeginPerformanceMode(refreshHz);
}

// Slow-frame samples count toward the current section while the song plays
// (naomi.7); none are taken in the pause menu or between songs.
void Runtime::UpdateSlowFrameSection() {
  bool const sampling = _perfActive && !_pauseMenuActive && GetSlowFrameSampler() && _perfSections.Count() > 0;
  VivifySlowFrameSection(sampling ? _perfSections.Count() - 1 : -1);
}

void Runtime::RecordPerfFrame(std::chrono::steady_clock::time_point frameStart) {
  if (!_perfActive) {
    BeginPerfSummary();
    _perfLastFrame = frameStart;
    return;
  }
  int const gc = VivifyGcCount();
  if (_pauseMenuActive || _perfSkipNext) {
    // A paused frame, or the first after a pause or the level's start.
    _perf.SkipFrame();
    _perfSkipNext = _pauseMenuActive;
  } else {
    float const frameMs = std::chrono::duration<float, std::milli>(frameStart - _perfLastFrame).count();
    int const gcDelta = gc >= 0 && _perfGcLast >= 0 ? gc - _perfGcLast : -1;
    _perf.AddFrame(frameMs, CurrentSongTime(), gcDelta,
                   [this](char* buffer, size_t size) { WritePerfLabel(buffer, size); });
    float gpuMs = -1.0f, gpuBusy = -1.0f;
    ReadGpuMetrics(gpuMs, gpuBusy);
    _perfSections.AddFrame(frameMs, static_cast<float>(_perfVivifyMs), CurrentSongTime(), gpuMs, gpuBusy,
                           std::max(gcDelta, 0));
    PerformanceModeFrame(frameMs, gpuMs, std::chrono::duration<double>(frameStart - _perfStartWall).count());
    // Every frame into the black box (naomi.10): about 30 s of them survive a crash.
    VivifyBlackBoxRecord(BlackBox::kFrame, {}, frameMs, gpuMs, _perfScale);
    if (GetDetailedLogging()) {
      auto const start = std::chrono::steady_clock::now();
      VivifyLogLine("FRAME", fmt::format("dt={:.1f} gpu={:.1f} busy={:.0f} scale={:.2f} vivify={:.2f} gc={}", frameMs,
                                         gpuMs, gpuBusy, _perfScale, _perfVivifyMs, gcDelta),
                    false, false);
      if (frameMs > 50.0f) LogSlowFrameDetail(frameMs);
      VivifyAddLayer3Cost(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()));
    }
  }
  _perfVivifyMs = 0.0;
  _perfGcLast = gc;
  _perfLastFrame = frameStart;
  _perfEventsSinceFrame = 0;
}

void Runtime::WritePerfLabel(char* buffer, size_t size) const {
  char crumb[96];
  VivifyCopyBreadcrumb(crumb, sizeof(crumb));
  if (_perfLastEventType.empty()) {
    std::snprintf(buffer, size, "%d Vivify event(s) since the previous frame; no Vivify event yet; last breadcrumb: %s",
                  _perfEventsSinceFrame, crumb);
  } else {
    std::snprintf(buffer, size,
                  "%d Vivify event(s) since the previous frame; last Vivify event: %.*s at beat %.2f; last "
                  "breadcrumb: %s",
                  _perfEventsSinceFrame, static_cast<int>(_perfLastEventType.size()), _perfLastEventType.data(),
                  _perfLastEventBeat, crumb);
  }
}

void Runtime::UpdatePerfPrefabCounts() {
  // The section is the set of live prefabs, by id (743 Aether: one section
  // prefab at a time).
  if (_perfActive) {
    std::vector<std::string_view> ids;
    for (auto const& [id, prefab] : _livePrefabs) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    std::string label;
    for (auto id : ids) {
      if (label.size() > 80) {
        label += "+...";
        break;
      }
      if (!label.empty()) label += "+";
      label += id;
    }
    _perfSections.Begin(label.empty() ? std::string("no prefab") : label, CurrentSongTime());
    UpdateSlowFrameSection();
  }
  _perfPrefabsNow = static_cast<int>(_livePrefabs.size());
  _perfRenderersNow = _perfParticlesNow = _perfLightsNow = 0;
  for (auto const& [id, prefab] : _livePrefabs) {
    _perfRenderersNow += prefab.renderers;
    _perfParticlesNow += static_cast<int>(prefab.particleSystems.size());
    _perfLightsNow += prefab.lights;
  }
  _perfPrefabsPeak = std::max(_perfPrefabsPeak, _perfPrefabsNow);
  _perfRenderersPeak = std::max(_perfRenderersPeak, _perfRenderersNow);
  _perfParticlesPeak = std::max(_perfParticlesPeak, _perfParticlesNow);
  _perfLightsPeak = std::max(_perfLightsPeak, _perfLightsNow);
}

// What was live and drawn, every two seconds of song (naomi.7): active
// renderers under the live prefabs and how many were in a camera's view last
// frame, particle systems with particles alive, and Unity's own render
// counters where this build keeps them.
void Runtime::SamplePerfLive() {
  PerfSections::Live live;
  live.renderers = live.visibleRenderers = live.particleSystems = live.particles = 0;
  // Renderer.isVisible has no binding in this game build.
  static auto* isVisible =
      reinterpret_cast<bool (*)(UnityEngine::Object*)>(ResolveEngineIcall("UnityEngine.Renderer::get_isVisible"));
  if (isVisible == nullptr) live.visibleRenderers = -1;
  for (auto const& [id, prefab] : _livePrefabs) {
    if (!IsAlive(prefab.gameObject) || !prefab.gameObject->get_activeInHierarchy()) continue;
    for (auto* renderer : prefab.gameObject->GetComponentsInChildren<UnityEngine::Renderer*>(false)) {
      if (!IsAlive(renderer) || !renderer->get_enabled()) continue;
      live.renderers++;
      if (isVisible != nullptr && isVisible(renderer)) live.visibleRenderers++;
    }
    for (auto* system : prefab.particleSystems) {
      if (!IsAlive(system)) continue;
      int const count = system->get_particleCount();
      if (count <= 0) continue;
      live.particleSystems++;
      live.particles += count;
    }
  }
  ReadPerfRecorders(live.drawCalls, live.batches, live.setPassCalls, live.triangles, live.vertices);
  VivifyDeviceSample const device = VivifyLatestDeviceSample();
  live.residentMB = device.residentMB;
  live.nativeHeapMB = device.nativeHeapMB;
  live.hottestC = device.hottestC;
  live.batteryC = device.batteryC;
  live.gpuMHz = device.gpuMHz;
  _perfSections.AddLive(live);
}

// Detailed mode, a frame over 50 ms (naomi.10): what was in view, by
// material, for the first three such frames of each section, and the render
// state for the first. Nothing here runs with detailed mode off.
void Runtime::LogSlowFrameDetail(float frameMs) {
  int const section = _perfSections.Count() - 1;
  if (section != _detailSlowSection) {
    _detailSlowSection = section;
    _detailSlowCount = 0;
  }
  if (_detailSlowCount >= 3) return;
  _detailSlowCount++;
  static auto* isVisible =
      reinterpret_cast<bool (*)(UnityEngine::Object*)>(ResolveEngineIcall("UnityEngine.Renderer::get_isVisible"));
  std::map<std::string, int> materials;
  int inView = 0;
  for (auto const& [id, prefab] : _livePrefabs) {
    if (!IsAlive(prefab.gameObject) || !prefab.gameObject->get_activeInHierarchy()) continue;
    for (auto* renderer : prefab.gameObject->GetComponentsInChildren<UnityEngine::Renderer*>(false)) {
      if (!IsAlive(renderer) || !renderer->get_enabled()) continue;
      if (isVisible != nullptr && !isVisible(renderer)) continue;
      inView++;
      auto* material = renderer->get_sharedMaterial().unsafePtr();
      std::string name = IsAlive(material) ? ToStdString(material->get_name()) + " (" +
                                                 ShaderNameForLog(material->get_shader().unsafePtr()) + ")"
                                           : std::string("(no material)");
      materials[name]++;
    }
  }
  std::vector<std::pair<int, std::string>> ranked;
  for (auto const& [name, count] : materials) ranked.emplace_back(count, name);
  std::sort(ranked.rbegin(), ranked.rend());
  std::string list;
  for (size_t i = 0; i < ranked.size() && i < 20; i++) {
    list += (list.empty() ? "" : ", ") + ranked[i].second + " x" + std::to_string(ranked[i].first);
  }
  char label[192];
  WritePerfLabel(label, sizeof(label));
  VivifyLogLine("SLOW", fmt::format("frame {:.1f} ms in {}: {} renderer(s) in view{}; by material: {}; {}", frameMs,
                                    _perfSections.Describe(section), inView,
                                    isVisible == nullptr ? " (visibility unknown: all counted)" : "", list, label),
                false, false);
  if (_detailSlowCount == 1) DumpRenderingState("slow frame, detailed logging");
}

void Runtime::WritePerfSummary() {
  _perfActive = false;
  UpdateSlowFrameSection();
  PerfStats::Context context;
  std::filesystem::path const level(_selectedLevelPath);
  context.map = level.filename().string().empty() ? std::string("(unknown map)") : level.filename().string();
  context.wallSeconds = std::chrono::duration<double>(_perfLastFrame - _perfStartWall).count();
  context.gcStart = _perfGcStart;
  context.gcEnd = _perfGcLast;
  context.heapUsedStartMB = _perfHeapUsedStartMB;
  context.heapSizeStartMB = _perfHeapSizeStartMB;
  if (!VivifyHeapMB(context.heapUsedEndMB, context.heapSizeEndMB)) context.heapUsedEndMB = context.heapSizeEndMB = -1;
  context.prefabsEnd = _perfPrefabsNow;
  context.prefabsPeak = _perfPrefabsPeak;
  context.renderersEnd = _perfRenderersNow;
  context.renderersPeak = _perfRenderersPeak;
  context.particleSystemsEnd = _perfParticlesNow;
  context.particleSystemsPeak = _perfParticlesPeak;
  context.lightsEnd = _perfLightsNow;
  context.lightsPeak = _perfLightsPeak;
  for (auto const& line : _perf.Summary(context)) VivifyWriteFlushedLine("[PERF] " + line);
  if (GetDebugCostTest() != kCostTestNone) {
    VivifyWriteFlushedLine(std::string("[PERF] cost test: ") + CostTestName(GetDebugCostTest()) +
                           " switched off for this song (debug setting); the section lines show what is left");
  }
  for (auto const& line : _perfSections.Summary(_perf.BudgetMs())) VivifyWriteFlushedLine("[PERF] " + line);
  if (GetSlowFrameSampler()) {
    for (auto const& line : VivifySlowFrameSummary([this](int section) { return _perfSections.Describe(section); })) {
      VivifyWriteFlushedLine("[PERF] " + line);
    }
  }
  {
    VivifyLogCost const cost = VivifyLoggingCost();
    int const frames = std::max(_perf.Frames(), 1);
    VivifyWriteFlushedLine(fmt::format(
        "[PERF] logging cost on the game's threads, per frame on average: session log {:.4f} ms, black box {:.4f} ms, "
        "detailed mode {:.4f} ms ({})",
        (cost.layer1Ns - _perfLogCostStart.layer1Ns) / 1e6 / frames,
        (cost.layer2Ns - _perfLogCostStart.layer2Ns) / 1e6 / frames,
        (cost.layer3Ns - _perfLogCostStart.layer3Ns) / 1e6 / frames, GetDetailedLogging() ? "on" : "off"));
  }
  EndViewProbeSong();
  EndPerformanceMode(true);
  if (!PerfRecordersValid()) {
    VivifyWriteFlushedLine("[PERF] draw call counts: not available in this game build");
  }
  StopPerfRecorders();
  _perfLastEventType = {};
  _perfEventsSinceFrame = 0;
  _perfPrefabsPeak = _perfRenderersPeak = _perfParticlesPeak = _perfLightsPeak = 0;
}

// ---------------------------------------------------------------------------
// Leak check (naomi.5)
//
// What is alive after a level: Unity objects by type (the whole process, not
// only Vivify's) and what Vivify itself still holds. Run at the end of a level
// and again in the menu a few seconds later, once the game scene has gone; a
// count that climbs from one level's menu check to the next is something not
// being let go. Costs a scan of every loaded object, which is why it runs
// only at those two moments.
void Runtime::LogLeakCheck(char const* when) {
  auto count = [](System::Type* type) -> int {
    try {
      return static_cast<int>(UnityEngine::Resources::FindObjectsOfTypeAll(type).size());
    } catch (...) {
      return -1;
    }
  };
  std::array<int, 5> const counts = {
      count(csTypeOf(UnityEngine::GameObject*)), count(csTypeOf(UnityEngine::Mesh*)),
      count(csTypeOf(UnityEngine::Material*)), count(csTypeOf(UnityEngine::Texture2D*)),
      count(csTypeOf(UnityEngine::RenderTexture*))};
  std::string line = fmt::format(
      "leak check ({}): alive {} GameObject, {} Mesh, {} Material, {} Texture2D, {} RenderTexture; Vivify holds "
      "{} decoded texture(s), {} rebuilt mesh(es), {} stand-in copy(ies), {} declared texture(s), {} camera(s), "
      "{} live prefab(s), {} preloaded prefab(s), {} synced object(s), {} particle bake(s) ({} baked)",
      when, counts[0], counts[1], counts[2], counts[3], counts[4], _decodedTextureObjects.size(),
      _emulationMeshes.size(), _emulationStandIns.size(), _declaredTextures.size(), _secondaryCameras.size(),
      _livePrefabs.size(), _instantiatePrefabs.size(), _syncedObjects.size(), _particleBakes.size(),
      std::count_if(_particleBakes.begin(), _particleBakes.end(),
                    [](auto const& bake) { return bake && bake->mesh; }));
  // The menu checks are the comparable ones: the game scene is gone each time.
  bool const menu = std::string_view(when).rfind("menu", 0) == 0;
  if (menu && _lastMenuLeakCounts[0] >= 0) {
    line += fmt::format("; since the previous level's menu check {:+d} GameObject, {:+d} Mesh, {:+d} Material, "
                        "{:+d} Texture2D, {:+d} RenderTexture",
                        counts[0] - _lastMenuLeakCounts[0], counts[1] - _lastMenuLeakCounts[1],
                        counts[2] - _lastMenuLeakCounts[2], counts[3] - _lastMenuLeakCounts[3],
                        counts[4] - _lastMenuLeakCounts[4]);
  }
  if (menu) _lastMenuLeakCounts = counts;
  VivifyBreadcrumb(line);
  if (menu) LogMenuMeshGrowth();
}

// Which meshes the menu check found more of than the previous level's
// (naomi.10-beta.5). naomi.9 counted +131 and +59 Meshes after its first two
// Aether plays, and a count alone does not say whose they are; a few hundred
// names, once per level in the menu.
void Runtime::LogMenuMeshGrowth() {
  std::map<std::string, int> names;
  try {
    auto meshes = UnityEngine::Resources::FindObjectsOfTypeAll(csTypeOf(UnityEngine::Mesh*));
    for (auto entry : meshes) {
      auto* object = entry.unsafePtr();
      if (!IsManagedAlive(object)) continue;
      std::string name = ToStdString(object->get_name());
      if (name.empty()) name = "(no name)";
      names[name]++;
    }
  } catch (...) {
    return;
  }
  if (_haveMenuMeshNames) {
    std::vector<std::pair<int, std::string>> grew;
    for (auto const& [name, count] : names) {
      auto it = _lastMenuMeshNames.find(name);
      int const before = it == _lastMenuMeshNames.end() ? 0 : it->second;
      if (count > before) grew.emplace_back(count - before, name);
    }
    if (!grew.empty()) {
      std::sort(grew.begin(), grew.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
      std::string list;
      for (size_t i = 0; i < grew.size() && i < 10; i++) {
        list += fmt::format("{}'{}' +{}", i ? ", " : "", grew[i].second.substr(0, 60), grew[i].first);
      }
      if (grew.size() > 10) list += fmt::format(" and {} more name(s)", grew.size() - 10);
      PaperLogger.info("Vivify leak check (menu): meshes there are more of than after the previous level, by name: {}",
                       list);
    }
  }
  _lastMenuMeshNames = std::move(names);
  _haveMenuMeshNames = true;
}

}
