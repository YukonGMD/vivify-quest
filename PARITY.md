# Vivify PC ↔ Quest parity

An inventory of everything PC Vivify does, and where the Quest port stands on each item.

- **PC reference:** `~/projects/Vivify` at `f83aa3c` (C#, Beat Saber 1.29–1.40). Built from its source; its README only links to the external wiki.
- **Quest port:** this repo, branch `visual-fixes` at `1dc0564b`.
- **Method:** every row comes from reading both codebases. Nothing here was checked on a headset. Rows marked *(unverified)* are inferences that a test map should confirm.

**Status legend**

| Status | Meaning |
|---|---|
| implemented | Same behaviour as PC, as far as the code shows |
| partial | Present, but some PC behaviour is missing or different |
| missing | PC does it; Quest does not, and nothing stops Quest from doing it |
| platform-limited | Can't match PC on Quest hardware or the Quest game build (see [Platform limits](#platform-limits)) |
| n/a | Exists only because of a PC-only mod or setting |

Paths: PC paths are relative to `~/projects/Vivify/Vivify/`, Quest paths to this repo.

---

## 1. Custom events

PC registers 11 events (`VivifyController.cs`, `Installers/VivifyPlayerInstaller.cs`) and parses them in `HeckImplementation/CustomDataDeserializer.cs`. Quest dispatches them in `Runtime::DispatchEvent` (`src/VivifyEvents.cpp`).

| Event | PC file | Quest status | Quest function | Test map would show |
|---|---|---|---|---|
| `Blit` | `Events/ApplyPostProcessing.cs` | partial (see §6) | `Runtime::HandleBlit` (`src/VivifyPostProcessing.cpp`) | A full-screen colour-invert material for 4 beats, then the normal view |
| `CreateCamera` | `Events/DeclareCullingTexture.cs` | partial (see §6) | `Runtime::HandleCreateCamera` | A second camera's view blitted onto `_Main` |
| `CreateScreenTexture` | `Events/DeclareRenderTexture.cs` | implemented (fixed in `f5c669a9`) | `Runtime::HandleCreateScreenTexture`, `CreateDeclaredTexture` | Blit `_Main` → texture → `_Main` with a distortion material: a distorted but complete image |
| `DestroyObject` | `Events/DestroyPrefab.cs` | implemented | `Runtime::DestroyObjects` (`src/VivifyObjectPrefabs.cpp`) | A prefab, a camera and a screen texture each vanish at their destroy beat |
| `InstantiatePrefab` | `Events/InstantiatePrefab.cs` | partial (left-handed, see §3) | `Runtime::PreloadInstantiatePrefabs`, `Runtime::InstantiatePrefab` | A cube appears at its beat at `position`/`rotation`/`scale` |
| `SetMaterialProperty` | `Events/SetMaterialProperty.cs` | implemented | `Runtime::HandleSetMaterialProperty` (`src/VivifyProperties.cpp`) | A material's `_Color` fades red→blue over 8 beats |
| `SetAnimatorProperty` | `Events/SetAnimatorProperty.cs` | implemented (Integer differs, §4) | `Runtime::HandleSetAnimatorProperty` | A prefab's animator Bool/Float/Integer/Trigger drive state changes |
| `SetGlobalProperty` | `Events/SetGlobalProperty.cs` | implemented | `Runtime::HandleSetGlobalProperty` | A global `_Fade` float animating every material that reads it |
| `SetCameraProperty` | `Events/SetCameraProperty.cs` | partial (mirror cameras, §6) | `Runtime::HandleSetCameraProperty` | `_Main` clear flags SolidColor with a red background; culling whitelist |
| `SetRenderingSettings` | `Events/SetRenderingSettings.cs`, `Events/RenderingCapturedSettings.cs` | partial (§5) | `Runtime::HandleSetRenderingSettings` | Fog colour/density animating; skybox material swap |
| `AssignObjectPrefab` | `Events/AssignObjectPrefab.cs` | partial (§3) | `Runtime::HandleAssignObjectPrefab` | Notes on track `A` replaced by a custom model; custom saber and trail |

Quest also accepts the aliases `PostProcess`, `PostProcessing` and `ScreenEffect` for `Blit` (`IsPostProcessingEvent`, `include/VivifyTypes.hpp`). These are extras that PC maps never use.

**Event timing:** PC events fire through Heck's callbacks. Quest additionally replays every Vivify event at or before the first event's time when it prepares the beatmap (`Runtime::ApplyMissedVivifyEvents`). The result is equivalent.

## 2. Event and beatmap data fields

| Field | PC file | Quest status | Quest code | Notes / test |
|---|---|---|---|---|
| `duration`, `easing` on all animated events | `CustomDataProperties.cs`, `CustomDataTypes.cs` | implemented | `DurationBeatsToSeconds`, `ParseEasing` | Both convert beats to seconds with the current BPM. Quest returns 0 seconds if no BPM controller exists yet. |
| Point definitions (`value: [[…, t]]` or a named point definition) | Heck `GetPointData` | implemented | `Runtime::MakePointDefinition` (Tracks `PointDefinitionW`) | A named point definition from `pointDefinitions` animates a material |
| `track` on `InstantiatePrefab` | `InstantiatePrefabData` | implemented | `ReadTracks` + `Tracks::GameObjectTrackController::HandleTrackData` | Animating the track's `position` moves the prefab |
| `position`/`localPosition`/`rotation`/`localRotation`/`scale` on `InstantiatePrefab` | Heck `TransformData` | implemented | `Tracks::TransformData::Apply` | v2 maps scale positions by 0.6 in both |
| `id` on `InstantiatePrefab` (unique) | `CustomDataDeserializer.cs` (throws on a duplicate) | implemented | `PreloadInstantiatePrefabs` drops duplicates, with a "PC parity" log line | A second prefab with the same id never appears |
| `asset`, `priority`, `pass`, `source`, `destination` (string or array), `order`, `properties` on `Blit` | `ApplyPostProcessingData` | implemented | `HandleBlit`, `ParsePostProcessingOrder` | Quest also reads `material`/`effect`/… synonyms and `clear`/`enabled` (extras) |
| `id`, `texture`, `depthTexture`, `properties` on `CreateCamera` | `CreateCameraData` | implemented | `HandleCreateCamera` | — |
| `id`, `xRatio`, `yRatio`, `width`, `height`, `colorFormat`, `filterMode` on `CreateScreenTexture` | `CreateScreenTextureData` | implemented | `HandleCreateScreenTexture` | An unsupported format is downgraded (`SupportedRenderTextureFormat`); PC only warns |
| `depthTextureMode`, `clearFlags`, `backgroundColor`, `culling{track, whitelist}`, `bloomPrePass`, `mainEffect` camera properties | `CameraProperty` in `CustomDataProperties.cs` | implemented (parse) | `ParseCameraPropertyData` | PC accepts `depthTextureMode` only as an array; Quest accepts a string too |
| Material property `type`: Texture, Color, Float, Vector, Keyword | `MaterialPropertyType` | implemented | `ParseMaterialProperty`, `ApplyMaterialProperty` | — |
| Material property `type`: Int | PC enum has it but throws "not supported" | Quest extra | `MaterialPropertyKind::Int` | — |
| Material property `type`: FloatArray, VectorArray | PC enum, throws | not implemented in either | — | Parity |
| Animator property `type`: Bool, Float, Integer, Trigger | `AnimatorPropertyType` | implemented | `ParseAnimatorProperty` | See §4 for the Integer difference |
| `track` on notes, bombs and chains (`VivifyObjectData`) | `CustomDataTypes.cs` `VivifyObjectData` | implemented | `AssignmentMatchesTracks` (TracksAD) | — |
| `Info.dat` `_customData._assetBundle._windows2021` / `_windows2019` checksum | `Managers/AssetBundleManager.cs` | partial | `ReadBundleChecksumFromInfoDat` (`src/VivifyAssets.cpp`) | PC passes it as a CRC to `LoadFromFile` and refuses a mismatched bundle. Quest uses it only to download, and loads without a CRC check. |
| `Vivify` requirement / capability | `HeckImplementation/FeaturesModule.cs` | implemented | `SongCore::API::Capabilities::RegisterCapability(kCapability)` | — |

## 3. Assets, prefabs and object replacement

| Feature | PC file | Quest status | Quest code | Test map would show |
|---|---|---|---|---|
| Load `bundleWindows2021.vivify` with CRC; all assets loaded up front | `Managers/AssetBundleManager.cs` | platform-limited / partial | `PreloadBundle`, `LoadMainBundle`, `CacheBundleAssets` | Quest loads the Android bundle, or a PC bundle converted on device. No CRC check. |
| Bundle download from `repo.totalbs.dev` after the user opts in | `Controllers/AssetBundleDownloadViewController.cs`, `Config.cs` (`AllowDownload`, `BundleRepository`) | partial | `DownloadBundleTo` | Quest downloads automatically from a hard-coded URL, with no consent screen and no configurable repository |
| Prefab instances made at load, activated at their beat | `Events/InstantiatePrefab.cs` `Initialize` | implemented | `PreloadInstantiatePrefabs` | — |
| Left-handed mode: prefabs parented under an X-mirrored root | `Events/InstantiatePrefab.cs` (`_mirroredParent`) | **missing** | — | With Left Handed on: on PC, environment prefabs mirror with the notes; on Quest they don't |
| Animator and particle speed follow song speed | `Controllers/Sync/{SyncController,AnimatorSyncController,ParticleSystemSyncController,SyncExtensions}.cs` | implemented | `Runtime::UpdatePrefabAnimationSpeed`, `RegisterSyncedObject` (`src/VivifyCore.cpp`) | Pausing freezes the animators; 1.5× speed plays them at 1.5× |
| Animator `updateMode` forced to Normal | `AnimatorSyncController.Awake` | missing | — | Only matters for prefabs exported with UnscaledTime/AnimatePhysics |
| VideoPlayer sync | `Controllers/Sync/VideoPlayerSyncController.cs` | partial | `Runtime::UpdateSyncedObjects` | PC: only `playOnAwake` players are synced; prepares before play; `playbackSpeed` = song time scale; re-seeks over 0.2 s of drift. Quest: syncs every player, never sets `playbackSpeed`, re-seeks over 0.15 s. At a speed modifier, Quest's video keeps re-seeking (stutters). |
| AudioSource sync | `Controllers/Sync/AudioSourceSyncController.cs` (commented out) | n/a | — | Neither syncs audio |
| `DestroyObject` lookup order: camera, then texture, then prefab | `Events/DestroyPrefab.cs` | implemented | `DestroyObjects` | Quest also forgets the camera's stored properties (PC keeps them), so re-creating a camera with the same id loses earlier `SetCameraProperty` values |
| Practice rewind (Heck `ReLoader`): despawn and re-init | `Managers/PrefabManager.cs`, `CameraEffectApplier.cs`, etc. | partial | `Runtime::DetectSongRestart` → `ResetRuntime`, then replay on the next event | Seeking back in practice clears everything until the next Vivify event fires |
| `AssignObjectPrefab` `colorNotes` / `bombNotes` / `burstSliders` / `burstSliderElements` `asset` | `Events/AssignObjectPrefab.cs`, `ObjectPrefab/Managers/NotePrefabManager.cs` | implemented | `HandleAssignObjectPrefab`, `ApplyNotePrefabFor`, `ReplaceNoteVisuals` | Notes on the track show the custom model, coloured with the note colour |
| `anyDirectionAsset` (dot notes) | same | implemented | `AssignedPrefabKind::AnyDirectionObject` | Dot notes use their own model |
| `debrisAsset` (note and chain debris) | `ObjectPrefab/Managers/DebrisPrefabManager.cs` | implemented (+ bombs, an extra) | `ReplaceDebrisVisuals`, `PushActiveDebrisPrefabs` (hook on `NoteCutCoreEffectsSpawner`) | Cut halves show the custom debris model |
| `loadMode` Single / Additive; `asset: null` restores the original | `ObjectPrefab/Collections/PrefabCollections.cs`, `PrefabList.cs` | partial | `ClearAssignedPrefabs`, `ShouldHideOriginal` | Additive keeps the original note visible, as on PC. Difference: on PC, `null` in Additive mode adds "original visible" without removing other prefabs; Quest clears them all. |
| Assignment with no `track` | `AssignObjectPrefab.cs` (logs "No track defined", assigns nothing) | **differs** | `AssignmentMatchesTracks` returns true for empty tracks | A track-less `colorNotes` assignment replaces nothing on PC and every note on Quest |
| Re-skin notes already on screen when an assignment changes | `NotePrefabManager.On*PrefabsChanged` → `RefreshObjects` over the game's active pool items | implemented (fixed in this branch) | `RefreshActiveNoteVisuals` walks the four note pools of `BasicBeatmapObjectManager` (captured by the `BasicBeatmapObjectManager.Init` hook) as well as existing replacements | Assign a prefab mid-song while notes are in flight: notes already on screen switch to it immediately, on both |
| Prefab pooling and pre-warm (10 per asset) | `ObjectPrefab/Pools/PrefabPool.cs`, `BeatmapObjectPrefabManager.PrewarmGameObjectPrefabPool` | missing (performance only, see `PERF.md`) | `InstantiateReplacementPrefab` instantiates and destroys each time | Visually identical |
| Animator `Rebind()` + `Update(0.01)` on spawn | `PrefabPool.Spawn` | implemented | `InstantiateReplacementPrefab` | — |
| Hide original renderers; borrow the note's `MaterialPropertyBlockController` so prefabs get the note colour | `ObjectPrefab/Hijackers/MpbControllerHijacker.cs` | implemented | `ApplyReplacementRenderersToMaterialBlock`, `DisableOriginalRenderers` | Quest also keeps the original when the prefab can't draw (`ReplacementCanRender`) |
| `saber` `asset` with `type` Left/Right/Both | `ObjectPrefab/Managers/SaberPrefabManager.cs`, `Hijackers/SaberModelControllerHijacker.cs` | implemented *(parenting differs)* | `ApplySaberVisuals`, `TrackSaberModel` | PC parents to the `Saber` transform, Quest to the `SaberModelController` transform; normally identical *(unverified)* |
| Saber prefab colour follows the saber colour | `ObjectPrefab/SetSaberManyColor.cs` (patches `SetSaberGlowColor.SetColors` and SiraUtil `SetColors`) | partial *(unverified)* | `UpdateSaberReplacementColors` polls `ColorManager.ColorForSaberType` | A Chroma/SiraUtil runtime saber-colour change updates the custom saber on PC; Quest polls the colour scheme, which may not see it |
| `trailAsset` + `trailTopPos`/`trailBottomPos`/`trailDuration`/`trailSamplingFrequency`/`trailGranularity` | `ObjectPrefab/FollowedSaberTrail.cs`, `Pools/TrailPool.cs`, `Hijackers/SaberTrailHijacker.cs` | implemented | `ApplySaberTrailVisuals`, `FollowedSaberTrail` (`src/VivifyComponents.cpp`) | Quest clamps duration ≤ 2 s, frequency ≤ 120, granularity ≤ 240 |
| Keep trail renderers alive across `SaberTrail` enable/disable | `HarmonyPatches/SaberTrailRendererEnabler.cs` | **missing** | — | After pausing or hiding a saber, PC resets trail data; Quest may show one frame of stretched trail *(unverified)* |
| CustomSabersLite / Saber Factory model and trail hiding | `SaberModelControllerHijacker.LiteModelContract`, `SaberTrailHijacker` | n/a | Quest hides every renderer under the `SaberModelController` | Qosmetics trails are not explicitly handled *(unverified)* |

## 4. Materials, animators and global properties

| Feature | PC file | Quest status | Quest code | Test map would show |
|---|---|---|---|---|
| Flat Color/Vector `[r,g,b,a]`, and malformed `[r,g,b]` | `CustomDataProperties.cs` `CreateMaterialProperty` (every list → Heck `GetPointData<Vector4>`; the literal branch in `SetMaterialProperty.cs` is dead code) | implemented (fixed in this branch) | `IsMalformedVectorShorthand`, `ParseMaterialProperties` (`src/VivifyProperties.cpp`); `HandleBlit` | `[1,0,0,1]` is red on both. `[1,0,0]`: Heck makes it `[1,0,0,0]` (four numbers where five are needed), throws, and PC skips the whole event. tracks-rs read it as `(1,0,0,0)`, a zero-alpha colour. Quest now skips the whole `SetMaterialProperty`/`SetGlobalProperty`/`Blit` event too, and logs which property was malformed. |
| Animated property: apply over `duration` with `easing` | `AnimatePropertyCoroutine` | implemented | `QueueMaterialPropertyAnimation`, `UpdateMaterialAnimations` | PC's coroutine stops without writing the final value; Quest writes progress 1 at the end. Quest's end state is the exact final value. |
| Event already in the past (`start + duration < songTime`) applies the final value | all property events | implemented | `completed` checks | — |
| Texture property (asset name) | `SetMaterialProperty.cs` | implemented | `ApplyMaterialProperty` Texture | — |
| Keyword (bool or animated, on at ≥ 1) | same | implemented | `SetMaterialKeyword` + `ApplyStereoKeywords` | — |
| Global properties restored at level end | `SetGlobalProperty.Initialize/Dispose` | implemented | `SaveOriginalGlobalProperty`, `RestoreGlobalProperties` | Quest also restores them while paused and re-applies them on resume (Quest-only) |
| Animator Integer, static value | `SetAnimatorProperty.cs` calls `SetFloat` (a PC bug: does nothing on an int parameter) | differs | `ApplyAnimatorProperty` calls `SetInteger` | A static Integer works on Quest but not on PC. Animated Integer is `SetInteger` on both. |
| Animators targeted by `SetAnimatorProperty` | `InstantiatedPrefab.Animators` = `GetComponentsInChildren<Animator>()` (active only) | superset | `LivePrefab::animators` (includes inactive) | — |

## 5. Rendering settings (`SetRenderingSettings`)

| Feature | PC file | Quest status | Quest code | Test map would show |
|---|---|---|---|---|
| `renderSettings`: ambient*, defaultReflection*, flare*, fog*, halo, reflection*, subtractiveShadowColor | `SetRenderingSettings.cs` | implemented | `kRenderSettingTypes`, `RawApplyRenderSetting*` (`src/VivifyProperties.cpp`) | Fog colour lerps over 4 beats |
| `renderSettings.skybox` (material asset), `sun` (directional Light in a prefab, by id) | `RenderMaterialCapturedSetting`, `RenderLightCapturedSetting` | implemented | `ApplyRenderSettingObject` | Skybox swaps at its beat |
| `DynamicGI.UpdateEnvironment()` after each static setting | `StructSettingHandler.Handle`, `ClassSettingHandler.Handle` | **missing** | — | After a skybox or ambient change with `ambientMode: Skybox`, PC relights; on Quest the ambient/reflection keeps the old skybox |
| `qualitySettings`: anisotropicFiltering, antiAliasing, pixelLightCount, realtimeReflectionProbes, softParticles | same | implemented | Internal-call setters (`ResolveSettingIcall`) | — |
| `qualitySettings.shadow*` (shadows, resolution, projection, cascades, distance, near plane, shadowmask) | same | platform-limited (opt-in) | Skipped unless **Map Realtime Shadows** is on | Shadowed variants under multiview draw fully dark on Quest |
| `xrSettings.useOcclusionMesh` | `Extras/XRSettingsSetter.cs` | implemented | Internal call | — |
| Every setting restored at level end | `Capture`/`Reset` | implemented | `SaveRenderSetting`, `RestoreRenderSettings` | — |
| Extra keys (lodBias, maximumLODLevel, particleRaycastBudget, skinWeights, softVegetation, billboardsFaceCameraPosition, occlusionMaskScale; enum names as strings) | not on PC | Quest extra | — | — |

## 6. Cameras and post-processing

| Feature | PC file | Quest status | Quest code | Test map would show |
|---|---|---|---|---|
| Blit orders BeforeSkybox…AfterAlpha (mid-render command buffers) | `PostProcessing/PostProcessingController.OnPreRender` | implemented | `AddMidRenderCommandBuffers`, `BuildMidRenderCommandBuffer` | A blit at AfterOpaque tints opaque geometry but not arcs/trails drawn after |
| Blit orders BeforeMainEffect / AfterMainEffect around the game's main effect | `PostProcessingController.OnRenderImage`, `AddComponentsToCamera.StopRenderImage` | **partial** | `CameraApplier::OnRenderImage` (`src/VivifyComponents.cpp`); `hasMainEffect` is always false, so both lists run after the game's `ImageEffectController` | Only matters if the game's main effect (bloom) runs on Quest: BeforeMainEffect effects then land after bloom |
| Sources `_Main` / screen texture / camera; targets `_Main` / texture(s); self-blit through a temporary | `PostProcessingController.RenderImage` | implemented | `ApplyBlits` | — |
| Priority order; one-frame blits (duration 0); expiry | `MaterialData`, `SortedListExtensions.cs`, `KillPostProcessingCoroutine` | implemented | `AddOrUpdateBlitEffect`, `UpdateBlitEffects` | Quest merges identical blits (PC applies duplicates twice) and caps the list at 96 |
| Blit `properties` animate the material | `ApplyPostProcessing` → `SetMaterialProperties` | implemented | `HandleBlit` | — |
| Screen texture created from the camera descriptor, per eye | `PostProcessingController.CreateDeclaredTextures` | implemented (fixed in `f5c669a9`) | `CreateDeclaredTexture`, `CacheMainRenderDescriptor` | — |
| Secondary camera: copies main camera FOV/clip/culling, matches matrices in `OnPreCull`, renders at depth −1 | `PostProcessing/SecondaryCameraController.cs` | implemented | `SecondaryCameraController::OnPreCull` | — |
| Secondary colour texture per eye, bound as a global before main render | `SecondaryCameraController.OnRenderImage`, `PostProcessingController.OnPreRender` | implemented | `SecondaryCameraController::OnRenderImage`, `BindSecondaryCameraTextures` | — |
| Secondary depth texture (`depthTexture`) via the DepthBlit shader (array-slice aware) | `Managers/DepthShaderManager.cs`, `Resources/DepthBlit*` | partial *(unverified)* | Default blit from `BuiltinRenderTextureType::Depth` into an RFloat texture | A depth-fog map: correct per-eye depth on PC; on Quest, verify that each eye gets its own slice |
| Secondary camera `mainEffect` | `MainEffectRenderer.cs` | implemented | `ApplySecondaryCameraMainEffects` | — |
| Secondary camera gets a copy of `BloomPrePass` (`LateBloomPrePass`) | `PostProcessingController.CreateCamera`, `PostProcessing/LateBloomPrePass.cs` | **missing** *(unverified whether the Quest camera has a BloomPrePass)* | `HandleCreateCamera` uses `Camera.CopyFrom` only | Glowing lights in a camera capture: glow on PC; none on Quest. `bloomPrePass` on a secondary camera does nothing. |
| Culling tracks / whitelist (layer 22) | `CullingCameraController.cs`, `TrackGameObject/CullingTextureTracker.cs` | implemented | `CullingCameraController::CullingPreCull` | A whitelist camera renders only the tracked prefab |
| Culling picks up renderers added to tracked objects later | `RendererController.cs` (watches child changes) | implemented (re-scanned every frame) | `CullingPreCull` | — |
| Camera properties on `_Main`: depthTextureMode, clearFlags, backgroundColor, culling, bloomPrePass | `Controllers/CameraPropertyController.cs`, `Managers/CameraPropertyManager.cs` | implemented | `ApplyCameraProperties`, `ApplyCameraGameObjectProperties` | — |
| Mirror cameras get culling and `_Main` camera properties | `HarmonyPatches/AddComponentsToCamera.AddMirrorComponents` | implemented (fixed in this branch) | `ApplyMainPropertiesToMirrorCamera` (from the `MirrorRendererSO.CreateOrUpdateMirrorCamera` hook and from `HandleSetCameraProperty`), `RestoreMirrorCameras` | A culling whitelist on `_Main` with a mirror in the scene: the mirror is culled the same way as the main view, on both |
| Mirror texture cache per camera | `HarmonyPatches/IndexMirrorByHash.cs` | implemented (fixed in `fe7caa2b`) | `MirrorRendererSO_*` hooks (`src/VivifyHooks.cpp`) | Mirrors stay world-fixed with a CreateCamera present |
| `_StereoActiveEye` global int | `Controllers/MultipassKeywordController.cs` | implemented | `MultipassKeywordController::OnPreRender` | — |
| `MULTIPASS_ENABLED` keyword under OpenXR multi-pass | same | platform-limited | Never enabled (Quest renders single-pass multiview) | — |
| Camera pre-warm (1 camera if the map creates any) | `CameraEffectApplier` | missing (performance only) | — | — |
| Camera2 integration, `MaxCamera2Cams` setting | `HarmonyPatches/Camera2Priority*.cs`, `Settings/SettingsMenu.cs` | n/a | — | No Camera2 on Quest |

## 7. Other game patches

| Feature | PC file | Quest status | Quest code |
|---|---|---|---|
| Move `AlwaysVisibleQuad` out of view | `HarmonyPatches/MoveAlwaysVisibleQuad.cs` (y = −10) | implemented (y = −1000, Vivify maps only) | `AlwaysVisibleQuad_OnEnable` hook |
| Vivify only active when the map requires it | `FeaturesModule` condition | implemented | `_selectedMapHasVivifyRequirement` |

## 8. Settings

| PC setting | PC file | Quest |
|---|---|---|
| `MaxCamera2Cams` (0–20, default 2) | `Config.cs`, `Resources/Settings.bsml` | n/a |
| `AllowDownload` (set by the consent screen) | `Config.cs`, `AssetBundleDownloadViewController.cs` | missing: Quest downloads without asking |
| `BundleRepository` | `Config.cs` | missing: hard-coded URL in `DownloadBundleTo` |

Quest adds about 25 settings with no PC equivalent (`src/main.cpp`): debug logging, isolation toggles (Disable All Blits / CreateCamera Depth / Custom Note Visuals / VR Center Adjust), conversion and shader-translation options, stand-in shading, scene depth, AudioLink, realtime shadows, Effect Resolution, Offscreen and Far culling, Crash Guard, and maintenance buttons.

---

## Platform limits

These come from Quest hardware or the Quest build of the game, not from porting logic. Logic fixes can't remove them; workarounds can only soften them.

1. **Geometry shaders.** Multiview rendering forbids a geometry stage. Wireframe notes, exploding-triangle effects and geometry-shader grass in converted Windows bundles (743 Aether, Through The Screen) can't run as authored. "Draw Geometry-Shader Effects (experimental)" draws them without that stage instead.
2. **DirectX-only shaders in Windows bundles.** PC-only maps ship DXBC programs. Quest translates them to GLSL ES (`src/VivifyDxbc.cpp`). Anything untranslatable becomes a stand-in shader. A stand-in on a full-screen blit material is skipped (`CanUseBlitMaterial`), so that effect is simply missing.
3. **Unity 2019 single-pass double-wide shader variants** in Windows bundles assume `unity_StereoScaleOffset` layouts that Quest's multiview never sets.
4. **BC/DXT texture formats.** These are decoded to RGBA32 on the CPU when the texture is readable (`DecodeUnsupportedBundleTextures`, 8 s budget). Unreadable ones draw untextured.
5. **Realtime shadows under multiview** draw shadow-receiving shaders fully dark. They are off unless the player opts in.
6. **GPU performance.** Quest 3S is a tile-based mobile GPU. These all cost proportionally far more than on a PC: a full scene re-render per CreateCamera, every full-screen blit, `OnRenderImage` compositing, and the scene-depth pre-pass. Heavy maps may need Effect Resolution below 100% or Far Culling. See `PERF.md`.
7. **PC-only mod integrations.** Camera2, CustomSabersLite, Saber Factory and OpenXR multi-pass have no Quest equivalents.
8. **Converted-map instancing.** GPU instancing is turned off on converted materials (`RepairMaterialShader`), because translated instanced variants misplace instances. This is correct, but costs draw calls.

---

## Logic gaps, ranked

Ordered by expected effect on gameplay and visuals across real maps. Items 1–3 are already fixed on this branch and are listed for completeness.

| # | Gap | Effect | Where to fix |
|---|---|---|---|
| ✓ | Mirror texture shared between Vivify cameras and the main camera | Mirrors followed the headset | Fixed `fe7caa2b` |
| ✓ | Screen textures were 2D, not eye-array textures | Empty or flat screen-texture effects (Through The Screen, possibly Get Your Wish) | Fixed `f5c669a9` |
| ✓ | Offscreen Culling (Quest-only) on by default | Animated particles and scenery never appeared | Made opt-in `1dc0564b` |
| ✓ | Active notes not re-skinned when an assignment first applies mid-song | Fixed on this branch: `RefreshActiveNoteVisuals` walks the game's active note pools | — |
| 2 | Secondary depth texture copied without PC's array-slice DepthBlit | Depth-driven effects (fog, raymarchers, intersections) may be wrong in one eye *(unverified)* | `SecondaryCameraController::OnRenderImage` |
| 3 | Left-handed mode doesn't mirror prefabs | Environments are mirrored relative to the notes for left-handed players | `Runtime::InstantiatePrefab`: parent under a −X-scaled root when left-handed |
| ✓ | Mirror cameras didn't get culling / `_Main` camera properties | Fixed on this branch: mirror cameras take the `_Main` properties and are restored at level end | — |
| ✓ | `[r,g,b]` colours in property events: PC rejects the event, Quest applied a zero-alpha colour | Fixed on this branch: Quest skips the event like PC. (In `SetRenderingSettings`, Quest still accepts `[r,g,b]` with alpha 1 where PC drops the event; left as a harmless leniency.) | — |
| 6 | VideoPlayer sync: no `playbackSpeed`, no `playOnAwake` filter, no prepare-first | Video stutter at speed modifiers; videos meant to start later autoplay | `UpdateSyncedObjects` / `RegisterSyncedObject` |
| 7 | No `DynamicGI.UpdateEnvironment()` after settings changes | Ambient light doesn't follow skybox swaps | `ApplyRenderSettingObject` / `Apply*` |
| 8 | Secondary cameras lack a copied `BloomPrePass` | Camera captures lose light glow *(unverified on Quest)* | `HandleCreateCamera` |
| 9 | BeforeMainEffect blits run after the game's main effect | Wrong layering with bloom, only where the main effect is active | `CameraApplier` / `ShouldBypassImageEffect` (`hasMainEffect` never set) |
| 10 | Saber replacement colour polls the colour scheme | Runtime saber colour changes (Chroma) may not reach custom sabers *(unverified)* | Hook `SetSaberGlowColor.SetColors` like `SetSaberManyColor` |
| 11 | Track-less `AssignObjectPrefab` applies to all notes | A malformed map looks different from PC | `AssignmentMatchesTracks` |
| 12 | `SaberTrailRendererEnabler` not ported | Possible trail smear after re-enable *(unverified)* | Hooks on `SaberTrail.OnEnable/OnDisable` |
| 13 | Camera properties dropped on DestroyObject; `null` in Additive clears everything | Edge cases with re-created cameras and mixed load modes | `DestroySecondaryCameraById`, `HandleAssignObjectPrefab` |
| 14 | Animator `updateMode` not forced to Normal | Prefabs exported with unscaled/physics update drift | `RegisterSyncedObject` |
| 15 | Bundle CRC not verified; downloads happen without consent | Not visual: a stale bundle loads silently, and downloads happen without asking | `PreloadBundle`, `DownloadBundleTo` |

**Intentional or benign differences** (not gaps): Quest writes the final animation value where PC stops one frame short; Quest restores globals and render settings while paused; Quest merges duplicate blits; Quest uses `SetInteger` for static animator integers; Quest accepts extra event synonyms, enum names and setting keys.
