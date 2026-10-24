// Host test driver for src/VivifyWatchdog.cpp.
//
//   watchdog parse <stat-file>
//     prints state= utime= stime=, or "unparsed" (exit 1)
//   watchdog run <healthyMs> <stallMs> <afterMs> <busy|sleep|paused> <maxReports>
//     beats every 10 ms for healthyMs, stops beating for stallMs (spinning or
//     sleeping on this thread, or paused), beats again for afterMs, then stops
//     the watchdog and prints every line it wrote, one per line.
//   watchdog selftest
//     checks the stack walker, /proc/self/maps resolution and frame formatting
//     on synthetic input; prints PASS/FAIL lines
//   watchdog sample <spin|taken|blocked>
//     a second thread registers as the main thread and spins; this thread
//     samples it and prints the result and frames ("taken": the signal already
//     has a handler; "blocked": the spinning thread blocks it)
//   watchdog slowframes
//     slow-frame sampling (naomi.7): beats every 10 ms, then three 250 ms
//     frames spinning in SlowWorkA (section 0) and three sleeping in
//     SlowSleepB (section 1), while a thread named VivifyTestGfx waits in
//     RenderWait; prints "expect A=", "expect B=" and "expect R=" with those
//     functions as the summary names them, then the summary
// Thresholds are scaled down: stall after 200 ms, report every 200 ms, poll
// every 20 ms, CPU sample 100 ms, stack sample every 100 ms.
#include "VivifySlowFrames.hpp"
#include "VivifyStackSampler.hpp"
#include "VivifyWatchdog.hpp"

#include <elf.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <cstring>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;

namespace {

int gSelftestFailures = 0;
void Check(char const* name, bool ok, std::string const& detail = "") {
  std::printf("%s %s%s\n", ok ? "PASS" : "FAIL", name, ok ? "" : (" " + detail).c_str());
  if (!ok) gSelftestFailures++;
}

// A small library image for the function lookup: an ELF header, an
// .eh_frame_hdr at 0x1000 whose table lists two functions, a CIE and their
// FDEs at 0x1100 -- f1 covering [0x2000, 0x2100) and f2 [0x2200, 0x2300) --
// and code at 0x2000-0x3000.
std::vector<uint8_t> FakeLibrary() {
  std::vector<uint8_t> bytes(0x3000, 0);
  auto put = [&](size_t at, auto value) { std::memcpy(bytes.data() + at, &value, sizeof(value)); };
  Elf64_Ehdr header{};
  std::memcpy(header.e_ident, ELFMAG, SELFMAG);
  header.e_ident[EI_CLASS] = ELFCLASS64;
  header.e_phoff = sizeof(Elf64_Ehdr);
  header.e_phentsize = sizeof(Elf64_Phdr);
  header.e_phnum = 2;
  put(0, header);
  Elf64_Phdr load{};
  load.p_type = PT_LOAD;
  load.p_memsz = 0x3000;
  put(sizeof(Elf64_Ehdr), load);
  Elf64_Phdr ehFrame{};
  ehFrame.p_type = PT_GNU_EH_FRAME;
  ehFrame.p_vaddr = 0x1000;
  ehFrame.p_memsz = 0x40;
  put(sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr), ehFrame);

  // CIE at 0x1100: version 1, "zR", code align 1, data align -8, return
  // register 30, one byte of augmentation data: FDE pointers pcrel|sdata4.
  size_t const cie = 0x1100;
  uint8_t const cieBody[] = {0, 0, 0, 0, 1, 'z', 'R', 0, 1, 0x78, 30, 1, 0x1B, 0, 0, 0};
  put(cie, static_cast<uint32_t>(sizeof(cieBody)));
  std::memcpy(bytes.data() + cie + 4, cieBody, sizeof(cieBody));
  size_t const fde1 = cie + 4 + sizeof(cieBody);
  auto fde = [&](size_t at, uint32_t begin, uint32_t range) {
    put(at, static_cast<uint32_t>(16));  // CIE pointer, pc_begin, pc_range, aug length + padding
    put(at + 4, static_cast<uint32_t>(at + 4 - cie));
    put(at + 8, static_cast<int32_t>(begin - (at + 8)));
    put(at + 12, range);
    return at + 20;
  };
  size_t const fde2 = fde(fde1, 0x2000, 0x100);
  fde(fde2, 0x2200, 0x100);

  // .eh_frame_hdr at 0x1000.
  size_t const hdr = 0x1000;
  bytes[hdr] = 1;
  bytes[hdr + 1] = 0x1B;  // .eh_frame pointer: pcrel|sdata4
  bytes[hdr + 2] = 0x03;  // count: udata4
  bytes[hdr + 3] = 0x3B;  // table: datarel|sdata4
  put(hdr + 4, static_cast<int32_t>(cie - (hdr + 4)));
  put(hdr + 8, static_cast<uint32_t>(2));
  put(hdr + 12, static_cast<int32_t>(0x2000 - hdr));
  put(hdr + 16, static_cast<int32_t>(fde1 - hdr));
  put(hdr + 20, static_cast<int32_t>(0x2200 - hdr));
  put(hdr + 24, static_cast<int32_t>(fde2 - hdr));
  return bytes;
}

void SlowFrameSelftest() {
  using namespace Vivify;
  Check("syscall: stack pointer while in a system call",
        ParseSyscallStackPointer("202 0x1 0x80 0x0 0x0 0x0 0x0 0x7ffc8a1b2c30 0x7f1234567890\n") ==
            std::optional<uint64_t>(0x7ffc8a1b2c30ull));
  Check("syscall: stack pointer while blocked outside one",
        ParseSyscallStackPointer("-1 0x7ffc00001000 0x7f0000002000") == std::optional<uint64_t>(0x7ffc00001000ull));
  Check("syscall: a running thread gives none", !ParseSyscallStackPointer("running\n"));
  Check("syscall: garbage gives none", !ParseSyscallStackPointer("202 0x1 0x2") && !ParseSyscallStackPointer(""));

  auto stacks = ParseProcMaps(
      "7e00000000-7e00080000 rw-p 00000000 00:00 0   [anon:stack_and_tls:1234]\n"
      "7e00080000-7e00090000 rw-p 00000000 00:00 0   [anon:stack_and_tls:1234]\n"
      "7e10000000-7e10010000 rw-p 00000000 00:00 0 \n"
      "7e20000000-7e20010000 r--p 00000000 fd:01 11 /data/app/x/lib/arm64/libunity.so\n");
  auto named = FindNamedThreadStack(stacks, 1234);
  Check("stack: found by bionic's name, both halves", named && named->lo == 0x7e00000000ull &&
        named->hi == 0x7e00090000ull);
  Check("stack: another thread's name is not it", !FindNamedThreadStack(stacks, 123));
  auto holding = FindStackHolding(stacks, 0x7e10008000ull);
  Check("stack: the anonymous mapping holding the stack pointer", holding && holding->lo == 0x7e10000000ull &&
        holding->hi == 0x7e10010000ull);
  Check("stack: a file mapping is not a stack", !FindStackHolding(stacks, 0x7e20000100ull) &&
        !FindStackHolding(stacks, 0x10));

  static std::vector<uint8_t> const library = FakeLibrary();
  auto memory = [](uint64_t address, void* buffer, size_t length) {
    for (uint64_t base : {0x7200000000ull, 0x7300000000ull}) {
      if (address >= base && address + length <= base + library.size()) {
        std::memcpy(buffer, library.data() + (address - base), length);
        return true;
      }
    }
    return false;
  };
  auto maps = ParseProcMaps(
      "7200000000-7200002000 r--p 00000000 fd:01 21 /data/app/x/lib/arm64/libfake.so\n"
      "7200002000-7200003000 r-xp 00002000 fd:01 21 /data/app/x/lib/arm64/libfake.so\n"
      "7300000000-7300002000 r--p 00200000 fd:01 22 /data/app/x/base.apk\n"
      "7300002000-7300003000 r-xp 00202000 fd:01 22 /data/app/x/base.apk\n"
      "7400000000-7400001000 r-xp 00005000 fd:01 23 /data/app/x/oat/arm64/base.odex\n"
      "7500000000-7500001000 r-xp 00000000 00:00 0 \n");
  ModuleTable modules;
  modules.Build(maps, memory);
  auto describe = [&](uint64_t address) { return FormatCodeLocation(modules.Locate(address), modules.Names()); };
  Check("function: an address inside the first function names its start", describe(0x7200002050ull) ==
        "libfake.so+0x2000", describe(0x7200002050ull));
  Check("function: the first byte of the second", describe(0x7200002200ull) == "libfake.so+0x2200",
        describe(0x7200002200ull));
  Check("function: a gap between functions keeps the address", describe(0x7200002150ull) == "libfake.so@0x2150",
        describe(0x7200002150ull));
  Check("function: past the last function's end keeps the address", describe(0x7200002ff0ull) ==
        "libfake.so@0x2ff0", describe(0x7200002ff0ull));
  Check("function: below the first function keeps the address", describe(0x7200001800ull) == "libfake.so@0x1800",
        describe(0x7200001800ull));
  Check("function: a library inside an APK is named by its offset", describe(0x7300002010ull) ==
        "base.apk!0x200000+0x2000", describe(0x7300002010ull));
  Check("function: a file without an ELF header gives file offsets", describe(0x7400000010ull) ==
        "base.odex@0x5010", describe(0x7400000010ull));
  Check("function: anonymous code is one bucket", describe(0x7500000010ull) == "[anonymous]" &&
        modules.Locate(0x7500000010ull).address == 0 && modules.Locate(0x7500000ff0ull).address == 0);
  Check("function: unmapped", describe(0x10) == "[unmapped]" && describe(0x7600000000ull) == "[unmapped]");
  auto const names = modules.Names();
  modules.Build(maps, memory);
  Check("function: modules keep their index across a rebuild", modules.Names() == names &&
        describe(0x7200002050ull) == "libfake.so+0x2000");

  // A table encoding that cannot be searched: addresses only.
  std::vector<uint8_t> odd = library;
  odd[0x1003] = 0x1B;
  auto oddMemory = [&](uint64_t address, void* buffer, size_t length) {
    if (address < 0x7200000000ull || address + length > 0x7200000000ull + odd.size()) return false;
    std::memcpy(buffer, odd.data() + (address - 0x7200000000ull), length);
    return true;
  };
  ModuleTable oddModules;
  oddModules.Build(maps, oddMemory);
  Check("function: an unsearchable table falls back to addresses",
        FormatCodeLocation(oddModules.Locate(0x7200002050ull), oddModules.Names()) == "libfake.so@0x2050");

  // Counting and the summary.
  SlowFrameProfile profile;
  CodeLocation const f1{0, 0x2000, true}, f2{0, 0x2200, true}, entry{0, 0x100, true}, anonymous{1, 0, false};
  std::vector<std::string> const moduleNames{"libfake.so", "[anonymous]"};
  profile.CountSlowFrame(0);
  profile.CountSlowFrame(0);
  for (int i = 0; i < 3; i++) {
    CodeLocation stack[] = {f1, f2, f1, entry};  // f1 twice: recursion counts once
    profile.AddSample(0, SlowFrameProfile::kMain, true, stack, 4);
  }
  CodeLocation waiting[] = {anonymous, f2, entry};
  profile.AddSample(0, SlowFrameProfile::kMain, false, waiting, 3);
  profile.AddSample(0, SlowFrameProfile::kRender, false, waiting, 3);
  profile.CountSlowFrame(2);
  auto lines = profile.Summary([](int section) { return "section " + std::to_string(section); }, moduleNames, 2);
  std::string all;
  for (auto const& line : lines) all += line + "\n";
  Check("summary: one block per section with samples, in order", lines.size() == 6 &&
        lines[0].rfind("section 0: 2 slow frame(s) sampled; main thread 4 time(s), on a CPU in 75%, render thread 1 "
                       "time(s), on a CPU in 0%", 0) == 0 &&
        lines[5] == "section 2: 1 slow frame(s) sampled", all);
  Check("summary: where the main thread was", lines.size() > 1 &&
        lines[1] == "  main thread was in: libfake.so+0x2000 75%, [anonymous] 25%", all);
  Check("summary: anywhere on the stack, recursion once, top 2", lines.size() > 2 &&
        lines[2] == "  main thread, anywhere on the stack: libfake.so+0x2200 100%, libfake.so+0x100 100%", all);
  Check("summary: the render thread", lines.size() > 4 &&
        lines[3] == "  render thread was in: [anonymous] 100%" &&
        lines[4] == "  render thread, anywhere on the stack: [anonymous] 100%, libfake.so+0x2200 100%", all);
  SlowFrameProfile many;
  for (int i = 0; i < 10; i++) {
    CodeLocation stack[] = {i < 6 ? f1 : f2, entry};
    many.AddSample(1, SlowFrameProfile::kMain, true, stack, 2);
  }
  auto manyLines = many.Summary([](int) { return std::string("s"); }, moduleNames, 5);
  Check("summary: percentages of the thread's samples", manyLines.size() == 3 &&
        manyLines[1] == "  main thread was in: libfake.so+0x2000 60%, libfake.so+0x2200 40%" &&
        manyLines[2] == "  main thread, anywhere on the stack: libfake.so+0x100 100%, libfake.so+0x2000 60%, "
                        "libfake.so+0x2200 40%", manyLines.size() > 2 ? manyLines[2] : "");
  SlowFrameProfile same;
  for (int i = 0; i < 10; i++) {
    CodeLocation stack[] = {anonymous, f2, f1, entry};
    same.AddSample(0, SlowFrameProfile::kRender, false, stack, 4);
  }
  auto sameLines = same.Summary([](int) { return std::string("s"); }, moduleNames, 3);
  Check("summary: equal counts list the frames nearest where the thread was first", sameLines.size() == 3 &&
        sameLines[2] == "  render thread, anywhere on the stack: [anonymous] 100%, libfake.so+0x2200 100%, "
                        "libfake.so+0x2000 100%", sameLines.size() > 2 ? sameLines[2] : "");
}

int Selftest() {
  using namespace Vivify;
  // A fake stack of three frame records, growing upwards: {next fp, return}.
  alignas(16) uint64_t stack[32] = {};
  auto at = [&](int word) { return reinterpret_cast<uint64_t>(&stack[word]); };
  uintptr_t const lo = at(0), hi = at(32);
  stack[4] = at(10);   stack[5] = 0x1111;
  stack[10] = at(20);  stack[11] = 0x2222;
  stack[20] = 0;       stack[21] = 0x3333;
  StackSample sample;
  WalkFramePointers(0xAAAA, 0, at(4), lo, hi, sample);
  Check("walk: pc then every return address", sample.count == 4 && sample.frames[0] == 0xAAAA &&
        sample.frames[1] == 0x1111 && sample.frames[2] == 0x2222 && sample.frames[3] == 0x3333,
        std::to_string(sample.count));
  Check("walk: ends at a zero frame pointer", sample.stop == StackWalkStop::ChainEnd);

  WalkFramePointers(0xAAAA, 0xBBBB, at(4), lo, hi, sample);
  Check("walk: an unconfirmed lr is kept and flagged", sample.count == 5 && sample.frames[1] == 0xBBBB &&
        sample.lrUnconfirmed);
  WalkFramePointers(0xAAAA, 0x1111, at(4), lo, hi, sample);
  Check("walk: an lr the first record confirms is not repeated", sample.count == 4 && sample.frames[1] == 0x1111 &&
        !sample.lrUnconfirmed);

  stack[10] = at(4);  // loops back down
  WalkFramePointers(0xAAAA, 0, at(4), lo, hi, sample);
  Check("walk: stops when frame pointers stop climbing", sample.stop == StackWalkStop::NotIncreasing &&
        sample.count == 3);
  stack[10] = at(20);

  WalkFramePointers(0xAAAA, 0, at(4) + 4, lo, hi, sample);
  Check("walk: refuses a misaligned frame pointer", sample.stop == StackWalkStop::Misaligned && sample.count == 1);
  WalkFramePointers(0xAAAA, 0, at(4), lo, at(11), sample);
  Check("walk: never reads a record past the stack's end", sample.stop == StackWalkStop::OutOfStack &&
        sample.count == 2);
  WalkFramePointers(0xAAAA, 0, 0x10, lo, hi, sample);
  Check("walk: refuses a frame pointer below the stack", sample.stop == StackWalkStop::OutOfStack);

  stack[5] = 0x007F000000001111ull;  // pointer-authentication bits above bit 48
  WalkFramePointers(0x004000000000AAAAull, 0, at(4), lo, hi, sample);
  Check("walk: strips pointer-authentication bits", sample.frames[0] == 0xAAAA && sample.frames[1] == 0x1111);
  stack[5] = 0x1111;

  // A long chain fills the buffer and stops.
  std::vector<uint64_t> deep(2 * (kMaxStackFrames + 10) + 2, 0);
  for (size_t i = 0; i + 2 < deep.size(); i += 2) {
    deep[i] = reinterpret_cast<uint64_t>(&deep[i + 2]);
    deep[i + 1] = 0x100 + i;
  }
  WalkFramePointers(0xAAAA, 0, reinterpret_cast<uint64_t>(&deep[0]), reinterpret_cast<uintptr_t>(&deep[0]),
                    reinterpret_cast<uintptr_t>(&deep[0] + deep.size()), sample);
  Check("walk: stops at the frame limit", sample.stop == StackWalkStop::Full && sample.count == kMaxStackFrames);

  // /proc/self/maps: a library whose header mapping is at offset 0, and one
  // stored inside an APK at offset 0x200000.
  std::string const maps =
      "7000000000-7000001000 r--p 00000000 fd:01 11 /data/app/x/lib/arm64/libunity.so\n"
      "7000001000-7000005000 r-xp 00000000 fd:01 11 /data/app/x/lib/arm64/libunity.so\n"
      "7000010000-7000011000 rw-p 00000000 00:00 0 \n"
      "7100000000-7100001000 r--p 00200000 fd:01 12   /data/app/x/base.apk\n"
      "7100001000-7100003000 r-xp 00201000 fd:01 12   /data/app/x/base.apk\n";
  auto entries = ParseProcMaps(maps);
  Check("maps: every line parsed", entries.size() == 5, std::to_string(entries.size()));
  Check("maps: path and offset", entries.size() == 5 && entries[4].path == "/data/app/x/base.apk" &&
        entries[4].offset == 0x201000 && entries[2].path.empty() && entries[1].readable);
  // Fake memory: an ELF header with one PT_LOAD at vaddr 0 at each image start.
  auto fakeMemory = [](uint64_t address, void* buffer, size_t length) {
    static std::vector<uint8_t> image = [] {
      std::vector<uint8_t> bytes(sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr), 0);
      Elf64_Ehdr header{};
      std::memcpy(header.e_ident, ELFMAG, SELFMAG);
      header.e_ident[EI_CLASS] = ELFCLASS64;
      header.e_phoff = sizeof(Elf64_Ehdr);
      header.e_phentsize = sizeof(Elf64_Phdr);
      header.e_phnum = 1;
      Elf64_Phdr load{};
      load.p_type = PT_LOAD;
      std::memcpy(bytes.data(), &header, sizeof(header));
      std::memcpy(bytes.data() + sizeof(header), &load, sizeof(load));
      return bytes;
    }();
    for (uint64_t base : {0x7000000000ull, 0x7100000000ull}) {
      if (address >= base && address + length <= base + image.size()) {
        std::memcpy(buffer, image.data() + (address - base), length);
        return true;
      }
    }
    return false;
  };
  auto unity = ResolveAddress(0x70000017dcull, entries, fakeMemory);
  Check("resolve: library offset from its load base", unity.known && unity.module == "libunity.so" &&
        unity.offset == 0x17dc, unity.module + " " + std::to_string(unity.offset));
  auto apk = ResolveAddress(0x7100001234ull, entries, fakeMemory);
  Check("resolve: library inside an APK", apk.known && apk.module == "base.apk" && apk.offset == 0x1234,
        apk.module + " " + std::to_string(apk.offset));
  auto anon = ResolveAddress(0x7000010010ull, entries, fakeMemory);
  Check("resolve: anonymous memory keeps the absolute address", !anon.known && anon.module == "[anonymous]" &&
        anon.offset == 0x7000010010ull);
  auto none = ResolveAddress(0x10, entries, fakeMemory);
  Check("resolve: unmapped address", !none.known && none.module == "?" && none.offset == 0x10);
  auto noHeader = ResolveAddress(0x70000017dcull, entries, [](uint64_t, void*, size_t) { return false; });
  Check("resolve: no readable header falls back to the file offset", noHeader.known && noHeader.offset == 0x7dc,
        std::to_string(noHeader.offset));

  Check("format: frame line", FormatFrame(0, unity, false) == "#00 pc 00000000000017dc  libunity.so",
        FormatFrame(0, unity, false));
  Check("format: lr frame", FormatFrame(1, unity, true) == "#01 pc 00000000000017dc  libunity.so (lr)",
        FormatFrame(1, unity, true));
  Check("format: anonymous frame", FormatFrame(2, anon, false) == "#02 pc 0000007000010010  [anonymous] (absolute)",
        FormatFrame(2, anon, false));

  SlowFrameSelftest();
  return gSelftestFailures == 0 ? 0 : 1;
}

std::atomic<bool> gSpinning{false};
std::atomic<bool> gStopSpinning{false};

__attribute__((noinline)) void SpinHere() {
  volatile uint64_t spin = 0;
  gSpinning = true;
  while (!gStopSpinning.load(std::memory_order_relaxed)) spin = spin + 1;
}

void IgnoreSignal(int) {}

int SampleCommand(std::string const& mode) {
  using namespace Vivify;
  if (mode == "taken") {
    struct sigaction other {};
    other.sa_handler = &IgnoreSignal;
    sigaction(StackSampler::ChosenSignal(), &other, nullptr);
  }
  std::thread spinner([&] {
    if (mode == "blocked") {
      sigset_t set;
      sigemptyset(&set);
      sigaddset(&set, StackSampler::ChosenSignal());
      pthread_sigmask(SIG_BLOCK, &set, nullptr);
    }
    StackSampler::RegisterTarget();
    SpinHere();
  });
  while (!gSpinning) std::this_thread::sleep_for(milliseconds(1));
  StackSample sample;
  auto const result = StackSampler::Sample(milliseconds(500), sample);
  // A second request must work too (the handler stays installed).
  StackSample second;
  auto const again = result == StackSampler::Result::Ok ? StackSampler::Sample(milliseconds(500), second) : result;
  gStopSpinning = true;
  spinner.join();
  std::printf("result=%s\n", StackSampler::DescribeResult(result).c_str());
  std::printf("again=%s\n", StackSampler::DescribeResult(again).c_str());
  if (result != StackSampler::Result::Ok) return 0;
  auto const maps = ParseProcMaps([] {
    std::ifstream in("/proc/self/maps");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }());
  auto const spin = ResolveAddress(reinterpret_cast<uint64_t>(&SpinHere), maps, &ReadOwnMemory);
  std::printf("spin=%s %llx\n", spin.module.c_str(), static_cast<unsigned long long>(spin.offset));
  std::printf("stop=%s\n", DescribeStop(sample.stop));
  for (int i = 0; i < sample.count; i++) {
    std::printf("%s\n", FormatFrame(i, ResolveAddress(sample.frames[i], maps, &ReadOwnMemory),
                                    i == 1 && sample.lrUnconfirmed).c_str());
  }
  return 0;
}

__attribute__((noinline)) void SlowWorkA(int ms) {
  volatile uint64_t spin = 0;
  auto const end = steady_clock::now() + milliseconds(ms);
  while (steady_clock::now() < end) spin = spin + 1;
}

__attribute__((noinline)) void SlowSleepB(int ms) {
  std::this_thread::sleep_for(milliseconds(ms));
  asm volatile("" ::: "memory");  // keeps the call from being a tail call
}

std::mutex gRenderMutex;
std::condition_variable gRenderWake;
bool gRenderStop = false;

__attribute__((noinline)) void RenderWait() {
  std::unique_lock lock(gRenderMutex);
  while (!gRenderStop) gRenderWake.wait_for(lock, milliseconds(500));
  asm volatile("" ::: "memory");
}

// The summary's name for a function in this executable: a PIE's first load
// address is 0, so its start is its distance from where the file is mapped.
std::string ExpectedName(void (*function)()) {
  char exe[4096] = {};
  ssize_t const length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  std::ifstream in("/proc/self/maps");
  auto const maps = Vivify::ParseProcMaps(std::string((std::istreambuf_iterator<char>(in)),
                                                      std::istreambuf_iterator<char>()));
  for (auto const& entry : maps) {
    if (length > 0 && entry.path == std::string(exe, static_cast<size_t>(length)) && entry.offset == 0) {
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "+0x%llx",
                    static_cast<unsigned long long>(reinterpret_cast<uint64_t>(function) - entry.start));
      std::string name(exe, static_cast<size_t>(length));
      return name.substr(name.rfind('/') + 1) + buffer;
    }
  }
  return "?";
}

int SlowFramesCommand() {
  std::mutex linesMutex;
  std::vector<std::string> lines;
  Vivify::Watchdog::Options options;
  options.poll = milliseconds(200);
  options.renderThreadName = "VivifyTestGfx";
  options.slowFrameAfter = milliseconds(100);
  options.slowSampleEvery = milliseconds(20);
  options.slowSampleTimeout = milliseconds(200);
  options.slowSamplesPerFrame = 4;
  Vivify::Watchdog watchdog;
  watchdog.Start(
      [&](std::string const& line) {
        std::lock_guard lock(linesMutex);
        lines.push_back(line);
      },
      options);
  std::thread render([] {
    pthread_setname_np(pthread_self(), "VivifyTestGfx");
    RenderWait();
  });
  auto beatFor = [&](int ms) {
    auto const end = steady_clock::now() + milliseconds(ms);
    while (steady_clock::now() < end) {
      watchdog.Beat();
      std::this_thread::sleep_for(milliseconds(10));
    }
  };
  beatFor(300);
  watchdog.ResetSlowFrames();
  watchdog.SetSlowFrameSection(0);
  beatFor(300);  // healthy frames are not sampled
  for (int i = 0; i < 3; i++) {
    watchdog.Beat();
    SlowWorkA(250);
  }
  watchdog.SetSlowFrameSection(1);
  for (int i = 0; i < 3; i++) {
    watchdog.Beat();
    SlowSleepB(250);
  }
  watchdog.SetSlowFrameSection(2);
  beatFor(300);
  watchdog.SetSlowFrameSection(-1);
  auto const summary = watchdog.TakeSlowFrameSummary([](int section) { return "section " + std::to_string(section); });
  auto const after = watchdog.TakeSlowFrameSummary([](int) { return std::string("again"); });
  {
    std::lock_guard lock(gRenderMutex);
    gRenderStop = true;
  }
  gRenderWake.notify_all();
  render.join();
  watchdog.Stop();
  std::printf("expect A=%s\n", ExpectedName(reinterpret_cast<void (*)()>(&SlowWorkA)).c_str());
  std::printf("expect B=%s\n", ExpectedName(reinterpret_cast<void (*)()>(&SlowSleepB)).c_str());
  std::printf("expect R=%s\n", ExpectedName(&RenderWait).c_str());
  for (auto const& line : summary) std::printf("%s\n", line.c_str());
  std::printf("---- taken twice\n");
  for (auto const& line : after) std::printf("%s\n", line.c_str());
  std::printf("---- written\n");
  for (auto const& line : lines) std::printf("%s\n", line.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "parse") {
    std::ifstream in(argv[2], std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto stat = Vivify::ParseTaskStat(text);
    if (!stat) {
      std::printf("unparsed\n");
      return 1;
    }
    std::printf("state=%c utime=%llu stime=%llu\n", stat->state, static_cast<unsigned long long>(stat->utimeTicks),
                static_cast<unsigned long long>(stat->stimeTicks));
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "selftest") return Selftest();
  if (argc >= 3 && std::string(argv[1]) == "sample") return SampleCommand(argv[2]);
  if (argc >= 2 && std::string(argv[1]) == "slowframes") return SlowFramesCommand();
  if (argc >= 7 && std::string(argv[1]) == "run") {
    int const healthyMs = std::atoi(argv[2]);
    int const stallMs = std::atoi(argv[3]);
    int const afterMs = std::atoi(argv[4]);
    std::string const mode = argv[5];
    std::mutex linesMutex;
    std::vector<std::string> lines;
    Vivify::Watchdog::Options options;
    options.stallAfter = milliseconds(200);
    options.reportEvery = milliseconds(200);
    options.poll = milliseconds(20);
    options.cpuSample = milliseconds(100);
    options.maxReports = std::atoi(argv[6]);
    options.renderThreadName = "no-such-thread";
    options.sampleEvery = milliseconds(100);
    options.sampleTimeout = milliseconds(200);
    Vivify::Watchdog watchdog;
    watchdog.Start(
        [&](std::string const& line) {
          std::lock_guard lock(linesMutex);
          lines.push_back(line);
        },
        options);
    auto beatFor = [&](int ms) {
      auto const end = steady_clock::now() + milliseconds(ms);
      while (steady_clock::now() < end) {
        watchdog.Beat();
        std::this_thread::sleep_for(milliseconds(10));
      }
    };
    beatFor(healthyMs);
    watchdog.SetBreadcrumb("phase two");
    if (mode == "paused") watchdog.SetPaused(true);
    auto const stallEnd = steady_clock::now() + milliseconds(stallMs);
    if (mode == "busy") {
      volatile uint64_t spin = 0;
      while (steady_clock::now() < stallEnd) spin = spin + 1;
    } else {
      std::this_thread::sleep_until(stallEnd);
    }
    if (mode == "paused") watchdog.SetPaused(false);
    beatFor(afterMs);
    watchdog.Stop();
    for (auto const& line : lines) std::printf("%s\n", line.c_str());
    return 0;
  }
  std::fprintf(stderr,
               "usage: watchdog parse <file> | watchdog run <healthyMs> <stallMs> <afterMs> <mode> <max> | "
               "watchdog selftest | watchdog sample <spin|taken|blocked> | watchdog slowframes\n");
  return 2;
}
