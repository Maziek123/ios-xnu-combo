#!/usr/bin/env bash
# FFmpeg 6.1.2 (LGPL), only the H.264 decoder + parser, static, for iOS 12 arm64 -> out/ffmpeg/ios.
# Source: https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz (downloaded to out/ffmpeg/, not in git).
set -euo pipefail
cd "$(dirname "$0")/../.."; source tools/env.sh
R=$PWD/out/ffmpeg
[ -d "$R/src/ffmpeg-6.1.2" ] || { mkdir -p "$R/src"; [ -f "$R/ffmpeg-6.1.2.tar.xz" ] || curl -fL -o "$R/ffmpeg-6.1.2.tar.xz" https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz; tar -xJf "$R/ffmpeg-6.1.2.tar.xz" -C "$R/src"; }
cd "$R/src/ffmpeg-6.1.2"
# decode-order output under AV_CODEC_FLAG_LOW_DELAY (see the patch)
patch -p1 -N --dry-run < "$OLDPWD/userland/op2video/patches/h264-decode-order.patch" >/dev/null 2>&1 &&
  patch -p1 -N < "$OLDPWD/userland/op2video/patches/h264-decode-order.patch"
./configure --prefix="$R/ios" --enable-cross-compile --target-os=darwin --arch=aarch64 --cc=/usr/bin/clang \
  --sysroot="$IPHONEOS_SDK" --extra-cflags="-arch arm64 -miphoneos-version-min=12.0 -O2" \
  --extra-ldflags="-arch arm64 -miphoneos-version-min=12.0" --disable-everything --enable-decoder=h264 \
  --enable-parser=h264 --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-swresample \
  --disable-swscale --disable-avfilter --disable-postproc --disable-network --disable-autodetect --enable-pic \
  --enable-static --disable-shared
make -j8 && make install
