#!/bin/bash
# Installs the current h4 build as a self-contained ~/Applications/Heroes IV.app
# (binary, x86recomp libraries, game data) and puts a shortcut on the Desktop.
#
# Everything lives outside ~/Desktop: macOS does not let an app launched from
# Finder read files there (the build tree, the game) without a permission
# prompt, and a script app gets no prompt at all.
#
#   tools/install_app.sh            update binary + libraries (game data kept)
#   tools/install_app.sh --data     also (re)copy the game data
#   tools/install_app.sh --loc      re-apply the Ukrainian translation (loc/)
set -euo pipefail

H4PORT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${H4_BUILD:-$H4PORT/h4/out/build/release}"  # H4_BUILD: another build of h4
APP="$HOME/Applications/Heroes IV.app"
C="$APP/Contents"

mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources"

# Binary and libraries; the binary finds the libraries next to it. Each is
# copied beside and renamed over the old one: a game still running keeps its
# files (overwriting a mapped, signed binary in place gets it killed).
put() { cp "$1" "$2.new" && mv -f "$2.new" "$2"; }
put "$BUILD/h4" "$C/MacOS/h4"
for f in "$BUILD"/x86recomp/*.dylib; do put "$f" "$C/Frameworks/$(basename "$f")"; done
install_name_tool -add_rpath "@executable_path/../Frameworks" "$C/MacOS/h4" 2>/dev/null || true

# Game data (1.1 GB): only on first install or with --data.
if [[ ! -d "$C/Resources/game" || "${1:-}" == "--data" ]]; then
  rm -rf "$C/Resources/game"
  cp -R "$H4PORT/game" "$C/Resources/game"
fi

# Ukrainian translation: patches fresh copies of the archives it touches
# (the patch appends to them).
if [[ "${1:-}" == "--data" || "${1:-}" == "--loc" ]]; then
  for a in heroes4.h4r updates.h4r text.h4r storm.h4r x2.h4r; do
    cp "$H4PORT/game/Data/$a" "$C/Resources/game/Data/$a"
  done
  python3 -I "$H4PORT/loc/tools/apply.py" "$C/Resources/game"
fi

# Combat wings (left/right panels beside the 1280 combat screen in wide mode),
# from the optional art pack (art/combat_wings, tools/collage).
mkdir -p "$C/Resources/wings"
for f in "$H4PORT"/art/combat_wings/{paradise,hell}_296x2048.png \
         "$H4PORT"/art/combat_wings/factions/*_{light,dark}.png; do
  [[ -f "$f" ]] || continue
  b="$(basename "$f" .png)"
  sips -s format bmp "$f" --out "$C/Resources/wings/${b%_296x2048}.bmp" >/dev/null
done

# Icon: art/app_icon.png (a full-bleed square, optional), else the GOG
# installer's .ico; made again whenever the art is newer than the icon.
ICON_ART="$H4PORT/art/app_icon.png"
if [[ ! -f "$C/Resources/h4.icns" || ( -f "$ICON_ART" && "$ICON_ART" -nt "$C/Resources/h4.icns" ) ]]; then
  tmp="$(mktemp -d)"
  if [[ -f "$ICON_ART" ]]; then
    cp "$ICON_ART" "$tmp/icon.png"
  else
    sips -s format png "$H4PORT/game/goggame-1207658915.ico" --out "$tmp/icon.png" >/dev/null
  fi
  mkdir "$tmp/h4.iconset"
  for sz in 16 32 128 256 512; do
    sips -z $sz $sz "$tmp/icon.png" --out "$tmp/h4.iconset/icon_${sz}x${sz}.png" >/dev/null
    sips -z $((sz * 2)) $((sz * 2)) "$tmp/icon.png" --out "$tmp/h4.iconset/icon_${sz}x${sz}@2x.png" >/dev/null
  done
  iconutil -c icns "$tmp/h4.iconset" -o "$C/Resources/h4.icns"
  rm -rf "$tmp"
fi

cat > "$C/MacOS/heroes4" <<'EOF'
#!/bin/bash
# Heroes of Might and Magic IV, native port (x86recomp).
# Log: ~/Library/Logs/h4.log; crash reports: ~/Library/Application Support/h4/crashes
here="$(cd "$(dirname "$0")" && pwd)"
log="$HOME/Library/Logs/h4.log"
mkdir -p "$(dirname "$log")"
echo "=== $(date) ===" >> "$log"
# Wide screen (16:10 at 1024 lines, the interface's 1280x1024 art centred and
# the adventure map across the whole width), scaled up with AMD FSR 1 (EASU + RCAS); F10
# cycles the filters. 16-bit colour's banding smoothed first (H4_DEBAND), RCAS's
# sharpness in stops (H4_SHARPNESS, 0 the sharpest). All can be overridden from
# the environment.
export H4_RESOLUTION="${H4_RESOLUTION:-wide}"
export H4_FILTER="${H4_FILTER:-fsr}"
export H4_DEBAND="${H4_DEBAND:-1}"
export H4_SHARPNESS="${H4_SHARPNESS:-0.2}"
export H4_WINGS_DIR="${H4_WINGS_DIR:-$here/../Resources/wings}"
exec "$here/h4" --game-dir "$here/../Resources/game" >> "$log" 2>&1
EOF
chmod +x "$C/MacOS/heroes4"

cat > "$C/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Heroes IV</string>
  <key>CFBundleDisplayName</key><string>Heroes IV</string>
  <key>CFBundleIdentifier</key><string>local.h4port.heroes4</string>
  <key>CFBundleVersion</key><string>0.1</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>heroes4</string>
  <key>CFBundleIconFile</key><string>h4</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <!-- No App Nap: it coalesces the game's millisecond timers and stalls its animation. -->
  <key>NSAppSleepDisabled</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.strategy-games</string>
</dict>
</plist>
EOF

# Modified Mach-O files need a (local, ad-hoc) signature on Apple Silicon.
for f in "$C"/Frameworks/*.dylib "$C/MacOS/h4"; do codesign --force -s - "$f" 2>/dev/null; done
codesign --force -s - "$APP" 2>/dev/null
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP"

# Desktop shortcut (a symlink: the app itself stays in ~/Applications).
ln -sfn "$APP" "$HOME/Desktop/Heroes IV"
echo "встановлено: $APP"
