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
constexpr std::string_view kSceneryInstancingConfigKey = "sceneryGpuInstancing";
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
bool gSceneryInstancing = false;
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

// Where each setting in Vivify::Settings' table is held while the game runs.
struct BoundSetting {
  std::string_view key;
  bool* boolValue = nullptr;
  int* intValue = nullptr;
  std::string* stringValue = nullptr;
};
std::array<BoundSetting, 37> const kBoundSettings = {{
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
    {kSceneryInstancingConfigKey, &gSceneryInstancing},
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
}};

// Both diagnostic files live beside the mod's own data, and both are .txt.
//
// This used to be Logs/Vivify.log. A .log file has no default handler on Android
// or Windows, so tapping it does nothing and it looks like no log exists at all
// -- and it sat in a different directory from the per-level report, so there
// were two places to look. One directory, two .txt files, both openable.
constexpr std::string_view kVivifyLogDir = "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify";
constexpr std::string_view kVivifyLogPath =
    "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifySession.txt";

// A session log must not fill a headset, and it is truncated at launch anyway,
// so this only has to bound one play session.
constexpr std::streamoff kVivifyLogMaxBytes = 8 * 1024 * 1024;

std::ofstream gVivifyLogFile;
std::mutex gVivifyLogMutex;
// vivify_global.txt: every session appended, never truncated; 9 MB parts
// (vivify_global-p2.txt, ...), the older half deleted past 1 GB in total.
Vivify::GlobalLog gVivifyGlobalLog(std::string(kVivifyLogDir), "vivify_global", 9ull * 1024 * 1024,
                                   1024ull * 1024 * 1024);

std::string WallClockText() {
  std::time_t const now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
  return buffer;
}
bool gVivifyLogSinkInstalled = false;
bool gVivifyLogCapped = false;
std::chrono::steady_clock::time_point gVivifyLogLastFlush{};
std::chrono::steady_clock::time_point gVivifyLogOpened = std::chrono::steady_clock::now();

// Writes one line to both log files and flushes them, on the calling thread.
// Lines through PaperLogger reach the files later, from Paper's own thread,
// and ordinary ones are only flushed every 250 ms -- so when the game hangs and
// is killed, the last of them never reach disk. Breadcrumbs and watchdog lines
// go through here instead, and so can land ahead of PaperLogger lines that
// were logged just before them.
void WriteFlushedLogLine(std::string const& line) {
  std::lock_guard<std::mutex> lock(gVivifyLogMutex);
  if (gVivifyGlobalLog.IsOpen()) {
    gVivifyGlobalLog.Write(WallClockText() + " " + line);
    gVivifyGlobalLog.Flush();
  }
  if (gVivifyLogFile.is_open() && !gVivifyLogCapped) {
    gVivifyLogFile << line << '\n';
    gVivifyLogFile.flush();
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

void InstallVivifyFileLogSink() {
  if (gVivifyLogSinkInstalled) return;
  gVivifyLogSinkInstalled = true;
  std::error_code ec;
  std::filesystem::create_directories(std::string(kVivifyLogDir), ec);

  gVivifyLogFile.open(std::string(kVivifyLogPath), std::ios::out | std::ios::trunc);
  if (!gVivifyLogFile.is_open()) {
    PaperLogger.warn("Vivify: could not open log file at {} (logging to logcat only)", kVivifyLogPath);
    return;
  }
  gVivifyLogFile << "=== Vivify " << VERSION << " (commit " << VIVIFY_GIT_COMMIT << ") session log ===\n";
  gVivifyLogFile.flush();
  gVivifyLogLastFlush = std::chrono::steady_clock::now();
  gVivifyGlobalLog.Open("=== Vivify " + std::string(VERSION) + " (commit " + VIVIFY_GIT_COMMIT + ") session started " +
                        WallClockText() + " ===");

  // Started once the files are open, so its lines have somewhere to go.
  gWatchdog.Start([](std::string const& line) {
    WriteFlushedLogLine("[" + ElapsedText() + "] " + line);
    WatchdogLogger.warn("{}", line);
  });

  Paper::Logger::AddLogSink([](Paper::LogData const& data) {
    if (!data.tag.has_value() || *data.tag != std::string_view(MOD_ID)) return;
    std::lock_guard<std::mutex> lock(gVivifyLogMutex);
    bool const important = data.level >= Paper::LogLevel::WRN;
    if (gVivifyGlobalLog.IsOpen()) {
      gVivifyGlobalLog.Write(WallClockText() + " [" + std::string(Paper::format_as(data.level)) + "] " +
                             std::string(data.message));
      if (important) gVivifyGlobalLog.Flush();
    }
    if (!gVivifyLogFile.is_open() || gVivifyLogCapped) return;

    gVivifyLogFile << '[' << Paper::format_as(data.level) << "] " << data.message << '\n';

    if (gVivifyLogFile.tellp() > kVivifyLogMaxBytes) {
      gVivifyLogFile << "=== log capped at " << (kVivifyLogMaxBytes / (1024 * 1024))
                     << "MB; further lines go to logcat only ===\n";
      gVivifyLogFile.flush();
      gVivifyLogCapped = true;
      return;
    }

    // Flushing every line meant an sdcard write per log line, on whichever
    // thread logged -- including the main thread mid-gameplay, where Vivify can
    // be noisy. Warnings and errors still flush immediately, because those are
    // the lines that matter if the game stops before the buffer is written;
    // ordinary lines are flushed at most a few times a second.
    auto const now = std::chrono::steady_clock::now();
    if (important || now - gVivifyLogLastFlush > std::chrono::milliseconds(250)) {
      gVivifyLogFile.flush();
      gVivifyGlobalLog.Flush();
      gVivifyLogLastFlush = now;
    }
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
  WriteFlushedLogLine("[CRUMB " + ElapsedText() + "] " + text + gc);
  CrumbLogger.info("{}{}", text, gc);
}

void VivifyHeartbeat() {
  gWatchdog.Beat();
}

void VivifySetAppPaused(bool paused) {
  gWatchdog.SetPaused(paused);
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
        BSML::Lite::CreateText(parent, StringW(std::string("Vivify ") + VERSION));

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
                  "Every Vivify setting back to its default. Maps and converted files are kept.",
                  PressTwice("Press Reset Settings again within 5 s: every Vivify setting goes back to its default",
                             []() {
                               auto& config = getConfig();
                               Vivify::Settings::ResetToDefaults(config.config);
                               config.Write();
                               ApplyConfigValues();
                               PaperLogger.info("Vivify settings: all reset to their defaults from the menu");
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
        BSML::Lite::CreateText(parent, u"Diagnostics (both plain .txt, same folder):");
        BSML::Lite::CreateText(parent, StringW("<size=70%>" + Vivify::Report::FilePath() + "</size>"));
        BSML::Lite::CreateText(parent, StringW("<size=70%>" + std::string(kVivifyLogPath) + "</size>"));
        BSML::Lite::CreateText(
            parent, StringW("<size=70%>" + std::string(kVivifyLogDir) + "/vivify_global.txt (every session)</size>"));

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
        debug(AddToggle(parent, kSceneryInstancingConfigKey, gSceneryInstancing));
        debug(AddHeading(parent, "<size=80%>Switches for isolating a problem</size>"));
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

bool GetSceneryGpuInstancing() {
  return gSceneryInstancing;
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

void EnsureConfigDefaults() {
  auto& config = getConfig();
  // Missing, mistyped and out-of-range values become defaults; saved ones
  // carry over by key; keys nothing reads any more are dropped (naomi.10).
  if (Vivify::Settings::Load(config.config)) {
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
  PaperLogger.info("Vivify file logging active -> {}", kVivifyLogPath);
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
