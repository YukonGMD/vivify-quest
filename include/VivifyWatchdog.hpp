#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace Vivify {

// The fields of /proc/<pid>/task/<tid>/stat a stall report needs. The second
// field (comm) is in parentheses and may itself contain spaces or ')', so the
// rest is read from after the *last* ')'.
struct TaskStat {
  char state = '?';
  uint64_t utimeTicks = 0;
  uint64_t stimeTicks = 0;
};
std::optional<TaskStat> ParseTaskStat(std::string_view text);

// Main-thread stall watchdog (0.14.25-naomi.2).
//
// The main thread calls Beat() once a frame; that is one relaxed atomic store.
// A background thread wakes once a second and, while the last beat is less
// than stallAfter old, does nothing else. Once it is older, it writes a stall
// line through `write` (which must flush), then one every reportEvery, up to
// maxReports per stall, and a "resumed" line when beats start again. From
// naomi.3 it also takes up to maxSamples native stack samples of the main
// thread per stall (VivifyStackSampler.hpp).
//
// The watchdog thread never calls Unity or il2cpp: it reads only its own
// atomics, the breadcrumb string, files under /proc/self, and the ELF headers
// of loaded libraries.
class Watchdog {
 public:
  struct Options {
    std::chrono::milliseconds stallAfter{5000};
    std::chrono::milliseconds reportEvery{5000};
    std::chrono::milliseconds poll{1000};
    // How long the first report waits between two /proc samples, so it can
    // say how much CPU the thread used while stalled.
    std::chrono::milliseconds cpuSample{1000};
    int maxReports = 12;
    // The render thread is found by this name in /proc/self/task/*/comm.
    std::string renderThreadName = "UnityGfxDeviceW";
    // Native stack samples of the main thread during a stall (naomi.3): the
    // first right after the first report, then one every sampleEvery, at most
    // maxSamples per stall (0 turns sampling off).
    int maxSamples = 3;
    std::chrono::milliseconds sampleEvery{3000};
    std::chrono::milliseconds sampleTimeout{500};
  };
  using Writer = std::function<void(std::string const& line)>;

  Watchdog() = default;
  ~Watchdog();
  Watchdog(Watchdog const&) = delete;
  Watchdog& operator=(Watchdog const&) = delete;

  // Starts the thread once; later calls do nothing.
  void Start(Writer write, Options options);
  void Start(Writer write) { Start(std::move(write), Options{}); }
  // Stops and joins the thread (tests; the game never stops it).
  void Stop();

  // Main thread, once a frame. The first beat also records which thread the
  // main thread is.
  void Beat();
  // While paused (the app in the background), stalls are not reported.
  void SetPaused(bool paused);
  void SetBreadcrumb(std::string_view text);
  std::string Breadcrumb() const;
  // The breadcrumb into a buffer, cut to fit, without allocating.
  void CopyBreadcrumb(char* buffer, size_t size) const;
  // True on the thread that beats (false until the first beat).
  bool OnMainThread() const;

 private:
  void Run();
  // Takes one stack sample and writes it; false when sampling cannot work at
  // all in this process (said once, in the line written).
  bool WriteStackSample(int number, double stalledSeconds);
  std::string DescribeThread(char const* label, int tid, std::optional<TaskStat> const& before,
                             std::optional<TaskStat> const& after, double sampleSeconds) const;

  Writer _write;
  Options _options;
  std::thread _thread;
  std::mutex _sleepMutex;
  std::condition_variable _wake;
  std::atomic<bool> _stop{false};
  std::atomic<bool> _started{false};
  std::atomic<int64_t> _lastBeatNs{0};
  std::atomic<int> _mainTid{0};
  std::atomic<bool> _paused{false};
  mutable std::mutex _breadcrumbMutex;
  std::string _breadcrumb = "none";
};

}
