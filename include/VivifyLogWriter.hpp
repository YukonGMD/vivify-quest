#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "VivifySessionLog.hpp"

namespace Vivify {

// Writes a session log on a thread of its own (0.14.25-naomi.10).
//
// The game's threads only format a line's prefix and queue it; scrubbing,
// rate limiting, file writes and flushes happen on the writer thread. Lines
// are flushed four times a second, and at once when one is marked important
// (warnings, errors, breadcrumbs), so a hang loses at most a quarter second
// of ordinary lines. What a crash cuts off, the black box keeps.
//
// Unity-free; tools/sessionlog tests it.
class LogWriter {
 public:
  struct Options {
    uint64_t maxBytes = 48ull * 1024 * 1024;  // a session file stops growing here
    int flushMs = 250;
    int burst = 8;           // rate limit: lines with one key per window
    double windowSeconds = 10.0;
  };

  LogWriter() = default;
  ~LogWriter();
  LogWriter(LogWriter const&) = delete;
  LogWriter& operator=(LogWriter const&) = delete;

  // Opens `path` for appending, writes `header` (as given: not scrubbed or
  // limited) and starts the thread. False when the file cannot be opened.
  bool Start(std::string const& path, std::vector<std::string> const& header, Options options);
  bool Start(std::string const& path, std::vector<std::string> const& header) {
    return Start(path, header, Options{});
  }
  // Queues one line: `prefix` as FormatPrefix made it, `text` the message.
  // `limit` false exempts it from rate limiting (summaries, headers).
  void Push(std::string prefix, std::string text, bool important = false, bool limit = true);
  // Runs `task` on the writer thread, after every line queued before it.
  void Post(std::function<void()> task);
  // Returns once everything queued so far is written and flushed.
  void Sync();
  // Writes everything queued, then `footer`, and closes. Later pushes are
  // dropped.
  void Stop(std::vector<std::string> const& footer);

  bool Running() const { return _running.load(); }
  std::string const& Path() const { return _path; }
  uint64_t BytesWritten() const { return _bytes.load(); }
  uint64_t LinesWritten() const { return _lines.load(); }
  uint64_t LinesSuppressed() const { return _suppressed.load(); }

 private:
  struct Item {
    std::string prefix;
    std::string text;
    bool important = false;
    bool limit = true;
    std::function<void()> task;
  };
  void Run();
  void WriteLine(std::string const& prefix, std::string const& text);

  Options _options;
  std::string _path;
  std::ofstream _file;
  std::thread _thread;
  std::mutex _mutex;
  std::condition_variable _wake;
  std::deque<Item> _queue;
  bool _stopping = false;
  bool _flushNow = false;
  std::vector<std::string> _footer;
  std::atomic<bool> _running{false};
  std::atomic<uint64_t> _bytes{0};
  std::atomic<uint64_t> _lines{0};
  std::atomic<uint64_t> _suppressed{0};
  bool _capped = false;
  SessionLog::RateLimiter _limiter;
};

}  // namespace Vivify
