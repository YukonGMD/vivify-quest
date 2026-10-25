# Vivify settings

Every setting in the Vivify menu (Mod Settings → Vivify), as of naomi.10.
The table in `src/VivifySettings.cpp` is the source of truth for keys,
defaults, ranges, names and hover hints; the host test in `tools/settings`
checks this file against it.

The menu has three parts:

- **Everyday**: what a player changes from one session to the next. It fits
  on one screen without scrolling.
- **Advanced**: everything else a player might reasonably change, grouped by
  what it is for. It starts below the everyday screen.
- **Debug**: switches for tests and for isolating a problem. Hidden unless
  "Show Developer Options" (at the end of Advanced) is on. When the section is
  hidden but one of its switches is not at its default, a line above says
  which.

Settings are stored in `Configs/Vivify.json` under the keys below. Values
carry over between versions by key. A missing, mistyped or out-of-range
value becomes the default. "Reset All Vivify Settings To Defaults" (Advanced,
press twice) puts every setting back to its default; maps and converted
files are kept.

Settings marked *reconverts* change how PC maps are converted. A converted
map is rebuilt the next time it loads after one of them changes.

## Everyday

| Menu name | Key | Default | What it does |
|---|---|---|---|
| Performance Mode | `performanceModeHeavyMaps` | off | Lowers the resolution while a heavy map runs slower than the headset's refresh rate. Changes how the map looks. Its fine-tuning is under Advanced → Performance Mode. |
| Effect Resolution % | `effectResolutionPercent` | 100 | Draws a map's full-screen effects (blits, screen textures) at 25–100% of the eye resolution: faster, but blurrier. |
| Plain Notes And Sabers | `disableCustomNoteVisuals` | off | Keeps the game's own notes, sabers and debris instead of the map's custom ones. |
| Submit Scores On Vivify Maps | `submitScoresOnVivifyMaps` | on | Sends scores and replays to the leaderboards on Vivify maps. |

Also on this screen: **Convert All PC Bundles Now**, which converts every map
that only has a PC bundle so it can be played (already-converted maps are
skipped), and its progress line.

## Advanced

### Performance Mode

| Menu name | Key | Default | What it does |
|---|---|---|---|
| Lowest Resolution % | `performanceModeFloorPercent` | 60 | How far Performance Mode may lower the resolution (40–90). |
| Also Scale Maps That Read Depth | `performanceModeScaleDepthMaps` | off | Lets Performance Mode scale maps whose effects read the depth texture; their soft edges can shift. |
| Learn Each Map | `performanceModeLearnedProfiles` | off | Remembers which parts of a map are heavy and eases down before them on later plays. **Forget Learned Maps** deletes what it remembered. |
| Aim For Half The Refresh Rate | `performanceModeHalfRefresh` | off | Aims for 60 fps at 120 Hz (36 or 45 at 72 or 90 Hz) instead of the full rate. |

### How maps look

| Menu name | Key | Default | What it does |
|---|---|---|---|
| AudioLink | `audioLink` | on | Lets map effects react to the music, as on PC. |
| Stand-Ins For Unsupported Shaders | `standInShading` | on | Draws objects whose shader cannot run in a plain colour instead of not at all. |
| Scene Depth For Map Shaders | `sceneDepthTexture` | on | Gives map shaders the depth texture they read for fog and soft edges. |
| Emulate Geometry-Shader Effects | `emulateGeometryShaderEffects` | on | Draws known wireframe, exploding-triangle and grass effects without a geometry stage, which the headset cannot run in its stereo mode. *Reconverts.* |
| Emulate Geometry Effects On Particles | `emulateParticleGeometry` | on | Bakes particle bursts that hold still into one mesh, so those effects draw on them too. |
| Draw Other Geometry Effects Without It (experimental) | `drawGeometryShaderEffects` | off | Draws the remaining geometry-shader effects without their geometry stage instead of as stand-ins; often wrong. *Reconverts.* |
| Blit Fix For Screen Textures (experimental) | `blitScreenTextureFix` | off | Builds a second, single-view copy of converted shaders for effects drawn into screen textures. |
| Map Realtime Shadows | `mapRealtimeShadows` | off | Lets maps turn on realtime shadows. Costs frame time. |
| Skip All Full-Screen Effects | `disableAllBlits` | off | Skips every full-screen effect a map draws: faster, but the map looks different. |
| Pause Offscreen Animations | `offscreenCullingOptIn` | off | Stops animating map objects that are out of view. An object whose own animation brings it into view can stay stuck. |
| Far Culling Distance m (0 = off) | `farCullingDistance` | 0 | Hides everything farther away than this (0–1000 m): faster, but far scenery disappears. |
| Disable Vivify Visuals In Multiplayer | `disableVisualsInMultiplayer` | on | Keeps map visuals off in multiplayer lobbies. |

### PC maps

| Menu name | Key | Default | What it does |
|---|---|---|---|
| Convert PC Maps On The Headset | `convertPcBundlesOnDevice` | on | Converts maps that only have a PC bundle, so they can be played. |
| Prepare Shaders Before Playing | `prepareShadersInMenu` | off | Compiles a map's shaders in the menu instead of in the first seconds of play. Its default does not change without the tester's say-so. |
| Use Only PC Bundle For Missing Shaders | `pcBundleForMissingShaders` | off | Plays a Quest map from its PC build when the Quest bundle shipped shaders empty, instead of patching those shaders in. |
| Replay Render Mode | `replayRenderMode` | off | Builds converted maps with single-view programs only, for replay and recording renderers. Kept in a cache of its own. |
| Crash Guard | `crashGuard` | on | After two loads of a converted map that never finish, plays it without translated shaders (stand-ins) so it can be played at all. |

Also here: **Force Reconvert All (ignore cache)** (press twice), which
converts every PC map again and clears crash-guard strikes.

### Other

| Menu name | Key | Default | What it does |
|---|---|---|---|
| Reuse Note And Debris Copies | `poolNoteReplacements` | on | Reuses custom note and debris objects instead of making new ones for every note. |
| Show Developer Options | `showDeveloperOptions` | off | Shows the Debug section. New in naomi.10; it exists because the Debug section must be hidden by default. |

Also here (each press twice): **Reset All Vivify Settings To Defaults**,
**Reset Vivify (clean slate)** (deletes converted maps and the PC bundles
Vivify downloaded into Quest maps' folders, and resets settings), and
**Redownload All Vivify Maps**. Below them are the paths of the diagnostic
files.

## Debug (Show Developer Options)

### Diagnostics

| Menu name | Key | Default | What it does |
|---|---|---|---|
| Debug Logging | `vivifyDebugLogging` | off | Writes far more detail to the session log. |
| Slow-Frame Sampler | `slowFrameSampler` | on | During frames over 100 ms, records where the main and render threads were, summed per song section in the log. Costs nothing while frames are fast. |
| Debug: Fixed Resolution % (0 = off) | `performanceModeFixedScalePercent` | 0 | Holds the resolution at this scale (40–95) for a whole song, for A/B tests. |
| Debug: Switch Off For A Cost Test | `debugCostTest` | Nothing | Switches one kind of drawing off for the song (skies, transparent effects, depth pre-pass, emulated geometry, blits, scenery), so the log shows its cost. The map looks wrong while it is set. |
| GPU Instancing For Scenery (experimental) | `sceneryGpuInstancing` | off | Keeps converted scenery's GPU instancing, as on PC. *Reconverts.* |

### Switches for isolating a problem

| Menu name | Key | Default | What it does |
|---|---|---|---|
| Translate Shaders On Conversion | `translateShadersOnConversion` | on | Off: converted maps keep no shaders of their own and draw as stand-ins. Reconvert after changing it. |
| Disable Beat 0 Filmgrain Blit | `disableBeat0FilmgrainBlit` | off | Skips a film-grain effect a map starts at beat 0. |
| Disable CreateCamera Depth | `disableCreateCameraDepth` | off | Gives the cameras a map creates no depth texture. |
| Disable VR Center Adjust Handling | `disableVRCenterAdjust` | off | Skips the game's room recentering while a Vivify map plays. |

## Config file only

| Key | Default | What it does |
|---|---|---|
| `standInShaderName` | empty | A shader name from the session log's shader list to use for stand-ins; empty picks one automatically. |

## Removed in naomi.10

| Key | Why |
|---|---|
| `multipassRendering` | Nothing read it. The feature it switched has been hard-wired off since it corrupted a global shader property (notes and menus went invisible); the saved value was ignored. |
| `pcShadersForEmptyQuestShaders` | Nothing read it. Empty Quest shaders have always got their PC build since 0.14.10, and its getter had no callers. |

Both are dropped from the config file on the first launch of naomi.10.

## What changed in naomi.10

No default changed, and no setting behaves differently. The menu is in the
three parts above, some names are plainer (the keys did not change), every
control has a hover hint, and there is a reset-to-defaults button.

| Old name | New name |
|---|---|
| Performance Mode For Heavy Maps | Performance Mode |
| Effect Resolution % (lower = less lag) | Effect Resolution % |
| Disable Custom Note Visuals | Plain Notes And Sabers |
| Performance Mode: Lowest Resolution % (and the other "Performance Mode:" settings) | the same name without the prefix, under Performance Mode |
| Forget Learned Performance Profiles | Forget Learned Maps |
| Stand-In Shading For Unsupported Shaders | Stand-Ins For Unsupported Shaders |
| Draw Geometry-Shader Effects (experimental) | Draw Other Geometry Effects Without It (experimental) |
| Disable All Blits | Skip All Full-Screen Effects |
| Offscreen Culling (less lag) | Pause Offscreen Animations |
| Convert PC Bundles On Device | Convert PC Maps On The Headset |
| Crash Guard (grey fallback after 2 freezes) | Crash Guard |
| Replay Render Mode (converted maps) | Replay Render Mode |
| Slow-Frame Sampler (diagnostics) | Slow-Frame Sampler (now under Debug) |
| Debug logging | Debug Logging (now under Debug) |

Moved to Debug: Debug Logging, Slow-Frame Sampler, Debug: Fixed Resolution %,
Debug: Switch Off For A Cost Test, GPU Instancing For Scenery, Translate
Shaders On Conversion, Disable Beat 0 Filmgrain Blit, Disable CreateCamera
Depth and Disable VR Center Adjust Handling.

## Adding a setting

- Add it to the table in `src/VivifySettings.cpp`, bind it in `main.cpp`'s
  `kBoundSettings`, add its control to the menu, and list it here.
- A switch that is only for tests goes in Debug.
- A new Everyday setting needs its reason stated here, and the Everyday list
  must still fit on one screen (the host test allows four settings).
- Never reuse or rename a key. When an old saved value must not carry over,
  give the setting a new key (as `prepareShadersInMenu` and
  `offscreenCullingOptIn` did) and add the old one to the removed list.
- Anything that makes a level look different from PC is off by default.
