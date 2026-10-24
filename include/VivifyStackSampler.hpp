#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Vivify {

// Native call-stack samples of the main thread, for the stall watchdog
// (0.14.25-naomi.3), and from naomi.7 of the render thread too, for the
// slow-frame sampler.
//
// The watchdog thread sends the main thread a real-time signal. The handler,
// running on the main thread, walks the frame-pointer chain from the
// interrupted registers into a preallocated buffer -- no calls, no allocation,
// and no memory read outside the main thread's own stack -- and the watchdog
// thread turns the addresses into "module + offset" lines afterwards, from
// /proc/self/maps and the ELF headers already in memory (no loader lock, so a
// main thread stuck inside dlopen cannot block it).
//
// The handler is installed only on the first stall, only if the signal has no
// handler already, and is never removed: a sample request that arrives late
// must still land on it rather than on the signal's default action, which
// would end the process.

constexpr int kMaxStackFrames = 64;

enum class StackWalkStop : uint8_t {
  ChainEnd,       // a zero frame pointer or return address: the outermost frame
  OutOfStack,     // the frame pointer left the thread's stack
  NotIncreasing,  // frame pointers must climb the stack; a loop or garbage
  Misaligned,     // frame records are 8-byte aligned
  Full,           // kMaxStackFrames reached
  Unsupported,    // no register access on this architecture
};
char const* DescribeStop(StackWalkStop stop);

struct StackSample {
  uint64_t frames[kMaxStackFrames];
  int count = 0;
  // frames[1] is the link register, and no frame record confirmed it (so in a
  // function that has called something since, it can be stale).
  bool lrUnconfirmed = false;
  StackWalkStop stop = StackWalkStop::ChainEnd;
};

// Pointer-authentication and tag bits off a code address (arm64 signs saved
// return addresses on some builds; user addresses fit in 48 bits).
constexpr uint64_t StripCodeAddress(uint64_t address) {
  return address & 0x0000FFFFFFFFFFFFull;
}

// Async-signal-safe. pc and lr are the interrupted thread's registers (lr 0
// where there is none, as on x86-64), fp its frame pointer. Each frame record
// is {previous fp, return address}, and only records wholly inside
// [stackLo, stackHi) are read.
void WalkFramePointers(uint64_t pc, uint64_t lr, uint64_t fp, uintptr_t stackLo, uintptr_t stackHi,
                       StackSample& out);

struct MapEntry {
  uint64_t start = 0;
  uint64_t end = 0;
  uint64_t offset = 0;
  bool readable = false;
  bool executable = false;
  std::string path;
};
std::vector<MapEntry> ParseProcMaps(std::string_view text);

struct ResolvedFrame {
  std::string module;   // basename of the mapped file, or "?" when unmapped
  uint64_t offset = 0;  // address within the module as llvm-addr2line wants it
  bool known = false;   // false: offset is the absolute address
};
// Reads `length` bytes at `address`, or returns false. Lets the tests supply
// fake memory; the game reads its own mappings directly.
using MemoryReader = std::function<bool(uint64_t address, void* buffer, size_t length)>;
ResolvedFrame ResolveAddress(uint64_t address, std::vector<MapEntry> const& maps, MemoryReader const& read);
bool ReadOwnMemory(uint64_t address, void* buffer, size_t length);

// "#00 pc 00000000006cd7dc  libunity.so", with " (lr)" for an unconfirmed
// link-register frame.
std::string FormatFrame(int index, ResolvedFrame const& frame, bool fromUnconfirmedLr);

namespace StackSampler {

// Main thread, once (the first heartbeat): records which thread to sample and
// the bounds of its stack. Not called from the handler.
void RegisterTarget();
int TargetTid();

// The real-time signal used: chosen from the top of the range, which runtimes
// that claim signals for themselves (bionic, ART, the GC) leave alone.
int ChosenSignal();

enum class Result {
  Ok,
  NoTarget,         // RegisterTarget never ran
  HandlerTaken,     // something else already handles the signal
  InstallFailed,
  Blocked,          // the thread blocks the signal
  SendFailed,
  Timeout,          // not delivered in time (the request is withdrawn)
  Unsupported,      // no register access on this architecture
};
// Watchdog thread only. The first call installs the handler. Sample takes the
// main thread; SampleThread (naomi.7) any thread of this process, given the
// bounds of its stack (VivifySlowFrames.hpp finds them). Requests must not
// overlap: they share one buffer.
Result Sample(std::chrono::milliseconds timeout, StackSample& out);
Result SampleThread(int tid, uintptr_t stackLo, uintptr_t stackHi, std::chrono::milliseconds timeout,
                    StackSample& out);
std::string DescribeResult(Result result);

}  // namespace StackSampler

}  // namespace Vivify
