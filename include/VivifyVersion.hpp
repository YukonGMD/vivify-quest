#pragma once

#include <string_view>

// The codename of this version (naomi.10): display text only. It appears in
// the settings menu's header next to the exact build name, in every session
// log's header and in CHANGELOG.md -- never in a version string or a file
// name. Betas share their version's codename; a codename is never reused.
// CODENAMES.md lists every version's and the unused candidates.
namespace Vivify {
inline constexpr std::string_view kCodename = "Twilight Sparkle";
}
