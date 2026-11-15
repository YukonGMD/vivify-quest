# Getting Vivify's logs

When a map doesn't work, Vivify's own session log is the most useful thing to
send. It records what Vivify did with the map: converting its bundle,
translating shaders, stand-ins, loading, events, effects and performance.
An Android logcat is a useful extra, but it doesn't replace the session log.

This page is for naomi.10 and later. Earlier builds kept one log in
`VivifySession.txt`, replaced at every launch; from naomi.10 that file only
says where the logs are.

## Where the files are

On the headset, in `/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/`:

```
Mods/Vivify/
├── Sessions/
│   ├── Vivify_2026-10-25_01-47-12_naomi.10-beta.1-89f98f5.txt   one per launch
│   ├── index.txt          every saved session: start, build, maps, how it ended
│   ├── latest.txt         the current session's file name
│   ├── last-summary.txt   build, mods and settings at the last launch
│   └── blackbox.bin       the black box (binary; not needed in a report)
├── VivifyReport.txt       the per-level report
└── VivifySession.txt      a pointer to Sessions/
```

For crashes, also `/sdcard/ModData/com.beatgames.beatsaber/logs2/`.

## Which files to send

1. **The session file** from the launch in which the problem happened. Find
   it in `index.txt`, which lists each session's start time, build, maps
   played, and how it ended:
   - `clean`: you quit through the game.
   - `background`: the headset closed the game while it was in the
     background, its usual way of quitting an app.
   - `unexpected`: a crash, a freeze the system killed, or power loss.
   - `running`: the current session.
2. **`Sessions/index.txt`.**
3. **`VivifyReport.txt`**, when a particular map doesn't work.
4. **The `logs2` folder**, after a crash.

**After a crash,** start the game once more and wait for the menu before you
copy anything. That launch adds the black box, the crashed session's last
events, to the end of that session's file, and `index.txt` marks it
`unexpected`.

**For stutters or visual glitches,** turn on Detailed Logging (Mod Settings →
Vivify → Advanced → Show Developer Options, then Debug) and play the part
again. The log then has a line per frame. Turn it off afterwards.

Attach the files to the issue rather than pasting them. If GitHub won't take a
file, zip it first.

## How to get the files off the headset

- **USB file access:** connect the headset to a computer, allow file access
  when the headset asks, and open the headset's storage. The files are under
  `ModData/com.beatgames.beatsaber/Mods/Vivify/`.
- **adb:**

  ```sh
  adb pull /sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/Sessions
  adb pull /sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifyReport.txt
  adb pull /sdcard/ModData/com.beatgames.beatsaber/logs2
  ```

- **This repository's tools** (a computer with adb and Python 3):

  ```sh
  python3 tools/pull/pull_sessions.py pulled-logs
  ```

  This copies every saved session, `index.txt`, `latest.txt` and
  `last-summary.txt` into `pulled-logs/`. It only reads from the headset and
  never overwrites a copy it already pulled. It writes a one-page digest
  beside each new session and prints the digests. For one file:

  ```sh
  python3 tools/logdigest/logdigest.py pulled-logs/Vivify_....txt
  python3 tools/logdigest/logdigest.py --index pulled-logs/index.txt
  ```

## What a log contains

The header records the build and commit, the headset model and OS build, the
GPU and its driver, memory, battery and temperature, every Vivify setting,
and every loaded mod with its version. The rest is Vivify's own messages,
level loads, and per-song performance, each with the time and song time.

Before writing, Vivify replaces e-mail addresses with `<email>`. The values
after `user=`, `username=`, `account=`, `player=`, `ssid=` and `serial=`
become `<hidden>`. Vivify never reads the device serial or account names.
Map names and folders are kept. Read a log before you post it publicly.

[LOGGING.md](LOGGING.md) describes every file and line format in full.

## Logcat (optional)

Android's system-wide log covers the game, Unity, every mod and the system,
so it is much noisier than Vivify's log. If you can, add it as an extra:

```sh
adb logcat -c                 # clear it, then reproduce the problem
adb logcat -d > logcat.txt    # save it
```

After a crash, `adb logcat -d -b crash > crash.txt` saves the crash buffer.
Do this before restarting the headset, because a restart clears it.
