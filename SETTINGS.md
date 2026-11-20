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
carry over between versions by key, with one exception: the first launch of
naomi.10-rc.1 or later resets them once (see "The one-time reset" below). A
missing, mistyped or out-of-range value becomes the default. "Reset All
Vivify Settings To Defaults" (Advanced, press twice) puts every setting
except the three logging settings back to its default; maps and converted
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
**Reset Vivify (clean slate)** (deletes converted maps and resets settings;
since rc.1 it leaves map folders alone), and
**Redownload All Vivify Maps**. Below them are the paths of the diagnostic
files.

## Debug (Show Developer Options)

### Diagnostics

| Menu name | Key | Default | What it does |
|---|---|---|---|
| Debug Logging | `vivifyDebugLogging` | off | Writes far more detail to the session log. |
| Detailed Logging | `detailedLogging` | off | Layer 3 of the session log (LOGGING.md): a line per frame during songs, Vivify's own cost by part every second, and detail around slow frames. New in naomi.10. |
| Slow-Frame Sampler | `slowFrameSampler` | on | During frames over 100 ms, records where the main and render threads were, summed per song section in the log. Costs nothing while frames are fast. |
| Debug: Fixed Resolution % (0 = off) | `performanceModeFixedScalePercent` | 0 | Holds the resolution at this scale (40–95) for a whole song, for A/B tests. |
| Debug: Switch Off For A Cost Test | `debugCostTest` | Nothing | Switches one kind of drawing off for the song (skies, transparent effects, depth pre-pass, emulated geometry, blits, scenery), so the log shows its cost. The map looks wrong while it is set. |

### Switches for isolating a problem

| Menu name | Key | Default | What it does |
|---|---|---|---|
| Two-Eye Screen Textures | `screenTexturesAsArrays` | on | Converted shaders read the screen and the depth texture as the Quest's two-eye array, as Unity's own two-eye build does. New in naomi.10-beta.2: the fix for Blits that covered the view in white (743 Aether, Get Your Wish). Off puts back the old reads, for comparison. These switches act on converted PC maps; PC shaders merged into a Quest map and the Blit Fix copy always use the defaults. *Reconverts.* |
| Upright Screen Reads | `uprightScreenReads` | on | PC renders its eye textures upside down (Direct3D render targets), and some PC shaders read the screen in a way that is only right because of that: Lifelike's water showed the top half of the view mirrored into the bottom half. Converted shaders read the screen the right way up, as on PC. New in naomi.10-beta.4. Off puts back the old reads, for comparison. These switches act on converted PC maps; PC shaders merged into a Quest map and the Blit Fix copy always use the defaults. *Reconverts.* |
| Upright Reads: Projection Sign Only | `uprightReadsProjectionSignOnly` | off | A test of a narrower fix than Upright Screen Reads: it may only flip the sign of a `_ProjectionParams.x` read, in either stage (the factor ComputeScreenPos and Clear Water's own conversion multiply by), nothing else. On the tester's library it gives Lifelike's water the same picture as Upright Screen Reads, and leaves Through The Screen's particles and Before The Infinite Eclipse's black hole upside down, because their wrong row comes from a Direct3D constant Unity baked into the PC program. Only does anything with Upright Screen Reads on. New in naomi.10-beta.5. These switches act on converted PC maps; PC shaders merged into a Quest map and the Blit Fix copy always use the defaults. *Reconverts.* |
| Translate Shaders On Conversion | `translateShadersOnConversion` | on | Off: converted maps keep no shaders of their own and draw as stand-ins. Reconvert after changing it. |
| Disable Beat 0 Filmgrain Blit | `disableBeat0FilmgrainBlit` | off | Skips a film-grain effect a map starts at beat 0. |
| Disable CreateCamera Depth | `disableCreateCameraDepth` | off | Gives the cameras a map creates no depth texture. |
| Disable VR Center Adjust Handling | `disableVRCenterAdjust` | off | Skips the game's room recentering while a Vivify map plays. |

### Experiments

| Menu name | Key | Default | What it does |
|---|---|---|---|
| Bake Large Particle Effects (experimental) | `bakeLargeParticleSystems` | off | A still particle effect on a geometry-shader effect is baked into one mesh so the effect draws (Emulate Geometry Effects On Particles), up to 400,000 vertices. With this on, a larger one (743 Aether's wireframe 'rock particles' at 2:30: 1000 particles of 876 vertices) is built instead as meshes of up to 65,535 vertices, one a frame, up to 1,000,000 vertices (about 62 MB) in all. Each mesh holds the particles in one wedge of directions, so wedges behind you are culled. Until the last mesh is in, the particles keep their stand-in. With Detailed Logging on, the log has a line per mesh with its build time and, every 2 s, how many meshes were in view. New in naomi.10-release; not yet tried on the headset. |

## Defaults

The rule for the release (naomi.10-rc.1): a new player gets Vivify as it
normally behaves, and nothing experimental or test-only is on.

- A setting the original Quest port (trail-b1az3r/vivify-quest, 0.14.25)
  already had keeps that port's default. Upstream PC Vivify has no settings
  of its own beyond Camera2 and downloads, so the port is the reference.
- A setting added since (naomi series): on only if it makes maps look more
  like PC; off if it makes a map look different from PC.
- Performance Mode stays off and Effect Resolution stays at 100%.
- The Debug section is hidden, and every Debug switch is off except the
  fixes whose switch-off brings a bug back, and the Slow-Frame Sampler, a
  logging layer.
- The three logging settings (Debug Logging, Detailed Logging, Slow-Frame
  Sampler) keep the defaults they had.
- A setting that would only make things faster or steadier is on by default
  only when its output is proven identical by the host tests or the 28-map
  library and it has run on the headset in full plays. None qualifies yet:
  Prepare Shaders Before Playing does not change any converted file (it binds
  each material once in the menu) and has run in the tester's full plays,
  but what it changes is drawing on the headset, which neither proof covers.

No default changed for rc.1; every one was checked against the rule:

| Setting | Default | Why |
|---|---|---|
| Performance Mode | off | Changes how a map looks (lower resolution). |
| Effect Resolution % | 100 | Lower values blur effects. |
| Plain Notes And Sabers | off | Port default; on hides the map's notes. |
| Submit Scores On Vivify Maps | on | Port default; PC Vivify never blocks scores. |
| Lowest Resolution %, Also Scale Maps That Read Depth, Learn Each Map, Aim For Half The Refresh Rate | 60, off, off, off | Only act with Performance Mode on. |
| AudioLink | on | Port default; as on PC. |
| Stand-Ins For Unsupported Shaders | on | Port default: an object whose shader cannot run is drawn plain instead of missing. |
| Scene Depth For Map Shaders | on | Port default; maps read depth on PC. |
| Emulate Geometry-Shader Effects | on | Added in the naomi series; draws effects that are missing otherwise, closer to PC. |
| Emulate Geometry Effects On Particles | on | Same, for still particle bursts. |
| Draw Other Geometry Effects Without It | off | Port default; experimental, often wrong. |
| Blit Fix For Screen Textures | off | Port default; experimental. |
| Map Realtime Shadows | off | Port default, for frame time. PC draws the shadows a map asks for; turning it on is the player's choice. |
| Skip All Full-Screen Effects | off | Port default; on changes how maps look. |
| Pause Offscreen Animations | off | Added in the naomi series as an opt-in: objects can stay stuck, unlike PC. |
| Far Culling Distance | 0 (off) | Port default; hides far scenery. |
| Disable Vivify Visuals In Multiplayer | on | Port default (PC has no such switch; the port keeps lobbies plain). |
| Convert PC Maps On The Headset | on | Port default; PC-only maps cannot play otherwise. |
| Prepare Shaders Before Playing | off | Port default; see the speed rule above. |
| Use Only PC Bundle For Missing Shaders | off | Port default. |
| Replay Render Mode | off | Port default; for replay renderers only. |
| Crash Guard | on | Port default. |
| Reuse Note And Debris Copies | on | Added in the naomi series; upstream PC Vivify pools these copies too, and every headset test since naomi.4 ran with it. |
| Show Developer Options | off | Hides the Debug section. |
| Debug Logging, Detailed Logging | off | Logging; unchanged. The session header, the inventory and the black box are always on. |
| Slow-Frame Sampler | on | Logging; unchanged. Costs nothing while frames are fast. |
| Debug: Fixed Resolution %, Debug: Switch Off For A Cost Test | 0, Nothing | Test switches. |
| Two-Eye Screen Textures | on | A fix. Off: Blits cover the view in white (743 Aether, Get Your Wish). |
| Upright Screen Reads | on | A fix. Off: Lifelike's water shows the top of the view mirrored into the bottom. |
| Upright Reads: Projection Sign Only | off | A comparison test. |
| Translate Shaders On Conversion | on | Port default, and a fix in effect. Off: every converted map draws as stand-ins. |
| Disable Beat 0 Filmgrain Blit, Disable CreateCamera Depth, Disable VR Center Adjust Handling | off | Port defaults; isolation switches. |
| Bake Large Particle Effects (experimental) | off | Added in naomi.10-release, untested on the headset: it costs memory and GPU time. Not part of the one-time reset (a config from rc.1 gets it off). |
| Stand-in shader name (config file) | empty | Picks one automatically. |

The two screen fixes and Translate Shaders stay in Debug: a player has no
reason to turn them off, and hidden there they cannot be turned off by
accident. They are switches for comparing against the old behaviour.

### The one-time reset

`Configs/Vivify.json` holds a settings version (`settingsVersion`, 1 since
naomi.10-rc.1). On the first launch of a build with a higher version than the
file has (a file from beta.5 or earlier has none), every setting except the
three logging settings goes back to its default, Debug ones included, the new
version is written, and the session log's header says so with the values
that changed (`settings reset once to this version's defaults ...`). Choices
made after that are kept as usual. "Reset All Vivify Settings To Defaults"
and "Reset Vivify (clean slate)" reset the same way and also keep the
logging settings.

## Config file only

| Key | Default | What it does |
|---|---|---|
| `standInShaderName` | empty | A shader name from the session log's shader list to use for stand-ins; empty picks one automatically. |
| `settingsVersion` | 1 | Not a setting: which one-time reset the file has had (above). |

## Removed in naomi.10

| Key | Why |
|---|---|
| `multipassRendering` | Nothing read it. The feature it switched has been hard-wired off since it corrupted a global shader property (notes and menus went invisible); the saved value was ignored. |
| `pcShadersForEmptyQuestShaders` | Nothing read it. Empty Quest shaders have always got their PC build since 0.14.10, and its getter had no callers. |
| `sceneryGpuInstancing` | "GPU Instancing For Scenery (experimental)", a Debug switch, off by default. With it on, 743 Aether's instanced scenery (the intro's terrain, the ambient section's glass terrain, the bridge's gems) took 0.5-1.2 s of GPU time a frame and 1-2 GB of memory, until the headset froze (beta.1 and beta.3 tests). Removed in beta.4: converted materials are never GPU-instanced. |

All three are dropped from the config file on the first launch of the build that removed them (naomi.10-beta.1, and beta.4 for `sceneryGpuInstancing`).

## What changed in naomi.10

No existing default changed (GPU Instancing For Scenery was removed in beta.4, above), but saved values
are reset once (above). The menu is in the
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

New in Debug: Detailed Logging, Two-Eye Screen Textures (beta.2, on), Upright Screen Reads (beta.4, on) and Upright Reads: Projection Sign Only (beta.5, off); the two that are on change converted maps' screen reads to match PC. Moved to Debug: Debug Logging, Slow-Frame Sampler, Debug: Fixed Resolution %,
Debug: Switch Off For A Cost Test, GPU Instancing For Scenery (removed again in
beta.4, see above), Translate Shaders On Conversion, Disable Beat 0 Filmgrain Blit, Disable CreateCamera
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
