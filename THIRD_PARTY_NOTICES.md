# Third-party notices

Vivify Quest is under the MIT License (see [LICENSE](LICENSE)). It builds on
other people's work, listed here with the licence each project declares.
Licences were checked on 2026-11-15 against each project's repository and
the copies restored into `extern/includes/`.

This repository contains no Beat Saber binaries or game assets. You need
your own copy of the game.

## Projects this port comes from

| Project | Author | Licence | What this port takes from it |
|---|---|---|---|
| [Vivify](https://github.com/Aeroluna/Vivify) | Aeroluna | MIT, Copyright (c) 2025 Aeroluna | The PC mod this port follows: its events, data fields and behaviour (see PARITY.md). |
| [vivify-quest](https://github.com/trail-b1az3r/vivify-quest) | trail-b1az3r (Rayla) | MIT, Copyright (c) 2026 (LICENSE in this repository) | This repository is a fork of it: the merged Quest port, the on-device PC-to-Quest converter and DirectX-to-GLSL translator, AudioLink and the diagnostics. |
| [Vivifhy-Quest](https://github.com/Gay-Axolotl/Vivifhy-Quest) | axo-lotl (Gay-Axolotl) | MIT, Copyright (c) 2026 | The base of the merged port and its per-`CameraEvent` render ordering. |
| [BSAudioLink](https://github.com/Aeroluna/BSAudioLink) | Aeroluna | MIT, Copyright (c) 2022 Aeroluna | AudioLink's shader, material and texture from `AudioLink/Assets/Bundle2021` (commit 0b127ea), translated and built into `assets/audiolink/AudioLink.bundle`, which is linked into `libVivify.so`. |
| [AudioLink](https://github.com/llealloo/audiolink) | llealloo and contributors | MIT, Copyright 2026 llealloo, cnlohr, lox9973, pema99, float3, and other AudioLink contributors | AudioLink 3.1.2, as bundled by BSAudioLink. |
| [BSAudioLink-Quest](https://github.com/RedBrumbler/BSAudioLink-Quest) | RedBrumbler | MIT, Copyright (c) 2022 RedBrumbler | The earlier Quest port of AudioLink, followed where the Quest differs. |
| [bcdec](https://github.com/iOrange/bcdec) | Sergii Kudlai | MIT or public domain, Copyright (c) 2022 Sergii Kudlai | Vendored as `include/third_party/bcdec.h` (licence: `include/third_party/bcdec.LICENSE`); decodes BC/DXT textures. |
| [HLSLcc](https://github.com/Unity-Technologies/HLSLcc) | James Jones, Unity Technologies | MIT-style (`license.txt` in its repository) | A reference for what the DirectX-to-GLSL translation has to produce. |

### Earlier Quest ports merged upstream

trail-b1az3r merged features from these ports into vivify-quest. Their
repositories declare no licence. They are credited here and in the README.

| Port | Author | What it contributed |
|---|---|---|
| [Vivify-Quest-Port](https://github.com/rbatteries1-design/Vivify-Quest-Port-OUTDATED) | Braxed (rbatteries1-design) | Legacy Blit aliases, the `VRCenterAdjust` hooks, the note-visual and multiplayer settings, settings-menu fixes |
| [Vivify-Quest](https://github.com/Lars27110/Vivify-Quest) | Lars27110 | The same fixes |
| [Vivify-Quest-2](https://github.com/webbs7524-wq/Vivify-Quest-2) | webbs7524-wq | Blit extensions: material aliases, order spellings, clearing, persistent Blits |
| [Vivify-Quest3.0fork](https://github.com/PATTT160/Vivify-Quest3.0fork) | PATTT160 | The same Blit extensions |
| [Vivify-Quest_enabled-Play](https://github.com/gamesbeash-art/Vivify-Quest_enabled-Play) | gamesbeash-art | Named enum values in `SetRenderingSettings`, the full `RenderTextureFormat` list |

## Libraries

Restored into `extern/` by `qpm restore` or `scripts/restore-deps.py`. The
mods among them are installed on the headset as separate mods by your mod
manager; `Vivify.qmod` itself contains only `libVivify.so` and `mod.json`.

"Compiled in" means some of the library's headers are compiled into
`libVivify.so`; "loaded at run time" means `libVivify.so` loads the
library's own `.so`.

| Library | Source | Declared licence | Use |
|---|---|---|---|
| beatsaber-hook (with RapidJSON) | [QuestPackageManager/beatsaber-hook](https://github.com/QuestPackageManager/beatsaber-hook), [Tencent/rapidjson](https://github.com/Tencent/rapidjson) | MIT; RapidJSON MIT | compiled in, loaded at run time |
| bs-cordl | [QuestPackageManager/bs-cordl](https://github.com/QuestPackageManager/bs-cordl) | none declared | compiled in (generated headers for the game's types) |
| libil2cpp | packaged for QPM from Unity's IL2CPP headers | none stated in the package | compiled in |
| custom-types | [QuestPackageManager/Il2CppQuestTypePatching](https://github.com/QuestPackageManager/Il2CppQuestTypePatching) | MIT | compiled in, loaded at run time |
| CustomJSONData | [bsq-ports/CustomJSONData](https://github.com/bsq-ports/CustomJSONData) | MIT | compiled in, loaded at run time |
| Tracks | [bsq-ports/Tracks](https://github.com/bsq-ports/Tracks) | none declared | compiled in, loaded at run time |
| SongCore | [raineaeternal/Quest-SongCore](https://github.com/raineaeternal/Quest-SongCore) | MIT | compiled in, loaded at run time |
| BSML | [bsq-ports/Quest-BSML](https://github.com/bsq-ports/Quest-BSML) | none declared | compiled in, loaded at run time |
| scotland2 | [sc2ad/scotland2](https://github.com/sc2ad/scotland2) | MIT | compiled in, loaded at run time |
| paperlog (paper2_scotland2, with utfcpp) | [Fernthedev/paperlog](https://github.com/Fernthedev/paperlog), [nemtrif/utfcpp](https://github.com/nemtrif/utfcpp) | none declared; utfcpp BSL-1.0 | compiled in, loaded at run time |
| fmt | [fmtlib/fmt](https://github.com/fmtlib/fmt) | MIT, Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors | compiled in |
| Sombrero | [Fernthedev/Sombrero-quest](https://github.com/Fernthedev/Sombrero-quest) | LGPL-2.1 | compiled in (headers, through Tracks and others) |
| MetaCore | [Metalit/MetaCore](https://github.com/Metalit/MetaCore) | none declared | compiled in, loaded at run time |
| WebUtils | [bsq-ports/WebUtils](https://github.com/bsq-ports/WebUtils) | none declared | compiled in, loaded at run time |
| tinyxml2 | [MillzyDev/NDK-tinyxml2](https://github.com/MillzyDev/NDK-tinyxml2) | zlib | compiled in, loaded at run time |
| config-utils | [darknight1050/config-utils](https://github.com/darknight1050/config-utils) | GPL-3.0 | restored as a dependency; not included by the source and not linked (Vivify uses beatsaber-hook's own config helpers) |
| conditional-dependencies | [sc2ad/ConditionalDependencies](https://github.com/sc2ad/ConditionalDependencies) | none declared | restored as a dependency; not used |
| cpp-semver | packaged for QPM | MIT, Copyright (c) 2022 Peter Csajtai | restored as a dependency; not used |
| RapidjsonMacros | [Metalit/RapidjsonMacros](https://github.com/Metalit/RapidjsonMacros) | none declared | restored as a dependency; not used |

Each library's own licence file, where its package has one, is in
`extern/includes/<library>/`.

## MIT License text

The MIT-licensed projects above (Vivify, vivify-quest, Vivifhy-Quest,
BSAudioLink, AudioLink, BSAudioLink-Quest, bcdec, fmt and the MIT libraries)
are used under this licence, with the copyright lines given in the tables:

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

fmt adds an optional exception for binary distributions (its `LICENSE`).
HLSLcc's `license.txt` has the same terms with its own copyright lines.
