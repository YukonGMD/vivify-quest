#include "VivifyLogWriter.hpp"

#include <pthread.h>

#include <chrono>
#include <future>

namespace Vivify {

LogWriter::~LogWriter() {
  if (_running.load()) Stop({});
}

bool LogWriter::Start(std::string const& path, std::vector<std::string> const& header, Options options) {
  if (_running.load()) return false;
  _options = options;
  _path = path;
  _file.open(path, std::ios::out | std::ios::app | std::ios::binary);
  if (!_file.is_open()) return false;
  for (auto const& line : header) {
    _file << line << '\n';
    _bytes += line.size() + 1;
  }
  _file.flush();
  _limiter = SessionLog::RateLimiter(options.burst, options.windowSeconds);
  _stopping = false;
  _capped = false;
  _running = true;
  _thread = std::thread([this]() {
#if defined(__ANDROID__) || defined(__linux__)
    pthread_setname_np(pthread_self(), "VivifyLog");
#endif
    Run();
  });
  return true;
}

void LogWriter::Push(std::string prefix, std::string text, bool important, bool limit) {
  if (!_running.load()) return;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_stopping) return;
    _queue.push_back(Item{std::move(prefix), std::move(text), important, limit, nullptr});
    if (important) _flushNow = true;
  }
  if (important) _wake.notify_one();
}

void LogWriter::Post(std::function<void()> task) {
  if (!_running.load()) return;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_stopping) return;
    _queue.push_back(Item{{}, {}, false, false, std::move(task)});
  }
  _wake.notify_one();
}

void LogWriter::Sync() {
  if (!_running.load()) return;
  auto done = std::make_shared<std::promise<void>>();
  auto future = done->get_future();
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_stopping) return;
    _queue.push_back(Item{{}, {}, true, false, [done]() { done->set_value(); }});
    _flushNow = true;
  }
  _wake.notify_one();
  future.wait();
}

void LogWriter::Stop(std::vector<std::string> const& footer) {
  if (!_running.load()) return;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_stopping) return;
    _stopping = true;
    _footer = footer;
  }
  _wake.notify_one();
  if (_thread.joinable()) _thread.join();
  _running = false;
}

void LogWriter::WriteLine(std::string const& prefix, std::string const& text) {
  if (_capped) return;
  std::string const scrubbed = SessionLog::Scrub(text);
  if (_bytes.load() + prefix.size() + scrubbed.size() + 1 > _options.maxBytes) {
    std::string const notice = prefix + "(session log capped at " +
                               std::to_string(_options.maxBytes / (1024 * 1024)) +
                               " MB; the black box and the next session's file go on)";
    _file << notice << '\n';
    _bytes += notice.size() + 1;
    _capped = true;
    return;
  }
  _file << prefix << scrubbed << '\n';
  _bytes += prefix.size() + scrubbed.size() + 1;
  _lines++;
}

void LogWriter::Run() {
  using Clock = std::chrono::steady_clock;
  auto const started = Clock::now();
  auto lastFlush = started;
  std::deque<Item> batch;
  std::vector<std::string> reports;
  std::string lastPrefix;
  while (true) {
    bool stopping = false, flushNow = false;
    {
      std::unique_lock<std::mutex> lock(_mutex);
      _wake.wait_for(lock, std::chrono::milliseconds(_options.flushMs),
                     [this]() { return _stopping || _flushNow || _queue.size() >= 256; });
      batch.swap(_queue);
      stopping = _stopping;
      flushNow = _flushNow;
      _flushNow = false;
    }
    double const now = std::chrono::duration<double>(Clock::now() - started).count();
    try {
      for (auto& item : batch) {
        if (item.task) {
          // A task may read the file (its size) or be a Sync waiting on it.
          _file.flush();
          item.task();
          continue;
        }
        lastPrefix = item.prefix;
        if (item.limit && !_limiter.Allow(item.text, now, reports)) {
          _suppressed++;
        } else {
          WriteLine(item.prefix, item.text);
        }
        for (auto const& report : reports) WriteLine(item.prefix, report);
        reports.clear();
      }
      _limiter.Expire(now, reports);
      if (stopping) _limiter.ExpireAll(reports);
      for (auto const& report : reports) WriteLine(lastPrefix, report);
      reports.clear();
      if (stopping) {
        for (auto const& line : _footer) {
          _file << line << '\n';
          _bytes += line.size() + 1;
        }
      }
    } catch (...) {
      // A diagnostic file is never worth more than the game: drop the batch.
    }
    batch.clear();
    auto const nowClock = Clock::now();
    if (flushNow || stopping || nowClock - lastFlush >= std::chrono::milliseconds(_options.flushMs)) {
      _file.flush();
      lastFlush = nowClock;
    }
    if (stopping) break;
  }
  _file.close();
}

}  // namespace Vivify
