#!/usr/bin/env bash
# Build OP2NullAudio.driver (silent built-in output device, AudioServerPlugIn) into out/op2audio.
#   bash userland/op2audio/build.sh [--install]   (--install: scp to the phone over SSH)
set -euo pipefail
cd "$(dirname "$0")"; source ../../tools/env.sh
O=../../out/op2audio/OP2NullAudio.driver
rm -rf "$O"; mkdir -p "$O"
"$XCODE_ROOT/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang" -arch arm64 -isysroot "$IPHONEOS_SDK" \
  -miphoneos-version-min=12.0 -O2 -Wall -Wextra -Iinclude -bundle \
  -framework CoreFoundation -o "$O/OP2NullAudio" OP2NullAudio.c
cp Info.plist "$O/Info.plist"
codesign -s - --force "$O/OP2NullAudio"
ls -l "$O"
if [ "${1:-}" = "--install" ]; then
  scp -O -r "$O" root@192.168.7.2:/Library/Audio/Plug-Ins/HAL/
fi
