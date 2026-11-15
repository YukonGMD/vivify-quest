**Untested beta, back up your data first.**

# Vivify Quest 0.14.25-naomi.10-beta.3 (Twilight Sparkle)

An **untested pre-release** of the unofficial Quest port of Aeroluna's
Vivify, for **Beat Saber 1.40.8** (`1.40.8_7379`). This exact build has not
been played on a headset yet. The naomi builds have only ever been tested
on one Quest 3S.

Not affiliated with or endorsed by Aeroluna, Beat Games or Meta.

## Before you install

- **Back up** `/sdcard/ModData/com.beatgames.beatsaber/` from the headset.
  It holds your mods' files and settings, including Vivify's.
- Install `Vivify.qmod` with ModsBeforeFriday (**Upload Files**). It
  replaces whichever Vivify version you had installed.
- Your Vivify settings carry over. Converted PC maps are converted again,
  once, the next time each one loads, so that first load takes longer.
- To go back, upload the older `.qmod` the same way.

## What changed since 0.14.25

The naomi builds were made one at a time on top of upstream 0.14.25. All of
their changes are listed here. [CHANGELOG.md](../../CHANGELOG.md) has the
full detail.

### New in beta.3 (fixes for beta.2)

- **A crash with Detailed Logging on is fixed.** With the Debug setting
  "Detailed Logging" on, beta.2 crashed the game the moment a map started
  its first full-screen effect (Lifelike at once, you - Swifter at 1:24). A
  call into Unity was made with one argument missing. With Detailed Logging
  off this never happened.
- **One session log per launch.** beta.1 and beta.2 also saved a tiny
  pointer file as a fake extra session at every launch, which pushed real
  sessions out and confused crash reports. beta.3 stops that and deletes the
  leftovers at its first launch.

### Maps that look or work better

- **White screens in converted PC maps** (beta.2, not yet confirmed on the
  headset): 743 Aether and Get Your Wish covered the view in white for
  several seconds (upstream issues #67 and #69). PC maps that ship both a
  2019 and a 2021 build are now converted from the 2021 one, as PC Beat
  Saber does. Screen and depth textures are also read the way the Quest
  stores them, which fixes depth effects in the same maps.
- **Geometry-shader effects** (wireframes, exploding triangles, grass) are
  drawn on converted maps for three known shaders, including on still
  particle bursts. Before, they were plain gray stand-ins (setting "Emulate
  Geometry-Shader Effects", on).
- **The room offset** (your recentred position) now applies in Vivify maps,
  as it does everywhere else.
- **Ambient light** follows a map's lighting changes, as on PC.
- **A depth-only shader** that failed to link and drew gray now works
  (Through The Screen).
- **Converted shaders write counted loops as plain `for` loops**, which the
  headset's shader compiler can unroll. The results are identical; any
  speed gain has not been measured.

### Crashes fixed

- Random crashes when a level ended or another map was selected.
- A map started while Force Reconvert All was converting it could not be
  loaded again until the game restarted.
- A converted map's cache could claim a conversion it didn't have: after
  Translate Shaders was turned back on, the gray version could be reused.
- Textures decoded for converted maps were never freed.
- Crash guard: three corrections to how it counts failed loads and what its
  warning says.
- Crashes that only earlier naomi builds had (743 Aether in naomi.5 and
  naomi.8) are fixed as well.

### Settings

- **The menu is in three parts.** Everyday settings fit on one screen.
  Advanced settings are grouped by purpose. Debug switches are hidden unless
  "Show Developer Options" is on. Several names are plainer, every control
  has a hover hint, and there is a button to reset all settings. No default
  changed. SETTINGS.md lists every setting.
- **New, off by default:** Performance Mode, which lowers the resolution
  while a heavy map runs slow, with options to set its floor, learn each map
  or aim for half the refresh rate. It is off because it changes how a map
  looks.
- **New, on by default:** emulated geometry effects (above), and reusing
  note and debris copies instead of making new ones for every note.
- **New Debug switches:** Detailed Logging, a cost test that switches one
  kind of drawing off, a fixed resolution for comparisons, GPU instancing
  for scenery, and Two-Eye Screen Textures (on: it is the white-screen fix,
  and turning it off brings back the old reads for comparison).
- Two settings that nothing used were removed: `multipassRendering` and
  `pcShadersForEmptyQuestShaders`.
- Force Reconvert All now needs two presses and also clears crash-guard
  strikes.

### Logs and diagnostics

- **A saved log for every launch** in `Mods/Vivify/Sessions/`. The 10 most
  recent sessions are kept, plus the 5 most recent that ended unexpectedly.
  `index.txt` lists the maps played in each session and how it ended.
- **A black box** of recent events that survives a crash. The next launch
  adds it to the crashed session's log.
- **Per-song performance summaries:** frame times by song section, GPU time
  and load, what was on screen, and where slow frames spent their time.
- **Stall diagnostics:** a watchdog for freezes, and step-by-step markers
  during level loading.
- E-mail addresses and account-like values are hidden in the logs.
- `tools/pull` and `tools/logdigest` copy the logs off the headset and
  summarise each session on one page.

## Known issues

- This build has not been played yet.
- The white-screen fix is unconfirmed: 743 Aether and Get Your Wish haven't
  been played since beta.2.
- Some effects still draw as gray stand-ins, such as several particle
  systems in 743 Aether. GRAY.md lists them.
- Heavy maps are GPU-bound on a Quest 3S: By The Wind ran at about 30 fps in
  beta.2's test.
- The game sometimes crashed at start-up while other mods loaded. This has
  not been traced to Vivify.

## What to test

1. Turn on **Detailed Logging** (Advanced → Show Developer Options, then
   Debug).
2. Play **Lifelike** for at least 10 seconds.
3. Play **743 Aether** from the start to at least 2:00. Watch 0:00-0:07 and
   1:44-1:52 for white, and 3:27-3:34 if you can.
4. Play **Get Your Wish** through 2:46-3:07 and watch for white.
5. Quit through the game and launch it once more. Each launch should have
   made exactly one file in `Mods/Vivify/Sessions/`.

Report problems on [YukonGMD/vivify-quest](https://github.com/YukonGMD/vivify-quest)
and attach the session file and `Sessions/index.txt` (see the README).

## Build

- Tag `naomi.10-beta.3`, commit `9f5bd8e4`. The build stamp in the session
  log header is `9f5bd8e458`.
- `Vivify.qmod` SHA-256:
  `d2f8097ef5141ab00161ae4c11f676318cf10e1cd9172e67726158eebc9b3fbb`
