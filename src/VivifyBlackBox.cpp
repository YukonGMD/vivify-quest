#include "VivifyBlackBox.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace Vivify {

namespace {

constexpr char kMagic[8] = {'V', 'V', 'B', 'X', '0', '0', '0', '1'};
constexpr uint32_t kLayout = 1;

uint64_t WallMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}

char const* TypeName(uint16_t type) {
  switch (type) {
    case BlackBox::kFrame: return "FRAME";
    case BlackBox::kLoad: return "LOAD";
    case BlackBox::kUnload: return "UNLOAD";
    case BlackBox::kCamera: return "CAMERA";
    case BlackBox::kTarget: return "TARGET";
    case BlackBox::kScale: return "SCALE";
    case BlackBox::kShader: return "SHADER";
    case BlackBox::kEvent: return "EVENT";
    case BlackBox::kCrumb: return "CRUMB";
    case BlackBox::kWarning: return "WARN";
    case BlackBox::kGl: return "GL";
    case BlackBox::kSession: return "SESSION";
    default: return "?";
  }
}

}  // namespace

struct BlackBox::Header {
  char magic[8];
  uint32_t layout;
  uint32_t recordSize;
  uint32_t capacity;
  std::atomic<uint32_t> state;
  std::atomic<uint64_t> next;  // sequence number of the next record
  char session[192];
  char reserved[256 - 8 - 4 - 4 - 4 - 4 - 8 - 192];
};

struct BlackBox::Slot {
  std::atomic<uint64_t> committed;  // sequence + 1 once the record is whole; 0 empty
  uint64_t wallMs;
  float songTime;
  uint16_t type;
  uint16_t length;
  float values[3];
  char text[kTextSize];
};

BlackBox::~BlackBox() {
  Close();
}

std::string BlackBox::Describe(uint64_t wallMs, float songTime, uint16_t type, float a, float b, float c,
                               std::string_view text) {
  std::time_t const seconds = static_cast<std::time_t>(wallMs / 1000);
  std::tm local{};
  localtime_r(&seconds, &local);
  char head[64];
  if (std::isnan(songTime)) {
    std::snprintf(head, sizeof(head), "%02d:%02d:%02d.%03d s=- [%s] ", local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(wallMs % 1000), TypeName(type));
  } else {
    std::snprintf(head, sizeof(head), "%02d:%02d:%02d.%03d s=%.2f [%s] ", local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(wallMs % 1000), songTime, TypeName(type));
  }
  std::string line(head);
  char values[96] = {};
  switch (type) {
    case kFrame:
      std::snprintf(values, sizeof(values), "frame %.1f ms, GPU %.1f ms, scale %.2f", a, b, c);
      break;
    case kCamera:
    case kTarget:
      std::snprintf(values, sizeof(values), "%.0fx%.0f (%.0f) ", a, b, c);
      break;
    case kScale:
      std::snprintf(values, sizeof(values), "%.2f -> %.2f ", a, b);
      break;
    case kShader:
      std::snprintf(values, sizeof(values), "%.1f ms ", a);
      break;
    case kEvent:
      std::snprintf(values, sizeof(values), "beat %.2f ", a);
      break;
    default:
      break;
  }
  line += values;
  line += text;
  return line;
}

bool BlackBox::Open(std::string const& path, std::string_view session, uint32_t capacity, Previous& previous) {
  static_assert(sizeof(Header) == 256);
  static_assert(sizeof(Slot) == kRecordSize);
  // Shared with the file, so the atomics must not hide a lock.
  static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free);
  Close();
  previous = Previous{};
  if (capacity == 0) return false;
  size_t const bytes = sizeof(Header) + static_cast<size_t>(capacity) * sizeof(Slot);
  int const fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  struct stat info {};
  bool sized = ::fstat(fd, &info) == 0 && static_cast<size_t>(info.st_size) == bytes;
  if (!sized && ::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    ::close(fd);
    return false;
  }
  void* memory = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);  // the mapping keeps the file
  if (memory == MAP_FAILED) return false;
  auto* header = static_cast<Header*>(memory);
  auto* slots = reinterpret_cast<Slot*>(static_cast<char*>(memory) + sizeof(Header));

  // What the previous session left, if the layout is ours.
  if (sized && std::memcmp(header->magic, kMagic, sizeof(kMagic)) == 0 && header->layout == kLayout &&
      header->recordSize == sizeof(Slot) && header->capacity == capacity) {
    previous.found = true;
    previous.state = static_cast<State>(header->state.load(std::memory_order_acquire));
    previous.session.assign(header->session, strnlen(header->session, sizeof(header->session)));
    std::vector<std::pair<uint64_t, uint32_t>> order;
    for (uint32_t i = 0; i < capacity; i++) {
      uint64_t const committed = slots[i].committed.load(std::memory_order_acquire);
      if (committed != 0 && (committed - 1) % capacity == i) order.emplace_back(committed, i);
    }
    std::sort(order.begin(), order.end());
    for (auto const& [committed, i] : order) {
      Slot const& slot = slots[i];
      size_t const length = std::min<size_t>(slot.length, kTextSize);
      previous.lines.push_back(Describe(slot.wallMs, slot.songTime, slot.type, slot.values[0], slot.values[1],
                                        slot.values[2], std::string_view(slot.text, length)));
    }
  }

  // This session's, from empty.
  std::memset(memory, 0, bytes);
  std::memcpy(header->magic, kMagic, sizeof(kMagic));
  header->layout = kLayout;
  header->recordSize = sizeof(Slot);
  header->capacity = capacity;
  size_t const sessionLength = std::min(session.size(), sizeof(header->session) - 1);
  std::memcpy(header->session, session.data(), sessionLength);
  header->next.store(0, std::memory_order_relaxed);
  header->state.store(kRunning, std::memory_order_release);
  _header = header;
  _slots = slots;
  _capacity = capacity;
  _mappedBytes = bytes;
  return true;
}

void BlackBox::Close() {
  if (_header == nullptr) return;
  ::munmap(_header, _mappedBytes);
  _header = nullptr;
  _slots = nullptr;
  _capacity = 0;
  _mappedBytes = 0;
}

void BlackBox::Record(Type type, float songTime, std::string_view text, float a, float b, float c) {
  Header* const header = _header;
  if (header == nullptr) return;
  uint64_t const sequence = header->next.fetch_add(1, std::memory_order_relaxed);
  Slot& slot = _slots[sequence % _capacity];
  // Cleared first, so a record overwritten half-way is never read as whole.
  slot.committed.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  slot.wallMs = WallMs();
  slot.songTime = songTime;
  slot.type = type;
  slot.values[0] = a;
  slot.values[1] = b;
  slot.values[2] = c;
  size_t const length = std::min(text.size(), kTextSize);
  std::memcpy(slot.text, text.data(), length);
  slot.length = static_cast<uint16_t>(length);
  slot.committed.store(sequence + 1, std::memory_order_release);
}

void BlackBox::Sync() {
  if (_header != nullptr) ::msync(_header, _mappedBytes, MS_ASYNC);
}

void BlackBox::SetState(State state) {
  if (_header != nullptr) _header->state.store(state, std::memory_order_release);
}

}  // namespace Vivify
