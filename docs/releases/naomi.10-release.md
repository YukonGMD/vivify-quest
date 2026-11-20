# Vivify for Quest 0.14.25-naomi.10-release (Twilight Sparkle)

An unofficial community build of Aeroluna's Vivify for **Beat Saber 1.40.8**
(`1.40.8_7379`) on Meta Quest, based on trail-b1az3r's Quest port. It plays
Vivify maps on the headset and converts PC-only maps there.

**Not made, endorsed or supported by Aeroluna, Beat Games or Meta.** Please
report problems here, not to them.

Tested on **one Quest 3S** only. Quest 2, Quest 3 and Quest Pro have not
been tried.

## Before you install

- **Turn off automatic updates for Beat Saber.** A game update removes your
  mods, and this build only works on 1.40.8.
- **Back up** `/sdcard/ModData/com.beatgames.beatsaber/` from the headset.
  It holds your mods' files and settings.
- Install `Vivify-naomi-10-release.qmod` with ModsBeforeFriday (**Upload
  Files**). It replaces whichever Vivify version was installed.
- **Your Vivify settings are reset to their defaults once** at the first
  launch after a build older than naomi.10-rc.1 (logging choices are kept).
  A config that rc.1 already reset is not reset again. SETTINGS.md lists
  every default.
- Converted PC maps are converted again, once, the next time each one loads
  after a build that converts differently, so that first load takes longer.

## What naomi.10 changes

The highlights since upstream 0.14.25's Quest port; CHANGELOG.md has every
change.

### Maps

- **White screens in converted PC maps are fixed** (743 Aether, Get Your
  Wish: upstream issues #67 and #69). Screen and depth textures are read the
  way the Quest stores them.
- **Lifelike's water is no longer mirrored** (Upright Screen Reads).
- **743 Aether no longer freezes the headset.** The GPU-instancing switch
  that caused it is gone.
- **Geometry-shader effects** (wireframes, exploding triangles, grass) are
  drawn for three known shaders, including on still particle bursts,
  instead of as gray stand-ins.
- **A 16-second stall** at the start of some levels, in the game's saber
  trail, is guarded against.
- Crashes when a level ended or another map was selected, and several
  conversion-cache mistakes, are fixed.

### Settings

- **The menu is in three parts:** Everyday (fits one screen), Advanced
  (grouped by purpose, with the reset buttons) and Debug (hidden unless
  "Show Developer Options" is on).
- **Every default was checked** against the original Quest port and against
  "looks like PC", and test builds' leftover values are reset once.
- **"Reset Vivify (clean slate)" leaves map folders alone.** It deletes only
  converted maps and resets settings.
- **New, off by default:** Performance Mode (lowers the resolution while a
  heavy map runs slow), and the Debug experiment "Bake Large Particle
  Effects" (below).

### Logs

- A saved log for every launch in `Mods/Vivify/Sessions/`, a black box that
  survives a crash, per-song performance summaries, and Detailed Logging
  (Debug) for stutters. E-mail addresses and account-like values are hidden.
- `tools/pull` copies the logs off the headset and `tools/logdigest`
  summarises each session on one page.

## Known issues

- **Heavy maps run below the refresh rate**; the GPU is the limit (743
  Aether's intro and lattice, By The Wind at about 30 fps).
- **743 Aether's wireframe section (2:30-2:34) shows black squares and runs
  slower** (about 41 fps there in the one logged play). Its rock particles
  and ground fog use a wireframe effect that cannot be drawn here, and
  their stand-ins come out as solid black cubes and squares. With Stand-Ins
  off they are not drawn at all.
- **Experimental, off:** "Bake Large Particle Effects" (Debug) draws those
  rock particles with their real wireframe effect. It has not been tried on
  the headset yet; it uses about 54 MB more memory in Aether and some GPU
  time. The ground fog stays black.
- Some other effects draw as gray stand-ins (GRAY.md lists them). Get Your
  Wish's censor effect shows a default texture. Fog in maps that ship their
  own Quest fog shader comes out wrong.
- Lifelike's water is almost invisible: whether it darkens with depth as on
  PC is unverified.
- Burning Sands paused for about a second three times on its first play;
  unverified whether it happens again.
- The game has sometimes crashed at start-up while other mods loaded. This
  was never traced to Vivify.

## Reporting a problem

Open an issue on [YukonGMD/vivify-quest](https://github.com/YukonGMD/vivify-quest/issues).
Say which map, the song time, what you saw and what you expected, and
attach the session file from `Mods/Vivify/Sessions/` and `index.txt` (the
README says where they are and what they contain). Read a log before you
post it.

## Credits

Vivify is by [Aeroluna](https://github.com/Aeroluna/Vivify). The Quest port
this build is based on is [trail-b1az3r/vivify-quest](https://github.com/trail-b1az3r/vivify-quest)
by Rayla, which merges the earlier Quest ports by axo-lotl, Braxed,
Lars27110, webbs7524-wq, PATTT160 and gamesbeash-art. AudioLink is by
llealloo and contributors. The README lists everyone, and
THIRD_PARTY_NOTICES.md the licences.

## Build

- Tag `0.14.25-naomi.10-release`, commit `41c7864aae`. The session log header
  names the commit.
- `Vivify-naomi-10-release.qmod` SHA-256:
  `1c48dccbbfc79d4b2fd0646eee003670ec83f46aa723af5cb7af6c7145a2c97a`
