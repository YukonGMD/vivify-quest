#pragma once

#include <functional>
#include <string>
#include <vector>
// paper2_scotland2 4.7.0's shared/backtrace.hpp calls std::ifstream but includes
// only <sstream>, and beatsaber-hook pulls that header in from
// config-utils.hpp -> utils-functions.h. libc++ used to leak <fstream> through
// <sstream>; under NDK 27 it no longer does, so the template is incomplete at
// the point of use and every translation unit that reaches beatsaber-hook fails
// to compile. Including it here, ahead of that chain, fixes all of them without
// editing a vendored header that restore-deps.py regenerates.
#include <fstream>
#include "scotland2/shared/modloader.h"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#include "beatsaber-hook/shared/utils/logging.hpp"
#include "paper2_scotland2/shared/logger.hpp"
#include "_config.hpp"
#include "VivifyBlackBox.hpp"
#include <string_view>
Configuration &getConfig();
bool GetMultipassRenderingEnabled();
bool GetVivifyDebugLogging();
bool GetDisableBeat0FilmgrainBlit();
bool GetDisableAllBlits();
bool GetDisableCreateCameraDepth();
bool GetDisableCustomNoteVisuals();
bool GetDisableVisualsInMultiplayer();
bool GetDisableVRCenterAdjust();
bool GetConvertPcBundlesOnDevice();
bool GetStandInShading();
// Whether the main camera renders _CameraDepthTexture while a Vivify map plays.
bool GetSceneDepthTexture();
bool GetTranslateShadersOnConversion();
// Whether the built-in AudioLink port (VivifyAudioLink) runs and publishes
// _AudioTexture.
bool GetAudioLinkEnabled();
// Whether SetRenderingSettings may turn on realtime shadows (off: the
// shadow* quality settings in it are ignored).
bool GetMapRealtimeShadows();
// Replay render mode (0.14.9): converted maps get single-screen programs only,
// for replay and recording renderers. The headset draws none of them.
bool GetReplayRenderMode();
// Experimental blit companion (0.14.10, opt-in from 0.14.11).
bool GetBlitScreenTextureFix();
// Compile converted maps' shaders in the menu before play (0.14.15).
bool GetPrepareShadersBeforePlaying();
// Draw geometry-shader effects without their geometry stage (opt-in, 0.14.17).
bool GetDrawGeometryShaderEffects();
// Geometry-shader emulation for known shaders (naomi.5); on by default.
bool GetEmulateGeometryShaderEffects();
// Reuse note, debris and saber replacement copies (naomi.5); on by default.
bool GetPoolNoteReplacements();
// Converted maps' scenery keeps GPU instancing (naomi.5, experimental); off by default.
bool GetSceneryGpuInstancing();
// Fraction (0.25-1) of the eye resolution that map post-process effects render
// at (0.14.20); 1 renders them at full resolution, as before.
float GetEffectResolutionScale();
int GetEffectResolutionPercent();
// Culling (0.14.23): offscreen animators in a map's prefabs skip their update;
// the main camera draws nothing beyond GetFarCullingDistance() metres (0: off).
bool GetOffscreenCulling();
// Crash guard (0.14.24): whether interrupted loads of a converted bundle count
// toward its untranslated fallback.
bool GetCrashGuard();
// Whether a Quest bundle missing shaders is replaced by its converted PC build.
bool GetUsePcBundleForMissingShaders();
int GetFarCullingDistance();
// Slow-frame sampler (naomi.7): diagnostics, on by default.
bool GetSlowFrameSampler();
// Geometry emulation on static mesh particles (naomi.8): on by default.
bool GetEmulateParticleGeometry();
// Performance mode for heavy maps (naomi.7, reworked in naomi.8): off by
// default; changes the look. Floor and fixed scale are percent; the fixed
// scale (a debug setting) is 0 when off.
bool GetPerformanceMode();
int GetPerformanceModeFloor();
bool GetPerformanceModeHalfRefresh();
bool GetPerformanceModeScaleDepthMaps();
bool GetPerformanceModeLearn();
int GetPerformanceModeFixedScale();
// Cost test (naomi.8, debug): the Vivify::CostTest category switched off for
// whole songs; 0 is off.
int GetDebugCostTest();
// Empty when the stand-in shader should be chosen automatically.
std::string GetStandInShaderName();
// Whether a map carrying the Vivify requirement still submits its score.
// Submission gates replay recording, so turning it off leaves no replay behind.
bool GetSubmitScoresOnVivifyMaps();
void EnsureConfigDefaults();
// Level-start diagnostics (0.14.25-naomi.2). A breadcrumb is written to both
// log files and flushed at once, and becomes the "last breadcrumb" the
// watchdog names if the main thread then stalls. The heartbeat is called once
// a frame; while the app is paused, stalls are not reported.
void VivifyBreadcrumb(std::string const& text);
void VivifyHeartbeat();
void VivifySetAppPaused(bool paused);
// For the song performance summary (naomi.4). VivifyWriteFlushedLine writes
// one line to both log files and flushes them. VivifyGcCount (the number of
// garbage collections so far, -1 if unknown) and VivifyHeapMB (managed heap
// used and total, false if unknown) are cheap enough for every frame and do
// not allocate; neither does VivifyCopyBreadcrumb.
void VivifyWriteFlushedLine(std::string const& line);
int VivifyGcCount();
bool VivifyHeapMB(double& usedMB, double& totalMB);
void VivifyCopyBreadcrumb(char* buffer, size_t size);
// Slow-frame sampling (naomi.7), done by the watchdog thread. section >= 0
// counts frames that run past 100 ms toward that section of the song, -1
// stops. VivifySlowFrameReset forgets what was counted (a song's start);
// VivifySlowFrameSummary returns the summary lines, without prefixes, and
// forgets it. label(section) names a section.
void VivifySlowFrameSection(int section);
void VivifySlowFrameReset();
std::vector<std::string> VivifySlowFrameSummary(std::function<std::string(int)> const& label);
// Saved session logs (naomi.10; LOGGING.md). VivifySetSongTime stamps every
// later line with the song time (NaN outside a song). VivifyLogLine queues one
// line with its own tag ("LOAD", "FRAME", ...) on the writer thread; `limit`
// false keeps it out of the rate limit. VivifyBlackBoxRecord adds one record
// to the black box. VivifySessionMap names a map played, for the index.
// VivifySessionHeaderLate writes the header lines that need Unity, once, on the
// first frame. VivifySessionEnd writes the clean-exit footer (quit).
void VivifySetSongTime(float songTime);
void VivifyLogLine(std::string_view tag, std::string text, bool important = false, bool limit = true);
void VivifyBlackBoxRecord(Vivify::BlackBox::Type type, std::string_view text, float a = 0.0f, float b = 0.0f,
                          float c = 0.0f);
void VivifySessionMap(std::string const& key);
void VivifySessionHeaderLate();
// About once a second during a song: GL errors since the last check, read on
// the render thread.
void VivifyCheckGlErrors();
void VivifySessionEnd();
// What logging cost the game's threads so far, in nanoseconds, per layer:
// 1 the session log, 2 the black box, 3 detailed mode.
struct VivifyLogCost {
  uint64_t layer1Ns = 0, layer2Ns = 0, layer3Ns = 0;
};
VivifyLogCost VivifyLoggingCost();
void VivifyAddLayer3Cost(uint64_t ns);
// Detailed mode (Debug setting, off): per-frame lines and more (naomi.10).
bool GetDetailedLogging();
// Converted multiview programs read screen textures as two-layer arrays
// (naomi.10-beta.2, Debug setting, on).
bool GetScreenTexturesAsArrays();
// Converted shaders read the screen the right way up where PC relies on its
// flipped render targets (naomi.10-beta.4, Debug setting, on).
bool GetUprightScreenReads();
// Memory and heat for the per-section summary (naomi.10). Returns what was
// last read and asks the log's writer thread to read again, so /proc and
// /sys are never read on the calling thread. Negative (below -100 for
// temperatures): unknown.
struct VivifyDeviceSample {
  float residentMB = -1, nativeHeapMB = -1, hottestC = -1000, batteryC = -1000, gpuMHz = -1;
};
VivifyDeviceSample VivifyLatestDeviceSample();
constexpr auto PaperLogger = Paper::ConstLoggerContext("Vivify");

#define VIVIFY_DEBUG(...)                                          \
  do {                                                             \
    if (GetVivifyDebugLogging()) PaperLogger.info(__VA_ARGS__);    \
  } while (false)
