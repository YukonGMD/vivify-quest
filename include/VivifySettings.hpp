#pragma once

#include <span>
#include <string>
#include <string_view>

#include "beatsaber-hook/shared/rapidjson/include/rapidjson/document.h"

// Every Vivify setting in one table (0.14.25-naomi.10): its config key,
// default, range, where it sits in the menu, its name there and a one-line
// hover hint. The menu (main.cpp), the session log's settings snapshot,
// SETTINGS.md and the host tests (tools/settings) all read it.
//
// Unity-free so the host tests can load old config files through it.
//
// Rules for adding one: a test-only switch goes in Debug; a new Everyday
// setting needs a stated reason in SETTINGS.md; keys never change once
// shipped (a saved value carries over by key; a deliberate reset gets a new
// key, as prepareShadersInMenu and offscreenCullingOptIn did).
namespace Vivify::Settings {

enum class Kind { Bool, Int, String };

// Everyday fits one screen; Advanced follows it; Debug shows only with
// "Show Developer Options"; Hidden is config-file only.
enum class Section { Everyday, Advanced, Debug, Hidden };

struct Def {
  std::string_view key;
  Kind kind;
  int defaultValue;  // Bool: 0 or 1; String: unused (default "")
  int minValue;
  int maxValue;
  int step;  // Int: the menu's increment
  Section section;
  std::string_view group;  // sub-heading inside Advanced and Debug
  std::string_view label;
  std::string_view hint;
};

std::span<Def const> All();
Def const* Find(std::string_view key);

// Keys earlier versions wrote that nothing reads any more; Load removes them.
std::span<std::string_view const> RemovedKeys();

// Brings a loaded config document up to date: a non-object becomes an empty
// object, removed keys are dropped, and every setting that is missing, of the
// wrong type or out of range is set to its default. Saved values that are
// valid are kept as they are. True when the document changed and should be
// written.
bool Load(rapidjson::Document& doc);

// Every setting back to its default; the document is left holding only
// current keys.
void ResetToDefaults(rapidjson::Document& doc);

// Values from a document Load has seen.
bool GetBool(rapidjson::Document const& doc, std::string_view key);
int GetInt(rapidjson::Document const& doc, std::string_view key);
std::string GetString(rapidjson::Document const& doc, std::string_view key);

// "key=value key=value ..." in table order, for the session log.
std::string Snapshot(rapidjson::Document const& doc);

// The settings in a section that differ from their defaults, as
// "key=value" pairs; empty when none do.
std::string NonDefault(rapidjson::Document const& doc, Section section);

}  // namespace Vivify::Settings
