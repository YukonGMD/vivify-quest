# Gray ledger

What was drawn gray (or would be) on the headset, per map, with the reason.
First written for naomi.10 from the naomi.9 test (run9 logs, 2026-10-25) and
an offline audit of every shader in the library (`tools/shaderaudit`).

Classes:

- **a** intentional stand-in: Vivify cannot run the effect, so it draws a
  plain stand-in (soft gray when the material's only colour is the default
  white, dark gray when it has none; see `VivifyAssets.cpp`)
- **b** translation, compile or link failure
- **c** texture missing, dropped, unsupported or over budget
- **d** stale converted cache from before a fix
- **e** property or keyword that never reached the shader
- **f** other

## Maps played in naomi.9

| Map | Plays | a | b | c | d | e | f |
|---|---|---|---|---|---|---|---|
| 4968d 743 Aether (PC, converted from its 2019 bundle) | 10 | 8 particle systems per play | 0 of 194 programs | 0 | 0 | none seen | 0 |
| 4839c Lost in Data (Quest bundle) | 2 | 0 | 0 of 1148 programs | 0 | n/a | none seen | 0 |

The gray in naomi.9 was Aether's. Every play, 8 particle systems started on
the stand-in. Their shaders are geometry-shader effects that Vivify emulates
on meshes, but these systems are particle systems:

| Object | Material (shader) | Stand-in colour | Outcome |
|---|---|---|---|
| Explosion ×3 | 97-101 / 129-133 Triangle Explosion (Swifter/TriangleExplosion) | soft gray (colour is white or transparent, no texture) | baked about 0.5 s after the burst holds still, then correct; gray until then |
| Extending Arrows, (1), (2) | 673 - Triangle Explosion (Swifter/TriangleExplosion) | soft gray | stays gray: the particles move (velocity module), and only particles that hold still can be baked |
| rock particles | 363.33 - Wireframe (Mawntee/Beat Saber Wireframe 3D-Noise) | `_BaseColor` black | stays: 1000 particles × 876 vertices is over the 400,000-vertex bake limit |
| Ground Fog | 363.33 - Wireframe (Mawntee/Beat Saber Wireframe 3D-Noise) | `_BaseColor` black | stays: billboard particles, and only mesh particles can be baked |

Per shader: Swifter/TriangleExplosion had 3 systems gray for good and 3 gray
for about half a second; Mawntee/Beat Saber Wireframe 3D-Noise had 2 for good.

Before naomi.10 the log said nothing about 'Extending Arrows' or 'Ground
Fog' beyond "not a mesh renderer". It now says why each one keeps the
stand-in, and stops checking a system whose particles are still moving after
10 s. Each check allocated a copy of every particle.

Lost in Data logged no stand-ins and no texture problems. Its 28 materials
are inside prefabs, which the shader log did not cover; Stage 5's asset
inventory does. If something looked gray there, these logs do not show why.

Other evidence for both maps: texture decode replaced 0 references, skipped 0
over budget and refused 0; all 140 of Aether's material-shader checks in the
debug log say supported; logcat has no shader or GL errors.

## Library audit (all 25 maps)

`tools/shaderaudit/audit.py` converts each PC bundle the way the headset does
(geometry emulation on, unused variants skipped) and repacks each Quest
bundle. It then compiles and links every GLES program with glslangValidator.
A map that ships both a 2019 and a 2021 PC bundle has both checked, because
the headset takes whichever its folder lists first: Aether and Get Your Wish
were converted from 2019, the others from 2021.

| | Bundles | Programs that link | Fail |
|---|---|---|---|
| naomi.9 translator | 31 | 15,934 | 12 |
| naomi.10 translator | 31 | 15,946 | 0 |

Unity's own Quest programs say `#version 300 es` but use ES 3.1 binding and
location qualifiers, which the headset's GLES 3.2 driver accepts. Where
glslang rejects only those qualifiers, the audit rechecks the program as 3.10.

Found and fixed:

- **b, 12 programs, 43a5d Through The Screen, AreaLit/LightMeshClear.** A
  fragment program that writes only depth declared its depth output as a
  colour output at location 4294967295, so it failed to link and the material
  got the stand-in. The program now writes `gl_FragDepth` only. Covered by the
  dxbc host test. Conversion version 22 → 23.

Found, not fixable in this round (see VERSION_X.md):

- **a, 43a5d, AshleyScarlet/ParticleScreen/Visualizer** (whole shader) and
  **AreaLit/LightMesh** (14 variants): geometry programs that read the
  instance ID, or pass a varying straight through. Emulation cannot run them
  in the vertex stage, so they get the stand-in.
- **a, 43999 42-flux, WACOMalt/WACOWater4**: the mapper's Quest bundle ships
  this shader with no programs. "Use PC Bundle For Missing Shaders" (off by
  default) can fill it from the PC build. The clean slate at 01:25 deleted the
  23 PC bundles that setting had downloaded.

Textures: every PC bundle's textures read (converter counts). Quest bundles
were not texture-checked offline.

## Stale caches

None at test time. The clean slate at 01:25:14 removed all 11 converted
files, and the bulk convert at 01:25:22 rebuilt 9 (marker "22 17", the
version then current). naomi.10's version 23 makes every converted map
rebuild itself on its next load (all 9 took 8 s in naomi.9), so no Force
Reconvert is needed.

## Re-running

    python3 tools/shaderaudit/audit.py --conv <host conv> \
        --bundles <dir with one folder per map key> --out <dir>

Build the host `conv` as in `.github/workflows/validate.yml`. The bundles
are pulled read-only from the headset's song folders.
