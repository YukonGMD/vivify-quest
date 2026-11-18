// Host tests for the settings table and config migration (naomi.10).
//
//   settingstest <fixtures dir> <SETTINGS.md>
//
// fixtures/naomi9.json is the tester's config file as naomi.9 left it on the
// headset; fixtures/old-0.14.17.json is an older one with keys since renamed
// or removed, values out of range and a value of the wrong type.
#include <cstdio>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "VivifySettings.hpp"

namespace S = Vivify::Settings;

namespace {

int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& name, std::string const& detail = {}) {
  if (ok) {
    passed++;
  } else {
    failed++;
    std::printf("FAIL  %s%s%s\n", name.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
  }
}

std::string ReadFile(std::string const& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(rapidjson::Document const& doc, std::string_view key) {
  return doc.IsObject() && doc.HasMember(rapidjson::StringRef(key.data(), key.size()));
}

// The defaults naomi.9 shipped (its EnsureConfigDefaults), which naomi.10
// must not change.
std::map<std::string, int> const kNaomi9Defaults = {
    {"vivifyDebugLogging", 0},
    {"disableBeat0FilmgrainBlit", 0},
    {"disableAllBlits", 0},
    {"disableCreateCameraDepth", 0},
    {"disableCustomNoteVisuals", 0},
    {"disableVisualsInMultiplayer", 1},
    {"disableVRCenterAdjust", 0},
    {"convertPcBundlesOnDevice", 1},
    {"standInShading", 1},
    {"sceneDepthTexture", 1},
    {"submitScoresOnVivifyMaps", 1},
    {"translateShadersOnConversion", 1},
    {"audioLink", 1},
    {"mapRealtimeShadows", 0},
    {"replayRenderMode", 0},
    {"blitScreenTextureFix", 0},
    {"prepareShadersInMenu", 0},
    {"drawGeometryShaderEffects", 0},
    {"emulateGeometryShaderEffects", 1},
    {"poolNoteReplacements", 1},
    {"sceneryGpuInstancing", 0},
    {"effectResolutionPercent", 100},
    {"offscreenCullingOptIn", 0},
    {"crashGuard", 1},
    {"pcBundleForMissingShaders", 0},
    {"farCullingDistance", 0},
    {"slowFrameSampler", 1},
    {"emulateParticleGeometry", 1},
    {"performanceModeHeavyMaps", 0},
    {"performanceModeFloorPercent", 60},
    {"performanceModeHalfRefresh", 0},
    {"performanceModeScaleDepthMaps", 0},
    {"performanceModeLearnedProfiles", 0},
    {"performanceModeFixedScalePercent", 0},
    {"debugCostTest", 0},
};

int Value(rapidjson::Document const& doc, S::Def const& def) {
  return def.kind == S::Kind::Bool ? (S::GetBool(doc, def.key) ? 1 : 0) : S::GetInt(doc, def.key);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: settingstest <fixtures dir> <SETTINGS.md>\n");
    return 2;
  }
  std::string const fixtures = argv[1];
  std::string const settingsDoc = ReadFile(argv[2]);

  // --- The table --------------------------------------------------------------
  std::set<std::string_view> keys;
  int everyday = 0;
  for (auto const& def : S::All()) {
    std::string const key(def.key);
    Check(keys.insert(def.key).second, "unique key " + key);
    Check(!def.label.empty(), "label for " + key);
    Check(!def.hint.empty() && def.hint.size() <= 110, "one-line hint for " + key, std::string(def.hint));
    if (def.kind == S::Kind::Bool) Check(def.defaultValue == 0 || def.defaultValue == 1, "bool default " + key);
    if (def.kind == S::Kind::Int) {
      Check(def.minValue <= def.defaultValue && def.defaultValue <= def.maxValue, "default in range " + key);
      Check(def.step > 0, "step for " + key);
    }
    if (def.section == S::Section::Everyday) everyday++;
    Check(settingsDoc.find("`" + key + "`") != std::string::npos, "SETTINGS.md lists " + key);
    for (auto removed : S::RemovedKeys()) Check(def.key != removed, "removed key not in the table: " + key);
  }
  // A heading, these, the convert button and its status line: one screen.
  Check(everyday <= 4, "everyday list fits one screen", std::to_string(everyday) + " settings");
  for (auto removed : S::RemovedKeys()) {
    Check(settingsDoc.find("`" + std::string(removed) + "`") != std::string::npos,
          "SETTINGS.md explains removed " + std::string(removed));
  }
  for (char const* key : {"slowFrameSampler", "performanceModeFixedScalePercent", "debugCostTest",
                          "uprightReadsProjectionSignOnly"}) {
    auto const* def = S::Find(key);
    Check(def != nullptr && def->section == S::Section::Debug, std::string("test-only in Debug: ") + key);
  }
  for (char const* key : {"performanceModeFloorPercent", "performanceModeScaleDepthMaps",
                          "performanceModeLearnedProfiles", "performanceModeHalfRefresh"}) {
    auto const* def = S::Find(key);
    Check(def != nullptr && def->group == "Performance Mode", std::string("Performance Mode family: ") + key);
  }
  for (auto const& [key, value] : kNaomi9Defaults) {
    auto const removed = S::RemovedKeys();
    if (std::find(removed.begin(), removed.end(), key) != removed.end()) {
      Check(S::Find(key) == nullptr, "retired since naomi.9, not in the table: " + key);
      continue;
    }
    auto const* def = S::Find(key);
    Check(def != nullptr && def->defaultValue == value, "default unchanged since naomi.9: " + key);
  }
  Check(S::Find("prepareShadersInMenu") && S::Find("prepareShadersInMenu")->defaultValue == 0,
        "Prepare Shaders stays off by default");

  // --- naomi.9's own config file carries over ---------------------------------
  {
    std::string const text = ReadFile(fixtures + "/naomi9.json");
    rapidjson::Document saved;
    saved.Parse(text.c_str());
    rapidjson::Document doc;
    doc.Parse(text.c_str());
    Check(!doc.HasParseError() && doc.IsObject(), "naomi9.json parses");
    bool const changed = S::Load(doc);
    Check(changed, "naomi9.json is rewritten (removed keys dropped, new key added)");
    for (auto removed : S::RemovedKeys()) Check(!Has(doc, removed), "naomi9: dropped " + std::string(removed));
    for (auto const& def : S::All()) {
      std::string const key(def.key);
      if (!Has(saved, def.key)) {
        Check(def.key == "showDeveloperOptions" || def.key == "detailedLogging" ||
                  def.key == "screenTexturesAsArrays" || def.key == "uprightScreenReads" ||
                  def.key == "uprightReadsProjectionSignOnly",
              "naomi9.json had every key but naomi.10's new ones: " + key);
        continue;
      }
      auto const& before = saved[key.c_str()];
      bool same = false;
      if (def.kind == S::Kind::Bool) same = before.IsBool() && before.GetBool() == S::GetBool(doc, def.key);
      if (def.kind == S::Kind::Int) same = before.IsInt() && before.GetInt() == S::GetInt(doc, def.key);
      if (def.kind == S::Kind::String) same = before.IsString() && before.GetString() == S::GetString(doc, def.key);
      Check(same, "naomi9: saved value carried over: " + key);
    }
    Check(S::GetBool(doc, "mapRealtimeShadows"), "naomi9: Map Realtime Shadows still on");
    Check(S::GetBool(doc, "drawGeometryShaderEffects"), "naomi9: Draw Geometry-Shader Effects still on");
    Check(!S::GetBool(doc, "showDeveloperOptions"), "naomi9: developer options start hidden");
    Check(!S::GetBool(doc, "detailedLogging"), "naomi9: detailed logging starts off");
    Check(S::GetBool(doc, "screenTexturesAsArrays"), "naomi9: the white-screen fix starts on");
    Check(S::GetBool(doc, "uprightScreenReads"), "naomi9: the upright screen reads start on");
    Check(!S::GetBool(doc, "uprightReadsProjectionSignOnly"), "naomi9: the narrower upright test starts off");
    Check(!S::Load(doc), "loading again changes nothing");
    std::string const nonDefault = S::NonDefault(doc, S::Section::Advanced);
    Check(nonDefault == "drawGeometryShaderEffects=true mapRealtimeShadows=true",
          "naomi9: the two changed settings are the only non-defaults", nonDefault);
    Check(S::NonDefault(doc, S::Section::Debug).empty(), "naomi9: no developer option changed");
    std::string const snapshot = S::Snapshot(doc);
    for (auto const& def : S::All()) {
      Check(snapshot.find(std::string(def.key) + "=") != std::string::npos, "snapshot has " + std::string(def.key));
    }
  }

  // --- An older file: renamed keys stay reset, bad values become defaults -----
  {
    rapidjson::Document doc;
    doc.Parse(ReadFile(fixtures + "/old-0.14.17.json").c_str());
    Check(S::Load(doc), "old file is rewritten");
    Check(!S::GetBool(doc, "offscreenCullingOptIn"), "old offscreenCulling=true does not carry (renamed to reset)");
    Check(!S::GetBool(doc, "prepareShadersInMenu"), "old prepareShaders=true does not carry (renamed to reset)");
    Check(S::GetInt(doc, "effectResolutionPercent") == 100, "out-of-range effect resolution becomes 100");
    Check(S::GetInt(doc, "performanceModeFloorPercent") == 60, "a number saved as text becomes the default");
    Check(S::GetInt(doc, "debugCostTest") == 0, "out-of-range cost test becomes Nothing");
    Check(S::GetInt(doc, "farCullingDistance") == 200, "valid far culling carries over");
    Check(S::GetBool(doc, "vivifyDebugLogging"), "debug logging on carries over");
    Check(!S::GetBool(doc, "standInShading"), "stand-ins off carries over");
    Check(!S::GetBool(doc, "audioLink"), "AudioLink off carries over");
    Check(!S::GetBool(doc, "disableVisualsInMultiplayer"), "multiplayer visuals on carries over");
    Check(S::GetString(doc, "standInShaderName") == "Custom/UnlitGlow", "stand-in shader name carries over");
    Check(!Has(doc, "multipassRendering") && !Has(doc, "pcShadersForEmptyQuestShaders"), "old: removed keys dropped");
    Check(S::GetBool(doc, "slowFrameSampler"), "a key the old file lacks gets its default");
    Check(S::NonDefault(doc, S::Section::Debug) == "vivifyDebugLogging=true",
          "developer note names the hidden change", S::NonDefault(doc, S::Section::Debug));

    S::ResetToDefaults(doc);
    bool allDefault = true;
    for (auto const& def : S::All()) {
      if (def.kind != S::Kind::String && Value(doc, def) != def.defaultValue) allDefault = false;
    }
    Check(allDefault, "reset: every setting back to its default");
    Check(S::GetString(doc, "standInShaderName").empty(), "reset: stand-in shader name cleared");
    Check(!Has(doc, "offscreenCulling") && !Has(doc, "prepareShaders"), "reset: stale keys gone");
    Check(doc.MemberCount() == S::All().size(), "reset: only current keys remain");
  }

  // --- beta.3's tester had GPU Instancing For Scenery on: it is dropped ----------
  {
    rapidjson::Document doc;
    doc.Parse(R"({"sceneryGpuInstancing": true, "detailedLogging": true, "performanceModeHeavyMaps": true})");
    Check(S::Load(doc), "a file with the retired instancing switch is rewritten");
    Check(!Has(doc, "sceneryGpuInstancing"), "sceneryGpuInstancing=true is dropped (naomi.10-beta.4)");
    Check(S::GetBool(doc, "detailedLogging") && S::GetBool(doc, "performanceModeHeavyMaps"),
          "the settings beside it carry over");
  }

  // --- Damaged files ------------------------------------------------------------
  {
    rapidjson::Document doc;
    doc.Parse("[1, 2, 3]");
    Check(S::Load(doc) && doc.IsObject() && S::GetInt(doc, "effectResolutionPercent") == 100,
          "a non-object config becomes the defaults");
    rapidjson::Document empty;
    empty.SetObject();
    Check(S::Load(empty) && empty.MemberCount() == S::All().size(), "an empty config gets every key");
    rapidjson::Document unloaded;
    Check(S::GetBool(unloaded, "standInShading") && S::GetInt(unloaded, "performanceModeFloorPercent") == 60,
          "reads from a missing document give the defaults");
  }

  std::printf("%d passed, %d failed\n", passed, failed);
  return failed == 0 ? 0 : 1;
}
