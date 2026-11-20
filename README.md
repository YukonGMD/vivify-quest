# Vivify for Quest: naomi.10 "Twilight Sparkle"

An unofficial community build of [Aeroluna's Vivify](https://github.com/Aeroluna/Vivify)
for **Beat Saber 1.40.8** on Meta Quest. It plays Vivify maps (custom
models, shaders, post-processing effects) on the headset. When a map only
has a PC asset bundle, it converts that bundle on the headset, translating
the map's DirectX shaders to OpenGL ES.

> **Unofficial.** This build is not made, endorsed or supported by Aeroluna,
> Beat Games or Meta. Please don't report problems with it to them. It
> contains no Beat Saber binaries or game assets.

It is a fork of [trail-b1az3r/vivify-quest](https://github.com/trail-b1az3r/vivify-quest),
the Quest port by Rayla, and carries the **naomi** builds, versioned
`0.14.25-naomi.N` on top of that port's 0.14.25. naomi.10 has the codename
"Twilight Sparkle" ([CODENAMES.md](CODENAMES.md)). The port itself has moved
on to its own 0.15 releases; the two lines are separate, and installing one
replaces the other.

**Current build:** `0.14.25-naomi.10-rc.1`, a release candidate. What changed
is in [CHANGELOG.md](CHANGELOG.md).

## Requirements

- **Beat Saber 1.40.8** (`1.40.8_7379`), modded. The mod is built for that
  version only.
- **Tested on one Quest 3S only.** Quest 2, Quest 3 and Quest Pro have not
  been tried.
- **Turn off automatic updates for Beat Saber** in the headset's store
  settings. An update to a newer game version removes your mods, and Vivify
  does not work on any other version.
- **Back up first.** Copy `/sdcard/ModData/com.beatgames.beatsaber/` from the
  headset to your computer. It holds your mods' files and settings.

## Install

### With ModsBeforeFriday

1. Turn on developer mode for your Quest and connect it to your computer
   with a USB cable.
2. Open [ModsBeforeFriday](https://lauriethefish.github.io/ModsBeforeFriday/)
   in a browser that supports WebUSB (Chrome or Edge) and connect to the
   headset. If the game isn't modded yet, let ModsBeforeFriday patch it.
3. Download the `.qmod` file from this repository's
   [releases](https://github.com/YukonGMD/vivify-quest/releases).
4. In ModsBeforeFriday, press **Upload Files** (or drag the file onto the
   page) and choose it. Its dependencies (SongCore, CustomJSONData, Tracks,
   BSML and the others listed in `mod.json`) are installed with it. It
   replaces whichever Vivify version was installed before.

### With adb, over an existing install

If an earlier Vivify is already installed with its dependencies, you can
replace just the library. A `.qmod` is a zip file: take `libVivify.so` out of
it and run

```sh
adb push libVivify.so /sdcard/ModData/com.beatgames.beatsaber/Modloader/mods/libVivify.so
```

then restart the game. A mod manager keeps showing the old version number,
because it did not do the install.

### The first launch

The first launch of naomi.10-rc.1 resets Vivify's settings to their
defaults once (your logging choices are kept), because earlier test builds
let settings drift from them. Change them again afterwards if you like; they
are kept from then on. [SETTINGS.md](SETTINGS.md) lists every setting and its
default.

## How it differs from Vivify on PC

- **Shaders are converted on the headset.** The first time you play a map
  that only has a PC bundle, the headset converts it first, so that load
  takes noticeably longer (longest for big maps); the result is kept, so
  later loads are quick. A new version that converts differently converts
  each map again, once. "Convert All PC Bundles Now" does them all up front.
- **Some effects cannot run on the Quest** (geometry and tessellation
  shaders, apart from three that are emulated). Those objects are drawn in a
  plain gray "stand-in" instead of not at all. [GRAY.md](GRAY.md) lists the
  known ones.
- **Settings PC does not have** (Mod Settings → Vivify), in three parts:
  - **Everyday:** Performance Mode (off: lowers the resolution while a heavy
    map runs slow, so it looks softer), Effect Resolution % (100: draws
    full-screen effects at lower resolution, faster but blurrier), Plain
    Notes And Sabers, Submit Scores On Vivify Maps, and Convert All PC
    Bundles Now.
  - **Advanced:** grouped as Performance Mode, How maps look, PC maps and
    Other, including the reset buttons. Settings that make a map look
    different from PC are off by default.
  - **Debug:** hidden unless "Show Developer Options" is on; switches for
    testing and logging.
- **Vivify visuals are off in multiplayer lobbies** by default, as in the
  original port.
- **PC-only integrations** (Camera2, PC custom saber mods, OpenXR multi-pass)
  have no Quest equivalent. Left-handed mode doesn't mirror a map's
  environment.

[PARITY.md](PARITY.md) compares every PC feature with this port.

## Known issues

Checked against the logs of the tester's naomi.10 plays (Quest 3S, November
2026). "Unverified" means it has not been confirmed on the headset.

- **Heavy maps run below the headset's refresh rate.** The GPU is the limit.
  743 Aether's slowest parts are its intro (0:00-0:32, about 40 ms a frame),
  the lattice at 0:58-1:03 (400 large cubes drawn with a heavy noise shader,
  frames of up to about 100 ms) and the last second of the song. By The Wind runs
  at about 32 ms a frame (30 fps). Effect Resolution and Performance Mode
  help, at a cost in sharpness. [PERF.md](PERF.md) has the details.
- **Some of 743 Aether's particles are gray.** Ground Fog, Extending Arrows
  and the small wireframe cubes near you at 2:30 ("rock particles") use
  geometry-shader effects that only work on still mesh particles here; they
  draw as gray or dark stand-ins (nothing at all with Stand-Ins off). Its
  triangle-explosion bursts do draw.
- **Get Your Wish's censor effect** (FX/CensorWorldSpace) reads the screen
  through a grab pass the port does not handle, so it shows Unity's default
  texture instead of the blurred view.
- **Fog in maps that ship their own Quest fog shader** (Custom/Fog and
  Custom/FogMB, in several of slabs37's maps such as Lost in Data and Winter
  Wrap Up) reads the depth texture in a way the Quest does not support, so
  the fog comes out wrong. Vivify does not rewrite a map's own Quest shaders.
- **Lifelike's water (1:12-1:31) is almost invisible.** It draws and is no
  longer mirrored, but on PC it should also darken with depth. Whether that
  darkening works on the headset is unverified.
- **Burning Sands** paused three times for about a second at 2:01-2:06 on its
  first play. Vivify's own work was not the cause; unverified whether it
  happens again.
- **The game has sometimes crashed at start-up** while other mods were
  loading. These crashes were never traced to Vivify.
- Only one headset and one game version have been tested.

## Reporting a problem

Open an issue on [this repository](https://github.com/YukonGMD/vivify-quest/issues),
not on Aeroluna's or the original port's. Say which map, the song time, what
you saw and what you expected, and attach the files below.

They are on the headset in
`/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/`. To reach them,
connect the headset to a computer with USB and allow file access, or use
`adb pull`.

| File | When |
|---|---|
| `Sessions/Vivify_<date>_<time>_<build>-<commit>.txt` | Always: the session in which the problem happened. Each launch writes its own file. |
| `Sessions/index.txt` | Always. It lists every saved session and how it ended (`clean`, `background`, `unexpected`). |
| `VivifyReport.txt` | When one map doesn't work. |

**After a crash,** start the game once more and wait for the menu before
copying the files. That launch adds the crash's black-box record to the
crashed session's file.

**What the logs contain:** the headset model and OS build, the game and mod
versions, the other mods you have installed, your Vivify settings, and the
names and folders of the maps you played. Vivify replaces e-mail addresses
with `<email>` before writing, hides the values after `user=`, `username=`,
`account=`, `player=`, `ssid=` and `serial=`, and never reads the headset's
serial number or your account names. Still, read a log before you post it,
and remove anything you don't want to share.

For stutters and visual glitches, turn on Show Developer Options, then
Debug → Detailed Logging, play the map again and attach that session.

With adb on a computer, `python3 tools/pull/pull_sessions.py <folder>` copies
every saved session off the headset (it only reads) and writes a one-page
digest beside each. [log-help.md](log-help.md) and [LOGGING.md](LOGGING.md)
have more.

## Building

You need the Android NDK (`qpm.json` asks for 27.3; put its path in
`ndkpath.txt` or `ANDROID_NDK_HOME`), CMake, Ninja, PowerShell 7 (`pwsh`)
and Python 3. The dependencies are vendored in `extern/`;
[`scripts/README-deps.md`](scripts/README-deps.md) explains them. See the
[BSMG Quest modding guide](https://bsmg.wiki/quest/quest-modding-intro.html)
for the tools.

To build a qmod to install or share:

```sh
tools/build-release.sh naomi-10-rc.1
```

It builds the committed state in a fresh worktree, keeps build paths out of
the binary, checks the qmod for home and temp paths, and writes
`~/Vivify-naomi-10-rc.1.qmod`. For day-to-day work,
`pwsh scripts/build.ps1` builds `build/libVivify.so`.

The session log's header names the commit a build came from (`-dirty` when
it had uncommitted changes).

### Tests

The converter, shader translator, parsers, settings and log tools are plain
C++20 and Python, so they build and run on a desktop.
[`.github/workflows/validate.yml`](.github/workflows/validate.yml) has every
suite and its exact commands, including a glslang compile of every translated
program. `tools/shaderaudit` converts a whole library of PC bundles and
compiles and links every program offline.

## Credits

- **[Aeroluna](https://github.com/Aeroluna)** wrote
  [Vivify](https://github.com/Aeroluna/Vivify), the PC mod this follows, and
  [BSAudioLink](https://github.com/Aeroluna/BSAudioLink), whose AudioLink
  shader bundle is used here.
- **[trail-b1az3r](https://github.com/trail-b1az3r)** (Rayla) made
  [vivify-quest](https://github.com/trail-b1az3r/vivify-quest), which this
  build is based on. It merges the Quest ports below and adds the on-device
  converter, the DirectX-to-GLSL translator, AudioLink and the diagnostics.
- **Quest ports merged into it:**
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
- **Testing:** everyone who reported issues on the original port, and
  YukonGMD on a Quest 3S for the naomi builds.

## Licence

MIT, see [LICENSE](LICENSE). The licences and copyright notices of the
projects and libraries above are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Beat Saber is a game by Beat Games. Vivify is Aeroluna's. This build is a
fan project with no connection to either.
