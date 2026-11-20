#include "main.hpp"
#include "VivifyGlobalLog.hpp"
#include "VivifyPerfMode.hpp"
#include "VivifyWatchdog.hpp"
#include <ctime>
#include "VivifyRuntime.hpp"
#include "VivifyReport.hpp"
#include <string>
#include <string_view>
#include <fstream>
#include <chrono>
#include <mutex>
#include <filesystem>
#include "HMUI/ViewController.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "bsml/shared/BSML-Lite/Creation/Buttons.hpp"
#include "bsml/shared/BSML-Lite/Creation/Layout.hpp"
#include "bsml/shared/BSML-Lite/Creation/Settings.hpp"
#include "bsml/shared/BSML-Lite/Creation/Text.hpp"
#include "bsml/shared/BSML-Lite/Creation/Misc.hpp"
#include "bsml/shared/BSML/Components/Settings/ToggleSetting.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "VivifySettings.hpp"
#include "VivifyVersion.hpp"
#include "VivifyDeviceInfo.hpp"
#include "VivifyLogWriter.hpp"
#include "VivifySessionLog.hpp"
#include <atomic>
#include <cctype>
#include <limits>
#include <optional>
#include <sys/stat.h>
#include <map>
#include <set>
#include <sstream>
#include "UnityEngine/Application.hpp"
#include "UnityEngine/GL.hpp"
#include "UnityEngine/SystemInfo.hpp"
#include "GlobalNamespace/OVRPlugin.hpp"
#include <algorithm>
#include <array>
#include "beatsaber-hook/shared/utils/typedefs-wrappers.hpp"
#include "HMUI/CurvedTextMeshPro.hpp"
#include "bsml/shared/BSML/Settings/BSMLSettings.hpp"
#include "custom-types/shared/register.hpp"
#include "scotland2/shared/modloader.h"
#include "System/GC.hpp"
#include <dlfcn.h>

static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};

// Defined below, outside this namespace; the settings reset button re-applies it.
void EnsureConfigDefaults();

namespace {
constexpr std::string_view kVivifyDebugLoggingConfigKey = "vivifyDebugLogging";
constexpr std::string_view kDisableBeat0FilmgrainBlitConfigKey = "disableBeat0FilmgrainBlit";
constexpr std::string_view kDisableAllBlitsConfigKey = "disableAllBlits";
constexpr std::string_view kDisableCreateCameraDepthConfigKey = "disableCreateCameraDepth";
// Ported from the rbatteries1-design/Lars27110 base.
constexpr std::string_view kDisableCustomNoteVisualsConfigKey = "disableCustomNoteVisuals";
constexpr std::string_view kDisableVisualsInMultiplayerConfigKey = "disableVisualsInMultiplayer";
constexpr std::string_view kDisableVRCenterAdjustConfigKey = "disableVRCenterAdjust";
constexpr std::string_view kConvertPcBundlesOnDeviceConfigKey = "convertPcBundlesOnDevice";
constexpr std::string_view kStandInShadingConfigKey = "standInShading";
constexpr std::string_view kSceneDepthConfigKey = "sceneDepthTexture";
constexpr std::string_view kSubmitScoresConfigKey = "submitScoresOnVivifyMaps";
constexpr std::string_view kTranslateShadersConfigKey = "translateShadersOnConversion";
constexpr std::string_view kAudioLinkConfigKey = "audioLink";
constexpr std::string_view kMapRealtimeShadowsConfigKey = "mapRealtimeShadows";
constexpr std::string_view kReplayRenderModeConfigKey = "replayRenderMode";
constexpr std::string_view kBlitFixConfigKey = "blitScreenTextureFix";
// Renamed in 0.14.18 so every install starts with it off: 0.14.15 saved it
// as on, and it may be what froze converted maps in the menu.
constexpr std::string_view kPrepareShadersConfigKey = "prepareShadersInMenu";
constexpr std::string_view kGeometryEffectsConfigKey = "drawGeometryShaderEffects";
constexpr std::string_view kEmulateGeometryConfigKey = "emulateGeometryShaderEffects";
constexpr std::string_view kPoolReplacementsConfigKey = "poolNoteReplacements";
constexpr std::string_view kStandInShaderNameConfigKey = "standInShaderName";
constexpr std::string_view kEffectResolutionConfigKey = "effectResolutionPercent";
// A new key: the setting is now off by default, and the old key
// saved it on for everyone.
constexpr std::string_view kOffscreenCullingConfigKey = "offscreenCullingOptIn";
constexpr std::string_view kCrashGuardConfigKey = "crashGuard";
constexpr std::string_view kPcBundleForMissingShadersConfigKey = "pcBundleForMissingShaders";
constexpr std::string_view kFarCullingConfigKey = "farCullingDistance";
constexpr std::string_view kSlowFrameSamplerConfigKey = "slowFrameSampler";
constexpr std::string_view kParticleGeometryConfigKey = "emulateParticleGeometry";
constexpr std::string_view kPerformanceModeConfigKey = "performanceModeHeavyMaps";
constexpr std::string_view kPerfFloorConfigKey = "performanceModeFloorPercent";
constexpr std::string_view kPerfHalfRefreshConfigKey = "performanceModeHalfRefresh";
constexpr std::string_view kPerfDepthMapsConfigKey = "performanceModeScaleDepthMaps";
constexpr std::string_view kPerfLearnConfigKey = "performanceModeLearnedProfiles";
constexpr std::string_view kPerfFixedScaleConfigKey = "performanceModeFixedScalePercent";
constexpr std::string_view kDebugCostTestConfigKey = "debugCostTest";
constexpr std::string_view kShowDeveloperOptionsConfigKey = "showDeveloperOptions";
constexpr std::string_view kDetailedLoggingConfigKey = "detailedLogging";
constexpr std::string_view kScreenArraysConfigKey = "screenTexturesAsArrays";
constexpr std::string_view kUprightReadsConfigKey = "uprightScreenReads";
constexpr std::string_view kUprightNarrowConfigKey = "uprightReadsProjectionSignOnly";
bool gVivifyDebugLogging = false;
bool gDisableBeat0FilmgrainBlit = false;
bool gDisableAllBlits = false;
bool gDisableCreateCameraDepth = false;
bool gDisableCustomNoteVisuals = false;
// Defaults to true, matching the base this was ported from: Vivify's world-space
// note/saber/debris replacements aren't validated for multiplayer lobbies, so they
// stay off there unless the player opts back in.
bool gDisableVisualsInMultiplayer = true;
bool gDisableVRCenterAdjust = false;
// Replaces the old "allowUnsafeWindowsBundleFallback" toggle. That one handed a
// PC-built AssetBundle straight to Unity, which simply reports no assets on
// Android. This one instead retargets the archive to Android on device first
// (see VivifyBundleConvert), so the geometry/prefabs in it actually load.
// Defaults on: it only ever runs when a map has no Android bundle at all and no
// downloadable one, i.e. when the alternative is an unplayable map.
bool gConvertPcBundlesOnDevice = true;
// When a bundle's shader cannot run on this GPU, Vivify swaps in a generic
// stand-in so the mesh is at least visible. That trades "invisible" for
// "visible but wrong", and for a converted PC bundle "wrong" often means flat
// white. Turning this off leaves such meshes undrawn instead, which also means
// notes and sabers keep the game's own visuals rather than a white stand-in.
bool gStandInShading = true;
bool gSceneDepth = true;
// Vivify does not change note timing, scoring, or anything else a leaderboard
// cares about -- it changes how a map looks. Submission was nevertheless being
// turned off for every map carrying the Vivify requirement, and with it off
// BeatLeader and ScoreSaber record no replay, so Vivify maps had no replays to
// watch or render at all.
bool gSubmitScores = true;
// On-device conversion now cross-compiles a PC bundle's DirectX shader programs
// to GLSL ES (VivifyDxbc) rather than only retargeting the archive, so a
// converted map can render its own shading instead of a stand-in. A shader
// using anything outside the translated subset is left exactly as it was, so
// the worst case is the behaviour this replaces. Turning this off falls back to
// the retarget-only conversion, which is the escape hatch if a translated
// bundle turns out worse than an unshaded one -- no new build required, just
// reconvert.
bool gTranslateShaders = true;
bool gAudioLink = true;
bool gMapRealtimeShadows = false;
bool gReplayRenderMode = false;
bool gBlitFix = false;
bool gPrepareShaders = false;
bool gGeometryEffects = false;
bool gEmulateGeometry = true;
bool gPoolReplacements = true;
// Which shader to use as the stand-in, by name, overriding the automatic pick.
//
// Empty means "choose automatically". This exists because the right answer
// depends on what a particular Beat Saber build actually ships, and that list
// is only knowable from a headset: several builds shipped with a stand-in that
// turned out to render black on the device. The session log prints every
// runnable shader name, so a name from that list can be dropped in here and
// tried immediately rather than waiting for another build.
std::string gStandInShaderName;
// Percent of the eye resolution map post-process effects render at.
int gEffectResolution = 100;
// Animators in a map's prefabs skip updating while none of their renderers is
// on screen (0.14.23).
bool gOffscreenCulling = false;
// Two loads of a converted bundle that never finish put it into the
// untranslated (grey) fallback; off, that never happens (0.14.24).
bool gCrashGuard = true;
// A Quest bundle missing shaders its PC build has is played from the converted
// PC build instead of being patched (0.14.24).
bool gPcBundleForMissingShaders = false;
// Metres beyond which the main camera draws nothing; 0 is off (0.14.23).
int gFarCullingDistance = 0;
// Stack samples of the main and render threads during frames over 100 ms,
// summed up per section in the song's [PERF] summary (naomi.7).
bool gSlowFrameSampler = true;
// Static mesh particles on emulated geometry shaders baked into one mesh with
// the emulation's corner data, as PC draws them (naomi.8).
bool gParticleGeometry = true;
// Performance mode for heavy maps (naomi.8): a smaller viewport while a song
// runs over its frame budget, foveation HighTop and the GPU boost once it
// turns out heavy.
bool gPerformanceMode = false;
int gPerfFloor = 60;            // lowest viewport scale, percent
bool gPerfHalfRefresh = false;  // budget: two refreshes instead of one
bool gPerfDepthMaps = false;    // scale maps whose shaders read scene depth too
bool gPerfLearn = false;        // learned profiles per map and difficulty
int gPerfFixedScale = 0;        // debug: a fixed viewport scale, percent; 0 off
int gDebugCostTest = 0;         // debug: what is switched off for a cost test (Vivify::CostTest)
// Shows the Debug section of the menu (naomi.10).
bool gShowDeveloperOptions = false;
// Layer 3 of the session log: per-frame lines and more (naomi.10, debug).
bool gDetailedLogging = false;
// Converted multiview programs read screen textures as two-layer arrays (beta.2).
bool gScreenArrays = true;
// Converted shaders' screen reads made upright where PC relies on its flipped
// render targets (beta.4).
bool gUprightReads = true;
// ...flipping only _ProjectionParams.x signs (beta.5 Debug test, off).
bool gUprightNarrow = false;

// Where each setting in Vivify::Settings' table is held while the game runs.
struct BoundSetting {
  std::string_view key;
  bool* boolValue = nullptr;
  int* intValue = nullptr;
  std::string* stringValue = nullptr;
};
std::array<BoundSetting, 40> const kBoundSettings = {{
    {kVivifyDebugLoggingConfigKey, &gVivifyDebugLogging},
    {kDisableBeat0FilmgrainBlitConfigKey, &gDisableBeat0FilmgrainBlit},
    {kDisableAllBlitsConfigKey, &gDisableAllBlits},
    {kDisableCreateCameraDepthConfigKey, &gDisableCreateCameraDepth},
    {kDisableCustomNoteVisualsConfigKey, &gDisableCustomNoteVisuals},
    {kDisableVisualsInMultiplayerConfigKey, &gDisableVisualsInMultiplayer},
    {kDisableVRCenterAdjustConfigKey, &gDisableVRCenterAdjust},
    {kConvertPcBundlesOnDeviceConfigKey, &gConvertPcBundlesOnDevice},
    {kStandInShadingConfigKey, &gStandInShading},
    {kSceneDepthConfigKey, &gSceneDepth},
    {kSubmitScoresConfigKey, &gSubmitScores},
    {kTranslateShadersConfigKey, &gTranslateShaders},
    {kAudioLinkConfigKey, &gAudioLink},
    {kMapRealtimeShadowsConfigKey, &gMapRealtimeShadows},
    {kReplayRenderModeConfigKey, &gReplayRenderMode},
    {kBlitFixConfigKey, &gBlitFix},
    {kPrepareShadersConfigKey, &gPrepareShaders},
    {kGeometryEffectsConfigKey, &gGeometryEffects},
    {kEmulateGeometryConfigKey, &gEmulateGeometry},
    {kPoolReplacementsConfigKey, &gPoolReplacements},
    {kStandInShaderNameConfigKey, nullptr, nullptr, &gStandInShaderName},
    {kEffectResolutionConfigKey, nullptr, &gEffectResolution},
    {kOffscreenCullingConfigKey, &gOffscreenCulling},
    {kCrashGuardConfigKey, &gCrashGuard},
    {kPcBundleForMissingShadersConfigKey, &gPcBundleForMissingShaders},
    {kFarCullingConfigKey, nullptr, &gFarCullingDistance},
    {kSlowFrameSamplerConfigKey, &gSlowFrameSampler},
    {kParticleGeometryConfigKey, &gParticleGeometry},
    {kPerformanceModeConfigKey, &gPerformanceMode},
    {kPerfFloorConfigKey, nullptr, &gPerfFloor},
    {kPerfHalfRefreshConfigKey, &gPerfHalfRefresh},
    {kPerfDepthMapsConfigKey, &gPerfDepthMaps},
    {kPerfLearnConfigKey, &gPerfLearn},
    {kPerfFixedScaleConfigKey, nullptr, &gPerfFixedScale},
    {kDebugCostTestConfigKey, nullptr, &gDebugCostTest},
    {kShowDeveloperOptionsConfigKey, &gShowDeveloperOptions},
    {kDetailedLoggingConfigKey, &gDetailedLogging},
    {kScreenArraysConfigKey, &gScreenArrays},
    {kUprightReadsConfigKey, &gUprightReads},
    {kUprightNarrowConfigKey, &gUprightNarrow},
}};

// Both diagnostic files live beside the mod's own data, and both are .txt.
//
// This used to be Logs/Vivify.log. A .log file has no default handler on Android
// or Windows, so tapping it does nothing and it looks like no log exists at all
// -- and it sat in a different directory from the per-level report, so there
// were two places to look. One directory, two .txt files, both openable.
constexpr std::string_view kVivifyLogDir = "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify";
// naomi.9 and earlier wrote one session log here, replaced at every launch;
// it now only says where the saved sessions are.
constexpr std::string_view kVivifyLogPath =
    "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifySession.txt";

// Saved sessions (naomi.10, LOGGING.md): one file per launch in Sessions/,
// named by start time, build and commit, with index.txt, latest.txt and the
// black box beside them. The game's threads only queue lines; a thread of
// its own writes them (VivifyLogWriter).
constexpr std::string_view kSessionsDir = "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/Sessions";
constexpr uint32_t kBlackBoxRecords = 4096;  // 512 KB: about 30 s of frames plus everything else

Vivify::LogWriter gSessionLog;
Vivify::BlackBox gBlackBox;
std::string gSessionFile;
std::string gSessionStarted;
long gUtcOffsetSeconds = 0;  // taken at launch; lines use it rather than looking the zone up each time
std::atomic<float> gSongTime{std::numeric_limits<float>::quiet_NaN()};
std::atomic<uint64_t> gLayer1Ns{0}, gLayer2Ns{0}, gLayer3Ns{0};
std::mutex gSessionMutex;  // gSessionMaps, gLoadTimeline
std::string gSessionMaps;
Vivify::SessionLog::LoadTimeline gLoadTimeline;
bool gSessionEnded = false;

std::string WallClockText() {
  std::time_t const now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
  return buffer;
}
bool gVivifyLogSinkInstalled = false;
std::chrono::steady_clock::time_point gVivifyLogOpened = std::chrono::steady_clock::now();

// Adds the time from construction to destruction to a layer's total.
class CostTimer {
 public:
  explicit CostTimer(std::atomic<uint64_t>& total) : _total(total), _start(std::chrono::steady_clock::now()) {}
  ~CostTimer() {
    _total.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - _start)
                                               .count()),
                     std::memory_order_relaxed);
  }

 private:
  std::atomic<uint64_t>& _total;
  std::chrono::steady_clock::time_point _start;
};

std::string LinePrefix(std::string_view tag) {
  auto const now = std::chrono::system_clock::now();
  auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  std::time_t const local = static_cast<std::time_t>(ms / 1000) + gUtcOffsetSeconds;
  std::tm parts{};
  gmtime_r(&local, &parts);
  return Vivify::SessionLog::FormatPrefix(parts, static_cast<int>(ms % 1000), gSongTime.load(std::memory_order_relaxed),
                                          tag);
}

void PushLine(std::string_view tag, std::string text, bool important, bool limit) {
  gSessionLog.Push(LinePrefix(tag), std::move(text), important, limit);
}

void BlackBoxRecord(Vivify::BlackBox::Type type, std::string_view text, float a = 0.0f, float b = 0.0f,
                    float c = 0.0f) {
  CostTimer cost(gLayer2Ns);
  gBlackBox.Record(type, gSongTime.load(std::memory_order_relaxed), text, a, b, c);
}

// A line that names its own tag, "[PERF] ...", is written under that tag.
void WriteFlushedLogLine(std::string const& line) {
  CostTimer cost(gLayer1Ns);
  size_t const close = line.find("] ");
  if (!line.empty() && line[0] == '[' && close != std::string::npos && close < 16) {
    PushLine(std::string_view(line).substr(1, close - 1), line.substr(close + 2), true, false);
  } else {
    PushLine("INFO", line, true, false);
  }
}

// Logcat copies of breadcrumb and watchdog lines, under their own tags so the
// file sink (which takes only MOD_ID's lines) does not write them twice.
constexpr auto CrumbLogger = Paper::ConstLoggerContext("VivifyCrumb");
constexpr auto WatchdogLogger = Paper::ConstLoggerContext("VivifyWatchdog");

Vivify::Watchdog gWatchdog;

std::string ElapsedText() {
  auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                        gVivifyLogOpened)
                      .count();
  return "+" + std::to_string(ms) + "ms";
}

// latest.txt names this session's file; VivifySession.txt, where naomi.9 and
// earlier kept their one log, says where the logs went.
void WritePointers() {
  std::ofstream latest(std::string(kSessionsDir) + "/latest.txt", std::ios::trunc);
  latest << gSessionFile << "\n";
  std::ofstream old(std::string(kVivifyLogPath), std::ios::trunc);
  old << Vivify::SessionLog::kPointerText << "\n"
      << "This session: Sessions/" << gSessionFile << "\n";
}

// vivify_global.txt (every session appended since 0.14.23) is kept as it is
// and no longer written: each session has its own file now. Said once, in
// the file itself.
void RetireGlobalLog() {
  std::string const marker = std::string(kSessionsDir) + "/.global-log-retired";
  std::error_code ec;
  if (std::filesystem::exists(marker, ec)) return;
  Vivify::GlobalLog global(std::string(kVivifyLogDir), "vivify_global", 9ull * 1024 * 1024, 1024ull * 1024 * 1024);
  if (!global.Parts().empty()) {
    global.Open("=== " + WallClockText() + " " + VERSION + ": this file is kept as history and no longer written; "
                "each session now has its own file in Sessions/ (LOGGING.md) ===");
  }
  std::ofstream(marker) << VERSION << "\n";
}

void InstallVivifyFileLogSink() {
  if (gVivifyLogSinkInstalled) return;
  gVivifyLogSinkInstalled = true;
  std::error_code ec;
  std::filesystem::create_directories(std::string(kSessionsDir), ec);

  std::time_t const now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  gUtcOffsetSeconds = local.tm_gmtoff;
  gSessionFile = Vivify::SessionLog::SessionFileName(local, VERSION, VIVIFY_GIT_COMMIT);
  std::string const offset = Vivify::SessionLog::UtcOffset(local);
  gSessionStarted = WallClockText() + " " + offset;

  // What the previous session left, before anything of this one is written.
  Vivify::BlackBox::Previous previous;
  gBlackBox.Open(std::string(kSessionsDir) + "/blackbox.bin", gSessionFile, kBlackBoxRecords, previous);
  Vivify::SessionLog::Session current;
  current.file = gSessionFile;
  current.started = gSessionStarted;
  current.build = VERSION;
  Vivify::SessionLog::Startup startup;
  try {
    startup = Vivify::SessionLog::BeginSession(
        std::string(kSessionsDir), current,
        Vivify::SessionLog::PreviousBox{previous.found, previous.state, previous.session, previous.lines},
        std::string(kVivifyLogPath));
  } catch (std::exception const& ex) {
    startup.notes.push_back(std::string("the session index could not be updated: ") + ex.what());
  }

  std::string const path = std::string(kSessionsDir) + "/" + gSessionFile;
  std::vector<std::string> header = {
      "=== Vivify session log, schema " + std::to_string(Vivify::SessionLog::kSchemaVersion) + " ===",
      "build: " + std::string(VERSION) + " · " + std::string(Vivify::kCodename),
      "commit: " + std::string(VIVIFY_GIT_COMMIT),
      "started: " + gSessionStarted + " (local time, UTC" + offset + ")",
  };
  if (!gSessionLog.Start(path, header)) {
    PaperLogger.warn("Vivify: could not open the session log at {} (logging to logcat only)", path);
  }
  PushLine("HEADER", "converter: conversion version " + std::to_string(Vivify::BundleConversionVersion()), false, false);
  PushLine("HEADER", "device: " + Vivify::DeviceInfo::DeviceLine(), false, false);
  PushLine("HEADER", "memory: " + Vivify::DeviceInfo::MemoryLine(), false, false);
  PushLine("HEADER", "battery: " + Vivify::DeviceInfo::BatteryLine(), false, false);
  PushLine("HEADER", "thermal: " + Vivify::DeviceInfo::ThermalLine(), false, false);
  PushLine("HEADER", "GPU clock: " + Vivify::DeviceInfo::GpuClockLine(), false, false);
  PushLine("HEADER", "black box: " + std::string(gBlackBox.IsOpen() ? "on" : "could not be opened") + ", " +
                         std::to_string(kBlackBoxRecords) + " records",
           false, false);
  for (auto const& note : startup.notes) PushLine("SESSION", note, false, false);
  if (!startup.deleted.empty()) {
    PushLine("SESSION", "deleted " + std::to_string(startup.deleted.size()) +
                            " old session file(s) to stay within 10 recent, 5 unexpected and 96 MB",
             false, false);
  }
  BlackBoxRecord(Vivify::BlackBox::kSession, "session started");
  // A process that exits through exit() without Unity's quit callback still
  // gets its footer. Registered after gSessionLog and gBlackBox exist, so it
  // runs before they are destroyed.
  std::atexit([]() { VivifySessionEnd(); });
  gSessionLog.Post([]() {
    WritePointers();
    RetireGlobalLog();
  });

  // Started once the file is open, so its lines have somewhere to go.
  gWatchdog.Start([](std::string const& line) {
    {
      CostTimer cost(gLayer1Ns);
      PushLine("WATCHDOG", ElapsedText() + " " + line, true, false);
    }
    BlackBoxRecord(Vivify::BlackBox::kWarning, line);
    WatchdogLogger.warn("{}", line);
  });

  // Paper calls this on its own thread, so the game's threads pay nothing
  // for it; lines are stamped as they arrive (within a few ms of the call).
  Paper::Logger::AddLogSink([](Paper::LogData const& data) {
    if (!data.tag.has_value() || *data.tag != std::string_view(MOD_ID)) return;
    bool const important = data.level >= Paper::LogLevel::WRN;
    std::string_view message = data.message;
    std::string tag(Paper::format_as(data.level));
    // A message that starts "[ASSETS] " is written under that tag.
    size_t const close = message.find("] ");
    if (message.size() > 3 && message[0] == '[' && close != std::string_view::npos && close < 12 &&
        std::all_of(message.begin() + 1, message.begin() + static_cast<long>(close),
                    [](char c) { return std::isupper(static_cast<unsigned char>(c)) || c == '_'; })) {
      tag = std::string(message.substr(1, close - 1));
      message.remove_prefix(close + 2);
    }
    gSessionLog.Push(LinePrefix(tag), std::string(message), important, true);
    if (important) gBlackBox.Record(Vivify::BlackBox::kWarning, gSongTime.load(std::memory_order_relaxed), message);
  });
}

}  // namespace

namespace {

// il2cpp's heap-size exports, looked up once. beatsaber-hook's wrappers
// abort when a function is missing, and diagnostics must never do that.
using SizeFn = int64_t (*)();
struct GcFunctions {
  SizeFn usedSize = nullptr;
  SizeFn heapSize = nullptr;
};
GcFunctions const& Gc() {
  static GcFunctions const functions = [] {
    GcFunctions found;
    if (void* il2cpp = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD)) {
      found.usedSize = reinterpret_cast<SizeFn>(dlsym(il2cpp, "il2cpp_gc_get_used_size"));
      found.heapSize = reinterpret_cast<SizeFn>(dlsym(il2cpp, "il2cpp_gc_get_heap_size"));
    }
    return found;
  }();
  return functions;
}

// " | gc 0/1/2=a/b/c heap used=..MB of ..MB", so the log shows whether a
// garbage collection ran around a crash or stall. Main thread only (il2cpp is
// not called from anywhere else); empty elsewhere. il2cpp's Boehm collector is
// not generational, so the three counts are expected to move together.
std::string GcText() {
  if (!gWatchdog.OnMainThread()) return {};
  std::string text = " | gc 0/1/2=" + std::to_string(System::GC::CollectionCount(0)) + "/" +
                     std::to_string(System::GC::CollectionCount(1)) + "/" +
                     std::to_string(System::GC::CollectionCount(2));
  double used = 0, total = 0;
  if (VivifyHeapMB(used, total)) text += fmt::format(" heap used={:.1f}MB of {:.1f}MB", used, total);
  return text;
}

}  // namespace

void VivifyWriteFlushedLine(std::string const& line) {
  WriteFlushedLogLine(line);
}

int VivifyGcCount() {
  // cordl calls the compiled method directly: no invoke, no boxing.
  try {
    return System::GC::CollectionCount(0);
  } catch (...) {
    return -1;
  }
}

bool VivifyHeapMB(double& usedMB, double& totalMB) {
  auto const& gc = Gc();
  if (!gc.usedSize || !gc.heapSize) return false;
  usedMB = static_cast<double>(gc.usedSize()) / (1024.0 * 1024.0);
  totalMB = static_cast<double>(gc.heapSize()) / (1024.0 * 1024.0);
  return true;
}

void VivifyCopyBreadcrumb(char* buffer, size_t size) {
  gWatchdog.CopyBreadcrumb(buffer, size);
}

void VivifyBreadcrumb(std::string const& text) {
  gWatchdog.SetBreadcrumb(text);
  std::string const gc = GcText();
  std::optional<std::string> timeline;
  {
    CostTimer cost(gLayer1Ns);
    PushLine("CRUMB", ElapsedText() + " " + text + gc, true, false);
    std::lock_guard<std::mutex> lock(gSessionMutex);
    double const ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gVivifyLogOpened).count();
    timeline = gLoadTimeline.Feed(text, ms);
  }
  BlackBoxRecord(Vivify::BlackBox::kCrumb, text);
  if (timeline) VivifyLogLine("LOAD", *timeline, false, false);
  CrumbLogger.info("{}{}", text, gc);
}

void VivifyHeartbeat() {
  gWatchdog.Beat();
}

void VivifySetAppPaused(bool paused) {
  gWatchdog.SetPaused(paused);
  // The headset usually closes an app while it is in the background, without
  // a quit: the black box's state tells that apart from a crash (naomi.10).
  gBlackBox.SetState(paused ? Vivify::BlackBox::kPaused : Vivify::BlackBox::kRunning);
  BlackBoxRecord(Vivify::BlackBox::kSession, paused ? "app paused" : "app resumed");
  PushLine("SESSION", paused ? "=== app paused at " + WallClockText() + " ===" : std::string("=== app resumed ==="),
           true, false);
  if (paused) {
    gBlackBox.Sync();
    std::string maps;
    {
      std::lock_guard<std::mutex> lock(gSessionMutex);
      maps = gSessionMaps;
    }
    gSessionLog.Post([maps]() {
      Vivify::SessionLog::UpdateSession(std::string(kSessionsDir), gSessionFile, maps, {}, gSessionLog.BytesWritten());
    });
  }
}

namespace {

// GL, from the system library Unity's context uses; looked up once.
struct GlFunctions {
  using GetString = unsigned char const* (*)(unsigned int);
  using GetStringi = unsigned char const* (*)(unsigned int, unsigned int);
  using GetIntegerv = void (*)(unsigned int, int*);
  using GetError = unsigned int (*)();
  GetString getString = nullptr;
  GetStringi getStringi = nullptr;
  GetIntegerv getIntegerv = nullptr;
  GetError getError = nullptr;
};

GlFunctions const& Gl() {
  static GlFunctions const functions = [] {
    GlFunctions found;
    void* gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_NOLOAD);
    if (gles == nullptr) gles = dlopen("libGLESv2.so", RTLD_NOW);
    if (gles != nullptr) {
      found.getString = reinterpret_cast<GlFunctions::GetString>(dlsym(gles, "glGetString"));
      found.getStringi = reinterpret_cast<GlFunctions::GetStringi>(dlsym(gles, "glGetStringi"));
      found.getIntegerv = reinterpret_cast<GlFunctions::GetIntegerv>(dlsym(gles, "glGetIntegerv"));
      found.getError = reinterpret_cast<GlFunctions::GetError>(dlsym(gles, "glGetError"));
    }
    return found;
  }();
  return functions;
}

// The extensions a Vivify problem tends to turn on.
constexpr std::string_view kImportantExtensions[] = {
    "GL_OVR_multiview2", "GL_OVR_multiview_multisampled_render_to_texture", "GL_EXT_multisampled_render_to_texture",
    "GL_EXT_geometry_shader", "GL_EXT_tessellation_shader", "GL_EXT_shader_framebuffer_fetch",
    "GL_QCOM_shader_framebuffer_fetch_noncoherent", "GL_EXT_color_buffer_float", "GL_EXT_color_buffer_half_float",
    "GL_EXT_texture_filter_anisotropic", "GL_KHR_texture_compression_astc_ldr", "GL_KHR_texture_compression_astc_hdr",
    "GL_EXT_texture_compression_s3tc", "GL_EXT_texture_compression_bptc", "GL_EXT_disjoint_timer_query",
    "GL_QCOM_tiled_rendering", "GL_EXT_clip_cull_distance", "GL_OES_texture_float_linear",
    "GL_EXT_texture_cube_map_array", "GL_QCOM_texture_foveated", "GL_EXT_buffer_storage"};

constexpr int kRenderEventGlInfo = 0x56490001;
constexpr int kRenderEventGlErrors = 0x56490002;

char const* GlErrorName(unsigned int error) {
  switch (error) {
    case 0x0500: return "GL_INVALID_ENUM";
    case 0x0501: return "GL_INVALID_VALUE";
    case 0x0502: return "GL_INVALID_OPERATION";
    case 0x0505: return "GL_OUT_OF_MEMORY";
    case 0x0506: return "GL_INVALID_FRAMEBUFFER_OPERATION";
    default: return "?";
  }
}

// Runs on Unity's render thread, with its GL context current, through
// GL.IssuePluginEvent -- the way native plugins reach GL without a hook.
void OnRenderThreadEvent(int eventId) {
  try {
    auto const& gl = Gl();
    if (eventId == kRenderEventGlInfo && gl.getString != nullptr) {
      auto text = [&gl](unsigned int name) {
        auto const* value = gl.getString(name);
        return value != nullptr ? std::string(reinterpret_cast<char const*>(value)) : std::string("?");
      };
      VivifyLogLine("HEADER", "GL: " + text(0x1F00) + " / " + text(0x1F01) + " / " + text(0x1F02) + " / " + text(0x8B8C),
                    false, false);
      std::set<std::string, std::less<>> present;
      int count = 0;
      if (gl.getIntegerv != nullptr && gl.getStringi != nullptr) {
        gl.getIntegerv(0x821D, &count);  // GL_NUM_EXTENSIONS
        for (int i = 0; i < count && i < 1024; i++) {
          if (auto const* name = gl.getStringi(0x1F03, static_cast<unsigned int>(i))) {
            present.emplace(reinterpret_cast<char const*>(name));
          }
        }
      }
      std::string have, lack;
      for (auto name : kImportantExtensions) {
        std::string& list = present.contains(name) ? have : lack;
        list += (list.empty() ? "" : " ") + std::string(name);
      }
      VivifyLogLine("HEADER", "GL extensions: " + std::to_string(count) + " in all; present: " +
                                  (have.empty() ? "none" : have) + "; missing: " + (lack.empty() ? "none" : lack),
                    false, false);
      // Drained once here, so the first check in a song reports only its own.
      if (gl.getError != nullptr) {
        for (int i = 0; i < 16 && gl.getError() != 0; i++) {
        }
      }
    } else if (eventId == kRenderEventGlErrors && gl.getError != nullptr) {
      for (int i = 0; i < 8; i++) {
        unsigned int const error = gl.getError();
        if (error == 0) break;
        char text[96];
        std::snprintf(text, sizeof(text), "error 0x%04x (%s) since the last check", error, GlErrorName(error));
        VivifyLogLine("GL", text, true, true);
        VivifyBlackBoxRecord(Vivify::BlackBox::kGl, text);
      }
    }
  } catch (...) {
  }
}

void IssueRenderEvent(int eventId) {
  try {
    UnityEngine::GL::IssuePluginEvent(System::IntPtr(reinterpret_cast<void*>(&OnRenderThreadEvent)), eventId);
  } catch (...) {
  }
}

std::string Text(StringW value) {
  return value ? static_cast<std::string>(value) : std::string();
}

std::string ModTime(char const* path) {
  struct stat info {};
  if (path == nullptr || ::stat(path, &info) != 0) return "?";
  std::tm local{};
  localtime_r(&info.st_mtime, &local);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
  return buffer;
}

}  // namespace

// Once, on the first frame: what needs Unity running (naomi.10).
void VivifySessionHeaderLate() {
  static bool done = false;
  if (done) return;
  done = true;
  std::map<std::string, std::string> summary;
  summary["build"] = VERSION;
  summary["commit"] = VIVIFY_GIT_COMMIT;
  summary["os"] = Vivify::DeviceInfo::Property("ro.build.display.id");
  try {
    std::string const game = Text(UnityEngine::Application::get_version());
    std::string const unity = Text(UnityEngine::Application::get_unityVersion());
    summary["beatsaber"] = game;
    PushLine("HEADER", "Beat Saber " + game + ", Unity " + unity, false, false);
    PushLine("HEADER",
             "SystemInfo: " + Text(UnityEngine::SystemInfo::get_deviceModel()) + "; " +
                 Text(UnityEngine::SystemInfo::get_operatingSystem()) + "; GPU " +
                 Text(UnityEngine::SystemInfo::get_graphicsDeviceName()) + " (" +
                 Text(UnityEngine::SystemInfo::get_graphicsDeviceVendor()) + "), " +
                 Text(UnityEngine::SystemInfo::get_graphicsDeviceVersion()) + ", " +
                 std::to_string(UnityEngine::SystemInfo::get_graphicsMemorySize()) + " MB graphics memory; " +
                 std::to_string(UnityEngine::SystemInfo::get_systemMemorySize()) + " MB system memory; " +
                 std::to_string(UnityEngine::SystemInfo::get_processorCount()) + " cores",
             false, false);
  } catch (std::exception const& ex) {
    PushLine("HEADER", std::string("Unity details could not be read: ") + ex.what(), false, false);
  }
  try {
    PushLine("HEADER", fmt::format("refresh rate: {:.0f} Hz", GlobalNamespace::OVRPlugin::get_systemDisplayFrequency()),
             false, false);
  } catch (...) {
    PushLine("HEADER", "refresh rate: unknown", false, false);
  }
  CModResults results = modloader_get_loaded();
  PushLine("HEADER", "mods loaded: " + std::to_string(results.size), false, false);
  for (size_t i = 0; i < results.size; i++) {
    auto const& mod = results.array[i];
    if (mod.info.id == nullptr) continue;
    std::string const version = mod.info.version != nullptr ? mod.info.version : "?";
    summary[std::string("mod:") + mod.info.id] = version;
    PushLine("HEADER", std::string("mod: ") + mod.info.id + " " + version + ", file from " + ModTime(mod.path), false,
             false);
  }
  modloader_free_results(&results);
  {
    auto const& doc = getConfig().config;
    for (auto const& def : Vivify::Settings::All()) {
      std::string value;
      switch (def.kind) {
        case Vivify::Settings::Kind::Bool: value = Vivify::Settings::GetBool(doc, def.key) ? "true" : "false"; break;
        case Vivify::Settings::Kind::Int: value = std::to_string(Vivify::Settings::GetInt(doc, def.key)); break;
        case Vivify::Settings::Kind::String: value = Vivify::Settings::GetString(doc, def.key); break;
      }
      summary["setting:" + std::string(def.key)] = value;
    }
  }
  // What changed since the last session, from the summary it left.
  std::string const summaryPath = std::string(kSessionsDir) + "/last-summary.txt";
  gSessionLog.Post([summary, summaryPath]() {
    std::string before;
    {
      std::ifstream in(summaryPath, std::ios::binary);
      std::stringstream text;
      text << in.rdbuf();
      before = text.str();
    }
    std::string const changed = Vivify::SessionLog::WhatChanged(Vivify::SessionLog::ParseSummary(before), summary);
    VivifyLogLine("HEADER", "changed since the last session: " + (changed.empty() ? std::string("(first session with a summary)") : changed),
                  false, false);
    std::ofstream out(summaryPath, std::ios::binary | std::ios::trunc);
    out << Vivify::SessionLog::FormatSummary(summary);
  });
  IssueRenderEvent(kRenderEventGlInfo);
}

// About once a second during a song: GL errors since the last check.
void VivifyCheckGlErrors() {
  IssueRenderEvent(kRenderEventGlErrors);
}

void VivifySetSongTime(float songTime) {
  gSongTime.store(songTime, std::memory_order_relaxed);
}

void VivifyLogLine(std::string_view tag, std::string text, bool important, bool limit) {
  CostTimer cost(gLayer1Ns);
  PushLine(tag, std::move(text), important, limit);
}

void VivifyBlackBoxRecord(Vivify::BlackBox::Type type, std::string_view text, float a, float b, float c) {
  BlackBoxRecord(type, text, a, b, c);
}

void VivifySessionMap(std::string const& key) {
  std::string maps;
  {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    std::string const listed = "," + gSessionMaps + ",";
    if (listed.find("," + key + ",") == std::string::npos) {
      gSessionMaps += (gSessionMaps.empty() ? "" : ",") + key;
    }
    maps = gSessionMaps;
  }
  gSessionLog.Post([maps]() {
    Vivify::SessionLog::UpdateSession(std::string(kSessionsDir), gSessionFile, maps, {}, gSessionLog.BytesWritten());
  });
}

namespace {
std::atomic<float> gSampleResident{-1}, gSampleNative{-1}, gSampleHottest{-1000}, gSampleBattery{-1000},
    gSampleGpu{-1};
std::atomic<bool> gSamplePending{false};
}  // namespace

VivifyDeviceSample VivifyLatestDeviceSample() {
  VivifyDeviceSample sample;
  sample.residentMB = gSampleResident.load();
  sample.nativeHeapMB = gSampleNative.load();
  sample.hottestC = gSampleHottest.load();
  sample.batteryC = gSampleBattery.load();
  sample.gpuMHz = gSampleGpu.load();
  if (!gSamplePending.exchange(true)) {
    gSessionLog.Post([]() {
      gSampleResident = static_cast<float>(Vivify::DeviceInfo::ResidentMB());
      gSampleNative = static_cast<float>(Vivify::DeviceInfo::NativeHeapMB());
      gSampleHottest = static_cast<float>(Vivify::DeviceInfo::HottestZoneC());
      gSampleBattery = static_cast<float>(Vivify::DeviceInfo::BatteryTempC());
      gSampleGpu = static_cast<float>(Vivify::DeviceInfo::GpuClockMHz());
      gSamplePending = false;
    });
  }
  return sample;
}

VivifyLogCost VivifyLoggingCost() {
  return VivifyLogCost{gLayer1Ns.load(), gLayer2Ns.load(), gLayer3Ns.load()};
}

void VivifyAddLayer3Cost(uint64_t ns) {
  gLayer3Ns.fetch_add(ns, std::memory_order_relaxed);
}

void VivifySessionEnd() {
  if (gSessionEnded) return;
  gSessionEnded = true;
  BlackBoxRecord(Vivify::BlackBox::kSession, "quit");
  PushLine("SESSION", "quit through the game", true, false);
  auto const cost = VivifyLoggingCost();
  char footer[256];
  std::snprintf(footer, sizeof(footer),
                "%s cleanly at %s: %llu lines written, %llu left out by the rate limit; logging cost the game's "
                "threads %.1f ms (session log), %.1f ms (black box), %.1f ms (detailed mode) ===",
                std::string(Vivify::SessionLog::kFooter).c_str(), WallClockText().c_str(),
                static_cast<unsigned long long>(gSessionLog.LinesWritten()),
                static_cast<unsigned long long>(gSessionLog.LinesSuppressed()), cost.layer1Ns / 1e6,
                cost.layer2Ns / 1e6, cost.layer3Ns / 1e6);
  gSessionLog.Stop({footer});
  gBlackBox.SetState(Vivify::BlackBox::kClean);
  gBlackBox.Sync();
  std::string maps;
  {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    maps = gSessionMaps;
  }
  std::error_code ec;
  auto const bytes = std::filesystem::file_size(std::string(kSessionsDir) + "/" + gSessionFile, ec);
  Vivify::SessionLog::UpdateSession(std::string(kSessionsDir), gSessionFile, maps, Vivify::SessionLog::kClean,
                                    ec ? 0 : static_cast<uint64_t>(bytes));
}

void VivifySlowFrameSection(int section) {
  gWatchdog.SetSlowFrameSection(section);
}

void VivifySlowFrameReset() {
  gWatchdog.ResetSlowFrames();
}

std::vector<std::string> VivifySlowFrameSummary(std::function<std::string(int)> const& label) {
  return gWatchdog.TakeSlowFrameSummary(label);
}

namespace {

void EnsureConfigObject() {
  auto& doc = getConfig().config;
  if (!doc.IsObject()) {
    doc.SetObject();
  }
}

// Copies every setting from the config document into the variable it lives
// in. The document has been through Vivify::Settings::Load, so every value is
// present and valid.
void ApplyConfigValues() {
  auto const& doc = getConfig().config;
  for (auto const& bound : kBoundSettings) {
    if (bound.boolValue) *bound.boolValue = Vivify::Settings::GetBool(doc, bound.key);
    if (bound.intValue) *bound.intValue = Vivify::Settings::GetInt(doc, bound.key);
    if (bound.stringValue) *bound.stringValue = Vivify::Settings::GetString(doc, bound.key);
  }
  for (auto const& def : Vivify::Settings::All()) {
    bool const bound = std::any_of(kBoundSettings.begin(), kBoundSettings.end(),
                                   [&](BoundSetting const& b) { return b.key == def.key; });
    if (!bound) PaperLogger.error("Vivify settings: '{}' is in the settings table but not bound", def.key);
  }
}

// True only while the settings view controller is being constructed.
//
// BSML toggles are live the moment they exist, and a toggle that fires its
// change callback while it is still being set up writes that transient value
// straight through to the config file -- which is how "Stand-In Shading" went
// from on to off mid-session without anybody touching it, and with it every
// converted map's geometry stopped being repaired. A player cannot tap a
// control that is not on screen yet, so any change arriving in this window is
// construction noise and is dropped.
bool gSettingsMenuBuilding = false;

void SetBoolConfigValue(std::string_view key, bool enabled, bool& value) {
  if (gSettingsMenuBuilding) {
    PaperLogger.info("Vivify settings: ignoring a '{}' change to {} that arrived while the menu was "
                     "still being built",
                     key, enabled ? "on" : "off");
    return;
  }
  auto& config = getConfig();
  auto& doc = config.config;
  EnsureConfigObject();
  auto& allocator = doc.GetAllocator();
  auto it = doc.FindMember(key.data());
  if (it == doc.MemberEnd()) {
    doc.AddMember(rapidjson::Value(key.data(), allocator), rapidjson::Value(enabled), allocator);
  } else {
    it->value.SetBool(enabled);
  }
  value = enabled;
  config.Write();
}

void SetIntConfigValue(std::string_view key, int newValue, int& value) {
  if (gSettingsMenuBuilding) {
    PaperLogger.info("Vivify settings: ignoring a '{}' change to {} that arrived while the menu was "
                     "still being built",
                     key, newValue);
    return;
  }
  auto& config = getConfig();
  auto& doc = config.config;
  EnsureConfigObject();
  auto& allocator = doc.GetAllocator();
  auto it = doc.FindMember(key.data());
  if (it == doc.MemberEnd()) {
    doc.AddMember(rapidjson::Value(key.data(), allocator), rapidjson::Value(newValue), allocator);
  } else {
    it->value.SetInt(newValue);
  }
  value = newValue;
  config.Write();
}

// Label under the bulk-convert button. The settings view controller is
// destroyed and rebuilt as the player navigates and conversion progress
// arrives asynchronously, so SafePtrUnity is used for its destroyed-object
// aware liveness check -- a plain SafePtr is not even permitted for Unity
// types.
SafePtrUnity<HMUI::CurvedTextMeshPro> gConvertStatusText;
// The same text beside the buttons in Advanced, which are a screen further
// down (naomi.10).
SafePtrUnity<HMUI::CurvedTextMeshPro> gActionStatusText;

void SetConvertStatusText(std::string const& text) {
  if (gConvertStatusText) gConvertStatusText->set_text(StringW(text));
  if (gActionStatusText) gActionStatusText->set_text(StringW(text));
}

// The Debug section's controls, shown only with "Show Developer Options"
// (naomi.10), and the line that says when a hidden one is not at its default.
std::vector<SafePtrUnity<UnityEngine::GameObject>> gDebugObjects;
SafePtrUnity<HMUI::CurvedTextMeshPro> gDeveloperNote;

void ShowDebugSection(bool show) {
  for (auto& object : gDebugObjects) {
    if (object) object->SetActive(show);
  }
  if (!gDeveloperNote) return;
  std::string const changed =
      Vivify::Settings::NonDefault(getConfig().config, Vivify::Settings::Section::Debug);
  gDeveloperNote->set_text(StringW("<size=70%>Developer options not at their defaults: " + changed + "</size>"));
  gDeveloperNote->get_gameObject()->SetActive(!show && !changed.empty());
}

StringW SettingLabel(std::string_view key) {
  auto const* def = Vivify::Settings::Find(key);
  return StringW(std::string(def != nullptr ? def->label : key));
}

UnityEngine::GameObject* WithHint(UnityEngine::GameObject* object, std::string_view hint) {
  if (object != nullptr && !hint.empty()) BSML::Lite::AddHoverHint(object, StringW(std::string(hint)));
  return object;
}

UnityEngine::GameObject* WithSettingHint(UnityEngine::GameObject* object, std::string_view key) {
  auto const* def = Vivify::Settings::Find(key);
  return WithHint(object, def != nullptr ? def->hint : std::string_view());
}

// A toggle for one on/off setting from the table, named and hinted from it.
UnityEngine::GameObject* AddToggle(UnityEngine::Transform* parent, std::string_view key, bool& value,
                                   std::function<void(bool)> after = nullptr) {
  auto* toggle = BSML::Lite::CreateToggle(parent, SettingLabel(key), value, [key, &value, after](bool enabled) {
    SetBoolConfigValue(key, enabled, value);
    if (after) after(enabled);
  });
  return toggle != nullptr ? WithSettingHint(toggle->get_gameObject(), key) : nullptr;
}

// An increment for one number setting from the table, with its range and step.
UnityEngine::GameObject* AddIncrement(UnityEngine::Transform* parent, std::string_view key, int& value) {
  auto const* def = Vivify::Settings::Find(key);
  if (def == nullptr) return nullptr;
  auto* setting = BSML::Lite::CreateIncrementSetting(
      parent, SettingLabel(key), 0, static_cast<float>(def->step), static_cast<float>(value), true, true,
      static_cast<float>(def->minValue), static_cast<float>(def->maxValue), [key, def, &value](float chosen) {
        SetIntConfigValue(key, std::clamp(static_cast<int>(chosen + 0.5f), def->minValue, def->maxValue), value);
      });
  return setting != nullptr ? WithSettingHint(setting->get_gameObject(), key) : nullptr;
}

UnityEngine::GameObject* AddButton(UnityEngine::Transform* parent, std::u16string_view text, std::string_view hint,
                                   std::function<void()> onClick) {
  auto* button = BSML::Lite::CreateUIButton(parent, StringW(text), onClick);
  return button != nullptr ? WithHint(button->get_gameObject(), hint) : nullptr;
}

UnityEngine::GameObject* AddHeading(UnityEngine::Transform* parent, std::string_view text) {
  auto* heading = BSML::Lite::CreateText(parent, StringW("<b>" + std::string(text) + "</b>"));
  return heading != nullptr ? heading->get_gameObject() : nullptr;
}

// A button that does nothing until it is pressed twice within five seconds,
// for actions that cannot be undone.
std::function<void()> PressTwice(std::string confirm, std::function<void()> action) {
  auto armedUntil = std::make_shared<std::chrono::steady_clock::time_point>();
  return [armedUntil, confirm = std::move(confirm), action = std::move(action)]() {
    auto const now = std::chrono::steady_clock::now();
    if (now > *armedUntil) {
      *armedUntil = now + std::chrono::seconds(5);
      SetConvertStatusText(confirm);
      return;
    }
    *armedUntil = {};
    action();
  };
}

void RegisterModSettings() {
  BSML::BSMLSettings::get_instance()->TryAddSettingsMenu(
      [](HMUI::ViewController* viewController, bool firstActivation, bool, bool) {
        if (!firstActivation || viewController == nullptr) return;
        gSettingsMenuBuilding = true;
        // Cleared however this scope is left, including through the catch below.
        struct BuildGuard {
          ~BuildGuard() { gSettingsMenuBuilding = false; }
        } buildGuard;

        // The whole menu is built inside a try/catch because it is built inside
        // a callback the game invokes: anything that throws here abandons the
        // rest of the construction and leaves the settings tab wedged, with the
        // game still running around it. That is exactly what the version label
        // added in 0.9.2 did -- it was the first widget in the list, so when it
        // threw, every control after it simply never existed and the menu could
        // not be used at all.
        //
        // A failure now costs the widgets after it and says so in the log,
        // instead of costing the menu.
        try {
        auto* container = BSML::Lite::CreateScrollableSettingsContainer(viewController->get_transform());
        if (container == nullptr) return;
        UnityEngine::Transform* const parent = container->get_transform().ptr();
        gDebugObjects.clear();

        // Which build is actually running, in the headset, without a file.
        // "the new features do not work" and "the new build did not install"
        // look identical from the outside, and a version number here separates
        // them in one glance.
        //
        // Built as a StringW from a std::string, the way every other text in
        // this menu is: the std::u16string this used to assemble by hand is
        // what took the menu down.
        BSML::Lite::CreateText(parent, StringW(std::string("Vivify ") + VERSION + " \u00b7 " +
                                               std::string(Vivify::kCodename)));

        // Everyday (naomi.10): what a player changes from one session to the
        // next, on one screen without scrolling. Everything else is under
        // Advanced, and test switches under Debug; see SETTINGS.md.
        AddToggle(parent, kPerformanceModeConfigKey, gPerformanceMode);
        AddIncrement(parent, kEffectResolutionConfigKey, gEffectResolution);
        AddToggle(parent, kDisableCustomNoteVisualsConfigKey, gDisableCustomNoteVisuals);
        AddToggle(parent, kSubmitScoresConfigKey, gSubmitScores);

        // A map whose only asset bundle is a PC build has its play button
        // disabled, so it can never be selected into -- which also means the
        // per-level conversion that runs on level select can never fire for it.
        // This button converts every installed map in one pass instead, so
        // those levels become playable without having to be playable first.
        //
        // One progress callback for both conversion buttons; they differ only
        // in whether an already-cached conversion is reused or thrown away
        // first.
        static auto const startConversion = [](bool force) {
          if (Vivify::IsBulkPcBundleConversionRunning()) return;
          SetConvertStatusText(force ? "Reconverting..." : "Scanning...");
          Vivify::StartBulkPcBundleConversion(
              [](Vivify::BulkConversionProgress const& progress) {
                if (progress.finished) {
                  SetConvertStatusText(progress.status);
                  return;
                }
                SetConvertStatusText(std::to_string(progress.levelsScanned) + "/" +
                                     std::to_string(progress.levelsTotal) + "  " + progress.status);
              },
              force);
        };
        AddButton(parent, u"Convert All PC Bundles Now",
                  "Converts every map that only has a PC bundle, so it can be played; converted maps are skipped.",
                  []() { startConversion(false); });
        gConvertStatusText = BSML::Lite::CreateText(parent, u"Idle");

        AddHeading(parent, "Advanced");
        AddHeading(parent, "<size=80%>Performance Mode</size>");
        AddIncrement(parent, kPerfFloorConfigKey, gPerfFloor);
        AddToggle(parent, kPerfDepthMapsConfigKey, gPerfDepthMaps);
        AddToggle(parent, kPerfLearnConfigKey, gPerfLearn);
        AddButton(parent, u"Forget Learned Maps", "Deletes what Learn Each Map has remembered about every map.", []() {
          std::error_code ec;
          auto const removed = std::filesystem::remove_all(Vivify::PerfProfileDir(), ec);
          SetConvertStatusText(ec ? "Could not remove the learned profiles"
                                  : "Forgot " + std::to_string(removed > 0 ? removed - 1 : 0) + " learned profile(s)");
        });
        AddToggle(parent, kPerfHalfRefreshConfigKey, gPerfHalfRefresh);

        AddHeading(parent, "<size=80%>How maps look</size>");
        AddToggle(parent, kAudioLinkConfigKey, gAudioLink);
        AddToggle(parent, kStandInShadingConfigKey, gStandInShading);
        AddToggle(parent, kSceneDepthConfigKey, gSceneDepth);
        AddToggle(parent, kEmulateGeometryConfigKey, gEmulateGeometry);
        AddToggle(parent, kParticleGeometryConfigKey, gParticleGeometry);
        AddToggle(parent, kGeometryEffectsConfigKey, gGeometryEffects);
        AddToggle(parent, kBlitFixConfigKey, gBlitFix);
        AddToggle(parent, kMapRealtimeShadowsConfigKey, gMapRealtimeShadows);
        AddToggle(parent, kDisableAllBlitsConfigKey, gDisableAllBlits);
        AddToggle(parent, kOffscreenCullingConfigKey, gOffscreenCulling);
        AddIncrement(parent, kFarCullingConfigKey, gFarCullingDistance);
        AddToggle(parent, kDisableVisualsInMultiplayerConfigKey, gDisableVisualsInMultiplayer);

        AddHeading(parent, "<size=80%>PC maps</size>");
        AddToggle(parent, kConvertPcBundlesOnDeviceConfigKey, gConvertPcBundlesOnDevice);
        AddToggle(parent, kPrepareShadersConfigKey, gPrepareShaders);
        AddToggle(parent, kPcBundleForMissingShadersConfigKey, gPcBundleForMissingShaders);
        AddToggle(parent, kReplayRenderModeConfigKey, gReplayRenderMode);
        AddToggle(parent, kCrashGuardConfigKey, gCrashGuard);
        // A cached conversion is keyed on the source bundle, so it is reused
        // even after the converter itself has been fixed. This is the way to
        // pick those fixes up without deleting the cache directory by hand.
        // It also clears every crash-guard strike.
        AddButton(parent, u"Force Reconvert All (ignore cache)",
                  "Converts every PC map again, ignoring what is cached, and clears crash-guard strikes.",
                  PressTwice("Press Force Reconvert again within 5 s: reconverts every PC map and clears "
                             "crash-guard strikes",
                             []() { startConversion(true); }));

        AddHeading(parent, "<size=80%>Other</size>");
        AddToggle(parent, kPoolReplacementsConfigKey, gPoolReplacements);
        gActionStatusText = BSML::Lite::CreateText(parent, u"");
        AddButton(parent, u"Reset All Vivify Settings To Defaults",
                  "Every Vivify setting but logging back to its default. Maps and converted files are kept.",
                  PressTwice("Press Reset Settings again within 5 s: every Vivify setting but logging goes back to "
                             "its default",
                             []() {
                               auto& config = getConfig();
                               Vivify::Settings::ResetToDefaults(config.config);
                               config.Write();
                               ApplyConfigValues();
                               PaperLogger.info("Vivify settings: all but the logging settings reset to their defaults from the menu");
                               SetConvertStatusText("Settings reset to defaults. Reopen this menu to see them.");
                             }));
        // Puts Vivify back to a fresh install's state, without touching the
        // maps: every converted bundle and crash-guard note, the PC bundles it
        // downloaded into Quest maps' folders, and its settings.
        AddButton(parent, u"Reset Vivify (clean slate)",
                  "Deletes converted maps and downloaded PC bundles and resets settings. Maps are kept.",
                  PressTwice("Press Reset again within 5 s: deletes converted maps and downloaded PC bundles, "
                             "resets settings",
                             []() {
                               if (Vivify::IsBulkPcBundleConversionRunning()) {
                                 SetConvertStatusText("A conversion pass is running; reset after it finishes");
                                 return;
                               }
                               auto& config = getConfig();
                               Vivify::Settings::ResetToDefaults(config.config);
                               config.Write();
                               ApplyConfigValues();
                               SetConvertStatusText("Resetting...");
                               Vivify::ResetToCleanSlate([](std::string const& summary) {
                                 SetConvertStatusText(summary + ". Reopen this menu to see the settings.");
                               });
                             }));
        // Lists every Vivify map in VivifyMaps.txt, then replaces each with a
        // fresh BeatSaver download, for maps whose folders were changed by an
        // earlier version.
        AddButton(parent, u"Redownload All Vivify Maps",
                  "Lists Vivify maps in VivifyMaps.txt, then replaces each with a fresh BeatSaver download.",
                  PressTwice("Press Redownload again within 5 s: lists Vivify maps in VivifyMaps.txt, then "
                             "replaces each with a fresh BeatSaver download",
                             []() {
                               SetConvertStatusText("Finding Vivify maps...");
                               Vivify::RedownloadVivifyMaps(
                                   [](std::string const& text) { SetConvertStatusText(text); });
                             }));

        // paperlog output is not reachable without adb, so Vivify writes its own
        // plain-text report next to its data. Showing the path here means the
        // file can be found without being told where to look.
        BSML::Lite::CreateText(parent, u"Diagnostics (plain .txt):");
        BSML::Lite::CreateText(parent, StringW("<size=70%>" + Vivify::Report::FilePath() + "</size>"));
        BSML::Lite::CreateText(parent, StringW("<size=70%>" + std::string(kSessionsDir) +
                                               "/ (one file per session; latest.txt names this one)</size>"));

        AddToggle(parent, kShowDeveloperOptionsConfigKey, gShowDeveloperOptions,
                  [](bool show) { ShowDebugSection(show); });
        gDeveloperNote = BSML::Lite::CreateText(parent, u"");

        // Debug: test and diagnostic switches, hidden unless "Show Developer
        // Options" is on. New test-only switches go here.
        // The whole row is hidden: its ancestor directly under the container,
        // whichever object of the control BSML put the component on.
        auto debug = [parent](UnityEngine::GameObject* object) {
          if (object == nullptr) return;
          UnityEngine::Transform* row = object->get_transform().ptr();
          while (row != nullptr && row->get_parent().ptr() != nullptr && row->get_parent().ptr() != parent) {
            row = row->get_parent().ptr();
          }
          if (row != nullptr && row->get_parent().ptr() == parent) gDebugObjects.emplace_back(row->get_gameObject());
        };
        debug(AddHeading(parent, "Debug"));
        debug(AddHeading(parent, "<size=80%>Diagnostics</size>"));
        debug(AddToggle(parent, kVivifyDebugLoggingConfigKey, gVivifyDebugLogging));
        debug(AddToggle(parent, kDetailedLoggingConfigKey, gDetailedLogging));
        // Changes nothing on screen: during a frame that takes over 100 ms,
        // where the main and render threads are, summed up per section of the
        // song in the session log. Costs nothing while frames are fast.
        debug(AddToggle(parent, kSlowFrameSamplerConfigKey, gSlowFrameSampler));
        // For A/B tests: the viewport at this scale for the whole song,
        // whatever the map does. 0 is off; 1-39 snaps to 40, 100 to off.
        {
          auto* setting = BSML::Lite::CreateIncrementSetting(
              parent, SettingLabel(kPerfFixedScaleConfigKey), 0, 5.0f, static_cast<float>(gPerfFixedScale), true,
              true, 0.0f, 100.0f, [](float value) {
                int percent = std::clamp(static_cast<int>(value + 0.5f), 0, 100);
                if (percent > 0 && percent < 40) percent = 40;
                SetIntConfigValue(kPerfFixedScaleConfigKey, percent == 100 ? 0 : percent, gPerfFixedScale);
              });
          if (setting != nullptr) debug(WithSettingHint(setting->get_gameObject(), kPerfFixedScaleConfigKey));
        }
        // For finding what the GPU time goes on: one category switched off for
        // the whole song, so the [PERF] section lines show its cost. The map
        // looks wrong while it is on: set it back to Nothing.
        {
          static std::array<std::string_view, Vivify::kCostTestCount> const costTests = {
              "Nothing", "Skies", "Transparent effects and particles", "Depth pre-pass",
              "Emulated geometry objects", "Post-processing blits", "Scenery"};
          static std::vector<std::string_view> costTestValues(costTests.begin(), costTests.end());
          auto* dropdown = BSML::Lite::CreateDropdown(
              parent, SettingLabel(kDebugCostTestConfigKey),
              StringW(std::string(costTests[std::clamp(gDebugCostTest, 0, Vivify::kCostTestCount - 1)])),
              std::span<std::string_view>(costTestValues), [](StringW value) {
                std::string const chosen(value);
                for (int i = 0; i < Vivify::kCostTestCount; i++) {
                  if (chosen == costTests[i]) SetIntConfigValue(kDebugCostTestConfigKey, i, gDebugCostTest);
                }
              });
          if (dropdown != nullptr) debug(WithSettingHint(dropdown->get_gameObject(), kDebugCostTestConfigKey));
        }
        debug(AddHeading(parent, "<size=80%>Switches for isolating a problem</size>"));
        debug(AddToggle(parent, kScreenArraysConfigKey, gScreenArrays));
        debug(AddToggle(parent, kUprightReadsConfigKey, gUprightReads));
        debug(AddToggle(parent, kUprightNarrowConfigKey, gUprightNarrow));
        debug(AddToggle(parent, kTranslateShadersConfigKey, gTranslateShaders));
        debug(AddToggle(parent, kDisableBeat0FilmgrainBlitConfigKey, gDisableBeat0FilmgrainBlit));
        debug(AddToggle(parent, kDisableCreateCameraDepthConfigKey, gDisableCreateCameraDepth));
        debug(AddToggle(parent, kDisableVRCenterAdjustConfigKey, gDisableVRCenterAdjust));
        ShowDebugSection(gShowDeveloperOptions);
        } catch (std::exception const& e) {
          PaperLogger.error("Vivify settings menu: construction threw ({}); the controls after the "
                            "failure are missing", e.what());
        } catch (...) {
          PaperLogger.error("Vivify settings menu: construction threw; the controls after the "
                            "failure are missing");
        }
      },
      "Vivify", false);
}
}

Configuration &getConfig() {
  static Configuration config(modInfo);
  return config;
}

bool GetMultipassRenderingEnabled() {
  // Hard-disabled; the setting that once switched it was removed in naomi.10,
  // its saved value long unread. When enabled, MultipassKeywordController::OnPreRender() (see
  // VivifyComponents.cpp) calls Shader::SetGlobalInt("_StereoActiveEye", eye)
  // every frame a level is active, using Camera::get_stereoActiveEye(). That
  // value is only meaningful under legacy multi-pass XR rendering; Quest
  // renders single-pass-instanced, so this writes a near-arbitrary value into
  // a GLOBAL shader property that isn't scoped to Vivify's own materials --
  // any other shader reading it (note materials, UI/menu shaders, saber and
  // arc effects) gets corrupted for as long as a level is active, including
  // while paused. That's what was causing notes to go invisible after
  // entering a level and arcs/saber effects/menus to go invisible in some
  // levels. Confirmed by removing the initializer/EnsureConfigDefaults default
  // mismatch that let a stale or manually-enabled config value take effect;
  // the previous ref1 base avoided this entirely by hardcoding it off. Leave
  // this hardcoded false until MultipassKeywordController is actually fixed
  // for single-pass-instanced rendering and verified on-device.
  return false;
}

bool GetVivifyDebugLogging() {
  return gVivifyDebugLogging;
}

bool GetDisableBeat0FilmgrainBlit() {
  return gDisableBeat0FilmgrainBlit;
}

bool GetDisableAllBlits() {
  // Also while the cost test switches blits off (naomi.8, debug).
  return gDisableAllBlits || gDebugCostTest == Vivify::kCostTestBlits;
}

bool GetDisableCreateCameraDepth() {
  return gDisableCreateCameraDepth;
}

bool GetDisableCustomNoteVisuals() {
  return gDisableCustomNoteVisuals;
}

bool GetDisableVisualsInMultiplayer() {
  return gDisableVisualsInMultiplayer;
}

bool GetDisableVRCenterAdjust() {
  return gDisableVRCenterAdjust;
}

bool GetConvertPcBundlesOnDevice() {
  return gConvertPcBundlesOnDevice;
}

bool GetStandInShading() {
  return gStandInShading;
}

bool GetSceneDepthTexture() {
  return gSceneDepth;
}

bool GetSubmitScoresOnVivifyMaps() {
  return gSubmitScores;
}

bool GetTranslateShadersOnConversion() {
  return gTranslateShaders;
}

bool GetAudioLinkEnabled() {
  return gAudioLink;
}

bool GetMapRealtimeShadows() {
  return gMapRealtimeShadows;
}

bool GetReplayRenderMode() {
  return gReplayRenderMode;
}

bool GetBlitScreenTextureFix() {
  return gBlitFix;
}

bool GetPrepareShadersBeforePlaying() {
  return gPrepareShaders;
}

bool GetDrawGeometryShaderEffects() {
  return gGeometryEffects;
}

bool GetEmulateGeometryShaderEffects() {
  return gEmulateGeometry;
}

bool GetPoolNoteReplacements() {
  return gPoolReplacements;
}

// "GPU Instancing For Scenery" was retired in naomi.10-beta.4 (VivifySettings.cpp,
// kRemovedKeys): converted materials are never instanced. The code it switched
// stays until the release cleanup; this keeps it off.
bool GetSceneryGpuInstancing() {
  return false;
}

bool GetCrashGuard() {
  return gCrashGuard;
}

bool GetUsePcBundleForMissingShaders() {
  return gPcBundleForMissingShaders;
}

bool GetOffscreenCulling() {
  return gOffscreenCulling;
}

bool GetScreenTexturesAsArrays() {
  return gScreenArrays;
}

bool GetUprightScreenReads() {
  return gUprightReads;
}

bool GetUprightReadsProjectionSignOnly() {
  return gUprightNarrow;
}

bool GetDetailedLogging() {
  return gDetailedLogging;
}

bool GetSlowFrameSampler() {
  return gSlowFrameSampler;
}

bool GetEmulateParticleGeometry() {
  return gParticleGeometry;
}

bool GetPerformanceMode() {
  return gPerformanceMode;
}

int GetPerformanceModeFloor() {
  return std::clamp(gPerfFloor, 40, 90);
}

bool GetPerformanceModeHalfRefresh() {
  return gPerfHalfRefresh;
}

bool GetPerformanceModeScaleDepthMaps() {
  return gPerfDepthMaps;
}

bool GetPerformanceModeLearn() {
  return gPerfLearn;
}

int GetDebugCostTest() {
  return std::clamp(gDebugCostTest, 0, Vivify::kCostTestCount - 1);
}

int GetPerformanceModeFixedScale() {
  return gPerfFixedScale >= 40 && gPerfFixedScale < 100 ? gPerfFixedScale : 0;
}

int GetFarCullingDistance() {
  return std::clamp(gFarCullingDistance, 0, 1000);
}

int GetEffectResolutionPercent() {
  return std::clamp(gEffectResolution, 25, 100);
}

float GetEffectResolutionScale() {
  return static_cast<float>(std::clamp(gEffectResolution, 25, 100)) / 100.0f;
}

std::string GetStandInShaderName() {
  return gStandInShaderName;
}

// What the settings version migration did at this launch, for the session
// log; empty when it did not run.
std::string gSettingsMigrationNote;

void EnsureConfigDefaults() {
  auto& config = getConfig();
  // Missing, mistyped and out-of-range values become defaults; saved ones
  // carry over by key; keys nothing reads any more are dropped (naomi.10).
  bool changed = Vivify::Settings::Load(config.config);
  // Once per settings version: values saved under older defaults go back to
  // this version's defaults, except the logging settings (naomi.10-rc.1).
  auto const migration = Vivify::Settings::Migrate(config.config);
  if (migration.ran) {
    changed = true;
    gSettingsMigrationNote = fmt::format(
        "settings reset once to this version's defaults (settings version {} -> {}); logging settings kept; {}",
        migration.fromVersion, Vivify::Settings::kSettingsVersion,
        migration.changedCount == 0 ? std::string("no saved value differed")
                                    : fmt::format("{} changed: {}", migration.changedCount, migration.changed));
    PaperLogger.info("Vivify {}", gSettingsMigrationNote);
  }
  if (changed) {
    config.Write();
  }
  ApplyConfigValues();
}

MOD_EXTERN_FUNC void setup(CModInfo *info) noexcept {
  *info = modInfo.to_c();
  try {
    InstallVivifyFileLogSink();
  } catch (...) {
  }
  // A config file another tool or a crash left unreadable must not take the
  // game down at launch (setup is noexcept): Vivify starts from its defaults.
  try {
    getConfig().Load();
  } catch (std::exception const& ex) {
    PaperLogger.warn("Vivify: config could not be read ({}); using defaults", ex.what());
    getConfig().config.SetObject();
  } catch (...) {
    getConfig().config.SetObject();
  }
  try {
    EnsureConfigDefaults();
  } catch (std::exception const& ex) {
    PaperLogger.warn("Vivify: config defaults could not be written ({})", ex.what());
  } catch (...) {
  }
  // Note: the settings are intentionally NOT
  // reset here. EnsureConfigDefaults() above already loaded the saved values (or
  // wrote the defaults on first run); hardcoding them back to a fixed value here
  // would silently discard the player's saved settings-menu choice on every launch.
  PushLine("HEADER", "settings: " + Vivify::Settings::Snapshot(getConfig().config), false, false);
  if (!gSettingsMigrationNote.empty()) PushLine("HEADER", gSettingsMigrationNote, false, false);
  PaperLogger.info("Vivify file logging active -> {}/{}", kSessionsDir, gSessionFile);
}
namespace {
// Every other mod the loader started, in one line of the session log: the
// first thing to know when Vivify misbehaves alongside something else (a
// freeze in 0.14.22 turned out to be Adaptive Audio Latency and Custom
// Models, found by turning mods off one at a time).
void LogLoadedMods() {
  CModResults results = modloader_get_loaded();
  std::string list;
  for (size_t i = 0; i < results.size; i++) {
    auto const& info = results.array[i].info;
    if (info.id == nullptr) continue;
    if (!list.empty()) list += ", ";
    list += info.id;
    if (info.version != nullptr) list += std::string(" ") + info.version;
  }
  PaperLogger.info("Vivify: loaded mods ({}): {}", results.size, list.empty() ? "none reported" : list);
  modloader_free_results(&results);
}
}

MOD_EXTERN_FUNC void late_load() noexcept {
  // noexcept: anything escaping from here terminates the game at launch. A
  // failure in Vivify's own start-up costs Vivify, not the game and every
  // other mod with it.
  try {
    il2cpp_functions::Init();
    custom_types::Register::AutoRegister();
    RegisterModSettings();
    Vivify::LateLoad();
  } catch (std::exception const& ex) {
    PaperLogger.error("Vivify late_load failed ({}); Vivify is not active this session", ex.what());
  } catch (...) {
    PaperLogger.error("Vivify late_load failed with a non-std exception; Vivify is not active this session");
  }
  try {
    LogLoadedMods();
  } catch (...) {
  }
}
