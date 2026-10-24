#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Vivify {

// Frame-time statistics for one song, for the performance summary written at
// its end (0.14.25-naomi.4).
//
// AddFrame is on the hot path: it touches a fixed histogram and a fixed list
// of the worst frames, and never allocates. A frame's label is only written
// (by the caller's function, into a fixed buffer) when the frame enters that
// list. Everything that formats text runs once, at the end.
//
// Deliberately Unity-free so the host tests can drive it.
class PerfStats {
 public:
  static constexpr int kBinsPerMs = 10;  // 0.1 ms resolution
  static constexpr int kMaxMs = 250;     // longer frames share the last bin; max is exact
  static constexpr int kBins = kMaxMs * kBinsPerMs;
  static constexpr int kWorst = 10;
  static constexpr size_t kLabelSize = 192;

  struct Hitch {
    float frameMs = 0.0f;
    float songTime = 0.0f;
    int gcDelta = 0;
    char label[kLabelSize] = {};
  };

  // refreshHz <= 0: not known yet; Summary estimates it from the median frame.
  void Reset(float refreshHz);
  float RefreshHz() const { return _refreshHz; }

  // One frame: its duration (time since the previous frame), the song time
  // at its end, how many garbage collections ran during it, and a function
  // `void(char* buffer, size_t size)` that writes what Vivify did before it,
  // called only if the frame is among the worst so far.
  template <class WriteLabel>
  void AddFrame(float frameMs, float songTime, int gcDelta, WriteLabel&& writeLabel) {
    if (!(frameMs >= 0.0f)) return;  // also refuses NaN
    _frames++;
    _sumMs += frameMs;
    if (frameMs > _maxMs) _maxMs = frameMs;
    int bin = static_cast<int>(frameMs * kBinsPerMs);
    if (bin >= kBins) bin = kBins - 1;
    _histogram[bin]++;
    if (_frames == 1) _firstSongTime = songTime;
    _lastSongTime = songTime;
    int slot = WorstSlotFor(frameMs);
    if (slot < 0) return;
    Hitch& hitch = _worst[slot];
    hitch.frameMs = frameMs;
    hitch.songTime = songTime;
    hitch.gcDelta = gcDelta;
    hitch.label[0] = '\0';
    writeLabel(hitch.label, sizeof(hitch.label));
    hitch.label[sizeof(hitch.label) - 1] = '\0';
  }
  void AddFrame(float frameMs, float songTime, int gcDelta) {
    AddFrame(frameMs, songTime, gcDelta, [](char*, size_t) {});
  }
  // A frame left out (paused, or the first after a pause).
  void SkipFrame() { _skipped++; }

  int Frames() const { return _frames; }
  int Skipped() const { return _skipped; }
  float FirstSongTime() const { return _firstSongTime; }
  float LastSongTime() const { return _lastSongTime; }
  double AverageMs() const { return _frames > 0 ? _sumMs / _frames : 0.0; }
  float MaxMs() const { return _maxMs; }
  // The frame time below which `fraction` of frames fall, to 0.1 ms (the
  // upper edge of the histogram bin it lands in, capped at the exact max).
  float PercentileMs(double fraction) const;
  // The refresh rate used for the budget: the one given, or the common
  // headset rate nearest the median frame.
  float BudgetHz() const;
  float BudgetMs() const { return 1000.0f / BudgetHz(); }
  // Frames that missed at least one refresh: longer than 1.2x the budget (a
  // vsync'd frame that misses comes out at about 2x).
  int FramesOverBudget() const;
  // The worst frames, longest first.
  std::vector<Hitch> WorstFrames() const;

  // Everything else the summary reports; gathered by the caller at the
  // start and end of the song.
  struct Context {
    std::string map;
    double wallSeconds = 0.0;
    int gcStart = -1, gcEnd = -1;  // -1: unknown
    double heapUsedStartMB = -1, heapSizeStartMB = -1, heapUsedEndMB = -1, heapSizeEndMB = -1;
    int prefabsEnd = 0, prefabsPeak = 0;
    int renderersEnd = 0, renderersPeak = 0;
    int particleSystemsEnd = 0, particleSystemsPeak = 0;
    int lightsEnd = 0, lightsPeak = 0;
  };
  // The summary, one line per element, without prefixes.
  std::vector<std::string> Summary(Context const& context) const;

 private:
  // The slot in _worst a frame of this length should take, or -1.
  int WorstSlotFor(float frameMs);

  float _refreshHz = 0.0f;
  int _frames = 0;
  int _skipped = 0;
  double _sumMs = 0.0;
  float _maxMs = 0.0f;
  float _firstSongTime = 0.0f;
  float _lastSongTime = 0.0f;
  uint32_t _histogram[kBins] = {};
  Hitch _worst[kWorst];
  int _worstCount = 0;
};

// The same song split into sections (naomi.7): from one set of live prefabs to
// the next (in 743 Aether, its intro, drop, ambient, bridge, buildup and
// outro), each with its own frame times, the share of frames that held 60 fps,
// what Vivify's own per-frame work cost, and what was live and drawn, so a
// summary can say how close each part of a map gets.
//
// AddFrame is on the hot path and never allocates; Begin allocates the new
// section's label, which happens a few times a song.
class PerfSections {
 public:
  static constexpr int kMaxSections = 32;  // later sections are counted in the last
  static constexpr int kBins = 300;        // 1 ms each; longer frames share the last
  // What was live and drawn, sampled every couple of seconds. -1: unknown.
  struct Live {
    int renderers = -1;         // active renderers under the live prefabs
    int visibleRenderers = -1;  // of them, inside a camera's view last frame
    int particleSystems = -1;
    int particles = -1;         // alive particles
    int64_t drawCalls = -1, batches = -1, setPassCalls = -1, triangles = -1, vertices = -1;
  };

  void Reset();
  // From now on, a section called `label`; nothing happens when it is the
  // current one.
  void Begin(std::string const& label, float songTime);
  // One frame (its duration and Vivify's own share of it) at that song time.
  void AddFrame(float frameMs, float vivifyMs, float songTime);
  void AddLive(Live const& live);
  int Count() const { return static_cast<int>(_sections.size()); }
  // One line per section, without prefixes. budgetMs: one refresh.
  std::vector<std::string> Summary(float budgetMs) const;

 private:
  struct Section {
    std::string label;
    float startSong = 0.0f;
    float endSong = 0.0f;
    int frames = 0;
    double sumMs = 0.0;
    double sumVivifyMs = 0.0;
    float maxMs = 0.0f;
    float maxVivifyMs = 0.0f;
    uint32_t bins[kBins] = {};
    int liveSamples = 0;
    Live peak;
    double sumRenderers = 0.0, sumVisible = 0.0, sumDrawCalls = 0.0, sumBatches = 0.0, sumTriangles = 0.0;
    int drawCallSamples = 0;
  };
  float PercentileMs(Section const& section, double fraction) const;
  std::vector<Section> _sections;
};

}  // namespace Vivify
