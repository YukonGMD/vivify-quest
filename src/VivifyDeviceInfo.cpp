#include "VivifyDeviceInfo.hpp"

#include <malloc.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace Vivify::DeviceInfo {

namespace {

bool ReadNumber(std::string const& path, double& value) {
  std::ifstream in(path);
  if (!in) return false;
  in >> value;
  return static_cast<bool>(in);
}

std::string ReadWord(std::string const& path) {
  std::ifstream in(path);
  std::string word;
  if (in) std::getline(in, word);
  while (!word.empty() && (word.back() == '\n' || word.back() == '\r' || word.back() == ' ')) word.pop_back();
  return word;
}

// Thermal zones report millidegrees, a few report degrees.
double ZoneCelsius(double raw) {
  return raw > 1000.0 ? raw / 1000.0 : raw;
}

double MemInfoMB(char const* key) {
  std::ifstream in("/proc/meminfo");
  std::string name;
  double kb = 0;
  std::string unit;
  while (in >> name >> kb >> unit) {
    if (name == key) return kb / 1024.0;
  }
  return -1.0;
}

std::string Format(char const* format, double a, double b = 0.0) {
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), format, a, b);
  return buffer;
}

}  // namespace

std::string Property(char const* name) {
#if defined(__ANDROID__)
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get(name, value) > 0) return value;
#else
  (void)name;
#endif
  return {};
}

std::string DeviceLine() {
  std::string const model = Property("ro.product.model");
  if (model.empty()) return "unavailable";
  return Property("ro.product.manufacturer") + " " + model + "; OS " + Property("ro.build.display.id") +
         " (Android " + Property("ro.build.version.release") + ", SDK " + Property("ro.build.version.sdk") +
         ", build " + Property("ro.build.version.incremental") + ")";
}

std::string MemoryLine() {
  double const total = MemInfoMB("MemTotal:");
  double const available = MemInfoMB("MemAvailable:");
  if (total < 0) return "unavailable";
  return Format("total %.0f MB, available %.0f MB", total, available);
}

std::string BatteryLine() {
  double percent = -1;
  if (!ReadNumber("/sys/class/power_supply/battery/capacity", percent)) return "unavailable";
  double const temp = BatteryTempC();
  std::string line = Format("%.0f%%", percent);
  if (temp > -100) line += Format(", %.1f C", temp);
  std::string const status = ReadWord("/sys/class/power_supply/battery/status");
  if (!status.empty()) line += ", " + status;
  return line;
}

double BatteryTempC() {
  double tenths = 0;
  if (!ReadNumber("/sys/class/power_supply/battery/temp", tenths)) return -1000.0;
  return tenths / 10.0;
}

double HottestZoneC() {
  double hottest = -1000.0;
  std::error_code ec;
  for (auto const& entry : std::filesystem::directory_iterator("/sys/class/thermal", ec)) {
    if (entry.path().filename().string().rfind("thermal_zone", 0) != 0) continue;
    double raw = 0;
    if (!ReadNumber((entry.path() / "temp").string(), raw)) continue;
    double const celsius = ZoneCelsius(raw);
    if (celsius > -50 && celsius < 150 && celsius > hottest) hottest = celsius;
  }
  return hottest;
}

std::string ThermalLine() {
  double hottest = -1000.0;
  std::string zone;
  int readable = 0;
  std::error_code ec;
  for (auto const& entry : std::filesystem::directory_iterator("/sys/class/thermal", ec)) {
    if (entry.path().filename().string().rfind("thermal_zone", 0) != 0) continue;
    double raw = 0;
    if (!ReadNumber((entry.path() / "temp").string(), raw)) continue;
    double const celsius = ZoneCelsius(raw);
    if (celsius < -50 || celsius > 150) continue;
    readable++;
    if (celsius > hottest) {
      hottest = celsius;
      zone = ReadWord((entry.path() / "type").string());
    }
  }
  if (readable == 0) return "unavailable";
  return "hottest zone " + (zone.empty() ? std::string("?") : zone) + Format(" %.1f C of %.0f readable", hottest, readable);
}

double GpuClockMHz() {
  double hz = 0;
  if (!ReadNumber("/sys/class/kgsl/kgsl-3d0/gpuclk", hz)) return -1.0;
  return hz / 1e6;
}

std::string GpuClockLine() {
  double const mhz = GpuClockMHz();
  if (mhz < 0) return "unavailable";
  double maxHz = 0;
  if (ReadNumber("/sys/class/kgsl/kgsl-3d0/max_gpuclk", maxHz)) return Format("%.0f MHz (max %.0f MHz)", mhz, maxHz / 1e6);
  return Format("%.0f MHz", mhz);
}

double ResidentMB() {
  std::ifstream in("/proc/self/statm");
  double size = 0, resident = 0;
  if (!(in >> size >> resident)) return -1.0;
  return resident * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
}

double NativeHeapMB() {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
  return static_cast<double>(mallinfo2().uordblks) / (1024.0 * 1024.0);
#else
  return static_cast<double>(mallinfo().uordblks) / (1024.0 * 1024.0);
#endif
}

}  // namespace Vivify::DeviceInfo
