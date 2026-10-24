#include "VivifyWatchdog.hpp"

#include "VivifyStackSampler.hpp"

#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Vivify {

namespace {

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string ReadSmallFile(std::string const& path) {
  std::ifstream in(path);
  if (!in) return {};
  std::string text;
  std::getline(in, text, '\0');
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

std::string TaskPath(int tid, char const* file) {
  return "/proc/self/task/" + std::to_string(tid) + "/" + file;
}

std::optional<TaskStat> ReadTaskStat(int tid) {
  if (tid <= 0) return std::nullopt;
  return ParseTaskStat(ReadSmallFile(TaskPath(tid, "stat")));
}

// The render thread, found by name. Unity names its threads, and the name is
// all /proc gives; 0 when no thread has it.
int FindThreadByName(std::string const& name) {
  std::error_code ec;
  for (auto const& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
    std::string const tidText = entry.path().filename().string();
    int const tid = std::atoi(tidText.c_str());
    if (tid <= 0) continue;
    if (ReadSmallFile(TaskPath(tid, "comm")) == name) return tid;
  }
  return 0;
}

}  // namespace

std::optional<TaskStat> ParseTaskStat(std::string_view text) {
  size_t const close = text.rfind(')');
  if (close == std::string_view::npos || close + 2 >= text.size()) return std::nullopt;
  std::istringstream rest{std::string(text.substr(close + 2))};
  // After comm: 3 state, 4 ppid, 5 pgrp, 6 session, 7 tty_nr, 8 tpgid,
  // 9 flags, 10 minflt, 11 cminflt, 12 majflt, 13 cmajflt, 14 utime, 15 stime.
  TaskStat stat;
  std::string skip;
  if (!(rest >> stat.state)) return std::nullopt;
  for (int field = 4; field <= 13; field++) {
    if (!(rest >> skip)) return std::nullopt;
  }
  if (!(rest >> stat.utimeTicks >> stat.stimeTicks)) return std::nullopt;
  return stat;
}

Watchdog::~Watchdog() {
  Stop();
}

void Watchdog::Start(Writer write, Options options) {
  if (_started.exchange(true)) return;
  _write = std::move(write);
  _options = std::move(options);
  _stop = false;
  _thread = std::thread([this]() { Run(); });
}

void Watchdog::Stop() {
  if (!_started) return;
  {
    std::lock_guard lock(_sleepMutex);
    _stop = true;
  }
  _wake.notify_all();
  if (_thread.joinable()) _thread.join();
  _started = false;
}

void Watchdog::Beat() {
  if (_mainTid.load(std::memory_order_relaxed) == 0) {
    StackSampler::RegisterTarget();
    _mainTid.store(static_cast<int>(syscall(SYS_gettid)), std::memory_order_relaxed);
  }
  _lastBeatNs.store(NowNs(), std::memory_order_relaxed);
}

void Watchdog::SetPaused(bool paused) {
  _paused.store(paused, std::memory_order_relaxed);
  // Coming back counts as a beat, so the time spent paused is not a stall.
  if (!paused) _lastBeatNs.store(NowNs(), std::memory_order_relaxed);
}

void Watchdog::SetBreadcrumb(std::string_view text) {
  std::lock_guard lock(_breadcrumbMutex);
  _breadcrumb.assign(text);
}

std::string Watchdog::Breadcrumb() const {
  std::lock_guard lock(_breadcrumbMutex);
  return _breadcrumb;
}

void Watchdog::CopyBreadcrumb(char* buffer, size_t size) const {
  if (size == 0) return;
  std::lock_guard lock(_breadcrumbMutex);
  size_t const length = std::min(size - 1, _breadcrumb.size());
  std::memcpy(buffer, _breadcrumb.data(), length);
  buffer[length] = '\0';
}

bool Watchdog::OnMainThread() const {
  int const mainTid = _mainTid.load(std::memory_order_relaxed);
  return mainTid != 0 && mainTid == static_cast<int>(syscall(SYS_gettid));
}

void Watchdog::SetSlowFrameSection(int section) {
  _slowSection.store(section, std::memory_order_relaxed);
}

void Watchdog::ResetSlowFrames() {
  std::lock_guard lock(_slowMutex);
  _slowProfile.Reset();
  _slowNotes.clear();
  _slowGeneration.fetch_add(1, std::memory_order_acq_rel);
}

std::vector<std::string> Watchdog::TakeSlowFrameSummary(std::function<std::string(int)> const& label) {
  std::lock_guard lock(_slowMutex);
  char head[256];
  std::snprintf(head, sizeof(head),
                "slow frames (over %lld ms): stacks of the main and render threads every %lld ms, at most %d a "
                "frame; functions are library+start address, which tools/symbolize names",
                static_cast<long long>(_options.slowFrameAfter.count()),
                static_cast<long long>(_options.slowSampleEvery.count()), _options.slowSamplesPerFrame);
  std::vector<std::string> lines{std::string(head) + (_slowProfile.Empty() ? "; no frame ran that long" : "")};
  for (auto const& note : _slowNotes) lines.push_back("  " + note);
  for (auto& line : _slowProfile.Summary(label, _slowNames, _options.slowTopFunctions)) {
    lines.push_back(std::move(line));
  }
  _slowProfile.Reset();
  _slowNotes.clear();
  _slowGeneration.fetch_add(1, std::memory_order_acq_rel);
  return lines;
}

void Watchdog::SlowFrameNote(std::string const& note) {
  std::lock_guard lock(_slowMutex);
  if (_slowNotes.size() < 8 && std::find(_slowNotes.begin(), _slowNotes.end(), note) == _slowNotes.end()) {
    _slowNotes.push_back(note);
  }
}

std::chrono::milliseconds Watchdog::SlowFrameTick(int64_t lastBeat, int64_t now) {
  int const section = _slowSection.load(std::memory_order_relaxed);
  if (section < 0 || _options.slowSamplesPerFrame <= 0) return _options.poll;
  int64_t const age = now - lastBeat;
  int64_t const after =
      std::chrono::duration_cast<std::chrono::nanoseconds>(_options.slowFrameAfter).count();
  if (age < after) {
    // Back when this frame would turn slow.
    return std::min(std::chrono::milliseconds((after - age) / 1000000 + 1), _options.poll);
  }
  bool const newFrame = lastBeat != _slowFrameBeat;
  if (newFrame) {
    _slowFrameBeat = lastBeat;
    _slowFrameSamples = 0;
  }
  if (_slowFrameSamples < _options.slowSamplesPerFrame) {
    _slowFrameSamples++;
    TakeSlowFrameSamples(section, newFrame);
  }
  return std::min(_options.slowSampleEvery, _options.poll);
}

void Watchdog::TakeSlowFrameSamples(int section, bool newFrame) {
  uint64_t const generation = _slowGeneration.load(std::memory_order_acquire);
  if (generation != _slowSeenGeneration) {
    // A new song: look the render thread and the libraries up again.
    _slowSeenGeneration = generation;
    _slowSongSamples = 0;
    _renderTid = 0;
    _renderStack.reset();
    _renderLookups = 0;
    _renderNoted = false;
    _modulesBuiltNs = 0;
  }
  if (newFrame) {
    std::lock_guard lock(_slowMutex);
    if (_slowGeneration.load(std::memory_order_acquire) == generation) _slowProfile.CountSlowFrame(section);
  }
  if (_slowUnavailable) return;
  if (_slowSongSamples >= _options.slowSamplesPerSong) {
    SlowFrameNote("stopped sampling after " + std::to_string(_options.slowSamplesPerSong) +
                  " samples; later slow frames are counted only in the frame times");
    return;
  }
  _slowSongSamples++;

  bool const needModules = _modulesBuiltNs == 0;
  bool const needRender = !_renderStack && _renderLookups < 5;
  std::vector<MapEntry> maps;
  if (needModules || needRender) maps = ParseProcMaps(ReadSmallFile("/proc/self/maps"));
  if (needRender) {
    _renderLookups++;
    if (_renderTid == 0) _renderTid = FindThreadByName(_options.renderThreadName);
    if (_renderTid != 0) {
      _renderStack = FindNamedThreadStack(maps, _renderTid);
      if (!_renderStack) {
        // Only while it is blocked in a system call; the next slow frame tries again.
        if (auto const sp = ParseSyscallStackPointer(ReadSmallFile(TaskPath(_renderTid, "syscall")))) {
          _renderStack = FindStackHolding(maps, *sp);
        }
      }
    }
    if (!_renderStack && _renderLookups == 5 && !_renderNoted) {
      _renderNoted = true;
      SlowFrameNote(_renderTid == 0 ? "no thread called " + _options.renderThreadName + "; main thread only"
                                    : "render thread tid=" + std::to_string(_renderTid) +
                                          ": its stack was not found; main thread only");
    }
  }
  if (needModules) {
    _modules.Build(maps, &ReadOwnMemory);
    _modulesBuiltNs = NowNs();
  }

  struct Taken {
    int thread;
    bool running;
    StackSample sample;
  };
  std::vector<Taken> taken;
  auto take = [&](int thread, int tid, std::optional<StackBounds> const& bounds) {
    std::optional<TaskStat> const stat = ReadTaskStat(tid);
    Taken result{thread, stat && stat->state == 'R', {}};
    StackSampler::Result const outcome =
        bounds ? StackSampler::SampleThread(tid, static_cast<uintptr_t>(bounds->lo),
                                            static_cast<uintptr_t>(bounds->hi), _options.slowSampleTimeout,
                                            result.sample)
               : StackSampler::Sample(_options.slowSampleTimeout, result.sample);
    if (outcome == StackSampler::Result::Ok) {
      taken.push_back(result);
      return;
    }
    bool const permanent = outcome == StackSampler::Result::HandlerTaken ||
                           outcome == StackSampler::Result::InstallFailed ||
                           outcome == StackSampler::Result::Unsupported;
    if (permanent) _slowUnavailable = true;
    if (thread == SlowFrameProfile::kRender && outcome == StackSampler::Result::SendFailed) {
      _renderTid = 0;  // gone: look for it again
      _renderStack.reset();
      _renderLookups = 0;
    }
    SlowFrameNote(std::string(thread == SlowFrameProfile::kMain ? "main" : "render") +
                  " thread not sampled: " + StackSampler::DescribeResult(outcome));
  };
  take(SlowFrameProfile::kMain, _mainTid.load(std::memory_order_relaxed), std::nullopt);
  if (_renderStack && !_slowUnavailable) take(SlowFrameProfile::kRender, _renderTid, _renderStack);

  bool unmapped = false;
  std::vector<std::vector<CodeLocation>> stacks;
  for (auto const& sample : taken) {
    std::vector<CodeLocation> stack;
    for (int i = 0; i < sample.sample.count; i++) {
      stack.push_back(_modules.Locate(sample.sample.frames[i]));
      unmapped |= stack.back().module < 0;
    }
    stacks.push_back(std::move(stack));
  }
  // A library loaded since the table was built: rebuild it, at most every 5 s.
  if (unmapped && NowNs() - _modulesBuiltNs > 5'000'000'000LL) _modulesBuiltNs = 0;

  std::lock_guard lock(_slowMutex);
  if (_slowGeneration.load(std::memory_order_acquire) != generation) return;
  for (size_t i = 0; i < taken.size(); i++) {
    _slowProfile.AddSample(section, taken[i].thread, taken[i].running, stacks[i].data(),
                           static_cast<int>(stacks[i].size()));
  }
  if (_slowNames.size() != _modules.Names().size()) _slowNames = _modules.Names();
}

bool Watchdog::WriteStackSample(int number, double stalledSeconds) {
  StackSample sample;
  auto const result = StackSampler::Sample(_options.sampleTimeout, sample);
  char head[96];
  std::snprintf(head, sizeof(head), "WATCHDOG: stack sample %d/%d (stalled %.0fs): ", number, _options.maxSamples,
                stalledSeconds);
  if (result != StackSampler::Result::Ok) {
    bool const permanent = result == StackSampler::Result::HandlerTaken ||
                           result == StackSampler::Result::InstallFailed ||
                           result == StackSampler::Result::NoTarget || result == StackSampler::Result::Unsupported;
    _write(std::string(head) + (permanent ? "stack sampling unavailable: " : "no sample: ") +
           StackSampler::DescribeResult(result));
    return !permanent;
  }
  _write(std::string(head) + "main tid=" + std::to_string(StackSampler::TargetTid()) + ", " +
         std::to_string(sample.count) + " frame(s), stopped at: " + DescribeStop(sample.stop));
  auto const maps = ParseProcMaps(ReadSmallFile("/proc/self/maps"));
  for (int i = 0; i < sample.count; i++) {
    auto const frame = ResolveAddress(sample.frames[i], maps, &ReadOwnMemory);
    _write("WATCHDOG:   " + FormatFrame(i, frame, i == 1 && sample.lrUnconfirmed));
  }
  return true;
}

std::string Watchdog::DescribeThread(char const* label, int tid, std::optional<TaskStat> const& before,
                                     std::optional<TaskStat> const& after, double sampleSeconds) const {
  if (tid <= 0) return std::string(label) + " not found";
  std::string text = std::string(label) + " tid=" + std::to_string(tid);
  if (!after) return text + " stat unreadable";
  text += " state=";
  text += after->state;
  if (before) {
    long const ticksPerSecond = sysconf(_SC_CLK_TCK);
    double const msPerTick = ticksPerSecond > 0 ? 1000.0 / static_cast<double>(ticksPerSecond) : 10.0;
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), " cpu user=%.0fms sys=%.0fms in the last %.1fs",
                  static_cast<double>(after->utimeTicks - before->utimeTicks) * msPerTick,
                  static_cast<double>(after->stimeTicks - before->stimeTicks) * msPerTick, sampleSeconds);
    text += buffer;
  }
  std::string const wchan = ReadSmallFile(TaskPath(tid, "wchan"));
  text += " wchan=" + (wchan.empty() ? std::string("?") : wchan);
  return text;
}

void Watchdog::Run() {
  // False when told to stop.
  auto sleepFor = [&](std::chrono::milliseconds duration) {
    std::unique_lock lock(_sleepMutex);
    return !_wake.wait_for(lock, duration, [this] { return _stop.load(); });
  };
  auto toNs = [](std::chrono::milliseconds duration) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
  };

  bool inStall = false;
  int reports = 0;
  int samples = 0;
  int64_t nextSampleNs = 0;
  bool samplingUnavailable = false;  // for the life of the process
  int renderTid = 0;
  int64_t nextReportNs = 0;
  std::optional<TaskStat> previousMain;
  std::optional<TaskStat> previousRender;
  int64_t previousSampleNs = 0;

  std::chrono::milliseconds delay = _options.poll;
  while (sleepFor(delay)) {
    delay = _options.poll;
    int64_t const lastBeat = _lastBeatNs.load(std::memory_order_relaxed);
    if (lastBeat == 0) continue;  // the first frame has not happened yet
    int64_t const now = NowNs();
    bool const paused = _paused.load(std::memory_order_relaxed);
    if (!paused) delay = SlowFrameTick(lastBeat, now);
    bool const stale = !paused && now - lastBeat > toNs(_options.stallAfter);

    if (!stale) {
      if (inStall) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "WATCHDOG: main thread resumed (%d stall report(s) written)",
                      reports);
        _write(buffer);
        inStall = false;
      }
      continue;
    }

    if (!inStall) {
      inStall = true;
      reports = 0;
      samples = 0;
      nextReportNs = now;
      previousMain.reset();
      previousRender.reset();
      renderTid = FindThreadByName(_options.renderThreadName);
    }

    if (reports < _options.maxReports && now >= nextReportNs) {
      int const mainTid = _mainTid.load(std::memory_order_relaxed);
      std::optional<TaskStat> beforeMain = previousMain;
      std::optional<TaskStat> beforeRender = previousRender;
      int64_t sampleStart = previousSampleNs;
      if (!beforeMain) {
        // First report of this stall: two samples a moment apart, so it can say
        // whether the thread is burning CPU or blocked.
        beforeMain = ReadTaskStat(mainTid);
        beforeRender = ReadTaskStat(renderTid);
        sampleStart = NowNs();
        if (!sleepFor(_options.cpuSample)) return;
      }
      std::optional<TaskStat> const afterMain = ReadTaskStat(mainTid);
      std::optional<TaskStat> const afterRender = ReadTaskStat(renderTid);
      int64_t const sampleEnd = NowNs();
      double const sampleSeconds = static_cast<double>(sampleEnd - sampleStart) / 1e9;
      double const stalledNow = static_cast<double>(sampleEnd - lastBeat) / 1e9;

      char head[64];
      std::snprintf(head, sizeof(head), "WATCHDOG: main thread stalled %.0fs", stalledNow);
      std::string line = std::string(head) + ", last breadcrumb=" + Breadcrumb() + " | " +
                         DescribeThread("main", mainTid, beforeMain, afterMain, sampleSeconds) + " | " +
                         DescribeThread(("render (" + _options.renderThreadName + ")").c_str(), renderTid,
                                        beforeRender, afterRender, sampleSeconds);
      _write(line);
      reports++;
      if (reports == _options.maxReports) {
        _write("WATCHDOG: further stall reports suppressed until the main thread resumes");
      }
      previousMain = afterMain;
      previousRender = afterRender;
      previousSampleNs = sampleEnd;
      nextReportNs = sampleEnd + toNs(_options.reportEvery);
      // The first stack sample follows the first report.
      if (reports == 1) nextSampleNs = sampleEnd;
    }

    if (samplingUnavailable || samples >= _options.maxSamples || NowNs() < nextSampleNs) continue;
    samples++;
    if (!WriteStackSample(samples, static_cast<double>(NowNs() - lastBeat) / 1e9)) samplingUnavailable = true;
    nextSampleNs = NowNs() + toNs(_options.sampleEvery);
  }
}

}
