# Vivify logging

What Vivify records about a session, where, what it costs, and the tools that
read it. As of naomi.10 (log schema 2).

Three layers:

1. **The session log** (always on): one text file per launch with a header,
   level loads, per-song performance, and Vivify's own messages.
2. **The black box** (always on): the last 4096 detailed events, kept in a
   memory-mapped file that survives a crash.
3. **Detailed mode** (Debug setting "Detailed Logging", off): a line per
   frame, Vivify's own cost by part, and detail around slow frames.

None of it changes how a map looks. Layers 1 and 2 aim to cost the game's
threads under 0.1 ms a frame on average, and every song's summary reports
what they actually cost (see Costs).

## Files

All under `/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/`:

| File | What |
|---|---|
| `Sessions/Vivify_<date>_<time>_<build>-<commit>.txt` | One per launch, e.g. `Vivify_2026-10-25_01-47-12_naomi.10-beta.1-89f98f5.txt`. Local start time; the build is the version after `0.14.25-`; the commit is cut to 7 characters (`-dirty` kept). |
| `Sessions/index.txt` | One tab-separated line per saved session: file, start time, build, maps played (folder keys), how it ended, size. |
| `Sessions/latest.txt` | The name of the current session's file. |
| `Sessions/blackbox.bin` | The black box (512 KB). |
| `Sessions/last-summary.txt` | Build, game version, mods and settings at the last launch, for "changed since the last session". |
| `VivifySession.txt` | naomi.9 and earlier kept their one log here, replaced at each launch. The first naomi.10 launch moves that last log into Sessions/ as a session of its own; afterwards this file only says where the logs are. |
| `vivify_global*.txt` | Every session from 0.14.23 to naomi.9, appended. Kept as history; no longer written (one line at its end says so). |
| `VivifyReport.txt` | The per-level report, unchanged. |

**Kept:** the 10 most recent sessions and the 5 most recent that ended
unexpectedly, within 96 MB in total; older ones are deleted at launch (the
log says how many). One session file stops growing at 48 MB.

**How a session ended** (the index's `ended`, settled at the next launch):

- `clean`: quit through the game. The file ends with a footer line starting
  `=== session ended cleanly`.
- `background`: closed while in the background. This is how the headset
  usually quits an app, without a quit callback; the black box was in its
  paused state.
- `unexpected`: stopped while running, from a crash, a hang the system
  killed, or power loss. The next launch appends the black box to that
  session's file and writes a `previous session ... ended unexpectedly` line,
  with its last events, near the top of the new session.
- `running`: the current session, or one not yet settled.
- `unknown`: the naomi.9 log moved in from VivifySession.txt.

## Line format

The first four lines are plain:

    === Vivify session log, schema 2 ===
    build: 0.14.25-naomi.10-beta.1 · Twilight Sparkle
    commit: 89f98f5d26
    started: 2026-10-25 01:47:12 -0700 (local time, UTC-0700)

Every line after them is:

    01:47:12.345 s=67.20 [TAG] text

Local wall-clock time to the millisecond, then the song time (`s=-` outside a
song), then a tag. Lines from Vivify's PaperLogger calls are stamped when
Paper hands them over, usually within a few milliseconds of the call.

| Tag | What |
|---|---|
| `HEADER` | The rest of the header (below). |
| `SESSION` | The previous session's end, files deleted, pause, resume, quit. |
| `INFO`, `WARN`, `ERROR`, `DEBUG` | Vivify's own messages, as before. |
| `CRUMB` | Breadcrumbs: each step of a level load and start, with ms since launch, GC counts and the managed heap. |
| `WATCHDOG` | Main-thread stalls and their stack samples. |
| `LOAD` | One line per level start: every load phase timed from its begin and end breadcrumbs, and level start to the first frame. |
| `ASSETS` | One line per level load: assets and prefabs; every shader its materials use, how many materials and how many draw a stand-in; textures by Unity format number. |
| `PERF` | The song summary (below). |
| `GL` | GL errors since the last check (every 2 s in a song). |
| `FRAME`, `COST`, `SLOW`, `WHITE` | Detailed mode only. |

Repeated lines are rate-limited: the first 8 with the same text (digits
ignored) in 10 s are written, then one line says how many more were left
out. Summaries and the header are never limited.

Personal details are scrubbed before writing: e-mail addresses, and the
values after `user=`, `username=`, `account=`, `player=`, `ssid=` and
`serial=` become `<hidden>`. Vivify never asks for the device serial or
account names; map names and paths are kept.

### Header

At launch: the converter's cache version; the headset model and OS build
(system properties); total and available memory; battery level, temperature
and state; the hottest thermal sensor; the GPU clock; whether the black box
opened; then every Vivify setting (`settings: key=value ...`).

On the first frame: Beat Saber and Unity versions; Unity's SystemInfo (device
model, OS, GPU name, vendor and driver version, graphics and system memory,
cores); the refresh rate; every loaded mod with its version and its file's
time; and `changed since the last session: ...` (build, commit, game, OS,
mods added, removed or updated, settings changed).

On the render thread, through Unity's plugin-event call (no hook): the GL
vendor, renderer, version and GLSL version, the number of extensions, and
which of the ones Vivify cares about are present or missing (multiview,
multisampled render-to-texture, geometry shaders, framebuffer fetch, float
colour buffers, ASTC, timer queries, tiled rendering, foveation and more).

### Song summary (`PERF`)

As before (frame times, worst frames, sections, the slow-frame sampler,
performance mode). New in naomi.10, under each section's line:

    'prefab_intro_2' frame times: under 9 ms 0.0%, 9-12 0.0%, ..., 100+ 0.0%;
    dropped about 7000 of 10800 refreshes (64.8%); garbage collections 1 (the
    slowest frame with one 31 ms); memory peak 1290 MB resident, native heap
    310 MB; hottest sensor 44.5 C, battery 33.0 C; GPU clock average 587 MHz

The bands are whole milliseconds: under 9 is one refresh at 120 Hz, under 12
at 90 Hz, under 14 at 72 Hz, under 17 holds 60 fps. A frame of n refreshes
dropped n - 1 of them. Memory and heat come from samples every 2 s, read on
the log's own thread.

When the slow-frame sampler finds the render thread inside the GPU driver's
shader compiler, it adds `render thread in the GPU driver's shader compiler
(...) in N% of samples: first-use shader compiles`.

Every performance-mode change (viewport scale, foveation, GPU boost) is
listed with its reason, as before, and also goes into the black box.

At the end, `logging cost on the game's threads, per frame on average:
session log ... ms, black box ... ms, detailed mode ... ms`.

## The black box

Sessions/blackbox.bin holds 4096 records of 128 bytes in a ring: each frame
of a song (frame time, GPU time, viewport scale), breadcrumbs, warnings and
errors, level loads and ends, cameras Vivify creates (target size), render
targets (size, format), performance-mode changes, custom events (type and
beat), GL errors, and pause, resume and quit. That is about the last 30 s of
a song.

A record is written with plain stores into a shared mapping of the file, so
the kernel keeps it even when the game crashes or is killed: no signal
handler is involved (the crash guard's handlers are untouched) and no system
call is made per record. The file's state says whether the session is
running, paused (in the background) or quit. The next launch reads it before
starting its own, and for a session that did not quit cleanly appends the
records to that session's file as `=== black box: the last N events before
this session ended (unexpected) ===`.

## Detailed mode

Debug setting "Detailed Logging" (Show Developer Options), off by default.
During songs it adds:

- `FRAME`: every frame's time, GPU time, GPU busy, viewport scale, Vivify's
  own time and garbage collections.
- `COST`: once a second, Vivify's per-frame work by part (particle bakes,
  animations, blits, render settings and cameras, notes and sabers).
- `SLOW`: for the first three frames over 50 ms in each section, the
  renderers in view grouped by material and shader, with the last Vivify event
  and breadcrumb; the first one also writes the render state (lights,
  ambient, fog, quality settings, largest textures).

- `WHITE` (naomi.10-beta.2): four times a second of song, the frame as it
  leaves Vivify is copied to a 16x16 texture and its left eye read back
  (AsyncGPUReadback where Unity supports it on the headset; otherwise a small
  ReadPixels once a second, which stalls a little). When nine in ten samples
  are near white, one line says so with everything that could be drawing it:
  every active Blit with its shader, order, end time and whether it reads the
  screen as 2D; screen textures; Vivify's cameras; the main camera's clear
  flags and background; fog; the last Vivify event and breadcrumb. Another
  line says when it ends, and the song summary counts the stretches.

Vivify cannot see individual draw calls (Unity loads GL through its own
table), so the slow-frame detail is per renderer and material.

Two warnings explain white or washed-out views even with detailed mode off
(naomi.10-beta.2): `Vivify screen texture check:` at level select, for a
shader that reads the camera's depth (or another camera texture) as one plain
2D texture in its two-eye program, and `Vivify Blit '...' reads the screen as
one plain 2D texture`, when such a Blit starts, with the song times it covers.

## Costs

All file writing happens on a thread of its own ("VivifyLog"). The game's
threads format a line's prefix and queue it, or store a black-box record.

Measured on the laptop (host tests, optimised build): queuing a log line
0.37 µs; a black-box record 0.02 µs. A song at 120 Hz writes one black-box
record per frame plus a few lines a second, so layers 1 and 2 come to a
fraction of a microsecond per frame. On the headset, each song's summary
reports the real figure per layer. Detailed mode adds a line per frame (about
1 µs to queue) and its timers.

## Tools

- `tools/pull/pull_sessions.py <dest>`: pulls index.txt, latest.txt,
  last-summary.txt and every session over adb (read-only), never overwriting
  a copy already pulled, and writes a digest beside each new session.
- `tools/logdigest/logdigest.py <session file>...`: a one-page summary per
  session: how it ended, crash evidence (the previous session's end,
  watchdog lines, errors, stalls, the black box), maps played, the slowest
  sections, what was drawn gray and why, shader-compile stalls, memory and
  heat peaks, GL errors, logging cost, and what changed. `--index index.txt`
  lists the saved sessions. It reads naomi.9's logs too.

## Changing the format

Bump `kSchemaVersion` in `include/VivifySessionLog.hpp` when a line that
logdigest reads changes, and update logdigest and its tests in the same
commit.
