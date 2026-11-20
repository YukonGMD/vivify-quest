# naomi.11 backlog

Work parked at the end of naomi.10 (0.14.25-naomi.10, "Twilight Sparkle").
Each item says what is known, where the evidence is, and what is left to
do. Rules that carry over: nothing may make a map look different from PC
unless it is behind a setting that is off by default; a new route stays
behind its own setting (off) until host tests and headset screenshots show
it matches; measure before and after on the headset (OVR Metrics with GPU
counters, and the session log's per-section GPU time). VERSION_X.md holds
the longer plan most of these come from.

## Looks

### Aether's remaining gray particles
743 Aether has three particle systems on geometry-shader effects that the
particle emulation cannot bake (GRAY.md has the table):

- **rock particles** (the small wireframe cubes near the player at 2:30,
  Mawntee/Beat Saber Wireframe 3D-Noise): 1000 mesh particles of 876
  vertices, over the 400,000-vertex limit for a CPU bake. A bake that size
  would cost far more than a frame.
- **Extending Arrows** (Swifter/TriangleExplosion): mesh particles that move
  (velocity module); only still particles can be baked.
- **Ground Fog** (the wireframe shader): billboard particles; only mesh
  particles can be baked.

They draw as gray stand-ins (nothing with Stand-Ins off). The planned route
(VERSION_X.md, "particles"): draw moving mesh particles by instancing the
already rebuilt mesh with each particle's transform and colour, and
billboards with a rebuilt quad facing the camera. Needs: reading particles
each frame (ParticleSystem.GetParticles), a translated shader variant with
instancing that is trustworthy (the scenery instancing switch was removed in
beta.4 because translated instanced variants were not), and bounds (below).

### Lifelike's water depth darkening
Lifelike's water (Silent/Clear Water, 1:12-1:31) draws and is upright since
beta.4, but looks almost invisible. Its material darkens what is seen
through it toward black (`_DepthColour` black over `_DepthDistance` 100 m);
looking down at open water with only distant sky behind it, PC should show
it close to black. In beta.3 the mirrored picture was not darkened either,
which suggests the depth term does not work on the headset: check what the
translated program reads from `_CameraDepthTexture` there (scene depth is on
for this map) against PC, with a screenshot pair.

### Sky draw order (skies last)
Opaque full-screen skies are drawn before the rest of the opaque queue, so
every sky pixel is shaded and then mostly covered. Drawing skies that match
(opaque, ZWrite on, LEqual, no blend, checked at load) after the other
opaque objects gives the same image with far fewer shaded sky pixels. Behind
a setting, off, until screenshots match.

### Particle and mesh bounds
Baked particle meshes and rebuilt emulated meshes need bounds that cover
what the shader draws (triangle explosions move vertices outward), or Unity
culls them early at the edge of the view. Check each against the emulated
shader's largest displacement; the session log's "in view" counts help.

## Speed

### Measure costs first (the cost milestone)
Debug → "Debug: Switch Off For A Cost Test" switches one category off for a
song (skies, transparent effects, depth pre-pass, emulated geometry, blits,
scenery). One play of Aether per category, plus plays with OVR Metrics GPU
counters on, gives a measured cost table to rank the items below. Not done
yet; every cost below is inferred.

### Bloom and raymarching in Aether
Aether's slow spots are GPU-bound: the intro (about 40 ms a frame), the
lattice at 0:58-1:03 (400 large cubes, Swifter's voronoi noise shader,
frames up to about 100 ms) and its full-screen effects (bokeh and bloom
Blits, raymarched elements). Measure each with the cost test and GPU
counters before choosing; anything that lowers quality is a setting, off.

### Depth from the main pass
With "Scene Depth For Map Shaders", every opaque renderer is drawn a second
time into a depth pre-pass, all song. A copy of the main pass's depth would
avoid that. Behind a setting, off, until the depth effects match.

### RenderTexture.DiscardContents() after Blits
The Quest's GPU renders in tiles; a render texture whose old contents are
not needed can skip loading them back from memory. Calling
DiscardContents() on Blit targets and temporaries before they are
overwritten may save memory bandwidth. Needs measuring (OVR Metrics
bandwidth counters); must not discard anything a later effect reads.

### First-use shader compiles and warm-up
The GPU driver compiles each shader variant the first time it is drawn.
"Prepare Shaders Before Playing" (off by default) binds every material once
in the menu; it ran in the tester's full plays without problems but is not
proven to change nothing on screen, so it stays off until that is shown or
the tester decides. The planned replacement warms exactly the variants a
map's materials use during the load. No compile stall has been caught by the
slow-frame sampler in naomi.10; Burning Sands' three 0.8-1.1 s main-thread
pauses at 2:01-2:06 on its first play (beta.5, GPU time normal, Vivify's
own work 0.14 ms) are the best candidate to check on a replay.

### Foveation
Research question: can the mod change fixed foveation at runtime on 1.40.8,
what does it save on the Quest 3S, and can it be raised only during heavy
effects? Known so far: Performance Mode already sets
OVRManager.foveatedRenderingLevel at runtime (to HighTop, 4) when a map runs
heavy, but only if the game's level is lower. OVR Metrics and the session
log show the game already runs at level 4 on the Quest 3S, with dynamic
foveation off, so that step never runs there, and whether a runtime change
takes effect has not been seen. Left to find out: whether
dynamic foveation or a different foveation mode is reachable from a mod on
1.40.8, and its cost and savings, measured on a heavy section.

## Correctness and housekeeping

### Mesh leak check
The menu leak check names meshes that grew from one level to the next. In
beta.5's first session, after a full set of plays, the unnamed meshes grew by
55 ('(no name)' +55, beta.5 session of 2026-11-18); later checks grew by 6.
With stand-ins off nothing grew per Aether play. Find what creates unnamed
meshes per level (baked particle meshes are named; rebuilt meshes are
cached) and whether they are freed at level end.

### Saber-trail time data
On some maps (743 Aether, at about 2 s and after a pause) the trail data
Vivify's followed saber trail reads has times like 688175448064 s or 0 s.
Since beta.5 the game's catch-up loop is capped, so it no longer stalls (it
did once, 16 s, in beta.4), and the first five cases a session are logged.
Where the bad times come from (the followed trail's ring buffer before it
fills, or a reset on pause) is not known.

### GPU memory during pauses
In one beta.5 session the game's GPU memory rose from 0.9 GB to 2.7 GB for
about 20 s while 743 Aether was paused at 1:08, and to 2.1 GB at its end
(OVR Metrics `app_gpu_physical_MB`). The other beta.5 session peaked at
1.4 GB. No crash. Watch for it with OVR Metrics running.

### Left over from the naomi.10 review
- The particle-emulation log line still says "keeps the stand-in" when
  Stand-Ins are off (the digest parses it).
- A baked particle mesh is held until the level ends if its particle system
  is destroyed some way other than DestroyObject.
- No converter-level end-to-end test of Upright Screen Reads (the planner
  has unit tests; the library check covers the rest).
- The PC-shader donor and the Blit Fix companion ignore the Debug
  screen-read switches (documented in SETTINGS.md).

## The new shader translator
VERSION_X.md's "Converter 2": first an analysis layer at conversion time
(cross-stage screen-texture detection, sky and Blit classification,
reachable variants) written as a sidecar, used only where it agrees with
the current heuristics on all 28 library maps; then a translator built on
it. A host A/B tool should render both translations of each program with a
software renderer and compare pixels, so "same image" is checked on a
computer before the headset.

## Beat Saber 1.45.3 (URP) port
Not started. Beat Saber moved to Unity 6 in 1.41.0 and to URP in 1.44.2;
1.45.3 is the newest moddable Quest version. Blockers as of November 2026:
CustomJSONData and Tracks, which Vivify depends on, have no 1.45.3 release,
and upstream PC Vivify has no URP release. Which graphics API the Quest
build of 1.45.3 uses (GLES or Vulkan) is not documented; check with
`adb logcat -d | grep -iE "vulkan|opengl es"` after launching it. The
rendering hooks (CameraEvent command buffers, OnRenderImage Blits) would
have to be redone for URP; the converter and translator mostly carry over.
Re-check the mod index before starting: these change quickly.
