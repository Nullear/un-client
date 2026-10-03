#!/usr/bin/env bash
set -euo pipefail

# Run on macOS with Xcode and the Godot 4.4.1 source tree already built for iOS.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GODOT_SOURCE="${GODOT_SOURCE:?Set GODOT_SOURCE to the Godot 4.4.1 source directory}"
GODOT_SOURCE="$(cd "$GODOT_SOURCE" && pwd)"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
DEST="$ROOT/ios/plugins/UnFalsusVideo"
mkdir -p "$DEST"

for variant in release debug; do
  flags=(-O2)
  if [ "$variant" = debug ]; then flags=(-O0 -g -DDEBUG_ENABLED); fi
  xcrun --sdk iphoneos clang++ -std=c++17 -x objective-c++ -fobjc-arc -fmodules -fcxx-modules \
    -fno-exceptions -arch arm64 -miphoneos-version-min=14.0 -isysroot "$SDK" \
    -DPTRCALL_ENABLED -DIOS_ENABLED -DUNIX_ENABLED \
    -I "$GODOT_SOURCE" -I "$GODOT_SOURCE/platform/ios" "${flags[@]}" \
    -c "$ROOT/ios/native/UnFalsusVideo.mm" -o "$DEST/UnFalsusVideo.$variant.o"
  xcrun libtool -static -o "$DEST/UnFalsusVideo.$variant.a" "$DEST/UnFalsusVideo.$variant.o"
  rm "$DEST/UnFalsusVideo.$variant.o"
done
cp "$ROOT/ios/native/UnFalsusVideo.gdip.template" "$DEST/UnFalsusVideo.gdip"
