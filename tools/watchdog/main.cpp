// Host test driver for src/VivifyWatchdog.cpp.
//
//   watchdog parse <stat-file>
//     prints state= utime= stime=, or "unparsed" (exit 1)
//   watchdog run <healthyMs> <stallMs> <afterMs> <busy|sleep|paused> <maxReports>
//     beats every 10 ms for healthyMs, stops beating for stallMs (spinning or
//     sleeping on this thread, or paused), beats again for afterMs, then stops
//     the watchdog and prints every line it wrote, one per line.
// Thresholds are scaled down: stall after 200 ms, report every 200 ms, poll
// every 20 ms, CPU sample 100 ms.
#include "VivifyWatchdog.hpp"

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
  std::fprintf(stderr, "usage: watchdog parse <file> | watchdog run <healthyMs> <stallMs> <afterMs> <mode> <max>\n");
  return 2;
}
