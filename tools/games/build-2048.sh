#!/usr/bin/env bash
# Build native 2048 UIKit game for iOS 12 on OnePlus 2
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
source "$ROOT/tools/env.sh"
op2_require_xcode || exit 1

X="$XCODE_ROOT"
SDK="$IPHONEOS_SDK"
CLANG="$X/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang"

SRC_DIR="$ROOT/userland/games/2048"
OUT_APP="$ROOT/out/games/build/Game2048.app"

echo "=== Building 2048 UIKit Game for iOS 12 ==="
rm -rf "$OUT_APP"
mkdir -p "$OUT_APP"

# Compile Objective-C sources
"$CLANG" -arch arm64 -isysroot "$SDK" \
  -miphoneos-version-min=12.0 \
  -O2 \
  -fobjc-arc \
  -framework UIKit \
  -framework Foundation \
  -framework CoreGraphics \
  -framework QuartzCore \
  -o "$OUT_APP/Game2048" \
  "$SRC_DIR/main.m" \
  "$SRC_DIR/AppDelegate.m" \
  "$SRC_DIR/GameViewController.m"

# Copy Info.plist
cp "$SRC_DIR/Info.plist" "$OUT_APP/Info.plist"

# Generate simple AppIcon if python PIL is available
python3 - "$OUT_APP" << 'PY' 2>/dev/null || true
import sys, os
from PIL import Image, ImageDraw

app_dir = sys.argv[1] if len(sys.argv) > 1 else "out/games/build/Game2048.app"
img = Image.new("RGBA", (180, 180), (237, 194, 46, 255)) # #EDC22E gold
draw = ImageDraw.Draw(img)
draw.text((32, 60), "2048", fill=(255, 255, 255, 255))
img.save(os.path.join(app_dir, "AppIcon60x60@3x.png"))
img.save(os.path.join(app_dir, "AppIcon60x60@2x.png"))
PY

# Ad-hoc sign binary
codesign -s - --force "$OUT_APP/Game2048"

echo "[SUCCESS] Game2048.app built successfully at: $OUT_APP"
file "$OUT_APP/Game2048"
