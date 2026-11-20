#!/usr/bin/env bash
# Makes a clean copy of the source to share: ~/vivify-quest-share/, a fresh
# git repository with one commit. The working repository is not changed.
#
#   tools/make-share-copy.sh [--commit <rev>] [--out <folder>] [--check <libVivify.so>]
#
#   --commit  what to copy (default HEAD); only committed files are copied
#   --out     where (default ~/vivify-quest-share; an earlier share copy
#             there is replaced, anything else there stops the run)
#   --check   also prove the copy is the same program: build it from scratch,
#             compare its compiled functions with this libVivify.so (a build
#             of the same commit from this repository, for example
#             testlogs/symbols/<label>-<commit>/libVivify.so from
#             tools/build-release.sh), and run the host tests in the copy
#
# What the copy leaves out or changes:
# - not copied: testlogs/ (never committed anyway), PROGRESS.md, CLAUDE.md,
#   .claude/, this script and tools/share-copy/, backups (*.bak, *.orig),
#   and a vendored dependency's build log that holds its author's home path;
# - comments are removed from this project's own source files by
#   tools/share-copy/clean.py, with a real tokenizer per language (C/C++
#   with clang's lexer, Python, PowerShell, YAML, CMake, shell, .gitignore);
#   licence, copyright and credit comments stay, strings are never touched,
#   lines in C and C++ keep their numbers (so the compiled code is the same)
#   and scripts lose their comment-only lines; each cleaned file is checked
#   to be the same code; vendored and third-party files are copied as they are;
# - doc phrases written as working notes are reworded by the exact rules in
#   tools/share-copy/rewording.json;
# - the copy is scanned for home and temp paths, the patterns in
#   testlogs/private-patterns.txt (one per line: an e-mail address, a headset
#   serial) and AI-assistant markers; any hit stops the run.
#
# The one commit is authored as YukonGMD with the GitHub no-reply address.
# Nothing is pushed or uploaded. To share it, zip the folder without .git:
#   cd ~ && zip -r vivify-quest-share.zip vivify-quest-share -x 'vivify-quest-share/.git/*'
# or create an empty GitHub repository and push it yourself:
#   cd ~/vivify-quest-share && git remote add origin <url> && git push -u origin main
#
# Needs: git, python3 (a venv with PyYAML is made in ~/.cache/vivify-share-copy;
# --check also installs lz4, UnityPy and semver there), pwsh, bash, the Android
# NDK (ndkpath.txt or ANDROID_NDK_HOME) for its clang; --check also needs
# cmake, ninja, g++, glslangValidator.
set -euo pipefail

rev="HEAD"
out="$HOME/vivify-quest-share"
reference=""
while [ $# -gt 0 ]; do
  case "$1" in
    --commit) rev="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --check) reference="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
    *) echo "unknown argument: $1 (see the top of this script)" >&2; exit 2 ;;
  esac
done

repo="$(git rev-parse --show-toplevel)"
cd "$repo"
commit="$(git rev-parse --short=10 "$rev")"
if [ -f ndkpath.txt ]; then ndk="$(head -n1 ndkpath.txt)"; else ndk="${ANDROID_NDK_HOME:-}"; fi
prebuilt="$ndk/toolchains/llvm/prebuilt/linux-x86_64"
if [ ! -x "$prebuilt/bin/clang" ]; then
  echo "No NDK clang: put the NDK's path in ndkpath.txt or set ANDROID_NDK_HOME." >&2
  exit 1
fi

name="YukonGMD"
email="147674123+YukonGMD@users.noreply.github.com"
cache="$HOME/.cache/vivify-share-copy"
venv="$cache/venv"
stage="$cache/stage"
mkdir -p "$cache"
if [ ! -x "$venv/bin/python" ]; then
  python3 -m venv "$venv"
fi
"$venv/bin/python" -c "import yaml" 2>/dev/null || "$venv/bin/pip" -q install pyyaml
if [ -n "$reference" ]; then
  "$venv/bin/python" -c "import lz4, UnityPy, semver" 2>/dev/null || "$venv/bin/pip" -q install lz4 UnityPy semver
fi

echo "Copying $commit"
rm -rf "$stage"
mkdir -p "$stage"
git archive --format=tar "$commit" | tar -x -C "$stage"
version="$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' "$stage/mod.json" | head -n1)"

# Left out.
rm -rf "$stage/testlogs" "$stage/.claude" "$stage/tools/share-copy" "$stage/tools/make-share-copy.sh" \
  "$stage/extern/includes/bs-cordl/build.log"
find "$stage" \( -name 'CLAUDE.md' -o -name 'PROGRESS.md' -o -name '*.bak' -o -name '*.orig' \) -exec rm -rf {} +

# Comments and working-note phrases.
report="$cache/clean-report.txt"
"$venv/bin/python" "$repo/tools/share-copy/clean.py" "$stage" --clang "$prebuilt/bin/clang" --report "$report"

# Scan. Patterns are matched as plain text, case-insensitively.
hits=0
scan() {  # scan <where: all|text-outside-extern> <label> <pattern>
  local where="$1" label="$2" pattern="$3" found
  if [ "$where" = all ]; then
    found="$(grep -r -a -i -l -F -- "$pattern" "$stage" || true)"
  else
    found="$(grep -r -I -i -l -F --exclude-dir=extern -- "$pattern" "$stage" || true)"
  fi
  if [ -n "$found" ]; then
    echo "SCAN: $label found in:" >&2
    echo "$found" | sed "s|$stage/|  |" >&2
    hits=$((hits + 1))
  fi
}
scan all "this computer's home folder" "$HOME/"
scan all "a temporary-folder path from this computer" "/tmp/claude"
scan text "a home path" "/home/"
scan text "a Windows user path" "C:\\Users"
for marker in "claude" "anthropic" "co-authored-by"; do
  scan all "an AI-assistant marker ($marker)" "$marker"
done
# Vendored projects' own comments may name other tools; only ours count.
scan text "an AI-assistant marker (chatgpt)" "chatgpt"
if [ -f "$repo/testlogs/private-patterns.txt" ]; then
  n=0
  while IFS= read -r line || [ -n "$line" ]; do
    [ -z "$line" ] && continue
    n=$((n + 1))
    scan all "private pattern $n" "$line"
  done <"$repo/testlogs/private-patterns.txt"
else
  echo "Note: no testlogs/private-patterns.txt, so no e-mail or serial was scanned for." >&2
fi
if [ "$hits" -ne 0 ]; then
  echo "Stopped: the copy in $stage holds $hits kind(s) of text it must not. Nothing was written to $out." >&2
  exit 1
fi
tmp_mentions="$( (grep -r -I -l -F --exclude-dir=extern "/tmp/" "$stage" || true) | sed "s|$stage/||" | tr '\n' ' ')"

# Replace an earlier share copy only.
if [ -e "$out" ]; then
  authors="$(git -C "$out" log --format=%ae 2>/dev/null || true)"
  if [ "$authors" != "$email" ]; then
    echo "$out exists and is not a share copy made by this script; move it away first." >&2
    exit 1
  fi
  rm -rf "$out"
fi
mkdir -p "$out"
cp -a "$stage/." "$out/"
git -C "$out" init -q -b main
git -C "$out" add -A
GIT_AUTHOR_NAME="$name" GIT_AUTHOR_EMAIL="$email" GIT_COMMITTER_NAME="$name" GIT_COMMITTER_EMAIL="$email" \
  git -C "$out" -c commit.gpgsign=false commit -q -m "Vivify for Quest $version"
files="$(git -C "$out" ls-files | wc -l)"
echo "Share copy: $out ($files files, one commit: \"Vivify for Quest $version\", from $commit)"
echo "Report of what was removed: $report"
if [ -n "$tmp_mentions" ]; then
  echo "Generic /tmp/ paths (test scratch files, not from this computer) appear in: $tmp_mentions"
fi

if [ -z "$reference" ]; then
  exit 0
fi

# --check: build the copy from scratch, compare, run the host tests.
echo "Building the copy from scratch (this takes a while)"
build="$cache/build"
rm -rf "$build"
maps="-ffile-prefix-map=$out=. -ffile-prefix-map=$ndk=ndk"
ANDROID_NDK_HOME="$ndk" cmake -G Ninja -S "$out" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS="$maps" -DCMAKE_CXX_FLAGS="$maps" >/dev/null
cmake --build "$build" -j "${JOBS:-2}" >/dev/null
echo "Comparing with $reference"
"$venv/bin/python" "$repo/tools/share-copy/compare_functions.py" "$reference" "$build/libVivify.so" \
  --objdump "$prebuilt/bin/llvm-objdump" || true
echo "Host tests in the copy"
(cd "$out" && "$venv/bin/python" tools/run-host-tests.py)
# The tests leave Python caches in the copy; they are not part of it.
find "$out" -name __pycache__ -type d -prune -exec rm -rf {} +
git -C "$out" status --short | head -5
