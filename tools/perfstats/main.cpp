// Host test driver for src/VivifyPerfStats.cpp.
//
//   perfstats <refreshHz>
//     reads frames from stdin, one per line: "<ms> <songTime> <gcDelta> [label]",
//     or "skip" for a frame left out; prints the summary, one line each.
//     PerfSections (naomi.7): "S <songTime> <label>" begins a section,
//     "F <ms> <vivifyMs> <songTime>" adds a frame to it, and
//     "L <renderers> <visible> <systems> <particles> <drawCalls> <batches> <triangles>"
//     a live sample; their summary follows the song's.
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
  auto sections = std::make_unique<Vivify::PerfSections>();
  sections->Reset();
  std::string line;
  int labelCalls = 0;
  while (std::getline(std::cin, line)) {
    if (line == "skip") {
      stats->SkipFrame();
      continue;
    }
    if (line.empty()) continue;
    if (line.rfind("S ", 0) == 0) {
      std::istringstream in(line.substr(2));
      float songTime = 0;
      std::string label;
      in >> songTime;
      std::getline(in >> std::ws, label);
      sections->Begin(label, songTime);
      continue;
    }
    if (line.rfind("F ", 0) == 0) {
      std::istringstream in(line.substr(2));
      std::string msText;
      float vivify = 0, songTime = 0;
      in >> msText >> vivify >> songTime;
      sections->AddFrame(std::strtof(msText.c_str(), nullptr), vivify, songTime);
      continue;
    }
    if (line.rfind("L ", 0) == 0) {
      std::istringstream in(line.substr(2));
      Vivify::PerfSections::Live live;
      in >> live.renderers >> live.visibleRenderers >> live.particleSystems >> live.particles >> live.drawCalls >>
          live.batches >> live.triangles;
      sections->AddLive(live);
      continue;
    }
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
  for (auto const& text : sections->Summary(stats->BudgetMs())) std::printf("%s\n", text.c_str());
  std::printf("label calls: %d\n", labelCalls);
  return 0;
}
