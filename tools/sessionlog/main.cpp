// Host tests for saved session logs (naomi.10): line format, scrubbing, rate
// limiting, session names, the index and its retention, how a session's end
// is read (including a process that aborts mid-session), the black box, the
// background writer, the load timeline, "what changed", and what each costs
// the thread that logs.
//
//   sessionlogtest <scratch dir>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>

#include "VivifyBlackBox.hpp"
#include "VivifyLogWriter.hpp"
#include "VivifySessionLog.hpp"

namespace fs = std::filesystem;
namespace L = Vivify::SessionLog;

namespace {

int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& name, std::string const& detail = {}) {
  if (ok) {
    passed++;
  } else {
    failed++;
    std::printf("FAIL  %s%s%s\n", name.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
  }
}

std::string ReadAll(std::string const& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

void WriteFile(std::string const& path, std::string const& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

int CountLines(std::string const& text, std::string const& needle) {
  int count = 0;
  size_t at = 0;
  while ((at = text.find(needle, at)) != std::string::npos) {
    count++;
    at += needle.size();
  }
  return count;
}

std::tm Local(int year, int month, int day, int hour, int minute, int second) {
  std::tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  t.tm_gmtoff = -7 * 3600;
  return t;
}

L::Session MakeSession(std::string const& file, std::string_view ended, uint64_t bytes) {
  L::Session s;
  s.file = file;
  s.started = "x";
  s.build = "naomi.10";
  s.ended = std::string(ended);
  s.bytes = bytes;
  return s;
}

std::string NameAt(int minute) {
  char name[96];
  std::snprintf(name, sizeof(name), "Vivify_2026-10-25_01-%02d-00_naomi.10-beta.1-89f98f5.txt", minute);
  return name;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: sessionlogtest <scratch dir>\n");
    return 2;
  }
  std::string const scratch = argv[1];
  std::error_code ec;
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch);

  // --- Line format and scrubbing ----------------------------------------------------
  std::tm const t = Local(2026, 10, 25, 1, 47, 12);
  Check(L::FormatPrefix(t, 345, std::numeric_limits<float>::quiet_NaN(), "INFO") == "01:47:12.345 s=- [INFO] ",
        "prefix outside a song", L::FormatPrefix(t, 345, std::nanf(""), "INFO"));
  Check(L::FormatPrefix(t, 5, 67.2f, "PERF") == "01:47:12.005 s=67.20 [PERF] ", "prefix in a song");
  Check(L::Scrub("mail me at someone.name+x@example.co.uk now") == "mail me at <email> now", "e-mail hidden",
        L::Scrub("mail me at someone.name+x@example.co.uk now"));
  Check(L::Scrub("login userName=Player_77 ok") == "login userName=<hidden> ok", "user name hidden",
        L::Scrub("login userName=Player_77 ok"));
  Check(L::Scrub("wifi ssid='Home Net 5G' up") == "wifi ssid='<hidden>' up", "quoted SSID hidden",
        L::Scrub("wifi ssid='Home Net 5G' up"));
  Check(L::Scrub("serial=TESTSERIAL0001, player=\"Bob\"") == "serial=<hidden>, player=\"<hidden>\"",
        "serial and player hidden", L::Scrub("serial=TESTSERIAL0001, player=\"Bob\""));
  std::string const keep =
      "Vivify main camera: name='MainCamera' level='/sdcard/ModData/x/4968d (743 Aether)' @ beat 3";
  Check(L::Scrub(keep) == keep, "camera names, paths and a lone @ are kept", L::Scrub(keep));

  // --- Rate limiting -------------------------------------------------------------------
  Check(L::RateKey("frame 1201 took 31.5 ms") == L::RateKey("frame 9 took 2.25 ms"), "numbers ignored in the key");
  {
    L::RateLimiter limiter(3, 10.0);
    std::vector<std::string> reports;
    int allowed = 0;
    for (int i = 0; i < 10; i++) allowed += limiter.Allow("GL error 0x" + std::to_string(500 + i), 1.0 + i * 0.1, reports);
    Check(allowed == 3 && reports.empty(), "burst of 3 then suppressed", std::to_string(allowed));
    Check(limiter.Allow("a different line", 2.0, reports), "other lines unaffected");
    limiter.Allow("GL error 0x600", 11.5, reports);
    Check(reports.size() == 1 && reports[0].find("left out 7 more") != std::string::npos,
          "the closed window says how many were left out", reports.empty() ? "" : reports[0]);
    Check(limiter.Suppressed() == 7, "suppressed total");
  }

  // --- Names and the index ---------------------------------------------------------------
  Check(L::SessionFileName(t, "0.14.25-naomi.10-beta.1", "89f98f5d26") ==
            "Vivify_2026-10-25_01-47-12_naomi.10-beta.1-89f98f5.txt",
        "session file name", L::SessionFileName(t, "0.14.25-naomi.10-beta.1", "89f98f5d26"));
  Check(L::SessionFileName(t, "0.14.25-naomi.10", "89f98f5d26-dirty") ==
            "Vivify_2026-10-25_01-47-12_naomi.10-89f98f5-dirty.txt",
        "dirty build name");
  Check(L::UtcOffset(t) == "-0700", "UTC offset", L::UtcOffset(t));
  {
    std::vector<L::Session> sessions = {MakeSession(NameAt(1), L::kClean, 100),
                                        MakeSession(NameAt(2), L::kUnexpected, 200)};
    sessions[1].maps = "4968d,4839c";
    std::string const text = L::FormatIndex(sessions) + "garbage line\nonly\ttwo\n";
    auto const parsed = L::ParseIndex(text);
    Check(parsed.size() == 2 && parsed[1].maps == "4968d,4839c" && parsed[1].bytes == 200 &&
              parsed[1].ended == L::kUnexpected,
          "index round trip, damaged lines skipped");
  }
  {
    // 20 sessions, every third unexpected, 1 MB each.
    std::vector<L::Session> sessions;
    for (int i = 0; i < 20; i++) sessions.push_back(MakeSession(NameAt(i), i % 3 == 0 ? L::kUnexpected : L::kClean, 1 << 20));
    auto const remove = L::FilesToDelete(sessions, NameAt(19));
    // Kept: 10 newest (10-19) + the 5 newest unexpected (18, 15, 12, 9, 6): 9 and 6 are extra.
    Check(remove.size() == 8, "retention keeps 10 recent and 5 unexpected", std::to_string(remove.size()));
    bool keptUnexpected = std::find(remove.begin(), remove.end(), NameAt(6)) == remove.end() &&
                          std::find(remove.begin(), remove.end(), NameAt(9)) == remove.end();
    Check(keptUnexpected, "older unexpected sessions kept");
    Check(std::find(remove.begin(), remove.end(), NameAt(3)) != remove.end(), "the 6th-newest unexpected goes");
    auto const capped = L::FilesToDelete(sessions, NameAt(19), 10, 5, 5ull << 20);
    Check(capped.size() == 15 && std::find(capped.begin(), capped.end(), NameAt(19)) == capped.end(),
          "size cap deletes oldest kept, never the current one", std::to_string(capped.size()));
  }

  // --- How a session ended -------------------------------------------------------------------
  Check(L::ClassifyEnd("...\n=== session ended cleanly at 01:50 ===\n", std::nullopt) == L::kClean, "footer: clean");
  Check(L::ClassifyEnd("no footer", 2u) == L::kBackground, "black box paused: background");
  Check(L::ClassifyEnd("no footer", 1u) == L::kUnexpected, "black box running: unexpected");
  Check(L::ClassifyEnd("x\n=== app paused at 1 ===\n", std::nullopt) == L::kBackground, "paused tail: background");
  Check(L::ClassifyEnd("=== app paused ===\n=== app resumed ===\nmore", std::nullopt) == L::kUnexpected,
        "resumed then cut off: unexpected");

  // --- What changed ----------------------------------------------------------------------------
  {
    auto const before = L::ParseSummary("build=naomi.9\nmod:chroma=2.9.1\nmod:bl=0.8\nsetting:mapRealtimeShadows=false\n");
    auto const after = L::ParseSummary("build=naomi.10-beta.1\nmod:chroma=2.9.2\nmod:noodle=1.0\nsetting:mapRealtimeShadows=true\n");
    std::string const changed = L::WhatChanged(before, after);
    Check(changed == "build naomi.9 -> naomi.10-beta.1; mods: -bl, chroma 2.9.1 -> 2.9.2, +noodle 1.0; settings: "
                     "mapRealtimeShadows false -> true",
          "what changed", changed);
    Check(L::WhatChanged(after, after) == "nothing", "nothing changed");
    Check(L::WhatChanged({}, after).empty(), "first session: no line");
    Check(L::ParseSummary(L::FormatSummary(after)) == after, "summary round trip");
  }

  // --- Load timeline (crumbs as naomi.9 logged them) ----------------------------------------------
  {
    L::LoadTimeline timeline;
    double ms = 0;
    for (char const* crumb : {"bundle preload begin: '/sdcard/x.vivify'", "bundle preload end: 37 asset(s) in 161ms",
                              "level start begin: 12 custom event(s), trigger time 0",
                              "level bundle begin (already preloaded): '/sdcard/x'", "texture decode begin",
                              "texture decode end; shader repair begin", "geometry emulation begin",
                              "level bundle end: 6 asset(s)", "prefab preload: asset='a' id='b'",
                              "catch-up begin: events up to time 0", "catch-up end: 7 event(s) in 2ms",
                              "level start end: 5 live prefab(s), 5 preloaded; waiting for the next frame"}) {
      Check(!timeline.Feed(crumb, ms), std::string("no summary before the first frame: ") + crumb);
      ms += 10;
    }
    auto const line = timeline.Feed("first frame after level start: 21ms later", ms);
    Check(line && line->find("bundle preload 10 ms") != std::string::npos &&
              line->find("texture decode 10 ms") != std::string::npos &&
              line->find("shader repair (no end)") != std::string::npos &&
              line->find("level start to first frame 100 ms") != std::string::npos,
          "load timeline line", line.value_or("(none)"));
  }

  // --- Black box ------------------------------------------------------------------------------------
  std::string const boxPath = scratch + "/blackbox.bin";
  {
    Vivify::BlackBox box;
    Vivify::BlackBox::Previous previous;
    Check(box.Open(boxPath, "first.txt", 64, previous) && !previous.found, "fresh black box");
    for (int i = 0; i < 10; i++) box.Record(Vivify::BlackBox::kFrame, 1.0f + i, "", 8.3f + i, 7.0f, 1.0f);
    box.Record(Vivify::BlackBox::kCrumb, 11.0f, "level start end");
    box.SetState(Vivify::BlackBox::kClean);
    box.Close();
    Vivify::BlackBox again;
    Check(again.Open(boxPath, "second.txt", 64, previous) && previous.found, "black box found again");
    Check(previous.state == Vivify::BlackBox::kClean && previous.session == "first.txt" && previous.lines.size() == 11,
          "clean session, its name and records", std::to_string(previous.lines.size()));
    Check(previous.lines.back().find("[CRUMB] level start end") != std::string::npos &&
              previous.lines.front().find("frame 8.3 ms, GPU 7.0 ms, scale 1.00") != std::string::npos,
          "records in order and formatted", previous.lines.front());
    // Wrapping: 40 records into 16 slots keeps the last 16, in order.
    Vivify::BlackBox small;
    small.Open(scratch + "/small.bin", "s", 16, previous);
    for (int i = 0; i < 40; i++) small.Record(Vivify::BlackBox::kEvent, 0, "event " + std::to_string(i));
    small.Close();
    small.Open(scratch + "/small.bin", "t", 16, previous);
    Check(previous.lines.size() == 16 && previous.lines.front().find("event 24") != std::string::npos &&
              previous.lines.back().find("event 39") != std::string::npos,
          "a full ring keeps the newest records", previous.lines.empty() ? "" : previous.lines.front());
    // A different capacity is another layout: started fresh.
    small.Close();
    small.Open(scratch + "/small.bin", "u", 32, previous);
    Check(!previous.found, "another layout is not read");
  }
  {
    // A session that aborts while running: no handler, no close, no footer.
    pid_t const child = fork();
    if (child == 0) {
      Vivify::BlackBox box;
      Vivify::BlackBox::Previous previous;
      box.Open(boxPath, NameAt(30), 4096, previous);
      for (int i = 0; i < 100; i++) box.Record(Vivify::BlackBox::kFrame, i * 0.008f, "", 8.0f, 7.5f, 1.0f);
      box.Record(Vivify::BlackBox::kCrumb, 0.8f, "particle emulation: baking 'Explosion'");
      std::abort();
    }
    int status = 0;
    waitpid(child, &status, 0);
    Check(WIFSIGNALED(status), "the child really aborted");
    Vivify::BlackBox box;
    Vivify::BlackBox::Previous previous;
    box.Open(boxPath, "next.txt", 4096, previous);
    Check(previous.found && previous.state == Vivify::BlackBox::kRunning && previous.lines.size() == 101 &&
              previous.lines.back().find("baking 'Explosion'") != std::string::npos,
          "an aborted session's records survive, state running", std::to_string(previous.lines.size()));
    box.Close();

    // ...and the next launch settles it as unexpected.
    std::string const dir = scratch + "/Sessions";
    fs::create_directories(dir);
    WriteFile(dir + "/" + NameAt(29), "=== header ===\nlines\n=== session ended cleanly at 01:29 ===\n");
    WriteFile(dir + "/" + NameAt(30), "=== header ===\nplaying 4968d\n");
    WriteFile(dir + "/index.txt", L::FormatIndex({MakeSession(NameAt(29), L::kRunning, 0),
                                                 MakeSession(NameAt(30), L::kRunning, 0)}));
    WriteFile(scratch + "/VivifySession.txt", "=== Vivify 0.14.25-naomi.9 (commit 21b3a30c26) session log ===\nold\n");
    // Written by an older build, before any of these sessions.
    fs::last_write_time(scratch + "/VivifySession.txt", fs::file_time_type::clock::now() - std::chrono::hours(24 * 400));
    L::PreviousBox prev{previous.found, previous.state, previous.session, previous.lines};
    L::Session current = MakeSession(NameAt(31), L::kRunning, 0);
    auto const startup = L::BeginSession(dir, current, prev, scratch + "/VivifySession.txt");
    auto const index = L::ParseIndex(ReadAll(dir + "/index.txt"));
    Check(index.size() == 4, "index: legacy, two settled, current", std::to_string(index.size()));
    Check(index.size() == 4 && index[0].build == "naomi.9" && index[0].ended == "unknown",
          "legacy VivifySession.txt moved in as naomi.9", index.empty() ? "" : index[0].file);
    Check(!fs::exists(scratch + "/VivifySession.txt"), "legacy file moved, not copied");
    Check(index.size() == 4 && index[1].ended == L::kClean && index[2].ended == L::kUnexpected &&
              index[3].ended == L::kRunning,
          "clean, unexpected, running");
    std::string const crashed = ReadAll(dir + "/" + NameAt(30));
    Check(crashed.find("=== black box: the last 101 events before this session ended (unexpected) ===") !=
                  std::string::npos &&
              crashed.find("=== end of black box ===") != std::string::npos,
          "black box appended to the session that crashed");
    bool noted = false;
    for (auto const& note : startup.notes) noted |= note.find("ended unexpectedly") != std::string::npos;
    Check(noted && startup.previous && startup.previous->file == NameAt(30), "the new session is told");
    L::UpdateSession(dir, NameAt(31), "4968d", L::kClean, 1234);
    auto const updated = L::ParseIndex(ReadAll(dir + "/index.txt"));
    Check(updated.back().maps == "4968d" && updated.back().ended == L::kClean && updated.back().bytes == 1234,
          "index update");
    // The writer made the current session's file; it ended cleanly.
    WriteFile(dir + "/" + NameAt(31), "=== header ===\n=== session ended cleanly at 01:32 ===\n");
    // A session file the index never listed is picked up and settled.
    WriteFile(dir + "/" + NameAt(32), "=== header ===\n=== app paused at 01:33 ===\n");
    auto const later = L::BeginSession(dir, MakeSession(NameAt(33), L::kRunning, 0), L::PreviousBox{});
    Check(later.sessions.size() == 6 && later.sessions[4].file == NameAt(32) &&
              later.sessions[4].ended == L::kBackground,
          "an unlisted session file is added and settled", std::to_string(later.sessions.size()));
  }
  {
    // naomi.10-beta.3: VivifySession.txt as naomi.10 writes it is only a
    // pointer. beta.1 and beta.2 moved it in at every launch as an "unknown"
    // session named a second after the launch, so the next launch took it
    // for the previous session. As on the headset on 2026-11-04: session 40
    // crashed, its pointer was written a second later, and an older launch
    // left a pointer copy behind.
    std::string const dir = scratch + "/SessionsPointer";
    std::string const pointer = scratch + "/PointerVivifySession.txt";
    fs::create_directories(dir);
    std::string const pointerText = std::string(L::kPointerText) + "\nThis session: Sessions/" + NameAt(40) + "\n";
    std::string const leftover = "Vivify_2026-10-25_01-39-01_unknown-unknown.txt";
    WriteFile(dir + "/" + NameAt(39), "=== header ===\n=== session ended cleanly at 01:39 ===\n");
    WriteFile(dir + "/" + leftover, std::string(L::kPointerText) + "\nThis session: Sessions/" + NameAt(39) + "\n");
    WriteFile(dir + "/" + NameAt(40), "=== header ===\nplaying 43a25\n");
    WriteFile(dir + "/index.txt",
              L::FormatIndex({MakeSession(NameAt(39), L::kClean, 0), MakeSession(leftover, "unknown", 158),
                              MakeSession(NameAt(40), L::kRunning, 0)}));
    WriteFile(pointer, pointerText);
    L::PreviousBox box{true, Vivify::BlackBox::kRunning, NameAt(40),
                       {"23:46:30.361 s=0.00 [CRUMB] first frame after level start: 36ms later"}};
    auto const startup = L::BeginSession(dir, MakeSession(NameAt(41), L::kRunning, 0), box, pointer);
    auto const index = L::ParseIndex(ReadAll(dir + "/index.txt"));
    Check(ReadAll(pointer) == pointerText, "the pointer is left where it is, not moved in");
    Check(!fs::exists(dir + "/" + leftover), "a pointer copy left by beta.1/beta.2 is deleted");
    int unknown = 0;
    for (auto const& entry : fs::directory_iterator(dir)) {
      unknown += entry.path().filename().string().find("unknown-unknown") != std::string::npos;
    }
    Check(unknown == 0, "no unknown-unknown session file", std::to_string(unknown));
    Check(index.size() == 3 && index[0].file == NameAt(39) && index[1].file == NameAt(40) &&
              index[2].file == NameAt(41),
          "index: the real sessions only", std::to_string(index.size()));
    Check(startup.previous && startup.previous->file == NameAt(40) && startup.previous->ended == L::kUnexpected,
          "the previous session is the one that crashed", startup.previous ? startup.previous->file : "none");
    bool told = false, boxShown = false, removedNote = false;
    for (auto const& note : startup.notes) {
      told |= note.find(NameAt(40)) != std::string::npos && note.find("ended unexpectedly") != std::string::npos;
      boxShown |= note.find("first frame after level start") != std::string::npos;
      removedNote |= note.find("removed 1 leftover copy") != std::string::npos;
    }
    Check(told && boxShown, "the new session is told about the crash, with its last black-box events");
    Check(removedNote, "the clean-up is noted");
    // A naomi.9 log in VivifySession.txt is still moved in (checked above);
    // a second launch with the pointer in place changes nothing.
    WriteFile(dir + "/" + NameAt(41), "=== header ===\n=== session ended cleanly at 01:42 ===\n");
    WriteFile(pointer, std::string(L::kPointerText) + "\nThis session: Sessions/" + NameAt(41) + "\n");
    auto const again = L::BeginSession(dir, MakeSession(NameAt(42), L::kRunning, 0), L::PreviousBox{}, pointer);
    Check(again.sessions.size() == 4 && again.previous && again.previous->file == NameAt(41),
          "next launch: still no pointer copy, previous is the last real session",
          std::to_string(again.sessions.size()));
  }

  // --- The writer -----------------------------------------------------------------------------------
  {
    std::string const path = scratch + "/writer.txt";
    Vivify::LogWriter writer;
    Vivify::LogWriter::Options options;
    options.burst = 5;
    Check(writer.Start(path, {"=== header ==="}, options), "writer starts");
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
      threads.emplace_back([&writer, t]() {
        for (int i = 0; i < 500; i++) {
          writer.Push("p ", "thread " + std::string(1, static_cast<char>('a' + t)) + " line " + std::to_string(i),
                      false, false);
        }
      });
    }
    for (auto& thread : threads) thread.join();
    for (int i = 0; i < 50; i++) writer.Push("p ", "repeated warning " + std::to_string(i), false, true);
    writer.Push("p ", "contact me@example.com", true);
    writer.Sync();
    std::string text = ReadAll(path);
    Check(CountLines(text, "thread ") == 2000, "every unlimited line written", std::to_string(CountLines(text, "thread ")));
    Check(CountLines(text, "repeated warning") == 5, "rate limit: 5 of 50", std::to_string(CountLines(text, "repeated warning")));
    Check(text.find("me@example.com") == std::string::npos && text.find("contact <email>") != std::string::npos,
          "scrubbed on the way out");
    // Each thread's lines stay in order.
    Check(text.find("thread a line 10\n") < text.find("thread a line 11\n"), "per-thread order kept");
    writer.Stop({"=== session ended cleanly ==="});
    text = ReadAll(path);
    Check(text.find("left out 45 more line(s) like this") != std::string::npos, "suppressed count reported at the end");
    Check(text.rfind("=== session ended cleanly ===") == text.size() - 30, "footer is the last line");
    writer.Push("p ", "after stop");
    Check(ReadAll(path).find("after stop") == std::string::npos, "pushes after Stop are dropped");

    Vivify::LogWriter capped;
    Vivify::LogWriter::Options small;
    small.maxBytes = 1000;
    capped.Start(scratch + "/capped.txt", {}, small);
    for (int i = 0; i < 100; i++) capped.Push("p ", "a line of some length number " + std::to_string(i), false, false);
    capped.Stop({});
    std::string const cappedText = ReadAll(scratch + "/capped.txt");
    Check(cappedText.size() < 1200 && cappedText.find("session log capped") != std::string::npos, "size cap",
          std::to_string(cappedText.size()));
  }

  // --- Cost on the logging thread ---------------------------------------------------------------------
  {
    using Clock = std::chrono::steady_clock;
    Vivify::LogWriter writer;
    writer.Start(scratch + "/bench.txt", {});
    std::tm local = t;
    constexpr int kLines = 20000;
    auto const start = Clock::now();
    for (int i = 0; i < kLines; i++) {
      writer.Push(L::FormatPrefix(local, i % 1000, 12.5f, "INFO"),
                  "Vivify geometry emulation: an object kept the stand-in: 'Explosion' is not a mesh renderer");
    }
    double const pushUs = std::chrono::duration<double, std::micro>(Clock::now() - start).count() / kLines;
    writer.Stop({});
    Vivify::BlackBox box;
    Vivify::BlackBox::Previous previous;
    box.Open(scratch + "/bench.bin", "bench", 4096, previous);
    constexpr int kRecords = 200000;
    auto const boxStart = Clock::now();
    for (int i = 0; i < kRecords; i++) box.Record(Vivify::BlackBox::kFrame, i * 0.008f, "", 8.3f, 7.1f, 1.0f);
    double const recordUs = std::chrono::duration<double, std::micro>(Clock::now() - boxStart).count() / kRecords;
    std::printf("cost on the calling thread: log line %.2f us, black-box record %.3f us\n", pushUs, recordUs);
    // Generous bounds (sanitizer builds are slow); the real figures are printed.
    Check(pushUs < 25.0, "a log line costs the caller little", std::to_string(pushUs));
    Check(recordUs < 5.0, "a black-box record costs the caller little", std::to_string(recordUs));
  }

  std::printf("%d passed, %d failed\n", passed, failed);
  return failed == 0 ? 0 : 1;
}
