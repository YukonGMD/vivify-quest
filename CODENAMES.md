# Codenames

Every version of the naomi test series has a My Little Pony character as its
codename. Rules:

- One codename per version. Betas share their version's codename
  (naomi.10-beta.1, naomi.10-beta.2 and naomi.10 are all Twilight Sparkle).
- It fits what the version is about.
- Never reused. Pick from the unused list below, then strike it from that
  list and add it to the table.
- Display text only: the settings menu header (next to the exact build
  name), the first lines of every session log, and CHANGELOG.md. Never in a
  version string, a file name or a tag. The code holds it in
  `include/VivifyVersion.hpp`.

## Version strings

Betas are `0.14.25-naomi.N-beta.M`; a release is `0.14.25-naomi.N-release`.
A bare `0.14.25-naomi.N` would sort *below* its own betas under SemVer (a
numeric identifier ranks below an alphanumeric one), which is how
ModsBeforeFriday compares versions; `-release` sorts above every `-beta.M`
and below the next version's betas. `tools/version/check_versions.py`
checks this.

## Given

| Version | Codename | Why |
|---|---|---|
| naomi.1 | Cheerilee | The teacher taking stock: the PC parity and performance audits, and builds that say which commit they came from. |
| naomi.2 | Shining Armor | The guard captain: a watchdog for main-thread freezes, and breadcrumbs at every step of a level start. |
| naomi.3 | Zecora | Reads what others cannot see: native stack samples of a stalled thread, GC counts in breadcrumbs. |
| naomi.4 | Spitfire | Times every flier: the song performance summary and the OVR Metrics tool. |
| naomi.5 | Starlight Glimmer | Recreates other ponies' magic: geometry-shader effects emulated without a geometry stage, plus instancing and copy reuse. |
| naomi.6 | Nurse Redheart | Patches ponies up: 743 Aether stopped crashing on every start. |
| naomi.7 | Rainbow Dash | Speed: the performance round (performance mode, per-section timing, slow-frame sampler, loops the GPU can unroll). |
| naomi.8 | Maud Pie | Rocks that hold still: particle bursts baked into meshes; plus the cost test and GPU time per section. |
| naomi.9 | Fluttershy | Gentle and careful: fixed naomi.8 overwriting its own stack when reading particles. |
| naomi.10 | Twilight Sparkle | The organiser ("Version X"): crash fixes, the gray ledger and shader audit, a tidied settings menu, layered logging and the Version X plan. |

## Unused candidates

Applejack, Pinkie Pie, Rarity, Spike, Princess Celestia, Princess Luna,
Princess Cadance, Sunset Shimmer, Trixie, Discord, Derpy, Lyra Heartstrings,
Bon Bon, Vinyl Scratch, Octavia Melody, Sweetie Belle, Apple Bloom,
Scootaloo, Granny Smith, Big McIntosh, Star Swirl the Bearded, Sunburst,
Moondancer, Cheese Sandwich, Coloratura, Daring Do, Soarin, Fleetfoot,
Photo Finish, Mayor Mare, Mrs. Cake, Thorax, Ember, Smolder, Gallus,
Silverstream, Yona, Sandbar, Ocellus, Autumn Blaze, Tempest Shadow,
Zephyr Breeze, Fancy Pants, Babs Seed, Twist, Hoity Toity.

Some that fit likely themes: a stability release could be Applejack
(dependable); a pure speed-up, Soarin; a big visual-parity release, Rarity;
a release that removes stalls, Pinkie Pie (never stops moving); a cleanup,
Granny Smith.
