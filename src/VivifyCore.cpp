#include "VivifyRuntimeInternal.hpp"
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
      ResetRuntime("level ended (quit or finished)");

      _activeSabers.clear();
      return;
    }
    if (_currentBeatmapData == nullptr) {
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
    UpdateMaterialAnimations();
    UpdateGlobalAnimations();
    UpdateAnimatorAnimations();
    UpdatePrefabAnimationSpeed();
    UpdateBlitEffects();
    UpdateRenderSettingAnimations();
    RefreshCameraComponents(true);
    DetectSongRestart();
    UpdateSaberReplacementColors();
    SyncReplacementTintColors();
  } catch (std::exception const& ex) {
    LogThrottledUpdateError(ex.what());
  } catch (...) {
    LogThrottledUpdateError("non-std exception");
  }

  double const frameMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
  _worstFrameMs = std::max(_worstFrameMs, frameMs);
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
  ReleaseCachedBlitTextures();
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
}

// ---------------------------------------------------------------------------
// Song performance summary (naomi.4)
//
// Gathered from the first gameplay frame to the end of the level, and written
// to the session log as [PERF] lines when ResetRuntime runs. Per frame it is
// one clock read, the garbage collection count, the song time (already cached
// for the frame) and PerfStats::AddFrame, none of which allocates.

void Runtime::BeginPerfSummary() {
  float refreshHz = 0.0f;
  try {
    if (auto* display = GlobalNamespace::OVRManager::get_display()) refreshHz = display->get_displayFrequency();
  } catch (...) {
  }
  _perf.Reset(refreshHz);
  _perfActive = true;
  _perfSkipNext = true;  // the first frame's gap is level loading, not play
  _perfStartWall = std::chrono::steady_clock::now();
  _perfGcStart = _perfGcLast = VivifyGcCount();
  if (!VivifyHeapMB(_perfHeapUsedStartMB, _perfHeapSizeStartMB)) _perfHeapUsedStartMB = _perfHeapSizeStartMB = -1.0;
  UpdatePerfPrefabCounts();
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
  }
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

void Runtime::WritePerfSummary() {
  _perfActive = false;
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
      "{} live prefab(s), {} preloaded prefab(s), {} synced object(s)",
      when, counts[0], counts[1], counts[2], counts[3], counts[4], _decodedTextureObjects.size(),
      _emulationMeshes.size(), _emulationStandIns.size(), _declaredTextures.size(), _secondaryCameras.size(),
      _livePrefabs.size(), _instantiatePrefabs.size(), _syncedObjects.size());
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
}

}
