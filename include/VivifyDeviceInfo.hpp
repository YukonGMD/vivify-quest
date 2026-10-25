#pragma once

#include <string>

// Device facts for the session log (0.14.25-naomi.10), read from system
// properties, /proc and /sys. Unity-free. Every reader returns "unavailable"
// (or -1) rather than failing: what an app may read under /sys differs by
// headset build.
namespace Vivify::DeviceInfo {

// An Android system property ("" off Android or when unset). Never asked for
// the serial number.
std::string Property(char const* name);

// "Oculus Quest 3S; OS <display id> (Android 14, SDK 34, build <incremental>)"
std::string DeviceLine();
// "total 7612 MB, available 3020 MB"
std::string MemoryLine();
// "54%, 31.2 C, Discharging"
std::string BatteryLine();
// "hottest zone cpu-1-0 41.0 C of 37 readable"
std::string ThermalLine();
// "587 MHz (max 615 MHz)"
std::string GpuClockLine();

// For per-section samples: -1 when unknown.
double BatteryTempC();
double HottestZoneC();
double GpuClockMHz();
// This process's resident memory and native (malloc) heap in use.
double ResidentMB();
double NativeHeapMB();

}  // namespace Vivify::DeviceInfo
