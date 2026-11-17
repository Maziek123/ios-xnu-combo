#!/usr/bin/env bash
# Build libop2h264.dylib (software H.264 VideoToolbox decoder) against FFmpeg in out/ffmpeg/ios.
#   bash userland/op2video/build-ffmpeg.sh first (FFmpeg 6.1.2, H.264 decoder only, LGPL)
set -euo pipefail
cd "$(dirname "$0")"; source ../../tools/env.sh
F=../../out/ffmpeg/ios
O=../../out/op2video; mkdir -p "$O"
/usr/bin/clang -arch arm64 -isysroot "$IPHONEOS_SDK" -miphoneos-version-min=12.0 -O2 -Wall -dynamiclib \
  -I"$F/include" -install_name /usr/local/lib/libop2h264.dylib \
  -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox \
  -o "$O/libop2h264.dylib" OP2H264Decoder.c "$F/lib/libavcodec.a" "$F/lib/libavutil.a"
codesign -s - --force "$O/libop2h264.dylib"
ls -l "$O/libop2h264.dylib"
