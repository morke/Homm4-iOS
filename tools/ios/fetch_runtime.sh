#!/usr/bin/env bash
# The iOS-capable runtime in third_party/x86recomp-ios: x86recomp as
# macnativegames carries it for its Red Alert 2 port (upstream 1c90fe3 with
# WinNativeGames' changes, windows_games/ra2/third_party/x86recomp, at a
# pinned commit), then patches/ios/x86recomp-ios.patch: this port's runtime
# patch merged onto it and the iOS fixes. Nothing of it is kept in this
# repository.
#
#   tools/ios/fetch_runtime.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MNG=https://github.com/ly05010419/macnativegames.git
MNG_COMMIT=d1509b4b62211265fad5449cf53cc9f13484ecc5
MNG_DIR=windows_games/ra2/third_party/x86recomp
X="$ROOT/third_party/x86recomp-ios"

if [[ -f "$X/CMakeLists.txt" ]]; then
  echo "runtime already in $X"
  exit 0
fi
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# Only that folder, at that commit (the repository is large).
git -C "$tmp" init -q
git -C "$tmp" remote add origin "$MNG"
git -C "$tmp" sparse-checkout set "$MNG_DIR"
git -C "$tmp" fetch -q --depth 1 --filter=blob:none origin "$MNG_COMMIT"
git -C "$tmp" -c advice.detachedHead=false checkout -q FETCH_HEAD
mkdir -p "$X"
cp -R "$tmp/$MNG_DIR/." "$X/"
# A repository of its own: inside this one, git apply would skip every path
# outside the current directory, silently.
git -C "$X" init -q
git -C "$X" apply "$ROOT/patches/ios/x86recomp-ios.patch"
echo "runtime in $X"
