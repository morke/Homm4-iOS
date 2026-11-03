#!/usr/bin/env bash
# Builds Heroes IV for iOS into build/ios-<platform>/h4.app, with the game
# (game/, as tools/install_app.sh puts it in the Mac app), the wings and
# the icon inside (the game only with BUNDLE_GAME=1, the simulator's default;
# a device gets it once in Documents, tools/ios/install_device.py --game).
# After macnativegames' windows_games/ra2
# scripts/ios/build_app.sh, with the simulator added.
#
#   tools/ios/build_app.sh [device|simulator]   (default: simulator)
#
# The lifted code comes from the Mac (h4/generated-ios, recomp_lift of the
# iOS-capable runtime in third_party/x86recomp-ios). The simulator's app
# is signed ad hoc here; a device's is signed by tools/ios/install_device.py.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PLATFORM="${1:-simulator}"
OUT="$ROOT/build/ios-$PLATFORM"
PREFIX="$ROOT/build/ios-deps-$PLATFORM/prefix"
IOS_MIN="${IOS_MIN:-17.0}"
X86RECOMP="$ROOT/third_party/x86recomp-ios"
GENERATED="${GENERATED_DIR:-$ROOT/h4/generated-ios}"
APP="$OUT/h4.app"
if [ "$PLATFORM" = simulator ]; then BUNDLE_GAME="${BUNDLE_GAME:-1}"; else BUNDLE_GAME="${BUNDLE_GAME:-0}"; fi

DEVELOPER_DIR="${DEVELOPER_DIR:-$(xcode-select -p)}"
export DEVELOPER_DIR
case "$PLATFORM" in
  device)    SDK_NAME=iPhoneOS;        ACTOOL_PLATFORM=iphoneos ;;
  simulator) SDK_NAME=iPhoneSimulator; ACTOOL_PLATFORM=iphonesimulator ;;
  *) echo "usage: $0 [device|simulator]" >&2; exit 2 ;;
esac
SDK="$DEVELOPER_DIR/Platforms/$SDK_NAME.platform/Developer/SDKs/$SDK_NAME.sdk"
TOOLCHAIN="$DEVELOPER_DIR/Toolchains/XcodeDefault.xctoolchain/usr/bin"
[ -f "$GENERATED/table.cpp" ] || { echo "ERROR: no lifted code in $GENERATED" >&2; exit 1; }
[ -f "$ROOT/game/heroes4.exe" ] || { echo "ERROR: no game in $ROOT/game" >&2; exit 1; }

"$ROOT/tools/ios/build_deps.sh" "$PLATFORM"   # skips the libraries already built

# SoftFloat, CORE-MATH and Zydis from the Mac build's downloads when there
# (CORE-MATH's server is slow), else fetched.
FETCHED=()
MAC_DEPS="$ROOT/third_party/x86recomp/out/build/release/_deps"
for d in coremath softfloat; do
  [ -d "$MAC_DEPS/$d-src" ] &&
    FETCHED+=("-DFETCHCONTENT_SOURCE_DIR_$(echo "$d" | tr a-z A-Z)=$MAC_DEPS/$d-src")
done

# pkg-config (ffmpeg) must see only the iOS builds.
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH=""
cmake -S "$ROOT/h4" -B "$OUT" -G Ninja -U '__pkg_config_checked_*' -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SDK" -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_MIN" \
  -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
  -DCMAKE_C_COMPILER="$TOOLCHAIN/clang" -DCMAKE_CXX_COMPILER="$TOOLCHAIN/clang++" \
  -DRECOMP_STATIC=ON -DRECOMP_HOST_TOOLS=OFF \
  -DX86RECOMP_DIR="$X86RECOMP" -DH4_GENERATED="$GENERATED" -DH4_IOS_PLATFORM="$SDK_NAME" \
  ${FETCHED[@]+"${FETCHED[@]}"}
cmake --build "$OUT" --target h4 -j "${JOBS:-$(sysctl -n hw.ncpu)}"

# The game (clones on APFS: no extra room), and the wings as BMPs.
rm -rf "$APP/Game" "$APP/wings"
[ "$BUNDLE_GAME" = 1 ] && cp -Rc "$ROOT/game" "$APP/Game"
mkdir -p "$APP/wings"
for f in "$ROOT"/art/combat_wings/{paradise,hell}_296x2048.png \
         "$ROOT"/art/combat_wings/factions/*_{light,dark}.png; do
  [[ -f "$f" ]] || continue
  b="$(basename "$f" .png)"
  sips -s format bmp "$f" --out "$APP/wings/${b%_296x2048}.bmp" >/dev/null
done

# The icon: an asset catalog with one 1024x1024 image, from art/app_icon.png
# (a full-bleed square, optional) or GOG's .ico.
ICON_DIR="$OUT/icon"
rm -rf "${ICON_DIR:?}"
mkdir -p "$ICON_DIR/Assets.xcassets/AppIcon.appiconset"
if [ -f "$ROOT/art/app_icon.png" ]; then
  cp "$ROOT/art/app_icon.png" "$ICON_DIR/icon.png"
else
  sips -s format png "$ROOT/game/goggame-1207658915.ico" --out "$ICON_DIR/icon.png" >/dev/null
fi
sips -z 1024 1024 "$ICON_DIR/icon.png" \
  --out "$ICON_DIR/Assets.xcassets/AppIcon.appiconset/AppIcon.png" >/dev/null
cat > "$ICON_DIR/Assets.xcassets/Contents.json" <<'JSON'
{ "info" : { "author" : "xcode", "version" : 1 } }
JSON
cat > "$ICON_DIR/Assets.xcassets/AppIcon.appiconset/Contents.json" <<'JSON'
{
  "images" : [
    { "filename" : "AppIcon.png", "idiom" : "universal", "platform" : "ios", "size" : "1024x1024" }
  ],
  "info" : { "author" : "xcode", "version" : 1 }
}
JSON
xcrun actool --compile "$APP" --platform "$ACTOOL_PLATFORM" \
  --minimum-deployment-target "$IOS_MIN" --target-device iphone --target-device ipad \
  --app-icon AppIcon --output-partial-info-plist "$ICON_DIR/partial.plist" \
  --output-format human-readable-text --notices --warnings --errors \
  "$ICON_DIR/Assets.xcassets" >"$ICON_DIR/actool.log"

if [ "$PLATFORM" = simulator ]; then
  codesign --force --sign - --timestamp=none "$APP"
fi
echo "built $APP"
