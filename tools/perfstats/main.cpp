// Host test driver for src/VivifyPerfStats.cpp.
//
//   perfstats <refreshHz>
//     reads frames from stdin, one per line: "<ms> <songTime> <gcDelta> [label]",
//     or "skip" for a frame left out; prints the summary, one line each.
//     PerfSections (naomi.7): "S <songTime> <label>" begins a section,
//     "F <ms> <vivifyMs> <songTime> [gpuMs [gpuBusy]]" adds a frame to it, and
//     "L <renderers> <visible> <systems> <particles> <drawCalls> <batches> <triangles>"
//     a live sample; their summary follows the song's. "D <index>" prints how
//     other summaries name that section, after everything else.
//     Performance mode (naomi.8, VivifyPerfMode.hpp):
//     "V <floor> <targetMs>" resets the viewport controller; "v <ms> <gpu 0|1>
//     <now> <cap> <hold 0|1>" runs one frame and prints "scale <s> at <now>
//     (<reason>)" when the scale changes.
//     "C <hitchMs>" resets the change log; "cf <ms>" adds a frame and prints
//     "hitch <mask>" when a change caused one; "cr <wall> <song> <mechanism>
//     <before> <after> <reason>" records a change. Its lines come last.
//     "P note <song> <scale> <load>" notes into the play being recorded;
//     "P merge" merges it into the stored profile; "P cap <t> <lead>
//     <margin>", "P heavy", "P roundtrip <key>", "P wrongkey <key> <other>"
//     print results.
#include "VivifyPerfMode.hpp"
#include "VivifyPerfStats.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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
  std::vector<int> describe;
  Vivify::ViewportController controller;
  auto changes = std::make_unique<Vivify::PerfModeChangeLog>();
  bool changeLogUsed = false;
  auto play = std::make_unique<Vivify::PerfProfile>();
  auto stored = std::make_unique<Vivify::PerfProfile>();
  play->Clear();
  stored->Clear();
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
      float vivify = 0, songTime = 0, gpu = -1, busy = -1;
      int gc = 0;
      in >> msText >> vivify >> songTime;
      if (!(in >> gpu)) gpu = -1;
      if (!(in >> busy)) busy = -1;
      if (!(in >> gc)) gc = 0;
      sections->AddFrame(std::strtof(msText.c_str(), nullptr), vivify, songTime, gpu, busy, gc);
      continue;
    }
    if (line.rfind("V ", 0) == 0) {
      std::istringstream in(line.substr(2));
      Vivify::ViewportController::Config config;
      in >> config.floor >> config.targetMs;
      controller.Reset(config);
      continue;
    }
    if (line.rfind("v ", 0) == 0) {
      std::istringstream in(line.substr(2));
      std::string msText;
      int gpu = 0, hold = 0;
      double now = 0;
      float cap = 1;
      in >> msText >> gpu >> now >> cap >> hold;
      float const before = controller.Scale();
      float const after = controller.Update(std::strtof(msText.c_str(), nullptr), gpu != 0, now, cap, hold != 0);
      if (after != before) std::printf("scale %.2f at %.3f (%s)\n", after, now, controller.Reason());
      continue;
    }
    if (line.rfind("C ", 0) == 0) {
      changes->Reset(std::strtof(line.c_str() + 2, nullptr));
      changeLogUsed = true;
      continue;
    }
    if (line.rfind("cf ", 0) == 0) {
      unsigned const hitched = changes->AddFrame(std::strtof(line.c_str() + 3, nullptr));
      if (hitched != 0) std::printf("hitch %u\n", hitched);
      continue;
    }
    if (line.rfind("cr ", 0) == 0) {
      std::istringstream in(line.substr(3));
      double wall = 0;
      float song = 0, before = 0, after = 0;
      int mechanism = 0;
      std::string reason;
      in >> wall >> song >> mechanism >> before >> after;
      std::getline(in >> std::ws, reason);
      changes->Record(wall, song, static_cast<Vivify::PerfMechanism>(mechanism), before, after, reason.c_str());
      continue;
    }
    if (line.rfind("P ", 0) == 0) {
      std::istringstream in(line.substr(2));
      std::string command;
      in >> command;
      if (command == "note") {
        float song = 0, scale = 0, load = 0;
        in >> song >> scale >> load;
        play->Note(song, scale, load);
      } else if (command == "merge") {
        stored->MergeFrom(*play);
        play->Clear();
      } else if (command == "cap") {
        float t = 0, lead = 0, margin = 0;
        in >> t >> lead >> margin;
        std::printf("cap %.2f\n", stored->CapAt(t, lead, margin));
      } else if (command == "heavy") {
        std::printf("heavy %d of %d\n", stored->HeavySeconds(), stored->Seconds());
      } else if (command == "roundtrip") {
        std::string key;
        in >> key;
        std::string const text = stored->Serialize(key);
        auto back = std::make_unique<Vivify::PerfProfile>();
        bool const parsed = back->Parse(text, key);
        std::printf("roundtrip parsed=%d same=%d lines=%d file=%s\n", parsed, back->Serialize(key) == text,
                    static_cast<int>(std::count(text.begin(), text.end(), '\n')),
                    Vivify::PerfProfileFileName(key).c_str());
      } else if (command == "wrongkey") {
        std::string key, other;
        in >> key >> other;
        auto back = std::make_unique<Vivify::PerfProfile>();
        std::printf("wrongkey parsed=%d\n", back->Parse(stored->Serialize(key), other));
      }
      continue;
    }
    if (line.rfind("D ", 0) == 0) {
      describe.push_back(std::atoi(line.c_str() + 2));
      continue;
    }
    if (line.rfind("L ", 0) == 0) {
      std::istringstream in(line.substr(2));
      Vivify::PerfSections::Live live;
      in >> live.renderers >> live.visibleRenderers >> live.particleSystems >> live.particles >> live.drawCalls >>
          live.batches >> live.triangles;
      if (!(in >> live.residentMB >> live.nativeHeapMB >> live.hottestC >> live.batteryC >> live.gpuMHz)) {
        live.residentMB = live.nativeHeapMB = live.gpuMHz = -1;
        live.hottestC = live.batteryC = -1000;
      }
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
  for (int index : describe) std::printf("describe %d: %s\n", index, sections->Describe(index).c_str());
  if (changeLogUsed) {
    for (auto const& text : changes->Lines()) std::printf("change: %s\n", text.c_str());
  }
  return 0;
}
