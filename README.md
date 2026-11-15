# Vivify Quest (naomi builds)

An unofficial Quest port of [Aeroluna's Vivify](https://github.com/Aeroluna/Vivify)
for **Beat Saber 1.40.8** on Meta Quest. It plays Vivify maps on the headset.
When a map ships only a PC (Windows) asset bundle, it converts that bundle on
the headset, translating the map's DirectX shaders to OpenGL ES.

> **Unofficial.** This project is not affiliated with or endorsed by
> Aeroluna, Beat Games or Meta. Please don't report problems with it to
> them. It contains no Beat Saber binaries or game assets.

This repository is YukonGMD's fork of
[trail-b1az3r/vivify-quest](https://github.com/trail-b1az3r/vivify-quest). It
carries the **naomi** test builds, versioned `0.14.25-naomi.N` and based on
upstream 0.14.25. Upstream has moved on to its own 0.15 releases; the two lines
are separate, and installing one replaces the other.

**Current build:** `0.14.25-naomi.10-beta.3`, an **untested pre-release**.
What changed is in [CHANGELOG.md](CHANGELOG.md) and the
[release notes](docs/releases/naomi.10-beta.3.md).

## Tested on

Only **one Quest 3S** running **Beat Saber 1.40.8** (`1.40.8_7379`). Nothing
else has been tested: not Quest 2, Quest 3 or Quest Pro, and no other game
version. The mod is built for `1.40.8_7379` only.

## What it does

- **Vivify events on Quest:** custom prefabs, materials, animated
  properties, Blits (post-processing), cameras, screen textures and rendering
  settings. Blits run at the same point in the frame as on PC.
  [PARITY.md](PARITY.md) compares every PC feature with this port.
- **PC-only maps on Quest.** A map with no Quest bundle has its PC bundle
  converted on the headset the first time it loads, and the result is kept.
  The DirectX shader programs are translated to GLES for the Quest's two-eye
  (multiview) rendering. Anything that can't be translated is drawn with a
  plain stand-in instead of nothing.
- **Broken Quest shaders replaced.** A Quest bundle shader that the mapper's
  Unity left empty is replaced with the translated PC version of that shader.
- **Geometry-shader effects emulated** for three known shaders (Swifter's
  triangle explosion, Mawntee's 3D-noise wireframe, Bytesized's grass). The
  Quest can't run geometry shaders in its two-eye mode.
- **AudioLink** for maps whose shaders use it.
- **Crash guard:** a converted map that never finishes loading twice is
  played with stand-ins instead, so it stays playable.
- **Session logs** saved on the headset, one per launch, with a crash-proof
  "black box" of recent events ([LOGGING.md](LOGGING.md)).

### Seen on the headset

- naomi.10-beta.1: 16 plays of 12 Vivify maps in one session, including the
  converted PC map 743 Aether.
- naomi.10-beta.2: By The Wind and Lost in Data played through. Lifelike and
  you - Swifter crashed, but only with the Debug setting "Detailed Logging"
  on; beta.3 fixes that bug.
- Offline, every shader program in the tester's 28 Vivify maps (18,507
  programs) translates and then compiles and links with glslang.

## Known issues

- **beta.3 has not been played yet.** It fixes two bugs from beta.2: a crash
  with Detailed Logging on, and two session files per launch.
- **The white-screen fix is unconfirmed.** 743 Aether and Get Your Wish
  covered the view in white for a few seconds (upstream issues #67 and #69).
  beta.2 fixed the cause, but neither map has been played since.
- **Some objects are still drawn gray.** A stand-in is drawn where an effect
  can't run on the Quest. In 743 Aether several particle systems stay gray
  (Extending Arrows, rock particles, Ground Fog). [GRAY.md](GRAY.md) lists
  each one and why.
- **Geometry and tessellation shaders** other than the three emulated ones
  can't run on the Quest; they are drawn with a stand-in.
- **Some effects can't be copied from PC.** Get Your Wish's
  FX/CensorWorldSpace uses a grab pass, which the port doesn't handle. Lost
  in Data and Winter Wrap Up ship their own Quest fog shader, which reads
  depth in a way the Quest doesn't support; the port doesn't rewrite a
  map's Quest shaders.
- **Heavy maps are slow.** The Quest 3S GPU is the limit: in beta.2's test
  By The Wind ran at a median of 34 ms a frame, about 30 fps.
  [PERF.md](PERF.md) lists what costs the most. Effect Resolution and
  Performance Mode (below) can help.
- **The game sometimes crashed at start-up** while other mods were loading.
  These crashes were never traced to Vivify; the cause is unknown.
- **Left-handed mode** doesn't mirror a map's environment (PARITY.md).
- **PC-only integrations** (Camera2, custom saber mods, OpenXR multi-pass)
  have no Quest equivalent.

## Install with ModsBeforeFriday

**Back up first.** Copy the folder
`/sdcard/ModData/com.beatgames.beatsaber/` from the headset to your
computer. It holds your mods' files and settings, including Vivify's
converted maps and logs.

1. Turn on developer mode for your Quest and connect it to your computer
   with a USB cable.
2. Open [ModsBeforeFriday](https://lauriethefish.github.io/ModsBeforeFriday/)
   in a browser that supports WebUSB (Chrome or Edge) and connect to the
   headset. If the game isn't modded yet, let ModsBeforeFriday patch it.
   Beat Saber must be version 1.40.8.
3. Download `Vivify.qmod` from this repository's
   [releases](https://github.com/YukonGMD/vivify-quest/releases).
4. In ModsBeforeFriday, press **Upload Files** (or drag the file onto the
   page) and choose `Vivify.qmod`. Its dependencies (SongCore,
   CustomJSONData, Tracks, BSML and the others in `mod.json`) are installed
   with it. It replaces whichever Vivify version was installed before.

Settings carry over between versions. When a version changes how PC maps are
converted, each converted map is converted again, once, the next time it
loads. Expect that load to take longer.

## Settings

In the game: **Mod Settings → Vivify**. Settings that make a level look
different from PC are off by default. [SETTINGS.md](SETTINGS.md) lists every
setting with its default.

**Everyday** (the first screen):

- **Performance Mode** (off): lowers the resolution while a heavy map runs
  slower than the headset's refresh rate. The map looks softer.
- **Effect Resolution %** (100): draws full-screen effects at a lower
  resolution. Try 75 or 50 if a map lags; effects get blurrier.
- **Plain Notes And Sabers** (off): keeps the game's own notes, sabers and
  debris instead of the map's custom ones.
- **Submit Scores On Vivify Maps** (on).
- **Convert All PC Bundles Now**: converts every PC-only map up front, so
  the first play doesn't have to wait.

**Advanced** (below the first screen) is grouped by purpose:

- **Performance Mode**: its lowest resolution, whether it may scale maps that
  read depth, learning each map, and aiming for half the refresh rate.
- **How maps look**: AudioLink, stand-ins, scene depth, the geometry-effect
  emulation, realtime shadows, skipping all full-screen effects, pausing
  offscreen animations, a far culling distance, and visuals in multiplayer
  (off there by default). Skipping effects, pausing animations and far
  culling make a map faster but different from PC.
- **PC maps**: converting on the headset, preparing shaders in the menu
  before playing (off), Replay Render Mode for replay renderers, and Crash
  Guard (on). **Force Reconvert All** (press twice) converts every PC map
  again.
- **Other**: Reuse Note And Debris Copies (on), Show Developer Options
  (off), and three buttons that each need two presses: **Reset All Vivify
  Settings To Defaults**; **Reset Vivify (clean slate)**, which deletes
  converted maps and the PC bundles Vivify downloaded and resets the
  settings; and **Redownload All Vivify Maps**.

**Debug** appears only with Show Developer Options on. It holds logging
switches ("Debug Logging", "Detailed Logging") and switches for testing and
isolating problems. If any of them is not at its default, a line in the
menu says so.

## Reporting a problem

Open an issue on [this repository](https://github.com/YukonGMD/vivify-quest/issues),
not on Aeroluna's or upstream's. Say which map, the song time, what you saw,
and what you expected, and attach the files below.

All of these are in `/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/` on
the headset. To reach them, connect the headset to a computer with USB and
allow file access, or use `adb pull`.

| File | When |
|---|---|
| `Sessions/Vivify_<date>_<time>_<build>-<commit>.txt` | Always: the session in which the problem happened. Each launch writes its own file. |
| `Sessions/index.txt` | Always. It lists every saved session: start time, build, maps played, and how it ended (`clean`, `background`, `unexpected`). |
| `VivifyReport.txt` | When one map doesn't work. |
| `/sdcard/ModData/com.beatgames.beatsaber/logs2/` | After a crash. |

**After a crash,** start the game once more and wait for the menu before
copying the files. That launch adds the crash's black-box record to the
crashed session's file, and `index.txt` marks that session `unexpected`.
Detailed Logging (Debug) adds a line per frame to the session log, which
helps with stutters and visual glitches.

**What the logs contain:** the headset model and OS build, the game and mod
versions, your Vivify settings, and the names and folders of the maps you
played. Before writing, Vivify replaces e-mail addresses with `<email>`.
The values after `user=`, `username=`, `account=`, `player=`, `ssid=` and
`serial=` become `<hidden>`. Vivify never reads the device serial or account
names. Read a log before you post it.

### Tools for a PC with adb

- `python3 tools/pull/pull_sessions.py <folder>` copies every saved session,
  `index.txt`, `latest.txt` and `last-summary.txt` off the headset. It only
  reads from the headset and never overwrites a copy already pulled. It also
  writes a one-page digest beside each new session.
- `python3 tools/logdigest/logdigest.py <session file>` writes that one-page
  digest for any session. It covers how the session ended, crash evidence,
  maps played, the slowest sections, what was drawn gray and why, and what
  changed since the previous session. `--index index.txt` lists the saved
  sessions.

[log-help.md](log-help.md) has more detail on getting the files.

## Building

You need [QPM](https://github.com/QuestPackageManager/QPM.CLI), the Android
NDK (`qpm.json` asks for 27.3), CMake, Ninja, PowerShell 7 (`pwsh`) and
Python 3. See the
[BSMG Quest modding guide](https://bsmg.wiki/quest/quest-modding-intro.html).

```sh
python3 scripts/restore-deps.py   # dependencies from GitHub (or: qpm restore)
qpm s build                       # builds build/libVivify.so
qpm s qmod                        # builds and packs Vivify.qmod
```

The build writes the commit into the binary, and the session log header
shows it. A build from a tree with uncommitted changes is marked `-dirty`.
[`scripts/README-deps.md`](scripts/README-deps.md) explains the dependency
sources.

### Tests

The converter, shader translator, parsers and log tools are plain C++20 and
Python, so they build and run on a desktop.
[`.github/workflows/validate.yml`](.github/workflows/validate.yml) has every
suite and the exact commands. It includes a glslang compile of every
translated program. `tools/shaderaudit` converts a whole library of PC
bundles and compiles and links every program offline.

## Credits and thanks

- **[Aeroluna](https://github.com/Aeroluna)** wrote
  [Vivify](https://github.com/Aeroluna/Vivify), the PC mod this port follows,
  and [BSAudioLink](https://github.com/Aeroluna/BSAudioLink), whose AudioLink
  shader bundle this port uses.
- **[trail-b1az3r](https://github.com/trail-b1az3r)** (Rayla) made
  [vivify-quest](https://github.com/trail-b1az3r/vivify-quest), which this
  fork is based on. It merges the Quest ports below and adds the on-device
  converter, the DirectX-to-GLSL translator, AudioLink and the diagnostics.
- **Quest ports merged upstream:**
  - axo-lotl ([Gay-Axolotl](https://github.com/Gay-Axolotl)):
    [Vivifhy-Quest](https://github.com/Gay-Axolotl/Vivifhy-Quest), the base
    and its per-`CameraEvent` render ordering.
  - Braxed ([rbatteries1-design](https://github.com/rbatteries1-design)):
    legacy Blit aliases, the `VRCenterAdjust` hooks, and the note-visual,
    multiplayer and settings-menu fixes.
  - [Lars27110](https://github.com/Lars27110): the same fixes.
  - [webbs7524-wq](https://github.com/webbs7524-wq) and
    [PATTT160](https://github.com/PATTT160): Blit extensions.
  - [gamesbeash-art](https://github.com/gamesbeash-art): named
    `SetRenderingSettings` values and the full `RenderTextureFormat` list.
- **[llealloo](https://github.com/llealloo)** and contributors wrote
  [AudioLink](https://github.com/llealloo/audiolink).
  **[RedBrumbler](https://github.com/RedBrumbler)** wrote
  [BSAudioLink-Quest](https://github.com/RedBrumbler/BSAudioLink-Quest), the
  earlier Quest port of AudioLink.
- **Quest modding libraries:** beatsaber-hook, bs-cordl, custom-types,
  CustomJSONData, Tracks, SongCore, BSML, scotland2, paperlog, Sombrero,
  MetaCore, WebUtils and QPM, by the Quest modding community.
  [bcdec](https://github.com/iOrange/bcdec) by Sergii Kudlai decodes PC
  texture formats. Unity's [HLSLcc](https://github.com/Unity-Technologies/HLSLcc)
  was the reference for the shader translation.
- **[ModsBeforeFriday](https://github.com/Lauriethefish/ModsBeforeFriday)**
  by Lauriethefish installs it, and the
  [Beat Saber Modding Group](https://bsmg.wiki) documents Quest modding.
- **Testing:** everyone who reported issues upstream, and YukonGMD on a
  Quest 3S for the naomi builds.

## Licence

MIT, see [LICENSE](LICENSE). The licences and copyright notices of the
projects and libraries above are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Beat Saber is a game by Beat Games. Vivify is Aeroluna's. This port is a
fan project with no connection to either.
