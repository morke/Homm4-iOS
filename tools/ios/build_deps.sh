#!/usr/bin/env bash
# Cross-compiles the runtime's third-party libraries for iOS (arm64,
# static) into build/ios-deps-<platform>/prefix: SDL3, glslang,
# SPIRV-Cross, and an ffmpeg with only what the runtime decodes (Bink
# movies, MP3 streams). After macnativegames' windows_games/ra2
# scripts/ios/build_deps.sh, with the simulator added.
#
#   tools/ios/build_deps.sh [device|simulator]   (default: device)
#
# Needs Xcode with the iOS platform. Sources are downloaded once into
# build/ios-deps-src; a library is rebuilt when its stamp file is missing
# or records another version or configuration.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PLATFORM="${1:-device}"
OUT="$ROOT/build"
SRC="$OUT/ios-deps-src"
DEPS="$OUT/ios-deps-$PLATFORM"
PREFIX="$DEPS/prefix"
IOS_MIN="${IOS_MIN:-17.0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

SDL_VERSION=3.4.14
GLSLANG_TAG=16.6.0
SPIRV_CROSS_TAG=vulkan-sdk-1.4.363.0
FFMPEG_VERSION=8.1

DEVELOPER_DIR="${DEVELOPER_DIR:-$(xcode-select -p)}"
export DEVELOPER_DIR
case "$PLATFORM" in
  device)
    SDK_NAME=iPhoneOS
    TARGET="arm64-apple-ios$IOS_MIN" ;;
  simulator)
    SDK_NAME=iPhoneSimulator
    TARGET="arm64-apple-ios$IOS_MIN-simulator" ;;
  *) echo "usage: $0 [device|simulator]" >&2; exit 2 ;;
esac
SDK="$DEVELOPER_DIR/Platforms/$SDK_NAME.platform/Developer/SDKs/$SDK_NAME.sdk"
TOOLCHAIN="$DEVELOPER_DIR/Toolchains/XcodeDefault.xctoolchain/usr/bin"
[ -d "$SDK" ] && [ -x "$TOOLCHAIN/clang" ] || {
  echo "ERROR: no $SDK_NAME SDK in $DEVELOPER_DIR: install Xcode with the iOS platform" >&2
  exit 1
}
mkdir -p "$SRC" "$PREFIX"

CMAKE_IOS=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS
  -DCMAKE_OSX_SYSROOT="$SDK" -DCMAKE_OSX_ARCHITECTURES=arm64
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_MIN" -DCMAKE_INSTALL_PREFIX="$PREFIX"
  -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_FIND_ROOT_PATH="$PREFIX"
  -DCMAKE_C_COMPILER="$TOOLCHAIN/clang" -DCMAKE_CXX_COMPILER="$TOOLCHAIN/clang++"
  -DBUILD_SHARED_LIBS=OFF)

# done_stamp <name> [<what it was built with>]: built already, the same way.
done_stamp() { [ -f "$DEPS/$1.done" ] && [ "$(cat "$DEPS/$1.done")" = "${2:-}" ]; }
mark_done() { printf '%s' "${2:-}" > "$DEPS/$1.done"; }

fetch() {  # fetch <url> <archive name> <unpacked dir>
  [ -d "$SRC/$3" ] && return
  curl -fsSL -o "$SRC/$2" "$1"
  tar -xf "$SRC/$2" -C "$SRC"
}

if ! done_stamp sdl3 "$SDL_VERSION"; then
  fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz" \
    "SDL3-$SDL_VERSION.tar.gz" "SDL3-$SDL_VERSION"
  cmake -S "$SRC/SDL3-$SDL_VERSION" -B "$DEPS/build-sdl3" "${CMAKE_IOS[@]}" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF
  cmake --build "$DEPS/build-sdl3" -j "$JOBS"
  cmake --install "$DEPS/build-sdl3"
  mark_done sdl3 "$SDL_VERSION"
fi

if ! done_stamp glslang "$GLSLANG_TAG"; then
  [ -d "$SRC/glslang" ] ||
    git clone -q --depth 1 --branch "$GLSLANG_TAG" https://github.com/KhronosGroup/glslang "$SRC/glslang"
  cmake -S "$SRC/glslang" -B "$DEPS/build-glslang" "${CMAKE_IOS[@]}" \
    -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF -DENABLE_HLSL=OFF
  cmake --build "$DEPS/build-glslang" -j "$JOBS"
  cmake --install "$DEPS/build-glslang"
  mark_done glslang "$GLSLANG_TAG"
fi

if ! done_stamp spirv-cross "$SPIRV_CROSS_TAG"; then
  [ -d "$SRC/SPIRV-Cross" ] ||
    git clone -q --depth 1 --branch "$SPIRV_CROSS_TAG" https://github.com/KhronosGroup/SPIRV-Cross "$SRC/SPIRV-Cross"
  cmake -S "$SRC/SPIRV-Cross" -B "$DEPS/build-spirv-cross" "${CMAKE_IOS[@]}" \
    -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SHARED=OFF \
    -DSPIRV_CROSS_ENABLE_HLSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF \
    -DSPIRV_CROSS_ENABLE_C_API=OFF -DSPIRV_CROSS_ENABLE_UTIL=OFF
  cmake --build "$DEPS/build-spirv-cross" -j "$JOBS"
  cmake --install "$DEPS/build-spirv-cross"
  mark_done spirv-cross "$SPIRV_CROSS_TAG"
fi

# Only what the runtime decodes for Heroes IV: Bink movies and audio, MP3
# (music and voice-overs, through the Miles RIB codec in hle_extra.cpp).
FFMPEG_CODECS=(
  --enable-decoder=bink,binkaudio_dct,binkaudio_rdft,mp3,mp3float
  --enable-demuxer=bink,mp3 --enable-parser=mpegaudio --enable-protocol=file
)
FFMPEG_STAMP="$FFMPEG_VERSION ${FFMPEG_CODECS[*]}"
if ! done_stamp ffmpeg "$FFMPEG_STAMP"; then
  rm -rf "$DEPS/build-ffmpeg"
  fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" "ffmpeg-$FFMPEG_VERSION.tar.xz" \
    "ffmpeg-$FFMPEG_VERSION"
  mkdir -p "$DEPS/build-ffmpeg"
  (
    cd "$DEPS/build-ffmpeg"
    flags="-target $TARGET -isysroot $SDK"
    "$SRC/ffmpeg-$FFMPEG_VERSION/configure" --prefix="$PREFIX" \
      --enable-cross-compile --target-os=darwin --arch=arm64 \
      --cc="$TOOLCHAIN/clang" --sysroot="$SDK" \
      --extra-cflags="$flags" --extra-ldflags="$flags" \
      --enable-static --disable-shared --disable-programs --disable-doc \
      --disable-network --disable-autodetect --disable-avdevice --disable-avfilter \
      --disable-everything --enable-swscale "${FFMPEG_CODECS[@]}"
    make -j "$JOBS"
    make install
  )
  # "-framework X" is two arguments CMake's pkg-config import splits and
  # de-duplicates; the one-argument linker form survives.
  sed -i '' -E 's/-framework ([A-Za-z]+)/-Wl,-framework,\1/g' "$PREFIX"/lib/pkgconfig/libav*.pc \
    "$PREFIX"/lib/pkgconfig/libsw*.pc
  mark_done ffmpeg "$FFMPEG_STAMP"
fi

echo "iOS ($PLATFORM) dependencies in $PREFIX"
