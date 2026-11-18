#include "VivifySettings.hpp"

#include <algorithm>
#include <array>

#include "VivifyPerfMode.hpp"

namespace Vivify::Settings {

namespace {

using enum Kind;
using enum Section;

constexpr std::string_view kPerformance = "Performance Mode";
constexpr std::string_view kLook = "How maps look";
constexpr std::string_view kConversion = "PC maps";
constexpr std::string_view kOther = "Other";
constexpr std::string_view kDiagnostics = "Diagnostics";
constexpr std::string_view kIsolation = "Switches for isolating a problem";

// Menu order within each section is the order here.
constexpr std::array kDefs = {
    // --- Everyday: one screen, no scrolling ---------------------------------
    Def{"performanceModeHeavyMaps", Bool, 0, 0, 1, 1, Everyday, {}, "Performance Mode",
        "Lowers the resolution while a heavy map runs slow. Changes how the map looks."},
    Def{"effectResolutionPercent", Int, 100, 25, 100, 25, Everyday, {}, "Effect Resolution %",
        "Draws a map's full-screen effects at lower resolution: faster, but blurrier."},
    Def{"disableCustomNoteVisuals", Bool, 0, 0, 1, 1, Everyday, {}, "Plain Notes And Sabers",
        "Keeps the game's own notes, sabers and debris instead of the map's custom ones."},
    Def{"submitScoresOnVivifyMaps", Bool, 1, 0, 1, 1, Everyday, {}, "Submit Scores On Vivify Maps",
        "Sends scores and replays to the leaderboards on Vivify maps."},

    // --- Advanced -----------------------------------------------------------
    Def{"performanceModeFloorPercent", Int, 60, 40, 90, 5, Advanced, kPerformance, "Lowest Resolution %",
        "How far Performance Mode may lower the resolution."},
    Def{"performanceModeScaleDepthMaps", Bool, 0, 0, 1, 1, Advanced, kPerformance,
        "Also Scale Maps That Read Depth",
        "Lets Performance Mode scale maps whose effects read depth; their soft edges can shift."},
    Def{"performanceModeLearnedProfiles", Bool, 0, 0, 1, 1, Advanced, kPerformance, "Learn Each Map",
        "Remembers which parts of a map are heavy and eases down before them on later plays."},
    Def{"performanceModeHalfRefresh", Bool, 0, 0, 1, 1, Advanced, kPerformance, "Aim For Half The Refresh Rate",
        "Aims for 60 fps at 120 Hz (36 or 45 at 72 or 90 Hz) instead of the full rate."},

    Def{"audioLink", Bool, 1, 0, 1, 1, Advanced, kLook, "AudioLink",
        "Lets map effects react to the music, as on PC."},
    Def{"standInShading", Bool, 1, 0, 1, 1, Advanced, kLook, "Stand-Ins For Unsupported Shaders",
        "Draws objects whose shader cannot run in a plain colour instead of not at all."},
    Def{"sceneDepthTexture", Bool, 1, 0, 1, 1, Advanced, kLook, "Scene Depth For Map Shaders",
        "Gives map shaders the depth texture they read for fog and soft edges."},
    Def{"emulateGeometryShaderEffects", Bool, 1, 0, 1, 1, Advanced, kLook, "Emulate Geometry-Shader Effects",
        "Draws known wireframe, explosion and grass effects without a geometry stage. Reconverts maps."},
    Def{"emulateParticleGeometry", Bool, 1, 0, 1, 1, Advanced, kLook, "Emulate Geometry Effects On Particles",
        "Bakes particle bursts that hold still, so those effects draw on them too."},
    Def{"drawGeometryShaderEffects", Bool, 0, 0, 1, 1, Advanced, kLook,
        "Draw Other Geometry Effects Without It (experimental)",
        "Draws the rest without their geometry stage instead of as stand-ins; often wrong. Reconverts maps."},
    Def{"blitScreenTextureFix", Bool, 0, 0, 1, 1, Advanced, kLook, "Blit Fix For Screen Textures (experimental)",
        "Builds a second copy of converted shaders for effects drawn into screen textures."},
    Def{"mapRealtimeShadows", Bool, 0, 0, 1, 1, Advanced, kLook, "Map Realtime Shadows",
        "Lets maps turn on realtime shadows. Costs frame time."},
    Def{"disableAllBlits", Bool, 0, 0, 1, 1, Advanced, kLook, "Skip All Full-Screen Effects",
        "Skips every full-screen effect a map draws: faster, but the map looks different."},
    Def{"offscreenCullingOptIn", Bool, 0, 0, 1, 1, Advanced, kLook, "Pause Offscreen Animations",
        "Stops animating map objects that are out of view. Some objects can stay stuck."},
    Def{"farCullingDistance", Int, 0, 0, 1000, 50, Advanced, kLook, "Far Culling Distance m (0 = off)",
        "Hides everything farther away than this: faster, but far scenery disappears."},
    Def{"disableVisualsInMultiplayer", Bool, 1, 0, 1, 1, Advanced, kLook, "Disable Vivify Visuals In Multiplayer",
        "Keeps map visuals off in multiplayer lobbies."},

    Def{"convertPcBundlesOnDevice", Bool, 1, 0, 1, 1, Advanced, kConversion, "Convert PC Maps On The Headset",
        "Converts maps that only have a PC bundle, so they can be played."},
    Def{"prepareShadersInMenu", Bool, 0, 0, 1, 1, Advanced, kConversion, "Prepare Shaders Before Playing",
        "Compiles a map's shaders in the menu instead of in the first seconds of play."},
    Def{"pcBundleForMissingShaders", Bool, 0, 0, 1, 1, Advanced, kConversion,
        "Use Only PC Bundle For Missing Shaders",
        "Plays a Quest map from its PC build when the Quest bundle has empty shaders."},
    Def{"replayRenderMode", Bool, 0, 0, 1, 1, Advanced, kConversion, "Replay Render Mode",
        "Builds converted maps for replay and recording renderers (one view)."},
    Def{"crashGuard", Bool, 1, 0, 1, 1, Advanced, kConversion, "Crash Guard",
        "After two loads of a map that never finish, plays it with stand-ins instead."},

    Def{"poolNoteReplacements", Bool, 1, 0, 1, 1, Advanced, kOther, "Reuse Note And Debris Copies",
        "Reuses custom note and debris objects instead of making new ones for every note."},
    Def{"showDeveloperOptions", Bool, 0, 0, 1, 1, Advanced, kOther, "Show Developer Options",
        "Shows the Debug section: test and diagnostic switches."},

    // --- Debug (Show Developer Options) -------------------------------------
    Def{"vivifyDebugLogging", Bool, 0, 0, 1, 1, Debug, kDiagnostics, "Debug Logging",
        "Writes far more detail to the session log."},
    Def{"detailedLogging", Bool, 0, 0, 1, 1, Debug, kDiagnostics, "Detailed Logging",
        "Adds a line per frame, Vivify's cost by part, and detail around slow frames to the log."},
    Def{"slowFrameSampler", Bool, 1, 0, 1, 1, Debug, kDiagnostics, "Slow-Frame Sampler",
        "During frames over 100 ms, notes where the game's threads were, for the log."},
    Def{"performanceModeFixedScalePercent", Int, 0, 0, 100, 5, Debug, kDiagnostics,
        "Debug: Fixed Resolution % (0 = off)", "Holds the resolution at this scale for a whole song, for A/B tests."},
    Def{"debugCostTest", Int, 0, 0, kCostTestCount - 1, 1, Debug, kDiagnostics, "Debug: Switch Off For A Cost Test",
        "Switches one kind of drawing off for the song, so the log shows its cost. The map looks wrong."},
    Def{"screenTexturesAsArrays", Bool, 1, 0, 1, 1, Debug, kIsolation, "Two-Eye Screen Textures",
        "Converted shaders read the screen as the Quest's two-eye array. Off: the old way (white Blits). Reconverts."},
    Def{"uprightScreenReads", Bool, 1, 0, 1, 1, Debug, kIsolation, "Upright Screen Reads",
        "Converted shaders read the screen the right way up, as on PC. Off: the old reads. Reconverts."},
    Def{"translateShadersOnConversion", Bool, 1, 0, 1, 1, Debug, kIsolation, "Translate Shaders On Conversion",
        "Off: converted maps keep no shaders of their own and draw as stand-ins. Reconvert after."},
    Def{"disableBeat0FilmgrainBlit", Bool, 0, 0, 1, 1, Debug, kIsolation, "Disable Beat 0 Filmgrain Blit",
        "Skips a film-grain effect a map starts at beat 0."},
    Def{"disableCreateCameraDepth", Bool, 0, 0, 1, 1, Debug, kIsolation, "Disable CreateCamera Depth",
        "Gives cameras a map creates no depth texture."},
    Def{"disableVRCenterAdjust", Bool, 0, 0, 1, 1, Debug, kIsolation, "Disable VR Center Adjust Handling",
        "Skips the game's room recentering while a Vivify map plays."},

    // --- Hidden: config file only -------------------------------------------
    Def{"standInShaderName", String, 0, 0, 0, 0, Hidden, {}, "Stand-in shader name",
        "A shader name from the session log to use for stand-ins; empty picks one."},
};

// multipassRendering: GetMultipassRenderingEnabled has been hard-wired off
// since it corrupted a global shader property; the saved value was never
// read. pcShadersForEmptyQuestShaders: no longer optional since 0.14.10, and
// nothing read it. sceneryGpuInstancing (naomi.10-beta.4): with it on, 743
// Aether's instanced scenery took 0.5-1.2 s of GPU time a frame and 1-2 GB of
// memory until the headset froze (beta.1 and beta.3 tests); the translated
// instanced variants are not trustworthy, so converted materials are never
// instanced and the saved value is dropped.
constexpr std::array<std::string_view, 3> kRemovedKeys = {"multipassRendering", "pcShadersForEmptyQuestShaders",
                                                          "sceneryGpuInstancing"};

rapidjson::Value Key(std::string_view key, rapidjson::Document::AllocatorType& allocator) {
  return rapidjson::Value(key.data(), static_cast<rapidjson::SizeType>(key.size()), allocator);
}

bool Valid(Def const& def, rapidjson::Value const& value) {
  switch (def.kind) {
    case Bool: return value.IsBool();
    case Int: return value.IsInt() && value.GetInt() >= def.minValue && value.GetInt() <= def.maxValue;
    case String: return value.IsString();
  }
  return false;
}

void SetDefault(Def const& def, rapidjson::Value& value, rapidjson::Document::AllocatorType& allocator) {
  switch (def.kind) {
    case Bool: value.SetBool(def.defaultValue != 0); break;
    case Int: value.SetInt(def.defaultValue); break;
    case String: value.SetString("", allocator); break;
  }
}

rapidjson::Value const* Member(rapidjson::Document const& doc, std::string_view key) {
  if (!doc.IsObject()) return nullptr;
  auto it = doc.FindMember(rapidjson::StringRef(key.data(), key.size()));
  return it == doc.MemberEnd() ? nullptr : &it->value;
}

std::string ValueText(Def const& def, rapidjson::Document const& doc) {
  switch (def.kind) {
    case Bool: return GetBool(doc, def.key) ? "true" : "false";
    case Int: return std::to_string(GetInt(doc, def.key));
    case String: return "'" + GetString(doc, def.key) + "'";
  }
  return {};
}

bool IsDefault(Def const& def, rapidjson::Document const& doc) {
  switch (def.kind) {
    case Bool: return GetBool(doc, def.key) == (def.defaultValue != 0);
    case Int: return GetInt(doc, def.key) == def.defaultValue;
    case String: return GetString(doc, def.key).empty();
  }
  return true;
}

}  // namespace

std::span<Def const> All() {
  return kDefs;
}

Def const* Find(std::string_view key) {
  for (auto const& def : kDefs) {
    if (def.key == key) return &def;
  }
  return nullptr;
}

std::span<std::string_view const> RemovedKeys() {
  return kRemovedKeys;
}

bool Load(rapidjson::Document& doc) {
  bool changed = false;
  if (!doc.IsObject()) {
    doc.SetObject();
    changed = true;
  }
  auto& allocator = doc.GetAllocator();
  for (auto key : kRemovedKeys) {
    changed |= doc.RemoveMember(rapidjson::StringRef(key.data(), key.size()));
  }
  for (auto const& def : kDefs) {
    auto it = doc.FindMember(rapidjson::StringRef(def.key.data(), def.key.size()));
    if (it == doc.MemberEnd()) {
      rapidjson::Value value;
      SetDefault(def, value, allocator);
      doc.AddMember(Key(def.key, allocator), value, allocator);
      changed = true;
    } else if (!Valid(def, it->value)) {
      SetDefault(def, it->value, allocator);
      changed = true;
    }
  }
  return changed;
}

void ResetToDefaults(rapidjson::Document& doc) {
  doc.SetObject();
  Load(doc);
}

bool GetBool(rapidjson::Document const& doc, std::string_view key) {
  auto const* def = Find(key);
  auto const* value = Member(doc, key);
  if (value != nullptr && value->IsBool()) return value->GetBool();
  return def != nullptr && def->defaultValue != 0;
}

int GetInt(rapidjson::Document const& doc, std::string_view key) {
  auto const* def = Find(key);
  auto const* value = Member(doc, key);
  if (def == nullptr) return value != nullptr && value->IsInt() ? value->GetInt() : 0;
  if (value != nullptr && def->kind == Int && Valid(*def, *value)) return value->GetInt();
  return def->defaultValue;
}

std::string GetString(rapidjson::Document const& doc, std::string_view key) {
  auto const* value = Member(doc, key);
  if (value != nullptr && value->IsString()) return std::string(value->GetString(), value->GetStringLength());
  return {};
}

std::string Snapshot(rapidjson::Document const& doc) {
  std::string out;
  for (auto const& def : kDefs) {
    if (!out.empty()) out += ' ';
    out += std::string(def.key) + "=" + ValueText(def, doc);
  }
  return out;
}

std::string NonDefault(rapidjson::Document const& doc, Section section) {
  std::string out;
  for (auto const& def : kDefs) {
    if (def.section != section || IsDefault(def, doc)) continue;
    if (!out.empty()) out += ' ';
    out += std::string(def.key) + "=" + ValueText(def, doc);
  }
  return out;
}

}  // namespace Vivify::Settings
