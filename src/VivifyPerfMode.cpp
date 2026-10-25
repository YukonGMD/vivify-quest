#include "VivifyPerfMode.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace Vivify {

namespace {

float RoundToPercent(float scale) {
  return std::round(scale * 100.0f) / 100.0f;
}

}  // namespace

void ViewportController::Reset(Config const& config) {
  _config = config;
  _config.floor = std::clamp(_config.floor, 0.3f, 1.0f);
  if (!(_config.targetMs > 1.0f)) _config.targetMs = 1000.0f / 72.0f;
  _scale = 1.0f;
  _smoothed = 0.0f;
  _primed = false;
  _lastDown = _lastUp = -1e9;
  _headroomSince = -1.0;
  _framesSinceStep = 1000;
  _reason = "";
}

float ViewportController::Update(float measuredMs, bool gpuTime, double now, float cap, bool hold) {
  cap = std::clamp(cap, _config.floor, 1.0f);
  if (hold && _scale != 1.0f) {
    _scale = 1.0f;
    _reason = "held at full size";
  }
  if (!(measuredMs >= 0.0f) || !std::isfinite(measuredMs)) return _scale;
  _framesSinceStep++;
  // GPU times move smoothly; frame times come in whole refreshes and need more
  // smoothing.
  float const alpha = gpuTime ? 0.25f : 0.15f;
  if (!_primed) {
    _smoothed = measuredMs;
    _primed = true;
  } else {
    _smoothed += alpha * (measuredMs - _smoothed);
  }
  if (hold) {
    _headroomSince = -1.0;
    return _scale;
  }
  float const target = _config.targetMs;
  // A GPU time near the budget already misses refreshes now and then; a frame
  // time over the budget means it missed.
  bool const over = gpuTime ? _smoothed > target * 0.95f : _smoothed > target * 1.15f;
  bool const headroom = gpuTime ? _smoothed < target * 0.75f : _smoothed <= target * 1.05f;

  if (_scale > cap + 0.001f) {
    // A learned profile says a heavy part is coming: ease toward it.
    _headroomSince = -1.0;
    if (now - _lastDown >= _config.downEvery) {
      _scale = RoundToPercent(std::max(cap, _scale - _config.minStep));
      _lastDown = now;
      _reason = "easing before a part known to be heavy";
    }
    return _scale;
  }
  if (over) {
    _headroomSince = -1.0;
    if (_scale > _config.floor && now - _lastDown >= _config.downEvery && _framesSinceStep >= 3) {
      // The GPU's work grows with the pixels drawn, the square of the scale:
      // aim at 90% of the budget, in steps of 2-5%.
      float const load = _smoothed / target;
      float const wanted = _scale * std::sqrt(0.9f / load);
      float const step = std::clamp(_scale - wanted, _config.minStep, _config.maxStep);
      _scale = RoundToPercent(std::max(_config.floor, _scale - step));
      _lastDown = now;
      _framesSinceStep = 0;
      _reason = "over the frame budget";
    }
    return _scale;
  }
  if (!headroom) {
    _headroomSince = -1.0;  // in the band between: leave it
    return _scale;
  }
  if (_headroomSince < 0.0) _headroomSince = now;
  if (_scale < cap && now - _headroomSince >= _config.upAfter && now - _lastUp >= _config.upEvery &&
      now - _lastDown >= _config.upAfter) {
    _scale = RoundToPercent(std::min(cap, _scale + _config.stepUp));
    _lastUp = now;
    _reason = "headroom";
  }
  return _scale;
}

char const* PerfMechanismName(PerfMechanism mechanism) {
  switch (mechanism) {
    case PerfMechanism::Scale: return "viewport scale";
    case PerfMechanism::Foveation: return "foveation";
    case PerfMechanism::Boost: return "GPU boost";
    case PerfMechanism::Count: break;
  }
  return "?";
}

void PerfModeChangeLog::Reset(float hitchMs) {
  _hitchMs = hitchMs;
  _count = _dropped = _pendingFrom = 0;
  _recentCount = _recentNext = 0;
}

unsigned PerfModeChangeLog::AddFrame(float frameMs) {
  unsigned hitched = 0;
  for (int i = _pendingFrom; i < _count; i++) {
    Change& change = _changes[static_cast<size_t>(i)];
    if (change.framesAfter >= kWindow) continue;
    change.frames[kWindow + change.framesAfter] = frameMs;
    change.framesAfter++;
    if (change.hitched || change.framesAfter > 3 || !(frameMs > _hitchMs)) continue;
    float before[kWindow];
    std::copy(change.frames + kWindow - change.framesBefore, change.frames + kWindow, before);
    float median = 0.0f;
    if (change.framesBefore > 0) {
      std::nth_element(before, before + change.framesBefore / 2, before + change.framesBefore);
      median = before[change.framesBefore / 2];
    }
    if (frameMs > 2.0f * median) {
      change.hitched = true;
      hitched |= 1u << static_cast<unsigned>(change.mechanism);
    }
  }
  while (_pendingFrom < _count && _changes[static_cast<size_t>(_pendingFrom)].framesAfter >= kWindow) _pendingFrom++;
  _recent[_recentNext] = frameMs;
  _recentNext = (_recentNext + 1) % kWindow;
  if (_recentCount < kWindow) _recentCount++;
  return hitched;
}

void PerfModeChangeLog::Record(double wallSeconds, float songSeconds, PerfMechanism mechanism, float before,
                               float after, char const* reason) {
  if (_count >= kCapacity) {
    _dropped++;
    return;
  }
  Change& change = _changes[static_cast<size_t>(_count++)];
  change = Change();
  change.wallSeconds = wallSeconds;
  change.songSeconds = songSeconds;
  change.mechanism = mechanism;
  change.before = before;
  change.after = after;
  std::snprintf(change.reason, sizeof(change.reason), "%s", reason != nullptr ? reason : "");
  // The last frames added, oldest first, end at frames[kWindow - 1].
  change.framesBefore = _recentCount;
  for (int i = 0; i < _recentCount; i++) {
    int const from = (_recentNext - _recentCount + i + kWindow) % kWindow;
    change.frames[kWindow - _recentCount + i] = _recent[from];
  }
}

std::vector<std::string> PerfModeChangeLog::Lines() const {
  std::vector<std::string> lines;
  for (int i = 0; i < _count; i++) {
    Change const& change = _changes[static_cast<size_t>(i)];
    char head[192];
    std::snprintf(head, sizeof(head), "song %.2fs (wall +%.3fs): %s %.2f -> %.2f (%s); frames before:",
                  change.songSeconds, change.wallSeconds, PerfMechanismName(change.mechanism), change.before,
                  change.after, change.reason);
    std::string line = head;
    char number[16];
    for (int f = kWindow - change.framesBefore; f < kWindow; f++) {
      std::snprintf(number, sizeof(number), " %.1f", change.frames[f]);
      line += number;
    }
    line += " | after:";
    for (int f = 0; f < change.framesAfter; f++) {
      std::snprintf(number, sizeof(number), " %.1f", change.frames[kWindow + f]);
      line += number;
    }
    if (change.hitched) line += " -- a hitch followed: this mechanism was turned off for the song";
    lines.push_back(std::move(line));
  }
  if (_dropped > 0) lines.push_back(std::to_string(_dropped) + " further change(s) not kept (the log is full)");
  return lines;
}

void PerfProfile::Clear() {
  _scale.fill(0);
  _load.fill(0);
  _seconds = 0;
}

void PerfProfile::Note(float songSeconds, float scale, float loadPercent) {
  if (!(songSeconds >= 0.0f) || !(scale > 0.0f)) return;
  int const second = static_cast<int>(songSeconds);
  if (second >= kMaxSeconds) return;
  auto const percent = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(scale * 100.0f)), 1, 100));
  uint8_t& stored = _scale[static_cast<size_t>(second)];
  if (stored == 0 || percent < stored) stored = percent;
  if (loadPercent >= 0.0f) {
    auto const load = static_cast<uint16_t>(std::min(65535L, std::lround(loadPercent)));
    _load[static_cast<size_t>(second)] = std::max(_load[static_cast<size_t>(second)], load);
  }
  _seconds = std::max(_seconds, second + 1);
}

void PerfProfile::MergeFrom(PerfProfile const& play) {
  for (int s = 0; s < play._seconds; s++) {
    auto const index = static_cast<size_t>(s);
    if (play._scale[index] == 0) continue;
    if (_scale[index] == 0) {
      _scale[index] = play._scale[index];
      _load[index] = play._load[index];
    } else {
      _scale[index] = static_cast<uint8_t>((_scale[index] + play._scale[index] + 1) / 2);
      _load[index] = static_cast<uint16_t>((_load[index] + play._load[index] + 1) / 2);
    }
  }
  _seconds = std::max(_seconds, play._seconds);
}

float PerfProfile::CapAt(float songSeconds, float leadSeconds, float margin) const {
  if (!(songSeconds >= 0.0f)) return 1.0f;
  int const from = static_cast<int>(songSeconds);
  int const to = std::min(_seconds - 1, static_cast<int>(songSeconds + std::max(0.0f, leadSeconds)));
  int lowest = 101;
  for (int s = from; s <= to; s++) {
    uint8_t const stored = _scale[static_cast<size_t>(s)];
    if (stored != 0) lowest = std::min<int>(lowest, stored);
  }
  if (lowest > 100) return 1.0f;
  return std::min(1.0f, static_cast<float>(lowest) / 100.0f + margin);
}

int PerfProfile::HeavySeconds() const {
  int heavy = 0;
  for (int s = 0; s < _seconds; s++) {
    uint8_t const stored = _scale[static_cast<size_t>(s)];
    if (stored != 0 && stored < 95) heavy++;
  }
  return heavy;
}

std::string PerfProfile::Serialize(std::string_view key) const {
  std::string text = "# vivify-perf-profile 1\nkey " + std::string(key) + "\n";
  for (int s = 0; s < _seconds; s++) {
    auto const index = static_cast<size_t>(s);
    if (_scale[index] == 0) continue;
    text += std::to_string(s) + " " + std::to_string(_scale[index]) + " " + std::to_string(_load[index]) + "\n";
  }
  return text;
}

bool PerfProfile::Parse(std::string_view text, std::string_view key) {
  Clear();
  std::istringstream in{std::string(text)};
  std::string line;
  if (!std::getline(in, line) || line != "# vivify-perf-profile 1") return false;
  if (!std::getline(in, line) || line != "key " + std::string(key)) return false;
  while (std::getline(in, line)) {
    int second = -1, scale = 0, load = 0;
    if (std::sscanf(line.c_str(), "%d %d %d", &second, &scale, &load) != 3) continue;
    if (second < 0 || second >= kMaxSeconds || scale < 1 || scale > 100 || load < 0 || load > 65535) continue;
    _scale[static_cast<size_t>(second)] = static_cast<uint8_t>(scale);
    _load[static_cast<size_t>(second)] = static_cast<uint16_t>(load);
    _seconds = std::max(_seconds, second + 1);
  }
  return true;
}

std::string PerfProfileFileName(std::string_view key) {
  std::string name;
  for (char c : key) {
    bool const keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    name += keep ? c : '_';
    if (name.size() >= 120) break;
  }
  return name.empty() ? std::string("unnamed") : name;
}

}  // namespace Vivify
