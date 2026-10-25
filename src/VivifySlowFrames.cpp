#include "VivifySlowFrames.hpp"

#include <elf.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace Vivify {

std::optional<StackBounds> FindNamedThreadStack(std::vector<MapEntry> const& maps, int tid) {
  std::string const name = "[anon:stack_and_tls:" + std::to_string(tid) + "]";
  std::optional<StackBounds> bounds;
  for (auto const& entry : maps) {
    if (entry.path != name || !entry.readable) continue;
    if (!bounds) {
      bounds = StackBounds{entry.start, entry.end};
    } else if (entry.start == bounds->hi) {
      bounds->hi = entry.end;  // the same area split in two
    }
  }
  return bounds;
}

std::optional<StackBounds> FindStackHolding(std::vector<MapEntry> const& maps, uint64_t sp) {
  for (auto const& entry : maps) {
    if (sp < entry.start || sp >= entry.end) continue;
    // Anonymous memory only: a stack pointer inside a file mapping is not one.
    bool const anonymous = entry.path.empty() || entry.path.rfind("[anon:", 0) == 0 || entry.path == "[stack]";
    if (!anonymous || !entry.readable) return std::nullopt;
    return StackBounds{entry.start, entry.end};
  }
  return std::nullopt;
}

std::optional<uint64_t> ParseSyscallStackPointer(std::string_view text) {
  // "<nr> <6 argument registers> <sp> <pc>" while in a system call, "-1 <sp>
  // <pc>" while blocked outside one, "running" while on a CPU.
  std::istringstream in{std::string(text)};
  std::vector<std::string> fields;
  std::string field;
  while (in >> field) fields.push_back(field);
  if (fields.empty() || fields[0] == "running") return std::nullopt;
  std::string const* sp = nullptr;
  if (fields[0] == "-1") {
    if (fields.size() >= 3) sp = &fields[1];
  } else if (fields.size() >= 9) {
    sp = &fields[7];
  }
  if (sp == nullptr || sp->rfind("0x", 0) != 0) return std::nullopt;
  char* end = nullptr;
  uint64_t const value = std::strtoull(sp->c_str() + 2, &end, 16);
  if (end == sp->c_str() + 2 || *end != '\0' || value == 0) return std::nullopt;
  return value;
}

namespace {

std::string Basename(std::string const& path) {
  size_t const slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

constexpr uint8_t kEncodingOmit = 0xFF;
constexpr uint8_t kEncodingIndirect = 0x80;
constexpr uint8_t kSearchTableEncoding = 0x3B;  // DW_EH_PE_datarel | DW_EH_PE_sdata4

}  // namespace

// Reads DWARF unwind data inside one image, never outside its readable
// mappings.
struct ModuleTable::Cursor {
  ModuleTable const& table;
  Image const& image;
  uint64_t pos;

  bool Bytes(void* out, size_t length) {
    if (!table.Read(image, pos, out, length)) return false;
    pos += length;
    return true;
  }
  bool U8(uint8_t& value) { return Bytes(&value, 1); }
  bool U32(uint32_t& value) { return Bytes(&value, 4); }
  bool U64(uint64_t& value) { return Bytes(&value, 8); }
  bool Uleb(uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      uint8_t byte = 0;
      if (!U8(byte)) return false;
      value |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if (!(byte & 0x80)) return true;
    }
    return false;
  }
  bool Sleb(int64_t& value) {
    uint64_t result = 0;
    int shift = 0;
    uint8_t byte = 0;
    do {
      if (shift >= 64 || !U8(byte)) return false;
      result |= static_cast<uint64_t>(byte & 0x7F) << shift;
      shift += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) result |= ~0ull << shift;
    value = static_cast<int64_t>(result);
    return true;
  }
  // A DW_EH_PE-encoded value. pointer: apply its base (pc-relative to where it
  // is stored, data-relative to .eh_frame_hdr, absolute plus the load bias);
  // otherwise only its format counts, as for an FDE's address range.
  bool Encoded(uint8_t encoding, bool pointer, uint64_t& out) {
    if (encoding == kEncodingOmit || (encoding & kEncodingIndirect)) return false;
    uint64_t const at = pos;
    uint64_t value = 0;
    switch (encoding & 0x0F) {
      case 0x00: if (!U64(value)) return false; break;  // absptr
      case 0x01: if (!Uleb(value)) return false; break;
      case 0x02: { uint16_t v; if (!Bytes(&v, 2)) return false; value = v; break; }
      case 0x03: { uint32_t v; if (!U32(v)) return false; value = v; break; }
      case 0x04: if (!U64(value)) return false; break;
      case 0x09: { int64_t v; if (!Sleb(v)) return false; value = static_cast<uint64_t>(v); break; }
      case 0x0A: { int16_t v; if (!Bytes(&v, 2)) return false; value = static_cast<uint64_t>(static_cast<int64_t>(v)); break; }
      case 0x0B: { int32_t v; if (!Bytes(&v, 4)) return false; value = static_cast<uint64_t>(static_cast<int64_t>(v)); break; }
      case 0x0C: if (!U64(value)) return false; break;
      default: return false;
    }
    if (!pointer) {
      out = value;
      return true;
    }
    switch (encoding & 0x70) {
      case 0x00: out = value + image.bias; return true;
      case 0x10: out = value + at; return true;
      case 0x30: out = value + image.hdr; return true;
      default: return false;
    }
  }
};

int ModuleTable::ModuleIndex(std::string const& name) {
  auto const found = _moduleIndex.find(name);
  if (found != _moduleIndex.end()) return found->second;
  int const index = static_cast<int>(_names.size());
  _names.push_back(name);
  _moduleIndex.emplace(name, index);
  return index;
}

bool ModuleTable::Read(Image const& image, uint64_t address, void* out, size_t length) const {
  for (auto const& [start, end] : image.readable) {
    if (address >= start && address <= end && end - address >= length) return _read(address, out, length);
  }
  return false;
}

void ModuleTable::Build(std::vector<MapEntry> const& maps, MemoryReader read) {
  _read = std::move(read);
  _images.clear();
  _cieEncodings.clear();
  auto covered = [&](uint64_t address) {
    for (auto const& image : _images) {
      if (address >= image.lo && address < image.hi) return true;
    }
    return false;
  };
  for (size_t i = 0; i < maps.size(); i++) {
    MapEntry const& code = maps[i];
    if (!code.executable || covered(code.start)) continue;
    Image image;
    if (code.path.empty() || code.path[0] == '[') {
      // Not a file: one bucket per kind of mapping.
      image.module = ModuleIndex(code.path.empty() ? std::string("[anonymous]") : code.path);
      image.lo = code.start;
      image.hi = code.end;
      _images.push_back(std::move(image));
      continue;
    }
    // The ELF header is in the nearest mapping of the same file at or below
    // this one that starts with one (for a library stored inside an APK, that
    // is not file offset 0). Only libraries' own mappings are read.
    image.file = true;
    std::string name = Basename(code.path);
    for (size_t j = i + 1; j-- > 0 && i - j < 32;) {
      MapEntry const& candidate = maps[j];
      if (candidate.path != code.path || candidate.offset > code.offset || !candidate.readable) continue;
      if (candidate.end - candidate.start < sizeof(Elf64_Ehdr)) continue;
      Elf64_Ehdr header;
      if (!_read(candidate.start, &header, sizeof(header))) continue;
      if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64) continue;
      if (header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0 || header.e_phnum > 64) continue;
      uint64_t const span = candidate.end - candidate.start;
      if (header.e_phoff > span || (span - header.e_phoff) / sizeof(Elf64_Phdr) < header.e_phnum) continue;
      uint64_t minVaddr = UINT64_MAX, maxEnd = 0, hdrVaddr = 0, hdrSize = 0;
      bool ok = true;
      for (int p = 0; p < header.e_phnum && ok; p++) {
        Elf64_Phdr phdr;
        ok = _read(candidate.start + header.e_phoff + static_cast<uint64_t>(p) * sizeof(Elf64_Phdr), &phdr,
                   sizeof(phdr));
        if (!ok) break;
        if (phdr.p_type == PT_LOAD) {
          minVaddr = std::min<uint64_t>(minVaddr, phdr.p_vaddr);
          maxEnd = std::max<uint64_t>(maxEnd, phdr.p_vaddr + phdr.p_memsz);
        } else if (phdr.p_type == PT_GNU_EH_FRAME) {
          hdrVaddr = phdr.p_vaddr;
          hdrSize = phdr.p_memsz;
        }
      }
      if (!ok || minVaddr == UINT64_MAX || maxEnd <= minVaddr) continue;
      minVaddr &= ~static_cast<uint64_t>(0xFFF);
      image.elf = true;
      image.bias = candidate.start - minVaddr;
      image.lo = candidate.start;
      image.hi = std::max(code.end, (image.bias + maxEnd + 0xFFF) & ~static_cast<uint64_t>(0xFFF));
      if (hdrSize >= 12) image.hdr = image.bias + hdrVaddr;
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".apk") == 0) {
        char offset[32];
        std::snprintf(offset, sizeof(offset), "!0x%" PRIx64, candidate.offset);
        name += offset;
      }
      break;
    }
    if (!image.elf) {
      // No usable header: file offsets, as a tombstone would give them.
      image.lo = code.start;
      image.hi = code.end;
      image.bias = code.start - code.offset;
    }
    image.module = ModuleIndex(name);
    for (auto const& entry : maps) {
      if (entry.path == code.path && entry.readable && entry.start >= image.lo && entry.end <= image.hi) {
        image.readable.emplace_back(entry.start, entry.end);
      }
    }
    if (image.hdr != 0) {
      // version 1, then the encodings of the .eh_frame pointer, the entry
      // count and the table; only the datarel/sdata4 table can be searched.
      Cursor cursor{*this, image, image.hdr};
      uint8_t version = 0, framePointerEncoding = 0, countEncoding = 0, tableEncoding = 0;
      uint64_t framePointer = 0, count = 0;
      if (cursor.U8(version) && version == 1 && cursor.U8(framePointerEncoding) && cursor.U8(countEncoding) &&
          cursor.U8(tableEncoding) && tableEncoding == kSearchTableEncoding &&
          cursor.Encoded(framePointerEncoding, true, framePointer) && cursor.Encoded(countEncoding, false, count) &&
          count > 0 && count < (1u << 26)) {
        uint32_t probe[2];
        uint64_t const tableBytes = count * 8;
        if (Read(image, cursor.pos, probe, sizeof(probe)) &&
            Read(image, cursor.pos + tableBytes - sizeof(probe), probe, sizeof(probe))) {
          image.table = cursor.pos;
          image.entries = static_cast<uint32_t>(count);
        }
      }
    }
    _images.push_back(std::move(image));
  }
  std::sort(_images.begin(), _images.end(), [](Image const& a, Image const& b) { return a.lo < b.lo; });
}

bool ModuleTable::CieFdeEncoding(Image const& image, uint64_t cie, uint8_t& encoding) const {
  for (auto const& [at, known] : _cieEncodings) {
    if (at == cie) {
      encoding = known;
      return true;
    }
  }
  Cursor cursor{*this, image, cie};
  uint32_t length32 = 0;
  if (!cursor.U32(length32) || length32 == 0) return false;
  bool const wide = length32 == 0xFFFFFFFFu;
  uint64_t length = length32;
  if (wide && !cursor.U64(length)) return false;
  uint64_t id = 0;
  if (wide) {
    if (!cursor.U64(id)) return false;
  } else {
    uint32_t id32 = 0;
    if (!cursor.U32(id32)) return false;
    id = id32;
  }
  uint8_t version = 0;
  if (id != 0 || !cursor.U8(version) || (version != 1 && version != 3 && version != 4)) return false;
  char augmentation[9] = {};
  for (int i = 0;; i++) {
    uint8_t c = 0;
    if (!cursor.U8(c)) return false;
    if (c == 0) break;
    if (i == 8) return false;
    augmentation[i] = static_cast<char>(c);
  }
  if (version == 4) {
    uint8_t sizes[2];
    if (!cursor.Bytes(sizes, 2)) return false;
  }
  if (std::strstr(augmentation, "eh") != nullptr) {
    uint64_t skip = 0;
    if (!cursor.U64(skip)) return false;
  }
  uint64_t codeAlign = 0, returnRegister = 0;
  int64_t dataAlign = 0;
  if (!cursor.Uleb(codeAlign) || !cursor.Sleb(dataAlign)) return false;
  if (version == 1) {
    uint8_t reg = 0;
    if (!cursor.U8(reg)) return false;
  } else if (!cursor.Uleb(returnRegister)) {
    return false;
  }
  encoding = 0x00;  // absptr, unless 'R' says otherwise
  if (augmentation[0] == 'z') {
    uint64_t dataLength = 0;
    if (!cursor.Uleb(dataLength)) return false;
    for (char const* c = augmentation + 1; *c != '\0'; c++) {
      if (*c == 'R') {
        if (!cursor.U8(encoding)) return false;
        break;
      }
      if (*c == 'L') {
        uint8_t lsda = 0;
        if (!cursor.U8(lsda)) return false;
      } else if (*c == 'P') {
        uint8_t personality = 0;
        uint64_t skip = 0;
        if (!cursor.U8(personality) || !cursor.Encoded(personality & 0x0F, false, skip)) return false;
      } else if (*c != 'S' && *c != 'B' && *c != 'G') {
        break;  // unknown: keep what is known
      }
    }
  }
  if (_cieEncodings.size() < 64) _cieEncodings.emplace_back(cie, encoding);
  return true;
}

bool ModuleTable::FdeCovers(Image const& image, uint64_t fde, uint64_t address, uint64_t& start) const {
  Cursor cursor{*this, image, fde};
  uint32_t length32 = 0;
  if (!cursor.U32(length32) || length32 == 0) return false;
  bool const wide = length32 == 0xFFFFFFFFu;
  uint64_t length = length32;
  if (wide && !cursor.U64(length)) return false;
  uint64_t const idAt = cursor.pos;
  uint64_t cieOffset = 0;
  if (wide) {
    if (!cursor.U64(cieOffset)) return false;
  } else {
    uint32_t offset32 = 0;
    if (!cursor.U32(offset32)) return false;
    cieOffset = offset32;
  }
  if (cieOffset == 0 || cieOffset > idAt) return false;  // 0 marks a CIE, not an FDE
  uint8_t encoding = 0;
  if (!CieFdeEncoding(image, idAt - cieOffset, encoding)) return false;
  uint64_t begin = 0, range = 0;
  if (!cursor.Encoded(encoding, true, begin) || !cursor.Encoded(encoding & 0x0F, false, range)) return false;
  if (address < begin || address - begin >= range) return false;
  start = begin;
  return true;
}

CodeLocation ModuleTable::Locate(uint64_t address) const {
  CodeLocation location;
  auto const after = std::upper_bound(_images.begin(), _images.end(), address,
                                      [](uint64_t value, Image const& image) { return value < image.lo; });
  if (after == _images.begin()) return location;
  Image const& image = *(after - 1);
  if (address >= image.hi) return location;
  location.module = image.module;
  if (!image.elf) {
    location.address = image.file ? address - image.bias : 0;
    return location;
  }
  location.address = address - image.bias;
  if (image.table == 0) return location;
  // The last entry starting at or before the address.
  uint32_t lo = 0, hi = image.entries;
  while (lo < hi) {
    uint32_t const mid = lo + (hi - lo) / 2;
    int32_t startOffset = 0;
    if (!Read(image, image.table + static_cast<uint64_t>(mid) * 8, &startOffset, 4)) return location;
    if (image.hdr + static_cast<uint64_t>(static_cast<int64_t>(startOffset)) <= address) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return location;
  int32_t entry[2];
  if (!Read(image, image.table + static_cast<uint64_t>(lo - 1) * 8, entry, sizeof(entry))) return location;
  uint64_t start = 0;
  if (!FdeCovers(image, image.hdr + static_cast<uint64_t>(static_cast<int64_t>(entry[1])), address, start)) {
    return location;
  }
  location.address = start - image.bias;
  location.function = true;
  return location;
}

std::string FormatCodeLocation(CodeLocation const& location, std::vector<std::string> const& modules) {
  if (location.module < 0 || location.module >= static_cast<int>(modules.size())) return "[unmapped]";
  std::string const& name = modules[location.module];
  if (!name.empty() && name[0] == '[') return name;
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%s0x%" PRIx64, location.function ? "+" : "@", location.address);
  return name + buffer;
}

void SlowFrameProfile::Reset() {
  _sections.clear();
}

void SlowFrameProfile::CountSlowFrame(int section) {
  _sections[section].slowFrames++;
}

namespace {

uint64_t ProfileKey(CodeLocation const& location) {
  return (static_cast<uint64_t>(location.module + 1) << 48) | (location.address & 0x0000FFFFFFFFFFFFull);
}

CodeLocation FromProfileKey(uint64_t key, bool function) {
  CodeLocation location;
  location.module = static_cast<int>(key >> 48) - 1;
  location.address = key & 0x0000FFFFFFFFFFFFull;
  location.function = function;
  return location;
}

}  // namespace

void SlowFrameProfile::AddSample(int section, int thread, bool running, CodeLocation const* stack, int count) {
  if (thread < 0 || thread >= kThreads || count <= 0) return;
  PerThread& counts = _sections[section].threads[thread];
  counts.samples++;
  if (running) counts.running++;
  uint64_t seen[kMaxStackFrames];
  int seenCount = 0;
  for (int i = 0; i < count && i < kMaxStackFrames; i++) {
    uint64_t const key = ProfileKey(stack[i]);
    Counts& entry = counts.functions[key];
    entry.function = stack[i].function;
    if (i == 0) entry.self++;
    // Once per sample, however often recursion puts it on the stack.
    if (std::find(seen, seen + seenCount, key) == seen + seenCount) {
      seen[seenCount++] = key;
      entry.inclusive++;
      entry.depthSum += i;
    }
  }
}

std::vector<std::string> SlowFrameProfile::Summary(std::function<std::string(int)> const& label,
                                                   std::vector<std::string> const& modules, int top) const {
  static char const* const kNames[kThreads] = {"main", "render"};
  auto percent = [](int part, int whole) {
    return whole > 0 ? static_cast<int>((100.0 * part) / whole + 0.5) : 0;
  };
  std::vector<std::string> lines;
  for (auto const& [index, section] : _sections) {
    std::string head = label(index) + ": " + std::to_string(section.slowFrames) + " slow frame(s) sampled";
    for (int t = 0; t < kThreads; t++) {
      PerThread const& thread = section.threads[t];
      if (thread.samples == 0) continue;
      head += std::string(t == 0 ? "; " : ", ") + kNames[t] + " thread " + std::to_string(thread.samples) +
              " time(s), on a CPU in " + std::to_string(percent(thread.running, thread.samples)) + "%";
    }
    lines.push_back(head);
    for (int t = 0; t < kThreads; t++) {
      PerThread const& thread = section.threads[t];
      if (thread.samples == 0) continue;
      std::vector<std::pair<uint64_t, Counts>> entries(thread.functions.begin(), thread.functions.end());
      auto list = [&](bool self) {
        // Most samples first; among equals (a thread waiting in one place has
        // every frame on every stack), the frames nearest where it was.
        std::sort(entries.begin(), entries.end(), [self](auto const& a, auto const& b) {
          int const ca = self ? a.second.self : a.second.inclusive;
          int const cb = self ? b.second.self : b.second.inclusive;
          if (ca != cb) return ca > cb;
          if (!self && a.second.depthSum * cb != b.second.depthSum * ca) {
            return a.second.depthSum * cb < b.second.depthSum * ca;
          }
          return a.first < b.first;
        });
        std::string text;
        int listed = 0;
        for (auto const& [key, counts] : entries) {
          int const count = self ? counts.self : counts.inclusive;
          if (count == 0 || listed == top) break;
          if (listed++ > 0) text += ", ";
          text += FormatCodeLocation(FromProfileKey(key, counts.function), modules) + " " +
                  std::to_string(percent(count, thread.samples)) + "%";
        }
        return text;
      };
      lines.push_back(std::string("  ") + kNames[t] + " thread was in: " + list(true));
      lines.push_back(std::string("  ") + kNames[t] + " thread, anywhere on the stack: " + list(false));
      // naomi.10: a render thread inside the GPU driver's shader compiler is
      // a first-use compile -- the stall Version X wants gone. Counted by the
      // compiler function seen in the most samples (each once per sample).
      if (t == kRender) {
        int compiling = 0;
        std::string library;
        for (auto const& [key, counts] : entries) {
          std::string const where = FormatCodeLocation(FromProfileKey(key, counts.function), modules);
          bool const compiler = where.find("llvm") != std::string::npos || where.find("glnext") != std::string::npos;
          if (compiler && counts.inclusive > compiling) {
            compiling = counts.inclusive;
            library = where.substr(0, where.find_first_of("+@"));
          }
        }
        if (compiling > 0) {
          lines.push_back("  render thread in the GPU driver's shader compiler (" + library + ") in " +
                          std::to_string(percent(compiling, thread.samples)) +
                          "% of samples: first-use shader compiles");
        }
      }
    }
  }
  return lines;
}

}  // namespace Vivify
