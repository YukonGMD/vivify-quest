#pragma once

#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Saved session logs (0.14.25-naomi.10): the pieces that do not need Unity,
// so the host tests (tools/sessionlog) can drive them. LOGGING.md describes
// the files they produce.
namespace Vivify::SessionLog {

// Bumped whenever a line format that tools/logdigest reads changes.
inline constexpr int kSchemaVersion = 2;

// --- Lines -------------------------------------------------------------------

// "01:47:12.345 s=67.20 [INFO] text"; "s=-" outside a song (songTime NaN).
std::string FormatPrefix(std::tm const& local, int millis, float songTime, std::string_view tag);

// Personal details a line could carry, replaced: e-mail addresses, the value
// after user=, username=, account=, player=, name= and ssid= (any case),
// and device serial numbers given as serial=. Map names and paths are kept.
std::string Scrub(std::string_view text);

// The key lines are rate-limited by: digits become '#', so "frame 1201 took
// 31 ms" and "frame 1202 took 29 ms" are the same line.
std::string RateKey(std::string_view text);

// Lets the first `burst` lines with the same key through in each window of
// `windowSeconds`; the rest are counted, and when the window closes one line
// says how many were left out.
class RateLimiter {
 public:
  explicit RateLimiter(int burst = 8, double windowSeconds = 10.0) : _burst(burst), _window(windowSeconds) {}
  // Whether to write `text` now. Lines about windows that closed before
  // `now` are appended to `reports`.
  bool Allow(std::string_view text, double now, std::vector<std::string>& reports);
  // Closes every window that ended before `now`.
  void Expire(double now, std::vector<std::string>& reports);
  // Closes every window (at the end of the session).
  void ExpireAll(std::vector<std::string>& reports);
  uint64_t Suppressed() const { return _suppressedTotal; }

 private:
  struct Entry {
    double windowStart = 0.0;
    int count = 0;
    int suppressed = 0;
    std::string example;
  };
  static std::string Report(Entry const& entry);
  int _burst;
  double _window;
  std::unordered_map<std::string, Entry> _entries;
  uint64_t _suppressedTotal = 0;
};

// --- Session files -------------------------------------------------------------

// "Vivify_2026-10-25_01-47-12_naomi.10-beta.1-89f98f5.txt": local start time,
// the part of the version after "0.14.25-" (all of it when there is no
// such part), and the commit cut to 7 characters ("-dirty" kept).
std::string SessionFileName(std::tm const& local, std::string_view version, std::string_view commit);

// "+0100" style offset of local time from UTC.
std::string UtcOffset(std::tm const& local);

// How a session ended.
inline constexpr std::string_view kRunning = "running";        // still going, or not looked at yet
inline constexpr std::string_view kClean = "clean";            // quit through the game
inline constexpr std::string_view kBackground = "background";  // closed while in the background (Quest's usual quit)
inline constexpr std::string_view kUnexpected = "unexpected";  // stopped while running: crash, kill or hang

struct Session {
  std::string file;
  std::string started;  // "2026-10-25 01:47:12 -0700"
  std::string build;
  std::string maps;  // map folder keys, comma-separated
  std::string ended;
  uint64_t bytes = 0;
};

// index.txt: a header line, then one tab-separated line per session, oldest
// first. Unknown or damaged lines are skipped.
std::vector<Session> ParseIndex(std::string_view text);
std::string FormatIndex(std::vector<Session> const& sessions);

// Which session files to delete: everything except the `keepRecent` newest,
// the `keepUnexpected` newest that ended unexpectedly, and `current`; then,
// oldest first, kept ones too while the total is over `maxBytes` (never
// `current`).
std::vector<std::string> FilesToDelete(std::vector<Session> const& sessions, std::string_view current,
                                       size_t keepRecent = 10, size_t keepUnexpected = 5,
                                       uint64_t maxBytes = 96ull * 1024 * 1024);

// The line every cleanly ended session finishes with starts with this.
inline constexpr std::string_view kFooter = "=== session ended";

// How a session that the index still calls running ended: `tail` is the end
// of its file; `blackBoxState` what the black box recorded for it, if it
// recorded that session (BlackBox::State values; nullopt otherwise).
std::string_view ClassifyEnd(std::string_view tail, std::optional<uint32_t> blackBoxState);

// --- Launch and updates ------------------------------------------------------------

struct Startup {
  std::vector<Session> sessions;     // the index as this launch left it
  std::vector<std::string> deleted;  // session files removed to stay within the limits
  std::vector<std::string> notes;    // lines for the new session's log
  std::optional<Session> previous;   // the session before this one
};

// What the previous session's black box held (BlackBox::Previous, without
// the dependency).
struct PreviousBox {
  bool found = false;
  uint32_t state = 0;
  std::string session;
  std::vector<std::string> lines;
};

// At launch, in the sessions directory `dir`: reads index.txt (adding any
// session file it does not list), settles every session still marked
// running (ClassifyEnd on its tail and, for the one the black box belongs
// to, the box's state), appends the black box's records to that session's
// file when it did not end cleanly, adds `current` as running, deletes what
// FilesToDelete picks, and writes index.txt. `legacyLog` is the old single
// VivifySession.txt: when present it is moved in as a session of its own.
Startup BeginSession(std::string const& dir, Session const& current, PreviousBox const& box,
                     std::string const& legacyLog = {});

// Rewrites one session's maps, end and size in index.txt.
void UpdateSession(std::string const& dir, std::string const& file, std::string const& maps,
                   std::string_view ended, uint64_t bytes);

// --- What changed since the last session -----------------------------------------

// A session summary: "key=value" lines. Keys used: build, commit, beatsaber,
// os, mod:<id>, setting:<key>.
std::map<std::string, std::string> ParseSummary(std::string_view text);
std::string FormatSummary(std::map<std::string, std::string> const& summary);
// "build naomi.9 -> naomi.10-beta.1; mods: +foo 1.2.0, -bar, baz 1.0 -> 1.1;
// settings: mapRealtimeShadows false -> true"; "nothing" when equal; empty
// when there was no earlier summary.
std::string WhatChanged(std::map<std::string, std::string> const& before,
                        std::map<std::string, std::string> const& after);

// --- Level load timeline ---------------------------------------------------------

// Fed the breadcrumbs of a level start ("bundle preload begin: ...",
// "texture decode end; shader repair begin", ...), it times every phase
// between its "begin" and "end" and, at "first frame after level start",
// returns one line with all of them.
class LoadTimeline {
 public:
  // Returns the summary when this crumb completes a level start.
  std::optional<std::string> Feed(std::string_view crumb, double nowMs);
  void Reset();

 private:
  struct Phase {
    std::string name;
    double startMs = 0.0;
    double durationMs = -1.0;
  };
  std::vector<Phase> _phases;
  double _firstMs = -1.0;
};

}  // namespace Vivify::SessionLog
