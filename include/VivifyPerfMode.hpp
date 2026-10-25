#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Vivify {

// Performance mode for heavy maps (0.14.25-naomi.8). naomi.7 lowered the eye
// resolution with XRSettings.eyeTextureResolutionScale, which reallocates the
// eye buffers; while the game ran slowly the compositor then showed white or
// black for seconds at every step. From naomi.8 the eye buffers are never
// reallocated during a song: only the part of them that is rendered into
// shrinks (XRSettings.renderViewportScale, the path Meta's own dynamic
// resolution takes in this game), a few percent at a time.
//
// Everything here is Unity-free and allocation-free per frame, so the host
// tests can drive it; the runtime measures, applies and logs.

// Picks the viewport scale frame by frame: down fast while over the frame
// budget, up slowly once there has been headroom for a while, and nothing in
// between.
class ViewportController {
 public:
  struct Config {
    float floor = 0.6f;               // lowest scale
    float targetMs = 1000.0f / 72.0f; // frame budget: one refresh, or two
    float minStep = 0.02f;            // each step is 2-5%
    float maxStep = 0.05f;
    float stepUp = 0.02f;
    double downEvery = 0.1;  // seconds between steps down
    double upAfter = 1.0;    // seconds of headroom before a step up
    double upEvery = 0.5;    // seconds between steps up
  };
  void Reset(Config const& config);
  // One frame ending at wall-clock `now`. measuredMs is the app's GPU time
  // when gpuTime, else the frame time. cap is the highest scale allowed now (1,
  // or lower ahead of a part a learned profile knows is heavy); hold keeps the
  // full scale. Returns the scale wanted for the next frame.
  float Update(float measuredMs, bool gpuTime, double now, float cap, bool hold);
  float Scale() const { return _scale; }
  float SmoothedMs() const { return _smoothed; }
  float TargetMs() const { return _config.targetMs; }
  // Why the last change was made, for the log ("over budget", ...).
  char const* Reason() const { return _reason; }

 private:
  Config _config;
  float _scale = 1.0f;
  float _smoothed = 0.0f;
  bool _primed = false;
  double _lastDown = -1e9;
  double _lastUp = -1e9;
  double _headroomSince = -1.0;
  int _framesSinceStep = 1000;  // GPU times arrive a few frames late
  char const* _reason = "";
};

enum class PerfMechanism : uint8_t { Scale = 0, Foveation = 1, Boost = 2, Count = 3 };
char const* PerfMechanismName(PerfMechanism mechanism);

// Every change performance mode makes, with the five frame times before it
// and the five after, kept in a fixed buffer and written at the song's end.
// It is also the safety check: a change followed within three frames by one
// over hitchMs that is also over twice the median before it caused a hitch.
class PerfModeChangeLog {
 public:
  static constexpr int kCapacity = 1024;
  static constexpr int kWindow = 5;
  struct Change {
    double wallSeconds = 0.0;
    float songSeconds = 0.0f;
    PerfMechanism mechanism = PerfMechanism::Scale;
    float before = 0.0f;
    float after = 0.0f;
    char reason[64] = {};
    float frames[2 * kWindow] = {};
    int framesBefore = 0;
    int framesAfter = 0;
    bool hitched = false;
  };

  void Reset(float hitchMs = 250.0f);
  // Each measured frame. Returns a bit per mechanism (1 << mechanism) whose
  // change just turned out to have caused a hitch.
  unsigned AddFrame(float frameMs);
  void Record(double wallSeconds, float songSeconds, PerfMechanism mechanism, float before, float after,
              char const* reason);
  int Count() const { return _count; }
  int Dropped() const { return _dropped; }
  Change const& At(int index) const { return _changes[static_cast<size_t>(index)]; }
  // One line per change, without prefixes.
  std::vector<std::string> Lines() const;

 private:
  float _hitchMs = 250.0f;
  std::array<Change, kCapacity> _changes{};
  int _count = 0;
  int _dropped = 0;
  int _pendingFrom = 0;  // changes before this have all their frames
  float _recent[kWindow] = {};
  int _recentCount = 0;
  int _recentNext = 0;
};

// The learned profile of one map and difficulty (a setting): the scale each
// second of the song ended up at and the load it saw, blended over plays, so
// a later play can start easing down a little before the heavy parts.
class PerfProfile {
 public:
  static constexpr int kMaxSeconds = 1800;
  void Clear();
  // The lowest scale and the highest load (percent of the budget) seen in that
  // second of song; called once a frame, no allocation.
  void Note(float songSeconds, float scale, float loadPercent);
  // Blends a finished play in: each second it has becomes the mean of both.
  void MergeFrom(PerfProfile const& play);
  // The highest scale to allow at song time t: the lowest stored scale in
  // [t, t + lead] plus margin, or 1 where nothing is stored.
  float CapAt(float songSeconds, float leadSeconds, float margin) const;
  int Seconds() const { return _seconds; }
  // Seconds stored with a scale under 95%.
  int HeavySeconds() const;
  std::string Serialize(std::string_view key) const;
  // False when the text is not a profile for this key.
  bool Parse(std::string_view text, std::string_view key);

 private:
  std::array<uint8_t, kMaxSeconds> _scale{};  // percent; 0 = nothing stored
  std::array<uint16_t, kMaxSeconds> _load{};  // percent of the budget
  int _seconds = 0;
};

// Where learned profiles are kept (VivifyPerformanceMode.cpp; the runtime
// only).
std::string PerfProfileDir();

// A file name for a profile key (map folder, characteristic, difficulty):
// letters, digits, '-' and '_' kept, anything else '_', at most 120 long.
std::string PerfProfileFileName(std::string_view key);

}  // namespace Vivify
