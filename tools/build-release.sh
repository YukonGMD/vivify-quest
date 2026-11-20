#!/usr/bin/env bash
# Builds the qmod to install or share, from the committed HEAD, in a fresh
# worktree, and leaves exactly one copy of it: ~/Vivify-<label>.qmod.
#
#   tools/build-release.sh <label>          e.g. tools/build-release.sh naomi-10-rc.1
#
# - Refuses to run with uncommitted changes to tracked files: the build must
#   be the commit it says it is.
# - Builds in build/release-wt (a git worktree of HEAD, recreated every time,
#   so CMake is configured fresh and the session log names the right commit).
#   Paths are mapped in the binary (-ffile-prefix-map): the worktree becomes
#   "." and the NDK "ndk", so no home or build path ends up in it.
# - Writes ~/Vivify-<label>.qmod, and the unstripped library with a note of
#   what it is to testlogs/symbols/<label>-<commit>/ (for reading crash
#   addresses later; testlogs/ is never committed).
# - Deletes every other Vivify.qmod this build or an earlier one left in the
#   project folder or the worktree, so the copy in ~ is the only one.
# - Scans the qmod for this computer's home folder, temp paths and Windows
#   user folders, and for
#   each line of testlogs/private-patterns.txt (an e-mail address, a headset
#   serial; that file is yours and is never committed). Any hit fails the run.
#
# Needs: git, cmake, ninja, pwsh (for scripts/createqmod.ps1), unzip, and the
# NDK in ndkpath.txt or $ANDROID_NDK_HOME. JOBS=<n> sets the build jobs
# (default 2; more needs more than 7 GB of memory).
set -euo pipefail

label="${1:-}"
if [ -z "$label" ]; then
  echo "usage: tools/build-release.sh <label>   (for example naomi-10-rc.1)" >&2
  exit 2
fi

repo="$(git rev-parse --show-toplevel)"
cd "$repo"
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "There are uncommitted changes to tracked files; commit them first." >&2
  exit 1
fi

if [ -f ndkpath.txt ]; then
  ndk="$(head -n1 ndkpath.txt)"
else
  ndk="${ANDROID_NDK_HOME:-}"
fi
if [ -z "$ndk" ] || [ ! -d "$ndk" ]; then
  echo "No NDK: put its path in ndkpath.txt or set ANDROID_NDK_HOME." >&2
  exit 1
fi

commit="$(git rev-parse --short=10 HEAD)"
version="$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' mod.json | head -n1)"
wt="$repo/build/release-wt"
out="$HOME/Vivify-$label.qmod"
symbols="$repo/testlogs/symbols/$label-$commit"

echo "Building $version (commit $commit) as $out"

git worktree remove --force "$wt" >/dev/null 2>&1 || true
rm -rf "$wt"
git worktree prune
git worktree add --detach "$wt" HEAD >/dev/null
cp ndkpath.txt "$wt/" 2>/dev/null || true

maps="-ffile-prefix-map=$wt=. -ffile-prefix-map=$ndk=ndk"
cmake -G Ninja -S "$wt" -B "$wt/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS="$maps" -DCMAKE_CXX_FLAGS="$maps" >/dev/null
cmake --build "$wt/build" -j "${JOBS:-2}"

(cd "$wt" && pwsh -NoProfile -File scripts/createqmod.ps1 >/dev/null)
if [ ! -f "$wt/Vivify.qmod" ]; then
  echo "createqmod.ps1 made no Vivify.qmod" >&2
  exit 1
fi

# The scan, before anything is copied out.
scan="$(mktemp -d)"
trap 'rm -rf "$scan"' EXIT
unzip -q "$wt/Vivify.qmod" -d "$scan/qmod"
# This computer's home folder (which also covers the QPM cache under it),
# temp paths and a Windows user folder.
patterns=("$HOME/" "/tmp/" "C:\\Users")
if [ -f "$repo/testlogs/private-patterns.txt" ]; then
  while IFS= read -r line || [ -n "$line" ]; do
    [ -n "$line" ] && patterns+=("$line")
  done <"$repo/testlogs/private-patterns.txt"
else
  echo "Note: no testlogs/private-patterns.txt, so no e-mail or serial was scanned for." >&2
fi
hits=0
for i in "${!patterns[@]}"; do
  if grep -r -a -i -l -F -- "${patterns[$i]}" "$scan/qmod" >/dev/null 2>&1; then
    # Only the pattern's number is printed, so a private pattern is not echoed.
    echo "SCAN FAILED: pattern $((i + 1)) of ${#patterns[@]} found in:" >&2
    grep -r -a -i -l -F -- "${patterns[$i]}" "$scan/qmod" | sed "s|$scan/qmod/|  |" >&2
    hits=$((hits + 1))
  fi
done
if [ "$hits" -ne 0 ]; then
  echo "Not copied out: $hits pattern(s) found. The qmod is left in $wt for a look." >&2
  exit 1
fi

cp "$wt/Vivify.qmod" "$out"
mkdir -p "$symbols"
cp "$wt/build/libVivify.so" "$symbols/libVivify.so"
build_id="$(readelf -n "$symbols/libVivify.so" 2>/dev/null | sed -n 's/.*Build ID: //p' | head -n1)"
sum="$(sha256sum "$out" | cut -d' ' -f1)"
cat >"$symbols/WHICH-BUILD.txt" <<EOF
$version, commit $commit, built $(date '+%Y-%m-%d %H:%M %z') by tools/build-release.sh
from a fresh worktree of HEAD (RelWithDebInfo, paths mapped to . and ndk).
Copy: ~/Vivify-$label.qmod sha256 $sum
libVivify.so Build ID $build_id (this unstripped library is the one in the qmod)
Scan: none of ${#patterns[@]} patterns found (home and temp paths, QPM path, private patterns).
EOF

# One copy only: the worktree's and any stale one in the project folder go.
rm -f "$wt/Vivify.qmod" "$repo/Vivify.qmod"

echo "Done: $out"
echo "  sha256 $sum"
echo "  library Build ID $build_id"
echo "  symbols in ${symbols#$repo/}"
