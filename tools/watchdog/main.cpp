// Host test driver for src/VivifyWatchdog.cpp.
//
//   watchdog parse <stat-file>
//     prints state= utime= stime=, or "unparsed" (exit 1)
//   watchdog run <healthyMs> <stallMs> <afterMs> <busy|sleep|paused> <maxReports>
//     beats every 10 ms for healthyMs, stops beating for stallMs (spinning or
//     sleeping on this thread, or paused), beats again for afterMs, then stops
//     the watchdog and prints every line it wrote, one per line.
//   watchdog selftest
//     checks the stack walker, /proc/self/maps resolution and frame formatting
//     on synthetic input; prints PASS/FAIL lines
//   watchdog sample <spin|taken|blocked>
//     a second thread registers as the main thread and spins; this thread
//     samples it and prints the result and frames ("taken": the signal already
//     has a handler; "blocked": the spinning thread blocks it)
// Thresholds are scaled down: stall after 200 ms, report every 200 ms, poll
// every 20 ms, CPU sample 100 ms, stack sample every 100 ms.
#include "VivifyStackSampler.hpp"
#include "VivifyWatchdog.hpp"

#include <elf.h>
#include <pthread.h>
#include <signal.h>
#include <atomic>
#include <cstring>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;

namespace {

int gSelftestFailures = 0;
void Check(char const* name, bool ok, std::string const& detail = "") {
  std::printf("%s %s%s\n", ok ? "PASS" : "FAIL", name, ok ? "" : (" " + detail).c_str());
  if (!ok) gSelftestFailures++;
}

int Selftest() {
  using namespace Vivify;
  // A fake stack of three frame records, growing upwards: {next fp, return}.
  alignas(16) uint64_t stack[32] = {};
  auto at = [&](int word) { return reinterpret_cast<uint64_t>(&stack[word]); };
  uintptr_t const lo = at(0), hi = at(32);
  stack[4] = at(10);   stack[5] = 0x1111;
  stack[10] = at(20);  stack[11] = 0x2222;
  stack[20] = 0;       stack[21] = 0x3333;
  StackSample sample;
  WalkFramePointers(0xAAAA, 0, at(4), lo, hi, sample);
  Check("walk: pc then every return address", sample.count == 4 && sample.frames[0] == 0xAAAA &&
        sample.frames[1] == 0x1111 && sample.frames[2] == 0x2222 && sample.frames[3] == 0x3333,
        std::to_string(sample.count));
  Check("walk: ends at a zero frame pointer", sample.stop == StackWalkStop::ChainEnd);

  WalkFramePointers(0xAAAA, 0xBBBB, at(4), lo, hi, sample);
  Check("walk: an unconfirmed lr is kept and flagged", sample.count == 5 && sample.frames[1] == 0xBBBB &&
        sample.lrUnconfirmed);
  WalkFramePointers(0xAAAA, 0x1111, at(4), lo, hi, sample);
  Check("walk: an lr the first record confirms is not repeated", sample.count == 4 && sample.frames[1] == 0x1111 &&
        !sample.lrUnconfirmed);

  stack[10] = at(4);  // loops back down
  WalkFramePointers(0xAAAA, 0, at(4), lo, hi, sample);
  Check("walk: stops when frame pointers stop climbing", sample.stop == StackWalkStop::NotIncreasing &&
        sample.count == 3);
  stack[10] = at(20);

  WalkFramePointers(0xAAAA, 0, at(4) + 4, lo, hi, sample);
  Check("walk: refuses a misaligned frame pointer", sample.stop == StackWalkStop::Misaligned && sample.count == 1);
  WalkFramePointers(0xAAAA, 0, at(4), lo, at(11), sample);
  Check("walk: never reads a record past the stack's end", sample.stop == StackWalkStop::OutOfStack &&
        sample.count == 2);
  WalkFramePointers(0xAAAA, 0, 0x10, lo, hi, sample);
  Check("walk: refuses a frame pointer below the stack", sample.stop == StackWalkStop::OutOfStack);

  stack[5] = 0x007F000000001111ull;  // pointer-authentication bits above bit 48
  WalkFramePointers(0x004000000000AAAAull, 0, at(4), lo, hi, sample);
  Check("walk: strips pointer-authentication bits", sample.frames[0] == 0xAAAA && sample.frames[1] == 0x1111);
  stack[5] = 0x1111;

  // A long chain fills the buffer and stops.
  std::vector<uint64_t> deep(2 * (kMaxStackFrames + 10) + 2, 0);
  for (size_t i = 0; i + 2 < deep.size(); i += 2) {
    deep[i] = reinterpret_cast<uint64_t>(&deep[i + 2]);
    deep[i + 1] = 0x100 + i;
  }
  WalkFramePointers(0xAAAA, 0, reinterpret_cast<uint64_t>(&deep[0]), reinterpret_cast<uintptr_t>(&deep[0]),
                    reinterpret_cast<uintptr_t>(&deep[0] + deep.size()), sample);
  Check("walk: stops at the frame limit", sample.stop == StackWalkStop::Full && sample.count == kMaxStackFrames);

  // /proc/self/maps: a library whose header mapping is at offset 0, and one
  // stored inside an APK at offset 0x200000.
  std::string const maps =
      "7000000000-7000001000 r--p 00000000 fd:01 11 /data/app/x/lib/arm64/libunity.so\n"
      "7000001000-7000005000 r-xp 00000000 fd:01 11 /data/app/x/lib/arm64/libunity.so\n"
      "7000010000-7000011000 rw-p 00000000 00:00 0 \n"
      "7100000000-7100001000 r--p 00200000 fd:01 12   /data/app/x/base.apk\n"
      "7100001000-7100003000 r-xp 00201000 fd:01 12   /data/app/x/base.apk\n";
  auto entries = ParseProcMaps(maps);
  Check("maps: every line parsed", entries.size() == 5, std::to_string(entries.size()));
  Check("maps: path and offset", entries.size() == 5 && entries[4].path == "/data/app/x/base.apk" &&
        entries[4].offset == 0x201000 && entries[2].path.empty() && entries[1].readable);
  // Fake memory: an ELF header with one PT_LOAD at vaddr 0 at each image start.
  auto fakeMemory = [](uint64_t address, void* buffer, size_t length) {
    static std::vector<uint8_t> image = [] {
      std::vector<uint8_t> bytes(sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr), 0);
      Elf64_Ehdr header{};
      std::memcpy(header.e_ident, ELFMAG, SELFMAG);
      header.e_ident[EI_CLASS] = ELFCLASS64;
      header.e_phoff = sizeof(Elf64_Ehdr);
      header.e_phentsize = sizeof(Elf64_Phdr);
      header.e_phnum = 1;
      Elf64_Phdr load{};
      load.p_type = PT_LOAD;
      std::memcpy(bytes.data(), &header, sizeof(header));
      std::memcpy(bytes.data() + sizeof(header), &load, sizeof(load));
      return bytes;
    }();
    for (uint64_t base : {0x7000000000ull, 0x7100000000ull}) {
      if (address >= base && address + length <= base + image.size()) {
        std::memcpy(buffer, image.data() + (address - base), length);
        return true;
      }
    }
    return false;
  };
  auto unity = ResolveAddress(0x70000017dcull, entries, fakeMemory);
  Check("resolve: library offset from its load base", unity.known && unity.module == "libunity.so" &&
        unity.offset == 0x17dc, unity.module + " " + std::to_string(unity.offset));
  auto apk = ResolveAddress(0x7100001234ull, entries, fakeMemory);
  Check("resolve: library inside an APK", apk.known && apk.module == "base.apk" && apk.offset == 0x1234,
        apk.module + " " + std::to_string(apk.offset));
  auto anon = ResolveAddress(0x7000010010ull, entries, fakeMemory);
  Check("resolve: anonymous memory keeps the absolute address", !anon.known && anon.module == "[anonymous]" &&
        anon.offset == 0x7000010010ull);
  auto none = ResolveAddress(0x10, entries, fakeMemory);
  Check("resolve: unmapped address", !none.known && none.module == "?" && none.offset == 0x10);
  auto noHeader = ResolveAddress(0x70000017dcull, entries, [](uint64_t, void*, size_t) { return false; });
  Check("resolve: no readable header falls back to the file offset", noHeader.known && noHeader.offset == 0x7dc,
        std::to_string(noHeader.offset));

  Check("format: frame line", FormatFrame(0, unity, false) == "#00 pc 00000000000017dc  libunity.so",
        FormatFrame(0, unity, false));
  Check("format: lr frame", FormatFrame(1, unity, true) == "#01 pc 00000000000017dc  libunity.so (lr)",
        FormatFrame(1, unity, true));
  Check("format: anonymous frame", FormatFrame(2, anon, false) == "#02 pc 0000007000010010  [anonymous] (absolute)",
        FormatFrame(2, anon, false));
  return gSelftestFailures == 0 ? 0 : 1;
}

std::atomic<bool> gSpinning{false};
std::atomic<bool> gStopSpinning{false};

__attribute__((noinline)) void SpinHere() {
  volatile uint64_t spin = 0;
  gSpinning = true;
  while (!gStopSpinning.load(std::memory_order_relaxed)) spin = spin + 1;
}

void IgnoreSignal(int) {}

int SampleCommand(std::string const& mode) {
  using namespace Vivify;
  if (mode == "taken") {
    struct sigaction other {};
    other.sa_handler = &IgnoreSignal;
    sigaction(StackSampler::ChosenSignal(), &other, nullptr);
  }
  std::thread spinner([&] {
    if (mode == "blocked") {
      sigset_t set;
      sigemptyset(&set);
      sigaddset(&set, StackSampler::ChosenSignal());
      pthread_sigmask(SIG_BLOCK, &set, nullptr);
    }
    StackSampler::RegisterTarget();
    SpinHere();
  });
  while (!gSpinning) std::this_thread::sleep_for(milliseconds(1));
  StackSample sample;
  auto const result = StackSampler::Sample(milliseconds(500), sample);
  // A second request must work too (the handler stays installed).
  StackSample second;
  auto const again = result == StackSampler::Result::Ok ? StackSampler::Sample(milliseconds(500), second) : result;
  gStopSpinning = true;
  spinner.join();
  std::printf("result=%s\n", StackSampler::DescribeResult(result).c_str());
  std::printf("again=%s\n", StackSampler::DescribeResult(again).c_str());
  if (result != StackSampler::Result::Ok) return 0;
  auto const maps = ParseProcMaps([] {
    std::ifstream in("/proc/self/maps");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }());
  auto const spin = ResolveAddress(reinterpret_cast<uint64_t>(&SpinHere), maps, &ReadOwnMemory);
  std::printf("spin=%s %llx\n", spin.module.c_str(), static_cast<unsigned long long>(spin.offset));
  std::printf("stop=%s\n", DescribeStop(sample.stop));
  for (int i = 0; i < sample.count; i++) {
    std::printf("%s\n", FormatFrame(i, ResolveAddress(sample.frames[i], maps, &ReadOwnMemory),
                                    i == 1 && sample.lrUnconfirmed).c_str());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "parse") {
    std::ifstream in(argv[2], std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto stat = Vivify::ParseTaskStat(text);
    if (!stat) {
      std::printf("unparsed\n");
      return 1;
    }
    std::printf("state=%c utime=%llu stime=%llu\n", stat->state, static_cast<unsigned long long>(stat->utimeTicks),
                static_cast<unsigned long long>(stat->stimeTicks));
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "selftest") return Selftest();
  if (argc >= 3 && std::string(argv[1]) == "sample") return SampleCommand(argv[2]);
  if (argc >= 7 && std::string(argv[1]) == "run") {
    int const healthyMs = std::atoi(argv[2]);
    int const stallMs = std::atoi(argv[3]);
    int const afterMs = std::atoi(argv[4]);
    std::string const mode = argv[5];
    std::mutex linesMutex;
    std::vector<std::string> lines;
    Vivify::Watchdog::Options options;
    options.stallAfter = milliseconds(200);
    options.reportEvery = milliseconds(200);
    options.poll = milliseconds(20);
    options.cpuSample = milliseconds(100);
    options.maxReports = std::atoi(argv[6]);
    options.renderThreadName = "no-such-thread";
    options.sampleEvery = milliseconds(100);
    options.sampleTimeout = milliseconds(200);
    Vivify::Watchdog watchdog;
    watchdog.Start(
        [&](std::string const& line) {
          std::lock_guard lock(linesMutex);
          lines.push_back(line);
        },
        options);
    auto beatFor = [&](int ms) {
      auto const end = steady_clock::now() + milliseconds(ms);
      while (steady_clock::now() < end) {
        watchdog.Beat();
        std::this_thread::sleep_for(milliseconds(10));
      }
    };
    beatFor(healthyMs);
    watchdog.SetBreadcrumb("phase two");
    if (mode == "paused") watchdog.SetPaused(true);
    auto const stallEnd = steady_clock::now() + milliseconds(stallMs);
    if (mode == "busy") {
      volatile uint64_t spin = 0;
      while (steady_clock::now() < stallEnd) spin = spin + 1;
    } else {
      std::this_thread::sleep_until(stallEnd);
    }
    if (mode == "paused") watchdog.SetPaused(false);
    beatFor(afterMs);
    watchdog.Stop();
    for (auto const& line : lines) std::printf("%s\n", line.c_str());
    return 0;
  }
  std::fprintf(stderr,
               "usage: watchdog parse <file> | watchdog run <healthyMs> <stallMs> <afterMs> <mode> <max> | "
               "watchdog selftest | watchdog sample <spin|taken|blocked>\n");
  return 2;
}
