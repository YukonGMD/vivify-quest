#include "VivifyStackSampler.hpp"

#include <elf.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>

namespace Vivify {

#define VIVIFY_NO_ASAN __attribute__((no_sanitize("address")))

char const* DescribeStop(StackWalkStop stop) {
  switch (stop) {
    case StackWalkStop::ChainEnd: return "end of the frame chain";
    case StackWalkStop::OutOfStack: return "frame pointer left the stack";
    case StackWalkStop::NotIncreasing: return "frame pointers stopped climbing";
    case StackWalkStop::Misaligned: return "misaligned frame pointer";
    case StackWalkStop::Full: return "frame limit reached";
    case StackWalkStop::Unsupported: return "no register access on this architecture";
  }
  return "?";
}

// Runs inside the signal handler: no calls, no allocation, and the only memory
// read is the frame records it has just checked lie inside the stack.
VIVIFY_NO_ASAN void WalkFramePointers(uint64_t pc, uint64_t lr, uint64_t fp, uintptr_t stackLo,
                                      uintptr_t stackHi, StackSample& out) {
  out.count = 0;
  out.lrUnconfirmed = false;
  out.stop = StackWalkStop::ChainEnd;
  out.frames[out.count++] = StripCodeAddress(pc);
  if (lr != 0) {
    out.frames[out.count++] = StripCodeAddress(lr);
    out.lrUnconfirmed = true;
  }
  uint64_t previous = 0;
  while (true) {
    if (fp == 0) {
      out.stop = StackWalkStop::ChainEnd;
      return;
    }
    if (fp % 8 != 0) {
      out.stop = StackWalkStop::Misaligned;
      return;
    }
    if (fp < stackLo || fp > stackHi || stackHi - fp < 16) {
      out.stop = StackWalkStop::OutOfStack;
      return;
    }
    if (fp <= previous) {
      out.stop = StackWalkStop::NotIncreasing;
      return;
    }
    auto const* record = reinterpret_cast<uint64_t const volatile*>(static_cast<uintptr_t>(fp));
    uint64_t const next = record[0];
    uint64_t const returnAddress = StripCodeAddress(record[1]);
    if (returnAddress == 0) {
      out.stop = StackWalkStop::ChainEnd;
      return;
    }
    // In a function that saved its link register, the first record holds the
    // same address: that confirms frames[1] rather than repeating it.
    if (out.lrUnconfirmed && out.count == 2 && out.frames[1] == returnAddress) {
      out.lrUnconfirmed = false;
    } else {
      if (out.count >= kMaxStackFrames) {
        out.stop = StackWalkStop::Full;
        return;
      }
      out.frames[out.count++] = returnAddress;
    }
    previous = fp;
    fp = next;
  }
}

std::vector<MapEntry> ParseProcMaps(std::string_view text) {
  std::vector<MapEntry> maps;
  std::istringstream in{std::string(text)};
  std::string line;
  while (std::getline(in, line)) {
    // start-end perms offset dev inode [path]
    unsigned long long start = 0, end = 0, offset = 0;
    char perms[8] = {};
    int pathAt = 0;
    if (std::sscanf(line.c_str(), "%llx-%llx %7s %llx %*s %*s %n", &start, &end, perms, &offset, &pathAt) < 4) {
      continue;
    }
    MapEntry entry;
    entry.start = start;
    entry.end = end;
    entry.offset = offset;
    entry.readable = perms[0] == 'r';
    if (pathAt > 0 && static_cast<size_t>(pathAt) < line.size()) entry.path = line.substr(pathAt);
    while (!entry.path.empty() && entry.path.back() == ' ') entry.path.pop_back();
    maps.push_back(std::move(entry));
  }
  return maps;
}

namespace {

std::string Basename(std::string const& path) {
  size_t const slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The lowest PT_LOAD address of the ELF image whose header is at `base`,
// page-aligned as the loader maps it, or nullopt when `base` holds no ELF.
// Reads nothing at or past `limit`, the end of base's mapping.
std::optional<uint64_t> MinLoadAddress(uint64_t base, uint64_t limit, MemoryReader const& read) {
  if (limit - base < sizeof(Elf64_Ehdr)) return std::nullopt;
  Elf64_Ehdr header;
  if (!read(base, &header, sizeof(header))) return std::nullopt;
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64) {
    return std::nullopt;
  }
  if (header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0 || header.e_phnum > 64) return std::nullopt;
  if (header.e_phoff > limit - base || (limit - base - header.e_phoff) / sizeof(Elf64_Phdr) < header.e_phnum) {
    return std::nullopt;
  }
  uint64_t minVaddr = UINT64_MAX;
  for (int i = 0; i < header.e_phnum; i++) {
    Elf64_Phdr phdr;
    if (!read(base + header.e_phoff + static_cast<uint64_t>(i) * sizeof(Elf64_Phdr), &phdr, sizeof(phdr))) {
      return std::nullopt;
    }
    if (phdr.p_type == PT_LOAD && phdr.p_vaddr < minVaddr) minVaddr = phdr.p_vaddr;
  }
  if (minVaddr == UINT64_MAX) return std::nullopt;
  return minVaddr & ~static_cast<uint64_t>(0xFFF);
}

}  // namespace

ResolvedFrame ResolveAddress(uint64_t address, std::vector<MapEntry> const& maps, MemoryReader const& read) {
  ResolvedFrame frame;
  frame.module = "?";
  frame.offset = address;
  size_t found = maps.size();
  for (size_t i = 0; i < maps.size(); i++) {
    if (address >= maps[i].start && address < maps[i].end) {
      found = i;
      break;
    }
  }
  if (found == maps.size()) return frame;
  MapEntry const& hit = maps[found];
  if (hit.path.empty()) {
    frame.module = "[anonymous]";
    return frame;
  }
  frame.module = Basename(hit.path);
  // The image's ELF header is in the nearest mapping of the same file at or
  // below this one that starts with one (for a library stored inside an APK,
  // that is not file offset 0). Addresses relative to it, plus the image's
  // first load address, are what the unstripped library's symbols use.
  for (size_t i = found + 1; i-- > 0;) {
    MapEntry const& candidate = maps[i];
    if (candidate.path != hit.path || candidate.offset > hit.offset || !candidate.readable) continue;
    if (auto minLoad = MinLoadAddress(candidate.start, candidate.end, read)) {
      frame.offset = address - candidate.start + *minLoad;
      frame.known = true;
      return frame;
    }
  }
  // No header found: the file offset, which matches for most segments.
  frame.offset = address - hit.start + hit.offset;
  frame.known = true;
  return frame;
}

bool ReadOwnMemory(uint64_t address, void* buffer, size_t length) {
  // Callers only pass addresses inside readable mappings from /proc/self/maps.
  std::memcpy(buffer, reinterpret_cast<void const*>(static_cast<uintptr_t>(address)), length);
  return true;
}

std::string FormatFrame(int index, ResolvedFrame const& frame, bool fromUnconfirmedLr) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "#%02d pc %016" PRIx64 "  ", index, frame.offset);
  std::string text = buffer + frame.module;
  if (!frame.known && frame.module != "?") text += " (absolute)";
  if (fromUnconfirmedLr) text += " (lr)";
  return text;
}

namespace StackSampler {

namespace {

// Request states. The handler only acts on Armed; the watchdog thread only
// reads the buffer once the handler has set Done.
enum : int { kIdle = 0, kArmed = 1, kWriting = 2, kDone = 3 };

std::atomic<int> gTargetTid{0};
std::atomic<uintptr_t> gStackLo{0};
std::atomic<uintptr_t> gStackHi{0};
std::atomic<int> gState{kIdle};
std::atomic<bool> gInstalled{false};
StackSample gBuffer;

static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<uintptr_t>::is_always_lock_free);

bool RegistersFrom(void* context, uint64_t& pc, uint64_t& lr, uint64_t& fp) {
  auto* uc = static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
  pc = uc->uc_mcontext.pc;
  fp = uc->uc_mcontext.regs[29];
  lr = uc->uc_mcontext.regs[30];
  return true;
#elif defined(__x86_64__)
  pc = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
  fp = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RBP]);
  lr = 0;
  return true;
#else
  (void)uc;
  pc = lr = fp = 0;
  return false;
#endif
}

VIVIFY_NO_ASAN void Handler(int, siginfo_t*, void* context) {
  int const savedErrno = errno;
  if (static_cast<int>(syscall(SYS_gettid)) == gTargetTid.load(std::memory_order_relaxed)) {
    int expected = kArmed;
    if (gState.compare_exchange_strong(expected, kWriting, std::memory_order_acq_rel)) {
      uint64_t pc = 0, lr = 0, fp = 0;
      if (RegistersFrom(context, pc, lr, fp)) {
        WalkFramePointers(pc, lr, fp, gStackLo.load(std::memory_order_relaxed),
                          gStackHi.load(std::memory_order_relaxed), gBuffer);
      } else {
        gBuffer.count = 0;
        gBuffer.stop = StackWalkStop::Unsupported;
      }
      gState.store(kDone, std::memory_order_release);
    }
  }
  errno = savedErrno;
}

bool HandlerIsOurs(int signal) {
  struct sigaction current {};
  if (sigaction(signal, nullptr, &current) != 0) return false;
  return (current.sa_flags & SA_SIGINFO) && current.sa_sigaction == &Handler;
}

// The SigBlk mask of a thread, from /proc (bit n-1 is signal n).
bool SignalBlockedBy(int tid, int signal) {
  std::ifstream status("/proc/self/task/" + std::to_string(tid) + "/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("SigBlk:", 0) != 0) continue;
    unsigned long long mask = 0;
    if (std::sscanf(line.c_str() + 7, "%llx", &mask) != 1) return false;
    return (mask >> (signal - 1)) & 1ull;
  }
  return false;
}

}  // namespace

void RegisterTarget() {
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* stackAddr = nullptr;
    size_t stackSize = 0;
    if (pthread_attr_getstack(&attr, &stackAddr, &stackSize) == 0) {
      gStackLo.store(reinterpret_cast<uintptr_t>(stackAddr), std::memory_order_relaxed);
      gStackHi.store(reinterpret_cast<uintptr_t>(stackAddr) + stackSize, std::memory_order_relaxed);
    }
    pthread_attr_destroy(&attr);
  }
  gTargetTid.store(static_cast<int>(syscall(SYS_gettid)), std::memory_order_release);
}

int TargetTid() {
  return gTargetTid.load(std::memory_order_acquire);
}

int ChosenSignal() {
  return SIGRTMAX - 3;
}

Result Sample(std::chrono::milliseconds timeout, StackSample& out) {
#if !defined(__aarch64__) && !defined(__x86_64__)
  (void)timeout;
  (void)out;
  return Result::Unsupported;
#else
  int const tid = TargetTid();
  if (tid <= 0) return Result::NoTarget;
  int const signal = ChosenSignal();
  if (!gInstalled.load()) {
    struct sigaction existing {};
    if (sigaction(signal, nullptr, &existing) != 0) return Result::InstallFailed;
    bool const hasHandler = (existing.sa_flags & SA_SIGINFO) ? existing.sa_sigaction != nullptr
                                                              : existing.sa_handler != SIG_DFL;
    if (hasHandler) return Result::HandlerTaken;
    struct sigaction ours {};
    ours.sa_sigaction = &Handler;
    ours.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&ours.sa_mask);
    if (sigaction(signal, &ours, nullptr) != 0) return Result::InstallFailed;
    gInstalled.store(true);
  }
  // Checked before every request: should anything have replaced the handler,
  // the signal could now do something else entirely (or end the process).
  if (!HandlerIsOurs(signal)) return Result::HandlerTaken;
  if (SignalBlockedBy(tid, signal)) return Result::Blocked;

  gState.store(kArmed, std::memory_order_release);
  if (syscall(SYS_tgkill, getpid(), tid, signal) != 0) {
    gState.store(kIdle, std::memory_order_release);
    return Result::SendFailed;
  }
  auto const deadline = std::chrono::steady_clock::now() + timeout;
  while (gState.load(std::memory_order_acquire) != kDone) {
    if (std::chrono::steady_clock::now() >= deadline) {
      int expected = kArmed;
      if (gState.compare_exchange_strong(expected, kIdle, std::memory_order_acq_rel)) return Result::Timeout;
      // The handler started just now; it finishes in microseconds.
      while (gState.load(std::memory_order_acquire) != kDone) std::this_thread::yield();
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  out = gBuffer;
  gState.store(kIdle, std::memory_order_release);
  return out.stop == StackWalkStop::Unsupported ? Result::Unsupported : Result::Ok;
#endif
}

std::string DescribeResult(Result result) {
  std::string const signal = "signal " + std::to_string(ChosenSignal());
  switch (result) {
    case Result::Ok: return "ok";
    case Result::NoTarget: return "the main thread was never registered";
    case Result::HandlerTaken: return signal + " already has a handler in this process";
    case Result::InstallFailed: return "could not install a handler for " + signal;
    case Result::Blocked: return "the main thread blocks " + signal;
    case Result::SendFailed: return "could not send " + signal + " to the main thread";
    case Result::Timeout: return "the main thread did not take " + signal + " in time";
    case Result::Unsupported: return "no register access on this architecture";
  }
  return "?";
}

}  // namespace StackSampler

}  // namespace Vivify
