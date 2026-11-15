#!/usr/bin/env bash
# Build lib_op2_springboard.dylib and sb_cmd for iOS 12 on OnePlus 2
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "${REPO_ROOT}/tools/env.sh"

CLANG="${XCODE_ROOT}/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang"

OUT_DIR="${REPO_ROOT}/out/springboard"
mkdir -p "${OUT_DIR}"

echo "[*] Building lib_op2_springboard.dylib..."
"${CLANG}" -arch arm64 -isysroot "${IPHONEOS_SDK}" \
    -D_XOPEN_SOURCE -dynamiclib -fobjc-arc \
    -framework Foundation -framework UIKit -framework CoreGraphics -framework QuartzCore \
    -o "${OUT_DIR}/lib_op2_springboard.dylib" \
    "${REPO_ROOT}/userland/springboard/lib_op2_springboard.m" \
    "${REPO_ROOT}/userland/games/2048/GameViewController.m"

codesign --remove-signature "${OUT_DIR}/lib_op2_springboard.dylib" 2>/dev/null || true

echo "[*] Building sb_cmd..."
"${CLANG}" -arch arm64 -isysroot "${IPHONEOS_SDK}" \
    -o "${OUT_DIR}/sb_cmd" \
    "${REPO_ROOT}/userland/springboard/sb_cmd.c"

codesign --remove-signature "${OUT_DIR}/sb_cmd" 2>/dev/null || true

echo "[+] Built products in ${OUT_DIR}:"
ls -la "${OUT_DIR}"
