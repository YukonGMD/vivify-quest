#include "VivifySessionLog.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace Vivify::SessionLog {

namespace {

bool IsWordChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-';
}

bool IsDomainChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-';
}

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string Sanitize(std::string_view text, std::string_view allowedExtra) {
  std::string out;
  for (char c : text) {
    bool const ok = std::isalnum(static_cast<unsigned char>(c)) || allowedExtra.find(c) != std::string_view::npos;
    out += ok ? c : '_';
  }
  return out;
}

std::string Field(std::string_view text) {
  std::string out;
  for (char c : text) out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
  return out;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  while (true) {
    size_t const at = text.find(separator, start);
    parts.push_back(text.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
    if (at == std::string_view::npos) break;
    start = at + 1;
  }
  return parts;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  return text;
}

// The keys whose values Scrub hides, lower case.
constexpr std::string_view kPrivateKeys[] = {"user", "username", "user_name", "account", "player",
                                             "playername", "player_name", "ssid", "serial", "email"};

}  // namespace

std::string FormatPrefix(std::tm const& local, int millis, float songTime, std::string_view tag) {
  char buffer[64];
  if (std::isnan(songTime)) {
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d s=- [", local.tm_hour, local.tm_min, local.tm_sec,
                  millis);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d s=%.2f [", local.tm_hour, local.tm_min, local.tm_sec,
                  millis, songTime);
  }
  std::string out(buffer);
  out += tag;
  out += "] ";
  return out;
}

std::string Scrub(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    // key=value with a private key: keep the key, hide the value.
    if (text[i] == '=' && i > 0) {
      size_t keyStart = i;
      while (keyStart > 0 && (std::isalnum(static_cast<unsigned char>(text[keyStart - 1])) || text[keyStart - 1] == '_')) {
        keyStart--;
      }
      std::string const key = Lower(text.substr(keyStart, i - keyStart));
      bool const hidden = std::find(std::begin(kPrivateKeys), std::end(kPrivateKeys), key) != std::end(kPrivateKeys);
      if (hidden) {
        out += '=';
        size_t end = i + 1;
        if (end < text.size() && (text[end] == '\'' || text[end] == '"')) {
          char const quote = text[end];
          size_t const close = text.find(quote, end + 1);
          end = close == std::string_view::npos ? text.size() : close + 1;
          out += quote;
          out += "<hidden>";
          out += quote;
        } else {
          while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && text[end] != ',' &&
                 text[end] != ';') {
            end++;
          }
          out += "<hidden>";
        }
        i = end;
        continue;
      }
    }
    // An e-mail address.
    if (text[i] == '@' && i > 0 && IsWordChar(text[i - 1])) {
      size_t end = i + 1;
      while (end < text.size() && IsDomainChar(text[end])) end++;
      std::string_view const domain = text.substr(i + 1, end - i - 1);
      size_t const dot = domain.rfind('.');
      if (dot != std::string_view::npos && dot > 0 && domain.size() - dot > 2) {
        size_t localStart = out.size();
        while (localStart > 0 && IsWordChar(out[localStart - 1])) localStart--;
        out.erase(localStart);
        out += "<email>";
        i = end;
        continue;
      }
    }
    out += text[i];
    i++;
  }
  return out;
}

std::string RateKey(std::string_view text) {
  std::string key;
  key.reserve(text.size());
  for (char c : text) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      if (key.empty() || key.back() != '#') key += '#';
    } else {
      key += c;
    }
  }
  return key;
}

std::string RateLimiter::Report(Entry const& entry) {
  std::string example = entry.example.substr(0, 160);
  return "(left out " + std::to_string(entry.suppressed) + " more line(s) like this in 10 s: " + example + ")";
}

void RateLimiter::Expire(double now, std::vector<std::string>& reports) {
  for (auto it = _entries.begin(); it != _entries.end();) {
    if (now - it->second.windowStart >= _window) {
      if (it->second.suppressed > 0) reports.push_back(Report(it->second));
      it = _entries.erase(it);
    } else {
      ++it;
    }
  }
}

void RateLimiter::ExpireAll(std::vector<std::string>& reports) {
  for (auto const& [key, entry] : _entries) {
    if (entry.suppressed > 0) reports.push_back(Report(entry));
  }
  _entries.clear();
}

bool RateLimiter::Allow(std::string_view text, double now, std::vector<std::string>& reports) {
  Expire(now, reports);
  auto& entry = _entries[RateKey(text)];
  if (entry.count == 0) {
    entry.windowStart = now;
    entry.example = std::string(text);
  }
  entry.count++;
  if (entry.count <= _burst) return true;
  entry.suppressed++;
  _suppressedTotal++;
  return false;
}

std::string SessionFileName(std::tm const& local, std::string_view version, std::string_view commit) {
  size_t const dash = version.find('-');
  std::string_view const build = dash == std::string_view::npos ? version : version.substr(dash + 1);
  std::string hash(commit.substr(0, std::min<size_t>(7, commit.find('-') == std::string_view::npos
                                                            ? commit.size()
                                                            : commit.find('-'))));
  if (commit.find("-dirty") != std::string_view::npos) hash += "-dirty";
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d_%02d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1,
                local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
  return "Vivify_" + std::string(stamp) + "_" + Sanitize(build, ".-") + "-" + Sanitize(hash, "-") + ".txt";
}

std::string UtcOffset(std::tm const& local) {
  long const offset = local.tm_gmtoff;
  long const minutes = std::labs(offset) / 60;
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%c%02ld%02ld", offset < 0 ? '-' : '+', minutes / 60, minutes % 60);
  return buffer;
}

std::vector<Session> ParseIndex(std::string_view text) {
  std::vector<Session> sessions;
  for (auto line : Split(text, '\n')) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line.rfind("file\t", 0) == 0) continue;
    auto const fields = Split(line, '\t');
    if (fields.size() < 6 || fields[0].empty() || fields[0].find('/') != std::string_view::npos) continue;
    Session session;
    session.file = std::string(fields[0]);
    session.started = std::string(fields[1]);
    session.build = std::string(fields[2]);
    session.maps = std::string(fields[3]);
    session.ended = std::string(fields[4]);
    session.bytes = std::strtoull(std::string(fields[5]).c_str(), nullptr, 10);
    sessions.push_back(std::move(session));
  }
  return sessions;
}

std::string FormatIndex(std::vector<Session> const& sessions) {
  std::string out = "file\tstarted\tbuild\tmaps\tended\tbytes\n";
  for (auto const& session : sessions) {
    out += Field(session.file) + '\t' + Field(session.started) + '\t' + Field(session.build) + '\t' +
           Field(session.maps) + '\t' + Field(session.ended) + '\t' + std::to_string(session.bytes) + '\n';
  }
  return out;
}

std::vector<std::string> FilesToDelete(std::vector<Session> const& sessions, std::string_view current,
                                       size_t keepRecent, size_t keepUnexpected, uint64_t maxBytes) {
  std::vector<bool> keep(sessions.size(), false);
  size_t recent = 0, unexpected = 0;
  for (size_t i = sessions.size(); i-- > 0;) {
    if (sessions[i].file == current) {
      keep[i] = true;
      continue;
    }
    if (recent < keepRecent) {
      keep[i] = true;
      recent++;
    }
    if (sessions[i].ended == kUnexpected && unexpected < keepUnexpected) {
      keep[i] = true;
      unexpected++;
    }
  }
  uint64_t total = 0;
  for (size_t i = 0; i < sessions.size(); i++) {
    if (keep[i]) total += sessions[i].bytes;
  }
  for (size_t i = 0; i < sessions.size() && total > maxBytes; i++) {
    if (!keep[i] || sessions[i].file == current) continue;
    keep[i] = false;
    total -= sessions[i].bytes;
  }
  std::vector<std::string> remove;
  for (size_t i = 0; i < sessions.size(); i++) {
    if (!keep[i]) remove.push_back(sessions[i].file);
  }
  return remove;
}

std::string_view ClassifyEnd(std::string_view tail, std::optional<uint32_t> blackBoxState) {
  if (tail.find(kFooter) != std::string_view::npos) return kClean;
  if (blackBoxState) {
    switch (*blackBoxState) {
      case 3: return kClean;
      case 2: return kBackground;
      default: return kUnexpected;
    }
  }
  size_t const paused = tail.rfind("=== app paused");
  size_t const resumed = tail.rfind("=== app resumed");
  if (paused != std::string_view::npos && (resumed == std::string_view::npos || resumed < paused)) return kBackground;
  return kUnexpected;
}

std::map<std::string, std::string> ParseSummary(std::string_view text) {
  std::map<std::string, std::string> out;
  for (auto line : Split(text, '\n')) {
    size_t const eq = line.find('=');
    if (eq == std::string_view::npos || eq == 0) continue;
    std::string_view value = line.substr(eq + 1);
    if (!value.empty() && value.back() == '\r') value.remove_suffix(1);
    out[std::string(line.substr(0, eq))] = std::string(value);
  }
  return out;
}

std::string FormatSummary(std::map<std::string, std::string> const& summary) {
  std::string out;
  for (auto const& [key, value] : summary) out += Field(key) + "=" + Field(value) + "\n";
  return out;
}

std::string WhatChanged(std::map<std::string, std::string> const& before,
                        std::map<std::string, std::string> const& after) {
  if (before.empty()) return {};
  std::vector<std::string> top, mods, settings;
  std::set<std::string> keys;
  for (auto const& [key, value] : before) keys.insert(key);
  for (auto const& [key, value] : after) keys.insert(key);
  for (auto const& key : keys) {
    auto const b = before.find(key);
    auto const a = after.find(key);
    bool const inBefore = b != before.end(), inAfter = a != after.end();
    if (inBefore && inAfter && b->second == a->second) continue;
    if (key.rfind("mod:", 0) == 0) {
      std::string const id = key.substr(4);
      if (!inBefore) mods.push_back("+" + id + " " + a->second);
      else if (!inAfter) mods.push_back("-" + id);
      else mods.push_back(id + " " + b->second + " -> " + a->second);
    } else if (key.rfind("setting:", 0) == 0) {
      std::string const name = key.substr(8);
      settings.push_back(name + " " + (inBefore ? b->second : "(new)") + " -> " + (inAfter ? a->second : "(removed)"));
    } else {
      top.push_back(key + " " + (inBefore ? b->second : "?") + " -> " + (inAfter ? a->second : "?"));
    }
  }
  std::string out;
  auto join = [](std::vector<std::string> const& items) {
    std::string text;
    for (auto const& item : items) text += (text.empty() ? "" : ", ") + item;
    return text;
  };
  for (auto const& item : top) out += (out.empty() ? "" : "; ") + item;
  if (!mods.empty()) out += std::string(out.empty() ? "" : "; ") + "mods: " + join(mods);
  if (!settings.empty()) out += std::string(out.empty() ? "" : "; ") + "settings: " + join(settings);
  return out.empty() ? "nothing" : out;
}

void LoadTimeline::Reset() {
  _phases.clear();
  _firstMs = -1.0;
}

std::optional<std::string> LoadTimeline::Feed(std::string_view crumb, double nowMs) {
  if (crumb.rfind("first frame after level start", 0) == 0) {
    double levelStart = -1.0;
    std::string line = "load timeline:";
    bool first = true;
    for (auto const& phase : _phases) {
      if (phase.name == "level start") levelStart = phase.startMs;
      char buffer[96];
      if (phase.durationMs >= 0.0) {
        std::snprintf(buffer, sizeof(buffer), "%s %.0f ms", phase.name.c_str(), phase.durationMs);
      } else {
        std::snprintf(buffer, sizeof(buffer), "%s (no end)", phase.name.c_str());
      }
      line += std::string(first ? " " : ", ") + buffer;
      first = false;
    }
    if (first) line += " (no phases seen)";
    if (levelStart >= 0.0) {
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "; level start to first frame %.0f ms", nowMs - levelStart);
      line += buffer;
    }
    Reset();
    return line;
  }
  for (auto part : Split(crumb, ';')) {
    part = Trim(part);
    for (std::string_view marker : {std::string_view(" begin"), std::string_view(" end")}) {
      size_t const at = part.find(marker);
      if (at == std::string_view::npos || at == 0) continue;
      size_t const after = at + marker.size();
      if (after < part.size() && part[after] != ':' && part[after] != ' ') continue;
      std::string const name(part.substr(0, at));
      if (name.find(':') != std::string::npos || name.size() > 40) continue;
      auto it = std::find_if(_phases.begin(), _phases.end(), [&](Phase const& p) { return p.name == name; });
      if (marker == " begin") {
        if (it == _phases.end()) {
          if (_phases.size() >= 32) break;
          _phases.push_back({name, nowMs, -1.0});
        } else {
          *it = Phase{name, nowMs, -1.0};
        }
      } else if (it != _phases.end() && it->durationMs < 0.0) {
        it->durationMs = nowMs - it->startMs;
      }
      break;
    }
  }
  return std::nullopt;
}

namespace {

namespace fs = std::filesystem;

std::string ReadAll(std::string const& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string ReadTail(std::string const& path, size_t bytes) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return {};
  std::streamoff const size = in.tellg();
  std::streamoff const start = size > static_cast<std::streamoff>(bytes) ? size - static_cast<std::streamoff>(bytes) : 0;
  in.seekg(start);
  std::string tail(static_cast<size_t>(size - start), '\0');
  in.read(tail.data(), static_cast<std::streamsize>(tail.size()));
  return tail;
}

// Written beside the target and renamed, so a cut-off write never leaves a
// half index.
void WriteAtomic(std::string const& path, std::string const& text) {
  std::string const temporary = path + ".new";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << text;
    if (!out) return;
  }
  std::error_code ec;
  fs::rename(temporary, path, ec);
  if (ec) fs::remove(temporary, ec);
}

uint64_t SizeOf(std::string const& path) {
  std::error_code ec;
  auto const size = fs::file_size(path, ec);
  return ec ? 0 : static_cast<uint64_t>(size);
}

bool IsSessionFileName(std::string_view name) {
  return name.rfind("Vivify_", 0) == 0 && name.size() > 31 && name.substr(name.size() - 4) == ".txt";
}

// VivifySession.txt as naomi.10 writes it: two short lines, no log.
bool IsPointerOnly(std::string const& path) {
  uint64_t const size = SizeOf(path);
  if (size == 0 || size > 4096) return false;
  return ReadAll(path).rfind(kPointerText, 0) == 0;
}

// "Vivify_2026-10-25_01-47-12_naomi.10-beta.1-89f98f5.txt" -> start time and build.
void FromFileName(std::string_view name, std::string& started, std::string& build) {
  if (!IsSessionFileName(name)) return;
  std::string_view const date = name.substr(7, 10);
  std::string_view const time = name.substr(18, 8);
  started = std::string(date) + " " + std::string(time);
  std::replace(started.begin() + 11, started.end(), '-', ':');
  std::string_view rest = name.substr(27, name.size() - 27 - 4);
  if (rest.size() > 6 && rest.substr(rest.size() - 6) == "-dirty") rest.remove_suffix(6);
  size_t const dash = rest.rfind('-');
  build = std::string(dash == std::string_view::npos ? rest : rest.substr(0, dash));
}

std::string Explain(Session const& session) {
  std::string const head = "previous session " + session.file + " (" + session.build + ")";
  if (session.ended == kClean) return head + " ended cleanly";
  if (session.ended == kBackground) {
    return head + " was closed while in the background (the usual way the headset quits an app)";
  }
  if (session.ended == kUnexpected) {
    return head + " ended unexpectedly: it stopped while running (a crash, a hang the system killed, or power)";
  }
  return head + " ended without a record of how (" + session.ended + ")";
}

}  // namespace

Startup BeginSession(std::string const& dir, Session const& current, PreviousBox const& box,
                     std::string const& legacyLog) {
  Startup out;
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::string const indexPath = dir + "/index.txt";
  std::vector<Session> sessions = ParseIndex(ReadAll(indexPath));

  // naomi.9 and earlier kept one VivifySession.txt, replaced at every launch.
  // Since naomi.10 it only points at Sessions/; that pointer is not a log
  // (beta.1 and beta.2 moved it in at every launch as an "unknown" session).
  if (!legacyLog.empty() && fs::exists(legacyLog, ec) && !IsPointerOnly(legacyLog)) {
    std::string const head = ReadAll(legacyLog).substr(0, 200);
    std::string version = "unknown", commit = "unknown";
    size_t const v = head.find("=== Vivify ");
    size_t const c = head.find("(commit ");
    if (v != std::string::npos) {
      size_t const end = head.find(' ', v + 11);
      if (end != std::string::npos) version = head.substr(v + 11, end - v - 11);
    }
    if (c != std::string::npos) {
      size_t const end = head.find(')', c + 8);
      if (end != std::string::npos) commit = head.substr(c + 8, end - c - 8);
    }
    auto const written = fs::last_write_time(legacyLog, ec);
    std::time_t stamp = std::time(nullptr);
    if (!ec) {
      stamp = std::chrono::system_clock::to_time_t(std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          written - fs::file_time_type::clock::now() + std::chrono::system_clock::now()));
    }
    std::tm local{};
    localtime_r(&stamp, &local);
    std::string const name = SessionFileName(local, version, commit);
    std::string const target = dir + "/" + name;
    if (!fs::exists(target, ec)) {
      fs::rename(legacyLog, target, ec);
      if (ec) {
        ec.clear();
        if (fs::copy_file(legacyLog, target, ec)) fs::remove(legacyLog, ec);
      }
    }
    if (fs::exists(target, ec)) {
      Session legacy;
      legacy.file = name;
      FromFileName(name, legacy.started, legacy.build);
      legacy.started += " (file time)";
      legacy.ended = "unknown";
      legacy.bytes = SizeOf(target);
      sessions.insert(sessions.begin(), legacy);
      out.notes.push_back("the old VivifySession.txt (" + version + ") is now " + name);
    }
  }

  // The pointer copies beta.1 and beta.2 left: not sessions. Without this they
  // take places in the 10 kept, and the newest of them (named a second after
  // its launch) was taken for the previous session.
  std::vector<std::string> pointerFiles;
  for (auto const& entry : fs::directory_iterator(dir, ec)) {
    std::string const name = entry.path().filename().string();
    if (IsSessionFileName(name) && name != current.file && IsPointerOnly(entry.path().string())) {
      pointerFiles.push_back(entry.path().string());
    }
  }
  size_t pointerCopies = 0;
  for (auto const& path : pointerFiles) {
    std::error_code removeEc;
    if (fs::remove(path, removeEc)) pointerCopies++;
  }
  if (pointerCopies > 0) {
    out.notes.push_back("removed " + std::to_string(pointerCopies) +
                        " leftover copy(ies) of the VivifySession.txt pointer (naomi.10-beta.1 and beta.2 "
                        "saved one as a session at every launch)");
  }

  // Session files the index does not list (a launch cut off before writing it).
  std::set<std::string> listed;
  for (auto const& session : sessions) listed.insert(session.file);
  for (auto const& entry : fs::directory_iterator(dir, ec)) {
    std::string const name = entry.path().filename().string();
    if (!IsSessionFileName(name) || listed.count(name) || name == current.file) continue;
    Session found;
    found.file = name;
    FromFileName(name, found.started, found.build);
    found.ended = std::string(kRunning);
    sessions.push_back(found);
  }
  // Index lines whose file is gone.
  sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                [&](Session const& s) { return !fs::exists(dir + "/" + s.file, ec); }),
                 sessions.end());
  std::stable_sort(sessions.begin(), sessions.end(), [](Session const& a, Session const& b) {
    // Names start with the local start time, so they sort in start order.
    return a.file.substr(0, 26) < b.file.substr(0, 26);
  });

  // Settle what is still marked running.
  for (auto& session : sessions) {
    if (session.ended != kRunning) continue;
    std::string const path = dir + "/" + session.file;
    bool const boxed = box.found && box.session == session.file;
    session.ended = std::string(ClassifyEnd(ReadTail(path, 8192), boxed ? std::optional<uint32_t>(box.state) : std::nullopt));
    if (boxed && session.ended != kClean && !box.lines.empty()) {
      std::ofstream file(path, std::ios::binary | std::ios::app);
      file << "\n=== black box: the last " << box.lines.size() << " events before this session ended ("
           << session.ended << ") ===\n";
      for (auto const& line : box.lines) file << line << '\n';
      file << "=== end of black box ===\n";
    }
    session.bytes = SizeOf(path);
  }
  for (auto& session : sessions) {
    if (session.bytes == 0) session.bytes = SizeOf(dir + "/" + session.file);
  }

  if (!sessions.empty()) {
    out.previous = sessions.back();
    out.notes.push_back(Explain(sessions.back()));
    if (box.found && box.session == sessions.back().file && sessions.back().ended == kUnexpected) {
      size_t const shown = std::min<size_t>(box.lines.size(), 12);
      if (shown > 0) {
        out.notes.push_back("its last " + std::to_string(shown) + " black-box events (all " +
                            std::to_string(box.lines.size()) + " are at the end of its file):");
      }
      for (size_t i = box.lines.size() - shown; i < box.lines.size(); i++) out.notes.push_back("  " + box.lines[i]);
    }
  }

  Session running = current;
  running.ended = std::string(kRunning);
  sessions.push_back(running);
  out.deleted = FilesToDelete(sessions, current.file);
  for (auto const& file : out.deleted) fs::remove(dir + "/" + file, ec);
  sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                [&](Session const& s) {
                                  return std::find(out.deleted.begin(), out.deleted.end(), s.file) != out.deleted.end();
                                }),
                 sessions.end());
  WriteAtomic(indexPath, FormatIndex(sessions));
  out.sessions = sessions;
  return out;
}

void UpdateSession(std::string const& dir, std::string const& file, std::string const& maps, std::string_view ended,
                   uint64_t bytes) {
  std::string const indexPath = dir + "/index.txt";
  auto sessions = ParseIndex(ReadAll(indexPath));
  bool found = false;
  for (auto& session : sessions) {
    if (session.file != file) continue;
    session.maps = maps;
    if (!ended.empty()) session.ended = std::string(ended);
    session.bytes = bytes;
    found = true;
  }
  if (found) WriteAtomic(indexPath, FormatIndex(sessions));
}

}  // namespace Vivify::SessionLog
