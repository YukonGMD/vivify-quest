# Vivify Quest performance audit

A read-only audit of where Vivify Quest spends CPU, GPU and memory on a Quest 3S (Snapdragon XR2 Gen 2, tile-based Adreno GPU, 72–120 Hz, so 8–14 ms per frame for everything).

- **Branch:** `visual-fixes` at `19d7b09a`. No source was changed.
- **Evidence:** everything comes from reading the code. Nothing was profiled on a headset, so the impact estimates are reasoned, not measured. Each item says how to measure it.

## How to measure (common tools)

- **OVR Metrics Tool** (on headset): CPU/GPU frame time, GPU utilisation, stale frames. Compare a dense section with and without the feature, using the isolation toggles Vivify already has (Disable All Blits, Disable CreateCamera Depth, Disable Custom Note Visuals, Effect Resolution, Scene Depth).
- **Vivify's own logs:** "Vivify level load: … took Nms", "Vivify frame stall", "Vivify frame took …ms" in `VivifySession.txt`; `VivifyReport.txt`.
- **Timers:** wrap a suspect in `std::chrono::steady_clock` and log a per-second total, the same way `LoadMainBundle`'s `phase` lambda does.
- **GPU captures:** RenderDoc for Oculus or Snapdragon Profiler, to count full-screen passes, render-target switches (resolves) and draw calls.
- **Memory:** `adb shell dumpsys meminfo com.beatgames.beatsaber` before and after several plays of one map; Snapdragon Profiler's memory track.
- **GC churn:** sample `System.GC.GetTotalMemory(false)` / `GC.CollectionCount(0)` once a second (il2cpp Boehm GC) and log the deltas during a dense section.

## Code that runs every frame

| Where | What |
|---|---|
| `RuntimeBehaviour::Update` (`src/VivifyComponents.cpp`) | `AudioLink::Tick`, `PollShaderWarmup`, `PollBlitCompanion`, then `Runtime::Update` (in the menu too) |
| `Runtime::Update` (`src/VivifyCore.cpp`) | `CheckDownloadTimeout`, `UpdateSyncedObjects`, `UpdateMaterialAnimations`, `UpdateGlobalAnimations`, `UpdateAnimatorAnimations`, `UpdatePrefabAnimationSpeed`, `UpdateBlitEffects` (+ `SyncSingleViewBlitMaterials`), `UpdateRenderSettingAnimations`, `RefreshCameraComponents`, `DetectSongRestart`, `UpdateSaberReplacementColors`, `SyncReplacementTintColors`. In the menu, only `CheckDownloadTimeout`, `UpdateSyncedObjects` and `RefreshCameraComponents(false)`. |
| `CameraApplier` on the main camera (`src/VivifyComponents.cpp`) | `OnPreCull` → `RestoreSecondaryCullingLayers`; `OnPreRender` → `BindSecondaryCameraTextures`, `ApplySecondaryCameraMainEffects`, `AddMidRenderCommandBuffers`; `OnRenderImage` → `ApplyBlits`; `OnPostRender` → `RemoveMidRenderCommandBuffers` |
| Each CreateCamera camera | `SecondaryCameraController::OnPreCull` (+ `CullingPreCull`), `OnRenderImage`, `OnPostRender` |
| Main camera with `culling` | `CullingCameraController::OnPreCull/OnPostRender` |
| `MultipassKeywordController::OnPreRender` | One global int per camera per eye |
| Each custom trail | `FollowedSaberTrail::Update/LateUpdate` |
| Per note/bomb/chain spawn and per cut (hooks in `src/VivifyHooks.cpp`) | `ApplyNotePrefabFor` → `ReplaceNoteVisuals`; `NoteDebris_Init` → `ReplaceDebrisVisuals` |

## Code that runs at level selection and load

| Where | What |
|---|---|
| `HandleLevelSelected` → `PreloadBundle` (`src/VivifyAssets.cpp`) | Synchronous `AssetBundle.LoadFromFile`; `CacheBundleAssets` (synchronous `LoadAsset` per asset); `StartShaderWarmup`; `ScanBundleForSceneDepth` (worker thread) |
| `ConvertPcBundleAsync`, `PrepareBlitCompanion`, bulk conversion | Worker threads; companion loads asynchronously |
| `PollShaderWarmup` (menu, opt-in) | `SetPass` on 2 materials per frame |
| `PrepareBeatmap` (`src/VivifyEvents.cpp`) at the first Vivify event | `LoadMainBundle` → `CacheBundleAssets` **again**, `DecodeUnsupportedBundleTextures`, `RepairLoadedMaterialShaders`, `WriteLevelStartReport`; `PreloadInstantiatePrefabs`; `ApplyMissedVivifyEvents` |

---

## Suspects, ranked by expected impact on Quest 3S

### 1. Note, bomb, chain and debris replacements: instantiate and destroy on every spawn, no pooling

- **Where:** `src/VivifyObjectPrefabs.cpp`: `ApplyNotePrefabFor`, `ReplaceNoteVisuals`, `InstantiateReplacementPrefab`, `RestoreReplacementData` (via `RestoreNoteVisuals`), `ReplaceDebrisVisuals`. Called from the `GameNoteController/BombNoteController/BurstSliderGameNoteController.Init` and `NoteDebris.Init` hooks.
- **Why it's costly:**
  - Every note that has a prefab assigned destroys the previous copy on that pooled controller and runs `Object.Instantiate` on the prefab.
  - It then makes about ten managed-array calls: `GetComponentsInChildren` on the original renderers, rigidbodies, colliders, renderers for the material repair, animators (twice, once in `CullOffscreenAnimators`), renderers per animator, VideoPlayer/Animator/ParticleSystem in `RegisterSyncedObject`, and the replacement renderers.
  - It also calls `get_sharedMaterials` and `try_cast` per renderer (`ReplacementCanRender`), builds the fingerprint strings, and runs `Rebind`/`Update` on each animator.
  - Each cut does the same for two debris halves.
  - At 10–20 notes per second that is a steady stream of main-thread spikes and managed garbage. Il2cpp's GC then pauses for it.
  - `AssignmentMatchesTracks` also re-parses the note's JSON tracks (`ReadTracks`) when TracksAD has none.
  - PC Vivify pools: `ObjectPrefab/Pools/PrefabPool.cs`, pre-warmed with 10 per asset in `AssignObjectPrefab.Initialize`.
- **Expected impact:** high on maps with custom notes (most Vivify maps), worst in dense sections. Shows as CPU frame-time spikes and stale frames.
- **Measure:**
  - Time `ReplaceNoteVisuals` and `ReplaceDebrisVisuals` and log the per-second total and maximum.
  - Count `Instantiate` calls per second.
  - Watch GC count deltas.
  - Compare OVR Metrics CPU time with "Disable Custom Note Visuals" on and off over the same section.
- **Direction:** a per-asset pool (stack of inactive instances) keyed like PC's `PrefabPool`. Return instances on note despawn and pre-warm at assignment. Cache each pooled instance's renderer, animator and particle lists once.

### 2. `OnRenderImage` compositing on the main XR camera, with extra full-screen copies

- **Where:** `CameraApplier::OnRenderImage` (`src/VivifyComponents.cpp`), `Runtime::ApplyBlits` (`src/VivifyPostProcessing.cpp`), and `RefreshCameraApplier` (enables it when `needsBlit`, which includes "any CreateCamera exists").
- **Why it's costly:**
  - Any enabled `OnRenderImage` makes Unity render the eye into an intermediate texture and copy it to the eye buffer. On a tiled GPU that is a full resolve and a store per frame.
  - `ApplyBlits` always copies `src → _mainBlitTexture` first and `mainCurrent → dest` last, so even one effect costs two extra full-resolution, two-slice copies on top of the effect pass.
  - With no effect active but a secondary camera present, the passthrough path still does one full copy.
  - Each pass is roughly 2 × 1.8k × 1.9k pixels of bandwidth at Quest 3S eye resolution.
- **Expected impact:** high, on GPU time and power, for any map using blits or CreateCamera.
- **Measure:**
  - Count full-screen passes in a RenderDoc capture.
  - In OVR Metrics, compare GPU time with "Disable All Blits" and with Effect Resolution at 100% vs 50%.
  - Check that the passthrough path disappears in a capture when `needsBlit` is only true because of a camera.
- **Direction:**
  - Blit `src` directly into the first effect's destination, and the last effect directly into `dest`, skipping the copies when a single `_Main → _Main` effect runs.
  - Don't enable the `OnRenderImage` component while no effect targets `_Main` (camera-only frames need `OnPreRender` binding, not `OnRenderImage`).

### 3. CreateCamera cameras: extra blit, per-frame renderer scans for culling, full scene re-render

- **Where:** `SecondaryCameraController::OnRenderImage`, `SecondaryCameraController::OnPreCull`, `CullingCameraController::CullingPreCull` (`src/VivifyComponents.cpp`).
- **Why it's costly:**
  - Each camera re-renders the scene; that is inherent and PC does the same (see Platform limits in `PARITY.md`).
  - On top of that, Quest's `OnRenderImage` ends with `Graphics.Blit(src, dest)`. PC's `SecondaryCameraController.OnRenderImage` doesn't do this: the camera's screen output is discarded because the main camera draws over it. That is one wasted full-screen pass per camera per frame.
  - Its colour capture (`Graphics.Blit(src, tex)`) and depth copy each add a pass; that part is needed.
  - `CullingPreCull` runs per camera per frame. For every tracked object it allocates a managed `GetComponentsInChildren<Renderer>(true)` array and a `std::unordered_set`, then sets the layer on every renderer's GameObject twice (move and restore). PC caches the renderer list and rebuilds it only when children change (`RendererController.cs`, `CullingTextureTracker.GameObjects` dirty flag).
  - `OnPreCull` also calls `Camera.main` and `GetComponent<CullingCameraController>` every frame.
- **Expected impact:** high GPU and medium CPU on CreateCamera maps (glitch, portal and screen maps); grows with the number of tracked objects and renderers.
- **Measure:**
  - Time `CullingPreCull` and log the renderer count.
  - In a RenderDoc capture, check for the src→dest pass after each secondary camera.
  - In OVR Metrics, compare GPU time with "Disable CreateCamera Depth".
- **Direction:**
  - Remove the src→dest blit, after confirming in a capture that nothing reads the secondary camera's screen output.
  - Cache the culled renderer list per track and refresh it when the track's GameObjects change (Tracks fires add/remove events) or when a tracked hierarchy changes.

### 4. Level-load and selection work on the main thread

- **Where:** `PreloadBundle`, `LoadMainBundle`, `CacheBundleAssets`, `DecodeUnsupportedBundleTextures` and `ResolveUsableTexture`, `RepairLoadedMaterialShaders` (`src/VivifyAssets.cpp`); `PreloadInstantiatePrefabs` (`src/VivifyObjectPrefabs.cpp`).
- **Why it's costly:**
  - `AssetBundle.LoadFromFile` and the per-asset `LoadAsset` loop are synchronous. Large bundles block the menu at selection.
  - On the common preloaded path, `LoadMainBundle` runs `CacheBundleAssets` a second time at level start: `GetAllAssetNames`, a native `LoadAsset` per asset, string normalisation and shader classification. It also re-runs decode and repair.
  - `DecodeUnsupportedBundleTextures` decodes BC/DXT textures to RGBA32 on the CPU on the main thread, with a budget of up to 8 seconds. `bcdec` decoding is pure CPU and could run on worker threads; only `GetRawTextureData` and `LoadRawTextureData`/`Apply` need the main thread.
  - `PreloadInstantiatePrefabs` instantiates every `InstantiatePrefab` event's prefab in one frame (PC does too).
  - Song start then compiles shader variants on first draw: the 0.14.7 log shows an 88 s stall. "Prepare Shaders Before Playing" exists but is off by default.
- **Expected impact:** high, but only at load. These are long freezes rather than in-song stutter; a long one risks the crash guard's two-strike fallback.
- **Measure:**
  - Read the existing log lines: "Vivify level load: cache bundle assets / decode textures / repair shaders took Nms", "Vivify texture decode: … in Nms", "Vivify frame stall".
  - Add the same timer around `PreloadBundle`'s `LoadFromFile` and its `CacheBundleAssets`, and around `PreloadInstantiatePrefabs`.
- **Direction:**
  - Use `LoadFromFileAsync` / `LoadAllAssetsAsync` (already used for the blit companion).
  - Skip the second `CacheBundleAssets` when the bundle path hasn't changed.
  - Decode on a thread pool and upload on the main thread a few textures per frame.
  - Spread prefab pre-instantiation over frames.

### 5. Memory used by decoded textures

- **Where:** `ResolveUsableTexture` and `ResetRuntime` (`src/VivifyCore.cpp`).
- **Why it's costly:**
  - RGBA32 replacements are 4× the size of BC3/BC7 and 8× BC1. A 4096² texture with mips is about 85 MB.
  - The replacement is created readable and `Apply()` is called with defaults, so Unity keeps a CPU copy as well as the GPU copy. It also regenerates mipmaps, overwriting the decoded mip chain.
  - During decode there are three transient copies: the raw managed array, a `std::vector`, and a new managed array.
  - `ResetRuntime` clears `_decodedTextures` without destroying the replacements. They are freed only if a scene change's `Resources.UnloadUnusedAssets` happens to collect them, so memory may grow across replays *(unverified)*.
- **Expected impact:** medium to high on converted maps with large BC textures. Quest 3S has 8 GB shared with the system, and running out kills the app.
- **Measure:**
  - Run `dumpsys meminfo` (Graphics and Native heaps) after the 1st and 5th play of the same converted map.
  - Log the total decoded bytes in `DecodeUnsupportedBundleTextures`.
- **Direction:**
  - Call `Apply(false, true)`: keep the decoded mips and drop the CPU copy.
  - Destroy the replacements in `ResetRuntime` when their bundle unloads.
  - Consider re-encoding to ASTC/ETC2 on a worker thread instead of RGBA32.

### 6. `UpdatePrefabAnimationSpeed` writes every animator and particle system every frame

- **Where:** `src/VivifyCore.cpp` `UpdatePrefabAnimationSpeed`, `RegisterSyncedObject`, `UpdateSyncedObjects`.
- **Why it's costly:**
  - Every frame it calls `Animator.set_speed` and `ParticleSystem.get_main().set_simulationSpeed` (internal calls) on every animator and particle system in `_syncedObjects` and `_livePrefabs`.
  - `_syncedObjects` gains one entry per spawned note or debris replacement (item 1). Dead entries are pruned with an O(n) `remove_if` every frame, and `RegisterSyncedObject` does an O(n) erase first.
  - The speed is derived from audio time over frame time, so it jitters every frame. A dead-band would avoid most writes.
- **Expected impact:** medium, scaling with prefab complexity. Environment-heavy maps (Aether) can have hundreds of animators and particle systems.
- **Measure:** log the counts of synced animators and particles, and the function's time per second.
- **Direction:**
  - Write only when the speed changes beyond a small epsilon (1.0 ± 0.01 covers normal play).
  - Store synced objects in a map keyed by root.
  - Drop note replacements from the sync list once item 1 pools them.

### 7. `RefreshCameraComponents` every frame, menu included

- **Where:** `src/VivifyPostProcessing.cpp` `RefreshCameraComponents`, `RefreshMultipassRendering` → `EnsureMultipassKeywordController`, `ApplyCameraProperties`, `ApplyCameraGameObjectProperties`, `ApplyFarCulling`, `RefreshCameraApplier`.
- **Why it's costly:**
  - Every frame it calls `Camera.main` and `GetComponent<MultipassKeywordController>`.
  - When `_Main` has properties, it reapplies `depthTextureMode`, `clearFlags` and `backgroundColor` through native setters, calls `GetComponent<CullingCameraController>` and copies the `std::vector<TrackW>` of culling tracks (`SetCullingData`), and calls `GetComponent<BloomPrePass>`.
  - It also checks the culling mask and the scene-depth state.
  - Each call is small, but it is about 10–20 native transitions per frame for state that changes only on events.
- **Expected impact:** low to medium.
- **Measure:** time the function, and count calls per frame with a debug counter.
- **Direction:** cache the components per camera GameObject, and reapply camera properties only when `_mainCameraPropsDirty` is set or the camera changes.

### 8. Converted maps lose GPU instancing

- **Where:** `RepairMaterialShader` (`src/VivifyAssets.cpp`) turns off `enableInstancing` on every converted-bundle material.
- **Why it's costly:** repeated environment meshes (rocks, grass, particles using mesh renderers) and note chords then become one draw call each instead of one per batch. This is a correctness workaround for misplaced instances, so it's partly a platform limit.
- **Expected impact:** medium on converted environment-heavy maps (CPU render thread and driver overhead).
- **Measure:** compare draw-call counts in a capture, and the render-thread time in OVR Metrics, on a converted map vs its Quest-built version where one exists.
- **Direction:** fix the translated instanced variants (instance ID / `unity_InstanceID` plumbing in `VivifyDxbc.cpp`) and re-enable instancing per shader once verified.

### 9. AudioLink runs every frame in every map and in the menu

- **Where:** `src/VivifyAudioLink.cpp` `Tick`, `Update`, `SendAudio`, `FindSource`. AudioLink is on by default (`audioLink` in `src/main.cpp`).
- **Why it's costly:**
  - Per frame: one or two `AudioSource.GetOutputData` calls, 8 `Material.SetFloatArray` calls, about 10 `SetVector`/`SetFloat` calls, and `localtime_r`.
  - The AudioLink CustomRenderTexture updates in Realtime mode on the GPU, whether or not the map samples `_AudioTexture`.
  - When no source is playing (paused, or the menu with no preview), every 30 frames it runs `FindObjectOfType<AudioTimeSyncController>` and `Resources.FindObjectsOfTypeAll<GameplayCoreInstaller>`. The latter scans every loaded object and can hitch the menu.
- **Expected impact:** low to medium; a small constant GPU cost plus periodic menu hitches.
- **Measure:**
  - In OVR Metrics, compare GPU time with AudioLink on and off on a non-AudioLink map.
  - Time `FindSource` when it searches.
- **Direction:**
  - Run AudioLink only while a selected map requires `AudioLink` (or during previews of one).
  - Cache the `GameplayCoreInstaller` per scene instead of searching all objects.

### 10. Mid-render command buffers rebuilt every frame

- **Where:** `AddMidRenderCommandBuffers`, `BuildMidRenderCommandBuffer`, `RemoveMidRenderCommandBuffers` (`src/VivifyPostProcessing.cpp`).
- **Why it's costly:**
  - Each frame and each camera event: remove, clear, re-record every blit, and re-add the buffer.
  - Self-blits take a `RenderTexture.GetTemporary` per frame.
  - The buffers are pooled since 0.14.25, so the remaining cost is re-recording and the engine's command-buffer bookkeeping. PC does the same.
- **Expected impact:** low, plus the GPU passes themselves, which count under item 2.
- **Measure:** time the add/remove pair per frame on a map with mid-render blits.
- **Direction:** re-record only when the effect list, the textures or the descriptor change.

### 11. Small per-frame allocations and calls

| Where | Cost | Direction |
|---|---|---|
| `ApplyMaterialProperty` Keyword case (`src/VivifyProperties.cpp`) | A new `StringW` and an `ApplyStereoKeywords` call every frame for each animated material keyword (the global path was fixed in 0.14.25) | Track the last state per material and keyword, as `_currentGlobalKeywords` does |
| `SyncReplacementTintColors`, `UpdateSaberReplacementColors` (`src/VivifyObjectPrefabs.cpp`) | Iterates every note replacement, including pooled inactive notes: `get_materialPropertyBlock` plus two `GetColor` calls each, every frame | Skip inactive controllers; only sync on colour-change events |
| `CameraApplier::OnPreCull` / `OnPreRender` | `GetComponent<CullingCameraController>` / `<SecondaryCameraController>` per secondary camera, twice per frame | Store the component pointers in `SecondaryCameraData` |
| `PollShaderWarmup` (menu, opt-in) | `Shader.GetAllGlobalKeywords()` (allocates an array and names) every frame of the warmup | Look the keyword up once in `StartShaderWarmup` |
| `ApplySecondaryCameraMainEffects` | `GetTemporary` plus a main-effect render plus a copy back, per `mainEffect` camera per frame | Render the main effect straight into the capture texture where the source can be a temporary |

### 12. Logging

- **Where:** `PaperLogger` calls across the code; `VIVIFY_DEBUG` in per-note and per-event paths (`ReplaceNoteVisuals`, `HandleBlit`, `CullingPreCull`); `vivify_global.txt`, which is appended every session.
- **Why it's costly:** with Debug logging on, every note spawn and every event formats strings and writes to a file on the main thread. With it off, the remaining unconditional lines are rare.
- **Expected impact:** high with Debug logging on, negligible with it off. Always measure with it off.
- **Measure:** compare frame time with Debug logging on and off in a dense section.

---

## Not suspects (checked)

- `CurrentSongTime` is cached per frame; `FindObjectOfType` runs only while the controller is missing.
- Scene depth (`_CameraDepthTexture` pre-pass) is already enabled only for maps whose shaders read it (0.14.21). Measure it with the "Scene Depth For Map Shaders" toggle. A map can still ask for it itself with SetCameraProperty, as Aether does (see the Aether section below).
- Bundle conversion, the scene-depth scan and the blit-companion build run on worker threads with lowered priority.
- The immediate and mid-render command buffers are pooled (0.14.25).
- Material repair is memoised per material (`_repairedMaterials`). The per-spawn cost in item 1 is the hierarchy walk, not the repair.

## Suggested order of work

1. Pool note and debris replacements (item 1); it also shrinks item 6.
2. Remove the redundant full-screen copies (items 2 and 3) and the per-frame culling scans (item 3).
3. Move texture decode and the bundle load off the main thread; skip the second `CacheBundleAssets` (item 4); fix decoded-texture memory (item 5).
4. The remaining low-cost cleanups (items 6, 7, 9, 10, 11).

---

## Measured on 743 Aether (0.14.25-naomi.4)

Read-only audit of Aether's PC bundle (`bundleWindows2021.vivify`), its converted bundle, its beatmap and the naomi.3 session logs. Nothing was profiled on the GPU yet; the counts below come from the bundles, and the timings from the logs.

### What the level is made of

| | |
|---|---|
| Section prefabs | intro, drop, ambient, bridge, buildup, outro; one live at a time, each preloaded (instantiated, inactive) at level start |
| Mesh renderers | 4,587 in total (intro 979, drop 345, ambient 1,147, bridge 584, buildup 212, outro 1,282), drawing only 22 distinct meshes |
| GameObjects / particle systems / animators / lights | 6,513 / 230 / 7 / 0 |
| Materials / shaders | 226 / 35; 64 materials have GPU instancing on in the PC build |
| Repetition | the same mesh and material, many times: intro 727 + 242 terrain cubes; outro 606 + 312 + 202 + 104; ambient 270 + 180 + 126 + 99 + 90 + 72; bridge 128 + 100 + 100 + 100 + 50 |
| Textures | 5, 1.3 MB in total (BC4 ×2, DXT1, Alpha8, a font texture). Not a memory or bandwidth factor |
| Meshes | 22, 28,571 vertices, 38,883 triangles; none marked readable |
| Render textures | none in the bundle; 4 short Blit events (10–16 beats each) |
| Camera | the map sets `depthTextureMode = Depth` at beat 0 for the whole song, so Unity renders a depth pre-pass of every opaque renderer every frame |
| Shaders | procedural and math-heavy: CrackedRainbowSky's fragment program is about 1,370 statements with 6 loops; RainbowSkybox, TriangleExplosion, Gemstone (9 loops), Lattice and Star are 140–220. Almost no texture reads. All 180 translated fragment programs are `precision highp`; none use `mediump` |

The headset's GLES driver reports `GL_EXT_texture_compression_s3tc`, `_rgtc` and `_bptc`, so BC1–BC7 are sampled natively and the RGBA32 decode path (item 5) does nothing for Aether (the log agrees: 0 of 222 materials needed it).

### Re-ranked suspects for Aether

| Rank | What | Expected gain | Risk to the look | Effort | How to measure |
|---|---|---|---|---|---|
| 1 | **GPU instancing is off on every converted material** (item 8; `RepairMaterialShader`, since 0.13.2). On PC those 727 cubes are a couple of instanced draws; here each is its own draw call, and again in the depth pre-pass. The 0.13.2 cause (instanced copies misplaced) was diagnosed the same day as two instancing fixes (0.13.1 array size; 0.11's `gl_InstanceID * 2 + gl_ViewID_OVR` stereo numbering) and a later uniform-block change (0.14.3); there is no record it was re-tested after them. | High on CPU (render thread and driver) in intro, ambient, bridge and outro: hundreds of draw calls a frame fewer | None if the instanced variants are right; misplaced objects if not. Test behind an off-by-default setting first | Low to test (a setting that skips `set_enableInstancing(false)`); medium if the instanced translation needs fixing | `[PERF]` summary and OVR Metrics app CPU time, same section, setting on vs off; look at chords and chains, and at terrain placement |
| 2 | **Note and debris replacements instantiate per spawn** (item 1) | Medium for steady load, high for hitches: the naomi.3 stalls at song 61–82 s line up with the densest note bursts of that stretch (5 notes at 60 s, 8 at 80 s) and happened in the 10th play of the session, with the managed heap already 95 MB (60 MB at the session's first play) | None (pooling reuses identical instances) | Medium | `[PERF]` worst frames: garbage collections during each, and the last event; GC count over a play |
| 3 | **Depth pre-pass for `depthTextureMode = Depth`** | Medium to high: one more pass over every opaque renderer, all song. Halves with rank 1 | None if the depth comes from a copy of the main pass's depth instead of a re-render; that copy has its own cost on a tiled GPU | Medium to high | OVR Metrics GPU time with "Scene Depth For Map Shaders" off (it shows how much the pass costs, though it changes StarFlare/RingArtifact/Bokeh) |
| 4 | **`highp` everywhere in translated shaders** | Medium on GPU for ALU-bound shaders (Adreno runs FP16 at about twice the rate) | Real: banding or precision loss where a value needs 32 bits (positions, noise hashes, time). Only `mediump` on values the DX bytecode marked minimum-precision, or per shader by hand | Medium (translator) | OVR Metrics GPU time and utilisation; visual A/B |
| 5 | **Managed heap growth across plays** | Fewer and shorter collections late in a session | None | Low to investigate (the `[PERF]` heap figures per play), then depends | Heap at each level start over 10 plays of one map |
| 6 | **`UpdatePrefabAnimationSpeed` on every animator and particle system** (item 6) | Low to medium: Aether's live section has up to 111 particle systems | None with a dead-band | Low | Time it per second |
| 7 | **Per-frame allocations in our code** | Low each; adds to rank 5 | None | Low | See the list below |

Per-frame allocations and lookups found in our own code (naomi.4 audit):

- `ApplyMaterialProperty` keyword case: a new managed `StringW` per animated material keyword per frame.
- `CameraApplier::OnRenderImage`: two `RenderTexture.GetTemporary` per frame (pooled by Unity, but still a lookup and bookkeeping each frame).
- `Camera.main` and `GetComponent<...>` every frame in `RefreshCameraComponents`, `SecondaryCameraController::OnPreCull/OnPostRender`, `CameraApplier::OnPreRender`, `BindSecondaryCameraTextures`, `RestoreSecondaryCullingLayers`.
- `ApplyBlits`: a `std::vector` per frame while blits run.

### What the naomi.3 stalls were not

The six stalls of 1.0–2.1 s at song 61–82 s were in the drop section (32–108 s). Vivify's events in that window are 44 `AssignPathAnimation` and one `AssignObjectPrefab`: no prefab instantiation, blit, material change or camera change. The same difficulty, played five times in that session, stalled in only one play, and an earlier play ran past 82 s without a stall, so it was not first use of a shader or anything else in the content itself. Most likely: garbage collections triggered by note and debris replacement churn in a heap that had grown over ten plays (moderate confidence). The `[PERF]` summary now records the collections during each of the worst frames, which settles it.
