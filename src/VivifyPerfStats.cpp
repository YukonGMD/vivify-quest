#include "VivifyPerfStats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Vivify {

namespace {

std::string Format(char const* format, auto... args) {
  char buffer[512];
  std::snprintf(buffer, sizeof(buffer), format, args...);
  return buffer;
}

}  // namespace

void PerfStats::Reset(float refreshHz) {
  *this = PerfStats();
  _refreshHz = refreshHz;
}

int PerfStats::WorstSlotFor(float frameMs) {
  if (_worstCount < kWorst) return _worstCount++;
  int shortest = 0;
  for (int i = 1; i < kWorst; i++) {
    if (_worst[i].frameMs < _worst[shortest].frameMs) shortest = i;
  }
  return frameMs > _worst[shortest].frameMs ? shortest : -1;
}

float PerfStats::PercentileMs(double fraction) const {
  if (_frames == 0) return 0.0f;
  uint64_t const wanted = static_cast<uint64_t>(std::ceil(fraction * _frames));
  uint64_t seen = 0;
  for (int bin = 0; bin < kBins; bin++) {
    seen += _histogram[bin];
    if (seen >= std::max<uint64_t>(wanted, 1)) {
      if (bin == kBins - 1) return _maxMs;
      return std::min(static_cast<float>(bin + 1) / kBinsPerMs, _maxMs);
    }
  }
  return _maxMs;
}

float PerfStats::BudgetHz() const {
  if (_refreshHz > 0.0f) return _refreshHz;
  float const median = PercentileMs(0.5);
  if (median <= 0.0f) return 72.0f;
  float best = 72.0f;
  for (float hz : {60.0f, 72.0f, 80.0f, 90.0f, 120.0f}) {
    if (std::fabs(1000.0f / hz - median) < std::fabs(1000.0f / best - median)) best = hz;
  }
  return best;
}

int PerfStats::FramesOverBudget() const {
  float const threshold = BudgetMs() * 1.2f;
  int over = 0;
  for (int bin = 0; bin < kBins; bin++) {
    // A bin counts when all of it is past the threshold.
    if (static_cast<float>(bin) / kBinsPerMs >= threshold) over += static_cast<int>(_histogram[bin]);
  }
  return over;
}

std::vector<PerfStats::Hitch> PerfStats::WorstFrames() const {
  std::vector<Hitch> worst(_worst, _worst + _worstCount);
  std::sort(worst.begin(), worst.end(), [](Hitch const& a, Hitch const& b) { return a.frameMs > b.frameMs; });
  return worst;
}

std::vector<std::string> PerfStats::Summary(Context const& c) const {
  std::vector<std::string> lines;
  lines.push_back("=== song performance: " + c.map + " ===");
  float const hz = BudgetHz();
  lines.push_back(Format("song %.1fs to %.1fs (%.1fs wall clock), %d frames measured, %d left out (paused)",
                         _firstSongTime, _lastSongTime, c.wallSeconds, _frames, _skipped));
  lines.push_back(Format("refresh %.0f Hz%s, frame budget %.1f ms", hz,
                         _refreshHz > 0.0f ? "" : " (not reported by the headset; nearest to the median frame)",
                         1000.0f / hz));
  if (_frames == 0) return lines;
  lines.push_back(Format("frame time: average %.1f ms, median %.1f, 95th percentile %.1f, 99th %.1f, max %.1f",
                         AverageMs(), PercentileMs(0.5), PercentileMs(0.95), PercentileMs(0.99), _maxMs));
  int const over = FramesOverBudget();
  lines.push_back(Format("frames that missed a refresh (over %.1f ms): %d (%.1f%%)", BudgetMs() * 1.2f, over,
                         100.0 * over / _frames));
  auto mb = [](double value) { return value < 0 ? std::string("?") : Format("%.1fMB", value); };
  auto count = [](int value) { return value < 0 ? std::string("?") : std::to_string(value); };
  std::string gc = "garbage collections: " + count(c.gcStart) + " at start, " + count(c.gcEnd) + " at end";
  if (c.gcStart >= 0 && c.gcEnd >= 0) gc += Format(" (%d during the song)", c.gcEnd - c.gcStart);
  lines.push_back(gc + "; managed heap used " + mb(c.heapUsedStartMB) + " of " + mb(c.heapSizeStartMB) +
                  " at start, " + mb(c.heapUsedEndMB) + " of " + mb(c.heapSizeEndMB) + " at end");
  lines.push_back(Format("Vivify prefab instances: %d at end (peak %d), with renderers %d (peak %d), particle "
                         "systems %d (peak %d), lights %d (peak %d)",
                         c.prefabsEnd, c.prefabsPeak, c.renderersEnd, c.renderersPeak, c.particleSystemsEnd,
                         c.particleSystemsPeak, c.lightsEnd, c.lightsPeak));
  auto const worst = WorstFrames();
  lines.push_back(Format("worst %d frame(s):", static_cast<int>(worst.size())));
  for (size_t i = 0; i < worst.size(); i++) {
    Hitch const& h = worst[i];
    std::string const gc = h.gcDelta < 0 ? std::string("garbage collections unknown")
                                         : Format("%d garbage collection(s) during it", h.gcDelta);
    lines.push_back(Format("  %2d. %7.1f ms at song %.2fs, %s; %s", static_cast<int>(i + 1), h.frameMs,
                           h.songTime, gc.c_str(),
                           h.label[0] ? h.label : "no Vivify event or breadcrumb recorded before it"));
  }
  return lines;
}

void PerfSections::Reset() {
  _sections.clear();
  _sections.reserve(kMaxSections);
}

void PerfSections::Begin(std::string const& label, float songTime) {
  if (!_sections.empty() && _sections.back().label == label) return;
  if (static_cast<int>(_sections.size()) >= kMaxSections) {
    // Out of room: the last section takes the rest of the song.
    _sections.back().label = "later sections";
    return;
  }
  _sections.emplace_back();
  _sections.back().label = label;
  _sections.back().startSong = _sections.back().endSong = songTime;
}

std::string PerfSections::Describe(int index) const {
  if (index < 0 || index >= Count()) return "section " + std::to_string(index);
  Section const& section = _sections[static_cast<size_t>(index)];
  char range[64];
  std::snprintf(range, sizeof(range), " (song %.1f-%.1fs)", section.startSong, section.endSong);
  return "'" + section.label + "'" + range;
}

void PerfSections::AddFrame(float frameMs, float vivifyMs, float songTime, float gpuMs, float gpuBusy) {
  if (_sections.empty() || !(frameMs >= 0.0f)) return;
  Section& section = _sections.back();
  if (gpuMs >= 0.0f) {
    section.gpuFrames++;
    section.maxGpuMs = std::max(section.maxGpuMs, gpuMs);
    section.gpuBins[std::min(static_cast<int>(gpuMs), kBins - 1)]++;
  }
  if (gpuBusy >= 0.0f) {
    section.busyFrames++;
    section.sumBusy += gpuBusy;
  }
  if (section.frames == 0) section.startSong = songTime;
  section.endSong = songTime;
  section.frames++;
  section.sumMs += frameMs;
  section.maxMs = std::max(section.maxMs, frameMs);
  if (vivifyMs >= 0.0f) {
    section.sumVivifyMs += vivifyMs;
    section.maxVivifyMs = std::max(section.maxVivifyMs, vivifyMs);
  }
  int bin = static_cast<int>(frameMs);
  if (bin >= kBins) bin = kBins - 1;
  section.bins[bin]++;
}

void PerfSections::AddLive(Live const& live) {
  if (_sections.empty()) return;
  Section& section = _sections.back();
  section.liveSamples++;
  auto peak = [](auto& to, auto value) {
    if (value > to) to = value;
  };
  peak(section.peak.renderers, live.renderers);
  peak(section.peak.visibleRenderers, live.visibleRenderers);
  peak(section.peak.particleSystems, live.particleSystems);
  peak(section.peak.particles, live.particles);
  peak(section.peak.drawCalls, live.drawCalls);
  peak(section.peak.batches, live.batches);
  peak(section.peak.setPassCalls, live.setPassCalls);
  peak(section.peak.triangles, live.triangles);
  peak(section.peak.vertices, live.vertices);
  section.sumRenderers += std::max(live.renderers, 0);
  section.sumVisible += std::max(live.visibleRenderers, 0);
  if (live.drawCalls >= 0) {
    section.drawCallSamples++;
    section.sumDrawCalls += static_cast<double>(live.drawCalls);
    section.sumBatches += static_cast<double>(std::max<int64_t>(live.batches, 0));
    section.sumTriangles += static_cast<double>(std::max<int64_t>(live.triangles, 0));
  }
}

// The upper edge of the bin holding that fraction of the frames, at most max.
float PerfSections::Percentile(uint32_t const* bins, int count, double fraction) {
  if (count == 0) return 0.0f;
  uint64_t const wanted = std::max<uint64_t>(static_cast<uint64_t>(std::ceil(fraction * count)), 1);
  uint64_t seen = 0;
  for (int bin = 0; bin < kBins; bin++) {
    seen += bins[bin];
    if (seen >= wanted) return static_cast<float>(bin + 1);
  }
  return static_cast<float>(kBins);
}

float PerfSections::PercentileMs(Section const& section, double fraction) const {
  if (section.frames == 0) return 0.0f;
  uint64_t const wanted = std::max<uint64_t>(static_cast<uint64_t>(std::ceil(fraction * section.frames)), 1);
  uint64_t seen = 0;
  for (int bin = 0; bin < kBins; bin++) {
    seen += section.bins[bin];
    if (seen >= wanted) return bin == kBins - 1 ? section.maxMs : std::min(static_cast<float>(bin + 1), section.maxMs);
  }
  return section.maxMs;
}

std::vector<std::string> PerfSections::Summary(float budgetMs) const {
  std::vector<std::string> lines;
  if (_sections.empty()) return lines;
  lines.push_back(Format("by section (%d), with the share of frames that held 60 fps (16.7 ms or less):",
                         static_cast<int>(_sections.size())));
  for (Section const& section : _sections) {
    if (section.frames == 0) continue;
    // Frames of 16.7 ms or less, counted by whole bins below 17 ms.
    uint64_t held60 = 0, missed = 0;
    for (int bin = 0; bin < kBins; bin++) {
      if (bin < 17) held60 += section.bins[bin];
      if (static_cast<float>(bin) >= budgetMs * 1.2f) missed += section.bins[bin];
    }
    float const median = PercentileMs(section, 0.5);
    std::string line = Format(
        "  '%s' song %.1f-%.1fs: %d frames, median %.0f ms (%.0f fps), 95th %.0f, max %.0f; 60 fps in %.0f%% of "
        "frames; missed a refresh in %.0f%%; Vivify's own work %.2f ms a frame on average (max %.1f)",
        section.label.c_str(), section.startSong, section.endSong, section.frames, median,
        median > 0.0f ? 1000.0f / median : 0.0f, PercentileMs(section, 0.95), section.maxMs,
        100.0 * held60 / section.frames, 100.0 * missed / section.frames, section.sumVivifyMs / section.frames,
        section.maxVivifyMs);
    if (section.gpuFrames > 0) {
      line += Format("; GPU time (OVR plugin) median %.0f ms, 95th %.0f, max %.0f",
                     std::min(Percentile(section.gpuBins, section.gpuFrames, 0.5), section.maxGpuMs),
                     std::min(Percentile(section.gpuBins, section.gpuFrames, 0.95), section.maxGpuMs), section.maxGpuMs);
    }
    if (section.busyFrames > 0) line += Format("; GPU busy %.0f%%", section.sumBusy / section.busyFrames);
    if (section.liveSamples > 0) {
      line += Format("; live: %.0f renderers (%.0f in view, peak %d), peak %d particle systems with %d particles",
                     section.sumRenderers / section.liveSamples, section.sumVisible / section.liveSamples,
                     section.peak.visibleRenderers, section.peak.particleSystems, section.peak.particles);
      if (section.drawCallSamples > 0) {
        line += Format("; draw calls %.0f (peak %lld), batches %.0f, triangles %.0f",
                       section.sumDrawCalls / section.drawCallSamples, static_cast<long long>(section.peak.drawCalls),
                       section.sumBatches / section.drawCallSamples, section.sumTriangles / section.drawCallSamples);
      }
    }
    lines.push_back(line);
  }
  return lines;
}

}  // namespace Vivify
