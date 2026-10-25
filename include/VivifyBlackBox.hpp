#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The black box (0.14.25-naomi.10): the last few thousand detailed events of
// a session, kept in a memory-mapped file.
//
// A record is written with plain stores into a MAP_SHARED mapping of
// Sessions/blackbox.bin. The kernel owns those pages, so they reach the file
// even when the game is killed or crashes: no signal handler is involved (the
// crash guard's handlers are left alone) and no system call is made per
// event. The next launch reads what the previous session left before
// starting its own: the state says whether that session quit, went to the
// background, or stopped while running.
//
// Unity-free; tools/sessionlog tests it, including a process that aborts
// mid-session.
namespace Vivify {

class BlackBox {
 public:
  enum State : uint32_t { kEmpty = 0, kRunning = 1, kPaused = 2, kClean = 3 };

  enum Type : uint16_t {
    kFrame = 1,   // values: frame ms, GPU ms (-1 unknown), viewport scale
    kLoad,        // a bundle, level or prefab loaded or started
    kUnload,      // a level ended, a bundle released
    kCamera,      // a camera set up: values = target width, height, depth
    kTarget,      // a render target: values = width, height, format
    kScale,       // a performance-mode change: values = before, after
    kShader,      // a shader prepared or a compile stall seen: values = ms
    kEvent,       // a Vivify custom event
    kCrumb,       // a breadcrumb
    kWarning,     // a warning or error log line
    kGl,          // a GL error
    kSession,     // session state changes
  };

  struct Previous {
    bool found = false;  // a black box from an earlier session was there
    State state = kEmpty;
    std::string session;             // its session file name
    std::vector<std::string> lines;  // its records, oldest first, formatted
  };

  BlackBox() = default;
  ~BlackBox();
  BlackBox(BlackBox const&) = delete;
  BlackBox& operator=(BlackBox const&) = delete;

  // Maps `path` (created at the right size when missing or of another
  // layout), returns what the previous session left, and starts this one.
  // False when the file could not be mapped; Record then does nothing.
  bool Open(std::string const& path, std::string_view session, uint32_t capacity, Previous& previous);
  void Close();
  bool IsOpen() const { return _header != nullptr; }

  // Safe from any thread, lock-free. Text beyond the record's room is cut.
  void Record(Type type, float songTime, std::string_view text, float a = 0.0f, float b = 0.0f, float c = 0.0f);
  void SetState(State state);
  // Asks the kernel to start writing the records to storage (at a pause:
  // a headset switched off in the background loses unwritten pages).
  void Sync();

  // One record as a log line, for Previous::lines and the tests.
  static std::string Describe(uint64_t wallMs, float songTime, uint16_t type, float a, float b, float c,
                              std::string_view text);

  static constexpr size_t kRecordSize = 128;
  static constexpr size_t kTextSize = 92;

 private:
  struct Header;
  struct Slot;
  Header* _header = nullptr;
  Slot* _slots = nullptr;
  size_t _mappedBytes = 0;
  uint32_t _capacity = 0;
};

}  // namespace Vivify
