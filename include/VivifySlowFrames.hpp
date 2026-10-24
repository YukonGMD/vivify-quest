#pragma once

#include "VivifyStackSampler.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Vivify {

// Slow-frame sampling (0.14.25-naomi.7). When a frame runs past ~100 ms the
// watchdog thread takes stack samples of the main and render threads
// (VivifyStackSampler.hpp) and counts them here, by function and by section of
// the song, for a summary at the song's end. Everything in this file runs on
// the watchdog thread, or on the main thread once the song is over.

// Where a thread's stack is. bionic names each thread's stack mapping
// "[anon:stack_and_tls:<tid>]"; where nothing has that name (glibc, or a
// kernel without mapping names), the stack is the mapping holding the stack
// pointer /proc/self/task/<tid>/syscall reports while the thread is blocked in
// a system call.
struct StackBounds {
  uint64_t lo = 0;
  uint64_t hi = 0;
};
std::optional<StackBounds> FindNamedThreadStack(std::vector<MapEntry> const& maps, int tid);
std::optional<StackBounds> FindStackHolding(std::vector<MapEntry> const& maps, uint64_t sp);
// The stack pointer in /proc/<pid>/task/<tid>/syscall text; nullopt when the
// thread is running (no stack pointer given) or the text is not understood.
std::optional<uint64_t> ParseSyscallStackPointer(std::string_view text);

// A code address, grouped by function without symbols: every library here
// keeps an .eh_frame_hdr in its loaded image -- a sorted table of function
// start addresses that exception unwinding uses -- so the function holding an
// address is a binary search away. Names are added later, off the headset,
// from the start address (tools/symbolize).
struct CodeLocation {
  int module = -1;       // index into ModuleTable::Names(); -1 when unmapped
  uint64_t address = 0;  // the function's start as llvm-symbolizer wants it, or
                         // the address itself when no function covers it
                         // (0 for memory that is not a file)
  bool function = false;
};

class ModuleTable {
 public:
  // From /proc/self/maps. A module keeps its index across rebuilds, so counts
  // taken before a rebuild still name the right library.
  void Build(std::vector<MapEntry> const& maps, MemoryReader read);
  CodeLocation Locate(uint64_t address) const;
  std::vector<std::string> const& Names() const { return _names; }
  bool Empty() const { return _images.empty(); }

 private:
  struct Image {
    int module = -1;
    uint64_t lo = 0, hi = 0;  // the absolute span it covers
    uint64_t bias = 0;        // absolute address = link-time address + bias
    bool file = false;        // false: anonymous or special memory (one bucket)
    bool elf = false;         // false: no usable ELF header (file offsets)
    uint64_t table = 0;       // .eh_frame_hdr's search table, absolute; 0 when none
    uint64_t hdr = 0;         // .eh_frame_hdr itself (its entries are relative to it)
    uint32_t entries = 0;
    std::vector<std::pair<uint64_t, uint64_t>> readable;  // sorted, absolute
  };
  struct Cursor;
  int ModuleIndex(std::string const& name);
  bool Read(Image const& image, uint64_t address, void* out, size_t length) const;
  bool FdeCovers(Image const& image, uint64_t fde, uint64_t address, uint64_t& start) const;
  bool CieFdeEncoding(Image const& image, uint64_t cie, uint8_t& encoding) const;

  MemoryReader _read;
  std::vector<Image> _images;  // sorted by lo, spans disjoint
  std::vector<std::string> _names;
  std::unordered_map<std::string, int> _moduleIndex;
  mutable std::vector<std::pair<uint64_t, uint8_t>> _cieEncodings;  // CIE address -> FDE pointer encoding
};

// Counts of slow-frame samples per section of the song, thread and function.
class SlowFrameProfile {
 public:
  enum Thread : int { kMain = 0, kRender = 1, kThreads = 2 };

  void Reset();
  void CountSlowFrame(int section);
  // stack[0] is where the thread was, then its callers. running: the thread
  // was on a CPU rather than waiting when it was sampled.
  void AddSample(int section, int thread, bool running, CodeLocation const* stack, int count);
  bool Empty() const { return _sections.empty(); }
  // One block per section with samples, without prefixes. label(section)
  // names a section; modules are ModuleTable::Names(); top: functions listed
  // per thread and kind.
  std::vector<std::string> Summary(std::function<std::string(int)> const& label,
                                   std::vector<std::string> const& modules, int top) const;

 private:
  struct Counts {
    int self = 0;          // samples with the thread inside this function itself
    int inclusive = 0;     // samples with it anywhere on the stack
    int64_t depthSum = 0;  // its depth in those samples (0: where the thread was)
    bool function = false;
  };
  struct PerThread {
    int samples = 0;
    int running = 0;
    std::unordered_map<uint64_t, Counts> functions;  // by (module + 1) << 48 | address
  };
  struct Section {
    int slowFrames = 0;
    PerThread threads[kThreads];
  };
  std::map<int, Section> _sections;
};

// "libunity.so+0x6cd7dc" for a function start; "libunity.so@0x6cd7e1" for an
// address no function covers; the mapping's name alone for memory that is not
// a file.
std::string FormatCodeLocation(CodeLocation const& location, std::vector<std::string> const& modules);

}  // namespace Vivify
