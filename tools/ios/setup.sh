#!/usr/bin/env bash
# One-time iPad setup, after tools/setup.sh (the Mac build, which also puts
# the game in game/): the iOS-capable runtime (tools/ios/fetch_runtime.sh),
# its lifter, heroes4.exe lifted with it into h4/generated-ios, then the app
# for a device (tools/ios/build_app.sh). Needs Xcode with the iOS platform.
#
#   tools/ios/setup.sh
#   python3 -I tools/ios/install_device.py --game   # sign, install, copy the game
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
X="$ROOT/third_party/x86recomp-ios"
[[ -f "$ROOT/game/heroes4.exe" ]] || { echo "error: no game/heroes4.exe: run tools/setup.sh first" >&2; exit 1; }

"$ROOT/tools/ios/fetch_runtime.sh"

# The lifter, on this Mac (SoftFloat and CORE-MATH from the Mac build's
# downloads when there: CORE-MATH's server is slow).
FETCHED=()
MAC_DEPS="$ROOT/third_party/x86recomp/out/build/release/_deps"
for d in coremath softfloat zydis; do
  [[ -d "$MAC_DEPS/$d-src" ]] &&
    FETCHED+=("-DFETCHCONTENT_SOURCE_DIR_$(echo "$d" | tr a-z A-Z)=$MAC_DEPS/$d-src")
done
(cd "$X" && cmake --preset release ${FETCHED[@]+"${FETCHED[@]}"} >/dev/null &&
  cmake --build --preset release --target recomp_lift)

if [[ ! -f "$ROOT/h4/generated-ios/table.cpp" ]]; then
  "$X/out/build/release/tools/lifter/recomp_lift" --exe "$ROOT/game/heroes4.exe" \
    --symbols "$ROOT/symbols" --out "$ROOT/h4/generated-ios" --manifest "$ROOT/h4/lifter.manifest"
fi

"$ROOT/tools/ios/build_app.sh" device
