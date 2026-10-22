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

}  // namespace Vivify
