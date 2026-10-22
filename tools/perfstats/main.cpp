// Host test driver for src/VivifyPerfStats.cpp.
//
//   perfstats <refreshHz>
//     reads frames from stdin, one per line: "<ms> <songTime> <gcDelta> [label]",
//     or "skip" for a frame left out; prints the summary, one line each.
#include "VivifyPerfStats.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: perfstats <refreshHz> < frames\n");
    return 2;
  }
  auto stats = std::make_unique<Vivify::PerfStats>();
  stats->Reset(static_cast<float>(std::atof(argv[1])));
  std::string line;
  int labelCalls = 0;
  while (std::getline(std::cin, line)) {
    if (line == "skip") {
      stats->SkipFrame();
      continue;
    }
    if (line.empty()) continue;
    std::istringstream in(line);
    std::string msText;
    float songTime = 0;
    int gc = 0;
    std::string label;
    in >> msText >> songTime >> gc;
    float const ms = std::strtof(msText.c_str(), nullptr);  // accepts "nan"

    std::getline(in >> std::ws, label);
    stats->AddFrame(ms, songTime, gc, [&](char* buffer, size_t size) {
      labelCalls++;
      std::snprintf(buffer, size, "%s", label.c_str());
    });
  }
  Vivify::PerfStats::Context context;
  context.map = "test map";
  context.wallSeconds = 12.5;
  context.gcStart = 4;
  context.gcEnd = 7;
  context.heapUsedStartMB = 60.25;
  context.heapSizeStartMB = 70;
  context.prefabsEnd = 2;
  context.prefabsPeak = 5;
  for (auto const& text : stats->Summary(context)) std::printf("%s\n", text.c_str());
  std::printf("label calls: %d\n", labelCalls);
  return 0;
}
