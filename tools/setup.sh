#!/bin/bash
# One-time setup: from the GOG installer of Heroes of Might and Magic IV
# Complete to ~/Applications/Heroes IV.app. Takes 15-30 minutes, most of it
# compiling the game's code.
#
#   tools/setup.sh [setup_heroes_of_might_and_magic_4_complete_*.exe]
#
# Without an argument the GOG installer is looked for in ~/Downloads, and so
# is the optional art pack (heroes4-mac-assets.zip), unpacked here.
# Needs Apple's Command Line Tools and Homebrew. Running it again skips the
# finished steps; the game files never leave this folder and ~/Applications.
set -euo pipefail

H4PORT="$(cd "$(dirname "$0")/.." && pwd)"
X86RECOMP=https://github.com/JTM-rootstorm/x86recomp.git
X86RECOMP_COMMIT=1c90fe3a5dad37d0cca551fc9c8ab66a106ede1e
# heroes4.exe of GOG's "Complete 3.0" (setup_..._3.0_win11_gog_0.2_(76223)).
EXE_SHA256=b7c2f6caeeba640b1c5f1d812b172938bb4dd4ec62e34ec93ad827bd1ffaec87

say() { printf '\n== %s\n' "$*"; }
die() { printf 'помилка: %s\n' "$*" >&2; exit 1; }

[[ "$(uname -m)" == arm64 ]] || die "потрібен Мак з Apple Silicon"
xcode-select -p >/dev/null 2>&1 || { xcode-select --install; die "встановіть Command Line Tools (відкрилося вікно) і запустіть скрипт ще раз"; }
# Homebrew's installer does not put brew on the PATH of the Terminal it ran in.
[[ -x /opt/homebrew/bin/brew ]] && eval "$(/opt/homebrew/bin/brew shellenv)"
command -v brew >/dev/null || die "спершу встановіть Homebrew: https://brew.sh"

say "Інструменти (Homebrew)"
brew install cmake ninja pkgconf sdl3 ffmpeg glslang spirv-cross innoextract llvm@22

say "Файли гри"
if [[ ! -f "$H4PORT/game/heroes4.exe" ]]; then
  setup="${1:-$(ls -t "$HOME"/Downloads/setup_heroes_of_might_and_magic_4_complete*.exe 2>/dev/null | head -1 || true)}"
  [[ -n "$setup" && -f "$setup" ]] || die "не знайшов інсталятор гри з GOG (setup_heroes_of_might_and_magic_4_complete_….exe) у теці Завантаження"
  innoextract -s -d "$H4PORT/game" "$setup"
fi
[[ "$(shasum -a 256 "$H4PORT/game/heroes4.exe" | cut -d' ' -f1)" == "$EXE_SHA256" ]] ||
  die "game/heroes4.exe — не з GOG Complete 3.0, під яку зроблено порт"

say "x86recomp"
X="$H4PORT/third_party/x86recomp"
if [[ ! -d "$X/.git" ]]; then
  git clone -q "$X86RECOMP" "$X"
  git -C "$X" checkout -q "$X86RECOMP_COMMIT"
  git -C "$X" apply "$H4PORT/patches/x86recomp-h4.patch"
  # Fixes on top of it, one patch each, in name order.
  for p in "$H4PORT"/patches/x86recomp-*.patch; do
    [[ "$p" == */x86recomp-h4.patch ]] || git -C "$X" apply "$p"
  done
fi
(cd "$X" && cmake --preset release >/dev/null && cmake --build --preset release --target recomp_lift)

say "Перекомпіляція гри"
cd "$H4PORT/h4"
if [[ ! -d generated ]]; then
  "$X/out/build/release/tools/lifter/recomp_lift" --exe ../game/heroes4.exe --symbols ../symbols \
    --out generated --manifest lifter.manifest
fi
cmake --preset release >/dev/null
cmake --build --preset release --target h4

say "Український переклад"
python3 -I "$H4PORT/loc/tools/prepare.py" "$H4PORT/game"

# Safari unpacks a downloaded zip by itself: take the folder or the zip.
assets="$(ls -td "$HOME"/Downloads/heroes4-mac-assets* 2>/dev/null | head -1 || true)"
if [[ -d "$assets" ]]; then
  say "Арт і озвучка"
  ditto "$assets" "$H4PORT"
elif [[ -f "$assets" ]]; then
  say "Арт і озвучка"
  unzip -qo "$assets" -d "$H4PORT"
fi

say "Встановлення"
bash "$H4PORT/tools/install_app.sh" --data
