# Version X plan (naomi.10, "Twilight Sparkle")

Written after naomi.10-beta.1 (crash fix, gray ledger and shader audit,
settings menu, logging) and updated after beta.1's test: **beta.2 is the
white-screen fix**, done; every milestone planned before it moved down by
one, and none of them is started. Each milestone is one beta; the release is
`0.14.25-naomi.10-release` (see CODENAMES.md for why not a bare `naomi.10`).

Rules that hold throughout: nothing makes a level look different from PC
unless it is behind a setting that is off by default; the crash guard stays
frozen except naomi.7's three lifted fixes; Prepare Shaders' default does not
change without the tester's say-so; a rewrite needs a measured reason.

## Where things stand

**743 Aether is GPU-bound.** naomi.9, default settings, OVR Metrics capture
of the 01:22 session: GPU busy 95% (median), app GPU time 12.4 ms median
against the 8.3 ms budget at 120 Hz, 65 fps median, GPU clock pinned at
690 MHz, eye buffers 1680x1760. Vivify's own work on the main thread is
0.14-0.20 ms a frame. The CPU is not what misses the frames.

| Aether section (song s) | Median frame, default settings | 95th | Held 60 fps |
|---|---|---|---|
| intro (0-32) | 31 ms | 56 | 21% |
| drop (32-108) | 9 ms | 10-49 | 84-99% |
| ambient (108-154) | 34 ms | 83 | 1% |
| bridge (154-211) | 22 ms | 31 | 0% |
| buildup (211-237) | 19 ms | 32 | 43% |
| outro (237-272) | 9 ms | 16-18 | 93-97% |

With Performance Mode on (earlier naomi.9 plays) the same sections ran
15-21 ms, 9, 15, 10, 11 and 9: resolution is a strong lever, which says the
cost is per pixel.

PERF.md's naomi.5 analysis of what draws where: the intro, ambient and
bridge are thousands of opaque scenery draws (SimpleTerrainFog 713,
GlassTerrain 865, Gemstone 412 renderers) plus a depth pre-pass of all of
them (the map sets `depthTextureMode = Depth` for the whole song); the drop
and outro are transparent effect overdraw (StarFlare, RingArtifact,
GlowingWisp); the buildup is one full-screen procedural sky
(CrackedRainbowSky, about 1,370 operations a pixel). Opaque skies are drawn
first (a sphere centred on the player sorts nearest), so every sky pixel is
shaded before scenery covers it.

**beta.1's test (2026-10-25, 16 plays of 12 maps):** no crash. Aether's
sections at default settings: intro 33-42 ms, drop 9, ambient 33, bridge 22,
outro 9. The intro's figure includes IntroBokeh, which costs about 9 ms of GPU
time while it runs (GPU median 36 ms during it, 27 after). One play with
scenery instancing on and stand-ins off ran ambient at 24 ms and buildup at 12
-- a lead for the measuring milestone, not a result (two settings changed at
once). Logging cost 0.003-0.005 ms a frame for the black box and up to
0.015 ms for the session log with Debug Logging on; well under the 0.1 ms
target.

**The white screen** (fixed in beta.2, GRAY.md): converted maps' Unity 2019
Blit shaders read the screen as one plain 2D texture; on the Quest the screen
is a two-layer array and Unity gave them its default white. Aether had about
18 s of white per full play; Get Your Wish 32 beats. Frames kept coming (OVR
11-52 fps through it), so it was content, not a freeze. The same reads broke
Aether's depth effects. Risk to watch in beta.2: with real pixels to read,
the Bokeh Blits may cost more GPU than they did reading a default texture.

**Stability:** naomi.9's two crashes were Vivify use-after-free during level
teardown (fixed in beta.1); one more was another mod's start-up crash. No
memory pressure near any of them.

**Gray:** the only gray in naomi.9 was Aether's particle systems on emulated
geometry shaders (GRAY.md). All 15,946 GLES programs in the tester's 25 maps
now compile and link offline.

**Measurement gaps:** OVR Metrics' GPU counters (fill, fragment
instructions per pixel, share of time shading fragments) were not switched on
in any capture so far (every such column reads 0), so per-pixel cost is
inferred, not measured; see "Capturing GPU counters" below. Shader compile
stalls were not visible before beta.1; the slow-frame sampler now names them,
and beta.1 caught none.

## System audit

| System | What it does | Health | Measured cost | Verdict |
|---|---|---|---|---|
| Converter (`VivifyBundleConvert`, `VivifySerializedFile`) | Retargets PC bundles to Android, translates their shaders, makes meshes readable for emulation | 0 failures (353 maps scanned, 9 converted, naomi.9); offline every bundle converts. The 2019-or-2021 choice was directory order and gave Aether and Get Your Wish their 2019 builds; beta.2 prefers 2021 | 9 maps in 8 s on the headset | **Keep** |
| Shader translator (`VivifyDxbc`) | DXBC to GLSL ES 3.x, multiview stereo, per-function modes for emulation | Two bugs found since beta.1: a depth-only output (fixed in beta.1) and 2019 builds' screen reads as 2D, which drew white Blits and broke depth effects (fixed in beta.2). Geometry programs that read the instance ID are refused | Programs are `highp` throughout; loops are `for` loops since naomi.7 | **Keep.** Fix: `mediump` only where the DXBC marks minimum precision (setting, off) |
| Texture path (`VivifyTextureDecode`) | Decodes textures the GPU cannot sample | Quest 3S samples BC1-BC7 natively; 0 textures needed decoding in the maps played | None in Aether | **Keep** as is |
| Geometry emulation and particle bakes | Runs known geometry programs in the vertex stage on rebuilt meshes; bakes still particle bursts | 297 Aether objects rebuilt, 7 shaders; 5 particle systems stay gray (moving, billboard, over the bake budget). Menu Mesh count grows 59-131 per Aether play since naomi.8 | Bake 3-9 ms once per burst | **Fix:** the per-play Mesh growth; draw moving mesh particles by instancing the rebuilt mesh instead of baking; billboard particles. Not a rewrite |
| Crash guard | Two unfinished loads of a converted map, then untranslated stand-ins | No strikes in naomi.9 | None | **Keep frozen.** Recommend only |
| Resolution scaling (Performance Mode) | Lowers the drawn part of the eye buffers while a song runs over budget; foveation and GPU boost | No blanking since naomi.8; halves Aether's slow sections | Big win, but it changes the look, so it stays off by default | **Keep.** Tune with per-section data; no new mechanism |
| Note pooling, scenery instancing | Reuses note and debris copies; keeps converted scenery's GPU instancing (experimental, off) | Pooling on and stable; instancing untested since its 0.13 problems | Instancing would cut draws and the depth pass's draws; Aether's bound is pixels, so expect less there than the draw count suggests | **Keep.** Test instancing on Aether with the cost test before deciding |
| Scene and camera handling | Vivify cameras, blits, screen textures, the scene depth texture, mid-frame copies | Stable | The depth pre-pass draws every opaque renderer again, all song, in Aether | **Fix:** depth from the main pass instead of a pre-pass (setting, off until it matches) |
| Settings | Table, three-part menu, migration | Done in beta.1; migration host-tested on the tester's config | None | **Keep** |
| Logging | Saved sessions, black box, detailed mode, digest and pull tools; beta.2 adds screen-texture warnings and a mostly-white view detector (detailed mode) | Worked through beta.1's 16 plays | Headset, per frame: black box 0.003-0.005 ms, session log up to 0.015 ms with Debug Logging on, detailed mode 0.021-0.026 ms | **Keep** |

## Where Version X spends its effort

Per-pixel GPU work and stalls, in this order, each measured before and after
with OVR Metrics (GPU counters on) and the session log's per-section GPU
time, frame bands and dropped refreshes:

1. **Opaque full-screen skies last.** Same image (opaque, depth-written,
   depth-tested, unblended), far fewer shaded sky pixels; biggest in the
   buildup, drop and bridge.
2. **Depth without a pre-pass.** A copy of the main pass's depth instead of
   re-drawing every opaque object; measure first with the cost test's "Depth
   pre-pass".
3. **First-use shader compiles.** Warm exactly the variants a map's
   materials use, during the load, and confirm with the slow-frame sampler's
   compile line. (Prepare Shaders stays as it is and off unless the tester
   says otherwise.)
4. **Transparent overdraw** in the drop and outro: measure (fill and fragment
   counters, cost test "Transparent effects") before choosing; any reduced
   resolution pass is a look change and would be a setting, off.
5. **Render-target formats and copies:** the mid-frame copies and blit
   targets' size and format against what each effect needs; Effect
   Resolution already scales blits.
6. **Shader precision** last, and only where the bytecode allows it.

## Targets

Aether (4968d), default settings, 120 Hz:

- No frame over 250 ms after the first 2 s of a song; no compile line from
  the slow-frame sampler after the first frame; first frame after level start
  under 500 ms.
- App GPU time median at most 10 ms (from 12.4).
- Medians: intro and ambient at most 16.7 ms (from 31 and 34), bridge and
  buildup at most 12.5 ms (from 22 and 19), drop and outro no worse than 9 ms;
  60 fps held in at least 90% of frames in every section.
- No new stand-ins; at the end of the particle milestone, 'Extending
  Arrows' and 'Ground Fog' drawn by emulation.
- Menu Mesh count after the level grows by at most 10 per play over five
  plays.
- Logging (layers 1 and 2) under 0.1 ms a frame.

Lost in Data (4839c, a Quest bundle; median 15 ms, 95th 19-21): no
regression (median at most 15.5 ms, 95th at most 21), and the sky and depth
milestones applied if they apply.

Whole library: all 34 bundles of the tester's 28 maps compile and link in the
offline audit; no conversion failures; no crash in the fixed test set.

## Regression checklist

On the laptop, every beta: the full host suite (34 checks), the offline
shader audit over the tester's 28 maps (`tools/shaderaudit`; 18,507 programs
at beta.2), `conv --scan` showing no Blit shader that reads the screen as 2D
in Aether's and Get Your Wish's conversions, and converter output
byte-identical for any path a beta does not mean to change.

On the headset, every beta, the same plays:

1. Aether, full song, twice, default settings; once more with Detailed
   Logging on (the first beta that changes rendering).
2. Lost in Data, full song.
3. Lifelike (43a25, converted) and Great Asset (48dd1, the largest Quest
   bundle), each once right after installing (conversion and first compiles).
4. Five level switches in a row, and one quit to the menu mid-song (the
   teardown crashes).
5. The settings menu: the Everyday screen fits without scrolling; Show
   Developer Options shows and hides Debug; Reset To Defaults.
6. Two launches, then `tools/pull/pull_sessions.py`: two session files, the
   index, digests compared with the previous beta's.
7. No white view: Aether at 2-6 s, 104-112 s and 207-214 s, and Get Your
   Wish from 166 s to about 187 s (where beta.1 went white). With Detailed
   Logging on, the song summary's "mostly-white view" count is 0 or only a
   map's own intended fade (Aether's FadeWhite peaks white for well under a
   second at 108 s).

Passes when: no crash; no new gray in the digests; no section's median
worse than the previous beta by more than 5%; logging under 0.1 ms a frame;
every change that alters the image behind a setting that is off.

## Milestones

Each is one beta. A new rendering path ships behind a setting, off, with the
old path untouched, until the fixed test set and an A/B screenshot show it
right; only then does a later beta turn it on (asking first where the image
could differ at all).

1. **beta.2: the white screen** (done; `0.14.25-naomi.10-beta.2`). Converted
   maps prefer their 2021 PC bundle; the translator reads screen-space
   textures in two-eye programs as two-layer arrays, as Unity's multiview
   build does (Debug setting "Two-Eye Screen Textures", on, to undo it);
   conversion version 24. New warnings and a white-view detector (detailed
   mode) confirm it on the headset. A parity fix, so on by default.
2. **beta.3: measure, and the leak.** No look changes. Cost-test plays of
   Aether (one per category: skies, transparent effects, depth pre-pass,
   emulated geometry, blits, scenery) and separate GPU-counter plays (below),
   giving a measured cost table that replaces the inferred one; scenery
   instancing on against off, one setting at a time. Find and fix the menu
   Mesh/GameObject growth per play.
3. **beta.4: skies last.** Opaque full-screen skies drawn after the rest of
   the opaque queue, for shaders that match (opaque, ZWrite on, LEqual, no
   blend; checked at load). Setting "Draw Skies Last", off; turned on in a
   later beta once screenshots match.
4. **beta.5: depth from the main pass.** Scene depth for map shaders copied
   from the main pass instead of a depth pre-pass. Setting, off.
5. **beta.6: compile stalls.** Warm the variants a map's materials use during
   the load; target no compile lines after the first frame.
6. **beta.7: particles.** Moving mesh particles on emulated shaders drawn by
   instancing the rebuilt mesh with each particle's transform; billboard
   particles likewise. Under "Emulate Geometry Effects On Particles", as the
   bakes are.
7. **beta.8 (if the numbers call for it): transparent overdraw or precision**,
   chosen by beta.3's cost table, behind a setting, off.
8. **naomi.10-release** once the fixed test set passes on the last beta.

Recommended next three: beta.3 (measure and fix the leak), beta.4 (skies
last), beta.5 (depth from the main pass). They need no look change to pay
off, and beta.3 decides whether anything after beta.5 is worth doing.

## Capturing GPU counters

The per-pixel counters (fill per eye, instructions per fragment, time
shading fragments, texture fetch stalls) come from the headset's GPU
profiler, which has to be switched on from a computer first. They add some
overhead, so a play with them on is never used for timing: do counter plays
and timing plays separately, and say which is which.

What you need: the headset connected over USB (or Wi-Fi ADB) with developer
mode on, `adb` on the computer, and OVR Metrics Tool installed on the
headset **before** Beat Saber is started (installing it while the game runs
closes the game).

1. Quit Beat Saber.
2. On the computer: `adb shell ovrgpuprofiler -e` (Meta's documented way to
   enable the profiler; their page says some OVR Metrics stats need it
   before they can be selected).
3. In the headset, open OVR Metrics Tool. On its main screen switch on
   "Record all captured metrics to csv files".
4. On the **Stats** tab, select: Average Fill Percentage per Eye, Average
   Instructions per Fragment, Average Instructions per Vertex, Average
   Vertices Per Frame, Percentage Time Shading Fragments, Percentage Time
   Shading Vertices, Texture Fetch Stall Percentage, L1 and L2 Texture Miss
   Percentage. (The Basic and Advanced buttons do not include these.)
5. Start Beat Saber and play the counter plays: Aether once full length at
   default settings; then, if there is time, once each with "Debug: Switch
   Off For A Cost Test" set to Skies, Transparent effects, Depth pre-pass,
   Emulated geometry and Post-processing blits (set it back to Nothing
   after).
6. Quit Beat Saber. On the computer: `adb shell ovrgpuprofiler -d` to turn
   the profiler off again (if that option is not recognised, restarting the
   headset turns it off; `adb shell ovrgpuprofiler --help` lists the
   options).
7. Tell me which plays were counter plays. I pull the CSVs read-only from
   `/sdcard/Android/data/com.oculus.ovrmonitormetricsservice/files/CapturedMetrics/`.

If the new columns (`avg_fill_percentage`, `avg_inst_per_frag`,
`percent_time_shading_frags`) still read 0 in the CSV, the counters did not
switch on: tell me what `adb shell ovrgpuprofiler -e` printed, since Meta's
page does not document an in-app switch for them.

## Ambitious option: a new core

The question: rewrite Vivify's core pipeline from the ground up (converter,
shader translator, material and texture handling, effect emulation,
resolution control) to make it as fast and efficient as it can be. This
section weighs it against what the measurements say. Short answer at the
end: **not a ground-up rewrite; a second pipeline built beside the first, one
measured piece at a time, starting after beta.3's cost table**.

### What cannot change, and what that leaves

Beat Saber 1.40.8 on the Quest renders with **Unity 2022.3.33f1's built-in
render pipeline**, on **OpenGL ES 3** with **multiview** (one draw for both
eyes into a two-layer eye buffer, 1680x1760 per eye at 120 Hz). The evidence:
the game's managed code carries no URP or HDRP assemblies (the
`RenderPipelineAsset`/`ScriptableRenderContext` types present are core
UnityEngine ones every build has); the game's own `MainEffectController`
works through `OnPreRender`/`OnPostRender`, and Vivify's Blits through
`OnRenderImage`, callbacks that exist only in the built-in pipeline; the log
shows `stereoMode=3` (multiview) and two-slice eye textures. Quality settings
in the session log: antiAliasing 0, eyeTextureResolutionScale 1.0, shadows
off, pixel lights 0.

What that **allows**: per-camera properties (depth texture mode, clear
flags, culling mask, far plane, MSAA and HDR flags, render order by depth);
command buffers attached at the built-in camera events (before or after the
opaque pass, before image effects, after everything); `OnRenderImage`
effects; the XR viewport scale (cheap) and eye-texture scale (reallocates;
naomi.7's blanks); fixed foveation and GPU/CPU levels through OVRManager;
`Graphics.DrawMeshInstanced`, `CommandBuffer.DrawMeshInstanced` and
`Mesh.CombineMeshes`; compute shaders (`ComputeShader` is in the game's
code; GLES 3.1+ and the Adreno 740 support them; dispatch and procedural
draws would go through engine calls, since their managed wrappers are
stripped).

What it **rules out**: everything that lives in a scriptable pipeline --
render graph, renderer features, the SRP Batcher, URP's motion-vector pass
and so Application SpaceWarp (next section) -- and Vulkan-only features. The
game picks its graphics API and pipeline at build time; a mod cannot switch
either.

### Hooking into the game's rendering, option by option

| Option | What it would do | Possible for a mod? | Evidence and payoff for Aether | Risk, including game updates |
|---|---|---|---|---|
| Per-camera settings | Depth texture mode, clear flags, culling, MSAA/HDR flags, render order, Vivify cameras' resolution | Yes, Vivify already sets several | The depth pre-pass that `depthTextureMode = Depth` costs is a whole second opaque pass all song; Vivify cameras could render at Effect Resolution | Low; public Unity API, stable across game updates |
| Our own steps through camera events and command buffers | Draw opaque skies after the other opaque objects; copy the main pass's depth instead of a pre-pass; downsampled Blits; a cheap view probe | Yes; Vivify's Blits and the white-view probe already work this way | Skies: CrackedRainbowSky about 1,370 operations a pixel, drawn first today. Depth: see above | Low to medium; public API. Wrong ordering shows as wrong images, which A/B screenshots catch |
| Resolution, refresh rate and foveation | Viewport scale (Performance Mode), refresh rate (72/90/120 Hz), fixed foveation | Yes; viewport scale and foveation are done; the refresh rate is OVRPlugin's `systemDisplayFrequency` (the game's own settings should also offer a refresh-rate choice on the Quest; to confirm on the headset) | Measured: Performance Mode took Aether's intro from 31 to 15-21 ms and ambient from 34 to 15 (naomi.9). At 72 Hz the budget is 13.9 ms against Aether's 12.4 ms median GPU time | Low technically; every one of them changes what the player sees (sharpness, smoothness), so settings, off |
| Hooking Unity's native rendering functions | Inline hooks inside libunity (GfxDevice draw and state calls): skip, reorder or rewrite single draws, read draw-call detail | Technically, with pattern-scanned addresses; libunity is not exported for this | Little that command buffers cannot do already; draw-level detail for diagnostics | High: any game update rebuilds libunity and moves every address; a wrong hook crashes every map, not one |
| Application SpaceWarp (render at half rate, the system synthesises the other frames) | Halve the GPU work per displayed frame | **No.** Meta's documentation requires URP with SpaceWarp support, Vulkan as the only graphics API, a `MotionVectors` pass in every shader, depth submission, and the feature switched on at build time. Beat Saber is built-in pipeline on GLES, and its and the maps' shaders write no motion vectors. `OVRManager.SetSpaceWarp` exists in the game's code, but with no motion-vector or depth layers behind it the compositor has nothing to synthesise from | Would be the largest win if it existed (about half the GPU time) | Out of reach for a mod: it needs a different build of the game |
| The compositor's own reprojection | Already always on (timewarp of the last frame to the latest head pose) | Not controllable | It is what keeps head motion smooth at 65 fps today | n/a |

### Which parts of today's pipeline cost speed, and which caused the gray and white

- **White** (beta.1): the translator turned each shader's own program into
  GLSL faithfully but did not know which textures were the screen. Fixed by
  a heuristic (coordinates from `unity_StereoScaleOffset`, camera texture
  names). A screen coordinate computed in the vertex stage, or a Blit shader
  that samples the screen with plain UVs, still slips through; only a
  cross-stage analysis (an IR, below) closes that.
- **Gray**: geometry-shader emulation covers three known shaders and
  triangle or point programs only, and particle systems only when their
  particles hold still and fit a CPU bake (Aether's 5 remaining gray
  systems). Stand-ins also replace any variant the translator refuses. Quest
  bundles' own shaders are never touched (Lost in Data's FogMB reads depth as
  2D).
- **Speed**: Aether is GPU-bound at 95% busy; Vivify's own CPU work is
  0.14-0.20 ms a frame, so anything that only saves CPU time saves nothing
  that shows. The GPU time goes on per-pixel work: full-screen procedural
  skies shaded before scenery covers them, the depth pre-pass, transparent
  effects' overdraw, full-resolution Blits (IntroBokeh costs about 9 ms while
  it runs), and `highp` arithmetic everywhere.

### What an optimising converter could do, honestly weighed

| Piece | What it does | Expected payoff, with evidence | Cost and risk |
|---|---|---|---|
| Classify shaders at conversion time | Mark skies (opaque, depth-written, full-screen), Blits, screen textures across both stages, reachable keyword variants | Enables the sky reorder, the depth copy and exact warm-up; no speed by itself | Low; analysis only, old output unchanged |
| Dead-keyword and dead-variant removal | Drop variants no material and no event can turn on (beyond today's "skip unused", 877 variants skipped in 43a5d) | Shorter conversions and loads, fewer first-use compiles. beta.1 saw no compile stall in-song but first frames of 0.7-1.2 s after level start on four maps and 0.3-0.9 s of Prepare Shaders. **No change to frame time** in steady play | Low to medium: a variant wrongly judged dead draws as a stand-in; needs the map's events scanned for keyword changes |
| Shader simplification and precision lowering | Fold constants, drop dead code, and use `mediump` where the DXBC marks minimum precision or a value provably fits | Adreno 740 runs FP16 at about twice the FP32 rate, so ALU-heavy skies and Gemstone could gain noticeably. **Unmeasured**: the GPU counters (instructions per fragment, time shading fragments) were never on. Aether's sky hashes (`fract(sin(x) * 43758.5)`) break in 16-bit, so most of Aether's heavy shaders are exactly the ones that cannot drop precision | Medium cost; high look risk; setting, off, per shader |
| SPIR-V optimisation round trip | GLSL to SPIR-V (glslang), spirv-opt (inlining, dead code, loop unrolling), back to GLSL ES (SPIRV-Cross) | **Small.** The DXBC is already the output of Microsoft's optimising compiler, and the Adreno driver compiles our GLSL with its own LLVM-based optimiser, which removes the bit-cast noise our translation adds. Possibly shorter compile times | High: two large libraries in the mod, and SPIRV-Cross renames uniforms and blocks that Unity binds by name (`hlslcc_mtx`, `UnityStereoGlobals`, locations). Not worth it on the evidence |
| ASTC recompression and mip handling | Recompress BC textures to ASTC (smaller, same sampling), generate missing mips | **None for Aether** (5 textures, 1.3 MB in all; the GPU samples BC1-BC7 natively). Some for texture-heavy maps (Coconut Mall has 48), mainly memory and bandwidth; unmeasured | Medium-high cost (an ASTC encoder is slow on the headset); quality loss is a look change, setting, off |
| Mesh optimisation and LOD | Vertex-cache reordering, simplified distant meshes | **None for Aether** (22 meshes, 38,883 triangles in all) | LOD changes the look; vertex reordering is safe but buys nothing here |
| Merging materials and draws | GPU instancing (already an experimental setting), `Mesh.CombineMeshes` for objects that never move | Aether draws 4,587 renderers from 22 meshes, and the depth pre-pass draws them again. One beta.1 play with instancing on ran ambient at 24 ms against 33, but stand-ins were off in it too, so unproven. Since the GPU is the bound, expect less than the draw count suggests | Medium: objects the map animates or moves cannot be combined; misplaced instances were the 0.13 bug |
| Geometry emulation through compute | Run any geometry program as a compute pass writing a vertex buffer, then draw that | Clears the remaining gray (moving and billboard particles, the instance-ID programs in 43a5d) and makes the CPU bakes unnecessary. GPU cost about what PC pays for the geometry stage | High: a compute translation path, dispatch and procedural draws through engine calls, per-frame buffers; the biggest single piece of work here |
| Resolution control | Per-section learned scale (exists), Vivify cameras and Blits at reduced resolution, refresh-rate choice | Measured: the largest lever there is (Aether's slow sections halved) | Low technically; look change, settings, off |

### What the best-possible design would look like

Given that Unity's built-in renderer, GLES and the game's camera setup stay
as they are, the best core is a **compiler with a whole-map view** feeding a
**thin runtime**:

1. **At conversion time** the converter parses every program into one
   intermediate form (SSA, typed, both stages of a variant together) instead
   of emitting text straight from DXBC. Analyses run there: which textures
   are the screen (across stages), which shaders are skies, Blits or
   depth-only, which variants are reachable from the map's materials and
   events, which values tolerate `mediump`. It writes GLSL from that form and
   a sidecar file with every decision, so the runtime does not rediscover
   them at load.
2. **Material and texture handling** becomes data from that sidecar: stand-in
   plans, emulation plans and texture decisions are computed once per
   conversion, not at every level load.
3. **Effect emulation** moves to compute where the GPU supports it, with
   today's vertex-stage emulation and bakes as the fallback.
4. **The runtime** keeps doing what only it can: events, animations, cameras,
   and the command-buffer steps (sky order, depth copy, Blits at their own
   resolution), plus resolution control.

That is a large piece of work, and most of its speed would come from the
command-buffer steps and resolution control, which do not need the new
converter at all. The converter half mainly buys correctness (no more
heuristics, which is what the white and gray problems came from) and shorter
loads.

### Ranked for Aether-type maps (payoff against risk)

1. **Opaque skies after the opaque queue** (command buffer or render-queue
   change). Same image; large in the buildup, drop and bridge; low risk.
2. **Scene depth from the main pass instead of a pre-pass.** A whole opaque
   pass saved all song; medium risk.
3. **Resolution and refresh-rate control for heavy maps** (exists in part).
   Measured largest lever; low risk; changes the look, so settings, off.
4. **Blits at their own resolution** (Effect Resolution exists; defaults to
   100%). IntroBokeh's 9 ms; look change, setting.
5. **Exact variant warm-up and dead-variant removal.** Load time and stalls;
   low to medium risk.
6. **Instancing and combined meshes.** Medium payoff, unmeasured; medium risk.
7. **Compute geometry emulation.** Clears the gray; high cost and risk.
8. **Precision lowering.** Potentially large on some shaders, but Aether's
   heaviest break; high look risk.
9. **ASTC, mesh optimisation, SPIR-V round trip.** Little or nothing for
   Aether on the evidence.
10. **Native render hooks.** Breaks with game updates; little beyond 1-4.
11. **Application SpaceWarp.** Not possible for a mod.

### Recommendation and the step-by-step path

Do **not** start a ground-up rewrite. The measurements say the speed is in
per-pixel GPU work that the runtime's command-buffer steps and resolution
control reach without a new converter, and a rewrite would put every working
map at risk at once. Do build the new core **beside** the old one, in this
order, each step a beta with the new route behind its own setting (off) and
the old route untouched until the fixed test set and A/B screenshots prove
the new one:

1. **beta.3** (as planned): the measured cost table and GPU counters. Gate:
   if skies, depth and transparent effects together are not most of Aether's
   GPU time, re-rank everything below before going on.
2. **beta.4-beta.5** (as planned): skies last, depth from the main pass.
3. **beta.6**: the conversion-time **analysis layer** ("Converter 2,
   analysis only"): cross-stage screen-texture detection, sky/Blit
   classification, reachable variants, written as a sidecar. It feeds the
   compile-stall warm-up of beta.6 and replaces beta.2's heuristic only when
   its answers match on all 28 maps. A host A/B tool runs both translations
   of every program through glslang and a software renderer (for example
   Mesa's llvmpipe) and compares their output pixels, so "same image" is
   checked on the laptop before the headset.
4. **beta.9 onward** (after naomi.10-release, as naomi.11): the IR-based
   emitter for one shader class at a time (Blits first, then skies), setting
   "New Shader Translator" off; precision analysis on top of it; compute
   geometry emulation last.

That gives the speed early (beta.4-beta.5), the correctness of a real
compiler without betting the library on it, and an exit at every step if a
piece does not pay.

## After Version X (backlog, by priority)

1. Geometry emulation for programs that read the instance ID or pass a
   varying through (43a5d's ParticleScreen/Visualizer and AreaLit/LightMesh).
2. 42-flux (43999): WACOMalt/WACOWater4 shipped empty; make re-fetching the
   PC donor after a clean slate visible, or say in the menu why it is gray.
3. A shorter gray flash before an explosion bakes (check every frame for
   the first second, with a reused, pinned particle buffer).
4. Sky cubemap or lower-resolution sky (look changes; settings, off).
5. Lower-resolution transparent effects pass (look change; setting, off).
6. `mediump` where the bytecode marks minimum precision (setting, off).
7. The crash guard's remaining issues, as recommendations for the tester.
8. Other mods' start-up crashes (libbl and others' memcpy) reported upstream
   with the symbolised stacks.
9. Per-draw-call detail in detailed mode, only if a measured need appears
    (Unity loads GL through its own table, so it would mean a hook).
10. The offline shader audit in CI, with a small redistributable bundle as
    its fixture.
11. Quest-built shaders that read the camera's depth as one plain 2D texture
    (Lost in Data's and Winter Wrap Up's Custom/FogMB): a GLSL rewrite of
    those reads for Quest bundles, behind a setting, off, since it changes a
    Quest map from how its own Quest build draws.
12. Kitchen Gun's video screen (43a4a, `vidscreen.mat` reads a VideoPlayer's
    render texture): check whether the video plays on the Quest, with the
    white-view detector, before changing anything.
13. Grab-pass textures (`_GrabTexture`, Get Your Wish's FX/CensorWorldSpace)
    read as 2D in two-eye programs.
